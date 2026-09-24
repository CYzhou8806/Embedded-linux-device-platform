// SPDX-License-Identifier: BSD-2-Clause
/*
 * Device identity TA: holds one ECDSA P-256 key in OP-TEE secure storage and
 * answers "prove you are this device" challenges with it. The private key is
 * generated inside the TA, stored without TEE_USAGE_EXTRACTABLE, and never
 * crosses into the normal world - not even to this TA's own code as bytes.
 *
 * See docs/security/optee.md for where this sits in the threat model (F12).
 */
#include <string.h>
#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>

#include <device_identity_ta.h>

#define KEY_BITS	256

static const char key_object_id[] = "devid-key-v1";

TEE_Result TA_CreateEntryPoint(void)
{
	return TEE_SUCCESS;
}

void TA_DestroyEntryPoint(void)
{
}

TEE_Result TA_OpenSessionEntryPoint(uint32_t param_types __unused,
				    TEE_Param params[4] __unused,
				    void **sess_ctx __unused)
{
	return TEE_SUCCESS;
}

void TA_CloseSessionEntryPoint(void *sess_ctx __unused)
{
}

static TEE_Result open_key(TEE_ObjectHandle *key)
{
	return TEE_OpenPersistentObject(TEE_STORAGE_PRIVATE, key_object_id,
					sizeof(key_object_id) - 1,
					TEE_DATA_FLAG_ACCESS_READ, key);
}

/* Public attributes are readable whatever the object's usage flags are. */
static TEE_Result export_pubkey(TEE_ObjectHandle key, TEE_Param *out)
{
	uint8_t *buf = out->memref.buffer;
	uint8_t x[KEY_BITS / 8] = { 0 };
	uint8_t y[KEY_BITS / 8] = { 0 };
	size_t x_len = sizeof(x);
	size_t y_len = sizeof(y);
	TEE_Result res = TEE_ERROR_GENERIC;

	if (out->memref.size < DEVID_PUBKEY_LEN) {
		out->memref.size = DEVID_PUBKEY_LEN;
		return TEE_ERROR_SHORT_BUFFER;
	}

	res = TEE_GetObjectBufferAttribute(key, TEE_ATTR_ECC_PUBLIC_VALUE_X,
					   x, &x_len);
	if (res)
		return res;
	res = TEE_GetObjectBufferAttribute(key, TEE_ATTR_ECC_PUBLIC_VALUE_Y,
					   y, &y_len);
	if (res)
		return res;
	if (x_len > sizeof(x) || y_len > sizeof(y))
		return TEE_ERROR_GENERIC;

	/* Big-endian coordinates may come back without leading zero bytes. */
	memset(buf, 0, DEVID_PUBKEY_LEN);
	buf[0] = 0x04;
	TEE_MemMove(buf + 1 + sizeof(x) - x_len, x, x_len);
	TEE_MemMove(buf + 1 + sizeof(x) + sizeof(y) - y_len, y, y_len);
	out->memref.size = DEVID_PUBKEY_LEN;
	return TEE_SUCCESS;
}

static TEE_Result cmd_provision(uint32_t param_types, TEE_Param params[4])
{
	const uint32_t expected = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_OUTPUT,
						  TEE_PARAM_TYPE_NONE,
						  TEE_PARAM_TYPE_NONE,
						  TEE_PARAM_TYPE_NONE);
	TEE_ObjectHandle transient = TEE_HANDLE_NULL;
	TEE_ObjectHandle persistent = TEE_HANDLE_NULL;
	TEE_Attribute curve = { };
	TEE_Result res = TEE_ERROR_GENERIC;

	if (param_types != expected)
		return TEE_ERROR_BAD_PARAMETERS;

	res = open_key(&persistent);
	if (res == TEE_SUCCESS) {
		TEE_CloseObject(persistent);
		EMSG("device key already provisioned, refusing to replace it");
		return TEE_ERROR_ACCESS_CONFLICT;
	}
	if (res != TEE_ERROR_ITEM_NOT_FOUND)
		return res;

	res = TEE_AllocateTransientObject(TEE_TYPE_ECDSA_KEYPAIR, KEY_BITS,
					  &transient);
	if (res)
		return res;

	TEE_InitValueAttribute(&curve, TEE_ATTR_ECC_CURVE,
			       TEE_ECC_CURVE_NIST_P256, 0);
	res = TEE_GenerateKey(transient, KEY_BITS, &curve, 1);
	if (res)
		goto out;

	/*
	 * Sign only, and not extractable: the secret value can't be read back
	 * with TEE_GetObjectBufferAttribute by anyone, this TA included. The
	 * restriction is copied into the persistent object below.
	 */
	res = TEE_RestrictObjectUsage1(transient, TEE_USAGE_SIGN);
	if (res)
		goto out;

	/* No ACCESS_WRITE_META: the object can't be renamed or deleted either. */
	res = TEE_CreatePersistentObject(TEE_STORAGE_PRIVATE, key_object_id,
					 sizeof(key_object_id) - 1,
					 TEE_DATA_FLAG_ACCESS_READ, transient,
					 NULL, 0, &persistent);
	if (res)
		goto out;

	res = export_pubkey(persistent, &params[0]);
	IMSG("device key provisioned");
out:
	if (persistent != TEE_HANDLE_NULL)
		TEE_CloseObject(persistent);
	TEE_FreeTransientObject(transient);
	return res;
}

