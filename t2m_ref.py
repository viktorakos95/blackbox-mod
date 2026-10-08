"""Reference for src/t2m.c: the Trigger2MIDI detector, transcribed line for line from the gen~ codebox in
viktorakos95/trigger2midi (max/SPD_Trigger2MIDI_gen_codebox_FIXED_20260930.txt), in double precision as gen~ runs it.

Nothing here is optimised or reorganised: the per-sample exp() and log() stay, the state names and the order of the
statements are the codebox's. Outputs that only feed Max print / display objects (env, crossing and accept times,
peak_found_ms) are kept anyway so the transcription stays complete.

The glue after the gen~ box (velocity scaling, note / CC sending, note length) follows the Max patch
(max/SPD_Trigger2MIDI_plainMax_gen_FIXED_20260930.maxpat); see Glue below for the three places it differs on purpose.
"""
from math import exp, log, pow

SAMPLERATE = 48000.0


def maxf(a, b):
    return a if a > b else b


def minf(a, b):
    return a if a < b else b


def clip(x, lo, hi):
    return lo if x < lo else hi if x > hi else x


class Gen:
    """The codebox. step(in1..in11) runs one sample and returns (out1, ..., out11)."""

    def __init__(self):
        self.state = 0
        self.timer = 0
        self.peak_velo = 0.0
        self.dynamic_thresh = 0.0
        self.env = 0.0
        self.has_dipped = 1
        self.last_peak = 0.0
        self.last_floor = 0.0
        self.last_corrected = 0.0
        self.last_shaped = 0.0
        self.reset_prev = 0.0
        self.quiet_samples = 0.0
        self.sample_count = 0.0
        self.pending_crossing_time = 0.0
        self.last_crossing_time = 0.0
        self.last_accept_time = 0.0
        self.peak_found_at_sample = 0.0
        self.last_peak_found_ms = 0.0
        self.fast_env = 0.0
        self.slow_env = 0.0
        self.note_active = 0.0
        self.note_quiet_samples = 0.0
        self.note_active_samples = 0.0
        self.lp1 = 0.0
        self.lp2 = 0.0
        self.band_low_env = 0.0
        self.band_mid_env = 0.0
        self.band_high_env = 0.0
        self.peak_band_low = 0.0
        self.peak_band_mid = 0.0
        self.peak_band_high = 0.0
        self.best_diff_db = -200.0

    def step(self, in1, in2, in3, in4, in5, in6, in7, in8, in9, in10, in11):
        s = self
        samplerate = SAMPLERATE

        ENV_RELEASE_MS = 80.0
        DIP_FRACTION = 0.5

        SLOW_MS = 80.0
        OFF_THRESH_DB = 3.0
        FLOOR_DB = -60.0
        LN10 = 2.302585093
        PI = 3.14159265358979

        NOTE_OFF_QUIET_MS = 60.0
        NOTE_OFF_RESET_RATIO = 1.5
        DYNAMIC_NOTE_MAX_MS = 2000.0

        FLUX_FC1 = 500.0
        FLUX_FC2 = 3000.0
        FLUX_ENV_MS = 2.0

        threshold = in2
        scan_ms = in3
        mask_ms = in4
        retrig_cancel_ms = maxf(in5, 1.0) * 20.0
        curve_exp = in6
        reset_in = in7
        onset_strictness_db = in8
        onset_speed_ms = maxf(in9, 0.1)
        spectral_strictness_param = maxf(in10, 0.0)
        spectral_range_param = maxf(in11, 0.0)

        scan_samples = scan_ms * 0.001 * samplerate
        onset_confirm_samples = onset_speed_ms * 0.001 * samplerate * 3.0
        effective_scan_samples = maxf(scan_samples, onset_confirm_samples)
        mask_samples = mask_ms * 0.001 * samplerate
        note_off_quiet_min_samples = NOTE_OFF_QUIET_MS * 0.001 * samplerate
        note_max_samples = DYNAMIC_NOTE_MAX_MS * 0.001 * samplerate

        rect = abs(in1)
        trig_out = 0.0
        crossing_out = 0.0

        s.sample_count += 1

        if reset_in > 0.5 and s.reset_prev < 0.5:
            s.state = 0
            s.timer = 0
            s.peak_velo = 0.0
            s.dynamic_thresh = threshold
            s.env = 0.0
            s.has_dipped = 1
            s.last_peak = 0.0
            s.last_floor = 0.0
            s.last_corrected = 0.0
            s.last_shaped = 0.0
            s.sample_count = 0.0
            s.last_crossing_time = 0.0
            s.last_accept_time = 0.0
            s.quiet_samples = 0.0
            s.fast_env = 0.0
            s.slow_env = 0.0
            s.note_active = 0.0
            s.note_quiet_samples = 0.0
            s.note_active_samples = 0.0
            s.lp1 = 0.0
            s.lp2 = 0.0
            s.band_low_env = 0.0
            s.band_mid_env = 0.0
            s.band_high_env = 0.0
            s.peak_band_low = 0.0
            s.peak_band_mid = 0.0
            s.peak_band_high = 0.0
            s.best_diff_db = -200.0
        s.reset_prev = reset_in

        fast_coeff = exp(-1.0 / (onset_speed_ms * 0.001 * samplerate))
        slow_coeff = exp(-1.0 / (SLOW_MS * 0.001 * samplerate))
        s.fast_env = rect + (s.fast_env - rect) * fast_coeff
        s.slow_env = rect + (s.slow_env - rect) * slow_coeff

        fast_db = 20.0 * log(maxf(s.fast_env, 0.000001)) / LN10
        slow_db = 20.0 * log(maxf(s.slow_env, 0.000001)) / LN10
        diff_db = fast_db - slow_db

        quiet_min_samples = maxf(samplerate * 0.001, scan_samples)

        if rect < s.dynamic_thresh * DIP_FRACTION:
            s.has_dipped = 1

        if diff_db < OFF_THRESH_DB:
            s.quiet_samples += 1
            if s.quiet_samples >= quiet_min_samples:
                s.has_dipped = 1
        else:
            s.quiet_samples = 0

        if rect < threshold:
            s.note_quiet_samples += 1
        elif rect >= threshold * NOTE_OFF_RESET_RATIO:
            s.note_quiet_samples = 0
        if s.note_active > 0.5 and s.note_quiet_samples >= note_off_quiet_min_samples:
            s.note_active = 0.0
            s.note_active_samples = 0.0
        if s.note_active > 0.5:
            s.note_active_samples += 1
            if s.note_active_samples >= note_max_samples:
                s.note_active = 0.0
                s.note_active_samples = 0.0
        else:
            s.note_active_samples = 0.0

        lp1_coeff = exp(-2.0 * PI * FLUX_FC1 / samplerate)
        lp2_coeff = exp(-2.0 * PI * FLUX_FC2 / samplerate)
        s.lp1 = rect + (s.lp1 - rect) * lp1_coeff
        s.lp2 = rect + (s.lp2 - rect) * lp2_coeff

        band_low = s.lp1
        band_mid = s.lp2 - s.lp1
        band_high = rect - s.lp2

        flux_env_coeff = exp(-1.0 / (FLUX_ENV_MS * 0.001 * samplerate))
        s.band_low_env = abs(band_low) + (s.band_low_env - abs(band_low)) * flux_env_coeff
        s.band_mid_env = abs(band_mid) + (s.band_mid_env - abs(band_mid)) * flux_env_coeff
        s.band_high_env = abs(band_high) + (s.band_high_env - abs(band_high)) * flux_env_coeff

        if s.state == 0:
            if rect > threshold and slow_db > FLOOR_DB and s.has_dipped > 0.5:
                s.timer = 0
                s.peak_velo = rect
                s.peak_found_at_sample = 0
                s.has_dipped = 0
                s.state = 1
                s.pending_crossing_time = s.sample_count / samplerate
                crossing_out = 1.0
                s.best_diff_db = diff_db
                s.peak_band_low = s.band_low_env
                s.peak_band_mid = s.band_mid_env
                s.peak_band_high = s.band_high_env
            else:
                idle_coeff = exp(-1.0 / (retrig_cancel_ms * 0.001 * samplerate))
                s.dynamic_thresh = threshold + (s.dynamic_thresh - threshold) * idle_coeff
        elif s.state == 1:
            s.timer += 1
            cur = abs(in1)
            if cur > s.peak_velo:
                s.peak_found_at_sample = s.timer
                s.peak_band_low = s.band_low_env
                s.peak_band_mid = s.band_mid_env
                s.peak_band_high = s.band_high_env
            s.peak_velo = maxf(s.peak_velo, cur)
            if diff_db > s.best_diff_db:
                s.best_diff_db = diff_db
            if s.timer >= effective_scan_samples:
                s.state = 2
        elif s.state == 2:
            shaped_velo = pow(clip(s.peak_velo, 0.0, 1.0), curve_exp)
            s.last_peak = s.peak_velo
            s.last_floor = s.dynamic_thresh
            s.last_corrected = s.peak_velo
            s.last_shaped = shaped_velo
            s.last_accept_time = s.sample_count / samplerate
            s.last_crossing_time = s.pending_crossing_time
            s.last_peak_found_ms = s.peak_found_at_sample * 1000.0 / samplerate
            margin = (s.peak_velo - s.dynamic_thresh) / maxf(s.dynamic_thresh, 0.000001)
            max_band = maxf(maxf(s.peak_band_low, s.peak_band_mid), s.peak_band_high)
            min_band = minf(minf(s.peak_band_low, s.peak_band_mid), s.peak_band_high)
            spectral_flatness = min_band / maxf(max_band, 0.000001)
            if s.peak_velo > s.dynamic_thresh:
                if s.best_diff_db <= onset_strictness_db or (margin < spectral_range_param and spectral_flatness < spectral_strictness_param):
                    s.state = 0
                else:
                    trig_out = 1.0
                    s.env = shaped_velo
                    s.dynamic_thresh = s.peak_velo
                    s.timer = 0
                    s.state = 3
                    s.note_active = 1.0
                    s.note_quiet_samples = 0.0
                    s.note_active_samples = 0.0
            else:
                s.state = 0
        elif s.state == 3:
            s.timer += 1
            rc_coeff = exp(-1.0 / (retrig_cancel_ms * 0.001 * samplerate))
            s.dynamic_thresh = s.dynamic_thresh * rc_coeff
            env_coeff = exp(-1.0 / (ENV_RELEASE_MS * 0.001 * samplerate))
            s.env = s.env * env_coeff
            if s.timer >= mask_samples:
                s.state = 0

        return (s.env, trig_out, s.last_peak, s.last_floor, s.last_corrected, s.last_shaped, s.last_crossing_time,
                s.last_accept_time, s.last_peak_found_ms, crossing_out, s.note_active)


