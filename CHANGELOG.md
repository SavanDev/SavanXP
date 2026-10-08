# Changelog

Record of user-visible changes per version for SavanXP.

Cut-off notes:

- `v0.1.0` was reconstructed retroactively from the history up to `d822857`.
- `v0.1.1` covers the changes after `v0.1.0`, including work already merged into
  the tree but not yet tagged in git.

## [Unreleased]

### Changed

- **The `legacy` branch identifies the system as the Legacy Edition.** `SAVANXP_EDITION`
  in `include/shared/version.h` feeds System Properties, the boot splash and the
  desktop stamp.

## [0.4.0] - 2026-10-08

### Added

- **Per-song `GENMIDI` bank in `libsxmidi`.** `sx_midi_song_create_with_bank` takes the lump
  with the MIDI, `sx_midi_song_uses_genmidi` reports the fallback, the port passes it always.
  Host checks plus `doom-mus-test` on the real `D_E1M1` cover both paths (see `docs/MIDI.md`).

- **`libsxmidi.so.0.4`, a General MIDI synthesizer the tree builds and installs
  in `/disk/lib`.** It parses a Standard MIDI File, plays it through a built-in
  16-family instrument bank and a drum kit, and renders mono PCM for the mixer.
  No soundfont and no `libm`; the public surface is `savanxp/midi.h` and the
  design is in `docs/MIDI.md`.

- **The DoomGeneric port has music.** Each MUS lump is converted to MIDI with
  the upstream `mus2mid.c`, `libsxmidi` synthesises it into the mixer's buffer as
  the game runs and a dedicated voice loops that buffer. The port is now PIE and
  declares a `DT_NEEDED` for the library, so building it requires
  `./build.sh build` to have produced `/disk/lib/libsxmidi.so.0.4` first.

- **The port's music is checked on the guest audio, and on what follows it.**
  `./build.sh smoke doom-music-smoke` opens the port from the desktop, starts the
  level, fails if the WAV QEMU captured comes out silent, then closes the window
  and fails if any audio outlives the last music.

- **Three SpicyGame pixel fonts join the baked set.** `RetroSans.ttf`,
  `PixelSans.ttf` and `ImpactfulBits.ttf` (CC0) are baked by
  `tools/font/genfont.py` into committed tables; only the RetroSans caption is
  linked, the other two are spares. `genfont.py` now accepts any `SX_*` prefix.

- **Start menu (trimmed): Start button plus a flat Programs list.** `/bin/startmenu`
  opens anchored above the button with the progman catalog grouped by category,
  launches on click and closes on toggle or outside click. Mouse only, no cascade.

- **The Start menu gains a footer, a banner strip, a logo button and a self-listed launcher.**
  Shut Down (with its icon) and Restart with two-step confirm over a margin, a 24px
  gradient strip in title blue, the brand logo on Start, Program Manager under System.

- **`libffmpeg.so.0.4`, FFmpeg's five components as one shared library.** The
  FFmpeg port installs it at `/disk/lib`, and `ldso_load` maps it: 6.8 MB on disk,
  2334 exported symbols and 9228 relocations, which is the first workload the
  loader has seen that it was not built for.

- **The Media Player is a program that depends on a library.** It went from 7 MB
  with FFmpeg linked in to 344 KB with one `DT_NEEDED`, and it decodes through
  `/disk/lib/libffmpeg.so.0.4`.

- **The Media Player is a program of the system tree, and the FFmpeg port builds
  no program at all.** Its source moved to `subsystems/posix/userland/mediaplayer/`
  and the port's only artifact is `/disk/lib/libffmpeg.so.0.4`. Opening `.mp3`,
  `.avi` and `.flac` from Files goes through `/bin/mediaplayer`, which says in its
  own window which library it could not load. The program is built when the port
  has been built — FFmpeg's headers are generated, not versioned — and CMake says
  so instead of skipping quietly; without the port the launcher drops the entry,
  as it does for Doom.

- **A program whose library is missing now says so and keeps running.** It used
  to die with a page fault at the address of a PLT stub. `ldso_missing()` is part
  of the SDK, and the player checks it before its first call.

- **A missing dependency no longer leaves the program unrelocated, and no longer
  takes the rest of them with it.** Relocations were skipped when a `DT_NEEDED`
  failed to load, which left every global pointer at its link-time value —
  `stdout` among them, so the program could not even print the reason it was
  broken. And the walk stopped at the first missing library, so a missing
  `libffmpeg.so.0.4` also left `libsxgfx`, `libgfx2d` and `libsxgui` unloaded: the
  Media Player started and died on its first toolkit call, jumping through
  `sx_rect_make@plt` with no link between that address and its cause.

- **The loader applies `R_X86_64_64`.** A data slot holding the address of an
  imported symbol needs a runtime resolution, and the loader aborted the load on
  it. `libffmpeg.so.0.4` has 253 of them.

- **`libgfx2d.so.0.4`, the 2D painter and window chrome, as a shared library.**
  No library asks the program for a single symbol now — every reference any of them
  makes is to the C runtime or a system call. A program no longer has to be PIE so
  that a toolkit can find its drawing code.

- **Up to 32 shared libraries per process, instead of 8.** The longest chain the
  tree can build is seven — a program, its two interface libraries and FFmpeg's
  codec, utility, scale and resample libraries — so eight was one slot from a wall
  that FFmpeg integration would have discovered instead of us. Raising it alone would
  have cost 134 KiB of resident memory in every process, including programs that load
  no library at all, so the per-library slot was halved first.

- **The library loader no longer leaks a descriptor per library.** It kept the file
  descriptor and a whole-file section open after the segments were mapped, two per
  library, forever. A session with the task bar and the keyboard popup ran out of
  descriptor headroom once a third library was added.

- **A rebuilt program now reaches the bootable image.** The rootfs staging step
  depended on an aggregate build target that produces no file, so it was considered
  up to date while the programs were already rebuilt. A change to any program left
  the image booting the previous binary, with every check still passing.

- **The visual scenarios now assert the glyphs on screen, not just that something
  moved.** A toolkit that painted text in the background colour passed: the scroll
  was checked with the scrollbar thumb, which moves regardless. The expected pixels
  are rendered from the font tables inside the built graphics library, so the check
  is about the characters rather than about ink.

- **A program can now ask the loader which dependency failed to load.** The loader
  was never fatal — a program with an unresolvable library still starts — but there
  was no way to find that out, so the ability existed by accident rather than by
  design. Both properties are now covered by a self-test that removes a declared
  library from the volume.

- **The Media Player is built position-independent.** Its objects were compiled
  `-fno-pic`, so they could not go into a shared library at all. `libffmpeg.so.0.4`
  now links from them: 6.7 MB, 2334 exported symbols, no relocation errors.

- **Two Media Player smoke scenarios are removed.** They tested a `--availability`
  mode that stopped existing when SxMedia was withdrawn, and they had been asking for
  output nothing produced since. The absence-detection they covered is going away for
  a better reason: the player becomes a system application and reports a missing
  library itself.

- **A missing shared library is now recorded as reported by the application itself.**
  The loader does not abort when a dependency fails, so a program can still open a
  window and say what is missing; the two pieces the loader would need for that are
  written down, along with the fact that an installed library cannot be uninstalled
  and why that is correct.

- **Dead code in the binaries is now measured against what would recover it:** 5.5 MB
  across the tree, held back by `--export-dynamic`, which has to stay while the
  libraries resolve the C runtime against the program.

- **A relocation type the loader does not implement now says so,** instead of
  failing with a step number that leads nowhere. It cannot arise from this linker,
  which only emits the three supported types; it is there for a hand-linked object.

- **Three more loader failures now say what they are, and each has a self-test.** A
  missing library and a full slot table were both just numbers, and they are
  different problems with different fixes. A dependency shared by two branches of a
  dependency graph was loaded once per branch; nothing exercised that case, because
  a two-library chain cannot produce it.

- **The library loader says which symbol it could not resolve.** A load failure
  reported a step number, which says where the failure was and nothing about what
  it was. A dependency built with an incomplete `DT_NEEDED` now names the missing
  symbol and the library that wanted it, and a self-test asserts the name rather
  than just the failure.

- **The runtime for external applications is derived from the one for the system's
  own programs**, instead of being listed again by hand. The two lists have to agree
  and nothing complained when they did not — the symptom is an undefined symbol much
  later than the change that caused it.

- **The build now fails if a program carries its own copy of a library it loads.**
  Such a program links, runs and passes every test — the library is mapped and
  never called — so the sharing silently stops happening and nothing notices.

- **The kernel now tells a program where its own image starts.** The library
  loader was guessing it by walking backwards a page at a time looking for the ELF
  header, which assumed every page on the way was mapped. A linker can leave a gap
  between two loadable segments, and reading one of those is a page fault — a
  program died before `main`. A self-test with text starting at `0xc000` and its
  first segment ending at `0xaff3` is enough to trigger it.

- **`libsxgfx.so.0.4`, the graphics layer, as a shared library.** The toolkit
  now links against it instead of asking the program to export the graphics
  entry points, so a program no longer has to be PIE just to satisfy it. The task
  bar, the keyboard layout popup, the shell wallpaper, the window server and the
  desktop shell draw through it, and no executable carries its own copy of it.

- **`sxgui-smoke`, which drives the visual scenario as a smoke test.** The SxGUI
  gate used to run only if someone remembered to ask for it.

- **`libsxgui.so.0.4`, and ten programs now draw through it.** Every user of the
  toolkit — the calculator, the text editor, the control gallery, the file
  manager, the application wizard, the system properties window, Minesweeper, the
  self-test, Program Manager and Task Manager — declares it and carries a copy
  of neither. The toolkit resolves the graphics layer against the program that
  started it, so the layer underneath stays where it is for now. Verified at the
  pixel level: the editor's scroll screenshots are byte-identical with and
  without the library.

- **`ldso_symbol_is_shared()`** reports whether a symbol came from a shared
  library or from the program itself, so a program can check that it really is
  using the library and not a copy of its own.

- **`sx_start_dynamic` reports a failing interpreter.** A program that could not
  be relocated said nothing and failed later somewhere unrelated.

- **`calc` now links as a PIE**, the first desktop program to do so. It joins
  `libtest` in proving the profile beyond the test programs.

- **`crt0` now runs the library loader before `main`.** A program that links the
  loader is ready to call into its libraries with no setup of its own; programs
  that do not link it are unaffected.

- **`libtest`, the first program that links against a shared library.** It calls
  `sqrt` like any other function instead of resolving it by hand, after
  `ldso_start()` loads what the program declares and fills its `GOT`. It is
  built without `math.c`, so `sqrt` genuinely comes from `libmath.so.0.4`.

- **The loader relocates the executable, not just libraries.** The kernel maps
  the main image but leaves its `GOT` empty, so a program that links a library
  has to call `ldso_start()` before using it. `R_X86_64_RELATIVE` is now
  applied: without it a relocated image keeps pointers to address zero.

- **The executable is now in the library loader's symbol scope.** A library can
  resolve a symbol against the program that started it, so `-fstack-protector-
  strong` is back on for shared libraries. `ldtest` checks the stack canary
  resolves and holds a live value.

- **A library now loads the libraries it declares.** `DT_NEEDED` is walked from
  `/lib`, each dependency is mapped once however many ask for it, and a symbol
  resolves through the whole chain. `ldtest` calls a function in a dependency and
  checks the result.

- **`pietest`, the first PIE executable in the tree.** `ET_DYN`, relocated by the
  kernel onto a base, with `.dynsym` and `.dynamic`. It proves a relocated image
  loads and runs — and it proves the premise the whole feature rests on: a PIE
  executable exports symbols a library can resolve against, which lld denies to
  a non-PIE `ET_EXEC`.

- **The kernel loads `ET_DYN` images with a load bias.** An `ET_EXEC` is unchanged
  — bias zero, exactly where the linker script put it. An `ET_DYN` is relocated
  onto a base, which starts at `kUserBase`. Per-process entropy for that base, and
  ASLR for the executable image, is the next step.

- **The kernel reads `PT_INTERP` and hands the path to `crt0`.** A binary linked
  with `--dynamic-linker` now has its interpreter path delivered in `rcx` and
  readable through `savanxp_interpreter_path()`. `interptest` is linked with one
  and checks it arrives. The interpreter is not run yet: `crt0` still goes
  straight to `main`.

- **`libmath.so.0.4`, a real shared library, and a minimal loader that maps
  it.** `/lib/libmath.so.0.4` is built as PIC and lands on the volume, not in
  `/bin`, so Program Manager never lists it. `ldso` reads its program headers,
  places each `PT_LOAD` at the address the ELF asks for, copies the writable
  segments into private anonymous sections, applies `R_X86_64_JUMP_SLOT` and
  `R_X86_64_GLOB_DAT`, and resolves symbols by name. `/disk/bin/ldtest` calls
  `sqrt`, `fabs` and `floor` through the loaded pointers.

- **`section_open_range` backs a section with part of a file.** A `PT_LOAD` is a
  slice of the file, not the whole file, and mapping the whole thing puts the
  bytes at addresses that are not theirs.

- **`section_open` maps a file into a read-only section.** Given an open file
  descriptor it returns a section handle holding the file's bytes, so user mode
  can map a library image. Write access cannot be requested: the backing is
  read-only and writable segments are separate anonymous sections. Two processes
  asking for the same file share one set of pages.

- **`map_view_at` maps a section view at an address the caller chooses.** The
  base must be page-aligned and free. `map_view` is the same call with the base
  left to the kernel.

- **`savanxp_system_info` reports live section counts.** `sections_live` and
  `file_sections_live`, so a shared mapping is observable instead of assumed.

- **`SAVANXP_SECTION_EXEC` maps a section view as executable.** Execution is a
  permission of the view, granted by the section, instead of a hardcoded `NX` on
  every mapping. A view that asks for write and execute is still refused, with
  `EINVAL`. `mmap` keeps refusing `PROT_EXEC`: executable mappings go through
  `section_create`/`map_view` only.

- **`fscheck` reports SxFS consistency from inside the system.** It reconciles
  the block bitmap against the inodes in both directions and walks the tree for
  unreachable inodes, aliased entries and duplicate names. Read-only: it repairs
  nothing and runs against the mounted `/disk`. Exits non-zero on findings.

- **Celeste Classic is now an official port.** `./ports/ccleste/build.sh` installs
  `/disk/bin/ccleste` plus the game assets under `/disk/games/celeste`, building
  the upstream engine unpatched because the overlay replaces its SDL frontend.
  `./build.sh smoke ccleste-selftest` and `./tools/shoot.sh --scenario ccleste`
  cover it. Music is deferred until the OS has a dynamic linker, so that a
  program can reach a codec it was not built with.

- **The SDK now owns Doom-style pixel presentation and timed PCM mixing.**
  `sx_scaled_presenter` handles scaling/centering/row damage; opt-in
  `savanxp/audio.h` provides `sx_audio_mixer`, and Doom uses both.

- **Linux builds can now be driven by Bash and CMake.** `build.sh` drives the
  freestanding CMake graph, stages the rootfs, validates sibling SxFS candidates,
  and produces EFI/ISO artifacts through one native workflow.

- **Official ports now have a versioned layout.** Each port keeps its upstream
  pin, focused patches, SavanXP overlay, standalone build script, and persistence
  instructions; local experiments remain outside the official tree.

- **DoomGeneric now has a versioned official port.** `ports/doomgeneric` pins
  upstream, separates reviewed patches from the SavanXP overlay, builds with
  Bash, and installs through the persistent SxFS candidate flow.

- **The optional Haxe subsystem now has a standalone Linux build.**
  `subsystems/native/build.sh` pins Haxe/reflaxe, keeps the base CMake target
  independent, and installs native demos through the SxFS candidate flow.

