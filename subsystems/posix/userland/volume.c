#include "libc.h"
#include "savanxp/audio_server.h"

#define VOLUME_CONFIG_PATH "/disk/audio.cfg"
#define VOLUME_LIST_CAPACITY 16u

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

/* Volumen por app: censo y niveles del demonio, no del device. Esto no se
 * persiste: los streams mueren con sus programas y el nivel con ellos. */
static int volume_query_apps(
    struct sx_audio_server_entry* entries,
    size_t capacity,
    long* count_out)
{
    long sock;
    long count;

    sock = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP);
    if (sock < 0) {
        eprintf("volume: sin socket (%s)\n", result_error_string(sock));
        return 1;
    }
    count = sx_audio_server_list((int)sock, entries, capacity);
    savanxp_close((int)sock);
    if (count < 0) {
        eprintf("volume: sin demonio (%s)\n", result_error_string(count));
        return 1;
    }
    *count_out = count;
    return 0;
}

static int volume_list_apps(void) {
    struct sx_audio_server_entry entries[VOLUME_LIST_CAPACITY];
    long count = 0;
    long index;

    if (volume_query_apps(entries, VOLUME_LIST_CAPACITY, &count) != 0) {
        return 1;
    }
    if (count == 0) {
        printf("volume: no hay apps sonando\n");
        return 0;
    }
    for (index = 0; index < count && (size_t)index < VOLUME_LIST_CAPACITY; ++index) {
        printf("%u %s %u\n",
               (unsigned int)entries[index].port,
               entries[index].name[0] != '\0' ? entries[index].name : "(anonimo)",
               (unsigned int)entries[index].volume);
    }
    if (count > (long)VOLUME_LIST_CAPACITY) {
        printf("volume: ... y %u mas\n", (unsigned int)(count - (long)VOLUME_LIST_CAPACITY));
    }
    return 0;
}

static int volume_set_app(const char* name, const char* level_text) {
    struct sx_audio_server_entry entries[VOLUME_LIST_CAPACITY];
    unsigned int level = 0;
    long count = 0;
    long matches = 0;
    long index;
    long sock;
    long status;

    if (name == 0 || name[0] == '\0' || !parse_uint(level_text, &level) || level > 100u) {
        puts_fd(2, "usage: volume set <app> <0-100>\n");
        return 1;
    }
    if (volume_query_apps(entries, VOLUME_LIST_CAPACITY, &count) != 0) {
        return 1;
    }
    for (index = 0; index < count && (size_t)index < VOLUME_LIST_CAPACITY; ++index) {
        if (strcmp(entries[index].name, name) == 0) {
            ++matches;
        }
    }
    if (matches != 1) {
        eprintf("volume: '%s' coincide con %d apps (tiene que ser 1)\n", name, matches);
        return 1;
    }
    sock = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP);
    if (sock < 0) {
        eprintf("volume: sin socket (%s)\n", result_error_string(sock));
        return 1;
    }
    status = 0;
    for (index = 0; index < count && (size_t)index < VOLUME_LIST_CAPACITY; ++index) {
        if (strcmp(entries[index].name, name) == 0) {
            status = sx_audio_server_set_volume((int)sock, entries[index].port, (int)level);
        }
    }
    savanxp_close((int)sock);
    if (status < 0) {
        eprintf("volume: no se pudo fijar (%s)\n", result_error_string(status));
        return 1;
    }
    printf("volume: %s a %u\n", name, level);
    return 0;
}

int main(int argc, char** argv) {
    unsigned int volume = 0;
    unsigned int muted = 0;
    long fd;
    long status;

    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        return volume_set_app(argv[2], argv[3]);
    }
    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        return volume_list_apps();
    }
    if (argc != 1 && argc != 2) {
        puts_fd(2, "usage: volume [0-100 | mute | unmute | list | set <app> <0-100>]\n");
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
        puts_fd(2, "usage: volume [0-100 | mute | unmute | list | set <app> <0-100>]\n");
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
