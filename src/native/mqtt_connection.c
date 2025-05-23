/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <jni.h>

#include <aws/common/atomics.h>
#include <aws/common/condition_variable.h>
#include <aws/common/logging.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>
#include <aws/common/thread.h>
#include <aws/http/connection.h>
#include <aws/http/proxy.h>
#include <aws/http/request_response.h>
#include <aws/io/channel.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/io/socket.h>
#include <aws/io/socket_channel_handler.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/mqtt/client.h>

#include <ctype.h>
#include <string.h>

#include "crt.h"

#include "http_request_utils.h"
#include "java_class_ids.h"
#include "mqtt5_client_jni.h"
#include "mqtt_connection.h"

/*******************************************************************************
 * mqtt_jni_ws_handshake - Data needed to perform the async websocket handshake
 * transform operations. Destroyed when transform is complete.
 ******************************************************************************/
struct mqtt_jni_ws_handshake {
    struct mqtt_jni_connection *connection;
    struct aws_http_message *http_request;
    aws_mqtt_transform_websocket_handshake_complete_fn *complete_fn;
    void *complete_ctx;
};

static void s_mqtt_connection_destroy(JNIEnv *env, struct mqtt_jni_connection *connection);

static void s_mqtt_jni_connection_acquire(struct mqtt_jni_connection *connection) {
    size_t old_value = aws_atomic_fetch_add(&connection->ref_count, 1);

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "mqtt_jni_connection acquire, ref count now = %d", (int)old_value + 1);
}

static void s_on_shutdown_disconnect_complete(struct aws_mqtt_client_connection *connection, void *user_data);

static void s_mqtt_jni_connection_release(struct mqtt_jni_connection *connection) {
    size_t old_value = aws_atomic_fetch_sub(&connection->ref_count, 1);

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "mqtt_jni_connection release, ref count now = %d", (int)old_value - 1);
}

/* The destroy function is called on Java MqttClientConnection resource release. */
static void s_mqtt_jni_connection_destroy(struct mqtt_jni_connection *connection) {
    /* For mqtt311 client, we have to call aws_mqtt_client_connection_disconnect before releasing the underlying c
     * connection.*/
    if (aws_mqtt_client_connection_disconnect(
            connection->client_connection, s_on_shutdown_disconnect_complete, connection) != AWS_OP_SUCCESS) {

        /*
         * This can happen under normal code paths if the client happens to be disconnected at cleanup/shutdown
         * time. Log it (in case it was unexpected) and then shutdown the underlying connection manually.
         */
        AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "Client disconnect failed. Release the client connection.");
        s_on_shutdown_disconnect_complete(connection->client_connection, NULL);
    }
}

static struct mqtt_jni_async_callback *s_mqtt_jni_async_callback_new(
    struct mqtt_jni_connection *connection,
    jobject async_callback,
    JNIEnv *env) {

    if (env == NULL) {
        return NULL;
    }

    struct aws_allocator *allocator = aws_jni_get_allocator();
    /* allocate cannot fail */
    struct mqtt_jni_async_callback *callback = aws_mem_calloc(allocator, 1, sizeof(struct mqtt_jni_async_callback));
    callback->connection = connection;
    callback->async_callback = async_callback ? (*env)->NewGlobalRef(env, async_callback) : NULL;
    aws_byte_buf_init(&callback->buffer, aws_jni_get_allocator(), 0);

    return callback;
}

static void s_mqtt_jni_async_callback_destroy(struct mqtt_jni_async_callback *callback, JNIEnv *env) {
    AWS_FATAL_ASSERT(callback && callback->connection);

    if (env == NULL) {
        return;
    }

    if (callback->async_callback) {
        (*env)->DeleteGlobalRef(env, callback->async_callback);
    }

    aws_byte_buf_clean_up(&callback->buffer);

    struct aws_allocator *allocator = aws_jni_get_allocator();
    aws_mem_release(allocator, callback);
}

static jobject s_new_mqtt_exception(JNIEnv *env, int error_code) {
    jobject exception = (*env)->NewObject(
        env, mqtt_exception_properties.jni_mqtt_exception, mqtt_exception_properties.jni_constructor, error_code);
    return exception;
}

/* on 32-bit platforms, casting pointers to longs throws a warning we don't need */
#if UINTPTR_MAX == 0xffffffff
#    if defined(_MSC_VER)
#        pragma warning(push)
#        pragma warning(disable : 4305) /* 'type cast': truncation from 'jlong' to 'jni_tls_ctx_options *' */
#    else
#        pragma GCC diagnostic push
#        pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
#        pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#    endif
#endif

/*******************************************************************************
 * new
 ******************************************************************************/
static void s_on_connection_disconnected(struct aws_mqtt_client_connection *client_connection, void *user_data);
static void s_on_connection_complete(
    struct aws_mqtt_client_connection *client_connection,
    int error_code,
    enum aws_mqtt_connect_return_code return_code,
    bool session_present,
    void *user_data) {
    (void)client_connection;
    (void)return_code;

    struct mqtt_jni_async_callback *connect_callback = user_data;
    struct mqtt_jni_connection *connection = connect_callback->connection;

    /********** JNI ENV ACQUIRE **********/
    JavaVM *jvm = connection->jvm;
    JNIEnv *env = aws_jni_acquire_thread_env(jvm);
    if (env == NULL) {
        /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
        return;
    }

    jobject mqtt_connection = (*env)->NewLocalRef(env, connection->java_mqtt_connection);
    if (mqtt_connection != NULL) {
        (*env)->CallVoidMethod(
            env, mqtt_connection, mqtt_connection_properties.on_connection_complete, error_code, session_present);

        (*env)->DeleteLocalRef(env, mqtt_connection);

        if (aws_jni_check_and_clear_exception(env)) {
            aws_jni_release_thread_env(connection->jvm, env);
            /********** JNI ENV RELEASE EARLY OUT **********/
            aws_mqtt_client_connection_disconnect(client_connection, s_on_connection_disconnected, connect_callback);
            return; /* callback and ref count will be cleaned up in s_on_connection_disconnected */
        }
    }

    s_mqtt_jni_async_callback_destroy(connect_callback, env);

    aws_jni_release_thread_env(jvm, env);
    /********** JNI ENV RELEASE **********/
    s_mqtt_jni_connection_release(connection);
}

