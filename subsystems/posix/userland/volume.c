#include "libc.h"

#define VOLUME_CONFIG_PATH "/disk/audio.cfg"

static int parse_uint(const char* text, unsigned int* value) {
    unsigned int result = 0;
    size_t index = 0;
    if (text == 0 || text[0] == '\0') {
        return 0;
    }

    while (text[index] != '\0') {
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
        result = result * 10u + (unsigned int)(text[index] - '0');
        ++index;
    }
    *value = result;
    return 1;
}

/* Guarda el par que el kernel ya tiene: la fuente de verdad es el ioctl, el
 * fichero solo recuerda para el proximo arranque (lo lee init). Si el disco
 * no deja escribir, avisa pero no falla: el nivel quedo aplicado. */
static void persist_volume(unsigned int volume, unsigned int muted) {
    char text[16];
    size_t length = 0;
    unsigned int values[2];
    size_t value_index;
    size_t digit_index;
    char digits[4];
    long config_fd;

    values[0] = volume;
    values[1] = muted;
    for (value_index = 0; value_index < 2; ++value_index) {
        unsigned int remainder = values[value_index];
        digit_index = 0;
        do {
            digits[digit_index++] = (char)('0' + remainder % 10u);
            remainder /= 10u;
        } while (remainder != 0 && digit_index < sizeof(digits));
        while (digit_index != 0) {
            if (length >= sizeof(text) - 2) {
                return;
            }
            text[length++] = digits[--digit_index];
        }
        text[length++] = value_index == 0 ? ' ' : '\n';
    }

    config_fd = savanxp_open_mode(
        VOLUME_CONFIG_PATH,
        SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (config_fd < 0) {
        eprintf("volume: no se pudo guardar %s (%s)\n",
                VOLUME_CONFIG_PATH, result_error_string(config_fd));
        return;
    }
    if (savanxp_write((int)config_fd, text, length) != (long)length) {
        eprintf("volume: no se pudo guardar %s\n", VOLUME_CONFIG_PATH);
    }
    savanxp_close((int)config_fd);
    /* La preferencia tiene que sobrevivir a un apagado inmediato: sin esto,
     * el huesped del smoke muere segundos despues y el fichero no llega. */
    (void)savanxp_sync();
}

int main(int argc, char** argv) {
    unsigned int volume = 0;
    unsigned int muted = 0;
    long fd;
    long status;

    if (argc != 1 && argc != 2) {
        puts_fd(2, "usage: volume [0-100 | mute | unmute]\n");
        return 1;
    }

    fd = (long)audio_open();
    if (fd < 0) {
        eprintf("volume: /dev/audio0 unavailable (%s)\n", result_error_string(fd));
        return 1;
    }

    status = audio_get_volume((int)fd);
    if (status < 0) {
        eprintf("volume: AUDIO_IOC_GET_VOLUME failed (%s)\n", result_error_string(status));
        savanxp_close((int)fd);
        return 1;
    }
    volume = (unsigned int)status;
    status = audio_get_muted((int)fd);
    if (status < 0) {
        eprintf("volume: AUDIO_IOC_GET_MUTED failed (%s)\n", result_error_string(status));
        savanxp_close((int)fd);
        return 1;
    }
    muted = (unsigned int)status;

    if (argc == 1) {
        printf("volume: %u\nmuted: %s\n", volume, muted != 0 ? "yes" : "no");
        savanxp_close((int)fd);
        return 0;
    }

    if (strcmp(argv[1], "mute") == 0) {
        muted = 1;
    } else if (strcmp(argv[1], "unmute") == 0) {
        muted = 0;
    } else if (parse_uint(argv[1], &volume) && volume <= 100u) {
        /* volume ya trae el numero; muted conserva el que habia. */
    } else {
        puts_fd(2, "usage: volume [0-100 | mute | unmute]\n");
        savanxp_close((int)fd);
        return 1;
    }

    status = audio_set_volume((int)fd, (int)volume);
    if (status < 0) {
        eprintf("volume: AUDIO_IOC_SET_VOLUME failed (%s)\n", result_error_string(status));
        savanxp_close((int)fd);
        return 1;
    }
    status = audio_set_muted((int)fd, (int)muted);
    if (status < 0) {
        eprintf("volume: AUDIO_IOC_SET_MUTED failed (%s)\n", result_error_string(status));
        savanxp_close((int)fd);
        return 1;
    }
    savanxp_close((int)fd);

    persist_volume(volume, muted);
    return 0;
}
