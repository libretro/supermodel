#!/usr/bin/env python3
"""Run isolated RetroArch boot/video smoke tests for Games.xml ROM sets."""

from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path


DEFAULT_FPS = 60.0
CRASH_MARKERS = (
    "segmentation fault",
    "bus error",
    "fatal error",
    "uncaught exception",
    "assertion failed",
)


@dataclass(frozen=True)
class Game:
    name: str
    title: str
    version: str


@dataclass
class Result:
    name: str
    title: str
    version: str
    status: str
    exit_code: int | None
    signal: str
    frames: int
    emulated_seconds: float
    real_seconds: float
    fast_forward_requested: bool
    fast_forward_activated: bool
    video: str
    final_frame: str
    save: str
    log: str
    console_log: str
    note: str


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description=(
            "Launch each Games.xml set in an isolated RetroArch environment, "
            "stop after a deterministic number of emulated frames, and save a "
            "ScreenCaptureKit video, its final frame, logs, generated SRAM, "
            "CSV, and JSON summary."
        )
    )
    parser.add_argument("--retroarch", type=Path, required=True,
                        help="RetroArch executable")
    parser.add_argument("--core", type=Path,
                        default=repo_root / "supermodel_libretro.dylib",
                        help="Supermodel Libretro core (default: repository macOS build)")
    parser.add_argument("--ffmpeg", type=Path,
                        help="FFmpeg executable (default: first ffmpeg in PATH)")
    parser.add_argument("--rom-dir", type=Path, required=True,
                        help="Directory containing <set>.zip ROMs")
    parser.add_argument("--games-xml", type=Path,
                        default=repo_root / "Config" / "Games.xml",
                        help="Games.xml used to enumerate sets")
    parser.add_argument("--system-assets", type=Path,
                        help="Directory containing Games.xml and Music.xml; defaults to Games.xml's directory")
    parser.add_argument("--base-config", type=Path,
                        help="Optional RetroArch config copied before isolated path overrides")
    parser.add_argument("--output", type=Path,
                        help=("Persistent results directory (default: "
                              "~/Documents/RetroArch/supermodel-smoke/<timestamp>)"))
    parser.add_argument("--seconds", type=float, default=60.0,
                        help="Emulated seconds before capture (default: 60)")
    parser.add_argument("--fps", type=float, default=DEFAULT_FPS,
                        help="Frames per emulated second (default: 60)")
    parser.add_argument("--wall-timeout", type=float, default=180.0,
                        help="Real-time process timeout per game in seconds (default: 180)")
    parser.add_argument("--fast-forward", action=argparse.BooleanOptionalAction,
                        default=True, help="Toggle RetroArch fast-forward after startup (default: enabled)")
    parser.add_argument("--game", action="append", default=[], metavar="SET",
                        help="Test only this set; repeat for multiple sets")
    parser.add_argument("--from-game", metavar="SET",
                        help="Start at this set in Games.xml order (for resuming)")
    parser.add_argument("--list", action="store_true",
                        help="List selected games and ROM availability without launching RetroArch")
    parser.add_argument("--keep-workdirs", action="store_true",
                        help="Preserve per-game temporary RetroArch environments for diagnosis")
    return parser.parse_args()


def load_games(path: Path) -> list[Game]:
    root = ET.parse(path).getroot()
    games: list[Game] = []
    for node in root.findall("game"):
        name = node.get("name", "").strip()
        if not name:
            continue
        title = (node.findtext("identity/title") or name).strip()
        version = (node.findtext("identity/version") or "").strip()
        games.append(Game(name, title, version))
    return games


def select_games(games: list[Game], requested: list[str], start: str | None) -> list[Game]:
    known = {game.name: game for game in games}
    unknown = sorted(set(requested) - known.keys())
    if unknown:
        raise ValueError("unknown Games.xml set(s): " + ", ".join(unknown))
    selected = [known[name] for name in requested] if requested else games
    if start:
        positions = {game.name: index for index, game in enumerate(selected)}
        if start not in positions:
            raise ValueError(f"--from-game set is not selected: {start}")
        selected = selected[positions[start]:]
    return selected


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def quote_config(value: Path | str) -> str:
    return str(value).replace("\\", "\\\\").replace('"', '\\"')


