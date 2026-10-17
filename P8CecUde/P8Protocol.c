/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include "P8Cec.h"

#define P8_POOL_TAG 'ceC8'
#define P8_RETRY_MS 500

EVT_WDF_REQUEST_COMPLETION_ROUTINE P8RxComplete;
EVT_WDF_REQUEST_COMPLETION_ROUTINE P8TxComplete;
EVT_WDF_TIMER P8RetryTimer;
EVT_WDF_TIMER P8CecTxTimer;

static VOID P8PostReceive(_In_ PP8_DEVICE_CONTEXT Ctx);

VOID P8SendMsg(PP8_DEVICE_CONTEXT Ctx, UCHAR Code, const UCHAR *Payload, ULONG Length)
{
    KIRQL oldIrql;

    ULONG need = 3 + Length * 2;
    ULONG i;

    KeAcquireSpinLock(&Ctx->TxLock, &oldIrql);
    if (P8_TX_FIFO_SIZE - Ctx->TxCount < need)
    {
        Ctx->TxDropped++;
        KeReleaseSpinLock(&Ctx->TxLock, oldIrql);
        return;
    }

#define P8_PUT(b) do { Ctx->TxFifo[(Ctx->TxHead + Ctx->TxCount) % P8_TX_FIFO_SIZE] = (UCHAR)(b); Ctx->TxCount++; } while (0)
    P8_PUT(P8_MSGSTART);
    P8_PUT(Code);
    for (i = 0; i < Length; i++)
    {
        if (Payload[i] >= P8_MSGESC)
        {
            P8_PUT(P8_MSGESC);
            P8_PUT(Payload[i] - P8_ESCOFFSET);
        }
        else
        {
            P8_PUT(Payload[i]);
        }
    }
    P8_PUT(P8_MSGEND);
#undef P8_PUT

    KeReleaseSpinLock(&Ctx->TxLock, oldIrql);
    Trace(TRACE_LEVEL_VERBOSE, TRACE_P8,
          "reply code 0x%02X (%u payload byte(s))", Code, Length);
    P8KickTx(Ctx);
}

static VOID P8Accept(PP8_DEVICE_CONTEXT Ctx, UCHAR Code)
{
    P8SendMsg(Ctx, MSGCODE_COMMAND_ACCEPTED, &Code, 1);
}

static VOID P8Reject(PP8_DEVICE_CONTEXT Ctx, UCHAR Code)
{
    P8SendMsg(Ctx, MSGCODE_COMMAND_REJECTED, &Code, 1);
}

static VOID P8EmitFrame(PP8_DEVICE_CONTEXT Ctx, const OEMBRIDGE_CEC_FRAME *Frame)
{
    UCHAR ack = ((Frame->Data[0] & 0x0F) != 0x0F) ? P8_FRAME_ACK : 0;
    UCHAR code;
    ULONG i;

    code = (UCHAR)(MSGCODE_FRAME_START | ack | ((Frame->Length == 1) ? P8_FRAME_EOM : 0));
    P8SendMsg(Ctx, code, &Frame->Data[0], 1);

    for (i = 1; i < Frame->Length; i++)
    {
        code = (UCHAR)(MSGCODE_FRAME_DATA | ack | ((i == (ULONG)Frame->Length - 1) ? P8_FRAME_EOM : 0));
        P8SendMsg(Ctx, code, &Frame->Data[i], 1);
    }
}

