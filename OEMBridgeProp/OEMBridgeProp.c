/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <winioctl.h>
#include <initguid.h>
#include <strsafe.h>
#include "..\OEMBridge\OEMBridgeIoctl.h"
#include "resource.h"

static HINSTANCE g_hInst;

typedef struct _PAGE_CONTEXT
{
    HDEVINFO               DevInfo;
    SP_DEVINFO_DATA        DevInfoData;
    HANDLE                 Device;
    DWORD                  OpenError;
    BOOL                   InfoValid;
    OEMBRIDGE_DISPLAY_INFO Info;
    BOOL                   AudioValid;
    OEMBRIDGE_AUDIO_STATE  Audio;
    BOOL                   CecValid;
    OEMBRIDGE_CEC_STATE    Cec;
    BOOL                   LoggingValid;
    OEMBRIDGE_LOGGING_STATE Logging;
} PAGE_CONTEXT;

static BOOL BridgeIoctl(HANDLE Device, DWORD Code, LPVOID In, DWORD InLen,
                        LPVOID Out, DWORD OutLen, LPDWORD Returned, DWORD TimeoutMs);

static VOID QueryLogging(PAGE_CONTEXT *ctx)
{
    DWORD returned = 0;

    ctx->LoggingValid = FALSE;
    if (ctx->Device == INVALID_HANDLE_VALUE) return;
    ZeroMemory(&ctx->Logging, sizeof(ctx->Logging));
    if (BridgeIoctl(ctx->Device, IOCTL_OEMBRIDGE_LOGGING_GET_STATE, NULL, 0,
                    &ctx->Logging, sizeof(ctx->Logging), &returned, 3000) &&
        returned >= sizeof(ctx->Logging) && ctx->Logging.Size == sizeof(ctx->Logging) &&
        OEMBridgeLoggingLevelValid(ctx->Logging.Level))
    {
        ctx->LoggingValid = TRUE;
    }
}

static VOID PopulateLogging(HWND hDlg, PAGE_CONTEXT *ctx)
{
    static const struct { ULONG Level; LPCWSTR Name; } choices[] = {
        { OEMBRIDGE_LOG_DISABLED, L"Disabled" },
        { OEMBRIDGE_LOG_ERROR, L"Errors" },
        { OEMBRIDGE_LOG_WARNING, L"Warnings" },
        { OEMBRIDGE_LOG_INFORMATION, L"Information" },
        { OEMBRIDGE_LOG_VERBOSE, L"Verbose" }
    };
    HWND combo = GetDlgItem(hDlg, IDC_LOGGING_LEVEL);
    ULONG i;
    int selected = 0;
    BOOL populated = TRUE;

    ComboBox_ResetContent(combo);
    for (i = 0; i < ARRAYSIZE(choices); i++)
    {
        int index = ComboBox_AddString(combo, choices[i].Name);
        if (index < 0 || ComboBox_SetItemData(combo, index, choices[i].Level) == CB_ERR)
        {
            populated = FALSE;
            break;
        }
        if (ctx->LoggingValid && choices[i].Level == ctx->Logging.Level) selected = index;
    }
    if (populated) ComboBox_SetCurSel(combo, selected);
    EnableWindow(combo, populated && ctx->LoggingValid);
    SetDlgItemTextW(hDlg, IDC_LOGGING_STATUS,
                    populated && ctx->LoggingValid ?
                    L"Kernel debugger output for the bridge and CEC adapter. Applies without restarting Windows." :
                    L"Logging control is unavailable with this driver.");
}

static VOID QueryAudio(PAGE_CONTEXT *ctx)
{
    DWORD returned = 0;

    ctx->AudioValid = FALSE;
    if (ctx->Device == INVALID_HANDLE_VALUE) return;
    ZeroMemory(&ctx->Audio, sizeof(ctx->Audio));
    if (BridgeIoctl(ctx->Device, IOCTL_OEMBRIDGE_AUDIO_GET_STATE, NULL, 0,
                    &ctx->Audio, sizeof(ctx->Audio), &returned, 3000) &&
        returned >= sizeof(ctx->Audio) && ctx->Audio.Size == sizeof(ctx->Audio) &&
        ctx->Audio.Enabled <= 1)
    {
        ctx->AudioValid = TRUE;
    }
}

