/* Host tests for the shared-key AEAD layer. Compiled against the real
 * vendored Monocypher, the same source file the board runs, so a signature
 * mismatch or a tampering bug shows up here instead of on hardware. */

#include "fcrypto.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                           \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static void section(const char *n) { printf("%s\n", n); }

static void test_derive_is_deterministic(void) {
    section("key derivation is deterministic and passphrase sensitive");

    uint8_t a[FCRYPTO_KEY_LEN], b[FCRYPTO_KEY_LEN], c[FCRYPTO_KEY_LEN];
    fcrypto_derive_key("north gate blue door", 21, a);
    fcrypto_derive_key("north gate blue door", 21, b);
    fcrypto_derive_key("north gate blue Door", 21, c);   /* one bit different */

    CHECK(memcmp(a, b, sizeof a) == 0, "the same passphrase must derive the same key");
    CHECK(memcmp(a, c, sizeof a) != 0, "a different passphrase must derive a different key");
}

static void test_fingerprint_is_stable_and_distinguishes(void) {
    section("key fingerprint");

    uint8_t k1[FCRYPTO_KEY_LEN], k2[FCRYPTO_KEY_LEN];
    fcrypto_derive_key("alpha", 5, k1);
    fcrypto_derive_key("beta", 4, k2);

    char f1a[16], f1b[16], f2[16];
    fcrypto_key_fingerprint(k1, f1a, sizeof f1a);
    fcrypto_key_fingerprint(k1, f1b, sizeof f1b);
    fcrypto_key_fingerprint(k2, f2, sizeof f2);

    CHECK(strcmp(f1a, f1b) == 0, "the same key must always print the same fingerprint");
    CHECK(strcmp(f1a, f2) != 0, "different keys should print different fingerprints");
    CHECK(strlen(f1a) == 8, "fingerprint is 4 bytes as 8 hex characters, got %zu", strlen(f1a));
}

static void test_roundtrip(void) {
    section("encrypt then decrypt recovers the plaintext");

    uint8_t key[FCRYPTO_KEY_LEN];
    fcrypto_derive_key("shared network key", 19, key);

    uint8_t nonce[FCRYPTO_NONCE_LEN];
    for (int i = 0; i < FCRYPTO_NONCE_LEN; i++) nonce[i] = (uint8_t)(i * 7);

    const char *msg = "bring the small antenna if you still have it";
    const size_t len = strlen(msg);

    uint8_t cipher[128], tag[FCRYPTO_TAG_LEN], plain[128];
    fcrypto_encrypt(key, nonce, (const uint8_t *)msg, len, cipher, tag);

    CHECK(memcmp(cipher, msg, len) != 0, "ciphertext must not equal the plaintext");

    const bool ok = fcrypto_decrypt(key, nonce, cipher, len, tag, plain);
    CHECK(ok, "decryption with the right key and nonce should succeed");
    CHECK(memcmp(plain, msg, len) == 0, "decrypted text should match the original exactly");
}

static void test_wrong_key_fails(void) {
    section("the wrong key is rejected, not silently garbled");

    uint8_t key[FCRYPTO_KEY_LEN], wrong[FCRYPTO_KEY_LEN];
    fcrypto_derive_key("correct horse", 13, key);
    fcrypto_derive_key("incorrect horse", 15, wrong);

    uint8_t nonce[FCRYPTO_NONCE_LEN] = {0};
    const char *msg = "on my way";
    uint8_t cipher[32], tag[FCRYPTO_TAG_LEN], plain[32];

    fcrypto_encrypt(key, nonce, (const uint8_t *)msg, strlen(msg), cipher, tag);

    memset(plain, 0xAA, sizeof plain);
    const bool ok = fcrypto_decrypt(wrong, nonce, cipher, strlen(msg), tag, plain);
    CHECK(!ok, "the wrong key must be rejected");
    CHECK(plain[0] == 0, "rejected output must be wiped, not left as whatever was there");
}

static void test_tampering_detected(void) {
    section("a flipped bit anywhere is caught");

    uint8_t key[FCRYPTO_KEY_LEN];
    fcrypto_derive_key("tamper test", 11, key);
    uint8_t nonce[FCRYPTO_NONCE_LEN] = {0};

    const char *msg = "the passphrase is on the whiteboard";
    const size_t len = strlen(msg);
    uint8_t cipher[64], tag[FCRYPTO_TAG_LEN], plain[64];
    fcrypto_encrypt(key, nonce, (const uint8_t *)msg, len, cipher, tag);

    uint8_t bad_cipher[64];
    memcpy(bad_cipher, cipher, len);
    bad_cipher[3] ^= 0x01;
    CHECK(!fcrypto_decrypt(key, nonce, bad_cipher, len, tag, plain),
          "a single flipped ciphertext bit must be rejected");

    uint8_t bad_tag[FCRYPTO_TAG_LEN];
    memcpy(bad_tag, tag, sizeof bad_tag);
    bad_tag[0] ^= 0x01;
    CHECK(!fcrypto_decrypt(key, nonce, cipher, len, bad_tag, plain),
          "a single flipped tag bit must be rejected");

    uint8_t bad_nonce[FCRYPTO_NONCE_LEN];
    memcpy(bad_nonce, nonce, sizeof bad_nonce);
    bad_nonce[0] ^= 0x01;
    CHECK(!fcrypto_decrypt(key, bad_nonce, cipher, len, tag, plain),
          "the wrong nonce must be rejected");
}

static void test_nonce_reuse_is_visibly_dangerous(void) {
    section("reusing a nonce still decrypts, which is why net.c must never do it");

    /* This is not a bug to fix in fcrypto: reuse is a caller error, and a
     * stream cipher's whole contract is that a nonce is used once. Documented
     * here as a property test so the danger is explicit rather than folklore,
     * and to prove two different messages under one key and nonce each still
     * decrypt correctly on their own, which is the property net.c relies on
     * as long as it keeps generating a fresh nonce per message. */
    uint8_t key[FCRYPTO_KEY_LEN];
    fcrypto_derive_key("nonce discipline", 16, key);
    uint8_t nonce[FCRYPTO_NONCE_LEN] = {0};

    const char *a = "first message";
    const char *b = "second, different message";
    uint8_t ca[64], ta[FCRYPTO_TAG_LEN], pa[64];
    uint8_t cb[64], tb[FCRYPTO_TAG_LEN], pb[64];

    fcrypto_encrypt(key, nonce, (const uint8_t *)a, strlen(a), ca, ta);
    fcrypto_encrypt(key, nonce, (const uint8_t *)b, strlen(b), cb, tb);

    CHECK(fcrypto_decrypt(key, nonce, ca, strlen(a), ta, pa) && memcmp(pa, a, strlen(a)) == 0,
          "message a still decrypts correctly");
    CHECK(fcrypto_decrypt(key, nonce, cb, strlen(b), tb, pb) && memcmp(pb, b, strlen(b)) == 0,
          "message b still decrypts correctly");
}

int main(void) {
    printf("fcrypto tests\n\n");

    test_derive_is_deterministic();
    test_fingerprint_is_stable_and_distinguishes();
    test_roundtrip();
    test_wrong_key_fails();
    test_tampering_detected();
    test_nonce_reuse_is_visibly_dangerous();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
