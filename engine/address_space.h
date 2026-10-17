/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>
/* Shared host/runtime/renderer bounds. The top 16 GiB of 40-bit VA is unused. */
#define BB_USER_ADDRESS_MIN UINT64_C(0x1000000000)
#define BB_USER_ADDRESS_LIMIT UINT64_C(0xfc00000000)
#define BB_HOST_LOW_LIMIT UINT64_C(0x0800000000)
