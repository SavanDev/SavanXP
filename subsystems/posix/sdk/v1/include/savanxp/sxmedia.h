#pragma once

/* SxMedia: the userland half of multimedia.
 *
 * Playback is a raw PCM device in the kernel; this is the module that owns the
 * userland half, the same way `savanxp/audio.h` does for sound alone. It owns
 * the contract between a media source (container + codecs) on one side, the
 * media devices on the other, and the programs in between.
 *
 * The shape of it is three things and the boundary between them is the whole
 * design:
 *
 *   - the ENGINE knows a source and nothing else. No screen, no speaker, no
 *     window, no wall clock, and no file I/O: opening the source is the
 *     backend's job, which is why this half is testable without a kernel.
 *   - the BACKEND is registered as three separable roles -- source, decoder,
 *     converter. A library that ships no demuxer leaves `claim_source` NULL and
 *     a library that ships no decoder leaves the decoder role NULL. FFmpeg
 *     fills all three; a single-codec library fills one.
 *   - the PROGRAM asks whether a codec exists, plays if it does, and says so in
 *     one shared wording if it does not. A program never learns which library
 *     decoded anything: `sx_media_backend_name_at` is for `--probe` and the
 *     selftests, and no user-facing string in the tree may name a backend.
 *
 * The reason the decoder role is separate from the source role is the fallback
 * relationship. The demuxer and the decoder are allowed to come from different
 * libraries in the same process, which is only possible because the engine --
 * and only the engine -- knows both the packet source and the decoder list.
 * What crosses that boundary is `sx_media_stream_desc.extradata`, because an
 * H.264 or HEVC decoder cannot start without the parameter sets and only the
 * demuxer that parsed the container has them.
 *
 * All times cross this interface in microseconds from the start of the source,
 * with the container's `start_time` already subtracted, and SX_MEDIA_NO_TIME for
 * "unknown". No time base, pixel format or sample format leaves a backend.
 *
 * Link `runtime/sxmedia.c`, or build an external app with
 * `tools/build-user.sh --media`. A program registers its backend before calling
 * `sx_media_open`; a program with no backend registered gets
 * SX_MEDIA_ERR_NO_BACKEND and nothing else.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SX_MEDIA_NO_TIME INT64_MIN

/* Packets held per stream. Reached only if one stream goes unread for
 * minutes; the engine discards past this and counts it, rather than growing
 * without bound on a fixed arena. */
#define SX_MEDIA_QUEUE_PACKET_LIMIT 8192

#define SX_MEDIA_MAX_STREAMS 8
#define SX_MEDIA_MAX_BACKENDS 8
#define SX_MEDIA_MAX_MISSING 8
#define SX_MEDIA_CODEC_CAPACITY 32

/* Bumped only for a change that removes or reshapes an `ops` entry. Adding an
 * entry at the end of `sx_media_backend_ops` does not bump it: a provider
 * compiled against the older header leaves the new pointer NULL. */
#define SX_MEDIA_BACKEND_ABI 1u

/* Why a source did not open. A program cannot write a useful notice from a
 * boolean, and it must never have to parse a string to find out what happened.
 * Each of these wants a DIFFERENT sentence to the user, which is why they are
 * not merged:
 *
 *   NO_BACKEND   nobody registered at all    -> this build has no media backend
 *   NO_DEMUXER   no demuxer matched the file -> cannot read this format
 *   NO_CODEC     read it, codec not here     -> no <codec> decoder
 *   UNREADABLE   matched, then failed on it  -> damaged or not a media file
 *   UNSUPPORTED  read it, nothing playable inside
 *
 * NO_DEMUXER and NO_CODEC both arrive as "it will not play", and the user can
 * only act on one of them. */
enum sx_media_status {
    SX_MEDIA_OK = 0,
    SX_MEDIA_ERR_NO_BACKEND,
    SX_MEDIA_ERR_NO_DEMUXER,
    SX_MEDIA_ERR_NO_CODEC,
    SX_MEDIA_ERR_UNREADABLE,
    SX_MEDIA_ERR_UNSUPPORTED,
    SX_MEDIA_ERR_NOMEM,
};

enum sx_media_stream_kind {
    /* What a decoder must be asked with when `sx_media_has_codec` is checking
     * a name it has no stream for. A provider's `claim_stream` must decide on
     * `codec` alone when it sees this. */
    SX_MEDIA_KIND_ANY = 0,
    SX_MEDIA_KIND_VIDEO = 1,
    SX_MEDIA_KIND_AUDIO = 2,
    SX_MEDIA_KIND_TEXT = 3,
};

