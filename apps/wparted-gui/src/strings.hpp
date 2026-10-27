// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// UI strings. English and German are built in; the language follows the
// Windows UI language. Adding a language means adding one column to the table
// in strings.cpp (a resource-based catalogue is planned, see docs/design).
#pragma once

namespace wparted::ui {

enum class S {
    AppTitle,
    MenuApp, MenuRefresh, MenuDevices, MenuRestartAdmin, MenuQuit,
    MenuEdit, MenuUndo, MenuClearOps, MenuApplyOps,
    MenuView, MenuDeviceInfo, MenuPendingOps, MenuTheme, MenuThemeSystem, MenuThemeLight, MenuThemeDark, MenuFsSupport,
    MenuDevice, MenuCreateTable, MenuExportReport,
    MenuPartition, MenuNew, MenuDelete, MenuResizeMove, MenuCopy, MenuPaste, MenuFormatTo, MenuManageFlags, MenuCheck,
    MenuLabelFs, MenuNewUuid, MenuPartitionInfo,
    MenuTools, MenuOpenLogFolder,
    MenuHelp, MenuHelpContents, MenuAbout,
    BtnRefresh, BtnNew, BtnDelete, BtnResizeMove, BtnCopy, BtnPaste, BtnUndo, BtnApply,
    ColPartition, ColName, ColFileSystem, ColMountPoint, ColLabel, ColSize, ColUsed, ColUnused, ColFlags, ColStatus,
    Unallocated, UnknownRegion, VolumeOnly, Scanning, ScanDoneFmt, NoDisks, ReadOnlyBuild, PlannedFeature,
    NotElevatedBanner, NotElevatedStatus, ElevatedStatus,
    PendingNone,
    StatusWindows, StatusSystemCritical, StatusMounted, StatusEncrypted, StatusNotReadable,
    InfoTitle, DeviceInfoTitle, AboutTitle, AboutText, FsSupportTitle,
    ColFsDetect, ColFsUsage, ColFsCreate, ColFsGrow, ColFsShrink, ColFsMove, ColFsCopy, ColFsCheck, ColFsLabel, ColFsUuid,
    ExportOk, ExportFailed,
    Cancel, MenuName, MenuOpenImage, MenuSavePlan, PlanningTitle, FormatCleared,
    DlgNewTitle, DlgResizeTitle, DlgPasteTitle, DlgLabelTitle, DlgNameTitle, DlgFlagsTitle, DlgTableTitle,
    FieldFreeBefore, FieldNewSize, FieldFreeAfter, FieldAlign, AlignMiB, AlignNone, FieldCreateAs,
    KindPrimary, KindExtended, KindLogical, FieldFs, FieldLabel, FieldName, FieldTableType, FieldSource,
    TableWarning, ErrTooLarge, ErrNeedsTable, PendingFmt, ApplyTitle, ApplyNoExec, BtnExportPlan, OpRejected,
    ImageOpenFailed, CopiedFmt, Count_
};

void initLanguage();             // Selects the language from the Windows UI language.
const wchar_t* tr(S id);

}  // namespace wparted::ui
