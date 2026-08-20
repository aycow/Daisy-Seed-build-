# Audio Corruption Diagnostic Stages

The diagnostic framework isolates firmware additions without asserting a cause
for the audio corruption. `DIAG_STAGE_MINIMAL_BYPASS` is the default and matches
the physically verified direct stereo bypass baseline.

## Selecting A Stage

Normally, change only this line in `src/diagnostic_config.h`:

```cpp
#define DIAG_STAGE DIAG_STAGE_MINIMAL_BYPASS
```

The command line can override it without editing the header:

```powershell
make BUILD_DIR=build_stage3 DIAG_STAGE=3
```

Always confirm the compiler message before flashing:

```text
DIAG_STAGE=3 CONTROLS_INIT
DIAG_AUDIO_MODE=0, FX[P,R,S,G]=0,0,0,0, FORCE=0, THERMAL_AUDIO=0
```

On Windows, the vendored `make clean` target requires Unix `rm`. If unavailable,
remove only the verified project build directory with `Remove-Item -Recurse`, or
use a new `BUILD_DIR` for every stage as shown above.

## Stage Matrix

Every row includes all behavior above it. Stages 0 through 16 retain the
known-good direct stereo callback except Stage 10, which additionally writes the
input block to the tuner capture ring.

| Value | Selector | Newly constructed or enabled | New init/service work | Callback |
|---:|---|---|---|---|
| 0 | `DIAG_STAGE_MINIMAL_BYPASS` | `DaisySeed` only | `hw.Init`, 48 kHz, block 48; empty loop | direct stereo bypass |
| 1 | `DIAG_STAGE_MAILBOX_OBJECT` | `ControlMailbox` | none | direct stereo bypass |
| 2 | `DIAG_STAGE_CONTROLS_OBJECT` | `Controls` | none | direct stereo bypass |
| 3 | `DIAG_STAGE_CONTROLS_INIT` | unchanged | `Controls::Init`; ADC1 scan starts | direct stereo bypass |
| 4 | `DIAG_STAGE_CONTROLS_SERVICE` | unchanged | `Controls::Process` | direct stereo bypass |
| 5 | `DIAG_STAGE_TELEMETRY_OBJECT` | `Telemetry` | no UART init | direct stereo bypass |
| 6 | `DIAG_STAGE_TELEMETRY_INIT` | unchanged | `Telemetry::Init`; USART1 configured | direct stereo bypass |
| 7 | `DIAG_STAGE_TELEMETRY_TRAFFIC` | unchanged | normal controls `E/P/S` traffic | direct stereo bypass |
| 8 | `DIAG_STAGE_TUNER_OBJECTS` | `TunerCapture`, `TunerAnalyzer`, tuner buffers in cached SRAM | no tuner init | direct stereo bypass |
| 9 | `DIAG_STAGE_TUNER_INIT` | unchanged | capture `Init`, analyzer `Reset` | direct stereo bypass |
| 10 | `DIAG_STAGE_TUNER_CAPTURE` | unchanged | unchanged | capture input, then direct stereo bypass |
| 11 | `DIAG_STAGE_TUNER_ANALYSIS` | source window | `CopyAnalysisWindow` and YIN `Analyze` | capture plus direct stereo bypass |
| 12 | `DIAG_STAGE_THERMAL_OBJECT` | `ThermalMonitor` | no thermal init | direct stereo bypass |
| 13 | `DIAG_STAGE_THERMAL_INIT` | unchanged | internal sensor/ADC3 init only; no read or conversion | direct stereo bypass |
| 14 | `DIAG_STAGE_THERMAL_SERVICE` | unchanged | first and recurring thermal `Service`; faults tracked/reported without loop `continue` | direct stereo bypass; never thermal-muted |
| 15 | `DIAG_STAGE_AUDIOENGINE_OBJECT` | core `AudioEngine`; optional effect members | no engine init | direct stereo bypass |
| 16 | `DIAG_STAGE_AUDIOENGINE_INIT` | unchanged | engine `Init`; enabled effect `Init` calls | direct stereo bypass |
| 170 | `DIAG_STAGE_AUDIOENGINE_PROCESS_DRY` | unchanged | prior foreground work | `AudioEngine::Process`, raw mono dry only |
| 171 | `DIAG_STAGE_AUDIOENGINE_PROCESS_MAILBOX` | unchanged | unchanged | 170 plus sequence-counter mailbox read |
| 172 | `DIAG_STAGE_AUDIOENGINE_PROCESS_TUNER` | unchanged | unchanged | 171 plus tuner capture |
| 173 | `DIAG_STAGE_AUDIOENGINE_PROCESS_TRANSITION` | unchanged | unchanged | 172 plus wet-gain/transition path |
| 174 | `DIAG_STAGE_AUDIOENGINE_PROCESS_THERMAL` | unchanged | latched thermal mode is published; thermal audio enforcement enabled | 173 plus thermal fade/mute handling |
| 175 | `DIAG_STAGE_AUDIOENGINE_PROCESS_DISPATCH` | unchanged | thermal state still monitored/reported but not published to audio | transition/dispatch path; forced tuner/no effect call |
| 180 | `DIAG_STAGE_FX_PHASER` | phaser only | phaser init; CPU telemetry; thermal audio enforcement disabled | full engine, forced phaser only |
| 181 | `DIAG_STAGE_FX_REVERB` | reverb only | reverb init; CPU telemetry | full engine, forced reverb only |
| 182 | `DIAG_STAGE_FX_PITCH_SHIFTER` | pitch shifter only plus 48,000-byte SDRAM history | pitch shifter init; CPU telemetry | full engine, forced pitch shifter only |
| 183 | `DIAG_STAGE_FX_GRANULAR` | granular only plus SDRAM history | granular init; CPU telemetry | full engine, forced granular only |
| 1000 | `DIAG_STAGE_PRODUCTION` | all production objects/effects | full production startup and foreground loop | full production AudioEngine |