static VOID PopulateAudio(HWND hDlg, PAGE_CONTEXT *ctx)
{
    HWND box = GetDlgItem(hDlg, IDC_AUDIO_ENABLE);

    EnableWindow(box, ctx->AudioValid);
    Button_SetCheck(box, ctx->AudioValid && ctx->Audio.Enabled ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextW(hDlg, IDC_AUDIO_STATUS,
                    ctx->AudioValid ? L"Applies without restarting Windows." :
                    (ctx->InfoValid ? L"Audio control is unavailable with this driver." : L""));
}

static VOID QueryCec(PAGE_CONTEXT *ctx)
{
    DWORD returned = 0;

    ctx->CecValid = FALSE;
    if (ctx->Device == INVALID_HANDLE_VALUE)
    {
        return;
    }
    ZeroMemory(&ctx->Cec, sizeof(ctx->Cec));
    if (BridgeIoctl(ctx->Device, IOCTL_OEMBRIDGE_CEC_GET_STATE, NULL, 0,
                    &ctx->Cec, sizeof(ctx->Cec), &returned, 3000) &&
        returned >= sizeof(ctx->Cec) && ctx->Cec.Size == sizeof(ctx->Cec))
    {
        ctx->CecValid = TRUE;
    }
}

static VOID FormatPhysicalAddress(USHORT Pa, WCHAR *Buf, size_t Cch)
{
    if (Pa == 0xFFFF)
    {
        StringCchCopyW(Buf, Cch, L"unknown");
        return;
    }
    StringCchPrintfW(Buf, Cch, L"%u.%u.%u.%u",
                     (Pa >> 12) & 0xF, (Pa >> 8) & 0xF, (Pa >> 4) & 0xF, Pa & 0xF);
}

static VOID PopulateCec(HWND hDlg, PAGE_CONTEXT *ctx)
{
    HWND  box = GetDlgItem(hDlg, IDC_CEC_ENABLE);
    WCHAR text[200];
    WCHAR pa[24];

    if (!ctx->CecValid)
    {

        Button_SetCheck(box, BST_UNCHECKED);
        EnableWindow(box, FALSE);
        SetDlgItemTextW(hDlg, IDC_CEC_STATUS,
                        ctx->InfoValid ? L"This driver build has no CEC engine." : L"");
        return;
    }

    EnableWindow(box, TRUE);
    Button_SetCheck(box, ctx->Cec.Enabled ? BST_CHECKED : BST_UNCHECKED);

    if (!ctx->Cec.Enabled)
    {
        StringCchCopyW(text, ARRAYSIZE(text), L"CEC engine off.");
    }
    else
    {
        FormatPhysicalAddress(ctx->Cec.PhysicalAddress, pa, ARRAYSIZE(pa));
        if (ctx->Cec.LogicalAddress == 15)
        {
            StringCchPrintfW(text, ARRAYSIZE(text),
                             L"CEC engine %s, no logical address claimed yet, physical address %s. "
                             L"Sent %u (%u not acknowledged), received %u.",
                             ctx->Cec.HwOn ? L"on" : L"not programmed", pa,
                             ctx->Cec.TxOk, ctx->Cec.TxNack, ctx->Cec.RxFrames);
        }
        else
        {
            StringCchPrintfW(text, ARRAYSIZE(text),
                             L"CEC engine %s, logical address %u, physical address %s. "
                             L"Sent %u (%u not acknowledged), received %u.",
                             ctx->Cec.HwOn ? L"on" : L"not programmed", ctx->Cec.LogicalAddress, pa,
                             ctx->Cec.TxOk, ctx->Cec.TxNack, ctx->Cec.RxFrames);
        }
    }
    SetDlgItemTextW(hDlg, IDC_CEC_STATUS, text);
}

static VOID OpenBridge(PAGE_CONTEXT *ctx)
{
    HDEVINFO                  set;
    SP_DEVICE_INTERFACE_DATA  ifData;
    DWORD                     index;

    ctx->Device    = INVALID_HANDLE_VALUE;
    ctx->OpenError = ERROR_NOT_FOUND;

    set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_OEMBRIDGE, NULL, NULL,
                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
    {
        ctx->OpenError = GetLastError();
        return;
    }

    ZeroMemory(&ifData, sizeof(ifData));
    ifData.cbSize = sizeof(ifData);

    for (index = 0;
         SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_OEMBRIDGE, index, &ifData);
         index++)
    {
        DWORD                              needed = 0;
        SP_DEVINFO_DATA                    owner;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;

        SetupDiGetDeviceInterfaceDetailW(set, &ifData, NULL, 0, &needed, NULL);
        if (needed == 0)
        {
            continue;
        }
        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
        if (detail == NULL)
        {
            ctx->OpenError = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        ZeroMemory(&owner, sizeof(owner));
        owner.cbSize = sizeof(owner);

        if (SetupDiGetDeviceInterfaceDetailW(set, &ifData, detail, needed, NULL, &owner) &&
            owner.DevInst == ctx->DevInfoData.DevInst)
        {
            ctx->Device = CreateFileW(detail->DevicePath,
                                      GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            ctx->OpenError = (ctx->Device == INVALID_HANDLE_VALUE) ? GetLastError() : ERROR_SUCCESS;
            HeapFree(GetProcessHeap(), 0, detail);
            break;
        }
        HeapFree(GetProcessHeap(), 0, detail);
    }

    SetupDiDestroyDeviceInfoList(set);
}

static BOOL BridgeIoctl(HANDLE Device, DWORD Code, LPVOID In, DWORD InLen,
                        LPVOID Out, DWORD OutLen, LPDWORD Returned, DWORD TimeoutMs)
{
    OVERLAPPED ov;
    BOOL       ok;
    DWORD      err;

    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ov.hEvent == NULL)
    {
        return FALSE;
    }

    ok = DeviceIoControl(Device, Code, In, InLen, Out, OutLen, Returned, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject(ov.hEvent, TimeoutMs) == WAIT_OBJECT_0)
        {
            ok = GetOverlappedResult(Device, &ov, Returned, FALSE);
        }
        else
        {
            CancelIoEx(Device, &ov);
            GetOverlappedResult(Device, &ov, Returned, TRUE);
            SetLastError(WAIT_TIMEOUT);
            ok = FALSE;
        }
    }
    err = GetLastError();
    CloseHandle(ov.hEvent);
    SetLastError(err);
    return ok;
}