- **Linux now has a headless smoke runner.** `./build.sh smoke <scenario>`
  injects the scenario into the initramfs, runs QEMU on a validated disposable
  image copy, records serial output, and supports QMP keyboard/taskbar input.

- **Linux smoke orchestration now covers audio, TCP, and external native apps.**
  Audio backends/WAV files, the TCP echo server, and `tools/build-user.sh` keep
  these scenarios outside the base CMake target.

- **Linux build commands now match the one-word smoke aliases and SDK install destinations.**
  Direct `windowd-smoke`/`gpu-soak` targets, `build-user.sh --destination`, and
  `tools/run-user.sh` cover the matching Bash workflows.

- **The host `sxfs-cli check` command validates SxFS metadata.** Linux image
  synchronization checks before and after candidate applies and fails closed on
  corruption, dirty/pending journals, or no-space conditions.

- **SxFS images now compact safely when fragmentation blocks an apply.** The
  Linux sync path preserves reachable external data, validates a same-size
  staging image, and atomically installs it without deleting the original first.

- **Stack canaries are randomized at kernel boot and for every new process.**
  `fork` inherits the parent guard, while `exec` installs a fresh one before
  `main`; the runtime no longer embeds a reusable cookie in each image.

- **Kernel security boundaries are documented and enforced.** NX/W^X, stack
  canaries, partial mapping ASLR and checked user/device/storage boundaries land
  together with the [kernel audit](docs/KERNEL_SECURITY.md).

- **Alt+F4 closes the active window,** the same as its X button: a dialog is
  cancelled, a main window closes with its process, and it also works for an app
  in fullscreen.

- **System name and version at the bottom right of the desktop,** above the
  taskbar, as classic desktop systems show them. Drawn with the wallpaper, so every
  wallpaper mode has it and no window is covered.

- **Dialogs are windows of their own.** Under `sxgui_app_run`, `sxgui_dialog_begin`
  opens a WM window owned by the app — framed, kept above its owner, which takes
  no input meanwhile — so Minesweeper's About no longer gets cut off at Beginner.
  New `gfx_window_open`/`gfx_window_close` and `SAVANXP_SYS_PIPE_SEND_HANDLE`/
  `_RECEIVE_HANDLE` (handle passing over a pipe), tested by `handletest`.
  [How it works](docs/OWNED_WINDOWS.md).

- **Media Player is now a system application in Accessories.** `/bin/mediaplayer`
  is always built, delegates to `/disk/bin/mediaplayer-ffmpeg` when installed,
  and otherwise opens an in-OS warning explaining that `ports/ffmpeg` must be
  built; the installed player keeps the audio/video controls and associations.
  [How it plays](docs/MEDIA_PLAYER.md).

- **The FFmpeg port decodes the common formats.** H.264, HEVC, VP8, VP9, MPEG-4,
  Theora and MPEG-1/2 video; AAC, MP3, Vorbis, Opus, FLAC and AC-3 audio; MP4,
  Matroska/WebM, AVI, Ogg and MPEG-PS/TS containers; plus swscale and swresample.

