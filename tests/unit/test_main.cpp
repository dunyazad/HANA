#include <cstdio>

#include "test.h"

namespace hana::test {

namespace {
int g_failures = 0;
const char* g_current = "";
} // namespace

std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

void report_failure(const char* expression, const char* file, int line)
{
    ++g_failures;
    std::fprintf(stderr, "%s:%d: [%s] expectation failed: %s\n", file, line, g_current, expression);
}

} // namespace hana::test

int main()
{
    using namespace hana::test;
    for (const TestCase& test : registry()) {
        g_current = test.name;
        test.body();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d expectation(s) failed\n", g_failures);
        return 1;
    }
    std::printf("%zu tests passed\n", registry().size());
    return 0;
}
