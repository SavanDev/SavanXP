#pragma once

#include "boot/boot_info.hpp"

[[noreturn]] void kernel_main(const boot::BootInfo& boot_info);