- **Gears (`/bin/gears`), in the Diagnostics group.** A hand-written software
  rasterizer — matrices, viewport, z-buffer, backface culling and flat shading
  with one directional light — drawing the three meshed gears. It is the
  consumer that defines the subset of the future SxGL, not a port.
  New `tools/shoot.sh --scenario gears`. [The plan it serves](docs/SXGL_ROADMAP.md#batch-0--the-consumer-before-any-api).

- **Fixed-size windows: `window_flags=fixed_size` in a `.sxres`.** The edges
  stop grabbing and maximize is drawn disabled, so a layout that cannot stretch
  no longer gets an empty margin around it; Minesweeper and Calculator declare
  it. The program still sizes its own window — in a fixed one *every* size hint
  is honoured, so changing Minesweeper's level resizes the window.
  [How it works](docs/WM_SUBSYSTEM.md#fixed-size-windows).

- **Floating point in every app of the image.** The in-tree userland compiles
  with `-msse2` and links the libm of `runtime/math.c`, so `double`, `%f`,
  `strtod` and `sqrt` work with no switch to remember; `--sse` stays for
  external apps. [The rule it replaces](docs/SYSTEM_LAYERING.md#an-in-tree-app-has-floating-point).

- **`docs/SXGL_ROADMAP.md`.** OpenGL does not grow out of SxGFX: SxGL would be a
  sibling layer binding to the window surface and `gfx_present_region`, as a
  runtime module of the SDK. Nothing is implemented.

- **Calculator (`/bin/calc`), in the Accessories group.** Four operations,
  `%`, `sqrt`, `1/x`, memory keys, copy/paste and the numeric keypad, over a
  decimal engine of 16 significant digits written in integers, so `0.1 + 0.2`
  is `0.3`. New `./build.sh smoke calc-smoke` and `tools/shoot.sh --scenario calc`.
  [Why its engine is decimal and integer anyway](docs/SYSTEM_LAYERING.md#an-in-tree-app-has-floating-point).

- **Minesweeper (`/bin/mines`), in the Games group.** The first game that comes
  in the image instead of being installed like Doom: three levels, LED counters,
  the face button, `?` marks, chord with both buttons, keyboard play and best
  times in `/disk/mines.ini`. New `./build.sh smoke mines-smoke`, and apps that paint
  their own content now get the system 3D edges from `sxgui_draw_raised_edge()`.
  [Why it is in C and not Haxe](docs/SYSTEM_LAYERING.md#games-and-the-first-one).

- **System master volume and mute.** `AUDIO_IOC_SET/GET_VOLUME` (0..100, 100 is
  identity) and `SET/GET_MUTED` on `/dev/audio0` scale every writer's PCM in the
  kernel; `/bin/volume` reads, sets and persists it to `/disk/audio.cfg`.

- **Taskbar volume button and popup.** The speaker left of the layout indicator
  opens a frameless slider with mute, applied live through the master ioctls;
  a second click or a click outside closes it, like the Start menu.

- **`sx_audio_mixer_set_voice_loop`.** A mixer voice can repeat its buffer
  instead of stopping at the end, for short music loops; starting a voice
  leaves it off, so looping stays opt-in per start.

- **Celeste Classic now plays music.** The port links PIE against
  `libffmpeg.so.0.4` and decodes the five OGG loops on first request into a
  dedicated looping mixer voice; without the FFmpeg port it runs silent.

- **Two programs can now sound at once.** `/bin/audiod` owns `/dev/audio0`
  and sums what clients send over UDP loopback; every writer falls back to
  direct turn-taking without it, so nothing changes where it does not run.

- **Per-application volume.** Streams carry names now, the daemon levels each
  one, and `volume list` / `volume set <app> <0-100>` drive the census; the
  mixer, the player, Doom, Celeste and the test tone all introduce
  themselves.

- **Per-application sliders in the volume popup.** Every application that is
  sounding gets a row with its own slider, refreshed from the daemon census
  once per second; levels are per session and are not persisted.

### Changed

- **Doom music now plays the WAD's OPL2 instruments.** With `GENMIDI` the song renders
  through a two-operator FM voice instead of the families, without it bit-identically;
  the smoke log names the path (`(FM)` vs `(familias)`).

- **The MIDI bank stopped being one waveform per family.** A voice now has a
  second partial (interval and weight), a blend toward sine, an optional vibrato
  LFO and an attack pitch sweep, and the drum kit uses all of it.

- **The calculator's arithmetic now comes from `libmath.so.0.4`.** Its own
  16-digit decimal engine existed because the in-tree userland was built
  `-mno-sse` and `double` did not compile. `sqrt` is now resolved from the
  shared library, so `calc` carries no copy of it.

- **The calculator shows 15 significant digits instead of 16**, and accepts 15
  digits of input instead of 16. A `double` carries about 15.95 decimal digits,
  so the sixteenth was not always true. `1/3 × 3` now reads `1` instead of
  `0.9999999999999999`, and `2/3` reads `0.666666666666667`.

- **Section budgets raised: 64 section views per process, 256 sections
  system-wide.** The old limits left no room for shared libraries, and 32 views
  per process was already tight for heap arenas alone. Exhaustion is still a
  clean `ENOMEM`.

- **Two `PT_LOAD` segments may now share a page.** Required by any binary linked
  with an interpreter, where the text segment does not end on a page boundary.
  The page takes the union of both segments' permissions, and a union that is
  both writable and executable is still refused.

- **The ELF loader reports which program header it rejected.** `bad elf segment`
  now carries the segment index, or 65535 when the rejection is not about any
  one segment. Without it, a `PT_LOAD` table with four entries fails as one
  opaque message.

- **The section cache keys on the file range, not just the inode.** Asking for
  the whole file and asking for a slice of it are different sections, and
  confusing them mapped a segment with the whole image behind it.

- **`savanxp_system_info` reports live section counts** and why the last view
  mapping failed, so a shared mapping is observable instead of assumed.

- **Window caption bars are two pixels taller.** The title area now has a 20px
  caption, giving the compact title text and caption buttons more breathing room.

- **The test machine now boots with 1 GiB of RAM**, and the persistent volume is
  1 GiB while the LiveCD carries 256 MiB. The image stays sparse, so it costs
  ~30 MiB of real disk.

- **SxFS format v2: 4096 inodes and a 1 GiB ceiling.** The inode table went from
  64 to 1024 sectors and the block bitmap from 32 to 512, so a volume is no
  longer capped at 64 MiB. `build/disk.img` is reformatted once; a v1 image does
  not mount, and `sxfs-cli` says so instead of failing obscurely later.

- **ATA now splits large requests instead of rejecting them.** A metadata commit
  asks for 1537 sectors and the PIO count register holds 8 bits, so writes over
  255 sectors were being refused — harmless while the metadata fit in 97
  sectors, fatal on the default IDE machine once it did not.

- **`fscheck` reports metadata commit cost.** `commits`, `bytes_written` and
  `bytes_per_commit` per volume, so the journal's cost is a measurement instead
  of an inference from the wall clock.

- **The development path no longer carries the persistent volume as a boot
  module.** `build/disk.img` reaches QEMU as an attached disk instead, which
  frees the 64 MiB it was occupying in RAM and drops the staged EFI tree from
  542 MiB to 30 MiB. The ISO keeps its own tree and its `livecd` volume, since
  there is no disk to attach, staged at its exact filesystem size so a grown
  image does not pad the ISO.

- **`build/disk.img` may now be larger than the volume inside it.** `sxfs-cli`
  accepts an oversized image and still rejects a truncated one, and a build
  preserves both the size and the sparseness of the file, so `truncate -s` is
  enough to make room for a later volume grow.

- **Window titles use RetroSans instead of Noto Sans.** The caption face is
  `RetroSans.ttf` at 18px with no faux-bold; the desktop body stays on Noto Sans 13px.

- **Active captions follow the declared accent instead of tinting the blue base.**
  A declared accent renders as its own shade-to-tint ramp; windows without one
  keep the Windows Standard gradient. Fixes the two-tone split on accents like Files.

- **init relaunches the audio daemon only when it dies serving.** A deliberate
  startup exit (no network, no audio, another daemon) leaves it stopped and the
  system on direct turn-taking instead of respawning forever.

- **The test machine now boots with 512 MiB of RAM**, up from 256 MiB, for
  `run`, `debug`, the smoke scenarios, `gpu-soak` and the ISO boot test.

- **Window title text is now one pixel smaller and bold.** Caption text uses a
  dedicated 12px Noto Sans face while the rest of the UI keeps its regular font.

- **Normal builds no longer regenerate desktop source icons.** Checked-in PNGs are
  treated as editable artwork; `tools/gen_desktop_source_art.py` remains available
  through the optional `savanxp_desktop_source_art` target.

- **Gears (`/bin/gears`) has a redesigned app icon.** The launcher and taskbar now
  use fuller 16×16 and 32×32 gear artwork.

- **The About app now identifies the system as the Alpha edition.**

- **The boot screen fades in from black, like a classic desktop splash.** Logo,
  name, progress bar and status line brighten together over about a second,
  driven by the monotonic clock; without a calibrated clock the splash appears
  at once.

- **Visual desktop verification uses the native Bash launcher.**
  `tools/shoot.sh` reuses the shared scenario driver over Unix QMP and a
  disposable SxFS copy.

- **Linux Program Manager and Add/Remove Programs smokes restore Doom when its
  external ELF exists.** The optional image remains unchanged when no ELF is
  installed.

- **Doom persistence now has an isolated Linux regression command.**
  `tools/verify_doom_persistence.sh` rebuilds a disposable image and compares
  the Doom binary, WAD, configuration, and savegame bytes.

- **ISO verification now covers El Torito plus BIOS and UEFI boot.**
  `tools/iso_boot_test.py` boots the generated image through both firmware paths
  and waits for the guest handoff token.

- **WM↔client protocol v4: the session opens all 12 windows.** Each costs
  `windowd` two descriptors instead of nine: one event pipe (`SAVANXP_WM_FD_EVENTS`),
  a wake event, one submit event per session, and hints and launches in the surface
  header (`gfx_should_close` is new). Apps no longer inherit `windowd`'s descriptors.
  **Breaks the client ABI:** rebuild external apps such as `doomgeneric`.
  [Descriptor budget](docs/WM_SUBSYSTEM.md#descriptor-budget).

- **The automated smokes honour `--accel`.** `./build.sh smoke smoke --accel kvm`
  runs the suite with KVM instead of TCG; the default remains `tcg`.

- **The system 3D edges moved to `savanxp/sxchrome.h`, out of the toolkit.**
  `sxchrome_draw_edge/_raised/_sunken/_inset/_etched`, `sxchrome_fill_raised`
  and the disabled relief (`_draw_text_disabled`, `_draw_glyph_disabled`) are in
  every binary's base runtime, so `windowd`, `calc` and `taskmgr` dropped their
  private copies. [Why not in SXGUI-C](docs/SYSTEM_LAYERING.md#the-two-layers).

- **The userland links with `--gc-sections`.** Each binary keeps only what it
  reaches from `_start` instead of whole runtime objects: the initramfs goes
  from ~14 MB to ~5.5 MB, libm included.

- **The managed app layer (Haxe on a VM) is deferred until after v1.0.** Until
  then everything ships in C against the POSIX SDK; `subsystems/native` stays as
  a frozen, validated experiment that nothing in the image depends on. If it is
  ever needed, it arrives as a selective port, UWP-style, beside the native apps.
  [The decision and the shape it would have to fit](docs/SYSTEM_LAYERING.md#the-managed-layer-is-deferred-to-after-v10).

- **About is now System Properties.** General shows the edition, the version,
  where it is installed and the processor (brand, speed and features from
  `CPUID`); Hardware lists the devices found. The live counters it used to show
  moved to the Task Manager.

- **The Shell window opens and repaints far faster.** `/bin/shellapp` dropped an
  unused 8 MiB static backbuffer, repaints only the text rows that actually
  changed instead of the whole surface, blinks the cursor without a repaint, and
  keeps scrollback in a ring. Monospace text blits ~16x faster system-wide. New
  `shellapp-stats:` line over `/dev/serial` and `tools/shoot.sh --scenario shell`.

- **The compositor accumulates damage as an exact region.** `windowd` stopped
  merging dirty rectangles by bounding box: dragging a window now repaints and
  presents the ring that changed, not the box around the old and new frames.

- **`tools/shoot.sh` drives other hardware and loads the display path.**
  `--virtio`/`--accel` (`tcg`/`kvm`) pick devices and accelerator; the
  `bench`, `saturate` and `spin` scenarios feed `windowd-stats`.

- **Launcher icon captions wrap to two lines.** A name that does not fit on one
  line breaks at a space and is centred over two, and only what still does not
  fit is cut with an ellipsis — before, a long caption was clipped at *both*
  ends and read as broken rather than as truncated. The cell now derives its
  height from the active font instead of a baked 76 pixels.

- **The boot screen is now a splash with the project logo.** Black background,
  `assets/brand/logo.png` baked in by `tools/gen_boot_logo.py`, the system name
  below it, a sliding block bar, and the step names now in English.

- **The splash bar keeps moving through the long boot steps.** It advances on
  `monotonic_ns()`, now calibrated at the start of boot by
  `timer::calibrate_monotonic()` instead of during the ACPI bring-up.

- **All repository documentation is in English, and the README leads with a
  quickstart.** The Linux requirements move to `docs/BUILD_LINUX.md`,
  `docs/README.md` indexes the design docs, and `CLAUDE.md` points at
  `AGENTS.md`.

- **The filesystem is now called `SxFS`, and the `2` leaves the name.** The
  constants and the namespace never carried the number (`SVFS_*`, `svfs::`), so
  the name said one version and the code another. The on-disk magic goes from
  `SVFS2` to `SXFS` (the journal's from `SVJNL2` to `SXJNL`) and `SXFS_VERSION`
  returns to `1`. **Every earlier image stops mounting**: `build/disk.img`
  regenerates itself, but a persistent VirtualBox/QEMU disk must be reformatted.

- **`assert()` reports when it fails.** It was `((void)(expression))`, so a
  failing assert went unnoticed. It now prints and terminates the process; with
  `NDEBUG` it still disappears.

- **`struct stat` has the fields third-party code names.** `st_uid`, `st_gid`,
  `st_nlink`, `st_blksize`, `st_blocks`, `st_atime`, `st_mtime`, `st_ctime`. The
  kernel only reports type and size, so the rest stay zero.

- **`exec` no longer copies the whole image into physically contiguous pages.**
  `elf::load_user_image` takes an `elf::ImageReader` and reads the file directly
  onto the process's pages: half the memory during exec, and no contiguous
  reservation that fails on a fragmented heap.

- **The user stack goes from a fixed 128 KiB to 1 MiB on demand, with a guard
  page.** Only 8 pages are mapped at startup and the rest appear when touched
  (`process::grow_user_stack`), so a shallow process uses LESS memory than
  before. An overflow now dies with a readable fault.

- **`spawn`/`exec` accept 128 arguments and 8 KiB total, instead of 15
  arguments of 63 characters.** The buffer now comes from the kernel heap
  instead of 4 pages of kernel stack, and overflow fails with `E2BIG` (new)
  instead of truncating silently.

- **Streams read in batches and `malloc` aligns to 16.** `fgets` was issuing one
  `read()` per character; file streams now fill a 512-byte buffer. 16 is
  x86-64's `max_align_t`: with 8, a `movaps` in an `--sse` app could fault.

- **The libc stops renaming with `#define`: the standard names are now real
  symbols.** The SDK headers defined `#define read sx_read` and ~140 more, so
  the preprocessor rewrote any identifier with those names — a `.close` struct
  field included — and `malloc` or `stat` were not symbols a third-party object
  could link. The raw syscall layer is prefixed `savanxp_*`; `math.h` and
  `setjmp.h` keep their macros on purpose.

- **One userland runtime and one `printf`.** `runtime/posix.c` is linked into
  every program and `runtime/libc.c` is reduced to the raw syscall layer plus
  gfx. Which formatter ran used to depend on the headers each `.c` included, and
  libc.c's did not even understand `%ld`/`%lu`. Raw `puts` becomes `puts_out`
  and `putchar(fd, c)` becomes `putchar_fd`.

- **`icon=` in `progman.ini` points at a program, not at an art catalog.** It
  stores a PATH and the icon is read from the binary it points at. The old names
  still work as aliases, and an explicit `icon=` beats the binary.

- **The sxgui chrome moves to the Win95 two-pixel bevel.** A period 3D border
  takes four tones — `SXGUI_COLOR_BEVEL` (223,223,223) was missing — and sunken
  is now the exact reverse of raised. Also dotted focus rectangle, 50% scrollbar
  trough, redrawn checkbox tick, concentric radio button, engraved disabled
  text.

- **The sxgui apps share a single layout grid.** The metrics live in
  `savanxp/sxgui.h` (`SXGUI_MARGIN`, `SXGUI_GAP`, `SXGUI_BUTTON_WIDTH`, ...) and
  are used by notepad, files, progman, aboutapp and widgetsdemo.

- **`tools/shoot.sh` gains the `files` scenario.** The explorer is the window
  with the most distinct controls at once, so it is where a stray toolkit margin
  shows up.

- **The keyboard event carries the modifiers.** `savanxp_input_event` gains
  `modifiers` (`SAVANXP_KEY_MOD_*`); the kernel already computed it and threw it
  away, so the WM tracked Ctrl by hand and got stuck if a KEY_UP was lost. The
  event grows from 12 to 16 bytes: **external apps built against the previous
  SDK need a rebuild** — Doom reads this struct.

### Removed

- **The SxMedia multimedia layer is withdrawn, and the Media Player is the
  optional FFmpeg port again rather than a system program.** A program can only
  use the codec libraries it was built with, so "one player that plays whatever is
  installed" cannot be arranged without a dynamic linker or a codec service. The
  design, the findings and the prerequisites to try again are in
  [docs/SXMEDIA.md](docs/SXMEDIA.md).

- **The alternate host compatibility surface is removed.** Its entry points,
  bootstrap/toolchain cache, non-native accelerator selection, and TCP QMP
  fallback are gone; Bash/CMake is the canonical build and test path.

- **The legacy SDK trees for Doom and FFmpeg are removed.** Doom and FFmpeg now
  live exclusively under `ports/doomgeneric` and `ports/ffmpeg`.

- **The `wavinfo` and `player` FFmpeg demos.** Every binary that links
  libavcodec carries all its decoders, so they became modes of the player:
  `mediaplayer --probe`, `--selftest` and `--gpu-hold`.

- **The icon set baked into `desktop_icons.h` is down to one.** Only
  `DESKTOP_ICON_DESKTOP` survives, as the safety net for a binary that cannot be
  read at all. The PNGs in `assets/desktop/icons/` stay as the catalog each
  `.sxres` references with `icon=<name>`.

- **The start menu strip art is deleted.** The menu was retired with the rest of
  the Win95 chrome, but the chain that drew it was still whole, down to a symbol
  nobody used.

- **The eight hand-written coreutils in `subsystems/posix/userland` are
  deleted.** `cat.c`, `echo.c`, `ls.c`, `mv.c`, `rm.c`, `sleep.c`, `true.c` and
  `false.c` were built by nothing: `/bin` gets them from the busybox multicall.

### Fixed

- **Celeste's effects no longer saturate at any volume.** Its 16-bit WAVs were
  decoded taking the high byte unsigned, shifting everything half scale (even
  silence played full-negative); the mixer also compresses past 0.75 FS now.

- **A jittery audio stream is concealed instead of zeroed.** While a stream has
  played a frame, the daemon repeats its last frame — scaled by its own volume —
  for up to 50 ms of jitter, so a brief packet gap does not click but a client
  that is gone does not keep the device busy either.

- **Changing the music no longer stalls the game for a second.** The Doom port
  synthesised a whole track before playing it, which costs ~120 ms natively and
  about a second under TCG; it now keeps a one-second lead and fills a
  quarter-second chunk per frame while the looped voice plays the buffer.

- **A missing library is now named deterministically, and counted.** It used to be
  whichever failed last, so the same image could name three different libraries for
  the same three absent files. It is now the first, plus a count of how many
  libraries could not be loaded — which is not the same as how many files are
  missing: two absent files can stop four libraries from loading.

- **The emulated disk transfers 32 bits per port access instead of 16**, which
  makes a 390 KB write ~26% faster (measured: 365930 and 401218 ms down to
  280703 and 278922). Most of that cost is not the port writes at all: dropping
  every cache flush changes nothing, and the per-sector cost on the device side
  is what remains. Bus-master DMA is the fix for that, and the kernel does not
  use it.

- **Four loader diagnostics can now fail.** `brokentest`, `missingtest`,
  `slottest` and `diamondtest` asserted only through `eprintf`, so they had
  nothing a smoke scenario could look for and none of them had one: they were
  built, shipped in the image and never run. Each now prints a success token,
  each has a scenario, and each was checked against a build with the loader
  logic it tests disabled — where it stops passing.

- **The launcher's baked table and the manifests agree.** The table said
  `Main`/`Games`/`Diagnostics` and every `.sxres` said
  `Accessories`/`System`/`Games`/`Diagnostics`, and had done for months without
  anything failing, because the real catalog is rebuilt by scanning the
  manifests. A plan B that disagrees with the plan A is a second answer to the
  same question; `progman-smoke` now fails if a group exists in one and not the
  other.

- **The visual smoke scenarios work on a virtio machine.** They drove an absolute
  tablet with relative moves, so their clicks landed on the wrong window, and they
  compared screenshots pixel by pixel including the cursor glyph, so an editor that
  had not moved read as one that had. `sxgui-smoke --virtio` was failing on "the
  wheel did not scroll the editor".

- **Moving the mouse over a virtio machine no longer freezes the VM.** Letting the
  cursor into the QEMU window was enough for the guest to take `#14 page fault` and
  halt. [What the fix was about](docs/VIRTIO.md#a-capability-never-stores-a-pointer-into-its-own-device).

- **`smoke_runner_layout_test.py` runs again.** It asserted two Media Player
  scenarios withdrawn with SxMedia, and a `remove_paths` entry from before
  `libffmpeg.so.0.4` moved to `/disk/lib`, so it had been red and was guarding
  nothing.

- **`progman-smoke` passes again.** It was failing on its 180 s timeout, not on
  anything it asserts. Copying the 390 KB stamped fixture into the volume costs
  365930 ms on its own — five metadata commits, each copying the whole 1537-sector
  metadata region twice — while the rest of the smoke adds up to 3 s. The timeout
  is now 900 s and the smoke prints what the copy cost, because a test that goes
  red without saying why is what made this read as a broken launcher.

- **Relocated global pointers in a PIE no longer become a pointer to the image
  base.** `R_X86_64_RELATIVE` took its value from memory, but `lld` leaves the
  slot at zero and stores the address in the relocation's addend. Every
  `stdin`/`stdout`/`stderr` and any other relocated global in a PIE program was
  affected.

- **`crt0` no longer loses `argc` and `argv`.** Running the library loader before
  `main` used the registers the kernel had put them in, so a program with
  arguments lost them. Every program that ran the loader with real work to do
  was affected.

- **The interpreter path no longer overwrites an argument string.** It was
  written above the `argv` pointer array, where the argument strings live; a
  program could read a path instead of its own argument.

- **PIE executables no longer declare a glibc interpreter path.** `lld` inserts
  `/lib64/ld-linux-x86-64.so.2` by default for `-pie`, a path from the host that
  builds and not one SavanXP has.

- **A shared library no longer resolves its calls against the wrong image.** All
  its `PT_LOAD` segments now share one load bias, and each segment's file bytes
  land at their `p_vaddr` instead of at the start of the mapping. A `PLT` in the
  text segment jumping through a `GOT` in the data segment was landing in
  unrelated code with no visible error.

- **A library that calls nothing from outside now loads.** A missing
  `DT_JMPREL`/`DT_PLTRELSZ` is skipped instead of treated as a failure.

- **The mouse no longer drifts away from the host's.** A `virtio-tablet` event
  now carries the true pointer position to userland, not only a delta, so an
  event dropped by a full `/dev/mouse0` queue re-places the cursor instead of
  leaving it offset for good. `savanxp_mouse_event` grows `absolute_x`,
  `absolute_y` and `flags` (`SAVANXP_MOUSE_FLAG_ABSOLUTE`); `windowd` assigns
  that position and still sums deltas for a relative mouse (PS/2).
  [The rule the event used to break](docs/VIRTIO.md#a-dropped-delta-is-not-a-dropped-position).

- **The AC'97 driver measures its own feed.** An `ac97-stats:` line every
  ~11 s of audio over `/dev/serial`: wall time against audio actually
  delivered, periods discarded because the ring was full, underruns, and the
  longest gap between two writes. [How to read it](docs/TIME.md#diagnosing-a-clock-problem).

- **`./build.sh smoke clock-smoke`.** Measures `uptime_ms` and `monotonic_ns` against
  the RTC while spinning and while idle, and fails only if a clock changes rate
  with idleness. [What it measured](docs/SYSTEM_MONITORING.md#ticks-are-guest-time-not-wall-time).

- **Sockets that ported code can use.** `recv()`/`send()` work on streams,
  `SO_RCVTIMEO`/`SO_SNDTIMEO` govern the wait through the new `setsockopt()`
  (0 = wait forever, replacing a hardcoded 5 s cap on reads), and a `write()`
  larger than one segment writes what fits instead of failing. A blocking
  socket `read()` now parks the process instead of spinning.
  [The contract](docs/NETWORKING.md#the-socket-contract).

- **A network adapter with no driver is reported as such.** `system_info` gained
  `net_hardware` and the PCI id of the adapter it found, so System Properties and
  the Task Manager stop saying "no adapter" on a machine that has one SavanXP
  cannot drive — VirtualBox's default Intel PRO/1000, for one.
  [What to do about it](docs/SYSTEM_MONITORING.md#a-network-adapter-can-exist-without-a-driver).

- **Task Manager (`/bin/taskmgr`), in the System group.** Live CPU, memory and
  handles per process with an End Process that asks first; meters, history
  graphs and totals in Performance; adapter throughput in Networking; `File >
  New Task` and `Shut Down`. `proc_info`/`system_info` gained the counters it
  reads (`cpu_ticks`, `memory_bytes`, `cpu_ticks_total`, `memory_free_bytes`).
  New `./build.sh smoke taskmgr-smoke`. [How it measures](docs/SYSTEM_MONITORING.md).

- **TCP survives a network that loses and reorders.** Segments are kept and
  resent with backoff (`SYN` included), out-of-order data is reassembled, and a
  dead connection now reports `ECONNRESET`/`ETIMEDOUT` instead of a clean end of
  file. New `./build.sh smoke tcp-smoke`, `NET_IOC_GET_TCP_STATS` and
  `NET_IOC_SET_TCP_FAULT`. [What it guarantees](docs/NETWORKING.md).

- **The mouse wheel works.** PS/2 negotiates IntelliMouse 4-byte packets (`ps2:
  mouse wheel enabled`), virtio reads `REL_WHEEL`, and the new `wheel` field of
  `savanxp_gui_pointer_event` scrolls lists, text views and scrollbars under the
  cursor. [Why it accumulates](docs/WM_SUBSYSTEM.md#the-pointer-channel-carries-a-wheel-and-it-accumulates).

- **The compositor measures itself.** `windowd` reports compose, sync and
  present times — present split into `ipc`/`svc`/`gpu`/`tl`, and `blk_us` for
  the total time blocked on `compositord` — plus damage area, over the new
  write-only `/dev/serial`. New `monotonic_ns()`; compositor protocol v2.
  [How to read it](docs/GRAPHICS_PERF.md).

- **Gfx Demo has an auto-motion mode.** `S` moves the box every frame without
  input, so the demo presents as fast as the compositor allows — the load that
  shows the display path's real ceiling.

- **Every core runs user processes, under one big kernel lock.** Each core the
  bootloader reports gets its own TSS, idle process and LAPIC timer (`smp:
  planificando en N de M cores`); `/bin/smptest` in `./build.sh smoke` proves the
  overlap. Task Manager usage is now of all cores together. Unmapped kernel
  pages are flushed from every core's TLB (`smp: TLB perezosa ok`). `--smp <n>`
  still defaults to 1. Phases 0-3 of [the SMP roadmap](docs/SMP_ROADMAP.md).

- **Add or Remove Programs (`/bin/appwiz`), in the System group.** It lists what
  was installed outside the system image — in `/disk/bin` and not in `/bin` —
  and deletes the binary, optionally with the data directory the program
  declares as `data_dir=` in its `.sxres`. New `./build.sh smoke appwiz-smoke`.
  [Why that is the rule](docs/SXE_FORMAT.md#uninstalling-the-other-half-of-the-same-idea).

- **Program Manager lists every installed program on its own.** A binary whose
  `.sxres` declares `category=` shows up under that group with nothing to
  register: installing is copying it to `/disk/bin`, uninstalling is deleting
  it. Without the key it stays launchable, just unlisted. `F5` (File >
  Actualizar) rebuilds the catalog, and launching an entry that is gone says so
  instead of nothing. [How it works](docs/SXE_FORMAT.md#all-programs-the-catalog-discovers-itself).

- **`virtio-net` driver for the NIC.** With `--virtio`, `./build.sh` builds
  `virtio-net-pci` instead of `rtl8139`, and `nic::` prefers
  `virtio_net::driver()`. Polling-only, no IRQ of its own.

- **`--accel kvm`.** `run`/`debug` accelerate against `/dev/kvm` with `-cpu
  host` on Linux. The smokes stay on TCG on purpose, to remain deterministic.

- **Audio capture (`virtio-sound` RX) and duplex `/dev/audio0`.** `read()` has
  its own session owner, independent of `write()`, so recording and playback can
  come from different processes. New `audiotest --record` and `virtio-record`.

- **Keyboard over `virtio-input`.** `--virtio` adds `virtio-keyboard-pci`; its
  `EV_KEY` events become scancode set 1 (`ps2::inject_scancode`) and reuse the
  existing layout logic. `ps2::` disables its own keyboard while it is active.

- **`virtio-blk` driver for the persistent disk.** With `--virtio` the machine
  gets `virtio-blk-pci` instead of IDE, appearing in `block:` as `vblk0`. It
  flushes after every write, or a "successful" write could stay in QEMU's cache.

- **The file list shows an icon per type.** The catalog is standalone `.sxicon`
  blobs in `/disk/icons`, mapped by extension in `diskfs/mimeicon.ini`. **This
  is extension to icon, not type detection: `MIME_OPEN` is still unresolved.**

- **Video player on FFmpeg: MJPEG decoding, YUV->RGB through swscale, on
  screen.** `./build.sh smoke ffmpeg-smoke` runs `wavinfo`, the player in `--selftest`
  and the player in `--hold`, which presents over `/dev/gpu0`.

- **FFmpeg runs inside SavanXP.** libavutil, libavcodec, libavformat and
  libswresample compile against this libc and really decode. Minimal codec set
  (WAV + PCM s16le), no asm and no threads; built separately with GNU make, see
  `ports/ffmpeg/README.md`.

- **`static_assert` in `<assert.h>`, the `PRI*`/`SCN*` in `<inttypes.h>`, and
  `fpclassify`.** Plus the missing errnos (`EDOM`, `EILSEQ`, `EPERM`, `ESPIPE`),
  `mkdir`, `F_SETFD`/`FD_CLOEXEC` and `imaxabs`/`strtoimax`/`strtoumax`.

- **`./build.sh smoke` runs `imagetest`.** It inspects itself to verify what a
  streaming loader can break: `.rodata` byte by byte, `.bss` zeroed and `.data`
  with its initial values, from `/bin` and from `/disk/bin`.

- **`./build.sh smoke` runs `stacktest`.** Deep recursion in the process and in a
  child, an overflow that must die with a `#PF` on the guard page, and a
  40-argument `argv`.

- **The libc gains what a port takes for granted.** Strings, integers, aligned
  memory, stdio (`fgetc`, `ungetc`, `fdopen`, `setvbuf`, `fseeko`, `perror`),
  `sscanf` with scansets, and a full calendar. Everything is UTC.

- **Real `%f`, `%e` and `%g`, and `strtod`.** `%f` was a stub that consumed the
  argument and wrote `0`. Now with precision, width, rounding carry and the odd
  cases (`nan`, `inf`, signed zero). All behind `__SSE2__`, like libm.

- **libm: the missing ones.** `asin`, `acos`, `hypot`, `cbrt`, `rint`, `lrint`,
  `lround`, `frexp`, `modf`, `scalbn`, `nextafter`, `fdim`, `fma`, `log1p`,
  `expm1`, the hyperbolics, `remainder` and their `float` variants.

- **`./build.sh smoke` runs `libctest`.** Exercises the libc surface a port
  consumes: standard names as struct fields, `qsort`/`bsearch`, streams,
  directories and the POSIX error convention (`-1` plus `errno`).

- **ES/EN keyboard layout selector, in the taskbar.** US QWERTY joins the baked
  ES map, chosen at runtime through `/dev/input0`
  (`INPUT_IOC_SET_LAYOUT`/`GET_LAYOUT`), saved in `/disk/keyboard.cfg` and
  applied at boot, before windowd. UI in `kbdlayoutpopup.c`.

- **`./build.sh smoke kbd-smoke`: the keyboard driver is tested on its own.** It closes
  the real PS/2 -> IRQ -> `kernel/ps2.cpp` -> `/dev/input0` path, which the
  harnesses that inject already-formed events do not cover.

- **`./build.sh smoke pointer-smoke --virtio`: the pointer is tested on its own.**
  The pointer twin of `kbd-smoke`: `mousetest --selftest` reads the real
  `/dev/mouse0` while the host moves the emulated tablet over QMP, and asserts the
  centre, a click and the opposite corner as screen positions. Nothing covered the
  `virtio-input` queue: `cursor-repro` injects events by hand.

- **SXE stamping is no longer opt-in: everything built for the system comes out
  in that format.** Coverage over the complete image is 68/68 installed
  binaries, in-tree and external (Doom, busybox) alike. Every program gets a
  `.sxmeta` with or without a `.sxres` — name, version, subsystem and the short
  commit as `BUILD_ID`; icon, accent, description and mimes are never invented
  and stay `.sxres` enrichment.

- **`icon_file=` in the manifest: the icon travels in the binary, not in
  `assets/`.** It takes a PNG relative to the `.sxres` and derives the two sizes
  the runtime needs. Mutually exclusive with `icon=`.

- **Floating point in userland, with `--sse` and a libm of our own.** Code using
  `float`/`double` compiled but did NOT link: under `-mno-sse` clang resolves
  each operation through compiler-rt soft-float helpers this system does not
  have. The switch is opt-in, and `math.h` declares the library under `#if
  defined(__SSE2__)` so the error appears at COMPILE time naming the function.
  New `./build.sh smoke float-smoke`.

- **Tab control in sxgui (`sxgui_tabs`).** The active tab is drawn taller and
  covers the page's top border, so both parts read as one sheet. Program
  Manager's group selector now uses it.

- **Taskbar at the bottom of the screen, listing open windows.** It is a WM
  CLIENT (`/bin/taskbar`), not windowd chrome — the explorer.exe model. No start
  menu and no notification area. Buttons keep a STABLE order by slot, not by
  z-order, or the next click would land on something else. What a client cannot
  know arrives over the new channels in `savanxp/wm_shell_protocol.h`.

- **`tools/shoot.sh`: visual verification of the session, headless.** It boots
  without a window, sends keys over QMP (holding modifiers, which the monitor's
  `sendkey` cannot) and captures PNGs. The `taskbar` scenario verifies pixels
  and is wired as `./build.sh smoke taskbar-smoke`. Not part of the build.

- **Alt+Tab to switch windows.** While Alt is held each Tab moves the selection
  and releasing it confirms; Alt+Shift+Tab goes backwards. The switcher shown is
  the Task List: no new UI.

- **Text selection in sxgui, with Cut, Copy and Paste.** Shift with the arrows,
  Home, End and the page keys extends the selection; click anchors, drag
  stretches. Ctrl+C, Ctrl+X, Ctrl+V and Ctrl+A; other Ctrl+letter combinations
  are consumed instead of typed, except with AltGr. Also exposed as
  `sxgui_textedit_copy`, `_cut`, `_paste`, `_select_all` and `_has_selection`;
  Notepad gains its Edit menu.

- **System clipboard, in `/dev/clipboard`.** The content is a value, not a
  stream: a `write` replaces everything and a `read` returns from the start.
  Over `SAVANXP_CLIPBOARD_CAPACITY` (8 KiB) it fails with `ENOSPC`. SDK wrappers
  `clipboard_set_text`, `_get_text`, `_get_info` and `_clear`.

- **Closing a window no longer freezes the desktop while the program is still loading.**
  `windowd` reaped the client with a blocking `waitpid` inside its event loop, so killing
  a program stuck in its loader stalled input, compose and present until it died.

- **The native SXGUI smoke label is now distinct from the native GUI host.**
  `sxguihost` no longer reports `NATIVEGUI HOST` when its runner exits.

- **CMake-generated UEFI images now include `startup.nsh`.** The file selects
  `fs0:\EFI\BOOT\BOOTX64.EFI` for firmware and removable-media boot paths.

- **Linux `gpu-soak` now uses the legacy 96-iteration default.** Explicit
  iteration counts derive a proportional timeout on both build frontends.

- **DoomGeneric verifies its vendored upstream tree before building.** The
  deterministic digest in `UPSTREAM` rejects accidental source drift.

- **`build.sh --jobs` now applies to every CMake build target.** Kernel,
  userland, ISO, smoke, run, debug, test and clean paths share the same limit.

- **Linux QEMU launches honor `SAVANXP_QEMU` and custom OVMF pairs.** Normal
  runs and smoke scenarios use the same explicit toolchain overrides.

- **Native Haxe `--force` now clears stale generated artifacts.** The normal
  path preserves reusable output directories instead of silently rebuilding
  from an identical clean state.

- **BusyBox is built with stack canaries enabled.** The port boots and runs its
  applets with the same runtime protection as other external applications.

- **SxFS directory reconstruction now uses a bounded 32-level depth.** The
  visited-cycle check remains, while recursive frames stay within the kernel
  stack budget used by the current 32 KiB stacks.

- **SxFS now rejects data extents claimed by more than one inode.** Metadata
  validation tracks a per-volume claim bitmap across the complete inode table.

- **SMEP and kernel write protection are enabled on supported processors.**
  Every application processor establishes the same execution boundary; SMAP
  remains a separate follow-up.

- **Initramfs archives that exceed the VFS node limit now fail closed.** The
  parser no longer silently discards entries and mounts a partial root filesystem.

- **RTL8139 accepts valid maximum-size frames and recovers from bad records.**
  RX validation now accounts for the CRC header and wrap tail, and resynchronizes
  with the hardware pointer instead of repeatedly parsing one malformed frame.

- **NX is now enabled explicitly on every application processor.** APs no
  longer depend on the per-core `EFER.NXE` state inherited from the bootloader.

- **The keyboard-layout popup can change the active layout again.** It opens
  `/dev/input0` for the write access required by the setter and no longer
  persists a selection that the kernel rejected.

- **Kernel stack-canary failures report the actual return address.** The panic
  diagnostic no longer assumes a compiler-specific stack-frame offset.

- **User memory is no longer uniformly executable.** NX is now enforced for data,
  stacks and shared sections; ELF segments must be valid, non-RWX and remain in
  the user half. Stack canaries cover kernel, in-tree, BusyBox and supported
  external SDK builds.

- **Malformed storage and boot images are rejected before use.** SxFS extents,
  inode metadata and directory graphs, CPIO names/hex/trailers, GPU geometry and
  device-reported packet lengths are now bounded before they reach kernel memory.

- **Mutating device ioctls require a writable descriptor.** Init and idle tasks
  cannot be killed, and reparented descendants no longer leave permanent zombies.

- **Restoring a maximized window no longer leaves residue on the wallpaper.**
  Only the restored frame was repainted, so the rest of the maximized area kept
  stale pixels until something passed over it. `windowd-smoke` now checks it.

- **`fork` hands the child the floating-point registers too.** It used to start
  with the clean FPU/SSE state the kernel seeds, so a `double` live across the
  call read as 0 in the child. `forktest` now checks it.

- **A command typed in the desktop Shell keeps its arguments.** The parser
  tokenizes the line in place, and the window then handed that same line to
  `/bin/sh -c`: only the first word survived, so `echo hola` printed nothing.

- **`netinfo` over `virtio-net` no longer kills the machine on VirtualBox.** A
  64-bit virtio MMIO field now goes in two 32-bit halves: a single 64-bit access
  is what VirtualBox answers with a guru meditation. [The rule](docs/VIRTIO.md).

- **QEMU starts from a path with a space in it.** `./build.sh run`, smoke targets,
  and `tools/shoot.sh` now pass OVMF, disk, and log paths as quoted arguments,
  so QEMU cannot interpret half a path as a second drive.

- **The kernel/userland build handles host paths with spaces.** CMake and Ninja
  receive quoted include and output paths, so a directory containing whitespace
  is passed as one argument instead of being split.

- **Closing a window that was playing sound no longer mutes the machine.**
  `windowd` kills the client, so the close handler of `/dev/audio0` ran in the
  killer's context and the device stayed owned by a dead pid: every later
  program got `EBUSY`. It is released by pid on exit, like the GPU session.

- **The wall clock follows the ACPI PM timer on machines that expose a usable
  one.** The TSC measures the host's time, not the virtual machine's: with
  VirtualBox under load it counted 561% of real time, and everything paced on
  it ran ahead — Doom offered three times the audio the device could take and
  the driver dropped the rest, which is what a chopped-up sound is. Needs a
  32-bit PM timer; QEMU's is 24-bit and stays on the TSC. [Why](docs/TIME.md).

- **The kernel's wall clock is a free-running counter, not a count of timer
  interrupts.**
  `uptime_ms` and every deadline — `sleep_ms`, `poll` timeouts, the TCP RTO —
  used to advance only when an interrupt was delivered, and on VirtualBox that
  delivery turns bursty once the machine idles: measured between 151 Hz and
  4000 Hz with the timer set to 1000. Anything paced on it stalled and then
  fast-forwarded. [How it was found](docs/SYSTEM_MONITORING.md#the-wall-clock-is-a-free-running-counter-ticks-only-count-cpu).

- **A process waiting in `poll()` wakes on the event, not only on the next
  tick.** Blocking `poll()` left the compositor noticing a client's frame up to
  a tick late, and that latency landed on every frame drawn.

- **`./build.sh build` no longer deletes `build/disk.img` when it cannot read
  it.** A locked image — a VM running on it — raised the same error as a corrupt
  one and fell into the branch that recreates it. It now reports and stops.

- **`poll()` blocks instead of spinning, and an idle machine halts.** The
  syscall parks the caller (`WaitReason::poll`) and the tick wakes it, so a
  process that only waits no longer books every tick of the wait: the idle
  desktop went from `windowd 99%` to `idle 99%`, `CPU Usage: 0%`, with average
  compose time ~4x lower. `yield` now halts for the idle process.
  [What it found and how](docs/SYSTEM_MONITORING.md#waiting-is-not-running-what-the-first-measurement-found).

- **Notepad's editor ignored the mouse wheel and its own scrollbar.** The
  wheel dispatcher and the embedded-scrollbar hit test only knew about
  `SXGUI_LISTBOX`/`SXGUI_TEXTVIEW`, not the editable `SXGUI_TEXTEDIT`; a click
  on the bar moved the caret instead of scrolling. `tools/shoot.sh
  --scenario notepadwheel` covers it.

- **`tools/shoot.sh` guarded the wrong path for a planted automation spec.**
  It checked `build/image/SMOKE` while `./build.sh` plants `build/rootfs/SMOKE`,
  so after any smoke target the guest silently ran that harness, not the
  desktop.

- **A blocking pipe read did not get the CPU when its data arrived.** Only
  event waits asked for the preemptive wakeup, so a synchronous RPC over pipes
  waited for the round-robin. Present drops ~470x, and 1.4 to 5.8 fps.

- **The taskbar drew the generic icon for every window.** The shell window list
  only carried a baked-in icon id, and that set is now just the generic one, so
  each button gets the window's own 16x16 `.sxicon` instead.

- **The ELF loader failed on two `PT_LOAD`s sharing a page.**
  `map_segment_pages` treated an already-mapped page as an error and the load
  died reporting "out of memory". The shared page now keeps the union of the
  permissions.

- **`argc` could exceed the arguments that existed.** The kernel copied at most
  15 arguments but handed the process the original `argc`, so a longer `argv`
  made the program read pointers that were never written.

- **`time()` returned the uptime, not the Unix epoch.** It computed
  `uptime_ms() / 1000` with the RTC right there, so any derived date came out as
  1970.

- **`strtoul` and friends wrapped silently on overflow.** A number larger than
  `ULONG_MAX` returned garbage; it now saturates and sets `ERANGE`.

- **`waitpid()` was infinite recursion.** The variadic macro in `<sys/wait.h>`
  expanded back into itself. Nothing in the tree reached it, but any port
  calling POSIX `waitpid` ate the 128 KiB stack with no net.

- **A read-only SxFS volume became writable again once published.**
  `sxfs::attach()` set the status to `mounted` without looking at how the mount
  had ended, so a volume whose journal could not be recovered accepted writes.
  New `./build.sh smoke sxfs-smoke` mounts a broken volume from a host test.

- **Two windowd bugs uncovered by the keyboard selector popup.** The click-down
  landed on the overlay window path, and the popup never entered the
  composed/retire signalling lists, so its second `gfx_present()` hung forever.

- **The SXE resource generator blurred an icon when enlarging it.**
  `collect_icons_from_file()` only detected the integer multiple when shrinking,
  so a 16x16 original derived its 32x32 with LANCZOS instead of NEAREST.

- **The Type column in files was clipped when the scrollbar appeared.** The
  width split subtracted a fixed 6 pixels instead of the two bevels plus the
  bar's 16. The bar is now always reserved, so columns no longer jump width.

- **Row text did not line up with its column label.** The header cell's bevel
  eats two pixels the row was not subtracting.

- **In widgetsdemo the free-standing scrollbar overlapped the second column.**
  The column origin was computed from the list width without counting the bar
  between the two.

- **Pressing Ctrl dropped the selection, so Ctrl+C copied nothing.** A modifier
  key produces its own event with `ascii` at zero, and the text widgets treat
  "any other key" as a reason to drop the selection. Modifiers no longer reach
  the widgets.

## [0.3.4] - 2026-08-28

### Added

- **The userland malloc no longer lives in a fixed BSS arena: it grows by asking
  the kernel for sections.** The single BSS arena was resident physical RAM per
  process from exec onwards even if the app never touched a byte. A 256 KiB
  bootstrap stays in the BSS and the rest comes from `section_create` +
  `map_view`. External apps stop forcing `-DSX_HEAP_SIZE`. **A normal `malloc`
  can now return addresses above 4 GiB.**

- **The whole VRAM aperture is mapped write-combining.** The firmware maps only
  the visible mode (4000 KiB of 16 MiB), and that was the ceiling for everything.
  `fb_gpu` now asks dispi how much VRAM there is and maps the complete aperture,
  choosing the WC index from `IA32_PAT` instead of assuming the layout. New in
  `vm::`: `kPagePat`, `map_kernel_device_memory`, `kernel_page_cache_flags`,
  `write_combining_page_flags`.

- **Double buffering by panning on the flat framebuffer (no tearing).** With a
  virtual height twice the visible one, dispi's `Y_OFFSET` picks which frame is
  shown; the compositor always composes onto the one not on screen. Every present
  flips, partial damage included, by reapplying the previous frame's damage on
  top of the current one. `boot::FramebufferInfo` gains `mapped_bytes`, which
  decides whether both buffers fit.

- **The flat framebuffer backend can change modes (Bochs VBE).** It detects the
  dispi interface (ports 0x1CE/0x1CF, implemented by QEMU's standard VGA and
  VBoxVGA) and advertises `MUTABLE_MODE_SETTING`. The ceiling is the native mode;
  changing modes needs the graphics session and no live imported surfaces, and
  releasing the session returns to native. New boot log: `fb_gpu: <W>x<H> native,
  mode-setting ...`.

- **The SXE format: executables carry their identity inside.** A `<name>.sxres`
  manifest is stamped into non-alloc ELF sections (`.sxmeta`/`.sxicon`), so the
  binary declares its own title, version, icons, accent and the extensions it
  opens. Program Manager, `windowd` and Files read from there instead of
  per-path tables that had to be recompiled. Format in `docs/SXE_FORMAT.md`; new
  `build.ps1 sxe-smoke` and `filesapp-smoke`.

- **Default button in dialogs** (`default_button` in `struct sxgui_dialog`).
  Enter triggers it and it is drawn with the period double border. Declared as
  Save in the notepad, OK in the About boxes and **No** in the shutdown one.

- **The notepad warns before losing unsaved changes.** Exit, New and Open ask
  Save / Discard / Cancel and resume the action after saving. Closing via the
  window's X still kills the process: the WM sends SIGKILL instead of requesting
  a close.

- **Notepad (`/bin/notepad`).** A Win95-shaped text editor: File/Edit/Search/Help
  menu, full text area and a status bar with the open file and a modified
  marker. New, Open..., Save, Save As...; F2 saves, F3 opens. 32 KB per document.
  It is the OS's first app that writes files.

- **Multiline editor in sxgui (`sxgui_textedit`).** Caret, insertion and
  deletion, Enter splitting the line, arrows preserving the column,
  Home/End/PageUp/PageDown, click positioning and vertical scrolling. No word
  wrap: long lines follow the caret horizontally.

- **A launch can carry an argument.** `savanxp_desktop_launch_request` gains an
  `argument` field the WM passes as `argv[1]`, exposed as
  `gfx_desktop_launch_arg()`. It is what enables "open this file".

- **Dialogs can start with focus on a widget** (`initial_focus` in `struct
  sxgui_dialog`). Without it a text input dialog opened with no focus.

- **Listbox with columns in sxgui (details view).** Setting `columns` draws a
  fixed header and splits each item by TAB, with optional right alignment
  (`SXGUI_COLUMN_RIGHT`) and per-cell clipping. Opt-in.

- **Size hint: every window starts at the size of its content.** A new
  client->WM channel (`SAVANXP_WM_FD_SIZE_HINT`, fd 11) the WM applies once at
  startup, clamped to the surface capacity. SDK: `gfx_request_content_size()` /
  `gfx_wait_content_size()`; in sxgui, `sxgui_app_autosize()` or
  `sxgui_app_set_content_size()`.

- **`svfs-cli rm`: something can be taken out of an SVFS2 image.** Files and
  empty directories only. Previously the sync was purely additive and removing a
  binary meant recreating the whole image.

- **Resizing windows by their edges.** Edges and corners drag and the cursor
  anticipates them (`RESIZE_H`/`RESIZE_V`), with the opposite edge anchored. Not
  for frameless, maximized or fullscreen windows.

- **Multi-shape Win9x-style cursors.** Eight shapes (`arrow`, `wait`, `text`,
  `move`, `resize-h`, `resize-v`, `unavailable`, `link`). Apps request theirs
  over `savanxp_desktop_cursor_hint` and the WM resolves by priority.

- **Factory wallpaper in `/disk/wallpaper.bmp`.** Committed in `diskfs/` and
  generated by `tools/GenerateDefaultWallpaper.py` with a fixed seed. It is
  changed from Program Manager's Options, which rewrites `/disk/desktop.cfg`.

- **`build.ps1 net-smoke`: automated NIC coverage.** PCI presence, a non-zero
  MAC, address and gateway, and ARP + ICMP against slirp's gateway requiring the
  tx/rx counters to advance. The network had no harness before.

- **`build.ps1 build -NoTestApps`: an image without the diagnostic apps.** Test
  binaries stay out of the rootfs (59 -> 32) and the launcher is built without
  their entries. The automation commands always include them.

### Changed

- **Window frames now use the Windows 95 caption treatment.** Active and inactive
  title bars use the period gradients, classic 16x14 caption buttons, and the
  shared system 3D palette; the Task List follows the same scheme.

- **Application accents tint the active title gradient.** The declared accent is
  blended more strongly into the dark end and more gently into the light end;
  inactive captions and the Task List keep the shared system colors.

- **Client controls sit closer to the window edge.** The shared inset is now 4 px
  for window content and 8 px for dialogs, keeping controls attached without
  losing their breathing room.

- **F11 lowers the scanout resolution instead of scaling in software.** The shell
  used to stretch a fullscreen app's 640x400 buffer every frame, at two integer
  divisions per output pixel. It now asks the compositor for the client surface's
  mode (new `SAVANXP_COMPOSITOR_MSG_SET_MODE`) and the pixels go 1:1. The mode
  belongs to the scanout, so it is restored on leaving fullscreen, on the app
  dying and on the compositor dying. Without mode setting, scaled as before.

- **The kernel's cached geometry is restored on every mode change.**
  `GPU_IOC_SET_MODE` now calls `ui::sync_framebuffer_geometry()`, which
  reprograms the absolute pointer's extent. With the old extent the cursor
  pointed elsewhere after a mode change.

- **`memcpy`/`memset` stop moving memory one byte at a time.** The tree's three
  implementations were C loops of one byte per iteration, and the tree is built
  without `-O`. They now use `rep movsq` / `rep stosq` plus the byte tail; 8
  bytes is the maximum with `-mgeneral-regs-only`. `windowd-smoke` drops from
  ~37 s to ~30 s end to end.

- **`cld` on every kernel entry and at the start of every process.** DF is part
  of the process's RFLAGS, so a program could make the kernel's string
  instructions walk backwards. It is the precondition for the memory routines
  above, and for turning on `-O2` later.

- **Full-rectangle blits are copied in a single pass.** `fb_gpu::blit_rect` and
  `sx_painter_blit_bitmap` issued one `memcpy` per row even when the rows were
  already contiguous in source and destination. Small dirty rects still go row by
  row.

- **Files opens files in the notepad.** Activating something that is not a
  program launches `/bin/notepad` with the path instead of refusing.

- **Files takes the shape of the Win95-era file explorer.** Address bar with "Up
  One Level", a details list with a Name/Size/Type header, and a two-pane status
  bar with the object count and total. The menu becomes File/View/Help.

- **Files no longer shows file contents.** The preview pane is gone: reading a
  file is an editor's job. Opening something unlaunchable now says so in the
  status bar instead of dumping the first bytes.

- **Display, audio, block and NIC pick drivers through a registry, not branches
  in `kernel_main`.** Each driver self-describes with `register_driver` and the
  HAL binds by priority: `bind_best` for display, audio and NIC; `probe_all` for
  block, whose devices coexist. API: `block::initialize` -> `block::probe_all`,
  `block::register_ramdisk` -> `ramdisk::attach_image`. New boot logs: `display:`,
  `audio:`, `nic:`, `block: N device(s)`.

- **Desktop assets: from System.Drawing (GDI+) to Pillow.** The three generators
  move to Python + Pillow, with the same generated C header and no ABI change.
  New build requirement on every platform: `python3` + Pillow.

- **Build migration to Linux: host paths and tools.** The separate builds use `/`
  instead of a literal `\`, and `Build-Iso` compiles the `limine` deployer with
  `make` when there is no prebuilt binary. New requirement on Linux/macOS: `make`
  + `cc`.

- **Program Manager no longer lists programs that are not installed.** New
  `progman_registry_prune_missing(exists)` discards items whose path cannot be
  opened and the groups left empty. It can leave the registry empty, which is the
  honest answer.

- **The build no longer regenerates the desktop art if nothing changed.** With a
  Pillow version different from the one that produced the committed PNGs, every
  build left spurious binary diffs in `assets/desktop/`. The generators now
  compare mtimes.

- **QEMU is assembled with "base" hardware by default.** Standard VGA, PS/2 mouse
  and AC'97 audio, so the fallback backends get exercised without leaving QEMU.
  `-Virtio` goes back to the paravirtualized path and applies to every command
  that launches QEMU.

- **The window manager was separated from the shell (NT 3.5 model).** The old
  `desktop` process managed windows and drew the Win95 chrome at once. The WM is
  now `windowd` and the shell is client processes: `shellui` for the background
  and `progman` for the launcher, with `/disk/progman.ini` editable without
  recompiling. Taskbar, start menu and desktop icons give way to the **Task
  List** (Ctrl+Esc). Contract in `savanxp/wm_protocol.h`.

- **SVFS2 has a single implementation, shared kernel<->host.** The on-disk format
  lives in `include/svfs/svfs_format.h` and is compiled by both the kernel and
  the host tool. `libsvfs/` adds the portable core and `svfs-cli`, which does
  **all** writes to `build/disk.img`; the legacy PowerShell writer was deleted.
  That double implementation was the historical source of the desync bugs.

### Removed

- **The Haxe variants of About and Files (`aboutapp-hx`, `filesapp-hx`) were
  retired.** They were AOT-chain validation demos, not official apps. They go
  with their harnesses, the `native-about`/`native-files` targets and the
  **Native** launcher group, plus the `haxe-toolkit/` widgets only they used. The
  `sx_sysinfo.c`/`sx_fs.c` runtimes stay: they are declared native ABI surface.

- **Deleted `subsystems/posix/userland/busybox.c`.** The hand-written multicall
  was built by nothing any more: the installed applets come from
  `vendor/busybox-port`.

### Fixed

- **`realloc` could hang while growing a block.** The in-place growth loop called
  `sx_merge_with_next` on an occupied block, and that helper does nothing unless
  the block is free, so the loop never advanced. `sx_absorb_next` now reports
  whether it absorbed anything.

- **The AC'97 driver truncated the userland buffer address to 32 bits.**
  `copy_period` took it as a `uint32_t`. It was latent while every audio buffer
  came from the ELF image or the BSS, but a section view is mapped at 64 GiB, and
  there `audio_write` returns EINVAL — a client that mutes on the first `write`
  error (Doom) stays mute for the session. `audiotest` now allocates its buffer
  with `section_create` so the 64-bit path is exercised.

- **A smoke left the automation spec stuck forever.** The build wrote `SMOKE`
  into the rootfs but never deleted it, so `init` started that runner instead of
  the desktop on every later build, until the next `clean`.

- **Opening an app briefly showed the empty window at the generic size.** The WM
  composed the window from the fork, without waiting for the first frame. While
  the client starts, the feedback is now the WAIT cursor.

- **Resize: the window ended up half black.** `resize_overlay_client_surface`
  cleared the surface **after** publishing the new dimensions, erasing the frame
  the client had just copied. It surfaced with the size hint, but the bug was in
  edge resizing all along.

- **The power button did not shut the machine down: the SCI was routed twice.**
  `acpi::start_sci()` and the uACPI glue routed the same GSI, overwriting each
  other. An interrupt line now admits several owners, which also covers two PCI
  links resolving to the same GSI. New `acpi::enable_power_button()`, called
  again after `uacpi_initialize` turns off every fixed event.

- **VirtualBox with I/O APIC enabled did not finish booting: xAPIC MMIO support
  in the local APIC.** `initialize_local_apic` only spoke x2APIC over MSR, which
  VBox never exposes, so with the I/O APIC active the firmware masked LINT0, the
  PIT fallback died and the boot froze on the splash. It now maps the MMIO window
  from `IA32_APIC_BASE` and restores LINT0 as ExtINT. New log: `cpu: local APIC
  in xAPIC|x2APIC mode (id N)`.

- **Key Test** overwrote the second help line with the first event, and **Mouse
  Test** clipped the last line of its panel.

- **The build did not compile on native Linux.** `svfs-cli` needs
  `_POSIX_C_SOURCE` for `fseeko`/`off_t` under `clang -std=c11`, and
  `New-SvfsManifest` built relative paths assuming a `\` separator, leaving a
  leading `/` that `svfs-cli apply` rejected. Neither affects Windows.

- **`exec`/`spawn` reported ENOENT for any load failure.** A perfectly installed
  binary failed with `no such file or directory` when the real problem was
  memory. Each step now reports its own reason: `ENOMEM`, `ENOEXEC`, `EIO`,
  `EACCES`, and the kernel logs the failing step and the free page count.

- **The WM hung entirely because of a client that did not drain its input.**
  Opening a second instance of a program froze the whole session: the WM writes
  input non-blocking, but the kernel ignored `O_NONBLOCK` on **partial** pipe
  writes.

- **The cursor froze with the Task List open.** Its handler consumed the pointer
  event and skipped the cursor repaint.

- **`apply` on `build/disk.img` failed with "no contiguous space" without being
  fragmented.** The buffer where `ensure_capacity` preserves the inode was on the
  stack and capped at 20400 bytes. The image also self-compacts now and apply
  retries.

- **`ps` printed the literal `%-13s` in the STATE column.** The busybox applet's
  `printf` did not support the `-` left-justification flag.

## [0.3.3] - 2026-07-09

### Added

- **Audio on VirtualBox: AC'97 driver + audio HAL with backends.** Audio only
  worked with `virtio-sound-pci`, so everything was mute on VBox, Doom included.
  The subsystem moves to a HAL mirroring the display one, with a
  backend-agnostic `/dev/audio0` and two backends: `virtio_sound` and the new
  `ac97` (`kernel/ac97.cpp`), driving the Intel ICH over bus-master DMA with
  pure CIV polling to dodge the INTx VBox never delivers.

- **LiveCD: a self-contained `/disk` in the ISO through a writable ramdisk.**
  `disk.img` travels as a second Limine module and the kernel exposes it as an
  in-memory block device, so the ISO boots with `/disk` mounted. It is
  writable-ephemeral: changes are lost on reboot. A persistent IDE disk keeps
  priority.

- **Doom with Freedoom (a free IWAD) on the LiveCD.**
  `sdk/doomgeneric/build.ps1` bakes `freedoom1.wad` into `/disk/games/doom/`, so
  the ISO ships a playable Doom with no proprietary content. The engine still
  detects `doom1.wad`/`doom.wad` if supplied.

- **IOAPIC/MADT layer and IRQ routing by GSI.** It parses the MADT, programs the
  redirection entries and routes GSIs to IDT vectors through the Local APIC
  (`ioapic::route_gsi` / `route_legacy_irq`), with vectors 50-63 reserved. PS/2
  migrates to it, falling back to the legacy PIC without a MADT. Prerequisite for
  the ACPI SCI and for INTx through `_PRT`.

- **ACPI: SCI routed through the IOAPIC + power button.** `acpi::start_sci()`
  enables ACPI mode, masks every GPE (no AML interpreter, to avoid storms on the
  level-triggered SCI), enables PWRBTN and routes the SCI. The handler triggers
  `acpi::shutdown()` (S5), with no graceful userland teardown.

- **uACPI vendored and integrated: a real AML interpreter.** A baked copy of
  v6.0.0 under `vendor/uacpi/`, with glue in `kernel/uacpi_glue.cpp` over
  heap/vmm/pci/timer/ioapic and time from the TSC, because interrupts are off
  during bringup. It runs alongside the hand-rolled ACPI;
  `uacpi_namespace_initialize()` is deferred to the events stage.

- **INTx routing through uACPI's `_PRT`, closed end to end.**
  `route_pci_intx()` resolves the link device to its real GSI by evaluating
  `_CRS` and programs the IOAPIC with the firmware's polarity and trigger.
  `rtl8139` moves to interrupt-driven over this path. Legacy INTx now arrives on
  q35+APIC, the bottleneck that had forced MSI-X for virtio-gpu.

- **Native subsystem — Phase 2: native ABI v1 + a real runtime.**
  `savanxp_native_abi.h` is the single source: a partitioned syscall space
  (`< 0x1000` delegated to posix, `>= 0x1000` its own), a mandatory version
  handshake (exit 132 on mismatch) and the first two native syscalls
  (`SXN_SYS_INFO`, `SXN_SYS_LOG`). The runtime gains a heap, `operator
  new/delete` and a mini freestanding `<memory>`, so Haxe classes run.

- **Native subsystem — `_std` override: real Haxe String and Array.**
  reflaxe.CPP's `_std` is exposed as build-generated `*.cross.hx` overrides,
  Haxe's official per-platform mechanism. Two codegen fixes without patching the
  pinned libs: a `Math.hx` shadow and the `UniqueLocalNames` preprocessor,
  because flattened sibling blocks collided two for-in counters in one C++ scope.

- **Native subsystem — gfx ABI + a GUI hello in Haxe.** A graphics syscall block
  (`0x1010`: GFX_INFO / ACQUIRE / RELEASE / PRESENT). The display is first-class
  ABI, with no `/dev/gpu0` and no ioctls, sharing `display::`'s internals and the
  per-pid session. Haxe's Float does not work in freestanding yet.

- **Native subsystem — compositor client protocol (windowed apps).** An
  `sxn_gui_*` layer speaking the v3 surface contract over fds 3..9, on baseline
  syscalls only. First native windowed app: `nativegui`, verified headless with
  `test/guihost.c` — also the first test of a subsystem switch through exec.

- **A generic waitable header in the Object Manager (`object::Header`).** The
  signalling state duplicated per type moves into the common base, so a new
  waitable type no longer forces touching the wait dispatcher.

- **A real semaphore (`SAVANXP_SYS_SEMAPHORE_CREATE`/`_RELEASE`).** The first use
  of the generic header: it saturates at `max_count` and rejects a release that
  would exceed it, with SDK wrappers and `semaphoretest` in the `smoke` suite.

- **`build.ps1 run`/`debug`: `-Accel whpx` support.** It forces `-cpu qemu64`
  because `-cpu max`/`host` under whpx crash OVMF with a `#GP` in `PlatformPei`.
  The automated targets stay on TCG on purpose.

- **`virtio-sound`: the end of silent muting.** If the device responds but no
  output stream offers the ABI's fixed format, the failure is logged instead of
  leaving `/dev/audio0` unregistered without a trace.

### Changed

- **Kernel+userland compilation through Ninja.** It replaces `build.ps1`'s
  sequential, non-incremental phase (~250-300 sources) with parallel builds and
  header dependency tracking through `-MMD`, pinned in the toolchain. The link,
  the image, the ISO and QEMU are unchanged.

- `savanxp_mode_bits` (SDK) loses the enum's fixed underlying type, which
  triggered `-Wfixed-enum-extension` in every TU including `syscall.h`. No
  functional change.

### Fixed

- **`virtio-gpu`: `SET_SCANOUT` hung forever under WHPX.** The boot got stuck at
  "Preparing display": the second wait tier fell to `HLT`, and during early boot
  the kernel runs with `IF=0`, so it only wakes on an NMI. The wait now uses
  `timer::monotonic_ns()` with a bounded busy spin.

- **virtio-sound: async multi-buffer TX playback.** The TX path sent one period
  and spun waiting for the device with interrupts disabled. The queue now uses a
  ring of `kTxSlots` periods and drops instead of blocking when it is full.

- **AC'97: non-blocking playback, a silence cushion and no IOC.** Three fixes for
  choppy audio on VirtualBox: `submit_period` stops spinning with IRQs off (it
  froze the guest clock Doom uses to pace audio *and* its logic); the BDL entries
  lose the interrupt-on-completion bit nobody serviced; and periods of silence
  are preloaded to absorb producer jitter. New `ac97-count` and `ac97-stream`.

- **"Robotic"/stuttering audio in Doom (underfeeding the device).**
  `DG_Sound_Update` advanced every channel's position but wrote only the total
  rounded down to the period, feeding the device at ~0.75x and running effects
  ~1.34x fast. It was in the glue's common layer, so it sounded the same on both
  drivers.

- **Visual cursor residue over compositor elements.** `sx_painter_draw_frame`
  drew the frame of the rect already intersected with the clip, so a fragmented
  repaint gave each fragment its own border. It now traces the original rect as
  four clipped strips. New headless regression `build.ps1 cursor-repro`.

- **Polish for `fb_gpu` and `virtio-gpu`.** `present_region` read the origin from
  row 0 instead of the surface offset and `GET_STATS` returned zeros;
  `refresh_scanouts` no longer clobbers a fullscreen flip; the cursor queue's
  `used` header is read volatile; `notify_off_multiplier == 0` no longer skips
  the notify; and `REFRESH_SCANOUTS` requires the graphics session.

- **`build.ps1` silently forked to PowerShell 5.1 to generate assets.** `&
  powershell` always resolves to Windows PowerShell, so running everything with
  `pwsh` 7 gave a false sense of portability. The scripts are now invoked
  in-process.

- **`Join-Path` paths with an embedded `\`, incompatible outside Windows.** On
  Windows it worked by accident; on Linux `\` stays a literal character. ~18
  occurrences normalized to `/`.

## [0.3.2] - 2026-07-03

### Added

- **Complete sxgui: a Win9x-style widget toolkit.** From 5 basic controls to a
  full toolkit, keeping the allocation-free retained-mode model: focus traversal,
  a textfield with a real caret, scrollbar and scrolling listbox, action reasons
  (`CLICK`/`CHANGE`/`ACTIVATE`), radio groups, a combobox drawn inside its own
  backbuffer, a menu bar with `on_command(id)`, modal dialogs as a state machine
  in the same main loop, groupbox, progress bar and textview.

- **The `sxgui_app` app frame.** It encapsulates the gfx session and the main
  loop every widget app repeated (input polling, RESIZED, gated repaint, present,
  16 ms throttle), with optional `on_key`/`on_paint`/`on_resize` hooks.

- `widgetsdemo` grows into a reference gallery of the whole toolkit.

### Changed

- **`aboutapp` and `filesapp` ported to sxgui.** aboutapp becomes declarative;
  filesapp keeps its filesystem logic but delegates the list, preview, menu bar
  and status bar. Both lose their static 8 MiB backbuffer and manual event loop.

- **The SDK's malloc arena drops from 48 MiB to 8 MiB by default.** The static
  heap lives in the BSS and the kernel maps the whole BSS at exec, so each app
  cost ~50 MiB resident and three of them exhausted physical memory. External
  builds keep 48 MiB through `-DSX_HEAP_SIZE` for heavy apps like Doom.

### Fixed

- **Physical memory leak on fork from section view pages.**
  `vm::clone_address_space` copied every present user page and only then
  discarded the section view ones, without freeing the copy. Since the desktop
  maps every client surface's view, each launch lost ~4 MiB per view and after a
  few of them no `exec` worked until reboot.

- `desktop_client.path` stored the pointer received at launch, which for
  client-requested launches pointed at the request's stack buffer: the window
  title and the logs read dangling memory. The client now keeps its own copy.

## [0.3.1] - 2026-07-02

### Added

- **Separate graphics compositor (`/bin/compositord`).** Direct `/dev/gpu0`
  access, display surface import, batched presents, the present timeline and the
  hardware cursor moved into a userland daemon. `desktop` starts it with pipes
  and an inherited framebuffer section and speaks a versioned binary protocol
  (`savanxp/compositor_protocol.h`). The shell keeps window policy, chrome and
  input routing but no longer issues GPU ioctls.

- **GPU HAL: a swappable display backend.** `namespace display` goes from a fixed
  passthrough to `virtio_gpu` to a real `display::Backend` vtable, with
  `/dev/gpu0` registered by a backend-agnostic dispatcher
  (`kernel/gpu_device.cpp`). New `kernel/fb_gpu.cpp` backend: software
  composition straight onto Limine's linear framebuffer, for when there is no
  virtio-gpu. `kernel_main` autodetects.

- **Composited fullscreen for apps (F11 key).** A fullscreen-capable app (Doom,
  Gfx Demo) goes chromeless: the shell scales its 640x400 buffer and presents
  through `compositord`, with no mode change and no client scanout flip. It works
  over virtio-gpu and over the flat framebuffer alike.

- **Native subsystem (Haxe) — Phase 0 kickoff.** The chain is proven end to end
  at compile/link level: `Main.hx` -> `reflaxe.CPP` -> C++17 -> freestanding
  clang++ -> a native ELF. New `subsystems/native/` with a seed SDK and a
  separate `build.ps1` that pins reflaxe under `toolchain/haxe-libs/`.

- **Native subsystem — Phase 1: real native processes.** A native binary is
  marked with `e_ident[EI_OSABI]=0x53` and the loader assigns
  `subsystem::Id::native` by the binary's ABI, not by inheritance from the
  parent. Its syscalls enter through `dispatch_native_syscall`, which delegates
  the baseline to posix and stands as the point of divergence.

- Real typefaces baked offline into C tables with `tools/font/genfont.py`: **GNU
  UniFont 8x16** for the kernel console and the terminal, and proportional
  antialiased **Noto Sans** for the desktop chrome. The OS still does not parse
  TrueType at runtime.

- A monospace text path in `sxgfx` (`gfx_blit_text_mono`,
  `gfx_cell_width/height`) and per-pixel alpha blending (`gfx_pixel_blend`) for
  Noto's antialiased text.

- Real interrupts through **MSI-X** in `virtio-gpu`, with an ISR/DPC pattern
  instead of pure polling. The kernel has no IOAPIC and q35 in APIC mode does not
  deliver legacy INTx, so MSI-X is the only path. Includes
  `pci::find_capability`, `virtio_pci::enable_msix` and IDT vector 49.

- `poll()` reports readiness for kernel waitable objects (events, timers), so the
  compositor can wait for its clients' submit events in the same poll set.

- An on-screen FPS and present latency overlay in Doom's backend, and a dump of
  the driver's per-stage stats in `gputest --soak`.

- A `PIT` timer backend (8254, IRQ0) as a fallback when the local APIC does not
  support x2APIC or calibration fails; previously the scheduler never started
  there. `savanxp_system_info` exposes the active backend
  (`SAVANXP_TIMER_LOCAL_APIC` / `_PIT` / `_NONE`).

### Changed

- The desktop compositor composes by **regions with occlusion culling**: it
  builds the layer list in z-order and paints each one once over its visible
  region, eliminating overdraw under opaque windows. New
  `sx_rect_set_subtract_rect`; `sx_rect_set` capacity goes from 32 to 64.

- The kernel console moves from the hand-authored 5x7 bitmap font to UniFont
  8x16, with ASCII + Latin-1 + box drawing through a sparse table.

- `gfx_blit_text` rasterizes Noto Sans from an 8-bit coverage atlas; `shellapp`
  uses the UniFont monospace path. The desktop chrome metrics were readjusted for
  the larger line height.

- UniFont is baked from `unifont.hex` (crisp on-grid bitmaps), not from the TTF
  outline, which rasterized off-grid with artifacts.

- The kernel timer goes from 200 Hz to **1000 Hz**, with the scheduler quantum
  rescaled to keep ~20 ms of wall clock. Signalling an event now yields to the
  woken thread on the syscall return instead of waiting for the next tick.

- The compositor wakes on a client's frame submit instead of exhausting the
  16 ms timeout, which remains as a backstop.

- The `virtio-gpu` driver becomes interrupt-driven: the present waiters' spin is
  cut from 50000 to 2000 iterations before halting, and the backstop timeouts are
  expressed in wall-clock milliseconds to survive the tick rate change.

### Removed

- The hand-generated 8x8 font system (`tools/font/genfont.ps1`, `font8x8.txt`,
  `gfx_font8x8.inc`), replaced by the `genfont.py` toolchain.

### Fixed

- `release_surface_allocation` (`virtio-gpu`) freed the primary surface's backing
  without a RESOURCE_UNREF of the host resource, so the first real runtime mode
  change failed with `RESOURCE_CREATE_2D` -> INVALID_RESOURCE_ID.

- `decode_bar_size` computed `~mask + 1` in 64 bits over a mask holding only the
  low 32 bits, returning garbage sizes for any 64-bit memory BAR under 4 GiB and
  preventing them from being mapped — the MSI-X table among them.

- **Inode corruption from rounding in the SVFS2 bitmap.** The host-side installer
  computed the byte index with `[int]($Bit / 8)`, which in PowerShell rounds
  instead of truncating, desyncing from the kernel's floor indexing and
  reassigning live inodes (symptom: "expected inode N but read 0"). Now `-shr 3`.

- An orderly QEMU shutdown in `Run-AutomationQemu`: `quit` over the HMP monitor
  instead of `Stop-Process -Force`, so QEMU flushes its block backends and closes
  the disk file cleanly, with a fallback to the forced kill.

- The OS hung indefinitely after "Starting welcome" on hypervisors without
  x2APIC: `initialize_local_apic` failed silently and the scheduler never
  started. Resolved by the `PIT` fallback above.

- **Erratic cursor: PS/2 packet framing corrupted in streaming.**
  `process_mouse_byte` discarded every `0xFA`/`0xFE` assuming ACK/RESEND, but in
  streaming those are also legitimate deltas (`-6` and `-2`), and losing the byte
  desynced the 3-byte framing. The bug was directional — only left/down. A
  defensive clamp of +-150 per axis clips instead of discarding the packet.

- `reserve_kernel_mmio_window` reserved a PML4 entry without installing the
  corresponding PDPT, so any later `map_kernel_mmio` over that window failed or
  corrupted memory.

- `fb_gpu`'s present timeline always returned `submitted_sequence = 0,
  retired_sequence = 0`, so `wait_present` depended on a lucky coincidence
  instead of a real counter.

- **Experimental VirtualBox compatibility (`VBoxVGA` backend).** The system now
  boots stably to a graphical session and the PS/2 mouse responds in all four
  directions. Open and undiagnosed: the binaries in `/disk` still do not start
  under VirtualBox even with the SVFS2 volume attached.

## [0.3.0] - 2026-06-18

### Added

- An automated headless graphics smoke for the compositor: `desktop --selftest`
  starts the compositor, imports the scanout, launches a real client and
  deterministically validates multi-window composition and the advance of the
  GPU's present timeline, also exercising maximize/restore/minimize/move.
  Exposed as `.uild.ps1 desktop-smoke` (token `DESKTOP SMOKE PASS/FAIL`),
  closing a graphics path that used to be tested only by hand.
- New desktop client apps: `filesapp` (browsing `/disk` with a preview) and
  `aboutapp` (a system summary), wired into the Start menu.
- Explicit policy and traceability for adopting components inspired by
  SerenityOS, with new documentation in `docs/THIRD_PARTY_ADOPTION.md` and
  `docs/THIRD_PARTY_PROVENANCE.md`.
- A reusable 2D graphics layer `sxgfx` in the POSIX SDK v1, with `sx_bitmap`,
  `sx_painter`, alpha blending, clipping and `sx_rect_set` for handling multiple
  damage rects from userland.
- A `display` facade inspired by `DisplayConnector` over the `virtio-gpu`
  backend, with connector properties, surface import/release, present batching,
  a timeline and waitable events exportable to userland.
- The `SAVANXP_GPU_CLIENT_SURFACE_VERSION_3` graphics contract for client apps,
  based on `section_create/map_view` plus dirty rect batches and
  `submit/retire/shutdown` events instead of the legacy present pipe.
- A new, wider public ABI on `/dev/gpu0` for explicit present tracking, batching
  and connector capabilities, including the timeline, waits/events and
  property/scanout queries without owning the display.
- A real multi-window desktop compositor, with simultaneous overlays, simple
  z-order, an active window, dragging from the title bar and a close button in
  the top right corner.
- A bitmap asset pipeline of our own for the desktop, with embedded PNG icons
  and art generated inside the repository for the taskbar, Start menu, title bars
  and side strip, removing the dependency on SerenityOS assets.
- Wider `virtio-gpu` statistics with end-to-end per-frame latency, including
  accumulated samples and the worst case observed.

### Changed

- The system reports itself as `v0.3.0` in the kernel, the shell, `uname`,
  `sysinfo`, `aboutapp` and the other components consuming the shared version.
- `virtio-gpu`'s background progress stops depending on the input subsystem and
  is pumped from a kernel device service invoked on the timer and on blocking
  waits.
- The `desktop` leaves behind the single fullscreen client model and moves to a
  surface compositor with multi-rectangle invalidation, composition by clipping
  and batched presents to the primary scanout.
- The `desktop`'s main loop is decoupled into layout/render/menu/session, keeping
  a single binary but better separating the responsibilities of the compositor,
  the background shell and the overlay windows.
- `shellapp`, `doomgeneric` and the other client apps migrate to the async
  version 3 surface channel; the terminal forces a full redraw when scrolling
  moves the history, to avoid visual artifacts in the window.
- Compositor-GPU synchronization moves to an explicit timeline and to retiring
  the previous frame before recycling the visible backbuffer, reducing logical
  tearing and improving the desktop's pacing.
- The Start menu and the taskbar get several passes of visual and behavioral
  polish: a cleaner layout, a bitmap sidebar, text that fits better, stable
  hover and better feedback for the active client.
- Overlay windows can now be moved inside the desktop's work area and closed
  straight from their title bar with a classic shell-style cross.
- The desktop's embedded art is now generated from the repository's own assets,
  replacing the temporary references used during the SerenityOS-inspired
  prototyping.
- `gputest --smoke` now also validates the present timeline and the driver's
  explicit pacing, not just internal stats/stages.
- The persistent `build/disk.img` flow is re-validated after every large batch of
  changes, with `doomgeneric` as the real non-regression test.
- `virtio-gpu` reorganizes its internal state around an `Adapter` with separate
  substates for transport, display, cursor, presents and runtime, leaving a
  better base for fine-grained locking and more predictable recovery.
- `virtio-gpu`'s background work is split into explicit phases of queue draining,
  pipeline advance and config event processing, with short atomic serialization
  for virtqueue submit/drain.
- The present timeline now recognizes `present_cookie` as a real correlation
  even with coalescing, range retirement and damage batching.
- The partial presentation path can now update the active front buffer without a
  full clone when the resource is idle, and imported batches keep real rects for
  `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH` before falling back to the bounding
  rect only if the internal capacity runs out.
- The `desktop` starts using the waitable present event exported by `/dev/gpu0`
  as a readiness hint to reduce unnecessary polling of the timeline, keeping
  `WAIT_PRESENT` as the strong synchronization.
- `virtio-gpu`'s display/scanout event handling is hardened: a failed refresh now
  triggers deliberate degradation and recovery instead of staying a silent
  failure of the hotplug/config path.
- `gputest --smoke` raises driver coverage by validating waitable present and
  scanout handles, together with a light soak of partial presents and repeated
  refreshes to catch pacing and recovery regressions earlier.
- The surface reservation and pending slot timeouts in `virtio-gpu` are now
  treated as symptoms of a real pipeline stall, entering degraded mode and
  attempting recovery just like the other critical waits.
- `virtio-gpu`'s recovery avoids simultaneous reentry and `gputest --smoke`
  hardens event coverage by also checking that the handles go back to unsignalled
  after `event_reset`, with a somewhat more aggressive soak.
- `gputest` adds a dedicated `--soak` mode to exercise the `/dev/gpu0` backend
  for longer with a deterministic mix of full presents, partial presents, event
  waits and scanout refreshes, without slowing down the normal smoke.
- `build.ps1` adds the `gpu-soak` command, reusing `/SMOKE` as the runner
  selector so `init` can launch `gputest --soak` in QEMU and report `SOAK
  PASS/FAIL` tokens suitable for automated validation.
- `virtio-gpu`'s recovery becomes less global in non-critical domains: a failure
  to rearm the cursor now degrades to a software cursor and non-critical imported
  surfaces can be dropped locally during recovery instead of tipping over the
  device's whole rearm.
- `virtio-gpu`'s scanout refreshes become transactional over the connector
  cache: if the host delivers an incomplete event or rearming the primary fails,
  the driver restores the previous state and only escalates to global recovery
  when it cannot even keep the primary scanout.
- `gputest --soak` now accepts an iteration count and also covers imported
  surfaces through `GPU_IOC_IMPORT_SECTION` and
  `GPU_IOC_PRESENT_SURFACE_BATCH`, with `build.ps1 gpu-soak -GpuSoakIterations N`
  to repeat longer runs without touching the normal smoke.
- The `desktop` compositor fixes the pacing of its imported presents by using
  the real timeline to generate `present_cookie`, avoiding retiring frames before
  the GPU is done and reducing visible tearing in heavy apps.
- The SDK's `gfx` runtime stops exposing as a direct backbuffer the same shared
  buffer the compositor reads: each client draws into a private backbuffer and
  the runtime copies to the shared surface only once the previous frame has been
  composed, eliminating the frame backlog without waiting for the GPU's final
  retirement.
- The client surface protocol adds `composed_sequence` to separate desktop
  composition from GPU retirement; the compositor signals progress as soon as it
  copies the frame into the visible backbuffer and keeps `retired_sequence` for
  the present's real retirement.
- The `desktop` limits how many mouse events it drains per frame so that
  dragging windows does not accumulate a large backlog before composing again.
- `poll()` stops treating every device as always readable: `/dev/input0` and
  `/dev/mouse0` expose real queue readiness, reducing the compositor's busy loop
  when there are no pending events.
- The `desktop`'s mouse handling moves to reading blocks per frame and coalescing
  consecutive movements with the same button state, following SerenityOS's
  WindowServer pattern so as not to compose a frame per raw packet.
- `sx_rect_set` fixes the merging of adjacent rectangles so it does not join
  separate areas that merely share an edge coordinate, avoiding artificially huge
  dirty rects when moving windows.
- `doomgeneric` and the compositor now print the concrete present or invalid
  batch error when a client surface fails, making visual regressions easier to
  diagnose.

## [0.2.2] - 2026-04-01

### Added

- A new `subsystems/` tree with `subsystems/posix` as the OS's first explicit
  subsystem, separating `kernel`, `userland` and `sdk` under a single ownership.

### Changed

- The POSIX syscall entry and dispatcher move under `subsystems/posix/kernel`,
  while `kernel/` keeps the scheduler, base processes, VM, VFS, drivers and the
  other generic mechanisms.
- The canonical SDK v1 moves to `subsystems/posix/sdk/v1`; the main build,
  `tools/build-user.ps1` and the VS Code extension now consume that path as the
  POSIX subsystem's public reference.
- The internal userland moves to `subsystems/posix/userland` and builds against
  the SDK's shared canonical runtime, removing internal duplicates and leaving
  top-level `sdk/` as the root for examples and ports.
- The public definition of the visible ABI is unified in
  `subsystems/posix/sdk/v1/include/savanxp/syscall.h`, with no parallel copy in
  `include/shared`.
- The system reports itself as `v0.2.2` in the kernel, the shell, `uname`,
  `sysinfo` and the components consuming the shared version.
- The migration re-validates the persistent image flow:
  `.\sdk\doomgeneric\build.ps1` and `.\build.ps1 build` still keep
  `doomgeneric` and `doom1.wad` in `build/disk.img` without recreating the image
  under normal operation.

## [0.2.1] - 2026-03-30

### Added

- A new public ABI on `/dev/gpu0` for diagnostics and extended 2D control:
  `GPU_IOC_GET_STATS`, `GPU_IOC_GET_SCANOUTS`, `GPU_IOC_REFRESH_SCANOUTS`,
  `GPU_IOC_SET_CURSOR` and `GPU_IOC_MOVE_CURSOR`.
- Wider `virtio-gpu` statistics for presents, stages, waits, timeouts,
  completions, IRQs, recovery and cursor operations.
- Scanout enumeration and basic display info/hotplug refresh in the `virtio-gpu`
  backend, keeping `desktop` single-display by default.
- Initial hardware cursor plane support in `virtio-gpu`, with a transparent
  fallback to the `desktop`'s software cursor when the backend does not expose it
  or fails.
- Additional automated coverage in `gputest --smoke` to validate driver progress
  through `GPU_IOC_GET_STATS` and scanout enumeration.

### Changed

- The system reports itself as `v0.2.1` in the kernel, the shell, `uname`,
  `sysinfo` and the components consuming the shared version.
- The normal graphics model is now definitively desktop-first: the taskbar stays
  visible, client apps render over a stable work area and the `desktop` becomes
  the normal owner of the scanout.
- `shellapp`, `gfxdemo`, `keytest`, `mousetest` and `doomgeneric` are aligned to
  the compositor's client path instead of the legacy direct fullscreen over
  `/dev/gpu0`.
- The taskbar and the Start menu get a pass of visual and behavioral polish to
  fit the new desktop-first contract better.
- The `virtio-gpu` backend stops depending on opportunistic caller reentry to
  make progress: the internal scheduler now coalesces presents per resource,
  reduces redundant `SET_SCANOUT`s and advances work in the background with IRQ
  support when the PCI line is available.
- `virtio-gpu` adds deliberate recovery and a predictable degraded mode in the
  face of device timeouts, trying to restore the primary scanout and the console
  without requiring an immediate OS restart.
- The main build and the associated tooling make it more explicit that
  `build/disk.img` is persistent by default: `SVFS2` consistency is validated,
  recreating the image is avoided except on real corruption, and `doomgeneric`
  together with `doom1.wad` is kept as a practical persistence regression test.
- The QEMU profiles used by `run`, `smoke` and the graphics utilities align
  better with the current stack's real virtual hardware (`virtio-gpu` +
  `virtio-tablet` + `desktop`).
- `doomgeneric` moves permanently to living as a compositor client and is aimed
  at manual validation inside the normal graphics session, instead of depending
  on a host-side smoke of its own.

## [0.2.0] - 2026-03-22

### Added

- A `desktop-first` session with the `desktop` compositor, the `shellapp`
  fullscreen shell, client surfaces shared through a `SectionObject` and
  graphical app launching through `fd 3..6`.
- An extended public ABI on `/dev/gpu0` with `GPU_IOC_SET_MODE`,
  `GPU_IOC_IMPORT_SECTION`, `GPU_IOC_RELEASE_SURFACE`,
  `GPU_IOC_PRESENT_SURFACE_REGION` and `GPU_IOC_WAIT_IDLE`.
- A new public ABI for audio with `SAVANXP_IOCTL_GROUP_AUDIO`,
  `AUDIO_IOC_GET_INFO` and `struct savanxp_audio_info`.
- A playback-only `virtio-sound` driver over `virtio_pci`, exposing
  `/dev/audio0` with the fixed `S16LE stereo 48 kHz` format.
- A new `audiotest` utility and automated coverage in `.\build.ps1 smoke` to
  validate `/dev/audio0`.
- A minimal object manager with generic kernel handles for I/O, events, timers
  and sections.
- New `EVENT_*`, `WAIT_ONE`, `WAIT_MANY`, `TIMER_*`, `SECTION_CREATE`,
  `MAP_VIEW` and `UNMAP_VIEW` syscalls, with updated wrappers in userland and
  SDK v1.
- Initial anonymous `Section/View` support in the kernel, including shared
  memory between processes, `shared` vs `private` inheritance on `fork()` and
  new `eventtest`, `timertest`, `sectiontest` and `mmaptest` tests.
- A new POSIX layer for anonymous `mmap` / `munmap` in
  `subsystems/posix/sdk/v1`, plus the standard `sys/mman.h` header.

### Changed

- The system reports itself as `v0.2.0` in the kernel, the shell, `uname`,
  `sysinfo` and the components consuming the shared version.
- The normal boot now supervises `desktop` from `init`; `/SMOKE` still avoids
  the desktop and keeps the automated headless smoke.
- `gfx_open` and the graphics runtime become compositor-first, with a direct
  fallback over `/dev/gpu0`; the legacy `/dev/fb0` node is no longer exposed in
  the current system.
- The main build now installs the ported BusyBox multicall in `/bin` and
  `/disk/bin` for `ls`, `cat`, `echo`, `mkdir`, `rm`, `mv`, `cp` and `ps`.
- `virtio-gpu` moves to presenting over an internal set of three surfaces and the
  legacy `FB_IOC_*` path leaves the current ABI.
- The `run` and `smoke` QEMU profiles add `virtio-sound-pci` with an `audiodev`
  separate from the `pcspeaker` path.
- `sleep_ms()` now runs over kernel waitable timers instead of a separate special
  path, and `fork()` preserves anonymous views as shared or private according to
  the mapping type.
- The Start menu no longer offers `Exit Desktop`, and `shellapp` can be closed
  with `exit` to return to the desktop and reopened later from `Menu -> Shell`.
- The desktop compositor cuts some redundant work in the presentation path and
  better fixes input routing/polling for fullscreen clients.

## [0.1.4] - 2026-03-19

### Added

- New POSIX syscalls and wrappers for `fork`, `kill`, `raise`, `poll`, `select`
  and `fcntl(F_GETFL/F_SETFL)` with `O_NONBLOCK` support.
- An automated runner `.\build.ps1 smoke`, which rebuilds, installs into
  `/disk/bin`, boots QEMU headless and validates `fork`, basic signals, polling
  and real persistence over `SVFS2`.
- A `busybox` multicall userland to start replacing the stopgap utilities,
  including `echo`, `cat`, `ls`, `mkdir`, `rm`, `mv`, `cp`, `true`, `false` and
  `sleep`.

### Changed

- The system reports itself as `v0.1.4` in the kernel, the shell, `uname`,
  `sysinfo` and the components consuming the shared version.
- The system's base timer is now calibrated targeting `200 Hz` instead of
  `100 Hz`, slightly improving perceived mouse response and the practical
  rounding of `sleep_ms()` for graphics and input loops.
- The internal ceilings go up for processes, descriptors, pipes, sockets, VFS
  and `SVFS2`, leaving more headroom for ports and a real userland.
- `SVFS2` can now mount `/disk` in a degraded read-only mode if recovery does not
  leave the volume safe for `RW`, avoiding it going straight offline in the face
  of recoverable failures.
- The main build also installs the internal binaries in `/disk/bin`, so the shell
  and the automated smoke exercise the same persistent copy of the userland.

## [0.1.3] - 2026-03-17

### Added

- A shared `virtio_pci` base for modern `virtio` drivers over PCI/MMIO, reused
  by `virtio-input` and prepared for synchronous polling queues.
- A new 2D `virtio-gpu` driver for QEMU, with MVP support for
  `GET_DISPLAY_INFO`, `RESOURCE_CREATE_2D`, `RESOURCE_ATTACH_BACKING`,
  `SET_SCANOUT`, `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH`.
- A new `/dev/gpu0` node with the public ABI `GPU_IOC_GET_INFO`,
  `GPU_IOC_ACQUIRE`, `GPU_IOC_RELEASE`, `GPU_IOC_PRESENT` and
  `GPU_IOC_PRESENT_REGION`.
- A new `gputest` utility to validate the direct presentation path over
  `/dev/gpu0`.
- Calibration of the `local APIC/x2APIC` timer against the `RTC/CMOS` during boot
  so that `uptime_ms` and `sleep_ms` line up better with real time in QEMU.

### Changed

- `/dev/fb0` keeps compatibility with the existing fullscreen apps, but can now
  present over `virtio-gpu` when the backend is available.
- The QEMU profile in `build.ps1 run` now adds `virtio-vga` with
  `xres=1280,yres=800`, and `limine.conf` asks for `1280x800x32` so the boot
  framebuffer and the graphics backend line up during the handoff.
- The console and the fullscreen UI can stay visible over `virtio-gpu`'s primary
  resource, including the return from exclusive graphics sessions and a clean
  redraw of the whole shell with no residue at the margins.
- `virtio-gpu` now tries to keep the boot framebuffer's large mode before falling
  back to the native scanout the device reports, preventing the system from
  returning to `640x480` at the end of boot when the larger mode is accepted.
- `virtio-input` moved to using the active framebuffer's effective geometry to
  normalize the absolute tablet, fixing the mouse desyncing from the host after
  the switch to `virtio-gpu`.
- The kernel heap stopped being linear-only and now recycles freed blocks, does
  `split/coalesce` and can return whole arenas to the physical allocator when
  they end up empty.
- The POSIX runtime of `subsystems/posix/sdk/v1` replaced its arena/bump
  allocator with a fixed recyclable heap, so `malloc`, `free`, `calloc` and
  `realloc` now reuse memory in external apps.
- `sdk/doomgeneric` no longer runs accelerated by an incorrect base clock; game
  time is back on a time backend closer to real, leaving the remaining
  performance tuning on the port's side.

### Known limits

- This `virtio-gpu` MVP visibly improves the fullscreen GUI in QEMU, but pixel
  upload is still synchronous, copied by the CPU, with no `mmap`, page flipping
  or real double buffering.
- Porting apps to `/dev/gpu0` reduces layers and sets up the evolution better,
  but the big improvement in smoothness is left for a later stage with shared
  buffers and less blocking presentation.
- The final performance of large external ports such as `sdk/doomgeneric` still
  depends heavily on the scaling cost and the frame size once the system runs at
  higher resolutions.

## [0.1.2] - 2026-03-14

### Added

- Basic `PS/2` mouse support over the `i8042` controller's auxiliary port, with
  IRQ12, standard 3-byte packets and safe degradation to keyboard-only if the
  mouse does not initialize.
- A new `/dev/mouse0` node with dedicated mouse events for graphical apps,
  without breaking `/dev/input0`'s previous semantics.
- The shared ABI extended with `struct savanxp_mouse_event`, public button flags
  and new `mouse_open` / `mouse_poll_event` helpers in the libc/runtime.
- A new fullscreen graphical shell `desktop`, inspired by the visual language of
  Windows 2000, with a taskbar, Start button, clock and cursor.
- A new `mousetest` utility to validate `/dev/mouse0`, relative movement and
  buttons from userland.

### Changed

- The kernel's fullscreen layer now registers `/dev/mouse0` alongside `/dev/fb0`
  and `/dev/input0`, and clears the keyboard/mouse queues when acquiring or
  releasing the exclusive graphics session.
- Under QEMU, the desktop and `mousetest` now prefer an absolute
  `virtio-tablet-pci` backend when available, while `/dev/mouse0` keeps the delta
  ABI so as not to break already-compiled apps.
- The kernel now reserves an MMIO window of its own for modern PCI drivers and
  uses it to map memory BARs safely during boot.
- In QEMU environments with `virtio-tablet-pci`, the `PS/2` stack stops
  initializing the auxiliary mouse and stays keyboard-only; the `PS/2` mouse is
  kept as a fallback when `virtio-input` is not available.
- `RTC/CMOS` reading was added to the kernel along with an additive public helper
  for querying real time from userland; the desktop's clock no longer depends on
  `uptime` alone.
- The builtin shell now lists `desktop` and `mousetest` in the interactive help.
- The main documentation and the SDK v1 reference reflect the new mouse input,
  the initial desktop and the jump to `v0.1.2`.

### Known limits

- `v0.1.2` exposes only relative movement and basic buttons in the public ABI;
  internally it can use an absolute pointer under QEMU, but there is no wheel, no
  real windows, no compositor and no raw input for games.
- `gfx_poll_event` is still keyboard-only at this stage; the mouse comes in
  through `/dev/mouse0`.

## [0.1.1] - 2026-03-10

### Added

- `SVFS2` as the new version of `/disk`'s persistent filesystem, with a
  primary/secondary `superblock`, a fixed metadata journal, a block bitmap, an
  inode bitmap and an inode table with extents.
- A new `sync` syscall and a userland `sync` command to force an explicit
  checkpoint of the persistent state.
- Minimal networking over `rtl8139` + `QEMU user-net`, with `ARP`, `IPv4`,
  `ICMP` echo request/reply, basic IPv4 UDP sockets and a minimal TCP client.
- An extended public ABI for devices and `ioctl`, with the `/dev/fb0`,
  `/dev/input0`, `/dev/net0` and `/dev/pcspk` nodes.
- An initial fullscreen GUI with `gfx_*` primitives, the internal `gfxdemo` demo
  and the external `sdk/gfxhello` example.
- Minimal sound over the `PC speaker` with a `beep` command.
- The first large external port in `sdk/doomgeneric`, used as a practical
  milestone for external graphical apps over the system's ABI.
- An initial POSIX/libc layer for SDK v1 with public standard headers:
  `unistd.h`, `fcntl.h`, `stdio.h`, `stdlib.h`, `string.h`, `dirent.h`,
  `sys/stat.h`, `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`, `time.h` and
  friends.
- A new `subsystems/posix/sdk/v1/runtime/posix.c` runtime for external apps,
  with basic `stdio`, `DIR*`, a simple arena-style heap, conversions, string
  helpers, time and client sockets.
- New syscalls/base ABI for `getpid`, `stat`, `fstat`, `chdir` and `getcwd`.
- An external smoke test `sdk/posixsmoke`, compiled only against standard
  headers.
- A `keytest` utility to inspect keyboard events over `/dev/input0` in fullscreen
  and validate `key down/up`, `keycode` and `ascii`.
- `FB_IOC_PRESENT_REGION` as an extension of the graphics ABI to present only a
  region of the framebuffer from userland.

### Changed

- The VFS layer now centralizes path normalization and raised the internal path
  capacity to `256` bytes, so `process`, `cwd` and filesystem operations share a
  single canonical resolution.
- The host tooling over `build/disk.img` (`build.ps1`, `tools/build-user.ps1`
  and `tools/UserAppCommon.ps1`) stopped writing `SVFS1` and moved to creating
  and installing directly onto `SVFS2` images.
- `/disk`'s persistent paths stopped depending on path names as their on-disk
  identity and moved to being mounted from stable inodes cached in kernel memory.
- The kernel now resolves relative paths against a per-process `cwd`, so `open`,
  `exec`, `spawn` and filesystem operations share the current directory.
- The `PS/2` keyboard stack was hardened with a more robust controller init,
  decoding decoupled from `TTY`/`UI`, and better support for `AltGr`, locks and
  special keys.
- `sdk/doomgeneric` stopped depending on its private set of standard headers and
  moved to consuming the SDK's public layer, reducing `savanxp_compat.c` to
  port-specific glue.
- The shared runtime's `gfx_*` primitives were optimized to work with contiguous
  spans/rectangles and reduce the per-frame drawing cost.
- `gfxdemo`, `sdk/gfxhello` and `keytest` stopped refreshing the whole screen on
  every iteration and now use dirty regions or on-demand redraws to improve
  fluidity in fullscreen.
- `sdk/doomgeneric`'s backend replaced per-pixel division-based scaling with
  cached row expansion, lowering the per-frame CPU cost during fullscreen
  rendering.
- The main documentation and the SDK reference were updated to reflect the
  available POSIX/libc surface and its practical limits.

### Known limits

- The recent validation of the jump to `SVFS2` covers a full build and host-side
  verification of the `build-user` flow, but does not yet include reboot/replay
  smoke tests inside QEMU.
- If journal or base metadata recovery fails at mount time, the volume goes
  offline; there is no read-only degraded mode yet.
- `free()` does not recycle memory yet; the userland allocator is still
  arena/bump style.
- `DIR->d_type` is filled in by a best-effort `stat()` in userland.
- `setsockopt`/`getsockopt` cover only basic client flags and timeouts.
- Depending on the host and QEMU's keyboard capture, `PrintScreen` may not reach
  the guest as a dedicated key and may require `Alt+PrintScreen` for manual
  testing.
- The recent validation of `v0.1.1` is host-side; the new POSIX smoke has not
  been run inside QEMU in this batch.

## [0.1.0] - 2026-03-08

The experiment's first published version.

### Added

- Kernel bootstrap over `x86_64 + UEFI + Limine`, receiving `bootloader info`,
  `framebuffer`, `memory map`, `HHDM` and `initramfs`.
- A text console over the framebuffer with scrolling and a cursor, plus early
  serial output over `COM1` / `debugcon`.
- GDT/IDT with user segments, `TSS`, basic exceptions and a syscall gate through
  `int 0x80`.
- An early physical allocator, a kernel heap and a minimal VMM for user address
  spaces.
- A `PS/2` keyboard driver, a canonical `TTY` and an initial interactive shell.
- A minimal `VFS` mounting a `cpio newc` `initramfs`, with dynamic in-memory
  files and a persistent `SVFS` volume mounted at `/disk`.
- A static `ELF64` loader for simple `ring 3` processes.
- A preemptive round-robin scheduler with blocking on `wait`, `read` and `sleep`.
- A shell with `pipes`, redirection (`|`, `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`),
  a single/double quote parser and the `exec`, `which` and `mkdir` builtins.
- Refcounted handles with `dup`, `dup2`, `waitpid(-1)` and zombie/reap
  processes.
- Page reclaim on `exit`/`exec`, `VmSpace` destruction and freeing of kernel
  stacks when reaping processes.
- `SVFS` with simple persistent subdirectories under `/disk`, `mkdir`, `rmdir`
  of empty directories, explicit `truncate` and persistent `rename`.
- A minimal SDK v1 in `C`, with `crt0`, `libc`, a linker script, the `savanxp/*`
  headers, host tooling to install external apps into `build/disk.img` and base
  examples (`sdk/hello`, `sdk/errdemo`, `sdk/fsdemo`, `sdk/pathops`,
  `sdk/procpeek`, `sdk/spawnwait`, `sdk/statusdemo`, `sdk/multifile`,
  `sdk/template`).
- An initial userland with `init`, `sh`, `echo`, `uname`, `ls`, `cat`, `sleep`,
  `ticker`, `demo`, `true`, `false`, `ps`, `fdtest`, `waittest`, `pipestress`,
  `spawnloop`, `badptr`, `rm`, `rmdir`, `truncate`, `seektest`, `truncatetest`
  and `errtest`.

### Changed

- Processes, pipes and persistence were consolidated so that `/disk` is
  operational as the main working flow across reboots.
- The syscall surface and the minimal `libc` were widened with the filesystem and
  process operations needed for the shell, pipes and external apps.
- The repository and SDK v1 documentation was frozen to leave a useful public
  base from the first numbered version onwards.
