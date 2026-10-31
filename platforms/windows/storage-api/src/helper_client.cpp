// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
#include "wparted/win/helper_client.hpp"

#include <windows.h>

#include "wparted/core/helper_protocol.hpp"
#include "wparted/win/pipe.hpp"
#include "wparted_storage_version.h"

namespace wparted::win {

bool helperAvailable() { return GetFileAttributesW((ownDirectory() + L"wparted-helper.exe").c_str()) != INVALID_FILE_ATTRIBUTES; }

HelperRun applyViaHelper(const std::string& image, const std::string& planText, std::uint32_t sectorSize,
                         const HelperCallbacks& cb) {
    HelperRun run;
    const std::wstring helperPath = ownDirectory() + L"wparted-helper.exe";
    const std::wstring pipeName = randomPipeName();
    Pipe pipe;
    if (!pipe.create(pipeName, run.error)) return run;
    std::wstring cmd = L"\"" + helperPath + L"\" --pipe " + pipeName;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(helperPath.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        run.error = "wparted-helper.exe cannot be started (" + std::to_string(GetLastError()) + ")";
        return run;
    }
    CloseHandle(pi.hThread);
    auto finish = [&]() {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        return run;
    };
    std::string err;
    auto send = [&](const helper::Message& m) { return pipe.send(helper::encode(m), err); };
    auto receive = [&](helper::Message& m, unsigned long timeout) {
        std::string text;
        return pipe.receive(text, timeout, err) && helper::decode(text, m, err);
    };
    if (!pipe.accept(pi.dwProcessId, 10000, run.error)) return finish();
    helper::Message hello;
    hello.type = helper::Type::Hello;
    hello.protocol = helper::kProtocol;
    hello.version = WPARTED_STORAGE_VERSION;
    helper::Message m;
    if (!send(hello) || !receive(m, 10000) || m.type != helper::Type::Hello || m.protocol != helper::kProtocol) {
        run.error = "the helper handshake failed " + err;
        return finish();
    }
    run.started = true;
    run.helperVersion = m.version;
    helper::Message plan;
    plan.type = helper::Type::Plan;
    plan.plan = planText;
    plan.image = image;
    plan.sectorSize = sectorSize;
    if (!send(plan) || !receive(m, 60000)) {
        run.error = err;
        return finish();
    }
    if (m.type == helper::Type::Done) {
        run.result = m.result;
        run.error = m.error;
        return finish();
    }
    if (m.type != helper::Type::Validation) {
        run.error = "unexpected message from the helper";
        return finish();
    }
    run.validationOk = m.ok;
    run.diagnostics = m.diagnostics;
    const std::string expected = helper::sha256Hex(planText);
    if (m.planHash != expected) {  // the helper validated something else: never confirm
        helper::Message c;
        c.type = helper::Type::Cancel;
        send(c);
        run.error = "the helper validated a different plan; nothing was confirmed";
        return finish();
    }
    run.planHash = expected;
    if (!m.ok) {
        if (receive(m, 10000) && m.type == helper::Type::Done) {
            run.result = m.result;
            run.error = m.error;
        }
        return finish();
    }
    helper::Message confirm;
    confirm.type = helper::Type::Confirm;
    confirm.planHash = expected;
    if (!send(confirm)) {
        run.error = err;
        return finish();
    }
    bool cancelSent = false;
    while (receive(m, 10 * 60 * 1000)) {
        if (m.type == helper::Type::Progress) {
            if (cb.progress) cb.progress(m.step, m.done, m.total);
            if (cb.cancel && cb.cancel->load() && !cancelSent) {
                helper::Message c;
                c.type = helper::Type::Cancel;
                cancelSent = send(c);
            }
        } else if (m.type == helper::Type::Log) {
            run.logJson = m.log;
        } else if (m.type == helper::Type::Done) {
            run.result = m.result;
            run.error = m.error;
            break;
        }
    }
    if (run.result.empty() && run.error.empty()) run.error = "the helper stopped without a result: " + err;
    return finish();
}

}  // namespace wparted::win
