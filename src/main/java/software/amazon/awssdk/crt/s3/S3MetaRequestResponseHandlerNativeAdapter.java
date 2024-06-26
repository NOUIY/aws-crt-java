/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
package software.amazon.awssdk.crt.s3;

import software.amazon.awssdk.crt.http.HttpHeader;

import java.nio.ByteBuffer;

class S3MetaRequestResponseHandlerNativeAdapter {
    private S3MetaRequestResponseHandler responseHandler;

    S3MetaRequestResponseHandlerNativeAdapter(S3MetaRequestResponseHandler responseHandler) {
        this.responseHandler = responseHandler;
    }

    int onResponseBody(byte[] bodyBytesIn, long objectRangeStart, long objectRangeEnd) {
        return this.responseHandler.onResponseBody(ByteBuffer.wrap(bodyBytesIn), objectRangeStart, objectRangeEnd);
    }

    void onFinished(int errorCode, int responseStatus, byte[] errorPayload, String errorOperationName, int checksumAlgorithm, boolean didValidateChecksum, Throwable cause, final ByteBuffer headersBlob) {
        HttpHeader[] errorHeaders = headersBlob == null ? null : HttpHeader.loadHeadersFromMarshalledHeadersBlob(headersBlob);
        S3FinishedResponseContext context = new S3FinishedResponseContext(errorCode, responseStatus, errorPayload, errorOperationName, ChecksumAlgorithm.getEnumValueFromInteger(checksumAlgorithm), didValidateChecksum, cause, errorHeaders);
        this.responseHandler.onFinished(context);
    }

    void onResponseHeaders(final int statusCode, final ByteBuffer headersBlob) {
        responseHandler.onResponseHeaders(statusCode, HttpHeader.loadHeadersFromMarshalledHeadersBlob(headersBlob));
    }

    void onProgress(final S3MetaRequestProgress progress) {
        responseHandler.onProgress(progress);
    }
}
