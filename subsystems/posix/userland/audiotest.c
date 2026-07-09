#include "libc.h"

#define AUDIOTEST_MAX_SAMPLES (16384 / (int)sizeof(int16_t))

static int16_t g_samples[AUDIOTEST_MAX_SAMPLES];

static int is_smoke_mode(int argc, char** argv) {
    return argc > 1 && strcmp(argv[1], "--smoke") == 0;
}

static int is_stream_mode(int argc, char** argv) {
    return argc > 1 && strcmp(argv[1], "--stream") == 0;
}

/* Reproduce el patron de alimentacion de Doom para exponer underruns: cada
 * iteracion escribe "lo que paso en tiempo real" (elapsed-worth) de un tono
 * cuadrado continuo y luego duerme un poco, imitando el trabajo de un frame.
 * La salida deberia ser un tono continuo; los underruns aparecen como silencios
 * periodicos en la captura WAV. */
static int run_stream(int fd, const struct savanxp_audio_info* info) {
    const uint32_t rate = info->sample_rate_hz;
    const uint32_t channels = info->channels;
    const uint32_t max_frames = (uint32_t)(sizeof(g_samples) / (channels * sizeof(int16_t)));
    uint32_t full_period = rate / 440u;
    unsigned long start_ms = uptime_ms();
    unsigned long last_ms = start_ms;
    uint32_t phase = 0;

    if (full_period == 0u) {
        full_period = 1u;
    }

    while (uptime_ms() - start_ms < 2500UL) {
        unsigned long now_ms;
        uint32_t delta_ms;
        uint32_t frames;
        uint32_t frame;
        long want;

        sleep_ms(15);
        now_ms = uptime_ms();
        delta_ms = (uint32_t)(now_ms - last_ms);
        last_ms = now_ms;
        if (delta_ms == 0u) {
            continue;
        }

        frames = delta_ms * rate / 1000u;
        if (frames > max_frames) {
            frames = max_frames;
        }

        for (frame = 0; frame < frames; ++frame) {
            const int16_t sample = (phase < (full_period / 2u)) ? 12000 : -12000;
            uint32_t channel;
            for (channel = 0; channel < channels; ++channel) {
                g_samples[frame * channels + channel] = sample;
            }
            if (++phase >= full_period) {
                phase = 0;
            }
        }

        want = (long)(frames * info->frame_bytes);
        if (write(fd, g_samples, (unsigned long)want) != want) {
            puts_fd(2, "audiotest: stream write failed\n");
            return 1;
        }
    }

    printf("AUDIO STREAM PASS\n");
    return 0;
}

static void fill_square_wave(const struct savanxp_audio_info* info, uint32_t frequency_hz) {
    const uint32_t frame_count = info->buffer_bytes / info->frame_bytes;
    uint32_t half_period = info->sample_rate_hz / (frequency_hz * 2u);
    if (half_period == 0) {
        half_period = 1;
    }

    for (uint32_t frame = 0; frame < frame_count; ++frame) {
        const int16_t sample = ((frame / half_period) & 1u) != 0 ? 12000 : -12000;
        const uint32_t sample_index = frame * info->channels;
        for (uint32_t channel = 0; channel < info->channels; ++channel) {
            g_samples[sample_index + channel] = sample;
        }
    }
}

int main(int argc, char** argv) {
    struct savanxp_audio_info info = {0};
    long fd = audio_open();
    long result;
    const int smoke_mode = is_smoke_mode(argc, argv);
    const int stream_mode = is_stream_mode(argc, argv);

    if (fd < 0) {
        puts_fd(2, "audiotest: /dev/audio0 not available\n");
        return 1;
    }

    if (audio_get_info((int)fd, &info) < 0) {
        puts_fd(2, "audiotest: AUDIO_IOC_GET_INFO failed\n");
        close((int)fd);
        return 1;
    }
    if (info.sample_rate_hz != 48000u ||
        info.channels != 2u ||
        info.bits_per_sample != 16u ||
        info.frame_bytes != 4u ||
        info.period_bytes == 0u ||
        info.buffer_bytes == 0u ||
        info.buffer_bytes > (uint32_t)sizeof(g_samples)) {
        puts_fd(2, "audiotest: unexpected audio format\n");
        close((int)fd);
        return 1;
    }

    if (stream_mode) {
        const int rc = run_stream((int)fd, &info);
        close((int)fd);
        return rc;
    }

    fill_square_wave(&info, smoke_mode ? 440u : 660u);
    result = write((int)fd, g_samples, info.buffer_bytes);
    if (result != (long)info.buffer_bytes) {
        eprintf("audiotest: write failed (%s)\n", result_error_string(result));
        close((int)fd);
        return 1;
    }

    if (!smoke_mode) {
        printf("audiotest: wrote %u bytes to /dev/audio0 (%u Hz, %u channels)\n",
            info.buffer_bytes,
            info.sample_rate_hz,
            info.channels);
    }

    close((int)fd);
    return 0;
}
