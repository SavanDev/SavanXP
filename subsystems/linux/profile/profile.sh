#!/usr/bin/env bash
# L0 of the Linux subsystem (SSL): rebuild the pinned profile and its
# reference outputs on a Linux build host.
#
#   profile.sh setup      fetch the pinned Alpine root and packages, by hash
#   profile.sh fixtures   build the fixtures and print their hashes
#   profile.sh trace      run every fixture and Xeyes under strace, with the
#                         X requests recorded, into build/linux-profile/out
#   profile.sh all        the three in order
#
# Everything runs as an unprivileged user inside a user namespace (bwrap):
# no root, no package installed on the host. Nothing it fetches is
# committed; see docs/userland/LINUX_XEYES_PROFILE.md for what is.
#
# Host tools: bash, bwrap, curl, sha256sum, tar, clang, ld.lld, python3, and
# ImageMagick (magick) for the screenshots.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
work="${SSL_PROFILE_DIR:-$repo/build/linux-profile}"
root="$work/root"
out="$work/out"
pkgs="$work/packages"
xsock="$work/xsock"
fixsrc="$repo/subsystems/linux/fixtures"
lock="$here/alpine.lock"

mirror="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
branch="v3.24"
rootfs="alpine-minirootfs-3.24.4-x86_64.tar.gz"
rootfs_sha256="cdaa38cb8de3537967ee5b6c9b6cd00cc962da2b018e6eea48696bb274923012"

need() {
    for t in "$@"; do
        command -v "$t" >/dev/null || { echo "profile.sh: '$t' not found on PATH" >&2; exit 1; }
    done
}

# Run a command inside the Alpine root. The X socket directory is shared
# with the host, where the recording proxy runs.
guest() {
    mkdir -p "$xsock" "$out"
    bwrap --bind "$root" / --proc /proc --dev /dev --tmpfs /tmp \
        --bind "$xsock" /tmp/.X11-unix --bind "$out" /out \
        --unshare-user --uid 0 --gid 0 --unshare-net --unshare-uts --hostname savanxp \
        --cap-add CAP_SYS_CHROOT \
        --clearenv --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin --setenv HOME /root \
        "$@"
}

cmd_setup() {
    need bwrap curl sha256sum tar
    mkdir -p "$work" "$pkgs"
    if [ ! -f "$work/$rootfs" ]; then
        curl -fsSL -o "$work/$rootfs.part" "$mirror/$branch/releases/x86_64/$rootfs"
        mv "$work/$rootfs.part" "$work/$rootfs"
    fi
    echo "$rootfs_sha256  $work/$rootfs" | sha256sum -c --quiet
    rm -rf "$root"
    mkdir -p "$root"
    tar -xzf "$work/$rootfs" -C "$root"

    # Every package of the closure, fetched by name and version and checked
    # against the lock before apk sees it. apk then checks its signature.
    local list=()
    while read -r name version repo sha; do
        case "$name" in ''|'#'*) continue ;; esac
        local file="$name-$version.apk"
        if [ ! -f "$pkgs/$file" ]; then
            curl -fsSL -o "$pkgs/$file.part" "$mirror/$branch/$repo/x86_64/$file"
            mv "$pkgs/$file.part" "$pkgs/$file"
        fi
        echo "$sha  $pkgs/$file" | sha256sum -c --quiet
        list+=("/packages/$file")
    done < "$lock"
    bwrap --bind "$root" / --ro-bind "$pkgs" /packages --proc /proc --dev /dev \
        --tmpfs /tmp --unshare-user --uid 0 --gid 0 --unshare-net \
        --clearenv --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin \
        apk add --no-network --quiet --repositories-file /dev/null "${list[@]}"
    echo "profile.sh: root ready at $root"
}

# Static C fixtures: the project's clang and lld against the pinned musl-dev
# files, and nothing from the host's C library or compiler runtime.
musl_static() {
    local src="$1" dst="$2"
    clang --target=x86_64-linux-musl -nostdinc -isystem "$root/usr/include" \
        -isystem "$(clang -print-resource-dir)/include" \
        -static -nostdlib -fuse-ld=lld -O2 -Wall -s -Wl,--build-id=none \
        -o "$dst" "$root/usr/lib/crt1.o" "$root/usr/lib/crti.o" "$src" \
        "$root/usr/lib/libc.a" "$root/usr/lib/crtn.o"
}

