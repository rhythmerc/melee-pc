# Meta Quest mixed-reality build

The game runs on a virtual screen floating in front of the player, over
passthrough. It is the regular Android build with OpenXR presentation added in
aurora (`extern/aurora/lib/xr`). The game renders exactly as it does for a
window, and only the present step changes.

## Hybrid app: launcher panel, immersive game

It's a hybrid app, following Meta's
[hybrid apps guide](https://developers.meta.com/horizon/documentation/spatial-sdk/hybrid-apps-overview/):

- **Launcher.** `MeleeActivity` is a 2D panel in the Home environment, with
  the `com.oculus.intent.category.2D` category. Disc selection, Verify, and
  settings work with the controller pointer like any panel app.
- **Play.** It starts `MeleeXrActivity` with the chosen disc as the `disc`
  extra, then closes the panel (`src/pc/main.c`,
  `pc_android_launch_xr`).
- **Game.** `MeleeXrActivity` is immersive, with the `VR` category, and runs
  in its own `:xr` process, because SDL keeps process-wide state. Before SDL
  starts, it sets `AURORA_XR=1` and `MELEE_XR_ACTIVITY=1`. The disc extra
  skips the launcher, so it boots straight into the game.
- **No disc.** If the immersive activity starts without one, it reopens the
  launcher panel in Home instead of showing the launcher in the headset.

XR is off in the panel's process, so the launcher renders to its window as
on a phone.

## 3D fights

During a fight, the stage and fighters are drawn in stereo 3D on a tabletop
arena over passthrough. The HUD floats above it. Everything else (menus,
character and stage select, results) stays on the virtual screen. The plan
and the investigation behind it are in docs/xr-3d-plan.md.

- **Tagging.** The game tags what it draws (`src/pc/xr_scene.c`):
  - The fight camera (`cm/camera.c`, `fn_800301D0`) tags world draws.
  - The HUD camera (`if/ifall.c`) tags HUD draws.
  - Everything else is mono, meaning it's only in the normal frame.

  The tags travel through the GX FIFO (`aurora_xr_camera`,
  `GX_AURORA_XR_CAMERA`), so they line up with the draws, and each recorded
  command keeps its category.
- **Stereo replay.** Each frame the normal flat frame renders as before. It
  still feeds the Pokémon Stadium screen and other framebuffer copies.
  Then `lib/xr` replays the world draws once per eye (`gfx/xr_replay.hpp`).
  GX bind group 3 swaps the game's projection for
  `P_eye · V_eye · A · V_game⁻¹`: undo the game camera, place the arena in
  the room, then look from the eye. Lighting, skinning, and projected
  shadows are unchanged.
- **Passthrough compositing.** Both eyes go side by side into one shared
  image, submitted as a projection layer with the poses they were rendered
  for. Alpha is depth coverage, with premultiplied blending:
  - Geometry hides the room.
  - Translucent parts that write no depth add light over it, like
    holograms.
- **HUD.** HUD draws are replayed onto their own plane above the arena.
  Bright text and icons are opaque, and black areas are see-through.
- **Stage backgrounds.** Stage parts on layer 2, the far background, are
  left out of the 3D view. Layers 0 and 1 are the stage itself. Per-stage
  rules in `xr_scene.c` can name a whole part, one joint of a part (and
  everything under it), or one mesh of a joint. Hidden geometry still draws
  into the flat frame, because the Pokémon Stadium screen copies it.
  - **Pokémon Stadium:** shows only the big screen and the stage.
    - **Hidden:** the city and sky (part 1, joints 11 and 12), the whole
      stadium bowl with its stands, walls, floor and light rings (part 2,
      joint 2), and the column under the platform (part 2, joint 3, meshes
      28-31).
    - **Moved:** the big screen is pulled in from z -215 to just behind the
      stage and shrunk to 0.55, in 3D only (`MELEE_XR_JUMBOTRON`).
- **Moving pieces.** A rule can also move a joint in the 3D view only.
  `aurora_xr_world_transform` gives the following world draws an extra
  world-space placement, and the replay binds a separate eye matrix for
  each one.
- **Background colour.** The fight camera fills the screen with the stage's
  background colour before it draws. That fill stays out of the 3D view.
  Before, it was a card behind the arena, a blue one on Green Greens.

To survey a stage on desktop:
- **Boot into it:** `MELEE_BOOT_SCENE=vs MELEE_DEBUG_VS_STAGE=<StKind>`.
- **List its contents:** `MELEE_XR_STAGE_LOG` logs every part with its
  layer, and `MELEE_XR_JOINT_LOG` adds each part's joints with their mesh
  counts and positions.
- **Try rules:** with `MELEE_XR_PARTS`.
- **Capture the result:** `AURORA_XR_DUMP` writes the eye images,
  `AURORA_XR_DUMP_AFTER` picks the frame, and `AURORA_XR_ARENA_POS` and
  `AURORA_XR_ARENA_YAW` frame the view.

Checked on desktop against Monado (`AURORA_XR_DUMP` images):
- **Final Destination** (VS boot scene) and **Battlefield** (training boot
  scene) render in 3D at 60 frames per second.
- **The title screen** stays on the virtual screen.

### Placing the arena

Pause the fight to move the arena. Each controller shows a laser, and the
grips grab instead of pressing Z. In some modes Z on the pause screen retries
the match, so the grips don't pass it through while paused. Triggers, A and
Start work as usual, so L+R+A+Start still quits.

- **One hand.** Point at the arena and squeeze a grip. The arena hangs off
  the laser at the point you grabbed, and its front turns to face you. Let
  go and it stays there, facing the way it was.
- **Two hands.** While holding with one hand, squeeze the other grip
  anywhere. Spreading or closing your hands scales the arena, and turning
  them turns it about the vertical axis, around the point between your
  hands. Letting go of either hand ends the grab, so the turn is kept.

The HUD moves, turns and scales with the arena. The placement lasts for the
session and is logged when you let go (`Arena placed at x,y,z, yaw, scale`),
which is handy for `AURORA_XR_ARENA_POS` and `AURORA_XR_ARENA_SCALE`.

The lasers are quad layers with static textures, drawn at display rate from
the latest controller poses. Coloring them needs
`XR_KHR_composition_layer_color_scale_bias`. A laser is cyan on the arena,
amber while grabbing, and faint white otherwise. The arena moves at the
game's frame rate.

### Performance

On a Quest 3 (2026-10-01), a VS match on Final Destination at full eye
resolution (1680×1760 per eye):

| | First run | Now |
|---|---|---|
| Display | 72 Hz, 33 to 51 fps | 120 Hz, 120/120, no stale frames |
| Game frames | about 35/s | 60/s, each shown for 2 display frames |
| 3D eyes (replay plus coverage) | about 8.7 ms (two replays plus composite) | 6.4 to 7.4 ms, one pass |
| Bridge copy of the 3D image | 3.5 ms | 0.84 to 0.98 ms |
| Flat present during fights | rendered | skipped |
| App GPU time | 21 to 27 ms per 72 Hz frame | 4.5 to 5 ms per 120 Hz frame (about 9 to 10 ms per game frame, 16.7 ms budget) |

What changed:

- **Flat present skipped during fights.** Nothing shows the virtual screen
  during a fight, so aurora skips the present entirely (`xr::skip_present`).
- **Direct 3D path.** Both eyes render straight into the shared 3D image in
  one pass when the framebuffer format matches and MSAA is off. Coverage goes
  into alpha in the same pass: two full-screen depth-tested triangles write
  only alpha. Depth is discarded. There's no separate compose pass and no
  private eye images.
- **Shader copy on the bridge.** `vkCmdCopyImage` ran at about 13 GB/s on
  Adreno. A full-screen draw into the swapchain image (`lib/xr/shaders`)
  is about four times faster. It decodes sRGB before the sRGB attachment
  re-encodes it, so the bytes come through unchanged.
- **HUD at half resolution.**
- **120 Hz, lock-step.** The Quest 3 offers 72, 80, 90, and 120 Hz, with no
  60. 120 is requested (`XR_FB_display_refresh_rate`), and the game waits on
  the XR thread's tick every second display frame (`aurora_xr_pace`, from
  `src/pc/vi.c`) instead of its own timer. Every game frame is then shown for
  exactly two display frames, instead of 72 Hz repeating every fifth.
  Netplay keeps its own timer.
