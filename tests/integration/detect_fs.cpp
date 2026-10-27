// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Prints "<type>\t<label>\t<uuid>" for the file system at the start of an
// image file. Used by tools/ci/mkfs-fixtures.sh to compare WParted's
// detection with images made by the real mkfs tools. Opens the file read-only.
#include <cstdio>

#include "wparted/core/block_reader.hpp"
#include "wparted/core/filesystem.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: detect_fs <image>\n");
        return 2;
    }
    wparted::FileBlockReader reader;
    if (!reader.open(argv[1], 512)) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    const auto d = wparted::detectFilesystem(reader);
    std::printf("%s\t%s\t%s\n", wparted::fsTypeName(d.type), d.label.c_str(), d.uuid.c_str());
    return 0;
}
