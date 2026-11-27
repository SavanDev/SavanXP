# shellcheck shell=bash
# The compile flags of an external program and of the prebuilt SDK runtime it
# links. tools/make-sdk.sh compiles the runtime with them and build-user.sh
# compiles the program with them, so the two always agree. The sysroot ships a
# copy of this file next to its build-user.sh.

# savanxp_sdk_abi_flags SSE [PIE]
# The flags that make an object fit the SDK's runtime: the target, the code
# model, position independence and the register set. A program and the runtime
# variant it links must agree on these. PIE is the in-tree PIC profile
# (SAVANXP_USER_COMPILE_OPTIONS_PIC in CMakeLists.txt), SSE included, as every
# PIE program in the tree has it. tools/sxcc gives a foreign build system these
# and nothing else.
savanxp_sdk_abi_flags() {
    if ((${2:-0})); then
        printf '%s\n' \
            -target x86_64-unknown-none-elf \
            -ffreestanding \
            -fstack-protector-strong \
            -fPIC \
            -mno-red-zone \
            -mcmodel=medium \
            -mno-mmx \
            -msse \
            -msse2 \
            -ffunction-sections \
            -fdata-sections
        return
    fi
    printf '%s\n' \
        -target x86_64-unknown-none-elf \
        -ffreestanding \
        -fstack-protector-strong \
        -fno-pic \
        -fno-pie \
        -mno-red-zone \
        -mcmodel=small \
        -mno-mmx
    if (($1)); then
        printf '%s\n' -msse -msse2
    else
        printf '%s\n' -mno-sse -mno-sse2 -mgeneral-regs-only
    fi
}

# savanxp_sdk_warning_flags
# The warnings the repository builds its own code with.
savanxp_sdk_warning_flags() {
    printf '%s\n' \
        -Wall \
        -Wextra \
        -Wpedantic \
        -Wno-language-extension-token \
        -Wno-c23-extensions
}

# savanxp_sdk_compile_flags SSE
# The flags build-user.sh compiles a static program and the runtime with:
# the ABI flags and the repository's warnings. SSE is 1 for the --sse variant.
savanxp_sdk_compile_flags() {
    savanxp_sdk_abi_flags "$1" 0
    savanxp_sdk_warning_flags
}

# savanxp_sdk_pie_compile_flags
# The same for a PIE program and the runtime variant it links.
savanxp_sdk_pie_compile_flags() {
    savanxp_sdk_abi_flags 1 1
    savanxp_sdk_warning_flags
}

# savanxp_sdk_pie_link_flags NATIVE
# The link flags of a PIE program, for clang as the link driver: the in-tree
# PIE profile (SAVANXP_USER_LINK_OPTIONS_PIE). The Linux triple makes clang
# hand the link to ld.lld. PT_INTERP names the loader's path, and the loader
# itself is linked into the program (ldso.o in the sysroot), which crt0 runs.
# A POSIX program exports its symbols, for dlopen and for the libraries that
# resolve against it. A SUN program does not, so --gc-sections can drop what
# it does not call, unless it opens libraries at run time (sxn/sxnlibrary.h)
# and asks for the exports with a second argument of 1.
savanxp_sdk_pie_link_flags() {
    printf '%s\n' \
        -target x86_64-unknown-linux-gnu \
        -nostdlib \
        -fuse-ld=lld \
        -pie \
        -Wl,-z,max-page-size=0x1000 \
        -Wl,--dynamic-linker,/lib/ld.so.0.4 \
        -Wl,--build-id=none
    if (($1 == 0 || ${2:-0} == 1)); then
        printf '%s\n' -Wl,--export-dynamic
    fi
    printf '%s\n' \
        -Wl,--gc-sections \
        -Wl,-e,_start
}

# savanxp_sdk_variant SSE [PIE]
# The name of the runtime variant: the sysroot directory under lib/.
savanxp_sdk_variant() {
    if ((${2:-0})); then
        printf 'pie\n'
    elif (($1)); then
        printf 'sse\n'
    else
        printf 'nosse\n'
    fi
}

# savanxp_sdk_groups SSE [PIE]
# The runtime groups the sysroot builds for a variant. math, midi and gl need
# SSE. The PIE variant also gets the loader (make-sdk.sh).
savanxp_sdk_groups() {
    if ((${2:-0})); then
        printf '%s\n' core math gui audio midi gl setjmp
    elif (($1)); then
        printf '%s\n' core math gui audio midi gl setjmp
    else
        printf '%s\n' core gui audio setjmp
    fi
}

# savanxp_sdk_runtime_objects SYSROOT VARIANT GROUP...
# Prints, in link order, the path of every object of the named groups in the
# sysroot's lib/VARIANT/units.txt. The ports that link against the sysroot
# use it as build-user.sh does.
savanxp_sdk_runtime_objects() {
    local sysroot=$1 variant=$2
    shift 2
    local manifest="$sysroot/lib/$variant/units.txt" group object wanted
    [[ -f "$manifest" ]] || {
        echo "sdk_flags: no runtime variant '$variant' in $sysroot" >&2
        return 1
    }
    while read -r group object; do
        for wanted in "$@"; do
            if [[ "$group" == "$wanted" ]]; then
                printf '%s\n' "$sysroot/lib/$variant/$object"
                break
            fi
        done
    done <"$manifest"
}

# savanxp_sdk_heap_source BYTES
# Prints a C translation unit with a strong bootstrap heap arena of BYTES,
# which replaces the runtime's weak 256 KiB one (runtime/posix.c).
savanxp_sdk_heap_source() {
    printf '%s\n' \
        '#include <stddef.h>' \
        "_Alignas(16) unsigned char sx_heap_bootstrap[${1}u];" \
        "const size_t sx_heap_bootstrap_size = ${1}u;"
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