static void s_on_connection_interrupted_internal(
    struct mqtt_jni_connection *connection,
    int error_code,
    jobject ack_callback,
    JNIEnv *env) {

    AWS_FATAL_ASSERT(env);

    jobject mqtt_connection = (*env)->NewLocalRef(env, connection->java_mqtt_connection);
    if (mqtt_connection) {
        (*env)->CallVoidMethod(
            env, mqtt_connection, mqtt_connection_properties.on_connection_interrupted, error_code, ack_callback);

        (*env)->DeleteLocalRef(env, mqtt_connection);

        AWS_FATAL_ASSERT(!aws_jni_check_and_clear_exception(env));
    }
}

static void s_on_connection_interrupted(
    struct aws_mqtt_client_connection *client_connection,
    int error_code,
    void *user_data) {
    (void)client_connection;

    struct mqtt_jni_connection *connection = user_data;

    /********** JNI ENV ACQUIRE **********/
    JNIEnv *env = aws_jni_acquire_thread_env(connection->jvm);
    if (env == NULL) {
        /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
        return;
    }

    s_on_connection_interrupted_internal(user_data, error_code, NULL, env);

    aws_jni_release_thread_env(connection->jvm, env);
    /********** JNI ENV RELEASE **********/
}

static void s_on_connection_success(
    struct aws_mqtt_client_connection *client_connection,
    enum aws_mqtt_connect_return_code return_code,
    bool session_present,
    void *user_data) {
    (void)client_connection;
    (void)return_code;

    struct mqtt_jni_connection *connection = user_data;

    /********** JNI ENV ACQUIRE **********/
    JNIEnv *env = aws_jni_acquire_thread_env(connection->jvm);
    if (env == NULL) {
        /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
        return;
    }
    jobject mqtt_connection = (*env)->NewLocalRef(env, connection->java_mqtt_connection);
    if (mqtt_connection) {

        (*env)->CallVoidMethod(env, mqtt_connection, mqtt_connection_properties.on_connection_success, session_present);

        (*env)->DeleteLocalRef(env, mqtt_connection);

        AWS_FATAL_ASSERT(!aws_jni_check_and_clear_exception(env));
    }
    aws_jni_release_thread_env(connection->jvm, env);
    /********** JNI ENV RELEASE **********/
}

static void s_on_connection_failure(
    struct aws_mqtt_client_connection *client_connection,
    int error_code,
    void *user_data) {
    (void)client_connection;

    struct mqtt_jni_connection *connection = user_data;

    /********** JNI ENV ACQUIRE **********/
    JNIEnv *env = aws_jni_acquire_thread_env(connection->jvm);
    if (env == NULL) {
        /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
        return;
    }
    jobject mqtt_connection = (*env)->NewLocalRef(env, connection->java_mqtt_connection);
    if (mqtt_connection) {
        (*env)->CallVoidMethod(env, mqtt_connection, mqtt_connection_properties.on_connection_failure, error_code);

        (*env)->DeleteLocalRef(env, mqtt_connection);

        AWS_FATAL_ASSERT(!aws_jni_check_and_clear_exception(env));
    }
    aws_jni_release_thread_env(connection->jvm, env);
    /********** JNI ENV RELEASE **********/
}

static void s_on_connection_resumed(
    struct aws_mqtt_client_connection *client_connection,
    enum aws_mqtt_connect_return_code return_code,
    bool session_present,
    void *user_data) {
    (void)client_connection;
    (void)return_code;

    struct mqtt_jni_connection *connection = user_data;

    /********** JNI ENV ACQUIRE **********/
    JNIEnv *env = aws_jni_acquire_thread_env(connection->jvm);
    if (env == NULL) {
        /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
        return;
    }

    jobject mqtt_connection = (*env)->NewLocalRef(env, connection->java_mqtt_connection);
    if (mqtt_connection) {

        (*env)->CallVoidMethod(env, mqtt_connection, mqtt_connection_properties.on_connection_resumed, session_present);

        (*env)->DeleteLocalRef(env, mqtt_connection);

        AWS_FATAL_ASSERT(!aws_jni_check_and_clear_exception(env));
    }

    aws_jni_release_thread_env(connection->jvm, env);
    /********** JNI ENV RELEASE **********/
}

static void s_on_connection_disconnected(struct aws_mqtt_client_connection *client_connection, void *user_data) {
    (void)client_connection;

    struct mqtt_jni_async_callback *connect_callback = user_data;
    struct mqtt_jni_connection *jni_connection = connect_callback->connection;

    /********** JNI ENV ACQUIRE **********/
    JNIEnv *env = aws_jni_acquire_thread_env(jni_connection->jvm);
    if (env == NULL) {
        /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
        return;
    }

    s_on_connection_interrupted_internal(connect_callback->connection, 0, connect_callback->async_callback, env);

    s_mqtt_jni_async_callback_destroy(connect_callback, env);

    AWS_FATAL_ASSERT(!aws_jni_check_and_clear_exception(env));

    aws_jni_release_thread_env(jni_connection->jvm, env);
    /********** JNI ENV RELEASE **********/

    /* Do not call release here: s_on_connection_closed will (normally) be called
     * right after and so we can call the release there instead. */
}

static void s_on_connection_closed(
    struct aws_mqtt_client_connection *client_connection,
    struct on_connection_closed_data *data,
    void *user_data) {
    (void)client_connection;
    (void)data;

    struct mqtt_jni_connection *connection = user_data;

    /********** JNI ENV ACQUIRE **********/
    JNIEnv *env = aws_jni_acquire_thread_env(connection->jvm);
    if (env == NULL) {
        /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
        return;
    }

    // Make sure the Java object has not been garbage collected
    if (!(*env)->IsSameObject(env, connection->java_mqtt_connection, NULL)) {
        jobject mqtt_connection = (*env)->NewLocalRef(env, connection->java_mqtt_connection);
        if (mqtt_connection) {
            (*env)->CallVoidMethod(env, mqtt_connection, mqtt_connection_properties.on_connection_closed);
            (*env)->DeleteLocalRef(env, mqtt_connection);
            AWS_FATAL_ASSERT(!aws_jni_check_and_clear_exception(env));
        }
    }
    aws_jni_release_thread_env(connection->jvm, env);
    /********** JNI ENV RELEASE **********/
}

