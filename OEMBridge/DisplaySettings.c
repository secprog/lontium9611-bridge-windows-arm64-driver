/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include "Driver.h"
#include "Device.h"

#define BRIDGE_MAX_PCLK_KHZ   297000
#define BRIDGE_MAX_H_ACTIVE   3840

#define OEMBRIDGE_EFI_VAR_ATTRIBUTES  0x00000007

static const OEMBRIDGE_MODE CeaModes[] = {
    {  640,  480,   16, 96,  48, 10,  2, 33, 0, 0,  1, 0,  25000, 0, 0 },
    {  720,  480,   16, 62,  60,  9,  6, 30, 0, 0,  2, 0,  27000, 0, 0 },
    { 1280,  720,  110, 40, 220,  5,  5, 20, 1, 1,  4, 0,  74250, 0, 0 },
    { 1920, 1080,   88, 44, 148,  4,  5, 36, 1, 1, 16, 0, 148500, 0, 0 },
    {  720,  576,   12, 64,  68,  5,  5, 39, 0, 0, 17, 0,  27000, 0, 0 },
    { 1280,  720,  440, 40, 220,  5,  5, 20, 1, 1, 19, 0,  74250, 0, 0 },
    { 1920, 1080,  528, 44, 148,  4,  5, 36, 1, 1, 31, 0, 148500, 0, 0 },
    { 1920, 1080,  638, 44, 148,  4,  5, 36, 1, 1, 32, 0,  74250, 0, 0 },
    { 1920, 1080,  528, 44, 148,  4,  5, 36, 1, 1, 33, 0,  74250, 0, 0 },
    { 1920, 1080,   88, 44, 148,  4,  5, 36, 1, 1, 34, 0,  74250, 0, 0 },
    { 3840, 2160, 1276, 88, 296,  8, 10, 72, 1, 1, 93, 0, 297000, 0, 0 },
    { 3840, 2160, 1056, 88, 296,  8, 10, 72, 1, 1, 94, 0, 297000, 0, 0 },
    { 3840, 2160,  176, 88, 296,  8, 10, 72, 1, 1, 95, 0, 297000, 0, 0 },
};

static const OEMBRIDGE_MODE *CeaMode(UCHAR Vic)
{
    ULONG i;

    for (i = 0; i < ARRAYSIZE(CeaModes); i++)
    {
        if (CeaModes[i].Vic == Vic)
        {
            return &CeaModes[i];
        }
    }
    return NULL;
}

static BOOLEAN EdidDecodeDtd(const UCHAR *d, OEMBRIDGE_MODE *m)
{
    ULONG pclk = ((((ULONG)d[1]) << 8) | d[0]) * 10;
    ULONG hAct, hBlank, vAct, vBlank, hFp, hSync, vFp, vSync;

    RtlZeroMemory(m, sizeof(*m));

    if (pclk == 0)
    {
        return FALSE;
    }
    if (d[17] & 0x80)
    {
        return FALSE;
    }

    hAct   = ((((ULONG)d[4])  & 0xF0) << 4) | d[2];
    hBlank = ((((ULONG)d[4])  & 0x0F) << 8) | d[3];
    vAct   = ((((ULONG)d[7])  & 0xF0) << 4) | d[5];
    vBlank = ((((ULONG)d[7])  & 0x0F) << 8) | d[6];
    hFp    = ((((ULONG)d[11]) & 0xC0) << 2) | d[8];
    hSync  = ((((ULONG)d[11]) & 0x30) << 4) | d[9];
    vFp    = ((((ULONG)d[11]) & 0x0C) << 2) | ((d[10] & 0xF0) >> 4);
    vSync  = ((((ULONG)d[11]) & 0x03) << 4) | (d[10] & 0x0F);

    if (hAct == 0 || vAct == 0 ||
        hBlank <= hFp + hSync || vBlank <= vFp + vSync ||
        hAct > 0xFFF || vAct > 0xFFF)
    {
        return FALSE;
    }

    m->Width   = (USHORT)hAct;
    m->Height  = (USHORT)vAct;
    m->HFront  = (USHORT)hFp;
    m->HSync   = (USHORT)hSync;
    m->HBack   = (USHORT)(hBlank - hFp - hSync);
    m->VFront  = (USHORT)vFp;
    m->VSync   = (USHORT)vSync;
    m->VBack   = (USHORT)(vBlank - vFp - vSync);
    m->PclkKhz = pclk;

    if ((d[17] & 0x18) == 0x18)
    {
        m->HPolPositive = (d[17] & 0x02) ? 1 : 0;
        m->VPolPositive = (d[17] & 0x04) ? 1 : 0;
    }
    else
    {
        m->HPolPositive = 1;
        m->VPolPositive = 1;
    }
    return TRUE;
}

