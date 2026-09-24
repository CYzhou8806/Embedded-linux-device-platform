/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef USER_TA_HEADER_DEFINES_H
#define USER_TA_HEADER_DEFINES_H

#include <device_identity_ta.h>

#define TA_UUID			TA_DEVICE_IDENTITY_UUID

/* One instance, shared by all sessions: there is exactly one device key. */
#define TA_FLAGS		(TA_FLAG_SINGLE_INSTANCE | TA_FLAG_MULTI_SESSION)

#define TA_STACK_SIZE		(4 * 1024)
#define TA_DATA_SIZE		(32 * 1024)

#define TA_VERSION		"1.0"
#define TA_DESCRIPTION		"device-platform: device identity key (ECDSA P-256)"

#endif /* USER_TA_HEADER_DEFINES_H */
