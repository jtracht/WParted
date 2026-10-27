// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Plan execution on disk image files (see executor.hpp). The only write
// paths are ImageFileDevice (a user-chosen regular file, never a device) and
// MemoryBlockDevice (tests).
#include "wparted/core/executor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <sys/stat.h>

#include "wparted/core/layout_writer.hpp"
#include "wparted/core/types.hpp"

#ifdef _WIN32
#include <share.h>
#endif

namespace wparted {

bool MemoryBlockDevice::read(std::uint64_t offset, void* buffer, std::size_t len) {
    if (offset > data_.size() || len > data_.size() - offset) return false;
    if (len) std::memcpy(buffer, data_.data() + offset, len);
    return true;
}

bool MemoryBlockDevice::write(std::uint64_t offset, const void* buffer, std::size_t len) {
    if (offset > data_.size() || len > data_.size() - offset) return false;
    if (len) std::memcpy(data_.data() + offset, buffer, len);
    return true;
}

bool isDevicePath(const std::string& path) {
    std::string p;
    for (char c : path) p += c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (p.rfind("\\\\.\\", 0) == 0 || p.rfind("\\\\?\\", 0) == 0 || p.rfind("\\??\\", 0) == 0) return true;
    if (p.rfind("\\dev\\", 0) == 0) return true;               // /dev/sda, /dev/disk2, ...
    if (p.size() == 2 && p[1] == ':') return true;            // "C:" names a volume
    return p.find("globalroot") != std::string::npos;
}

// ---------------------------------------------------------------------------
// ImageFileDevice

ImageFileDevice::~ImageFileDevice() {
    if (file_) std::fclose(file_);
}

bool ImageFileDevice::open(const std::string& path, std::uint32_t sectorSize, std::string& error) {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    if (isDevicePath(path)) {
        error = "only image files can be modified; device paths are refused";
        return false;
    }
#ifdef _WIN32
    // UTF-8 path -> UTF-16; exclusive access so that a VM or a second WParted
    // instance cannot use the image at the same time.
    std::vector<std::uint8_t> wide(2 * 32768 + 2, 0);
    if (!utf8ToUtf16le(path, wide.data(), 32767)) {
        error = "invalid file name";
        return false;
    }
    std::wstring wpath;
    for (std::size_t i = 0; wide[2 * i] || wide[2 * i + 1]; ++i)
        wpath += static_cast<wchar_t>(wide[2 * i] | (wide[2 * i + 1] << 8));
    file_ = _wfsopen(wpath.c_str(), L"r+b", _SH_DENYRW);  // readonly-guard: allow (user-chosen image file)
    struct _stat64 st {};
    const bool regular = file_ && _fstat64(_fileno(file_), &st) == 0 && (st.st_mode & _S_IFMT) == _S_IFREG;
#else
    file_ = std::fopen(path.c_str(), "r+b");  // readonly-guard: allow (user-chosen image file)
    struct stat st {};
    const bool regular = file_ && fstat(fileno(file_), &st) == 0 && S_ISREG(st.st_mode);
#endif
    if (!file_) {
        error = "the image file cannot be opened for writing (missing, read-only or in use)";
        return false;
    }
    if (!regular) {
        error = "not a regular file";
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    size_ = static_cast<std::uint64_t>(st.st_size);
    sectorSize_ = sectorSize;
    if (size_ == 0 || sectorSize_ == 0 || size_ % sectorSize_ != 0) {
        error = "the image size is not a multiple of the sector size";
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    return true;
}

namespace {
bool seekTo(std::FILE* f, std::uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<long long>(offset), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}
}  // namespace

bool ImageFileDevice::read(std::uint64_t offset, void* buffer, std::size_t len) {
    if (!file_ || offset > size_ || len > size_ - offset || !seekTo(file_, offset)) return false;
    return std::fread(buffer, 1, len, file_) == len;
}

bool ImageFileDevice::write(std::uint64_t offset, const void* buffer, std::size_t len) {
    // Never grows the file: writes must stay inside the existing image.
    if (!file_ || offset > size_ || len > size_ - offset || !seekTo(file_, offset)) return false;
    return std::fwrite(buffer, 1, len, file_) == len;
}

bool ImageFileDevice::flush() { return file_ && std::fflush(file_) == 0; }

// ---------------------------------------------------------------------------
// Execution

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Partition table on the device equals the layout the plan was made for.
bool sameTable(const DiskLayout& a, const DiskLayout& b, std::string& why) {
    if (a.type != b.type || a.sectorSize != b.sectorSize || a.diskBytes != b.diskBytes) {
        why = "table type, sector size or device size differs";
        return false;
    }
    if (a.type == TableType::Gpt && a.diskGuid != b.diskGuid) { why = "GPT disk GUID differs"; return false; }
    if (a.type == TableType::Mbr && a.mbrSignature != b.mbrSignature) { why = "MBR disk signature differs"; return false; }
    if (a.partitions.size() != b.partitions.size()) { why = "number of partitions differs"; return false; }
    auto byStart = [](std::vector<PartitionEntry> v) {
        std::sort(v.begin(), v.end(), [](const PartitionEntry& x, const PartitionEntry& y) {
            return x.firstLba != y.firstLba ? x.firstLba < y.firstLba : x.number < y.number;
        });
        return v;
    };
    const auto x = byStart(a.partitions), y = byStart(b.partitions);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const PartitionEntry &p = x[i], &q = y[i];
        if (p.number != q.number || p.kind != q.kind || p.firstLba != q.firstLba || p.sectorCount != q.sectorCount ||
            p.mbrType != q.mbrType || p.mbrBootable != q.mbrBootable || p.typeGuid != q.typeGuid ||
            (a.type == TableType::Gpt && !(q.uniqueGuid == Guid{}) && p.uniqueGuid != q.uniqueGuid) || p.name != q.name ||
            p.gptAttributes != q.gptAttributes) {
            why = "partition #" + std::to_string(p.number) + " differs";
            return false;
        }
    }
    return true;
}

class Runner {
public:
    Runner(BlockDevice& d, const ExecuteOptions& o) : dev_(d), o_(o), ss_(d.sectorSize()) {}

    // Overlap-safe raw copy of `bytes` from `from` to `to` (byte offsets).
    bool copy(std::size_t op, std::uint64_t from, std::uint64_t to, std::uint64_t bytes, std::string& err) {
        if (from == to || bytes == 0) return true;
        const std::uint64_t chunk = std::max<std::uint64_t>(ss_, o_.chunkBytes - o_.chunkBytes % ss_);
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(std::min(chunk, bytes)));
        const bool backwards = to > from && to < from + bytes;  // destination overlaps the source tail
        for (std::uint64_t done = 0; done < bytes;) {
            const std::uint64_t n = std::min<std::uint64_t>(buf.size(), bytes - done);
            const std::uint64_t off = backwards ? bytes - done - n : done;
            if (!dev_.read(from + off, buf.data(), static_cast<std::size_t>(n))) {
                err = "read failed at byte " + std::to_string(from + off);
                return false;
            }
            if (!dev_.write(to + off, buf.data(), static_cast<std::size_t>(n))) {
                err = "write failed at byte " + std::to_string(to + off);
                return false;
            }
            done += n;
            if (o_.progress) o_.progress(op, done, bytes);
        }
        return dev_.flush() || (err = "flush failed", false);
    }

    // Clears old file-system signatures at the start of a region (up to 1 MiB).
    bool wipeStart(std::uint64_t firstLba, std::uint64_t sectors, std::string& err) {
        const std::uint64_t bytes = std::min<std::uint64_t>(sectors * ss_, 1024 * 1024);
        std::vector<std::uint8_t> zero(static_cast<std::size_t>(bytes), 0);
        if (!dev_.write(firstLba * ss_, zero.data(), zero.size()) || !dev_.flush()) {
            err = "clearing old signatures failed";
            return false;
        }
        return true;
    }

    bool writeTable(DiskLayout& layout, std::string& err) {
        assignNewIdentifiers(layout);
        SerializeOptions opt;
        opt.originalSector0.resize(ss_);
        if (!dev_.read(0, opt.originalSector0.data(), ss_)) {
            err = "reading sector 0 failed";
            return false;
        }
        std::vector<SectorWrite> writes;
        if (!serializeLayout(layout, opt, writes, err)) return false;
        for (const auto& w : writes)
            if (!dev_.write(w.lba * ss_, w.data.data(), w.data.size())) {
                err = "writing sector " + std::to_string(w.lba) + " failed";
                return false;
            }
        return dev_.flush() || (err = "flush failed", false);
    }

private:
    BlockDevice& dev_;
    const ExecuteOptions& o_;
    std::uint32_t ss_;
};

}  // namespace

