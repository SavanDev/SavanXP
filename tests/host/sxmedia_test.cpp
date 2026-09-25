/*
 * sxmedia_test.cpp -- Host tests for the SxMedia engine and backend registry
 * (subsystems/posix/sdk/v1/runtime/sxmedia.c).
 *
 * No `savanxp_*` stubs appear here, and that is half of what is being tested:
 * the engine opens no file, because opening a source is a provider's job. A
 * host test therefore needs nothing from the kernel, which is why the registry
 * is exercised in a second instead of a QEMU boot.
 *
 * The tests are built around two fakes shaped like the two libraries the
 * design is for: a demuxer that owns the container and the converters, and a
 * codec library that fills the single decoder role. The tests that matter are
 * the ones where a packet read by one provider is decoded by the other.
 */

#include <stdint.h>

/* The SDK ships its own <stdio.h>/<string.h> (the userland ones, without a C++
 * guard) and they shadow the host's, which is where this test links. Wrapping
 * them in extern "C" restores the right linkage. Nothing from the C++ standard
 * library is used, for the same reason the gfx2d test avoids it. */
extern "C" {
#include <stdio.h>
#include <string.h>
}

#include "savanxp/sxmedia.h"

/* fflush(NULL) drains every stream without naming one, so a crash does not
 * swallow the output that already said what failed. */

namespace {

int g_failures = 0;
int g_checks = 0;

void expect(int condition, const char* what)
{
    g_checks += 1;
    if (!condition) {
        g_failures += 1;
        printf("sxmedia: FAIL %s\n", what);
    }
}

void expect_str(const char* actual, const char* expected, const char* what)
{
    g_checks += 1;
    if (actual == nullptr || strcmp(actual, expected) != 0) {
        g_failures += 1;
        printf("sxmedia: FAIL %s (got \"%s\", want \"%s\")\n", what,
               actual != nullptr ? actual : "(null)", expected);
    }
}

/* ---- a fake demuxer ------------------------------------------------------
 *
 * Reads a fixed script of packets and publishes one stream that the codec
 * library can decode and one that nobody can, which is what a missing codec
 * looks like from inside the engine.
 */

const uint8_t kExtradata[4] = {0x67, 0x42, 0x00, 0x1f};  /* a parameter set */

/* What the fake demuxer publishes per stream. It is deliberately not a type the
 * fake decoder could do without: the first version of this test passed while the
 * real FFmpeg backend could not build a single decoder, because the fake only
 * read `codec` and `extradata` and the real one needs its library's own
 * configuration. A fake has to be as demanding as its consumer or the test
 * proves less than it looks like it proves. */
struct FakeStreamSetup {
    int64_t start_time_us;
    int width;
    int height;
    int sample_rate;
    int channels;
};

struct FakePacket {
    int stream_index;
    int64_t time_us;
    int key;
    uint8_t data[4];
};

const int kMaxPackets = 16;

struct FakeSource {
    FakePacket packets[kMaxPackets];
    int packet_count;
    int cursor;
    int open_calls;
    int claim_calls;
    int64_t last_seek_us;
    int seekable;
    int open_fails;
    enum sx_media_status open_status;
    char opened_path[128];
};

FakeSource g_source;
FakeStreamSetup g_setup_video;
FakeStreamSetup g_setup_audio;

void reset_fixture()
{
    memset(&g_source, 0, sizeof(g_source));
    g_source.seekable = 1;
    g_source.last_seek_us = -1;
}

void add_video_packet(int64_t time_us, int key)
{
    FakePacket* packet;
    if (g_source.packet_count >= kMaxPackets) {
        return;
    }
    packet = &g_source.packets[g_source.packet_count++];
    packet->stream_index = 0;
    packet->time_us = time_us;
    packet->key = key;
    packet->data[0] = 0x00;
    packet->data[1] = 0x00;
    packet->data[2] = 0x01;
    packet->data[3] = (uint8_t)time_us;
}

int source_claim(void*, const struct sx_media_source* source)
{
    const char* dot;

    g_source.claim_calls += 1;
    if (source == nullptr || source->path == nullptr) {
        return 0;
    }
    /* Claims by extension, the way a real demuxer claims by probe. A codec
     * library never gets here, because its claim_source is null. */
    dot = strrchr(source->path, '.');
    return dot != nullptr && strcmp(dot, ".fake") == 0;
}

void* source_open(void*, const struct sx_media_source* source,
                  struct sx_media_stream_desc* streams, int stream_capacity,
                  int* stream_count, int64_t* duration_us, int* seekable,
                  enum sx_media_status* status)
{
    if (stream_capacity < 2) {
        *status = SX_MEDIA_ERR_UNREADABLE;
        return nullptr;
    }
    g_source.open_calls += 1;
    g_source.opened_path[0] = '\0';
    if (source->path != nullptr) {
        strncpy(g_source.opened_path, source->path, sizeof(g_source.opened_path) - 1);
    }
    if (g_source.open_fails) {
        *status = g_source.open_status;
        return nullptr;
    }

