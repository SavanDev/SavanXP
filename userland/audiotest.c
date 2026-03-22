#include "libc.h"

#define AUDIOTEST_MAX_SAMPLES (16384 / (int)sizeof(int16_t))

static int16_t g_samples[AUDIOTEST_MAX_SAMPLES];

static int is_smoke_mode(int argc, char** argv) {
    return argc > 1 && strcmp(argv[1], "--smoke") == 0;
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

    (void)argv;

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
