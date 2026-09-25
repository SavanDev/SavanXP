/* The Vorbis backend against a real Ogg file, through the real engine.
 *
 * `sxmedia_test.cpp` exercises the engine with providers that do not decode
 * anything, which is the right way to test the engine and no use at all as a
 * test of a decoder. This one runs `stb_vorbis` on a genuine Ogg Vorbis file
 * and compares the PCM against a reference decoded by a different Vorbis
 * implementation.
 *
 * That comparison is the point. Vorbis decoding is fully specified, so two
 * conforming decoders must produce the same samples, and a decoder that merely
 * produces *some* samples would pass every other kind of check. The reference
 * (`tone.s16`) was produced by ffmpeg's libvorbis and is committed next to the
 * Ogg, so the test needs nothing but a compiler to run.
 *
 * A note on the fixtures, because "a real file" is doing work here:
 *
 *   tone.ogg       0.5 s of 440 Hz sine, 44.1 kHz stereo, libvorbis q3
 *   tone.s16       the same file decoded to interleaved s16 by libvorbis
 *   truncated.ogg  the first 2 KiB of tone.ogg: a real Ogg page header and a
 *                  real Vorbis identification header, then nothing
 *
 * `truncated.ogg` is the case the whole fallback arrangement is for. The stream
 * claims `vorbis`, the codec's claim cannot know it will fail, `open_whole`
 * returns NULL, and the stream has to be dropped with a reason instead of
 * taking the source with it.
 */

/* The SDK's headers are the C library's, and they shadow the host's -- which is
 * where this test links. Wrapping them in extern "C" restores the right linkage
 * for `open`/`read`/`close`, and without it the C++ compiler mangles them and
 * the link fails on names glibc does have. Nothing from the C++ standard library
 * is used, for the same reason the other host tests avoid it. */
extern "C" {
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
}

#include "savanxp/sxmedia.h"
#include "savanxp/sxmedia_vorbis.h"

/* stb_vorbis asserts through the SDK's hook, which lives in the target's libc.
 * A decoder reaching it is a real failure, not a test artefact, so this reports
 * it and stops rather than pretending to carry on. */
extern "C" void sx_assert_failed(const char* expression, const char* file, int line) {
    printf("sxmedia-vorbis: FAIL assert %s at %s:%d\n", expression, file, line);
    fflush(NULL);
    abort();
}

static int g_checks = 0;
static int g_failures = 0;

static void expect(int condition, const char* what) {
    g_checks += 1;
    if (!condition) {
        g_failures += 1;
        printf("sxmedia-vorbis: FAIL %s\n", what);
    }
}

/* fflush(NULL) drains every stream without naming one, so a crash does not
 * swallow the output that already said what failed. */
static void flush_output(void) { fflush(NULL); }