static BOOLEAN ModeFits(const OEMBRIDGE_MODE *m)
{
    return (m->PclkKhz <= BRIDGE_MAX_PCLK_KHZ && m->Width <= BRIDGE_MAX_H_ACTIVE)
           ? TRUE : FALSE;
}

static VOID ModeFinish(OEMBRIDGE_MODE *m)
{
    ULONGLONG total = (ULONGLONG)(m->Width + m->HFront + m->HSync + m->HBack) *
                      (ULONGLONG)(m->Height + m->VFront + m->VSync + m->VBack);
    ULONG hz;

    if (total == 0)
    {
        m->RefreshCentiHz = 0;
        m->Packed         = 0;
        return;
    }
    m->RefreshCentiHz = (ULONG)(((ULONGLONG)m->PclkKhz * 100000ULL + total / 2) / total);
    hz = (m->RefreshCentiHz + 50) / 100;
    m->Packed = (hz == 0 || hz > 0xFF) ? 0 : OEMBRIDGE_MODE_PACK(m->Width, m->Height, hz);
}

static VOID ModeAdd(PDEVICE_CONTEXT ctx, OEMBRIDGE_MODE *m, UCHAR Source)
{
    ULONG i;

    if (!ModeFits(m))
    {
        return;
    }
    ModeFinish(m);
    if (m->Packed == 0)
    {
        return;
    }
    for (i = 0; i < ctx->SinkModeCount; i++)
    {
        if (ctx->SinkModes[i].Packed == m->Packed)
        {
            return;
        }
    }
    if (ctx->SinkModeCount >= OEMBRIDGE_MAX_MODES)
    {
        return;
    }
    m->Source = Source;
    ctx->SinkModes[ctx->SinkModeCount++] = *m;
}