/* The one wording for each status, so no program writes its own. The same rule
 * `result_error_string` follows for SAVANXP_E*. */
const char* sx_media_status_string(enum sx_media_status status);

/* ---- capability, before anything is opened -------------------------------
 *
 * The question a program asks to decide whether to try at all, and to have
 * something to say before the user has picked a file. Static, cheap, answered
 * by the registry, and the reason the whole "the codec exists or it does not"
 * contract is answerable without opening anything. */
int sx_media_has_codec(const char* codec);
int sx_media_backend_count(void);
const char* sx_media_backend_name_at(int index);

/* ---- backend registry ---------------------------------------------------- */

struct sx_media_source;

/* What the caller wants to play. `audio_out` of NULL means "ignore audio",
 * which is what lets a headless consumer decode video without naming a device.
 * The engine passes this to the backend and never opens it itself. */
struct sx_media_audio_format {
    int sample_rate;
    int channels;
};

struct sx_media_source {
    const char* path;  /* or fd >= 0, never both */
    int fd;
    const struct sx_media_audio_format* audio_out;
};

/* A stream as the source provider found it, before any decoder was chosen.
 * `extradata` is the field that makes a decoder from another library possible:
 * the identification/comment/setup headers of Vorbis, the parameter sets of
 * H.264 or HEVC, an AAC AudioSpecificConfig. Out of band in Matroska and MP4,
 * so a decoder has no way to invent them. Borrowed from the source handle and
 * valid until the source is closed. */
struct sx_media_stream_desc {
    int index;      /* within the source; -1 if absent */
    int kind;       /* enum sx_media_stream_kind */
    char codec[SX_MEDIA_CODEC_CAPACITY];
    int width, height;
    int sample_rate, channels;
    int sar_num, sar_den;
    uint64_t duration_us;
    /* One frame of this stream, 0 when the container does not say. The engine
     * falls back to 40 ms, and needs no backend call to ask. */
    int64_t frame_duration_us;
    const uint8_t* extradata;
    size_t extradata_size;
    /* Codec setup the source provider published for this stream, opaque to the
     * engine and to any other provider. This is what a decoder needs beyond the
     * fields above, and it exists because the honest answer to "what does a
     * decoder need to configure itself" is "whatever its library's context
     * struct wants", which no portable field list can be.
     *
     * A decoder provider that reads only `codec` and `extradata` leaves this
     * NULL and works with any source provider -- that is the case that makes a
     * cross-library decoder possible at all. One that needs its library's own
     * setup type can only be fed by a source provider that produces the same
     * type, which in practice means the same library filling both roles, and
     * that is a real limit rather than an oversight: decoder configuration *is*
     * demuxer data.
     *
     * Borrowed from the source handle, valid until the source is closed. NULL
     * when the source provider had nothing to publish. */
    const void* setup;
};

/* A packet is BORROWED: `data` is valid until the next `read_packet` on the
 * same handle. The engine copies it into its own bounded queue, which is what
 * keeps the queue backend-agnostic and makes the engine's memory cost its own. */
struct sx_media_packet {
    int stream_index;
    int64_t time_us;
    int key;
    const uint8_t* data;
    size_t size;
};

/* A decoded frame, opaque to the engine. The engine never reads the samples or
 * the pixels; it hands the frame to the same provider's scaler or resampler, so
 * the pixel and sample formats never leave the library that chose them.
 *
 * What the engine does read is metadata, because three decisions are made with
 * it and none of them belong to a library: `time_us`, which is the whole A/V
 * contract; the video size and sample aspect, which decide what gets presented;
 * and the audio shape, which decides whether the converter has to be rebuilt --
 * a resampler built for one sample format is wrong for the next frame if the
 * stream changes it mid-flight, which is exactly what `media.c` had to handle.
 *
 * One type for audio and video on purpose: an audio decoder has no business
 * filling `width`, and with two types that mistake was representable. */
struct sx_media_frame {
    int64_t time_us;
    int width, height;            /* video, as decoded; 0 for audio */
    int sample_rate, channels;    /* audio, as decoded; 0 for video */
    int format;                   /* backend-private format tag; see above */
    int sar_num, sar_den;
    int key;
    /* Audio: how many sample FRAMES `data` holds, not how many interleaved
     * values. A resampler cannot write a correct amount without it, and a
     * backend has no way to say "a whole block" -- blocks differ. Video: 0,
     * because a picture's extent is its width and height.
     *
     * It is here because the Vorbis backend is what made it necessary: a
     * Vorbis block is between one and 4096 frames, and `out_capacity` alone does
     * not say which. */
    int count;
    void* data;                   /* backend-private, never read here */
    void (*release)(void* data);
};

