# Sher Khan Filterbank

A dual multimode filter with input overdrive, modelled on the **Sherman Filterbank 2**. It runs on the
**Akai Force** as a native MPC OS insert effect: a VST2 plugin with its own touchscreen page, installed with
root access from MockbaMod. See **[mpc/README.md](mpc/README.md)** to install it. The same DSP core also builds as an
LV2 plugin for desktop listening tests.

**Download for the Force:** [`dist/SK-Filterbank-0.1.0-mpc-armv7.zip`](dist/SK-Filterbank-0.1.0-mpc-armv7.zip)

## Read this first

- **Unofficial.** Akai doesn't support third-party plugins on standalone MPC OS. Loading one needs root (MockbaMod) and
  an edit of MPC's plugin list. The included installer backs that list up first. Use at your own risk.
- **Not a 1:1 clone.** No schematics, impulse responses or recordings of a unit went into it. The model follows public
  descriptions of the hardware. Matching a specific unit "exactly" needs one on the bench (see *Calibration*).
- **Not yet run on a Force.** The ARM binary passes the full host test under emulation and the installer passes dry
  runs under the Force's BusyBox version. Loading, CPU and the page on the real device are still unverified
  ([mpc/README.md](mpc/README.md) lists exactly what was checked).

## Signal path

```
in ─► drive (asymmetric soft clip, 0..+40 dB, ADAA) ─► [switched-cap sampling @ 50 x fc] ─► SVF A (LP<>BP<>HP)
   ─► series: SVF B   |   parallel: (A + B) / 2
   ─► safety limiter (+12 dBFS) ─► DC block ─► output gain ─► mix with the latency-matched dry signal
cutoff = knob + envelope follower (+-6 oct) + LFO (0..3 oct)
```

Everything between the input and the limiter runs **4x oversampled** through two linear-phase halfband FIR stages
(63 and 21 taps, 36 samples latency, reported to the host).

| Hardware trait | How it is modelled |
|---|---|
| LTC1060 switched-capacitor filters | 12 dB/oct TPT state-variable filters. The input is sample-and-held at the filter clock (50 x cutoff), so low cutoffs fold high content down. Clock bleed is scaled by **Grit**, which only acts below ~1.7 kHz, where the clock enters the audio-rate model |
| Screaming resonance, self-oscillation | Amplitude-dependent damping (Van der Pol style). Resonance compresses on loud input, and full resonance starts oscillating from the -110 dBFS noise floor and settles at a stable, loud level. Small signals see an exactly linear filter |
| Input overdrive | Asymmetric Pade-tanh clipper (even harmonics), unity gain at 0 dB, first-order antiderivative anti-aliasing |

Measured (see `tests/test_core.cpp`): the passband is flat to within 0.1 dB, and with both filters open 10 kHz is within
0.3 dB. Worst alias below 16 kHz on a 3.7 kHz tone is -71 dBc at +30 dB drive and -55 dBc at +40 dB.

## Build and test

```sh
make && make test        # LV2 bundle, x86 VST2 build; DSP tests (19) + VST2 host test (30)
make force test-force    # the Force binary (arm-linux-gnueabihf-g++) and the host test under qemu-arm
mpc/build.sh             # everything above + skin + installer zip (needs the mpc-vst-plugins checkout, see mpc/README.md)
```

To listen on a desktop, copy `lv2/sherkhan.lv2` to `~/.lv2/` and load it in any LV2 host (Carla, Ardour, jalv). Its
controls are the same 16 normalized parameters as the Force version.

## Calibration (to get closer to "exact")

1. Record sine sweeps and noise through the hardware at a grid of Freq/Reso/Mode/Drive settings.
2. Fit the cutoff knob law, the damping law, the clock ratio (50:1 or 100:1), the resonance limiter and the overdrive
   curve to those recordings.
3. Put the fitted curves into `sk::map`, `damping`, `kClockRatio`, `SVF::kVdP` and `kBias` in `dsp/ShermanCore.h`, and
   turn the measurements into regression tests.

## Layout

- `dsp/ShermanCore.h`: the whole DSP model, header-only, no dependencies.
- `mpc/`: the Force/MPC OS plugin (VST2 wrapper, page layout, build, bench, install guide).
- `lv2/`: desktop LV2 wrapper.
- `tests/`: DSP tests and the VST2 host test.
- `dist/`: the built Force package.
