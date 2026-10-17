/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) Microsoft Corporation. All Rights Reserved.
 * Copyright (c) Bingxing Wang. All Rights Reserved.
 * Copyright (c) LumiaWoA authors. All Rights Reserved.
 * Copyright (c) DuoWoA authors. All Rights Reserved.
 * See ../LICENSES/BSD-3-Clause.txt.
 */

#include "driver.h"
#include "spb.h"

NTSTATUS
SpbDoWriteDataSynchronously(
	IN SPB_CONTEXT* SpbContext,
	IN UCHAR Address,
	IN PVOID Data,
	IN ULONG Length
)

{
	PUCHAR buffer;
	ULONG length;
	WDFMEMORY memory;
	WDF_MEMORY_DESCRIPTOR memoryDescriptor;
	NTSTATUS status;
	ULONG_PTR bytesWritten = 0;

	length = Length + 1;
	memory = NULL;

	if (length > DEFAULT_SPB_BUFFER_SIZE)
	{
		status = WdfMemoryCreate(
			WDF_NO_OBJECT_ATTRIBUTES,
			NonPagedPoolNx,
			SPB_POOL_TAG,
			length,
			&memory,
			&buffer);

		if (!NT_SUCCESS(status))
		{
			Trace(
				TRACE_LEVEL_ERROR,
				TRACE_SPB,
				"Error allocating memory for Spb write - 0x%08lX",
				status);
			goto exit;
		}

		WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(
			&memoryDescriptor,
			memory,
			NULL);
	}
	else
	{
		buffer = (PUCHAR)WdfMemoryGetBuffer(SpbContext->WriteMemory, NULL);

		WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
			&memoryDescriptor,
			(PVOID)buffer,
			length);
	}

	RtlCopyMemory(buffer, &Address, sizeof(Address));

	RtlCopyMemory((buffer + sizeof(Address)), Data, length - sizeof(Address));

	status = WdfIoTargetSendWriteSynchronously(
		SpbContext->SpbIoTarget,
		NULL,
		&memoryDescriptor,
		NULL,
		NULL,
		&bytesWritten);

	if (NT_SUCCESS(status) && bytesWritten != length)
	{
		status = STATUS_DEVICE_DATA_ERROR;
	}

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error writing to Spb reg %02X - 0x%08lX (bytes %Iu/%lu)",
			Address, status, bytesWritten, length);
		goto exit;
	}

exit:

	if (NULL != memory)
	{
		WdfObjectDelete(memory);
	}

	return status;
}

NTSTATUS
SpbWriteDataSynchronously(
	IN SPB_CONTEXT* SpbContext,
	IN UCHAR Address,
	IN PVOID Data,
	IN ULONG Length
)

{
	NTSTATUS status;

	WdfWaitLockAcquire(SpbContext->SpbLock, NULL);

	status = SpbDoWriteDataSynchronously(
		SpbContext,
		Address,
		Data,
		Length);

	WdfWaitLockRelease(SpbContext->SpbLock);

	return status;
}

NTSTATUS
SpbReadDataSynchronously(
	IN SPB_CONTEXT* SpbContext,
	IN UCHAR Address,
	_In_reads_bytes_(Length) PVOID Data,
	IN ULONG Length
)

{
	PUCHAR buffer;
	WDFMEMORY memory;
	WDF_MEMORY_DESCRIPTOR memoryDescriptor;
	NTSTATUS status;
	ULONG_PTR bytesRead;

	WdfWaitLockAcquire(SpbContext->SpbLock, NULL);

	memory = NULL;
	status = STATUS_INVALID_PARAMETER;
	bytesRead = 0;

	status = SpbDoWriteDataSynchronously(
		SpbContext,
		Address,
		NULL,
		0);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error setting address pointer for Spb read - 0x%08lX",
			status);
		goto exit;
	}

	if (Length > DEFAULT_SPB_BUFFER_SIZE)
	{
		status = WdfMemoryCreate(
			WDF_NO_OBJECT_ATTRIBUTES,
			NonPagedPoolNx,
			SPB_POOL_TAG,
			Length,
			&memory,
			&buffer);

		if (!NT_SUCCESS(status))
		{
			Trace(
				TRACE_LEVEL_ERROR,
				TRACE_SPB,
				"Error allocating memory for Spb read - 0x%08lX",
				status);
			goto exit;
		}

		WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(
			&memoryDescriptor,
			memory,
			NULL);
	}
	else
	{
		buffer = (PUCHAR)WdfMemoryGetBuffer(SpbContext->ReadMemory, NULL);

		WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
			&memoryDescriptor,
			(PVOID)buffer,
			Length);
	}

	status = WdfIoTargetSendReadSynchronously(
		SpbContext->SpbIoTarget,
		NULL,
		&memoryDescriptor,
		NULL,
		NULL,
		&bytesRead);

	if (NT_SUCCESS(status) && bytesRead != Length)
	{
		status = STATUS_DEVICE_DATA_ERROR;
	}
	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error reading from Spb reg %02X - 0x%08lX (bytes %Iu/%lu)",
			Address, status, bytesRead, Length);
		goto exit;
	}

	RtlCopyMemory(Data, buffer, Length);