static VOID QueryInfo(PAGE_CONTEXT *ctx)
{
    DWORD returned = 0;

    ctx->InfoValid = FALSE;
    if (ctx->Device == INVALID_HANDLE_VALUE)
    {
        return;
    }
    ZeroMemory(&ctx->Info, sizeof(ctx->Info));
    if (BridgeIoctl(ctx->Device, IOCTL_OEMBRIDGE_GET_DISPLAY_INFO, NULL, 0,
                    &ctx->Info, sizeof(ctx->Info), &returned, 3000) &&
        returned >= sizeof(ctx->Info) && ctx->Info.Size == sizeof(ctx->Info))
    {
        if (ctx->Info.ModeCount > OEMBRIDGE_MAX_MODES)
        {
            ctx->Info.ModeCount = OEMBRIDGE_MAX_MODES;
        }
        ctx->Info.SinkName[sizeof(ctx->Info.SinkName) - 1] = 0;
        ctx->InfoValid = TRUE;
    }
    else
    {
        ctx->OpenError = GetLastError();
    }
}

static VOID FormatMode(ULONG Packed, WCHAR *Buf, size_t Cch)
{
    StringCchPrintfW(Buf, Cch, L"%u x %u @ %u Hz",
                     OEMBRIDGE_MODE_WIDTH(Packed), OEMBRIDGE_MODE_HEIGHT(Packed),
                     OEMBRIDGE_MODE_REFRESH(Packed));
}

