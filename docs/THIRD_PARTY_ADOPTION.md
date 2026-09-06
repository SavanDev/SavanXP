# Policy for adopting third-party code and design

SavanXP may study, reuse or take inspiration from components of other projects
when that speeds up development without compromising legal clarity or the
maintainability of the repository.

## Mandatory categories

Every adoption has to be recorded under one of these categories:

- `Reference`: the design or behavior is studied and implemented from scratch
  inside SavanXP.
- `Selective port`: small, self-contained code is copied, preserving its
  copyright and license notices.
- `Do not adopt`: the component is discarded because of tight coupling, an
  unclear license or maintenance cost.

## Default operating rule

- compositor, base graphics toolkit, window APIs and desktop flow: `Reference`
- small helpers for geometry, rects, simple bitmaps or similar utilities:
  `Selective port` if the license is clear and the resulting dependency stays
  minimal
- assets, icons, cursors, fonts or content that is not strictly code:
  `Do not adopt` until license and provenance are reviewed case by case
- bounded exception: temporary reference assets for visual prototypes may be
  brought in if they stay isolated under `assets/.../reference/...`, are
  documented in the provenance registry, and are scheduled for replacement by
  the project's own art

## Requirements for any adoption

- record the exact origin of the component in
  `docs/THIRD_PARTY_PROVENANCE.md`
- note the verified license
- justify the technical decision
- preserve the original notices whenever there is a `Selective port`
- do not relicense a piece copied from a third party under another permissive
  license as pure MIT

## SerenityOS

For SerenityOS the repository's initial approach is:

- `WindowServer`, `Compositor` and `LibGUI`: `Reference`
- base `LibGfx`, 2D primitives and rect structures: `Reference` by default,
  with `Selective port` only for small, self-contained helpers
- non-VirtIO GPU backends, assets, icons and fonts: `Do not adopt` at this
  stage
- assets, icons and visual art of the current desktop: the repository's own,
  generated locally, with no active dependency on SerenityOS assets

## OpenBSD

Almost the whole OpenBSD tree is ISC or BSD-2, compatible with MIT: the only
requirement is preserving the original notices. It is the external source with
the least legal friction for this repository, so the default here is more
permissive than for SerenityOS: small, self-contained pieces can go straight to
`Selective port`.

`Selective port`:

- safe string functions from `lib/libc` (strlcpy, strlcat, strtonum,
  reallocarray, recallocarray, freezero, explicit_bzero): there are still uses
  of strcpy, strcat and sprintf in the kernel and userland that these replace
  without changing the shape of the code
- `sys/kern/subr_prf.c`: printf with no FILE dependency, with width and padding
  support. It unifies the two userland printfs (libc.c and posix.c) and covers
  the formatting that is missing today
- `arc4random` (ChaCha20, `lib/libcrypto/arc4random`): there is no RNG in the
  repository; the ephemeral ports and the TCP ISNs in kernel/net.cpp need one,
  and today they are predictable
- `sys/dev/pci/ac97.c`: only the warm reset sequence with codec-ready timeouts
  and the mixer table, which is where VMs differ
- `pcidevs` plus `devlist2h.awk`: a PCI ID table generated from text, to name
  devices in pci.cpp and in the sysinfo and netinfo views
- `signify`: Ed25519 signing in ~1000 lines of ISC, for SXE binaries as an
  additional non-alloc section

`Reference`:

- sndio and the audio(4) API: the same shape as the project's own audio HAL
  (dispatcher plus backends); what it contributes is the under-run and latency
  policy
- `amd64/lapic.c`, `ioapic.c`, `acpimadt.c`: xAPIC over MMIO, x2APIC, MADT
  overrides and virtual wire mode, in short and readable code
- wscons, wsdisplay, wskbd and wsmouse: separation between device, terminal
  emulation and input; includes PS/2 decoding with 0xFA and 0xFE resends
- userland `malloc.c`: guard pages, canaries, junk fill, unmap on free and
  runtime debug flags
- the pledge and unveil model: capabilities declared per process, with the
  subsystems and the SXE metadata as the natural cut point. The implementation
  is not ported: it is coupled to their syscall table
- `src/regress`: how the regression tests are organized
- `tcp_input.c`: read-only, for retransmission timers, sliding window and close
  states. Coupled to mbuf, not portable

`Do not adopt`:

- UVM, FFS and UFS, pf, xenocara and the DRM imported from Linux: tight
  coupling and no fit with SxFS or with the project's own display HAL
- ksh: shell_core.c already exists
- OpenBSD has no virtio-gpu driver, so there is nothing to take for the main
  graphics path

Every piece that becomes a `Selective port` goes into the provenance registry
with its original ISC header intact: those notices are a few lines long, and
losing them is the easy mistake to make.

## Traceability: the registry is the source of truth

Traceability lives in `docs/THIRD_PARTY_PROVENANCE.md`, not in commit messages.
The previous rule asked every commit to cite its category; it is retired
because it points at the wrong place: when you have to audit what is being
distributed, what you look at is the current tree, not the history.

Registry invariant:

- every directory under `vendor/` and `sdk/` holding third-party code has an
  entry
- every third-party piece that ends up baked into the ISO, the initramfs or the
  disk image has an entry, even when the repository does not version it and it
  is downloaded or supplied separately
- each entry declares origin, pinned version or commit, verified license,
  decision, and where the distributed bit ends up

It is verified by listing `vendor/` and `sdk/` against the registry. That is a
manual review, and the moment to do it is the change that adds or updates the
component, not afterwards.

File-level rules:

- with `Selective port`, the file keeps its original header or an equivalent
  comment with origin and license
- with `Reference`, the inspiration is documented in the registry and not
  repeated in every file
- code of our own that wraps or adapts a third-party piece is a derivative work
  and inherits that piece's license, not the repository's MIT: shims carry a
  header too
