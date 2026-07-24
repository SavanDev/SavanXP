#include "libc.h"
#include "progman_registry.h"

/*
 * Program Manager (A2.3, ver docs/WM_SUBSYSTEM.md).
 *
 * En este paso (A2.3b) progman es solo el duenio del registro de programas y su
 * harness de verificacion: --selftest ejercita el parser, los defaults, el mapeo
 * de iconos/flags, el truncado y los limites de capacidad. La ventana real
 * (grupo con grid de iconos, doble click -> gfx_desktop_launch_ex) llega en
 * A2.3c; recien ahi progman se conecta como cliente del WM.
 */

static int progman_selftest(void)
{
    int failures = progman_registry_selftest();

    if (failures != 0)
    {
        printf("PROGMAN SMOKE FAIL %d checks\n", failures);
        return 1;
    }

    progman_registry_load_defaults();
    printf("PROGMAN SMOKE PASS groups=%d items=%d\n",
        progman_group_count(),
        progman_item_count());
    return 0;
}

static void progman_dump_registry(void)
{
    int group_index;

    progman_registry_load();
    printf("progman: registro desde %s\n",
        progman_registry_source() == PROGMAN_REGISTRY_SOURCE_FILE ? PROGMAN_REGISTRY_PATH : "defaults horneados");

    for (group_index = 0; group_index < progman_group_count(); ++group_index)
    {
        const struct progman_group *group = progman_group_at(group_index);
        int item_index;

        if (group == 0)
        {
            continue;
        }
        printf("[%s]\n", group->name);
        for (item_index = 0; item_index < group->item_count; ++item_index)
        {
            const struct progman_item *item = progman_group_item_at(group_index, item_index);
            if (item == 0)
            {
                continue;
            }
            printf("  %s -> %s%s\n",
                item->name,
                item->path,
                (item->launch_flags & SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN) != 0 ? " [fullscreen]" : "");
        }
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        return progman_selftest();
    }

    /* Sin ventana todavia (A2.3c): listar el registro deja el binario util para
     * inspeccionar que catalogo esta viendo el sistema. */
    progman_dump_registry();
    return 0;
}
