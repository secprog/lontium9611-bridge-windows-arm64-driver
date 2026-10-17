/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include "device.h"
#include "trace.h"
#include "spb.h"

EXTERN_C_START

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD OEMBridgeEvtDeviceAdd;

EXTERN_C_END
