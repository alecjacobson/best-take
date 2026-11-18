// Tiny test harness: TEST(name) { ... } with CHECK macros.
#pragma once
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

struct TestCase {
    std::string name;
    std::function<void()> fn;
};
inline std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> r;
    return r;
}
inline int& test_failures() {
    static int n = 0;
    return n;
}
struct TestRegistrar {
    TestRegistrar(const char* name, std::function<void()> fn) { test_registry().push_back({name, std::move(fn)}); }
};

#define TEST(name)                                         \
    static void test_##name();                             \
    static TestRegistrar registrar_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            ++test_failures();                                                             \
            std::cerr << "  " << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
        }                                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                      \
    do {                                                                                           \
        double va = (a), vb = (b);                                                                 \
        if (!(std::abs(va - vb) <= (tol))) {                                                       \
            ++test_failures();                                                                     \
            std::cerr << "  " << __FILE__ << ":" << __LINE__ << ": CHECK_NEAR failed: " #a " = " << va \
                      << ", " #b " = " << vb << ", tol " << (tol) << "\n";                         \
        }                                                                                          \
    } while (0)

#define CHECK_THROWS(expr)                                                                   \
    do {                                                                                     \
        bool threw = false;                                                                  \
        try {                                                                                \
            (void)(expr);                                                                    \
        } catch (const std::invalid_argument&) {                                             \
            threw = true;                                                                    \
        }                                                                                    \
        if (!threw) {                                                                        \
            ++test_failures();                                                               \
            std::cerr << "  " << __FILE__ << ":" << __LINE__ << ": expected std::invalid_argument: " #expr "\n"; \
        }                                                                                    \
    } while (0)
