/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include "Driver.h"
#include "Device.h"

#define CEC_POLL_MS               15
#define CEC_TX_TIMEOUT_100NS      (1000ULL * 10000ULL)
#define CEC_RX_LOCK_BYTE_100NS    (25ULL * 10000ULL)
#define CEC_IDLE_RX_RECOVERY_100NS (1000ULL * 10000ULL)
#define CEC_IDLE_RX_MAX_GAP_100NS  (250ULL * 10000ULL)

#define CEC_ST_SEND_DONE          0x01
#define CEC_ST_REC_DATA           0x02
#define CEC_ST_NACK               0x04
#define CEC_ST_ARB_LOST           0x08
#define CEC_ST_ERROR_INITIATOR    0x10
#define CEC_ST_ERROR_FOLLOWER     0x20
#define CEC_ST_WAKE_UP            0x40

EVT_WDF_TIMER CecPollTimer;

NTSTATUS LT9611Write(WDFDEVICE FxDevice, UCHAR Address, UCHAR Data);
NTSTATUS LT9611Read(WDFDEVICE FxDevice, UCHAR Address, UCHAR *Data, ULONG Length);

static ULONG CecReadDword(WDFDEVICE Device, PCWSTR Name, ULONG Default)
{
    WDFKEY         key;
    UNICODE_STRING name;
    ULONG          value = Default;

    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key)))
    {
        RtlInitUnicodeString(&name, Name);
        if (!NT_SUCCESS(WdfRegistryQueryULong(key, &name, &value)))
        {
            value = Default;
        }
        WdfRegistryClose(key);
    }
    return value;
}

static NTSTATUS CecWriteDword(WDFDEVICE Device, PCWSTR Name, ULONG Value)
{
    WDFKEY         key;
    UNICODE_STRING name;
    NTSTATUS       status;

    status = WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_SET_VALUE,
                                      WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    RtlInitUnicodeString(&name, Name);
    status = WdfRegistryAssignULong(key, &name, Value);
    WdfRegistryClose(key);
    return status;
}

static NTSTATUS CecHwWrite(WDFDEVICE Device, PCSTR Operation, UCHAR Address, UCHAR Value)
{
    NTSTATUS status = LT9611Write(Device, Address, Value);
    if (!NT_SUCCESS(status))
    {
        DeviceGetContext(Device)->Cec.I2cErrors++;
        Trace(TRACE_LEVEL_ERROR, TRACE_DEVICE,
              "cec: i2c %s write reg %02X=%02X failed 0x%08X", Operation, Address, Value, status);
    }
    return status;
}

static NTSTATUS CecHwRead(WDFDEVICE Device, PCSTR Operation, UCHAR Address, UCHAR *Value)
{
    NTSTATUS status = LT9611Read(Device, Address, Value, 1);
    if (!NT_SUCCESS(status))
    {
        DeviceGetContext(Device)->Cec.I2cErrors++;
        Trace(TRACE_LEVEL_ERROR, TRACE_DEVICE,
              "cec: i2c %s read reg %02X failed 0x%08X", Operation, Address, status);
    }
    return status;
}