static void s_on_connection_terminated(void *user_data) {

    struct mqtt_jni_connection *jni_connection = (struct mqtt_jni_connection *)user_data;

    /********** JNI ENV ACQUIRE **********/
    JNIEnv *env = aws_jni_acquire_thread_env(jni_connection->jvm);
    if (env == NULL) {
        /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
        return;
    }

    jobject mqtt_connection = (*env)->NewLocalRef(env, jni_connection->java_mqtt_connection);
    if (mqtt_connection != NULL) {
        (*env)->CallVoidMethod(env, mqtt_connection, crt_resource_properties.release_references);

        (*env)->DeleteLocalRef(env, mqtt_connection);

        aws_jni_check_and_clear_exception(env);
    }

    JavaVM *jvm = jni_connection->jvm;

    s_mqtt_connection_destroy(env, jni_connection);
    aws_jni_release_thread_env(jvm, env);
    /********** JNI ENV RELEASE **********/
}

static struct mqtt_jni_connection *s_mqtt_connection_new(
    JNIEnv *env,
    struct aws_mqtt_client *client3,
    struct aws_mqtt5_client_java_jni *client5_jni,
    jobject java_mqtt_connection) {
    struct aws_allocator *allocator = aws_jni_get_allocator();

    struct mqtt_jni_connection *connection = aws_mem_calloc(allocator, 1, sizeof(struct mqtt_jni_connection));
    if (!connection) {
        aws_jni_throw_runtime_exception(
            env, "MqttClientConnection.mqtt_connect: Out of memory allocating JNI connection");
        return NULL;
    }

    aws_atomic_store_int(&connection->ref_count, 1);
    connection->java_mqtt_connection = (*env)->NewWeakGlobalRef(env, java_mqtt_connection);
    jint jvmresult = (*env)->GetJavaVM(env, &connection->jvm);
    AWS_FATAL_ASSERT(jvmresult == 0);

    if (client3 != NULL) {
        connection->client = client3;
        connection->client_connection = aws_mqtt_client_connection_new(client3);
    } else if (client5_jni != NULL) {
        connection->client_connection = aws_mqtt_client_connection_new_from_mqtt5_client(client5_jni->client);
    }

    if (!connection->client_connection) {
        aws_jni_throw_runtime_exception(
            env,
            "MqttClientConnection.mqtt_connect: aws_mqtt_client_connection_new failed, unable to create new "
            "connection");
        goto on_error;
    }

    if (aws_mqtt_client_connection_set_connection_termination_handler(
            connection->client_connection, s_on_connection_terminated, connection)) {
        aws_jni_throw_runtime_exception(
            env,
            "MqttClientConnection.mqtt_connect: aws_mqtt_client_connection_new failed, unable to set termination "
            "callback");
        goto on_error;
    }

    return connection;

on_error:

    s_mqtt_jni_connection_release(connection);

    return NULL;
}

static void s_mqtt_connection_destroy(JNIEnv *env, struct mqtt_jni_connection *connection) {
    if (connection == NULL) {
        return;
    }

    if (connection->on_message) {
        s_mqtt_jni_async_callback_destroy(connection->on_message, env);
    }

    if (connection->java_mqtt_connection) {
        (*env)->DeleteWeakGlobalRef(env, connection->java_mqtt_connection);
    }

    aws_tls_connection_options_clean_up(&connection->tls_options);

    struct aws_allocator *allocator = aws_jni_get_allocator();
    aws_mem_release(allocator, connection);
}

JNIEXPORT jlong JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionNewFrom311Client(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_client,
    jobject jni_mqtt_connection) {
    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = NULL;
    struct aws_mqtt_client *client3 = (struct aws_mqtt_client *)jni_client;
    if (!client3) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_new: Mqtt3 Client is invalid/null");
        return (jlong)NULL;
    }

    connection = s_mqtt_connection_new(env, client3, NULL, jni_mqtt_connection);
    if (!connection) {
        return (jlong)NULL;
    }

    aws_mqtt_client_connection_set_connection_result_handlers(
        connection->client_connection, s_on_connection_success, connection, s_on_connection_failure, connection);
    aws_mqtt_client_connection_set_connection_interruption_handlers(
        connection->client_connection, s_on_connection_interrupted, connection, s_on_connection_resumed, connection);
    aws_mqtt_client_connection_set_connection_closed_handler(
        connection->client_connection, s_on_connection_closed, connection);

    return (jlong)connection;
}

JNIEXPORT jlong JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionNewFrom5Client(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_client,
    jobject jni_mqtt_connection) {
    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = NULL;
    struct aws_mqtt5_client_java_jni *client5_jni = (struct aws_mqtt5_client_java_jni *)jni_client;
    if (!client5_jni) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_new: Mqtt5 Client is invalid/null");
        return (jlong)NULL;
    }

    connection = s_mqtt_connection_new(env, NULL, client5_jni, jni_mqtt_connection);
    if (!connection) {
        return (jlong)NULL;
    }

    aws_mqtt_client_connection_set_connection_result_handlers(
        connection->client_connection, s_on_connection_success, connection, s_on_connection_failure, connection);
    aws_mqtt_client_connection_set_connection_interruption_handlers(
        connection->client_connection, s_on_connection_interrupted, connection, s_on_connection_resumed, connection);
    aws_mqtt_client_connection_set_connection_closed_handler(
        connection->client_connection, s_on_connection_closed, connection);

    return (jlong)connection;
}

