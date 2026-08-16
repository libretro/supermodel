# Libretro Port Maintenance

This document records the ancestry and maintenance model of the modern
Supermodel Libretro port. The goal is to make updates reviewable for the
official Libretro repository and easy to exchange with the developers whose
work made the port possible.

## Repository ancestry

The `supermodel-modern` branch deliberately preserves three sources:

| Role | Repository | Recorded revision |
| --- | --- | --- |
| Historical Libretro lineage | `libretro/supermodel` (forked as `Zer0one/supermodel`) | `44ba6a72c820fec474c2b3a748ec409c8f8e2090` |
| Emulator engine | `trzy/Supermodel` (`upstream`) | `24d2ffcfc7f14229337f05f4920fe26b56633d9d` |
| Modern Libretro frontend | `sgiannop/Libretro-Supermodel` | `1c094999664b23876323a50815022bad1f353a04` |

The first modernization commit is a real two-parent merge. Its first parent is
the historical Libretro branch, its second parent is the current Supermodel
revision, and its tree is exactly the Supermodel upstream tree. This avoids a
source dump with no ancestry and lets later engine updates use ordinary Git
merges.

The next commit imports sgiannop's frontend-owned files without modifying them.
Adaptations required by the newer engine are kept in later commits. Reviewers
can therefore distinguish existing port work from new compatibility changes,
and sgiannop can cherry-pick useful adaptation commits without re-importing his
own frontend.

## Ownership boundaries

Keep future changes within these boundaries whenever practical:

- `Src/OSD/libretro/`, the root `Makefile`, `jni/`, and
  `supermodel_libretro.info` are the Libretro integration layer.
- The rest of `Src/` follows `trzy/Supermodel`. Engine changes needed only by
  Libretro should be small, conditional on `SUPERMODEL_OSD_LIBRETRO`, and kept
  in focused commits.
- Third-party snapshots such as `libretro-common` and `deps/ugui` should be
  updated separately from engine synchronization.

These boundaries are guidelines rather than a claim of independent ownership:
the imported frontend remains derived from and credited to sgiannop's work.

## Updating the Supermodel engine

The configured `upstream` remote points to `https://github.com/trzy/Supermodel.git`.
After the initial ancestry merge, normal update merges are sufficient:

```sh
git fetch upstream
git switch supermodel-modern
git merge --no-ff upstream/master
```

Resolve engine changes in the engine-facing bridge first, then rebuild every
supported platform. Do not replace the engine tree wholesale or squash the
upstream merge: either action would discard the relationship this branch was
created to preserve.

## Following sgiannop's frontend

Adding a read-only reference remote is optional but useful during development:

```sh
git remote add sgiannop https://github.com/sgiannop/Libretro-Supermodel.git
git fetch sgiannop
git log --oneline 1c094999664b23876323a50815022bad1f353a04..sgiannop/master
```

Review new commits one at a time. Cherry-pick frontend fixes when they apply
cleanly, then place any adaptation to the newer engine in a following commit.
This preserves authorship and makes the result useful in both directions.

Conversely, sgiannop does not need the import commit, because it contains his
own work. He can fetch this repository and cherry-pick only the focused
post-import compatibility commits that are useful to his branch.

## Pull request policy

The primary integration target is `libretro/supermodel`. Pull requests should:

1. Credit `sgiannop/Libretro-Supermodel` prominently and name the exact source
   revision used.
2. Preserve imported frontend work and new adaptations as separate commits.
3. Describe the exact Supermodel upstream revision and retain its merge
   ancestry.
4. List tested hosts, architectures, frontends, games, renderers, input paths,
   audio, save states, and known limitations.
5. Avoid claiming platform support that has not been revalidated after an
   upstream update.

## Libretro continuous integration

`.github/workflows/libretro-ci.yml` builds the actual Libretro core rather
than the standalone Supermodel executable. Its desktop matrix currently
covers Linux x86_64, Windows x86_64, macOS ARM64, and macOS x86_64.

Each job starts from a clean checkout, builds through the unified root
`Makefile`, verifies the binary format and target architecture, checks the
required Libretro API exports, and audits runtime dependencies. The uploaded
artifact contains the core, `supermodel_libretro.info`, and the required
system assets under `system/supermodel/`.

Compilation proves that the integration layer remains portable; it does not
replace real-ROM runtime validation. A platform should only be described as
runtime-tested after loading content in a frontend on that platform.

## Renderer capabilities

