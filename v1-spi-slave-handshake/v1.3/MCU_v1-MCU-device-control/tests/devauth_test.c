/* Host-side test driver for Core/Src/devauth.c: reads commands on stdin,
 * prints hex results, so tests/devauth_test.py can compare them with
 * Python's hashlib/hmac. Not built for the MCU. */
#include <stdio.h>
#include <string.h>
#include "devauth.h"

static size_t unhex(const char *s, uint8_t *out)
{
	size_t n = 0;
	unsigned v;
	while (sscanf(s + 2 * n, "%2x", &v) == 1 && s[2 * n] && s[2 * n] != '\n')
		out[n++] = (uint8_t)v;
	return n;
}

static void hex(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		printf("%02x", p[i]);
	printf("\n");
}

int main(void)
{
	char cmd[16], a[4096], b[4096];
	static uint8_t x[2048], y[2048];
	uint8_t out[32];
	while (scanf("%15s", cmd) == 1) {
		if (!strcmp(cmd, "sha")) {
			if (scanf("%4095s", a) != 1)
				break;
			size_t n = strcmp(a, "-") ? unhex(a, x) : 0;
			sha256(x, n, out);
			hex(out, 32);
		} else if (!strcmp(cmd, "hmac")) {
			if (scanf("%4095s %4095s", a, b) != 2)
				break;
			size_t kn = unhex(a, x), mn = strcmp(b, "-") ? unhex(b, y) : 0;
			hmac_sha256(x, kn, y, mn, out);
			hex(out, 32);
		} else if (!strcmp(cmd, "auth")) {
			unsigned id, fw;
			if (scanf("%4095s %x %x %4095s", a, &id, &fw, b) != 4)
				break;
			unhex(a, x);
			unhex(b, y);
			devauth_compute(x, id, fw, y, out);
			hex(out, DEVAUTH_MAC_LEN);
		}
	}
	return 0;
}
