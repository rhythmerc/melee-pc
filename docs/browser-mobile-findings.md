# Browser build on iPhone: findings (2026-10-06 to 10-08)

Exploratory work toward the "headset hosts a LAN session, phones join from a
browser" idea. Nothing here is shipped except the disc read-ahead (7634536).
The test harness is in `tools/browser/phone_test/`; see the end of this file.

## Setup

- Device: iPhone on iOS 26 Safari, joined over Tailscale; the page was served
  from the desktop with `tailscale serve` (tailnet-only HTTPS with a real
  certificate). WebGPU, SharedArrayBuffer and service workers all need a
  secure context, so a plain `http://<LAN IP>` page cannot run the build, and
  an HTTPS page cannot fetch from an `http://` LAN address either.
- The disc was not on the phone: `?disc=/disc.iso` made `Module.readDisc`
  fetch 512 KiB blocks by HTTP Range from the server (the engine's
  `readDisc(offset, size)` hook needed no change). This is the asset-streaming
  half of the LAN idea, working as is.
- Scene: `MELEE_BOOT_SCENE=vs MELEE_DEBUG_VS=cpu4`, stage picked with
  `MELEE_DEBUG_VS_STAGE=<StKind>`.

## Results

- **Performance holds:** 60 fps, p99 17-18 ms, four CPUs, in every steady
  state measured. Mobile Safari had never been tried before this.
- **Disc streaming is small:** a full match pulls 36-46 MiB of the 1.4 GB
  image.
- **Hitches come from two sources, both found.**

### 1. Streamed music reads (fixed)

Stage music is read one block at a time as it plays, every ~855 frames. Each
new block missed the cache and suspended the wasm for a whole network fetch,
which showed up as a 54-117 ms hitch every ~14 s. A local `File` costs about 1
ms here, so this only matters when the disc comes over a network.

Fixed in 7634536: `disc-cache.mjs` treats a read whose last block follows a
recent read's last block as a stream, and fetches the next block in the
background. On the phone, the stream's later blocks then no longer hitch. Only
the first music read (frame ~324) still costs one fetch.

Open: read-ahead also triggers during a match load. That added about 8 MiB of
fetching (36 to 44 MiB), and the load stall may have grown (2.5 s against
1.5-2.4 s before; possibly noise). Limiting read-ahead to small reads is a
candidate fix, but the log has no read sizes yet to tune it with.

### 2. Shader compiles stall Safari, not the engine

On a shader the phone has never compiled, the stall is 600-1100 ms per frame
for roughly the first 10 s of a new stage, plus ~1 s whenever a new effect
appears mid-match. The probes ruled out the engine step by step:

1. **The calls themselves are fast.** `CreateShaderModule` takes at most 0.2 ms
   and `createRenderPipelineAsync` returns in 0.0-0.1 ms. The browser path
   already compiles asynchronously and skips draws whose pipeline is not ready
   (`gfx/pipeline_cache.cpp` `get_pipeline`, `gx/gx.cpp`
   `CreateRenderPipelineAsync`).
2. **The engine is not running long.** Asyncify hooks show the engine yields
   at `pc_frame_boundary` on time. The yield's MessageChannel reply, which
   should arrive in about 0 ms, waits 650-1280 ms, and a 10 ms timer is late by
   the same amount. Safari holds the main thread outside our code until the
   compile finishes.
3. **Holding presentation only moves the stall.** With `?hold=1`,
   `getCurrentTexture` returned an off-screen texture while compiles were
   pending. The stall then moved into ordinary recording calls:
   `setBindGroup` 342 ms, `setPipeline` 350 ms, `drawIndexed` 292 ms,
   `createBindGroup` 411 ms. This fits Safari sending every WebGPU command
   through one fixed-size buffer to its GPU process, which stops reading it
   while it compiles. Once that buffer is full, whichever call comes next
   waits.

So on Safari, skipping draws (what Quest does) is necessary but not
sufficient. Anything that keeps sending commands while a compile is running
will stall. Chrome compiles on a separate thread, which is why desktop does
not show this.

**Warm vs cold.** iOS keeps compiled Metal shaders in a system cache across
page loads. A pipeline whose shader the phone has compiled before is ready in
2-7 ms, against ~80-200 ms or more for one it has never compiled. This is why
the first-ever run was much worse than later ones.

### Shadow compiles on a worker device (promising, not shipped)

`?shadow=1` sends every pipeline request to a Web Worker that has its own
WebGPU device, created with the same adapter options and device descriptor.
The worker gets the same WGSL and pipeline state and compiles it first. Only
when it finishes does the page's own device get the request, which then hits
the Metal cache. Worker WebGPU works on iOS 26, and every descriptor was
passed to the worker without errors.

Corneria, cold, with the worker:

- **Stage start:** frames 4-51 ran at 25-68 ms (mostly 25-45 ms) while the
  worker compiled ~150 pipelines over ~5 s. Without the worker, a cold stage
  (Venom) had frames of 500-1100 ms for ~10 s. The page's device kept running
  the whole time and draws were skipped meanwhile, which is the Quest
  behavior.
- **Mid-match:** 7 new pipelines compiled in the worker over 1.2 s with no
  hitch.
