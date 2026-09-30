# Meta Quest mixed-reality build

The game runs on a virtual screen floating in front of the player, over
passthrough. It is the regular Android build with OpenXR presentation added in
aurora (`extern/aurora/lib/xr`). The game renders exactly as it does for a
window, and only the present step changes.

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
window size isn't used. The game keeps its own 60 Hz pacing, since
`aurora_vsync_enabled()` reports false while XR is active. The XR thread runs
at display rate and shows the newest finished frame.

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
| Either grip | Z |
| Left menu button | Start |

## Knobs

On Quest, set these in `/sdcard/Android/data/dev.melee.game/files/melee-env.txt`.

| Variable | Default | Effect |
|---|---|---|
| `AURORA_XR` | on in Quest builds, off on desktop | Present to the headset |
| `AURORA_XR_PASSTHROUGH` | 1 | Passthrough behind the screen |
| `AURORA_XR_SCREEN_WIDTH` | 1.6 | Screen width in meters |
| `AURORA_XR_SCREEN_DISTANCE` | 1.5 | Meters in front of the starting head position |
| `AURORA_XR_SCREEN_Y` | 0 | Height offset in meters |
| `AURORA_XR_SCREEN_HEIGHT` | 1080 | Screen texture height in pixels |
| `AURORA_XR_DEBUG` | 0 | Log a centre pixel every 300 frames |

## Measured (Quest 3, 72 Hz)

| | Result |
|---|---|
| Display rate | 72/72 |
| Game frames on the screen | 60 per second |
| App GPU time (VrApi) | about 3.6 to 4.1 ms |

## Known gaps

- There's no recentering beyond the system's own.
- There's no controller haptics.
- The screen texture size is fixed at the first frame.
- Monado's `XR_FB_passthrough` layer crashes Monado, so desktop testing runs
  without passthrough.