Renderer options are controlled by build capabilities rather than runtime
platform-name checks:

| Define | Core behavior |
| --- | --- |
| `HAVE_LEGACY3D` | Exposes runtime selection between New3D and experimental Legacy3D and negotiates a desktop OpenGL compatibility context for the latter. Forced `RENDERER=legacy` builds remain fixed to Legacy3D. |
| `HAVE_QUAD_RENDERING` | Exposes New3D native-quad rendering and requests an OpenGL 4.5 core context when enabled. |
| `HAVE_CRT_COLOURS` | Exposes Supermodel's native New3D pre-output colour/gamma transforms. |
| `HAVE_SUPERSAMPLING` | Exposes Supermodel's native 1x-8x supersampling pass on desktop OpenGL builds. |

The root Makefile derives these defines from the renderer sources and graphics
API supported by each build. A future macOS or GLES implementation should
enable the corresponding capability only after its renderer path and Libretro
context negotiation are actually supported. Legacy3D deliberately carries a
frontend-crash warning: the compatibility profile and fixed-function entry
points cannot be made safe through a fallback after context creation.

## Standalone timing modes

After reviewing the remaining standalone settings, `RefreshRate` and
`SoundFreq` are exposed as one atomic `Model 3 Timing Mode`. The default keeps
standalone's 60 Hz cadence. Native mode advertises 57.524160 FPS while retaining
the original 44.1 kHz stereo stream. An integer remainder accumulator submits a
deterministic sequence of 766/767-sample packets whose long-term average is
exactly `44100 / 57.524160`; no resampling or measured-performance feedback is
used. Native timing requires either multi-threaded emulation mode so the sound
board can continuously fill the 44.1 kHz ring buffer. A content restart is
required.

Other standalone-only UI, audio-routing, Legacy3D customization, and
bitmap-crosshair settings are intentionally not planned as Libretro Core
Options.

## CPU options

`Emulation Threading` represents the three meaningful combinations of the
standalone `MultiThreaded` and `GPUMultiThreaded` settings. `Single Thread`
disables both; `Multi-threaded` runs the sound and drive boards on worker
threads; and the default `Multi-threaded + GPU` mode additionally overlaps
PowerPC emulation with graphics processing. The option is internal to
Supermodel and is independent of RetroArch's frontend-side `Threaded Video`
setting. A content restart is required after changing it.

`Emulated PowerPC Frequency` is the Libretro representation of standalone
Supermodel's `PowerPCFrequency` setting. `Auto` preserves the current upstream
stepping defaults (66, 100, or 166 MHz); the explicit values are emulated CPU
clocks, not host-CPU or frontend performance settings. A content restart is
required after changing the option.

The ARM64 PowerPC JIT is imported from sgiannop's Libretro work and kept behind
the build-time `HAVE_PPC_JIT` capability. It is shown only by targets that
actually compile the backend (Android ARM64, Raspberry Pi 64-bit, and generic
Linux AArch64, plus native macOS ARM64), is labelled experimental, and is
disabled by default. The interpreter remains the correctness reference.

Native floating-point translation is currently disabled inside the ARM64 JIT,
while integer, control-flow, and memory instructions remain JIT-compiled. This
targeted fallback fixes a reproducible gameplay divergence in Harley-Davidson:
with native FP translation, the attract-mode riders repeatedly collide with
walls; interpreting PPC floating-point instructions restores the reference
behaviour and retains substantial JIT performance headroom. Future work should
identify the exact FP/FPSCR semantic mismatch, validate it instruction by
instruction against the interpreter, and re-enable native FP only after broad
gameplay regression testing.

On Apple Silicon the backend allocates its code cache with `MAP_JIT` and uses
`pthread_jit_write_protect_np()` to alternate the current thread between write
and execute access while emitting blocks. This follows the established pattern
used by ARM64 Libretro dynarecs such as Flycast and ParaLLEl N64. It also
requires the frontend executable to carry Apple's `allow-jit` entitlement; if
allocation is denied, the core logs the failure and falls back to the PowerPC
interpreter. Native Intel and osxcross universal builds do not expose this JIT.

No `Frame Skip` core option is exposed. Current standalone Supermodel has no
equivalent setting, and the former Libretro implementation only suppressed the
final frontend presentation while `CModel3::RunFrame()` still performed the
internal render. Frame duplication or dropping therefore remains frontend
policy.

