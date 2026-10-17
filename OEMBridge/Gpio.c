/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include "driver.h"
#include "device.h"
#include "gpio.h"
#include <gpio.h>

NTSTATUS SetGPIO(WDFIOTARGET gpio, unsigned char* value)
{
    NTSTATUS status = STATUS_SUCCESS;
    WDF_MEMORY_DESCRIPTOR inputDescriptor, outputDescriptor;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDescriptor, value, 1);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDescriptor, value, 1);

    status = WdfIoTargetSendIoctlSynchronously(gpio, NULL, IOCTL_GPIO_WRITE_PINS, &inputDescriptor, &outputDescriptor, NULL, NULL);

    return status;
}

NTSTATUS OpenIOTarget(PDEVICE_CONTEXT ctx, LARGE_INTEGER res, ACCESS_MASK use, WDFIOTARGET* target)
{
    NTSTATUS status = STATUS_SUCCESS;
    WDF_OBJECT_ATTRIBUTES ObjectAttributes;
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;
    UNICODE_STRING ReadString;
    WCHAR ReadStringBuffer[260];

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "OpenIOTarget Entry");

    RtlInitEmptyUnicodeString(&ReadString,
        ReadStringBuffer,
        sizeof(ReadStringBuffer));

    status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&ReadString,
        res.LowPart,
        res.HighPart);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "RESOURCE_HUB_CREATE_PATH_FROM_ID failed 0x%x", status);
        goto Exit;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&ObjectAttributes);
    ObjectAttributes.ParentObject = ctx->FxDevice;

    status = WdfIoTargetCreate(ctx->FxDevice, &ObjectAttributes, target);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "WdfIoTargetCreate failed 0x%x", status);
        goto Exit;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams, &ReadString, use);
    status = WdfIoTargetOpen(*target, &OpenParams);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "WdfIoTargetOpen failed 0x%x", status);

        WdfObjectDelete(*target);
        *target = NULL;
        goto Exit;
    }

Exit:
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "OpenIOTarget Exit");
    return status;
}