static VOID Populate(HWND hDlg, PAGE_CONTEXT *ctx)
{
    HWND   combo = GetDlgItem(hDlg, IDC_MODE);
    WCHAR  text[160];
    WCHAR  mode[48];
    int    idx, sel = 0;
    BOOL   requestedListed = FALSE;
    ULONG  i;

    ComboBox_ResetContent(combo);

    idx = ComboBox_AddString(combo, L"Auto  (monitor's preferred mode, or the best one it advertises)");
    ComboBox_SetItemData(combo, idx, OEMBRIDGE_MODE_AUTO);

    if (!ctx->InfoValid)
    {
        StringCchPrintfW(text, ARRAYSIZE(text),
                         L"Cannot reach the bridge driver (error %u). Is OEMBridge started?",
                         ctx->OpenError);
        SetDlgItemTextW(hDlg, IDC_SINK, text);
        SetDlgItemTextW(hDlg, IDC_ACTIVE, L"");
        ComboBox_SetCurSel(combo, 0);
        EnableWindow(combo, FALSE);
        return;
    }

    for (i = 0; i < ctx->Info.ModeCount; i++)
    {
        const OEMBRIDGE_MODE *m = &ctx->Info.Modes[i];

        FormatMode(m->Packed, mode, ARRAYSIZE(mode));
        StringCchPrintfW(text, ARRAYSIZE(text), L"%s%s", mode,
                         (m->Source == OEMBRIDGE_MODE_SRC_PREFERRED) ? L"   (monitor preferred)" : L"");
        idx = ComboBox_AddString(combo, text);
        ComboBox_SetItemData(combo, idx, m->Packed);
        if (m->Packed == ctx->Info.RequestedMode)
        {
            sel = idx;
            requestedListed = TRUE;
        }
    }

    if (ctx->Info.RequestedMode != OEMBRIDGE_MODE_AUTO && !requestedListed)
    {
        FormatMode(ctx->Info.RequestedMode, mode, ARRAYSIZE(mode));
        StringCchPrintfW(text, ARRAYSIZE(text),
                         L"%s   (not advertised by this monitor - firmware uses Auto)", mode);
        idx = ComboBox_AddString(combo, text);
        ComboBox_SetItemData(combo, idx, ctx->Info.RequestedMode);
        sel = idx;
    }
    ComboBox_SetCurSel(combo, sel);
    EnableWindow(combo, TRUE);

    if (!ctx->Info.SinkPresent)
    {
        StringCchCopyW(text, ARRAYSIZE(text),
                       L"No monitor detected on the HDMI port. The list shows the last EDID read.");
    }
    else if (!ctx->Info.EdidValid)
    {
        StringCchCopyW(text, ARRAYSIZE(text),
                       L"Monitor connected, but its EDID could not be read.");
    }
    else
    {
        WCHAR name[16];
        int   n;

        for (n = 0; n < 14 && ctx->Info.SinkName[n] != 0; n++)
        {
            name[n] = (WCHAR)(UCHAR)ctx->Info.SinkName[n];
        }
        name[n] = 0;
        StringCchPrintfW(text, ARRAYSIZE(text), L"Monitor: %s  -  %u mode%s the bridge can drive",
                         (n > 0) ? name : L"(unnamed)", ctx->Info.ModeCount,
                         (ctx->Info.ModeCount == 1) ? L"" : L"s");
    }
    SetDlgItemTextW(hDlg, IDC_SINK, text);

    if (ctx->Info.ActiveMode != 0)
    {
        FormatMode(ctx->Info.ActiveMode, mode, ARRAYSIZE(mode));
        StringCchPrintfW(text, ARRAYSIZE(text), L"Current output: %s", mode);
    }
    else if (ctx->Info.ActiveWidth != 0)
    {
        StringCchPrintfW(text, ARRAYSIZE(text), L"Current output: %u x %u (timing not recognised)",
                         ctx->Info.ActiveWidth, ctx->Info.ActiveHeight);
    }
    else
    {
        StringCchCopyW(text, ARRAYSIZE(text), L"Current output: no signal from the display controller");
    }
    SetDlgItemTextW(hDlg, IDC_ACTIVE, text);
}

