# Supermodel (Sega Model 3) - Libretro Port (Modernized)

> [!IMPORTANT]
> This port is under active development. The current branch passes the Linux
> x86_64, Windows x86_64, macOS x86_64, and macOS ARM64 CI matrix. Extensive
> real-ROM runtime testing has been performed on macOS ARM64 and Batocera 43.1
> x86_64; the remaining advertised targets still require equivalent runtime
> validation against the current Supermodel engine.

This branch combines three explicitly preserved lines of development:

- the historical [`libretro/supermodel`](https://github.com/libretro/supermodel)
  core lineage;
- the current [`trzy/Supermodel`](https://github.com/trzy/Supermodel) engine;
- the modern Libretro frontend developed by
  [`sgiannop/Libretro-Supermodel`](https://github.com/sgiannop/Libretro-Supermodel),
  imported at commit `1c094999664b23876323a50815022bad1f353a04`.

Most of the modern frontend implementation comes from sgiannop's work. The
subsequent commits in this branch adapt that frontend to the current upstream
engine while keeping the integration changes separate and reviewable. See
[`PORTING.md`](PORTING.md) for provenance, synchronization, and contribution
guidance.

## 🚀 Key Improvements
- **Unified Makefile:** Single build configuration supporting 6 platforms (Linux, Windows, macOS, Android, RPi64, aarch64) following libretro/skeletor standards.
- **Platform Auto-Detection:** Automatic platform detection with sensible defaults; platform-specific source filtering for incompatible features.
- **Native Libretro Audio:** Removed legacy SDL audio dependency in favor of native `audio_batch_cb` synchronization at a fixed 60 Hz cadence.
- **C++17 Migration:** Replaced legacy SDL-based threading and synchronization with native C++17 `std::mutex`, `std::lock_guard`, and atomic operations.
- **Ubuntu 24.04 Compatibility:** Fixed header conflicts and link-time errors present in the original codebase specifically for modern GCC versions.
- **Synchronous A/V Timing:** Matches standalone Supermodel's default 60 Hz cadence and 735-sample stereo audio packets; RetroArch owns final A/V synchronization.
- **Improved Input Mapping:** Full support for Analog/Digital gamepads and keyboard out of the box with improved deadzone handling.
- **Configurable Service & Test Buttons:** Service and Test buttons are now mappable through the RetroArch input configuration.
- **Force Feedback / Rumble:** Enabled by default for supported steering-wheel games via the Libretro rumble interface.
- **True Widescreen:** Expands the 3D horizontal field of view into a native
  16:9 framebuffer, with an optional wide lower-background layer. Plain 4:3
  stretching remains the frontend's responsibility.
- **Libretro Portability:** Remapped configuration, NVRAM, and asset paths to follow official Libretro standards (`system` and `save` directories).
- **Validated Save States:** Uses the current standalone format metadata plus
  Libretro integrity checking, rejects incompatible/corrupt states before
  modifying the machine, and remains compatible with older core states.
- **No External GL Dependency:** GLEW replaced with `glsym` from libretro-common — no system GL extension library required on any platform.
- **Android Support:** Full NDK integration with architecture-specific optimization (arm64, arm with NEON, x86_64, x86) and OpenGL ES 3.0.
- **macOS Universal Binary:** Builds for both Intel (x86_64) and Apple Silicon (arm64) via osxcross with automatic CPU tuning.
- **Raspberry Pi Optimized:** GLES3 rendering with CPU-specific tuning for RPi5 (Cortex-A76), RPi4 (Cortex-A72), and generic aarch64.
- **Windows Support:** Full cross-platform support with dedicated Windows build targets using MinGW — no vendored prebuilt libraries required.

## 📂 System assets and configuration

Place the engine data files in the RetroArch system directory:

* **Preferred path:** `[RetroArch System Directory]/supermodel/`
* **Files:**
    * `Games.xml` is required. It describes Model 3 games, hardware, ROM files,
      regions, and CRCs, and is therefore engine data rather than a user
      preference file.
    * `Music.xml` is optional and enables the corresponding music metadata.
    * `Supermodel.ini` is an optional expert override. If present, it is read
      before core options are applied. The core never creates or modifies it.

For compatibility with the initial port, files are also discovered in
`[RetroArch System Directory]/supermodel/Config/`. New installations should
use the preferred flat path. Without `Games.xml`, the core cannot identify or
load a game.

Normal settings belong to RetroArch: use core options for video, audio, CPU,
and control-layout choices; input remaps for controller bindings; and the
frontend save directory for NVRAM. Core options take precedence over an
optional `Supermodel.ini`.

## Video geometry

`Widescreen Mode` reproduces Supermodel's native widescreen behavior instead
of stretching the 4:3 image:

* `Disabled` renders the original 496x384 view.
* `Widescreen` expands full-screen 3D viewports horizontally inside a 16:9
  framebuffer (683x384 at native resolution), while keeping 2D layers at their
  original geometry.
* `Widescreen + Wide Background` also stretches the lower 2D background layer
  to fill the side areas. HUD and upper overlays retain their original aspect.

Supermodel's standalone `Stretch` setting is intentionally not exposed: final
4:3-to-16:9 stretching is already provided by RetroArch's aspect-ratio and
scaling controls. True widescreen renders about 37.7% more horizontal pixels
than the native 4:3 mode, so its performance should be measured separately.
Changes to `Widescreen Mode` take effect after restarting the content, matching
the renderer-initialization semantics of standalone Supermodel.

## Renderer options

Desktop builds that include both Supermodel renderers expose `3D Renderer`.
`New3D` remains the default. `Legacy3D` is experimental, uses the desktop
OpenGL compatibility profile, and may crash the frontend or fail to initialize
when the selected video driver does not provide the required fixed-function
OpenGL support. The option is omitted when Legacy3D is not part of the build,
including current macOS and normal GLES builds. This capability-based check
allows a future platform port to expose it without adding a platform-name
exception.

`Quad Rendering` is available only on builds that can negotiate an OpenGL 4.5
core context. It uses New3D's geometry-shader path to render Model 3
quadrilaterals directly and is ignored by Legacy3D. `CRT Colour` applies
Supermodel's own pre-output colour and gamma transform; it is separate from a
RetroArch shader and also applies only to New3D. All three settings require a
content restart because they affect renderer or OpenGL-context creation.

`Supersampling` exposes standalone Supermodel's native 1x-8x supersampling
pass on supported desktop OpenGL builds. It is applied on top of `Internal
Resolution`: 2x evaluates four samples per output pixel, 3x evaluates nine,
and 8x evaluates 64. The combined scale can consume substantial GPU time and
memory, and changes require restarting the content.

`Disable White Flash` mirrors standalone Supermodel's `NoWhiteFlash` setting.
It suppresses the white frame normally produced when a game disables 3D
rendering, is off by default, and requires restarting the content.

At resolutions above native, `2D Layer Upscaling Filter` selects Supermodel's
own `UpscaleMode` for tile layers before they are composited with the 3D scene.
At 496x384 the engine deliberately uses nearest-neighbor filtering regardless
of this option.

`SCSP DSP Engine` defaults to the current MAME-derived implementation. The
legacy ElSemi engine is retained as a compatibility choice for titles such as
Fighting Vipers 2; this is standalone's `LegacySoundDSP` setting. Both this
setting and the 2D filter require a content restart. Sound and DSB music volume
follow standalone's full 0–200% range. The separate standalone `EmulateDSB`
switch is intentionally not exposed.

## Timing and synchronization

`Model 3 Timing Mode` defaults to the same `60 FPS` video and `44100 Hz` stereo
audio cadence used by standalone Supermodel. This mode submits one fixed packet
of 735 stereo samples per `retro_run`. The optional native mode reports
57.524160 FPS and packetizes the unchanged 44.1 kHz stream into a deterministic
766/767-sample cadence, preserving exact long-term A/V synchronization without
resampling. A display capable of matching 57.524160 Hz provides the smoothest
result; native timing can judder on an ordinary fixed 60 Hz display. Changing
the mode requires restarting content. Native timing also requires either
multi-threaded emulation mode so the sound board can continuously fill the
44.1 kHz ring buffer; `Single Thread` remains intended for the default 60 Hz
mode.

The core does not expose its own VSync setting: the Libretro frontend owns the
display swapchain, video presentation, audio synchronization, and any final
sample-rate conversion. The core timing option changes the advertised cadence
and matching audio packet sizes together; it never changes only one side.

For objective performance checks, enable `Frame Timing Overlay`. Besides the
engine's PPC, renderer, GPU, synchronization and sound timings, it shows
61-frame averages for the engine, frontend audio submission, overlay, final
blit, miscellaneous core work, frontend presentation, total `retro_run`, the
worst frame, actual frontend cadence, engine capacity, and callback capacity. At 60 Hz,
sustained `retro_run` time must remain
below the 16.67 ms frame budget; `Present` may include the frontend's VSync wait.
`Actual` measures the real cadence between calls from the frontend, while
`Engine cap` is derived from Supermodel's frame execution alone. `Callback cap`
is the theoretical rate implied by all time spent inside `retro_run` and is not
the displayed frame rate. `Audio/pacing` may include intentional frontend
waiting when RetroArch audio synchronization is enabled.

For broad boot testing, `Scripts/libretro_smoke_test.py` enumerates the sets in
`Games.xml` and launches each available ROM in a separate temporary RetroArch
environment. `--seconds` is converted to a deterministic `--max-frames` count,
so it measures emulated time even when `--fast-forward` is enabled. On macOS,
the script records the actual RetroArch window through ScreenCaptureKit and
uses FFmpeg to extract the final video frame. Results include one named `.mov`,
final-frame `.png`, and log per set, generated isolated `.srm` files, and
CSV/JSON summaries containing missing ROMs, timeouts, non-zero exits, and crash
signals. These results are persistent and default to a timestamped folder under
`~/Documents/RetroArch/supermodel-smoke/`; only each game's disposable RetroArch
runtime environment uses temporary storage. The calling terminal needs macOS
Screen Recording permission. Run the script with `--help` for platform paths
and filters.

Controller mappings and RetroArch input labels are selected automatically from
the control signature declared by the loaded game. See
[Libretro control profiles](CONTROL_PROFILES.md) for the complete catalog and
the fallback rules.

`Input > Star Wars Trilogy Upright Mode X-Axis Inversion Fix` is enabled by
default and reverses the horizontal analog input only when Star Wars Trilogy
Arcade is configured as an Upright cabinet. Disable it if the frontend or input
device already applies that inversion. The option changes only input
interpretation and does not modify NVRAM.

NVRAM is normally persisted by the frontend as `<content>.srm`. To import a
standalone Supermodel save, place `<rom-set>.nv` directly in the frontend save
directory. The core reads `.nv` only when no `.srm` data was supplied. If both
are present, `.srm` takes precedence and the ignored `.nv` path is reported in
the log. `Automatic Initial NVRAM Setup` is enabled by default. When neither a
frontend `.srm` nor a valid native `.nv` exists, it uses the same field-level
NVRAM patcher as `NVRAM Settings` to select `Single`, `Stand Alone`, or `No
Link` in every supported linked-cabinet game and the `Upright` cabinet in Star
Wars Trilogy Arcade. It never copies a complete `.srm` or Backup RAM image and
does not disable lever feedback. Unrelated EEPROM fields retain the validated
smoke-test state. Existing saves are never
modified, and deleting a game's `.srm` regenerates the initial setup on its
next launch. For each affected released ROM set, the core starts from that
set's validated smoke-test EEPROM data so the field-level update exists before
the first emulated frame; Backup RAM is not copied. Disable the option before
first launch to retain the game's unconfigured factory defaults. The core
never writes or overwrites native `.nv` files.

`System > NVRAM Settings` provides optional game-aware editing and is disabled
by default. Every supported game has independent option keys. When enabled,
the menu displays only the parameters for the currently loaded game, and each
parameter contains all and only the values supported by that game. Every field
has a concrete game-specific default, marked `(Default)` in its value list. If
the game supports linked cabinets, its link field defaults to the corresponding
`Single`, `No Link`, or `Stand Alone` mode; Star Wars Trilogy Arcade defaults
to the `Upright` cabinet. If
the option is enabled, RetroArch becomes authoritative for every displayed
field: the selected values are applied at startup and therefore replace later
Service Menu changes. If it is disabled, the core does not modify these fields.
A game cannot inherit selections made for another game. Values are listed in
their stored order, which may differ from the order in which the Service Menu
cycles them. The available fields are derived from a controlled 353-sample
Service-menu campaign:

| Game family | Available settings |
| --- | --- |
| Sega Bass Fishing / Get Bass | Country, Difficulty |
| Daytona USA 2 | Country, Difficulty, Link Mode, Car Number, Cabinet Type, Vocal |
| Dirt Devils | Country, Difficulty, Link Mode, Machine Number |
| Emergency Call Ambulance | Difficulty |
| Fighting Vipers 2 | Country, Difficulty |
| Harley-Davidson | Country, Difficulty, Link Mode, Cabinet Number, Cabinet Type |
| L.A. Machineguns | Country, Difficulty, Cabinet Type |
| Le Mans 24 | Country, Difficulty, Link Mode, Cabinet Number, Cabinet Type, Special Car |
| The Lost World | Country, Difficulty |
| Magical Truck Adventure | Country, Difficulty, Christmas Mode |
| The Ocean Hunter | Country, Difficulty, Cabinet Type |
| Scud Race / Scud Race Plus | Country, Difficulty, Link Mode, Car Number, Cabinet Type |
| Ski Champ | Country, Link Mode, Cabinet Number |
| Spikeout / Spikeout Final Edition | Country, Difficulty, Link Mode |
| Sega Rally 2 | Country, Difficulty, combined Link Configuration, Cabinet Type, Default View |
| Star Wars Trilogy Arcade | Country, Difficulty, Cabinet Type |
| Virtua Fighter 3 | Country, Difficulty |
| Virtual On 2 | Country, Difficulty, Link Mode, Seat, Display Type |
| Virtual Striker 2 | Country, Difficulty |
| Virtual Striker 2 '98 | Country, Difficulty |
| Virtual Striker 2 '99 | Country, Difficulty |

Ordinary release clones inherit their parent's layout while preserving
revision-specific data. Prototypes, location tests (`lostwsgp`, `swtrilgyp`,
and `ecap`), and the `mgtrkbad` bad dump are not patched. Country is omitted
where that Service setting does not apply: `scudau`, `vs215o`, `vs29815`,
`vs299j`, and `vs29915j`. EEPROM games
receive the appropriate XMODEM, Sega-A3, or GENIBUS checksum and redundant-copy
updates. Fighting Vipers 2 and Virtua Fighter 3 instead patch their confirmed
Backup RAM operator bytes, which do not require a checksum. Restart content
before expecting the game to use a changed machine setting.

### Experimental linked cabinets

`System > Network Board` is the Libretro equivalent of standalone
Supermodel's `Network` setting. It is connected by default and is shown only
for games whose `Games.xml` entry includes a network board. Connecting it makes
the hardware visible to the game; this is also required for games such as Sega
Rally 2 that expose Network Assignments in the Service Menu only when the board
is present. Restart content after changing the option.

The same option enables experimental linked play when a supported RetroArch
Netplay session is active. The netpacket transport recognizes the Daytona USA
2, Harley-Davidson, Scud Race, Sega Rally 2, Ski Champ, and Spikeout Type 1
families, plus the Le Mans 24, Virtual-On 2, and Dirt Devils Type 2 families.
Set `System > Linked Cabinets` to the total number of cabinets expected in the
session; every instance must use the same value. The protocol accepts 2 to 16
cabinets. Runtime testing currently covers two-cabinet links, three-cabinet
Harley-Davidson and Dirt Devils links on macOS, and wired links between macOS
ARM64 and Batocera x86_64. Larger sessions and additional Type 2 role
combinations require broader cross-host validation. The standalone emulator's
SDL_net transport is not used or modified.

Configure both instances before starting Netplay:

- set `System > Network Board` to `Connected`, select the same
  `Linked Cabinets` value on every instance, and enable `NVRAM Settings`;
- set the RetroArch host to `Link Mode: Master`, normally with cabinet/car
  number 1;
- set every client to a non-Master link role supported by the game (`Slave`
  for Type 1, or the appropriate Slave/Relay/Satellite role for Type 2), using
  a different cabinet/car number where the game exposes one;
- restart the content on every instance, then host/join the same content
  through RetroArch Netplay.

The core uses RetroArch's official Netpacket interface exclusively: frontend
client identifiers, broadcast packets, callbacks, and receive polling provide
the complete transport. It opens no sockets, performs no private discovery or
IPC, and adds no fallback transport. The RetroArch host must be the Model 3
Master and every client must use a non-Master role supported by that game. The
core derives a common cabinet roster from
the frontend identifiers and reconstructs the standalone network ring's
per-cabinet segment order. RetroArch disables pause, fast-forward, rewind, and
Save State loading while the linked session is active. All currently recognized
Type 1 and Type 2 families use this API-native transport; untested cabinet
counts and role combinations remain future work.

Use a stable, low-latency wired LAN. The emulated cabinets exchange one network
segment in lockstep, matching standalone Supermodel's send/receive behavior;
network latency and jitter therefore stall emulation rather than being hidden
by RetroArch's normal input rollback. A 250 ms bounded wait detects a broken
link, but it is a safety timeout rather than a supported latency target.

## 🛠 Build Instructions

**Unified Makefile System:** This core uses a single unified `Makefile` supporting 6 platforms:
- **Linux/Unix** (native)
- **Windows** (MinGW cross-compile or native MSYS2)
- **macOS** (osxcross universal binary: x86_64 + arm64)
- **Android** (NDK: arm64, arm, x86_64, x86)
- **Raspberry Pi 64-bit** (RPi5, RPi4, generic aarch64)
- **Generic aarch64** (ARM64 Linux)

### Quick Start (All Platforms)

```bash
# Linux (default platform)
make -j$(nproc)

# Windows (cross-compile from Linux)
make platform=win -j$(nproc)

# macOS (osxcross from Linux)
make platform=osx -j$(nproc)

# Android (all architectures default to arm64)
make platform=android -j$(nproc)

# Raspberry Pi 64-bit
make platform=rpi64 -j$(nproc)

# Generic aarch64
make platform=aarch64 -j$(nproc)

# Debug build (any platform)
make DEBUG=1 platform=<platform> -j$(nproc)

# Clean build artifacts
make clean
```

---

### Linux (Native Build)

#### 1. Install Dependencies (Ubuntu/Debian)
```bash
sudo apt update
sudo apt install build-essential libgl1-mesa-dev libglu1-mesa-dev zlib1g-dev
```

#### 2. Compile
```bash
make -j$(nproc)
```

#### 3. Install
```bash
cp supermodel_libretro.so ~/.config/retroarch/cores/
```

---

### Windows

You can build the core either natively on Windows or via cross-compilation from Linux.

#### Option A: Native Windows Build (MSYS2)

1. **Install MSYS2**: Download and install from [msys2.org](https://www.msys2.org/).
2. **Open the "MSYS2 MinGW 64-bit" terminal** (avoid the default MSYS terminal).
3. **Install Dependencies**:
    ```bash
    pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-make mingw-w64-x86_64-zlib
    ```
4. **Compile**:
    ```bash
    make platform=win -j%NUMBER_OF_PROCESSORS%
    ```

#### Option B: Cross-Compilation (from Linux)

1. **Install the MinGW-w64 toolchain**:
    ```bash
    sudo apt update
    sudo apt install binutils-mingw-w64-x86-64 g++-mingw-w64-x86-64 libz-mingw-w64-dev
    ```
2. **Compile**:
    ```bash
    make platform=win -j$(nproc)
    ```

**Output:** `supermodel_libretro.dll` → Copy to RetroArch `cores/` directory.

---

### macOS (Intel & Apple Silicon Universal Binary)

This core generates a universal binary supporting both Intel (x86_64) and Apple Silicon (arm64 M1/M2/M3+).

#### Option A: Native macOS Build

If building on a macOS system with Xcode installed:

```bash
# Install dependencies via Homebrew
brew install zlib

# Compile (generates universal binary)
make platform=osx -j$(sysctl -n hw.ncpu)

# Install
cp supermodel_libretro.dylib ~/.config/retroarch/cores/
```

#### Option B: Cross-Compilation from Linux (Recommended for CI/CD)

1. **Install osxcross toolchain** (to `/opt/osxcross`):
    ```bash
    sudo mkdir -p /opt
    sudo git clone https://github.com/tpoechtrager/osxcross /opt/osxcross
    cd /opt/osxcross
    
    # Download macOS SDK (follow osxcross documentation for MacOSX12.0.sdk)
    wget -nc https://github.com/rtrussell/osxcross-build/releases/download/12.0/MacOSX12.0.sdk.tar.xz
    mv MacOSX12.0.sdk.tar.xz tarballs/
    
    # Build osxcross
    sudo ./build.sh
    ```

2. **Compile** (Makefile auto-detects osxcross):
    ```bash
    # Universal binary (x86_64 + arm64)
    make platform=osx -j$(nproc)
    
    # With debug symbols
    make platform=osx DEBUG=1 -j$(nproc)
    ```
    
    The Makefile automatically:
    - Detects osxcross at `/opt/osxcross`
    - Uses `o64-clang`/`o64-clang++` compilers
    - Configures SDK and deployment target
    - Generates universal binary for both architectures
    
    To override osxcross location:
    ```bash
    make platform=osx OSXCROSS_ROOT=/custom/path -j$(nproc)
    ```

3. **Transfer to macOS** (if building on Linux):
    ```bash
    scp supermodel_libretro.dylib user@mac:~/Downloads/
    # On Mac: cp ~/Downloads/supermodel_libretro.dylib ~/.config/retroarch/cores/
    ```

**Build Details:**
- **Deployment Target:** macOS 10.15 (Catalina) and newer
- **Architecture:** Universal binary (x86_64 + arm64)
- **Renderer:** New3D with OpenGL 4.1 (Legacy3D and Quad Rendering excluded)
- **Build Time:** ~90 seconds on 4 cores

---

### Android (NDK)

Requires Android NDK. The Makefile checks `~/Android/Sdk/ndk/28.2.13676358` by default.

```bash
# arm64 (default and recommended)
make platform=android -j$(nproc)

# 32-bit ARM (armv7-a with NEON)
make platform=android arch=arm -j$(nproc)

# x86_64
make platform=android arch=x86_64 -j$(nproc)

# x86 (32-bit)
make platform=android arch=x86 -j$(nproc)

# Custom NDK path
NDK_ROOT=/path/to/ndk make platform=android -j$(nproc)
```

**Build Features:**
- Automatic NEON optimization for ARM architectures
- Position-independent code (PIC) for all binaries
- Static C++ runtime linking
- API level 24 (NDK Clang toolchain)
- GLES 3.0 rendering support

**Output:** `supermodel_libretro_android.so` → Copy to Android RetroArch cores directory (usually `/data/data/com.retroarch/cores/`)

---

### Raspberry Pi 64-bit (RPi5, RPi4, aarch64)

Build for Raspberry Pi 4/5 with OpenGL ES 3.0 support.

#### 1. Install Cross-Compilation Toolchain (on build machine)

```bash
sudo apt update
sudo apt install aarch64-linux-gnu-gcc aarch64-linux-gnu-g++ zlib1g-dev:arm64
```

#### 2. Compile

```bash
# Raspberry Pi 5 (cortex-a76)
make platform=rpi64 -j$(nproc)

# or generic aarch64
make platform=aarch64 -j$(nproc)
```

The Makefile auto-tunes the CPU based on platform:
- **rpi5**: Cortex-A76 optimizations
- **rpi4-64**: Cortex-A72 optimizations
- **rpi64 (default)**: Cortex-A53 optimizations
- **aarch64**: Generic aarch64 (no CPU-specific tuning)

#### 3. Transfer to Raspberry Pi

```bash
scp supermodel_libretro_aarch64.so pi@raspberrypi:~/.config/retroarch/cores/supermodel_libretro.so
```

**Platform Features:**
- GLES 3.0 rendering via `glsym_es3`
- Legacy3D renderer excluded (GLES incompatible)
- Optimized for aarch64 ARM architecture
- Full force feedback support
- Synchronous 60 Hz audio (735 stereo samples per frame)

**Performance Notes:** On Raspberry Pi, performance varies by game and model:
- **RPi 5:** Can handle most games at native resolution
- **RPi 4:** Older/simpler titles (VF3, Daytona) run well; may need scaling for demanding titles
- **RPi 3:** Limited performance; resolution scaling recommended

---

### Generic aarch64 (Standalone ARM64 Linux)

For generic ARM64 Linux systems (not Raspberry Pi):

```bash
make platform=aarch64 -j$(nproc)
```

Same as RPi64 but without CPU-specific tuning. Suitable for:
- ARM64 servers
- Generic ARM development boards
- Amazon Graviton instances
- Other aarch64 Linux systems

---

## 🎮 Performance Notes
For performance-heavy titles (e.g., Sega Rally 2 or Daytona USA 2), ensure you are running the core in Release mode. This core uses synchronous audio; if your CPU cannot maintain the full 60 Hz emulation speed, you may experience audio stuttering.

On Raspberry Pi, performance depends on the specific model and game. Older titles (e.g., Virtua Fighter 3) run well on Pi 4/5, while demanding titles may require resolution scaling adjustments via RetroArch core options.
