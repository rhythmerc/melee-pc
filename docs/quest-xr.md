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
  - **Under the fighters.** In mixed reality the HUD's quad layer goes
    under the 3D view, which is see-through wherever nothing drew, so
    fighters, items and effects passing over the HUD hide it. That works
    because no stage scenery reaches the HUD's height (below). Full VR
    draws the whole stage and its sky, so there the HUD stays on top.
    `AURORA_XR_HUD_ON_TOP=1` keeps it on top in mixed reality too.
  - **Off-screen fighters.** Past the flat camera's view, the game stops
    drawing a fighter and shows it in a magnifier bubble at the screen's
    edge. The 3D view sees the whole arena, so in XR the fighter keeps
    drawing, pulsing between 30% and 85% opacity (dithered, 1.5 times a
    second), and the bubble stays out of the HUD plane (flat frame only).
    The game's own test still runs and sets its flags, and the bubble's
    logic is untouched, so the off-screen damage tick is the same.
    `MELEE_XR_OFFSCREEN=0` keeps the game's behavior.
  - **Height per stage.** The HUD's bottom edge, where the damage meters are, sits about
    a fighter's height (`AURORA_XR_HUD_CLEARANCE`, 40 units) above the
    stage's highest floor. That's taken from the collision lines inside the
    blast zones on the fight's first frame (`aurora_xr_set_stage_top`).
    Before, it was a fixed 0.55 m above the arena center, and the meters
    covered top platforms (Battlefield, Yoshi's Story, Yoshi's Island).
  - **Previews.** With `AURORA_XR_DUMP`, `xr_layout.txt` holds the left
    eye's pose and field of view and the HUD quad's pose and size, so a dump
    can be composited with the HUD offline.
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
  - **Survey of 2026-10-01**, obvious splits only:
    - Fountain of Dreams: vortex, sea and rings.
    - Kongo Jungle: the jungle.
    - Corneria: the terrain.
    - Yoshi's Story: the cardboard sea and hills.
    - Great Bay: land, sky and mountains (the sea stays, because it hides
      underwater geometry).
    - Venom: the canyon.
    - Kongo Jungle 64: the jungle and sky.
    - Yoshi's Island: the sky.
    - Already fine: Temple, Brinstar Depths, Green Greens, Poké Floats,
      Dream Land, Yoshi's Island 64, Battlefield, Final Destination.
  - **Open questions** (left as they are):
    - Kongo Jungle: the waterfall shares a part with the plateau.
    - Rainbow Cruise and Icicle Mountain: scrolling stages.
    - Flat Zone: the Game & Watch frame.
    - Mute City and Big Blue: left for their own treatment.
  - **Second pass (2026-10-02), after the first headset look:**
    - Fountain of Dreams: the pole below the ornament is clipped away. The
      reflection isn't rendered in mixed reality: it was a flat-camera
      picture that couldn't line up in 3D, and it cost a whole extra scene
      render (`MELEE_XR_IZUMI_REFLECTION=1` keeps it).
    - Kongo Jungle: the waterfall is clipped below the plateau.
    - Corneria: centered on the Great Fox, and the coastline that scrolls
      past after about a minute is hidden.
    - Yoshi's Story: clipped below the Shy Guys' path.
    - Great Bay: the sea is shrunk to the stage's footprint and moved under
      it, and the turtle and the pier's stilts, rocks and screw are clipped
      at the waterline.
    - Yoshi's Island: the ground is clipped halfway down.
  - **Third pass (2026-10-02):**
    - Hard clips: Adreno 740 can't create multiview pipelines that write
      clip distances, so all clips use the discard variant (fade 0 is a
      hard cut).
    - Parts take up to two planes, for example below a height and behind a
      depth (`CLIP_BELOW` / `CLIP_BEHIND`, `MELEE_XR_CLIPZ`).
    - Kongo Jungle: the waterfall is also cut behind the stage.
    - Yoshi's Story: the cut is lower, so the Shy Guys' path shows.
    - Great Bay: the sea is moved back to the rear of the pier.
    - Fountain of Dreams: the reflection image is cleared to a water colour
      instead of left stale (`MELEE_XR_IZUMI_WATER`; the desktop build's
      BGRA framebuffer swaps red and blue in that copy).
  - **Fourth pass (2026-10-02):**
    - Parts take up to four planes, in any direction (`CLIP_LEFT`,
      `CLIP_RIGHT`, `CLIP_FRONT` join `CLIP_BELOW` / `CLIP_BEHIND`;
      `MELEE_XR_CLIPP` tries an arbitrary plane).
    - Kongo Jungle: only the front waterfall is left. The flat river behind
      it (part 4, joints 7 and 9) and the surface layers over it (joints 5,
      10, 26 and 35) are hidden, and the back cut moved to z −90.
    - Great Bay: the sea is bounded to the stage's footprint, x −320 to 220
      and in front at z 220.
    - Stage particles can be hidden by bank, or bank and id (see Particles
      below). Kongo Jungle hides its stage bank (30), whose particles are all
      river splashes that floated in the air once the river was gone.
  - **Jungle Japes (2026-10-02):**
    - Hidden: the jungle, sky and moon (part 4), and the birds far behind
      (part 5).
    - The river (part 6, on the far layer) is shown again and bounded to a
      rectangle just past the piers (x −125 to 125, z −100 to 45, 8-unit
      fade), like Great Bay's sea.
    - To check: the Klaptrap leaps from the river outside that rectangle.
  - **Peach's Castle (2026-10-03):** everything is in part 3.
    - Hidden: the red grounds (joint 8's own three meshes), the bridge and
      stairs (13), the hills, path and fence (25), the warp medallion (26),
      and the trees and hedge (children of 28).
    - The roof flags (65, 74, 83, 92, also under 28) are shown again by
      their own rules: a joint's rule overrides its parent's.
    - The castle body (14) stays whole under the roof, like a model on the
      table. A clip below the roof line would leave only the roof.
  - **Brinstar (2026-10-04):**
    - Hidden: the cave, with its walls, stalactites, pillars and chains
      (part 1).
    - The acid (part 8) fades in as it rises to just under the stage
      (`LevelRule`: its joint 1's height is the acid level, from about −250
      to 90, with the surface about 90 below it; hidden below −50, solid
      from −10, dithered in between through `aurora_xr_world_clips4_fade`).
      At its starting level it is far out of bounds. When shown, it's boxed
      to the stretch of its river around the stage (x −120 to 125, z −90 to
      70).
    - `MELEE_XR_LEVEL_LOG` logs a level rule's joint height while
      surveying.
  - **Onett (2026-10-04):** everything is in part 5. The town behind
    (joint 14) and the hills (23) are hidden, and the rest is boxed in by
    clip planes (x −140 to 150, z −70 to 70). That leaves the block you
    fight on: its lot, the road in front, and the clothesline between the
    poles over the right house, which has collision. The clothesline isn't
    one of the town joint's (8) meshes, so a box was simpler than mesh rules.
    Mesh rules now reach up to 256 meshes per joint (joint 8 has 100).
  - **Yoshi's Island (2026-10-04):** the clouds (part 1 joints 20-23,
    25-27 and 29, with the Bullet Bill and the sign) are pulled in from far
    behind and to the sides. They're shrunk to 0.3 about the middle of the
    cloud field, to just behind the stage, a little above the back pillars'
    trees (move rules). The back pillars' trees (joints 2-7) stay.
    - Still open: the tall slanted rock at the right. Its body is mesh 30 of
      joint 10, but more of it (a strip down its face, with the island's
      rock base) doesn't respond to mesh rules. A tilted clip took the left
      islands' trees with it.
    - Mesh rules now reach meshes 32-63 of a joint (a 64-bit mask). Before,
      `1 << 33` wrapped to mesh 1.
  - **Fourside (2026-10-08):** the city is all one joint (part 6 joint 2)
    and is hidden. That leaves the three buildings you fight on (joints 11,
    24 and 31 of the same part), whole down to their bases like a model on
    the table, at 0.8 like Onett. The crane (part 1) has collision and
    stays. The helicopter (part 3) and the UFO (part 5, a platform) are
    left as they are: both come in close above the buildings.
  - **Mushroom Kingdom (2026-10-09):** everything is in part 3. The sky
    and hills are their own joint (46) and are hidden. The ground's deep
    blocks are cut at y −40 with a 15-unit fade, leaving a slab under the
    floor. The castle walls, pipes (joint 2), and the mushroom poles and
    clouds just behind the stage (joint 32) stay. Scale 0.9.
  - **Mushroom Kingdom II (2026-10-09):** everything is in part 2. The
    clouds and trees behind (joint 8) are hidden. The side cliffs (joints 3
    and 5) are floor at y 21 from x ±52 out to ±304, well past the blast
    zones (±150), so they stay, boxed by clip planes at x ±150 and cut
    below y −50, which also cuts the waterfall and the pillars under the
    bridge. Scale 0.8, for the cliffs' width.
  - **Big Blue (2026-10-09):** the action stays put in game space (the
    Falcon Flyer, and the floors, which move only as cars come and go)
    while the track streams past, curving and banking. Hidden: the sky and
    clouds (part 2) and the corner brackets around the action (31). Boxed:
    the track (parts 34 and 36), the cars (33) and the craft fighters ride
    (32), solid out to the blast zones (x ±152) and dissolving over the 70
    units past them, so the track streams in and out of the box. The track
    and cars are also cut below y −60 (its pylons, cars thrown off it), and
    the track above y 150, where other stretches of it sweep overhead.
    Scale 0.85. A window onto the whole world, behind the box, is for later
    (also for Mute City, Rainbow Cruise and Venom).
  - **To revisit: Fountain of Dreams.** It runs badly on the Quest even
    without the reflection render. Profile it, then build a proper 3D
    reflection: mirror the world draws about the water plane per eye,
    clipped to above the water, under the translucent surface.