def prepare_environment(work: Path, assets: Path, base_config: Path | None, port: int) -> list[str]:
    for directory in ("system/supermodel", "saves", "states", "logs", "config", "remaps"):
        (work / directory).mkdir(parents=True, exist_ok=True)
    for asset in assets.iterdir():
        if asset.is_file() and asset.suffix.lower() in {".xml", ".bmp"}:
            shutil.copy2(asset, work / "system" / "supermodel" / asset.name)

    overrides_path = work / "smoke-test-overrides.cfg"
    overrides = {
        "config_save_on_exit": "false",
        "system_directory": work / "system",
        "savefile_directory": work / "saves",
        "savestate_directory": work / "states",
        "log_dir": work / "logs",
        "log_to_file": "true",
        "log_to_file_timestamp": "false",
        "log_verbosity": "true",
        "core_options_path": work / "config" / "core-options.cfg",
        "input_remapping_directory": work / "remaps",
        "content_history_path": work / "config" / "content_history.lpl",
        "content_favorites_path": work / "config" / "content_favorites.lpl",
        "network_cmd_enable": "true",
        "network_cmd_port": str(port),
        "notification_show_fast_forward": "true",
        "fps_show": "false",
        "framecount_show": "false",
        "fastforward_ratio": "0.000000",
        "fastforward_frameskip": "true",
        "video_fullscreen": "false",
        "video_windowed_fullscreen": "false",
        "video_window_custom_size_enable": "true",
        "video_windowed_position_width": "496",
        "video_windowed_position_height": "384",
        "video_scale": "1.000000",
        "video_window_save_positions": "false",
        "pause_nonactive": "false",
        "savestate_auto_load": "false",
        "savestate_auto_save": "false",
    }
    lines = ["# Isolated Supermodel smoke-test overrides"]
    lines.extend(f'{key} = "{quote_config(value)}"' for key, value in overrides.items())
    overrides_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    if not base_config:
        return ["-c", str(overrides_path)]
    base_copy = work / "base-retroarch.cfg"
    shutil.copy2(base_config, base_copy)
    # RetroArch gives --appendconfig priority over its base configuration.
    # Keeping the files separate avoids duplicate-key first-value semantics.
    return ["-c", str(base_copy), f"--appendconfig={overrides_path}"]


def activate_fast_forward(port: int, process: subprocess.Popen[bytes], deadline: float) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(0.20)
        while process.poll() is None and time.monotonic() < deadline:
            try:
                sock.sendto(b"GET_STATUS\n", ("127.0.0.1", port))
                status, _ = sock.recvfrom(4096)
                if b"PLAYING" not in status.upper():
                    time.sleep(0.05)
                    continue
                sock.sendto(b"FAST_FORWARD\n", ("127.0.0.1", port))
                return True
            except TimeoutError:
                time.sleep(0.05)
            except OSError:
                time.sleep(0.05)
    return False


def terminate_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def copy_if_present(source: Path, destination: Path) -> str:
    if not source.is_file():
        return ""
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return str(destination)