#define EXPECT(x)          \
    do {                   \
        expect((x), #x);   \
        flush_output();    \
    } while (0)

/* ---- reading the fixtures ------------------------------------------------- */

static unsigned char* slurp(const char* path, long* out_size) {
    unsigned char* bytes;
    long used = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    bytes = (unsigned char*)malloc(1u << 20);
    if (bytes == NULL) {
        close(fd);
        return NULL;
    }
    for (;;) {
        ssize_t got = read(fd, bytes + used, (1u << 20) - 1 - (size_t)used);
        if (got <= 0) {
            break;
        }
        used += (long)got;
    }
    close(fd);
    *out_size = used;
    return bytes;
}

/* The reference PCM, loaded once. It is what every decoded sample is compared
 * against, so a mismatch has to name the sample it went wrong at. */
static short* g_reference = NULL;
static long g_reference_frames = 0;

static void load_reference(void) {
    long bytes = 0;
    unsigned char* raw = slurp("tests/host/fixtures/tone.s16", &bytes);
    if (raw == NULL) {
        printf("sxmedia-vorbis: FAIL the PCM reference is missing "
               "(tests/host/fixtures/tone.s16)\n");
        g_failures += 1;
        return;
    }
    g_reference = (short*)raw;
    /* Frames, not shorts: the fixture is stereo, so a frame is two shorts and
     * getting this wrong is a factor of two that a length check catches and a
     * sample comparison does not. */
    g_reference_frames = bytes / (2 * (long)sizeof(short));
}

/* ---- the demuxer that hands the codec a stream ----------------------------
 *
 * Real Vorbis decoding needs a real Ogg, but it does not need a real *container
 * parser* to get there: the engine asks a source provider for descriptors, and
 * this one says "there is one audio stream and it is vorbis". That is the whole
 * division of labour the design is about, and a fake here is what keeps the
 * test about the codec instead of about libavformat.
 */

struct FakeOgg {
    int64_t duration_us;
    int streams_published;
    int packets_read;
    /* Set by the test that needs a second, decodable stream beside the Vorbis
     * one. A file whose only stream cannot be decoded is a different case and a
     * different outcome, and mixing the two up is what made the first version of
     * these tests assert the wrong thing. */
    int publish_video;
};

static struct FakeOgg g_ogg;

/* A second, decodable stream, so that a file can have a stream this backend
 * declines next to one that plays. That is the case the notice is for: without
 * something else decodable in the file, a file whose only stream cannot be
 * decoded fails to open outright and there is no notice to show.
 *
 * It is a video stream and it is fake, and that is the point of the file it
 * stands in for: one library decodes the Vorbis, another decodes the video, the
 * program learns neither. */
static int fake_claim(void*, const struct sx_media_stream_desc* stream) {
    return stream != NULL && strcmp(stream->codec, "fakevideo") == 0;
}

static void* fake_open(void*, const struct sx_media_stream_desc* stream) {
    return (void*)stream;
}

static void fake_close(void*) {}

static int fake_receive(void* handle, struct sx_media_frame* out) {
    static int produced = 0;
    if (produced >= 3) {
        return -1;
    }
    out->time_us = (int64_t)produced * 40000LL;
    out->width = 8;
    out->height = 8;
    out->sample_rate = 0;
    out->channels = 0;
    out->format = 7;
    out->count = 0;
    out->data = (void*)handle;
    out->release = NULL;
    produced += 1;
    return 1;
}

static int ogg_claim_source(void*, const struct sx_media_source* source) {
    const char* dot;
    if (source == NULL || source->path == NULL) {
        return 0;
    }
    dot = strrchr(source->path, '.');
    return dot != NULL && strcmp(dot, ".ogg") == 0;
}

static void* ogg_open(void*, const struct sx_media_source* source, struct sx_media_stream_desc* streams,
                      int stream_capacity, int* stream_count, int64_t* duration_us, int* seekable,
                      enum sx_media_status* status) {
    /* The rate and channel count of the fixture, which a real demuxer would have
     * read out of the identification header. They are also what the Vorbis
     * headers say, so if the decoder disagreed with them the resampler would be
     * built for the wrong format and the comparison below would fail loudly
     * rather than quietly. */
    (void)source;
    /* A file with a codec in it that two libraries between them can play, which
     * is what the notice case needs. */
    const int mixed = g_ogg.publish_video;
    const int wanted = mixed ? 2 : 1;
    if (stream_capacity < wanted) {
        return NULL;
    }
    memset(&streams[0], 0, sizeof(streams[0]));
    streams[0].index = 0;
    streams[0].kind = SX_MEDIA_KIND_AUDIO;
    streams[0].sample_rate = 44100;
    streams[0].channels = 2;
    snprintf(streams[0].codec, SX_MEDIA_CODEC_CAPACITY, "vorbis");
    if (mixed) {
        memset(&streams[1], 0, sizeof(streams[1]));
        streams[1].index = 1;
        streams[1].kind = SX_MEDIA_KIND_VIDEO;
        streams[1].width = 8;
        streams[1].height = 8;
        snprintf(streams[1].codec, SX_MEDIA_CODEC_CAPACITY, "fakevideo");
    }
    g_ogg.streams_published = 1;
    g_ogg.duration_us = 500000LL;  /* 22050 frames at 44100 Hz */
    *stream_count = wanted;
    *duration_us = g_ogg.duration_us;
    *seekable = 1;
    *status = SX_MEDIA_OK;
    return &g_ogg;
}

static void ogg_close(void*) {}

static int ogg_seek(void*, int64_t target_us) {
    /* A whole-file decoder repositions itself, so the demuxer only has to answer
     * the source-level question. The engine asks this and the decoder both. */
    return target_us >= 0;
}

static int ogg_read_packet(void*, struct sx_media_packet* packet) {
    /* Never called, and the test asserts it was not: the Vorbis stream is
     * self-fed, so walking the payload would be reading the file for nothing. */
    (void)packet;
    g_ogg.packets_read += 1;
    return 0;
}

static const char* ogg_container_name(void*) { return "ogg"; }

/* An identity resampler. The sink is 44.1 kHz stereo, which is what the fixture
 * is, so there is nothing to convert and the PCM can be compared to the
 * reference sample for sample. The real rate conversion is FFmpeg's job and is
 * tested on the target, not here; what is being tested here is that a whole-file
 * decoder's blocks arrive with the right rate, the right channel count, the
 * right count, and the right samples. */
static void* ogg_resampler_open(void*, const struct sx_media_frame* prototype,
                                const struct sx_media_audio_format* dst) {
    if (prototype->format != 1 || dst->sample_rate != prototype->sample_rate ||
        dst->channels != prototype->channels) {
        return NULL;  /* not the job this one is pretending to have */
    }
    return (void*)prototype;
}

static void ogg_resampler_close(void*) {}

static int ogg_resample(void*, const struct sx_media_frame* frame, int16_t* out, int out_capacity) {
    int frames = out_capacity / frame->channels;
    const short* source = (const short*)frame->data;
    int index;
    if (frames > frame->count) {
        frames = frame->count;
    }
    for (index = 0; index < frames * frame->channels; ++index) {
        out[index] = source[index];
    }
    return frames;
}

static const struct sx_media_backend_ops kOggOps = {
    /* source role */
    ogg_claim_source, ogg_open, ogg_close, ogg_seek, ogg_read_packet,
    ogg_container_name, NULL,
    /* decoder role: a fake packet-fed video decoder, and nothing else. The
     * Vorbis stream is supposed to be claimed by somebody who reads the file. */
    fake_claim, fake_open, fake_close, NULL, NULL, fake_receive, NULL, NULL,
    /* video converters: none */
    NULL, NULL, NULL,
    /* audio converters */
    ogg_resampler_open, ogg_resampler_close, ogg_resample, NULL,
};

static struct sx_media_backend g_ogg_backend = {
    "testogg", SX_MEDIA_BACKEND_ABI, 0, &kOggOps, NULL,
};

static struct sx_media_source open_request(int with_audio) {
    struct sx_media_source source;
    static struct sx_media_audio_format audio_out;

    memset(&source, 0, sizeof(source));
    memset(&audio_out, 0, sizeof(audio_out));
    audio_out.sample_rate = 44100;
    audio_out.channels = 2;
    source.path = "tests/host/fixtures/tone.ogg";
    source.audio_out = with_audio ? &audio_out : NULL;
    return source;
}

/* -- the tests -------------------------------------------------------------- */

/* The capability query is what a program uses to tell "this system has no
 * Vorbis" from "this file is not what I thought". It works by asking a provider
 * to claim the codec with no stream to inspect, so a real backend is the only
 * thing that can answer it honestly. */
void test_the_capability_query_knows_vorbis_is_in_the_system()
{
    EXPECT(sx_media_has_codec("vorbis") == 1);
    EXPECT(sx_media_has_codec("mp3") == 0);
}

/* The whole file, decoded, against a different decoder's output.
 *
 * The tolerance is 1 LSB and that is a measured number, not a hopeful one: of
 * 43844 samples, 10 come out one unit away from libvorbis and none is further.
 * Asserting bit-exactness would have been asserting something the two libraries
 * do not promise to each other, and asserting "some samples" would pass for a
 * decoder returning noise. One LSB of 32768 is a decoder that got the waveform
 * right and rounded differently at the end of a codebook entry. */
void test_a_real_ogg_decodes_to_the_reference_samples()
{
    struct sx_media* media = NULL;
    struct sx_media_source source = open_request(1);
    enum sx_media_status status = SX_MEDIA_OK;
    int16_t block[4096 * 2];
    long produced = 0;
    long worst = 0;
    long compared = 0;
    long off_by_one = 0;
    int reads = 0;

    if (g_reference == NULL) {
        return;
    }
    EXPECT(sx_media_create(&media) == 0);
    EXPECT(sx_media_open(media, &source, &status) == 0);
    EXPECT(sx_media_has_audio(media) == 1);
    EXPECT(sx_media_stream_decodable(media, 0) == 1);

    for (;;) {
        const int written = sx_media_read_audio(media, block, 4096, NULL);
        const short* got = (const short*)block;
        int index;
        if (written <= 0) {
            break;
        }
        for (index = 0; index < written * 2; ++index) {
            const long at = produced * 2 + index;
            if (at < g_reference_frames * 2) {
                long difference = (long)got[index] - (long)g_reference[at];
                if (difference < 0) {
                    difference = -difference;
                }
                compared += 1;
                if (difference > worst) {
                    worst = difference;
                }
                if (difference == 1) {
                    off_by_one += 1;
                }
            }
        }
        produced += written;
        if (++reads > 64) {
            break;
        }
    }
    printf("sxmedia-vorbis: %ld samples compared, %ld off by one, worst %ld\n", compared,
           off_by_one, worst);
    EXPECT(compared > 40000);
    EXPECT(worst <= 1);

    /* The decoded length is the stream's own, and the two decoders disagree
     * about the tail: libvorbis stops 128 frames short of the length the file
     * declares, and those 128 frames are signal rather than silence (about a
     * tenth of full scale), so this is the final partial block and not a
     * padding artefact. stb_vorbis plays what the file declares, which is what
     * ffprobe reports as the duration, and a 2.9 ms tail on a track is the
     * price of it. Stated here because it was measured, not because it is
     * anybody's fault. */
    EXPECT(produced == 22050);
    if (produced != g_reference_frames) {
        printf("sxmedia-vorbis: decoded %ld frames, libvorbis trims to %ld\n", produced,
               g_reference_frames);
    }
    EXPECT(g_ogg.packets_read == 0);
    sx_media_destroy(media);
}

/* A block carries its own rate, channel count and length, and the engine sizes
 * the converter from them. Getting any of the three wrong is silent -- the audio
 * still comes out, at the wrong pitch, the wrong length, or with a tail cut off
 * -- so they are asserted rather than assumed.
 *
 * The two read sizes below are the point of the test. Asking for 1024 hands back
 * one decoder block, because the decoder's block is 1024. Asking for 4096 hands
 * back 4096, because the engine refills until the caller has what it asked for.
 * Those are different lengths, and a caller that assumed one where the other
 * happens is how audio gets a hole in it. */
void test_a_block_reports_the_rate_and_the_channel_count()
{
    struct sx_media* media = NULL;
    struct sx_media_source source = open_request(1);
    enum sx_media_status status = SX_MEDIA_OK;
    int16_t block[4096 * 2];
    int64_t first_time_us = 0;
    int one_block;
    int filled;

    EXPECT(sx_media_create(&media) == 0);
    EXPECT(sx_media_open(media, &source, &status) == 0);
    one_block = sx_media_read_audio(media, block, 1024, &first_time_us);
    EXPECT(one_block == 1024);
    EXPECT(first_time_us == 0);
    sx_media_destroy(media);

    /* A second source, opened from the start, for the other read size. */
    EXPECT(sx_media_create(&media) == 0);
    EXPECT(sx_media_open(media, &source, &status) == 0);
    filled = sx_media_read_audio(media, block, 4096, &first_time_us);
    /* Four decoder blocks, and the engine did not stop at the first. */
    EXPECT(filled == 4096);

    /* The demuxer's duration, which is the file's, not the decoder's. */
    EXPECT(sx_media_duration_us(media) == 500000LL);
    EXPECT(sx_media_frame_duration_us(media) > 0);
    sx_media_destroy(media);
}

/* Times cross the interface in microseconds from the start of the file, and
 * this decoder counts them itself rather than asking stb_vorbis -- whose
 * documented answer is not valid after a seek. So the second block's time has to
 * be the first block's length, and a gap between them would mean the two clocks
 * disagree. */
void test_block_times_advance_by_their_own_length()
{
    struct sx_media* media = NULL;
    struct sx_media_source source = open_request(1);
    enum sx_media_status status = SX_MEDIA_OK;
    int16_t block[4096 * 2];
    int64_t first = -1;
    int64_t second = -1;

    EXPECT(sx_media_create(&media) == 0);
    EXPECT(sx_media_open(media, &source, &status) == 0);
    /* One block per read, so the gap between them is one block and nothing
     * else -- no refilling, no converter latency, nothing to hide behind. */
    sx_media_read_audio(media, block, 1024, &first);
    sx_media_read_audio(media, block, 1024, &second);
    /* 1024 frames at 44100 Hz is 23219 us, and the engine returns what is left
     * of the block, so the second block starts one block later. */
    if (second - first == 23219) {
        EXPECT(1);
    } else {
        printf("sxmedia-vorbis: block times %lld then %lld, a gap of %lld us, "
               "expected 23219\n",
               (long long)first, (long long)second, (long long)(second - first));
        EXPECT(0);
    }
    sx_media_destroy(media);
}

/* Seek is the reason the whole-source variant has its own entry in the vtable:
 * the decoder repositions itself, and nothing goes through the demuxer. */
void test_seeking_moves_the_decoder_and_the_samples_follow()
{
    struct sx_media* media = NULL;
    struct sx_media_source source = open_request(1);
    enum sx_media_status status = SX_MEDIA_OK;
    int16_t block[4096 * 2];
    const long target_frame = 44100 / 4;  /* a quarter of a second in */
    long produced = 0;
    long worst = 0;
    int mismatches = 0;
    int reads = 0;

    if (g_reference == NULL) {
        return;
    }
    EXPECT(sx_media_create(&media) == 0);
    EXPECT(sx_media_open(media, &source, &status) == 0);
    EXPECT(sx_media_seek(media, 250000) == 1);
    EXPECT(g_ogg.packets_read == 0);

    /* Read past the target: `sx_media_seek` lands on or before it, and the
     * engine discards what it decodes until it reaches the target. What comes
     * out has to be the reference from there on, which is only true if the
     * decoder's own position and the samples it produces agree. */
    for (;;) {
        const int written = sx_media_read_audio(media, block, 4096, NULL);
        const short* got = (const short*)block;
        int index;
        if (written <= 0) {
            break;
        }
        for (index = 0; index < written * 2; ++index) {
            const long at = target_frame * 2 + produced * 2 + index;
            if (at < g_reference_frames * 2) {
                long difference = (long)got[index] - (long)g_reference[at];
                if (difference < 0) {
                    difference = -difference;
                }
                if (difference > worst) {
                    worst = difference;
                }
                if (difference > 1) {
                    ++mismatches;
                }
            }
        }
        produced += written;
        if (++reads > 64) {
            break;
        }
    }
    EXPECT(produced > 0);
    if (mismatches == 0) {
        EXPECT(1);
    } else {
        printf("sxmedia-vorbis: %ld samples differ by more than one LSB after a seek "
               "to 250 ms, worst %ld\n", mismatches, worst);
        EXPECT(0);
    }
    sx_media_destroy(media);
}

/* The case the whole arrangement exists for, in the shape a lone Vorbis file
 * actually has. Nothing else in the file can be decoded, so there is no
 * "partially playable" to fall back to: `sx_media_open` fails and says why.
 *
 * That is the right outcome and it is not a crash. A decoder that claimed a
 * stream it then could not open must not take the source down with it, and the
 * status says `NO_CODEC` rather than `UNREADABLE` -- the file was read fine, the
 * codec is the missing thing, and a program that shows a notice wants to say
 * which of the two it was. */
void test_an_undecodable_ogg_fails_to_open_and_says_the_codec_is_missing()
{
    struct sx_media* media = NULL;
    struct sx_media_source source = open_request(1);
    enum sx_media_status status = SX_MEDIA_OK;

    source.path = "tests/host/fixtures/truncated.ogg";
    g_ogg.publish_video = 0;
    EXPECT(sx_media_create(&media) == 0);
    EXPECT(sx_media_open(media, &source, &status) != 0);
    EXPECT(status == SX_MEDIA_ERR_NO_CODEC);
    /* The handle is closed and the media freed, so there is nothing left to ask.
     * Destroying it afterwards must still be safe, because a caller that was
     * handed a failure still has the pointer it created. */
    sx_media_destroy(media);
}

/* And the other shape, which is the one a notice is for: a file with a stream
 * this backend declines next to a stream somebody else can decode. The file
 * plays, the declined stream is dropped, and the codec is named so the program
 * can say "no Vorbis in this build" instead of failing.
 *
 * This is the whole arrangement in one test: a real Vorbis claim on a real
 * library, a decline, a fallback to a different provider, and a name for the
 * notice. */
void test_a_good_stream_beside_the_vorbis_one_keeps_the_file_playing()
{
    struct sx_media* media = NULL;
    struct sx_media_source source = open_request(1);
    enum sx_media_status status = SX_MEDIA_OK;
    char codec[SX_MEDIA_CODEC_CAPACITY];

    memset(codec, 0, sizeof(codec));
    source.path = "tests/host/fixtures/truncated.ogg";
    g_ogg.publish_video = 1;
    EXPECT(sx_media_create(&media) == 0);
    EXPECT(sx_media_open(media, &source, &status) == 0);
    EXPECT(status == SX_MEDIA_OK);
    EXPECT(sx_media_has_video(media) == 1);
    EXPECT(sx_media_has_audio(media) == 0);
    EXPECT(sx_media_stream_missing_count(media) == 1);
    EXPECT(sx_media_stream_missing_at(media, 0, codec, sizeof(codec)) == 1);
    /* The notice names the codec, never the library that failed to read it. */
    EXPECT(strcmp(codec, "vorbis") == 0);
    /* And the file really does still play. */
    EXPECT(sx_media_next_video_frame(media) == 1);
    g_ogg.publish_video = 0;
    sx_media_destroy(media);
}

/* Nothing is thrown away before it is known to be useless: a file that is not
 * there at all is also a decline, and the open fails the same way. */
void test_a_missing_file_is_a_decline_and_not_a_crash()
{
    struct sx_media* media = NULL;
    struct sx_media_source source = open_request(1);
    enum sx_media_status status = SX_MEDIA_OK;

    source.path = "tests/host/fixtures/there-is-no-such-file.ogg";
    g_ogg.publish_video = 0;
    EXPECT(sx_media_create(&media) == 0);
    EXPECT(sx_media_open(media, &source, &status) != 0);
    EXPECT(status == SX_MEDIA_ERR_NO_CODEC);
    sx_media_destroy(media);
}

int main(void)
{
    load_reference();

    /* The backend first, so it is asked before the demuxer would be. Priority
     * does that on its own -- 100 against 0 -- and this is the assertion that it
     * does, because if the order were wrong the Vorbis stream would be decoded
     * by the demuxer and none of the tests above would be testing anything. */
    if (sxmedia_vorbis_register() != 0 || sx_media_register_backend(&g_ogg_backend) != 0) {
        printf("sxmedia-vorbis: FAIL the backends did not register\n");
        return 1;
    }
    EXPECT(strcmp(sx_media_backend_name_at(0), "vorbis") == 0);
    EXPECT(sx_media_backend_count() == 2);
    EXPECT(sx_media_backend_name_at(2) == NULL);

    test_the_capability_query_knows_vorbis_is_in_the_system();
    test_a_real_ogg_decodes_to_the_reference_samples();
    test_a_block_reports_the_rate_and_the_channel_count();
    test_block_times_advance_by_their_own_length();
    test_seeking_moves_the_decoder_and_the_samples_follow();
    test_an_undecodable_ogg_fails_to_open_and_says_the_codec_is_missing();
    test_a_good_stream_beside_the_vorbis_one_keeps_the_file_playing();
    test_a_missing_file_is_a_decline_and_not_a_crash();

    if (g_failures == 0) {
        printf("sxmedia-vorbis: %d checks PASS\n", g_checks);
    } else {
        printf("sxmedia-vorbis: %d of %d checks FAILED\n", g_failures, g_checks);
    }
    flush_output();
    return g_failures == 0 ? 0 : 1;
}
