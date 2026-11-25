// Host tests of the common part of a driver's start and stop
// (include/kernel/lifecycle.hpp, docs/kernel/DRIVER_LIFECYCLE.md): what a
// start acquired is given back in reverse, exactly once, and an injected
// failure hits one driver at one stage, once.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "kernel/lifecycle.hpp"

namespace {

int g_checks = 0;
char g_order[32] = {};
size_t g_order_length = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "lifecycle-test: FAIL %s\n", what);
        std::exit(1);
    }
}

void note(void* context) {
    g_order[g_order_length++] = *static_cast<const char*>(context);
    g_order[g_order_length] = 0;
}

void reset_order() {
    g_order[0] = 0;
    g_order_length = 0;
}

void test_unwind() {
    static const char a = 'a';
    static const char b = 'b';
    static const char c = 'c';
    lifecycle::Acquired acquired;
    reset_order();
    check(acquired.push("claim", &note, const_cast<char*>(&a)), "push the claim");
    check(acquired.push("io", &note, const_cast<char*>(&b)), "push the registers");
    check(acquired.push("irq", &note, const_cast<char*>(&c)), "push the line");
    check(acquired.count() == 3 && std::strcmp(acquired.stage(1), "io") == 0, "each stage is named");
    check(acquired.stage(3) == nullptr, "nothing past the end");
    acquired.unwind();
    check(std::strcmp(g_order, "cba") == 0, "given back last taken first");
    check(acquired.count() == 0, "empty after");
    acquired.unwind();
    check(std::strcmp(g_order, "cba") == 0, "a second unwind gives nothing back twice");
    check(!acquired.push("none", nullptr, nullptr), "a step needs a release");
}

void test_full() {
    static const char x = 'x';
    lifecycle::Acquired acquired;
    reset_order();
    for (size_t index = 0; index < lifecycle::kMaxSteps; ++index) {
        check(acquired.push("step", &note, const_cast<char*>(&x)), "room for every step");
    }
    check(!acquired.push("one too many", &note, const_cast<char*>(&x)), "a full record refuses");
    acquired.unwind();
    check(g_order_length == lifecycle::kMaxSteps, "every recorded step given back, the refused one not");
}

void test_injection() {
    check(!lifecycle::injected("rtl8139", 1), "nothing injected");
    lifecycle::inject_failure("rtl8139", 3);
    check(!lifecycle::injected("rtl8139", 2), "another stage runs");
    check(!lifecycle::injected("rtl8168", 3), "another driver runs");
    check(!lifecycle::injected("rtl813", 3), "a prefix is another driver");
    check(lifecycle::injected("rtl8139", 3), "the stage fails");
    check(!lifecycle::injected("rtl8139", 3), "once");
    lifecycle::inject_failure("rtl8139", 2);
    lifecycle::inject_failure("rtl8139", 0);
    check(!lifecycle::injected("rtl8139", 2), "0 clears it");
    lifecycle::inject_failure(nullptr, 1);
    check(!lifecycle::injected("rtl8139", 1), "no driver named, none hit");
    check(!lifecycle::injected(nullptr, 1), "a missing name is never hit");
}

} // namespace

int main() {
    test_unwind();
    test_full();
    test_injection();
    std::printf("lifecycle-test: %d checks passed\n", g_checks);
    return 0;
}
