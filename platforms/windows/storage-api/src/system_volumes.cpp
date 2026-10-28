// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Marks volumes that matter for booting and for BitLocker (WP-071). Read-only
// throughout: two registry values and one WMI query.
//   - boot manager: HKLM\SYSTEM\Setup\SystemPartition names the device of the
//     partition Windows booted from (EFI system partition or "System Reserved");
//   - page file:    HKLM\...\Memory Management\PagingFiles;
//   - BitLocker:    Win32_EncryptableVolume (root\CIMV2\Security\
//     MicrosoftVolumeEncryption). Windows answers administrators only; for
//     everyone else the state stays NotReported.
#include "system_volumes.hpp"

#include <windows.h>
#include <wbemidl.h>

#include <cstdio>
#include <cwctype>
#include <string>

#include "wparted/core/log.hpp"

namespace wparted::win {

namespace {

// From wbemcli.h; defined here so that no import library is needed.
const CLSID kClsidWbemLocator = {0x4590f811, 0x1d3a, 0x11d0, {0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
const IID kIidWbemLocator = {0xdc12a687, 0x737f, 0x11cf, {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value, DWORD flags) {
    DWORD bytes = 0;
    if (RegGetValueW(root, key, value, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0) return {};
    std::wstring s(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, value, flags, nullptr, &s[0], &bytes) != ERROR_SUCCESS) return {};
    s.resize(bytes / sizeof(wchar_t));
    while (!s.empty() && s.back() == L'\0') s.pop_back();  // the size includes the terminator(s)
    return s;
}

// "\\?\Volume{...}\" -> "\Device\HarddiskVolumeN"
std::wstring ntDeviceOf(const std::wstring& guidPath) {
    if (guidPath.size() < 6 || guidPath.compare(0, 4, L"\\\\?\\") != 0) return {};
    std::wstring name = guidPath.substr(4);
    if (!name.empty() && name.back() == L'\\') name.pop_back();
    wchar_t target[MAX_PATH] = L"";
    return QueryDosDeviceW(name.c_str(), target, MAX_PATH) ? std::wstring(target) : std::wstring();
}

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

bool uintProperty(IWbemClassObject* o, const wchar_t* name, unsigned& out) {
    VARIANT v;
    VariantInit(&v);
    const bool ok = SUCCEEDED(o->Get(name, 0, &v, nullptr, nullptr)) && (v.vt == VT_I4 || v.vt == VT_UI4);
    if (ok) out = v.vt == VT_I4 ? static_cast<unsigned>(v.lVal) : v.ulVal;
    VariantClear(&v);
    return ok;
}

BitLockerState stateOf(bool haveProtection, unsigned protection, bool haveConversion, unsigned conversion) {
    if (haveConversion && conversion >= 2 && conversion <= 5) return BitLockerState::Converting;
    if (haveProtection && protection == 1) return BitLockerState::ProtectionOn;
    if (haveProtection && protection == 2) return BitLockerState::Locked;  // "unknown" = volume locked
    if (haveProtection && protection == 0 && haveConversion)
        return conversion == 0 ? BitLockerState::NotEncrypted : BitLockerState::ProtectionOff;
    return BitLockerState::NotReported;  // Windows 7 has no ConversionStatus property
}

void queryBitLocker(std::vector<VolumeInfo>& volumes) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);  // RPC_E_CHANGED_MODE: COM is already set up on this thread
    {
        ComPtr<IWbemLocator> locator;
        ComPtr<IWbemServices> services;
        ComPtr<IEnumWbemClassObject> rows;
        BSTR ns = SysAllocString(L"ROOT\\CIMV2\\Security\\MicrosoftVolumeEncryption");
        BSTR wql = SysAllocString(L"WQL");
        BSTR query = SysAllocString(L"SELECT DeviceID, ProtectionStatus, ConversionStatus FROM Win32_EncryptableVolume");
        HRESULT hr = CoCreateInstance(kClsidWbemLocator, nullptr, CLSCTX_INPROC_SERVER, kIidWbemLocator,
                                      reinterpret_cast<void**>(&locator));
        if (SUCCEEDED(hr)) hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
        if (SUCCEEDED(hr))
            hr = CoSetProxyBlanket(services.p, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                                   RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        if (SUCCEEDED(hr))
            hr = services->ExecQuery(wql, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &rows);
        int reported = 0;
        while (SUCCEEDED(hr) && rows.p) {
            ComPtr<IWbemClassObject> row;
            ULONG n = 0;
            if (rows->Next(WBEM_INFINITE, 1, &row, &n) != WBEM_S_NO_ERROR || n == 0) break;
            VARIANT id;
            VariantInit(&id);
            if (SUCCEEDED(row->Get(L"DeviceID", 0, &id, nullptr, nullptr)) && id.vt == VT_BSTR && id.bstrVal) {
                unsigned protection = 0, conversion = 0;
                const bool hp = uintProperty(row.p, L"ProtectionStatus", protection);
                const bool hc = uintProperty(row.p, L"ConversionStatus", conversion);
                const std::wstring device = lower(id.bstrVal);
                for (auto& v : volumes)
                    if (lower(v.guidPath) == device) {
                        v.bitLocker = stateOf(hp, protection, hc, conversion);
                        ++reported;
                    }
            }
            VariantClear(&id);
        }
        if (FAILED(hr))
            log::info("BitLocker state not available (WMI 0x" + [&] {
                char b[16];
                std::snprintf(b, sizeof b, "%08lx", static_cast<unsigned long>(hr));
                return std::string(b);
            }() + "; administrators only)");
        else
            log::info("BitLocker state reported for " + std::to_string(reported) + " volume(s)");
        SysFreeString(ns);
        SysFreeString(wql);
        SysFreeString(query);
    }
    if (uninit) CoUninitialize();
}

}  // namespace

void annotateSystemVolumes(std::vector<VolumeInfo>& volumes) {
    // Boot manager.
    const std::wstring system =
        lower(regString(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", L"SystemPartition", RRF_RT_REG_SZ));
    // Page files: "C:\pagefile.sys 0 0" or "?:\pagefile.sys" (system managed, on the Windows volume).
    const std::wstring paging = regString(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                                          L"PagingFiles", RRF_RT_REG_MULTI_SZ);
    for (auto& v : volumes) {
        if (!system.empty() && lower(ntDeviceOf(v.guidPath)) == system) v.isSystemPartition = true;
        for (std::size_t at = 0; at < paging.size();) {
            const std::size_t end = paging.find(L'\0', at);
            const std::wstring entry = paging.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
            if (entry.size() >= 2 && entry[1] == L':') {
                for (const auto& m : v.mountPoints)
                    if (m.size() >= 2 && std::towlower(m[0]) == std::towlower(entry[0]) && m[1] == L':')
                        v.isPageFileVolume = true;
                if (entry[0] == L'?' && v.containsWindows) v.isPageFileVolume = true;
            }
            if (end == std::wstring::npos) break;
            at = end + 1;
        }
    }
    queryBitLocker(volumes);
}

}  // namespace wparted::win