NTSTATUS P8BridgeOpen(WDFDEVICE Device)
{
    PP8_DEVICE_CONTEXT        ctx = P8GetContext(Device);
    WDF_OBJECT_ATTRIBUTES     attributes;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    WDF_TIMER_CONFIG          timerConfig;
    UNICODE_STRING            name;
    PWSTR                     links = NULL;
    NTSTATUS                  status;

    if (ctx->Bridge != NULL)
    {
        return STATUS_SUCCESS;
    }

    status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_OEMBRIDGE, NULL, 0, &links);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    if (links == NULL || links[0] == L'\0')
    {
        if (links != NULL)
        {
            ExFreePool(links);
        }
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    RtlInitUnicodeString(&name, links);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;

    status = WdfIoTargetCreate(Device, &attributes, &ctx->Bridge);
    if (NT_SUCCESS(status))
    {
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, &name, GENERIC_READ | GENERIC_WRITE);
        openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
        status = WdfIoTargetOpen(ctx->Bridge, &openParams);
        if (!NT_SUCCESS(status))
        {
            WdfObjectDelete(ctx->Bridge);
            ctx->Bridge = NULL;
        }
    }
    ExFreePool(links);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    ctx->RxBuffer = (OEMBRIDGE_CEC_FRAME *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(OEMBRIDGE_CEC_FRAME), P8_POOL_TAG);
    ctx->TxBuffer = (OEMBRIDGE_CEC_FRAME *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(OEMBRIDGE_CEC_FRAME), P8_POOL_TAG);
    if (ctx->RxBuffer == NULL || ctx->TxBuffer == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }

    status = WdfRequestCreate(&attributes, ctx->Bridge, &ctx->RxRequest);
    if (NT_SUCCESS(status))
    {
        status = WdfMemoryCreatePreallocated(&attributes, ctx->RxBuffer, sizeof(OEMBRIDGE_CEC_FRAME), &ctx->RxMemory);
    }
    if (NT_SUCCESS(status))
    {
        status = WdfRequestCreate(&attributes, ctx->Bridge, &ctx->TxRequest);
    }
    if (NT_SUCCESS(status))
    {
        status = WdfMemoryCreatePreallocated(&attributes, ctx->TxBuffer, sizeof(OEMBRIDGE_CEC_FRAME), &ctx->TxMemory);
    }
    if (NT_SUCCESS(status) && ctx->RetryTimer == NULL)
    {
        WDF_OBJECT_ATTRIBUTES timerAttributes;

        WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
        timerAttributes.ParentObject   = Device;
        timerAttributes.ExecutionLevel = WdfExecutionLevelPassive;
        WDF_TIMER_CONFIG_INIT(&timerConfig, P8RetryTimer);
        status = WdfTimerCreate(&timerConfig, &timerAttributes, &ctx->RetryTimer);
    }
    if (!NT_SUCCESS(status))
    {
        goto fail;
    }

    if (ctx->CecTxTimer == NULL)
    {
        WDF_OBJECT_ATTRIBUTES timerAttributes;

        WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
        timerAttributes.ParentObject   = Device;
        timerAttributes.ExecutionLevel = WdfExecutionLevelPassive;
        WDF_TIMER_CONFIG_INIT(&timerConfig, P8CecTxTimer);
        timerConfig.AutomaticSerialization = FALSE;
        status = WdfTimerCreate(&timerConfig, &timerAttributes, &ctx->CecTxTimer);
        if (!NT_SUCCESS(status))
        {
            goto fail;
        }
    }

    Trace(TRACE_LEVEL_INFORMATION, TRACE_P8, "OEMBridge CEC interface opened");
    return STATUS_SUCCESS;

fail:
    P8BridgeClose(Device);
    return status;
}

VOID P8BridgeClose(WDFDEVICE Device)
{
    PP8_DEVICE_CONTEXT ctx = P8GetContext(Device);

    P8BridgeStop(Device);

    if (ctx->Bridge != NULL)
    {
        WdfIoTargetClose(ctx->Bridge);
    }
    if (ctx->RxRequest != NULL) { WdfObjectDelete(ctx->RxRequest); ctx->RxRequest = NULL; }
    if (ctx->TxRequest != NULL) { WdfObjectDelete(ctx->TxRequest); ctx->TxRequest = NULL; }
    if (ctx->RxMemory  != NULL) { WdfObjectDelete(ctx->RxMemory);  ctx->RxMemory  = NULL; }
    if (ctx->TxMemory  != NULL) { WdfObjectDelete(ctx->TxMemory);  ctx->TxMemory  = NULL; }
    if (ctx->RxBuffer  != NULL) { ExFreePoolWithTag(ctx->RxBuffer, P8_POOL_TAG); ctx->RxBuffer = NULL; }
    if (ctx->TxBuffer  != NULL) { ExFreePoolWithTag(ctx->TxBuffer, P8_POOL_TAG); ctx->TxBuffer = NULL; }
    if (ctx->Bridge != NULL)
    {
        WdfObjectDelete(ctx->Bridge);
        ctx->Bridge = NULL;
    }
}

