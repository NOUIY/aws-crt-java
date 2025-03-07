/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
package software.amazon.awssdk.crt.s3;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.Optional;
import java.util.List;

public enum ChecksumAlgorithm {

    NONE(0),

    CRC32C(1),

    CRC32(2),

    SHA1(3),

    SHA256(4),

    CRC64NVME(5);

    ChecksumAlgorithm(int nativeValue) {
        this.nativeValue = nativeValue;
    }

    public int getNativeValue() {
        return nativeValue;
    }

    public static ChecksumAlgorithm getEnumValueFromInteger(int value) {
        ChecksumAlgorithm enumValue = enumMapping.get(value);
        if (enumValue != null) {
            return enumValue;
        }

        throw new RuntimeException("Invalid S3 Meta Request type");
    }

    private static Map<Integer, ChecksumAlgorithm> buildEnumMapping() {
        Map<Integer, ChecksumAlgorithm> enumMapping = new HashMap<Integer, ChecksumAlgorithm>();
        enumMapping.put(NONE.getNativeValue(), NONE);
        enumMapping.put(CRC32C.getNativeValue(), CRC32C);
        enumMapping.put(CRC32.getNativeValue(), CRC32);
        enumMapping.put(SHA1.getNativeValue(), SHA1);
        enumMapping.put(SHA256.getNativeValue(), SHA256);
        enumMapping.put(CRC64NVME.getNativeValue(), CRC64NVME);
        return enumMapping;
    }

    private int nativeValue;

    private static Map<Integer, ChecksumAlgorithm> enumMapping = buildEnumMapping();

    private static int[] algorithmsListToIntArray(final List<ChecksumAlgorithm> algorithms) {
        ArrayList<Integer> intList = new ArrayList<>();
        algorithms.forEach((algorithm) -> {
            intList.add(algorithm.getNativeValue());
        });
        return intList.stream()
                .mapToInt(Integer::intValue)
                .toArray();
    }

    /**
     * @hidden Marshals a list of algorithm into an array for Jni to deal with
     *
     * @param algorithms list of algorithms
     * @return a int[] that with the [algorithms.nativeValue, *]
     */
    public static int[] marshallAlgorithmsForJNI(final List<ChecksumAlgorithm> algorithms) {
        return Optional.ofNullable(algorithms).map(ChecksumAlgorithm::algorithmsListToIntArray).orElse(null);
    }

}
