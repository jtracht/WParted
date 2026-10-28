// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// GParted-style operation planning in the main window: dialogs, pending
// operation list, future-layout preview and the Apply safety summary.
// SAFETY: nothing in this file writes to a disk. Operations only go into an
// OperationQueue (pure simulation); "Apply" shows the validation result and
// offers to export the plan.
#include <windows.h>

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <cwchar>
#include <initializer_list>
#include <memory>
#include <vector>

#include "commands.hpp"
#include "form_dialog.hpp"
#include "main_window.hpp"
#include "strings.hpp"
#include "wparted/core/block_reader.hpp"
#include "wparted/core/executor.hpp"
#include "wparted/core/fs_tools.hpp"
#include "wparted/core/log.hpp"
#include "wparted/core/operation_log.hpp"

namespace wparted::ui {

// Replaces %1..%9 in a translated pattern.
std::wstring formatText(S pattern, std::initializer_list<std::wstring> args) {
    std::wstring out;
    const std::vector<std::wstring> a(args);
    for (const wchar_t* p = tr(pattern); *p; ++p) {
        if (p[0] == L'%' && p[1] >= L'1' && p[1] <= L'9' && static_cast<std::size_t>(p[1] - L'1') < a.size()) {
            out += a[static_cast<std::size_t>(p[1] - L'1')];
            ++p;
        } else {
            out += *p;
        }
    }
    return out;
}

const wchar_t* supportText(Support s) {
    switch (s) {
        case Support::Supported: return tr(S::SupSupported);
        case Support::Limited: return tr(S::SupLimited);
        case Support::OfflineOnly: return tr(S::SupOffline);
        case Support::Experimental: return tr(S::SupExperimental);
        case Support::ReadOnly: return tr(S::SupReadOnly);
        case Support::Unsupported: return tr(S::SupUnsupported);
        case Support::Planned: return tr(S::SupPlanned);
    }
    return tr(S::ValUnknown);
}

const wchar_t* tristateText(win::Tristate t) {
    return tr(t == win::Tristate::Yes ? S::ValYes : t == win::Tristate::No ? S::ValNo : S::ValUnknown);
}

namespace {

// Result of applying a plan to an image file, posted to the window as LPARAM.
struct ImageApplyResult {
    ExecuteResult result;
    std::wstring path;
    std::string name;
};

bool isImageDisk(const win::PhysicalDisk* d) { return d && d->busType == "Image file"; }

// Images have no Windows volumes; for file systems whose usage WParted can
// read itself (FAT) a volume entry carries used/free space, so the list shows
// it and validation refuses shrinking below the data.
void addImageVolumes(win::PhysicalDisk& d, BlockReader& r) {
    d.volumes.clear();
    for (const auto& p : d.layout.partitions) {
        if (p.kind == PartitionKind::Extended || p.fs.type == FsType::Unknown) continue;
        SubRangeReader part(r, p.firstLba * d.layout.sectorSize, p.sectorCount * d.layout.sectorSize);
        fstools::CheckReport rep;
        std::string err;
        if (fstools::canCheck(p.fs.type)) {
            if (!fstools::check(part, p.fs.type, rep, err) || !rep.totalBytes) continue;
        } else {  // e.g. NTFS: used clusters from $Bitmap, size of the partition
            rep.usedBytes = fstools::usedBytes(part, p.fs.type);
            if (!rep.usedBytes) continue;
            rep.totalBytes = p.sectorCount * d.layout.sectorSize;
        }
        win::VolumeInfo v;
        v.diskNumber = d.number;
        v.startingOffset = p.firstLba * d.layout.sectorSize;
        v.extentLength = p.sectorCount * d.layout.sectorSize;
        v.extentCount = 1;
        v.fileSystem = win::widen(fsTypeName(p.fs.type));
        v.label = win::widen(p.fs.label);
        v.hasUsage = true;
        v.totalBytes = rep.totalBytes;
        v.freeBytes = rep.totalBytes - std::min(rep.usedBytes, rep.totalBytes);
        d.volumes.push_back(v);
    }
}

// Result of the background rescue scan, posted to the window as LPARAM.
struct RescueResult {
    std::vector<RescueCandidate> found;
    std::string error;
    bool ok = false;
    std::uint32_t ss = 512;
};

constexpr std::uint64_t kMiB = 1024 * 1024;

std::wstring w(const std::string& s) { return win::widen(s); }

std::wstring fsChoiceName(FsType fs) { return fs == FsType::Unknown ? std::wstring(tr(S::FsUnformatted)) : w(fsTypeName(fs)); }

const wchar_t* riskText(Risk r) {
    switch (r) {
        case Risk::Low: return tr(S::RiskLow);
        case Risk::Medium: return tr(S::RiskMedium);
        case Risk::High: return tr(S::RiskHigh);
        case Risk::Critical: return tr(S::RiskCritical);
    }
    return L"";
}

// Localised counterpart of core describe() (which stays English for logs,
// CLI and plan files).
std::wstring describeOp(const Operation& op, const DiskLayout& before) {
    const std::uint32_t ss = before.sectorSize;
    const auto size = [ss](std::uint64_t sectors) { return w(formatSize(sectors * ss)); };
    std::wstring target = tr(S::OpPartUnknown);
    if (const PartitionEntry* t = findPlanned(before, op.target))
        target = t->kind == PartitionKind::Extended
                     ? formatText(S::OpExtRef, {std::to_wstring(t->number), size(t->sectorCount)})
                     : formatText(S::OpPartRef, {std::to_wstring(t->number), fsChoiceName(t->fs.type), size(t->sectorCount)});
    switch (op.type) {
        case OpType::CreateTable: return formatText(S::OpCreateTable, {w(tableTypeName(op.table))});
        case OpType::CreatePartition: {
            const S kind = op.kind == PartitionKind::Logical    ? S::KindLogicalAdj
                           : op.kind == PartitionKind::Extended ? S::KindExtendedAdj
                                                                : S::KindPrimaryAdj;
            return formatText(S::OpCreate, {tr(kind), op.kind == PartitionKind::Extended ? tr(S::KindExtendedAdj)
                                                                                           : fsChoiceName(op.fs),
                                            size(op.sectorCount), std::to_wstring(op.firstLba)});
        }
        case OpType::DeletePartition: return formatText(S::OpDelete, {target});
        case OpType::ResizeMove:
            return formatText(S::OpResize, {target, std::to_wstring(op.firstLba),
                                            std::to_wstring(op.firstLba + op.sectorCount - 1), size(op.sectorCount)});
        case OpType::Copy: return formatText(S::OpCopy, {target, std::to_wstring(op.firstLba), size(op.sectorCount)});
        case OpType::Format: return formatText(S::OpFormat, {target, fsChoiceName(op.fs)});
        case OpType::Check: return formatText(S::OpCheck, {target});
        case OpType::SetLabel: return formatText(S::OpLabel, {target, w(op.label)});
        case OpType::SetName: return formatText(S::OpName, {target, w(op.name)});
        case OpType::SetFlags: {
            std::wstring flags;
            for (const auto& f : op.flagsOn) flags += L" +" + w(f);
            for (const auto& f : op.flagsOff) flags += L" -" + w(f);
            return formatText(S::OpFlags, {target, flags.empty() ? flags : flags.substr(1)});
        }
        case OpType::NewUuid: return formatText(S::OpUuid, {target});
    }
    return w(describe(op, before));
}

// Translates a validation diagnostic by its code; unknown codes or argument
// counts fall back to the English message from the core.
std::wstring diagText(const Diagnostic& d) {
    struct Entry { const char* code; S text; std::size_t args; };
    static const Entry plain[] = {
        {"plan.source_errors", S::DiagSourceErrors, 0}, {"device.write_protected", S::DiagWriteProtected, 0},
        {"device.managed_by_windows", S::DiagManaged, 0}, {"plan.identity_mismatch", S::DiagIdentityMismatch, 1},
        {"plan.identity_unknown", S::DiagIdentityUnknown, 0}, {"execution.unavailable", S::DiagExecUnavailable, 0},
        {"op.shrink_below_used", S::DiagShrinkBelowUsed, 1}, {"op.busy", S::DiagBusy, 1},
        {"op.encrypted", S::DiagEncrypted, 1}, {"op.encrypted_format", S::DiagEncryptedFormat, 1},
        {"layout.out_of_bounds", S::DiagOutOfBounds, 1}, {"mbr.limit_2tib", S::DiagMbr2TiB, 1},
        {"op.too_small", S::DiagTooSmall, 3}, {"layout.unaligned", S::DiagUnaligned, 2},
        {"layout.physical_unaligned", S::DiagPhysUnaligned, 1}, {"layout.too_many_primary", S::DiagTooManyPrimary, 0},
        {"layout.multiple_extended", S::DiagMultipleExtended, 0}, {"layout.logical_without_extended", S::DiagLogicalNoExt, 0},
        {"mbr.multiple_bootable", S::DiagMultipleBootable, 0}, {"layout.logical_outside_extended", S::DiagLogicalOutside, 1},
        {"layout.no_room_for_ebr", S::DiagNoRoomEbr, 1}, {"op.bitlocker_unknown", S::DiagBitLockerUnknown, 1},
        {"op.bitlocker_recovery", S::DiagBitLockerRecovery, 1},
        // partition-table parser
        {"gpt.no_valid_header", S::PdNoValidHeader, 2},
        {"gpt.primary_invalid", S::PdPrimaryInvalid, 1},
        {"gpt.backup_invalid", S::PdBackupInvalid, 1},
        {"gpt.primary_backup_mismatch", S::PdMismatch, 0},
        {"gpt.backup_location", S::PdBackupLocation, 0},
        {"gpt.entry_reversed", S::PdEntryReversed, 1},
        {"gpt.entry_outside_usable", S::PdEntryOutside, 1},
        {"mbr.ebr_loop", S::PdEbrLoop, 0},
        {"mbr.too_many_logical", S::PdTooManyLogical, 0},
        {"mbr.ebr_unreadable", S::PdEbrUnreadable, 0},
        {"mbr.ebr_signature", S::PdEbrSignature, 0},
        {"mbr.logical_outside_extended", S::PdLogicalOutside, 1},
        {"mbr.ebr_link_type", S::PdEbrLinkType, 0},
        {"mbr.multiple_extended", S::PdMultipleExtended, 0},
        {"layout.beyond_device", S::PdBeyondDevice, 1},
        {"layout.overlap", S::PdOverlap, 2},
        {"device.too_small", S::PdTooSmall, 0},
        {"device.read_failed", S::PdReadFailed, 0},
        {"gpt.sector_size_mismatch", S::PdSectorSize, 2},
        {"layout.whole_device_fs", S::PdWholeDevice, 1},
        {"table.foreign", S::PdForeign, 1},
        {"mbr.invalid_entries", S::PdInvalidEntries, 0},
    };
    std::vector<std::wstring> a;
    for (const auto& s : d.args) a.push_back(w(s));
    auto fmt = [&](S text) {
        std::initializer_list<std::wstring> none{};
        switch (a.size()) {
            case 0: return formatText(text, none);
            case 1: return formatText(text, {a[0]});
            case 2: return formatText(text, {a[0], a[1]});
            default: return formatText(text, {a[0], a[1], a[2]});
        }
    };
    std::wstring body;
    for (const auto& e : plain)
        if (d.code == e.code && d.args.size() == e.args) body = fmt(e.text);
    if (d.code == "op.system_critical" && a.size() == 2 && a[0] == L"windows") body = formatText(S::DiagSystemWindows, {a[1]});
    if (d.code == "op.bcd_offset" && a.size() == 1)
        body = formatText(d.severity == Severity::Error ? S::DiagBcdOffset : S::DiagBcdOffsetImage, {a[0]});
    if (d.code == "op.bitlocker_active" && a.size() == 2) {
        static const std::pair<const char*, S> states[] = {{"protection on", S::BlOn}, {"locked", S::BlLocked},
            {"encryption or decryption in progress", S::BlConverting}};
        std::wstring state = a[1];
        for (const auto& x : states)
            if (d.args[1] == x.first) state = tr(x.second);
        body = formatText(S::DiagBitLockerActive, {a[0], state});
    }
    if (d.code == "op.system_critical" && a.size() == 3 && a[0] == L"type") body = formatText(S::DiagSystemType, {a[1], a[2]});
    if (d.code == "layout.overlap" && a.size() == 3)
        body = formatText(a[0] == L"logical" ? S::DiagOverlapLogical : S::DiagOverlapPrimary, {a[1], a[2]});
    if (d.code == "op.capability" && a.size() == 3) {
        static const std::pair<const wchar_t*, S> actions[] = {{L"create", S::ActCreate}, {L"grow", S::ActGrow},
            {L"shrink", S::ActShrink}, {L"move", S::ActMove}, {L"copy", S::ActCopy}, {L"check", S::ActCheck},
            {L"label", S::ActLabel}, {L"uuid", S::ActUuid}};
        static const std::pair<const char*, S> levels[] = {{"Supported", S::SupSupported},
            {"Supported with limitations", S::SupLimited}, {"Requires offline mode", S::SupOffline},
            {"Experimental", S::SupExperimental}, {"Read-only", S::SupReadOnly}, {"Unsupported", S::SupUnsupported},
            {"Planned", S::SupPlanned}};
        const wchar_t* act = nullptr;
        const wchar_t* level = nullptr;
        for (const auto& x : actions)
            if (a[0] == x.first) act = tr(x.second);
        for (const auto& x : levels)
            if (d.args[2] == x.first) level = tr(x.second);
        if (act && level) body = formatText(S::DiagCapability, {act, a[1], level});
    }
    if (body.empty()) return w(d.message);
    return d.operation > 0 ? formatText(S::DiagOperation, {std::to_wstring(d.operation), body}) : body;
}

// One line per operation, each described against the layout before it
// (same walk as OperationQueue::descriptions()).
std::vector<std::wstring> describeQueue(const OperationQueue& q) {
    std::vector<std::wstring> out;
    DiskLayout l = q.original();
    std::uint32_t next = static_cast<std::uint32_t>(l.partitions.size()) + 1;
    std::string err;
    for (Operation op : q.operations()) {
        out.push_back(describeOp(op, l));
        simulate(op, l, next, err);
    }
    return out;
}

std::uint64_t ceilDiv(std::uint64_t a, std::uint64_t b) { return (a + b - 1) / b; }

// Shared geometry of the New / Paste / Resize dialogs (GParted layout:
// free space preceding, new size, free space following, alignment).
struct Placement {
    std::uint64_t lo = 0, hi = 0;     // usable sector range (inclusive)
    std::uint64_t spm = 2048;         // sectors per MiB
    std::uint64_t minFirst = 0;       // e.g. lo + 1 for logical partitions (room for the EBR)
    bool compute(const std::vector<FormField>& f, std::uint64_t& first, std::uint64_t& count) const {
        first = lo + f[0].number * spm;
        if (first < minFirst) first = minFirst;
        if (f[3].selected == 0) first = ceilDiv(first, spm) * spm;  // MiB alignment
        count = f[1].number * spm;
        return count > 0 && first >= lo && first + count - 1 <= hi;
    }
    std::uint64_t maxMiB() const { return (hi - lo + 1) / spm; }
};

std::vector<FormField> placementFields(const Placement& pl, std::uint64_t beforeMiB, std::uint64_t sizeMiB,
                                       std::uint64_t minSizeMiB) {
    std::vector<FormField> f(4);
    f[0].kind = FormField::Kind::Number;
    f[0].label = tr(S::FieldFreeBefore);
    f[0].number = beforeMiB;
    f[0].maximum = pl.maxMiB();
    f[1].kind = FormField::Kind::Number;
    f[1].label = tr(S::FieldNewSize);
    f[1].number = sizeMiB;
    f[1].minimum = std::max<std::uint64_t>(1, minSizeMiB);
    f[1].maximum = std::max<std::uint64_t>(1, pl.maxMiB());
    f[2].kind = FormField::Kind::Note;
    f[2].label = tr(S::FieldFreeAfter);
    f[3].kind = FormField::Kind::Choice;
    f[3].label = tr(S::FieldAlign);
    f[3].choices = {tr(S::AlignMiB), tr(S::AlignNone)};
    return f;
}

FormField previewField(COLORREF color) {
    FormField pv;
    pv.kind = FormField::Kind::Preview;
    pv.previewColor = color;
    return pv;
}

FormUpdate placementUpdate(const Placement& pl) {
    return [pl](std::vector<FormField>& f, int) -> std::wstring {
        std::uint64_t first = 0, count = 0;
        const bool fits = pl.compute(f, first, count);
        const std::uint64_t end = first + count - 1;
        f[2].text = fits ? std::to_wstring((pl.hi - end) / pl.spm) : L"-";
        for (auto& field : f) {
            if (field.kind != FormField::Kind::Preview) continue;
            field.previewTotal = pl.hi - pl.lo + 1;
            field.previewStart = fits ? first - pl.lo : 0;
            field.previewSize = fits ? count : 0;
            field.previewLabel = std::to_wstring(f[1].number) + L" MiB";
        }
        return fits ? std::wstring() : std::wstring(tr(S::ErrTooLarge));
    };
}

}  // namespace

std::wstring diagnosticText(const Diagnostic& d) { return diagText(d); }

// ---------------------------------------------------------------------------
// State

const win::PhysicalDisk* MainWindow::currentDisk() const {
    if (!result_ || diskIndex_ < 0 || diskIndex_ >= static_cast<int>(result_->disks.size())) return nullptr;
    return &result_->disks[static_cast<std::size_t>(diskIndex_)];
}

OperationQueue* MainWindow::currentQueue() {
    const win::PhysicalDisk* d = currentDisk();
    if (!d || !d->layoutAvailable) return nullptr;  // never plan on a volumes-only view
    auto it = queues_.find(d->path);
    if (it == queues_.end())
        it = queues_.emplace(d->path, OperationQueue(d->layout, identityOf(d->layout, d->serial, d->modelName()))).first;
    return &it->second;
}

const Row* MainWindow::selectedRowData() const {
    if (selectedRow_ < 0 || selectedRow_ >= static_cast<int>(view_.rows.size())) return nullptr;
    return &view_.rows[static_cast<std::size_t>(selectedRow_)];
}

const PartitionEntry* MainWindow::selectedPartition() const {
    const Row* r = selectedRowData();
    if (!r || r->kind != Segment::Kind::Partition || !view_.layout) return nullptr;
    return &view_.layout->partitions[static_cast<std::size_t>(r->partitionIndex)];
}

ValidationContext MainWindow::validationContext(ValidationMode mode) const {
    ValidationContext ctx;
    ctx.mode = mode;
    const win::PhysicalDisk* d = currentDisk();
    auto it = d ? queues_.find(d->path) : queues_.end();
    if (!d || it == queues_.end()) return ctx;
    const OperationQueue& q = it->second;
    ctx.physicalSectorSize = d->physicalSectorSize;
    ctx.deviceWriteProtected = d->writeProtected == win::Tristate::Yes;
    for (const auto& v : d->volumes)
        if (v.extentCount > 1) ctx.managedByWindows = true;
    if (isManagedByWindows(q.original())) ctx.managedByWindows = true;
    for (const auto& p : q.original().partitions) {
        PartitionState st;
        if (const auto* vol = win::volumeForPartition(*d, p)) {
            st.mounted = !vol->mountPoints.empty();
            st.containsWindows = vol->containsWindows;
            st.pageFile = vol->isPageFileVolume;
            st.bootManager = vol->isSystemPartition;
            st.bitLocker = vol->bitLocker;
            if (vol->hasUsage) st.usedBytes = vol->totalBytes - vol->freeBytes;
        }
        ctx.state[p.planId] = st;
    }
    if (mode != ValidationMode::Planning) {
        ctx.haveCurrentIdentity = true;
        ctx.currentIdentity = identityOf(d->layout, d->serial, d->modelName());
    }
    return ctx;
}

void MainWindow::refreshView() {
    const win::PhysicalDisk* d = currentDisk();
    view_ = DiskView{};
    OperationQueue* q = currentQueue();
    if (d) {
        view_.disk = d;
        if (q) {
            view_.layout = &q->current();
            view_.original = &q->original();
            view_.tableRead = true;
        } else {
            view_.layout = &fallbackLayouts_[static_cast<std::size_t>(diskIndex_)];
        }
        view_.rows = flattenSegments(*view_.layout, buildSegments(*view_.layout));
    }
    SendMessageW(map_, DM_SETVIEW, reinterpret_cast<WPARAM>(&palette(isDark())),
                 reinterpret_cast<LPARAM>(view_.layout ? &view_ : nullptr));
    fillPartitionList();

    SendMessageW(pending_, LB_RESETCONTENT, 0, 0);
    int errors = 0, warnings = 0;
    if (q && !q->empty()) {
        const auto descriptions = describeQueue(*q);
        for (std::size_t i = 0; i < descriptions.size(); ++i) {
            std::wstring line = std::to_wstring(i + 1) + L".  " + descriptions[i] + L"   ·  " +
                                formatText(S::RiskLabel, {riskText(riskOf(q->operations()[i]))});
            const LRESULT idx = SendMessageW(pending_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
            SendMessageW(pending_, LB_SETITEMDATA, static_cast<WPARAM>(idx), static_cast<LPARAM>(riskOf(q->operations()[i])) + 1);
        }
        const ValidationResult vr = validatePlan(*q, validationContext(isImageDisk(currentDisk()) ? ValidationMode::ImageExecution : ValidationMode::Planning));
        for (const auto& diag : vr.diagnostics) {
            (diag.severity == Severity::Error ? errors : warnings)++;
            const std::wstring line = diagText(diag);
            const LRESULT idx = SendMessageW(pending_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
            SendMessageW(pending_, LB_SETITEMDATA, static_cast<WPARAM>(idx), diag.severity == Severity::Error ? 11 : 10);
        }
    } else {
        SendMessageW(pending_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(d && !q ? S::ErrNeedsTable : S::PendingNone)));
    }
    updateStatus();
    if (q && !q->empty()) {
        wchar_t buf[256];
        swprintf_s(buf, sizeof buf / sizeof buf[0], tr(S::PendingFmt), static_cast<int>(q->operations().size()), errors,
                   warnings);
        SetWindowTextW(status_, buf);
    }
    InvalidateRect(hwnd_, &pendingCard_, TRUE);  // section title shows the count
    if (selectedRow_ >= static_cast<int>(view_.rows.size())) selectedRow_ = -1;
    selectRow(selectedRow_, true);
}

void MainWindow::updateInspector() {
    InspectorData& d = inspectorData_;
    d = InspectorData{};
    const Palette& pal = palette(isDark());
    const win::PhysicalDisk* disk = currentDisk();
    const Row* row = selectedRowData();
    const DiskLayout* l = view_.layout;
    auto add = [&d](S label, const std::wstring& value) {
        if (!value.empty()) d.rows.emplace_back(tr(label), value);
    };
    auto range = [&](std::uint64_t first, std::uint64_t count) {
        return std::to_wstring(first) + L" – " + std::to_wstring(first + count - 1) + L"  (" + std::to_wstring(count) + L")";
    };
    if (!disk || !l) {
        d.title = tr(S::AppTitle);
        d.hint = tr(S::HintSelect);
    } else if (!row) {
        d.title = w(disk->modelName());
        add(S::InsDevice, disk->busType == "Image file" ? disk->path : disk->path + L"  ·  " + w(disk->busType));
        add(S::InsSize, w(formatSize(disk->sizeBytes)));
        add(S::InsTable, view_.tableRead ? w(tableTypeName(l->type)) : std::wstring(tr(S::StatusNotReadable)));
        add(S::InsSectors, std::to_wstring(disk->logicalSectorSize) + L" / " +
                               (disk->physicalSectorSize ? std::to_wstring(disk->physicalSectorSize) : std::wstring(L"?")) + L" B");
        add(S::InsPartitions, std::to_wstring(l->partitions.size()));
        if (!view_.tableRead) {
            d.hint = tr(S::ErrNeedsTable);
            d.warnHint = true;
        } else if (isManagedByWindows(*l)) {
            d.hint = tr(S::HintManaged);
            d.warnHint = true;
        } else {
            d.hint = disk->busType == "Image file" ? std::wstring(tr(S::HintImage)) + L" " + tr(S::HintSelect) : tr(S::HintSelect);
        }
    } else if (row->kind == Segment::Kind::Unallocated) {
        d.title = view_.tableRead ? tr(S::Unallocated) : tr(S::UnknownRegion);
        add(S::InsSize, w(formatSize(row->sectorCount * l->sectorSize)));
        add(S::InsRange, range(row->firstLba, row->sectorCount));
        d.hint = view_.tableRead ? tr(S::HintUnallocated) : tr(S::ErrNeedsTable);
        d.warnHint = !view_.tableRead;
    } else {
        const PartitionEntry& p = l->partitions[static_cast<std::size_t>(row->partitionIndex)];
        const auto* vol = viewVolume(view_, p);
        d.title = p.number > 0 ? L"#" + std::to_wstring(p.number) : std::wstring(tr(S::VolumeOnly));
        if (vol && !vol->mountPoints.empty()) d.title += L"  " + vol->mountPoints.front();
        else if (!p.fs.label.empty()) d.title += L"  " + w(p.fs.label);
        d.pill = p.kind == PartitionKind::Extended ? std::wstring(L"extended") : w(fsTypeName(p.fs.type));
        d.pillColor = p.kind == PartitionKind::Extended ? pal.mutedText : fsColor(p.fs.type);
        add(S::InsType, w(partitionTypeDescription(*l, p)));
        add(S::InsSize, w(formatSize(p.sectorCount * l->sectorSize)));
        if (vol && vol->hasUsage)
            add(S::InsUsage, w(formatSize(vol->totalBytes - vol->freeBytes)) + L" / " + w(formatSize(vol->totalBytes)));
        if (vol) {
            std::wstring mounts;
            for (const auto& m : vol->mountPoints) mounts += (mounts.empty() ? L"" : L", ") + m;
            add(S::ColMountPoint, mounts);
        }
        add(S::ColLabel, vol && !vol->label.empty() ? vol->label : w(p.fs.label));
        add(S::ColName, w(p.name));
        std::wstring flags;
        for (const auto& f : partitionFlags(*l, p)) flags += (flags.empty() ? L"" : L", ") + w(f);
        add(S::ColFlags, flags);
        add(S::InsRange, range(p.firstLba, p.sectorCount));
        const bool aligned = (p.firstLba * l->sectorSize) % (1024 * 1024) == 0;
        add(S::InsAlign, std::wstring(L"1 MiB: ") + (aligned ? tr(S::InsYes) : tr(S::InsNo)));
        add(S::InsUuid, w(p.fs.uuid));
        const bool planned = view_.original && p.planId > view_.original->partitions.size();
        if (planned) {
            d.hint = tr(S::HintPlanned);
        } else if (isSystemCriticalType(*l, p) || (vol && vol->containsWindows)) {
            d.hint = tr(S::HintCritical);
            d.warnHint = true;
        } else {
            d.hint = view_.tableRead ? tr(S::HintPartition) : tr(S::ErrNeedsTable);
            d.warnHint = !view_.tableRead;
        }
    }
    SendMessageW(inspector_, IM_SETDATA, reinterpret_cast<WPARAM>(&pal), reinterpret_cast<LPARAM>(&d));
}

void MainWindow::updateCommandState() {
    OperationQueue* q = currentQueue();
    const Row* row = selectedRowData();
    const PartitionEntry* p = selectedPartition();
    const bool editable = q && (q->current().type == TableType::Gpt || q->current().type == TableType::Mbr);
    const bool unalloc = editable && row && row->kind == Segment::Kind::Unallocated;
    const bool part = editable && p && p->kind != PartitionKind::WholeDevice;
    const bool data = part && p->kind != PartitionKind::Extended;
    const bool hasFs = data && p->fs.type != FsType::Unknown;
    const bool pending = q && !q->empty();
    const win::PhysicalDisk* d = currentDisk();
    const bool canPaste = unalloc && copySource_ != 0 && d && copySourceDisk_ == d->path &&
                          findPlanned(q->current(), copySource_) != nullptr;
    const struct { int id; bool on; } states[] = {
        {IDM_NEW, unalloc}, {IDM_DELETE, part}, {IDM_RESIZE, part}, {IDM_COPY, data}, {IDM_PASTE, canPaste},
        {IDM_FLAGS, part}, {IDM_CHECK, hasFs}, {IDM_LABEL, hasFs}, {IDM_UUID, hasFs},
        {IDM_NAME, part && q->current().type == TableType::Gpt}, {IDM_PART_INFO, p != nullptr},
        {IDM_CREATE_TABLE, q != nullptr}, {IDM_UNDO, pending}, {IDM_CLEAR_OPS, pending}, {IDM_APPLY_OPS, pending},
        {IDM_SAVE_PLAN, pending},
    };
    for (const auto& s : states) {
        EnableMenuItem(menu_, static_cast<UINT>(s.id), MF_BYCOMMAND | (s.on ? MF_ENABLED : MF_GRAYED));
        if (HWND b = GetDlgItem(hwnd_, s.id)) EnableWindow(b, s.on);
    }
    for (int i = 0; i < kFormatChoiceCount; ++i)
        EnableMenuItem(formatMenu_, static_cast<UINT>(IDM_FORMAT_BASE + i), MF_BYCOMMAND | (data ? MF_ENABLED : MF_GRAYED));
}

bool MainWindow::addOperation(Operation op) {
    OperationQueue* q = currentQueue();
    if (!q) return false;
    std::string err;
    if (!q->add(op, err)) {
        MessageBoxW(hwnd_, w(err).c_str(), tr(S::OpRejected), MB_ICONWARNING);
        return false;
    }
    log::info("Planned: " + describe(op, q->original()) + " (not executed)");
    refreshView();
    return true;
}

// ---------------------------------------------------------------------------
// Dialogs

void MainWindow::planNew() {
    OperationQueue* q = currentQueue();
    const Row* r = selectedRowData();
    if (!q || !r || r->kind != Segment::Kind::Unallocated) return;
    const DiskLayout& l = q->current();
    Placement pl;
    pl.spm = std::max<std::uint64_t>(1, kMiB / l.sectorSize);
    pl.lo = r->firstLba;
    pl.hi = r->firstLba + r->sectorCount - 1;
    const bool logicalGap = r->depth == 1;
    pl.minFirst = logicalGap ? pl.lo + 1 : pl.lo;
    std::uint64_t first0 = std::max(pl.lo, pl.minFirst);
    first0 = ceilDiv(first0, pl.spm) * pl.spm;
    const std::uint64_t maxSize = first0 <= pl.hi ? (pl.hi - first0 + 1) / pl.spm : 0;
    if (maxSize == 0) {
        MessageBoxW(hwnd_, tr(S::ErrTooLarge), tr(S::OpRejected), MB_ICONWARNING);
        return;
    }
    auto f = placementFields(pl, 0, maxSize, 1);
    FormField kind;
    kind.kind = FormField::Kind::Choice;
    kind.label = tr(S::FieldCreateAs);
    std::vector<PartitionKind> kinds;
    bool hasExtended = false;
    for (const auto& p : l.partitions)
        if (p.kind == PartitionKind::Extended) hasExtended = true;
    if (logicalGap) kinds = {PartitionKind::Logical};
    else if (l.type == TableType::Mbr && !hasExtended) kinds = {PartitionKind::Primary, PartitionKind::Extended};
    else kinds = {PartitionKind::Primary};
    for (auto k : kinds)
        kind.choices.push_back(k == PartitionKind::Primary ? tr(S::KindPrimary)
                               : k == PartitionKind::Extended ? tr(S::KindExtended) : tr(S::KindLogical));
    kind.enabled = kinds.size() > 1;
    f.push_back(kind);
    FormField name;
    name.kind = FormField::Kind::Text;
    name.label = tr(S::FieldName);
    name.maxLength = 36;
    name.enabled = l.type == TableType::Gpt;
    f.push_back(name);
    FormField fs;
    fs.kind = FormField::Kind::Choice;
    fs.label = tr(S::FieldFs);
    for (FsType t : kFormatChoices) fs.choices.push_back(fsChoiceName(t));
    // On image files, preselect the first file system WParted can create there.
    if (isImageDisk(currentDisk()))
        for (int i = 0; i < kFormatChoiceCount; ++i)
            if (fstools::canCreate(kFormatChoices[i])) {
                fs.selected = i;
                break;
            }
    f.push_back(fs);
    FormField label;
    label.kind = FormField::Kind::Text;
    label.label = tr(S::FieldLabel);
    label.maxLength = 32;
    f.push_back(label);
    f.push_back(previewField(fsColor(FsType::Ntfs)));

    const FormUpdate base = placementUpdate(pl);
    const FormUpdate update = [base, kinds](std::vector<FormField>& v, int changed) {
        const bool extended = kinds[static_cast<std::size_t>(std::max(0, v[4].selected))] == PartitionKind::Extended;
        v[6].enabled = v[7].enabled = !extended;
        return base(v, changed);
    };
    if (!runForm(hwnd_, tr(S::DlgNewTitle), L"", f, palette(isDark()), font_, dpi_, update)) return;
    Operation op;
    op.type = OpType::CreatePartition;
    if (!pl.compute(f, op.firstLba, op.sectorCount)) return;
    op.kind = kinds[static_cast<std::size_t>(std::max(0, f[4].selected))];
    op.name = win::narrow(f[5].text);
    op.fs = op.kind == PartitionKind::Extended ? FsType::Unknown : kFormatChoices[std::max(0, f[6].selected)];
    op.label = win::narrow(f[7].text);
    addOperation(op);
}

void MainWindow::planDelete() {
    if (const PartitionEntry* p = selectedPartition()) {
        Operation op;
        op.type = OpType::DeletePartition;
        op.target = p->planId;
        addOperation(op);
    }
}

void MainWindow::planResize() {
    OperationQueue* q = currentQueue();
    const PartitionEntry* p = selectedPartition();
    if (!q || !p) return;
    const DiskLayout& l = q->current();
    const bool logical = p->kind == PartitionKind::Logical;
    const PartitionEntry* ext = nullptr;
    for (const auto& o : l.partitions)
        if (o.kind == PartitionKind::Extended) ext = &o;
    Placement pl;
    pl.spm = std::max<std::uint64_t>(1, kMiB / l.sectorSize);
    pl.lo = logical && ext ? ext->firstLba + 1 : l.firstUsableLba;
    pl.hi = logical && ext ? ext->lastLba() : l.lastUsableLba;
    for (const auto& o : l.partitions) {
        if (&o == p || (o.kind == PartitionKind::Logical) != logical) continue;
        if (o.lastLba() < p->firstLba) pl.lo = std::max(pl.lo, o.lastLba() + (logical ? 2 : 1));
        if (o.firstLba > p->lastLba()) pl.hi = std::min(pl.hi, o.firstLba - (logical ? 2 : 1));
    }
    pl.minFirst = pl.lo;
    const std::uint64_t before0 = (p->firstLba - pl.lo) / pl.spm;
    const std::uint64_t size0 = std::max<std::uint64_t>(1, p->sectorCount / pl.spm);
    auto f = placementFields(pl, before0, size0, 1);
    f[3].selected = (p->firstLba % pl.spm == 0) ? 0 : 1;  // keep unaligned partitions unaligned unless asked
    f.push_back(previewField(fsColor(p->fs.type)));
    const std::wstring intro = w(partitionKindName(p->kind)) + L" #" + std::to_wstring(p->number) + L"  " +
                               w(fsTypeName(p->fs.type)) + L"  " + w(formatSize(p->sectorCount * l.sectorSize));
    if (!runForm(hwnd_, tr(S::DlgResizeTitle), intro, f, palette(isDark()), font_, dpi_, placementUpdate(pl))) return;
    Operation op;
    op.type = OpType::ResizeMove;
    op.target = p->planId;
    if (f[0].number == before0 && f[1].number == size0 && f[3].selected == ((p->firstLba % pl.spm == 0) ? 0 : 1))
        return;  // unchanged
    if (!pl.compute(f, op.firstLba, op.sectorCount)) return;
    if (f[1].number == size0 && f[0].number != before0) op.sectorCount = p->sectorCount;  // pure move keeps the exact size
    if (op.firstLba == p->firstLba && op.sectorCount == p->sectorCount) return;
    addOperation(op);
}

void MainWindow::planCopy() {
    const PartitionEntry* p = selectedPartition();
    const win::PhysicalDisk* d = currentDisk();
    if (!p || !d || p->kind == PartitionKind::Extended) return;
    copySource_ = p->planId;
    copySourceDisk_ = d->path;
    wchar_t buf[200];
    swprintf_s(buf, sizeof buf / sizeof buf[0], tr(S::CopiedFmt), p->number);
    SetWindowTextW(status_, buf);
    updateCommandState();
}

void MainWindow::planPaste() {
    OperationQueue* q = currentQueue();
    const Row* r = selectedRowData();
    if (!q || !r || r->kind != Segment::Kind::Unallocated) return;
    const PartitionEntry* src = findPlanned(q->current(), copySource_);
    if (!src) return;
    const DiskLayout& l = q->current();
    Placement pl;
    pl.spm = std::max<std::uint64_t>(1, kMiB / l.sectorSize);
    pl.lo = r->firstLba;
    pl.hi = r->firstLba + r->sectorCount - 1;
    pl.minFirst = r->depth == 1 ? pl.lo + 1 : pl.lo;
    const std::uint64_t minMiB = ceilDiv(src->sectorCount, pl.spm);
    auto f = placementFields(pl, 0, minMiB, minMiB);
    f.push_back(previewField(fsColor(src->fs.type)));
    const std::wstring intro = std::wstring(tr(S::FieldSource)) + L": #" + std::to_wstring(src->number) + L"  " +
                               w(fsTypeName(src->fs.type)) + L"  " + w(formatSize(src->sectorCount * l.sectorSize));
    if (!runForm(hwnd_, tr(S::DlgPasteTitle), intro, f, palette(isDark()), font_, dpi_, placementUpdate(pl))) return;
    Operation op;
    op.type = OpType::Copy;
    op.target = src->planId;
    op.kind = r->depth == 1 ? PartitionKind::Logical : PartitionKind::Primary;
    if (!pl.compute(f, op.firstLba, op.sectorCount)) return;
    addOperation(op);
}

void MainWindow::planFormat(FsType fs) {
    const PartitionEntry* p = selectedPartition();
    if (!p || p->kind == PartitionKind::Extended) return;
    Operation op;
    op.type = OpType::Format;
    op.target = p->planId;
    op.fs = fs;
    addOperation(op);
}

void MainWindow::planLabel() {
    const PartitionEntry* p = selectedPartition();
    if (!p) return;
    std::vector<FormField> f(1);
    f[0].kind = FormField::Kind::Text;
    f[0].label = tr(S::FieldLabel);
    f[0].text = w(p->fs.label);
    f[0].maxLength = 32;
    if (!runForm(hwnd_, tr(S::DlgLabelTitle), L"", f, palette(isDark()), font_, dpi_)) return;
    Operation op;
    op.type = OpType::SetLabel;
    op.target = p->planId;
    op.label = win::narrow(f[0].text);
    addOperation(op);
}

void MainWindow::planName() {
    const PartitionEntry* p = selectedPartition();
    if (!p) return;
    std::vector<FormField> f(1);
    f[0].kind = FormField::Kind::Text;
    f[0].label = tr(S::FieldName);
    f[0].text = w(p->name);
    f[0].maxLength = 36;
    if (!runForm(hwnd_, tr(S::DlgNameTitle), L"", f, palette(isDark()), font_, dpi_)) return;
    Operation op;
    op.type = OpType::SetName;
    op.target = p->planId;
    op.name = win::narrow(f[0].text);
    addOperation(op);
}

void MainWindow::planFlags() {
    OperationQueue* q = currentQueue();
    const PartitionEntry* p = selectedPartition();
    if (!q || !p) return;
    const auto flags = settableFlags(q->current().type);
    const auto current = partitionFlags(q->current(), *p);
    std::vector<FormField> f;
    for (const auto& name : flags) {
        FormField c;
        c.kind = FormField::Kind::Check;
        c.label = w(name);
        c.checked = std::find(current.begin(), current.end(), name) != current.end();
        f.push_back(c);
    }
    const std::wstring intro = L"#" + std::to_wstring(p->number) + L"  " + w(partitionTypeDescription(q->current(), *p));
    if (!runForm(hwnd_, tr(S::DlgFlagsTitle), intro, f, palette(isDark()), font_, dpi_)) return;
    Operation op;
    op.type = OpType::SetFlags;
    op.target = p->planId;
    for (std::size_t i = 0; i < flags.size(); ++i) {
        const bool was = std::find(current.begin(), current.end(), flags[i]) != current.end();
        if (f[i].checked && !was) op.flagsOn.push_back(flags[i]);
        if (!f[i].checked && was) op.flagsOff.push_back(flags[i]);
    }
    if (!op.flagsOn.empty() || !op.flagsOff.empty()) addOperation(op);
}

void MainWindow::planSimple(OpType type) {
    if (const PartitionEntry* p = selectedPartition()) {
        Operation op;
        op.type = type;
        op.target = p->planId;
        addOperation(op);
    }
}

void MainWindow::planCreateTable() {
    if (!currentQueue()) return;
    std::vector<FormField> f(1);
    f[0].kind = FormField::Kind::Choice;
    f[0].label = tr(S::FieldTableType);
    f[0].choices = {L"gpt", L"msdos"};
    if (!runForm(hwnd_, tr(S::DlgTableTitle), tr(S::TableWarning), f, palette(isDark()), font_, dpi_)) return;
    Operation op;
    op.type = OpType::CreateTable;
    op.table = f[0].selected == 1 ? TableType::Mbr : TableType::Gpt;
    addOperation(op);
}

void MainWindow::showApplySummary() {
    OperationQueue* q = currentQueue();
    if (!q || q->empty()) return;
    const win::PhysicalDisk* d = currentDisk();
    const bool image = isImageDisk(d);
    const ValidationResult vr =
        validatePlan(*q, validationContext(image ? ValidationMode::ImageExecution : ValidationMode::Execution));
    std::wstring text;
    const auto descriptions = describeQueue(*q);
    for (std::size_t i = 0; i < descriptions.size(); ++i)
        text += std::to_wstring(i + 1) + L". [" + formatText(S::RiskLabel, {riskText(riskOf(q->operations()[i]))}) + L"] " +
                descriptions[i] + L"\n";
    text += L"\n";
    for (const auto& diag : vr.diagnostics)
        text += std::wstring(diag.severity == Severity::Error ? L"✖ " : L"⚠ ") + diagText(diag) + L"\n";
    std::wstring head = std::wstring(tr(S::ApplyTitle)) + L": " + (d ? w(d->modelName()) : L"") +
                       (d && !d->serial.empty() ? L" (" + w(d->serial) + L")" : L"");
    const bool canApply = image && vr.ok();
    TASKDIALOG_BUTTON buttons[] = {{102, tr(S::BtnApplyImage)}, {100, tr(S::BtnExportPlan)}, {101, tr(S::BtnSaveDetails)}};
    TASKDIALOGCONFIG cfg{sizeof cfg};
    cfg.hwndParent = hwnd_;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    cfg.pszWindowTitle = tr(S::ApplyTitle);
    cfg.pszMainInstruction = head.c_str();
    cfg.pszContent = !image ? tr(S::ApplyNoExec) : canApply ? tr(S::ApplyImageReady) : tr(S::ApplyImageBlocked);
    cfg.pszExpandedInformation = text.c_str();
    cfg.dwFlags |= TDF_EXPANDED_BY_DEFAULT;
    cfg.pButtons = canApply ? buttons : buttons + 1;
    cfg.cButtons = canApply ? 3 : 2;
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    cfg.nDefaultButton = IDCLOSE;  // writing is never the default
    cfg.pszMainIcon = TD_WARNING_ICON;
    int pressed = 0;
    TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    if (pressed == 102) {
        // Second, explicit confirmation naming the file.
        const std::wstring title = formatText(S::ConfirmImageTitle, {std::to_wstring(q->operations().size())});
        const std::wstring body = formatText(S::ConfirmImageText, {d->path});
        TASKDIALOG_BUTTON yes[] = {{103, tr(S::BtnApplyNow)}};
        TASKDIALOGCONFIG c{sizeof c};
        c.hwndParent = hwnd_;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        c.pszWindowTitle = tr(S::ApplyResultTitle);
        c.pszMainInstruction = title.c_str();
        c.pszContent = body.c_str();
        c.pButtons = yes;
        c.cButtons = 1;
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.nDefaultButton = IDCANCEL;
        c.pszMainIcon = TD_WARNING_ICON;
        int confirmed = 0;
        TaskDialogIndirect(&c, &confirmed, nullptr, nullptr);
        if (confirmed == 103) startImageApply();
        return;
    }
    if (pressed == 100) savePlan();
    if (pressed == 101) {
        const std::wstring path = askSavePath(L"wparted-plan-details.html", L"HTML (*.html)\0*.html\0", L"html");
        if (!path.empty()) {
            const std::string name = d ? d->modelName() : std::string("disk");
            const bool ok = saveTextFile(path, logToHtml(planLog(*q, vr, name), "WParted plan details - " + name));
            MessageBoxW(hwnd_, ok ? tr(S::ExportOk) : tr(S::ExportFailed), tr(S::AppTitle), ok ? MB_ICONINFORMATION : MB_ICONERROR);
        }
    }
}

void MainWindow::startImageApply(bool resume) {
    const win::PhysicalDisk* d = currentDisk();
    OperationQueue* q = currentQueue();
    if (!isImageDisk(d) || applying_) return;
    JournalState journal;
    std::string journalErr;
    if (resume ? !readJournal(journalPathFor(win::narrow(d->path)), journal, journalErr) : !q || q->empty()) return;
    if (applyWorker_.joinable()) applyWorker_.join();
    applying_ = true;
    applyTotal_ = resume ? journal.operationCount : q->operations().size();
    onImageApplyProgress(0, 0);
    const OperationQueue plan = resume ? OperationQueue() : *q;  // the worker gets its own copies
    const std::wstring path = d->path;
    const std::string name = d->modelName();
    const std::uint32_t ss = d->logicalSectorSize;
    HWND target = hwnd_;
    log::info("Applying " + std::to_string(applyTotal_) + " operation(s) to image file " + win::narrow(path));
    applyWorker_ = std::thread([plan, path, name, ss, target, resume] {
        auto* r = new ImageApplyResult;
        r->path = path;
        r->name = name;
        ImageFileDevice dev;
        std::string err;
        if (!dev.open(win::narrow(path), ss, err)) {
            r->result.error = err;
        } else {
            ExecuteOptions opt;
            opt.journalPath = journalPathFor(win::narrow(path));  // interrupted runs can be resumed
            int lastPct = -1;
            std::size_t lastOp = 0;
            opt.progress = [&](std::size_t op, std::uint64_t done, std::uint64_t total) {
                const int pct = total ? static_cast<int>(done * 100 / total) : 100;
                if (pct == lastPct && op == lastOp) return;
                lastPct = pct;
                lastOp = op;
                PostMessageW(target, WM_APP + 52, static_cast<WPARAM>(op), static_cast<LPARAM>(pct));
            };
            r->result = resume ? resumePlanOnImage(dev, name, opt) : executePlanOnImage(dev, plan, name, opt);
        }
        if (!PostMessageW(target, WM_APP + 53, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

void MainWindow::onImageApplyProgress(WPARAM op, LPARAM percent) {
    const std::wstring text = formatText(S::ApplyRunning, {std::to_wstring(static_cast<std::size_t>(op) + 1),
                                                           std::to_wstring(applyTotal_), std::to_wstring(percent)});
    SetWindowTextW(status_, text.c_str());
}

void MainWindow::onImageApplyDone(LPARAM lp) {
    std::unique_ptr<ImageApplyResult> r(reinterpret_cast<ImageApplyResult*>(lp));
    applying_ = false;
    if (applyWorker_.joinable()) applyWorker_.join();
    const ExecuteResult& res = r->result;
    log::info(res.ok ? "Image apply finished: " + std::to_string(res.completed) + " operation(s) verified"
                     : "Image apply failed: " + res.error);
    std::wstring steps;
    for (std::size_t i = 0; i < res.log.children.size(); ++i) {
        const LogStep& s = res.log.children[i];
        const wchar_t* mark = s.status == LogStep::Status::Success   ? L"✔ "
                              : s.status == LogStep::Status::Warning ? L"⚠ "
                              : s.status == LogStep::Status::Error   ? L"✖ "
                                                                     : L"– ";
        steps += std::to_wstring(i + 1) + L". " + mark + w(s.title) + L"\n";
    }
    const std::wstring head = res.ok ? formatText(S::ApplyDoneOk, {std::to_wstring(res.completed)}) : std::wstring(tr(S::ApplyDoneFailed));
    const std::wstring body = res.ok ? r->path : w(res.error);
    TASKDIALOG_BUTTON buttons[] = {{101, tr(S::BtnSaveDetails)}};
    TASKDIALOGCONFIG cfg{sizeof cfg};
    cfg.hwndParent = hwnd_;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT | TDF_EXPANDED_BY_DEFAULT;
    cfg.pszWindowTitle = tr(S::ApplyResultTitle);
    cfg.pszMainInstruction = head.c_str();
    cfg.pszContent = body.c_str();
    cfg.pszExpandedInformation = steps.c_str();
    cfg.pButtons = buttons;
    cfg.cButtons = 1;
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    cfg.pszMainIcon = res.ok ? TD_INFORMATION_ICON : TD_ERROR_ICON;
    int pressed = 0;
    TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    if (pressed == 101) {
        const std::wstring path = askSavePath(L"wparted-apply-details.html", L"HTML (*.html)\0*.html\0", L"html");
        if (!path.empty()) {
            const bool ok = saveTextFile(path, logToHtml(res.log, "WParted - " + r->name));
            MessageBoxW(hwnd_, ok ? tr(S::ExportOk) : tr(S::ExportFailed), tr(S::AppTitle), ok ? MB_ICONINFORMATION : MB_ICONERROR);
        }
    }
    reloadImage(r->path);  // show what is in the file now; the old plan no longer applies
}

void MainWindow::reloadImage(const std::wstring& path) {
    if (!result_) return;
    queues_.erase(path);
    for (std::size_t i = 0; i < result_->disks.size(); ++i) {
        win::PhysicalDisk& d = result_->disks[i];
        if (d.path != path || !isImageDisk(&d)) continue;
        FileBlockReader r;  // read-only
        if (!r.open(win::narrow(path), d.logicalSectorSize)) {
            log::warn("Re-reading image failed: " + win::narrow(path));
            continue;
        }
        d.layout = parseDiskLayout(r, true);
        addImageVolumes(d, r);
        log::info("Re-read image after apply: " + std::to_string(d.layout.partitions.size()) + " partition(s)");
        for (auto& img : images_)
            if (img.path == path) {
                img.layout = d.layout;
                img.volumes = d.volumes;
            }
        selectDisk(static_cast<int>(i));
    }
}

void MainWindow::startRescue() {
    const win::PhysicalDisk* d = currentDisk();
    if (!d || rescuing_) return;
    if (!d->layoutAvailable) {
        MessageBoxW(hwnd_, tr(S::ErrNeedsTable), tr(S::RescueTitle), MB_ICONINFORMATION);
        return;
    }
    if (rescueWorker_.joinable()) rescueWorker_.join();
    rescuing_ = true;
    SetWindowTextW(status_, tr(S::RescueRunning));
    const win::PhysicalDisk copy = *d;  // the worker must not touch UI-owned data
    HWND target = hwnd_;
    rescueWorker_ = std::thread([copy, target] {
        auto* r = new RescueResult;
        r->ss = copy.logicalSectorSize;
        r->ok = win::rescueScanDisk(copy, RescueOptions{}, r->found, r->error);
        if (!PostMessageW(target, WM_APP + 51, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

void MainWindow::onRescueDone(LPARAM lp) {
    std::unique_ptr<RescueResult> r(reinterpret_cast<RescueResult*>(lp));
    rescuing_ = false;
    updateStatus();
    std::wstring text;
    if (!r->ok) {
        text = w(r->error);
    } else if (r->found.empty()) {
        text = tr(S::RescueNone);
    } else {
        for (const auto& c : r->found)
            text += L"\u2022 " + std::to_wstring(c.firstLba) + L":  " + w(fsTypeName(c.fs.type)) +
                    (c.sectorCount ? L"  " + w(formatSize(c.sectorCount * r->ss)) : std::wstring()) +
                    (c.fs.label.empty() ? std::wstring() : L"  \"" + w(c.fs.label) + L"\"") + L"  (" +
                    tr((c.firstLba * r->ss) % kMiB == 0 ? S::RescueAligned : S::RescueUnaligned) + L")\n";
        text += L"\n";
        text += tr(S::RescueHint);
    }
    log::info("Rescue scan finished: " + std::to_string(r->found.size()) + " candidate(s)");
    TASKDIALOGCONFIG cfg{sizeof cfg};
    cfg.hwndParent = hwnd_;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    cfg.pszWindowTitle = tr(S::RescueTitle);
    cfg.pszMainInstruction = tr(S::RescueTitle);
    cfg.pszContent = text.c_str();
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    bool sized = false;
    for (const auto& c : r->found) sized = sized || c.sectorCount > 0;
    TASKDIALOG_BUTTON recreate[] = {{300, tr(S::BtnRecreate)}};
    if (r->ok && sized && currentQueue()) {
        cfg.dwFlags |= TDF_USE_COMMAND_LINKS;
        cfg.pButtons = recreate;
        cfg.cButtons = 1;
    }
    int pressed = 0;
    TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    if (pressed == 300 && currentQueue()) {
        std::string err;
        const std::size_t n = planRecreate(*currentQueue(), r->found, err);
        log::info("Rescue: " + std::to_string(n) + " partition(s) to recreate planned" + (err.empty() ? "" : " (" + err + ")"));
        refreshView();
        std::wstring msg = formatText(S::RecreatePlanned, {std::to_wstring(n)});
        if (!err.empty()) msg += L"\n\n" + w(err);
        MessageBoxW(hwnd_, msg.c_str(), tr(S::RescueTitle), n ? MB_ICONINFORMATION : MB_ICONWARNING);
    }
}

// ---------------------------------------------------------------------------
// Files (plan export, disk images)

std::wstring MainWindow::askSavePath(const wchar_t* defaultName, const wchar_t* filter, const wchar_t* ext) {
    wchar_t file[MAX_PATH];
    wcscpy_s(file, MAX_PATH, defaultName);
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = ext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&ofn) ? std::wstring(file) : std::wstring();
}

bool MainWindow::saveTextFile(const std::wstring& path, const std::string& text) {
    // Only ordinary files chosen by the user; never device namespaces.
    if (path.empty() || path.rfind(L"\\\\.\\", 0) == 0 || path.rfind(L"\\\\?\\", 0) == 0) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);  // readonly-guard: allow (user-chosen plan file)
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();  // readonly-guard: allow
    CloseHandle(f);
    return ok;
}

void MainWindow::savePlan() {
    OperationQueue* q = currentQueue();
    if (!q || q->empty()) return;
    const std::wstring path = askSavePath(L"wparted-plan.json", L"WParted plan (*.json)\0*.json\0", L"json");
    if (path.empty()) return;
    const bool ok = saveTextFile(path, q->toJson());
    log::info(std::string("Plan export ") + (ok ? "succeeded" : "failed"));
    MessageBoxW(hwnd_, ok ? tr(S::ExportOk) : tr(S::ExportFailed), tr(S::AppTitle), ok ? MB_ICONINFORMATION : MB_ICONERROR);
}

void MainWindow::chooseImage() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Disk images (*.img;*.raw;*.bin;*.dd)\0*.img;*.raw;*.bin;*.dd\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) openImage(file);
}

void MainWindow::openImage(const std::wstring& path) {
    if (!result_) {
        pendingImage_ = path;  // discovery still running
        return;
    }
    if (path.rfind(L"\\\\.\\", 0) == 0 || path.rfind(L"\\\\?\\", 0) == 0) return;  // images only, no devices
    DiskLayout layout;
    std::uint64_t size = 0;
    std::uint32_t sectorSize = 512;
    for (std::uint32_t ss : {512u, 4096u}) {
        FileBlockReader r;  // opened read-only
        if (!r.open(win::narrow(path), ss)) {
            MessageBoxW(hwnd_, tr(S::ImageOpenFailed), tr(S::AppTitle), MB_ICONERROR);
            return;
        }
        layout = parseDiskLayout(r, true);
        size = r.size();
        sectorSize = ss;
        bool mismatch = false;
        for (const auto& d : layout.diagnostics)
            if (d.code == "gpt.sector_size_mismatch") mismatch = true;
        if (!mismatch) break;
    }
    win::PhysicalDisk d;
    d.number = 900 + static_cast<std::uint32_t>(images_.size());
    d.path = path;
    const std::size_t slash = path.find_last_of(L"\\/");
    d.vendor = "Image";
    d.product = win::narrow(slash == std::wstring::npos ? path : path.substr(slash + 1));
    d.busType = "Image file";
    d.sizeBytes = size;
    d.logicalSectorSize = sectorSize;
    d.rawReadable = true;
    d.layoutAvailable = true;
    d.layout = layout;
    {
        FileBlockReader r;  // read-only
        if (r.open(win::narrow(path), sectorSize)) addImageVolumes(d, r);
    }
    images_.push_back(d);
    result_->disks.push_back(d);
    fallbackLayouts_.push_back(DiskLayout{});
    log::info("Opened disk image read-only: " + win::narrow(path));
    rebuildDeviceMenu();
    selectDisk(static_cast<int>(result_->disks.size()) - 1);
    offerResume(path);
}

// An image with an execution journal was interrupted (crash, power loss or
// cancel during a move): offer to continue it, or to discard the journal.
void MainWindow::offerResume(const std::wstring& path) {
    const std::string journalPath = journalPathFor(win::narrow(path));
    if (!journalExists(journalPath)) return;
    JournalState s;
    std::string err;
    std::wstring content;
    if (!readJournal(journalPath, s, err)) {
        content = w(err);
    } else {
        content = formatText(S::JournalFound, {std::to_wstring(s.completed), std::to_wstring(s.operationCount)});
        if (s.phase == "copy")
            content += L"\n\n" + formatText(S::JournalCopy, {std::to_wstring(s.operation + 1), w(formatSize(s.copyDone))});
        if (s.phase == "work") content += L"\n\n" + formatText(S::JournalWork, {std::to_wstring(s.operation + 1), w(s.step)});
        content += L"\n\n" + std::wstring(tr(s.resumable() ? S::JournalResumable : S::JournalNotResumable));
    }
    log::warn("Execution journal found for " + win::narrow(path) + ": " + s.phase + ", " + std::to_string(s.completed) +
              " operation(s) completed");
    std::vector<TASKDIALOG_BUTTON> buttons;
    if (s.resumable()) buttons.push_back({200, tr(S::BtnResume)});
    buttons.push_back({201, tr(S::BtnDiscardJournal)});
    buttons.push_back({202, tr(S::BtnLater)});
    TASKDIALOGCONFIG cfg{sizeof cfg};
    cfg.hwndParent = hwnd_;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS | TDF_SIZE_TO_CONTENT;
    cfg.pszWindowTitle = tr(S::JournalTitle);
    cfg.pszMainInstruction = tr(S::JournalTitle);
    cfg.pszContent = content.c_str();
    cfg.pButtons = buttons.data();
    cfg.cButtons = static_cast<UINT>(buttons.size());
    cfg.nDefaultButton = 202;
    cfg.pszMainIcon = TD_WARNING_ICON;
    int pressed = 0;
    TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    if (pressed == 200) startImageApply(true);
    if (pressed == 201) {
        const bool ok = discardJournal(journalPath, err);
        log::info(ok ? "Execution journal discarded" : "Discarding the journal failed: " + err);
        MessageBoxW(hwnd_, ok ? tr(S::JournalDiscarded) : w(err).c_str(), tr(S::AppTitle), ok ? MB_ICONINFORMATION : MB_ICONERROR);
    }
}

}  // namespace wparted::ui