Detailed macOS profiling shows that `audio_batch_cb` can spend roughly 0–9 ms
per frame in frontend pacing. RetroArch 1.22.2's synchronous audio path accepts
each complete Libretro batch and may block while the output FIFO drains; this
variable time therefore completes the 16.67 ms frame budget rather than
representing additional Supermodel audio emulation. With the experimental
ARM64 JIT enabled, the measured engine cost falls to about 5.4 ms in Daytona 2
and 4.9 ms in Sega Rally 2, while callback-inclusive capacity remains governed
largely by frontend pacing. Do not treat that capacity as pure emulator compute
capacity, and do not replace the synchronous path merely to reduce the timing
overlay value: the current output is clean and stably paced at 60 Hz.

The Libretro batch callback returns the number of frames processed. RetroArch
1.22.2 currently returns the complete requested count after routing the batch
through its audio pipeline, but the core still preserves any unaccepted frames
in its ring buffer for compatibility with other conforming frontends. A partial
submission is reported once at shutdown if it occurred during the session.

RetroArch audio rate control requires a driver that exposes buffer occupancy.
The SDL2 audio driver does not implement that interface and emits `Rate control
was desired, but driver does not support needed features`; macOS `coreaudio3`
does implement it and removes the warning. This is a frontend audio-driver
capability, not a Supermodel core option. The controller driver is independent
and may remain SDL2.

A 90-second Daytona 2 interpreter run measured 7.93 ms in the PowerPC and
7.95 ms in the engine, versus 5.37 and 5.43 ms respectively in the comparable
JIT run. This is a roughly 32% reduction in CPU/engine time and raises measured
engine capacity from about 126 to 184 FPS. Overall callback capacity improves
only from about 78 to 84 FPS because the frontend consumes much of the freed
frame budget in audio/pacing waits.

## Current validation and limitations

The Libretro timing contract offers standalone Supermodel's default 60 Hz mode
and native 57.524160 Hz mode. Both report 44100 Hz stereo audio. The former
submits 735 samples per frontend frame; the latter uses the exact fractional
766/767-sample packetizer described above. Native timing requires a
multi-threaded emulation mode; `Single Thread` remains intended for 60 Hz.
VSync remains frontend-owned.

Validated on macOS ARM64, with additional Batocera x86_64 coverage noted
below:

- clean compilation and linking with `make platform=osx`;
- Mach-O ARM64 dynamic library output;
- dynamic loading without SDL or SDL_net dependencies;
- loading by RetroArch 1.22.2 with VFS v3, core options v2, hardware OpenGL,
  XRGB8888, and full-path content negotiation;
- loading and CRC validation of the real `dayto2pe` ROM set through the
  current upstream `Games.xml`;
- discovery of `Games.xml` and `Music.xml` in the preferred flat
  `system/supermodel` directory without creating a `Supermodel.ini`;
- negotiation of an OpenGL 4.1 core profile, successful creation of the
  Libretro and Real3D framebuffers, and sustained execution of the frame loop;
- visible boot of `dayto2pe` beyond its network-board check with standalone
  Supermodel NVRAM configured for a single cabinet;
- successful `MAP_JIT` allocation and sustained ARM64 JIT execution in Daytona
  2 and Sega Rally 2, with no evident gameplay, graphics, or audio regressions;
- correct New3D rendering of Star Wars Trilogy Arcade's close-range Death Star
  surface after compiling the 3D renderers without fast-math, preserving the
  engine's explicit NaN/Inf fog-value sanitization;
- visually aligned 61-frame averages in the optional Frame Timing Overlay;
- service-mode validation of the dedicated fishing profile: rod and fishing
  stick axes, analog `00-FF` reel speed, cast, and select; the Japan Standard
  `getbass` set additionally exposes and correctly reads the tension axis;
- automated Save State save/load cycles for Daytona 2 PE, Sega Rally 2, and
  Ocean Hunter, including a new-session load, legacy-state compatibility,
  cross-game rejection, and checksum-corruption rejection; interactive loads
  in all three games also restored their relevant video, audio, controls, and
  force-feedback state without an observed anomaly;
- exported `retro_init`, `retro_load_game`, `retro_run`, save-state, and unload
  entry points.
- real-ROM runtime on Batocera 43.1 x86_64 using the CI-built Linux core,
  including New3D, audio, controller profiles, NVRAM, Save States, force
  feedback, CPU-speed options, and the standard 60 Hz timing mode;
- wired cross-host Netpacket sessions between macOS ARM64 and Batocera x86_64,
  including linked gameplay in Daytona USA 2 and Dirt Devils; localhost testing
  additionally covers the implemented Type 1 and Type 2 families described
  below.

