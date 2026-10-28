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
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sys/stat.h>

#include "wparted/core/fs_tools.hpp"
#include "wparted/core/json.hpp"
#include "wparted/core/json_value.hpp"
#include "wparted/core/layout_writer.hpp"
#include "wparted/core/types.hpp"

#ifdef _WIN32
#include <io.h>
#include <share.h>
#else
#include <unistd.h>
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

namespace {
#ifdef _WIN32
// UTF-8 path -> UTF-16 for the _w* file functions.
bool widePath(const std::string& path, std::wstring& out) {
    std::vector<std::uint8_t> wide(2 * 32768 + 2, 0);
    if (!utf8ToUtf16le(path, wide.data(), 32767)) return false;
    out.clear();
    for (std::size_t i = 0; wide[2 * i] || wide[2 * i + 1]; ++i)
        out += static_cast<wchar_t>(wide[2 * i] | (wide[2 * i + 1] << 8));
    return true;
}
#endif
}  // namespace

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
    // Exclusive access so that a VM or a second WParted instance cannot use
    // the image at the same time.
    std::wstring wpath;
    if (!widePath(path, wpath)) {
        error = "invalid file name";
        return false;
    }
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

// Durable: the journal relies on data being on disk before it records progress.
bool ImageFileDevice::flush() {
    if (!file_ || std::fflush(file_) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file_)) == 0;
#else
    return fsync(fileno(file_)) == 0;
#endif
}

// ---------------------------------------------------------------------------
// Execution journal

std::string journalPathFor(const std::string& imagePath) { return imagePath + ".wparted-journal"; }

