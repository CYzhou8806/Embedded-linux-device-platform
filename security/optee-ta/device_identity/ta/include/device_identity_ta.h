/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Interface of the device identity TA, shared by the TA and its normal-world
 * client. See docs/security/optee.md.
 */
#ifndef DEVICE_IDENTITY_TA_H
#define DEVICE_IDENTITY_TA_H

/* 564401c1-f0e8-4dfd-9c2f-d602e04b7232 */
#define TA_DEVICE_IDENTITY_UUID \
	{ 0x564401c1, 0xf0e8, 0x4dfd, \
		{ 0x9c, 0x2f, 0xd6, 0x02, 0xe0, 0x4b, 0x72, 0x32 } }

/*
 * Generate the device key. Fails with TEE_ERROR_ACCESS_CONFLICT if one
 * already exists: an identity is created once, at provisioning, never
 * silently replaced.
 *   [out] memref  public key, uncompressed SEC1: 0x04 || X || Y (65 bytes)
 */
#define TA_DEVID_CMD_PROVISION		0

/*
 * [out] memref  public key, same format as PROVISION.
 */
#define TA_DEVID_CMD_GET_PUBKEY		1

/*
 * Answer a challenge. The TA signs SHA-256(DEVID_CHALLENGE_DOMAIN || nonce),
 * never a caller-supplied digest, so it can't be used to sign anything that
 * isn't an identity challenge (an update, a certificate request...).
 *   [in]  memref  nonce, DEVID_NONCE_MIN..DEVID_NONCE_MAX bytes
 *   [out] memref  ECDSA P-256 signature, r || s (64 bytes)
 */
#define TA_DEVID_CMD_SIGN_CHALLENGE	2

/*
 * Demonstration only: tries to read the private scalar back out of the
 * stored key. The key is not TEE_USAGE_EXTRACTABLE, so the GP spec requires
 * the TA to panic; the client sees TEE_ERROR_TARGET_DEAD. Exists to show
 * the property, not to be called in normal operation.
 */
#define TA_DEVID_CMD_DEMO_EXPORT_PRIVATE	3

#define DEVID_PUBKEY_LEN	65
#define DEVID_SIG_LEN		64
#define DEVID_NONCE_MIN		16
#define DEVID_NONCE_MAX		64
#define DEVID_CHALLENGE_DOMAIN	"device-platform/devid-challenge/v1"

#endif /* DEVICE_IDENTITY_TA_H */
