// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the COBS framing used by the badgelink UART transport. Pure
// byte manipulation with no ESP dependency, and the one place where a wrong
// length byte silently corrupts a whole frame, so it is worth pinning.

#include "cobs.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                           \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                            \
        }                                                          \
    } while (0)

// Encode then decode; the result must equal the input and the encoded form must
// contain no interior zero (that is the whole point of COBS: the single trailing
// zero is the frame delimiter).
static void roundtrip(uint8_t const *in, size_t len, char const *what) {
    uint8_t enc[COBS_ENCODED_MAX_LENGTH(2048)];
    uint8_t dec[2048];

    size_t enc_len = cobs_encode(enc, in, len);
    CHECK(enc_len <= COBS_ENCODED_MAX_LENGTH(len), what);
    CHECK(enc_len >= 1 && enc[enc_len - 1] == 0, what);
    for (size_t i = 0; i + 1 < enc_len; i++) {
        if (enc[i] == 0) {
            printf("FAIL: %s: interior zero at %zu\n", what, i);
            failures++;
            break;
        }
    }

    size_t dec_len = cobs_decode(dec, enc, enc_len);
    if (dec_len != len) {
        printf("FAIL: %s: decoded %zu bytes, expected %zu\n", what, dec_len, len);
        failures++;
        return;
    }
    if (len && memcmp(dec, in, len) != 0) {
        printf("FAIL: %s: payload differs after round trip\n", what);
        failures++;
    }
}

int main(void) {
    uint8_t buf[2048];

    uint8_t const one[] = {0x01};
    roundtrip(one, sizeof(one), "single non-zero byte");

    uint8_t const zero[] = {0x00};
    roundtrip(zero, sizeof(zero), "single zero byte");

    uint8_t const zeros[] = {0, 0, 0, 0};
    roundtrip(zeros, sizeof(zeros), "all zeroes");

    uint8_t const mixed[] = {0x11, 0x00, 0x22, 0x00, 0x00, 0x33};
    roundtrip(mixed, sizeof(mixed), "zeroes between data");

    uint8_t const lead_trail[] = {0x00, 0x41, 0x42, 0x00};
    roundtrip(lead_trail, sizeof(lead_trail), "leading and trailing zero");

    // 253, 254 and 255 non-zero bytes straddle the overhead-byte boundary, which
    // is where an off-by-one in the encoder shows up.
    for (size_t n = 252; n <= 256; n++) {
        memset(buf, 0xAB, n);
        char what[48];
        snprintf(what, sizeof(what), "%zu non-zero bytes", n);
        roundtrip(buf, n, what);
    }

    // Same boundary, but with a zero right after the run.
    memset(buf, 0xCD, 254);
    buf[254] = 0x00;
    buf[255] = 0xEF;
    roundtrip(buf, 256, "run of 254 then a zero");

    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 7 + (i >> 3));
    }
    roundtrip(buf, sizeof(buf), "2 KB of mixed data");

    // An empty payload still produces a delimited frame that decodes to nothing.
    uint8_t enc[8];
    size_t  enc_len = cobs_encode(enc, buf, 0);
    CHECK(enc_len >= 1 && enc[enc_len - 1] == 0, "empty input is still delimited");
    uint8_t dec[8];
    CHECK(cobs_decode(dec, enc, enc_len) == 0, "empty input decodes to zero bytes");

    // cobs_decode returns 0 rather than reading anything when handed nothing.
    CHECK(cobs_decode(dec, enc, 0) == 0, "zero-length input is rejected");

    if (failures == 0) {
        printf("test_cobs: all checks passed\n");
        return 0;
    }
    printf("test_cobs: %d failure(s)\n", failures);
    return 1;
}
