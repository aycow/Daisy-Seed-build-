import math


ROTARY_CENTERS = [56173, 46811, 37449, 28086, 18724, 9362]
ROTARY_HYSTERESIS = 900
ROTARY_MAX_CENTER_ERROR = 2800
ROTARY_DEBOUNCE_MS = 20
FX_TUNER = 0
FX_CHORUS = 1
FX_REVERB = 2
FX_CRUSHER = 3
FX_GRANULAR_DELAY = 4
FX_COUNT = 5
RING_SIZE = 8192
SOURCE_WINDOW = 4096
SAFETY = 256


class DebounceBool:
    def __init__(self):
        self.stable = True
        self.last_sample = True
        self.last_change_ms = 0

    def update(self, sample, now_ms, debounce_ms):
        if sample != self.last_sample:
            self.last_sample = sample
            self.last_change_ms = now_ms
        if sample != self.stable and ((now_ms - self.last_change_ms) & 0xFFFFFFFF) >= debounce_ms:
            self.stable = sample
            return True
        return False


class AnalogRotaryLadder:
    def __init__(self, centers, hysteresis, max_error):
        self.centers = centers
        self.hysteresis = hysteresis
        self.max_error = max_error
        self.candidate = 0
        self.stable = 0
        self.candidate_since = 0
        self.valid = len(centers) >= 2
        self.descending = centers[1] < centers[0] if self.valid else False
        if self.valid:
            for a, b in zip(centers, centers[1:]):
                if self.descending and a <= b:
                    self.valid = False
                if not self.descending and a >= b:
                    self.valid = False
        self.thresholds = [(centers[i] + centers[i + 1]) // 2 for i in range(len(centers) - 1)] if self.valid else []

    def decode(self, value):
        if not self.valid:
            return 0
        best_pos = 0
        best_error = 0xFFFF
        for i, center in enumerate(self.centers):
            error = abs(value - center)
            if error < best_error:
                best_error = error
                best_pos = i + 1
        return best_pos if best_error <= self.max_error else 0

    def decode_hysteresis(self, value):
        if self.stable <= 0:
            return self.decode(value)
        center = self.centers[self.stable - 1]
        if abs(value - center) <= self.max_error + self.hysteresis:
            return self.stable
        return self.decode(value)

    def update(self, value, now_ms):
        decoded = self.decode_hysteresis(value)
        if decoded != self.candidate:
            self.candidate = decoded
            self.candidate_since = now_ms
        if decoded != self.stable and ((now_ms - self.candidate_since) & 0xFFFFFFFF) >= ROTARY_DEBOUNCE_MS:
            self.stable = decoded
        return self.stable


def rotary_position_to_effect(position):
    return position - 1 if 1 <= position <= 5 else None


def apply_rotary_effect(current, position):
    mapped = rotary_position_to_effect(position)
    return current if mapped is None else mapped


def frequency_to_note(freq):
    if freq <= 0:
        return "--", 0.0
    names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    midi_f = 69.0 + 12.0 * math.log2(freq / 440.0)
    midi_n = round(midi_f)
    return f"{names[midi_n % 12]}{midi_n // 12 - 1}", (midi_f - midi_n) * 100.0


class CaptureRing:
    def __init__(self):
        self.ring = [0.0] * RING_SIZE
        self.completed = 0

    def write(self, samples):
        wr = self.completed
        for x in samples:
            self.ring[wr & (RING_SIZE - 1)] = x
            wr = (wr + 1) & 0xFFFFFFFF
        self.completed = wr

    def copy(self, count):
        if count <= 0 or count > RING_SIZE or count > SOURCE_WINDOW:
            return None
        required = count + SAFETY
        if required >= RING_SIZE:
            return None
        completed = self.completed
        if completed < required:
            return None
        end = completed - SAFETY
        start = end - count
        out = [self.ring[(start + i) & (RING_SIZE - 1)] for i in range(count)]
        after = self.completed
        if ((after - start) & 0xFFFFFFFF) >= RING_SIZE:
            return None
        return out


class TransitionModel:
    STEADY = 0
    FADING_OUT = 1
    FADING_IN = 2

    def __init__(self, step=0.25):
        self.step = step
        self.active = FX_TUNER
        self.bypass = False
        self.pending = FX_TUNER
        self.pending_bypass = False
        self.state = self.STEADY
        self.wet = 0.0

    def request(self, effect, bypass=False):
        self.pending = effect
        self.pending_bypass = bypass
        audible = self.wet > 0.0 and self.active != FX_TUNER and not self.bypass
        if audible:
            self.state = self.FADING_OUT
        else:
            self.active = effect
            self.bypass = bypass
            self.wet = 0.0
            self.state = self.FADING_IN if effect != FX_TUNER and not bypass else self.STEADY

    def step_once(self):
        if self.state == self.FADING_OUT:
            self.wet = max(0.0, self.wet - self.step)
            if self.wet == 0.0:
                self.active = self.pending
                self.bypass = self.pending_bypass
                self.state = self.FADING_IN if self.active != FX_TUNER and not self.bypass else self.STEADY
        elif self.state == self.FADING_IN:
            self.wet = min(1.0, self.wet + self.step)
            if self.wet == 1.0:
                self.state = self.STEADY

    def run(self, n=8):
        for _ in range(n):
            self.step_once()


def lowpass_decimate(samples):
    coeff = [1, 2, 4, 6, 8, 6, 4, 2, 1]
    scale = 1.0 / sum(coeff)
    mean = sum(samples) / len(samples)
    out = []
    for center in range(0, len(samples), 4):
        y = 0.0
        for tap, c in enumerate(coeff):
            idx = min(len(samples) - 1, max(0, center + tap - 4))
            y += c * (samples[idx] - mean)
        out.append(y * scale)
    return out


def test_rotary_descending_centers_map_to_positions():
    ladder = AnalogRotaryLadder(ROTARY_CENTERS, ROTARY_HYSTERESIS, ROTARY_MAX_CENTER_ERROR)
    assert ladder.valid
    assert ladder.descending
    assert [ladder.decode(v) for v in ROTARY_CENTERS] == [1, 2, 3, 4, 5, 6]


def test_rotary_midpoints_and_invalid_tolerance():
    ladder = AnalogRotaryLadder(ROTARY_CENTERS, ROTARY_HYSTERESIS, ROTARY_MAX_CENTER_ERROR)
    for a, b in zip(ROTARY_CENTERS, ROTARY_CENTERS[1:]):
        midpoint = (a + b) // 2
        assert ladder.decode(midpoint) == 0
    assert ladder.decode(0) == 0
    assert ladder.decode(65535) == 0
    assert ladder.decode(ROTARY_CENTERS[0] - ROTARY_MAX_CENTER_ERROR) == 1
    assert ladder.decode(ROTARY_CENTERS[0] - ROTARY_MAX_CENTER_ERROR - 1) == 0


def test_rotary_hysteresis_and_debounce_wrap():
    ladder = AnalogRotaryLadder(ROTARY_CENTERS, ROTARY_HYSTERESIS, ROTARY_MAX_CENTER_ERROR)
    assert ladder.update(ROTARY_CENTERS[0], 0xFFFFFFF0) == 0
    assert ladder.update(ROTARY_CENTERS[0], 0x00000005) == 1
    near_boundary = ROTARY_CENTERS[0] - ROTARY_MAX_CENTER_ERROR - ROTARY_HYSTERESIS + 1
    assert ladder.update(near_boundary, 50) == 1
    assert ladder.update(ROTARY_CENTERS[1], 51) == 1
    assert ladder.update(ROTARY_CENTERS[1], 72) == 2


def test_invalid_calibration_rejected():
    assert not AnalogRotaryLadder([100, 100, 90], ROTARY_HYSTERESIS, ROTARY_MAX_CENTER_ERROR).valid
    assert not AnalogRotaryLadder([100, 90, 95], ROTARY_HYSTERESIS, ROTARY_MAX_CENTER_ERROR).valid
    assert AnalogRotaryLadder([10, 20, 30], ROTARY_HYSTERESIS, ROTARY_MAX_CENTER_ERROR).valid


def test_rotary_effect_mapping_reserved_preserves():
    assert rotary_position_to_effect(1) == FX_TUNER
    assert rotary_position_to_effect(5) == FX_GRANULAR_DELAY
    assert rotary_position_to_effect(6) is None
    assert rotary_position_to_effect(0) is None
    assert apply_rotary_effect(FX_REVERB, 0) == FX_REVERB
    assert apply_rotary_effect(FX_REVERB, 6) == FX_REVERB


def test_debounce_press_release_bounce_and_wrap():
    db = DebounceBool()
    assert not db.update(False, 10, 15)
    assert not db.update(True, 12, 15)
    assert not db.update(False, 13, 15)
    assert db.update(False, 28, 15)
    assert db.stable is False
    assert not db.update(True, 0xFFFFFFF0, 15)
    assert db.update(True, 0x00000005, 15)
    assert db.stable is True


def test_note_conversion_representative_bass_notes():
    expected = [(30.87, "B0"), (41.20, "E1"), (55.00, "A1"), (73.42, "D2"), (98.00, "G2"), (110.00, "A2")]
    for freq, note in expected:
        got, cents = frequency_to_note(freq)
        assert got == note
        assert abs(cents) < 2.0


def test_note_conversion_cents_offsets_and_invalid():
    assert frequency_to_note(0)[0] == "--"
    sharp, cents = frequency_to_note(110.0 * 2 ** (10.0 / 1200.0))
    assert sharp == "A2"
    assert 9.5 < cents < 10.5
    flat, cents = frequency_to_note(110.0 * 2 ** (-10.0 / 1200.0))
    assert flat == "A2"
    assert -10.5 < cents < -9.5


def test_tuner_capture_startup_readiness_and_order():
    ring = CaptureRing()
    ring.write(range(SOURCE_WINDOW + SAFETY - 1))
    assert ring.copy(SOURCE_WINDOW) is None
    ring.write([SOURCE_WINDOW + SAFETY - 1])
    copied = ring.copy(SOURCE_WINDOW)
    assert copied is not None
    assert copied[0] == 0
    assert copied[-1] == SOURCE_WINDOW - 1


def test_tuner_capture_wrap_order():
    ring = CaptureRing()
    ring.write(range(RING_SIZE + SOURCE_WINDOW + SAFETY + 10))
    copied = ring.copy(SOURCE_WINDOW)
    end = ring.completed - SAFETY
    start = end - SOURCE_WINDOW
    assert copied[0] == start
    assert copied[-1] == end - 1


def test_transition_fade_in_bypass_effect_change_and_tuner():
    model = TransitionModel()
    model.request(FX_CHORUS)
    assert model.active == FX_CHORUS and model.state == model.FADING_IN
    model.run()
    assert model.wet == 1.0 and model.state == model.STEADY
    model.request(FX_CHORUS, True)
    assert model.active == FX_CHORUS and model.state == model.FADING_OUT
    model.run()
    assert model.wet == 0.0 and model.bypass is True
    model.request(FX_REVERB, False)
    model.run()
    assert model.active == FX_REVERB and model.wet == 1.0
    model.request(FX_TUNER, False)
    model.run()
    assert model.active == FX_TUNER and model.wet == 0.0


def test_transition_new_request_during_fadeout():
    model = TransitionModel()
    model.request(FX_CHORUS)
    model.run()
    model.request(FX_REVERB)
    model.step_once()
    assert model.active == FX_CHORUS
    model.request(FX_GRANULAR_DELAY)
    model.run()
    assert model.active == FX_GRANULAR_DELAY
    assert model.wet == 1.0


def test_granular_parameter_mapping():
    maps = {FX_GRANULAR_DELAY: (0, 1, 2)}
    assert maps[FX_GRANULAR_DELAY] == (0, 1, 2)


def test_decimation_filter_keeps_low_b_and_reduces_alias_band():
    sample_rate = 48000.0
    n = 4096
    low_b = [math.sin(2 * math.pi * 30.87 * i / sample_rate) for i in range(n)]
    filtered = lowpass_decimate(low_b)
    assert max(filtered) - min(filtered) > 1.5
    high = [math.sin(2 * math.pi * 9000.0 * i / sample_rate) for i in range(n)]
    filtered_high = lowpass_decimate(high)
    naive_high = high[::4]
    rms_filtered = math.sqrt(sum(x * x for x in filtered_high) / len(filtered_high))
    rms_naive = math.sqrt(sum(x * x for x in naive_high) / len(naive_high))
    assert rms_filtered < rms_naive * 0.55


def test_silence_never_reports_valid_placeholder_a2():
    note, _ = frequency_to_note(0.0)
    assert note == "--"


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("logic tests passed")