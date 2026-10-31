// skaar_tests: unit tests for the engine-free game logic in game/shared.
//
// This file is part of Skaar. GPLv2 or later.
#include <exception>
#include <string>

#include "check.h"

int main() {
    for (const check::Case& c : check::Cases()) {
        const int before = check::Failures();
        // print first and guard the call: a throwing case (directory_iterator on
        // a missing data dir, per.at() after a regression) must not take the
        // results of every later case down with it
        std::printf("-- %s\n", c.name);
        try {
            c.fn();
        } catch (const std::exception& e) {
            check::Fail(__FILE__, __LINE__, std::string(c.name) + " threw: " + e.what());
        }
        std::printf("%s %s\n", check::Failures() == before ? "ok  " : "FAIL", c.name);
    }
    std::printf("%zu tests, %d failed checks\n", check::Cases().size(), check::Failures());
    return check::Failures() ? 1 : 0;
}
