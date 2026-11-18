# shellcheck shell=bash
# The compile flags of an external program and of the prebuilt SDK runtime it
# links. tools/make-sdk.sh compiles the runtime with them and build-user.sh
# compiles the program with them, so the two always agree. The sysroot ships a
# copy of this file next to its build-user.sh.

# savanxp_sdk_compile_flags SSE
# Prints the flags one per line. SSE is 1 for the --sse variant, 0 otherwise.
savanxp_sdk_compile_flags() {
    printf '%s\n' \
        -target x86_64-unknown-none-elf \
        -ffreestanding \
        -fstack-protector-strong \
        -fno-pic \
        -fno-pie \
        -mno-red-zone \
        -mcmodel=small \
        -mno-mmx \
        -Wall \
        -Wextra \
        -Wpedantic \
        -Wno-language-extension-token \
        -Wno-c23-extensions
    if (($1)); then
        printf '%s\n' -msse -msse2
    else
        printf '%s\n' -mno-sse -mno-sse2 -mgeneral-regs-only
    fi
}

# savanxp_sdk_variant SSE
# The name of the runtime variant: the sysroot directory under lib/.
savanxp_sdk_variant() {
    if (($1)); then
        printf 'sse\n'
    else
        printf 'nosse\n'
    fi
}

# savanxp_sdk_groups SSE
# The runtime groups the sysroot builds for a variant. math and gl need SSE.
savanxp_sdk_groups() {
    if (($1)); then
        printf '%s\n' core math gui audio gl setjmp
    else
        printf '%s\n' core gui audio setjmp
    fi
}

# savanxp_sdk_version ABI_HEADER
# Prints MAJOR.MINOR from SAVANXP_SDK_VERSION_MAJOR/MINOR in the ABI header.
savanxp_sdk_version() {
    local major minor
    major=$(sed -n 's/^#define SAVANXP_SDK_VERSION_MAJOR \([0-9][0-9]*\)$/\1/p' "$1")
    minor=$(sed -n 's/^#define SAVANXP_SDK_VERSION_MINOR \([0-9][0-9]*\)$/\1/p' "$1")
    if [[ -z "$major" || -z "$minor" ]]; then
        echo "sdk_flags: no SAVANXP_SDK_VERSION_MAJOR/MINOR in $1" >&2
        return 1
    fi
    printf '%s.%s\n' "$major" "$minor"
}
