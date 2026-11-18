#include "test.h"

int main(int argc, char** argv) {
    std::string filter = argc > 1 ? argv[1] : "";
    int ran = 0;
    for (const TestCase& t : test_registry()) {
        if (!filter.empty() && t.name.find(filter) == std::string::npos) continue;
        int before = test_failures();
        try {
            t.fn();
        } catch (const std::exception& e) {
            ++test_failures();
            std::cerr << "  uncaught exception: " << e.what() << "\n";
        }
        std::cout << (test_failures() == before ? "PASS " : "FAIL ") << t.name << std::endl;
        ++ran;
    }
    std::cout << ran << " tests, " << test_failures() << " failed checks" << std::endl;
    return test_failures() == 0 && ran > 0 ? 0 : 1;
}
