#!/usr/bin/env python3
"""Unattended performance runs on a Quest over adb.

  quest_perf.py run [--stage N] [--players 2|4] [--phases mr:40,vr:40] [--env K=V ...]
                    [--warmup S] [--label NAME]
      Boots a four-CPU match on stage N (StKind, gr/forward.h; 31 is
      Battlefield), then holds each phase (mode:seconds) in turn in the same
      session and prints the runtime's frame stats for each one. The arena is
      placed in front of wherever the headset lies (AURORA_XR_ARENA_AT_HEAD),
      and one eye image dumped in the warmup checks the stage is in view. Logs go to
      build/quest-perf/<label>-<phase>.log.
  quest_perf.py mode mr|vr      switch the running game (MELEE_XR_CONTROL)
  quest_perf.py summarize LOG   summarize a saved logcat

The game reads its knobs from melee-env.txt (src/pc/main.c,
pc_env_file_bootstrap), which this rewrites on every run. The disc comes from
the launcher's own launcher.cfg (debug builds only: run-as), or MELEE_DISC.
With more than one adb device and no ANDROID_SERIAL, the first device listed
by address is used: a wireless headset shows up under both its address and
its mDNS name.

Headset power: a run wakes the headset (reconnecting wireless adb if it
dropped while asleep), reports proximity closed and sets a 30-minute screen
timeout, so it stays on unworn. When the run ends, failed or not, it stops the
game, deletes the run files, hands proximity back to the sensor and sets the
usual 60-second timeout, so the headset sleeps until the next run wakes it.
--keep skips that.
"""
import argparse
import os
import re
import statistics
import subprocess
import sys
import time

PKG = "dev.melee.game"
FILES = f"/sdcard/Android/data/{PKG}/files"
ENV_FILE = f"{FILES}/melee-env.txt"
CTL_FILE = f"{FILES}/melee-ctl.txt"
OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "build", "quest-perf")


def adb(*args, check=True, capture=True):
    r = subprocess.run(["adb", *args], text=True, capture_output=capture)
    if check and r.returncode != 0:
        sys.exit(f"adb {' '.join(args)} failed: {r.stderr.strip()}")
    return r.stdout if capture else ""


def pick_device():
    if os.environ.get("ANDROID_SERIAL"):
        return
    devs = [l.split()[0] for l in adb("devices").splitlines()[1:] if l.endswith("\tdevice")]
    if not devs:
        sys.exit("no adb device")
    by_addr = [d for d in devs if re.match(r"^\d+\.\d+\.\d+\.\d+:\d+$", d)]
    os.environ["ANDROID_SERIAL"] = (by_addr or devs)[0]


RUN_SCREEN_TIMEOUT_MS = 1800000  # during a run: the display stays on unworn
IDLE_SCREEN_TIMEOUT_MS = 60000   # after it: the headset's usual


def wake_headset(tries=24):
    """Reconnects wifi adb if it dropped while the headset slept, then wakes
    it (proximity reported closed, a wake key) until it says it's awake."""
    serial = os.environ.get("ANDROID_SERIAL", "")
    for _ in range(tries):
        state = subprocess.run(["adb", "get-state"], text=True, capture_output=True)
        if state.stdout.strip() != "device":
            if ":" in serial:
                subprocess.run(["adb", "disconnect", serial], capture_output=True)
                subprocess.run(["adb", "connect", serial], capture_output=True, timeout=20)
            time.sleep(5)
            continue
        power = subprocess.run(["adb", "shell", "dumpsys power | grep mWakefulness="], text=True,
                               capture_output=True, timeout=20).stdout
        if "Awake" in power:
            return
        subprocess.run(["adb", "shell", "am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null; "
                        "input keyevent KEYCODE_WAKEUP"], capture_output=True, timeout=20)
        time.sleep(5)
    sys.exit("the headset didn't wake")


def hand_back():
    """After a run: the game stopped, its run files gone, proximity back to
    the sensor and the usual screen timeout, so the unworn headset sleeps.
    The next run wakes it (wake_headset)."""
    adb("shell", "am", "force-stop", PKG, check=False)
    adb("shell", "rm", "-f", ENV_FILE, CTL_FILE, check=False)
    adb("shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.automation_disable", check=False)
    adb("shell", "settings", "put", "system", "screen_off_timeout", str(IDLE_SCREEN_TIMEOUT_MS), check=False)


def shell_write(path, text):
    # chmod: the shell's umask leaves the file unreadable to the app.
    subprocess.run(["adb", "shell", f"cat > {path} && chmod 644 {path}"], input=text, text=True, check=True)


