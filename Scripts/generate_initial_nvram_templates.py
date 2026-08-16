#!/usr/bin/env python3
"""Generate first-boot EEPROM seeds from the validated smoke-test SRAM set."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


# Every released set for which Automatic Initial NVRAM Setup changes Link Mode,
# plus the two released Star Wars Trilogy Arcade sets whose cabinet is forced
# to Upright. Prototypes and location tests are intentionally excluded.
GAMES = (
    "daytona2", "dayto2pe",
    "dirtdvls", "dirtdvlsu", "dirtdvlsau", "dirtdvlsj", "dirtdvlsg",
    "harley", "harleya",
    "lemans24",
    "scud", "scudau", "scuddx", "scuddxo", "scudplus", "scudplusa",
    "skichamp",
    "spikeofe", "spikeout",
    "srally2", "srally2dx",
    "swtrilgy", "swtrilgya",
    "von2", "von254g", "von2a", "von2o",
)


def block_data(blob: bytes, wanted: bytes) -> bytes:
    offset = 0
    while offset + 12 <= len(blob):
        length, name_length, comment_length = struct.unpack_from("<III", blob, offset)
        if length < 12 + name_length + comment_length or offset + length > len(blob):
            break
        name_start = offset + 12
        name = blob[name_start:name_start + name_length].rstrip(b"\0")
        data_start = name_start + name_length + comment_length
        if name == wanted:
            return blob[data_start:offset + length]
        offset += length
    raise ValueError(f"block {wanted.decode()} not found")


def eeprom_words(path: Path) -> tuple[int, ...]:
    data = block_data(path.read_bytes(), b"93C46")
    if len(data) < 128:
        raise ValueError(f"short 93C46 block in {path}")
    return struct.unpack_from("<64H", data)


def render(smoke_saves: Path) -> str:
    lines = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace LibretroInitialNvram {",
        "",
        "struct Template {",
        "  std::string_view game;",
        "  std::array<uint16_t, 64> eeprom;",
        "};",
        "",
        "// Per-set factory EEPROM seeds extracted from the validated Model 3",
        "// smoke-test campaign. They provide a valid game-owned layout before",
        "// the first emulated frame; the field-level patcher then changes only",
        "// the requested offline/cabinet fields and regenerates the checksum.",
        "// Backup RAM and transient EEPROM controller state are never copied.",
        "static constexpr Template kTemplates[] = {",
    ]
    for game in GAMES:
        path = smoke_saves / f"{game}.srm"
        if not path.is_file():
            raise FileNotFoundError(path)
        words = eeprom_words(path)
        lines.append(f'  {{ "{game}", {{')
        for index in range(0, 64, 8):
            row = ", ".join(f"0x{word:04x}" for word in words[index:index + 8])
            lines.append(f"    {row},")
        lines.append("  } },")
    lines.extend((
        "};",
        "",
        "inline const Template *Find(std::string_view game)",
        "{",
        "  for (const Template &entry : kTemplates)",
        "    if (entry.game == game)",
        "      return &entry;",
        "  return nullptr;",
        "}",
        "",
        "}  // namespace LibretroInitialNvram",
        "",
    ))
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("smoke_saves", type=Path,
                        help="smoke-test saves directory containing <set>.srm")
    parser.add_argument("output", type=Path,
                        help="InitialNvramTemplates.h output path")
    args = parser.parse_args()
    args.output.write_text(render(args.smoke_saves), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
