# Time

Which clock the kernel reads for what, and why the answer changes inside a
hypervisor. Everything that is paced -- `sleep_ms`, `poll` deadlines, the TCP
retransmission timer, the frame clock of a game, the audio feed -- hangs off
what is written here.

## Two clocks, one job each

| | `timer::ticks()` | `process::now_ms()` / `timer::monotonic_ms()` |
| --- | --- | --- |
| What it is | how many timer interrupts arrived | the wall clock |
| Source | the interrupt handler, `+1` per tick | a free-running counter (below) |
| Good for | CPU accounting (`cpu_ticks`, see [`SYSTEM_MONITORING.md`](../userland/SYSTEM_MONITORING.md)) | every deadline in the system |
| Never use it for | measuring time | deciding who got the CPU |

Counting interrupts is not a clock: an interrupt that cannot be delivered --
because the previous handler is still running with `IF=0`, or because the
hypervisor is coalescing -- is time the counter never sees. That is why every
deadline in the kernel is measured with the wall clock and the tick is only the
*heartbeat* that goes and checks whether the deadlines came due.

## The wall clock is not the TSC

The obvious free-running counter is the TSC, and on bare metal it is the right
one: `rdtsc` is a couple of cycles and, on any CPU worth the name, it advances
at a constant rate regardless of frequency scaling.

Inside a hypervisor it stops being the right one, because there are two
different times and the TSC measures the wrong one:

- **Host time** -- the clock on the wall. The TSC follows this.
- **Virtual time** -- the machine's own time, which the hypervisor slows down
  when the guest asks for more CPU than it is being given, and speeds back up
  afterwards. The RTC, the PIT, the ACPI PM timer and the *emulated devices*
  all follow this one.

Measured in VirtualBox with the guest busy (`clocktest`, three spinning
children): the TSC counted **67 seconds while the RTC advanced 12** -- 561% of
real time. Idle, the two agree.

A wall clock that runs 5x fast is not a cosmetic problem. Anything that produces
work at a rate ("how much has to happen since last time") produces five times
too much of it, and it lands on devices that are running on virtual time.

## What that did to the audio

The clearest case, and the one that forced this decision. An audio producer
(Doom, ccleste) mixes *elapsed-worth* of samples once per frame and writes them;
the DAC drains at its own pace. Producer and consumer only stay in step while
both agree on what a millisecond is.

With the wall clock on the TSC, in VirtualBox, with Doom running
(`ac97-stats:`, same build, same VM, only the clock changed):

```
TSC:      real=17573 ms  audio=7895 ms  descartes=1319   <- half the audio thrown away
PM timer: real= 4886 ms  audio=4886 ms  descartes=0      <- exact
```

`descartes` are periods the driver refused because the ring was full: the
producer offered roughly three times more audio than the device could take, and
what did not fit was dropped. That is what a chopped-up sound is.

## The rule

**The wall clock is the ACPI PM timer when the machine exposes a usable one,
and the TSC otherwise** (`timer::adopt_pm_timer`, called from `kernel_main`
right after `acpi::initialize`).

The PM timer is a free-running counter at 3.579545 MHz declared by the FADT. It
has the two properties the job needs at once:

- it is a *counter*, so it does not lose time when the machine halts -- which
  is what disqualified counting ticks;
- it is driven by virtual time, so it agrees with the emulated devices.

"Usable" means **32 bits** (`TMR_VAL_EXT` in the FADT flags). The 24-bit
variant wraps every 4.7 s, and there is no honest way to know how many wraps
happened if nobody read it in that window; the TSC may drift, but it never
loses a chunk of time at once. Today that means VirtualBox (32-bit, port
0x4008) gets the PM timer and QEMU (24-bit, port 0x608) stays on the TSC --
where it measures 99-101% of real time anyway.

The boot log says which one is in use:

```
timer: reloj de pared por PM timer ACPI (0x4008, 32 bits)
timer: PM timer ACPI de 24 bits (0x608), el reloj de pared sigue en el TSC
```

The TSC is still calibrated and still used: it is the fallback, it is what
`timer::tsc_khz()` reports as the processor speed, and it is the anchor the PM
timer clock starts from so the wall clock does not jump when it is adopted.

## Diagnosing a clock problem

Three instruments, all on `/dev/serial`, all comparing something against
something else -- a clock on its own always looks right.

| Line | Compares | Read it as |
| --- | --- | --- |
| `timer-stats:` | ticks delivered vs. wall clock | interrupts the kernel never got. **Only as good as the wall clock:** with a runaway TSC it reported 52-62% lost where the truth was 2% |
| `ac97-stats:` | audio accepted vs. wall clock | `real` ≈ `audio` is a healthy feed; `descartes` is audio thrown away because the producer ran ahead |
| `clock-smoke` | wall clock vs. the RTC, spinning / sleeping / under load | the wall clock itself, against the only reference the guest cannot fake. **The load phase is the one that matters** -- idle, a bad clock looks fine |

