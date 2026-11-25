// The common part of a driver's start and stop. See
// docs/kernel/DRIVER_LIFECYCLE.md.

#include "kernel/lifecycle.hpp"

namespace {

const char* g_injected_driver = nullptr;
unsigned g_injected_stage = 0;

bool same_name(const char* left, const char* right) {
    if (left == nullptr || right == nullptr) {
        return false;
    }
    size_t index = 0;
    while (left[index] != 0 && left[index] == right[index]) {
        ++index;
    }
    return left[index] == right[index];
}

} // namespace

namespace lifecycle {

bool Acquired::push(const char* stage, Release release, void* context) {
    if (count_ >= kMaxSteps || release == nullptr) {
        return false;
    }
    steps_[count_++] = {stage, release, context};
    return true;
}

void Acquired::unwind() {
    while (count_ != 0) {
        const Step step = steps_[--count_];
        steps_[count_] = {};
        step.release(step.context);
    }
}

const char* Acquired::stage(size_t index) const {
    return index < count_ ? steps_[index].stage : nullptr;
}

void inject_failure(const char* driver, unsigned stage) {
    g_injected_driver = stage != 0 ? driver : nullptr;
    g_injected_stage = stage;
}

bool injected(const char* driver, unsigned stage) {
    if (g_injected_stage == 0 || stage != g_injected_stage || !same_name(driver, g_injected_driver)) {
        return false;
    }
    g_injected_driver = nullptr;
    g_injected_stage = 0;
    return true;
}

} // namespace lifecycle
