# Sher Khan Filterbank

A model of the **Sherman Filterbank 2**: two multimode filters with an input overdrive, ADSR / AR / envelope
follower, LFO, audio FM and AM, laid out like the hardware's front panel. It runs on the **Akai Force** as a native
MPC OS insert effect: a VST2 plugin with three touchscreen pages in the unit's silver-grey look. Its full state is
saved with the project, it has factory and user presets, and it installs with root access from MockbaMod. See
**[mpc/README.md](mpc/README.md)** to install it. The same DSP core also builds as an LV2 plugin for desktop
listening tests.

**Download for the Force:** [`dist/SK-Filterbank-0.3.1-mpc-armv7.zip`](dist/SK-Filterbank-0.3.1-mpc-armv7.zip)

**New in 0.3.1:** less CPU again. Controls and saved state are unchanged.
- **Silence costs almost nothing.** After a few ms of silent input, the plugin stops processing and outputs exact
  silence, provided nothing in it can sound by itself: no self-oscillation, envelope at rest, no knob gliding. MPC
  keeps feeding silent tracks and a stopped transport, so in a real project this is most of the saving. Any input,
  knob change or TRIG wakes it at once.
- **Faster with sound too.** With NEON the oversampled path now runs 32 samples at a time instead of one. Per block
  that is 170k instructions instead of 221k for the Init sound (23% fewer, 67% fewer than 0.2.0). Heavily driven
  sounds gain the most (Crusher: 292k to 217k).
- **One sound change: grit's clock bleed is now heard only while the clock itself is audible** (cutoffs under about
  200-400 Hz). Above that the real chip's clock is ultrasonic. The plugin's square wave up there only aliased down
  into a faint whine, about -72 dBFS on silent input with the default settings. That whine is gone, which also lets
  the plugin sleep. The grit sampling effect is unchanged.