static NTSTATUS CecHwWriteSequence(WDFDEVICE Device, PCSTR Operation,
                                   const UCHAR (*Writes)[2], ULONG Count)
{
    ULONG i;
    NTSTATUS status;
    for (i = 0; i < Count; i++)
    {
        status = CecHwWrite(Device, Operation, Writes[i][0], Writes[i][1]);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS CecHwSetLogicalAddress(WDFDEVICE Device, UCHAR La)
{

    UCHAR writes[][2] = { {0xff, 0x86}, {0xf7, 0}, {0xf8, 0} };
    if (La < 4)       { writes[1][1] = (UCHAR)(La + 1); }
    else if (La < 8)  { writes[1][1] = (UCHAR)((La - 3) << 4); }
    else if (La < 12) { writes[2][1] = (UCHAR)(La - 7); }
    else              { writes[2][1] = (UCHAR)((La - 11) << 4); }
    return CecHwWriteSequence(Device, "address", writes, RTL_NUMBER_OF(writes));
}

static NTSTATUS CecHwEnable(WDFDEVICE Device)
{
    static const UCHAR writes[][2] = {
        {0xff, 0x80}, {0x0d, 0xff}, {0x15, 0xf1}, {0x15, 0xf9},
        {0xff, 0x86}, {0xf5, 0x00}, {0xfe, 0xa5},
        {0xfa, 0x00}, {0xfc, 0x7f}, {0xfc, 0x00},
        {0xff, 0x82}, {0x01, 0x7f}, {0x05, 0xff}, {0x05, 0x7f}
    };
    NTSTATUS status = CecHwWriteSequence(Device, "enable", writes, RTL_NUMBER_OF(writes));
    if (NT_SUCCESS(status))
    {
        DeviceGetContext(Device)->Cec.TxRxLocked = FALSE;
    }
    return status;
}

static NTSTATUS CecHwDisable(WDFDEVICE Device)
{
    static const UCHAR writes[][2] = {
        {0xff, 0x86}, {0xfa, 0xff}, {0xf5, 0x00}, {0xfc, 0x7f},
        {0xff, 0x80}, {0x15, 0xf1}
    };
    NTSTATUS status = CecHwWriteSequence(Device, "disable", writes, RTL_NUMBER_OF(writes));
    DeviceGetContext(Device)->Cec.HwOn = FALSE;
    if (NT_SUCCESS(status))
    {
        DeviceGetContext(Device)->Cec.TxRxLocked = FALSE;
    }
    return status;
}

static NTSTATUS CecHwLogicalReset(WDFDEVICE Device, UCHAR La)
{
    static const UCHAR writes[][2] = { {0xff, 0x80}, {0x15, 0xf1}, {0x15, 0xf9} };
    NTSTATUS status = CecHwWriteSequence(Device, "reset", writes, RTL_NUMBER_OF(writes));
    if (NT_SUCCESS(status))
    {
        status = CecHwSetLogicalAddress(Device, La);
    }
    if (!NT_SUCCESS(status))
    {
        DeviceGetContext(Device)->Cec.HwOn = FALSE;
    }
    return status;
}

static NTSTATUS CecHwClearStatus(WDFDEVICE Device)
{
    static const UCHAR writes[][2] = {
        {0xff, 0x86}, {0xfc, 0x7f}, {0xfc, 0x00},
        {0xff, 0x82}, {0x05, 0xff}, {0x05, 0x7f}
    };
    return CecHwWriteSequence(Device, "clear", writes, RTL_NUMBER_OF(writes));
}

static NTSTATUS CecHwReadStatus(WDFDEVICE Device, UCHAR *Status)
{
    NTSTATUS status = CecHwWrite(Device, "status", 0xff, 0x86);
    if (NT_SUCCESS(status))
    {
        status = CecHwRead(Device, "status", 0xd2, Status);
    }
    return status;
}

static VOID CecIdleReset(PCEC_CONTEXT cec, BOOLEAN Rearm)
{
    cec->RxIdleWatching = FALSE;
    if (Rearm) { cec->RxIdleAttempted = FALSE; }
}

static NTSTATUS CecHwUnlockRx(WDFDEVICE Device)
{
    static const UCHAR writes[][2] = { {0xff, 0x86}, {0xf5, 0x00} };
    NTSTATUS status = CecHwWriteSequence(Device, "unlock", writes, RTL_NUMBER_OF(writes));
    if (NT_SUCCESS(status))
    {
        DeviceGetContext(Device)->Cec.TxRxLocked = FALSE;
    }
    else
    {

        DeviceGetContext(Device)->Cec.HwOn = FALSE;
    }
    return status;
}

static NTSTATUS CecHwRecoverIdleRx(WDFDEVICE Device, UCHAR *Status)
{
    PCEC_CONTEXT cec = &DeviceGetContext(Device)->Cec;
    UCHAR lockValue = 0, after = 0;
    NTSTATUS status, readbackStatus;
    ULONGLONG now;

    if (*Status != 0 || cec->TxBusy || cec->TxRxLocked)
    {
        CecIdleReset(cec, TRUE);
        return STATUS_SUCCESS;
    }
    status = CecHwRead(Device, "idle rx lock", 0xf5, &lockValue);
    if (!NT_SUCCESS(status))
    {
        CecIdleReset(cec, FALSE);
        return status;
    }
    if (lockValue != 0x01)
    {
        CecIdleReset(cec, TRUE);
        return STATUS_SUCCESS;
    }
    if (cec->RxIdleAttempted) { return STATUS_SUCCESS; }

    now = KeQueryInterruptTime();
    if (!cec->RxIdleWatching ||
        now - cec->RxIdleLastSample > CEC_IDLE_RX_MAX_GAP_100NS)
    {
        cec->RxIdleSince = now;
        cec->RxIdleWatching = TRUE;
    }
    cec->RxIdleLastSample = now;
    if (now - cec->RxIdleSince < CEC_IDLE_RX_RECOVERY_100NS)
    {
        return STATUS_SUCCESS;
    }

    status = CecHwReadStatus(Device, Status);
    if (!NT_SUCCESS(status))
    {
        CecIdleReset(cec, FALSE);
        return status;
    }
    if (*Status != 0)
    {
        CecIdleReset(cec, TRUE);
        return STATUS_SUCCESS;
    }

    cec->RxIdleAttempted = TRUE;
    cec->RxIdleWatching = FALSE;
    cec->RxIdleRecoveries++;
    status = CecHwUnlockRx(Device);

    readbackStatus = status;
    if (NT_SUCCESS(status))
    {
        readbackStatus = CecHwRead(Device, "idle rx unlock readback", 0xf5, &after);
    }
    Trace(TRACE_LEVEL_WARNING, TRACE_DEVICE,
          "cec: idle rx recovery #%lu F5=%02X->%02X readback=0x%08X unlock=0x%08X idle=%llu ms",
          cec->RxIdleRecoveries, lockValue, after, readbackStatus, status,
          (now - cec->RxIdleSince) / 10000);
    return status;
}

static BOOLEAN CecHwReadFrame(WDFDEVICE Device, OEMBRIDGE_CEC_FRAME *Frame, BOOLEAN KeepLocked)
{
    UCHAR size = 0, i;
    NTSTATUS status, unlockStatus;
    RtlZeroMemory(Frame, sizeof(*Frame));
    status = CecHwWrite(Device, "rx", 0xff, 0x86);
    if (NT_SUCCESS(status)) { status = CecHwWrite(Device, "rx", 0xf5, 0x01); }
    if (NT_SUCCESS(status)) { status = CecHwRead(Device, "rx length", 0xd3, &size); }
    if (NT_SUCCESS(status) && (size == 0 || size > OEMBRIDGE_CEC_MAX_FRAME))
    {
        Trace(TRACE_LEVEL_WARNING, TRACE_DEVICE, "cec: invalid RX length %u", size);
        status = STATUS_DEVICE_DATA_ERROR;
    }
    for (i = 0; NT_SUCCESS(status) && i < size; i++)
    {
        status = CecHwRead(Device, "rx data", (UCHAR)(0xd4 + i), &Frame->Data[i]);
    }
    if (!KeepLocked)
    {
        unlockStatus = CecHwUnlockRx(Device);
        if (NT_SUCCESS(status)) { status = unlockStatus; }
    }
    if (!NT_SUCCESS(status))
    {
        DeviceGetContext(Device)->Cec.RxDropped++;
        RtlZeroMemory(Frame, sizeof(*Frame));
        return FALSE;
    }
    Frame->Length = size;
    return TRUE;
}

static NTSTATUS CecHwTransmit(WDFDEVICE Device, const UCHAR *Frame, UCHAR Length)
{
    UCHAR i;
    NTSTATUS status;
    CecIdleReset(&DeviceGetContext(Device)->Cec, TRUE);
    status = CecHwWrite(Device, "tx", 0xff, 0x86);
    if (NT_SUCCESS(status)) { status = CecHwWrite(Device, "tx", 0xf5, 0x01); }
    if (NT_SUCCESS(status))
    {
        DeviceGetContext(Device)->Cec.TxRxLocked = TRUE;
        status = CecHwWrite(Device, "tx", 0xf4, Length);
    }
    for (i = 0; NT_SUCCESS(status) && i < Length; i++)
    {
        status = CecHwWrite(Device, "tx data", (UCHAR)(0xe4 + i), Frame[i]);
    }
    if (NT_SUCCESS(status)) { status = CecHwWrite(Device, "tx start", 0xf9, 0x03); }
    if (!NT_SUCCESS(status))
    {

        (VOID)CecHwLogicalReset(Device, DeviceGetContext(Device)->Cec.LogicalAddress);
        (VOID)CecHwUnlockRx(Device);
    }

    return status;
}

NTSTATUS CecHwRestore(WDFDEVICE Device)
{
    PCEC_CONTEXT cec = &DeviceGetContext(Device)->Cec;
    NTSTATUS status;
    CecIdleReset(cec, TRUE);
    cec->HwOn = FALSE;
    if (!cec->Available || !cec->Enabled) { return STATUS_SUCCESS; }
    status = CecHwEnable(Device);
    if (NT_SUCCESS(status)) { status = CecHwSetLogicalAddress(Device, cec->LogicalAddress); }
    if (!NT_SUCCESS(status))
    {
        Trace(TRACE_LEVEL_ERROR, TRACE_DEVICE, "cec: setup failed 0x%08X; engine unavailable", status);
        (VOID)CecHwDisable(Device);
        return status;
    }
    cec->HwOn = TRUE;
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
          "cec: setup writes completed, logical address %u", cec->LogicalAddress);
    return STATUS_SUCCESS;
}

static VOID CecCompleteRx(WDFREQUEST Request, const OEMBRIDGE_CEC_FRAME *Frame)
{
    OEMBRIDGE_CEC_FRAME *out = NULL;
    size_t               len = 0;
    NTSTATUS             status;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), &out, &len);
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, status);
        return;
    }
    *out = *Frame;
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*out));
}