struct sx_media_backend_ops {
    /* -- source role --------------------------------------------------------
     * The demuxer, and the only role that touches the file. */

    /* Can this provider parse this source at all? Asked before anything is
     * opened, so it must be cheap and must not keep state. Returns 1 to take
     * the source, 0 to let the next provider try. A codec library that ships
     * no demuxer leaves this NULL. */
    int (*claim_source)(void* user, const struct sx_media_source* source);

    /* Parses headers and enumerates streams. Consumes no packets. Returns
     * SX_MEDIA_OK, or the reason -- `NO_DEMUXER` when no demuxer matched and
     * `UNREADABLE` when one did and then failed on it, which is the
     * distinction a bare "open failed" does not make and which the engine must
     * not have to recover by matching on strings. */
    void* (*open)(void* user, const struct sx_media_source* source,
                  struct sx_media_stream_desc* streams, int stream_capacity,
                  int* stream_count, int64_t* duration_us, int* seekable,
                  enum sx_media_status* status);
    void (*close)(void* handle);
    int (*seek)(void* handle, int64_t target_us);
    int (*read_packet)(void* handle, struct sx_media_packet* packet);
    /* Diagnostics only. The container's name and its tags are the provider's to
     * report; a program shows a codec name to the user and a provider name to
     * --probe, and neither comes from a hardcoded string in the engine. */
    const char* (*container_name)(void* handle);
    const char* (*metadata)(void* handle, const char* key);

    /* -- decoder role -------------------------------------------------------
     * Claimed per stream, so one file can mix decoders from two libraries. */

    /* Asked once per stream, after the source is identified and before it is
     * opened, so the decoders are chosen before the first packet is read.
     * Returns 1 to own the stream, 0 to let the next provider try. Leaving the
     * whole role NULL is how a provider says "I only demux and convert".
     *
     * It is also called by `sx_media_has_codec` with a descriptor whose only
     * populated fields are `codec` and `kind`, and `kind` is
     * SX_MEDIA_KIND_ANY there: decide on the codec name alone when you see it,
     * or the capability query will answer "no" for a codec you can decode. */
    int (*claim_stream)(void* user, const struct sx_media_stream_desc* stream);

    /* Takes the descriptor and nothing else -- deliberately NOT the source
     * handle. `stream->extradata` is the whole of what crosses over. Returns
     * NULL if the decoder cannot be built after all, which drops the stream
     * rather than failing the source. */
    void* (*open_decoder)(void* user, const struct sx_media_stream_desc* stream);
    void (*close_decoder)(void* decoder);
    /* Drops whatever the decoder is holding, which after a reposition is not
     * just waste: a decoder left holding reference frames from before the seek
     * produces the wrong picture, and libavcodec requires this after every
     * seek. Called by the engine on every stream when `sx_media_seek` succeeds.
     * NULL is allowed and means "there is nothing buffered". */
    void (*flush)(void* decoder);

    /* Feeding and draining are separate, because every real decoder has this
     * shape: a decoder that has been sent input may have several frames
     * pending, and the engine has to be able to drain it without sending more.
     *
     * A packet is valid for the duration of the call only, and a NULL packet
     * is the end of stream -- the "flush" that makes the decoder emit whatever
     * is still inside it. A backend that needs the bytes afterwards must take
     * its own reference first; FFmpeg's `av_packet_ref` does, and its buffers
     * are refcounted, so the engine freeing its copy is a decrement and not a
     * dangling pointer.
     *
     * Returns 1 = consumed, 0 = wants more input, < 0 = the packet is corrupt
     * and is skipped. A decoder resynchronizes on the next keyframe by itself,
     * so an error here is not fatal to the stream. */
    int (*send_packet)(void* decoder, const struct sx_media_packet* packet);

    /* 1 = a frame is ready, 0 = nothing pending yet, < 0 = end of stream. The
     * frame's `data` is backend-private and the engine hands it back to this
     * same provider's scaler or resampler; `release` is how it lets go of it. */
    int (*receive_frame)(void* decoder, struct sx_media_frame* frame);