# ---- Glue: the Max objects between the gen~ box and noteout / ctlout ----
#
# Knobs into gen~:  in1 = input x Sensitivity (*~), in2 = scale 0 31 0.01 0.1 of the Threshold knob, in3 Scan,
#                   in4 Mask, in5 Retrig, in6 Curve, in7 reset (0), in8 Strict, in9 Speed, in10 / in11 anti-bleed.
# On a hit (edge~ on out2): v = clip 1 127 (round (scale 0. 1. 1 127 (clip 0. 1. out6))).
#   CC first (its trigger object sits to the right in the patch, so Max fires it first): if CC Enable, ctlout
#   (Fixed ? clip 1 127 of the fixed value : v), CC number, channel. No CC is sent at note-off.
#   Then the note, unless the menu says NO-NOTE: makenote velocity (Fixed ? fixed value : v), the menu's pitch.
# Note-off: Length Fixed = makenote's own note-off after the ms (one per note-on); Length Dynamic = when out11
#   (note_active) falls (edge~ right outlet), noteout pitch / 0.
# On purpose different from the patch (all three are stuck-note or wrong-channel fixes):
#   1. the Dynamic note-off goes on the MIDI channel setting (the patch hard-codes channel 1 there);
#   2. the Dynamic note-off is for the pitch that was played (the patch uses whatever the menu shows now);
#   3. a hit on a different pitch while a Dynamic note is still held ends the held one first.
#   (Dynamic mode also gives makenote a 60000 ms length in the patch; that stray note-off a minute later is dropped.)

