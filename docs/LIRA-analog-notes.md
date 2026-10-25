# LIRA analog engine: what the real Lyra-8 is, and what the model does about it

The signal flow of the engine (`src/lira.c`) comes from Mike Moreno DSP's LIRA-8 plugin (Pure Data). That plugin models the
Lyra-8 as clean DSP. This note records what is publicly known about the *hardware*, where each fact comes from, and which
parts of the engine follow it. Anything marked **assumption** is not documented anywhere I could find: SOMA does not publish
schematics (forum posts say so, and that the kit ships without one), so those constants are judgement calls, kept in
one place (`#define`s at the top of `lira.c`) so they can be changed after listening.

## Sourced facts

| Fact | Source |
|---|---|
| The voices are not VCOs: no linear or log dependence on control voltage, modelled on electric-organ tone generators. Non-linearity is used on purpose. | Lyra-8 user manual, "Structure" |
| Oscillator = **HEF4093** (a digital Schmitt-trigger chip) used for its analog behaviour; **the triangle is taken off the timing capacitor** (most 4093 oscillators use the square); the square still feeds the SHARP waveshaping. | Designer's FAQ, somasynths.com/faq |
| Envelope generators are part of the oscillators: they affect pitch and waveform and act as a waveshaper. There is **no VCA** at the end of the chain, so part of the circuit is always open and makes noise. | FAQ; manual "Noise and bleed" |
| Parts per unit: 4093 x5, 4016 x1, TL074 x3, TDA2822 x1, **PT2399 x2**, BC547/557 x17, BZX55C 10 V zeners x18, 1N4148 x13, 78L05 x2, 12 V supply. | DIY kit assembly manual, BOM |
| The voice ICs come from a batch with unusual properties: very wide tuning range, deeper brighter FM. Some voices, varying **from unit to unit**, give enharmonic harshness at high PITCH; PITCH 50-70 % avoids it. "Even a single resistor affects the sound." | FAQ |
| 4093 thresholds are loose: HEF4093B typical at 5 V: VT+ 2.9 V, VT- 2.2 V (hysteresis 0.7 V); at 15 V the *positive* threshold is specified anywhere from 4.7 to 11 V. | HEF4093B datasheet |
| **Delay = digital memory with PWM converters at both ends, an analog clock generator with an analog modulation input; feedback loop, input/output filters, two-tap mixer, wet/dry are analog.** The kit uses two PT2399. | FAQ; BOM |
| PT2399: ADC -> **44 kbit** RAM -> DAC, internal VCO clock. Delay = 683.21 / fck(MHz) + 0.08 ms; minimum ~31 ms (22 MHz); THD rises as the clock slows (0.13 % at 20 MHz, 1.0 % at 2 MHz) and it gets "crunchier" with longer delay. | PT2399 datasheet table 1; Electric Druid fit |
| The delay's own output modulates the **clock** (sample-rate) in SELF mode; CV only responds to positive voltage and only makes echoes faster. | SoS review; owner reports |
| The two delay lines share a common feedback loop ("cross-feedback ... resonance between the two lines"). | Manual; Lyra8-FX description |
| The delay is dark ("rather extreme filtering", output filter is a single capacitor), noisy, most at the longest times. | SoS review; manual |
| Slow envelope release ~20 s (SoS) to ~30 s (Waveform), varying voice to voice; FAST ~0.5 s. HOLD makes slow-envelope voices drone from ~12 o'clock; FAST voices need ~2 o'clock. | SoS; Waveform |
| The release is an RC discharge: 3 M ohm x 2.2 uF = 6.6 s time constant (slow), the attack is a 300 k-1 M resistor with 2.2 uF. Rail is 5.87 V (the 4093 thresholds scale with it: VT+ ~3.3 V, VT- ~2.55 V). | DIY kit docs: Tweaks sheet, BOM |
| The FM input is a resistor into the timing node of the modulated voice. The deviation is therefore in Hz, set by that voice's timing capacitor, not relative to its pitch; a negative sum stops the oscillator. | DIY kit docs: Tweaks sheet, assembly manual (my reading of the schematic-level notes, [Likely]) |
| Vibrato rates 3.5-6.5 Hz, from 30-56 k resistors with 10 uF (8 different values, one per voice). | DIY kit docs: BOM |
| The board has an EQ stage between the delay and the distortion; the sensor sensitivity is a 3-10 M ohm resistor. | DIY kit docs: BOM |
| Light touch / short strokes = slow attack; less pressure = envelope opens partially. FAST also makes the sensors less sensitive. | Manual |
| Eight independent vibrato generators, each voice with its own rate and depth, hard-wired; some rates "a tad slow". | Manual; SoS |
| Low-level bleed from the voices reaches the output even with delay and distortion out; level differences between voices; crosstalk between voice pairs; constant low rumble and high whine with nothing touched. | SoS; manual; lyraeight.blog (search excerpt) |
| The higher a voice is tuned, the less sensitive it is to FM. A modulating voice is always also in the mix. | Manual step 2; ModWiggler (search excerpt) |
| The Hyper LFO edges are soft when the two LFOs are added ("gradient"). | Manual; SoS |
| Output response: bass boosted a little, highs softer. | Manual |
| Distortion: last in the chain (after the delay), DRIVE = amount, MIX = clean / distorted. Keeps its low-frequency weight under heavy drive, a subtle low-pass character, smooth at high DRIVE and MIX. A 100 uF electrolytic sits in the distortion circuit (a builder fitted it backwards). No schematic is published. | Manual V2.1; FutureMusic and Sinesquares reviews; Mod Wiggler DIY thread (search excerpts) |