- **Guardian boundary.** In mixed reality the boundary is hidden
  (`XR_META_boundary_visibility`), since passthrough already shows the room.
  The runtime only allows that while a passthrough layer is shown, so full
  VR gets the boundary back. `AURORA_XR_BOUNDARY=1` keeps it in mixed
  reality too. It needs the `com.oculus.permission.BOUNDARY_VISIBILITY`
  permission in the manifest; without it the runtime doesn't offer the
  extension. With Guardian paused (`debug.oculus.guardian_pause`, for
  unattended runs) every request fails with `XR_ERROR_RUNTIME_FAILURE`.
- **Mixed reality vs full VR.** Every hide, clip and move applies only in
  mixed reality (`pc_xr_mixed_reality`). Full VR renders stages whole, and
  passthrough is stopped (`xrPassthroughPauseFB`, not just a missing layer).
  Per-stage centering applies in both. Full VR has had no work beyond every
  stage part rendering.
  - **Switching:** a gamepad's Select (SDL Back) switches between them at
    any time while XR is presenting (`pc_xr_toggle_mode`), instead of
    opening the port menu. That's handy for comparing a pruned stage with
    the original. `MELEE_XR_MODE=vr` starts in full VR.
- **Clip planes.** `aurora_xr_world_clips4` cuts the following world draws
  in the 3D view, keeping what's on the inside of up to four game-space
  planes. `xr_scene.c` sets them around a stage part (every `ClipRule` that
  names it), so fighters and items are never clipped. The cut is a fragment
  discard, not clip distances, because Adreno 740 fails to create multiview
  pipelines that write clip distances. Only clipped draws use the discarding
  pipeline variant (`RenderTargetLayout::xrSoftClip`), because discard costs
  early depth.
- **Soft clips.** A plane with a fade (`aurora_xr_world_clip_soft`, or a
  `ClipRule` fade above 0) dissolves geometry across a band inside the plane
  with a 4x4 ordered dither instead of cutting it. Passthrough shows through
  gradually and depth stays exact. Kongo Jungle's waterfall hangs below the
  plateau and fades out by the floating rock (y −35 to −70).
- **Particles.** `psDispParticles` asks `pc_xr_particle_begin` whether each
  particle stays in the 3D view. A `ParticleRule` names a stage and a
  particle bank, and optionally one id (ids number instances, so most rules
  take the whole bank). Hidden particles still draw into the flat frame.
- **Stage placement.** `aurora_xr_set_stage` names the stage, the game point
  at the arena position, and the stage's size (`s_placements`).
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
  counts and positions. `MELEE_XR_PTCL_LOG` lists each stage particle drawn,
  by bank and id.
- **Try rules:** with `MELEE_XR_PARTS`, `MELEE_XR_CLIP` / `MELEE_XR_CLIPZ` /
  `MELEE_XR_CLIPP`, `MELEE_XR_MOVE` and `MELEE_XR_PTCL`.