VOID P8BridgeStart(WDFDEVICE Device)
{
    PP8_DEVICE_CONTEXT ctx = P8GetContext(Device);
    KIRQL oldIrql;

    KeAcquireSpinLock(&ctx->CecTxLock, &oldIrql);
    ctx->Running = TRUE;
    KeReleaseSpinLock(&ctx->CecTxLock, oldIrql);
    P8PostReceive(ctx);
}

VOID P8BridgeStop(WDFDEVICE Device)
{
    PP8_DEVICE_CONTEXT ctx = P8GetContext(Device);
    KIRQL oldIrql;
    BOOLEAN cancelTx;

    KeAcquireSpinLock(&ctx->CecTxLock, &oldIrql);
    ctx->Running = FALSE;
    ctx->CecTxHead = ctx->CecTxCount = ctx->CecTxRejected = 0;
    KeReleaseSpinLock(&ctx->CecTxLock, oldIrql);

    if (ctx->CecTxTimer != NULL)
    {
        WdfTimerStop(ctx->CecTxTimer, TRUE);
    }
    if (ctx->RetryTimer != NULL)
    {
        WdfTimerStop(ctx->RetryTimer, TRUE);
    }
    if (ctx->RxPosted && ctx->RxRequest != NULL)
    {
        WdfRequestCancelSentRequest(ctx->RxRequest);
    }
    KeAcquireSpinLock(&ctx->CecTxLock, &oldIrql);
    cancelTx = ctx->CecTxInFlight;
    KeReleaseSpinLock(&ctx->CecTxLock, oldIrql);
    if (cancelTx && ctx->TxRequest != NULL)
    {
        WdfRequestCancelSentRequest(ctx->TxRequest);
    }
}

static VOID P8PostReceive(PP8_DEVICE_CONTEXT Ctx)
{
    WDF_REQUEST_REUSE_PARAMS reuse;
    NTSTATUS                 status;

    if (Ctx->Bridge == NULL || Ctx->RxPosted || !Ctx->Running)
    {
        return;
    }

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    WdfRequestReuse(Ctx->RxRequest, &reuse);

    status = WdfIoTargetFormatRequestForIoctl(Ctx->Bridge, Ctx->RxRequest, IOCTL_OEMBRIDGE_CEC_RECEIVE,
                                              NULL, NULL, Ctx->RxMemory, NULL);
    if (!NT_SUCCESS(status))
    {
        WdfTimerStart(Ctx->RetryTimer, WDF_REL_TIMEOUT_IN_MS(P8_RETRY_MS));
        return;
    }
    WdfRequestSetCompletionRoutine(Ctx->RxRequest, P8RxComplete, Ctx);

    Ctx->RxPosted = TRUE;
    if (!WdfRequestSend(Ctx->RxRequest, Ctx->Bridge, WDF_NO_SEND_OPTIONS))
    {
        Ctx->RxPosted = FALSE;
        WdfTimerStart(Ctx->RetryTimer, WDF_REL_TIMEOUT_IN_MS(P8_RETRY_MS));
    }
}

VOID
P8RxComplete(
    _In_ WDFREQUEST                     Request,
    _In_ WDFIOTARGET                    Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT                     Context
    )
{
    PP8_DEVICE_CONTEXT ctx = (PP8_DEVICE_CONTEXT)Context;
    NTSTATUS           status = Params->IoStatus.Status;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);

    ctx->RxPosted = FALSE;

    if (NT_SUCCESS(status) &&
        Params->IoStatus.Information >= sizeof(OEMBRIDGE_CEC_FRAME) &&
        ctx->RxBuffer->Length >= 1)
    {
        P8EmitFrame(ctx, ctx->RxBuffer);
    }

    if (!ctx->Running)
    {
        return;
    }

    WdfTimerStart(ctx->RetryTimer,
                  WDF_REL_TIMEOUT_IN_MS(NT_SUCCESS(status) ? 0 : P8_RETRY_MS));
}

VOID P8RetryTimer(WDFTIMER Timer)
{
    P8PostReceive(P8GetContext((WDFDEVICE)WdfTimerGetParentObject(Timer)));
}

