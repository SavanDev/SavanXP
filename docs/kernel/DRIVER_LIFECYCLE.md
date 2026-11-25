# Driver start and stop

Status: **D3 of [DRIVERS_ROADMAP.md](../roadmaps/open/DRIVERS_ROADMAP.md)
(2026-11-24).** The contracts below are common. Written to them: the RTL8139
(`kernel/rtl8139.cpp`) and the AC'97 (`kernel/ac97.cpp`). Every other driver
still starts once at boot and never stops. It builds on the inventory's
ownership and claims ([DEVICE_INVENTORY.md](DEVICE_INVENTORY.md)).

## The contracts

| Operation | Contract |
| --- | --- |
| `start` | Takes the device in stages. Each stage that acquires something records how to give it back; a stage that fails gives back exactly what the stages before it took, last taken first, and the start returns false |
| `quiesce` | Stops new work, masks the device's interrupts at the device, stops its DMA engines and turns bus mastering off, then **reads all of it back**. It returns true only when the device can neither interrupt nor reach memory |
| `stop` | `quiesce`, then gives back everything `start` took, in the same reverse order. A device that does not quiesce keeps everything and is listed as `FAILED`: memory it may still write is never freed |
| `remove`, `recover` | Not defined yet (D5 and later). A driver without them says so; nothing pretends |

A start and a stop leave the inventory in a known state:

| Outcome | Listed as | Who may take the function |
| --- | --- | --- |
| Started | `STARTED` | Its driver |
| Start failed, everything given back | `UNBOUND`, `START_FAILED`, with the driver's name (`inventory::pci_release_failed`) | Any driver |
| Start failed at the claim (another driver holds it) | Unchanged | Its holder |
| Stopped | `UNBOUND`, `NOT_CLAIMED` (`pci_release`) | Any driver |
| Did not quiesce | `FAILED`, `START_FAILED`, claims kept | Nobody else |

## The pieces

- **`lifecycle::Acquired`** (`include/kernel/lifecycle.hpp`): the record of
  one start. `push(stage, release, context)` after (or, for an allocation
  that can half succeed, before) each acquisition; `unwind()` calls every
  release, last pushed first, and leaves the record empty, so nothing is
  given back twice. A full record (16 steps) refuses the push, and the
  caller gives back what it just took itself.
- **Failure injection**: `lifecycle::inject_failure(driver, stage)` makes the
  next start of that driver fail at that stage, once;
  `lifecycle::injected(driver, stage)` is what a start asks at the top of
  each stage. Only self-tests set it.
- **Taking an interrupt handler back**: `arch::unregister_interrupt_handler`
  and, for a routed GSI, `ioapic::unroute_gsi`. The dispatcher takes the
  kernel lock before it reads a vector's chain, and the kernel runs with
  IF=0 while it holds that lock, so **a handler taken off under the kernel
  lock, or during boot on the BSP before the APs run, is not running
  anywhere when the call returns and never runs again.** The removal itself
  runs with IF=0, so a nested dispatch on the same core cannot see a chain
  half closed up. The last handler off a GSI masks its IOAPIC entry; the GSI
  keeps its vector, and routing it again unmasks it.
- **The inventory**: `pci_release_failed()` for a start that gave everything
  back, `pci_claim_count()` to check nothing is left claimed.

## The RTL8139

`start()` has five stages, in this order, each named in the log when it
fails:

1. **claim**: `inventory::pci_claim`. A refusal leaves the card alone.
2. **registers**: I/O BAR 0, claimed with its length; port decoding on.
3. **interrupt**: INTx through `_PRT` and the IOAPIC, and its GSI claimed as
   shared. A line that cannot be routed is not a failure: the driver polls,
   as before.
4. **buffers**: the receive ring and four transmit pages. The release is
   recorded first, so a half-done allocation is freed too.