typedef struct _CEC_DEFERRED
{
    WDFREQUEST          RxRequest;
    OEMBRIDGE_CEC_FRAME RxFrame;
    WDFREQUEST          TxRequest;
    ULONG               TxResult;
} CEC_DEFERRED;

static VOID CecDeferredInit(CEC_DEFERRED *d)
{
    RtlZeroMemory(d, sizeof(*d));
}

static VOID CecDeferredComplete(CEC_DEFERRED *d)
{
    if (d->RxRequest != NULL)
    {
        CecCompleteRx(d->RxRequest, &d->RxFrame);
        d->RxRequest = NULL;
    }
    if (d->TxRequest != NULL)
    {
        ULONG  *out = NULL;
        size_t  len = 0;

        if (NT_SUCCESS(WdfRequestRetrieveOutputBuffer(d->TxRequest, sizeof(ULONG), &out, &len)))
        {
            *out = d->TxResult;
            WdfRequestCompleteWithInformation(d->TxRequest, STATUS_SUCCESS, sizeof(ULONG));
        }
        else
        {
            WdfRequestComplete(d->TxRequest, STATUS_BUFFER_TOO_SMALL);
        }
        d->TxRequest = NULL;
    }
}

static VOID CecDeliverRx(PCEC_CONTEXT cec, const OEMBRIDGE_CEC_FRAME *Frame, CEC_DEFERRED *Deferred)
{
    WDFREQUEST request;

    cec->RxFrames++;

    if (Deferred->RxRequest == NULL &&
        NT_SUCCESS(WdfIoQueueRetrieveNextRequest(cec->RxQueue, &request)))
    {
        Deferred->RxRequest = request;
        Deferred->RxFrame   = *Frame;
        return;
    }
    if (cec->RxCount == CEC_RX_RING)
    {
        cec->RxHead = (cec->RxHead + 1) % CEC_RX_RING;
        cec->RxCount--;
        cec->RxDropped++;
    }
    cec->RxRing[(cec->RxHead + cec->RxCount) % CEC_RX_RING] = *Frame;
    cec->RxCount++;
}