- **Remaining cost:** when the page's device takes on the new pipelines, the
  next frame or two hitches roughly in proportion to how many arrive together:
  7 gave 191 ms, 4 gave 146 ms, and the stage-start batch of 150 gave 410 ms.
  A warm run without the worker shows the same kind of cost (a 116 ms frame
  right after seven pipelines that were each ready in 5 ms). So even a cached
  pipeline costs Safari roughly 15-30 ms the first time it is used, with the
  smaller batches at the high end.

### Next steps, if this is picked up again

1. **Pace the release:** hand finished pipelines to the page's device one or
   two per frame instead of all at once. That trades one 200-400 ms frame for
   a few 20-40 ms ones, at the cost of draws staying skipped a little longer.
   Not yet tested.
2. **Productize the shadow compile** as a page-side module in
   `platforms/browser/`. It is all JS around the WebGPU API, so Aurora's C++
   would not change. Questions to settle first:
   - Is ~5 s of skipped draws at a cold stage start visually acceptable?
   - Does it behave on Chrome, where it is unnecessary? It could be enabled
     only on Safari.
3. **Startup cost seen along the way:** `GPUCanvasContext.configure` took
   129-354 ms during startup (before the first frame), which is harmless
   there.
4. **Saved pipeline cache is unreliable on iOS.** It saves to IndexedDB only
   when the page is hidden. Runs loaded 216, 116, 222, 478 or no pipelines.
   Saving after "Preparing graphics" and every so often during play would fix
   it.
5. **Considered and set aside:**
   - A starter pipeline cache shipped with the page. Prewarming covers first
     launch only, so it doesn't replace draw skipping.
   - A generic fallback shader, in the style of Dolphin's ubershaders. Keep it
     in mind if the worker approach fails on some device.

## For the LAN-session idea

- **Game assets over the server:** works today through `readDisc`. Ranged
  fetches from the host, the streaming read-ahead above, and ~40 MiB per
  match.
- **Secure context is a hard requirement.** The options found:
  - an HTTPS page plus WebRTC to the headset with no signaling server. The
    page builds the headset's session description from a QR code, as libp2p's
    WebRTC Direct does.
  - a real certificate through some service, as Tailscale provided here.
  - a self-signed certificate, which shows a warning and may not get WebGPU
    after the click-through.
- **Netplay transport:** browsers have no UDP, so `net.c` refuses to connect
  in the browser. A WebRTC DataChannel set to unreliable and unordered is the
  fit for rollback.
- **Rollback is two-peer today.** A room of phones needs more peers, or a
  star topology with the host in the middle.
- **Phones keep up:** an iPhone held 60 fps with four CPUs. Rollback re-runs
  several frames at once, which has not been measured on a phone.
- **Shader stalls matter more for netplay.** A 1 s main-thread stall on one
  peer stalls every peer's rollback session. The worker approach, which keeps
  the game running through compiles, is a prerequisite for phones as peers.

## Reproducing

```sh
# Browser build (LLVM 22 with LibTooling is in the melee-xr distrobox)
distrobox enter melee-xr -- sh -c 'python3 tools/browser/setup_sdk.py && \
  LLVM_ROOT=/usr/lib/llvm-22 python3 tools/browser/build.py --jobs 10'

# Plain ISO from the RVZ (the browser build accepts only GALE01 rev 2 .iso/.gcm)
flatpak run --command=dolphin-tool --filesystem=home org.DolphinEmu.dolphin-emu \
  convert -i <disc>.rvz -o ~/Downloads/melee-iso/GALE01.iso -f iso

# Test page: the shipped shell plus probes, served next to the build
mkdir -p /tmp/pt && cp platforms/browser/shell.mjs tools/browser/phone_test/shadow-worker.js /tmp/pt/
patch /tmp/pt/shell.mjs tools/browser/phone_test/shell-probes.patch
python3 tools/browser/phone_test/serve_phone.py \
  build/browser/runtime/platforms/browser /tmp/pt ~/Downloads/melee-iso/GALE01.iso 5190 &
tailscale serve --bg 5190     # off again with: tailscale serve --https=443 off
```

Then open, on the phone:

```
https://<host>.ts.net/?disc=/disc.iso&MELEE_BOOT_SCENE=vs&MELEE_DEBUG_VS=cpu4&MELEE_DEBUG_VS_STAGE=<n>[&shadow=1][&hold=1]
```

The page sends its log, timestamped, to `POST /log`, which the server appends
to `/tmp/phone.log`, one directory above the overlay. Each line type:

| Line | Meaning |
|---|---|
| `HITCH` | Frame over 25 ms |
| `STATS` | fps and p99 every 600 frames |
| `DISC` | One block fetch and how long it took |
| `HEAP` | wasm memory grew |
| `SLOWCALL` | A WebGPU call over 5 ms |
| `BLOCKED` | The main thread was busy while a 10 ms timer came due |
| `WASMRUN` / `SUSPEND` | Asyncify run and suspend spans over 40 ms |
| `HOLD` | Frames presented to an off-screen texture (`?hold=1`) |
| `SHADOW` | Worker compile stats (`?shadow=1`) |

The per-pipeline `PT shader` and `PT pipeline` lines quoted above came from
temporary `Log.warn` timing around `build_shader_source`, `CreateShaderModule`
(`gx/shader.cpp`) and `CreateRenderPipelineAsync` (`gx/gx.cpp`). They are not
kept. Re-add them locally if needed.

To get cold shaders again, use a stage the device has not compiled yet. Stages
already compiled on the test phone: Pokémon Stadium (3), Big Blue (24),
Rainbow Cruise (11), Venom (22), Corneria (7), and the default stage.
