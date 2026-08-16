#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <core> <file-description-regex>" >&2
    exit 2
fi

core=$1
expected_format=$2

if [[ ! -s $core ]]; then
    echo "Core is missing or empty: $core" >&2
    exit 1
fi

description=$(file -b "$core")
echo "Core: $core"
echo "Format: $description"

if [[ ! $description =~ $expected_format ]]; then
    echo "Unexpected core format. Expected pattern: $expected_format" >&2
    exit 1
fi

case $description in
    *Mach-O*)
        symbols=$(nm -gU "$core")
        ;;
    *)
        symbols=$(nm -g --defined-only "$core")
        ;;
esac
required_symbols=(
    retro_init
    retro_deinit
    retro_api_version
    retro_get_system_info
    retro_get_system_av_info
    retro_set_environment
    retro_set_video_refresh
    retro_set_audio_sample
    retro_set_audio_sample_batch
    retro_set_input_poll
    retro_set_input_state
    retro_set_controller_port_device
    retro_reset
    retro_run
    retro_serialize_size
    retro_serialize
    retro_unserialize
    retro_cheat_reset
    retro_cheat_set
    retro_load_game
    retro_load_game_special
    retro_unload_game
    retro_get_region
    retro_get_memory_data
    retro_get_memory_size
)

for symbol in "${required_symbols[@]}"; do
    if ! grep -Eq "[[:space:]]_?${symbol}$" <<<"$symbols"; then
        echo "Missing required Libretro symbol: $symbol" >&2
        exit 1
    fi
done

echo "Validated ${#required_symbols[@]} required Libretro symbols."

case $description in
    *ELF*)
        dependencies=$(ldd "$core")
        echo "$dependencies"
        if grep -q "not found" <<<"$dependencies"; then
            echo "The Linux core has unresolved runtime dependencies." >&2
            exit 1
        fi
        ;;
    *Mach-O*)
        otool -L "$core"
        ;;
    *PE32*)
        dependencies=$(objdump -p "$core" | grep "DLL Name:" || true)
        echo "$dependencies"
        if grep -Eqi "(libgcc|libstdc\+\+|zlib1)\.dll" <<<"$dependencies"; then
            echo "The Windows core depends on a runtime that should be statically linked." >&2
            exit 1
        fi
        ;;
esac