static BOOLEAN CecPopRx(PCEC_CONTEXT cec, OEMBRIDGE_CEC_FRAME *Frame)
{
    if (cec->RxCount == 0)
    {
        return FALSE;
    }
    *Frame = cec->RxRing[cec->RxHead];
    cec->RxHead = (cec->RxHead + 1) % CEC_RX_RING;
    cec->RxCount--;
    return TRUE;
}

static VOID CecFinishTx(WDFDEVICE Device, PCEC_CONTEXT cec, ULONG Result, CEC_DEFERRED *Deferred)
{
    WDFREQUEST request;

    if (!NT_SUCCESS(CecHwUnlockRx(Device)))
    {
        Result = OEMBRIDGE_CEC_TX_ERROR;
    }
    cec->TxBusy = FALSE;
    switch (Result)
    {
    case OEMBRIDGE_CEC_TX_OK:   cec->TxOk++;    break;
    case OEMBRIDGE_CEC_TX_NACK: cec->TxNack++;  break;
    default:                    cec->TxError++; break;
    }

    if (Deferred->TxRequest == NULL &&
        NT_SUCCESS(WdfIoQueueRetrieveNextRequest(cec->TxQueue, &request)))
    {
        Deferred->TxRequest = request;
        Deferred->TxResult  = Result;
    }
}

