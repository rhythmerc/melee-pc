# XR: 3D fights over passthrough (plan)

Status: the core is implemented. See docs/quest-xr.md, "3D fights", for
what works and what's open. Notes added during implementation:

- The default layer rule here was wrong. Layer 1 is part of the stage
  itself (Final Destination's main platform is parts 1 and 3 on layer 1).
  Only layer 2 is the far background. The default now hides only layer 2.
- The 3D view is drawn per game frame with the latest predicted head pose.
  Re-encoding at display rate (step 6) is still open.

**Goal.** During a fight, the stage and fighters render in stereo 3D, anchored
in the room over passthrough. The HUD sits on a flat plane. Stage backgrounds
are hidden except for parts chosen per stage, such as the Pokémon Stadium
screen. Menus and other non-fight scenes stay on the existing virtual screen
(docs/quest-xr.md).

## Findings

### 1. aurora can re-project a recorded frame without re-running the game

- GX calls write a real command FIFO, which `gx/command_processor.cpp` decodes
  into `g_gxState`. That state includes the projection as a 4×4 matrix
  (`gx.hpp`, `proj`) and ten position/normal matrix slots.
- Each draw's uniform data is assembled at record time
  (`gx/shader_info.cpp` around line 439): projection, position matrices,
  texture matrices, lights, TEV colors.
- The vertex shader computes the camera-space position `mv_pos` from the
  position matrix, then `clip = vec4(mv_pos, 1) * ubuf.proj`
  (`gx/shader.cpp` around line 1030).
- A frame is a `FramePacket` of `RenderPass`es, each holding a `CommandList`
  of `DrawCommand`s (`gfx/frame_packet.hpp`). The render worker encodes them.
  Framebuffer copies (`GXCopyTex`) split passes and set `resolveTarget`.

`mv_pos` is in the game camera's view space, so a world draw can be redrawn
for an eye by replacing only the last step:

```
clip_eye = mv_pos · V_game⁻¹ · A · V_eye · P_eye
```

`V_game⁻¹` undoes the game camera, `A` places the arena in the room, and
`V_eye`/`P_eye` are the OpenXR view and projection. That's one 4×4 per eye
per frame, computed on the CPU. Lighting, texgen, skinning, and projected
shadows all happen before the projection, so they don't change.

Two catches:

- **Fog.** It reads `in.pos.z` (`shader.cpp:1573`), the screen depth after
  projection. Eye passes need the game-projection depth passed as its own
  varying, or fog will shift.
- **Per-draw immediates.** They're already used for `DrawImmediateData`, and
  the limit is 64 bytes. The eye matrix needs its own small uniform, either
  a bind group or a dynamic offset, set once per eye pass.

### 2. Melee says which camera is drawing, and the camera says what it draws

- Every camera render goes through `HSD_CObjSetCurrent`
  (`sysdolphin/baselib/cobj.c:521`). It sets `GXSetProjection` and the
  view matrix (`cobj->view_mtx`, from `C_MTXLookAt`), and it knows whether
  the pass renders offscreen. That's the hook point: tell aurora "camera X
  is current, view matrix V, category C".
- A camera GObj draws the GX links set in its `gxlink_prios` bitmask.
  During a fight:

| Draw group | Contents | Registered in |
|---|---|---|
| 3 (sometimes 2, 5, 7) | Stage parts, through `grDisplay_801C5DB0` | `gr/*` |
| 4, 5 | Fighters (`ftDrawCommon_80080E18`) | `ft/*` |
| 6 | Items | `it/*` |
| 7, 8 | Effects (`efLib_render_callback`) | `ef/*` |
| 8 to 11 | HUD: name tags 9, HUD lights 10, HUD models 11 | `if/*` |

- **Main fight camera.** Its render callback is `fn_800301D0`
  (`cm/camera.c` around line 4026). It clears to the stage's background
  color, then draws:
  - groups 0 and 3 in stage passes 2, 1, and 0
  - groups 4 to 6, fighters and items
  - group 7, effects
  - stage pass 3, foreground
- **HUD.** It has its own orthographic camera (`if/ifall.c:236`,
  `gxlink_prios = 0xD00`: groups 8, 10, 11).

So **world versus HUD splits by which camera is current**, with no per-draw
guessing.

### 3. Stage parts carry a layer and an id, so selective backgrounds are feasible

- Each stage part (`Ground`) has a 3-bit layer, `x11_flags.b012`.
  `grDisplay_801C5DB0` draws it only when that layer matches the camera's
  current stage pass (`Camera_8003108C`), and the main camera walks passes
  2, 1, 0, then 3.
- Each part also has a `map_id`, its index in the stage's part table
  (`Ground_GetMapGObj`), which is stable per stage.
- Some parts are tied to a dedicated camera (`gp->x18`, `x10_flags.b3`).
  `grDisplay_801C5F60` is one such camera, which draws group 3 itself.

**Proposed rule.** By default, show stage passes 0 and 3 and hide passes 1
and 2, plus parts that use dedicated cameras. Then apply a per-stage
override table keyed by `(GrKind, map_id)` to force parts on or off. The
classification is applied in `grDisplay_801C5DB0`, which already returns
early for parts it shouldn't draw, so this is a small `TARGET_PC` hook.
Which layer each part is on per stage has to be mapped by hand. Step 2
below builds a tool for that.

### 4. Pokémon Stadium's screen, refraction, and shadows depend on a mono frame

- **Stadium screen.** The jumbotron feed is a copy of the main camera's
  finished frame (`grStadium_801D2FD0` → `pc_widescreen_copy_efb`), mapped
  onto the `PsType_Display` part. Its text window is a separate orthographic
  camera (`grStadium_801D2BEC`, `fn_801D2ED0`).
- **Refraction.** `lbRefract_8002247C` copies the framebuffer at 640×480 into
  a texture and projects it with the game camera, so it's a screen-space
  effect.
- **Shadows.** `lbShadow` renders silhouettes through an orthographic shadow
  camera into a texture, then projects them in camera space. Those
  coordinates are unchanged by the eye replay, so shadows should come
  through correctly.

**Consequence.** Keep the game's normal mono render of the main camera every
frame. It feeds the framebuffer copies (jumbotron, refraction). The eye
views are extra replays of that camera's world draws. The mono pass can run
at native or low resolution, since nobody sees it directly.

### 5. Things that will look wrong in 3D, by design

- **Billboards.** JObj billboards and particles face the game camera,
  because HSD computes them from `V_game`. They'll look flat and turned
  away from the viewer. That's acceptable at first. A later fix is to
  re-billboard per eye, or to the head pose, in the HSD billboard path.
- **Camera motion disappears.** The game camera's zoom, pan, and shake are
  undone by `V_game⁻¹`. Shake may be worth adding back as a small jitter
  of `A`.
- **Off-screen logic still follows the game camera.** That includes the
  magnifier bubbles, the "offscreen" state, and any camera-driven culling or
  part visibility. Scrolling stages (Rainbow Cruise, Icicle Mountain, Big
  Blue, Mute City) are built around the camera and will show edges and
  pop-in. **Start with static stages:** Final Destination, Battlefield,
  Yoshi's Story, Dream Land, Fountain of Dreams, Pokémon Stadium.
- **Refraction** samples the mono frame with game-camera coordinates, so in
  stereo it'll be offset per eye. Start with it disabled in eye passes.
- **Name tags** are group 9, outside the HUD camera's mask. Which camera
  draws them is an open question; check it when hooking up categories.

## Rendering design

1. **Categories.** `HSD_CObjSetCurrent`, behind `TARGET_PC`, tells aurora the
   current camera's category and `V_game`:
   - **World.** The main fight camera, `fn_800301D0`.
   - **HUD.** The `ifall` camera.
   - **Mono only.** Everything else: shadow and stadium cameras, and
     offscreen passes.

   aurora tags each recorded draw with the category, and with the camera's
   `V_game` for world draws.
2. **Mono pass.** Unchanged. It keeps the copies working, and it's what the
   virtual screen shows outside fights.
3. **Eye passes.** Two per frame, since the pinned Dawn has no multiview.
   - Re-encode the world-tagged draws of the main camera's passes into each
     eye's color and depth target, using the per-eye matrix from §1.
   - Viewport and scissor are overridden to the eye target.
   - The targets use the same render-target layout as the game's framebuffer,
     so existing pipelines are reused and nothing new compiles.
   - Clear to color 0 and alpha 0, then write alpha from depth coverage: an
     opaque surface gives 1. The OpenXR projection layer uses premultiplied
     alpha. Opaque geometry then hides the room, and additive effects that
     don't write depth add light over passthrough instead of being cut out.
4. **HUD plane.** HUD-tagged draws go to their own texture, shown on a quad
   layer. One option is a strip at the arena's front edge; another is head
   height above it. The current virtual-screen code already does quads.
5. **Arena anchor (`A`).**
   - Scale: Final Destination's main platform is about 170 game units. At
     about 0.6 cm per unit, the main stage is about 1 m wide on a table.
   - Placement starts as a fixed offset from `LOCAL_FLOOR`.
   - Then add grab-and-move with the grips. That needs a different button
     than Z, or a modifier.
   - Later, attach to a detected table through scene or spatial anchors
     (`XR_FB_scene`, `XR_FB_spatial_entity`).
6. **Scene switching.** Use 3D while the main fight camera is rendering, and
   the virtual screen otherwise. Character select, stage select, and results
   stay flat.
7. **Frame pacing.** The game runs at 60 Hz and the headset at 72 or 90 Hz.
   Re-encode the latest recorded world draws with a fresh head pose on every
   display frame, without re-running the game. Head motion is then smooth,
   and only fighter motion is 60 Hz. This needs the last frame's packet
   (buffers, bind groups) to stay alive until the next game frame, plus a
   render-worker hook that the XR thread can trigger. If GPU time is short,
   Meta's Application SpaceWarp (`XR_FB_space_warp`) is the fallback.

## Cost (to be measured; nothing below is measured in 3D yet)

- **Flat baseline on Quest 3** (render scale 1, 584×480): about 3.05 ms of
  GPU time per game frame. GPU binning cost about as much as rendering, so
  per-draw and per-vertex work is a large share.
- **Two eye passes** pay that geometry cost twice more, on top of the mono
  pass. Pixel cost grows with eye resolution, which is 1680×1760 at full
  Quest 3 size.
- **The bridge copy** of two full-size eye images was about 1.2 ms per frame
  (dawn-xr-bridge). Depth for reprojection adds more.
- **Budget:** 13.9 ms at 72 Hz.

Levers if it's over budget:
- render the eyes at 0.7 to 0.8 scale
- drop the mono pass to native resolution
- foveated rendering (`XR_FB_foveation`)
- SpaceWarp
- a Dawn fork for zero-copy into the swapchain

The fork was judged not worth it for the flat screen. Per-eye full-resolution
copies at display rate make it worth reconsidering.

## Order of work

1. **Spike: stereo of the main camera with no filtering.**
   - Add the `HSD_CObjSetCurrent` hook, record `V_game`, and replay world
     draws into two eye targets.
   - Submit a projection layer.
   - Test on Final Destination with a fixed anchor.
   - Measure GPU time on Quest 3.

   This answers the cost question before anything else gets built.
2. **Stage part survey tool.**
   - An env knob logs each part's `(GrKind, map_id, layer, dedicated
     camera)`.
   - A controller toggle hides one part at a time.
   - Use it to fill the per-stage override table for the starter stages.
3. **Selective backgrounds.** Add the pass-and-allowlist rule in
   `grDisplay_801C5DB0`, starting with Pokémon Stadium's `PsType_Display`
   screen.
4. **HUD plane.** Route HUD-tagged draws to their own texture and quad, and
   place it.
5. **Passthrough compositing.** Write alpha from depth, use a premultiplied
   projection layer, check effects over the room, and fix fog with the
   game-depth varying.
6. **Display-rate re-encode.** Re-encode with a fresh head pose every display
   frame, or SpaceWarp.
7. **Anchor UX.** Grab and move, recenter, then table anchoring.
8. **Polish.** Per-eye billboards, refraction, name tags, and camera shake.

## Open questions

- Which camera draws name tags (group 9), and should they float in 3D?
- Does any of the starter stages hide or show parts based on the game
  camera? The survey tool in step 2 will show it.
- Is full-screen-flash or screen-shake-style feedback (hit flashes, KO
  effects) drawn by the main camera as world geometry? If so, it needs the
  HUD category or suppression.
- Should 3D be opt-in per match, for example a toggle in the launcher panel?