## Effect Construction And Init Isolation

Stages 15 and 16 default to no effect members. Enable exactly one member without
changing the callback:

```powershell
make BUILD_DIR=build_construct_phaser DIAG_STAGE=15 DIAG_FX_PHASER=1
make BUILD_DIR=build_init_phaser      DIAG_STAGE=16 DIAG_FX_PHASER=1
```

Replace the flag with `DIAG_FX_REVERB`, `DIAG_FX_PITCH_SHIFTER`, or
`DIAG_FX_GRANULAR`. Disabled concrete members do not exist in `AudioEngine`,
their dispatch slots are null, and their init/map/process code is not referenced.
The granular implementation and SDRAM buffer are compiled out when granular is
disabled. Tuner implementation and cached-SRAM buffers are compiled out before
Stage 8.

## Confirmed DMA Placement Failure

Hardware testing isolated the original corruption to Stage 8. The four CPU-owned
tuner arrays had consumed 45,056 bytes of `.sram1_bss`, moving
`dsy_audio_tx_buffer`, `dsy_audio_rx_buffer`, and `adc1_dma_buffer` above
libDaisy's non-cacheable MPU window at `0x30000000-0x30008000`. Original Stage
8 produced repeating tonal corruption; relocating only those arrays to cached
SRAM restored clean bypass audio. This causally confirmed the placement bug.

Tuner storage and the foreground source window must remain in ordinary cached
SRAM. `DMA_BUFFER_MEM_SECTION` is reserved for actual DMA sources and
destinations. The application does not expand libDaisy's MPU region or modify
the linker script to accommodate CPU-only buffers.

## Audio Modes

`DIAG_AUDIO_MODE` defaults to direct stereo through Stage 16 and AudioEngine for
later stages. Optional modes are:

- `DIAG_AUDIO_DIRECT_STEREO_BYPASS`
- `DIAG_AUDIO_DIRECT_MONO_BYPASS`
- `DIAG_AUDIO_SINE_440`
- `DIAG_AUDIO_SILENCE`
- `DIAG_AUDIO_ENGINE`

AudioEngine process stages require `DIAG_AUDIO_ENGINE`; incompatible selections
fail at compile time. Effect objects require Stage 15 or later. Forced effects
require the corresponding compiled member and an actual effect-processing profile.
Production requires all four effects.

`DIAG_ENFORCE_THERMAL_AUDIO_SHUTDOWN` defaults to true only for Stage 174 and
production. It defaults to false for Stage 175 and Stages 180-183, so a thermal
sensor fault can still be recorded/reported without muting an unrelated engine
or effect isolation test. The compiler banner reports this as `THERMAL_AUDIO=0`
or `THERMAL_AUDIO=1`.

Actual effect profiles enable `AUDIO_CPU_LOAD_DEBUG` and emit foreground-only:

```text
D,CPU,<average_x10000>,<peak_x10000>,<overruns>,<effect>,<effect_peak_x10000>
```

No UART or formatting occurs in the callback.

## Hardware Test Sequence

1. Build and flash Stage 0; reconfirm the known-good stereo bypass.
2. Advance exactly one row at a time through Stage 14.
3. Stop at the first row that changes clean guitar into buzz and repeat the prior/current pair.
4. At Stage 15, test core-only, then each single-effect construction flag.
5. At Stage 16, repeat core-only and each single-effect init flag.
6. Advance through 170, 171, 172, 173, 174, and 175.
7. Only after 175 is clean, test 180 through 183 one at a time.
8. Record the compiler stage message, binary name, observed audio, UART state, and LED state for every flash.
9. Do not skip directly to a suspected subsystem; the first failing boundary is the evidence this framework is designed to produce.

No build result proves hardware audio behavior. Every stage still requires a
physical flash/listen test on the assembled board.
