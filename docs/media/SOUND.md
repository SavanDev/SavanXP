# Sound control panel

`/system/sound.sxe` is a SUN application drawn with SxGUI. Open **Sound settings...**
in the speaker popup, or start `sound.sxe` from the Command Prompt or the
System program group. The panel lists every stream that `audiod` currently
mixes; the popup shows up to three (see below).

## Volume popup

The speaker in the taskbar tray opens a 232-by-284 popup
(`subsystems/posix/userland/desktop/desktop_volume.c`) drawn in the desktop's
classic palette:

- **Volume** title, then **Output device** with the selected playback connector
  (`Default output (AC'97)`) and the master slider with its percentage. The
  speaker beside the device toggles master mute and is crossed out while muted.
  Master volume and mute are kernel ioctls, saved to `audio.cfg` like `volume`.
- **Applications**: one row per stream with its icon, name, slider and
  percentage (`Muted` when muted in Sound). The icon is the one the taskbar
  shows for the window of the stream's process, matched by the `pid` audiod
  reports; a stream without a window (a console program, an anonymous UDP
  sender) gets the generic application icon. More than three streams turn the last row into
  `+N more in Sound settings`. The wheel adjusts the row under the pointer, or
  the master elsewhere.
- **Sound settings...** opens this panel and closes the popup.

The rows come from `audiod`'s native control port (`sxnaudio_control_query`
and `sxnaudio_control_change`), the same snapshot this panel reads. The legacy
UDP census must not be used for the tray: it only answers where an IPv4 address
is configured, so on a physical host without a supported NIC the popup listed
no applications at all. `smoke volume-popup-offline` boots without a NIC and
checks the rows, the wheel, mute and the link.

The panel is a fixed-size property sheet modelled on the classic Sounds and
Audio Devices sheet, with five tabs:

| Tab | Contents |
|---|---|
| Volume | The playback device's name, **Device volume** (master slider, level meter, Mute) and **Speaker settings** (headphone switching). |
| Applications | Every stream `audiod` mixes, and the selected stream's volume, meter and Mute. The classic sheet had no per-application mixer; it sits where its Sounds tab was. |
| Audio | **Sound playback** and **Sound recording**, each with its default device; recording adds input gain, meter and Mute. |
| Voice | The recording **Hardware test**: a copy of the input meter, Record test and Play recording. |
| Hardware | Every connector of `audio0` with its type, and the selected one's device, connector and status. |

The status line under the pages is shared by all of them. Controls take effect
as they change, so **Apply** stays disabled and **OK** only closes. **Cancel**
restores the master and input levels and mutes the sheet found when it opened;
output/input choices, headphone switching and application levels stay applied.
The classic sheet's Sounds tab (event sounds) and MIDI group have no
counterpart, because SavanXP has neither yet.

The Volume tab's slider and Mute control operate on the kernel master gain. On
Applications, select a row to adjust that stream's volume or mute it without
changing its volume setting. Muting a stream also silences its packet-loss
concealment. The service saves master, recording and named application controls
as described under Preferences below. Anonymous legacy UDP streams keep
session controls. The popup and `volume` command share the same master setting.

The output dropdown (Audio tab, Sound playback) names the effective audio device
and connector. HD Audio lists each valid analog path (rear line-out and front
headphones on ALC892); firmware-disconnected pins are excluded. AC'97 and
virtio-sound expose their default connector. The catalog includes every bound
backend; output and input choose their default devices independently. HDA
names include PCI location and codec address for identical devices. An
unavailable endpoint is labelled explicitly and cannot be selected.
Selection goes through `audiod`,
the PCM writer, and waits for its acknowledgement. A failed route change
reports the error and queries the effective connector after rollback. If
rollback also fails, no connector is shown as selected; selecting one retries.
For detectable jacks, the dropdown also shows connected, unplugged or unknown.
An unavailable detection facility is labelled `no detection`, which is distinct
from an unplugged connector. **Automatically switch to headphones** is optional
and starts off. It is available only when the current analog device has a
headphone jack that can report presence and another usable output.

Enabling the policy remembers the current non-headphone output as its fallback
(or chooses one on that same device if headphones were already selected). A
valid plug reading selects headphones; a valid unplug returns to that fallback.
Unknown readings preserve the effective route. Another device's jacks and digital
receivers do not participate. A successful manual selection disables the policy.
Failed automatic changes are reported and attempted once per presence change;
toggle the policy or select an output to retry deliberately. The service saves
the explicit policy and its fallback, independently of automatic transitions.