- **Measurement.** `AURORA_XR_TIMING` logs per-pass GPU time (Dawn
  timestamps) and per-stream bridge copy time (Vulkan timestamps) every 10 s.

### Shaders

The immersive process builds pipelines on one low-priority compile thread.
At boot it builds every config this device has built before, and a draw
whose pipeline isn't ready yet is skipped for a frame instead of stalling the
render thread. The launcher panel builds and waits for nothing, because its
pipelines would die with it.

Both processes request the same Dawn device features, so they share one
Dawn blob cache. Before, the game process missed every entry the panel had
written, and the panel's prune deleted the game's entries on every launch.

On a Quest 3 (2026-10-01):

| | Before | After, cold cache | After, warm cache |
|---|---|---|---|
| Pipelines built by the game | ~470 | 1,584 in the background | 1,596 in the background, 9.3 s |
| Average per pipeline | ~85 ms | 61 ms | 5.8 ms |
| Built while a draw waited | 467 | 20 | 0 |

`AURORA_PIPELINE_INLINE=1` brings back inline compiles (Android's default
elsewhere, after an Adreno 750 rejected desktop-seed configs off the draw
thread).

### Known gaps in 3D

From the first headset playtest (2026-10-01):

- **Crash on KO.** The game flashes the 2D viewport and then crashes when a
  character dies. It's reproducible and not yet investigated.