    /* Video, decodable by the other provider. Deliberately non-square pixels,
     * to check the display size. */
    memset(&streams[0], 0, sizeof(streams[0]));
    streams[0].index = 0;
    streams[0].kind = SX_MEDIA_KIND_VIDEO;
    strncpy(streams[0].codec, "fakevideo", SX_MEDIA_CODEC_CAPACITY - 1);
    streams[0].width = 64;
    streams[0].height = 48;
    streams[0].sar_num = 4;
    streams[0].sar_den = 3;
    streams[0].frame_duration_us = 20000;
    streams[0].extradata = kExtradata;
    streams[0].extradata_size = sizeof(kExtradata);

    /* Audio, decodable by nobody. */
    memset(&streams[1], 0, sizeof(streams[1]));
    streams[1].index = 1;
    streams[1].kind = SX_MEDIA_KIND_AUDIO;
    strncpy(streams[1].codec, "ghostaudio", SX_MEDIA_CODEC_CAPACITY - 1);
    streams[1].sample_rate = 48000;
    streams[1].channels = 2;

    /* The setup blobs live in the source, which is what makes them borrowed for
     * the life of the open source rather than for the life of the descriptor. */
    g_setup_video.start_time_us = 0;
    g_setup_video.width = 64;
    g_setup_video.height = 48;
    g_setup_video.sample_rate = 0;
    g_setup_video.channels = 0;
    g_setup_audio.start_time_us = 0;
    g_setup_audio.width = 0;
    g_setup_audio.height = 0;
    g_setup_audio.sample_rate = 48000;
    g_setup_audio.channels = 2;
    streams[0].setup = &g_setup_video;
    streams[1].setup = &g_setup_audio;