/* The disconnect callback called on shutdown. We will release the underlying connection here, which should init the
** client shutdown process. Then on termination callback, we will finally release all jni resources.
*/
static void s_on_shutdown_disconnect_complete(struct aws_mqtt_client_connection *connection, void *user_data) {
    (void)user_data;

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "mqtt_jni_connection shutdown complete, releasing references");

    /* Release the underlying mqtt connection */
    aws_mqtt_client_connection_release(connection);
}

/*******************************************************************************
 * clean_up
 ******************************************************************************/
JNIEXPORT void JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionDestroy(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_connection) {
    (void)jni_class;
    (void)env;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = (struct mqtt_jni_connection *)jni_connection;
    s_mqtt_jni_connection_destroy(connection);
}

/*******************************************************************************
 * connect
 ******************************************************************************/
JNIEXPORT
void JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionConnect(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_connection,
    jstring jni_endpoint,
    jint jni_port,
    jlong jni_socket_options,
    jlong jni_tls_ctx,
    jstring jni_client_id,
    jboolean jni_clean_session,
    jint keep_alive_secs,
    jshort ping_timeout_ms,
    jint protocol_operation_timeout_ms) {
    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = (struct mqtt_jni_connection *)jni_connection;
    if (!connection) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_connect: Connection is invalid/null");
        return;
    }

    struct aws_byte_cursor client_id;
    AWS_ZERO_STRUCT(client_id);
    struct aws_byte_cursor endpoint = aws_jni_byte_cursor_from_jstring_acquire(env, jni_endpoint);
    uint32_t port = (uint32_t)jni_port;
    if (!port) {
        aws_jni_throw_runtime_exception(
            env,
            "MqttClientConnection.mqtt_new: Endpoint should be in the format hostname:port and port must not be 0");
        goto cleanup;
    }

    struct mqtt_jni_async_callback *connect_callback = s_mqtt_jni_async_callback_new(connection, NULL, env);
    if (connect_callback == NULL) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_connect: Failed to create async callback");
        goto cleanup;
    }

    s_mqtt_jni_connection_acquire(connection);

    struct aws_socket_options default_socket_options;
    AWS_ZERO_STRUCT(default_socket_options);
    default_socket_options.type = AWS_SOCKET_STREAM;
    default_socket_options.connect_timeout_ms = 3000;
    struct aws_socket_options *socket_options = &default_socket_options;
    if (jni_socket_options) {
        socket_options = (struct aws_socket_options *)jni_socket_options;
    }
    memcpy(&connection->socket_options, socket_options, sizeof(struct aws_socket_options));

    /* if a tls_ctx was provided, initialize tls options */
    struct aws_tls_ctx *tls_ctx = (struct aws_tls_ctx *)jni_tls_ctx;
    struct aws_tls_connection_options *tls_options = NULL;
    if (tls_ctx) {
        tls_options = &connection->tls_options;
        aws_tls_connection_options_init_from_ctx(tls_options, tls_ctx);
        aws_tls_connection_options_set_server_name(tls_options, aws_jni_get_allocator(), &endpoint);
    }

    client_id = aws_jni_byte_cursor_from_jstring_acquire(env, jni_client_id);
    bool clean_session = jni_clean_session != 0;

    struct aws_mqtt_connection_options connect_options;
    AWS_ZERO_STRUCT(connect_options);
    connect_options.host_name = endpoint;
    connect_options.port = port;
    connect_options.socket_options = &connection->socket_options;
    connect_options.tls_options = tls_options;
    connect_options.client_id = client_id;
    connect_options.keep_alive_time_secs = (uint16_t)keep_alive_secs;
    connect_options.ping_timeout_ms = ping_timeout_ms;
    connect_options.protocol_operation_timeout_ms = protocol_operation_timeout_ms;
    connect_options.clean_session = clean_session;
    connect_options.on_connection_complete = s_on_connection_complete;
    connect_options.user_data = connect_callback;

    int result = aws_mqtt_client_connection_connect(connection->client_connection, &connect_options);
    if (result != AWS_OP_SUCCESS) {
        s_mqtt_jni_connection_release(connection);
        s_mqtt_jni_async_callback_destroy(connect_callback, env);
        aws_jni_throw_runtime_exception(
            env, "MqttClientConnection.mqtt_connect: aws_mqtt_client_connection_connect failed");
    }

cleanup:
    aws_jni_byte_cursor_from_jstring_release(env, jni_endpoint, endpoint);
    aws_jni_byte_cursor_from_jstring_release(env, jni_client_id, client_id);
}

/*******************************************************************************
 * disconnect
 ******************************************************************************/
JNIEXPORT
void JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionDisconnect(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_connection,
    jobject jni_ack) {
    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = (struct mqtt_jni_connection *)jni_connection;
    if (!connection) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_disconnect: Invalid connection");
        return;
    }

    struct mqtt_jni_async_callback *disconnect_callback = s_mqtt_jni_async_callback_new(connection, jni_ack, env);
    if (disconnect_callback == NULL) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_disconnect: Failed to create async callback");
        return;
    }

    if (aws_mqtt_client_connection_disconnect(
            connection->client_connection, s_on_connection_disconnected, disconnect_callback) != AWS_OP_SUCCESS) {
        int error = aws_last_error();
        /*
         * Disconnect invoked on a disconnected connection can happen under normal circumstances.  Invoke the callback
         * manually since it won't get invoked otherwise.
         */
        AWS_LOGF_WARN(
            AWS_LS_MQTT_CLIENT,
            "MqttClientConnection.mqtt_disconnect: error calling disconnect - %d(%s)",
            error,
            aws_error_str(error));
        s_on_connection_disconnected(connection->client_connection, disconnect_callback);
    }
}

/*******************************************************************************
 * subscribe
 ******************************************************************************/
/* called from any sub, unsub, or pub ack */
static void s_deliver_ack_success(struct mqtt_jni_async_callback *callback, JNIEnv *env) {
    AWS_FATAL_ASSERT(callback);
    AWS_FATAL_ASSERT(callback->connection);

    if (callback->async_callback) {
        (*env)->CallVoidMethod(env, callback->async_callback, async_callback_properties.on_success);
        AWS_FATAL_ASSERT(!aws_jni_check_and_clear_exception(env));
    }
}

