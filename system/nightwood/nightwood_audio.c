/* The sound of Nightwood: every voice synthesized, nothing read from a file
 * (a SUN program has no files). A thread of its own keeps the stream about
 * 80 ms ahead of the speaker through sxn/sxnaudio.h -- audiod's native port
 * when the daemon runs, the device directly when it does not.
 *
 * The game thread only describes the moment (nw_audio_update): how windy, how
 * fast the steps, how close the Warden. This thread turns that into samples.
 * The description is a handful of aligned words, written by one thread and
 * read by the other; a value one block late is inaudible, so there is no lock.
 */

#include "nightwood.h"

#include <math.h>
#include <string.h>

#include "sxn/sxn.h"
#include "sxn/sxnaudio.h"

/* Ahead of the speaker by this much. audiod renders every 10 ms and its
 * queue holds 100 ms, so less than a few of its periods underruns; more than
 * the queue is dropped. */
#define NW_AUDIO_LEAD_MS 80u
#define NW_AUDIO_TICK_MS 8
#define NW_AUDIO_STACK_BYTES (64u * 1024u)
#define NW_SINE_SIZE 1024
#define NW_TWO_PI (2.0f * NW_PI)

struct nw_audio {
    struct sxnaudio_stream stream;
    long wake;
    long thread;
    volatile int running;
    volatile struct nw_audio_mix mix;
    float rate;

    /* Synthesis state, the sound thread's alone. */
    uint32_t noise;
    float sine[NW_SINE_SIZE];
    float wind_low;
    float wind_band;
    float gust_phase;
    float drone_phase[2];
    float step_phase;
    float step_envelope;
    float step_filter;
    float heart_phase;
    float heart_envelope;
    float heart_tone;
    int pickups_heard;
    float pickup_time; /* seconds since the last chime, or < 0 */
    float chime_phase[2];
    int caught_heard;
    float caught_time; /* seconds since the end, or < 0 */
    float fall_phase;

    int16_t pending[SAVANXP_AUDIOD_RECORD_MAX_FRAMES * 2u];
    uint32_t pending_frames;
};

static struct nw_audio g_audio;

