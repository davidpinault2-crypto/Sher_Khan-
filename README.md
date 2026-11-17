# Sher Khan Filterbank

A model of the **Sherman Filterbank 2**: two multimode filters with an input overdrive, ADSR / AR / envelope
follower, LFO, audio FM and AM, laid out like the hardware's front panel. It runs on the **Akai Force** and the other
MPCs as a native MPC OS insert effect: a VST2 plugin with three touchscreen pages in the unit's silver-grey look. Its
full state is saved with the project, and it has factory and user presets. See **[mpc/README.md](mpc/README.md)** to
install it. The same DSP core also builds as an LV2 plugin for desktop listening tests.

**Downloads:**
- MPC OS **3.10.1 or later**, any MPC: [`dist/SK-Filterbank-0.4.0-mpc-sdk.zip`](dist/SK-Filterbank-0.4.0-mpc-sdk.zip),
  a folder for `VST2 Plugins` on a drive (Akai's official external plugin support, no root).
- MPC OS **3.10.0 or older**, Force and the other Gen1 MPCs: [`dist/SK-Filterbank-0.4.0-mpc-armv7.zip`](dist/SK-Filterbank-0.4.0-mpc-armv7.zip),
  installed over SSH with root from MockbaMod.

**Also in this repository: [SK TI Link](virus/README.md)**, a plugin that drives an Access Virus TI / TI2 connected to
the MPC by USB (MIDI only: Linux has no driver for the TI's USB audio). Every Single parameter on 15 pages, READ / SEND /
LOAD / STORE, notes from the track, the patch saved with the project. Downloads:
[`SK-TI-Link-0.1.0-mpc-sdk.zip`](dist/SK-TI-Link-0.1.0-mpc-sdk.zip) (MPC OS 3.10.1+) and
[`SK-TI-Link-0.1.0-mpc-armv7.zip`](dist/SK-TI-Link-0.1.0-mpc-armv7.zip) (3.10.0 and older, MockbaMod).

**New in 0.4.0** ([details](mpc/README.md#whats-new-in-040)). Projects and User presets from 0.2.0 on load unchanged.
- **Akai's official route:** a package in the MPC Plugin Builder SDK's folder format, with armv7 and arm64 builds.
- **Checked against Akai's published host contract**, which fixed:
  - a +21 dB one-buffer burst when MPC nudges the first parameter during playback (3.9.1 does it on STOP);
  - SAVE and LOAD touching the drive on the audio thread;
  - the PRESET menu closing after every pick;
  - double-firing buttons;
  - Q-Link turns that stepped switches the wrong way.
- **Better sound:** the top end stays open. With the filters wide open, 18 kHz is 0.5 dB down instead of 0.95 dB,
  because the overdrive's anti-aliasing no longer dulls the wet path.
- **Less CPU in silence** (about half of 0.3.1), and less per-buffer overhead with small buffers.

## Read this first

- **Not made or endorsed by Akai.** MPC OS 3.10.1 and later load external VST2 plugins officially (Preferences >
  Enhancements). On 3.10.0 and older, loading one needs root (MockbaMod) and an edit of MPC's plugin list; the included
  installer backs that list up first. Use at your own risk.
- **Akai did not open-source MPC OS.** It published the MPC Plugin Builder SDK (the host contract and folder format
  this plugin follows) and the source of the device's GPL parts. The plugin host itself is closed.
- **Not a 1:1 clone.** No schematics, impulse responses or recordings of a unit went into it. The controls and their
  behaviour follow the Filterbank 2 manual excerpts and reviews that are public. Where those are silent (the 11
  harmonic ratios, the exact curves, the knob colours), the choice is ours and marked in the code. Matching a specific
  unit "exactly" needs one on the bench (see *Calibration*).
- **Not yet run on an MPC.** Both ARM binaries pass the full host test under emulation, both packages pass a check
  against Akai's folder rules, and the MockbaMod installer passed dry runs under the Force's BusyBox version. Loading,
  CPU and the pages on a real device are still unverified ([mpc/README.md](mpc/README.md) lists exactly what was
  checked).

## Signal path

```
in ─► INPUT gain (-30..+40 dB, unity at noon) ─► Hi Cut/Flat/Hi Boost ─► overdrive (asymmetric soft clip, ADAA)
   ─► [switched-cap sampling @ 50 x fc] ─► FILTER 1 (L-B-H morph + Correction) ─┐
   ─► PAR/SER blend: parallel (1 + 2) ... serial (1 into 2) ─► FILTER 2 ────────┴─► AM (by filter 2's output)
   ─► safety limiter (linear to 0 dBFS, soft up to +12 dBFS) ─► top-end compensation ─► DC block ─► OUTPUT
   ─► BYP/EFF with the latency-matched dry signal
cutoffs = FREQ + ADSR depth x envelope + LFO (filter 2 in or out of phase) + FM (the input audio);
HARMONICS can lock filter 2 to filter 1 at a fixed ratio.
envelope = ADSR / AR / follower, triggered by the level after INPUT (SENSITRIG = twice as sensitive), or TRIG
```

Everything between the overdrive and the limiter runs **4x oversampled** through two linear-phase halfband FIR stages
(63 and 21 taps). A 5-tap linear-phase filter afterwards undoes the low-pass that the overdrive's anti-aliasing adds
(a two-sample average at 4x). Together: 38 samples of latency, reported to the host and matched on the dry path.

| Hardware trait | How it is modelled |
|---|---|
| LTC1060 switched-capacitor filters | 12 dB/oct TPT state-variable filters. The input is sample-and-held at the filter clock (50 x cutoff), so low cutoffs fold high content down. Both that and the clock bleed are scaled by **Grit**. The sampling acts below ~1.7 kHz, where the clock enters the audio-rate model; the bleed acts only while the clock is audible (fading out from 10 to 20 kHz, cutoffs ~200-400 Hz) |
| Screaming resonance, self-oscillation | Amplitude-dependent damping (Van der Pol style). Resonance compresses on loud input, and full resonance starts oscillating from the -110 dBFS noise floor and settles at a stable, loud level. Filter 1 only just gets there; filter 2 screams. Small signals see an exactly linear filter |
| Input overdrive | Asymmetric Pade-tanh clipper (even harmonics), unity small-signal gain, first-order antiderivative anti-aliasing |
| Correction knob | Adds band-pass (left) or LP + HP - BP (right), so band-pass mode becomes a notch |

Measured (see `tests/test_core.cpp`): with both filters wide open, 10 kHz is within 0.15 dB and 18 kHz within 0.6 dB
(0.11 and 0.51 dB; what's left is the filters themselves at their 40 kHz maximum). Worst alias below 16 kHz on a
3.7 kHz tone is -76 dBc at +30 dB input and -56 dBc at +40 dB.

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
| NEON | Every MPC has it, and both MPC builds use it (armv7 with NEON and VFPv4, as Akai's SDK builds; arm64). The oversampled path runs a whole segment at a time: the FIRs four outputs at a time, the overdrive four steps at a time (every ADAA case, knee crossings included, with no division), the two filters both channels at a time with their states in registers. Without NEON (or with `SK_NO_NEON=1`) the plain path runs; the two agree to -110 dB. |
| Knob-derived settings | The smoothing targets and the envelope and LFO coefficients (a dozen pow/exp) are worked out only after a parameter changed, not on every buffer. |
| Sleep | After 256 samples of input under -140 dBFS, with the output under -90 dBFS and nothing able to sound by itself, the DSP stops and the output is exact silence. Input, a parameter change or TRIG wakes it. The silence check itself takes the largest magnitude as an integer max, four samples at a time. |

## Build and test

```sh
make && make test        # LV2 bundle, x86 VST2 build; DSP tests (50) + VST2 host test (68)
make force test-force    # the armv7 binary (arm-linux-gnueabihf-g++); under qemu-arm the DSP tests on the NEON path
                         # (including NEON vs plain) and the host test with and without NEON
make arm64 test-arm64    # the same for the arm64 binary (aarch64-linux-gnu-g++, qemu-aarch64)
mpc/build.sh             # everything above + binary checks + skin + both zips (needs the mpc-vst-plugins checkout, see mpc/README.md)
```

To listen on a desktop, copy `lv2/sherkhan.lv2` to `~/.lv2/` and load it in any LV2 host (Carla, Ardour, jalv). Its
controls are the same 29 normalized sound parameters as the MPC version (`lv2/gen_ttl.py` writes them from
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
- `mpc/`: the MPC OS plugin (VST2 wrapper with presets and state, pages, knob art, build, folder check, bench, install guide).
- `lv2/`: desktop LV2 wrapper.
- `tests/`: DSP tests and the VST2 host test.
- `dist/`: the built packages: `-mpc-sdk` (MPC OS 3.10.1+) and `-mpc-armv7` (MockbaMod, 3.10.0 and older).