def disc_uri():
    if os.environ.get("MELEE_DISC"):
        return os.environ["MELEE_DISC"]
    cfg = adb("shell", "run-as", PKG, "cat", "files/launcher.cfg", check=False)
    m = re.search(r'^disc "(.*)"$', cfg, re.M)
    if not m:
        sys.exit("no disc in launcher.cfg; set MELEE_DISC")
    return m.group(1)


# VrApi's once-a-second line, e.g.
# FPS=121/120,...,Stale=4,...,CPU4/GPU=6/2,2361/545MHz,...,App=5.71ms,...,GPU%=0.84,CPU%=0.55(W0.91)
VRAPI = re.compile(
    r"FPS=(\d+)/(\d+).*?Stale=(\d+).*?CPU4/GPU=(\d+)/(\d+),(\d+)/(\d+)MHz.*?"
    r"App=([\d.]+)ms.*?GPU%=([\d.]+),CPU%=([\d.]+)"
)
XR_FPS = re.compile(r"([\d.]+) display fps \((\d+)% 3D\); frames/s released: screen ([\d.]+), 3D ([\d.]+), HUD ([\d.]+)")
PIPELINES = re.compile(r"pipelines: (\d+) created .*?Dawn cache: (\d+) hits.*?(\d+) misses")
XR_GPU = re.compile(r"GPU 3D passes per frame \(us\): 3D eyes (\d+).*?\(total (\d+)\); (\d+) world draws")


PID = re.compile(r"^\S+ \S+ [VDIWE]/(\S+)\s*\(\s*(\d+)\)")


def game_lines(text):
    """The game's own lines: other apps (the Home shell) log VrApi stats too.
    The game is the process writing Aurora lines."""
    lines = text.splitlines()
    pids = {m.group(2) for m in map(PID.match, lines) if m and m.group(1) == "Aurora"}
    return [l for l in lines if (m := PID.match(l)) and m.group(2) in pids] if pids else lines


def summarize(text):
    lines = game_lines(text)
    rows = [m.groups() for m in map(VRAPI.search, lines) if m]
    if not rows:
        return "  no VrApi stats (is the game in front and the headset awake?)"

    def col(i, f=float):
        return [f(r[i]) for r in rows]

    def mean(v):
        return statistics.fmean(v)

    def levels(i):
        vals = col(i, int)
        return "/".join(f"{k}:{vals.count(k)}" for k in sorted(set(vals)))

    out = [
        f"  {len(rows)} s  fps {mean(col(0)):.1f}/{rows[0][1]}  stale/s {mean(col(2)):.1f} (max {max(col(2, int))})",
        f"  CPU level {levels(3)} @ {mean(col(5)):.0f} MHz   GPU level {levels(4)} @ {mean(col(6)):.0f} MHz",
        f"  GPU% {mean(col(8)):.2f}  CPU% {mean(col(9)):.2f}  App GPU {mean(col(7)):.2f} ms",
    ]
    fps = [m.groups() for m in map(XR_FPS.search, lines) if m]
    if fps:
        out.append(f"  game: 3D released {mean([float(f[3]) for f in fps]):.1f}/s, "
                   f"HUD {mean([float(f[4]) for f in fps]):.1f}/s ({len(fps)} samples)")
    # Pipelines still compiling (a new build or Dawn invalidates the cache)
    # load the CPU and make the run meaningless.
    pipes = [m.groups() for m in map(PIPELINES.search, lines) if m]
    if pipes and int(pipes[-1][2]) > 500:  # cumulative; a warm cache still misses ~100 at boot
        out.insert(0, f"  WARNING: pipeline cache cold ({pipes[-1][2]} misses so far); rerun")
    gpu = [m.groups() for m in map(XR_GPU.search, lines) if m]
    if gpu:
        out.append(f"  passes: 3D eyes {mean([int(g[0]) for g in gpu]):.0f} us, "
                   f"total {mean([int(g[1]) for g in gpu]):.0f} us, "
                   f"{mean([int(g[2]) for g in gpu]):.0f} world draws")
    return "\n".join(out)


DUMP_DIR = f"{FILES}/dump"


