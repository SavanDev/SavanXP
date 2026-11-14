# Time zones and the RTC mode

Status: **planned, no phase landed.** Today everything about the date is UTC:
the RTC holds UTC, `localtime` is `gmtime`, the taskbar shows UTC, and FAT
stamps files in UTC ([TIME.md](../../kernel/TIME.md#the-other-clock-the-calendar-date)).

This page is the plan for local time: what the system stores, who reads it,
how the hardware clock is interpreted, and the order of the work.

## The short version

- **A time zone is a POSIX `TZ` string** (`<-03>3`, `CET-1CEST,M3.5.0,M10.5.0/3`).
  The early phases accept only fixed offsets, but the ABI and the file format
  already carry the string, so daylight saving time (DST) needs no ABI change later.
- **The kernel owns the setting.** POSIX, SUN and Win32 all have to agree
  on the offset, and the kernel itself needs it (RTC in local mode, FAT).
- **The RTC mode is a switch: `utc` (default) or `local`.** `utc` is what
  Linux does and what Windows does with `RealTimeIsUniversal=1`. `local` is
  what a stock Windows does, for machines that dual-boot with it.
- **Everything above `kernel/rtc.cpp` stays UTC**: `struct savanxp_realtime`,
  `rtc::epoch_seconds()`, SxFS `mtime`, `sxn_system_time()`, `ntpsync`.
  Local time is a presentation matter, plus two hardware formats that
  insist on it: the RTC in `local` mode and FAT.
- **No IANA database.** Named zones with history (`America/Argentina/Buenos_Aires`)
  are out of scope (see [Rejected](#rejected-the-iana-database)).

## Decisions

### The setting is a `TZ` string, parsed by one shared module

A time zone is stored and passed as a POSIX `TZ` string: offsets are written
west-positive (`<-03>3` is UTC−3) and DST rules are written `Mm.w.d/time`. One freestanding C module
(working name `tzrule`) parses a string and answers "the offset at UTC
instant *t*" and "the UTC instant for local fields, with the DST hint". The
same source is compiled into the kernel, the POSIX libc, the SUN runtime
and the Win32 host, and tested on the host under CTest. Without that shared
module the subsystems would each parse the string their own way and end up
disagreeing.

Userland gets the string from the kernel once and does the arithmetic
itself, so `localtime` and `mktime` in a loop never trap.

### The kernel owns the setting and the file

The kernel keeps the current zone and RTC mode in memory and is the only
writer of **`/system/time.cfg`**:

```
tz=<-03>3
rtc=utc
rtc_offset=0
```

- `rtc_offset` is the offset, in seconds east of UTC, of the value the chip
  holds right now. It is always 0 in `utc` mode. In `local` mode it is what
  makes a zone change, and later a DST transition, survive a reboot (see
  [The RTC in local mode](#the-rtc-in-local-mode)).
- **Read once at boot, right after `fs::attach`**, before the first user
  process. No file, a pure ISO boot, or `--no-disk`: `tz=UTC0`, `rtc=utc`,
  which is exactly today's behavior.
- **Loading it drops the `epoch_seconds()` cache**, so the first date read
  after the load uses the right mode. Nothing may stamp an `mtime` before the
  load. This is checked by a phase gate, not assumed.
- Setting the zone or the mode **persists before it returns**. If the file
  cannot be written (read-only root), the call fails and nothing changes, so
  memory and disk never disagree.
- Setting needs the system, as `SET_REALTIME` does today
  (`cred::acts_as_system`): elevated tools only.
- Boot log: `time: zone <-03>3, rtc utc`.

The system has no settings mechanism yet. Per-user settings live in the profile
(`savanxp_settings_path`), and `/system/update.cfg` is still only planned
([SETUP_AND_UPDATES.md](../../userland/SETUP_AND_UPDATES.md)). This would be
the first system-wide setting, so its `key=value` format is kept that plain on
purpose.

### The ABI

- `SAVANXP_SYS_REALTIME` / `SET_REALTIME` and `struct savanxp_realtime` are
  **defined as UTC, always**, including in `local` mode, where the kernel
  converts at the chip. Their callers (`date`, `ntpsync`, the tests) do not
  change.
- New calls in both subsystems (rule 7), numbers assigned when they land:
  get and set of `{tz string, rtc mode}`. POSIX gets them through the SDK,
  SUN as `sxn_timezone_get` / `sxn_timezone_set`.
- POSIX's `TZ` environment variable overrides the system zone for one
  process, as everywhere else. It changes what `localtime` shows, never the RTC or FAT.

### The RTC in local mode

The conversion lives entirely in `kernel/rtc.cpp`: when the chip is read, it
subtracts `rtc_offset`. When it is written, it adds the offset for the instant
being written and stores that as the new `rtc_offset`.

- **Changing the zone in `local` mode rewrites the chip** so UTC does not move.
  Windows does the same. Changing it in `utc` mode only changes what is shown.
- **Switching the mode rewrites the chip** in the new interpretation.
- **Order: chip first, file second.** A power cut between the two leaves the
  date off by the difference in offsets, and `date` or `ntpsync` fixes it. The
  window is accepted, because closing it would need a journal for a single value.
- **The writable range** (`rtc::valid_time`, years 2000..2099) applies to
  what the chip holds, so in `local` mode the UTC limits move by the offset.

## The gotchas worth remembering

1. **`RealTimeIsUniversal` does not touch FAT.** Windows stamps FAT files in
   local time whichever way its RTC is set. SavanXP stamps them in UTC today
   (`get_fattime()` in `kernel/fat_diskio.cpp`), so a USB stick written here
   shows times off by the zone offset on Windows. That is fixed in phase T5 and does
   not depend on the RTC mode.
2. **DST in `local` mode with a stock Windows on the same disk.** Each OS
   moves the chip at the transition and records that it did so in its own
   storage (Windows: `ActiveTimeBias` in the registry; SavanXP: `rtc_offset`).
   Neither sees the other's record, so the second OS to boot moves the chip
   again. This cannot be coordinated from our side. The answer is `utc`
   mode on both, which is the setup this roadmap was written for.
3. **A bad offset looks like nothing at all** until a file is stamped, the
   same as a bad date ([TIME.md](../../kernel/TIME.md#which-test-proves-which-clock)).
   Every test that moves the zone or the mode restores what was there.

## Phases

Each phase lands with its changelog entry and tests, in the commit that makes
it true.

### T1 — the kernel holds the zone and the RTC mode

- `tzrule` with fixed offsets only (`UTC0`, `<-03>3`, `EST5`). A string with
  DST rules returns `-EINVAL`. Host tests for parsing and conversion.
- Zone and mode in the kernel, `/system/time.cfg` read after `fs::attach`
  and written on set, the boot log line.
- `local` mode in `rtc.cpp`, with chip rewrites on a zone or mode change.
- The get/set calls in both ABIs.
- **Gate:** `date-smoke` and `ntp-smoke` pass in both modes. A new
  `tz-smoke` sets a zone in `local` mode, checks that UTC did not move and that
  the chip did, drops the `epoch_seconds()` cache to read it back the way
  a reboot would, and restores.
  The persistent-image check from `CLAUDE.md` passes, since this writes
  under `/system`.

### T2 — POSIX shows local time

- `localtime(_r)` and `mktime` through `tzrule`. `tzset`, `tzname`,
  `timezone`/`daylight`, `tm_gmtoff`/`tm_zone`, `%z`/`%Z` in `strftime`,
  the `TZ` environment override, and the `timezone` argument of `gettimeofday`.
- `/bin/date` prints local time and gains `-u`. Setting the date stays UTC.
- **Gate:** `datetest`/`libctest` assertions for a negative and a positive
  offset, a `TZ` override, and `mktime(localtime(t)) == t`.

### T3 — SUN reads and sets it, and the `tz` tool

- `sxn_timezone_get` / `sxn_timezone_set` with their `nativetest` assertions.
- `/bin/tz`, a new SUN program (rule 6): bare it prints the zone, the
  mode and both times, `tz <TZ>` and `tz --rtc utc|local` set them. It needs
  elevation, as `date` does.

### T4 — the desktop in local time

- The taskbar clock and the calendar popup (`desktop_taskbar.c`,
  `desktop_clock.c`) show local time. "Today" in the calendar is the local day.

### T5 — FAT in local time

- `get_fattime()` and `fat_disk::epoch_seconds()` convert through the
  current offset, so FAT volumes interoperate with Windows (gotcha 1).
- **Gate:** a file written to a FAT volume reads back with the same `mtime`
  in SavanXP, and its raw FAT stamp is the local time.

### T6 — Win32

- `GetSystemTime`, `GetLocalTime`, `GetSystemTimeAsFileTime`,
  `FileTimeToSystemTime`, `SystemTimeToFileTime`,
  `FileTimeToLocalFileTime` / `LocalFileTimeToFileTime`,
  `GetTimeZoneInformation`. The WordPad profile already needs `GetLocalTime`
  and `FileTimeToLocalFileTime`
  ([WIN32_WORDPAD_PROFILE.md](../../userland/WIN32_WORDPAD_PROFILE.md)).
- `GetTimeZoneInformation` maps one-to-one from a `TZ` string: one standard
  and one daylight bias with one transition rule each, the same model.
- The emulated registry exposes
  `HKLM\SYSTEM\CurrentControlSet\Control\TimeZoneInformation\RealTimeIsUniversal`
  read from the RTC mode, read-only.
- `SetSystemTime`, `SetLocalTime` and `SetTimeZoneInformation` are not
  planned until a profile program needs them.

### T7 — DST rules

- `tzrule` accepts full POSIX `TZ` rules (`Mm.w.d/time`, `Jn`, `n`). The
  hard cases: the repeated autumn hour and the missing spring hour, and how
  `mktime` resolves them with `tm_isdst` (−1 picks the earlier instant, as glibc does).
- In `local` mode the kernel rewrites the chip at each transition and at boot
  when `rtc_offset` no longer matches the offset now in force, then updates the file.
- **Gate:** host tests across both transitions of a northern and a southern
  zone. `tz-smoke` checks a transition crossed while "off" by setting
  `rtc_offset` stale and checking the boot correction. The boot log warns
  when `local` mode is used with a DST zone (gotcha 2).

### Later, not phased

- A *Date and Time* dialog on SUN, for setting the date, zone and RTC mode from
  the desktop. Its consumer slice is the calendar popup, which today only
  displays.

## Rejected: the IANA database

Named zones with full history would need a TZif parser, the zoneinfo data in
the image (hundreds of KB), a build step for it (`zic`, which would have to
come through CMake discovery under rule 5), and a way to update the data when
a government changes its rules, and there is no updater yet. All that buys is
historical offsets and choosing a zone by name. A `TZ` string covers the
present for every zone, and named zones can still be added later as a
name-to-string table, without the history.
