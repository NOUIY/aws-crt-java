/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

package software.amazon.awssdk.crt.test;

import org.junit.Assert;
import org.junit.Assume;
import org.junit.Test;
import static org.junit.Assert.fail;
import org.junit.Rule;
import org.junit.rules.Timeout;

import software.amazon.awssdk.crt.CrtResource;
import software.amazon.awssdk.crt.io.TlsContext;
import software.amazon.awssdk.crt.io.TlsContextOptions;
import software.amazon.awssdk.crt.mqtt.*;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;


/* For environment variable setup, see SetupCrossCICrtEnvironment in the CRT builder */
public class PublishTest extends MqttClientConnectionFixture {
    private final static int MAX_TEST_RETRIES = 3;
    private final static int TEST_RETRY_SLEEP_MILLIS = 2000;

    @Rule
    public Timeout testTimeout = Timeout.seconds(15);

    public PublishTest() {
    }

    final String TEST_TOPIC = "publish/me/senpai/" + (UUID.randomUUID()).toString();
    static final String TEST_PAYLOAD = "PUBLISH ME! SHINY AND CHROME!";
    static final String EMPTY_PAYLOAD = "";

    final Lock receivedLock = new ReentrantLock();
    final Condition receivedSignal  = receivedLock.newCondition();

    ArrayList<MqttMessage> receivedMessages = new ArrayList<>();

    private void onPublishHandler(MqttMessage message) {
        receivedLock.lock();
        receivedMessages.add(message);
        receivedSignal.signal();
        receivedLock.unlock();
    }

    private void subscribe() {
        try {
            CompletableFuture<Integer> subscribed = connection.subscribe(TEST_TOPIC, QualityOfService.AT_LEAST_ONCE,
                    this::onPublishHandler);

            subscribed.get();
        } catch (Exception ex) {
            fail(ex.getMessage());
        }
    }

    private void publishAndCheck(byte[] payload) {
        try {
            MqttMessage message = new MqttMessage(TEST_TOPIC, payload, QualityOfService.AT_LEAST_ONCE);
            CompletableFuture<Integer> published = connection.publish(message);
            published.get();

            // test time out will break us out of this on failure
            receivedLock.lock();
            while(receivedMessages.size() == 0) {
                receivedSignal.await();
            }

            MqttMessage received = receivedMessages.get(0);
            Assert.assertEquals(
                    Arrays.toString(message.getPayload()),
                    Arrays.toString(payload));

            receivedLock.unlock();
        } catch (Exception ex) {
            fail(ex.getMessage());
        }
    }

    private void doRoundTripTest() {
        try (TlsContextOptions contextOptions = TlsContextOptions.createWithMtlsFromPath(
                AWS_TEST_MQTT311_IOT_CORE_RSA_CERT,
                AWS_TEST_MQTT311_IOT_CORE_RSA_KEY);
             TlsContext context = new TlsContext(contextOptions)) {
            connectDirect(
                context,
                AWS_TEST_MQTT311_IOT_CORE_HOST,
                8883,
                null,
                null,
                null);
            subscribe();
            publishAndCheck(TEST_PAYLOAD.getBytes());
            disconnect();
        } catch (Exception ex) {
            throw new RuntimeException(ex);
        } finally {
            close();
        }
    }

    @Test
    public void testRoundTrip() throws Exception {
        skipIfNetworkUnavailable();
        Assume.assumeNotNull(AWS_TEST_MQTT311_IOT_CORE_HOST, AWS_TEST_MQTT311_IOT_CORE_RSA_KEY, AWS_TEST_MQTT311_IOT_CORE_RSA_CERT);

        TestUtils.doRetryableTest(this::doRoundTripTest, TestUtils::isRetryableTimeout, MAX_TEST_RETRIES, TEST_RETRY_SLEEP_MILLIS);

        CrtResource.waitForNoResources();
    }

    private void doEmptyRoundTripTest() {
        try (TlsContextOptions contextOptions = TlsContextOptions.createWithMtlsFromPath(
                AWS_TEST_MQTT311_IOT_CORE_RSA_CERT,
                AWS_TEST_MQTT311_IOT_CORE_RSA_KEY);
             TlsContext context = new TlsContext(contextOptions)) {
            connectDirect(
                context,
                AWS_TEST_MQTT311_IOT_CORE_HOST,
                8883,
                null,
                null,
                null);
            subscribe();
            publishAndCheck(EMPTY_PAYLOAD.getBytes());
            disconnect();
        } catch (Exception ex) {
            throw new RuntimeException(ex);
        } finally {
            close();
        }
    }

    @Test
    public void testEmptyRoundTrip() throws Exception {
        skipIfNetworkUnavailable();
        Assume.assumeNotNull(AWS_TEST_MQTT311_IOT_CORE_HOST, AWS_TEST_MQTT311_IOT_CORE_RSA_KEY, AWS_TEST_MQTT311_IOT_CORE_RSA_CERT);

        TestUtils.doRetryableTest(this::doEmptyRoundTripTest, TestUtils::isRetryableTimeout, MAX_TEST_RETRIES, TEST_RETRY_SLEEP_MILLIS);

        CrtResource.waitForNoResources();
    }

    private void doNullRoundTripTest() {
        try (TlsContextOptions contextOptions = TlsContextOptions.createWithMtlsFromPath(
                AWS_TEST_MQTT311_IOT_CORE_RSA_CERT,
                AWS_TEST_MQTT311_IOT_CORE_RSA_KEY);
             TlsContext context = new TlsContext(contextOptions)) {
            connectDirect(
                context,
                AWS_TEST_MQTT311_IOT_CORE_HOST,
                8883,
                null,
                null,
                null);
            subscribe();
            publishAndCheck(null);
            disconnect();
        } catch (Exception ex) {
            throw new RuntimeException(ex);
        } finally {
            close();
        }
    }

    @Test
    public void testNullRoundTrip() throws Exception {
        skipIfNetworkUnavailable();
        Assume.assumeNotNull(AWS_TEST_MQTT311_IOT_CORE_HOST, AWS_TEST_MQTT311_IOT_CORE_RSA_KEY, AWS_TEST_MQTT311_IOT_CORE_RSA_CERT);

        TestUtils.doRetryableTest(this::doNullRoundTripTest, TestUtils::isRetryableTimeout, MAX_TEST_RETRIES, TEST_RETRY_SLEEP_MILLIS);

        CrtResource.waitForNoResources();
    }
};
