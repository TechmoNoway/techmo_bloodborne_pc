/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stddef.h>
#ifdef _WIN32
/* Zero-filled host records below the guest reservation and the 40-bit limit. */
void *win_low_alloc(size_t size);
#endif
