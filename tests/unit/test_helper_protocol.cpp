// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Helper protocol (ADR-0002, WP-023): strict decoding and the plan hash.
#include "test_framework.hpp"
#include "wparted/core/helper_protocol.hpp"

using namespace wparted;
using namespace wparted::helper;

TEST("helper protocol: SHA-256 matches the FIPS 180-4 test vectors") {
    CHECK_EQ(sha256Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(sha256Hex(std::string(1000000, 'a')),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST("helper protocol: every message type survives encode and decode") {
    for (Type t : {Type::Hello, Type::Plan, Type::Validation, Type::Confirm, Type::Progress, Type::Cancel, Type::Log, Type::Done}) {
        Message m;
        m.type = t;
        m.protocol = kProtocol;
        m.version = "0.4.0-alpha";
        m.plan = "{\"schema\":\"wparted-plan\"}";
        m.image = "C:\\images\\disk.img";
        m.sectorSize = 4096;
        m.ok = true;
        m.diagnostics.emplace_back(Severity::Warning, "op.bcd_offset", "text");
        m.planHash = sha256Hex(m.plan);
        m.step = 2;
        m.done = 3;
        m.total = 4;
        m.log = "{}";
        m.result = "success";
        Message back;
        std::string err;
        REQUIRE(decode(encode(m), back, err));
        CHECK(back.type == t);
        if (t == Type::Plan) {
            CHECK_EQ(back.plan, m.plan);
            CHECK_EQ(back.image, m.image);
            CHECK_EQ(back.sectorSize, 4096u);
        }
        if (t == Type::Validation) {
            CHECK(back.ok);
            REQUIRE(back.diagnostics.size() == 1);
            CHECK_EQ(back.diagnostics[0].code, std::string("op.bcd_offset"));
            CHECK(back.diagnostics[0].severity == Severity::Warning);
            CHECK_EQ(back.planHash, m.planHash);
        }
        if (t == Type::Progress) CHECK(back.step == 2 && back.done == 3 && back.total == 4);
        if (t == Type::Done) CHECK_EQ(back.result, std::string("success"));
    }
}

TEST("helper protocol: unknown types and fields, wrong types and oversized messages are rejected") {
    Message m;
    std::string err;
    CHECK(!decode("{\"type\":\"run\",\"command\":\"cmd.exe\"}", m, err));             // no generic commands
    CHECK(!decode("{\"type\":\"cancel\",\"command\":\"x\"}", m, err));               // unknown field
    CHECK(!decode("{\"type\":\"confirm\"}", m, err));                                // missing field
    CHECK(!decode("{\"type\":\"confirm\",\"planHash\":5}", m, err));                 // wrong type
    CHECK(!decode("{\"type\":\"done\",\"result\":\"maybe\",\"error\":\"\"}", m, err));  // unknown result
    CHECK(!decode("[1,2]", m, err));
    CHECK(!decode("{\"type\":\"log\",\"log\":\"" + std::string(kMaxMessage, 'x') + "\"}", m, err));
    CHECK(decode("{\"type\":\"cancel\"}", m, err));
    CHECK(m.type == Type::Cancel);
}
