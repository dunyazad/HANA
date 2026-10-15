#pragma once

#include <functional>
#include <utility>
#include <string>
#include <vector>

namespace hana::test {

struct TestCase {
    const char* name;
    std::function<void()> body;
};

std::vector<TestCase>& registry();
void report_failure(const char* expression, const char* file, int line);

struct Registrar {
    Registrar(const char* name, std::function<void()> body)
    {
        registry().push_back(TestCase{name, std::move(body)});
    }
};

} // namespace hana::test

#define HANA_TEST_CONCAT_INNER(a, b) a##b
#define HANA_TEST_CONCAT(a, b) HANA_TEST_CONCAT_INNER(a, b)

// Defines and registers a test function.
#define TEST(name)                                                                             \
    static void name();                                                                        \
    static ::hana::test::Registrar HANA_TEST_CONCAT(registrar_, name)(#name, name);            \
    static void name()

#define EXPECT(condition)                                                                      \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            ::hana::test::report_failure(#condition, __FILE__, __LINE__);                      \
        }                                                                                      \
    } while (false)