NOTE_ON, NOTE_OFF, CC = 0x90, 0x80, 0xB0


def max_round(x):
    """Max's [round] on a positive float: nearest integer, halves up."""
    return int(x + 0.5)


class Glue:
    def __init__(self, p):
        self.p = p
        self.prev_trig = 0.0
        self.prev_active = 0.0
        self.held = -1                 # Dynamic: pitch that is sounding
        self.offs = []                 # Fixed: [samples left, pitch] per note-on

    def step(self, outs, frame):
        p, ev = self.p, []
        trig, shaped, active = outs[1], outs[5], outs[10]
        ch = p["channel"] - 1
        # Fixed-length note-offs due now (makenote)
        keep = []
        for o in self.offs:
            o[0] -= 1
            if o[0] <= 0:
                ev.append((frame, NOTE_OFF | ch, o[1], 0))
            else:
                keep.append(o)
        self.offs = keep
        if trig > 0.0 and self.prev_trig <= 0.0:
            v = int(clip(max_round(1.0 + clip(shaped, 0.0, 1.0) * 126.0), 1, 127))
            if p["cc_on"]:
                ev.append((frame, CC | ch, p["cc_num"], int(clip(p["cc_fixed"], 1, 127)) if p["cc_mode"] else v))
            if p["note_on"]:
                vel = p["vel_fixed"] if p["vel_mode"] else v
                if p["len_mode"]:
                    self.offs.append([int(p["len_ms"] * SAMPLERATE / 1000.0 + 0.5), p["note"]])
                else:
                    if self.held >= 0 and self.held != p["note"]:
                        ev.append((frame, NOTE_OFF | ch, self.held, 0))
                    self.held = p["note"]
                ev.append((frame, NOTE_ON | ch, p["note"], vel))
        if self.prev_active > 0.0 and active <= 0.0 and self.held >= 0:
            ev.append((frame, NOTE_OFF | ch, self.held, 0))
            self.held = -1
        self.prev_trig, self.prev_active = trig, active
        return ev


DEFAULTS = dict(sens=2.0, thresh=2.0, retrig=8.0, mask_ms=15.0, scan_ms=2.0, curve=0.6, strict_db=8.0, speed_ms=3.0,
                spec_strict=0.15, spec_range=0.15, note_on=1, note=38, vel_mode=0, vel_fixed=100, len_mode=0,
                len_ms=50, cc_on=0, cc_num=1, cc_mode=0, cc_fixed=100, channel=1)


def run(signal, p):
    """Whole signal through gen~ + glue: list of (frame, status, data1, data2)."""
    g, glue, ev = Gen(), Glue(p), []
    thr = 0.01 + (p["thresh"] - 0.0) / (31.0 - 0.0) * (0.1 - 0.01)
    for i, x in enumerate(signal):
        outs = g.step(x * p["sens"], thr, p["scan_ms"], p["mask_ms"], p["retrig"], p["curve"], 0.0, p["strict_db"],
                      p["speed_ms"], p["spec_strict"], p["spec_range"])
        ev += glue.step(outs, i)
    return ev
