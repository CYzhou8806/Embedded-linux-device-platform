/*
 * Device authentication for the SPI register protocol (security work B6,
 * docs/security/device-authentication.md).
 *
 * The Raspberry Pi writes a 16-byte nonce and asks for a MAC; the MCU
 * answers with the first 16 bytes of
 *
 *   HMAC-SHA256(K, "acq-auth-v1" || DEVICE_ID || FW_VERSION || nonce)
 *
 * (DEVICE_ID and FW_VERSION as big-endian 32-bit words). K is 32 bytes in the
 * last flash page, written at pairing; an erased page means "no key".
 *
 * No HAL in here: the same file is compiled on the development host and
 * checked against Python's hmac module (tests/devauth_test.c).
 */
#ifndef DEVAUTH_H
#define DEVAUTH_H

#include <stddef.h>
#include <stdint.h>

#define DEVAUTH_KEY_LEN   32u
#define DEVAUTH_NONCE_LEN 16u
#define DEVAUTH_MAC_LEN   16u /* truncated HMAC-SHA256 */
#define DEVAUTH_DOMAIN    "acq-auth-v1"

void sha256(const uint8_t *msg, size_t len, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len, uint8_t out[32]);

/* Returns 0 if the key is erased (all 0xFF), 1 otherwise. */
int devauth_key_present(const uint8_t key[DEVAUTH_KEY_LEN]);

/* mac = first DEVAUTH_MAC_LEN bytes of the HMAC above. */
void devauth_compute(const uint8_t key[DEVAUTH_KEY_LEN],
                     uint32_t device_id, uint32_t fw_version,
                     const uint8_t nonce[DEVAUTH_NONCE_LEN],
                     uint8_t mac[DEVAUTH_MAC_LEN]);

#endif /* DEVAUTH_H */