static TEE_Result cmd_get_pubkey(uint32_t param_types, TEE_Param params[4])
{
	const uint32_t expected = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_OUTPUT,
						  TEE_PARAM_TYPE_NONE,
						  TEE_PARAM_TYPE_NONE,
						  TEE_PARAM_TYPE_NONE);
	TEE_ObjectHandle key = TEE_HANDLE_NULL;
	TEE_Result res = TEE_ERROR_GENERIC;

	if (param_types != expected)
		return TEE_ERROR_BAD_PARAMETERS;

	res = open_key(&key);
	if (res)
		return res;
	res = export_pubkey(key, &params[0]);
	TEE_CloseObject(key);
	return res;
}

static TEE_Result challenge_digest(const uint8_t *nonce, size_t nonce_len,
				   uint8_t digest[32])
{
	TEE_OperationHandle op = TEE_HANDLE_NULL;
	size_t digest_len = 32;
	TEE_Result res = TEE_ERROR_GENERIC;

	res = TEE_AllocateOperation(&op, TEE_ALG_SHA256, TEE_MODE_DIGEST, 0);
	if (res)
		return res;
	TEE_DigestUpdate(op, DEVID_CHALLENGE_DOMAIN,
			 sizeof(DEVID_CHALLENGE_DOMAIN) - 1);
	res = TEE_DigestDoFinal(op, nonce, nonce_len, digest, &digest_len);
	TEE_FreeOperation(op);
	return res;
}

static TEE_Result cmd_sign_challenge(uint32_t param_types, TEE_Param params[4])
{
	const uint32_t expected = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
						  TEE_PARAM_TYPE_MEMREF_OUTPUT,
						  TEE_PARAM_TYPE_NONE,
						  TEE_PARAM_TYPE_NONE);
	uint8_t nonce[DEVID_NONCE_MAX] = { 0 };
	uint8_t digest[32] = { 0 };
	uint8_t sig[DEVID_SIG_LEN] = { 0 };
	size_t nonce_len = 0;
	size_t sig_len = sizeof(sig);
	TEE_ObjectHandle key = TEE_HANDLE_NULL;
	TEE_OperationHandle op = TEE_HANDLE_NULL;
	TEE_Result res = TEE_ERROR_GENERIC;

	if (param_types != expected)
		return TEE_ERROR_BAD_PARAMETERS;

	nonce_len = params[0].memref.size;
	if (nonce_len < DEVID_NONCE_MIN || nonce_len > DEVID_NONCE_MAX)
		return TEE_ERROR_BAD_PARAMETERS;
	if (params[1].memref.size < DEVID_SIG_LEN) {
		params[1].memref.size = DEVID_SIG_LEN;
		return TEE_ERROR_SHORT_BUFFER;
	}

	/*
	 * The memref points into memory the normal world shares with us and
	 * can still write to. Copy once, then only use the copy, so what gets
	 * hashed is what was checked (no double fetch).
	 */
	TEE_MemMove(nonce, params[0].memref.buffer, nonce_len);

	res = challenge_digest(nonce, nonce_len, digest);
	if (res)
		return res;

	res = open_key(&key);
	if (res)
		return res;

	res = TEE_AllocateOperation(&op, TEE_ALG_ECDSA_SHA256, TEE_MODE_SIGN,
				    KEY_BITS);
	if (res)
		goto out;
	res = TEE_SetOperationKey(op, key);
	if (res)
		goto out;
	res = TEE_AsymmetricSignDigest(op, NULL, 0, digest, sizeof(digest),
				       sig, &sig_len);
	if (res)
		goto out;

	TEE_MemMove(params[1].memref.buffer, sig, sig_len);
	params[1].memref.size = sig_len;
out:
	if (op != TEE_HANDLE_NULL)
		TEE_FreeOperation(op);
	TEE_CloseObject(key);
	return res;
}

static TEE_Result cmd_demo_export_private(uint32_t param_types)
{
	uint8_t d[KEY_BITS / 8] = { 0 };
	size_t d_len = sizeof(d);
	TEE_ObjectHandle key = TEE_HANDLE_NULL;
	TEE_Result res = TEE_ERROR_GENERIC;

	if (param_types != TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
					   TEE_PARAM_TYPE_NONE,
					   TEE_PARAM_TYPE_NONE,
					   TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;

	res = open_key(&key);
	if (res)
		return res;
	/* Expected not to return: protected attribute, non-extractable key. */
	res = TEE_GetObjectBufferAttribute(key, TEE_ATTR_ECC_PRIVATE_VALUE,
					   d, &d_len);
	TEE_CloseObject(key);
	EMSG("private key was readable - usage restriction did not hold");
	return res ? res : TEE_ERROR_SECURITY;
}

TEE_Result TA_InvokeCommandEntryPoint(void *sess_ctx __unused, uint32_t cmd_id,
				      uint32_t param_types, TEE_Param params[4])
{
	switch (cmd_id) {
	case TA_DEVID_CMD_PROVISION:
		return cmd_provision(param_types, params);
	case TA_DEVID_CMD_GET_PUBKEY:
		return cmd_get_pubkey(param_types, params);
	case TA_DEVID_CMD_SIGN_CHALLENGE:
		return cmd_sign_challenge(param_types, params);
	case TA_DEVID_CMD_DEMO_EXPORT_PRIVATE:
		return cmd_demo_export_private(param_types);
	default:
		return TEE_ERROR_NOT_SUPPORTED;
	}
}