The trap worth remembering: when the wall clock is the thing that is broken,
every measurement taken with it is wrong in a way that looks like a different
bug. `timer-stats` blamed lost interrupts, `windowd` reported plausible frame
times, and the actual fault was that the ruler had changed length.

## The other clock: the calendar date

The wall clock above measures *intervals* -- how long until a deadline, how
much audio one frame owes. It never says what day it is. The calendar date
is a separate clock with a separate source: the CMOS RTC, read through ports
`0x70`/`0x71` (`kernel/rtc.cpp`, `include/kernel/rtc.hpp`).

Everything about the date is UTC. There are no time zones in the tree:
`localtime` is `gmtime`.

The read path distrusts the chip. It waits out the update cycle, reads twice,
and only believes the value when both reads agree, so a date never mixes two
seconds. It preserves whatever mode the chip is in (BCD or binary, 12 or 24
hour) instead of forcing one. The year is two digits plus 2000, which is why
the writable range below ends where it does.

## Setting it: `date` and `SET_REALTIME`

`SAVANXP_SYS_REALTIME` (37) reads the date into a `struct savanxp_realtime`
(`include/abi/savanxp_abi.h`); `SAVANXP_SYS_SET_REALTIME` (63) writes it back
(`realtime()` / `set_realtime()` in the SDK). The `valid` flag is output-only
and is ignored on input.

The kernel accepts years **2000..2099** with a leap-aware day check
(`rtc::valid_time`): a bad date returns `-EINVAL` without touching the chip,
and a chip that never leaves its update cycle returns `-EIO` instead of
hanging the caller. The write freezes the chip with the SET bit so all seven
registers land on the same second, recomputes the weekday, then releases it.

`/bin/date` is the userland face: bare it prints `YYYY-MM-DD HH:MM:SS`, with
six arguments it sets the clock and re-reads it:

```bash
date
date YYYY MM DD HH MM SS   # UTC
```

`date-smoke` (`datetest`) sets a known date mid-minute, re-reads it (the
second tolerates +1 for the tick boundary), proves month 13 comes back
`-EINVAL` with the chip untouched, and restores whatever time was there.

## Syncing it: `ntpsync` and SNTP

`/bin/ntpsync` is a one-shot SNTP client (RFC 4330, mode 3) over UDP
(`subsystems/posix/userland/ntpsync_lib.c`): numeric IPv4 only -- no DNS,
like the rest of the stack -- default port 123, 5 s timeout.

```bash
ntpsync <ipv4> [port]   # UTC
```

Network time becomes system time with suspicion, because whatever arrives
over UDP decides the machine's date. The reply must be mode 4, version 3 or
4, stratum 1..15 (0 is kiss-of-death, 16 is unsynchronized), with a non-zero
transmit stamp; the result lands in a sane window, 2020-01-01..2036-02-07,
where the floor rejects garbage and the ceiling is the 32-bit era wrapping
in 2036. Delay is compensated the cheap honest way -- transmit plus `rtt/2`,
measured with `uptime_ms`, in integer milliseconds because userland runs
without SSE. The tool applies the result through `SET_REALTIME` and reports
the applied time, the round trip, and the adjustment in seconds.

`ntp-smoke` (`ntptest`) syncs against the deterministic host fixture
(`tools/ntp_server.py`), checks the RTC landed within a second of what was
applied, checks a closed port fails instead of hanging or writing garbage,
and restores the original time.

## Showing it: the taskbar clock and calendar

The taskbar (`subsystems/posix/userland/taskbar.c`) shows a sunken `HH:MM`
readout taken from the RTC in UTC, with an `uptime_ms` fallback when no
valid RTC exists -- the same split as `windowd_current_clock_stamp()`.
Clicking it toggles `clockpopup` (178x180, no launcher entry, display-only):
a W2K-style month grid, Monday-first with Spanish day labels, today
highlighted, and a live `HH:MM:SS` footer. The WM opens and closes it
(toggle or outside click); its clicks drain without action.

## Which test proves which clock

| Test | Proves | Against |
| --- | --- | --- |
| `clock-smoke` | the wall clock (intervals) | the RTC, spinning / sleeping / under load |
| `date-smoke` | setting the date | a re-read, a rejection, and a restore |
| `ntp-smoke` | syncing the date over the network | a host fixture plus a closed port |

A bad wall clock looks like lost interrupts or chopped audio; a bad date
looks like nothing at all until something stamps a file. That is why the
first row compares two clocks and the other two restore the one they moved.
