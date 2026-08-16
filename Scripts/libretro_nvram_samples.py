#!/usr/bin/env python3
"""Generate reproducible Supermodel .srm samples from scripted RetroPad input.

Each sample runs in a fresh, isolated RetroArch environment initialized from
the same optional standard .srm.  Sequences and reusable macros are read from
a TOML file; see ``libretro_nvram_samples.example.toml``.

This utility is intentionally macOS-specific: clean RetroArch shutdown uses
CoreGraphics to post Esc directly to the process under test.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import tomllib
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


BUTTONS = {
    "B": 0,
    "Y": 1,
    "SELECT": 2,
    "START": 3,
    "UP": 4,
    "DOWN": 5,
    "LEFT": 6,
    "RIGHT": 7,
    "A": 8,
    "X": 9,
    "L": 10,
    "R": 11,
    "L2": 12,
    "R2": 13,
    "L3": 14,
    "R3": 15,
}
SUFFIX_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
WAIT_RE = re.compile(r"^WAIT\(\s*([0-9]+(?:\.[0-9]+)?)\s*\)$", re.IGNORECASE)
BUTTON_HOLD_RE = re.compile(
    r"^(SHORT|LONG)\(\s*([A-Za-z0-9_-]+)"
    r"(?:\s*\*\s*(0|[1-9][0-9]*))?\s*\)$",
    re.IGNORECASE,
)
LATCH_RE = re.compile(r"^(HOLD|RELEASE)\(\s*([A-Za-z0-9_-]+)\s*\)$", re.IGNORECASE)
REPEAT_RE = re.compile(r"^(.+?)\s*\*\s*(0|[1-9][0-9]*)$")
PLAYER_BUTTON_RE = re.compile(r"^P([12])_(.+)$")


@dataclass(frozen=True)
class Action:
    kind: str
    value: str | float | tuple["ButtonPress", ...] = ""


@dataclass(frozen=True)
class ButtonPress:
    name: str
    long: bool = False


@dataclass(frozen=True)
class Settings:
    config_path: Path
    repo_root: Path
    retroarch: Path
    core: Path
    rom_dir: Path
    system_assets: Path
    base_config: Path | None
    game: str
    standard_srm: Path | None
    output_dir: Path
    startup_wait: float
    button_hold: float
    button_hold_long: float
    default_wait: float
    settle_before_close: float
    close_gap: float
    shutdown_timeout: float
    window_width: int
    window_height: int


@dataclass(frozen=True)
class Sample:
    suffix: str
    actions: tuple[Action, ...]


@dataclass
class Result:
    sample: str
    status: str
    save: str
    log: str
    console_log: str
    elapsed_seconds: float
    note: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate isolated Supermodel NVRAM samples from a TOML campaign."
    )
    parser.add_argument("config", type=Path, help="TOML campaign file")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument(
        "--sample", action="append", default=[], metavar="SUFFIX",
        help="run only this sample; repeat to select multiple samples",
    )
    selection.add_argument(
        "--resume-from", metavar="SUFFIX",
        help="start with this sample and continue with all following samples",
    )
    parser.add_argument(
        "--output", type=Path,
        help="override campaign.output_dir (useful for temporary validation)",
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="validate and print fully expanded sequences without launching RetroArch",
    )
    parser.add_argument(
        "--overwrite", action="store_true",
        help="replace existing output samples",
    )
    parser.add_argument(
        "--keep-workdirs", action="store_true",
        help="preserve isolated RetroArch work directories under output/work",
    )
    return parser.parse_args()


def config_path(value: str | Path, base: Path) -> Path:
    expanded = Path(os.path.expandvars(os.path.expanduser(str(value))))
    return expanded.resolve() if expanded.is_absolute() else (base / expanded).resolve()


def table(data: dict[str, Any], name: str) -> dict[str, Any]:
    value = data.get(name)
    if not isinstance(value, dict):
        raise ValueError(f"missing or invalid [{name}] table")
    return value


def positive_number(data: dict[str, Any], name: str, default: float) -> float:
    value = data.get(name, default)
    if not isinstance(value, (int, float)) or isinstance(value, bool) or value <= 0:
        raise ValueError(f"{name} must be a positive number")
    return float(value)


def positive_integer(data: dict[str, Any], name: str, default: int) -> int:
    value = data.get(name, default)
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise ValueError(f"{name} must be a positive integer")
    return value


def split_tokens(value: Any, label: str, allow_empty: bool = False) -> list[str]:
    if isinstance(value, str):
        parts = [value]
    elif isinstance(value, list) and all(isinstance(item, str) for item in value):
        parts = value
    else:
        raise ValueError(f"{label} must be a string or an array of strings")
    tokens: list[str] = []
    for part in parts:
        tokens.extend(token.strip() for token in part.split(",") if token.strip())
    if not tokens and not allow_empty:
        raise ValueError(f"{label} must not be empty")
    return tokens


def normalize_button_name(value: str) -> str | None:
    normalized = value.upper().replace("-", "_")
    if normalized in BUTTONS:
        return normalized
    player_button = PLAYER_BUTTON_RE.fullmatch(normalized)
    if player_button and player_button.group(2) in BUTTONS:
        return normalized
    return None


def button_target(name: str) -> tuple[int, int]:
    player_button = PLAYER_BUTTON_RE.fullmatch(name)
    if player_button:
        user = int(player_button.group(1)) - 1
        button = player_button.group(2)
    else:
        user = 0
        button = name
    return user, BUTTONS[button]


def parse_button_presses(token: str) -> list[ButtonPress] | None:
    explicit_press = BUTTON_HOLD_RE.fullmatch(token)
    if explicit_press:
        hold_type = explicit_press.group(1).upper()
        normalized = normalize_button_name(explicit_press.group(2))
        if normalized is None:
            raise ValueError(f"unknown {hold_type.lower()}-press button: {token}")
        count = int(explicit_press.group(3) or 1)
        return [ButtonPress(normalized, hold_type == "LONG") for _ in range(count)]

    normalized = normalize_button_name(token)
    if normalized is not None:
        return [ButtonPress(normalized)]
    return None


def expand_actions(
    tokens: list[str], macros: dict[str, list[str]], stack: tuple[str, ...] = (),
) -> list[Action]:
    actions: list[Action] = []
    for token in tokens:
        repeat = REPEAT_RE.fullmatch(token)
        if repeat:
            base, count = repeat.group(1).strip(), int(repeat.group(2))
            actions.extend(expand_actions([base], macros, stack) * count)
            continue
        if "+" in token:
            chord: list[ButtonPress] = []
            for part in (part.strip() for part in token.split("+")):
                presses = parse_button_presses(part)
                if not part or presses is None:
                    raise ValueError(f"invalid simultaneous button sequence: {token}")
                if len(presses) != 1:
                    raise ValueError(
                        "repeat the complete simultaneous sequence outside the "
                        f"button expression: {token}"
                    )
                chord.extend(presses)
            names = [press.name for press in chord]
            if len(set(names)) != len(names):
                raise ValueError(f"duplicate button in simultaneous sequence: {token}")
            actions.append(Action("chord", tuple(chord)))
            continue
        latch = LATCH_RE.fullmatch(token)
        if latch:
            operation = latch.group(1).lower()
            normalized = normalize_button_name(latch.group(2))
            if normalized is None:
                raise ValueError(f"unknown {operation} button: {token}")
            actions.append(Action(operation, normalized))
            continue
        if token.startswith("@"):
            requested = token[1:].strip().lower()
            if not requested or requested not in macros:
                raise ValueError(f"unknown macro: {token}")
            if requested in stack:
                chain = " -> ".join(f"@{name}" for name in (*stack, requested))
                raise ValueError(f"recursive macro: {chain}")
            actions.extend(expand_actions(macros[requested], macros, (*stack, requested)))
            continue
        wait = WAIT_RE.fullmatch(token)
        if wait:
            seconds = float(wait.group(1))
            if seconds < 0:
                raise ValueError(f"invalid wait: {token}")
            actions.append(Action("wait", seconds))
            continue
        presses = parse_button_presses(token)
        if presses is not None:
            actions.extend(
                Action("button_long" if press.long else "button", press.name)
                for press in presses
            )
            continue
        normalized = token.upper().replace("-", "_")
        if normalized in {"ESC", "KEY_ESC"}:
            actions.append(Action("escape"))
        elif normalized == "CLOSE":
            actions.append(Action("close"))
        else:
            raise ValueError(f"unknown sequence token: {token}")
    return actions


def validate_close(actions: list[Action]) -> list[Action]:
    close_positions = [index for index, action in enumerate(actions) if action.kind == "close"]
    if not close_positions:
        actions.append(Action("close"))
        close_positions = [len(actions) - 1]
    if len(close_positions) > 1:
        raise ValueError("a sample sequence may contain CLOSE only once")
    if close_positions[0] != len(actions) - 1:
        raise ValueError("CLOSE must be the final action in a sample sequence")

    held: set[str] = set()
    for action in actions:
        if action.kind == "hold":
            name = str(action.value)
            if name in held:
                raise ValueError(f"button already held: {name}")
            held.add(name)
        elif action.kind == "release":
            name = str(action.value)
            if name not in held:
                raise ValueError(f"button released without HOLD: {name}")
            held.remove(name)
        elif action.kind in {"button", "button_long"}:
            name = str(action.value)
            if name in held:
                raise ValueError(f"button pressed again while held: {name}")
        elif action.kind == "chord":
            presses = action.value
            assert isinstance(presses, tuple)
            overlap = held.intersection(press.name for press in presses)
            if overlap:
                raise ValueError(
                    "simultaneous sequence presses an already held button: "
                    + ", ".join(sorted(overlap))
                )
        elif action.kind == "close" and held:
            raise ValueError(
                "CLOSE reached with held button(s): " + ", ".join(sorted(held))
            )
    return actions


def load_campaign(args: argparse.Namespace) -> tuple[Settings, list[Sample]]:
    script = Path(__file__).resolve()
    repo_root = script.parents[1]
    config = args.config.expanduser().resolve()
    with config.open("rb") as stream:
        data = tomllib.load(stream)
    retroarch = table(data, "retroarch")
    campaign = table(data, "campaign")
    base = config.parent

    game = str(campaign.get("game", "")).strip()
    if not game or not SUFFIX_RE.fullmatch(game):
        raise ValueError("campaign.game must be a valid short ROM-set name")
    standard_value = campaign.get("standard_srm")
    standard_srm = config_path(standard_value, base) if standard_value else None
    configured_output = campaign.get(
        "output_dir", f"~/Documents/RetroArch/model3-nvram-analysis/{game}/generated"
    )
    output_dir = (
        args.output.expanduser().resolve()
        if args.output else config_path(configured_output, base)
    )
    base_config_value = retroarch.get("base_config")
    settings = Settings(
        config_path=config,
        repo_root=repo_root,
        retroarch=config_path(retroarch.get("executable", ""), base),
        core=config_path(retroarch.get("core", ""), base),
        rom_dir=config_path(retroarch.get("rom_dir", ""), base),
        system_assets=config_path(retroarch.get("system_assets", repo_root / "Config"), base),
        base_config=config_path(base_config_value, base) if base_config_value else None,
        game=game,
        standard_srm=standard_srm,
        output_dir=output_dir,
        startup_wait=positive_number(campaign, "startup_wait", 10.0),
        button_hold=positive_number(campaign, "button_hold", 0.06),
        button_hold_long=positive_number(campaign, "button_hold_long", 0.5),
        default_wait=positive_number(campaign, "default_wait", 0.18),
        settle_before_close=positive_number(campaign, "settle_before_close", 0.5),
        close_gap=positive_number(campaign, "close_gap", 0.7),
        shutdown_timeout=positive_number(campaign, "shutdown_timeout", 15.0),
        window_width=positive_integer(campaign, "window_width", 496),
        window_height=positive_integer(campaign, "window_height", 384),
    )

    macro_data = data.get("macros", {})
    if not isinstance(macro_data, dict):
        raise ValueError("[macros] must be a TOML table")
    macros: dict[str, list[str]] = {}
    for name, value in macro_data.items():
        normalized = name.strip().lower()
        if not normalized or normalized in macros:
            raise ValueError(f"invalid or duplicate macro name: {name!r}")
        macros[normalized] = split_tokens(value, f"macro {name}", allow_empty=True)

    sample_data = data.get("samples")
    if not isinstance(sample_data, list) or not sample_data:
        raise ValueError("at least one [[samples]] entry is required")
    samples: list[Sample] = []
    seen: set[str] = set()
    for index, entry in enumerate(sample_data, 1):
        if not isinstance(entry, dict):
            raise ValueError(f"samples entry {index} must be a TOML table")
        suffix = str(entry.get("suffix", "")).strip()
        if not SUFFIX_RE.fullmatch(suffix):
            raise ValueError(f"invalid samples[{index}].suffix: {suffix!r}")
        if suffix in seen:
            raise ValueError(f"duplicate sample suffix: {suffix}")
        seen.add(suffix)
        tokens = split_tokens(entry.get("sequence"), f"sample {suffix} sequence")
        actions = validate_close(expand_actions(tokens, macros))
        samples.append(Sample(suffix, tuple(actions)))

    requested = set(args.sample)
    unknown = sorted(requested - seen)
    if unknown:
        raise ValueError("unknown sample suffix(es): " + ", ".join(unknown))
    if requested:
        samples = [sample for sample in samples if sample.suffix in requested]
    elif args.resume_from:
        if args.resume_from not in seen:
            raise ValueError(f"unknown resume sample suffix: {args.resume_from}")
        start = next(
            index for index, sample in enumerate(samples)
            if sample.suffix == args.resume_from
        )
        samples = samples[start:]
    return settings, samples


def validate_paths(settings: Settings) -> None:
    required_files = [settings.retroarch, settings.core]
    if settings.base_config:
        required_files.append(settings.base_config)
    if settings.standard_srm:
        required_files.append(settings.standard_srm)
    for path in required_files:
        if not path.is_file():
            raise FileNotFoundError(path)
    if not settings.rom_dir.is_dir():
        raise FileNotFoundError(settings.rom_dir)
    if not settings.system_assets.is_dir():
        raise FileNotFoundError(settings.system_assets)
    rom = settings.rom_dir / f"{settings.game}.zip"
    if not rom.is_file():
        raise FileNotFoundError(rom)


def action_label(action: Action) -> str:
    if action.kind == "button":
        return str(action.value)
    if action.kind == "button_long":
        return f"LONG({action.value})"
    if action.kind == "chord":
        presses = action.value
        assert isinstance(presses, tuple)
        return "+".join(
            f"LONG({press.name})" if press.long else press.name
            for press in presses
        )
    if action.kind == "hold":
        return f"HOLD({action.value})"
    if action.kind == "release":
        return f"RELEASE({action.value})"
    if action.kind == "wait":
        return f"WAIT({float(action.value):g})"
    if action.kind == "escape":
        return "KEY_ESC"
    return "CLOSE"


def print_dry_run(settings: Settings, samples: list[Sample]) -> None:
    print(f"Game:     {settings.game}")
    print(f"ROM:      {settings.rom_dir / (settings.game + '.zip')}")
    print(f"Standard: {settings.standard_srm or '(new blank save)'}")
    print(f"Output:   {settings.output_dir}")
    for sample in samples:
        destination = settings.output_dir / "saves" / f"{settings.game}-{sample.suffix}.srm"
        print(f"\n[{sample.suffix}] -> {destination}")
        print("  " + " -> ".join(action_label(action) for action in sample.actions))


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def quote_config(value: Path | str) -> str:
    return str(value).replace("\\", "\\\\").replace('"', '\\"')


def write_isolated_config(settings: Settings, work: Path, remote_port: int) -> list[str]:
    for directory in ("system/supermodel", "saves", "states", "logs", "config", "remaps"):
        (work / directory).mkdir(parents=True, exist_ok=True)
    for asset in settings.system_assets.iterdir():
        if asset.is_file() and asset.suffix.lower() in {".xml", ".bmp"}:
            shutil.copy2(asset, work / "system" / "supermodel" / asset.name)

    core_options = work / "config" / "core-options.cfg"
    core_options.write_text(
        'supermodel_initial_nvram_setup = "disabled"\n'
        'supermodel_nvram_settings = "disabled"\n'
        'supermodel_network_board = "enabled"\n'
        'supermodel_force_feedback = "disabled"\n',
        encoding="utf-8",
    )
    overrides = {
        "config_save_on_exit": "false",
        "system_directory": work / "system",
        "savefile_directory": work / "saves",
        "savestate_directory": work / "states",
        "log_dir": work / "logs",
        "log_to_file": "true",
        "log_to_file_timestamp": "false",
        "log_verbosity": "true",
        "core_options_path": core_options,
        "global_core_options": "true",
        "input_remapping_directory": work / "remaps",
        "content_history_path": work / "config" / "content_history.lpl",
        "content_favorites_path": work / "config" / "content_favorites.lpl",
        "sort_savefiles_enable": "false",
        "sort_savefiles_by_content_enable": "false",
        "network_remote_enable": "true",
        "network_remote_enable_user_p1": "true",
        "network_remote_enable_user_p2": "true",
        "network_remote_base_port": str(remote_port),
        "input_max_users": "2",
        "network_cmd_enable": "false",
        "fps_show": "false",
        "framecount_show": "false",
        "video_fullscreen": "false",
        "video_windowed_fullscreen": "false",
        "video_window_custom_size_enable": "true",
        "video_windowed_position_width": str(settings.window_width),
        "video_windowed_position_height": str(settings.window_height),
        "video_scale": "1.000000",
        "video_window_save_positions": "false",
        "video_shader_enable": "false",
        "pause_nonactive": "false",
        "savestate_auto_load": "false",
        "savestate_auto_save": "false",
        "audio_enable": "false",
        "autosave_interval": "0",
    }
    overrides_path = work / "nvram-samples-overrides.cfg"
    lines = ["# Isolated Supermodel NVRAM sampling overrides"]
    lines.extend(f'{key} = "{quote_config(value)}"' for key, value in overrides.items())
    overrides_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    if not settings.base_config:
        return ["-c", str(overrides_path)]
    base_copy = work / "base-retroarch.cfg"
    shutil.copy2(settings.base_config, base_copy)
    return ["-c", str(base_copy), f"--appendconfig={overrides_path}"]


def remote_packet(user: int, button: int, pressed: bool) -> bytes:
    return struct.pack("@iiiiHxx", user, 1, 0, button, int(pressed))


def send_button_state(
    sock: socket.socket, base_port: int, name: str, pressed: bool,
) -> None:
    user, button = button_target(name)
    sock.sendto(
        remote_packet(user, button, pressed),
        ("127.0.0.1", base_port + user),
    )


def send_button(
    sock: socket.socket, port: int, name: str, hold_seconds: float,
    settings: Settings,
) -> None:
    send_button_state(sock, port, name, True)
    time.sleep(hold_seconds)
    send_button_state(sock, port, name, False)
    time.sleep(settings.default_wait)


def send_chord(
    sock: socket.socket, port: int, presses: tuple[ButtonPress, ...],
    settings: Settings,
) -> None:
    for press in presses:
        send_button_state(sock, port, press.name, True)

    started = time.monotonic()
    releases = sorted(
        (
            (
                settings.button_hold_long if press.long else settings.button_hold,
                press,
            )
            for press in presses
        ),
        key=lambda item: item[0],
    )
    while releases:
        release_at = releases[0][0]
        remaining = started + release_at - time.monotonic()
        if remaining > 0:
            time.sleep(remaining)
        due = [item for item in releases if item[0] == release_at]
        releases = [item for item in releases if item[0] != release_at]
        for _, press in due:
            send_button_state(sock, port, press.name, False)
    time.sleep(settings.default_wait)


def send_latched_button(
    sock: socket.socket, port: int, name: str, pressed: bool,
    settings: Settings,
) -> None:
    send_button_state(sock, port, name, pressed)
    time.sleep(settings.default_wait)


def post_escape_to_pid(pid: int) -> None:
    application_services = ctypes.CDLL(
        "/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices"
    )
    core_foundation = ctypes.CDLL(
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"
    )
    application_services.CGEventCreateKeyboardEvent.restype = ctypes.c_void_p
    application_services.CGEventCreateKeyboardEvent.argtypes = [
        ctypes.c_void_p, ctypes.c_ushort, ctypes.c_bool,
    ]
    application_services.CGEventPostToPid.argtypes = [ctypes.c_int, ctypes.c_void_p]
    core_foundation.CFRelease.argtypes = [ctypes.c_void_p]
    for pressed in (True, False):
        event = application_services.CGEventCreateKeyboardEvent(None, 53, pressed)
        if not event:
            raise RuntimeError("CoreGraphics could not create an Esc key event")
        application_services.CGEventPostToPid(pid, event)
        core_foundation.CFRelease(event)
        time.sleep(0.03)


def wait_while_alive(process: subprocess.Popen[bytes], seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        exit_code = process.poll()
        if exit_code is not None:
            raise RuntimeError(f"RetroArch exited early with code {exit_code}")
        time.sleep(min(0.10, deadline - time.monotonic()))


def activate_process(process: subprocess.Popen[bytes], timeout: float = 3.0) -> None:
    """Bring the exact RetroArch instance launched for this sample to front."""
    script = (
        'tell application "System Events" to set frontmost of first process '
        f'whose unix id is {process.pid} to true'
    )
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"RetroArch exited before its window became active "
                f"(code {process.returncode})"
            )
        completed = subprocess.run(
            ["/usr/bin/osascript", "-e", script],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            check=False,
        )
        if completed.returncode == 0:
            return
        time.sleep(0.10)
    raise RuntimeError(
        "could not activate the new RetroArch instance; check macOS "
        "Automation permission for the terminal"
    )


def close_retroarch(process: subprocess.Popen[bytes], settings: Settings) -> None:
    if process.poll() is not None:
        raise RuntimeError(f"RetroArch exited before CLOSE (code {process.returncode})")
    time.sleep(settings.settle_before_close)
    post_escape_to_pid(process.pid)
    time.sleep(settings.close_gap)
    if process.poll() is None:
        post_escape_to_pid(process.pid)
    try:
        exit_code = process.wait(timeout=settings.shutdown_timeout)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            "RetroArch did not exit after Esc, wait, Esc; check macOS Accessibility permission"
        ) from error
    if exit_code != 0:
        raise RuntimeError(f"RetroArch exited with code {exit_code}")


def terminate_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5.0)


def execute_actions(
    process: subprocess.Popen[bytes], port: int, sample: Sample, settings: Settings,
) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        for action in sample.actions:
            if action.kind in {"button", "button_long"}:
                if process.poll() is not None:
                    raise RuntimeError(f"RetroArch exited before button {action.value}")
                hold_seconds = (
                    settings.button_hold_long
                    if action.kind == "button_long"
                    else settings.button_hold
                )
                send_button(sock, port, str(action.value), hold_seconds, settings)
            elif action.kind == "chord":
                if process.poll() is not None:
                    raise RuntimeError("RetroArch exited before simultaneous buttons")
                presses = action.value
                assert isinstance(presses, tuple)
                send_chord(sock, port, presses, settings)
            elif action.kind in {"hold", "release"}:
                if process.poll() is not None:
                    raise RuntimeError(
                        f"RetroArch exited before {action.kind.upper()}({action.value})"
                    )
                send_latched_button(
                    sock, port, str(action.value), action.kind == "hold", settings
                )
            elif action.kind == "wait":
                wait_while_alive(process, float(action.value))
            elif action.kind == "escape":
                if process.poll() is not None:
                    raise RuntimeError("RetroArch exited before KEY_ESC")
                post_escape_to_pid(process.pid)
                time.sleep(settings.default_wait)
            elif action.kind == "close":
                close_retroarch(process, settings)


def copy_if_present(source: Path, destination: Path) -> str:
    if not source.is_file():
        return ""
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return str(destination)


def find_save(work: Path, game: str) -> Path | None:
    candidates = sorted((work / "saves").rglob(f"{game}.srm"), key=lambda path: len(path.parts))
    return candidates[0] if candidates else None


def run_sample(
    settings: Settings, sample: Sample, overwrite: bool, keep_workdirs: bool,
) -> Result:
    stem = f"{settings.game}-{sample.suffix}"
    save_out = settings.output_dir / "saves" / f"{stem}.srm"
    log_out = settings.output_dir / "logs" / f"{stem}.log"
    console_out = settings.output_dir / "logs" / f"{stem}.console.log"
    if save_out.exists() and not overwrite:
        return Result(
            stem, "skipped", str(save_out),
            str(log_out) if log_out.is_file() else "",
            str(console_out) if console_out.is_file() else "",
            0.0, "sample already exists; use --overwrite to replace it",
        )

    started = time.monotonic()
    temporary = tempfile.TemporaryDirectory(prefix=f"supermodel-nvram-{stem}-")
    work = Path(temporary.name)
    port = free_udp_port()
    config_args = write_isolated_config(settings, work, port)
    if settings.standard_srm:
        shutil.copy2(settings.standard_srm, work / "saves" / f"{settings.game}.srm")
    core_log = work / "logs" / f"{stem}.log"
    console_tmp = work / "logs" / f"{stem}.console.log"
    rom = settings.rom_dir / f"{settings.game}.zip"
    command = [
        str(settings.retroarch), "-v", *config_args, "-L", str(settings.core),
        f"--log-file={core_log}", str(rom),
    ]
    status = "failed"
    note = ""
    save = ""
    process: subprocess.Popen[bytes] | None = None
    try:
        with console_tmp.open("wb") as console:
            process = subprocess.Popen(command, stdout=console, stderr=subprocess.STDOUT)
            activate_process(process)
            wait_while_alive(process, settings.startup_wait)
            execute_actions(process, port, sample, settings)
        generated = find_save(work, settings.game)
        if not generated:
            raise RuntimeError("RetroArch exited cleanly but no .srm was found")
        log_text = core_log.read_text(encoding="utf-8", errors="replace") if core_log.is_file() else ""
        if "Saving RAM type" not in log_text and "Saving NVRAM" not in log_text:
            raise RuntimeError("the log does not confirm a clean NVRAM save")
        save = copy_if_present(generated, save_out)
        status = "ok"
        note = "sample saved from a fresh copy of the standard .srm"
    except KeyboardInterrupt:
        if process and process.poll() is None:
            try:
                close_retroarch(process, settings)
            except RuntimeError:
                terminate_process(process)
        raise
    except (OSError, RuntimeError) as error:
        note = str(error)
        if process:
            terminate_process(process)
    finally:
        log = copy_if_present(core_log, log_out)
        console_log = copy_if_present(console_tmp, console_out)
        if keep_workdirs:
            kept = settings.output_dir / "work" / stem
            if kept.exists():
                shutil.rmtree(kept)
            shutil.copytree(work, kept)
        temporary.cleanup()
    return Result(
        stem, status, save, log, console_log,
        round(time.monotonic() - started, 3), note,
    )


def write_summary(output: Path, results: list[Result]) -> None:
    rows = [asdict(result) for result in results]
    (output / "summary.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    if sys.platform != "darwin":
        raise RuntimeError("this NVRAM sampling utility currently supports macOS only")
    args = parse_args()
    settings, samples = load_campaign(args)
    validate_paths(settings)
    if args.dry_run:
        print_dry_run(settings, samples)
        return 0

    for directory in ("saves", "logs"):
        (settings.output_dir / directory).mkdir(parents=True, exist_ok=True)
    print(f"Output: {settings.output_dir}")
    print(f"Game: {settings.game}; samples: {len(samples)}")
    results: list[Result] = []
    for index, sample in enumerate(samples, 1):
        print(f"[{index:02}/{len(samples):02}] {sample.suffix}", flush=True)
        result = run_sample(settings, sample, args.overwrite, args.keep_workdirs)
        results.append(result)
        write_summary(settings.output_dir, results)
        print(f"       {result.status}: {result.note}", flush=True)
    return 1 if any(result.status not in {"ok", "skipped"} for result in results) else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, FileExistsError, KeyError, OSError, RuntimeError,
            TypeError, ValueError, tomllib.TOMLDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
