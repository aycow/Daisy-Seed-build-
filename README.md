# project006

Daisy Seed/Seed3 guitar-pedal firmware for tuner, chorus, reverb, crusher, granular delay, footswitch bypass, analog rotary selection, and UART telemetry to a Raspberry Pi.

Compile-time audio-corruption isolation stages, build commands, and the hardware
test sequence are documented in [DIAGNOSTICS.md](DIAGNOSTICS.md).

## Runtime Architecture

Main loop:
- Samples and validates the STM32 die temperature outside the audio callback.
- Initializes and reads ADC channels for three pots plus the six-position rotary resistor ladder.
- Debounces the footswitch and analog rotary selector.
- Smooths pot values, applies deadbands, and publishes changed parameters through the control mailbox.
- Handles rotary calibration telemetry when explicitly enabled.
- Performs non-real-time tuner analysis.
- Emits UART telemetry to the Raspberry Pi.

Control mailbox:
- Transfers compact requested control state across the main-loop/audio-callback boundary.
- Uses a sequence-counter publication pattern around a trivially copyable snapshot.
- Carries effect id, bypass state, latched system mode, and three fixed-point pot parameters.

Audio callback:
- Consumes one valid snapshot at the start of each block.
- Exclusively owns and mutates chorus, reverb, crusher, and granular-delay DSP objects after audio starts.
- Applies changed effect parameters only when the mailbox revision changes.
- Performs effect/bypass transition ramps before skipping or switching DSP objects.
- Processes real-time audio and captures tuner samples into a power-of-two ring.
- Performs no UART, formatting, ADC reads, GPIO debounce, locks, allocation, delays, or tuner analysis.
- During a thermal fault, fades both outputs to zero, stops effect processing and tuner capture, and then writes silence.

Memory:
- Granular-delay history is a plain 24000-sample float buffer in external SDRAM.
- Tuner capture, analysis, and source-window buffers are CPU-owned storage in ordinary cached SRAM.
- LibDaisy audio and ADC DMA buffers remain in the first 32 KB of RAM_D2, which its MPU configuration marks non-cacheable.
- Callback-critical ownership state remains in normal internal SRAM.

Raspberry Pi:
- Consumes the unchanged production UART protocol.
- Owns higher-level UI, storage, networking, and coordination.

## UART Protocol

Production messages remain:

```text
E,<fx>
P,<p0_u10k>,<p1_u10k>,<p2_u10k>,<p3_u10k>
S,<down0or1>,<edge>,<press_count>
T,<valid0or1>,<freq_mHz>,<note>,<cents_c>,<conf_m>
```

`E,4` reports granular delay. `ROTARY_CALIBRATION_MODE=1` enables an additional debug-only `R,<raw_u16>,<position>` message. This message is disabled in normal production builds and reports the actual raw ADC value.

Latched safety faults add:

```text
F,THERMAL,<temperature_centi_c>
F,TEMP_SENSOR,<error_code>
```

Each fault record is attempted once with the existing 10 ms UART timeout. Shutdown does not wait for a Raspberry Pi acknowledgment, and the Raspberry Pi parser must accept and preserve the new `F` prefix. With `THERMAL_DEBUG_TELEMETRY=1`, development builds also emit `D,TEMP,<temperature_centi_c>,<raw_adc>,<state>` at no more than two messages per second. After calibration validation, they emit one startup record `D,TEMP_CAL,<revision_hex>,<cal1_raw>,<cal2_raw>,<cal2_temp_c>`. `revision_hex` is the low 16-bit STM32 revision ID in uppercase hexadecimal, padded to at least four digits; the remaining fields are decimal integers.

## Hardware Configuration

The firmware uses the normal libDaisy `DaisySeed` abstraction. The local libDaisy checkout does not expose an explicit Seed3 board version or Seed3 codec initialization path, so Seed3 codec compatibility still requires hardware validation.

ADC channels:
- `A0`: pot 0
- `A1`: pot 1
- `A2`: pot 2
- `A3`: six-position rotary resistor ladder on PCB `ADC_3`

