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
    ImageOpenFailed, CopiedFmt,
    SectionPartitions, SectionPending, InsType, InsSize, InsUsage, InsRange, InsAlign, InsUuid, InsDevice, InsTable,
    InsPartitions, InsSectors, InsYes, InsNo, HintSelect, HintUnallocated, HintPartition, HintCritical, HintPlanned,
    HintImage, MenuRescue, RescueTitle, RescueRunning, RescueNone, RescueHint, RescueAligned, RescueUnaligned, BtnSaveDetails,
    OpCreateTable, OpCreate, OpDelete, OpResize, OpCopy, OpFormat, OpCheck, OpLabel, OpName, OpFlags, OpUuid,
    OpPartRef, OpExtRef, OpPartUnknown, KindPrimaryAdj, KindLogicalAdj, KindExtendedAdj, FsUnformatted,
    RiskLow, RiskMedium, RiskHigh, RiskCritical, RiskLabel,
    DiagOperation, DiagSourceErrors, DiagWriteProtected, DiagManaged, DiagIdentityMismatch, DiagIdentityUnknown,
    DiagExecUnavailable, DiagShrinkBelowUsed, DiagCapability, DiagSystemWindows, DiagSystemType, DiagBusy,
    DiagEncrypted, DiagEncryptedFormat, DiagOutOfBounds, DiagMbr2TiB, DiagTooSmall, DiagUnaligned,
    DiagPhysUnaligned, DiagTooManyPrimary, DiagMultipleExtended, DiagLogicalNoExt, DiagMultipleBootable,
    DiagLogicalOutside, DiagNoRoomEbr, DiagOverlapPrimary, DiagOverlapLogical, DiagImageExtendedMove,
    ActCreate, ActGrow, ActShrink, ActMove, ActCopy, ActCheck, ActLabel, ActUuid,
    SupSupported, SupLimited, SupOffline, SupExperimental, SupReadOnly, SupUnsupported, SupPlanned,
    InfoModel, InfoSerial, InfoFirmwareRevision, InfoPath, InfoInterface, InfoSize, InfoLogicalSectorSize, InfoPhysicalSectorSize, InfoSolidStateNoSeekPenalty, InfoRemovableMedia, InfoWriteProtected, InfoPartitionTable, InfoDiskGuid, InfoUsableSectors, InfoGptPrimaryBackupHeader, InfoMbrDiskSignature, InfoContainsWindows, InfoFileSystem, InfoLabel, InfoFileSystemUuidSerial, InfoPartitionType, InfoTypeGuid, InfoPartitionGuid, InfoName, InfoAttributes, InfoKind, InfoFirstSector, InfoLastSector, InfoTotalSectors, InfoAlignedTo1Mib, InfoAlignedToPhysicalSector, InfoFlags, InfoSystemCritical, InfoWindowsVolume, InfoMountPoints, InfoWindowsFileSystem, InfoUsed, InfoUnused, ValYes, ValNo, ValUnknown, ValBytes, ValValid, ValInvalid, ValNotReadable, ValDetection, ValProtected, InfoDiagnostics,
    PdNoValidHeader, PdPrimaryInvalid, PdBackupInvalid, PdMismatch, PdBackupLocation, PdEntryReversed, PdEntryOutside, PdEbrLoop, PdTooManyLogical, PdEbrUnreadable, PdEbrSignature, PdLogicalOutside, PdEbrLinkType, PdMultipleExtended, PdBeyondDevice, PdOverlap, PdTooSmall, PdReadFailed, PdSectorSize, PdWholeDevice, PdForeign, PdInvalidEntries, ChipManaged, HintManaged, AccDiskMap, AccDetails, CapCreate, CapGrow, CapShrink, CapMove, CapCopy, CapCheck, CapLabel, CapUuid, CapImages, FsSupportNote,
    ApplyImageReady, ApplyImageBlocked, BtnApplyImage, ConfirmImageTitle, ConfirmImageText, BtnApplyNow,
    ApplyRunning, ApplyDoneOk, ApplyDoneFailed, ApplyResultTitle, ApplyBusy, Count_
};

void initLanguage();             // Selects the language from the Windows UI language.
const wchar_t* tr(S id);

}  // namespace wparted::ui
