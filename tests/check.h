// Minimal test harness for skaar_tests: TEST() registers a case, CHECK()
// records a failure and keeps going. No dependency (docs/deps.md unchanged).
//
// This file is part of Skaar. GPLv2 or later.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& Cases() {
    static std::vector<Case> cases;
    return cases;
}

inline int& Failures() {
    static int failures = 0;
    return failures;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { Cases().push_back({name, std::move(fn)}); }
};

inline void Fail(const char* file, int line, const std::string& what) {
    std::printf("  FAIL %s:%d: %s\n", file, line, what.c_str());
    Failures()++;
}

}  // namespace check

#define CHECK_CONCAT2(a, b) a##b
#define CHECK_CONCAT(a, b) CHECK_CONCAT2(a, b)
#define TEST(name)                                                                       \
    static void CHECK_CONCAT(test_, __LINE__)();                                         \
    static check::Register CHECK_CONCAT(reg_, __LINE__)(name, CHECK_CONCAT(test_, __LINE__)); \
    static void CHECK_CONCAT(test_, __LINE__)()

// variadic so braced initializers (commas) work inside
#define CHECK(...)                                          \
    do {                                                    \
        if (!(__VA_ARGS__)) {                               \
            check::Fail(__FILE__, __LINE__, #__VA_ARGS__);  \
        }                                                   \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                              \
    do {                                                                                                   \
        const double check_a = (a), check_b = (b);                                                         \
        /* negated <= so a NaN on either side fails: NaN > eps is false, NaN <= eps is false too */         \
        if (!(std::fabs(check_a - check_b) <= (eps))) {                                                     \
            check::Fail(__FILE__, __LINE__,                                                                \
                        std::string(#a " ~= " #b ": ") + std::to_string(check_a) + " vs " + std::to_string(check_b)); \
        }                                                                                                  \
    } while (0)
