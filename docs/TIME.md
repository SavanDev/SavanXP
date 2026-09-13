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
| Good for | CPU accounting (`cpu_ticks`, see [`SYSTEM_MONITORING.md`](SYSTEM_MONITORING.md)) | every deadline in the system |
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