def build_capture_helper(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    command = [
        "xcrun", "clang", "-fobjc-arc", "-fblocks", "-O2",
        "-framework", "Foundation", "-framework", "AppKit",
        "-framework", "CoreGraphics",
        "-framework", "CoreMedia", "-framework", "ScreenCaptureKit",
        "-framework", "AVFoundation", str(source), "-o", str(destination),
    ]
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(f"could not build ScreenCaptureKit helper: {detail}")


def extract_final_frame(ffmpeg: Path, video: Path, destination: Path) -> str:
    if not video.is_file() or video.stat().st_size == 0:
        return ""
    destination.parent.mkdir(parents=True, exist_ok=True)
    for offset in ("-0.10", "-0.50", "-1.00"):
        destination.unlink(missing_ok=True)
        completed = subprocess.run([
            str(ffmpeg), "-v", "error", "-y", "-sseof", offset,
            "-i", str(video), "-map", "0:v:0", "-frames:v", "1",
            "-update", "1", str(destination),
        ], capture_output=True, text=True)
        if completed.returncode == 0 and destination.is_file() and destination.stat().st_size:
            return str(destination)
    return ""


def run_game(args: argparse.Namespace, game: Game, output: Path, frames: int) -> Result:
    rom = args.rom_dir / f"{game.name}.zip"
    video_out = output / "videos" / f"{game.name}.mov"
    frame_out = output / "frames" / f"{game.name}.png"
    log_out = output / "logs" / f"{game.name}.log"
    console_out = output / "logs" / f"{game.name}.console.log"
    capture_log_out = output / "logs" / f"{game.name}.capture.log"
    save_out = output / "saves" / f"{game.name}.srm"
    if not rom.is_file():
        return Result(
            game.name, game.title, game.version, "missing_rom", None, "",
            frames, args.seconds, 0.0, args.fast_forward, False,
            "", "", "", "", "", f"ROM not found: {rom}",
        )

    temporary = tempfile.TemporaryDirectory(prefix=f"supermodel-smoke-{game.name}-")
    work = Path(temporary.name)
    port = free_udp_port()
    config_args = prepare_environment(work, args.system_assets, args.base_config, port)
    video_tmp = work / f"{game.name}.mov"
    core_log_tmp = work / "logs" / f"{game.name}.log"
    command = [
        str(args.retroarch), "-v", *config_args, "-L", str(args.core),
        f"--log-file={core_log_tmp}", f"--max-frames={frames}", str(rom),
    ]
    started = time.monotonic()
    timed_out = False
    ff_activated = False
    capture_exit_code: int | None = None
    process: subprocess.Popen[bytes] | None = None
    capture: subprocess.Popen[bytes] | None = None
    with console_out.open("wb") as console:
        try:
            process = subprocess.Popen(command, stdout=console, stderr=subprocess.STDOUT)
            with capture_log_out.open("wb") as capture_log:
                capture = subprocess.Popen([
                    str(args.capture_helper), "--pid", str(process.pid),
                    "--output", str(video_tmp),
                ], stdout=capture_log, stderr=subprocess.STDOUT)
                if args.fast_forward:
                    ff_activated = activate_fast_forward(port, process, time.monotonic() + 15.0)
                exit_code = process.wait(timeout=args.wall_timeout)
                try:
                    capture_exit_code = capture.wait(timeout=20.0)
                except subprocess.TimeoutExpired:
                    terminate_process(capture)
                    capture_exit_code = capture.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
            if process:
                terminate_process(process)
            exit_code = process.returncode
            if capture:
                try:
                    capture_exit_code = capture.wait(timeout=20.0)
                except subprocess.TimeoutExpired:
                    terminate_process(capture)
                    capture_exit_code = capture.returncode
        except KeyboardInterrupt:
            if process:
                terminate_process(process)
            if capture:
                terminate_process(capture)
            raise
    elapsed = time.monotonic() - started

    video = copy_if_present(video_tmp, video_out)
    final_frame = extract_final_frame(args.ffmpeg, video_out, frame_out) if video else ""
    log = copy_if_present(core_log_tmp, log_out)
    save_candidates = list((work / "saves").glob("**/*.srm"))
    save = copy_if_present(save_candidates[0], save_out) if save_candidates else ""
    signal_name = ""
    if exit_code is not None and exit_code < 0:
        try:
            signal_name = signal.Signals(-exit_code).name
        except ValueError:
            signal_name = str(-exit_code)

    diagnostic_text = ""
    for candidate in (core_log_tmp, console_out):
        if candidate.is_file():
            diagnostic_text += candidate.read_text(encoding="utf-8", errors="replace").lower()
    marker = next((item for item in CRASH_MARKERS if item in diagnostic_text), "")
    if timed_out:
        status, note = "timeout", f"exceeded {args.wall_timeout:g}s real-time limit"
    elif exit_code != 0 or signal_name or marker:
        status, note = "crash", marker or signal_name or f"exit code {exit_code}"
    elif capture_exit_code != 0 or not video:
        status, note = "no_capture", (
            f"ScreenCaptureKit helper failed with exit code {capture_exit_code}; "
            f"see {capture_log_out}"
        )
    elif not final_frame:
        status, note = "no_frame", "video was captured but FFmpeg could not extract its final frame"
    else:
        status, note = "ok", "inspect final frame or video for service/setup loops"
    if args.fast_forward and not ff_activated:
        note += "; fast-forward command was not acknowledged"

    if args.keep_workdirs:
        kept = output / "work" / game.name
        if kept.exists():
            shutil.rmtree(kept)
        shutil.copytree(work, kept)
    temporary.cleanup()
    return Result(game.name, game.title, game.version, status, exit_code, signal_name,
                  frames, args.seconds, round(elapsed, 3), args.fast_forward, ff_activated,
                  video, final_frame, save, log, str(console_out), note)


def write_summaries(output: Path, results: list[Result]) -> None:
    rows = [asdict(result) for result in results]
    (output / "summary.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    with (output / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]) if rows else ["name"])
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    args.retroarch = args.retroarch.expanduser().resolve()
    args.core = args.core.expanduser().resolve()
    args.rom_dir = args.rom_dir.expanduser().resolve()
    args.games_xml = args.games_xml.expanduser().resolve()
    args.system_assets = (args.system_assets or args.games_xml.parent).expanduser().resolve()
    args.base_config = args.base_config.expanduser().resolve() if args.base_config else None
    if args.seconds <= 0 or args.fps <= 0 or args.wall_timeout <= 0:
        raise ValueError("--seconds, --fps, and --wall-timeout must be positive")
    games = select_games(load_games(args.games_xml), args.game, args.from_game)
    if args.list:
        for game in games:
            state = "present" if (args.rom_dir / f"{game.name}.zip").is_file() else "MISSING"
            print(f"{game.name:12} {state:7} {game.title} ({game.version})")
        return 0
    ffmpeg = args.ffmpeg.expanduser() if args.ffmpeg else shutil.which("ffmpeg")
    if not ffmpeg:
        raise FileNotFoundError("ffmpeg (install it or pass --ffmpeg)")
    args.ffmpeg = Path(ffmpeg).resolve()
    for required in (args.retroarch, args.core, args.ffmpeg, args.games_xml,
                     args.system_assets):
        if not required.exists():
            raise FileNotFoundError(required)
    if args.base_config and not args.base_config.is_file():
        raise FileNotFoundError(args.base_config)

    if args.output:
        output = args.output.expanduser().resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        output = (Path.home() / "Documents" / "RetroArch" /
                  "supermodel-smoke" / stamp)
        output.mkdir(parents=True, exist_ok=False)
    for directory in ("videos", "frames", "logs", "saves", ".tools"):
        (output / directory).mkdir(exist_ok=True)
    helper_source = Path(__file__).with_name("ScreenCaptureWindow.m")
    if not helper_source.is_file():
        raise FileNotFoundError(helper_source)
    args.capture_helper = output / ".tools" / "ScreenCaptureWindow"
    build_capture_helper(helper_source, args.capture_helper)
    frames = max(1, round(args.seconds * args.fps))
    print(f"Results: {output}")
    print(f"Selected: {len(games)} sets; capture after {frames} frames ({args.seconds:g}s emulated)")

    results: list[Result] = []
    try:
        for index, game in enumerate(games, 1):
            print(f"[{index:02}/{len(games):02}] {game.name}: {game.title}", flush=True)
            result = run_game(args, game, output, frames)
            results.append(result)
            write_summaries(output, results)
            print(f"       {result.status} in {result.real_seconds:.1f}s", flush=True)
    except KeyboardInterrupt:
        print("Interrupted; completed results were preserved.", file=sys.stderr)
        return 130

    counts: dict[str, int] = {}
    for result in results:
        counts[result.status] = counts.get(result.status, 0) + 1
    print("Summary: " + ", ".join(f"{key}={value}" for key, value in sorted(counts.items())))
    print(f"Review final frames in: {output / 'frames'}")
    print(f"Review full window videos in: {output / 'videos'}")
    print(f"Generated isolated SRAM in: {output / 'saves'}")
    return 1 if any(result.status in {"crash", "timeout", "no_capture", "no_frame"}
                    for result in results) else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError, ET.ParseError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