def view_coverage():
    """How much of the left eye the 3D view drew (its alpha), from the dump."""
    raw = subprocess.run(["adb", "exec-out", "cat", f"{DUMP_DIR}/xr_3d_alpha.pgm"], capture_output=True).stdout
    adb("shell", "rm", "-rf", DUMP_DIR, check=False)
    parts = raw.split(maxsplit=4)
    if len(parts) < 5 or parts[0] != b"P5":
        return "  view: no eye dump (WARNING: can't tell whether the stage is in view)"
    w, h = int(parts[1]), int(parts[2])
    data = parts[4]
    # Multiview dumps stack the eyes vertically; the left eye is the top half.
    eye = data[: w * (h // 2)] if h > w else data
    covered = sum(1 for i in range(0, len(eye), 97) if eye[i] > 0) / max(len(range(0, len(eye), 97)), 1)
    warn = "  WARNING: stage barely in view; the GPU numbers are meaningless" if covered < 0.02 else ""
    return f"  view: the 3D view covers {covered * 100:.1f}% of the left eye{warn}"


def run(args):
    try:
        run_phases(args)
    finally:
        if not args.keep:
            hand_back()


def run_phases(args):
    phases = []
    for p in args.phases.split(","):
        mode, secs = p.split(":")
        if mode not in ("mr", "vr"):
            sys.exit(f"phase mode must be mr or vr: {p}")
        phases.append((mode, int(secs)))
    env = {
        "MELEE_BOOT_SCENE": "vs",
        "MELEE_DEBUG_VS": f"cpu{args.players}",
        "MELEE_DEBUG_VS_STAGE": str(args.stage),
        "AURORA_XR_TIMING": "1",
        "MELEE_XR_MODE": phases[0][0],
        "MELEE_XR_CONTROL": CTL_FILE,
        # Nobody wears the headset: place the arena from wherever it lies
        # and looks, or the stage can be out of view and the GPU numbers
        # meaningless (seen 2026-10-09: an empty eye at 7 ms).
        "AURORA_XR_ARENA_AT_HEAD": "1",
    }
    if args.view_check:
        # One eye image during the warmup, after the fight starts and before
        # measuring, to check the stage is in view.
        env["AURORA_XR_DUMP"] = DUMP_DIR
        env["AURORA_XR_DUMP_AFTER"] = str(max(args.warmup - 15, 5) * 60)
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v
    disc = disc_uri()
    if "AURORA_XR_DUMP" in env:
        adb("shell", "mkdir", "-p", env["AURORA_XR_DUMP"])
    wake_headset()
    shell_write(ENV_FILE, "".join(f"{k}={v}\n" for k, v in env.items()))
    shell_write(CTL_FILE, phases[0][0] + "\n")
    # Keep the display running with nobody wearing it, for the run.
    adb("shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.prox_close")
    adb("shell", "settings", "put", "system", "screen_off_timeout", str(RUN_SCREEN_TIMEOUT_MS))
    adb("shell", "am", "force-stop", PKG)
    adb("logcat", "-c")
    adb("shell", "am", "start", "-n", f"{PKG}/dev.melee.MeleeXrActivity", "--es", "disc", disc)
    print(f"{args.label}: stage {args.stage}, {args.players} CPUs, {' '.join(args.env) or 'default env'}; "
          f"warming up {args.warmup} s")
    time.sleep(args.warmup)
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, f"{args.label}-warmup.log"), "w") as f:
        f.write(adb("logcat", "-d", "-v", "time"))
    if args.view_check:
        print(view_coverage())
    for i, (mode, secs) in enumerate(phases):
        if i > 0:
            shell_write(CTL_FILE, mode + "\n")
            time.sleep(args.settle)
        adb("logcat", "-c")
        time.sleep(secs)
        text = adb("logcat", "-d", "-v", "time")
        path = os.path.join(OUT_DIR, f"{args.label}-{i}-{mode}.log")
        with open(path, "w") as f:
            f.write(text)
        print(f"[{mode}] {path}\n{summarize(text)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--stage", type=int, default=31)
    r.add_argument("--players", type=int, choices=[2, 4], default=4, help="CPU fighters")
    r.add_argument("--phases", default="mr:40,vr:40")
    r.add_argument("--env", action="append", default=[], help="extra K=V for melee-env.txt")
    r.add_argument("--warmup", type=int, default=50, help="seconds from launch to the first phase")
    r.add_argument("--settle", type=int, default=12, help="seconds after a mode switch before measuring")
    r.add_argument("--label", default="run")
    r.add_argument("--no-view-check", dest="view_check", action="store_false",
                   help="skip dumping an eye image to check the stage is in view")
    r.add_argument("--keep", action="store_true",
                   help="leave the game running and the headset awake afterwards (no hand_back)")
    m = sub.add_parser("mode")
    m.add_argument("mode", choices=["mr", "vr"])
    s = sub.add_parser("summarize")
    s.add_argument("log")
    args = ap.parse_args()
    if args.cmd == "summarize":
        with open(args.log) as f:
            print(summarize(f.read()))
        return
    pick_device()
    if args.cmd == "mode":
        shell_write(CTL_FILE, args.mode + "\n")
    else:
        run(args)


if __name__ == "__main__":
    main()