- **Capture the result:** `AURORA_XR_DUMP` writes the eye images,
  `AURORA_XR_DUMP_AFTER` picks the frame, and `AURORA_XR_ARENA_POS` and
  `AURORA_XR_ARENA_YAW` frame the view.

Checked on desktop against Monado (`AURORA_XR_DUMP` images):
- **Final Destination** (VS boot scene) and **Battlefield** (training boot
  scene) render in 3D at 60 frames per second.
- **The title screen** stays on the virtual screen. In XR it never times out
  into the attract demo, since a fight springing up unasked in the room is
  jarring, and A starts as well as Start, so a click on the screen (or bare
  hands) can get past it.

### Multiview

Both eyes draw in one pass. The 3D swapchain has one layer per eye, and the
replay pass has a view mask of `0b11`. Every world draw uses a twin pipeline:
- same GX config, multiview layout (`RenderTargetLayout::viewCount = 2`);
- its shader reads `@builtin(view_index)` and picks that eye's matrix from
  group 3.

The command processor resolves the twin when a world draw is recorded
(`gx::DrawData::xrPipeline`), and the replay binds it. Until a twin is
compiled, its draw is skipped. This needs the Dawn fork's multiview
(`ChromiumExperimentalMultiview`, Vulkan dynamic rendering).
`AURORA_XR_MULTIVIEW=0` goes back to replaying the eyes side by side, as
does MSAA.

On a Quest 3 (2026-10-01), Temple with 4 CPU players. Both runs are the
same unattended launch at the default arena scale, with only
`AURORA_XR_MULTIVIEW` changed:

| | Two passes | Multiview |
|---|---|---|
| World draws per frame | ~840 | ~420 |
| 3D eye pass | 14.5 to 15.3 ms | 10.4 to 10.7 ms (steady state) |
| Display | 76 to 85 of 120 fps | 94 to 102 of 120 fps |

That's about 30% off the eye pass. An earlier hand-played run had the stage
shrunk to minimum size, so it isn't comparable.

Resolution barely matters there. Same setup with multiview, eye scale 0.7
(half the pixels) against 1.0: 9.8 to 9.9 ms against 10.6 to 11.0 ms, about
8%. Temple's eye pass is limited by geometry, not pixels, so fixed foveated
rendering wouldn't help it much.

### Flat frame during fights

The flat frame nobody sees during a 3D fight no longer draws the world.
While the previous frame went to the headset as a 3D fight with the flat
present skipped, the normal frame leaves out world-tagged draws
(`set_xr_drop_flat_world`). Passes that an EFB copy or snapshot reads keep
them, so Pokémon Stadium's screen still shows the fight.
`AURORA_XR_FLAT_WORLD=1` keeps them all.

On a Quest 3 (2026-10-01), Temple with 4 CPU players, controlled runs at the
default arena size with multiview:

| | Flat world drawn | Flat world dropped |
|---|---|---|
| Display | 88 to 100 of 120 fps, 50 to 64 stale per second | 114 to 121 of 120, 0 to 22 stale |

The eye pass's own time doesn't change. Dropping the world from the flat
frame frees the GPU time it took on top of the eyes.

Two gaps let world geometry back into the flat frame (fixed 2026-10-03):

- **Parts hidden from 3D.** Mixed reality's hidden parts, joints and
  particles were tagged `AURORA_XR_MONO`, which is never dropped, so every
  pruned part was still drawn flat on every frame. They're now
  `AURORA_XR_HIDDEN`. That tag is never replayed in 3D, and it's dropped
  from the flat frame along with the world unless something reads the pass.
  Pokémon Stadium's screen feed still shows the fight and its background
  (checked with `AURORA_XR_DUMP`).
- **Missed 3D images.** When the 3D swapchain had no free image, the next
  flat frame drew the whole world again. That made the next frame heavier
  and the next miss likelier, so slow stages kept sliding. A missed fight
  frame now drops the flat world too.

ovrgpuprofiler showed the cost on Fountain of Dreams: a flat 584x480 pass
(the widescreen EFB) with 7 to 9 ms of binning on nearly every frame, more
vertex work than the eye pass. Quest 3, mixed reality, four CPUs, game
frames per second (of 60):

| Stage | Before | After |
|---|---|---|
| Fountain of Dreams | 22.6 | 51.3 |
| Jungle Japes | 50.9 | 57.9 |
| Pokémon Stadium | 44.9 | 47.8 |
| Brinstar | 44.2 | 46.7 |
| Battlefield, Peach's Castle, Corneria | 59.1 to 59.6 | 59.1 to 59.7 |

`AURORA_XR_FLAT_LOG=1` reports every 10 s the flat passes that kept world or
hidden draws because something reads them, what reads them, and the resolve
rect sizes.

Pokémon Stadium's jumbotron reads the flat frame itself: in its feed mode
(display state 7, a 584x406 grab) and its player zoom (state 8, about
113x80), each grab keeps that frame's whole flat world, background
included. That was about 4 ms of GPU work per grab (1.7 ms binning, 2.4 ms
render) against 7.6 ms for the eye pass. The screen's other modes grab
nothing.

- **Half-rate grabs.** In mixed reality the feed and zoom grab on every other
  rendered frame that asks for one, and the screen holds the last image in
  between (`MELEE_XR_PS_GRAB_EVERY=<n>`, 1 for every time). The count lives
  in the draw callback, not on simulation ticks: under load several ticks
  run per rendered frame, so tick parity still grabbed almost every rendered
  frame. The simulation isn't touched, so netplay with non-XR builds is
  unaffected.
- **Clipped grabs.** A flat pass kept only for an EFB copy clips its world
  and hidden draws to the copied rect (`AURORA_XR_FLAT_CLIP=0` draws them
  whole). The zoom's 113x80 corner then costs little more than its
  geometry.
- **Repeatable runs.** `MELEE_PS_TEST_FORM=<5|3|4|6|9>` (default, fire,
  grass, rock, water) pins a transformation, and `MELEE_PS_TEST_SCREEN=<n>`
  pins the jumbotron state. Both change what the stage draws from the RNG,
  so they're test fixtures only, never for netplay.

Quest 3, mixed reality, four CPUs, 40 s per run, game fps (of 60), feed at
half rate and zoom clipped:

| Form | Feed | Zoom | Eye pass |
|---|---|---|---|
| Default | 50.2 | 56.6 | 10.2 ms |
| Grass | 48.1 | 54.9 | 10.2 ms |
| Water | 40.0 | 47.0 | 12.6 ms |
| Fire | 40.4 | 41.5 | 12.6 ms |
| Rock | 38.4 | 41.1 | 13.2 ms |

With `MELEE_XR_PS_ZOOM_ONLY=1`, mixed reality shows a player zoom in place
of the feed. It's off by default since positions are decoded on the CPU
(below), which left it nothing to gain. The screen's states and their RNG
draws are the game's own, and only the grab and the screen's image change,
in the draw callbacks. The zoom takes turns between the fighters every three
seconds of the feed. Feed-state runs went from 50.2 / 48.1 / 40.0 / 40.4 /
38.4 to 54.4 / 50.0 / 43.6 / 47.4 / 46.6 (default, grass, water, fire, rock).

Particles cost 1 to 1.5 ms of the eye pass in the heavy forms
(`MELEE_XR_PTCL_TEST_HIDE=1` hides them all): fire 47.8 -> 52.6 fps with
none, rock 41.4 -> 44.4, water 42.6 -> 44.9. Halving the eye pixels
(`AURORA_XR_EYE_SCALE=0.7`) changed nothing measurable. GPU counters in rock
form show 38% vertex fetch stall with the shaders busy only 39% of the time.
Aurora's shaders pull vertices from storage buffers
(`vbuf`/`abuf: array<u32>`, decoded in the shader), so the eye pass looks
limited by vertex fetch, not by pixels.

### MSAA on the 3D eyes

`AURORA_XR_MSAA=<1|2|4>` multisamples the 3D eyes alone (4 by default on
Android); the flat frame keeps its own sample count. The eyes keep multiview and the direct path:
color, the extra attachments and depth are multisampled, transient where
the device allows (`TransientAttachments`, so on a tiler they stay in tile
memory), and the pass resolves into the shared image and discards the
samples. The coverage draws that write passthrough alpha run multisampled
too, so the resolved alpha softens silhouettes against the room.

This needs the Dawn fork's multiview resolve: a resolve target with a
layer per view, like its attachment (`CommandEncoder.cpp` validation and
the lazy clear in `CommandBuffer.cpp`).

Quest 3, mixed reality, four CPUs, warm pipeline cache, game fps (app GPU
per display frame), off -> 4x: Battlefield 59.9 -> 59.9 (3.35 -> 4.01 ms),
Fountain of Dreams 58.9 -> 59.8 (4.04 -> 4.22 ms), Pokémon Stadium rock
59.3 -> 58.4 (4.20 -> 4.52 ms; stale frames 1.5 -> 4.5 per second).

After a build that invalidates the pipeline cache (a new Dawn), the first
runs compile thousands of pipelines for over a minute and are CPU bound;
`quest_perf.py` flags them ("pipeline cache cold").

### Dynamic resolution

Dynamic resolution (multiview only; on by default on Android,
`AURORA_XR_DYNRES=0|1` overrides) makes the stereo swapchain at the
largest scale (`AURORA_XR_DYNRES_MAX`, 1.3) and renders each frame into its
top-left corner at a scale of the runtime's recommended eye size, between
`AURORA_XR_DYNRES_MIN` (0.8) and the maximum. The pass uses Dawn's
`RenderPassRenderAreaRect`, so tiles outside the corner are neither cleared,
resolved nor stored, and the projection layer's `imageRect` tells the
compositor which corner to stretch over the field of view. The Dawn fork
lets a partial render area survive attachments that are cleared and
discarded (the transient MSAA color and depth), and the stereo image is
begun as initialized, so Dawn doesn't fall back to the full area.

The controller follows common dynamic resolution practice:

- **Signal:** the whole frame's GPU time, from the first flat pass to the
  last XR pass (timestamps, compositor preemptions included), so the
  jumbotron's grab or anything else in the frame counts. Timing the 3D pass
  alone read differently per stage.
- **Model:** frame time = fixed + perPixel × scale², and the largest scale
  predicted to fit `AURORA_XR_DYNRES_TARGET_MS` (12 ms per game frame; 13.5
  filled the GPU enough for the XR thread to miss submits).
- **Steps:** down at once, up 0.01 every three frames.
- **Panic:** a missed swapchain image cuts 10%, unless the frame's GPU time
  was under `AURORA_XR_DYNRES_LOAD_MS` (9 ms). Then the CPU missed, and
  only three within a second count.
- **Testing:** `AURORA_XR_DYNRES_RANDOM=1` picks a random scale every frame.

Quest 3, mixed reality, 4x MSAA, game fps: Battlefield with two CPUs 60.0
at 1.30 throughout; Fountain of Dreams with four 59.6 at 0.83-0.90;
Pokémon Stadium rock with four 59.0 at 0.81-0.83 (55.5 at a fixed 1.0).
Fixed scales on the reference stages (2 / 4 CPUs): two-player fights hold
60 at 1.3 everywhere; with four, Battlefield holds 1.3, Fountain 1.15 and
Stadium rock not even 1.0.

`MELEE_DEBUG_VS=cpu2` is the two-CPU fixture, and
`tools/quest_perf.py --players 2`.

### Fixed-latency presentation

Under lock-step pacing, each game frame starts on a tick, and its images
were released as soon as they were ready. They finish right around a
display-frame boundary, so about a third of them alternated between being
shown three display frames and one instead of two and two. That's 17 to 21
such pairs a second on Battlefield, invisible to every frame-rate
statistic, since the average stayed at 60.

Each frame packet now carries the display frame of the tick that started it
(`FramePacket::xrTickFrame`, set from `aurora_xr_pace`), and the XR thread
releases its 3D and HUD images `latency` display frames after the tick.
`latency` is the smallest number of display frames by which 97% of recent
3D images were ready, at most a game frame's display frames plus one. It's on with dynamic resolution, which keeps frames on
time: without it, frames running late on an overloaded stage were held into
slots the render worker then lacked (Pokémon Stadium rock with four CPUs,
58.2 -> 55.5 game fps). `AURORA_XR_FIXED_LATENCY=0` turns it off, `=<n>`
forces it on at n.
On Battlefield it settles at 2 display frames, no later than before, and
every image is shown exactly two display frames (600 of 600 per 10 s,
against 188 to 480 without it). The XR log line reports the histogram and
the latency.

### CPU level

