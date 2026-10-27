// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "strings.hpp"

#include <windows.h>

namespace wparted::ui {

namespace {

struct Entry {
    S id;
    const wchar_t* en;
    const wchar_t* de;
};

// clang-format off
const Entry kStrings[] = {
    {S::AppTitle, L"WParted", L"WParted"},
    {S::MenuApp, L"&WParted", L"&WParted"},
    {S::MenuRefresh, L"&Refresh Devices\tCtrl+R", L"Geräte &aktualisieren\tStrg+R"},
    {S::MenuDevices, L"&Devices", L"&Geräte"},
    {S::MenuRestartAdmin, L"Restart as &Administrator", L"Als &Administrator neu starten"},
    {S::MenuQuit, L"&Quit\tCtrl+Q", L"&Beenden\tStrg+Q"},
    {S::MenuEdit, L"&Edit", L"&Bearbeiten"},
    {S::MenuUndo, L"&Undo Last Operation\tCtrl+Z", L"Letzte Operation &rückgängig\tStrg+Z"},
    {S::MenuClearOps, L"&Clear All Operations", L"Alle Operationen &verwerfen"},
    {S::MenuApplyOps, L"&Apply All Operations\tCtrl+Enter", L"Alle Operationen &anwenden\tStrg+Eingabe"},
    {S::MenuView, L"&View", L"&Ansicht"},
    {S::MenuDeviceInfo, L"Device &Information", L"Geräte&informationen"},
    {S::MenuPendingOps, L"Pending &Operations", L"Ausstehende &Operationen"},
    {S::MenuTheme, L"&Theme", L"&Design"},
    {S::MenuThemeSystem, L"&System", L"&System"},
    {S::MenuThemeLight, L"&Light", L"&Hell"},
    {S::MenuThemeDark, L"&Dark", L"&Dunkel"},
    {S::MenuFsSupport, L"&File System Support", L"&Dateisystemunterstützung"},
    {S::MenuDevice, L"&Device", L"&Gerät"},
    {S::MenuCreateTable, L"&Create Partition Table...", L"Partitions&tabelle erstellen..."},
    {S::MenuExportReport, L"&Export Report...", L"Bericht &exportieren..."},
    {S::MenuPartition, L"&Partition", L"&Partition"},
    {S::MenuNew, L"&New\tInsert", L"&Neu\tEinfg"},
    {S::MenuDelete, L"&Delete\tDelete", L"&Löschen\tEntf"},
    {S::MenuResizeMove, L"&Resize/Move", L"&Größe ändern/Verschieben"},
    {S::MenuCopy, L"&Copy\tCtrl+C", L"&Kopieren\tStrg+C"},
    {S::MenuPaste, L"&Paste\tCtrl+V", L"&Einfügen\tStrg+V"},
    {S::MenuFormatTo, L"&Format to", L"&Formatieren als"},
    {S::MenuManageFlags, L"Manage Flags", L"Flags verwalten"},
    {S::MenuCheck, L"C&heck", L"&Prüfen"},
    {S::MenuLabelFs, L"&Label File System", L"Dateisystem&bezeichnung"},
    {S::MenuNewUuid, L"New UU&ID", L"Neue UU&ID"},
    {S::MenuPartitionInfo, L"&Information\tEnter", L"&Informationen\tEingabe"},
    {S::MenuTools, L"&Tools", L"&Werkzeuge"},
    {S::MenuOpenLogFolder, L"Open &Log Folder", L"&Protokollordner öffnen"},
    {S::MenuHelp, L"&Help", L"&Hilfe"},
    {S::MenuHelpContents, L"&Contents\tF1", L"&Inhalt\tF1"},
    {S::MenuAbout, L"&About WParted", L"&Über WParted"},
    {S::BtnRefresh, L"Refresh", L"Aktualisieren"},
    {S::BtnNew, L"New", L"Neu"},
    {S::BtnDelete, L"Delete", L"Löschen"},
    {S::BtnResizeMove, L"Resize/Move", L"Größe/Verschieben"},
    {S::BtnCopy, L"Copy", L"Kopieren"},
    {S::BtnPaste, L"Paste", L"Einfügen"},
    {S::BtnUndo, L"Undo", L"Rückgängig"},
    {S::BtnApply, L"Apply", L"Anwenden"},
    {S::ColPartition, L"Partition", L"Partition"},
    {S::ColName, L"Name", L"Name"},
    {S::ColFileSystem, L"File System", L"Dateisystem"},
    {S::ColMountPoint, L"Mount Point", L"Einhängepunkt"},
    {S::ColLabel, L"Label", L"Bezeichnung"},
    {S::ColSize, L"Size", L"Größe"},
    {S::ColUsed, L"Used", L"Belegt"},
    {S::ColUnused, L"Unused", L"Frei"},
    {S::ColFlags, L"Flags", L"Flags"},
    {S::ColStatus, L"Status", L"Status"},
    {S::Unallocated, L"unallocated", L"nicht zugeordnet"},
    {S::UnknownRegion, L"unknown (table not read)", L"unbekannt (Tabelle nicht gelesen)"},
    {S::VolumeOnly, L"volume", L"Volume"},
    {S::Scanning, L"Scanning all devices...", L"Alle Geräte werden durchsucht..."},
    {S::ScanDoneFmt, L"%d device(s) found in %d ms. Read-only analysis; nothing was changed.",
                     L"%d Gerät(e) in %d ms gefunden. Nur lesende Analyse; nichts wurde verändert."},
    {S::NoDisks, L"No devices found.", L"Keine Geräte gefunden."},
    {S::ReadOnlyBuild, L"Read-only alpha", L"Nur-Lese-Alpha"},
    {S::PlannedFeature, L"Planned - not available in this read-only alpha (see roadmap).",
                        L"Geplant - in dieser Nur-Lese-Alpha nicht verfügbar (siehe Roadmap)."},
    {S::NotElevatedBanner, L"WParted is running without administrator rights. Partition tables cannot be read; only volumes mounted by Windows are shown.",
                           L"WParted läuft ohne Administratorrechte. Partitionstabellen können nicht gelesen werden; es werden nur von Windows eingebundene Volumes angezeigt."},
    {S::NotElevatedStatus, L"Not elevated", L"Ohne Adminrechte"},
    {S::ElevatedStatus, L"Administrator (read-only)", L"Administrator (nur lesend)"},
    {S::PendingNone, L"0 operations pending. Modifying operations are not available in this read-only alpha.",
                     L"0 Operationen ausstehend. Ändernde Operationen sind in dieser Nur-Lese-Alpha nicht verfügbar."},
    {S::StatusWindows, L"Windows", L"Windows"},
    {S::StatusSystemCritical, L"System (protected)", L"System (geschützt)"},
    {S::StatusMounted, L"Mounted", L"Eingebunden"},
    {S::StatusEncrypted, L"Encrypted (BitLocker)", L"Verschlüsselt (BitLocker)"},
    {S::StatusNotReadable, L"Table not read", L"Tabelle nicht gelesen"},
    {S::InfoTitle, L"Partition Information", L"Partitionsinformationen"},
    {S::DeviceInfoTitle, L"Device Information", L"Geräteinformationen"},
    {S::AboutTitle, L"About WParted", L"Über WParted"},
    {S::AboutText, L"WParted - Windows Partition Editor\n\nLicensed under the GNU General Public License v2.0 or later.\n\nWParted is an independent project. It uses GParted as a functional reference but is not affiliated with or endorsed by the GParted or GNOME projects.",
                   L"WParted - Windows Partition Editor\n\nLizenziert unter der GNU General Public License v2.0 oder später.\n\nWParted ist ein unabhängiges Projekt. GParted dient als funktionale Referenz; es besteht keine Verbindung zu den Projekten GParted oder GNOME."},
    {S::FsSupportTitle, L"File System Support (this build)", L"Dateisystemunterstützung (dieser Build)"},
    {S::ColFsDetect, L"Detect", L"Erkennen"},
    {S::ColFsUsage, L"Usage", L"Belegung"},
    {S::ColFsCreate, L"Create", L"Erstellen"},
    {S::ColFsGrow, L"Grow", L"Vergrößern"},
    {S::ColFsShrink, L"Shrink", L"Verkleinern"},
    {S::ColFsMove, L"Move", L"Verschieben"},
    {S::ColFsCopy, L"Copy", L"Kopieren"},
    {S::ColFsCheck, L"Check", L"Prüfen"},
    {S::ColFsLabel, L"Label", L"Bezeichnung"},
    {S::ColFsUuid, L"UUID", L"UUID"},
    {S::ExportOk, L"Report saved.", L"Bericht gespeichert."},
    {S::ExportFailed, L"The report could not be saved.", L"Der Bericht konnte nicht gespeichert werden."},
};
// clang-format on

static_assert(sizeof(kStrings) / sizeof(kStrings[0]) == static_cast<int>(S::Count_), "missing UI string");

// Indexed by S; filled once so table order does not matter.
const wchar_t* g_lookup[static_cast<int>(S::Count_)] = {};

}  // namespace

void initLanguage() {
    LANGID lang = GetUserDefaultUILanguage();
    const bool german = PRIMARYLANGID(lang) == LANG_GERMAN;
    for (const auto& e : kStrings) g_lookup[static_cast<int>(e.id)] = german ? e.de : e.en;
}

const wchar_t* tr(S id) {
    const wchar_t* s = g_lookup[static_cast<int>(id)];
    return s ? s : L"?";
}

}  // namespace wparted::ui
