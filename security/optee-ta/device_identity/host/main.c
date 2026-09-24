// SPDX-License-Identifier: BSD-2-Clause
/*
 * Normal-world client of the device identity TA.
 *
 *   devid provision            generate the device key once, print public key
 *   devid pubkey               print the public key
 *   devid sign <nonce-hex>     print the signature over a verifier's nonce
 *   devid demo-export-private  try to read the private key (must fail)
 *
 * Output is hex on stdout, so a verifier on another machine can check it
 * with verify.py. This program never sees the private key: it only passes
 * buffers through libteec -> /dev/tee0 -> SMC -> OP-TEE -> the TA.
 */
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tee_client_api.h>

#include <device_identity_ta.h>

struct ctx {
	TEEC_Context ctx;
	TEEC_Session sess;
};

static void open_ta(struct ctx *c)
{
	TEEC_UUID uuid = TA_DEVICE_IDENTITY_UUID;
	uint32_t origin = 0;
	TEEC_Result res;

	res = TEEC_InitializeContext(NULL, &c->ctx);
	if (res != TEEC_SUCCESS)
		errx(1, "TEEC_InitializeContext: 0x%08x", res);
	res = TEEC_OpenSession(&c->ctx, &c->sess, &uuid, TEEC_LOGIN_PUBLIC,
			       NULL, NULL, &origin);
	if (res != TEEC_SUCCESS)
		errx(1, "TEEC_OpenSession: 0x%08x (origin %u)", res, origin);
}

static void close_ta(struct ctx *c)
{
	TEEC_CloseSession(&c->sess);
	TEEC_FinalizeContext(&c->ctx);
}

static void print_hex(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		printf("%02x", buf[i]);
	printf("\n");
}

static size_t parse_hex(const char *hex, uint8_t *out, size_t max)
{
	size_t len = strlen(hex);

	if (len % 2 || len / 2 > max)
		errx(1, "nonce must be an even number of hex digits, at most %zu bytes", max);
	for (size_t i = 0; i < len / 2; i++) {
		unsigned int byte;

		if (sscanf(hex + 2 * i, "%2x", &byte) != 1)
			errx(1, "bad hex in nonce");
		out[i] = (uint8_t)byte;
	}
	return len / 2;
}

static TEEC_Result invoke_out(struct ctx *c, uint32_t cmd, uint8_t *out,
			      size_t *out_len, uint32_t *origin)
{
	TEEC_Operation op = { 0 };
	TEEC_Result res;

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_OUTPUT, TEEC_NONE,
					 TEEC_NONE, TEEC_NONE);
	op.params[0].tmpref.buffer = out;
	op.params[0].tmpref.size = *out_len;
	res = TEEC_InvokeCommand(&c->sess, cmd, &op, origin);
	*out_len = op.params[0].tmpref.size;
	return res;
}

int main(int argc, char *argv[])
{
	struct ctx c;
	uint8_t pub[DEVID_PUBKEY_LEN];
	size_t pub_len = sizeof(pub);
	uint32_t origin = 0;
	TEEC_Result res;

	if (argc < 2)
		errx(2, "usage: %s provision | pubkey | sign <nonce-hex> | demo-export-private", argv[0]);

	open_ta(&c);

	if (!strcmp(argv[1], "provision") || !strcmp(argv[1], "pubkey")) {
		uint32_t cmd = !strcmp(argv[1], "provision") ?
			       TA_DEVID_CMD_PROVISION : TA_DEVID_CMD_GET_PUBKEY;

		res = invoke_out(&c, cmd, pub, &pub_len, &origin);
		if (res != TEEC_SUCCESS)
			errx(1, "%s: 0x%08x (origin %u)", argv[1], res, origin);
		print_hex(pub, pub_len);
	} else if (!strcmp(argv[1], "sign") && argc == 3) {
		uint8_t nonce[DEVID_NONCE_MAX];
		uint8_t sig[DEVID_SIG_LEN];
		TEEC_Operation op = { 0 };
		size_t nonce_len = parse_hex(argv[2], nonce, sizeof(nonce));

		op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
						 TEEC_MEMREF_TEMP_OUTPUT,
						 TEEC_NONE, TEEC_NONE);
		op.params[0].tmpref.buffer = nonce;
		op.params[0].tmpref.size = nonce_len;
		op.params[1].tmpref.buffer = sig;
		op.params[1].tmpref.size = sizeof(sig);
		res = TEEC_InvokeCommand(&c.sess, TA_DEVID_CMD_SIGN_CHALLENGE,
					 &op, &origin);
		if (res != TEEC_SUCCESS)
			errx(1, "sign: 0x%08x (origin %u)", res, origin);
		print_hex(sig, op.params[1].tmpref.size);
	} else if (!strcmp(argv[1], "demo-export-private")) {
		TEEC_Operation op = { 0 };

		op.paramTypes = TEEC_PARAM_TYPES(TEEC_NONE, TEEC_NONE,
						 TEEC_NONE, TEEC_NONE);
		res = TEEC_InvokeCommand(&c.sess, TA_DEVID_CMD_DEMO_EXPORT_PRIVATE,
					 &op, &origin);
		printf("result 0x%08x origin %u%s\n", res, origin,
		       res == TEEC_ERROR_TARGET_DEAD ?
		       " (TA panicked: key is not extractable)" : "");
		/* A dead TA's session can't be closed normally. */
		if (res == TEEC_ERROR_TARGET_DEAD) {
			TEEC_FinalizeContext(&c.ctx);
			return 0;
		}
		close_ta(&c);
		return 1;
	} else {
		errx(2, "unknown command");
	}

	close_ta(&c);
	return 0;
}