## Recording

The Audio tab's Sound recording group selects the default input connector and controls its
software gain and mute independently of master playback. The live meter shows
the kernel's input peaks after gain/mute. Analog HDA exposes valid microphone,
line-in and auxiliary paths; virtio-sound exposes its capture stream. AC'97 has
no recording support in this backend and shows **No recording device available**.

On the Voice tab, **Record test** stores three seconds of stereo PCM in memory.
**Play recording** replays that buffer through the normal SUN mixer while
the input monitor continues. A dedicated SUN thread paces replay independently
of GUI work; stopping joins it before PCM or its stream can be reused.
No recording is written to disk. A changed input
discards the previous test; gain/mute changes affect subsequent input and leave
an existing recording intact. Lost capture frames or a device error invalidate
the test rather than filling it with fabricated silence. Closing Sound releases
its recording subscription without stopping other subscribers or playback.

Input selection waits for the service acknowledgement. An actual route change
ends existing capture subscriptions with an explicit retry error; Sound reopens
its monitor on the new input. Selecting the already effective input preserves
capture. Input gain and mute are global to the chosen capture stream.
HDMI is deferred to a separate objective.

Without `audiod`, the panel explains that application controls are unavailable
while leaving master controls and direct recording usable. Without an audio device those controls
are disabled too. A disconnected service is retried once a second.

## Native control API

`sxnaudio_endpoint_get` enumerates the kernel inventory without claiming PCM.
An endpoint identifies a hardware connector and groups it under a device id;
it reports direction, backend, availability and effective selection. HDA ids
encode PCI function, codec address and pin node, independently of row order.
Flags distinguish detection capability, a valid current reading and connected
state; clearing validity never represents an unplug event. The SUN helpers
`sxnaudio_jack_policy_query/set` receive request-correlated replies through
`sxnaudio_control_receive_ex`, including support, effective policy and its most
recent automatic transition error.

`AUDIO_IOC_SELECT_ENDPOINT` requires write access and ownership of the affected
PCM direction, or an idle direction. A panel therefore uses
`sxnaudio_endpoint_select` on the native service connection, receiving a
request-correlated acknowledgement and the effective endpoint id. Inventory
reads are available without the daemon; selection is disabled until it responds.

The helpers in `sxn/sxnaudio.h` provide a control-only connection, separate from
`sxnaudio_stream`: `sxnaudio_control_open/close`, `sxnaudio_control_query/read`,
`sxnaudio_control_change`, and typed master gain/mute helpers. The control
connection never acquires the PCM writer; recording replay opens its own
playback stream. `nativetest` exercises
the new surface against the real daemon, including a control connection that
does not appear in the playback census.

Each `savanxp_audiod_stream_state` carries the `pid` of its native client, as
the kernel reported it when audiod accepted the connection (0 for UDP). The
snapshot is read only by programs built from this tree; external ports use
playback records, whose layout is unchanged.

The extension in `audio/audiod_protocol.h` leaves the existing audio, DECLARE,
LIST and SET_VOLUME layouts intact. Native requests ask for one complete
snapshot in one fixed-size record, including all eight possible streams and
mixed output peaks. A request id pairs a reply with its query. The daemon
assigns a new stream id at registration; changes name both that id and its
source port. Delayed controls cannot affect a new stream reusing a port.
A control connection consumes a native connection slot, but no mixer stream.

The panel drains capture every 10 ms and refreshes controls every 200 ms,
without blocking its event loop. Playback selection follows
the stream id across updates. A control change discards an older in-flight
snapshot so it cannot briefly revert the slider. Losing the service clears the snapshot and
selection before reconnecting. Expired/replaced streams disable their controls;
the daemon also rejects an obsolete id even if a GUI event was already queued.
The census respects `audiod`'s existing eight-stream limit, rather than imposing
the popup's three-row presentation limit.

## Preferences

`audiod` automatically saves explicit output/input connectors, headphone policy
and fallback, master volume/mute, input gain/mute and named application levels.
Application keys combine the kernel-authenticated real uid and executable name
with the declared playback role. New stream ids, ports and process ids therefore
retain saved controls; changing the role creates a separate preference. The
table holds 32 nondefault application settings; returning to 100/unmuted removes
an entry. A full table reports an error rather than silently losing another key.
The daemon applies saved levels before the stream's first samples.