namespace {

constexpr std::size_t kJournalHeader = 512;
constexpr std::uint64_t kJournalMaxBytes = 64ull * 1024 * 1024;

std::FILE* openForRead(const std::string& path) {
#ifdef _WIN32
    std::wstring w;
    return widePath(path, w) ? _wfsopen(w.c_str(), L"rb", _SH_DENYNO) : nullptr;
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

bool commitFile(std::FILE* f) {
    if (std::fflush(f) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

// The checked part of the progress header.
std::string headerFields(const JournalState& s) {
    char fields[160];
    std::snprintf(fields, sizeof fields, "completed=%010llu phase=%s operation=%010llu done=%020llu",
                  static_cast<unsigned long long>(s.completed), s.phase.c_str(),
                  static_cast<unsigned long long>(s.operation), static_cast<unsigned long long>(s.copyDone));
    std::string step = s.step.substr(0, 300);
    for (char& c : step)
        if (c == '\n' || c == '\r') c = ' ';
    return std::string(fields) + "\nstep=" + step;
}

std::string headerText(const JournalState& s) {
    const std::string fields = headerFields(s);
    char check[40];
    std::snprintf(check, sizeof check, "\ncheck=%016llx\n", static_cast<unsigned long long>(fnv1a(fields)));
    std::string h = "WPARTED-JOURNAL 1\n" + fields + check;
    h.resize(kJournalHeader - 1, ' ');
    return h + '\n';
}

bool parseHeader(const std::string& h, JournalState& s) {
    if (h.size() != kJournalHeader || h.compare(0, 18, "WPARTED-JOURNAL 1\n") != 0) return false;
    const std::size_t stepAt = h.find("\nstep=", 18), checkAt = h.find("\ncheck=", 18);
    if (stepAt == std::string::npos || checkAt == std::string::npos || checkAt < stepAt) return false;
    // "name=value" fields of the first line; values end at a space or newline.
    const std::string line = h.substr(18, stepAt - 18);
    auto field = [&](const char* name) -> std::string {
        const std::string key = std::string(name) + "=";
        const std::size_t at = line.find(key);
        if (at == std::string::npos || (at && line[at - 1] != ' ')) return {};
        return line.substr(at + key.size(), line.find(' ', at) - at - key.size());
    };
    auto number = [](const std::string& v, int base, std::uint64_t& out) {
        if (v.empty()) return false;
        char* end = nullptr;
        out = std::strtoull(v.c_str(), &end, base);
        return end && *end == '\0';
    };
    std::uint64_t completed = 0, operation = 0, done = 0, check = 0;
    const std::size_t checkEnd = h.find('\n', checkAt + 1);
    if (!number(field("completed"), 10, completed) || !number(field("operation"), 10, operation) ||
        !number(field("done"), 10, done) || checkEnd == std::string::npos ||
        !number(h.substr(checkAt + 7, checkEnd - checkAt - 7), 16, check))
        return false;
    s.completed = static_cast<std::size_t>(completed);
    s.phase = field("phase");
    s.operation = static_cast<std::size_t>(operation);
    s.copyDone = done;
    s.step = h.substr(stepAt + 6, checkAt - stepAt - 6);
    return fnv1a(headerFields(s)) == check && (s.phase == "idle" || s.phase == "copy" || s.phase == "work");
}

std::string toHex(const std::vector<std::uint8_t>& v) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * 2);
    for (std::uint8_t b : v) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

bool fromHex(const std::string& s, std::vector<std::uint8_t>& out) {
    if (s.size() % 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    out.resize(s.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = nibble(s[2 * i]), lo = nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return true;
}

// Records every read, so that the layout detection can be replayed later
// from the journal and yields exactly the same original layout.
class RecordingReader final : public BlockReader {
public:
    explicit RecordingReader(BlockReader& inner) : inner_(inner) {}
    std::uint64_t size() const override { return inner_.size(); }
    std::uint32_t sectorSize() const override { return inner_.sectorSize(); }
    bool read(std::uint64_t offset, void* buffer, std::size_t len) override {
        if (!inner_.read(offset, buffer, len)) {
            failed.insert({offset, len});
            return false;
        }
        auto& piece = pieces[offset];
        if (piece.size() < len) piece.assign(static_cast<std::uint8_t*>(buffer), static_cast<std::uint8_t*>(buffer) + len);
        return true;
    }
    std::map<std::uint64_t, std::vector<std::uint8_t>> pieces;
    std::set<std::pair<std::uint64_t, std::size_t>> failed;

private:
    BlockReader& inner_;
};

class ReplayReader final : public BlockReader {
public:
    ReplayReader(std::uint64_t size, std::uint32_t sectorSize) : size_(size), sectorSize_(sectorSize) {}
    std::uint64_t size() const override { return size_; }
    std::uint32_t sectorSize() const override { return sectorSize_; }
    bool read(std::uint64_t offset, void* buffer, std::size_t len) override {
        if (failed.count({offset, len})) return false;
        auto* out = static_cast<std::uint8_t*>(buffer);
        for (std::uint64_t pos = offset; pos < offset + len;) {
            const std::vector<std::uint8_t>* hit = nullptr;
            std::uint64_t start = 0;
            for (auto it = pieces.upper_bound(pos); it != pieces.begin();) {
                --it;
                if (it->first + it->second.size() > pos) {
                    hit = &it->second;
                    start = it->first;
                    break;
                }
            }
            if (!hit) return false;  // never read during planning
            const std::uint64_t n = std::min<std::uint64_t>(offset + len, start + hit->size()) - pos;
            std::memcpy(out + (pos - offset), hit->data() + (pos - start), static_cast<std::size_t>(n));
            pos += n;
        }
        return true;
    }
    std::map<std::uint64_t, std::vector<std::uint8_t>> pieces;
    std::set<std::pair<std::uint64_t, std::size_t>> failed;

private:
    std::uint64_t size_;
    std::uint32_t sectorSize_;
};

class Journal {
public:
    Journal() = default;
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    ~Journal() { close(); }

    bool active() const { return file_ != nullptr; }

    // Creates a new journal; refuses to overwrite an existing one.
    bool create(const std::string& path, const std::string& body, const JournalState& state, std::string& err) {
        if (journalExists(path)) {
            err = "an execution journal already exists for this image";
            return false;
        }
#ifdef _WIN32
        std::wstring w;
        file_ = widePath(path, w) ? _wfsopen(w.c_str(), L"wb", _SH_DENYWR) : nullptr;  // readonly-guard: allow (journal next to the user-chosen image file)
#else
        file_ = std::fopen(path.c_str(), "wb");  // readonly-guard: allow (journal next to the user-chosen image file)
#endif
        path_ = path;
        const std::string header = headerText(state);
        if (!file_ || std::fwrite(header.data(), 1, header.size(), file_) != header.size() ||
            std::fwrite(body.data(), 1, body.size(), file_) != body.size() || !commitFile(file_)) {
            err = "the execution journal cannot be written next to the image";
            if (file_) remove();
            return false;
        }
        return true;
    }

    // Opens an existing journal to continue it.
    bool reopen(const std::string& path, std::string& err) {
#ifdef _WIN32
        std::wstring w;
        file_ = widePath(path, w) ? _wfsopen(w.c_str(), L"r+b", _SH_DENYWR) : nullptr;  // readonly-guard: allow (journal next to the user-chosen image file)
#else
        file_ = std::fopen(path.c_str(), "r+b");  // readonly-guard: allow (journal next to the user-chosen image file)
#endif
        path_ = path;
        if (!file_) err = "the execution journal cannot be opened for writing";
        return file_ != nullptr;
    }

    // Rewrites the progress header in place and forces it to disk.
    bool update(const JournalState& state, std::string& err) {
        if (!file_) return true;
        const std::string header = headerText(state);
        if (!seekTo(file_, 0) || std::fwrite(header.data(), 1, header.size(), file_) != header.size() ||
            !commitFile(file_)) {
            err = "updating the execution journal failed";
            return false;
        }
        return true;
    }

    void close() {
        if (file_) std::fclose(file_);
        file_ = nullptr;
    }

    bool remove() {
        close();
        std::string err;
        return path_.empty() || discardJournal(path_, err);
    }

private:
    std::FILE* file_ = nullptr;
    std::string path_;
};

std::string journalBody(const OperationQueue& plan, const RecordingReader& rec) {
    JsonWriter w;
    w.beginObject();
    w.key("schema").value("wparted-journal");
    w.key("version").value(1);
    w.key("imageBytes").value(rec.size());
    w.key("sectorSize").value(rec.sectorSize());
    w.key("plan").value(plan.toJson());
    w.key("snapshot").beginArray();
    for (const auto& p : rec.pieces) {
        w.beginObject();
        w.key("offset").value(p.first);
        w.key("hex").value(toHex(p.second));
        w.endObject();
    }
    w.endArray();
    w.key("failed").beginArray();
    for (const auto& f : rec.failed) {
        w.beginObject();
        w.key("offset").value(f.first);
        w.key("length").value(static_cast<std::uint64_t>(f.second));
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.str();
}

bool readJournalFile(const std::string& path, JournalState& s, JsonValue& body, std::string& err) {
    std::FILE* f = openForRead(path);
    if (!f) {
        err = "the execution journal cannot be read";
        return false;
    }
    std::string text;
    char buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) {
        text.append(buf, n);
        if (text.size() > kJournalMaxBytes) break;
    }
    std::fclose(f);
    if (text.size() > kJournalMaxBytes || text.size() <= kJournalHeader || !parseHeader(text.substr(0, kJournalHeader), s)) {
        err = "the execution journal is damaged";
        return false;
    }
    JsonParseLimits limits;
    limits.maxBytes = static_cast<std::size_t>(kJournalMaxBytes);
    if (!parseJson(text.substr(kJournalHeader), body, err, limits) || !body.isObject() ||
        body.getString("schema") != "wparted-journal" || body.getUint("version") != 1) {
        err = "the execution journal is damaged" + (err.empty() ? std::string() : ": " + err);
        return false;
    }
    s.imageBytes = body.getUint("imageBytes");
    s.sectorSize = static_cast<std::uint32_t>(body.getUint("sectorSize"));
    s.planJson = body.getString("plan");
    JsonValue plan;
    std::string planErr;
    if (parseJson(s.planJson, plan, planErr) && plan.isObject())
        if (const JsonValue* ops = plan.find("operations"))
            s.operationCount = ops->array.size();
    return true;
}

}  // namespace

bool journalExists(const std::string& journalPath) {
    std::FILE* f = openForRead(journalPath);
    if (f) std::fclose(f);
    return f != nullptr;
}

bool readJournal(const std::string& journalPath, JournalState& out, std::string& error) {
    JsonValue body;
    return readJournalFile(journalPath, out, body, error);
}

bool discardJournal(const std::string& journalPath, std::string& error) {
#ifdef _WIN32
    std::wstring w;
    const bool ok = widePath(journalPath, w) && _wremove(w.c_str()) == 0;
#else
    const bool ok = std::remove(journalPath.c_str()) == 0;
#endif
    if (!ok) error = "the execution journal cannot be deleted";
    return ok;
}

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

// Why a copy stopped early.
enum class CopyStop { None, Error, Cancelled, Aborted };

class Runner {
public:
    Runner(BlockDevice& d, const ExecuteOptions& o, Journal& j) : dev_(d), o_(o), journal_(j), ss_(d.sectorSize()) {}

    JournalState state;  // what the journal says right now
    CopyStop stop = CopyStop::None;

    bool record(std::string& err) { return journal_.update(state, err); }

    bool phase(const char* name, std::size_t op, const std::string& step, std::string& err) {
        state.phase = name;
        state.operation = op;
        state.copyDone = 0;
        state.step = step;
        return record(err);
    }

    // Raw copy of `bytes` from `from` to `to` (byte offsets), continuing after
    // `done` bytes. Where source and destination overlap, a chunk is never
    // larger than the distance between them: the chunk being written never
    // overlaps the source data it was read from, so repeating it after an
    // interruption is safe. Progress is journalled after the data is on disk.
    bool copy(std::size_t op, std::uint64_t from, std::uint64_t to, std::uint64_t bytes, std::uint64_t done,
              std::string& err) {
        if (from == to || bytes == 0) return true;
        const std::uint64_t distance = from > to ? from - to : to - from;
        std::uint64_t chunk = std::max<std::uint64_t>(ss_, o_.chunkBytes - o_.chunkBytes % ss_);
        if (distance < bytes) chunk = std::min(chunk, distance);  // overlapping move
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(std::min(chunk, bytes)));
        const bool backwards = to > from && to < from + bytes;  // destination overlaps the source tail
        state.phase = "copy";
        state.operation = op;
        state.copyDone = done;
        if (!record(err)) return fail(CopyStop::Error);
        std::uint64_t chunks = 0;
        while (done < bytes) {
            if (o_.cancel && o_.cancel->load()) {
                err = "cancelled during the block copy";
                return fail(CopyStop::Cancelled);
            }
            const std::uint64_t n = std::min<std::uint64_t>(buf.size(), bytes - done);
            const std::uint64_t off = backwards ? bytes - done - n : done;
            if (!dev_.read(from + off, buf.data(), static_cast<std::size_t>(n))) {
                err = "read failed at byte " + std::to_string(from + off);
                return fail(CopyStop::Error);
            }
            if (!dev_.write(to + off, buf.data(), static_cast<std::size_t>(n))) {
                err = "write failed at byte " + std::to_string(to + off);
                return fail(CopyStop::Error);
            }
            if (o_.abortAfterChunks && ++chunks >= o_.abortAfterChunks) {
                err = "aborted by fault injection";  // like a crash: no flush, no journal update
                return fail(CopyStop::Aborted);
            }
            if (!dev_.flush()) {
                err = "flush failed";
                return fail(CopyStop::Error);
            }
            done += n;
            state.copyDone = done;
            if (!record(err)) return fail(CopyStop::Error);
            if (o_.progress) o_.progress(op, done, bytes);
        }
        return true;
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
    bool fail(CopyStop why) {
        stop = why;
        return false;
    }

    BlockDevice& dev_;
    const ExecuteOptions& o_;
    Journal& journal_;
    std::uint32_t ss_;
};

// Where an execution starts: from the beginning, or from a journal.
struct StartPoint {
    std::size_t completed = 0;  // operations already applied
    bool inCopy = false;        // operation `completed` was interrupted during its block copy
    std::uint64_t copyDone = 0;
};

ExecuteResult runPlan(BlockDevice& dev, const OperationQueue& plan, const std::string& deviceName, const ExecuteOptions& o,
                      const StartPoint& start, Journal& journal, ExecuteResult r) {
    auto finish = [&](bool ok, const std::string& error) {
        r.ok = ok;
        r.error = error;
        r.log.status = ok ? LogStep::Status::Success : LogStep::Status::Error;
        if (!error.empty()) r.log.messages.push_back(error);
        r.log.endMs = nowMs();
        return r;
    };
    (void)deviceName;

    Runner run(dev, o, journal);
    run.state.completed = start.completed;
    run.state.phase = "idle";
    DiskLayout layout = plan.original();
    std::uint32_t next = static_cast<std::uint32_t>(layout.partitions.size()) + 1;
    for (std::size_t i = 0; i < start.completed; ++i) {  // replay what is already on the image
        Operation op = plan.operations()[i];
        std::string err;
        simulate(op, layout, next, err);
        r.log.children[i].status = LogStep::Status::Success;
        r.log.children[i].messages.push_back("Applied before the interruption");
    }
    r.completed = start.completed;
    std::string why;
    for (std::size_t i = start.completed; i < plan.operations().size(); ++i) {
        LogStep& step = r.log.children[i];
        const bool resumeCopy = start.inCopy && i == start.completed;
        if (o.cancel && o.cancel->load()) {
            for (std::size_t k = i; k < r.log.children.size(); ++k) r.log.children[k].status = LogStep::Status::Skipped;
            journal.close();  // stays: the remaining operations can be resumed
            return finish(false, "Cancelled before operation " + std::to_string(i + 1) +
                                     ". The completed operations stay applied; the rest can be resumed.");
        }
        step.startMs = nowMs();
        step.status = LogStep::Status::Running;
        Operation op = plan.operations()[i];
        step.title = describe(op, layout);
        const DiskLayout before = layout;
        std::string err;
        bool ok = simulate(op, layout, next, err);
        bool warn = false;
        if (ok && !resumeCopy) ok = run.phase("work", i, step.title, err);
        const std::uint32_t ss = layout.sectorSize;
        auto region = [&](const PartitionEntry& p) { return RegionDevice(dev, p.firstLba * ss, p.sectorCount * ss); };
        const PartitionEntry* a = findPlanned(before, op.target);  // target before the operation
        const PartitionEntry* b = findPlanned(layout, op.type == OpType::Copy || op.type == OpType::CreatePartition
                                                          ? op.result
                                                          : op.target);  // target/result after it
        const FsType fs = a ? a->fs.type : FsType::Unknown;
        const bool fsResize = ok && op.type == OpType::ResizeMove && a && b && a->kind != PartitionKind::Extended &&
                              fs != FsType::Unknown && a->sectorCount != b->sectorCount;
        if (resumeCopy) step.messages.push_back("Resuming the block copy after " + formatSize(start.copyDone));
        if (ok && op.type == OpType::ResizeMove && a && b && a->kind != PartitionKind::Extended) {
            if (ok && fsResize && !fstools::canResize(fs)) {
                ok = false;
                err = std::string("resizing ") + fsTypeName(fs) + " is not supported";
            }
            if (ok && fsResize && b->sectorCount < a->sectorCount && !resumeCopy) {  // shrink the file system first
                step.messages.push_back("Shrinking the " + std::string(fsTypeName(fs)) + " file system to " +
                                        formatSize(b->sectorCount * ss));
                RegionDevice part = region(*a);
                ok = fstools::resize(part, fs, b->sectorCount * ss, err);
            }
            if (ok && a->firstLba != b->firstLba) {
                if (!resumeCopy)
                    step.messages.push_back("Moving " + formatSize(std::min(a->sectorCount, b->sectorCount) * ss) +
                                            " of data from sector " + std::to_string(a->firstLba) + " to " +
                                            std::to_string(b->firstLba));
                ok = run.copy(i, a->firstLba * ss, b->firstLba * ss, std::min(a->sectorCount, b->sectorCount) * ss,
                              resumeCopy ? start.copyDone : 0, err);
            }
        } else if (ok && op.type == OpType::Copy) {
            if (!a || !b) {
                ok = false;
                err = "copy source or destination not found";
            } else {
                if (!resumeCopy)
                    step.messages.push_back("Copying " + formatSize(a->sectorCount * ss) + " from sector " +
                                            std::to_string(a->firstLba) + " to " + std::to_string(b->firstLba));
                ok = run.copy(i, a->firstLba * ss, b->firstLba * ss, a->sectorCount * ss, resumeCopy ? start.copyDone : 0,
                              err);
            }
        }
        if (!ok && (run.stop == CopyStop::Cancelled || run.stop == CopyStop::Aborted)) {
            step.status = LogStep::Status::Error;
            step.messages.push_back(err);
            for (std::size_t k = i + 1; k < r.log.children.size(); ++k) r.log.children[k].status = LogStep::Status::Skipped;
            journal.close();  // stays on disk: the copy continues from here on resume
            return finish(false, "Operation " + std::to_string(i + 1) + " was interrupted during the block copy (" +
                                     formatSize(run.state.copyDone) +
                                     " done). The partition data is split between the old and the new position; "
                                     "resume the execution to finish it.");
        }
        if (ok && run.state.phase == "copy") ok = run.phase("work", i, "writing the partition table", err);
        if (ok) ok = run.writeTable(layout, err);
        // A moved extended partition gets its first EBR at the new start; the old
        // one is cleared unless the new chain uses that sector.
        if (ok && op.type == OpType::ResizeMove && a && b && a->kind == PartitionKind::Extended &&
            a->firstLba != b->firstLba) {
            bool used = false;
            for (const auto& p : layout.partitions)
                if (p.kind == PartitionKind::Logical && p.firstLba - 1 <= a->firstLba && a->firstLba <= p.lastLba())
                    used = true;  // the EBR in front of a logical partition, or its data
            if (!used) {
                std::vector<std::uint8_t> zero(ss, 0);
                ok = dev.write(a->firstLba * ss, zero.data(), zero.size()) && dev.flush();
                if (!ok) err = "clearing the old extended boot record failed";
                else step.messages.push_back("Cleared the old extended boot record at sector " + std::to_string(a->firstLba));
            }
        }
        // File-system steps on the partition as it is now.
        std::string newUuid;
        if (ok && b) {
            RegionDevice part = region(*b);
            switch (op.type) {
                case OpType::CreatePartition:
                case OpType::Format:
                    if (b->kind == PartitionKind::Extended) break;
                    step.messages.push_back(op.type == OpType::Format ? "Clearing file system signatures in the first MiB"
                                                                      : "Clearing old signatures in the first MiB of the new partition");
                    ok = run.wipeStart(b->firstLba, b->sectorCount, err);
                    if (ok && op.fs != FsType::Unknown) {
                        step.messages.push_back("Creating " + std::string(fsTypeName(op.fs)) + " file system" +
                                                (op.label.empty() ? std::string() : " labelled \"" + op.label + "\""));
                        ok = fstools::create(part, op.fs, op.label, b->firstLba, err);
                    }
                    break;
                case OpType::ResizeMove:
                    if (fsResize && b->sectorCount > a->sectorCount) {  // grow the file system last
                        step.messages.push_back("Growing the " + std::string(fsTypeName(fs)) + " file system to " +
                                                formatSize(b->sectorCount * ss));
                        ok = fstools::resize(part, fs, b->sectorCount * ss, err);
                    }
                    break;
                case OpType::Copy:
                    if (b->sectorCount > a->sectorCount && fstools::canResize(fs)) {
                        step.messages.push_back("Growing the copied file system to the partition size");
                        ok = fstools::resize(part, fs, b->sectorCount * ss, err);
                    }
                    // Linux refuses or confuses two file systems with the same UUID.
                    if (ok && fstools::canSetUuid(fs) &&
                        (fs == FsType::Ext2 || fs == FsType::Ext3 || fs == FsType::Ext4 || fs == FsType::Xfs ||
                         fs == FsType::Btrfs || fs == FsType::LinuxSwap)) {
                        std::string uuidErr;
                        if (fstools::setNewUuid(part, fs, newUuid, uuidErr)) {
                            step.messages.push_back("New UUID for the copy: " + newUuid);
                        } else {  // the copy itself is fine; tell the user
                            step.messages.push_back("Warning: the copy keeps the UUID of the original (" + uuidErr +
                                                    "); give it a new UUID before using both at the same time");
                            warn = true;
                            newUuid.clear();
                        }
                    }
                    break;
                case OpType::SetLabel:
                    step.messages.push_back("Setting the label to \"" + op.label + "\"");
                    ok = fstools::setLabel(part, b->fs.type, op.label, err);
                    break;
                case OpType::NewUuid:
                    ok = fstools::setNewUuid(part, b->fs.type, newUuid, err);
                    if (ok) step.messages.push_back("New UUID/serial: " + newUuid);
                    break;
                case OpType::Check: {
                    fstools::CheckReport rep;
                    ok = fstools::check(part, b->fs.type, rep, err);
                    if (ok) {
                        for (const auto& e : rep.errors) step.messages.push_back("Error: " + e);
                        for (const auto& w : rep.warnings) step.messages.push_back("Warning: " + w);
                        step.messages.push_back("Checked (read-only, nothing repaired): " + formatSize(rep.usedBytes) +
                                                " used of " + formatSize(rep.totalBytes));
                        warn = !rep.warnings.empty();
                        if (!rep.ok) {
                            ok = false;
                            err = "the file system has errors; repair it with chkdsk or fsck before changing it";
                        }
                    }
                    break;
                }
                default: break;
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
        // ... and the file system must be what the operation produced.
        if (ok && b && b->kind != PartitionKind::Extended) {
            RegionDevice part = region(*b);
            const FsDetection d = detectFilesystem(part);
            auto upper = [](std::string x) {
                for (char& c : x) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                return x;
            };
            std::string bad;
            if ((op.type == OpType::CreatePartition || op.type == OpType::Format) && d.type != op.fs) bad = "file system type";
            if (op.type == OpType::SetLabel && upper(d.label) != upper(op.label)) bad = "label";
            if (op.type == OpType::NewUuid && upper(d.uuid) != upper(newUuid)) bad = "UUID";
            if (op.type != OpType::CreatePartition && op.type != OpType::Format && op.type != OpType::Check &&
                fs != FsType::Unknown && d.type != fs)
                bad = "file system type";
            if (!bad.empty()) {
                ok = false;
                err = "verification after writing failed: the " + bad + " read back differs from the plan";
            } else if ((fsResize || (op.type == OpType::CreatePartition && op.fs != FsType::Unknown) ||
                        (op.type == OpType::Format && op.fs != FsType::Unknown)) &&
                       fstools::canCheck(d.type)) {
                fstools::CheckReport rep;  // new or resized file systems must pass the check
                if (!fstools::check(part, d.type, rep, err) || !rep.ok) {
                    ok = false;
                    err = "the file system check after writing failed" + (rep.errors.empty() ? ": " + err : ": " + rep.errors.front());
                } else {
                    step.messages.push_back("File system check after writing: no errors");
                }
            }
        }
        if (ok) {
            run.state.completed = i + 1;
            ok = run.phase("idle", i + 1, "", err);
        }
        step.endMs = nowMs();
        if (!ok) {
            step.status = LogStep::Status::Error;
            step.messages.push_back(err);
            for (std::size_t k = i + 1; k < r.log.children.size(); ++k) r.log.children[k].status = LogStep::Status::Skipped;
            journal.remove();  // the failure is reported here; a stale journal would only block the next attempt
            return finish(false, "Operation " + std::to_string(i + 1) + " failed: " + err + ". " +
                                     (r.completed ? std::to_string(r.completed) +
                                                        " operation(s) before it were applied; there is no automatic rollback."
                                                  : std::string("Nothing before it was applied.")));
        }
        step.status = warn ? LogStep::Status::Warning : LogStep::Status::Success;
        step.messages.push_back("Verified: the partition table read back matches the plan");
        ++r.completed;
    }
    journal.remove();
    return finish(true, "");
}

ExecuteResult newResult(const OperationQueue& plan, const std::string& deviceName) {
    ExecuteResult r;
    r.log.title = "Apply " + std::to_string(plan.operations().size()) + " operation(s) to " + deviceName;
    r.log.startMs = nowMs();
    for (const auto& op : plan.operations()) {
        LogStep s;
        s.title = describe(op, plan.original());
        s.status = LogStep::Status::Pending;
        r.log.children.push_back(s);
    }
    return r;
}

ExecuteResult failed(ExecuteResult r, const std::string& error) {
    for (auto& c : r.log.children)
        if (c.status == LogStep::Status::Pending) c.status = LogStep::Status::Skipped;
    r.ok = false;
    r.error = error;
    r.log.status = LogStep::Status::Error;
    r.log.messages.push_back(error);
    r.log.endMs = nowMs();
    return r;
}

}  // namespace

void addImageUsage(BlockReader& image, const DiskLayout& layout, ValidationContext& ctx) {
    for (const auto& p : layout.partitions) {
        if (p.kind == PartitionKind::Extended || p.fs.type == FsType::Unknown) continue;
        SubRangeReader part(image, p.firstLba * layout.sectorSize, p.sectorCount * layout.sectorSize);
        if (const std::uint64_t used = fstools::usedBytes(part, p.fs.type)) ctx.state[p.planId].usedBytes = used;
    }
}

ExecuteResult executePlanOnImage(BlockDevice& dev, const OperationQueue& plan, const std::string& deviceName,
                                 const ExecuteOptions& o) {
    ExecuteResult r = newResult(plan, deviceName);

    // 0. An interrupted execution must be resumed or discarded first.
    if (!o.journalPath.empty() && journalExists(o.journalPath))
        return failed(r, "An interrupted execution was found for this image (" + o.journalPath +
                             "). Resume it or discard the journal first; nothing was written.");

    // 1. The device must still hold exactly the layout the plan was made for.
    // The reads are recorded for the journal.
    RecordingReader rec(dev);
    const DiskLayout now = parseDiskLayout(rec, true);
    std::string why;
    if (!sameTable(now, plan.original(), why)) return failed(r, "The image changed since planning: " + why + ".");

    // 2. Validate against the image backend.
    ValidationContext ctx;
    ctx.mode = ValidationMode::ImageExecution;
    ctx.haveCurrentIdentity = true;
    ctx.currentIdentity = identityOf(now, plan.identity().serial, plan.identity().model);
    addImageUsage(dev, plan.original(), ctx);
    r.validation = validatePlan(plan, ctx);
    for (const auto& d : r.validation.diagnostics)
        if (d.severity != Severity::Info) r.log.messages.push_back(d.message);
    if (!r.validation.ok()) {
        std::string first;
        for (const auto& d : r.validation.diagnostics)
            if (d.severity == Severity::Error && first.empty()) first = d.message;
        return failed(r, "The plan did not pass validation; nothing was written. " + first);
    }

    // 2b. File systems that cannot be shrunk as planned (e.g. NTFS data behind
    // the new end) are refused now, before the first write.
    {
        DiskLayout sim = plan.original();
        std::uint32_t simNext = static_cast<std::uint32_t>(sim.partitions.size()) + 1;
        std::set<std::uint32_t> changed;  // partitions whose content an earlier step changed
        std::string simErr;
        for (std::size_t i = 0; i < plan.operations().size(); ++i) {
            Operation op = plan.operations()[i];
            const PartitionEntry* t = findPlanned(sim, op.target);
            if (op.type == OpType::ResizeMove && t && !changed.count(op.target) && op.sectorCount < t->sectorCount &&
                fstools::canResize(t->fs.type)) {
                SubRangeReader part(dev, t->firstLba * sim.sectorSize, t->sectorCount * sim.sectorSize);
                const std::string problem = fstools::shrinkProblem(part, t->fs.type, op.sectorCount * sim.sectorSize);
                if (!problem.empty())
                    return failed(r, "Operation " + std::to_string(i + 1) + " cannot be executed: " + problem +
                                         ". Nothing was written.");
            }
            if (op.target) changed.insert(op.target);
            if (!simulate(op, sim, simNext, simErr)) break;
        }
    }

    // 3. Journal, then execute step by step on a copy of the planned original.
    Journal journal;
    if (!o.journalPath.empty()) {
        JournalState s;
        s.phase = "idle";
        std::string err;
        if (!journal.create(o.journalPath, journalBody(plan, rec), s, err)) return failed(r, err + "; nothing was written.");
    }
    return runPlan(dev, plan, deviceName, o, StartPoint{}, journal, r);
}

ExecuteResult resumePlanOnImage(BlockDevice& dev, const std::string& deviceName, const ExecuteOptions& o) {
    ExecuteResult empty;
    empty.log.title = "Resume an interrupted execution on " + deviceName;
    empty.log.startMs = nowMs();
    JournalState s;
    JsonValue body;
    std::string err;
    if (o.journalPath.empty() || !readJournalFile(o.journalPath, s, body, err)) return failed(empty, err.empty() ? "no journal" : err);
    if (!s.resumable())
        return failed(empty, "Operation " + std::to_string(s.operation + 1) + " was interrupted while " + s.step +
                                 ". That step changes data in place and cannot be resumed; the image may be "
                                 "inconsistent. Restore it from a backup.");
    if (s.imageBytes != dev.size() || s.sectorSize != dev.sectorSize())
        return failed(empty, "The journal belongs to an image of a different size or sector size.");

    // Rebuild the plan exactly as it was: replay the recorded layout detection.
    ReplayReader replay(s.imageBytes, s.sectorSize);
    if (const JsonValue* snap = body.find("snapshot"))
        for (const auto& p : snap->array) {
            std::vector<std::uint8_t> data;
            if (!fromHex(p.getString("hex"), data)) return failed(empty, "the execution journal is damaged");
            replay.pieces[p.getUint("offset")] = std::move(data);
        }
    if (const JsonValue* bad = body.find("failed"))
        for (const auto& f : bad->array) replay.failed.insert({f.getUint("offset"), static_cast<std::size_t>(f.getUint("length"))});
    OperationQueue plan;
    if (!OperationQueue::fromJson(s.planJson, parseDiskLayout(replay, true), plan, err))
        return failed(empty, "the plan in the journal cannot be rebuilt: " + err);
    ExecuteResult r = newResult(plan, deviceName);
    if (s.completed > plan.operations().size() || (s.phase == "copy" && s.operation != s.completed))
        return failed(r, "the execution journal is inconsistent");

    // The image must hold the table after the completed operations (the
    // interrupted copy has not written its table yet).
    DiskLayout expected = plan.original();
    std::uint32_t next = static_cast<std::uint32_t>(expected.partitions.size()) + 1;
    for (std::size_t i = 0; i < s.completed; ++i) {
        Operation op = plan.operations()[i];
        if (!simulate(op, expected, next, err)) return failed(r, "the plan in the journal cannot be replayed: " + err);
    }
    const DiskLayout now = parseDiskLayout(dev, false);
    std::string why;
    if (!sameTable(now, expected, why))
        return failed(r, "The image does not match the journal (" + why + "); it was changed after the interruption.");

    // Structural validation again (the usage check ran before the first write).
    ValidationContext ctx;
    ctx.mode = ValidationMode::ImageExecution;
    ctx.haveCurrentIdentity = true;
    ctx.currentIdentity = identityOf(plan.original(), plan.identity().serial, plan.identity().model);
    r.validation = validatePlan(plan, ctx);
    if (!r.validation.ok()) return failed(r, "The plan in the journal does not pass validation any more.");

    Journal journal;
    if (!journal.reopen(o.journalPath, err)) return failed(r, err);
    StartPoint start;
    start.completed = s.completed;
    start.inCopy = s.phase == "copy";
    start.copyDone = s.copyDone;
    r.log.title = "Resume " + std::to_string(plan.operations().size()) + " operation(s) on " + deviceName;
    return runPlan(dev, plan, deviceName, o, start, journal, r);
}

}  // namespace wparted
