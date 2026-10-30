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
| Sources | `system/nightwood/` (`nightwood.c`, `nightwood_world.c`, `nightwood.h`) |
| Manifest | `system/nightwood/nightwood.sxres`, compiled in as `nightwood.sunres.h` |
| Smoke | `./build.sh smoke nightwood-smoke` (`nightwood --selftest`, no window) |
| Look at it | `./tools/shoot.sh --scenario nightwood` (title, forest, pause, `--still`) |

## Playing

| Key | Does |
|---|---|
| W / S or Up / Down | walk forward and back |
| A / D | step sideways |
| Left / Right | turn |
| Page Up / Page Down | look up and down |
| Shift | run, while the stamina bar lasts |
| E (or Enter, Space) | take the photograph in front of you |
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
the textures, walks up to every photograph and checks that each one is in
reach, takes one and checks that the Warden wakes out of view, then renders
ten frames with the Warden five metres ahead and asserts that its eyes glow,
that the beam lights the ground and that GL raised no error. It also logs the
time per frame and the triangles per frame (about 170 ms and 1000 under TCG
on the development host). `nightwood --still` opens the window on a fixed pose
with the Warden in view and nothing moving, for screenshots.
