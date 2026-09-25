# SxMedia — the layer between the media devices and the programs

> **Status: DESIGN, nothing built.** Everything below is a proposal with its
> reasons and its verification, not a record of shipped work. The code that
> SxMedia is extracted from is real and lives in
> [`ports/ffmpeg/overlay/mediaplayer/`](../ports/ffmpeg/overlay/mediaplayer);
> [`MEDIA_PLAYER.md`](MEDIA_PLAYER.md) documents what that code does today and
> is the reference for every claim made here.
>
> This document answers one question: **what does SavanXP need between a media
> file and a program, so that the program never sees FFmpeg and the kernel
> never sees a codec?** It fixes the contract, names the decisions, and orders
> the work. Where a decision is genuinely open it is written as a decision
> point with a recommendation, not hidden.

## The one-line definition

**SxMedia is the userland half of multimedia: it owns the contract between a
media source (container + codecs) on one side, the media devices the kernel
exposes (`/dev/audio0`, the GPU/compositor) on the other, and the programs in
between.**

That is the same sentence the SDK already uses for the rest of its optional
modules, and for the same reason. `savanxp/audio.h` says it about sound:
playback stays a raw PCM device in the kernel, and the module owns the
userland half shared by legacy sound engines. SxMedia is that idea one layer up
— one module for files, video, sound and the clock that ties them together,
instead of one per consumer.

## The four seams that do not exist yet

### 1. A reusable engine lives inside a port, so nothing else can use it

`ports/ffmpeg/overlay/mediaplayer/` is 3025 lines in four pieces, and the split
is already the right one:

| File | Lines | What it is | What it knows |
|---|---|---|---|
| `mediaplayer.c` | 830 | window, transport controls, input | SXGUI, the engine |
| `playback.c` | 466 | clock, `/dev/audio0`, which frame goes on screen | the device, not the window |
| `media.c` | 627 | demux, packet queues, decoders, swscale, swresample, seek | nothing but FFmpeg |
| `selftest.c` | 489 | `--probe`, `--selftest`, `--gpu-hold` | the engine, no window |

