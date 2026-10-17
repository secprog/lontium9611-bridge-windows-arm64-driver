/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once

#define TRACE_DRIVER 0
#define TRACE_USB    0
#define TRACE_P8     0

#define TRACE_LEVEL_ERROR       1
#define TRACE_LEVEL_WARNING     2
#define TRACE_LEVEL_INFORMATION 4
#define TRACE_LEVEL_VERBOSE     5

extern volatile LONG g_P8CecLogLevel;

static __inline int P8TraceEnabled(unsigned int Level)
{
    LONG threshold = g_P8CecLogLevel;
    return Level > 0 && Level <= (unsigned int)threshold;
}

#define P8_TRACE_FILTER_LEVEL(LEVEL) \
    (((LEVEL) == TRACE_LEVEL_ERROR) ? DPFLTR_ERROR_LEVEL : \
     ((LEVEL) == TRACE_LEVEL_WARNING) ? DPFLTR_WARNING_LEVEL : \
     ((LEVEL) == TRACE_LEVEL_INFORMATION) ? DPFLTR_INFO_LEVEL : DPFLTR_TRACE_LEVEL)

#define Trace(LEVEL, FLAGS, MSG, ...) do { \
    if (P8TraceEnabled(LEVEL)) { \
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, P8_TRACE_FILTER_LEVEL(LEVEL), "P8CecUde: " MSG "\n", __VA_ARGS__); \
    } \
} while (0)