5. **translation**: with the IOMMU translating, a domain of its own
   ([IOMMU.md](IOMMU.md#domains-of-their-own-m3)). Stopping destroys it,
   which moves the card to the blocked domain.

The reset and the receive set-up are not part of `start`: `prepare()` runs
them when `net::` asks (`NET_IOC_UP`), as before. Its progress is now global
state that `stop()` resets, so a card started again resets from the
beginning. `quiesce()` clears IMR and the command register, acknowledges
pending status, turns bus mastering off, and checks all three.

`nic::` only ever starts the card (`probe` is `start`); nothing in the
running system stops it yet. The first caller of `stop()` outside the
self-test will be a management action (D6).

## The AC'97

`start()` has five stages:

1. **claim**: `inventory::pci_claim`.
2. **registers**: the mixer (NAM) and bus master (NABM) I/O BARs, claimed
   with their lengths; port decoding on.
3. **codec**: cold reset until the primary codec reports ready, then the
   mixer unmuted. Nothing to give back: a codec that never answers just
   fails the stage.
4. **buffers**: the BDL page and the 32 period buffers, one contiguous
   allocation; the release is recorded first.
5. **engine**: bus mastering on, the PCM out engine halted and reset, and
   the BDL's address programmed.

It has no interrupt: the driver polls CIV, and no BDL entry asks for one.
`quiesce()` halts the engine (run bit off, then the register reset), turns
bus mastering off, and reads back the run bit, the DMA-halted status bit and
the bus master bit. A stream (`ac97_configure`, which `audio::` asks for)
still comes after `start`, and `stop()` ends it first.

## The self-test

One test for every driver written to the contracts
(`kernel/lifecycle_selftest.cpp`). A driver hands it a `lifecycle::Subject`:
its name, how many stages its start has, and its own `find`, `start`,
`bring_up` (as far as its class core would take it; optional), `stop`,
`idle` (none of its own state left) and `vector` (where its interrupt is,
0 when it polls). Adding a driver is one entry in `kSubjects`.

Boot option `lifecycle-selftest=<driver>[,<driver>...]` runs it before any
class core takes a device. Each named driver must have a device to test, or
the test fails; a name not in `kSubjects` fails too. For each:

- A failure injected at each stage: the start returns false, the free page
  count is what it was, no range is claimed, the driver is idle, and the
  device is listed `UNBOUND`, with `START_FAILED` and the driver's name from
  stage 2 on (stage 1 fails before the claim, so the device is as it was).
- Four starts and stops, the third brought up when the driver can be: after
  each stop the pages, the claims and the state are back, the driver is
  idle, and its vector's chain is one handler shorter.
- The device is then taken by its class core as usual, and the scenario
  must pass over it: `netsmoke` for the RTL8139, interrupts included.

The log ends in `lifecycle: selftest passed`, or `lifecycle: selftest failed:
<driver>: <step>`.

`smoke lifecycle-rtl8139` runs `lifecycle-selftest=rtl8139` on the default machine;
`smoke lifecycle-rtl8139-iommu` with AMD-Vi translating, where each cycle
also creates and destroys the card's domain and its page tables, which the
page count covers. `smoke lifecycle-ac97` runs `lifecycle-selftest=ac97` on
QEMU's AC97, its third cycle playing silence when it stops, then plays a
stream. The pure part has `lifecycle-test` on the host.

## Not covered yet

- **References.** Nothing holds a stopped device: there are no handles,
  class sessions or queued requests that outlive it, because `nic::` never
  stops its card. Counting them, and refusing or waiting on a stop while any
  are held, comes with the first management action that stops a device in
  use.
- **The other drivers.** Each moves to these contracts on its own, the way
  the RTL8139 did. Drivers that touch shared state at start (USB host
  controllers and their children, the block devices `partition` hangs off)
  need their class core's help to stop.
- **PIC lines.** `arch::unregister_interrupt_handler` covers a PIC vector too,
  but nothing masks the PIC line when its last handler goes; no driver
  written to these contracts uses one.
- **MSI and MSI-X**, which no driver here takes back yet.