static float nw_white(void)
{
    uint32_t x = g_audio.noise;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_audio.noise = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* A sine from the table, phase in turns [0, 1). */
static float nw_sine(float phase)
{
    int index = (int)(phase * (float)NW_SINE_SIZE) & (NW_SINE_SIZE - 1);
    return g_audio.sine[index];
}

static float nw_advance(float* phase, float frequency)
{
    *phase += frequency / g_audio.rate;
    if (*phase >= 1.0f) {
        *phase -= floorf(*phase);
    }
    return *phase;
}

static void nw_synthesize(int16_t* out, uint32_t frames)
{
    struct nw_audio_mix mix;
    float step_decay = expf(-1.0f / (0.07f * g_audio.rate));
    float heart_decay = expf(-1.0f / (0.11f * g_audio.rate));
    uint32_t frame;

    mix.wind = g_audio.mix.wind;
    mix.steps = g_audio.mix.steps;
    mix.heartbeat = g_audio.mix.heartbeat;
    mix.static_level = g_audio.mix.static_level;
    mix.pickup = g_audio.mix.pickup;
    mix.caught = g_audio.mix.caught;

    if (mix.pickup != g_audio.pickups_heard) {
        g_audio.pickups_heard = mix.pickup;
        g_audio.pickup_time = 0.0f;
    }
    if (mix.caught != g_audio.caught_heard) {
        g_audio.caught_heard = mix.caught;
        g_audio.caught_time = 0.0f;
        g_audio.fall_phase = 0.0f;
    }

    for (frame = 0; frame < frames; ++frame) {
        float white = nw_white();
        float left = 0.0f;
        float right = 0.0f;
        float gust;
        float wind;
        float drone;
        float sample;

        /* Wind: noise through two one-pole filters, the cut-off breathing
         * with slow gusts. */
        gust = 0.5f + 0.5f * nw_sine(nw_advance(&g_audio.gust_phase, 0.07f));
        g_audio.wind_low += (white - g_audio.wind_low) * (0.010f + 0.030f * gust);
        g_audio.wind_band += (g_audio.wind_low - g_audio.wind_band) * 0.06f;
        wind = (g_audio.wind_low - g_audio.wind_band) * 5.0f * mix.wind * (0.4f + 0.6f * gust);
        left += wind * (0.8f + 0.2f * gust);
        right += wind * (1.0f - 0.2f * gust);

        /* A low drone, two detuned sines that beat; it swells as the
         * Warden gets close. */
        drone = (nw_sine(nw_advance(&g_audio.drone_phase[0], 55.0f)) +
                    nw_sine(nw_advance(&g_audio.drone_phase[1], 55.8f))) *
            (0.02f + 0.08f * mix.heartbeat);
        left += drone;
        right += drone;

        /* Footsteps: a burst of filtered noise per step, needles crunching. */
        if (mix.steps > 0.0f) {
            g_audio.step_phase += mix.steps / g_audio.rate;
            if (g_audio.step_phase >= 1.0f) {
                g_audio.step_phase -= 1.0f;
                g_audio.step_envelope = 1.0f;
            }
        }
        g_audio.step_filter += (white - g_audio.step_filter) * 0.35f;
        sample = g_audio.step_filter * g_audio.step_envelope * 0.45f;
        g_audio.step_envelope *= step_decay;
        left += sample;
        right += sample;

        /* The heartbeat: lub-dub, faster and louder as it closes in. */
        if (mix.heartbeat > 0.05f) {
            float rate = (55.0f + 85.0f * mix.heartbeat) / 60.0f;
            float before = g_audio.heart_phase;
            g_audio.heart_phase += rate / g_audio.rate;
            if (g_audio.heart_phase >= 1.0f) {
                g_audio.heart_phase -= 1.0f;
                g_audio.heart_envelope = 1.0f;
            } else if (before < 0.2f && g_audio.heart_phase >= 0.2f) {
                g_audio.heart_envelope = 0.75f;
            }
        }
        sample = nw_sine(nw_advance(&g_audio.heart_tone, 46.0f)) * g_audio.heart_envelope * mix.heartbeat * 0.9f;
        g_audio.heart_envelope *= heart_decay;
        left += sample;
        right += sample;

        /* The static the screen shows, heard. */
        if (mix.static_level > 0.0f) {
            float crackle = nw_white() > 0.995f ? 0.6f : 0.0f;
            sample = (white * 0.5f + crackle) * mix.static_level * mix.static_level;
            left += sample;
            right += sample;
        }

        /* A photograph: the shutter's click, then a cold two-note chime. */
        if (g_audio.pickup_time >= 0.0f) {
            float t = g_audio.pickup_time;
            sample = 0.0f;
            if (t < 0.025f) {
                sample += white * 0.7f * (1.0f - t / 0.025f);
            }
            if (t > 0.05f && t < 1.2f) {
                float fade = expf(-(t - 0.05f) * 3.0f);
                sample += (nw_sine(nw_advance(&g_audio.chime_phase[0], 659.0f)) +
                              0.6f * nw_sine(nw_advance(&g_audio.chime_phase[1], 987.0f))) *
                    0.18f * fade;
            }
            left += sample;
            right += sample;
            g_audio.pickup_time += 1.0f / g_audio.rate;
            if (g_audio.pickup_time > 1.2f) {
                g_audio.pickup_time = -1.0f;
            }
        }

        /* The end: a roar of static over a tone that falls away. */
        if (g_audio.caught_time >= 0.0f) {
            float t = g_audio.caught_time;
            float fade = 1.0f - t / 2.5f;
            float pitch = 420.0f * expf(-t * 1.4f) + 40.0f;
            sample = (white * 0.8f + 0.5f * nw_sine(nw_advance(&g_audio.fall_phase, pitch))) * fade;
            left += sample;
            right += sample;
            g_audio.caught_time += 1.0f / g_audio.rate;
            if (g_audio.caught_time > 2.5f) {
                g_audio.caught_time = -1.0f;
            }
        }

        /* A soft knee instead of a hard clip. */
        left = left / (1.0f + fabsf(left) * 0.6f);
        right = right / (1.0f + fabsf(right) * 0.6f);
        out[frame * 2u] = (int16_t)(left * 26000.0f);
        out[frame * 2u + 1u] = (int16_t)(right * 26000.0f);
    }
}

static void nw_audio_thread(void* argument)
{
    (void)argument;
    while (g_audio.running) {
        uint32_t due = sxnaudio_frames_due(&g_audio.stream, NW_AUDIO_LEAD_MS);

        while (due > 0 && g_audio.running) {
            long taken;
            if (g_audio.pending_frames == 0) {
                uint32_t count = due < SAVANXP_AUDIOD_RECORD_MAX_FRAMES ? due : SAVANXP_AUDIOD_RECORD_MAX_FRAMES;
                nw_synthesize(g_audio.pending, count);
                g_audio.pending_frames = count;
            }
            taken = sxnaudio_write(&g_audio.stream, g_audio.pending, g_audio.pending_frames);
            if (taken < 0) {
                /* The device went to someone else: be silent, not broken. */
                g_audio.running = 0;
                break;
            }
            if ((uint32_t)taken < g_audio.pending_frames) {
                /* The daemon is behind: keep the rest for the next tick. */
                memmove(g_audio.pending, g_audio.pending + (uint32_t)taken * 2u,
                    (g_audio.pending_frames - (uint32_t)taken) * 2u * sizeof(int16_t));
                g_audio.pending_frames -= (uint32_t)taken;
                break;
            }
            g_audio.pending_frames = 0;
            due = due > (uint32_t)taken ? due - (uint32_t)taken : 0;
        }
        (void)sxn_wait_one(g_audio.wake, NW_AUDIO_TICK_MS);
    }
    sxn_thread_exit(0);
}

long nw_audio_start(void)
{
    long result;
    long section;
    long base;
    int index;

    memset(&g_audio, 0, sizeof(g_audio));
    g_audio.thread = -1;
    g_audio.wake = -1;
    g_audio.noise = 0x2545F491u;
    g_audio.pickup_time = -1.0f;
    g_audio.caught_time = -1.0f;
    for (index = 0; index < NW_SINE_SIZE; ++index) {
        g_audio.sine[index] = sinf((float)index * NW_TWO_PI / (float)NW_SINE_SIZE);
    }

    result = sxnaudio_open(&g_audio.stream, "Nightwood");
    if (result < 0) {
        return result;
    }
    g_audio.rate = (float)g_audio.stream.rate_hz;
    g_audio.wake = sxn_event_create(0);
    /* The stack is a section of its own, as SUN threads expect: the caller
     * owns it, and its top is what the thread starts on. */
    section = sxn_section_create(NW_AUDIO_STACK_BYTES, SXN_SECTION_READ | SXN_SECTION_WRITE);
    base = section >= 0 ? sxn_map_view(section, SXN_SECTION_READ | SXN_SECTION_WRITE) : section;
    if (section >= 0) {
        (void)sxn_close(section);
    }
    if (g_audio.wake < 0 || base <= 0) {
        sxnaudio_close(&g_audio.stream);
        return g_audio.wake < 0 ? g_audio.wake : (base < 0 ? base : -SAVANXP_ENOMEM);
    }
    g_audio.running = 1;
    g_audio.thread = sxn_thread_create(nw_audio_thread, 0, (void*)(base + (long)NW_AUDIO_STACK_BYTES), 0);
    if (g_audio.thread < 0) {
        g_audio.running = 0;
        sxnaudio_close(&g_audio.stream);
        return g_audio.thread;
    }
    return g_audio.stream.mode;
}

void nw_audio_update(const struct nw_audio_mix* mix)
{
    g_audio.mix.wind = mix->wind;
    g_audio.mix.steps = mix->steps;
    g_audio.mix.heartbeat = mix->heartbeat;
    g_audio.mix.static_level = mix->static_level;
    g_audio.mix.pickup = mix->pickup;
    g_audio.mix.caught = mix->caught;
}

void nw_audio_stop(void)
{
    if (g_audio.thread < 0) {
        return;
    }
    g_audio.running = 0;
    (void)sxn_event_set(g_audio.wake);
    (void)sxn_wait_one(g_audio.thread, 500);
    (void)sxn_close(g_audio.thread);
    g_audio.thread = -1;
    sxnaudio_close(&g_audio.stream);
}