VOID OEMBridgeCacheSinkModes(WDFDEVICE Device, const UCHAR *Edid, ULONG Length)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    OEMBRIDGE_MODE  m;
    ULONG           off, i;

    ctx->SinkModeCount       = 0;
    ctx->SinkEdidValid       = 0;
    ctx->SinkPhysicalAddress = 0xFFFF;
    RtlZeroMemory(ctx->SinkName, sizeof(ctx->SinkName));

    if (Length < 128)
    {
        return;
    }

    for (off = 0x36; off <= 0x6C; off += 18)
    {
        const UCHAR *d = &Edid[off];

        if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0xFC)
        {
            ULONG n = 0;

            for (i = 0; i < 13; i++)
            {
                UCHAR c = d[5 + i];

                if (c == 0x0A || c == 0)
                {
                    break;
                }
                ctx->SinkName[n++] = (c < 0x20 || c > 0x7E) ? '?' : (CHAR)c;
            }
            while (n > 0 && ctx->SinkName[n - 1] == ' ')
            {
                ctx->SinkName[--n] = 0;
            }
            break;
        }
    }

    if (EdidDecodeDtd(&Edid[0x36], &m))
    {
        ModeAdd(ctx, &m, OEMBRIDGE_MODE_SRC_PREFERRED);
    }

    for (off = 0x36 + 18; off <= 0x6C; off += 18)
    {
        if (EdidDecodeDtd(&Edid[off], &m))
        {
            ModeAdd(ctx, &m, OEMBRIDGE_MODE_SRC_BASE_DTD);
        }
    }

    if (Length >= 256 && Edid[128] == 0x02)
    {
        ULONG dtdStart = 128 + Edid[130];
        ULONG pos;

        if (Edid[130] > 4)
        {
            pos = 128 + 4;
            while (pos < dtdStart && pos < 256)
            {
                ULONG tag = Edid[pos] >> 5;
                ULONG len = Edid[pos] & 0x1F;
                ULONG k;

                if (tag == 2)
                {
                    for (k = 1; k <= len && pos + k < 256; k++)
                    {
                        const OEMBRIDGE_MODE *c = CeaMode(Edid[pos + k] & 0x7F);

                        if (c != NULL)
                        {
                            m = *c;
                            ModeAdd(ctx, &m, OEMBRIDGE_MODE_SRC_CEA_VIC);
                        }
                    }
                }
                else if (tag == 3 && len >= 5 && pos + 5 < 256 &&
                         Edid[pos + 1] == 0x03 && Edid[pos + 2] == 0x0C && Edid[pos + 3] == 0x00)
                {

                    ctx->SinkPhysicalAddress = (USHORT)(((USHORT)Edid[pos + 4] << 8) | Edid[pos + 5]);
                }
                pos += 1 + len;
            }
        }

        if (Edid[130] >= 4)
        {
            for (pos = dtdStart; pos + 18 <= 256; pos += 18)
            {
                if (!EdidDecodeDtd(&Edid[pos], &m))
                {
                    break;
                }
                ModeAdd(ctx, &m, OEMBRIDGE_MODE_SRC_CEA_DTD);
            }
        }
    }

    ctx->SinkEdidValid = 1;

    Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
          "modes: sink '%s' advertises %u mode(s) the bridge can carry",
          ctx->SinkName, ctx->SinkModeCount);
    for (i = 0; i < ctx->SinkModeCount; i++)
    {
        const OEMBRIDGE_MODE *s = &ctx->SinkModes[i];

        Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
              "modes:   %ux%u @ %u.%02u Hz pclk=%u kHz vic=%u src=%u",
              s->Width, s->Height, s->RefreshCentiHz / 100, s->RefreshCentiHz % 100,
              s->PclkKhz, s->Vic, s->Source);
    }
}

static NTSTATUS ReadOutputModeVariable(ULONG *Mode)
{
    UNICODE_STRING name;
    GUID           guid = GUID_OEMBRIDGE_DISPLAY_VARS;
    ULONG          value = OEMBRIDGE_MODE_AUTO;
    ULONG          length = sizeof(value);
    ULONG          attributes = 0;
    NTSTATUS       status;

    RtlInitUnicodeString(&name, OEMBRIDGE_OUTPUT_MODE_VAR);
    status = ExGetFirmwareEnvironmentVariable(&name, &guid, &value, &length, &attributes);

    *Mode = (NT_SUCCESS(status) && length == sizeof(value)) ? value : OEMBRIDGE_MODE_AUTO;
    return status;
}

static NTSTATUS WriteOutputModeVariable(ULONG Mode)
{
    UNICODE_STRING name;
    GUID           guid = GUID_OEMBRIDGE_DISPLAY_VARS;

    RtlInitUnicodeString(&name, OEMBRIDGE_OUTPUT_MODE_VAR);
    return ExSetFirmwareEnvironmentVariable(&name, &guid, &Mode, sizeof(Mode),
                                            OEMBRIDGE_EFI_VAR_ATTRIBUTES);
}

VOID
OEMBridgeLoadLoggingSettings(_In_ WDFDEVICE Device)
{
    DECLARE_CONST_UNICODE_STRING(name, L"LoggingLevel");
    WDFKEY key;
    ULONG level = OEMBRIDGE_LOG_DISABLED;

    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_QUERY_VALUE,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key)))
    {
        if (!NT_SUCCESS(WdfRegistryQueryULong(key, &name, &level)) ||
            !OEMBridgeLoggingLevelValid(level))
        {
            level = OEMBRIDGE_LOG_DISABLED;
        }
        WdfRegistryClose(key);
    }
    InterlockedExchange(&g_OemBridgeLogLevel, (LONG)level);
}

