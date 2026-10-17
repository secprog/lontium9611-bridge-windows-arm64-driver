/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Licensed under the GNU General Public License, version 2 only.
 * Provided without warranty. See LICENSE for the license text.
 */

#pragma once

#include <ntddk.h>
#include <initguid.h>

EXTERN_C_START

DEFINE_GUID(GUID_DEVINTERFACE_OEM_BRIDGE_DRIVER,
    0xced8991e, 0xad1b, 0x44c7, 0xaf, 0xd8, 0x1d, 0x34, 0x2a, 0x21, 0x58, 0xea);

#define IOCTL_OEMPANEL_QUERY_INTERFACE_TABLE \
    CTL_CODE(0x0, 0x1000, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef void  *QDI_HandleType;
typedef ULONG  QDI_ErrorType;

#define QDI_STATUS_OK           0
#define QDI_STATUS_FAILED       1
#define QDI_STATUS_NOT_SUPPORTED 5

#define QDI_PANEL_EVENT_UPDATE_REQUEST        0x00000001
#define QDI_PANEL_EVENT_D_STATE_NOTIFICATION  0x00000004
#define QDI_PANEL_EVENT_INIT_SEQUENCE         0x00000010
#define QDI_PANEL_EVENT_RESET_START           0x00000100
#define QDI_PANEL_EVENT_AUDIO_NOTIFY          0x00000400
#define QDI_PANEL_EVENT_HIST_DATA_AVAIL       0x00010000
#define QDI_PANEL_EVENT_PRE_POWER             0x00040000
#define QDI_PANEL_EVENT_DRIVER_LOADED         0x00080000
#define QDI_PANEL_EVENT_AMBIENT_LEVEL_CHANGE  0x08000000
#define QDI_PANEL_EVENT_VSYNC_IRQ             0x10000000
#define QDI_PANEL_EVENT_VSYNC_DISABLED        0x40000000

typedef enum
{
    QDI_DISPLAY_NONE = 0,
    QDI_DISPLAY_PRIMARY,
    QDI_DISPLAY_SECONDARY,
    QDI_DISPLAY_THIRD,
    QDI_DISPLAY_EXTERNAL,
    QDI_DISPLAY_EXTERNAL2,
    QDI_DISPLAY_EXTERNAL3,
    QDI_DISPLAY_FRAMEBUFFER,
    QDI_DISPLAY_EXTERNAL4,
    QDI_DISPLAY_EXTERNAL5,
    QDI_DISPLAY_EXTERNAL6,
    QDI_DISPLAY_ID_MAX,
    QDI_DISPLAY_ID_FORCE_32BIT = 0x7FFFFFFF
} QDI_Display_IDType;

#define QDI_PANEL_API_MAJOR_REVISION  0x00000002
#define QDI_PANEL_API_MINOR_REVISION  0x00000001
#define QDI_PANEL_API_SUB_REVISION    0x00000074

#define QDI_PANEL_API_VERSION  \
    ((QDI_PANEL_API_MAJOR_REVISION << 24) | \
     (QDI_PANEL_API_MINOR_REVISION << 16) | \
      QDI_PANEL_API_SUB_REVISION)

typedef struct _QDI_Panel_AttrType      QDI_Panel_AttrType;
typedef struct _QDI_Display_PropertyConfigType QDI_Display_PropertyConfigType;

typedef struct _QDI_Panel_FunctionTableType
{
    ULONG           uPanelFunctionTableVersion;

    QDI_ErrorType (*QDI_Panel_Init)(
        QDI_HandleType       hOemHandle,
        QDI_Panel_AttrType  *pPanelConfig);

    QDI_ErrorType (*QDI_Panel_SetMode)(
        QDI_HandleType       hOemHandle,
        QDI_Panel_AttrType  *pPanelConfig);

    QDI_ErrorType (*QDI_Panel_Term)(
        QDI_HandleType       hOemHandle);

    QDI_ErrorType (*QDI_Panel_SetPower)(
        QDI_HandleType       hOemHandle,
        ULONG                bPowerOn);

    QDI_ErrorType (*QDI_Panel_SetBacklight)(
        QDI_HandleType       hOemHandle,
        ULONG                uLevel);

    QDI_ErrorType (*QDI_Panel_SetAbaBacklightRatio)(
        QDI_HandleType       hOemHandle,
        ULONG                uRatio);

    QDI_ErrorType (*QDI_Panel_Event)(
        QDI_HandleType       hOemHandle,
        ULONG                eEvents,
        void                *pEventData,
        ULONG                uEventDataSize);

    QDI_ErrorType (*QDI_Panel_Status)(
        QDI_HandleType       hOemHandle);

    QDI_ErrorType (*QDI_Panel_GetPropertyConfig)(
        QDI_HandleType                  hOemHandle,
        QDI_Display_PropertyConfigType *pPropertyData);

    QDI_ErrorType (*QDI_Panel_SetBrightness)(
        QDI_HandleType       hOemHandle,
        ULONG                uLevel,
        ULONG                uTransitionTimeMs);

} QDI_Panel_FunctionTableType;

typedef struct _QDI_DisplayUtil_FunctionTableType
{
    QDI_ErrorType (*QDI_DisplayUtil_Register)(
        QDI_Display_IDType            eDisplayId,
        QDI_HandleType                hOemHandle,
        QDI_Panel_FunctionTableType  *pFunctionTable,
        ULONG                         uFlags);

    QDI_ErrorType (*QDI_DisplayUtil_SendCmdBuf)();
    QDI_ErrorType (*QDI_DisplayUtil_ReadCmdBuf)();
    QDI_ErrorType (*QDI_DisplayUtil_GetLastDSIErrors)();
    QDI_ErrorType (*QDI_DisplayUtil_LinearFrameBufferInfo)();
    QDI_ErrorType (*QDI_DisplayUtil_LinearFrameBufferUpdate)();
    QDI_ErrorType (*QDI_DisplayUtil_ResetDisplay)();
    QDI_ErrorType (*QDI_DisplayUtil_SetProperty)();
    QDI_ErrorType (*QDI_DisplayUtil_GetProperty)();

} QDI_DisplayUtil_FunctionTableType;

C_ASSERT(sizeof(QDI_Panel_FunctionTableType) == 88);
C_ASSERT(sizeof(QDI_DisplayUtil_FunctionTableType) == 72);
C_ASSERT(FIELD_OFFSET(QDI_Panel_FunctionTableType, QDI_Panel_Init)     == 0x08);
C_ASSERT(FIELD_OFFSET(QDI_Panel_FunctionTableType, QDI_Panel_SetMode)  == 0x10);
C_ASSERT(FIELD_OFFSET(QDI_Panel_FunctionTableType, QDI_Panel_SetBrightness) == 0x50);

NTSTATUS
QdiBridgeInitialize(
    _In_ WDFDEVICE Device);

EXTERN_C_END