Light scenes let the CPU drop to its lowest clock, and the XR thread then
missed its 120 Hz submits: two-player Battlefield showed 6 to 7 stale frames
a second at CPU level 2. A sustained-high CPU request
(`XR_EXT_performance_settings`, still allowed with passthrough) holds level
3 and brought that to 0.2. It's the default on Android
(`AURORA_XR_PERF_CPU=off|low|high|boost`; `AURORA_XR_PERF_GPU` stays the
runtime's own).

### Decoded positions

Aurora's vertex shaders decode the game's GX vertex data themselves: an
index from the raw stream, then the attribute from its array, byte by byte,
byte-swapped and converted from fixed point, each word behind a bounds
check. On a Quest 3 the rock form's eye pass spent 61% of its binning (38k
vertices in 2.7 ms) and 57% of its per-bin render stalled on vertex fetch,
with the shaders busy 17 to 26% of the time (ovrgpuprofiler render-stage
metrics). A tiler runs the vertex shader in binning and again per bin.

`AURORA_POS_DECODE` decodes positions to float3 on the CPU as each draw is
recorded, from the same `AttrConfig` the shader generator reads, into the
storage pool. The pipeline takes them as vertex attribute 0
(`ShaderConfig::decodedPos`, a spare bit, so the persisted pipeline cache
stays valid), and the GPU fetches them in hardware. A merged draw merges
only while its decoded range can grow in place. Normals, colors and texture
coordinates still come through the shader. `AURORA_VTX_DECODE` decodes every
attribute into one interleaved vertex (`decoded_layout`: matrix indices as
u32, positions, normals, binormals and tangents as float3, colors as float4
so 565/4444/6666 stay exact, texture coordinates as float2), and a draw that
needs more than 16 vertex inputs falls back to positions only. Both are on
by default on Android (`AURORA_VTX_DECODE=0` leaves positions only,
`AURORA_POS_DECODE=0` with it neither).

Quest 3, mixed reality, four CPUs, game fps, decode off -> on:

| Stage | Off | On |
|---|---|---|
| Fountain of Dreams | 47.8 | 59.9 |
| Brinstar | 46.6 | 59.8 |
| Jungle Japes | 54.7 | 59.9 |
| Battlefield | 59.5 | 59.7 |
| Pokémon Stadium, rock (zoom only) | 41.5 | 58.6 |

Pokémon Stadium's full feed with positions decoded: default 59.9, grass
59.5, water 57.6, fire 57.2, rock 58.3. With every attribute decoded, water
58.8, fire 59.6, rock 59.7, about 2 stale frames per second, the app's GPU
time 4.2 ms per display frame in each, and CPU use 0.34 -> 0.41. The eye
pass on rock went from 13.2 ms (no decode) to 8.7 (positions) to 7.3
(everything).

Fire and rock are limited by their own scenery (360 to 380 world draws),
not by the jumbotron. In fire form the zoom went from 30.1 to 35.1 fps with
clipping, and the feed from 27.7 (every frame) to 41.3 (every other) and
43.3 (every third). The eye pass depends on what the headset faces, so only
compare runs taken from the same pose.

Also in the traces: the compositor preempts the app's GPU work about 175
times a second, at about 1 ms each, under passthrough at 120 Hz.

Requesting sustained-high clocks (`XR_EXT_performance_settings`) succeeded
but left the GPU at level 2 (640 MHz) with no measurable change, so it's
off by default (`AURORA_XR_PERF_GPU` / `AURORA_XR_PERF_CPU` = `low`, `high`,
`boost` to try).