static void s_deliver_ack_failure(struct mqtt_jni_async_callback *callback, int error_code, JNIEnv *env) {
    AWS_FATAL_ASSERT(callback);
    AWS_FATAL_ASSERT(callback->connection);
    AWS_FATAL_ASSERT(env);

    if (callback->async_callback) {
        jobject jni_reason = s_new_mqtt_exception(env, error_code);
        (*env)->CallVoidMethod(env, callback->async_callback, async_callback_properties.on_failure, jni_reason);
        (*env)->DeleteLocalRef(env, jni_reason);
        AWS_FATAL_ASSERT(!aws_jni_check_and_clear_exception(env));
    }
}

static void s_on_op_complete(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    int error_code,
    void *user_data) {
    AWS_FATAL_ASSERT(connection);
    (void)packet_id;

    struct mqtt_jni_async_callback *callback = user_data;
    if (!callback) {
        return;
    }

    /********** JNI ENV ACQUIRE **********/
    JavaVM *jvm = callback->connection->jvm;
    JNIEnv *env = aws_jni_acquire_thread_env(jvm);
    if (env == NULL) {
        return;
    }

    if (error_code) {
        s_deliver_ack_failure(callback, error_code, env);
    } else {
        s_deliver_ack_success(callback, env);
    }

    s_mqtt_jni_async_callback_destroy(callback, env);

    aws_jni_release_thread_env(jvm, env);
    /********** JNI ENV RELEASE **********/
}

static bool s_is_qos_successful(enum aws_mqtt_qos qos) {
    return qos < 128;
}

static void s_on_ack(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    const struct aws_byte_cursor *topic,
    enum aws_mqtt_qos qos,
    int error_code,
    void *user_data) {
    (void)topic;

    // Handle a case when the server processed SUBSCRIBE request successfully, but rejected a subscription for some
    // reason, i.e. error_code is 0 and qos is 0x80.
    // This mostly applies to mqtt5to3adapter, as MQTT3 client will be disconnected on unsuccessful subscribe.
    if (error_code == 0 && !s_is_qos_successful(qos)) {
        error_code = AWS_ERROR_MQTT_CONNECTION_SUBSCRIBE_FAILURE;
    }

    s_on_op_complete(connection, packet_id, error_code, user_data);
}

static void s_cleanup_handler(void *user_data) {
    struct mqtt_jni_async_callback *handler = user_data;

    /********** JNI ENV ACQUIRE **********/
    JavaVM *jvm = handler->connection->jvm;
    JNIEnv *env = aws_jni_acquire_thread_env(jvm);
    if (env == NULL) {
        return;
    }

    s_mqtt_jni_async_callback_destroy(handler, env);

    aws_jni_release_thread_env(jvm, env);
    /********** JNI ENV RELEASE **********/
}

static void s_on_subscription_delivered(
    struct aws_mqtt_client_connection *connection,
    const struct aws_byte_cursor *topic,
    const struct aws_byte_cursor *payload,
    bool dup,
    enum aws_mqtt_qos qos,
    bool retain,
    void *user_data) {

    AWS_FATAL_ASSERT(connection);
    AWS_FATAL_ASSERT(topic);
    AWS_FATAL_ASSERT(payload);
    AWS_FATAL_ASSERT(user_data);

    struct mqtt_jni_async_callback *callback = user_data;
    if (!callback->async_callback) {
        return;
    }

    /********** JNI ENV ACQUIRE **********/
    JNIEnv *env = aws_jni_acquire_thread_env(callback->connection->jvm);
    if (env == NULL) {
        /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
        return;
    }

    jbyteArray jni_payload = (*env)->NewByteArray(env, (jsize)payload->len);
    (*env)->SetByteArrayRegion(env, jni_payload, 0, (jsize)payload->len, (const signed char *)payload->ptr);

    jstring jni_topic = aws_jni_string_from_cursor(env, topic);

    (*env)->CallVoidMethod(
        env, callback->async_callback, message_handler_properties.deliver, jni_topic, jni_payload, dup, qos, retain);

    (*env)->DeleteLocalRef(env, jni_payload);
    (*env)->DeleteLocalRef(env, jni_topic);

    AWS_FATAL_ASSERT(!aws_jni_check_and_clear_exception(env));

    aws_jni_release_thread_env(callback->connection->jvm, env);
    /********** JNI ENV RELEASE **********/
}

JNIEXPORT
jshort JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionSubscribe(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_connection,
    jstring jni_topic,
    jint jni_qos,
    jobject jni_handler,
    jobject jni_ack) {
    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = (struct mqtt_jni_connection *)jni_connection;
    if (!connection) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_subscribe: Invalid connection");
        return 0;
    }

    struct mqtt_jni_async_callback *handler = s_mqtt_jni_async_callback_new(connection, jni_handler, env);
    if (!handler) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_subscribe: Unable to allocate handler");
        return 0;
    }

    /* from here, any failure requires error_cleanup */
    struct mqtt_jni_async_callback *sub_ack = NULL;
    if (jni_ack) {
        sub_ack = s_mqtt_jni_async_callback_new(connection, jni_ack, env);
        if (!sub_ack) {
            aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_subscribe: Unable to allocate sub ack");
            goto error_cleanup;
        }
    }

    struct aws_byte_cursor topic = aws_jni_byte_cursor_from_jstring_acquire(env, jni_topic);
    enum aws_mqtt_qos qos = jni_qos;

    uint16_t msg_id = aws_mqtt_client_connection_subscribe(
        connection->client_connection,
        &topic,
        qos,
        s_on_subscription_delivered,
        handler,
        s_cleanup_handler,
        s_on_ack,
        sub_ack);
    aws_jni_byte_cursor_from_jstring_release(env, jni_topic, topic);
    if (msg_id == 0) {
        aws_jni_throw_runtime_exception(
            env, "MqttClientConnection.mqtt_subscribe: aws_mqtt_client_connection_subscribe failed");
        goto error_cleanup;
    }

    return msg_id;

