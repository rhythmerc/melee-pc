# Debugging

## Log files

Everything goes through `pc_log_line()` (`src/pc/pc.h`), timestamped in
milliseconds from startup. The log goes to `stderr` and to a file; output is
flushed per line and both streams are flushed before a fatal `abort()`, so the
file survives a crash. `MELEE_LOG_FILE` picks the file (an empty value disables
it). Without it:

| Platform | Log file |
|---|---|
| Windows | `melee-pc.log` in the working directory (beside `melee.exe`); the session before it is kept as `melee-pc.1.log`. |
| Linux | `~/.local/share/melee-pc/logs/melee-<date>-<time>-<pid>.log` |
| macOS | `~/Library/Application Support/melee-pc/logs/melee-<date>-<time>-<pid>.log` |
| Android / Quest | `/sdcard/Android/data/dev.melee.game/files/logs/melee-<date>-<time>-<pid>.log`; the Quest's launcher panel and game are separate processes and write one each. |

The timestamped folders keep the newest 10 files. On Android every line also
goes to logcat (tag `Melee` for `pc_log_line`, `Aurora` for aurora's), and a
native crash's backtrace goes to logcat and the log file as `[FATAL]` lines
(`src/pc/main.c`); iOS also logs through `os_log`.
`MELEE_DEBUG=1` raises aurora's log level to debug. Any frame over 50 ms leaves
a marker in the log (`src/pc/vi.c`). Backend selection logs one line per
skipped backend with Dawn's reason (`No usable D3D12 adapter (...); trying the
next backend`) and then a summary such as
`graphics backend: d3d11 (auto), adapter: Intel(R) HD Graphics 4400 [8086:0a16], driver: ...`;
that line is the first thing to read in a GPU report.

The Windows zip ships `RUN-AND-LOG.bat`: it sets `MELEE_DEBUG`, `MELEE_LOG_FILE`
and `MELEE_FPS`, records the OS version and GPU driver into `melee-pc-env.log`,
boots a `melee.ciso` sitting beside the exe if there is one, and keeps the
window open. Ask for both `.log` files when triaging a Windows report.

## Send logs

Settings → Launcher → Logs → **Send** gathers the newest six logs (each cut to
its last 1 MB, at most 4 MB in all; home-folder paths shown as `~`, and
internet addresses as `ip-1`, `ip-2`, ... with one number per address across
the bundle, while loopback and LAN addresses stay) and on
Android the app's own logcat into one text bundle (`src/pc/log_report.cpp`).
The first press says how much it holds and what is in it, the second uploads
it to the log drop (`tools/log-drop`), which answers with a code such as
`XL-7K3QM2` for the player to post. `tools/fetch_logs.py <code>` fetches and
splits it; `tools/log-drop/README.md` has the setup. When the upload fails, or
the build has no log drop URL (compiled in from `XELEE_LOGS_URL` in the
configure environment, which CI takes from the repository secret; overridden
at run time by `MELEE_LOGS_URL`), the bundle is saved as `xelee-logs-<time>.txt` in
`/sdcard/Download` on Android, beside `melee-pc.log` on Windows, or in the log
folder, and the row says where.

## Crash handler (Windows)

A hard access violation never reaches the logger, so on Windows
`SetUnhandledExceptionFilter` (`src/pc/main.c`) writes the exception code, the
faulting address and a backtrace as `module+RVA` per frame to `stderr` and the
log file. The module name separates a fault inside `webgpu_dawn.dll` from one in
`melee.exe`; the RVA feeds `addr2line` against the matching build.

## gdb

`tools/run.sh <disc>` runs the game under gdb and prints every thread's
backtrace on a crash or SIGINT; `tools/run_dbg.sh` adds a FIFO/PAD state dump.
A backtrace is ground truth; a screenshot under Xwayland may be stale (see
[testing.md](testing.md)). For fields of a `DISC_STRUCT`, print a local the
program computed rather than trusting gdb's view of `DW_AT_endianity`.

## Heap check

`MELEE_HEAP_CHECK=1` appends a canary to every `OSAlloc` allocation
(`extern/aurora/lib/dolphin/os/OSAlloc.cpp`), checks them each frame, and aborts
at the first stomp with the allocating call site (resolve with
`addr2line -f -C -e build/melee <addr>`). This is the tool for "walked an object
by GameCube word number" and other off-by-pointer-width writes.

## Diagnostic environment variables

These are off by default and cost nothing when unset. They measure or suppress
only; none of them fixes anything. User-facing knobs (`MELEE_BACKEND`,
`MELEE_VSYNC`, ...) are in the [README](../README.md#environment-variables).

| Variable | Effect |
|---|---|
| `MELEE_DEBUG=1` | Debug-level aurora logging. |
| `MELEE_LOG_FILE=<path>` | Log file sink (defaults under [Log files](#log-files); empty disables). |
| `MELEE_LOGS_URL=<url>` | Where Send logs uploads, instead of the built-in log drop (e.g. a local `wrangler dev`). |
| `MELEE_FPS=1` | Print frame rate once a second. |
| `MELEE_HEAP_CHECK=1` | Canaries on every heap allocation, checked each frame; aborts at the first stomp. |
| `MELEE_SEED=<n>` | Deterministic RNG for the attract demo. |
| `MELEE_LAUNCHER_PLATFORM=desktop\|mobile\|quest` | Lay the launcher out as that platform does (what it hides), to check the phone or Quest panel layout from a desktop. |
| `MELEE_SLIPPI_AUTH_HOST=<url>`, `MELEE_SLIPPI_TYPED=<a;b;...>` | Point the Slippi account calls (sign-in, sign-up, email, rules, connect code) at `tools/slippi_fake_auth.py` instead of Firebase, with `MELEE_SLIPPI_FIREBASE_KEY=fake` and `MELEE_SLIPPI_GRAPHQL_ENDPOINT=<url>/graphql`; and answer the Login and Register keyboards from a list (`<back>` is Back). `MELEE_BOOT_SCENE=login` or `register` opens those pages. |
| `MELEE_AUDIO_DUMP=<file>` | Also write the mix as raw f32 stereo 32 kHz. |
| `MELEE_SFX_STATS=1` | Sound-effect request/accept/reject counts. |
| `MELEE_AUDIO_STATS=1` | Per-0.5s voice census. |
| `MELEE_AUDIO_ADDR=1` | Report voice sample addresses against the ARAM bounds. |
| `MELEE_GFX_STATS=1` | Peak per-pool high-water marks (vertex/uniform/index/storage/texture) for sizing frame budgets. |
| `MELEE_CPU_TRACE=1` | Per-CPU-player AI census every ~2s. |
| `MELEE_PIPELINE_SYNC=1` | Block the frame on a never-before-seen GX pipeline (old behaviour, the #46 stutter) instead of skipping its draw until it compiles. |
| `MELEE_SCENE_LOG=1` | Report scene model descriptor slots (the joint pointer `gmRegClearAddModel` panics on). |
| `MELEE_ARCHIVE_LOG=1` | Report every archive load with its header sizes. |
| `MELEE_INTRO_LOG=1`, `MELEE_ICON_LOG=1`, `MELEE_TEXANIM_LOG=1` | Team-splash capture buffers; CSS icon frame selection; out-of-range texture-animation frames. |
| `MELEE_MOBJ_MARK=1` | Tag draws with whether the material had a texture. |
| `MELEE_TEV_TREE=1` | Count compiled TEV stages and how many carry a texture. |
| `MELEE_TEX_ASSIGN=1` | Count tobjs assigned a texmap vs forced to null. |
| `MELEE_PS_TEXMISS=1` | Report particles that ask for a texture but resolve none. |
| `MELEE_EF_LOG=1`, `MELEE_EF_SKIP=a-b`, `MELEE_EF_MAT=<id>` | Report or suppress effect ids; dump the material of one effect. |
| `MELEE_ZTEX_BIAS=<n>` | Z-texture bias for the sobj tile pass (0 = retail; separates an empty Z capture from a missing draw). |
| `MELEE_CAM_BONE=1` | Report camera-bone inputs where the value is produced, so the first bad frame is logged. |
| `MELEE_INSTANT_WIN=1` | End a VS match after 60 frames (test fixture). |
| `MELEE_CLASSIC_STAGE_OVERRIDE=<1-11>`, `MELEE_CLASSIC_TEAM=kirby\|jiggly` | Force a Classic stage / team fight (test fixture); with `MELEE_BOOT_SCENE=classic` it boots straight into that stage (6 is Snag the Trophies, 9 is Race to the Finish). |
| `MELEE_ADVENTURE_SCENE=<id>` | With `MELEE_BOOT_SCENE=adventure`, boot Mario into that Adventure scene (the scene enum in `gm/gmadventure.c`, e.g. 0x01 the Mushroom Kingdom course, 0x3A the F-Zero Grand Prix). `MELEE_BOOT_SCENE=homerun` boots Mario into the Home-Run Contest. |
| `AURORA_LOG_UNTEX=1` | Report draws that bind no texture. |
| `AURORA_SKIP_UNTEX=1` | Drop every untextured draw. |
| `AURORA_SKIP_UNTEX_VTX=n`, `AURORA_ONLY_UNTEX_VTX=n` | Drop, or keep only, untextured draws with exactly n vertices. |
| `AURORA_LOG_TEV=1`, `AURORA_LOG_UNTEX_REGS=1`, `AURORA_LOG_QUADPOS=1` | Report what an untextured draw's TEV stages / registers / quad positions asked for. |
| `AURORA_LEGACY_TEX_LOD=1` | Restore the unconditional LOD-bias sample (positive control for the black-floor sweep). |
| `MELEE_XR_CONTROL=<file>` | Switch mixed reality / full VR from the file's `mr` or `vr`, read once a second (unattended runs, `tools/quest_perf.py`). |
| `AURORA_XR_MSAA=<1\|2\|4>` | Multisample the 3D eyes (default 4 on Android; needs the Dawn fork's multiview resolve). |
| `AURORA_XR_DYNRES=0\|1` | Dynamic resolution for the 3D eyes (default on Android); `_MIN`, `_MAX`, `_TARGET_MS`, `_LOAD_MS`, `_RANDOM=1` tune or test it. |
| `AURORA_XR_FIXED_LATENCY=0\|<n>` | Turn off fixed-latency presentation (on with dynamic resolution), or force it on at n display frames. |
| `AURORA_XR_PERF_CPU`, `AURORA_XR_PERF_GPU` | `off\|low\|high\|boost` clock requests (CPU high by default on Android). |
| `AURORA_XR_HANDS=0` | Leave OpenXR hand tracking off. |
| `AURORA_XR_FLAT_LOG=1` | Every 10 s, flat passes that kept world/hidden draws because an EFB copy or snapshot reads them, with resolve rect sizes. |
| `MELEE_XR_PS_GRAB_EVERY=<n>` | Pokémon Stadium's jumbotron grabs the flat frame one time in n in mixed reality (default 2; 1 every time). |
| `MELEE_XR_PS_ZOOM_ONLY=1` | Pokémon Stadium's jumbotron shows a player zoom in mixed reality instead of its full-arena feed. |
| `AURORA_VTX_DECODE=0\|1` | Decode every vertex attribute on the CPU into one interleaved vertex buffer (default on Android). |
| `AURORA_POS_DECODE=0\|1` | Decode vertex positions on the CPU for hardware vertex fetch (default on Android) or pull them in the shader. |
| `MELEE_XR_PTCL_TEST_HIDE=1` | Hide every particle from 3D and the flat frame (measuring their cost). |
| `AURORA_XR_FLAT_CLIP=0` | Don't clip a flat pass kept for an EFB copy to the copied rect. |
| `MELEE_PS_TEST_FORM=<5\|3\|4\|6\|9>`, `MELEE_PS_TEST_SCREEN=<n>` | Pin Pokémon Stadium's transformation (default, fire, grass, rock, water) or jumbotron state (test fixture; not for netplay). |
| `AURORA_NO_PIN=1`, `AURORA_PIN_THREADS=0` | Disable pinning aurora's worker threads to cores. |

`grep -rho 'getenv("\(MELEE\|AURORA\)_[A-Z0-9_]*")' src extern/aurora/lib` is the
authoritative list; the table lags it.
