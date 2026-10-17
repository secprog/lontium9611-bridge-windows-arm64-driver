/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once

#include "OEMBridgeIoctl.h"

#define CEC_RX_RING             8
#define CEC_LA_UNREGISTERED     15

typedef struct _CEC_CONTEXT
{
    BOOLEAN             Available;
    BOOLEAN             Enabled;
    BOOLEAN             HwOn;
    BOOLEAN             PollRunning;
    UCHAR               LogicalAddress;

    WDFTIMER            PollTimer;

    BOOLEAN             TxBusy;
    BOOLEAN             TxRxLocked;
    UCHAR               TxLen;
    ULONGLONG           TxStarted;
    WDFQUEUE            TxQueue;

    OEMBRIDGE_CEC_FRAME RxRing[CEC_RX_RING];
    ULONG               RxHead;
    ULONG               RxCount;
    WDFQUEUE            RxQueue;

    ULONGLONG           RxIdleSince;
    ULONGLONG           RxIdleLastSample;
    BOOLEAN             RxIdleWatching;
    BOOLEAN             RxIdleAttempted;
    ULONG               RxIdleRecoveries;

    ULONG               TxOk;
    ULONG               TxNack;
    ULONG               TxError;
    ULONG               RxFrames;
    ULONG               RxDropped;
    ULONG               I2cErrors;
} CEC_CONTEXT, *PCEC_CONTEXT;

NTSTATUS CecCreate(_In_ WDFDEVICE Device);
VOID     CecPrepare(_In_ WDFDEVICE Device);
VOID     CecStart(_In_ WDFDEVICE Device);
VOID     CecStop(_In_ WDFDEVICE Device);

NTSTATUS CecHwRestore(_In_ WDFDEVICE Device);

BOOLEAN  CecHandleIoctl(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ ULONG IoControlCode);

NTSTATUS CecChildInitList(_Inout_ PWDFDEVICE_INIT DeviceInit);
VOID     CecChildSetPresent(_In_ WDFDEVICE Device, _In_ BOOLEAN Present);
