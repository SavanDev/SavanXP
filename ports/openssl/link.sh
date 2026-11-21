#!/bin/bash
# Link libcrypto.so.0.4 and libssl.so.0.4 from the archives, and check what
# they ask the program for.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

mkdir -p "$OUT"
cd "$OUT"

# The two libraries upstream ships, so later ports (curl, the browser) find
# them as they expect. Whole archives, because nothing references their
# objects at link time; the undefined symbols are the C library's, resolved
# by the loader against the program, as every library here does
# (docs/kernel/SHARED_LIBRARIES.md). -Wl,-shared, not -shared: the none
# triple's driver ignores -shared.
link_library() {
    local name=$1 archive=$2
    shift 2
    $LINK_CC -target x86_64-unknown-none-elf -nostdlib -Wl,-shared -fuse-ld=lld \
        -Wl,--whole-archive "$archive" -Wl,--no-whole-archive \
        -Wl,--unresolved-symbols=ignore-all -Wl,-soname,"$name" "$@" \
        -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none -o "$name"
}
link_library "$LIBCRYPTO" "$BUILD/libcrypto.a"
link_library "$LIBSSL" "$BUILD/libssl.a" \
    -L"$OUT" -Wl,-Bdynamic -Wl,--no-as-needed -l:"$LIBCRYPTO" -Wl,-Bstatic

# What the libraries may ask the program for: the C functions both runtimes
# define and that work in SUN programs too (docs/userland/NATIVE_HEAP.md).
# Anything else -- a file, a socket, a thread -- is a POSIX call inside a
# SUN program, and fails the build.
ALLOWED="abort atexit atoi free getegid getentropy getenv geteuid getgid getpid
gettimeofday getuid gmtime isdigit isspace malloc memchr memcmp memcpy memmove
memset nanosleep qsort realloc sscanf __stack_chk_fail __stack_chk_guard strcat
strchr strcmp strcpy strcspn strdup strerror strlen strncmp strncpy strpbrk
strrchr strspn strstr strtol strtoul sx_errno_location time tolower"
unexpected=$("$NM_CMD" -D --undefined-only --format=just-symbols "$LIBCRYPTO" "$LIBSSL" |
    grep -v ':$' | sort -u | grep -vxF -f <(tr ' ' '\n' <<<"$ALLOWED" | grep -v '^$') |
    grep -vxF -f <("$NM_CMD" -D --defined-only --format=just-symbols "$LIBCRYPTO") || true)
if [ -n "$unexpected" ]; then
    echo "openssl: the libraries ask the program for more than both runtimes give:" >&2
    printf '  %s\n' $unexpected >&2
    exit 1
fi

mkdir -p "$OUTPUT_ROOT/external"
for library in "$LIBCRYPTO" "$LIBSSL"; do
    cp "$OUT/$library" "$OUTPUT_ROOT/external/$library"
    echo "== $library: $(du -h "$library" | cut -f1)"
done