static NTSTATUS WriteLoggingLevel(_In_ WDFDEVICE Device, _In_ ULONG Level)
{
    DECLARE_CONST_UNICODE_STRING(name, L"LoggingLevel");
    WDFKEY key;
    NTSTATUS status;

    status = WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_SET_VALUE,
                                      WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status)) return status;
    status = WdfRegistryAssignULong(key, &name, Level);
    WdfRegistryClose(key);
    if (NT_SUCCESS(status)) InterlockedExchange(&g_OemBridgeLogLevel, (LONG)Level);
    return status;
}

BOOLEAN
OEMBridgeHandleSettingsIoctl(
    _In_ WDFDEVICE  Device,
    _In_ WDFREQUEST Request,
    _In_ ULONG      IoControlCode,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    NTSTATUS        status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode == IOCTL_OEMBRIDGE_LOGGING_GET_STATE)
    {
        OEMBRIDGE_LOGGING_STATE *logging = NULL;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*logging), &logging, NULL);
        if (NT_SUCCESS(status))
        {
            logging->Size = sizeof(*logging);
            logging->Level = (ULONG)g_OemBridgeLogLevel;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*logging));
        }
        else
        {
            WdfRequestComplete(Request, status);
        }
        return TRUE;
    }

    if (IoControlCode == IOCTL_OEMBRIDGE_LOGGING_SET_LEVEL)
    {
        ULONG *in = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), &in, NULL);
        if (NT_SUCCESS(status))
        {
            ULONG level = *in;
            status = OEMBridgeLoggingLevelValid(level) ?
                     WriteLoggingLevel(Device, level) : STATUS_INVALID_PARAMETER;
        }
        WdfRequestComplete(Request, status);
        return TRUE;
    }

    if (IoControlCode == IOCTL_OEMBRIDGE_GET_DISPLAY_INFO)
    {
        OEMBRIDGE_DISPLAY_INFO *info = NULL;
        size_t                  len = 0;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*info), &info, &len);
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
            return TRUE;
        }

        RtlZeroMemory(info, sizeof(*info));
        info->Size = sizeof(*info);

        WdfWaitLockAcquire(ctx->BridgeLock, NULL);
        info->SinkPresent = ctx->LastHpd;
        info->EdidValid   = ctx->SinkEdidValid;
        RtlCopyMemory(info->SinkName, ctx->SinkName, sizeof(info->SinkName));
        info->ModeCount   = ctx->SinkModeCount;
        RtlCopyMemory(info->Modes, ctx->SinkModes, ctx->SinkModeCount * sizeof(OEMBRIDGE_MODE));
        if (ctx->LastInputLive)
        {
            info->ActiveWidth  = ctx->LockedHAct;
            info->ActiveHeight = ctx->LockedVAct;
            info->ActiveMode   = LT9611LockedModePacked(ctx);
        }
        WdfWaitLockRelease(ctx->BridgeLock);

        info->VariableStatus = ReadOutputModeVariable(&info->RequestedMode);

        Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
              "settings: GET requested=0x%08X (var 0x%08X) active %ux%u mode 0x%08X, %u sink mode(s)",
              info->RequestedMode, info->VariableStatus,
              info->ActiveWidth, info->ActiveHeight, info->ActiveMode, info->ModeCount);

        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*info));
        return TRUE;
    }

    if (IoControlCode == IOCTL_OEMBRIDGE_SET_OUTPUT_MODE)
    {
        ULONG  *in = NULL;
        size_t  len = 0;
        ULONG   mode;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), &in, &len);
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
            return TRUE;
        }
        mode = *in;

        if (mode != OEMBRIDGE_MODE_AUTO &&
            (OEMBRIDGE_MODE_WIDTH(mode) == 0 || OEMBRIDGE_MODE_HEIGHT(mode) == 0 ||
             OEMBRIDGE_MODE_REFRESH(mode) == 0))
        {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return TRUE;
        }

        status = WriteOutputModeVariable(mode);
        Trace(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
              "settings: SET HdmiOutputMode=0x%08X (%ux%u@%u) -> 0x%08X",
              mode, OEMBRIDGE_MODE_WIDTH(mode), OEMBRIDGE_MODE_HEIGHT(mode),
              OEMBRIDGE_MODE_REFRESH(mode), status);

        WdfRequestComplete(Request, status);
        return TRUE;
    }

    return FALSE;
}
