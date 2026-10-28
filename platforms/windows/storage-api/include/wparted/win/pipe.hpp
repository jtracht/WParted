// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Named-pipe channel between WParted and wparted-helper.exe (ADR-0002):
// message mode, one instance, remote clients rejected, a DACL for the current
// user, SYSTEM and Administrators only. Each side checks the other process:
// the server accepts only the process it started, the helper accepts only its
// parent, and only if that is wparted.exe or wparted-cli.exe next to it.
#pragma once

#include <cstdint>
#include <string>

namespace wparted::win {

class Pipe {
public:
    Pipe() = default;
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    ~Pipe();

    // Server side (WParted).
    bool create(const std::wstring& name, std::string& error);
    bool accept(unsigned long expectedClientPid, unsigned long timeoutMs, std::string& error);
    // Client side (helper).
    bool open(const std::wstring& name, std::string& error);
    bool verifyServerIsParent(std::string& error);

    bool send(const std::string& message, std::string& error);
    // Waits up to timeoutMs for one message (bounded by helper::kMaxMessage).
    bool receive(std::string& message, unsigned long timeoutMs, std::string& error);
    // True if a complete message is waiting (does not block).
    bool pending();
    void close();

private:
    void* handle_ = nullptr;
    bool server_ = false;
};

// \\.\pipe\WParted-<128-bit random hex>
std::wstring randomPipeName();
// Full path of the running executable's directory, with a trailing backslash.
std::wstring ownDirectory();

}  // namespace wparted::win