VOID CecPollTimer(WDFTIMER Timer)
{
    WDFDEVICE       device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PDEVICE_CONTEXT ctx = DeviceGetContext(device);
    PCEC_CONTEXT    cec = &ctx->Cec;
    UCHAR           status = 0;
    CEC_DEFERRED    deferred;

    CecDeferredInit(&deferred);
    WdfWaitLockAcquire(ctx->BridgeLock, NULL);

    if (!ctx->BridgeReady || !cec->PollRunning)
    {
        CecIdleReset(cec, TRUE);
        WdfWaitLockRelease(ctx->BridgeLock);
        return;
    }

    if (cec->TxBusy && !cec->HwOn)
    {
        CecFinishTx(device, cec, OEMBRIDGE_CEC_TX_ERROR, &deferred);
    }
    if (cec->Enabled && cec->HwOn)
    {

        if (cec->TxRxLocked && cec->TxBusy &&
            (KeQueryInterruptTime() - cec->TxStarted) >=
                ((ULONGLONG)cec->TxLen + 1) * CEC_RX_LOCK_BYTE_100NS)
        {
            if (!NT_SUCCESS(CecHwUnlockRx(device)))
            {
                CecFinishTx(device, cec, OEMBRIDGE_CEC_TX_ERROR, &deferred);
                goto rearm;
            }
        }
        if (!NT_SUCCESS(CecHwReadStatus(device, &status)))
        {
            CecIdleReset(cec, FALSE);
            status = 0;
        }
        else if (!NT_SUCCESS(CecHwRecoverIdleRx(device, &status)))
        {
            goto rearm;
        }
        if (status != 0)
        {
            if (!NT_SUCCESS(CecHwClearStatus(device)))
            {

                if (cec->TxBusy)
                {
                    (VOID)CecHwLogicalReset(device, cec->LogicalAddress);
                    CecFinishTx(device, cec, OEMBRIDGE_CEC_TX_ERROR, &deferred);
                }
                goto rearm;
            }
            if (status & (CEC_ST_ARB_LOST | CEC_ST_ERROR_INITIATOR | CEC_ST_ERROR_FOLLOWER))
            {
                Trace(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                      "cec: status 0x%02X tx=%u lock=%u", status, cec->TxBusy, cec->TxRxLocked);
            }

            if (status & CEC_ST_REC_DATA)
            {
                OEMBRIDGE_CEC_FRAME frame;

                if (CecHwReadFrame(device, &frame, cec->TxRxLocked))
                {
                    Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                          "cec: rx %u byte(s) %02X %02X %02X %02X (status 0x%02X)", frame.Length,
                          frame.Data[0], frame.Data[1], frame.Data[2], frame.Data[3], status);
                    CecDeliverRx(cec, &frame, &deferred);
                }
            }

            if (cec->TxBusy)
            {
                if (!cec->HwOn)
                {
                    CecFinishTx(device, cec, OEMBRIDGE_CEC_TX_ERROR, &deferred);
                }
                else if (status & CEC_ST_ERROR_INITIATOR)
                {
                    Trace(TRACE_LEVEL_WARNING, TRACE_DEVICE, "cec: tx initiator error - resetting");
                    CecHwLogicalReset(device, cec->LogicalAddress);
                    CecFinishTx(device, cec, OEMBRIDGE_CEC_TX_ERROR, &deferred);
                }
                else if (status & CEC_ST_ARB_LOST)
                {
                    CecFinishTx(device, cec, OEMBRIDGE_CEC_TX_LINE, &deferred);
                }
                else if (status & CEC_ST_NACK)
                {
                    CecFinishTx(device, cec, OEMBRIDGE_CEC_TX_NACK, &deferred);
                }
                else if (status & CEC_ST_ERROR_FOLLOWER)
                {
                    CecFinishTx(device, cec, OEMBRIDGE_CEC_TX_ERROR, &deferred);
                }
                else if (status & CEC_ST_SEND_DONE)
                {
                    CecFinishTx(device, cec, OEMBRIDGE_CEC_TX_OK, &deferred);
                }
            }
        }

        if (cec->TxBusy &&
            (KeQueryInterruptTime() - cec->TxStarted) > CEC_TX_TIMEOUT_100NS)
        {
            Trace(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                  "cec: tx timed out (status 0x%02X) - resetting", status);
            CecHwLogicalReset(device, cec->LogicalAddress);
            CecFinishTx(device, cec, OEMBRIDGE_CEC_TX_TIMEOUT, &deferred);
        }
    }
    else
    {
        CecIdleReset(cec, TRUE);
    }

