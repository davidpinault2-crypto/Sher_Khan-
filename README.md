# Sher Khan Filterbank

A dual multimode filter + overdrive modelled on the **Sherman Filterbank 2**,
packaged as an LV2 plugin with a dependency-free C++ DSP core.

## Read this first: what this is and isn't

- **Akai Force OS 3.9 has no public plugin SDK.** Standalone Force only loads
  plugins Akai ships or licenses (AIR, Akai Pro, NI MPC Editions). Nothing
  here installs on stock firmware. Getting it onto a Force needs a community
  mod that can host LV2 or native ARM binaries. Doing that is at your own risk
  and may void your warranty.
- **This is not a 1:1 clone.** No schematics, impulse responses or hardware
  captures went into it. The model follows public descriptions of the unit.
  Matching a specific unit "exactly" needs a real unit and a measurement pass
  (see *Calibration* below).

## Signal path

```
in ─► VCA overdrive (asym. soft clip, 0..+40 dB)
   ─► [switched-cap sampling @ 50×fc] ─► SVF A (LP↔BP↔HP, 12 dB/oct)
   ─► series: SVF B   |   parallel: (A + B) / 2
   ─► output headroom clip ─► DC block ─► dry/wet
cutoff mod = envelope follower (±6 oct) + LFO (0..3 oct)
```

All nonlinear stages run 2× oversampled.

| Hardware trait | How it is modelled |
|---|---|
| LTC1060 switched-capacitor filters | TPT state-variable filter. The input is zero-order-held at clock = 50 × cutoff, which folds HF down at low cutoffs. Clock feedthrough is added and scaled by **Grit**. |
| Screaming resonance / self-oscillation | Negative damping at the top of **Reso**, bounded by saturating integrator states |
| Input overdrive | Asymmetric Padé-tanh clipper (even harmonics), unity small-signal at drive 0 |

## Parameters

Drive · Freq A/B · Reso A/B · Mode A/B (0 = LP, 0.5 = BP, 1 = HP) · Parallel ·
Env Amount/Attack/Release · LFO Rate/Amount · Grit · Output · Mix

## Build & test

```sh
make          # builds lv2/sherkhan.lv2/sherkhan.so
make test     # behavioural tests (filter slopes, resonance, self-osc, stability)
```

To cross-compile for an ARM target such as Force, pass your cross toolchain,
e.g. `make CXX=arm-linux-gnueabihf-g++`. Check the actual CPU architecture and
glibc of the firmware you're targeting first. The LV2 ABI is declared inline,
so you don't need the lv2 headers.

To try it on desktop, copy `lv2/sherkhan.lv2` to `~/.lv2/` and load it in any
LV2 host (Carla, Ardour, jalv).

## Calibration (to get closer to "exact")

1. Record sine sweeps and noise through the hardware at a grid of
   Freq/Reso/Mode/Drive settings.
2. Fit the cutoff knob law, damping law, clock ratio (50:1 vs 100:1), and
   overdrive bias/curve against those recordings.
3. Add the fitted curves to `paramToOct`, `damping`, `kClockRatio` and `kBias`
   in `dsp/ShermanCore.h`, and turn the measurements into regression tests.
