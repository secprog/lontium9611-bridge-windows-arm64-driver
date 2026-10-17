/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include "driver.h"

volatile LONG g_OemBridgeLogLevel = 0;

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, OEMBridgeEvtDeviceAdd)
#endif

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )

{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config,
                           OEMBridgeEvtDeviceAdd
                           );

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &config,
                             WDF_NO_HANDLE
                             );

    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfDriverCreate failed 0x%08X", status);
        return status;
    }

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%s Exit", __FUNCTION__);

    return status;
}

NTSTATUS
OEMBridgeEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )

{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%s Entry", __FUNCTION__);

    status = OEMBridgeCreateDevice(DeviceInit);

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%s Exit", __FUNCTION__);

    return status;
}