Settings belong to the current single-user profile's `Settings` directory:
`audio.cfg` remains the shared master file and `audio-mixer.cfg` carries the
versioned route/policy/recording/application settings. Missing saved devices do
not overwrite the saved choice with a boot fallback. Malformed files leave
available defaults in use and report a load error. The panel reports save/load
errors; closing it requests a durable flush, waiting up to two seconds after
closing the window. Saving continues in the service if that wait expires.
Without the service, direct master/recording controls remain usable but the
panel cannot persist their changes itself (SUN currently has no writable file API).

`sxnaudio_preferences_query/flush` receive request-correlated generation,
saved-generation, enabled/dirty/saving/load-failure flags and the last error via
`sxnaudio_control_receive_full` (return value 4). A successful flush means its
requested generation reached the storage durability frontier. Merely queueing
a write or restarting the writer is insufficient. See the
[service storage contract](AUDIO_DAEMON.md#preferences-and-durability).

## Level meters

The daemon measures absolute stereo S16 peaks while it renders, with a 100 ms
hold window independent of how many panels read them. Each stream's peak is
after its gain and mute. The mixed peak is after the soft limiter and before
kernel master gain; the panel scales it by the current master volume and mute.
The table shows each stream's maximum channel level as a percentage of full
scale; the bars show the master output and the selected application.

These are PCM level meters, not measurements of an analog connector. They do
not prove that a physical amplifier is enabled or speakers are connected.
The input meter uses the last captured packet to decide whether data is fresh;
reading between two measurement windows must not blank a continuous signal.

## Verification

- `./build.sh smoke audio-preferences`: independent continuous source, saved
  output/input, master, capture and SUN/POSIX application levels after daemon
  restart with different pids/stream ids/ports. Exact captured input and a
  separate restored-volume WAV oracle are required, without NIC. Startup is
  primed while muted; autosaves run during the measured playback/capture interval.
- `audio-preferences-test` rejects malformed/versioned/overflowing settings;
  `audio-preference-io-test` injects write, sync and rename failures into the
  actual worker, including rollback and refusal to acknowledge failed durability.
- `./build.sh smoke native-test`: SUN master controls, control-only snapshot,
  application gain/mute and live peaks through the native daemon port, stable
  endpoint inventory, read-only access, PCM ownership, acknowledged selection
  rejection of an unknown id with the effective output retained, policy
  query/enable/disable and rejection of unsupported automation.
- `./build.sh smoke audio-offline` runs those same native assertions with HDA
  and no NIC/network backend.
- `./build.sh smoke audiod-smoke`: exact rendered samples from two clients,
  independent gain/mute, mute during concealment, unmute preserving gain,
  peak values and rejection of commands targeting a reused port.
- `./build.sh smoke sound-smoke` (or `tools/shoot.sh --scenario sound`): two
  sustained SUN sources, opening the panel from the speaker popup, complete
  application list, selection, mute and sliders, and meters that stop/resume.
  The sources are `nativetest.sxe audio-source ToneA/ToneB`, an explicitly
  requested visual-test mode that exits after three minutes. The smoke rebuilds
  and installs this SDK test program before booting. Before running `shoot.sh`
  directly, build/install it with
  `tools/build-user.sh --source sdk/nativetest --name nativetest --native`.
- `sxgui-smoke` still asserts real toolkit glyphs; the existing taskbar scenario
  still checks the popup's frame, slider and master mute geometry.
- `hda-capture` and `virtio-capture` assert actual deterministic stereo input,
  gain/mute, direct and daemon capture, concurrent playback, independent device
  ownership, shared subscribers and explicit packet loss. A host WAV oracle
  also requires a continuous duplex playback interval with the expected tone.
- `sound-capture-smoke` boots HDA without networking and checks input selection,
  live levels, gain/mute, a three-second recording and playback while input is
  muted. The recorded output must retain the fixture's independent 400/600 Hz
  stereo channels, amplitudes and duration; screenshots alone do not pass it.

Per-user persistence and failure recovery remain in the six-stage
[roadmap](../roadmaps/open/AZALIA_ROADMAP.md).

The policy itself is tested by `jack-policy-test`: default manual mode, plug,
unplug, unknown readings, another controller's jack, failed-transition latching,
manual override, unsupported detection and lack of a fallback. The Azalia host
tests check real sense verbs, firmware overrides and timeout/error states.
Physical presence detection and automatic routing remain for the user's final
hardware validation; QEMU's analog codec does not model those host jacks.
