#pragma once

// Tiny test helper: no framework, plays well with ctest.

#include <cmath>
#include <cstdio>
#include <cstdlib>

inline int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                    \
    do {                                                                         \
        const double _a = (a), _b = (b);                                         \
        if (std::fabs(_a - _b) > (tol)) {                                        \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %g, expected %g (+/- %g)\n", \
                         __FILE__, __LINE__, #a, _a, _b, static_cast<double>(tol)); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

inline int finish(const char* name) {
    if (g_failures == 0) {
        std::printf("%s: all checks passed\n", name);
        return 0;
    }
    std::printf("%s: %d check(s) failed\n", name, g_failures);
    return 1;
}
