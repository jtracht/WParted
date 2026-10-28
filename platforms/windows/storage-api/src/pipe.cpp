// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/win/pipe.hpp"

#include <windows.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <cwctype>
#include <vector>

#include "wparted/core/helper_protocol.hpp"
#include "wparted/core/partition_table.hpp"
#include "wparted/core/types.hpp"

namespace wparted::win {
namespace {

std::string lastError(const char* what) { return std::string(what) + " failed (" + std::to_string(GetLastError()) + ")"; }

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring imagePath(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    const bool ok = QueryFullProcessImageNameW(p, 0, buf, &n) != 0;
    CloseHandle(p);
    return ok ? std::wstring(buf, n) : std::wstring();
}

DWORD parentPid() {
    const DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e{sizeof e};
    DWORD parent = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (e.th32ProcessID == self) parent = e.th32ParentProcessID;
    CloseHandle(snap);
    return parent;
}

// "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;<user>)": only SYSTEM, Administrators and the current user.
bool pipeSecurity(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd, std::string& error) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error = lastError("OpenProcessToken");
        return false;
    }
    std::vector<BYTE> buf(256);
    DWORD need = 0;
    if (!GetTokenInformation(token, TokenUser, buf.data(), static_cast<DWORD>(buf.size()), &need)) {
        buf.resize(need);
        if (!GetTokenInformation(token, TokenUser, buf.data(), static_cast<DWORD>(buf.size()), &need)) {
            CloseHandle(token);
            error = lastError("GetTokenInformation");
            return false;
        }
    }
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
        error = lastError("ConvertSidToStringSid");
        return false;
    }
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;") + sid + L")";
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        error = lastError("ConvertStringSecurityDescriptorToSecurityDescriptor");
        return false;
    }
    sa = SECURITY_ATTRIBUTES{sizeof sa, sd, FALSE};
    return true;
}

bool waitIo(HANDLE h, OVERLAPPED& ov, DWORD timeoutMs, DWORD& bytes) {
    if (WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0) {
        CancelIo(h);
        GetOverlappedResult(h, &ov, &bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(h, &ov, &bytes, FALSE) != 0;
}

}  // namespace

Pipe::~Pipe() { close(); }

void Pipe::close() {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
        if (server_) DisconnectNamedPipe(handle_);
        CloseHandle(handle_);
    }
    handle_ = nullptr;
}

bool Pipe::create(const std::wstring& name, std::string& error) {
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!pipeSecurity(sa, sd, error)) return false;
    handle_ = CreateNamedPipeW(name.c_str(),
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                               static_cast<DWORD>(helper::kMaxMessage), static_cast<DWORD>(helper::kMaxMessage), 0, &sa);
    LocalFree(sd);
    server_ = true;
    if (handle_ == INVALID_HANDLE_VALUE) {
        handle_ = nullptr;
        error = lastError("CreateNamedPipe");
        return false;
    }
    return true;
}

bool Pipe::accept(unsigned long expectedClientPid, unsigned long timeoutMs, std::string& error) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = ConnectNamedPipe(handle_, &ov) != 0;
    if (!connected) {
        const DWORD e = GetLastError();
        DWORD bytes = 0;
        connected = e == ERROR_PIPE_CONNECTED || (e == ERROR_IO_PENDING && waitIo(handle_, ov, timeoutMs, bytes));
    }
    CloseHandle(ov.hEvent);
    if (!connected) {
        error = "the helper did not connect in time";
        return false;
    }
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(handle_, &pid) || pid != expectedClientPid) {
        error = "an unexpected process connected to the helper pipe";
        close();
        return false;
    }
    return true;
}

bool Pipe::open(const std::wstring& name, std::string& error) {
    if (name.rfind(L"\\\\.\\pipe\\WParted-", 0) != 0) {
        error = "not a WParted pipe name";
        return false;
    }
    handle_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,  // readonly-guard: allow (named pipe to WParted, not a device)
                          FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        handle_ = nullptr;
        error = lastError("opening the pipe");
        return false;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(handle_, &mode, nullptr, nullptr)) {
        error = lastError("SetNamedPipeHandleState");
        return false;
    }
    return true;
}

bool Pipe::verifyServerIsParent(std::string& error) {
    ULONG server = 0;
    if (!GetNamedPipeServerProcessId(handle_, &server) || server != parentPid()) {
        error = "the pipe server is not the process that started the helper";
        return false;
    }
    const std::wstring path = lower(imagePath(server)), dir = lower(ownDirectory());
    if (path != dir + L"wparted.exe" && path != dir + L"wparted-cli.exe") {
        error = "the pipe server is not wparted.exe or wparted-cli.exe next to the helper";
        return false;
    }
    return true;
}

bool Pipe::send(const std::string& message, std::string& error) {
    if (message.size() > helper::kMaxMessage) {
        error = "message too large";
        return false;
    }
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    bool ok = WriteFile(handle_, message.data(), static_cast<DWORD>(message.size()), &written, &ov) != 0;  // readonly-guard: allow (named pipe)
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = waitIo(handle_, ov, 30000, written);
    CloseHandle(ov.hEvent);
    if (!ok || written != message.size()) {
        error = lastError("writing to the pipe");
        return false;
    }
    return true;
}

bool Pipe::pending() {
    DWORD avail = 0, left = 0;
    return handle_ && PeekNamedPipe(handle_, nullptr, 0, nullptr, &avail, &left) && left + avail > 0;
}

bool Pipe::receive(std::string& message, unsigned long timeoutMs, std::string& error) {
    message.assign(helper::kMaxMessage, '\0');
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD got = 0;
    bool ok = ReadFile(handle_, &message[0], static_cast<DWORD>(message.size()), &got, &ov) != 0;
    if (!ok) {
        const DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) ok = waitIo(handle_, ov, timeoutMs, got);
        else if (e == ERROR_MORE_DATA) error = "message too large";
    }
    CloseHandle(ov.hEvent);
    if (!ok) {
        if (error.empty()) error = "no message from the other side (closed or timed out)";
        message.clear();
        return false;
    }
    message.resize(got);
    return true;
}

std::wstring randomPipeName() {
    const Guid a = randomGuid(), b = randomGuid();
    std::wstring name = L"\\\\.\\pipe\\WParted-";
    static const wchar_t* hex = L"0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        name += hex[a.bytes[i] >> 4];
        name += hex[a.bytes[i] & 15];
        name += hex[b.bytes[i] >> 4];
        name += hex[b.bytes[i] & 15];
    }
    return name;
}

std::wstring ownDirectory() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring p(buf, n);
    const std::size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
}

}  // namespace wparted::win