static VOID P8BridgeTransmit(WDFDEVICE Device)
{
    PP8_DEVICE_CONTEXT ctx = P8GetContext(Device);
    OEMBRIDGE_CEC_FRAME *frame;
    KIRQL oldIrql;
    BOOLEAN queued = FALSE;

    if (ctx->TxFrameLen == 0)
    {
        return;
    }
    KeAcquireSpinLock(&ctx->CecTxLock, &oldIrql);
    if (ctx->Bridge != NULL && ctx->Running && ctx->CecTxTimer != NULL)
    {
        if (ctx->CecTxCount < P8_CEC_TX_QUEUE_SIZE && ctx->CecTxRejected == 0)
        {
            frame = &ctx->CecTxQueue[(ctx->CecTxHead + ctx->CecTxCount) % P8_CEC_TX_QUEUE_SIZE];
            RtlZeroMemory(frame, sizeof(*frame));
            frame->Length = ctx->TxFrameLen;
            RtlCopyMemory(frame->Data, ctx->TxFrame, ctx->TxFrameLen);
            ctx->CecTxCount++;
        }
        else
        {

            ctx->CecTxRejected++;
        }
        queued = TRUE;
        if (!ctx->CecTxInFlight)
        {
            WdfTimerStart(ctx->CecTxTimer, WDF_REL_TIMEOUT_IN_MS(0));
        }
    }
    KeReleaseSpinLock(&ctx->CecTxLock, oldIrql);
    ctx->TxFrameLen = 0;
    if (!queued)
    {
        P8SendMsg(ctx, MSGCODE_TRANSMIT_FAILED_TIMEOUT_LINE, NULL, 0);
    }
}

static VOID P8FinishTransmit(PP8_DEVICE_CONTEXT Ctx, UCHAR Code)
{
    KIRQL oldIrql;

    P8SendMsg(Ctx, Code, NULL, 0);
    KeAcquireSpinLock(&Ctx->CecTxLock, &oldIrql);
    Ctx->CecTxInFlight = FALSE;
    if (Ctx->Running && (Ctx->CecTxCount != 0 || Ctx->CecTxRejected != 0))
    {
        WdfTimerStart(Ctx->CecTxTimer, WDF_REL_TIMEOUT_IN_MS(0));
    }
    KeReleaseSpinLock(&Ctx->CecTxLock, oldIrql);
}

VOID P8CecTxTimer(WDFTIMER Timer)
{
    PP8_DEVICE_CONTEXT ctx = P8GetContext((WDFDEVICE)WdfTimerGetParentObject(Timer));
    WDF_REQUEST_REUSE_PARAMS reuse;
    NTSTATUS status;
    KIRQL oldIrql;
    BOOLEAN overflow = FALSE;

    KeAcquireSpinLock(&ctx->CecTxLock, &oldIrql);
    if (!ctx->Running || ctx->CecTxInFlight ||
        (ctx->CecTxCount == 0 && ctx->CecTxRejected == 0))
    {
        KeReleaseSpinLock(&ctx->CecTxLock, oldIrql);
        return;
    }
    ctx->CecTxInFlight = TRUE;
    ctx->CecTxSequence++;
    if (ctx->CecTxCount != 0)
    {
        *ctx->TxBuffer = ctx->CecTxQueue[ctx->CecTxHead];
        ctx->CecTxHead = (ctx->CecTxHead + 1) % P8_CEC_TX_QUEUE_SIZE;
        ctx->CecTxCount--;
    }
    else
    {
        ctx->CecTxRejected--;
        overflow = TRUE;
    }
    KeReleaseSpinLock(&ctx->CecTxLock, oldIrql);
    if (overflow)
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_P8, "cec tx #%u: queue overflow", ctx->CecTxSequence);
        P8FinishTransmit(ctx, MSGCODE_TRANSMIT_FAILED_TIMEOUT_LINE);
        return;
    }

    Trace(TRACE_LEVEL_VERBOSE, TRACE_P8,
          "cec tx #%u: %u byte(s): %02X %02X %02X %02X", ctx->CecTxSequence,
          ctx->TxBuffer->Length, ctx->TxBuffer->Data[0], ctx->TxBuffer->Data[1],
          ctx->TxBuffer->Data[2], ctx->TxBuffer->Data[3]);

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    status = WdfRequestReuse(ctx->TxRequest, &reuse);
    if (NT_SUCCESS(status))
    {
        status = WdfIoTargetFormatRequestForIoctl(ctx->Bridge, ctx->TxRequest, IOCTL_OEMBRIDGE_CEC_TRANSMIT,
                                              ctx->TxMemory, NULL, ctx->TxMemory, NULL);
    }
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_P8, "cec tx #%u: prepare failed 0x%08X", ctx->CecTxSequence, status);
        P8FinishTransmit(ctx, MSGCODE_TRANSMIT_FAILED_TIMEOUT_LINE);
        return;
    }
    WdfRequestSetCompletionRoutine(ctx->TxRequest, P8TxComplete, ctx);

    if (!WdfRequestSend(ctx->TxRequest, ctx->Bridge, WDF_NO_SEND_OPTIONS))
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_P8, "cec tx #%u: send failed 0x%08X",
              ctx->CecTxSequence, WdfRequestGetStatus(ctx->TxRequest));
        P8FinishTransmit(ctx, MSGCODE_TRANSMIT_FAILED_TIMEOUT_LINE);
    }
}