rearm:
    if (cec->PollRunning)
    {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(CEC_POLL_MS));
    }

    WdfWaitLockRelease(ctx->BridgeLock);

    CecDeferredComplete(&deferred);
}

NTSTATUS CecCreate(WDFDEVICE Device)
{
    PCEC_CONTEXT          cec = &DeviceGetContext(Device)->Cec;
    WDF_TIMER_CONFIG      timerConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG   queueConfig;
    NTSTATUS              status;

    cec->Available = FALSE;
    cec->LogicalAddress = CEC_LA_UNREGISTERED;

    WDF_TIMER_CONFIG_INIT(&timerConfig, CecPollTimer);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject   = Device;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timerConfig, &attributes, &cec->PollTimer);
    if (!NT_SUCCESS(status))
    {
        goto fail;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(Device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &cec->TxQueue);
    if (!NT_SUCCESS(status))
    {
        goto fail;
    }
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(Device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &cec->RxQueue);
    if (!NT_SUCCESS(status)) { goto fail; }
    cec->Available = TRUE;
    return STATUS_SUCCESS;

fail:

    if (cec->RxQueue != NULL) { WdfObjectDelete(cec->RxQueue); cec->RxQueue = NULL; }
    if (cec->TxQueue != NULL) { WdfObjectDelete(cec->TxQueue); cec->TxQueue = NULL; }
    if (cec->PollTimer != NULL) { WdfObjectDelete(cec->PollTimer); cec->PollTimer = NULL; }
    return status;
}