static VOID Apply(HWND hDlg, PAGE_CONTEXT *ctx)
{
    HWND  combo = GetDlgItem(hDlg, IDC_MODE);
    int   sel = ComboBox_GetCurSel(combo);
    DWORD returned = 0;
    WCHAR text[256];
    BOOL  failed = FALSE;

    if (ctx->LoggingValid)
    {
        HWND loggingCombo = GetDlgItem(hDlg, IDC_LOGGING_LEVEL);
        int loggingSelection = ComboBox_GetCurSel(loggingCombo);
        if (loggingSelection >= 0)
        {
            ULONG level = (ULONG)ComboBox_GetItemData(loggingCombo, loggingSelection);
            if (OEMBridgeLoggingLevelValid(level) && level != ctx->Logging.Level)
            {
                if (BridgeIoctl(ctx->Device, IOCTL_OEMBRIDGE_LOGGING_SET_LEVEL,
                                &level, sizeof(level), NULL, 0, &returned, 5000))
                {
                    QueryLogging(ctx);
                    PopulateLogging(hDlg, ctx);
                }
                else
                {
                    DWORD error = GetLastError();
                    QueryLogging(ctx);
                    PopulateLogging(hDlg, ctx);
                    StringCchPrintfW(text, ARRAYSIZE(text),
                                     L"The logging level could not be saved (error %u).", error);
                    MessageBoxW(hDlg, text, L"LT9611 HDMI Bridge", MB_OK | MB_ICONERROR);
                    failed = TRUE;
                }
            }
        }
    }

    if (ctx->InfoValid && sel >= 0)
    {
        ULONG packed = (ULONG)ComboBox_GetItemData(combo, sel);

        if (packed != ctx->Info.RequestedMode)
        {
            if (BridgeIoctl(ctx->Device, IOCTL_OEMBRIDGE_SET_OUTPUT_MODE,
                            &packed, sizeof(packed), NULL, 0, &returned, 10000))
            {
                ctx->Info.RequestedMode = packed;
                PropSheet_RebootSystem(GetParent(hDlg));
            }
            else
            {
                StringCchPrintfW(text, ARRAYSIZE(text),
                                 L"The HdmiOutputMode firmware variable could not be written (error %u).\n\n"
                                 L"The UEFI runtime variable service refused the write; the output mode is unchanged.",
                                 GetLastError());
                MessageBoxW(hDlg, text, L"LT9611 HDMI Bridge", MB_OK | MB_ICONERROR);
                failed = TRUE;
            }
        }
    }

    if (ctx->AudioValid)
    {
        ULONG enable = (Button_GetCheck(GetDlgItem(hDlg, IDC_AUDIO_ENABLE)) == BST_CHECKED) ? 1 : 0;

        if (enable != ctx->Audio.Enabled)
        {
            if (BridgeIoctl(ctx->Device, IOCTL_OEMBRIDGE_AUDIO_SET_ENABLE,
                            &enable, sizeof(enable), NULL, 0, &returned, 5000))
            {
                QueryAudio(ctx);
                PopulateAudio(hDlg, ctx);
            }
            else
            {
                DWORD error = GetLastError();
                QueryAudio(ctx);
                PopulateAudio(hDlg, ctx);
                StringCchPrintfW(text, ARRAYSIZE(text),
                                 L"The HDMI audio setting could not be fully applied (error %u).", error);
                MessageBoxW(hDlg, text, L"LT9611 HDMI Bridge", MB_OK | MB_ICONERROR);
                failed = TRUE;
            }
        }
    }

    if (ctx->CecValid)
    {
        ULONG enable = (Button_GetCheck(GetDlgItem(hDlg, IDC_CEC_ENABLE)) == BST_CHECKED) ? 1 : 0;

        if ((enable != 0) != (ctx->Cec.Enabled != 0))
        {
            if (BridgeIoctl(ctx->Device, IOCTL_OEMBRIDGE_CEC_SET_ENABLE,
                            &enable, sizeof(enable), NULL, 0, &returned, 5000))
            {
                QueryCec(ctx);
                PopulateCec(hDlg, ctx);
            }
            else
            {
                StringCchPrintfW(text, ARRAYSIZE(text),
                                 L"The CEC setting could not be applied (error %u).", GetLastError());
                MessageBoxW(hDlg, text, L"LT9611 HDMI Bridge", MB_OK | MB_ICONERROR);
                failed = TRUE;
            }
        }
    }

    SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, failed ? PSNRET_INVALID_NOCHANGEPAGE : PSNRET_NOERROR);
}

