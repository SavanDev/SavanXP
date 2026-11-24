/*
 * fat-image -- builds FAT images on the host with FatFs, the same module the
 * kernel's FAT driver runs on (vendor/fatfs, configured by
 * kernel/fatfs/ffconf.h; docs/fs/FAT_ROADMAP.md).
 *
 *   fat-image create IMAGE MIB          a new image of MIB MiB, formatted as
 *                                       FAT (FatFs picks FAT12, 16 or 32 by
 *                                       size), no partition table
 *   fat-image create-fat32 IMAGE MIB    a FAT32 image for an EFI partition
 *   fat-image put IMAGE HOST-FILE PATH  copies HOST-FILE to PATH in the image,
 *                                       making the directories on the way
 *
 * The smokes use it for a FAT disk the guest mounts (diskmgmt-smoke).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ff.h"
#include "diskio.h"

static FILE *g_image;
static uint64_t g_sectors;

/* --- FatFs's disk layer, over one image file (drive 0) ----------------------- */

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv == 0 && g_image != NULL ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buffer, LBA_t sector, UINT count)
{
    if (pdrv != 0 || g_image == NULL || (uint64_t)sector + count > g_sectors)
    {
        return RES_PARERR;
    }
    if (fseek(g_image, (long)sector * FF_MAX_SS, SEEK_SET) != 0 ||
        fread(buffer, FF_MAX_SS, count, g_image) != count)
    {
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buffer, LBA_t sector, UINT count)
{
    if (pdrv != 0 || g_image == NULL || (uint64_t)sector + count > g_sectors)
    {
        return RES_PARERR;
    }
    if (fseek(g_image, (long)sector * FF_MAX_SS, SEEK_SET) != 0 ||
        fwrite(buffer, FF_MAX_SS, count, g_image) != count)
    {
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE command, void *buffer)
{
    if (pdrv != 0 || g_image == NULL)
    {
        return RES_NOTRDY;
    }
    switch (command)
    {
    case CTRL_SYNC:
        return fflush(g_image) == 0 ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buffer = (LBA_t)g_sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buffer = FF_MAX_SS;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buffer = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

DWORD get_fattime(void)
{
    const time_t now = time(NULL);
    const struct tm *local = localtime(&now);

    if (local == NULL || local->tm_year < 80)
    {
        return (DWORD)((1u << 21) | (1u << 16));
    }
    return (DWORD)(local->tm_year - 80) << 25 | (DWORD)(local->tm_mon + 1) << 21 | (DWORD)local->tm_mday << 16 |
        (DWORD)local->tm_hour << 11 | (DWORD)local->tm_min << 5 | (DWORD)(local->tm_sec / 2);
}

void *ff_memalloc(UINT size)
{
    return malloc(size);
}

void ff_memfree(void *block)
{
    free(block);
}

/* --- The commands ------------------------------------------------------------ */

static int fail(const char *what, int result)
{
    fprintf(stderr, "fat-image: %s (FatFs %d)\n", what, result);
    return 1;
}

static int open_image(const char *path, const char *mode)
{
    long size;

    g_image = fopen(path, mode);
    if (g_image == NULL)
    {
        fprintf(stderr, "fat-image: %s: %s\n", path, strerror(errno));
        return 0;
    }
    if (fseek(g_image, 0, SEEK_END) != 0 || (size = ftell(g_image)) < 0)
    {
        fprintf(stderr, "fat-image: %s: cannot size it\n", path);
        return 0;
    }
    g_sectors = (uint64_t)size / FF_MAX_SS;
    return 1;
}

static int create(const char *path, const char *mib_text, int fat32)
{
    static BYTE work[FF_MAX_SS * 8];
    const MKFS_PARM options = { (fat32 ? FM_FAT32 : FM_ANY) | FM_SFD, 0, 0, 0,
                               fat32 ? FF_MAX_SS : 0 };
    char *end = NULL;
    const unsigned long mib = strtoul(mib_text, &end, 10);
    FRESULT result;

    if (end == mib_text || *end != '\0' || mib == 0 || mib > 4095)
    {
        fprintf(stderr, "fat-image: size must be 1..4095 MiB\n");
        return 1;
    }
    g_image = fopen(path, "w+b");
    if (g_image == NULL || fseek(g_image, (long)(mib * 1024 * 1024 - 1), SEEK_SET) != 0 || fputc(0, g_image) == EOF)
    {
        fprintf(stderr, "fat-image: %s: %s\n", path, strerror(errno));
        return 1;
    }
    g_sectors = (uint64_t)mib * 1024 * 1024 / FF_MAX_SS;
    result = f_mkfs("0:", &options, work, sizeof(work));
    if (result != FR_OK)
    {
        return fail("f_mkfs failed", result);
    }
    return fclose(g_image) == 0 ? 0 : 1;
}

/* Makes every directory above `path` ("0:/a/b/file"). */
static int make_parents(char *path)
{
    char *slash;

    for (slash = strchr(path + 3, '/'); slash != NULL; slash = strchr(slash + 1, '/'))
    {
        FRESULT result;

        *slash = '\0';
        result = f_mkdir(path);
        *slash = '/';
        if (result != FR_OK && result != FR_EXIST)
        {
            return fail("f_mkdir failed", result);
        }
    }
    return 0;
}

static int put(const char *image, const char *host_file, const char *volume_path)
{
    FATFS fs;
    FIL file;
    FILE *source;
    char path[300];
    char buffer[8192];
    size_t got;
    FRESULT result;

    if (volume_path[0] != '/' || strlen(volume_path) + 3 >= sizeof(path))
    {
        fprintf(stderr, "fat-image: the path in the image must be absolute\n");
        return 1;
    }
    if (!open_image(image, "r+b"))
    {
        return 1;
    }
    result = f_mount(&fs, "0:", 1);
    if (result != FR_OK)
    {
        return fail("not a FAT image", result);
    }
    snprintf(path, sizeof(path), "0:%s", volume_path);
    if (make_parents(path) != 0)
    {
        return 1;
    }
    source = fopen(host_file, "rb");
    if (source == NULL)
    {
        fprintf(stderr, "fat-image: %s: %s\n", host_file, strerror(errno));
        return 1;
    }
    result = f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (result != FR_OK)
    {
        fclose(source);
        return fail("f_open failed", result);
    }
    while ((got = fread(buffer, 1, sizeof(buffer), source)) > 0)
    {
        UINT done = 0;

        result = f_write(&file, buffer, (UINT)got, &done);
        if (result != FR_OK || done != got)
        {
            fclose(source);
            return fail("the image is full", result);
        }
    }
    fclose(source);
    result = f_close(&file);
    if (result != FR_OK)
    {
        return fail("f_close failed", result);
    }
    (void)f_mount(NULL, "0:", 0);
    return fclose(g_image) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "create") == 0)
    {
        return create(argv[2], argv[3], 0);
    }
    if (argc == 4 && strcmp(argv[1], "create-fat32") == 0)
    {
        return create(argv[2], argv[3], 1);
    }
    if (argc == 5 && strcmp(argv[1], "put") == 0)
    {
        return put(argv[2], argv[3], argv[4]);
    }
    fprintf(stderr,
            "usage: fat-image create IMAGE MIB\n"
            "       fat-image create-fat32 IMAGE MIB\n"
            "       fat-image put IMAGE HOST-FILE PATH\n");
    return 2;
}