Still requiring validation or implementation:

- visual correctness and extended gameplay testing;
- extended audio, controls, force-feedback, and Save State coverage across more
  games;
- Windows, Android, Raspberry Pi, generic AArch64 Linux, and the remaining
  advertised targets that have CI or build coverage but no equivalent real-ROM
  runtime campaign;
- diagnose and correct the ARM64 JIT floating-point/FPSCR semantic mismatch
  exposed by Harley-Davidson's attract-mode AI, then replace the current FP-only
  interpreter fallback after cross-game regression testing;
- broader PPC JIT compatibility testing on macOS ARM64, plus runtime validation
  on Android ARM64, Raspberry Pi 64-bit, and generic Linux AArch64; compilation
  alone does not establish game compatibility;
- linked-cabinet runtime validation beyond three cabinets and broader
  cross-host validation of the implemented Type 1 and Type 2 families;
  automatic initial NVRAM setup continues to select Single/No Link for known
  affected revisions when linked play is not configured.

Libretro save RAM is canonical and is persisted by the frontend as a fixed-size
`.srm`. Its payload uses the same block container as standalone Supermodel
NVRAM, with zero padding after the final block. A native `<rom-set>.nv` in the
frontend save directory is imported only when the frontend supplied no `.srm`
data; when both sources exist, `.srm` wins and the decision is logged. The core
never modifies `.nv`; if native import fails, default NVRAM is initialized and
persisted to a new `.srm`. With `Automatic Initial NVRAM Setup` enabled, the
core uses the same field-level patcher and checksum implementations as the
game-specific NVRAM options whenever no valid save source exists. It selects
the available `Single`, `Stand Alone`, or `No Link` value in every supported
linked-cabinet game and the `Upright` cabinet in Star Wars Trilogy Arcade. No
complete `.srm` or Backup RAM image is copied, lever feedback is not modified,
and unrelated operator fields retain the smoke-test factory state. This is a
one-time initialization: an existing
`.srm` or valid `.nv` is never patched, and deleting the `.srm` explicitly
requests regeneration. A blank EEPROM does not yet contain a game-specific
layout, so every affected released ROM set has its own 64-word seed extracted
from the validated smoke-test campaign. The seed is installed before the first
emulated frame and then passed through the normal field patcher; Backup RAM is
left pristine. Older headerless `.srm` files from the initial Libretro port
remain readable.

### Model 3 EEPROM checksums

The 93C46 device does not impose a checksum. Each game writes and validates its
own 64-word EEPROM layout. Analysis of first-boot, Service-configured, embedded
template, and standalone NVRAM samples identified three checksum families. The
word values below are the 16-bit values seen by the emulated machine; bytes in
each word are therefore processed most-significant byte first.

| EEPROM format | Checksum | Covered logical bytes |
| --- | --- | --- |
| `M3SEGA`, format word `0xA3xx` | Sega CRC-CCITT variant described below, stored in word 3 | 12 through 69 |
| Other `M3SEGA` layouts | CRC-16/XMODEM (`poly=0x1021`, `init=0x0000`, `xorout=0x0000`), stored in word 3 | 12 through 69 |
| Tagged `M3SEGA SRC2` and `M3SEGA SWTA` layouts | CRC-16/GENIBUS (`poly=0x1021`, `init=0xFFFF`, `xorout=0xFFFF`), stored in word 5 | 16 through 127 |

The `0xA3xx` games use the standard 256-entry `0x1021` lookup table but a
non-standard 32-bit working state. The game routine is equivalent to the
following pseudocode, where `table` contains the normal big-endian CRC-CCITT
table and all arithmetic is 32-bit:

```text
state = 0xDEBDEB00
for byte in eeprom[12..69]:
    index = rotl32(state, 9) & 0x1FE
    state = ((state ^ (table[index / 2] << 8)) | byte) << 8
index = rotl32(state, 9) & 0x1FE
state ^= table[index / 2] << 8
checksum = rotl32(state, 24) & 0xFFFF
```

This classification reproduced all 336 checksum-bearing samples in the final
controlled campaign: 155 XMODEM, 150 Sega-A3, and 31 tagged GENIBUS images.
The remaining 17 of the 353 samples established that Fighting Vipers 2 and
Virtua Fighter 3 keep these operator settings in Backup RAM rather than in
their erased/older EEPROM layouts. Their confirmed byte offsets are 94
(Difficulty) and 111 (Country) for Fighting Vipers 2, and 122886 (Difficulty)
and 122910 (Country) for Virtua Fighter 3. Boot-and-clean-close tests verified
that these four bytes require no additional Backup RAM checksum.

