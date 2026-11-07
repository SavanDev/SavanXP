# Sound control panel

`/bin/sound.sxe` is a SUN application drawn with SxGUI. Open **Sound settings...**
in the speaker popup, or start `sound.sxe` from the Command Prompt or the
System program group. The popup keeps its compact size and shows up to three
application rows (including its overflow row); the panel lists every stream
that `audiod` currently mixes.

The upper slider and Mute control operate on the kernel master gain. Below
Applications, select a row to adjust that stream's volume or mute it without
changing its volume setting. Muting a stream also silences its packet-loss
concealment. Stream volume and mute last for that stream's lifetime. This
first panel applies live controls; it does not write persistent preferences.
The existing popup and `volume` command still save master settings as before.

The output dropdown above the master controls names the effective audio device
and connector. HD Audio lists each valid analog path (rear line-out and front
headphones on ALC892); firmware-disconnected pins are excluded. AC'97 and
virtio-sound expose their default connector. Selection goes through `audiod`,
the PCM writer, and waits for its acknowledgement. A failed route change
reports the error and queries the effective connector after rollback. If
rollback also fails, no connector is shown as selected; selecting one retries.
Capture controls and HDMI configuration are not implemented yet.
Without `audiod`, the panel explains that application controls are unavailable
while leaving master controls usable. Without an audio device those controls
are disabled too. A disconnected service is retried once a second.

## Native control API

`sxnaudio_endpoint_get` enumerates the kernel inventory without claiming PCM.
An endpoint identifies a hardware connector and groups it under a device id;
it reports direction, backend, availability and effective selection. HDA ids
encode PCI function, codec address and pin node, independently of row order.
`AUDIO_IOC_SELECT_ENDPOINT` requires write access and ownership of the affected
PCM direction, or an idle direction. A panel therefore uses
`sxnaudio_endpoint_select` on the native service connection, receiving a
request-correlated acknowledgement and the effective endpoint id. Inventory
reads are available without the daemon; selection is disabled until it responds.

The helpers in `sxn/sxnaudio.h` provide a control-only connection, separate from
`sxnaudio_stream`: `sxnaudio_control_open/close`, `sxnaudio_control_query/read`,
`sxnaudio_control_change`, and typed master gain/mute helpers. The panel never
writes PCM or acquires the audio device's writer slot. `nativetest` exercises
the new surface against the real daemon, including a control connection that
does not appear in the playback census.

The extension in `audio/audiod_protocol.h` leaves the existing audio, DECLARE,
LIST and SET_VOLUME layouts intact. Native requests ask for one complete
snapshot in one fixed-size record, including all eight possible streams and
mixed output peaks. A request id pairs a reply with its query. The daemon
assigns a new stream id at registration; changes name both that id and its
source port. Delayed controls cannot affect a new stream reusing a port.
A control connection consumes a native connection slot, but no mixer stream.

The panel polls every 200 ms without blocking its event loop. Selection follows
the stream id across updates. A control change discards an older in-flight
snapshot so it cannot briefly revert the slider. Losing the service clears the snapshot and
selection before reconnecting. Expired/replaced streams disable their controls;
the daemon also rejects an obsolete id even if a GUI event was already queued.
The census respects `audiod`'s existing eight-stream limit, rather than imposing
the popup's three-row presentation limit.

## Level meters

The daemon measures absolute stereo S16 peaks while it renders, with a 100 ms
hold window independent of how many panels read them. Each stream's peak is
after its gain and mute. The mixed peak is after the soft limiter and before
kernel master gain; the panel scales it by the current master volume and mute.
The table shows each stream's maximum channel level as a percentage of full
scale; the bars show the master output and the selected application.

These are PCM level meters, not measurements of an analog connector. They do
not prove that a physical amplifier is enabled or speakers are connected.

## Verification

- `./build.sh smoke native-test`: SUN master controls, control-only snapshot,
  application gain/mute and live peaks through the native daemon port, stable
  endpoint inventory, read-only access, PCM ownership, acknowledged selection
  and rejection of an unknown id with the effective output retained.
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

Per-user persistence, multiple controllers, recording and automatic jack
switching remain in the six-stage [roadmap](../roadmaps/open/AZALIA_ROADMAP.md). Their absence is not represented by
nonfunctional controls in this panel.