All four external channels are configured by the local libDaisy `AdcHandle` as a 16-bit, continuous ADC1 DMA scan. The pin mapping is `A0/PC0 -> ADC1_INP10`, `A1/PA3 -> ADC1_INP15`, `A2/PB1 -> ADC1_INP5`, and `A3/PA7 -> ADC1_INP7`. libDaisy does not allocate, initialize, start, or stop ADC3.

Footswitch:
- `D9`, active low with pull-up

UART:
- USART1 TX `D13`, RX `D14`, 115200 baud. RX `D14` is defined but production telemetry is TX-only.

## Thermal Safety

Thermal monitor

    samples STM32 internal die sensor outside audio callback
    filters and validates readings
    publishes latched shutdown state
    sends one UART fault message

Audio callback during fault

    fades output to zero
    stops DSP processing
    writes silence
    remains deterministic

Main loop during fault

    stops tuner and normal telemetry
    drives fault LED
    performs only minimal safety work

The monitor is enabled by default with `THERMAL_MONITOR_ENABLED=1`. The local STM32H7 HAL identifies the temperature sensor and VREFINT as ADC3-only internal channels. The backend therefore initializes a separate polling ADC3 once after the libDaisy ADC1 scan is established. It never calls `hw.adc.Stop()`, never changes ADC1 ranks or DMA, and never reads temperature in the audio callback.

The backend uses the checked-out HAL/LL definitions `TEMPSENSOR_CAL1_ADDR`, `TEMPSENSOR_CAL2_ADDR`, `VREFINT_CAL_ADDR`, `TEMPSENSOR_CAL1_TEMP`, and `VREFINT_CAL_VREF`. The vendored header defines `TEMPSENSOR_CAL2_TEMP` as fixed at 110 C, but conversion intentionally does not use that macro. Instead, startup calls `HAL_GetREVID()` and applies ST's revision rule: revision ID `0x1003` or earlier uses a 110 C CAL2 point, and later revisions use a 130 C CAL2 point. Revision IDs `0x0000` and `0xFFFF` are rejected as configuration faults instead of selecting a guessed calibration span.

Seed3 board revision and STM32 silicon revision are separate identifiers. The public Seed3 board datasheet does not guarantee the installed MCU `REV_ID`, so firmware reads the actual STM32H750 revision register on every startup rather than inferring it from the board name or manufacturing batch. The 110 C and 130 C values are factory calibration reference temperatures, not warning, trip, or critical safety thresholds. The vendored HAL and libDaisy checkout is left unchanged.

Each reading averages eight 16-bit conversions at the longest available 810.5-cycle sample time. VREFINT compensates the temperature ADC value for the current analog reference before the two-point factory calibration is applied. Zero, erased, reversed, or implausible calibration/conversion values are rejected before division.

Policy values are conservative firmware choices, not absolute device limits:

- Warning: 80 C for three filtered samples
- Warning recovery: at or below 75 C
- Shutdown: 90 C for three filtered samples
- Critical shutdown: one valid reading at or above 100 C
- Sampling: 100 ms
- Filter: four-sample moving average
- Persistent sensor failure: three invalid samples
- Audio fade: 10 ms

Shutdown and persistent `SensorFault` both latch until reset or power cycle. A sensor fault uses the conservative development policy: stop tuner and granular/effect work, fade to silence, send one sensor-fault record, and show the same three-short-flashes fault pattern. Ordinary warning operation continues and may recover through hysteresis. Production warning UART output is disabled.

The internal sensor measures only the STM32 die. It does not directly measure the codec, regulator, external ADC/DAC, or PCB hotspots. Firmware cannot protect hardware before startup, after a CPU crash, from 9 V reaching a signal pin, from shorts or regulator failure, or from an external chip overheating independently. It cannot replace a fuse, current limit, eFuse, reverse-polarity protection, or overvoltage protection. Firmware keeps the audio DMA callback producing silence; physically removing power requires a hardware power-disconnect circuit.

For hardware validation, enable `THERMAL_DEBUG_TELEMETRY` and temporarily use 30-40 C warning/trip thresholds. Do not intentionally heat the board to 90-100 C.

## Rotary Calibration

