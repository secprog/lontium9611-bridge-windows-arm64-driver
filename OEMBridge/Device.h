/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once

#include "Spb.h"
#include "LpiMux.h"
#include "HdmiAudio.h"
#include "OEMBridgeIoctl.h"
#include "Cec.h"

EXTERN_C_START

typedef struct _DEVICE_CONTEXT
{

    WDFDEVICE FxDevice;

    WDFTIMER HealthTimer;
    WDFWAITLOCK BridgeLock;

    BOOLEAN BridgeReady;
    BOOLEAN InitialD0Done;
    UCHAR LastHpd;
    UCHAR LastInputLive;
    UINT32 LockedHAct;
    UINT32 LockedVAct;
    UINT32 LockedHTot;
    int LockedMode;
    UINT32 PendHAct;
    UINT32 PendVAct;
    UINT32 PendHTot;
    UCHAR RasterPending;
    UCHAR RelockRetries;
    UCHAR PllStatusBad;

    SPB_CONTEXT I2CContext;

    LARGE_INTEGER ResetGpioId;
    WDFIOTARGET ResetGpio;

    BOOLEAN PowerOn;

    LPI_MUX_CONTEXT LpiMux;

    HDMI_AUDIO_CONTEXT HdmiAudio;

    OEMBRIDGE_MODE SinkModes[OEMBRIDGE_MAX_MODES];
    ULONG          SinkModeCount;
    CHAR           SinkName[14];
    UCHAR          SinkEdidValid;
    USHORT         SinkPhysicalAddress;

    CEC_CONTEXT    Cec;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

NTSTATUS LT9611AudioProgram(WDFDEVICE FxDevice, ULONG RateHz, ULONG Bits);
NTSTATUS LT9611AudioEnable(WDFDEVICE FxDevice);
NTSTATUS LT9611AudioDisable(WDFDEVICE FxDevice);

NTSTATUS
OEMBridgeCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    );

NTSTATUS LT9611VideoRelock(WDFDEVICE FxDevice);
NTSTATUS LT9611VideoOn(WDFDEVICE FxDevice, BOOLEAN PowerOn);

VOID LT9611RelockVerified(WDFDEVICE FxDevice, PCSTR Reason);
VOID LT9611SeedHealthState(WDFDEVICE FxDevice);

VOID    OEMBridgeCacheSinkModes(WDFDEVICE Device, const UCHAR *Edid, ULONG Length);
VOID    OEMBridgeLoadLoggingSettings(WDFDEVICE Device);
BOOLEAN OEMBridgeHandleSettingsIoctl(WDFDEVICE Device, WDFREQUEST Request, ULONG IoControlCode,
                                     size_t OutputBufferLength, size_t InputBufferLength);
ULONG   LT9611LockedModePacked(PDEVICE_CONTEXT DevContext);

#define LT9611_EDID_MAX 256
NTSTATUS LT9611EdidRead(WDFDEVICE FxDevice, UCHAR *Edid, ULONG *BytesRead);
NTSTATUS LT9611EdidRefresh(WDFDEVICE FxDevice);

EXTERN_C_END