VOID CecPrepare(WDFDEVICE Device)
{
    PCEC_CONTEXT cec = &DeviceGetContext(Device)->Cec;

    cec->Enabled = cec->Available && CecReadDword(Device, L"HdmiCec", 0) != 0;
    Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "cec: %s by registry",
          cec->Enabled ? "enabled" : "disabled");
}

VOID CecStart(WDFDEVICE Device)
{
    PCEC_CONTEXT cec = &DeviceGetContext(Device)->Cec;

    if (!cec->Available || cec->PollTimer == NULL)
    {
        return;
    }
    cec->PollRunning = TRUE;
    WdfTimerStart(cec->PollTimer, WDF_REL_TIMEOUT_IN_MS(CEC_POLL_MS));
}

VOID CecStop(WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    PCEC_CONTEXT    cec = &ctx->Cec;

    if (cec->PollTimer == NULL)
    {
        return;
    }
    WdfWaitLockAcquire(ctx->BridgeLock, NULL);
    cec->PollRunning = FALSE;
    cec->RxHead = cec->RxCount = 0;
    CecIdleReset(cec, TRUE);
    WdfWaitLockRelease(ctx->BridgeLock);
    WdfTimerStop(cec->PollTimer, TRUE);
}

BOOLEAN CecHandleIoctl(WDFDEVICE Device, WDFREQUEST Request, ULONG IoControlCode)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    PCEC_CONTEXT    cec = &ctx->Cec;
    NTSTATUS        status;
    size_t          len = 0;

    if (!cec->Available &&
        (IoControlCode == IOCTL_OEMBRIDGE_CEC_SET_ENABLE ||
         IoControlCode == IOCTL_OEMBRIDGE_CEC_SET_LOGICAL_ADDRESS ||
         IoControlCode == IOCTL_OEMBRIDGE_CEC_TRANSMIT ||
         IoControlCode == IOCTL_OEMBRIDGE_CEC_RECEIVE))
    {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return TRUE;
    }

    switch (IoControlCode)
    {
    case IOCTL_OEMBRIDGE_CEC_GET_STATE:
    {
        OEMBRIDGE_CEC_STATE *out = NULL;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), &out, &len);
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
            return TRUE;
        }
        RtlZeroMemory(out, sizeof(*out));
        WdfWaitLockAcquire(ctx->BridgeLock, NULL);

        out->Size            = sizeof(*out);
        out->Enabled         = cec->Enabled;
        out->HwOn            = cec->HwOn;
        out->LogicalAddress  = cec->LogicalAddress;
        out->SinkPresent     = ctx->LastHpd;
        out->PhysicalAddress = ctx->SinkPhysicalAddress;
        out->TxOk            = cec->TxOk;
        out->TxNack          = cec->TxNack;
        out->TxError         = cec->TxError;
        out->RxFrames        = cec->RxFrames;
        out->RxDropped       = cec->RxDropped;
        out->I2cErrors       = cec->I2cErrors;
        WdfWaitLockRelease(ctx->BridgeLock);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*out));
        return TRUE;
    }

    case IOCTL_OEMBRIDGE_CEC_SET_ENABLE:
    {
        ULONG       *in = NULL;
        CEC_DEFERRED deferred;
        NTSTATUS     hardwareStatus = STATUS_SUCCESS;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), &in, &len);
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
            return TRUE;
        }
        CecDeferredInit(&deferred);
        WdfWaitLockAcquire(ctx->BridgeLock, NULL);
        cec->Enabled = (*in != 0) ? TRUE : FALSE;
        if (!cec->Enabled)
        {

            cec->RxHead = cec->RxCount = 0;
            CecIdleReset(cec, TRUE);
        }

        status = CecWriteDword(Device, L"HdmiCec", cec->Enabled ? 1 : 0);
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "cec: %s by request (persist 0x%08X)",
              cec->Enabled ? "enabled" : "disabled", status);

        if (ctx->BridgeReady)
        {
            if (cec->TxBusy)
            {
                CecFinishTx(Device, cec, OEMBRIDGE_CEC_TX_ERROR, &deferred);
            }
            if (cec->Enabled)
            {
                hardwareStatus = CecHwRestore(Device);
            }
            else
            {
                hardwareStatus = CecHwDisable(Device);
            }
        }
        WdfWaitLockRelease(ctx->BridgeLock);
        CecDeferredComplete(&deferred);

        CecChildSetPresent(Device, cec->Enabled);

        WdfRequestComplete(Request, NT_SUCCESS(hardwareStatus) ? status : hardwareStatus);
        return TRUE;
    }

    case IOCTL_OEMBRIDGE_CEC_SET_LOGICAL_ADDRESS:
    {
        ULONG *in = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), &in, &len);
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
            return TRUE;
        }
        if (*in > CEC_LA_UNREGISTERED)
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return TRUE;
        }
        WdfWaitLockAcquire(ctx->BridgeLock, NULL);
        status = STATUS_SUCCESS;
        if (cec->TxBusy)
        {
            status = STATUS_DEVICE_BUSY;
        }
        else if (cec->HwOn)
        {
            status = CecHwSetLogicalAddress(Device, (UCHAR)*in);
            if (!NT_SUCCESS(status))
            {
                (VOID)CecHwDisable(Device);
            }
        }
        if (NT_SUCCESS(status))
        {
            cec->LogicalAddress = (UCHAR)*in;
            Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "cec: logical address %u", cec->LogicalAddress);
        }
        WdfWaitLockRelease(ctx->BridgeLock);
        WdfRequestComplete(Request, status);
        return TRUE;
    }

    case IOCTL_OEMBRIDGE_CEC_TRANSMIT:
    {
        OEMBRIDGE_CEC_FRAME *in = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), &in, &len);
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
            return TRUE;
        }
        if (in->Length == 0 || in->Length > OEMBRIDGE_CEC_MAX_FRAME)
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return TRUE;
        }

        WdfWaitLockAcquire(ctx->BridgeLock, NULL);
        if (!ctx->BridgeReady || !cec->Enabled || !cec->HwOn)
        {
            status = STATUS_DEVICE_NOT_READY;
        }
        else if (cec->TxBusy)
        {
            status = STATUS_DEVICE_BUSY;
        }
        else
        {
            cec->TxLen = in->Length;
            status = CecHwTransmit(Device, in->Data, cec->TxLen);
            if (NT_SUCCESS(status))
            {
                cec->TxBusy    = TRUE;
                cec->TxStarted = KeQueryInterruptTime();

                status = WdfRequestForwardToIoQueue(Request, cec->TxQueue);
                if (!NT_SUCCESS(status))
                {
                    (VOID)CecHwLogicalReset(Device, cec->LogicalAddress);
                    (VOID)CecHwUnlockRx(Device);
                    cec->TxBusy = FALSE;
                }
            }
            if (!NT_SUCCESS(status)) { cec->TxError++; }
        }
        WdfWaitLockRelease(ctx->BridgeLock);

        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
        }
        return TRUE;
    }

    case IOCTL_OEMBRIDGE_CEC_RECEIVE:
    {
        OEMBRIDGE_CEC_FRAME  frame;
        OEMBRIDGE_CEC_FRAME *out = NULL;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), &out, &len);
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
            return TRUE;
        }
        WdfWaitLockAcquire(ctx->BridgeLock, NULL);
        if (CecPopRx(cec, &frame))
        {
            WdfWaitLockRelease(ctx->BridgeLock);
            CecCompleteRx(Request, &frame);
            return TRUE;
        }
        status = WdfRequestForwardToIoQueue(Request, cec->RxQueue);
        WdfWaitLockRelease(ctx->BridgeLock);
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
        }
        return TRUE;
    }

    default:
        return FALSE;
    }
}