The rotary decoder supports six calibrated center values in logical throw order, both ascending and descending monotonic ladders, midpoint calculation, hysteresis, debounce, invalid readings, and reserved positions.

Positions:
- 1: tuner
- 2: chorus
- 3: reverb
- 4: crusher
- 5: granular delay
- 6: reserved, preserve current effect
- 0: invalid/between detents, preserve current effect

The checked-in theoretical centers assume seven equal 10 kOhm resistors from `3V3_A` to ground, with six intermediate throws. They are descending in logical throw order: `56173, 46811, 37449, 28086, 18724, 9362`. Measure each detent in `ROTARY_CALIBRATION_MODE=1` and replace or confirm `kRotaryCentersU16` with observed stable `A3` readings.

## Build

```sh
make clean
make
```

The generated binary is `build/project006.bin`.

## Map File Memory Excerpts

These excerpts are from the verified `DIAG_STAGE_PRODUCTION` map in
`build_production_permanent/project006.map`, with exact symbol sizes confirmed
from:

```sh
arm-none-eabi-nm -S --demangle build_production_permanent/project006.elf
```

SDRAM granular delay buffer:

```text
6005: .sdram_bss      0xc0000000    0x17700
6006:                 0xc0000000                . = ALIGN (0x4)
6007:                 0xc0000000                _ssdram_bss = .
6008:                 [!provide]                PROVIDE (__sdram_bss_start = _ssdram_bss)
6009:  *(.sdram_bss)
6010:  .sdram_bss     0xc0000000    0x17700 build_production_permanent/granulardelay_module.o
6011:  *(.sdram_bss*)
6012:                 0xc0017700                . = ALIGN (0x4)
6013:                 0xc0017700                _esdram_bss = .
```

```text
c0000000 00017700 b (anonymous namespace)::buffer_gran_delay
```

RAM_D2 DMA placement invariant:

```text
.sram1_bss           0x30000000-0x30004140
audio DMA TX buffer  0x30000000  size 0x2000
audio DMA RX buffer  0x30002000  size 0x2000
ADC1 DMA buffer      0x30004000  size 0x0040
ADC1 mux cache       0x30004040  size 0x0100
MPU boundary         0x30008000
```

The tuner ring, YIN work, difference, CMND, and source-window arrays are all
CPU-owned and must not use `DMA_BUFFER_MEM_SECTION`. The four arrays in
`tuner.cpp` total 45,056 bytes. When they occupied `.sram1_bss`, linker
ordering shifted the actual audio and ADC DMA buffers beyond libDaisy's 32 KB
non-cacheable MPU window. Hardware tests reproduced repeating tonal corruption
with that layout and restored clean bypass audio when only those four arrays
were moved to cached SRAM. The permanent fix preserves DMA placement rather than
expanding the MPU region or changing the linker script.

Verified production cached-SRAM placement:

```text
tuner_source_window       0x24067660  size 0x4000
tuner_cmnd_buffer         0x2406b710  size 0x1000
tuner_work_buffer         0x2406c710  size 0x1000
tuner_capture_ring        0x2406d710  size 0x8000
tuner_difference_buffer   0x24075710  size 0x1000
```

The callback/foreground thermal monitor object is small normal SRAM state:

```text
240675a0 000000c0 b (anonymous namespace)::thermal_monitor
```

Stack and heap symbols:

```text
3169:                 0x20020000                _estack = 0x20020000
```

```text
6060: .heap           0x2407944c        0x0
6061:                 0x2407944c                . = ALIGN (0x4)
6062:                 [!provide]                PROVIDE (__heap_start__ = .)
6063:  *(.heap)
6064:                 0x2407944c                . = ALIGN (0x4)
6065:                 [!provide]                PROVIDE (__heap_end__ = .)

6067: .reserved_for_stack
6068:                 0x2407944c        0x0
6069:                 0x2407944c                . = ALIGN (0x4)
6070:                 [!provide]                PROVIDE (__reserved_for_stack_start__ = .)
6071:  *(.reserved_for_stack)
6072:                 0x2407944c                . = ALIGN (0x4)
6073:                 [!provide]                PROVIDE (__reserved_for_stack_end__ = .)
```

```text
20020000 A _estack
```