    /* -- decoder role, whole-source variant --------------------------------
     *
     * A decoder role is packet-oriented by default: the demuxer reads packets and
     * the engine routes them to `send_packet`. Some libraries are not built that
     * way and cannot be. `stb_vorbis` is the case that motivated this -- its entry
     * points want the *file*, since `open_memory` takes the whole thing and the
     * pushdata workflow resynchronises by finding Ogg page boundaries inside the
     * bytes it is handed, while a demuxer hands out packets with that framing
     * already stripped. The two are not the same bytes and no adapter bridges
     * them.
     *
     * A provider that fills `open_whole` instead of `send_packet` is saying: this
     * stream is mine and I read the source myself. The engine then routes no
     * packet to it, queues none, and calls only `open_whole`, `receive_frame`
     * and `seek_stream`. The stream is still claimed by `claim_stream` in the
     * ordinary way, and a NULL from `open_whole` still drops the stream with a
     * reason rather than failing the source, so a whole-file decoder that cannot
     * read a particular file falls back to the next provider like any other.
     *
     * What it costs, stated plainly: a file with a self-fed stream in it is read
     * twice -- once by the provider that enumerates the streams and once by the
     * decoder. Both are sequential and read-only, and the engine stops pulling
     * packets once every stream is self-fed, so only the second read touches the
     * payload. A provider that wanted to avoid that entirely would have to be the
     * demuxer as well, which is what `claim_source` is for.
     */
    void* (*open_whole)(void* user, const struct sx_media_source* source,
                        const struct sx_media_stream_desc* stream);
    /* Repositions a self-fed stream. Optional: without it the stream cannot be
     * seeked and `sx_media_seek` fails for the source, which is honest. The
     * target is microseconds from the start of the source. */
    int (*seek_stream)(void* decoder, int64_t target_us);

    /* -- converter role -----------------------------------------------------
     *
     * These are the *frame's own* provider's, never the source provider's, and
     * that is a rule rather than a convention. A frame's `data` is opaque and
     * belongs to the library that produced it, so the converter that reads it has
     * to be that library's: the alternative is handing a block of interleaved s16
     * to a scaler that casts it to `AVFrame*`, which reads a channel layout out
     * of the middle of the PCM and fails with nothing to report.
     *
     * A source provider hands out packets, never frames, so it has nothing to
     * convert and filling this role on one is dead weight at best. A decoder
     * without a converter produces no converted audio for its stream, and says
     * so; there is deliberately no fallback to another provider's converter,
     * because a fallback reads somebody else's bytes as its own and looks like
     * it works whenever the bytes happen to line up.
     * To the formats the consumers fixed, never negotiated.
     *
     * Both converters take a prototype FRAME and not the stream descriptor,
     * because a converter is built for one input format and the container's
     * declared format is not always the format the next frame arrives in. A
     * stream whose sample format changes mid-flight needs a new resampler, and
     * only the frame knows. The engine rebuilds when the prototype changes. */

    void* (*scaler_open)(void* user, const struct sx_media_frame* prototype,
                         int width, int height);
    void (*scaler_close)(void* scaler);
    int (*scale)(void* scaler, const struct sx_media_frame* frame,
                 uint32_t* out, int width, int height, int stride);

    void* (*resampler_open)(void* user, const struct sx_media_frame* prototype,
                            const struct sx_media_audio_format* dst);
    void (*resampler_close)(void* resampler);
    /* Writes interleaved s16 and returns how many frames, or < 0 on error. At
     * most `frame->count` and at most `out_capacity`; a frame with no room left
     * is not an error, it is a block that has to be taken again. */
    int (*resample)(void* resampler, const struct sx_media_frame* frame,
                    int16_t* out, int out_capacity);
    /* The resampler's own delay, drained once the stream is over so the tail is
     * not lost. 0 when it has none. Returns how many frames it wrote. */
    int (*resample_flush)(void* resampler, int16_t* out, int out_capacity);
};

struct sx_media_backend {
    const char* name;
    uint32_t abi_version;
    int priority;  /* larger is asked first, in every role */
    const struct sx_media_backend_ops* ops;
    void* user;
};

/* Registers a backend for this process. There is no removal and no unload:
 * the SDK has no dynamic linker, so registration is a link-time fact.
 * Registration order is preserved and priority breaks ties. Returns 0, or
 * -EINVAL / -ENOSPC / -EEXIST. */
int sx_media_register_backend(const struct sx_media_backend* backend);

/* ---- the engine ---------------------------------------------------------- */

struct sx_media;  /* opaque */

/* Allocates an engine. The source is opened separately, so an engine can
 * outlive one source. */
int sx_media_create(struct sx_media** out);
void sx_media_destroy(struct sx_media* media);

