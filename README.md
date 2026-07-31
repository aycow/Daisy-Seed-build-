# project006

Daisy Seed/Seed3 guitar-pedal firmware for tuner, chorus, reverb, crusher, granular delay, footswitch bypass, analog rotary selection, and UART telemetry to a Raspberry Pi.

## Runtime Architecture

Main loop:
- Initializes and reads ADC channels for three pots plus the six-position rotary resistor ladder.
- Debounces the footswitch and analog rotary selector.
- Smooths pot values, applies deadbands, and publishes changed parameters through the control mailbox.
- Handles rotary calibration telemetry when explicitly enabled.
- Performs non-real-time tuner analysis.
- Emits UART telemetry to the Raspberry Pi.

Control mailbox:
- Transfers compact requested control state across the main-loop/audio-callback boundary.
- Uses a sequence-counter publication pattern around a trivially copyable snapshot.
- Carries effect id, bypass state, and three fixed-point pot parameters.

Audio callback:
- Consumes one valid snapshot at the start of each block.
- Exclusively owns and mutates chorus, reverb, crusher, and granular-delay DSP objects after audio starts.
- Applies changed effect parameters only when the mailbox revision changes.
- Performs effect/bypass transition ramps before skipping or switching DSP objects.
- Processes real-time audio and captures tuner samples into a power-of-two ring.
- Performs no UART, formatting, ADC reads, GPIO debounce, locks, allocation, delays, or tuner analysis.

Memory:
- Granular-delay history is a plain 24000-sample float buffer in external SDRAM.
- Tuner capture and work buffers are plain buffers in RAM_D2 through libDaisy's `.sram1_bss` section.
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

## Hardware Configuration

The firmware uses the normal libDaisy `DaisySeed` abstraction. The local libDaisy checkout does not expose an explicit Seed3 board version or Seed3 codec initialization path, so Seed3 codec compatibility still requires hardware validation.

ADC channels:
- `A0`: pot 0
- `A1`: pot 1
- `A2`: pot 2
- `A3`: six-position rotary resistor ladder on PCB `ADC_3`

Footswitch:
- `D9`, active low with pull-up

UART:
- USART1 TX `D13`, RX `D14`, 115200 baud. RX `D14` is defined but production telemetry is TX-only.

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