VOID
P8TxComplete(
    _In_ WDFREQUEST                     Request,
    _In_ WDFIOTARGET                    Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT                     Context
    )
{
    PP8_DEVICE_CONTEXT ctx = (PP8_DEVICE_CONTEXT)Context;

    UCHAR              code = MSGCODE_TRANSMIT_FAILED_TIMEOUT_LINE;
    ULONG              result = (ULONG)-1;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);

    if (NT_SUCCESS(Params->IoStatus.Status) && Params->IoStatus.Information >= sizeof(ULONG))
    {
        result = *(ULONG *)ctx->TxBuffer;
        switch (result)
        {
        case OEMBRIDGE_CEC_TX_OK:      code = MSGCODE_TRANSMIT_SUCCEEDED;           break;
        case OEMBRIDGE_CEC_TX_NACK:    code = MSGCODE_TRANSMIT_FAILED_ACK;          break;
        case OEMBRIDGE_CEC_TX_TIMEOUT: code = MSGCODE_TRANSMIT_FAILED_TIMEOUT_DATA; break;
        default:                       code = MSGCODE_TRANSMIT_FAILED_TIMEOUT_LINE; break;
        }
    }

    Trace((ctx->Running && code != MSGCODE_TRANSMIT_SUCCEEDED &&
           code != MSGCODE_TRANSMIT_FAILED_ACK) ? TRACE_LEVEL_WARNING : TRACE_LEVEL_VERBOSE,
          TRACE_P8, "cec tx #%u: completed 0x%08X, bridge result %u, reply 0x%02X",
          ctx->CecTxSequence, Params->IoStatus.Status, result, code);
    P8FinishTransmit(ctx, code);
}

static NTSTATUS P8BridgeSetLogicalAddress(PP8_DEVICE_CONTEXT Ctx, UCHAR La)
{
    WDF_MEMORY_DESCRIPTOR in;
    ULONG                 value = La;

    if (Ctx->Bridge == NULL)
    {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &value, sizeof(value));
    return WdfIoTargetSendIoctlSynchronously(Ctx->Bridge, NULL, IOCTL_OEMBRIDGE_CEC_SET_LOGICAL_ADDRESS,
                                             &in, NULL, NULL, NULL);
}

static USHORT P8BridgePhysicalAddress(PP8_DEVICE_CONTEXT Ctx)
{
    WDF_MEMORY_DESCRIPTOR out;
    OEMBRIDGE_CEC_STATE   state;
    ULONG_PTR             bytes = 0;

    if (Ctx->PhysicalAddressOverride != 0xFFFF)
    {
        return Ctx->PhysicalAddressOverride;
    }
    if (Ctx->Bridge == NULL)
    {
        return 0xFFFF;
    }
    RtlZeroMemory(&state, sizeof(state));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &state, sizeof(state));
    if (NT_SUCCESS(WdfIoTargetSendIoctlSynchronously(Ctx->Bridge, NULL, IOCTL_OEMBRIDGE_CEC_GET_STATE,
                                                     NULL, &out, NULL, &bytes)) &&
        bytes >= sizeof(state))
    {
        return state.PhysicalAddress;
    }
    return 0xFFFF;
}