static INT_PTR CALLBACK PageDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PAGE_CONTEXT *ctx = (PAGE_CONTEXT *)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (uMsg)
    {
    case WM_INITDIALOG:
        ctx = (PAGE_CONTEXT *)((LPPROPSHEETPAGE)lParam)->lParam;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)ctx);
        OpenBridge(ctx);
        QueryInfo(ctx);
        Populate(hDlg, ctx);
        QueryAudio(ctx);
        PopulateAudio(hDlg, ctx);
        QueryCec(ctx);
        PopulateCec(hDlg, ctx);
        QueryLogging(ctx);
        PopulateLogging(hDlg, ctx);
        return TRUE;

    case WM_COMMAND:
        if ((LOWORD(wParam) == IDC_MODE || LOWORD(wParam) == IDC_LOGGING_LEVEL) &&
            HIWORD(wParam) == CBN_SELCHANGE)
        {
            PropSheet_Changed(GetParent(hDlg), hDlg);
            return TRUE;
        }
        if ((LOWORD(wParam) == IDC_AUDIO_ENABLE || LOWORD(wParam) == IDC_CEC_ENABLE) &&
            HIWORD(wParam) == BN_CLICKED)
        {
            PropSheet_Changed(GetParent(hDlg), hDlg);
            return TRUE;
        }
        break;

    case WM_NOTIFY:
        switch (((LPNMHDR)lParam)->code)
        {
        case PSN_APPLY:
            if (ctx != NULL)
            {
                Apply(hDlg, ctx);
            }
            return TRUE;
        case PSN_KILLACTIVE:
            SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, FALSE);
            return TRUE;
        default:
            break;
        }
        break;

    default:
        break;
    }
    return FALSE;
}

static UINT CALLBACK PageCallback(HWND hWnd, UINT uMsg, LPPROPSHEETPAGE ppsp)
{
    UNREFERENCED_PARAMETER(hWnd);

    if (uMsg == PSPCB_RELEASE && ppsp->lParam != 0)
    {
        PAGE_CONTEXT *ctx = (PAGE_CONTEXT *)ppsp->lParam;

        if (ctx->Device != INVALID_HANDLE_VALUE && ctx->Device != NULL)
        {
            CloseHandle(ctx->Device);
        }
        HeapFree(GetProcessHeap(), 0, ctx);
        ppsp->lParam = 0;
    }
    return 1;
}

BOOL WINAPI
OEMBridgePropPageProvider(
    PSP_PROPSHEETPAGE_REQUEST pRequest,
    LPFNADDPROPSHEETPAGE      pfnAddPage,
    LPARAM                    lParam)
{
    PROPSHEETPAGEW  psp;
    HPROPSHEETPAGE  hPage;
    PAGE_CONTEXT   *ctx;

    if (pRequest == NULL || pRequest->cbSize < sizeof(SP_PROPSHEETPAGE_REQUEST) ||
        pRequest->PageRequested != SPPSR_ENUM_ADV_DEVICE_PROPERTIES ||
        pRequest->DeviceInfoData == NULL)
    {
        return FALSE;
    }

    ctx = (PAGE_CONTEXT *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*ctx));
    if (ctx == NULL)
    {
        return FALSE;
    }
    ctx->DevInfo     = pRequest->DeviceInfoSet;
    ctx->DevInfoData = *pRequest->DeviceInfoData;
    ctx->Device      = INVALID_HANDLE_VALUE;

    ZeroMemory(&psp, sizeof(psp));
    psp.dwSize      = sizeof(psp);
    psp.dwFlags     = PSP_USECALLBACK;
    psp.hInstance   = g_hInst;
    psp.pszTemplate = MAKEINTRESOURCEW(IDD_OUTPUT_PAGE);
    psp.pfnDlgProc  = PageDlgProc;
    psp.pfnCallback = PageCallback;
    psp.lParam      = (LPARAM)ctx;

    hPage = CreatePropertySheetPageW(&psp);
    if (hPage == NULL)
    {
        HeapFree(GetProcessHeap(), 0, ctx);
        return FALSE;
    }
    if (!pfnAddPage(hPage, lParam))
    {
        DestroyPropertySheetPage(hPage);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(lpReserved);

    if (dwReason == DLL_PROCESS_ATTACH)
    {
        g_hInst = hInstance;
        DisableThreadLibraryCalls(hInstance);
    }
    return TRUE;
}