error_cleanup:
    if (handler) {
        s_mqtt_jni_async_callback_destroy(handler, env);
    }

    if (sub_ack) {
        s_mqtt_jni_async_callback_destroy(sub_ack, env);
    }

    return 0;
}

JNIEXPORT
void JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionOnMessage(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_connection,
    jobject jni_handler) {
    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = (struct mqtt_jni_connection *)jni_connection;
    if (!connection) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqttClientConnectionOnMessage: Invalid connection");
        return;
    }

    if (!jni_handler) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqttClientConnectionOnMessage: Invalid handler");
        return;
    }

    struct mqtt_jni_async_callback *handler = s_mqtt_jni_async_callback_new(connection, jni_handler, env);
    if (!handler) {
        aws_jni_throw_runtime_exception(
            env, "MqttClientConnection.mqttClientConnectionOnMessage: Unable to allocate handler");
        return;
    }

    if (aws_mqtt_client_connection_set_on_any_publish_handler(
            connection->client_connection, s_on_subscription_delivered, handler)) {
        aws_jni_throw_runtime_exception(
            env, "MqttClientConnection.mqttClientConnectionOnMessage: Failed to install on_any_publish_handler");
        goto error_cleanup;
    }

    if (connection->on_message) {
        s_mqtt_jni_async_callback_destroy(connection->on_message, env);
    }

    connection->on_message = handler;

    return;

error_cleanup:
    if (handler) {
        s_mqtt_jni_async_callback_destroy(handler, env);
    }
}

/*******************************************************************************
 * unsubscribe
 ******************************************************************************/
JNIEXPORT
jshort JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionUnsubscribe(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_connection,
    jstring jni_topic,
    jobject jni_ack) {
    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = (struct mqtt_jni_connection *)jni_connection;
    if (!connection) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_unsubscribe: Invalid connection");
        return 0;
    }

    struct mqtt_jni_async_callback *unsub_ack = s_mqtt_jni_async_callback_new(connection, jni_ack, env);
    if (!unsub_ack) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_unsubscribe: Unable to allocate unsub ack");
        goto error_cleanup;
    }

    struct aws_byte_cursor topic = aws_jni_byte_cursor_from_jstring_acquire(env, jni_topic);
    uint16_t msg_id =
        aws_mqtt_client_connection_unsubscribe(connection->client_connection, &topic, s_on_op_complete, unsub_ack);
    aws_jni_byte_cursor_from_jstring_release(env, jni_topic, topic);
    if (msg_id == 0) {
        aws_jni_throw_runtime_exception(
            env, "MqttClientConnection.mqtt_unsubscribe: aws_mqtt_client_connection_unsubscribe failed");
        goto error_cleanup;
    }

    return msg_id;

error_cleanup:
    if (unsub_ack) {
        s_mqtt_jni_async_callback_destroy(unsub_ack, env);
    }
    return 0;
}

/*******************************************************************************
 * publish
 ******************************************************************************/
JNIEXPORT
jshort JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionPublish(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_connection,
    jstring jni_topic,
    jint jni_qos,
    jboolean jni_retain,
    jbyteArray jni_payload,
    jobject jni_ack) {
    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = (struct mqtt_jni_connection *)jni_connection;
    if (!connection) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_publish: Invalid connection");
        return 0;
    }

    if (!jni_topic) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_publish: Invalid/null topic");
        return 0;
    }

    struct mqtt_jni_async_callback *pub_ack = s_mqtt_jni_async_callback_new(connection, jni_ack, env);
    if (!pub_ack) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_publish: Unable to allocate pub ack");
        goto error_cleanup;
    }

    struct aws_byte_cursor topic = aws_jni_byte_cursor_from_jstring_acquire(env, jni_topic);

    struct aws_byte_cursor payload;
    AWS_ZERO_STRUCT(payload);
    if (jni_payload != NULL) {
        payload = aws_jni_byte_cursor_from_jbyteArray_acquire(env, jni_payload);
    }

    enum aws_mqtt_qos qos = jni_qos;
    bool retain = jni_retain != 0;

    uint16_t msg_id = aws_mqtt_client_connection_publish(
        connection->client_connection, &topic, qos, retain, &payload, s_on_op_complete, pub_ack);
    aws_jni_byte_cursor_from_jstring_release(env, jni_topic, topic);

    if (jni_payload != NULL) {
        aws_jni_byte_cursor_from_jbyteArray_release(env, jni_payload, payload);
    }

    if (msg_id == 0) {
        aws_jni_throw_runtime_exception(
            env, "MqttClientConnection.mqtt_publish: aws_mqtt_client_connection_publish failed");
        goto error_cleanup;
    }

    return msg_id;

error_cleanup:
    if (pub_ack) {
        s_mqtt_jni_async_callback_destroy(pub_ack, env);
    }

    return 0;
}

JNIEXPORT jboolean JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionSetWill(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_connection,
    jstring jni_topic,
    jint jni_qos,
    jboolean jni_retain,
    jbyteArray jni_payload) {
    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = (struct mqtt_jni_connection *)jni_connection;
    if (!connection) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_set_will: Invalid connection");
        return false;
    }

    if (jni_topic == NULL) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_set_will: Topic must be non-null");
        return false;
    }
    struct aws_byte_cursor topic = aws_jni_byte_cursor_from_jstring_acquire(env, jni_topic);

    struct aws_byte_cursor payload;
    AWS_ZERO_STRUCT(payload);
    if (jni_payload != NULL) {
        payload = aws_jni_byte_cursor_from_jbyteArray_acquire(env, jni_payload);
    }

    enum aws_mqtt_qos qos = jni_qos;
    bool retain = jni_retain != 0;

    int result = aws_mqtt_client_connection_set_will(connection->client_connection, &topic, qos, retain, &payload);
    aws_jni_byte_cursor_from_jstring_release(env, jni_topic, topic);

    if (jni_payload != NULL) {
        aws_jni_byte_cursor_from_jbyteArray_release(env, jni_payload, payload);
    }

    return (result == AWS_OP_SUCCESS);
}