ExecuteResult executePlanOnImage(BlockDevice& dev, const OperationQueue& plan, const std::string& deviceName,
                                 const ExecuteOptions& o) {
    ExecuteResult r;
    r.log.title = "Apply " + std::to_string(plan.operations().size()) + " operation(s) to " + deviceName;
    r.log.startMs = nowMs();
    auto finish = [&](bool ok, const std::string& error) {
        r.ok = ok;
        r.error = error;
        r.log.status = ok ? LogStep::Status::Success : LogStep::Status::Error;
        if (!error.empty()) r.log.messages.push_back(error);
        r.log.endMs = nowMs();
        return r;
    };

    // 1. The device must still hold exactly the layout the plan was made for.
    const DiskLayout now = parseDiskLayout(dev, false);
    std::string why;
    if (!sameTable(now, plan.original(), why)) return finish(false, "The image changed since planning: " + why + ".");

    // 2. Validate against the image backend.
    ValidationContext ctx;
    ctx.mode = ValidationMode::ImageExecution;
    ctx.haveCurrentIdentity = true;
    ctx.currentIdentity = identityOf(now, plan.identity().serial, plan.identity().model);
    r.validation = validatePlan(plan, ctx);
    for (const auto& d : r.validation.diagnostics)
        if (d.severity != Severity::Info) r.log.messages.push_back(d.message);
    for (const auto& op : plan.operations()) {
        LogStep s;
        s.title = describe(op, plan.original());
        s.status = LogStep::Status::Pending;
        r.log.children.push_back(s);
    }
    if (!r.validation.ok()) {
        for (auto& c : r.log.children) c.status = LogStep::Status::Skipped;
        std::string first;
        for (const auto& d : r.validation.diagnostics)
            if (d.severity == Severity::Error && first.empty()) first = d.message;
        return finish(false, "The plan did not pass validation; nothing was written. " + first);
    }

    // 3. Execute step by step on a copy of the planned original.
    Runner run(dev, o);
    DiskLayout layout = plan.original();
    std::uint32_t next = static_cast<std::uint32_t>(layout.partitions.size()) + 1;
    for (std::size_t i = 0; i < plan.operations().size(); ++i) {
        LogStep& step = r.log.children[i];
        if (o.cancel && o.cancel->load()) {
            for (std::size_t k = i; k < r.log.children.size(); ++k) r.log.children[k].status = LogStep::Status::Skipped;
            return finish(false, "Cancelled before operation " + std::to_string(i + 1) + ".");
        }
        step.startMs = nowMs();
        step.status = LogStep::Status::Running;
        Operation op = plan.operations()[i];
        step.title = describe(op, layout);
        const DiskLayout before = layout;
        std::string err;
        bool ok = simulate(op, layout, next, err);
        const std::uint32_t ss = layout.sectorSize;
        if (ok && op.type == OpType::ResizeMove) {
            const PartitionEntry* a = findPlanned(before, op.target);
            const PartitionEntry* b = findPlanned(layout, op.target);
            if (a && b && a->firstLba != b->firstLba && a->kind == PartitionKind::Extended) {
                ok = false;  // the logical partitions and EBR chain would have to move as well
                err = "moving an extended partition is not supported yet";
            } else if (a && b && a->firstLba != b->firstLba) {
                step.messages.push_back("Moving " + formatSize(std::min(a->sectorCount, b->sectorCount) * ss) +
                                        " of data from sector " + std::to_string(a->firstLba) + " to " +
                                        std::to_string(b->firstLba));
                ok = run.copy(i, a->firstLba * ss, b->firstLba * ss, std::min(a->sectorCount, b->sectorCount) * ss, err);
            }
        } else if (ok && op.type == OpType::Copy) {
            const PartitionEntry* a = findPlanned(before, op.target);
            const PartitionEntry* b = findPlanned(layout, op.result);
            if (!a || !b) {
                ok = false;
                err = "copy source or destination not found";
            } else {
                step.messages.push_back("Copying " + formatSize(a->sectorCount * ss) + " from sector " +
                                        std::to_string(a->firstLba) + " to " + std::to_string(b->firstLba));
                ok = run.copy(i, a->firstLba * ss, b->firstLba * ss, a->sectorCount * ss, err);
            }
        }
        if (ok) ok = run.writeTable(layout, err);
        if (ok && op.type == OpType::CreatePartition && op.kind != PartitionKind::Extended) {
            if (const PartitionEntry* p = findPlanned(layout, op.result)) {
                step.messages.push_back("Clearing old signatures in the first MiB of the new partition");
                ok = run.wipeStart(p->firstLba, p->sectorCount, err);
            }
        } else if (ok && op.type == OpType::Format && op.fs == FsType::Unknown) {
            if (const PartitionEntry* p = findPlanned(layout, op.target)) {
                step.messages.push_back("Clearing file system signatures in the first MiB");
                ok = run.wipeStart(p->firstLba, p->sectorCount, err);
            }
        }
        // Calibration: what is on the image now must be the simulated layout.
        if (ok) {
            const DiskLayout back = parseDiskLayout(dev, false);
            if (back.hasErrors() || !sameTable(back, layout, why)) {
                ok = false;
                err = "verification after writing failed: " + (back.hasErrors() ? std::string("table has errors") : why);
            }
        }
        step.endMs = nowMs();
        if (!ok) {
            step.status = LogStep::Status::Error;
            step.messages.push_back(err);
            for (std::size_t k = i + 1; k < r.log.children.size(); ++k) r.log.children[k].status = LogStep::Status::Skipped;
            return finish(false, "Operation " + std::to_string(i + 1) + " failed: " + err + ". " +
                                     (r.completed ? std::to_string(r.completed) +
                                                        " operation(s) before it were applied; there is no automatic rollback."
                                                  : std::string("Nothing before it was applied.")));
        }
        step.status = LogStep::Status::Success;
        step.messages.push_back("Verified: the partition table read back matches the plan");
        ++r.completed;
    }
    return finish(true, "");
}

}  // namespace wparted