That's Horizon OS's passthrough cap: on a Quest 3, GPU levels 3 and 4 and
CPU level 4 are only offered while the app isn't using passthrough
([CPU and GPU levels](https://developers.meta.com/horizon/documentation/unity/os-cpu-gpu-levels/)).
Mixed reality runs at GPU level 2 whatever we ask for. Full VR pauses
passthrough, and the runtime then raises the GPU to level 4 under the same
load (Battlefield, four CPUs, 2026-10-03). That accounts for full VR being
smoother.

### Profiling on the headset

`tools/quest_perf.py run --stage 31 --phases mr:40,vr:40` boots a four-CPU
match on a stage with no one wearing the headset (`prox_close`), holds each
phase in the same session, and summarizes the runtime's `VrApi` line for
each one: fps, stale frames, CPU and GPU levels and clocks, GPU and CPU
utilization, and app GPU time. It also includes our own `display fps` and
`AURORA_XR_TIMING` lines. `--env K=V` adds knobs, and logs go to
`build/quest-perf/`. The script writes the game's knobs to `melee-env.txt`
and takes the disc from the launcher's `launcher.cfg`.

- `MELEE_XR_CONTROL=<file>`: once a second, the game reads `mr` or `vr`
  from the file and switches when it changes (`quest_perf.py mode vr`).
- `AURORA_XR_HANDS=0`: leaves `XR_EXT_hand_tracking` off.
- On the headset already: OVR Metrics Tool
  (`com.oculus.ovrmonitormetricsservice`; overlay and CSV toggled by
  `am broadcast`), `perfetto`, and `ovrgpuprofiler` (live GPU counters,
  render-stage traces).
- **Per-surface GPU cost:** `adb shell ovrgpuprofiler -e` before launching
  the game, then `ovrgpuprofiler -t 0.5`. It splits each render target into
  binning (vertices), render (pixels) and preemption. Perfetto's surface
  slices include preemption in their duration.
- **System timeline:** `adb shell perfetto -c - --txt -o
  /data/misc/perfetto-traces/t.pftrace < tools/perfetto/quest.cfg`, then
  pull it and open it in ui.perfetto.dev, or query it with trace_processor.

### Placing the arena

Each stage starts placed by its own entry in `xr_scene.c`
(`s_placements`): the game point that sits at the arena position, which is
its main floor centered on where the fight happens (Corneria's Great Fox,
Peach's Castle's roof), and its size against the default arena scale.
Sizes meet halfway between the game's proportions and making every stage's
floors as wide as Final Destination's, `sqrt(171 / floor width)` rounded
(Fountain of Dreams at 1.15), except Temple, tuned down to 0.55 to fit in
view. Moving stages aren't listed and sit at the world origin at scale 1.

Pause the fight to move the arena. Put a controller down and that hand is
tracked instead (`XR_EXT_hand_tracking`), and a pinch does what the grip
does. Hands and controllers work the same way and can be mixed. While paused
the grips grab instead of pressing Z. In some modes Z on the pause screen
retries the match, so the grips don't pass it through while paused.
Triggers, A and Start work as usual, so L+R+A+Start still quits.

Each hand points a laser at the arena. A controller's laser comes out of
its tip. A tracked hand's runs from an estimated shoulder through the index
knuckle, so it holds still while you pinch. Once the touch point (the
controller's tip, or the pinch) is inside the arena's grab box, a bit larger
than the stage and centered on the arena center (Corneria's is the Great
Fox), the laser gives way to a dot there.

- **One hand.** Squeeze or pinch with the laser on the arena, or inside the
  box. The arena hangs off the laser at the point you grabbed, or off the
  touch point, and keeps its heading. Let go and it stays there.
- **Two hands.** While holding with one hand, squeeze or pinch with the
  other, anywhere. Spreading or closing your hands scales the arena, and
  turning them turns it about the vertical axis, around the point between
  the touch points. Moving both hands carries the arena along with that
  point. Letting go of one hand carries on with the other alone.

The HUD moves, turns and scales with the arena. It follows your resizes but
not the stage's own size. Placing the arena places only the stage you're
playing: other stages keep their own placement, and this one comes back
where you left it the next time it's played. The placements last for the
session, not across launches. Each is logged when you let go (`Arena for
stage N placed at x,y,z, yaw, scale`, the scale without the stage's own),
which is handy for tuning every stage's default with `AURORA_XR_ARENA_POS`
and `AURORA_XR_ARENA_SCALE` in `melee-env.txt`.

#### Before every fight

Every fight in XR starts with the game waiting on its first frame, so you
can place the stage before "Ready... GO!"
(`src/pc/xr_place.c`). The hands grab the arena the same way as in a pause.
A or Start begins the fight, B puts the stage back at its default placement, and Y
shows or hides the how-to cards. The cards float above where the arena
starts; the button legend sits just below its front edge. The cards open
with every hold. The HUD is hidden for the whole hold, cards open or not.
Cards and legend are softly locked to the head, sideways only: once the
head turns more than 20° away from them they swing around after it (about
where the head stood when the hold began) and settle when they catch up;
their height doesn't follow. The stage starts where you last left it this
session, so A at once keeps that. Every fight is held, the same stage twice
in a row included (`pc_xr_stage_load`, from the stage's load in
`gr/ground.c`), but only when the headset is already presenting as the
fight opens: a build without XR, or
an XR build playing on the flat window, never waits (the hook compiles to
`false` without `AURORA_ENABLE_OPENXR`). `MELEE_XR_PLACE=0` turns the hold
off.

The hold isn't Melee's pause, which is game state. The scene loop
(`gm_801A4D34`) keeps drawing every frame but runs no simulation ticks, and
the pad input that comes in is dropped, so no game state changes and no
frame passes. A fight that is being recorded or replayed is unaffected, and
the button that ends the hold counts on its release, so the fight never
starts with it held. Netplay never holds. The peer starts the match on the
agreed frame, whether it is this build, VR or not, so placement stays local
presentation: it never touches the simulation, the wire, or tick timing.
In netplay the stage starts at its default placement.

Four cards: Move, Turn, Scale and Ready? (the buttons). There are two
sets, and the hold shows the one for how you're holding things: tracked
hands (a controller put down; the legend says PINCH) or Touch controllers
(the legend says GRIP). Each set's gesture cards play looping clips of
ghost hands, or ghost controllers, placing Battlefield.

`tools/xr_cards/make_cards.py` draws them all in Melee's menu style into
`resources/xr/`. The clips are cut from headset recordings made on the `xr-recording`
branch, which adds a chroma-green backdrop and ghost hands and
controllers (`AURORA_XR_CHROMA=0,255,0`, `AURORA_XR_GHOST_HANDS=1`,
`AURORA_XR_PLACE_PICTURES=0` in `melee-env.txt`). Trimmed and cropped,
they're kept in `tools/xr_cards/footage/<set>-<gesture>.mp4`, where the
set is `hands` or `controllers`. The script keys out the green (in the
hand takes it first paints out the HUD's stock icons and "Ready"
leftovers, which were recorded before that branch hid the HUD),
composites each clip over the cards' grid, and packs its frames into
`place-clip-<set>-<gesture>.jpg`. `place-clips.txt` gives each clip's
frame layout, its rate, and where it sits on its set's cards. The Ready
card's still comes from a desktop capture in `tools/xr_cards/shots/`
(`AURORA_XR_DUMP`, left eye). The
headset plays each clip forward and then back, so a clip needs to show
only one direction of a gesture. Rerun the script after changing any of
these.

### Pointing at the screen

The virtual screen (menus, character and stage select, results) works like
a Quest window. Its lasers only appear while they point at it. While a laser
is on it, the grips don't press Z and the triggers don't press L or R.

- **Pointing.** A laser on the picture is a pointer. The main menus (1P,
  VS., Trophies, Options, Data and their lists) highlight the option under
  it, and the character and stage select cursors follow it. A trigger pull or
  a pinch clicks, which holds A while it's held. On those menus a click on
  nothing presses nothing. Every other menu (rules, name entry, results) takes
  a click as plain A, wherever it lands.
- **The pad still steers.** Moving a stick or the D-pad takes the lead from
  the pointer, so a laser resting on the screen doesn't fight it. The
  pointer takes the lead back once it moves away (8 logical pixels) or
  clicks.
- **Netplay.** The pointer is off there, because those menus run on the pads
  both peers exchange. Its clicks only ever reach the game as port 1's A,
  and the highlight it moves is menu state that no fight reads.

The bar under the screen is its handle:

- **One hand.** Squeeze, pull the trigger, or pinch with the laser on the bar,
  or touch the bar. The screen hangs off the laser or the touch point. While a
  controller drags it along its laser, that controller's stick pushes it
  away or pulls it in, instead of steering the menu.
- **Two hands.** While one hand holds the bar, the other joins by pressing
  with its laser anywhere on the screen. Spreading or closing the hands
  resizes the screen about its center, and moving them carries it along.

The bar is faint until a laser or hand is on it, then bright, then cyan while
the screen is held.

The game side is `src/pc/xr_pointer.c`, with a hook in each menu that
follows the pointer: `mnmain.c`, `mncharsel.c` and `mnstagesel.c`. The main
menus place each option's joint through the menu camera
(`pc_xr_pointer_project`). The two select screens put their cursor where
the pointer meets the cursor's plane (`pc_xr_pointer_unproject`).
`MELEE_POINTER_MOUSE=1` drives the pointer with the mouse in the window, so
these hooks can be tried flat on a desktop.

The screen always turns to face the head. Its pose is separate from the
arena's: moving or resizing one leaves the other alone. It lasts for the
session and is logged when you let go (`Screen placed at x,y,z, width w m`),
for `AURORA_XR_SCREEN_*`.

The lasers and dots are quad layers with static textures, drawn at display
rate from the latest controller and hand poses. Coloring them needs
`XR_KHR_composition_layer_color_scale_bias`. A laser or dot is cyan on the
arena, amber while grabbing, and faint white otherwise. A pinch closes when
the thumb and index tips come within 2 cm and opens past 3.5 cm. A tracked hand
whose palm turns toward the face isn't pointing, as on the Quest's own menus.
Its laser goes, and its pinches do nothing, until the palm turns away again,
unless that hand is already holding or clicking something. The arena moves at
the game's frame rate.

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
  timestamps) every 10 s.
- **Since then:** the single-device path (below) removed the bridge copies
  altogether.

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
- **Stage parts.** The static stages have had four passes (see Stage
  backgrounds). The open questions listed there are still open, and the
  moving stages haven't been looked at.
- **Full VR mode for fights (pinned).** An option for fights in full VR, not
  over passthrough, alongside the mixed-reality arena.

- **Background sparkles** (Battlefield's twinkles, Final Destination's
  stars) still show in mixed reality. That's kept on purpose: they look good
  over the room.
- **Fog** still uses the eye's depth instead of the game camera's.
- **Billboards and particles** face the game camera, not the eye.
- **Frame rate.** The 3D view updates at the game's 60 Hz. Every second
  display frame reuses the previous image and relies on the runtime's
  reprojection; re-rendering it with the newer head pose isn't done.
- **Placement.** It isn't saved between sessions, and there's no table or
  floor anchoring.
- **Coverage.** Only the static stages have been looked at.

## How the frame gets to the headset

There's one Vulkan device, and Dawn renders straight into OpenXR's
swapchain images. Stock Dawn can't do this. It creates its own Vulkan
instance and device and has no way to render into images it didn't create.
The XR build therefore uses Dawn built from a fork: `encounter/dawn` at
aurora's pinned `AURORA_DAWN_REF`, plus the `melee-xr` patch. The patch adds
three things to `dawn/native/VulkanBackend.h`:

- **`SetExternalVulkanHooks`.** Dawn creates its `VkInstance` and `VkDevice`
  through `xrCreateVulkanInstanceKHR` and `xrCreateVulkanDeviceKHR`
  (`XR_KHR_vulkan_enable2`). Only the physical device the runtime names is
  offered as an adapter. One extra queue is created in Dawn's queue family
  for the runtime.
- **`GetDeviceVkHandles` and `GetExtraQueue`.** These give the session's
  graphics binding Dawn's instance, device and the spare queue.
- **`CreateSharedTextureMemoryFromVkImage`.** This wraps each swapchain image
  as Dawn shared texture memory. Dawn never destroys the image.

Per frame:

1. The XR thread acquires a swapchain image and waits on it, ahead of the
   render worker. OpenXR allows one waited image per swapchain at a time.
2. The render worker begins access to it, draws into it, and ends access.
   Ending access exports a semaphore (sync FD on Quest, opaque FD on
   Linux).
3. The XR thread waits on that semaphore on its own queue. It adds a barrier
   back to `COLOR_ATTACHMENT_OPTIMAL` if Dawn left another layout, then
   releases images in the order it acquired them.

There are no copies. Dawn submits on queue 0, and the XR thread and runtime
use queue 1, so neither needs a lock around the other.

The game's frames are already sRGB-encoded bytes. The swapchains are sRGB
and created mutable-format, and Dawn sees each image as the matching UNORM
format, so it stores the bytes unchanged and the compositor decodes them.

`prepare_device` (from `webgpu::initialize`, before the adapter is
requested) creates the OpenXR instance and system and sets the hooks. The
session is created later on the XR thread, on Dawn's device. The OpenXR
instance is destroyed after WebGPU shuts down, because it created Dawn's
device.

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

`MELEE_XR=1` builds Dawn from source, from the fork at
[rhythmerc/dawn](https://github.com/rhythmerc/dawn), branch `melee-xr`.
- **Which checkout:** `MELEE_DAWN_SOURCE` if set, else `~/projects/dawn` if
  it exists, else the pinned commit (`MELEE_DAWN_REF`), cloned into
  `build/dawn-src` with its dependencies.
- **Your own checkout:** fetch its dependencies first with
  `python3 tools/fetch_dawn_dependencies.py`.
- **Build settings:** Dawn is built with C++ modules and protobuf turned off.

Desktop testing against Monado, with the same fork:

```sh
cmake -B build/linux-xr-fork -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAURORA_ENABLE_OPENXR=ON \
  -DAURORA_SDL3_PROVIDER=vendor -DAURORA_DAWN_PROVIDER=vendor -DFETCHCONTENT_SOURCE_DIR_DAWN=$HOME/projects/dawn \
  -DDAWN_SUPPORTS_CXX_MODULES=OFF
AURORA_XR=1 build/linux-xr-fork/melee disc.rvz
```

## Controls (Touch controllers, port 1)

| Touch | GameCube |
|---|---|
| Left stick | Control stick |
| Right stick | C-stick |
| A / B | A / B |
| X / Y | X / Y |
| Triggers | Analog L / R, digital past 90% |
| Either grip | Z (grabs the arena while paused, or the screen's bar with a laser on it; with hands, pinch) |
| Either trigger, laser on the screen | Clicks (A) where it points, instead of L / R |
| Left menu button | Start |

While a stage waits before a fight, A or Start begins it, B resets
the stage's placement, and Y shows or hides the how-to cards.

An external gamepad plays as player one, alongside the headset's
controllers, so it can also play netplay: the newest one connected takes
port 1, and when port 1 has none, the first connected moves there. Its
Select switches mixed reality and full VR instead of opening the port menu.

## Knobs

On Quest, set these in `/sdcard/Android/data/dev.melee.game/files/melee-env.txt`.

| Variable | Default | Effect |
|---|---|---|
| `AURORA_XR` | set by `MeleeXrActivity`; off otherwise | Present to the headset |
| `AURORA_XR_PASSTHROUGH` | 1 | Passthrough behind the screen |
| `AURORA_XR_BOUNDARY` | 0 | 1 keeps the Guardian boundary in mixed reality (by default it's hidden while passthrough shows the room) |
| `AURORA_XR_SCREEN_WIDTH` | 0.7 | Starting screen width in meters (grab with two hands to change) |
| `AURORA_XR_SCREEN_DISTANCE` | 0.85 | Meters in front of the starting head position |
| `AURORA_XR_SCREEN_Y` | 0 | Height offset in meters |
| `AURORA_XR_SCREEN_HEIGHT` | 1080 | Screen texture height in pixels |
| `AURORA_XR_3D` | 1 | 3D fights. Set 0 to keep fights on the virtual screen |
| `AURORA_XR_EYE_SCALE` | 1.0 | Eye resolution, as a fraction of the runtime's recommendation |
| `AURORA_XR_REFRESH` | unset | Display rate to request if offered (otherwise 60, then 120) |
| `AURORA_XR_LOCKSTEP` | 1 | Pace the game to the display when it runs at a multiple of 60 Hz |
| `AURORA_XR_FLAT_WORLD` | 0 | Keep world draws in the unseen flat frame during 3D fights |
| `AURORA_XR_PERF_GPU`, `AURORA_XR_PERF_CPU` | unset | Request `low`, `high` or `boost` clocks (XR_EXT_performance_settings) |
| `AURORA_XR_MULTIVIEW` | 1 | Both eyes in one pass (multiview) when the device supports it |
| `AURORA_XR_ONE_EYE` | 0 | Measurement: replay the left eye only (side-by-side path) |
| `AURORA_XR_DIRECT` | 1 | Render both eyes straight into the shared 3D image when possible |
| `AURORA_XR_FIGHT_SCREEN` | 0 | Keep presenting the flat screen during fights (debugging) |
| `AURORA_XR_HUD_SCALE` | 0.5 | HUD texture resolution, relative to the screen |
| `AURORA_XR_TIMING` | 1 | Log GPU pass times every 10 s |
| `AURORA_PIPELINE_INLINE` | 0 | Compile pipelines on the render thread instead of the compile thread |
| `AURORA_XR_ARENA_SCALE` | 0.0035 | Starting meters per game unit, times each stage's own size (grab with two hands to change) |
| `AURORA_XR_ARENA_YAW` | 0 | Starting arena turn in degrees (counter-clockwise from above) |
| `AURORA_XR_ARENA_POS` | `0,-0.25,-0.7` | Starting arena center, in meters, in the starting head space |
| `AURORA_XR_HUD_WIDTH` | 0.5 | HUD plane width in meters |
| `AURORA_XR_HUD_HEIGHT` | 0.3 | Lowest HUD plane height (center) above the arena in meters; set, it's fixed there |
| `AURORA_XR_HUD_CLEARANCE` | 40 | Game units between the stage's highest floor and the HUD's bottom edge |
| `AURORA_XR_HUD_BACKDROP` | 0 | Minimum HUD alpha, as a translucent panel behind it |
| `MELEE_POINTER_MOUSE` | 0 | 1: the mouse in the window is the screen's pointer (testing the menu hooks flat) |
| `AURORA_XR_HUD_ON_TOP` | 0 | 1: the HUD draws over the fighters in mixed reality too (it always does in full VR) |
| `AURORA_XR_DUMP` | unset | Directory to write each stream's image once (PPM, plus alpha as PGM) |
| `MELEE_XR_STAGE_LAYERS` | `0xB` | Stage layers shown in 3D, as a bitmask |
| `MELEE_XR_PARTS` | unset | Overrides, e.g. `16:1,-16:1/12,-16:2/2.27` (`stage:part[/joint[.mesh]]`, `-` hides) |
| `MELEE_XR_STAGE_LOG` | unset | Log each stage part's id and layer once |
| `MELEE_XR_JUMBOTRON` | `0.55,0,-10,-75` | Pokémon Stadium big screen in 3D: `scale,x,y,z` (game units) for its base |
| `MELEE_XR_MODE` | mixed reality | `vr`: start in full VR (no stage hides, clips or moves, passthrough off); Select switches |
| `MELEE_XR_CLIP` | unset | Try clips: `grkind:part:y[:fade],...` cuts that part below y, fading across `fade` units |
| `MELEE_XR_MOVE` | unset | Try moves: `gk:part:joint:scale:px:py:pz:tx:ty:tz;...` |
| `MELEE_XR_CENTER` | per stage | Arena center override, `x,y,z` game units |
| `MELEE_XR_STAGE_SCALE` | per stage | The stage's size against the arena scale (1: as Final Destination) |
| `MELEE_XR_IZUMI_REFLECTION` | unset | Keep Fountain of Dreams' reflection render in mixed reality |
| `MELEE_XR_IZUMI_WATER` | `60,100,160` | Fountain water colour in mixed reality (looks red/blue swapped in the desktop build) |
| `MELEE_XR_CLIPZ` | unset | Try back cuts: `grkind:part:z[:fade],...` cuts that part behind z |
| `MELEE_XR_LEVEL_FADE` | unset | Override every level rule's fade band, `min,full` (surveying) |
| `MELEE_XR_LEVEL_LOG` | unset | Log the height of each level rule's joint (`LevelRule`) about twice a second |
| `MELEE_XR_JOINT_LOG` | unset | Log each part's joints (index, depth, meshes, position) once |
| `MELEE_XR_CLIPP` | unset | Try any plane: `grkind:part:a:b:c:d[:fade];...` keeps where ax+by+cz+d ≥ 0. Any env plane replaces that part's built-in ones (up to 4) |
| `MELEE_XR_CLIP_LOG` | unset | Log the first 40 stage part begins (stage, part, layer) |
| `MELEE_XR_PTCL` | unset | Hide more stage particles: `grkind:bank:id,...` (id -1 is the whole bank) |
| `MELEE_XR_PTCL_LOG` | unset | Log each stage particle drawn once (bank, id, position) |
| `MELEE_XR_PLACE` | 1 | `0`: never hold a fight for placing its stage |
| `AURORA_XR_PLACE_CARDS` | `0,0.42,-0.3,0.85` | How-to cards: `dx,dy,dz,width` (meters) from where the arena starts |
| `AURORA_XR_PLACE_LEGEND` | `0,-0.17,0.12,0.34` | Button legend while placing, the same way |
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
