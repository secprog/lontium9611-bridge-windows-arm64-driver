/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include "Driver.h"
#include "Device.h"

typedef struct _CEC_CHILD_DESCRIPTION
{
    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Header;
    ULONG                                       Instance;
} CEC_CHILD_DESCRIPTION;

EVT_WDF_CHILD_LIST_CREATE_DEVICE CecChildEvtCreateDevice;

NTSTATUS CecChildInitList(_Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_CHILD_LIST_CONFIG config;

    WDF_CHILD_LIST_CONFIG_INIT(&config, sizeof(CEC_CHILD_DESCRIPTION), CecChildEvtCreateDevice);
    WdfFdoInitSetDefaultChildListConfig(DeviceInit, &config, WDF_NO_OBJECT_ATTRIBUTES);
    return STATUS_SUCCESS;
}

VOID CecChildSetPresent(_In_ WDFDEVICE Device, _In_ BOOLEAN Present)
{
    WDFCHILDLIST          list = WdfFdoGetDefaultChildList(Device);
    CEC_CHILD_DESCRIPTION description;
    NTSTATUS              status;

    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&description.Header, sizeof(description));
    description.Instance = 0;

    if (Present)
    {
        status = WdfChildListAddOrUpdateChildDescriptionAsPresent(list, &description.Header, NULL);
    }
    else
    {
        status = WdfChildListUpdateChildDescriptionAsMissing(list, &description.Header);
    }
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "cec: adapter child %s (0x%08X)",
          Present ? "present" : "absent", status);
}

NTSTATUS
CecChildEvtCreateDevice(
    _In_    WDFCHILDLIST                                List,
    _In_    PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdentificationDescription,
    _Inout_ PWDFDEVICE_INIT                             ChildInit
    )
{
    DECLARE_CONST_UNICODE_STRING(hardwareId, L"OEMBRIDGE\\P8CEC");
    DECLARE_CONST_UNICODE_STRING(instanceId, L"0");
    DECLARE_CONST_UNICODE_STRING(text,       L"Lontium LT9611 HDMI-CEC Adapter");
    DECLARE_CONST_UNICODE_STRING(location,   L"LT9611 HDMI bridge");
    WDF_OBJECT_ATTRIBUTES        attributes;
    WDF_DEVICE_PNP_CAPABILITIES  capabilities;
    WDFDEVICE                    child;
    NTSTATUS                     status;

    UNREFERENCED_PARAMETER(List);
    UNREFERENCED_PARAMETER(IdentificationDescription);

    status = WdfPdoInitAssignDeviceID(ChildInit, &hardwareId);
    if (NT_SUCCESS(status)) { status = WdfPdoInitAddHardwareID(ChildInit, &hardwareId); }
    if (NT_SUCCESS(status)) { status = WdfPdoInitAssignInstanceID(ChildInit, &instanceId); }
    if (NT_SUCCESS(status)) { status = WdfPdoInitAddDeviceText(ChildInit, &text, &location, 0x409); }
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    WdfPdoInitSetDefaultLocale(ChildInit, 0x409);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    status = WdfDeviceCreate(&ChildInit, &attributes, &child);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&capabilities);
    capabilities.Removable        = WdfFalse;
    capabilities.SurpriseRemovalOK = WdfTrue;
    capabilities.UniqueID         = WdfTrue;
    WdfDeviceSetPnpCapabilities(child, &capabilities);

    return STATUS_SUCCESS;
}
