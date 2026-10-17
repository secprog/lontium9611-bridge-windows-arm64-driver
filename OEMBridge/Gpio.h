/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once

#include <wdm.h>
#include <wdf.h>

NTSTATUS OpenIOTarget(PDEVICE_CONTEXT ctx, LARGE_INTEGER res, ACCESS_MASK use, WDFIOTARGET* target);
NTSTATUS SetGPIO(WDFIOTARGET gpio, unsigned char* value);