cmd_fixtures() {
    need clang ld.lld sha256sum
    local f="$root/fixtures"
    mkdir -p "$f"
    # The libc-free fixtures name their target, so the build (Linux.cmake)
    # makes the same bytes whatever its compiler defaults to.
    local target=--target=x86_64-pc-linux-gnu
    clang $target -nostdlib -static -fuse-ld=lld -Wl,--build-id=none \
        -o "$f/hello" "$fixsrc/hello/hello.S"
    clang $target -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector -O1 \
        -fuse-ld=lld -s -Wl,--build-id=none \
        -o "$f/startup" "$fixsrc/startup/entry.S" "$fixsrc/startup/startup.c"
    clang $target -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector -O1 \
        -fuse-ld=lld -s -Wl,--build-id=none \
        -o "$f/errors" "$fixsrc/errors/errors.c"
    clang $target -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector -O1 \
        -fuse-ld=lld -s -Wl,--build-id=none \
        -o "$f/regions" "$fixsrc/regions/regions.c"
    for n in args tls heap clock files; do
        musl_static "$fixsrc/static/$n.c" "$f/$n"
    done
    clang --target=x86_64-linux-musl -nostdinc -isystem "$root/usr/include" \
        -isystem "$(clang -print-resource-dir)/include" \
        -nostdlib -fuse-ld=lld -O2 -Wall -s -Wl,--build-id=none \
        -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -Wl,-z,now \
        -o "$f/xclient" "$root/usr/lib/crt1.o" "$root/usr/lib/crti.o" \
        "$fixsrc/xclient/xclient.c" -L"$root/usr/lib" -lX11 -lc "$root/usr/lib/crtn.o"
    (cd "$f" && sha256sum hello startup errors regions args tls heap clock files xclient)
}

# strace of one command; stdout and the exit status are kept beside it.
trace_one() {
    local name="$1"
    shift
    guest /bin/sh -c '
        name="$1"; shift
        cd /work
        strace -f -qq -s 48 -o "/out/$name.strace" "$@" >"/out/$name.stdout" 2>"/out/$name.stderr"
        echo $? >"/out/$name.status"
    ' sh "$name" "$@"
}

