#include "kernel/fat_diskio.hpp"

#include "kernel/block.hpp"
#include "kernel/heap.hpp"
#include "kernel/rtc.hpp"

#include "ff.h"
#include "diskio.h"

static_assert(FF_VOLUMES == fat_disk::kDrives, "one FatFs drive per FatFs volume");
static_assert(FF_MIN_SS == block::kSectorSize && FF_MAX_SS == block::kSectorSize,
              "FatFs and block:: agree on the sector size");

namespace {

struct Drive {
    bool bound;
    size_t device_index;
};

Drive g_drives[fat_disk::kDrives] = {};

const Drive* drive_for(BYTE pdrv) {
    return pdrv < fat_disk::kDrives && g_drives[pdrv].bound ? &g_drives[pdrv] : nullptr;
}

DSTATUS status_of(BYTE pdrv) {
    const Drive* drive = drive_for(pdrv);
    block::DeviceInfo info = {};
    if (drive == nullptr || !block::device_info(drive->device_index, info) || !info.present) {
        return STA_NOINIT;
    }
    return info.writable ? 0 : STA_PROTECT;
}

// Howard Hinnant's days_from_civil and civil_from_days, for the years FAT can
// hold (1980..2107), so everything stays non-negative.
uint32_t days_from_civil(uint32_t year, uint32_t month, uint32_t day) {
    year -= month <= 2 ? 1 : 0;
    const uint32_t era = year / 400;
    const uint32_t year_of_era = year - era * 400;
    const uint32_t day_of_year = (153u * (month > 2 ? month - 3 : month + 9) + 2u) / 5u + day - 1u;
    const uint32_t day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
    return era * 146097u + day_of_era - 719468u;
}

void civil_from_days(uint32_t days, uint32_t& year, uint32_t& month, uint32_t& day) {
    const uint32_t shifted = days + 719468u;
    const uint32_t era = shifted / 146097u;
    const uint32_t day_of_era = shifted - era * 146097u;
    const uint32_t year_of_era = (day_of_era - day_of_era / 1460u + day_of_era / 36524u - day_of_era / 146096u) / 365u;
    const uint32_t day_of_year = day_of_era - (365u * year_of_era + year_of_era / 4u - year_of_era / 100u);
    const uint32_t month_index = (5u * day_of_year + 2u) / 153u;
    day = day_of_year - (153u * month_index + 2u) / 5u + 1u;
    month = month_index < 10 ? month_index + 3 : month_index - 9;
    year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
}

constexpr uint32_t kFirstYear = 1980;
constexpr uint32_t kLastYear = 2107;

} // namespace

namespace fat_disk {

bool bind(uint8_t drive, size_t device_index) {
    if (drive >= kDrives || g_drives[drive].bound) {
        return false;
    }
    g_drives[drive] = {true, device_index};
    return true;
}

void unbind(uint8_t drive) {
    if (drive < kDrives) {
        g_drives[drive] = {};
    }
}

bool bound(uint8_t drive) {
    return drive < kDrives && g_drives[drive].bound;
}

uint32_t fat_time(uint64_t epoch_seconds) {
    const uint64_t first = static_cast<uint64_t>(days_from_civil(kFirstYear, 1, 1)) * 86400u;
    const uint64_t last = static_cast<uint64_t>(days_from_civil(kLastYear, 12, 31)) * 86400u + 86399u;
    if (epoch_seconds < first) {
        epoch_seconds = first;
    } else if (epoch_seconds > last) {
        epoch_seconds = last;
    }
    const uint32_t days = static_cast<uint32_t>(epoch_seconds / 86400u);
    const uint32_t seconds = static_cast<uint32_t>(epoch_seconds % 86400u);
    uint32_t year = 0;
    uint32_t month = 0;
    uint32_t day = 0;
    civil_from_days(days, year, month, day);
    const uint32_t date = ((year - kFirstYear) << 9) | (month << 5) | day;
    const uint32_t time = ((seconds / 3600u) << 11) | (((seconds / 60u) % 60u) << 5) | ((seconds % 60u) / 2u);
    return (date << 16) | time;
}

uint64_t epoch_seconds(uint16_t date, uint16_t time) {
    const uint32_t year = kFirstYear + (date >> 9);
    uint32_t month = (date >> 5) & 0x0f;
    uint32_t day = date & 0x1f;
    // A zero date is "never set": FAT has no other way to say it.
    if (date == 0) {
        return 0;
    }
    month = month < 1 ? 1 : (month > 12 ? 12 : month);
    day = day < 1 ? 1 : day;
    const uint32_t hours = (time >> 11) & 0x1f;
    const uint32_t minutes = (time >> 5) & 0x3f;
    const uint32_t seconds = (time & 0x1f) * 2u;
    return static_cast<uint64_t>(days_from_civil(year, month, day)) * 86400u +
        hours * 3600u + minutes * 60u + seconds;
}

} // namespace fat_disk

/* --- What FatFs calls ------------------------------------------------------ */

extern "C" {

DSTATUS disk_status(BYTE pdrv) {
    return status_of(pdrv);
}

DSTATUS disk_initialize(BYTE pdrv) {
    // block:: brought the device up at boot; there is nothing to start here.
    return status_of(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE* buffer, LBA_t sector, UINT count) {
    const Drive* drive = drive_for(pdrv);
    if (drive == nullptr) {
        return RES_NOTRDY;
    }
    return block::read(drive->device_index, sector, count, buffer) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buffer, LBA_t sector, UINT count) {
    const Drive* drive = drive_for(pdrv);
    if (drive == nullptr) {
        return RES_NOTRDY;
    }
    if ((status_of(pdrv) & STA_PROTECT) != 0) {
        return RES_WRPRT;
    }
    // Synchronous: a FAT volume is small and rarely written, and FatFs orders
    // its own writes; the write-back queue would only add a window of loss.
    return block::write(drive->device_index, sector, count, buffer) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE command, void* buffer) {
    const Drive* drive = drive_for(pdrv);
    if (drive == nullptr) {
        return RES_NOTRDY;
    }
    block::DeviceInfo info = {};
    if (!block::device_info(drive->device_index, info) || !info.present) {
        return RES_NOTRDY;
    }
    switch (command) {
    case CTRL_SYNC:
        return block::sync(drive->device_index) ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        *static_cast<LBA_t*>(buffer) = info.sector_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *static_cast<WORD*>(buffer) = static_cast<WORD>(block::kSectorSize);
        return RES_OK;
    case GET_BLOCK_SIZE:
        // Unknown erase block: f_mkfs then aligns to nothing in particular.
        *static_cast<DWORD*>(buffer) = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

DWORD get_fattime(void) {
    return fat_disk::fat_time(rtc::epoch_seconds());
}

void* ff_memalloc(UINT size) {
    return heap::allocate(size);
}

void ff_memfree(void* block) {
    heap::free(block);
}

} // extern "C"
