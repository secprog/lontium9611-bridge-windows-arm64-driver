/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once

#define TRACE_INIT   0
#define TRACE_DRIVER 0
#define TRACE_DEVICE 0
#define TRACE_SPB    0

#define TRACE_LEVEL_ERROR       1
#define TRACE_LEVEL_WARNING     2
#define TRACE_LEVEL_INFORMATION 4
#define TRACE_LEVEL_VERBOSE     5

extern volatile LONG g_OemBridgeLogLevel;

static __inline int OemTraceEnabled(unsigned int Level)
{
    LONG threshold = g_OemBridgeLogLevel;
    return Level > 0 && Level <= (unsigned int)threshold;
}

#define Trace(LEVEL, FLAGS, MSG, ...) do { \
    if (OemTraceEnabled(LEVEL)) { \
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, \
            (LEVEL) <= TRACE_LEVEL_ERROR ? DPFLTR_ERROR_LEVEL : \
            (LEVEL) == TRACE_LEVEL_WARNING ? DPFLTR_WARNING_LEVEL : \
            (LEVEL) == TRACE_LEVEL_INFORMATION ? DPFLTR_INFO_LEVEL : DPFLTR_TRACE_LEVEL, \
            "OEMBridge: " MSG "\n", __VA_ARGS__); \
    } \
} while (0)
