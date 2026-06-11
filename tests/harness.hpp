// Sententia - minimal test harness.
//
// Deliberately dependency-free. The build has to be hermetic and
// offline-reproducible: a test framework fetched at configure time is a
// network dependency in CI and a source of version drift, and this
// project's whole thesis is reproducibility. Roughly sixty lines buys
// everything the matching tests actually need.
//
// Each test file is its own executable registered with ctest, so a
// crash or an assertion failure isolates to one named test.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

namespace test {

inline int g_failures = 0;
inline int g_checks = 0;

inline void report(bool ok, const char* expr, const char* file, int line,
                   const std::string& extra) {
    ++g_checks;
    if (ok) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    if (!extra.empty()) {
        std::fprintf(stderr, "     %s\n", extra.c_str());
    }
}

// Renders operands when they are printable as numbers, and stays quiet
// otherwise, so CHECK_EQ works on enums and structs without needing an
// overload for every type in the domain model.
template <typename A, typename B>
inline std::string describe(const A& a, const B& b) {
    if constexpr (std::is_arithmetic_v<A> && std::is_arithmetic_v<B>) {
        return "left=" + std::to_string(a) + " right=" + std::to_string(b);
    } else {
        (void)a;
        (void)b;
        return {};
    }
}

inline int finish(const char* name) {
    if (g_failures == 0) {
        std::printf("PASS %s (%d checks)\n", name, g_checks);
        return 0;
    }
    std::fprintf(stderr, "FAILED %s: %d of %d checks failed\n", name, g_failures, g_checks);
    return 1;
}

}  // namespace test

#define CHECK(expr) ::test::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__, "")

#define CHECK_EQ(a, b)                                                 \
    do {                                                               \
        const auto lhs_ = (a);                                         \
        const auto rhs_ = (b);                                         \
        ::test::report(lhs_ == rhs_, #a " == " #b, __FILE__, __LINE__, \
                       ::test::describe(lhs_, rhs_));                  \
    } while (0)

#define CHECK_STR_EQ(a, b)                                              \
    do {                                                                \
        const std::string lhs_ = (a);                                   \
        const std::string rhs_ = (b);                                   \
        ::test::report(lhs_ == rhs_, #a " == " #b, __FILE__, __LINE__,  \
                       "left=\"" + lhs_ + "\" right=\"" + rhs_ + "\""); \
    } while (0)

#define TEST_MAIN(name)              \
    int main() {                     \
        run();                       \
        return ::test::finish(name); \
    }