static VOID P8HandleMessage(WDFDEVICE Device)
{
    PP8_DEVICE_CONTEXT ctx = P8GetContext(Device);
    UCHAR              code;
    const UCHAR       *payload;
    ULONG              plen;
    UCHAR              resp[16];

    if (ctx->RxLen == 0)
    {
        return;
    }
    code    = ctx->RxMsg[0] & (UCHAR)~(P8_FRAME_EOM | P8_FRAME_ACK);
    payload = &ctx->RxMsg[1];
    plen    = ctx->RxLen - 1;

    Trace(TRACE_LEVEL_VERBOSE, TRACE_P8,
          "command 0x%02X (%u payload byte(s))", code, plen);

    switch (code)
    {
    case MSGCODE_PING:
        P8Accept(ctx, code);
        break;

    case MSGCODE_FIRMWARE_VERSION:
        resp[0] = 0;
        resp[1] = P8_FIRMWARE_VERSION;
        P8SendMsg(ctx, code, resp, 2);
        break;

    case MSGCODE_GET_BUILDDATE:
        resp[0] = 0x66; resp[1] = 0x00; resp[2] = 0x00; resp[3] = 0x00;
        P8SendMsg(ctx, code, resp, 4);
        break;

    case MSGCODE_GET_ADAPTER_TYPE:
        resp[0] = P8_ADAPTERTYPE_EXTERNAL;
        P8SendMsg(ctx, code, resp, 1);
        break;

    case MSGCODE_SET_CONTROLLED:

        P8Accept(ctx, code);
        break;

    case MSGCODE_SET_ACK_MASK:
    {
        UCHAR la = P8_LA_UNREGISTERED;
        ULONG bit;

        ctx->AckMask = (plen >= 2) ? (USHORT)((payload[0] << 8) | payload[1]) : 0;
        for (bit = 0; bit < 15; bit++)
        {
            if (ctx->AckMask & (1u << bit))
            {
                la = (UCHAR)bit;
                break;
            }
        }
        (VOID)P8BridgeSetLogicalAddress(ctx, la);
        Trace(TRACE_LEVEL_INFORMATION, TRACE_P8, "ack mask 0x%04X -> logical address %u", ctx->AckMask, la);
        P8Accept(ctx, code);
        break;
    }

    case MSGCODE_TRANSMIT_IDLETIME:
    case MSGCODE_TRANSMIT_LINE_TIMEOUT:
        P8Accept(ctx, code);
        break;

    case MSGCODE_TRANSMIT_ACK_POLARITY:
        ctx->TxFrameLen = 0;
        P8Accept(ctx, code);
        break;

    case MSGCODE_TRANSMIT:
    case MSGCODE_TRANSMIT_EOM:
        if (plen >= 1 && ctx->TxFrameLen < OEMBRIDGE_CEC_MAX_FRAME)
        {
            ctx->TxFrame[ctx->TxFrameLen++] = payload[0];
        }
        P8Accept(ctx, code);
        if (code == MSGCODE_TRANSMIT_EOM)
        {
            P8BridgeTransmit(Device);
        }
        break;

    case MSGCODE_GET_AUTO_ENABLED:
        resp[0] = ctx->AutoEnabled;
        P8SendMsg(ctx, code, resp, 1);
        break;
    case MSGCODE_SET_AUTO_ENABLED:
        ctx->AutoEnabled = (plen >= 1) ? payload[0] : 0;
        P8Accept(ctx, code);
        break;

    case MSGCODE_GET_DEFAULT_LOGICAL_ADDRESS:
        resp[0] = ctx->DefaultLa;
        P8SendMsg(ctx, code, resp, 1);
        break;
    case MSGCODE_SET_DEFAULT_LOGICAL_ADDRESS:
        ctx->DefaultLa = (plen >= 1) ? payload[0] : ctx->DefaultLa;
        P8Accept(ctx, code);
        break;

    case MSGCODE_GET_LOGICAL_ADDRESS_MASK:
        resp[0] = (UCHAR)(ctx->AckMask >> 8);
        resp[1] = (UCHAR)ctx->AckMask;
        P8SendMsg(ctx, code, resp, 2);
        break;
    case MSGCODE_SET_LOGICAL_ADDRESS_MASK:
        P8Accept(ctx, code);
        break;

    case MSGCODE_GET_PHYSICAL_ADDRESS:
    {
        USHORT pa = P8BridgePhysicalAddress(ctx);
        resp[0] = (UCHAR)(pa >> 8);
        resp[1] = (UCHAR)pa;
        P8SendMsg(ctx, code, resp, 2);
        break;
    }
    case MSGCODE_SET_PHYSICAL_ADDRESS:
        ctx->PhysicalAddressOverride = (plen >= 2) ? (USHORT)((payload[0] << 8) | payload[1]) : 0xFFFF;
        P8Accept(ctx, code);
        break;

    case MSGCODE_GET_DEVICE_TYPE:
        resp[0] = ctx->DeviceType;
        P8SendMsg(ctx, code, resp, 1);
        break;
    case MSGCODE_SET_DEVICE_TYPE:
        ctx->DeviceType = (plen >= 1) ? payload[0] : ctx->DeviceType;
        P8Accept(ctx, code);
        break;

    case MSGCODE_GET_HDMI_VERSION:
        resp[0] = ctx->HdmiVersion;
        P8SendMsg(ctx, code, resp, 1);
        break;
    case MSGCODE_SET_HDMI_VERSION:
        ctx->HdmiVersion = (plen >= 1) ? payload[0] : ctx->HdmiVersion;
        P8Accept(ctx, code);
        break;

    case MSGCODE_GET_OSD_NAME:
    {
        ULONG n = 0;
        while (n < sizeof(ctx->OsdName) - 1 && ctx->OsdName[n] != 0)
        {
            resp[n] = (UCHAR)ctx->OsdName[n];
            n++;
        }
        P8SendMsg(ctx, code, resp, n);
        break;
    }
    case MSGCODE_SET_OSD_NAME:
    {
        ULONG n = (plen < sizeof(ctx->OsdName) - 1) ? plen : sizeof(ctx->OsdName) - 1;
        RtlZeroMemory(ctx->OsdName, sizeof(ctx->OsdName));
        RtlCopyMemory(ctx->OsdName, payload, n);
        P8Accept(ctx, code);
        break;
    }

    case MSGCODE_SET_ACTIVE_SOURCE:

        P8Accept(ctx, code);
        break;

    case MSGCODE_WRITE_EEPROM:
        P8Accept(ctx, code);
        break;

    case MSGCODE_START_BOOTLOADER:
    case MSGCODE_GET_AUTO_POWER_ON:
    case MSGCODE_SET_AUTO_POWER_ON:
    default:
        Trace(TRACE_LEVEL_WARNING, TRACE_P8, "rejecting command %u (%u byte(s))", code, plen);
        P8Reject(ctx, code);
        break;
    }
}

VOID P8HostBytes(WDFDEVICE Device, const UCHAR *Bytes, ULONG Length)
{
    PP8_DEVICE_CONTEXT ctx = P8GetContext(Device);
    ULONG              i;

    for (i = 0; i < Length; i++)
    {
        UCHAR b = Bytes[i];

        if (b == P8_MSGSTART)
        {
            ctx->RxStarted = TRUE;
            ctx->RxEscaped = FALSE;
            ctx->RxLen     = 0;
            continue;
        }
        if (!ctx->RxStarted)
        {
            continue;
        }
        if (b == P8_MSGEND)
        {
            ctx->RxStarted = FALSE;
            P8HandleMessage(Device);
            continue;
        }
        if (b == P8_MSGESC)
        {
            ctx->RxEscaped = TRUE;
            continue;
        }
        if (ctx->RxEscaped)
        {
            b = (UCHAR)(b + P8_ESCOFFSET);
            ctx->RxEscaped = FALSE;
        }
        if (ctx->RxLen < P8_RX_MSG_MAX)
        {
            ctx->RxMsg[ctx->RxLen++] = b;
        }
    }
}
