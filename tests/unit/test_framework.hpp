// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The WParted contributors
//
// Tiny dependency-free test harness. Each TEST registers itself; CHECK
// records a failure and continues, REQUIRE aborts the current test.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace wptest {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct AbortTest {};

inline int& failures() {
    static int f = 0;
    return f;
}

inline void fail(const char* file, int line, const std::string& expr) {
    ++failures();
    std::printf("    FAIL %s:%d: %s\n", file, line, expr.c_str());
}

}  // namespace wptest

#define WP_CAT2(a, b) a##b
#define WP_CAT(a, b) WP_CAT2(a, b)
#define TEST(name)                                                              \
    static void WP_CAT(test_fn_, __LINE__)();                                   \
    static ::wptest::Registrar WP_CAT(test_reg_, __LINE__)(name, &WP_CAT(test_fn_, __LINE__)); \
    static void WP_CAT(test_fn_, __LINE__)()

#define CHECK(expr) \
    do { if (!(expr)) ::wptest::fail(__FILE__, __LINE__, #expr); } while (0)
#define REQUIRE(expr) \
    do { if (!(expr)) { ::wptest::fail(__FILE__, __LINE__, #expr); throw ::wptest::AbortTest{}; } } while (0)
#define CHECK_EQ(a, b) \
    do { if (!((a) == (b))) ::wptest::fail(__FILE__, __LINE__, std::string(#a " == " #b)); } while (0)