JNIEXPORT void JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionSetLogin(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_connection,
    jstring jni_user,
    jstring jni_pass) {
    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = (struct mqtt_jni_connection *)jni_connection;
    if (!connection) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_set_login: Invalid connection");
        return;
    }

    struct aws_byte_cursor username = aws_jni_byte_cursor_from_jstring_acquire(env, jni_user);
    struct aws_byte_cursor password;
    struct aws_byte_cursor *password_ptr = NULL;
    AWS_ZERO_STRUCT(password);
    if (jni_pass != NULL) {
        password = aws_jni_byte_cursor_from_jstring_acquire(env, jni_pass);
        password_ptr = &password;
    }

    if (aws_mqtt_client_connection_set_login(connection->client_connection, &username, password_ptr)) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_set_login: Failed to set login");
    }

    aws_jni_byte_cursor_from_jstring_release(env, jni_user, username);

    if (password.len > 0) {
        aws_jni_byte_cursor_from_jstring_release(env, jni_pass, password);
    }
}

JNIEXPORT void JNICALL
    Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionSetReconnectTimeout(
        JNIEnv *env,
        jclass jni_class,
        jlong jni_connection,
        jlong jni_min_timeout,
        jlong jni_max_timeout) {
    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = (struct mqtt_jni_connection *)jni_connection;
    if (!connection) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.mqtt_reconnect_timeout: Invalid connection");
        return;
    }

    if (aws_mqtt_client_connection_set_reconnect_timeout(
            connection->client_connection, jni_min_timeout, jni_max_timeout)) {
        aws_jni_throw_runtime_exception(
            env, "MqttClientConnection.mqtt_reconnect_timeout: Failed to set reconnect timeout");
    }
}

///////
static void s_ws_handshake_destroy(struct mqtt_jni_ws_handshake *ws_handshake) {
    if (!ws_handshake) {
        return;
    }

    s_mqtt_jni_connection_release(ws_handshake->connection);
    aws_mem_release(aws_jni_get_allocator(), ws_handshake);
}

static void s_ws_handshake_transform(
    struct aws_http_message *request,
    void *user_data,
    aws_mqtt_transform_websocket_handshake_complete_fn *complete_fn,
    void *complete_ctx) {

    struct mqtt_jni_connection *connection = user_data;

    /********** JNI ENV ACQUIRE **********/
    JNIEnv *env = aws_jni_acquire_thread_env(connection->jvm);
    if (env == NULL) {
        /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
        complete_fn(request, AWS_ERROR_INVALID_STATE, complete_ctx);
        return;
    }

    struct aws_allocator *alloc = aws_jni_get_allocator();

    struct mqtt_jni_ws_handshake *ws_handshake = aws_mem_calloc(alloc, 1, sizeof(struct mqtt_jni_ws_handshake));
    if (!ws_handshake) {
        goto error;
    }

    ws_handshake->connection = connection;
    s_mqtt_jni_connection_acquire(ws_handshake->connection);

    ws_handshake->complete_ctx = complete_ctx;
    ws_handshake->complete_fn = complete_fn;
    ws_handshake->http_request = request;

    jobject java_http_request = aws_java_http_request_from_native(env, request, NULL);
    if (!java_http_request) {
        aws_raise_error(AWS_ERROR_UNKNOWN); /* TODO: given java exception, choose appropriate aws error code */
        goto error;
    }

    jobject mqtt_connection = (*env)->NewLocalRef(env, connection->java_mqtt_connection);
    if (mqtt_connection != NULL) {
        (*env)->CallVoidMethod(
            env, mqtt_connection, mqtt_connection_properties.on_websocket_handshake, java_http_request, ws_handshake);

        (*env)->DeleteLocalRef(env, mqtt_connection);

        AWS_FATAL_ASSERT(!aws_jni_check_and_clear_exception(env));
    }

    (*env)->DeleteLocalRef(env, java_http_request);
    aws_jni_release_thread_env(connection->jvm, env);
    /********** JNI ENV RELEASE SUCCESS PATH **********/

    return;

error:;

    int error_code = aws_last_error();
    s_ws_handshake_destroy(ws_handshake);
    complete_fn(request, error_code, complete_ctx);
    aws_jni_release_thread_env(connection->jvm, env);
    /********** JNI ENV RELEASE FAILURE PATH **********/
}

JNIEXPORT void JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionUseWebsockets(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_connection) {
    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = (struct mqtt_jni_connection *)jni_connection;
    if (!connection) {
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.useWebsockets: Invalid connection");
        return;
    }

    if (aws_mqtt_client_connection_use_websockets(
            connection->client_connection, s_ws_handshake_transform, connection, NULL, NULL)) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.useWebsockets: Failed to use websockets");
        return;
    }
}

JNIEXPORT
void JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionWebsocketHandshakeComplete(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_connection,
    jbyteArray jni_marshalled_request,
    jobject jni_throwable,
    jlong jni_user_data) {
    (void)jni_class;
    (void)jni_connection;
    aws_cache_jni_ids(env);

    struct mqtt_jni_ws_handshake *ws_handshake = (void *)jni_user_data;
    int error_code = AWS_ERROR_SUCCESS;

    if (jni_throwable != NULL) {
        if ((*env)->IsInstanceOf(env, jni_throwable, crt_runtime_exception_properties.crt_runtime_exception_class)) {
            error_code = (*env)->GetIntField(env, jni_throwable, crt_runtime_exception_properties.error_code_field_id);
        }

        if (error_code == AWS_ERROR_SUCCESS) {
            error_code = AWS_ERROR_UNKNOWN; /* is there anything more that could be done here? */
        }

        goto done;
    }

    if (aws_apply_java_http_request_changes_to_native_request(
            env, jni_marshalled_request, NULL, ws_handshake->http_request)) {
        error_code = aws_last_error();
        goto done;
    }

done:
    ws_handshake->complete_fn(ws_handshake->http_request, error_code, ws_handshake->complete_ctx);
    s_ws_handshake_destroy(ws_handshake);
}