Checksum support alone is not sufficient to patch settings safely. The core
uses a per-family descriptor for each exposed value and replaces only the
confirmed byte or masked word field. It preserves unrelated bits, revision
data, and sequence counters; duplicated XMODEM records are updated atomically.
All 353 parent values and 511 applicable release-clone values were replayed
against the campaign samples after implementation.

The Sega Rally 2 samples additionally locate `Default View` at bit `0x0004`
of EEPROM word 12: clear selects `Driver`, while set selects `Behind`. The
Service Menu also advances word 7, but that sequence counter is deliberately
preserved; the patcher changes only the confirmed view bit and regenerates the
GENIBUS checksum in word 5. The same layout and `Driver` default apply to the
released `srally2dx` clone through its independent per-game option key.

For Daytona USA 2, controlled Service-menu samples locate `Country` in the
high byte of EEPROM words 12 and 41 (primary and redundant settings copies):

| Country | Stored value |
| --- | --- |
| Japan | `0x01` |
| USA | `0x02` |
| Export | `0x03` |
| Australia | `0x04` |
| Korea | `0x05` |

The settings sequence counter is duplicated in words 6 and 35. Entering and
Saving through the Service menu also normalized unrelated fields in the
original template. The game-aware NVRAM settings implementation therefore does
not copy those incidental changes and updates only
both Country copies and the checksum. It leaves the sequence counter unchanged
because both redundant settings copies are replaced atomically.

Additional controlled Daytona USA 2 samples locate the Link, Car Number, and
Cabinet settings in the same two settings copies:

| Setting | Primary / redundant words | Stored value |
| --- | --- | --- |
| Link: Single | 15 / 44, high byte | `0x00` |
| Link: Master | 15 / 44, high byte | `0x01` |
| Link: Slave | 15 / 44, high byte | `0x02` |
| Link: Live | 15 / 44, high byte | `0x03` |
| Car Number | 15 / 44, low byte | displayed number minus one (`0x00` through `0x0F`) |
| Cabinet: Deluxe | 16 / 45, high byte | `0x00` |
| Cabinet: Twin | 16 / 45, high byte | `0x01` |

The low byte of words 16 and 45 was unchanged in the Cabinet samples and must
therefore be preserved. Each Service save advanced both copies of the sequence
counter by one; samples matching the base value (`Slave`, car 1, and `Twin`)
confirm the base configuration rather than introducing a third encoding.

