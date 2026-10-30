# Nightwood — the first game written against SUN

`/bin/nightwood.sxe`, in the Games group. A night in a fenced forest with a
flashlight: eight photographs are pinned to landmarks among the trees, and
something tall with antlers — the Warden — follows the player more closely
with every photograph taken. Looking at it for long is what ends the night.

It exists for two reasons, and both shape it:

- **It is the second consumer of SxGL** ([SXGL.md](../desktop/SXGL.md)), the
  one that asked for batches 3 and 4: textures, linear fog, alpha-tested
  needles, an additive glow, spot lighting and a scissor-free full-window GL
  surface. `gears` asks for the rest.
- **It is a SUN program** ([SUN.md](SUN.md)), so SxGL had to be platform code
  presented by the caller, not a POSIX library. Whatever SUN lacked for a game
  was added with it as the consumer, by SUN's rule 1.

| | |
|---|---|
| Sources | `system/nightwood/` (`nightwood.c`, `nightwood_world.c`, `nightwood_audio.c`, `nightwood.h`) |
| Manifest | `system/nightwood/nightwood.sxres`, compiled in as `nightwood.sunres.h` |
| Smoke | `./build.sh smoke nightwood-smoke` (`nightwood --selftest`, no window) |
| Look at it | `./tools/shoot.sh --scenario nightwood` (title, forest, mouse look, pause, `--still`) |

## Playing

| Key | Does |
|---|---|
| W / S or Up / Down | walk forward and back |
| A / D | step sideways |
| the mouse | look around (the pointer is captured while playing) |
| Left / Right, Page Up / Page Down | turn and look up and down without a mouse |
| Shift | run, while the stamina bar lasts |
| E, a left click, Enter or Space | take the photograph in front of you |
| F | switch the flashlight off and on |
| Esc | pause; Esc again gives up the night |

The rules of the Warden, all in `nw_update_play`:

- It does not exist until the first photograph is taken. Each photograph
  moves it at once.
- It appears where the player is not looking (90° to 270° from the view),
  closer with every photograph: about 22 m at the start, 7 m at the end. It
  moves again every 5 to 16 seconds, but never while it is being looked at.
- From the fourth photograph on, it walks towards the player while unseen.
- Looking at it — inside the view, not hidden behind a trunk — fills the fear
  meter, faster the nearer it is. Not looking drains it. The screen fills with
  static as the meter rises; when it is full, or when the Warden is within
  arm's reach, the night is over.
- Eight photographs end the night the other way.

Every night is a new forest: the seed is the clock. The eight landmarks (a
boulder, a shed, a water tank, a wrecked car, a ruined wall, an old oak, a
signpost and a stone arch) sit one per sector of a ring 20 to 48 m around the
clearing where the player starts, each facing roughly back towards it.

## Sound and mouse

**Sound** (`nightwood_audio.c`) is synthesized on a SUN thread of its own, so
a slow frame never starves the speaker: wind (noise through two filters that
breathe with slow gusts), a low drone of two detuned sines, a crunch of
filtered noise per footstep, a heartbeat that quickens and swells as the
Warden nears or is watched, the screen's static, the shutter and chime of a
photograph, and the roar at the end. Oscillators read a sine table; nothing is
a file, because a SUN program has none. The game thread only describes the
moment (`nw_audio_update`); the sound thread keeps the stream 80 ms ahead with
`sxnaudio_frames_due`. Through `audiod` it appears in the volume popup as
"Nightwood"; with no daemon it writes the device; with no device the game is
silent and otherwise unchanged.

**Mouse look** is SUN's pointer capture: entering play captures the pointer,
and the title, the pause and the endings release it. Each
`SAVANXP_WM_EVENT_POINTER_MOTION` turns the head a tenth of a degree per
count, and a left click takes the photograph. The WM suspends the capture by
itself whenever the window loses the focus, so Alt+Tab always works.

## How a frame is made

1. **SxGL draws the forest into a bitmap half the window's size**
   (`nw_world_render`). Half size is what keeps the software rasterizer at a
   playable rate, and the large pixels suit the night. The flashlight is a GL
   spot light placed in eye space; the moon a dim directional light fixed in
   the world; the fog linear from 3 to 26 m. Everything lit is tessellated to
   about a metre, because SxGL lights per vertex and a beam that falls between
   four far corners would light nothing.
2. **The SUN side scales it up by two into the window surface** (`nw_compose`),
   multiplying each pixel by a precomputed mask that is the beam's soft edge,
   adding film grain, and blending in the static.
3. **The HUD is drawn on top with the SxGFX painter** (`nw_draw_hud`) at full
   resolution, so the text stays sharp.
4. **The whole window is presented** with `sxnwm_submit_full`, after
   `sxnwm_wait_idle` says the compositor has read the previous frame.

`nightwood_world.c` makes no system call: generation, collision, the seven
procedural textures and the drawing are computation over memory, which is what
lets `nightwood --selftest` and a host harness run it without a window.

## Testing it

`nightwood --selftest` runs with no window: it builds a fixed forest, uploads
the textures, starts the sound thread (straight to the device: the smoke has
no daemon), walks up to every photograph and checks that each one is in
reach, takes one and checks that the Warden wakes out of view, feeds a
captured motion and checks the head turns the right way, then renders
ten frames with the Warden five metres ahead and asserts that its eyes glow,
that the beam lights the ground and that GL raised no error. It also logs the
time per frame and the triangles per frame (about 170 ms and 1000 under TCG
on the development host). `nightwood --still` opens the window on a fixed pose
with the Warden in view and nothing moving, for screenshots.