- **Stage parts.** They need a per-stage pass. Final Destination, Pokémon
  Stadium, Dream Land and Green Greens have been checked.
- **Full VR mode for fights (pinned).** An option for fights in full VR, not
  over passthrough, alongside the mixed-reality arena.

- **Background sparkles.** They're effects drawn under the fight camera, so
  some still show, for example Battlefield's twinkles and Final Destination's
  stars.
- **Fog** still uses the eye's depth instead of the game camera's.
- **Billboards and particles** face the game camera, not the eye.
- **Frame rate.** The 3D view updates at the game's 60 Hz. Every second
  display frame reuses the previous image and relies on the runtime's
  reprojection; re-rendering it with the newer head pose isn't done.
- **Placement.** It isn't saved between sessions, and there's no table or
  floor anchoring.
- **Coverage.** Only the static stages have been looked at.

## How the frame gets to the headset

Dawn can't adopt a Vulkan device someone else created and doesn't expose its
own, while OpenXR has to create the device its swapchains live on. So there
are two devices on the same GPU:

- **Bridge device.** An XR thread creates it through `XR_KHR_vulkan_enable2`.
  It owns the session, passthrough (`XR_FB_passthrough`), and a quad-layer
  swapchain, and it allocates three shared images.
- **Dawn device.** aurora's normal device imports those images as
  `SharedTextureMemory`. aurora's present pass (`aurora.cpp`, `end_frame`)
  draws into one instead of the window surface.
- **Handoff.** Semaphores go both ways as file descriptors. Only descriptors
  cross threads; every Dawn call stays on the render worker.

| | Quest (Android) | Linux / Monado |
|---|---|---|
| Shared memory | AHardwareBuffer | opaque FD |
| Semaphores | sync FD | opaque FD |
| Queue family handoff | `VK_QUEUE_FAMILY_FOREIGN_EXT` | `VK_QUEUE_FAMILY_EXTERNAL` |

The screen texture is 1080 pixels tall, with a width matching the game's
presented aspect. A Quest app's window spans the whole display panel, so the
window size isn't used. `aurora_vsync_enabled()` reports false while XR is
active. At 120 Hz the game is paced by the XR thread (see Performance);
otherwise it keeps its own 60 Hz timer. The XR thread runs at display rate
and shows the newest finished frame.

## Build and run

```sh
MELEE_XR=1 tools/build_android.sh            # release: dist/Melee-Quest-XR-arm64.apk
```

For a debug build without the release key, run the native step, then
`./gradlew :app:assembleDebug -Pmelee.xr=true` in `platforms/android`. The XR
build uses the same package id as the flat build, so it replaces it on the
device.

Desktop testing against Monado:

```sh
cmake -B build/linux-xr -G Ninja -DAURORA_ENABLE_OPENXR=ON ...   # same flags as build/linux
AURORA_XR=1 build/linux-xr/melee disc.rvz
```

## Controls (Touch controllers, port 1)