**0.3.0:** less CPU and smoother operation, with the same controls. The sound changed in three details:
- The output safety limiter is now exactly linear up to 0 dBFS (0.2.0's took 0.16 dB off a full-scale signal).
- With LIMIT on, AM and the clock bleed sound the same as with it off.
- The cutoff knobs glide over 30 ms instead of 15.

The other changes:
- **CPU.** With NEON, which the Force's processor has, it runs 57% fewer instructions per block than 0.2.0. That is
  counted on the ARM build under emulation, not yet timed on a Force. The audio path makes no divisions (0.2.0 made
  46 per sample) and normally no math-library calls (0.2.0 made at least four per sample).
- **Switches.** LIMIT, HARMONICS, LFO F2 and the LFO shape no longer click when you switch them during playback.
- **Presets.** Picking a preset during playback crossfades from the old sound to the new one in 30 ms.

Projects and User presets from 0.2.0 and 0.3.0 load unchanged. See [mpc/README.md](mpc/README.md#whats-new-in-031).

## Read this first

- **Unofficial.** Akai doesn't support third-party plugins on standalone MPC OS. Loading one needs root (MockbaMod) and
  an edit of MPC's plugin list. The included installer backs that list up first. Use at your own risk.
- **Not a 1:1 clone.** No schematics, impulse responses or recordings of a unit went into it. The controls and their
  behaviour follow the Filterbank 2 manual excerpts and reviews that are public. Where those are silent (the 11
  harmonic ratios, the exact curves, the knob colours), the choice is ours and marked in the code. Matching a specific
  unit "exactly" needs one on the bench (see *Calibration*).
- **Not yet run on a Force.** The ARM binary passes the full host test under emulation and the installer passes dry
  runs under the Force's BusyBox version. Loading, CPU and the pages on the real device are still unverified
  ([mpc/README.md](mpc/README.md) lists exactly what was checked).

## Signal path

```
in ─► INPUT gain (-30..+40 dB, unity at noon) ─► Hi Cut/Flat/Hi Boost ─► overdrive (asymmetric soft clip, ADAA)
   ─► [switched-cap sampling @ 50 x fc] ─► FILTER 1 (L-B-H morph + Correction) ─┐
   ─► PAR/SER blend: parallel (1 + 2) ... serial (1 into 2) ─► FILTER 2 ────────┴─► AM (by filter 2's output)
   ─► safety limiter (linear to 0 dBFS, soft up to +12 dBFS) ─► DC block ─► OUTPUT ─► BYP/EFF with the latency-matched dry signal
cutoffs = FREQ + ADSR depth x envelope + LFO (filter 2 in or out of phase) + FM (the input audio);
HARMONICS can lock filter 2 to filter 1 at a fixed ratio.
envelope = ADSR / AR / follower, triggered by the level after INPUT (SENSITRIG = twice as sensitive), or TRIG
```

Everything between the overdrive and the limiter runs **4x oversampled** through two linear-phase halfband FIR stages
(63 and 21 taps, 36 samples latency, reported to the host).

| Hardware trait | How it is modelled |
|---|---|
| LTC1060 switched-capacitor filters | 12 dB/oct TPT state-variable filters. The input is sample-and-held at the filter clock (50 x cutoff), so low cutoffs fold high content down. Both that and the clock bleed are scaled by **Grit**. The sampling acts below ~1.7 kHz, where the clock enters the audio-rate model; the bleed acts only while the clock is audible (fading out from 10 to 20 kHz, cutoffs ~200-400 Hz) |
| Screaming resonance, self-oscillation | Amplitude-dependent damping (Van der Pol style). Resonance compresses on loud input, and full resonance starts oscillating from the -110 dBFS noise floor and settles at a stable, loud level. Filter 1 only just gets there; filter 2 screams. Small signals see an exactly linear filter |
| Input overdrive | Asymmetric Pade-tanh clipper (even harmonics), unity small-signal gain, first-order antiderivative anti-aliasing |
| Correction knob | Adds band-pass (left) or LP + HP - BP (right), so band-pass mode becomes a notch |

Measured (see `tests/test_core.cpp`): the passband is flat to within 0.1 dB, and with both filters open 10 kHz is within
0.3 dB. Worst alias below 16 kHz on a 3.7 kHz tone is -76 dBc at +30 dB input and -56 dBc at +40 dB.

| Smooth operation | How |
|---|---|
| Knobs | Every value glides: the cutoffs and the harmonic ratio over 30 ms, everything else over 15 ms |
| LIMIT | Rescales the filters' internal state with the level, so the output doesn't jump |
| HARMONICS, LFO F2 | Crossfade between free and locked, same and inverted (15 ms) |
| LFO shape, SINE TRIG | A new shape glides in over 30 ms; a restart glides over 2 ms instead of jumping |
| Presets | The old and the new sound run side by side for a 30 ms crossfade (twice the CPU for that moment) |

| CPU | How |
|---|---|
| No divisions, (almost) no libm on the audio path | The cutoff comes from a compile-time table. The divisions that remain share one reciprocal (Montgomery's trick), and the output limiter needs none below 0 dBFS. |
| Segments | Each block runs in segments of up to 32 samples: control per sample, then the oversampled path, then the output. Glides are linear ramps per segment that cost nothing once settled, and steady cutoffs are worked out once per segment. |
| NEON, picked at load time | The oversampled path runs a whole segment at a time: the FIRs four outputs at a time, the overdrive four steps at a time (every ADAA case, knee crossings included, with no division), the two filters both channels at a time with their states in registers. Without NEON (or with `SK_NO_NEON=1`) the plain path runs; the two agree to -113 dB. |
| Sleep | After 256 samples of input under -140 dBFS, with the output under -90 dBFS and nothing able to sound by itself, the DSP stops and the output is exact silence. Input, a parameter change or TRIG wakes it. |

## Build and test

```sh
make && make test        # LV2 bundle, x86 VST2 build; DSP tests (49) + VST2 host test (51)
make force test-force    # the Force binary (arm-linux-gnueabihf-g++); under qemu-arm the DSP tests on the NEON path
                         # (including NEON vs plain) and the host test with and without NEON
mpc/build.sh             # everything above + skin + installer zip (needs the mpc-vst-plugins checkout, see mpc/README.md)
```

To listen on a desktop, copy `lv2/sherkhan.lv2` to `~/.lv2/` and load it in any LV2 host (Carla, Ardour, jalv). Its
controls are the same 29 normalized sound parameters as the Force version (`lv2/gen_ttl.py` writes them from
`mpc/params.json`).

## Calibration (to get closer to "exact")

1. Record sine sweeps and noise through the hardware at a grid of Freq/Reso/Mode/Drive settings.
2. Fit the cutoff knob law, the damping law, the clock ratio (50:1 or 100:1), the resonance limiter and the overdrive
   curve to those recordings.
3. Put the fitted curves into `sk::map` (including `kHarmonicRatio`), the resonance ranges in `computeTargets`,
   `kClockRatio`, `SVF::kVdP`, `kBias` and `kTrigThreshold` in `dsp/ShermanCore.h`, and turn the measurements into
   regression tests.

## Layout

- `dsp/ShermanCore.h`: the whole DSP model, header-only, no dependencies.
- `mpc/`: the Force/MPC OS plugin (VST2 wrapper with presets and state, pages, knob art, build, bench, install guide).
- `lv2/`: desktop LV2 wrapper.
- `tests/`: DSP tests and the VST2 host test.
- `dist/`: the built Force package.