## What the engine does

1. **Oscillator**: a Schmitt-trigger relaxation oscillator is simulated directly: a timing capacitor charged and discharged
   through the gate output between the HEF4093 thresholds. The triangle is the capacitor voltage, the square is the gate output
   (PolyBLEP-corrected at the exact crossing time). Thresholds, duty and level are drawn per voice from a seed (tolerance), so
   each pad/instance is a slightly different "unit". Replaces the plugin's low-passed-square triangle.
2. **Envelope**: RC charge/discharge (not linear ramps). Touch drive sets how far and how fast it opens (attack time constant
   0.2 s at full touch, slower for light touch); release time constant 6.6 s (-60 dB in ~23 s, +-20 % per voice), FAST -60 dB in
   0.5 s. The envelope is also a waveshaper: a transistor-like asymmetric curve whose bend grows as the
   envelope closes. Hold follows the sourced thresholds (slow voices from 0.5, FAST from 0.7 of the knob).
3. **No output VCA**: closed voices leak into the mix (rising with pitch), voices bleed into their neighbours' timing node
   (shared-supply coupling, the 4093s share a rail).
4. **FM is an additive deviation in Hz** proportional to the modulation and to the voice's timing capacitor (the geometric middle
   of its TUNE range): the higher a voice is tuned the less it is modulated, as the manual says, and a low voice can be
   pushed to a stall.
5. **Eight independent vibratos**, 3.5-6.5 Hz from the BOM's resistor values, shuffled among the voices per unit.
6. **Delay**: the PT2399 is modelled as a fixed 44,000-entry memory clocked at the VCO rate: delay = 44000 / sample-rate steps.
   Above ~0.92 s the memory is clocked below the audio rate, and the input goes through an adaptive 1-bit delta codec, so long
   delays get the real chip's coarse sampling, aliasing and noise instead of a clean tap. SELF/LFO modulate the clock, not a
   tap position. The two lines share one feedback loop. The plugin's compressor/expander are gone (they are not in the hardware);
   the loop saturates like op-amps/PT2399 overload.
7. **Output**: bass lift and soft top end after the distortion. The distortion (build 96, after a comparison with the real one: more
   DRIVE gave it more bass and body, the model only more edge): the distorted signal keeps its level (it was divided by up to 4x the drive
   gain), a one-pole low-pass after the clipper darkens it as DRIVE rises (6 kHz to 2.2 kHz), and the bass lift grows with DRIVE x MIX
   (0.6 -> about 1.9, i.e. +4 dB -> about +9 dB below ~150 Hz); half of the rise in level is taken back (up to -5.6 dB). Emulator, two low
   voices: bass +7 dB at full drive, the top 14 dB down; a mid voice at full drive: 5 dB more bass, the top 4 dB lower than before.

## Assumptions (not documented)

- Magnitudes of: voice leak and neighbour coupling, envelope-to-pitch coupling, the waveshaper bend, tolerance spreads, vibrato
  depths, delay filter corners, ADM step limits, tilt amounts, TIME knob taper (exponential 31 ms to 5.9 s), the full-touch
  attack time constant (the DIY docs give a resistor range, not the point it is set at).
- The FM scale: the deviation per unit of modulation is taken as the middle of the voice's tuning range; the real value depends on
  the resistor and the voice's capacitor. The *form* (additive, stalling) follows the docs, the size is a guess.
- The EQ stage of the board is not modelled separately (the tilt after the distortion stands in for it).
- Whether the plugin's TUNE-to-Hz table matches the hardware: kept as is.

Not modelled: EXT IN, CV inputs, HOLD GATE (no equivalent on the Blackbox pad), the headphone amp.
