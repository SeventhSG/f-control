# Vendored crypto

**Monocypher 4.0.2**, from https://github.com/LoupVaillant/Monocypher, tag `4.0.2`, files `src/monocypher.c` and `src/monocypher.h`. Dual licensed CC0 and BSD 2 clause. Checksums are in `SHA256SUMS` and are verified in CI so a silent substitution cannot happen.

## Why this and not mbedtls

The spec originally named mbedtls, which ships inside ESP-IDF. That would have made `fcrypto`, the single most security critical component in the project, the only one that could not be compiled or tested without the ESP32 toolchain. Every bug in it would have been found on hardware, late, by hand.

Monocypher is one C file with no dependencies. It compiles for the host and for the chip from the same source, so the handshake is tested by the same suite that tests the wire codec, on every commit, with an address sanitizer attached.

## What changes in the spec

The cipher suite moves from `Noise_XX_25519_AESGCM_SHA256` to **`Noise_XX_25519_ChaChaPoly_BLAKE2b`**. Both are standard Noise suites with published test vectors.

- **ChaCha20-Poly1305 instead of AES-256-GCM.** ESP32 has hardware AES, which sounds like a reason to prefer it, and is not one here. The largest thing this protocol ever encrypts is 180 bytes. ChaCha20 in software on a 240 MHz core is orders of magnitude faster than the radio can carry, so the hardware advantage buys nothing measurable. In exchange, ChaCha20-Poly1305 is constant time in software by construction, with no cache timing behaviour to reason about.
- **BLAKE2b instead of SHA-256.** Monocypher provides BLAKE2b, and it is one of the hash functions the Noise specification defines. There is no security argument between them at this size, only availability.

## What is not vendored

The Noise XX handshake itself is implemented in `fcrypto`, against these primitives. Writing a handshake by hand is a real risk, which is why it is validated against the official Noise test vectors rather than against its own round trip. A handshake that only agrees with itself agrees with nothing.