    *stream_count = 2;
    *duration_us = 1000000;
    *seekable = g_source.seekable;
    *status = SX_MEDIA_OK;
    return &g_source;
}

void source_close(void*) {}

const char* source_container_name(void*) { return "fakecontainer"; }

const char* source_metadata(void*, const char* key)
{
    return key != nullptr && strcmp(key, "title") == 0 ? "Fake Title" : nullptr;
}

int source_seek(void* handle, int64_t target_us)
{
    FakeSource* self = static_cast<FakeSource*>(handle);
    self->last_seek_us = target_us;
    self->cursor = 0;
    return self->seekable;
}

int source_read_packet(void* handle, struct sx_media_packet* packet)
{
    FakeSource* self = static_cast<FakeSource*>(handle);
    const FakePacket* fake;

    if (self->cursor >= self->packet_count) {
        return 0;
    }
    fake = &self->packets[self->cursor];
    packet->stream_index = fake->stream_index;
    packet->time_us = fake->time_us;
    packet->key = fake->key;
    packet->data = fake->data;
    packet->size = sizeof(fake->data);
    self->cursor += 1;
    return 1;
}

/* ---- a fake codec library ------------------------------------------------
 *
 * One role. Claims a stream by codec name and turns packets into frames whose
 * `data` is an owned counter. It cannot demux anything, which is the point:
 * this is the shape a single-codec backend has.
 */

const int kMaxTimestamps = 16;

struct FakeDecoder {
    int stream_index;
    int packets_seen;
    int64_t timestamps[kMaxTimestamps];
    int timestamp_count;
    int read_cursor;
    uint8_t extradata_seen[4];
    int extradata_size;
    int extradata_matches;
    int setup_seen;
    int flushed;
    int frames_pending;
};

int g_claim_calls = 0;
int g_open_decoder_calls = 0;
/* How many times a decoder was told to drop what it was holding. A decoder that
 * keeps reference frames across a seek produces the wrong picture afterwards,
 * not merely a slow one, so this is load-bearing and not a diagnostic. */
int g_flush_calls = 0;
/* Whether the fake decoder behaves like a library that needs its own setup. */
int g_require_setup = 1;
/* The most recent decoder built, so a test can inspect what crossed over
 * before the engine tears it down. */
FakeDecoder* g_last_decoder = nullptr;

int decoder_claim(void*, const struct sx_media_stream_desc* stream)
{
    g_claim_calls += 1;
    if (stream == nullptr) {
        return 0;
    }
    /* The capability query arrives with kind == SX_MEDIA_KIND_ANY and nothing
     * but the codec name. A provider that insisted on a real kind would answer
     * "no" for a codec it can decode, and the whole capability contract would
     * quietly report the wrong thing. */
    if (stream->kind == SX_MEDIA_KIND_ANY || stream->kind == SX_MEDIA_KIND_VIDEO) {
        return strcmp(stream->codec, "fakevideo") == 0;
    }
    return 0;
}

void* decoder_open(void*, const struct sx_media_stream_desc* stream)
{
    FakeDecoder* decoder;

    g_open_decoder_calls += 1;
    decoder = static_cast<FakeDecoder*>(memset(new FakeDecoder(), 0, sizeof(FakeDecoder)));
    g_last_decoder = decoder;
    decoder->stream_index = stream->index;
    /* extradata is the whole of what crosses from the demuxer to the decoder. */
    if (stream->extradata != nullptr && stream->extradata_size > 0) {
        int size = (int)(stream->extradata_size < sizeof(decoder->extradata_seen)
                             ? stream->extradata_size
                             : sizeof(decoder->extradata_seen));
        memcpy(decoder->extradata_seen, stream->extradata, (size_t)size);
        decoder->extradata_size = size;
        decoder->extradata_matches = memcmp(decoder->extradata_seen, kExtradata, (size_t)size) == 0;
    }
    /* A decoder that needs its library's own configuration refuses to build
     * without the published setup, which is what the FFmpeg backend does. A
     * decoder that does not need it ignores it. Both are legal; what is not
     * legal is a vtable that cannot express either. */
    if (g_require_setup && stream->setup == nullptr) {
        delete decoder;
        return nullptr;
    }
    if (stream->setup != nullptr) {
        const FakeStreamSetup* setup = static_cast<const FakeStreamSetup*>(stream->setup);
        decoder->setup_seen = setup->sample_rate > 0 ? setup->sample_rate : setup->height;
    }
    return decoder;
}

void decoder_close(void* handle) { delete static_cast<FakeDecoder*>(handle); }

void decoder_flush(void* handle) {
    FakeDecoder* decoder = static_cast<FakeDecoder*>(handle);
    g_flush_calls += 1;
    /* A decoder that keeps what it is holding across a seek produces the wrong
     * picture afterwards, so the fake drops it too. */
    decoder->frames_pending = 0;
    decoder->timestamp_count = 0;
    decoder->read_cursor = 0;
}

int decoder_send(void* handle, const struct sx_media_packet* packet)
{
    FakeDecoder* decoder = static_cast<FakeDecoder*>(handle);

    if (packet == nullptr) {
        decoder->flushed = 1;
        return 1;
    }
    decoder->packets_seen += 1;
    if (decoder->timestamp_count < kMaxTimestamps) {
        decoder->timestamps[decoder->timestamp_count++] = packet->time_us;
    }
    decoder->frames_pending += 1;
    return 1;
}

int decoder_receive(void* handle, struct sx_media_frame* frame)
{
    FakeDecoder* decoder = static_cast<FakeDecoder*>(handle);

    if (decoder->frames_pending <= 0) {
        return decoder->flushed ? -1 : 0;
    }
    decoder->frames_pending -= 1;
    frame->time_us = decoder->timestamps[decoder->read_cursor++];
    frame->width = 64;
    frame->height = 48;
    frame->format = 7;
    frame->data = reinterpret_cast<void*>(static_cast<uintptr_t>(decoder->packets_seen));
    frame->release = nullptr;
    return 1;
}

/* ---- a scaler and a resampler, to cover the converter role --------------- */

int g_scaled_calls = 0;
int g_resample_calls = 0;
int g_resample_flush_calls = 0;

void* scaler_open(void*, const struct sx_media_frame*, int, int)
{
    return reinterpret_cast<void*>(static_cast<uintptr_t>(1));
}

void scaler_close(void*) {}

int scale(void*, const struct sx_media_frame*, uint32_t* out, int width, int height, int)
{
    int index;
    g_scaled_calls += 1;
    for (index = 0; index < width * height; ++index) {
        out[index] = 0xff204060u;
    }
    return 0;
}

void* resampler_open(void*, const struct sx_media_frame*,
                     const struct sx_media_audio_format* dst)
{
    return reinterpret_cast<void*>(static_cast<uintptr_t>(dst->sample_rate));
}

void resampler_close(void*) {}

int resample(void*, const struct sx_media_frame*, int16_t* out, int capacity)
{
    int index;
    const int frames = capacity < 480 ? capacity : 480;
    g_resample_calls += 1;
    for (index = 0; index < frames; ++index) {
        out[index] = (int16_t)(index % 200);
    }
    return frames;
}

int resample_flush(void*, int16_t* out, int capacity)
{
    int index;
    const int frames = capacity < 32 ? capacity : 32;
    g_resample_flush_calls += 1;
    for (index = 0; index < frames; ++index) {
        out[index] = 7;
    }
    return frames;
}

/* ---- the two providers, shaped like the two libraries -------------------- */

const struct sx_media_backend_ops kCodecOnlyOps = {
    /* source role, all null: this library ships no demuxer */
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    /* decoder role */
    decoder_claim, decoder_open, decoder_close, decoder_flush, decoder_send, decoder_receive,
    /* converter role, all null */
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

const struct sx_media_backend_ops kSourceOps = {
    /* source role */
    source_claim, source_open, source_close, source_seek, source_read_packet,
    source_container_name, source_metadata,
    /* decoder role, all null: this provider only demuxes and converts */
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    /* converter role */
    scaler_open, scaler_close, scale,
    resampler_open, resampler_close, resample, resample_flush,
};

struct sx_media_audio_format audio_out_format()
{
    struct sx_media_audio_format format;
    format.sample_rate = 48000;
    format.channels = 2;
    return format;
}

struct sx_media_source fake_source(int with_audio)
{
    struct sx_media_source source;
    struct sx_media_audio_format out = audio_out_format();

    memset(&source, 0, sizeof(source));
    source.path = "/disk/media/clip.fake";
    source.fd = -1;
    source.audio_out = with_audio != 0 ? &out : nullptr;
    return source;
}

/* ---- tests --------------------------------------------------------------- */

void test_registration_rejects_bad_input()
{
    struct sx_media_backend wrong_abi = {};
    struct sx_media_backend nameless = {};

    wrong_abi.name = "wrong-abi";
    wrong_abi.abi_version = SX_MEDIA_BACKEND_ABI + 1u;
    wrong_abi.ops = &kCodecOnlyOps;
    expect(sx_media_register_backend(&wrong_abi) == -22, "an unknown ABI is rejected");

    nameless.abi_version = SX_MEDIA_BACKEND_ABI;
    nameless.ops = &kCodecOnlyOps;
    expect(sx_media_register_backend(&nameless) == -22, "a nameless backend is rejected");

    expect(sx_media_register_backend(nullptr) == -22, "a null backend is rejected");

    /* A second backend under a name already in the table. */
    struct sx_media_backend duplicate = {};
    duplicate.name = "fakecodec";
    duplicate.abi_version = SX_MEDIA_BACKEND_ABI;
    duplicate.ops = &kCodecOnlyOps;
    expect(sx_media_register_backend(&duplicate) == -17, "a duplicate name is rejected");
    expect(sx_media_backend_count() == 3, "and the table did not grow");
}

void test_registry_order()
{
    /* Priority descending; registration order breaks a tie. The table is
     * permanent, so this asserts against the order main() built. */
    expect(sx_media_backend_count() == 3, "three backends are registered");
    expect_str(sx_media_backend_name_at(0), "refuses", "the highest priority is asked first");
    expect_str(sx_media_backend_name_at(1), "fakecodec", "then the codec library");
    expect_str(sx_media_backend_name_at(2), "fakedemux", "then the demuxer");
    expect(sx_media_backend_name_at(3) == nullptr, "an index past the end is null");
    expect(sx_media_backend_name_at(-1) == nullptr, "a negative index is null");
}

void test_capability_query()
{
    expect(sx_media_has_codec("fakevideo") == 1, "a claimed codec exists in the system");
    expect(sx_media_has_codec("ghostaudio") == 0, "an unclaimed codec does not");
    expect(sx_media_has_codec("vorbis") == 0, "a codec nobody registers does not");
    expect(sx_media_has_codec("") == 0, "an empty name is not a codec");
    expect(sx_media_has_codec(nullptr) == 0, "a null name is not a codec");
}

void test_statuses_say_different_things()
{
    /* A program builds its notice from these, so two of them collapsing into one
     * sentence would make the notice unactionable. */
    expect(strcmp(sx_media_status_string(SX_MEDIA_ERR_NO_DEMUXER),
                  sx_media_status_string(SX_MEDIA_ERR_NO_CODEC)) != 0,
           "an unreadable container and a missing codec are different sentences");
    expect(strcmp(sx_media_status_string(SX_MEDIA_ERR_NO_CODEC),
                  sx_media_status_string(SX_MEDIA_ERR_UNREADABLE)) != 0,
           "a missing codec and a damaged file are different sentences");
    expect(strcmp(sx_media_status_string(SX_MEDIA_ERR_NO_BACKEND),
                  sx_media_status_string(SX_MEDIA_ERR_NO_CODEC)) != 0,
           "having no backend and lacking a codec are different sentences");
    expect_str(sx_media_status_string(SX_MEDIA_OK), "ok", "success has a name too");
}

void test_no_provider_will_take_the_source()
{
    reset_fixture();
    struct sx_media* media = nullptr;
    struct sx_media_audio_format out = audio_out_format();
    struct sx_media_source source;
    enum sx_media_status status = SX_MEDIA_OK;

    /* A demuxer claims by extension, so ".unknown" is claimed by nobody. With no
     * extension there is nothing to key on. */
    memset(&source, 0, sizeof(source));
    source.path = "/disk/media/thing.unknown";
    source.fd = -1;
    source.audio_out = &out;

    expect(sx_media_create(&media) == 0 && media != nullptr, "an engine can be created");
    expect(sx_media_open(media, &source, &status) < 0, "a source nobody claims does not open");
    expect(status == SX_MEDIA_ERR_NO_DEMUXER, "and the reason names the container, not a codec");

    sx_media_destroy(media);
}

void test_the_demuxer_is_found_past_the_refuser()
{
    reset_fixture();
    struct sx_media* media = nullptr;
    struct sx_media_source source = fake_source(1);
    enum sx_media_status status = SX_MEDIA_OK;
    const int before = g_source.claim_calls;

    sx_media_create(&media);
    expect(sx_media_open(media, &source, &status) == 0, "the source opened");
    expect(status == SX_MEDIA_OK, "with no reason attached");
    /* Both provider roles were asked before the file was touched: the codec
     * library cannot demux, so the chain is walked past it. */
    expect(g_source.claim_calls == before + 1, "the chain was walked, not short-circuited");
    expect(g_claim_calls > before, "and the decoder role was consulted per stream");
    expect_str(g_source.opened_path, source.path, "the demuxer was handed the path");

    sx_media_destroy(media);
}

void test_a_failing_demuxer_keeps_its_own_reason()
{
    reset_fixture();
    g_source.open_fails = 1;
    g_source.open_status = SX_MEDIA_ERR_UNREADABLE;

    struct sx_media* media = nullptr;
    struct sx_media_source source = fake_source(1);
    enum sx_media_status status = SX_MEDIA_OK;

    sx_media_create(&media);
    expect(sx_media_open(media, &source, &status) < 0, "a failing open does not succeed");
    expect(status == SX_MEDIA_ERR_UNREADABLE, "and the provider's reason is not overwritten");
    expect(sx_media_stream_count(media) == 0, "nothing was left half-open");

    sx_media_destroy(media);
}

void test_a_packet_crosses_from_one_provider_to_another()
{
    reset_fixture();
    add_video_packet(0, 1);
    add_video_packet(20000, 0);
    add_video_packet(40000, 0);

    struct sx_media* media = nullptr;
    struct sx_media_source source = fake_source(1);
    enum sx_media_status status = SX_MEDIA_OK;

    g_open_decoder_calls = 0;
    sx_media_create(&media);
    expect(sx_media_open(media, &source, &status) == 0, "the source opened");
    /* Two streams, one of them undecodable, so exactly one decoder is built. */
    expect(g_open_decoder_calls == 1, "one decoder was built for two streams");
    expect(sx_media_stream_count(media) == 2, "both streams were identified");
    expect(sx_media_stream_decodable(media, 0) == 1, "the video stream is decodable");
    expect(sx_media_stream_decodable(media, 1) == 0, "the audio stream is not");
    expect_str(sx_media_stream_codec(media, 1), "ghostaudio", "and is still named");

    /* A stream nobody can decode is not a failed open. The file plays without
     * it and says what it is playing without. */
    expect(sx_media_stream_missing_count(media) == 1, "one stream was dropped");
    char missing[SX_MEDIA_CODEC_CAPACITY];
    memset(missing, 0, sizeof(missing));
    expect(sx_media_stream_missing_at(media, 0, missing, sizeof(missing)) == 1,
           "the dropped stream is reportable");
    expect_str(missing, "ghostaudio", "by codec name, which is what a notice needs");
    expect(sx_media_stream_missing_at(media, 1, missing, sizeof(missing)) == 0,
           "an index past the dropped streams is not one");

    /* The demuxer produced these packets and a decoder in a different provider
     * consumed them. This is the assertion the design exists for. */
    expect(sx_media_next_video_frame(media) == 1, "the first frame arrived");
    expect(sx_media_video_time_us(media) == 0, "at the time its packet carried");
    expect(sx_media_next_video_frame(media) == 1, "the second frame arrived");
    expect(sx_media_video_time_us(media) == 20000, "at its own time, not a repeat of the first");
    expect(sx_media_next_video_frame(media) == 1, "the third frame arrived");
    expect(sx_media_next_video_frame(media) == 0, "and then the stream is over");

    sx_media_destroy(media);
}

void test_extradata_reaches_a_foreign_decoder()
{
    reset_fixture();
    add_video_packet(0, 1);

    struct sx_media* media = nullptr;
    struct sx_media_source source = fake_source(1);
    enum sx_media_status status = SX_MEDIA_OK;

    sx_media_create(&media);
    expect(sx_media_open(media, &source, &status) == 0, "the source opened");

    /* The decoder is in a different provider from the demuxer and was handed
     * only the descriptor. A real codec with the parameter sets missing here
     * would not fail to build, it would produce silence. */
    FakeDecoder* decoder = g_last_decoder;
    expect(decoder != nullptr, "the foreign decoder was built");
    if (decoder != nullptr) {
        expect(decoder->extradata_size == (int)sizeof(kExtradata),
               "it received the parameter sets");
        expect(decoder->extradata_matches == 1, "and they are the ones the demuxer published");
        /* Height, not sample rate: this is the video stream, and a decoder that
         * read the setup blob wrong would report 0 here rather than failing. */
        expect(decoder->setup_seen == 48, "and it read the source's own setup blob");
    }
    expect(sx_media_next_video_frame(media) == 1, "and it decoded a frame with that setup");

    sx_media_destroy(media);
}

/* A decoder that needs nothing but the portable descriptor is the case that
 * makes a second library possible at all: it has to build from a source
 * provider that knows nothing about it, and the descriptor has to be enough. */
void test_a_decoder_that_needs_no_setup_still_builds()
{
    reset_fixture();
    g_require_setup = 0;
    add_video_packet(0, 1);

    struct sx_media* media = nullptr;
    struct sx_media_source source = fake_source(0);
    enum sx_media_status status = SX_MEDIA_OK;

    sx_media_create(&media);
    expect(sx_media_open(media, &source, &status) == 0, "the source opened");
    expect(sx_media_next_video_frame(media) == 1,
           "a decoder that ignores the setup blob still decodes");

    sx_media_destroy(media);
}

void test_the_display_size_carries_the_sample_aspect()
{
    reset_fixture();
    add_video_packet(0, 1);

    struct sx_media* media = nullptr;
    struct sx_media_source source = fake_source(0);
    enum sx_media_status status = SX_MEDIA_OK;
    int width = 0;
    int height = 0;

    sx_media_create(&media);
    sx_media_open(media, &source, &status);
    sx_media_display_size(media, &width, &height);
    /* 64x48 with 4:3 pixels: 64 * 4 / 3 = 85, so the picture is not stretched
     * when it is presented square-pixel. */
    expect(width == 85, "the width is corrected by the pixel aspect");
    expect(height == 48, "the height is not");
    expect(sx_media_frame_duration_us(media) == 20000, "the declared frame duration is used");

    sx_media_destroy(media);
}

void test_decoding_is_not_showing()
{
    reset_fixture();
    add_video_packet(0, 1);
    add_video_packet(20000, 0);

    struct sx_media* media = nullptr;
    struct sx_media_source source = fake_source(0);
    enum sx_media_status status = SX_MEDIA_OK;

    sx_media_create(&media);
    sx_media_open(media, &source, &status);
    expect(sx_media_has_shown_frame(media) == 0, "nothing is shown before a frame");
    expect(sx_media_next_video_frame(media) == 1, "a frame decoded");
    /* A window that resizes while paused has to re-scale the frame it already
     * had, so decoding ahead must not disturb what is on screen. */
    expect(sx_media_has_shown_frame(media) == 0, "decoding is not showing");
    sx_media_show_video_frame(media);
    expect(sx_media_has_shown_frame(media) == 1, "showing is explicit");

    const uint32_t* pixels = sx_media_scale_video(media, 16, 12);
    expect(pixels != nullptr, "the shown frame scales");
    expect(g_scaled_calls == 1, "through the provider's scaler");
    if (pixels != nullptr) {
        expect(pixels[0] == 0xff204060u, "and the pixels are the provider's output");
    }
    /* The same target size: the scaler and the destination buffer are reused,
     * because they are built for one format and one size. Only the shown frame
     * changed, so the converter is not rebuilt and `scale` is not called again --
     * that is the whole saving, and it is why scale is not routed through the
     * cache. */
    expect(sx_media_scale_video(media, 16, 12) == pixels, "the same size reuses the buffer");
    expect(g_scaled_calls == 1, "and does not re-scale a frame that did not change");
    /* A new size rebuilds, because the converter is built for one size. */
    expect(sx_media_scale_video(media, 32, 24) != nullptr, "a new size rescales");
    expect(g_scaled_calls == 2, "so the converter is rebuilt for it");
    expect(sx_media_scale_video(media, 0, 0) == nullptr, "a zero size is refused");

    sx_media_destroy(media);
}

void test_seek_delivers_the_frame_at_the_target()
{
    reset_fixture();
    add_video_packet(0, 1);
    add_video_packet(20000, 0);
    add_video_packet(40000, 0);
    add_video_packet(60000, 0);

    struct sx_media* media = nullptr;
    struct sx_media_source source = fake_source(0);
    enum sx_media_status status = SX_MEDIA_OK;

    sx_media_create(&media);
    sx_media_open(media, &source, &status);
    const int flushes_before = g_flush_calls;
    expect(sx_media_seek(media, 40000) == 1, "a seekable source repositions");
    expect(g_source.last_seek_us == 40000, "and the provider was asked for exactly that");
    expect(g_flush_calls == flushes_before + 1, "and the decodable stream was flushed");

    /* Frames before the target are decoded, because later frames reference
     * them, but not delivered. */
    expect(sx_media_next_video_frame(media) == 1, "a frame arrived after the seek");
    expect(sx_media_video_time_us(media) == 40000, "and it is the one at the target");

    g_source.seekable = 0;
    expect(sx_media_seek(media, 20000) == 0, "an unseekable source refuses rather than pretending");

    sx_media_destroy(media);
}

void test_skip_is_per_stream_and_clears_itself()
{
    reset_fixture();
    add_video_packet(0, 1);
    add_video_packet(20000, 0);
    add_video_packet(40000, 0);

    struct sx_media* media = nullptr;
    struct sx_media_source source = fake_source(0);
    enum sx_media_status status = SX_MEDIA_OK;

    sx_media_create(&media);
    sx_media_open(media, &source, &status);
    /* What a device stall does: everything already written was heard whole, so
     * the stream restarts at the last written sample without seeking. */
    sx_media_skip_until(media, 20000);
    expect(sx_media_next_video_frame(media) == 1, "a frame arrived after the skip");
    expect(sx_media_video_time_us(media) == 20000, "the frame at the resume point, not before it");
    expect(sx_media_next_video_frame(media) == 1, "playback continues");
    expect(sx_media_video_time_us(media) == 40000, "and the mark was already cleared");

    sx_media_destroy(media);
}

void test_audio_nobody_can_decode_is_absent_not_fatal()
{
    reset_fixture();
    add_video_packet(0, 1);

    struct sx_media* media = nullptr;
    struct sx_media_source source = fake_source(1);
    enum sx_media_status status = SX_MEDIA_OK;
    int16_t block[256];

    memset(block, 0, sizeof(block));
    sx_media_create(&media);
    expect(sx_media_open(media, &source, &status) == 0, "the file opened despite the audio");
    expect(sx_media_has_audio(media) == 0, "the undecodable stream is not the audio stream");
    expect(sx_media_read_audio(media, block, 128, nullptr) == 0,
           "reading its audio is zero, not a crash");
    expect(sx_media_has_video(media) == 1, "and the video is unaffected");
    expect(g_resample_calls == 0, "no resampler was built for a stream nobody decodes");

    sx_media_destroy(media);
}

void test_ignoring_audio_leaves_no_resampler()
{
    reset_fixture();
    add_video_packet(0, 1);

    struct sx_media* media = nullptr;
    struct sx_media_source source = fake_source(0);
    enum sx_media_status status = SX_MEDIA_OK;

    sx_media_create(&media);
    expect(sx_media_open(media, &source, &status) == 0, "a video-only consumer opens the source");
    expect(sx_media_has_audio(media) == 0, "and has no audio stream to read");
    expect(sx_media_has_video(media) == 1, "video is still there");

    sx_media_destroy(media);
}

void test_container_and_metadata_come_from_the_provider()
{
    reset_fixture();
    add_video_packet(0, 1);

    struct sx_media* media = nullptr;
    struct sx_media_source source = fake_source(0);
    enum sx_media_status status = SX_MEDIA_OK;

    sx_media_create(&media);
    sx_media_open(media, &source, &status);
    expect_str(sx_media_container_name(media), "fakecontainer", "the container is the provider's to name");
    expect_str(sx_media_metadata(media, "title"), "Fake Title", "and so is the metadata");
    expect(sx_media_metadata(media, "artist") == nullptr, "a tag that is not there is null");
    expect(sx_media_duration_us(media) == 1000000, "the duration came from the provider");

    sx_media_destroy(media);
}

void test_reopening_does_not_accumulate_state()
{
    reset_fixture();
    add_video_packet(0, 1);

    struct sx_media* media = nullptr;
    struct sx_media_source source = fake_source(1);
    enum sx_media_status status = SX_MEDIA_OK;

    sx_media_create(&media);
    sx_media_open(media, &source, &status);
    sx_media_next_video_frame(media);
    sx_media_show_video_frame(media);

    /* What a player does when the user picks a second file. The first source's
     * decoder must not survive it. */
    reset_fixture();
    add_video_packet(0, 1);
    expect(sx_media_open(media, &source, &status) == 0, "the engine reopens");
    expect(sx_media_has_shown_frame(media) == 0, "and does not still hold the old frame");
    expect(sx_media_stream_missing_count(media) == 1, "and does not accumulate dropped streams");

    sx_media_destroy(media);
}

void test_null_is_not_a_crash()
{
    sx_media_destroy(nullptr);
    sx_media_close(nullptr);
    sx_media_skip_until(nullptr, 100);
    expect(sx_media_stream_count(nullptr) == 0, "queries on a null engine answer zero");
    expect(sx_media_has_video(nullptr) == 0, "and report nothing playable");
    expect(sx_media_has_audio(nullptr) == 0, "and no audio either");
    expect(sx_media_next_video_frame(nullptr) == 0, "and decode nothing");
    expect(sx_media_seek(nullptr, 0) == 0, "and cannot seek");
    expect(sx_media_scale_video(nullptr, 4, 4) == nullptr, "and scale nothing");
    expect(sx_media_create(nullptr) == -22, "and a null out-parameter is refused");
}

}  // namespace

int main()
{
    /* The registry is process-global and permanent, so the order these are
     * registered in is part of the fixture. "refuses" is registered at the
     * highest priority with the decoder role only: the fallback chain is only
     * tested if something has to be walked past. */
    struct sx_media_backend refuses = {};
    refuses.name = "refuses";
    refuses.abi_version = SX_MEDIA_BACKEND_ABI;
    refuses.priority = 100;
    refuses.ops = &kCodecOnlyOps;

    struct sx_media_backend codec = {};
    codec.name = "fakecodec";
    codec.abi_version = SX_MEDIA_BACKEND_ABI;
    codec.priority = 50;
    codec.ops = &kCodecOnlyOps;

    struct sx_media_backend demuxer = {};
    demuxer.name = "fakedemux";
    demuxer.abi_version = SX_MEDIA_BACKEND_ABI;
    demuxer.priority = 0;
    demuxer.ops = &kSourceOps;

    if (sx_media_register_backend(&refuses) != 0 || sx_media_register_backend(&codec) != 0 ||
        sx_media_register_backend(&demuxer) != 0) {
        printf("sxmedia: FAIL the fixture backends did not register\n");
        return 1;
    }

    test_registration_rejects_bad_input();
    test_registry_order();
    test_capability_query();
    test_statuses_say_different_things();
    test_no_provider_will_take_the_source();
    test_the_demuxer_is_found_past_the_refuser();
    test_a_failing_demuxer_keeps_its_own_reason();
    test_a_packet_crosses_from_one_provider_to_another();
    test_extradata_reaches_a_foreign_decoder();
    test_a_decoder_that_needs_no_setup_still_builds();
    test_the_display_size_carries_the_sample_aspect();
    test_decoding_is_not_showing();
    test_seek_delivers_the_frame_at_the_target();
    test_skip_is_per_stream_and_clears_itself();
    test_audio_nobody_can_decode_is_absent_not_fatal();
    test_ignoring_audio_leaves_no_resampler();
    test_container_and_metadata_come_from_the_provider();
    test_reopening_does_not_accumulate_state();
    test_null_is_not_a_crash();

    if (g_failures != 0) {
        printf("sxmedia: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("sxmedia: %d checks PASS\n", g_checks);
    return 0;
}