JNIEXPORT
void JNICALL Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionSetHttpProxyOptions(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_connection,
    jint jni_proxy_connection_type,
    jstring jni_proxy_host,
    jint jni_proxy_port,
    jlong jni_proxy_tls_context,
    jint jni_proxy_authorization_type,
    jstring jni_proxy_authorization_username,
    jstring jni_proxy_authorization_password) {

    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = (struct mqtt_jni_connection *)jni_connection;

    struct aws_http_proxy_options proxy_options;
    AWS_ZERO_STRUCT(proxy_options);

    if (!jni_proxy_host) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.setHttpProxyOptions: proxyHost must not be null.");
        return;
    }

    proxy_options.connection_type = (enum aws_http_proxy_connection_type)jni_proxy_connection_type;

    proxy_options.host = aws_jni_byte_cursor_from_jstring_acquire(env, jni_proxy_host);
    proxy_options.port = (uint32_t)jni_proxy_port;

    proxy_options.auth_type = (enum aws_http_proxy_authentication_type)jni_proxy_authorization_type;

    if (jni_proxy_authorization_username) {
        proxy_options.auth_username = aws_jni_byte_cursor_from_jstring_acquire(env, jni_proxy_authorization_username);
    }

    if (jni_proxy_authorization_password) {
        proxy_options.auth_password = aws_jni_byte_cursor_from_jstring_acquire(env, jni_proxy_authorization_password);
    }

    struct aws_tls_connection_options proxy_tls_conn_options;
    AWS_ZERO_STRUCT(proxy_tls_conn_options);

    if (jni_proxy_tls_context != 0) {
        struct aws_tls_ctx *proxy_tls_ctx = (struct aws_tls_ctx *)jni_proxy_tls_context;
        aws_tls_connection_options_init_from_ctx(&proxy_tls_conn_options, proxy_tls_ctx);
        aws_tls_connection_options_set_server_name(
            &proxy_tls_conn_options, aws_jni_get_allocator(), &proxy_options.host);
        proxy_options.tls_options = &proxy_tls_conn_options;
    }

    if (aws_mqtt_client_connection_set_http_proxy_options(connection->client_connection, &proxy_options)) {
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.setHttpProxyOptions: Failed to set proxy options");
    }

    if (jni_proxy_authorization_password) {
        aws_jni_byte_cursor_from_jstring_release(env, jni_proxy_authorization_password, proxy_options.auth_password);
    }

    if (jni_proxy_authorization_username) {
        aws_jni_byte_cursor_from_jstring_release(env, jni_proxy_authorization_username, proxy_options.auth_username);
    }

    aws_jni_byte_cursor_from_jstring_release(env, jni_proxy_host, proxy_options.host);
    aws_tls_connection_options_clean_up(&proxy_tls_conn_options);
}

JNIEXPORT jobject JNICALL
    Java_software_amazon_awssdk_crt_mqtt_MqttClientConnection_mqttClientConnectionGetOperationStatistics(
        JNIEnv *env,
        jclass jni_class,
        jlong jni_connection) {
    (void)jni_class;
    aws_cache_jni_ids(env);

    struct mqtt_jni_connection *connection = (struct mqtt_jni_connection *)jni_connection;
    if (!connection) {
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        aws_jni_throw_runtime_exception(env, "MqttClientConnection.getOperationStatistics: Invalid connection");
        return NULL;
    }

    /* Construct Java object */
    jobject jni_operation_statistics = (*env)->NewObject(
        env,
        mqtt_connection_operation_statistics_properties.statistics_class,
        mqtt_connection_operation_statistics_properties.statistics_constructor_id);
    if (jni_operation_statistics == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        aws_jni_throw_runtime_exception(
            env, "MqttClientConnection.getOperationStatistics: Could not create operation statistics object");
        return NULL;
    }

    struct aws_mqtt_connection_operation_statistics connection_stats;
    aws_mqtt_client_connection_get_stats(connection->client_connection, &connection_stats);

    (*env)->SetLongField(
        env,
        jni_operation_statistics,
        mqtt_connection_operation_statistics_properties.incomplete_operation_count_field_id,
        (jlong)connection_stats.incomplete_operation_count);
    if (aws_jni_check_and_clear_exception(env)) {
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        aws_jni_throw_runtime_exception(
            env, "MqttClientConnection.getOperationStatistics: could not create incomplete operation count");
        return NULL;
    }

    (*env)->SetLongField(
        env,
        jni_operation_statistics,
        mqtt_connection_operation_statistics_properties.incomplete_operation_size_field_id,
        (jlong)connection_stats.incomplete_operation_size);
    if (aws_jni_check_and_clear_exception(env)) {
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        aws_jni_throw_runtime_exception(
            env, "MqttClientConnection.getOperationStatistics: could not create incomplete operation size");
        return NULL;
    }

    (*env)->SetLongField(
        env,
        jni_operation_statistics,
        mqtt_connection_operation_statistics_properties.unacked_operation_count_field_id,
        (jlong)connection_stats.unacked_operation_count);
    if (aws_jni_check_and_clear_exception(env)) {
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        aws_jni_throw_runtime_exception(
            env, "MqttClientConnection.getOperationStatistics: could not create unacked operation count");
        return NULL;
    }

    (*env)->SetLongField(
        env,
        jni_operation_statistics,
        mqtt_connection_operation_statistics_properties.unacked_operation_size_field_id,
        (jlong)connection_stats.unacked_operation_size);
    if (aws_jni_check_and_clear_exception(env)) {
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        aws_jni_throw_runtime_exception(
            env, "MqttClientConnection.getOperationStatistics: could not create unacked operation size");
        return NULL;
    }

    return jni_operation_statistics;
}

#if UINTPTR_MAX == 0xffffffff
#    if defined(_MSC_VER)
#        pragma warning(pop)
#    else
#        pragma GCC diagnostic pop
#    endif
#endif
