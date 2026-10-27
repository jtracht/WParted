// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Writes a synthetic GPT disk image (ESP + MSR + NTFS + ext4) to the given
// path for end-to-end tests of wparted-cli. Never touches a device.
#include <cstdio>

#include "image_builder.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: make_test_image <output.img>\n");
        return 2;
    }
    wptest::Image im(64ull * 1024 * 1024, 512);
    im.gpt({{"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", 2048, 34815, "EFI system partition", 0},
            {"E3C9E316-0B5C-4DB8-817D-F92DF00215AE", 34816, 67583, "Microsoft reserved partition", 0},
            {"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", 67584, 100351, "Basic data partition", 0},
            {"0FC63DAF-8483-4772-8E79-3D69D8477DE4", 100352, 131038, "linux", 0}});
    wptest::writeFat32(im, 2048ull * 512, "ESP        ");
    wptest::writeNtfs(im, 67584ull * 512);
    wptest::writeExt4(im, 100352ull * 512, "rootfs");
    std::FILE* f = nullptr;
    if (fopen_s(&f, argv[1], "wb") != 0 || !f) return 1;
    bool ok = std::fwrite(im.data.data(), 1, im.data.size(), f) == im.data.size();
    std::fclose(f);
    return ok ? 0 : 1;
}
