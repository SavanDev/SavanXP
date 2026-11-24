/* Write the partition table for a SavanXP installation image. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <unistd.h>

#include "sxdisk/sxdisk.h"

static uint64_t sectors(const char *text) {
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    return (*text < '0' || *text > '9' || *end || errno || value > UINT32_MAX) ? 0 : value;
}

static int read_sectors(void *cookie, uint64_t lba, uint32_t count, void *buffer) {
    FILE *file = cookie;
    return fseeko(file, (off_t)lba * SXDISK_SECTOR_SIZE, SEEK_SET) != 0 ||
           fread(buffer, SXDISK_SECTOR_SIZE, count, file) != count ? -1 : 0;
}

static int write_sectors(void *cookie, uint64_t lba, uint32_t count, const void *buffer) {
    FILE *file = cookie;
    return fseeko(file, (off_t)lba * SXDISK_SECTOR_SIZE, SEEK_SET) != 0 ||
           fwrite(buffer, SXDISK_SECTOR_SIZE, count, file) != count ? -1 : 0;
}

static void random_bytes(void *cookie, void *out, size_t size) {
    (void)cookie;
    if (getrandom(out, size, 0) != (ssize_t)size) {
        perror("gpt-image: getrandom");
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: gpt-image FILE DISK_SECTORS ESP_SECTORS ROOT_SECTORS\n");
        return 2;
    }
    uint64_t disk = sectors(argv[2]);
    uint64_t esp = sectors(argv[3]);
    uint64_t root = sectors(argv[4]);
    if (!disk || !esp || !root || esp % SXDISK_ALIGN_SECTORS != 0) {
        fprintf(stderr, "gpt-image: invalid disk or partition sizes\n");
        return 2;
    }
    FILE *file = fopen(argv[1], "w+bx");
    if (file == NULL) {
        perror("gpt-image: create");
        return 1;
    }
    struct sxdisk_io io = {file, read_sectors, write_sectors, random_bytes};
    struct sxdisk_gpt table;
    uint32_t root_number;
    if (ftruncate(fileno(file), (off_t)disk * SXDISK_SECTOR_SIZE) != 0 ||
        sxdisk_gpt_init(&table, disk, &io) != SXDISK_OK ||
        sxdisk_gpt_add(&table, SXDISK_ALIGN_SECTORS, esp, sxdisk_type_esp, "SavanXP EFI", &io, NULL) != SXDISK_OK ||
        sxdisk_gpt_add(&table, SXDISK_ALIGN_SECTORS + esp, root, sxdisk_type_sxfs, "SavanXP", &io, &root_number) != SXDISK_OK ||
        sxdisk_gpt_write(&table, &io) != SXDISK_OK || fclose(file) != 0) {
        fprintf(stderr, "gpt-image: could not write a table for these partitions\n");
        return 1;
    }
    char root_guid[37];
    sxdisk_guid_format(sxdisk_gpt_entry_at(&table, root_number)->unique_guid, root_guid);
    puts(root_guid);
    return 0;
}