cmd_trace() {
    need bwrap python3 magick
    rm -rf "$out" "$xsock"
    mkdir -p "$out" "$xsock" "$root/work"
    rm -rf "$root/work"/*

    trace_one hello /fixtures/hello
    trace_one startup env -i A=1 B=two /fixtures/startup x 'y z'
    trace_one errors /fixtures/errors
    trace_one regions /fixtures/regions
    trace_one args env -i A=1 B=two /fixtures/args x 'y z'
    trace_one tls /fixtures/tls
    trace_one heap /fixtures/heap
    trace_one clock /fixtures/clock
    trace_one files-write /fixtures/files write /work
    trace_one files-check /fixtures/files check /work
    trace_one files-remove /fixtures/files remove /work

    # L3: the BusyBox applets of the profile, on a prepared tree.
    guest /bin/sh -c 'mkdir -p /work/tree/a/b && printf "one\ntwo three\n" >/work/tree/a/f.txt && printf "x\n" >/work/tree/z'
    trace_one bb-echo /bin/busybox.static echo hello world
    trace_one bb-cat /bin/busybox.static cat /work/tree/a/f.txt
    trace_one bb-ls /bin/busybox.static ls -1 /work/tree /work/tree/a
    trace_one bb-pwd /bin/busybox.static pwd
    trace_one bb-wc /bin/busybox.static wc /work/tree/a/f.txt
    trace_one bb-mkdir /bin/busybox.static mkdir -p /work/tree/new/deeper
    trace_one bb-cat-missing /bin/busybox.static cat /work/tree/missing

    # L4: a dynamic program with no display, using two libraries besides libc.
    guest /bin/sh -c 'printf "{\"a\":[1,2,3],\"s\":\"abbbc\"}\n" >/work/in.json'
    trace_one jq /usr/bin/jq -c '{d: (.a | map(. * 2)), m: (.s | test("ab+c"))}' /work/in.json
    # The two failures L4 must reproduce, in a tree that lacks libonig, and
    # one that lacks the interpreter. strace follows chroot into the program.
    guest /bin/sh -c '
        for t in nolib nointerp; do
            mkdir -p /work/$t/usr/bin /work/$t/usr/lib /work/$t/lib
            cp /usr/bin/jq /work/$t/usr/bin/
            cp /usr/lib/libjq.so.1 /work/$t/usr/lib/
        done
        cp /lib/ld-musl-x86_64.so.1 /work/nolib/lib/'
    trace_one jq-missing-lib chroot /work/nolib /usr/bin/jq --version
    trace_one jq-missing-interp chroot /work/nointerp /usr/bin/jq --version

    trace_x11 xclient /fixtures/xclient
    trace_x11 xeyes /usr/bin/xeyes
    echo "profile.sh: outputs in $out"
}

# An X client against Xvfb, through the recording proxy, with the pointer
# moved to known positions and a screenshot of each.
trace_x11() {
    local name="$1" prog="$2"
    local xvfb_pid proxy_pid client_pid
    rm -f "$xsock"/X0 "$xsock"/X1
    guest Xvfb :0 -screen 0 640x480x24 -nolisten tcp -noreset >"$out/$name.xvfb.log" 2>&1 &
    xvfb_pid=$!
    for _ in $(seq 100); do [ -S "$xsock/X0" ] && break; sleep 0.1; done
    python3 "$here/x11ctl.py" "$xsock/X0" warp 320 240
    python3 "$here/x11rec.py" --listen "$xsock/X1" --upstream "$xsock/X0" \
        --log "$out/$name.x11" --toplevel "$out/$name.toplevel" 2>/dev/null &
    proxy_pid=$!
    for _ in $(seq 100); do [ -S "$xsock/X1" ] && break; sleep 0.1; done

    guest /bin/sh -c '
        name="$1"; shift
        cd /work
        DISPLAY=:1 strace -f -qq -s 48 -o "/out/$name.strace" "$@" >"/out/$name.stdout" 2>"/out/$name.stderr"
        echo $? >"/out/$name.status"
    ' sh "$name" "$prog" &
    client_pid=$!
    for _ in $(seq 100); do [ -s "$out/$name.toplevel" ] && break; sleep 0.1; done
    sleep 1

    local i=0
    for pos in "75 50" "0 0" "149 99" "100 20" "400 300" "620 10" "10 460"; do
        # shellcheck disable=SC2086
        python3 "$here/x11ctl.py" "$xsock/X0" warp $pos
        sleep 0.5
        guest /bin/sh -c 'xwd -root -silent -display :0 >/out/shot.xwd'
        magick "$out/shot.xwd" -crop 220x170+0+0 +repage -strip -define png:exclude-chunks=date,time "$out/$name-$i-${pos// /x}.png"
        rm -f "$out/shot.xwd"
        i=$((i + 1))
    done

    python3 "$here/x11ctl.py" "$xsock/X0" delete "$(cat "$out/$name.toplevel")"
    for _ in $(seq 50); do kill -0 "$client_pid" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$client_pid" 2>/dev/null; then
        echo "profile.sh: $name did not exit on WM_DELETE_WINDOW" >&2
        kill "$client_pid" || true
    fi
    wait "$client_pid" || true
    kill "$proxy_pid" "$xvfb_pid" 2>/dev/null || true
    wait "$proxy_pid" "$xvfb_pid" 2>/dev/null || true
}

case "${1:-}" in
    setup) cmd_setup ;;
    fixtures) cmd_fixtures ;;
    trace) cmd_trace ;;
    all) cmd_setup; cmd_fixtures; cmd_trace ;;
    *) sed -n '2,13p' "$0"; exit 2 ;;
esac