/* Opens `source` with the first provider that claims it, identifies the
 * streams, then assigns a decoder to each one. A stream nobody claims is not
 * a failure: the source opens, the stream plays as silence, and it shows up in
 * `sx_media_stream_missing_count`. Returns 0 on success, or < 0 with `status`
 * set to the reason. */
int sx_media_open(struct sx_media* media, const struct sx_media_source* source,
                  enum sx_media_status* status);
void sx_media_close(struct sx_media* media);

const char* sx_media_container_name(const struct sx_media* media);
int sx_media_has_video(const struct sx_media* media);
int sx_media_has_audio(const struct sx_media* media);

/* Whether the source can be repositioned, so a program can grey out a seek bar
 * without having opened a second time to find out. */
int sx_media_is_seekable(const struct sx_media* media);

/* The codec of the stream actually being played, for a title line or a
 * property panel. Not "the codec of some stream": a program that wanted that
 * would ask `sx_media_stream_codec` with an index it chose. */
const char* sx_media_video_codec(const struct sx_media* media);
const char* sx_media_audio_codec(const struct sx_media* media);

/* The audio format of the source, as opposed to the interleaved s16 the sinks
 * asked for. Diagnostics and a resampler's own reporting; a player does not
 * need it, because it asked for its format in the first place. */
int sx_media_audio_source_format(const struct sx_media* media,
                                 struct sx_media_audio_format* out);
void sx_media_display_size(const struct sx_media* media, int* width, int* height);
int64_t sx_media_duration_us(const struct sx_media* media);
int64_t sx_media_frame_duration_us(const struct sx_media* media);
const char* sx_media_metadata(const struct sx_media* media, const char* key);

/* ---- streams, including the ones that were dropped ----------------------- */

int sx_media_stream_count(const struct sx_media* media);
int sx_media_stream_decodable(const struct sx_media* media, int stream_index);
const char* sx_media_stream_codec(const struct sx_media* media, int stream_index);

/* Play what you can and say what you did not get, which is what every player
 * does and what makes per-stream claiming worth having. A count of 0 is the
 * only case where the source plays completely. */
int sx_media_stream_missing_count(const struct sx_media* media);
int sx_media_stream_missing_at(const struct sx_media* media, int index,
                               char* codec, size_t codec_capacity);

/* Packets discarded because a stream's queue was full, summed over streams. A
 * healthy file never reaches the cap, and the selftest asserts this is zero: a
 * dropped packet is a hole in the audio and nothing else would report it. */
int sx_media_packet_drops(const struct sx_media* media);

/* ---- decoding ------------------------------------------------------------ */

/* 1 = a frame is ready, 0 = the stream is finished, < 0 = error. */
int sx_media_next_video_frame(struct sx_media* media);
void sx_media_show_video_frame(struct sx_media* media);
int sx_media_has_shown_frame(const struct sx_media* media);
int64_t sx_media_video_time_us(const struct sx_media* media);

/* Scales the shown frame to width x height in BGRX8888 -- the byte order of
 * `gfx_rgb`, which is what a SavanXP surface and SXGUI both use. The buffer is
 * the engine's and is valid until the next call. NULL if the backend has no
 * scaler or the allocation failed. */
const uint32_t* sx_media_scale_video(struct sx_media* media, int width, int height);

/* Fills `out` with up to `frames` interleaved s16 FRAMES, in the sink's
 * format, and returns how many it wrote; 0 when the audio is finished.
 *
 * `frames` counts frames, not samples: the buffer must hold
 * `frames * sink_channels` shorts, and the return value is in the same unit.
 * A caller that sizes the buffer by `frames` alone writes past the end of it,
 * which is why it is said here rather than left to the reader.
 *
 * `first_time_us`, when not NULL, receives the time of the first frame written,
 * or SX_MEDIA_NO_TIME if nothing was. */
int sx_media_read_audio(struct sx_media* media, int16_t* out, int frames,
                        int64_t* first_time_us);

/* Repositions to `target_us`. After a seek the first frame and the first sample
 * delivered are the ones at the requested instant, not the keyframe's. 1 = ok,
 * 0 = this source cannot be repositioned. */
int sx_media_seek(struct sx_media* media, int64_t target_us);

/* Suppresses delivery before `time_us` without seeking: the resume point after
 * a device stall, where everything already written was heard whole. Each stream
 * clears its own skip mark on its first delivery, so a timestamp that goes
 * backwards later is not discarded. */
void sx_media_skip_until(struct sx_media* media, int64_t time_us);

#ifdef __cplusplus
}
#endif