| Touch | GameCube |
|---|---|
| Left stick | Control stick |
| Right stick | C-stick |
| A / B | A / B |
| X / Y | X / Y |
| Triggers | Analog L / R, digital past 90% |
| Either grip | Z (grabs the arena while paused) |
| Left menu button | Start |

## Knobs

On Quest, set these in `/sdcard/Android/data/dev.melee.game/files/melee-env.txt`.

| Variable | Default | Effect |
|---|---|---|
| `AURORA_XR` | set by `MeleeXrActivity`; off otherwise | Present to the headset |
| `AURORA_XR_PASSTHROUGH` | 1 | Passthrough behind the screen |
| `AURORA_XR_SCREEN_WIDTH` | 1.6 | Screen width in meters |
| `AURORA_XR_SCREEN_DISTANCE` | 1.5 | Meters in front of the starting head position |
| `AURORA_XR_SCREEN_Y` | 0 | Height offset in meters |
| `AURORA_XR_SCREEN_HEIGHT` | 1080 | Screen texture height in pixels |
| `AURORA_XR_3D` | 1 | 3D fights. Set 0 to keep fights on the virtual screen |
| `AURORA_XR_EYE_SCALE` | 1.0 | Eye resolution, as a fraction of the runtime's recommendation |
| `AURORA_XR_REFRESH` | unset | Display rate to request if offered (otherwise 60, then 120) |
| `AURORA_XR_LOCKSTEP` | 1 | Pace the game to the display when it runs at a multiple of 60 Hz |
| `AURORA_XR_DIRECT` | 1 | Render both eyes straight into the shared 3D image when possible |
| `AURORA_XR_SHADER_COPY` | 1 | Copy into swapchains with a draw instead of `vkCmdCopyImage` |
| `AURORA_XR_FIGHT_SCREEN` | 0 | Keep presenting the flat screen during fights (debugging) |
| `AURORA_XR_HUD_SCALE` | 0.5 | HUD texture resolution, relative to the screen |
| `AURORA_XR_TIMING` | 1 | Log GPU pass and copy times every 10 s |
| `AURORA_PIPELINE_INLINE` | 0 | Compile pipelines on the render thread instead of the compile thread |
| `AURORA_XR_ARENA_SCALE` | 0.006 | Starting meters per game unit (grab with two hands to change) |
| `AURORA_XR_ARENA_YAW` | 0 | Starting arena turn in degrees (counter-clockwise from above) |
| `AURORA_XR_ARENA_POS` | `0,-0.45,-1.0` | Starting arena center, in meters, in the starting head space |
| `AURORA_XR_HUD_WIDTH` | 0.9 | HUD plane width in meters |
| `AURORA_XR_HUD_HEIGHT` | 0.55 | HUD plane height above the arena in meters |
| `AURORA_XR_HUD_BACKDROP` | 0 | Minimum HUD alpha, as a translucent panel behind it |
| `AURORA_XR_DUMP` | unset | Directory to write each stream's image once (PPM, plus alpha as PGM) |
| `MELEE_XR_STAGE_LAYERS` | `0xB` | Stage layers shown in 3D, as a bitmask |
| `MELEE_XR_PARTS` | unset | Overrides, e.g. `16:1,-16:1/12,-16:2/2.27` (`stage:part[/joint[.mesh]]`, `-` hides) |
| `MELEE_XR_STAGE_LOG` | unset | Log each stage part's id and layer once |
| `MELEE_XR_JUMBOTRON` | `0.55,0,-10,-75` | Pokémon Stadium big screen in 3D: `scale,x,y,z` (game units) for its base |
| `MELEE_XR_JOINT_LOG` | unset | Log each part's joints (index, depth, meshes, position) once |
| `AURORA_XR_DUMP_AFTER` | 300 | Stream frames to wait before `AURORA_XR_DUMP` writes |

## Measured: virtual screen only (Quest 3, 72 Hz, before 120 Hz lock-step)

| | Result |
|---|---|
| Display rate | 72/72 |
| Game frames on the screen | 60 per second |
| App GPU time (VrApi) | about 3.6 to 4.1 ms |

## Known gaps

- Leaving the game doesn't reopen the launcher panel yet.
- There's no recentering beyond the system's own.
- There's no controller haptics.
- The screen texture size is fixed at the first frame.
- Monado's `XR_FB_passthrough` layer crashes Monado, so desktop testing runs
  without passthrough.