`System > NVRAM Settings` is disabled by default. Enabling it dynamically
exposes only the fields supported by the loaded ROM set. At initial core-option
registration, every supported parent and clone receives independent persistent
keys for every parameter it supports. Each key has a concrete default read from
that game's smoke-test NVRAM, except that every linked-cabinet field defaults to
the game's non-linked `Single`, `No Link`, or `Stand Alone` mode and Star Wars
Trilogy Arcade defaults to the `Upright` cabinet. The selected default is
labelled `(Default)` and there is no `Keep Current` sentinel.
Consequently a selection for one set cannot leak
into another, while each visible parameter still offers all and only the values
valid for that set. When enabled, all displayed selections are applied through
Supermodel's normal NVRAM block interface after the frontend `.srm` or native
`.nv` has been loaded, making the frontend authoritative over later Service
Menu changes. When disabled, the patcher is not invoked. Options are ordered by
their progressively encoded stored values except where a composite field (for
example Sega Rally 2's role/car configuration) requires semantic labels.

Parent layouts are shared only with structurally compatible release clones.
Prototypes, location tests (`lostwsgp`, `swtrilgyp`, and `ecap`), and
`mgtrkbad` are excluded. Country is selectively
hidden for `scudau`, `vs215o`, `vs29815`, `vs299j`, and `vs29915j`, where the
corresponding regional Service Menu does not expose that setting. No complete
sample EEPROM or Backup RAM image is ever copied over an existing save.

## Libretro linked-cabinet transport

The Libretro backend registers `RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE` in
`retro_init()`. This lets RetroArch manage peer discovery and transport while
the core sends the actual emulated network-board data. The bundled
`libretro.h` predates the API, so `LibretroNetPacket.h` backports only the
official compatible declarations instead of replacing the entire vendored
libretro-common snapshot.

`CLibretroNetBoard` is independent from standalone `CSimNetBoard`: standalone
continues to use its SDL_net TCP ring. Board presence and linked transport are
separate in the Libretro backend: `System > Network Board` mirrors
standalone's `Network` boolean and attaches the board for every Type 1 or Type
2 family recognized by `CSimNetBoard`. This lets games detect the hardware and
expose board-dependent Service Menu pages even when no Netplay session exists.
The option defaults to connected and is hidden for games without a network
board.

The version-2 Libretro transport recognizes the Daytona USA 2,
Harley-Davidson, Scud Race, Sega Rally 2, Ski Champ, and Spikeout Type 1
families, plus the Le Mans 24, Virtual-On 2, and Dirt Devils Type 2 families.
It accepts a configured total of 2 to 16 cabinets. RetroArch client 0 must be
the Model 3 Master and every other client a non-Master role supported by that
game. Every participant broadcasts its role and expected cabinet count, then
derives the same sorted roster from the frontend-provided client identifiers.
After this handshake, each core broadcasts one reliable segment per emulated
network-board frame and rebuilds the circular communication-RAM order relative
to its own roster position. The final local segment is copied directly,
avoiding a redundant loopback packet.

This implementation is intentionally API-native and KISS. It uses only the
official Libretro Netpacket interface, including RetroArch-assigned client
identifiers, broadcasts, callbacks, and receive polling. It opens no core-owned
sockets, performs no private peer discovery or IPC, and provides no hidden
fallback transport. Standalone `CSimNetBoard` and its SDL_net TCP ring remain
unchanged.

This exchange deliberately remains lockstep. The official netpacket API's
`RETRO_NETPACKET_FLUSH_HINT` already flushes without blocking, and
`retro_netpacket_poll_receive_t` explicitly supports a short bounded receive
loop; the backend uses both. Standalone `CSimNetBoard` likewise sends and then
waits for the corresponding segment every network-board frame. RetroArch's
ordinary Netplay input prediction and rollback do not apply to custom
netpacket payloads, so replacing the wait with stale or predicted cabinet data
would change the emulated protocol rather than optimize its transport.

A real wired IPv4 Daytona USA 2 link between RetroArch 1.22.2 on Batocera
x86-64 and RetroArch 1.21.0 on macOS ARM64 remained synchronized without
timeouts. A controlled one-way 100 ± 25 ms delay kept the logical link alive
but reduced the Batocera instance to roughly 14 FPS. This confirms that the
current mode is intended for a stable low-latency LAN; the 250 ms wait is only
a bounded failure guard and must not be interpreted as a performance target.

The transport uses explicit little-endian protocol headers, validates the game
family, role, segment size, frame number, and protocol version, and performs a
short bounded receive poll inside `retro_run()`. A four-frame ready barrier
prevents one frontend from entering the first blocking exchange while its peer
is still completing the Netplay handshake. Localhost tests with isolated
Master/Slave NVRAM reached normal linked gameplay in Daytona USA 2 Power
Edition, Scud Race, Sega Rally 2, and Harley-Davidson. Harley-Davidson also
completed a three-instance linked race with a stable link and no observed
protocol divergence; Dirt Devils completed the corresponding three-instance
Type 2 test. Wired macOS ARM64/Batocera x86_64 sessions validated Daytona USA 2
and Dirt Devils across two physical hosts. Running three complete Model 3
instances on the test Mac reduced available performance headroom, so this
validates the three-cabinet protocol rather than establishing a performance
target. Values from 4 to 16 remain structurally supported but require runtime
validation. Type 2 uses its separate status and playable/relay index layout;
additional game-specific role combinations still require broader cross-host
coverage.

Libretro Save States use standalone Supermodel's current version-6 header and
ROM-set identifier before the normal subsystem blocks. This rejects states
from an incompatible engine format or different game before any emulator
memory is modified. A final Libretro-only integrity block contains a CRC32 of
the complete preceding payload, allowing padded truncation and in-place data
corruption to be detected even when a frontend supplies the expected buffer
size. Standalone Supermodel safely ignores this additional named block. The
core accepts standalone-compatible states without the integrity block and the
older headerless Libretro layout, logging which compatibility path was used.

Before loading, the core compares every block name and payload size against a
layout generated from the running game. Memory reads are bounded to the
selected block, and buffer overflow or underflow now propagates failure through
`retro_serialize()` or `retro_unserialize()` instead of returning success after
a partial operation.