[`MEDIA_PLAYER.md`](MEDIA_PLAYER.md#layers) already argues that this split is
what lets a headless selftest drive exactly the engine the window uses, and
what a second front end would reuse. The problem is purely one of **location**:
all four files sit in a port that is built by its own `configure.sh`/`link.sh`,
is not part of the CMake target graph, and is not in the SDK surface. So:

- `tools/build-user.sh` cannot link them. An external app gets `gfx2d`, `sxgui`,
  `sxchrome` and `audio`, and nothing else.
- the base CMake userland cannot link them either, which is why
  `/bin/mediaplayer` is a 118-line launcher that `exec`s
  `/disk/bin/mediaplayer-ffmpeg` — a hardcoded path to a hardcoded
  implementation.
- 830 lines of front end exist once per player, and a second front end would be
  a copy.

### 2. There is no seam where a second codec implementation can land

The user's requirement is a backend that works as a **fallback** until SavanXP
has its own codecs (`SxCodecs`). Today that is impossible to arrange without
rewriting the player, because:

- the *decode* half is `media.c`, which is one translation unit that
  `#include`s `<libavcodec/avcodec.h>` at the top and has FFmpeg types in its
  public struct. There is no interface to implement, only code to copy.
- the *delivery* half is a path string. `MEDIA_PLAYER_PORT` in
  `subsystems/posix/userland/mediaplayer.c` is a decision about **which
  implementation is compiled into the program**, made at the level of a file
  path, and it can only ever name one thing.
- The base manifest owns `mime_open`/`ext_open`, and the port's manifest is
  forbidden from declaring them (pinned by
  `tests/host/system_mediaplayer_layout_test.py`), so the installed backend
  cannot even offer itself as a handler.

### 3. The system advertises a format set that it hardcodes in a manifest

`subsystems/posix/userland/mediaplayer.sxres` declares

```text
mime_open=video/mp4,video/x-matroska,video/webm,audio/mpeg,...
ext_open=.mp4,.m4v,.m4a,.mov,.mkv,.webm,.avi,.ogg,...
```

That list **is** the FFmpeg build's demuxer set, written down by hand and
stamped into the base binary at build time. `file_assoc.c` reads it at runtime,
so the catalog is a *static, per-program* claim about what the machine can open.
It does not consult a backend, because there is no backend to consult: nothing
publishes a capability set at runtime.

So the shape after batches 1 to 3 is: the catalog advertises `.mp4` on a machine
whose only media program has **no backend linked in at all**, and the message
the user gets is "no media backend available". That mismatch already exists
today; SxMedia does not create it, and the batches do not have to fix it. But it
is the concrete reason the answer to "what does SavanXP play?" cannot be
answered by the registry yet, only by which program the catalog picked.

**The obvious fix is worse, and the reason is worth keeping.** Moving
`ext_open`/`mime_open` from the base manifest to the port's would make the
catalog correct — only a program that *can* decode would claim the extension —
but `file_assoc.c` answers "no handler" by falling back to the editor:

```c
/* subsystems/posix/userland/filesapp.c:488 */
if (program == 0)
{
    program = FILESAPP_EDITOR_PATH;   /* a .mp4 opened in Notepad */
}
```

Without the port installed, every video would open in Notepad, which is a worse
answer than "build `ports/ffmpeg`". The base manifest owning the associations is
therefore correct as it stands: it guarantees that a media file always reaches
*some* media program that can explain itself.

The real fix is a backend that **publishes** its capability set where the
catalog can read it — a file the installer writes, or a record a resident
service owns. That is the first half of the media service named below, and it
is why the catalog question belongs to the end state and not to batch 1.

### 4. Every program that touches a media device re-derives the same policy

`playback.c` is the part of the port that is **not** FFmpeg: it owns

- the wall clock as master clock, and the correction that makes the reported
  position the one being *heard* rather than the one being written;
- the `write()`-never-blocks contract (the driver discards a period once 8 are
  in flight) and the 40 ms lead that stays under it;
- the 4 primed periods of driver silence, re-primed after every underrun;
- pause/seek/stall restarting the stream, because anything that drains the
  driver's queue changes the latency;
- the drop policy (100 ms late, at most 8 in a row, never freeze for 250 ms);
- software volume on interleaved s16.

`runtime/audio.c` (`sx_audio_mixer`) already solves the mirror-image problem
for games: a wall-clock frame sink that any sound engine can write into. Two
modules, two policies for the same device, one of them unreachable from the
other. A game with a video cutscene and a Doom-style sound engine needs both.

The clock policy is also, today, an **estimate**: `/dev/audio0` cannot say how
much it has played, so the latency is a constant derived from the prime count
and the period size, and a stall is a heuristic. That is item 1 on the
`MEDIA_PLAYER.md` roadmap, and it is the one piece of the missing work that
belongs in the kernel.

## Where SxMedia sits

```
  ┌──────────────────────────────────────────────────────────────┐
  │  Programs: the player shell, games, anything that plays       │
  │  pick a sink, call pump, present                             │
  └──────────────────────────────────────────────────────────────┘
                              ▲  sinks (device-independent)
  ┌──────────────────────────────────────────────────────────────┐
  │  SxMedia                                                     │
  │    engine  — source → packets → frames, microseconds, no I/O  │
  │    player  — clock, position, drop policy, transport         │
  │    registry— who implements the stages (FFmpeg, SxCodecs)     │
  └──────────────────────────────────────────────────────────────┘
        ▲                            │
        │ savanxp/sxmedia.h          │ vtable, registered at startup
  ┌─────┴──────────────┐    ┌────────┴───────────────────────────┐
  │ backend: FFmpeg    │    │ backend: SxCodecs (later)         │
  │ (port overlay)     │    │ (in-tree)                          │
  └────────────────────┘    └────────────────────────────────────┘
                              ▲
                              │ open/read/write/ioctl
  ┌──────────────────────────────────────────────────────────────┐
  │  Kernel: /dev/audio0 (raw PCM), /dev/gpu0, the compositor    │
  │  No container, no codec, no media framework.                 │
  └──────────────────────────────────────────────────────────────┘
```

The analogy the rest of the docs use, extended:

| Role | SavanXP | Linux | Windows |
|---|---|---|---|
| 2D rasterization | SXGFX | — | GDI32 |
| Control toolkit | SXGUI-C | — | USER32 |
| Window manager | `windowd` | X server | win32k |
| **Media decode / pipeline** | **SxMedia** | GStreamer / libav* | Media Foundation |

**The rule that keeps it honest:** the *engine* knows nothing about screens,
speakers, windows or the wall clock. The *player* knows nothing about windows.
The *shell* knows nothing about codecs. The *backend* knows nothing about the
device. Each layer can be replaced without touching the one above it, and
today the same four rules already hold inside the port — SxMedia's job is to
make them hold *across* implementations too.

## The contract

`subsystems/posix/sdk/v1/include/savanxp/sxmedia.h`, the same shape as
`gfx2d.h` and `audio.h`: caller-owned structs with explicit
`_init`/`_destroy`, `>= 0` success and `< 0` `-errno` failure, no C++
in the header, nothing FFmpeg-shaped anywhere.

### Fixed by the consumers, not negotiated

- **Time is microseconds from the start of the file**, `start_time` already
  subtracted, and `SX_MEDIA_NO_TIME` for "unknown". No `time_base` leaves a
  backend. This is already the rule in `media.h` and it is the one that makes
  A/V sync testable end to end.
- **Video out is `SX_PIXEL_FORMAT_BGRX8888`** — the byte order of `gfx_rgb`,
  the same as the SavanXP framebuffer and as `savanxp/sxgui.h`. A backend
  picks whatever pixel format its decoder produces and converts. Note that this
  is the *one* place where the current code is FFmpeg-shaped: `media.c` hardcodes
  `AV_PIX_FMT_BGR0` and documents at length why that byte is `BGR0` and not
  `BGRA`. SxMedia states the same fact as `sx_pixel_format` and lets the
  backend do the translation.
- **Audio out is interleaved `s16`** at the rate and channel count the sink
  asks for. A source `audio_out` of `NULL` means "ignore audio", which is what
  lets a headless consumer decode video without opening the device.
- **The engine is pull-based.** The caller asks for the next video frame or the
  next N audio samples; packets of the other stream found on the way are
  queued, never dropped silently. This stays exactly as it is.

### The engine

```c
#define SX_MEDIA_NO_TIME INT64_MIN
#define SX_MEDIA_QUEUE_PACKET_LIMIT 8192   /* per stream, as today */

struct sx_media;                            /* opaque: create/destroy */

struct sx_media_audio_format { int sample_rate; int channels; };

struct sx_media_source {
    const char* path;                       /* or fd >= 0, never both */
    int fd;
    const struct sx_media_audio_format* audio_out;  /* NULL = ignore audio */
};

/* Why a source did not open, and why a stream was dropped. A program cannot
 * write a useful notice from a boolean, and it must never have to parse a
 * string to find out what happened. These are the only reasons there are, and
 * each one earns its place because it wants a DIFFERENT sentence to the user:
 *
 *   NO_BACKEND    nobody registered at all -> "this build has no media backend"
 *   NO_DEMUXER    the container is not here -> "cannot read this format"
 *   NO_CODEC      read it, the codec is not here -> "no <codec> decoder"
 *   UNREADABLE    not a media file, or damaged -> "this file is not readable"
 *   UNSUPPORTED   read it, nothing playable inside
 *
 * NO_DEMUXER and NO_CODEC being different is the whole point: both arrive as
 * "it will not play", and the user can only act on one of them. */
enum sx_media_status {
    SX_MEDIA_OK = 0,
    SX_MEDIA_ERR_NO_BACKEND,
    SX_MEDIA_ERR_NO_DEMUXER,
    SX_MEDIA_ERR_NO_CODEC,
    SX_MEDIA_ERR_UNREADABLE,
    SX_MEDIA_ERR_UNSUPPORTED,
    SX_MEDIA_ERR_NOMEM,
};

/* One wording for all of them, so a program never writes its own. */
const char* sx_media_status_string(enum sx_media_status status);

/* ---- capability, before anything is opened ----
 *
 * The question a program asks to decide whether to try at all, and to be able
 * to say something before the user has picked a file. Static, cheap, and
 * answered by the registry, so it is exactly the "does this system have a
 * decoder for vorbis" that the ccleste music callback needs. */
int sx_media_has_codec(const char* codec);
int sx_media_codec_count(void);
const char* sx_media_codec_name_at(int index);   /* the list, for a "what can
                                                   * this system play" screen */
int sx_media_backend_count(void);
const char* sx_media_backend_name_at(int index);

int  sx_media_create(struct sx_media** out);
void sx_media_destroy(struct sx_media* media);

/* 0 on success; on failure returns < 0 and sets `status` to the reason. The
 * reason for a partial open is never a failure: a file with one decodable
 * stream opens, and the rest are reported by the *_missing_* calls below. */
int  sx_media_open(struct sx_media* media, const struct sx_media_source* source,
                   enum sx_media_status* status);
void sx_media_close(struct sx_media* media);

const char* sx_media_backend_name(const struct sx_media* media);
const char* sx_media_container_name(const struct sx_media* media);
int   sx_media_has_video(const struct sx_media* media);
int   sx_media_has_audio(const struct sx_media* media);
void  sx_media_display_size(const struct sx_media* media, int* width, int* height);
int64_t sx_media_duration_us(const struct sx_media* media);
int64_t sx_media_frame_duration_us(const struct sx_media* media);
const char* sx_media_metadata(const struct sx_media* media, const char* key);

/* ---- what was dropped ----
 *
 * Play what you can and say what you did not get, which is what every player
 * does and what makes per-stream claiming worth having. A 0 count is the only
 * case where the source plays completely. */
int sx_media_stream_count(const struct sx_media* media);
int sx_media_stream_decodable(const struct sx_media* media, int stream_index);
const char* sx_media_stream_codec(const struct sx_media* media, int stream_index);
int sx_media_stream_missing_count(const struct sx_media* media);
/* Copies the codec name of the index-th undecodable stream. Returns 0 past the
 * end. Used to build the notice: "plays, without its HEVC video". */
int sx_media_stream_missing_at(const struct sx_media* media, int index,
                               char* codec, size_t codec_capacity);

/* 1 = a frame is ready, 0 = the source is finished, < 0 = error. */
int   sx_media_next_video_frame(struct sx_media* media);
void  sx_media_show_video_frame(struct sx_media* media);
int   sx_media_has_shown_frame(const struct sx_media* media);
int64_t sx_media_video_time_us(const struct sx_media* media);
/* BGRX8888 at width x height, engine-owned until the next call. */
const uint32_t* sx_media_scale_video(struct sx_media* media, int width, int height);

/* Fills `out` with up to `frames` interleaved s16 frames and reports the time
 * of the first one. Returns how many it wrote, 0 when the audio is finished. */
int   sx_media_read_audio(struct sx_media* media, int16_t* out, int frames, int64_t* first_time_us);

int   sx_media_seek(struct sx_media* media, int64_t target_us);
/* Suppresses delivery before `time_us` without seeking: the resume point after
 * a device stall, where the audio already written was heard whole. The
 * per-stream skip marks that `media.c` keeps are this operation, made public. */
void  sx_media_skip_until(struct sx_media* media, int64_t time_us);
```

`struct sx_media` is opaque on purpose. Today `struct media` is caller-owned
with public fields, and the port reaches into `media->audio_skip_until_us` from
`playback.c` (`playback.c:149`) — a reach-through that is exactly the coupling
SxMedia exists to remove. The stall restart becomes a named call,
`sx_media_skip_until()`, and the per-stream skip marks that `media.c` already
keeps stop being fields the device half writes behind the engine's back.

### The notice

The contract a program is given is: **ask SxMedia whether the codec exists; if
it does, play; if it does not, tell the user — and never name the
implementation.** Three rules make that hold everywhere instead of in one port.

**The reason is typed, not a string.** `media.c` today has exactly two failure
sentences, `"cannot open the file"` (`media.c:165`) and `"no stream with a
supported codec"` (`media.c:186`), and a program cannot build a notice from
either. `enum sx_media_status` is what a program switches on.

**The wording is written once.** `sx_media_status_string` is the only place
these sentences live, the same way `result_error_string` is the only place
`SAVANXP_E*` becomes English. A program that invents its own message is the
same anti-pattern the tree already settled once, when private copies of the
bevels moved down into SXCHROME because `calc`, `taskmgr` and `windowd` had
each grown one.

**The notice names the codec, never the backend.** "This system has no HEVC
decoder", not "no FFmpeg here". That is the same rule that
`system_mediaplayer_layout_test.py` already enforces in code with
`assert "libav" not in source.lower()`, extended from the source to the string
table. `sx_media_backend_name_at` exists for `--probe` and the selftests, which
is where an implementation name belongs.

Two ways to deliver it, one per consumer:

```c
/* A window, through SXGUI. Lives in sxmedia_app.c so the wording is not
 * retyped by each program; this is the same shared-notice rule as
 * sxgui_dialog_begin. */
int sx_media_report_unavailable(const char* path, enum sx_media_status status);
int sx_media_report_partial(const struct sx_media* media, const char* path);

/* No window: a game, a background decode, an init-time check. Still the
 * library's wording, written once to stderr. */
void sx_media_log_unavailable(const char* path, enum sx_media_status status);
```

The `ccleste` music callback is the smallest complete example of the whole
contract, and it is worth writing out because it touches no FFmpeg and needs no
window:

```c
/* overlay/ccleste_savanxp.c, case CELESTE_P8_MUSIC */
if (!sx_media_has_codec("vorbis")) {
    sx_media_log_unavailable("music.ogg", SX_MEDIA_ERR_NO_CODEC);
    return;                      /* the game keeps running, in silence */
}
if (sx_media_open(&g_music, &source, &status) == 0 && g_music.missing == 0) {
    sx_media_player_play(&g_music_player);   /* the sink is the music voice */
}
```

Four lines of policy, zero knowledge of whether the decoder behind that `1` is
`stb_vorbis` in this binary or FFmpeg's in the next one, and the port's
`"sin reproductor de musica"` log line becomes the *accurate* version of itself
instead of a permanent stub.

### What that does to the manifest mismatch

Seam 3 said the catalog advertises formats nothing in the image can decode. The
notice contract **downgrades that failure from silent to explained**: with
batches 1 to 3 in place and no Vorbis backend linked, Files still launches
`ccleste` for a `.ogg` because the catalog says it can — and `ccleste` says "no
Vorbis decoder in this system", which is the truth, from the program that
actually tried. The program is the one component that knows the answer, and this
is the arrangement that puts its answer in front of the user.

What it does **not** do is answer the question for a program that never runs:
the catalog still lists a format no backend in the image claims. Making *that*
true is the capability publication in the end state, and it stays there.

### Sinks — the device seam

This is the part that makes SxMedia the *intermediary*, and it is the part with
no precedent in the tree, so it is worth being precise about it.

Today the device half is welded to the engine: `playback.c` opens
`/dev/audio0` itself and the window half learns about the engine through
`playback_present_fn(user, struct media*)`, which leaks the engine type into the
front end. SxMedia replaces both with two small vtables. A program chooses a
sink; SxMedia is the only thing that ever talks to a media device.

```c
/* Where decoded video goes. The player shell presents it to the client
 * surface; a game blits it; a recorder would encode it. */
struct sx_media_video_sink {
    void* user;
    /* `pixels` is BGRX8888 with `stride` pixels per row, owned by the engine
     * until the next pump. `time_us` is the media time of the frame. */
    void (*present)(void* user, const uint32_t* pixels, int width, int height,
                    int stride, int64_t time_us);
};

/* What the player is playing *through*. */
struct sx_media_audio_sink {
    void* user;
    /* Volume is 0..100 and belongs to the sink: a device sink scales in
     * software, a mixer sink already has per-voice volume. */
    int  (*open)(void* user, const struct sx_media_audio_format* format);
    void (*close)(void* user);
    int  (*write)(void* user, const int16_t* frames, int frame_count);
    void (*set_volume)(void* user, int volume);
    /* The one capability the clock model wants. Returns 1 and sets `out_us`
     * to the media time of the sample currently leaving the device, 0 when the
     * sink cannot know. */
    int  (*played_position_us)(void* user, int64_t* out_us);
    /* Free-running nanoseconds, the same `monotonic_ns` the kernel exposes. */
    unsigned long long (*now_ns)(void* user);
};

/* The default: /dev/audio0, opened and closed by the player, with the 40 ms
 * lead, the prime-derived latency and the stall restart. */
int sx_media_audio_sink_device(struct sx_media_audio_sink* sink, void* user);
```

Two consequences worth writing down, because they are the reason this is the
design and not a wrapper:

- **The clock stops being an estimate where the hardware can answer.** The
  player keeps the wall clock and the latency model; when the sink reports
  `played_position_us`, the position the program sees and the frame video is
  timed against are the driver's own answer, and the stall heuristic is
  replaced by a number. This is `AUDIO_IOC_GET_POSITION` from the
  `MEDIA_PLAYER.md` roadmap, arriving as a capability instead of a rewrite.
- **`sx_audio_mixer` becomes a sink.** A game with a cutscene registers the
  mixer, and the mixer's existing wall-clock contract
  (`runtime/audio.c`, `sx_audio_mixer_update`) is exactly a `write` with a
  `now_ns`. Nothing in `audio.c` changes; it just gains a caller it did not
  have.
- **A mixer sink does not accept a long track yet, and that is a real gap.**
  `sx_audio_mixer_start_voice` takes the *whole* sample buffer — `samples` is
  "borrowed until the voice ends or is stopped" (`savanxp/audio.h`) — which is
  right for a Doom SFX lump and wrong for a three-minute music track. A decoded
  Vorbis track is megabytes, and `ports/ccleste` already compiles the runtime
  with `-DSX_HEAP_SIZE=1048576` because its 23 sound effects need 1 MiB. So the
  first real consumer of the audio sink needs one of two things, and neither
  exists yet:
  - a **streaming voice** in the mixer — feed N frames, loop, no whole-track
    buffer — which is the honest fix, because a game looping a track is the
    normal case and not the exotic one; or
  - a **chunked decode straight to the device sink**, which needs no mixer
    change but gives up per-voice volume and panning.

  Worth naming here rather than discovering during a game port: it is the first
  requirement the *sink* abstraction exposes and the *existing* mixer API
  cannot meet. The seam is right, and it is also where the next piece of work
  is.

### The player

`playback.c` verbatim in shape, minus the engine internals:

```c
struct sx_media_player;   /* opaque */

enum sx_media_pump_flags {
    SX_MEDIA_PUMP_PRESENTED     = 1u << 0,
    SX_MEDIA_PUMP_STATE_CHANGED = 1u << 1,
};

int   sx_media_player_create(struct sx_media_player** out);
void  sx_media_player_destroy(struct sx_media_player* player);
int   sx_media_player_open(struct sx_media_player* player, const struct sx_media_source* source);
void  sx_media_player_close(struct sx_media_player* player);
void  sx_media_player_play(struct sx_media_player* player);
void  sx_media_player_pause(struct sx_media_player* player);
void  sx_media_player_toggle(struct sx_media_player* player);
void  sx_media_player_stop(struct sx_media_player* player);
int   sx_media_player_seek(struct sx_media_player* player, int64_t target_us);
void  sx_media_player_set_volume(struct sx_media_player* player, int volume);
int64_t sx_media_player_position_us(const struct sx_media_player* player);
int64_t sx_media_player_duration_us(const struct sx_media_player* player);
/* Feeds audio and presents the frame that is due. `wait_ms` receives how long
 * the caller may sleep; never more than 10 ms while playing. */
unsigned sx_media_player_pump(struct sx_media_player* player,
                              const struct sx_media_video_sink* video,
                              const struct sx_media_audio_sink* audio,
                              unsigned long* wait_ms);
```

The pump stays **one thread, no decode-ahead thread**, because the kernel has
no threads. That is a real cost and it is not SxMedia's to fix; it is listed
here so nobody mistakes the absence for an oversight.

## Backends

A backend is registered as **three separable roles**, because they have
different lifetimes and different owners:

| Role | Question it answers | FFmpeg | SxCodecs (later) |
|---|---|---|---|
| **source** | "can I parse this container?" | yes, all of them | no, at first |
| **decoder** | "can I decode *this* stream?" | yes, every stream it can open | yes, H.264 / HEVC |
| **converter** | "can I resample / scale to the output formats?" | yes (swresample / swscale) | no |

The engine keeps the providers in one ordered list, by descending priority, and
asks each role separately. The reason the roles are separate rather than one
`ops` blob is the reason the whole design exists: **the demuxer and the decoder
are allowed to be different programs.** If `open_decoder` took the source
handle, a codec library that does not demux could never be used on a container
somebody else demuxed, and "FFmpeg fills the gaps" would be impossible to
express.

```c
#define SX_MEDIA_BACKEND_ABI 1u

struct sx_media_stream_desc {
    int index;                 /* within the source, -1 if absent */
    int kind;                  /* SX_MEDIA_KIND_VIDEO | _AUDIO | _TEXT */
    const char* codec;         /* "h264", "aac"; diagnostics and claims */
    int width, height;         /* video */
    int sample_rate, channels; /* audio */
    int sar_num, sar_den;
    uint64_t duration_us;
    /* Codec-private setup the demuxer owns, and there is no way around it:
     * an H.264 or HEVC decoder cannot start without the parameter sets (SPS,
     * PPS, VPS), an AAC decoder cannot without the AudioSpecificConfig, and
     * only the demuxer that parsed the container has them. In Matroska and
     * MP4 they arrive out of band, so the decoder would have no way to invent
     * them. This is the field that makes a foreign decoder possible at all, and
     * its size is whatever the source says. Borrowed from the source handle,
     * valid until close. */
    const uint8_t* extradata;
    size_t extradata_size;
};

/* A packet is BORROWED: the payload is valid until the next read_packet on the
 * same handle. The engine copies it into its own bounded pool, which is what
 * keeps the queue backend-agnostic and makes the memory cost of the engine its
 * own. */
struct sx_media_packet {
    int stream_index;
    int64_t time_us;
    int key;
    const uint8_t* data;
    size_t size;
};

struct sx_media_backend_ops {
    /* -- source role -------------------------------------------------------- */

    /* Can this provider parse this source at all? Asked before anything is
     * opened, so it must be cheap and must not keep state. Returns 1 to take
     * the source, 0 to let the next provider try. A codec library that ships
     * no demuxer returns 0 unconditionally. */
    int  (*claim_source)(void* user, const struct sx_media_source* source);

    /* Parses headers and enumerates streams. No packets are consumed, and the
     * handle it returns is what read_packet will pull from. Returns SX_MEDIA_OK,
     * or the reason — a backend reports `NO_DEMUXER` when no demuxer matched
     * the file and `UNREADABLE` when one did and then failed on it, which is
     * the distinction `avformat_open_input` alone does not make and which the
     * engine must not have to recover by matching on strings. */
    void* (*open)(void* user, const struct sx_media_source* source,
                  struct sx_media_stream_desc* streams, int stream_capacity,
                  int* stream_count, int64_t* duration_us, int* seekable,
                  enum sx_media_status* status);
    void (*close)(void* handle);
    int  (*seek)(void* handle, int64_t target_us);
    int  (*read_packet)(void* handle, struct sx_media_packet* packet);

    /* -- decoder role ------------------------------------------------------- */

    /* Asked once per stream, after the source is identified and before it is
     * opened, so that the decoders are chosen before the first packet is read.
     * Returns 1 to own the stream, 0 to let the next provider try. */
    int  (*claim_stream)(void* user, const struct sx_media_stream_desc* stream);

    /* Takes the descriptor and nothing else -- deliberately NOT the source
     * handle. `stream->extradata` is the whole of what crosses over. */
    void* (*open_decoder)(void* user, const struct sx_media_stream_desc* stream);
    void (*close_decoder)(void* decoder);
    int  (*decode_video)(void* decoder, struct sx_media_video_frame* frame);
    int  (*decode_audio)(void* decoder, struct sx_media_audio_frame* frame);

    /* -- converter role ----------------------------------------------------- */

    void* (*scaler_open)(void* user, const struct sx_media_stream_desc* src, int width, int height);
    void  (*scaler_close)(void* scaler);
    int   (*scale)(void* scaler, const struct sx_media_video_frame* frame,
                   uint32_t* out, int width, int height, int stride);
    void* (*resampler_open)(void* user, const struct sx_media_stream_desc* src,
                            const struct sx_media_audio_format* dst);
    void  (*resampler_close)(void* resampler);
    int   (*resample)(void* resampler, const int16_t* in, int in_frames,
                      int16_t* out, int out_capacity);
};

struct sx_media_backend {
    const char* name;          /* "ffmpeg"; later "sxcodecs" */
    uint32_t abi_version;      /* SX_MEDIA_BACKEND_ABI */
    int priority;              /* larger is asked first, in every role */
    const struct sx_media_backend_ops* ops;   /* NULL roles = "not mine" */
    void* user;
};

/* Registration order is preserved; priority breaks ties. Returns 0 or -errno. */
int sx_media_register_backend(const struct sx_media_backend* backend);
int sx_media_backend_count(void);
const char* sx_media_backend_name_at(int index);
```

### The order the engine asks, and why it is that order

```text
claim_source   ->  open  ->  identify streams (with extradata)
                           ->  claim_stream, per stream
                           ->  open_decoder, per stream
              ->  pump: read_packet -> route by stream_index -> decode
```

`claim_stream` cannot come first: **you cannot know what a stream is until
somebody has parsed the container**, and parsing is the source role's job. So
the source is chosen and identified first, and the decoders are assigned
afterwards, from the descriptor list. This is also why `identify` and `open` are
the same call today — FFmpeg's `avformat_find_stream_info` does both — and why
splitting them later, for a provider that can read a container header but not
its full stream info, is an additive change.

`read_packet` belongs to the source handle and `decode_*` belongs to whichever
decoder claimed that `stream_index`. **The engine owns that routing.** It is the
only piece of code that knows both lists at once, and it is why a demuxer and a
decoder from two different libraries can meet in the middle without either
knowing the other exists.

### Who registers what, and when

**FFmpeg is registered as a codec library, not as a player.** The port compiles
`ports/ffmpeg/overlay/sxmedia/sxmedia_ffmpeg.c` and links it; that one file
fills all three roles at once, at priority 0:

- `claim_source` returns 1 for everything — FFmpeg has every demuxer the port
  enables.
- `claim_stream` returns 1 for every stream FFmpeg can open, so it owns every
  decoder.
- the scaler and the resampler are `swscale` and `swresample`.

That is the whole of "FFmpeg as the codec library that covers everything in
this first instance": one priority-0 entry whose every role is filled, and the
engine never asks anything else. The program is still the *same* program it is
today — the vtable changes no configure flag, no static link order, and no
`--enable-decoder` list. FFmpeg keeps the demuxer, because FFmpeg needs a
demuxer to hand out `extradata`, and that is not a limitation, it is the job.

**SxCodecs registers one role and leaves the other two null.** `claim_source` is
`NULL`, so the engine never asks it to open anything; FFmpeg always demuxes.
`claim_stream` is non-`NULL` and answers 1 for the codecs it has — a Vorbis
backend answers 1 for `"vorbis"` and nothing else, which is what the `ccleste`
and `doomgeneric` music gaps ask for (batch 4). Priority above FFmpeg's, so it
is asked first. The result is precisely "FFmpeg fills the gaps SxCodecs has not
covered", **per stream rather than per file**: an MP4 whose video is H.264 and
whose audio is AAC decodes video through SxCodecs and audio through
swresample-fed FFmpeg, from the same `AVFormatContext`, in the same process,
with neither library knowing the other is in the binary.

The `ops` pointers being individually `NULL` is what makes a partial provider
legal. A backend with only `claim_stream`/`open_decoder`/`decode_*` is a codec
library; one with all of them is a complete implementation; the difference is
data, not a second vtable or a second registry.

### "Covering everything" is not the same as being good at it

Worth stating before the first SxCodecs milestone is argued about, because
"FFmpeg covers all the gaps" is true and is also the reason to want a
replacement. Today's FFmpeg build is `--disable-asm --disable-pthreads` with
`thread_count = 1`, on one thread, on a kernel that has no threads:

- **~6.6 MiB of text**, every enabled decoder statically linked, `allcodecs.c`
  keeping the ones no program ever opens.
- **~18 MiB of BSS** in `ff_tx_tab_*`, mapped whole at exec.
- **No frame or slice threading** and **no assembly**, which is where most
  H.264/HEVC throughput on more cores comes from — and `SMP_ROADMAP.md` is the
  blocker for the first, `nasm` the blocker for the second.

So the gap SxCodecs fills is **not coverage, it is throughput and
independence**: a decoder SavanXP controls, on a core the scheduler can put on a
second CPU, with an assembly path and no LGPL surface in the shipped image. The
fallback model is what makes that a safe, incremental thing to attempt.

There is a second reason that is not about quality at all, and batch 4 is where
it shows: **coverage has a size.** Two ports carry a feature switched off, in a
running system, because shipping a full codec library for it is too expensive.
Every such gap is a small, well-specified decoder waiting to become a one-role
backend. FFmpeg covering everything is what the image pays for carrying FFmpeg;
SxCodecs is how a format stops costing that.

### The honest part: how much of this is speculative?

[`SYSTEM_LAYERING.md`](SYSTEM_LAYERING.md) is explicit that an API which exists
because a future runtime might want it is an API nobody is using. Applied
honestly:

- The **registry itself is not speculative.** It replaces two hardcoded facts
  that exist today — `MEDIA_PLAYER_PORT`, a path string, and `#include
  <libavcodec/...>` at the top of the decode half. A lookup is strictly less
  hardcoded than a string constant.
- **The three roles are not speculative either**, and this is the part worth
  checking. They are not "an extension point for a future codec library"; they
  are the separation that already exists inside one translation unit today.
  `media.c` opens the container, then decodes, then converts, and its three
  halves have exactly three different reasons to change: a new demuxer, a new
  decoder, a new pixel format. `extradata` is not a designed handoff, it is the
  `avcodec_parameters_to_context` call at `media.c:139` that already moves
  codec parameters from the demuxer to the decoder. Naming the boundary turns
  an accident of file layout into a contract, and it happens to be the boundary
  a second library needs.
- **The per-stream claim is the speculative part**, and it is speculative in
  exactly one direction: nothing today ever asks a second provider. The
  mitigation shipped in the same batch is a **partial** fake backend in the host
  tests — `claim_stream` for one codec, `claim_source` null — because that is
  the shape that does not exist yet in the image, and a test that only ever
  exercises the all-roles-filled case would not catch the routing being wrong.
- If SxCodecs never arrives, the honest collapse is "one backend, chosen when
  the program is built". The engine would then hold one source handle and one
  decoder per stream, and dropping the lists is a deletion rather than a
  rewrite, because the routing was already the only code that knew about both.
  Recording it here is the point: the decision stays open and cheap, instead of
  being taken silently in the first implementation.

## Packaging: one shell, two programs

The FFmpeg backend program keeps its distinct name and its manifest stays free
of `category=`, `mime_open=` and `ext_open=` — pinned by
`tests/host/system_mediaplayer_layout_test.py` and
`tests/host/ffmpeg_port_layout_test.py`. This is about the SXE metadata, not
about the `claim_*` hooks above, and the vtable does not change it. SxMedia does
not need it to.

What the registry buys is the **front end**: 830 lines that currently exist
inside the port. SxMedia's shell is a reusable window over a source, exactly as
`sxgui_app.c` is a reusable window over a widget list, so the recommended place
for it is the runtime, where both the CMake userland target and the port's
`runtime.sh` already compile their units from and can compile it too:

```text
subsystems/posix/sdk/v1/runtime/sxmedia.c         engine + registry
subsystems/posix/sdk/v1/runtime/sxmedia_sink.c    device/clock half + default sinks
subsystems/posix/sdk/v1/runtime/sxmedia_app.c     window, transport controls, input
subsystems/posix/sdk/v1/include/savanxp/sxmedia.h
ports/ffmpeg/overlay/sxmedia/sxmedia_ffmpeg.c    the only file the port keeps
subsystems/posix/userland/mediaplayer.c           main() + manifest, no backend
```

Which gives two programs from one shell:

- `/bin/mediaplayer` — shell + SxMedia, **no backend registered**. Keeps
  `category=Accessories` and the media associations. Shows "no media backend
  available", which is the current message with FFmpeg's name removed.
- `/disk/bin/mediaplayer-ffmpeg` — shell + SxMedia + the FFmpeg backend, built
  by the port. Plays.

The transport icons have to move with the shell (`gen_icons.py` and
`icons.inc`, ~4 KiB of art that is useless outside a player), and the base
program's own `icon=app-mediaplayer` stays a desktop icon in
`assets/`.

### Decision point: how does the base player reach the installed one?

Today it is a hardcoded `exec`. The registry does not change that by itself —
the backend is inside a *different program*, and there is no dynamic linker, so
the base player cannot call into it. Two implementations of the same idea:

1. **An SXE capability tag.** `media_backend=1` in a manifest becomes
   `SXE_TAG_MEDIA_BACKEND`, which is one line in `include/sxe/sxe_format.h` and
   one in the key map of `tools/gen_sxe_resources.py`. The launcher reads the
   tag through `sxe.h` and `exec`s the first installed program that declares
   it. No scanning, no per-launch cost, and the answer is declared by the
   program that *is* the backend.
2. **The existing user policy.** `ports/ffmpeg/install.sh` stages
   `/disk/assoc.ini` mapping the media extensions to
   `/disk/bin/mediaplayer-ffmpeg`; policy always beats the scan in
   `file_assoc.c`, and the scan already covers both `/bin` and `/disk/bin`.

**Recommendation: (1).** It is smaller, it puts the fact next to the code that
is true, and it needs no new file on the persistent volume to keep in sync
with the installer's logic. (2) is a fine fallback and needs no new tag.

Either way, the requirement is the same and it is worth stating: the base
player must not name any implementation. `grep libav subsystems/posix/userland`
has to stay empty, and the layout test is the place to say so.

Note what this does **not** settle. The base program keeps the media
associations, and those associations are a hand-written list of the FFmpeg
build's formats (seam 3). Delegating to "a program that declares itself a media
backend" fixes *which* program runs; it does not make the catalog's list a
function of what is actually linked in. The catalog question waits for a
capability publication, and the launcher question does not have to.

### Not decided here

Whether the shell belongs in the SDK runtime or next to the app. The argument
for the runtime — the layout above — is de-duplication and symmetry with
`sxgui_app.c`; the argument against is that a player window is an app, and
`SYSTEM_LAYERING.md` puts apps outside the platform. Both are defensible, and
this document does not close it. What **is** closed is the alternative of
keeping 830 duplicated lines in the port: batch 3 either moves the shell or
drops the front end from the port and lets the base player carry it, and both
of those are a superset of sharing it.

## What the kernel owes SxMedia

### Needed now: a real playback position

`AUDIO_IOC_GET_POSITION` — frames actually played — is item 1 of the
`MEDIA_PLAYER.md` roadmap and the only kernel change SxMedia requires:

- `ac97.cpp` can answer it from `CIV` and `PICB`, with the same wrap handling
  the status register already needs.
- `virtio_sound.cpp` in QEMU completes buffers immediately, so there the honest
  answer is "I do not know". It reports the capability as **absent** rather
  than lying, and SxMedia falls back to the wall clock.

`savanxp_audio_info.flags` exists and is unused, so the capability is a bit in
a field that is already there — additive, no struct growth, no ABI break. A new
ioctl number in `SAVANXP_IOCTL_GROUP_AUDIO` is the same kind of addition as
every other group in that file. The SDK minor version moves; the frozen v1
limits in `REFERENCE.md` do not.

With the bit set, `sx_media_audio_sink_device.played_position_us` answers in
microseconds of media time and the player stops estimating.

### Deliberately not now: a media device

`/dev/media0`, with the container and the codecs in the kernel, is the shape
Linux chose and it is the wrong shape for SavanXP today, for four reasons
worth keeping on the record:

1. **The kernel is a raw transport by policy.** `savanxp/audio.h` states it for
   sound; the same rule applied to video keeps LGPL-licensed FFmpeg out of the
   kernel image, which the `configure.sh` LGPL/GPL/nonfree guard and
   `SOURCE.md` already go to some length to keep true.
2. **The kernel has no threads** ([`SMP_ROADMAP.md`](SMP_ROADMAP.md)), so a
   decoder in the kernel is a decoder that cannot be preempted inside a
   syscall.
3. **There is no file-backed `mmap`** (`REFERENCE.md`, "No incluido en v1"), and
   a decoder that cannot map its input copies it through `read()`.
4. A hardware decoder, when one exists, gets **its own device node** and
   **one more SxMedia backend**. The rule that makes that cheap: *SxMedia is the
   only thing in the system allowed to speak a media device protocol.* That is
   the whole point of the sink seam, and it is why the answer to "where does
   decoding happen" can change without changing a single program.

### The end state, named

An installed codec that any program can use, without every program being
rebuilt, means a media **service** — a resident process that owns the backends
and answers requests over a channel, which is the `subsystems/native` style
deferred-until-after-v1.0 question applied to media. It needs threads, a
session/ownership model like `audio_device.cpp`'s, and a real ABI, and none of
those exist yet. Naming it here so that the batches below are understood as
steps toward it rather than as the destination.

## Memory, and what a binary costs

Two measured facts from `MEDIA_PLAYER.md` that the design has to respect:

- **~18 MiB of BSS**, almost all `ff_tx_tab_*`, mapped whole at exec by the
  loader. Not SxMedia's problem to fix and not a reason not to build it: the
  fix is lazy BSS in the kernel, in the `MEDIA_PLAYER.md` roadmap.
- **~6.6 MiB of text** with every enabled decoder statically linked.

And one the design adds:

- **The SDK heap is 256 KiB by default** (`SX_HEAP_SIZE` in
  `runtime/posix.c`), and the FFmpeg port does not raise it — every allocation
  FFmpeg makes, including decoded frames, comes out of that 256 KiB, through
  `posix_memalign` in `runtime/posix.c`. The engine's own footprint is the part
  SxMedia controls: one packet pool per stream (bounded, and the queue limit is
  a named constant, not a magic number), one decoded frame, one shown frame,
  one scaled buffer, one audio chunk. It must be a fixed, documented budget,
  and `sx_media_open` has to return a clean `ENOMEM` rather than a half-built
  source.

## Batches

Each batch is a commit or a small series, and each one is verifiable before the
next starts.

### Batch 1 — the seam (no behavior change)

Move `media.c` into `runtime/sxmedia.c` behind `savanxp/sxmedia.h`, and turn
`ports/ffmpeg/overlay/mediaplayer/media.c` into
`ports/ffmpeg/overlay/sxmedia/sxmedia_ffmpeg.c`, a backend that registers
itself. `media.h`/`media.c` leave the port overlay. The host test suite gets a
fake backend plus a real `sxmedia` target, so the registry has two members in
CI even though the image has one.

- `tests/host/ffmpeg_port_layout_test.py`: the expected overlay set changes to
  `sxmedia/sxmedia_ffmpeg.c`.
- The FFmpeg `configure.sh` and `link.sh` keep their pipeline; `runtime.sh`
  gains `sxmedia` in the `libsavanxp.a` unit list.
- `media.c`'s two failure sentences map onto `enum sx_media_status`
  (`"cannot open the file"` becomes `UNREADABLE` or `NO_DEMUXER` depending on
  what `avformat_open_input` actually failed on, and `"no stream with a
  supported codec"` becomes `NO_CODEC`), and every caller that showed the raw
  string now shows `sx_media_status_string`.

**Verification:** `./build.sh test`; `./build.sh smoke mediaplayer-selftest`
and `mediaplayer-display` pass with identical output; `tools/shoot.sh
--scenario mediaplayer` captures are **pixel-identical** before and after.
Nothing about what plays, or how, may change — batch 1 changes where the engine
lives, not what it can do, and the one visible difference is that a failure now
has a reason instead of a sentence.

### Batch 2 — sinks, and a real clock

`sxmedia_sink.c` with both vtables and the device sink; `playback.c` leaves the
port. The latency model, the lead, the drop policy and the stall restart move
into the sink, where they are describable as *a device's* properties instead of
*the player's* habits. Then `AUDIO_IOC_GET_POSITION` in `ac97.cpp`, the
capability bit, and `sx_media_audio_sink_device` using it when present.

- `REFERENCE.md` and `subsystems/posix/sdk/v1/README.md` gain the new ioctl and
  the capability bit; the SDK minor version moves.
- `savanxp-audio-test` gains a position test; a new smoke asserts the position
  advances on `ac97` and that the player still passes `--sync`.

**Verification:** `mediaplayer-selftest` unchanged; `--sync` still within 20 ms;
the new position scenario; `mediaplayer-display` pixel-identical.
`sx_audio_mixer` gains an `sx_media_audio_sink` adapter as a second sink, and
`sdk/gfxhello`-style external consumption is checked with
`tools/build-user.sh --media` (the `--gui`/`--audio` precedent).

### Batch 3 — one shell, two programs

The window and the transport move to `runtime/sxmedia_app.c`;
`subsystems/posix/userland/mediaplayer.c` becomes `main()` plus a manifest; the
port's 830-line `mediaplayer.c` and its `icons.inc` go away. The base launcher
stops naming FFmpeg and looks up a program that declares itself a media
backend.

- `system_mediaplayer_layout_test.py` inverts its own assertion: `libav` must
  not appear, and the `MEDIA_PLAYER_PORT` string must not either.
- `mediaplayer-availability` and `mediaplayer-missing` keep passing, and the
  "unavailable" text loses the word FFmpeg.
- `tools/build-user.sh` grows `--media`; a new `sdk/sxmediaplayer`-style sample
  proves an external program can play a file with the SDK alone.

**Verification:** the two existing smokes, the availability smokes, and a
pixel-identical `shoot.sh` capture of the transport.

### Batch 4 — SxCodecs starts as Vorbis (not scheduled, but scoped)

**The first SxCodecs is a Vorbis decoder, and the reason is that two ports
already have music switched off for want of one.**

- `ports/ccleste`: the five Celeste Classic tracks ship as OGG Vorbis,
  `CELESTE_P8_MUSIC` is a logged no-op, the `.ogg` files are not even copied,
  and the game runs silent. Decoding them to PCM on the host was measured at
  ~24 MB, which does not fit the persistent image.
- `ports/doomgeneric`: music is disabled for the same reason. Upstream's own
  `i_sdlmusic.c` in the pinned tree is an Ogg page reader with a Vorbis comment
  parser (`LOOP_START`, `LOOP_END`) — the code that would consume a decoded
  stream is already there, waiting for a decoder.

That is two independent consumers of the same missing capability, which is what
turns "speculative extension point" into a known backlog item. And it is a much
better first SxCodecs than the format FFmpeg does not ship that this document
originally guessed at, on every axis:

| | Vorbis | a proprietary video codec |
|---|---|---|
| spec | Vorbis I, fully published | often not |
| reference in C | `stb_vorbis`, one file, public domain or MIT | none |
| size | a few thousand lines | a full H.264/HEVC profile |
| already compiled into the image | yes — `ports/ffmpeg/configure.sh` enables `vorbis` and the `ogg` demuxer | n/a |
| licence entanglement | none | patents |
| does the tree have a consumer | two ports, right now | none |

**The shape is exactly the "codec library with one role" case.** A Vorbis
backend registers `claim_stream` for `codec == "vorbis"` at a priority above
FFmpeg's, with `claim_source`, the scaler and the resampler left `NULL`.
FFmpeg's Ogg demuxer keeps parsing the container and keeps owning every other
codec.

**And it is the validation of the `extradata` field.** Vorbis cannot be decoded
from packets alone: the identification, comment and setup headers — three
packets, in the first Ogg page — are the codec's entire configuration, and
`stb_vorbis`'s `open_memory` takes precisely those. That is what
`stream->extradata` is in `sx_media_stream_desc`, and a Vorbis backend is the
cleanest possible demonstration that the handoff works: it cannot be faked, and
a decoder that ignored the field would produce silence.

**What it does not fix by itself:** distribution. SxMedia's registry is
per-process and static, so `ccleste` cannot call into `mediaplayer-ffmpeg`
where a Vorbis decoder already exists. The decoder has to be linked into
`ccleste`, which is why the honest options are "a small Vorbis backend in the
tree" or "link FFmpeg into the game" — and the first is smaller by two orders
of magnitude and carries no LGPL surface. The other option, a media service
that both programs talk to, is the end state below.

#### What the Vorbis work found before it found a decoder

The first attempt at this backend was written against the wrong API and is
recorded here because what it found is worth more than the file was.

**`stb_vorbis` v1.22 does not have the API the design assumed.** There is no
`stb_vorbis_get_channels`, no `stb_vorbis_get_sample_rate` and no
`stb_vorbis_pushdata` in that revision. The shape that exists is
`stb_vorbis_open_pushdata` plus `stb_vorbis_decode_frame_pushdata`, and it
**outputs float**, not interleaved s16 -- so a "pass-through resampler" writes
raw floats into a buffer the sinks read as s16, which is not a wrong resample,
it is noise. Any Vorbis backend needs a float-to-s16 step, and where it lives
has to be decided rather than skipped.

**`stb_vorbis_open_memory` really does need the whole stream.** Its comment says
so and it means it: it parses the three headers and then pumps a first frame
from the same buffer, so a decoder opened on `extradata` alone decodes nothing.
The pushdata workflow is the one that matches a per-packet `send_packet` /
`receive_frame` split -- which is itself a point in favour of that split being
mandatory rather than a convenience.

**`decode_frame_pushdata` needs accumulation.** It returns `need_more_data` for
an incomplete block, so the backend has to keep a partial buffer across calls,
and that buffer has to be reset by both `flush` and any reposition. That is real
state, and it is state the engine does not know about.

**A Vorbis block is between 1 and 4096 sample frames**, and nothing bounds it
from below. That exposed an engine bug that has nothing to do with Vorbis: the
audio buffer was sized from a fixed 2048 frames, which is smaller than a
4096-frame block needs when the sink's rate is above the source's. The converter
would have been handed less than it wanted and the tail dropped -- a hole in the
audio with nothing reporting it. `sx_media_frame` gained `count` and the engine
now sizes from the block and the rate ratio. FFmpeg never hit it because
`swr_get_out_samples` reported the real figure; the fixed guess was luck.

**And then the decisive one, which is that `stb_vorbis` cannot be a decoder-role
backend at all.** Its pushdata workflow takes *file bytes*: `open_pushdata` is
handed the first N bytes of the file, page headers and all, and
`decode_frame_pushdata` resynchronises by finding Ogg page boundaries inside
what it is given. The other entry point, `open_memory`, wants the entire file
for the same reason. A demuxer hands out **packets**, with the Ogg framing
already stripped, which is exactly what FFmpeg's `oggparsevorbis` does when it
puts the three header packets into `extradata`. So the bytes stb_vorbis needs
and the bytes a decoder role is given are not the same bytes.

That is not a bug in the adapter. It is a mismatch of shape: **the decoder role
in this design is packet-oriented, and `stb_vorbis` is a whole-file decoder.**
Closing the gap would mean either handing a decoder the whole source, which is a
second decoder shape in the vtable, or giving SxCodecs its own Ogg demuxer,
which is a different project wearing this one's name.

So the recommendation in this section was wrong and is corrected here: Vorbis is
a real demand with two waiting consumers, and it is the wrong *first* occupant
for the role as the role is defined. A decoder for it has to be one of

- a **packet-oriented Vorbis decoder** -- `libopus`-shaped work, a
  `opus_decode`-style interface fed packet by packet, which is what the role
  wants and what stb is not; or
- a **whole-file decoder**, which means adding a second shape to the vtable
  before anything can use it, and that is a design change with its own
  justification rather than a first milestone.

### The second shape of the decoder role, and the loop it forced

`stb_vorbis` is a whole-file decoder, so the decoder role grew a second shape: a
provider that fills `open_whole` instead of `send_packet` is saying that this
stream is its own and it reads the source itself. The engine then routes no
packet to it, queues none, and calls only `open_whole`, `receive_frame` and
`seek_stream`.

Writing the test for it turned up something the header had claimed and the code
did not do. A whole-file decoder **cannot know whether it can read a file until it
has tried**: its claim is by codec name, on purpose, because that is all it has. So
`open_whole` returning NULL means "not this one", and treating that as final would
lose a stream the provider behind it can decode -- the exact opposite of what a
fallback is for. Choosing a decoder was a single lookup over the registry, and
it became a loop that asks every claimant for a turn and keeps the first one that
actually opens the stream. Both shapes go through it now, which is also the more
honest reading of a claim: an intention, not a guarantee.

The two outcomes of that NULL are worth separating, because they are different
properties and a provider needs to know which one it is relying on:

- **someone else can decode it** -- the stream plays, invisibly. A notice never
  appears because nothing was lost.
- **nobody else wants it** -- the stream is dropped with a reason, and the codec
  is named. That is the only case that reaches a program as a notice.

What it costs is written in the header and repeated here because it is a real
price and not an implementation detail: **a file with a self-fed stream in it is
read twice**, once by the provider that enumerates the streams and once by the
decoder. Both are sequential and read-only, and the engine stops pulling packets
once every stream is self-fed, so only the second read touches the payload. A
provider that wanted to avoid that would have to be the demuxer too.

The engine change the earlier work justified is in `sx_media_frame.count` and is
in: a block is between 1 and 4096 frames and nothing bounds it from below, and
the fixed 2048 the engine used to size the converter's buffer was smaller than a
large block needs at a higher sink rate.

And one trap the same work walked into, which is now in the header because a
caller gets it wrong destructively: `sx_media_read_audio`'s `frames` counts
**frames**, not samples, so the buffer must hold `frames * channels` shorts. A
test in the suite had exactly that bug -- `int16_t small[64]` for a 64-frame
stereo read -- and AddressSanitizer found it, which is the argument for running
under a sanitizer rather than trusting a green suite.

Getting the shape right is what the two provider paths are for, and this is the
case that says so.

**Verification**, and the second assertion is the one that matters:

- a Vorbis stream plays in a program that links only the Vorbis backend, and
- a file with a SxCodecs video or Vorbis stream **and** an FFmpeg-only stream
  plays both — same container, same process, two libraries, with the
  `--sync` selftest still landing every flash on its beep. That is what proves
  `read_packet` and `decode_*` are genuinely independent, and it cannot be
  written until a second provider exists.
- `ccleste` stops logging "sin reproductor de musica", which means the
  streaming-voice gap in the sinks section is closed as well.
- the notice the user sees names the codec and not the implementation, which is
  asserted the way the launcher assertion already is: a layout check that
  `libav` appears in no user-facing string in the tree, not only in
  `subsystems/posix/userland`.

## Alternatives considered and rejected

- **Wrap FFmpeg's public API and re-export it.** An `AVCodecContext*` in a
  SavanXP header would freeze FFmpeg's ABI as ours, and `MEDIA_PLAYER.md`
  already records that the port is the only FFmpeg integration kept. This is
  the opposite of what SxMedia is for: the engine must not know which library
  decoded the frame.
- **A kernel media stack (`/dev/media0`).** Four reasons above. Revisit only
  for a hardware decoder, and then as a *new device plus a new backend*, never
  as a change to the engine.
- **A dynamic plugin (`backend.so`).** SDK v1 has no dynamic linker and no
  shared libraries, and `REFERENCE.md` lists both under "No incluido en v1".
  Static registration at link time is the ABI, not a workaround.
- **A userland media service now.** The end state, not the next step; it needs
  threads and a session model. See "The end state, named".
- **A framework: encoders, filters, streaming, live sources, pipeline
  negotiation.** No consumer. The only two programs that will exist are a
  player and a game with a cutscene, and both want the same handful of calls
  in the same order. If a filter graph ever has a real consumer it goes in
  beside SxMedia, the way `SXGL` would go beside `SXGFX`
  ([`SXGFX_ROADMAP.md`](SXGFX_ROADMAP.md) makes that argument for 3D).
- **Fix the `#include <libavcodec>` and stop there.** That is batch 1 without
  the registry, and it delivers the de-duplication and the external-app reach
  but not the fallback. The registry is the part the user asked for, so it is
  the part that must land.

## Open questions

1. **Where the shell lives** (runtime vs. userland). Recommendation: runtime,
   on the `sxgui_app.c` precedent. See "Not decided here".
2. **Capability tag vs. association policy** for the base-to-installed
   handoff. Recommendation: the SXE tag.
3. **Does the player still need `swscale`?** A decoded frame is
   BGRX8888 at the source's resolution; the client surface is whatever size the
   user made the window. Scaling to an arbitrary size is `swscale`'s job today
   with `SWS_FAST_BILINEAR`, but `sx_scaled_presenter` already owns
   integer-scaled, centered, row-damaged presentation for exactly this pixel
   format. Whether the player presents through the presenter and blits, or keeps
   asking the backend to scale, is a measurable question (one captures, one
   profile) and should be settled with data rather than taste. It only matters
   once the shell is shared, because only then can a different consumer pick the
   other path.
4. **Text streams.** `sx_media_stream_desc.kind` includes
   `SX_MEDIA_KIND_TEXT` because `MEDIA_PLAYER.md` lists subtitles as a known
   next step and the attached-picture stream is already skipped. If no consumer
   appears, the enum value goes with them.
5. **Whether `sx_media_open` takes a path or an fd.** A path today. An fd is
   what a network stream or a pipe would want, and `--enable-protocol=file`
   means FFmpeg has no other protocol compiled in, so an fd buys nothing yet.
   The field is in the struct now so that adding it is not a signature change.
6. **Where the format list lives.** Seam 3. The honest answer today is "the
   base manifest, hand-written, stale by construction", and the honest
   question is whether SxMedia is the right layer to fix it at all — because a
   registry inside a process cannot answer a question the kernel's Files
   process asks. A first, cheap step that needs no service: have the
   `mediaplayer-ffmpeg` installer write a plain-text capability record under
   `/disk` (name, version, and the extensions it claims), and have the base
   player prefer it when present. That makes the message exact and gives the
   catalog something real to read later, at the cost of one file that has to be
   kept in sync with the manifest — which is already the failure mode, just
   written down where a test can see it.

## What a reader should take away

- SxMedia is not a new feature; it is four files that already exist, moved out
  of a port into the SDK, plus the registry that makes "a fallback backend" a
  data structure instead of a hardcoded path.
- The kernel stays a raw transport and gains exactly one ioctl.
- The three layers — engine, player, shell — keep the rule they already obey
  inside the port: each one knows nothing about the layer above it.
- SxMedia does not decide what SavanXP can play. It routes to whatever backend
  is linked in, answers whether a codec exists at all, and — when it does not —
  says so in one shared wording that names the codec and not the
  implementation.
- The registry ships with a test backend so that it is exercised from day one,
  and it is honest about the fact that its second real member, `SxCodecs`, is a
  project of its own — with a Vorbis decoder and two waiting consumers already
  naming it.