exit:
	if (NULL != memory)
	{
		WdfObjectDelete(memory);
	}

	WdfWaitLockRelease(SpbContext->SpbLock);

	return status;
}

VOID
SpbTargetDeinitialize(
	IN WDFDEVICE FxDevice,
	IN SPB_CONTEXT* SpbContext
)

{
	UNREFERENCED_PARAMETER(FxDevice);
	UNREFERENCED_PARAMETER(SpbContext);

	if (SpbContext->SpbIoTarget != NULL)
	{
		if (WdfIoTargetGetState(SpbContext->SpbIoTarget) != WdfIoTargetClosed)
		{
			WdfIoTargetClose(SpbContext->SpbIoTarget);
		}
		WdfObjectDelete(SpbContext->SpbIoTarget);
		SpbContext->SpbIoTarget = NULL;
	}

	if (SpbContext->SpbLock != NULL)
	{
		WdfObjectDelete(SpbContext->SpbLock);
		SpbContext->SpbLock = NULL;
	}

	if (SpbContext->ReadMemory != NULL)
	{
		WdfObjectDelete(SpbContext->ReadMemory);
		SpbContext->ReadMemory = NULL;
	}

	if (SpbContext->WriteMemory != NULL)
	{
		WdfObjectDelete(SpbContext->WriteMemory);
		SpbContext->WriteMemory = NULL;
	}
}

NTSTATUS
SpbTargetInitialize(
	IN WDFDEVICE FxDevice,
	IN SPB_CONTEXT* SpbContext
)

{
	WDF_OBJECT_ATTRIBUTES objectAttributes;
	WDF_IO_TARGET_OPEN_PARAMS openParams;
	UNICODE_STRING spbDeviceName;
	WCHAR spbDeviceNameBuffer[RESOURCE_HUB_PATH_SIZE];
	NTSTATUS status;

	WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);
	objectAttributes.ParentObject = FxDevice;

	status = WdfIoTargetCreate(
		FxDevice,
		&objectAttributes,
		&SpbContext->SpbIoTarget);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error creating IoTarget object - 0x%08lX",
			status);

		goto exit;
	}

	RtlInitEmptyUnicodeString(
		&spbDeviceName,
		spbDeviceNameBuffer,
		sizeof(spbDeviceNameBuffer));

	status = RESOURCE_HUB_CREATE_PATH_FROM_ID(
		&spbDeviceName,
		SpbContext->I2cResHubId.LowPart,
		SpbContext->I2cResHubId.HighPart);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error creating Spb resource hub path string - 0x%08lX",
			status);
		goto exit;
	}

	WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
		&openParams,
		&spbDeviceName,
		(GENERIC_READ | GENERIC_WRITE));

	openParams.ShareAccess = 0;
	openParams.CreateDisposition = FILE_OPEN;
	openParams.FileAttributes = FILE_ATTRIBUTE_NORMAL;

	status = WdfIoTargetOpen(SpbContext->SpbIoTarget, &openParams);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error opening Spb target for communication - 0x%08lX",
			status);
		goto exit;
	}

	status = WdfMemoryCreate(
		WDF_NO_OBJECT_ATTRIBUTES,
		NonPagedPoolNx,
		SPB_POOL_TAG,
		DEFAULT_SPB_BUFFER_SIZE,
		&SpbContext->WriteMemory,
		NULL);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error allocating default memory for Spb write - 0x%08lX",
			status);
		goto exit;
	}

	status = WdfMemoryCreate(
		WDF_NO_OBJECT_ATTRIBUTES,
		NonPagedPoolNx,
		SPB_POOL_TAG,
		DEFAULT_SPB_BUFFER_SIZE,
		&SpbContext->ReadMemory,
		NULL);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error allocating default memory for Spb read - 0x%08lX",
			status);
		goto exit;
	}

	status = WdfWaitLockCreate(
		WDF_NO_OBJECT_ATTRIBUTES,
		&SpbContext->SpbLock);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_SPB,
			"Error creating Spb Waitlock - 0x%08lX",
			status);
		goto exit;
	}

exit:

	if (!NT_SUCCESS(status))
	{
		SpbTargetDeinitialize(FxDevice, SpbContext);
	}

	return status;
}
