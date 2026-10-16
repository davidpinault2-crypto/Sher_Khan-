# SK Filterbank on the Akai Force (MPC OS 3.9, MockbaMod)

The filter runs on the Force as a **native insert effect** with three touchscreen pages, Q-Links, presets in MPC's
PRESET menu, and its full state saved with your project. MPC OS has a VST2 host built in: a JUCE plugin loader that
Akai uses for its own plugins. Community tooling ([mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins), MIT)
worked out how third-party plugins can use it. Root access is needed to register a plugin, and MockbaMod provides it
over SSH.

| FILTERS | MODULATION | PRESETS |
|---|---|---|
| ![Filters page](../docs/mpc-filters.png) | ![Modulation page](../docs/mpc-modulation.png) | ![Presets page](../docs/mpc-presets.png) |

*Offline previews of the pages. On the device MPC draws the names and values itself (the boxes under the knobs) in
its own font, using the plugin's parameter names, for example "LFO Speed".*

## What you need

- An **Akai Force** (Gen1, 32-bit ARM) on **MPC OS 3.x**. Built and checked against the facts the community verified on
  a Force running MPC OS 3.9.1. Gen2 devices (MPC Live III and later, 64-bit) need a different build.
- **MockbaMod** for your firmware. For Force FW 3.5 and later it is distributed through the
  [MockbaMod Discord](https://discord.gg/HvNBpArSSa), not the GitHub page. MockbaMod gives you SSH/SFTP as root.
- A computer on the same network as the Force, with `ssh`/`scp`. On Windows, PuTTY and WinSCP work too.

## Install

1. Download `dist/SK-Filterbank-0.2.0-mpc-armv7.zip` from this repo.
2. **Save your project.** The installer stops MPC and starts it again.
3. Copy the zip to the Force and run the installer:
   ```sh
   scp SK-Filterbank-0.2.0-mpc-armv7.zip root@<force-ip>:/tmp/
   ssh root@<force-ip>
   cd /tmp && unzip -o SK-Filterbank-0.2.0-mpc-armv7.zip && sh SK-Filterbank-0.2.0/install.sh
   ```
   The installer is the toolkit's tested one:
   - It checks the device and stops MPC (the `acvs` or `inmusic-mpc` service).
   - It copies the plugin folder to `/sdcard/Synths/Sher Khan - VST - SK Filterbank/`.
   - It backs up `MPC.settings` (as `MPC.settings.bak-sk_filterbank-<date>` next to it) and adds one entry to MPC's
     plugin list (`pluginList-arm`), refusing to save an edit that isn't valid.
   - It starts MPC again.

   Running it again upgrades in place and **keeps your User presets** (the `presets/` folder).
4. On the Force, add **SK Filterbank** (manufacturer *Sher Khan*) to a track's insert effects from the plugin effects.
   The FILTERS / MODULATION / PRESETS tabs at the bottom switch pages, and the Q-Links follow the page.

Upgrading from 0.1.0: the controls changed completely, so a project saved with 0.1.0 opens with this plugin at its
Init sound.

Install on the internal drive (the default). A Force SSD is usually mounted `noexec`. MPC then lists the plugin but can
only show "Load Plugin", because it can't load code from that drive.

If you use MockbaMod's VST scanner (`vstscanner.sh`), it rebuilds the plugin list from the `plugin-meta.xml` files in the
`Synths` folders. This plugin folder has one, so it survives a rescan.

### Uninstall

```sh
ssh root@<force-ip> sh /tmp/SK-Filterbank-0.2.0/uninstall.sh
```
(If `/tmp` was cleared by a reboot, copy and unzip the package again first.) This stops MPC, removes the plugin and its
plugin-list entry (after a backup), and starts MPC. Your `presets/` folder is kept. Projects that used the plugin open
without it.

## Your settings and presets

- **With the project, automatically.** Every control is saved in the plugin's state when you save the project or the
  track program, and restored when you load it. There is no separate step: move a knob, save the project, and it comes
  back that way. The preset you last picked is saved too. MPC re-selecting that preset when the project opens doesn't
  overwrite your tweaks.
- **MPC's PRESET menu** lists the 12 factory sounds and the 16 User slots. Picking one loads it. Picking the one that
  is already selected does nothing (that's what protects a project's tweaks), so to go back to a preset after
  tweaking it, press LOAD on the PRESETS page.
- **PRESETS page:**
  - The arrows (or Q-Link 1) pick a preset, and **LOAD** recalls it.
  - **SAVE** stores the current sound in the selected User slot. Factory presets are read-only, and the status line
    says what happened.
  - User presets are small text files in `/sdcard/Synths/Sher Khan - VST - SK Filterbank/presets/` (`user_01.skfb` to
    `user_16.skfb`). Back them up or copy them to another Force like any file.
  - MPC itself has no "save preset" for third-party plugins, so saving happens on this page.

Factory presets: Init, Acid Squelch, Funk Follower, Twin Notch Sweep, Stereo Wobble, Vowel Bank, Drum Crusher,
Screaming Ring, Audio FM Growl, Sub Rumble, Harmonic Pluck, Slow Motion.

## The controls

The controls follow the Filterbank 2's front panel as its manual excerpts and reviews describe it. Each page row is one
Q-Link bank (Force knobs 1-8).

**FILTERS page**

| Bank | Knobs 1-8 |
|---|---|
| 1 | Input, Freq 1, Reso 1, ADSR 1, L-B-H 1, Corr 1, Par/Ser, Byp/Eff |
| 2 | FM, Freq 2, Reso 2, ADSR 2, L-B-H 2, Corr 2, AM, Output |

**MODULATION page**

| Bank | Knobs 1-8 |
|---|---|
| 1 | Attack, Decay, Sustain, Release, LFO Speed, LFO Depth, Harmonics, Grit |
| 2 | Env Mode, Trig Mode, –, LFO Shape, LFO F2, Input EQ, –, – |

On the MODULATION page the bottom-row switches step one position per Q-Link click, and the TRIG button sits between
them.

| Control | What it does |
|---|---|
| Input | -30 dB to +40 dB, unity at noon. Past noon it saturates, then distorts. It is also the trigger sensitivity, as on the unit |
| Input EQ | Hi Cut / Flat / Hi Boost shelf at 2.5 kHz, before the drive |
| Freq 1 / 2 | 20 Hz to 40 kHz |
| Reso 1 / 2 | Filter 1 only just reaches self-oscillation ("partial", as the manual says). Filter 2 screams from about 88% |
| ADSR 1 / 2 | Envelope depth: filter 1 0 to +6 octaves, filter 2 bipolar ±6 octaves |
| L-B-H 1 / 2 | Continuous lowpass > bandpass > highpass |
| Corr 1 / 2 | Correction: left adds band-pass, centre is off, right subtracts it and adds LP+HP, so BP + full right is a notch |
| Par/Ser | Continuous blend from parallel (1 + 2) to serial (1 into 2) |
| Harmonics | Free, or filter 2 locked to filter 1 at 1:1, 2:3, 1:2, 1:3, 1:4, 1:5, 1:6, 1:8, 1:10, 1:12, 1:16. Freq 2 then reads "Locked" |
| Attack / Decay / Sustain / Release | 1 ms-5 s / 5 ms-10 s / bipolar ±100% / 5 ms-10 s |
| Env Mode | ADSR (held while the input is above the trigger level), AR (one shot per trigger), Follow (envelope follower; Sustain sets its sensitivity) |
| Trig Mode | Normal; SensiTrig (twice as sensitive); Limit (gives the filters 6 dB more headroom when the input is overdriven) |
| TRIG | Fires the envelope by hand (and restarts a Sine Trig LFO) |
| LFO Speed / Depth | One cycle a minute up to 3 kHz (audio rate) / bipolar ±4 octaves |
| LFO Shape / LFO F2 | Sine, Saw, Sine Trig (restarts on each trigger) / filter 2 moves with filter 1 or opposite |
| FM | The input audio sweeps both cutoffs |
| AM | The output is amplitude-modulated by filter 2's output (try it with Reso 2 self-oscillating) |
| Byp/Eff | Dry/wet. The dry signal is delay-compensated, so there's no comb filtering |
| Output, Grit | Plugin additions: output level, and how much of the switched-capacitor sampling and clock bleed you hear (only below ~1.7 kHz cutoff) |

The plugin reports 36 samples (0.8 ms) of latency. That is the 4x oversampling filters.

## Built and checked so far

| Check | Result |
|---|---|
| DSP tests (`make test`) | 38/38: unity at noon, filter slopes, notch from the Correction knob, resonance, filter 2 self-oscillation, harmonic lock, Par/Ser, Hi Cut/Boost, ADSR/AR/follower, SensiTrig, TRIG, LFO rate, AM and FM sidebands, grit, aliasing, NaN safety, stress |
| VST2 host test, x86 and the **real ARM binary under qemu-arm** | 49/49: parameters and value texts, Q-Link nudges on switches, the PRESET menu, user preset save/load, buttons springing back, the project chunk with the preset, chunk-load jumps, in-place buffers, presets folder found next to the `.so` |
| Binary | armv7 hard-float, exports only `VSTPluginMain`, needs only libc/libm, highest glibc symbol 2.29 (the Force has 2.39) |
| Installer | install, upgrade and uninstall under BusyBox 1.36.1 (the Force's version) against a mock `MPC.settings`: one entry, other plugins untouched, User presets kept on upgrade and uninstall |
| CPU | About 60 µs per 128-frame block on an x86 VM (2% of the block). **Not measured on a Force.** Expect roughly 3x that on its Cortex-A17 |

**Not verified:**
- Loading on a real Force.
- The pages and knob art on the real screen, the PRESET menu and project reload on the device.
- CPU on the RK3288.
- The sound and the panel colours against a real Filterbank 2. The knob colours are a guess at its function coding;
  edit `KNOB_COLOURS` in `skin_post.py` to change them.

Please run the device CPU check before relying on several instances:

```sh
MPC_VST=../mpc-vst-plugins mpc/bench.sh <force-ip> -p    # PASS / WARN / FAIL, nothing is installed
```

## Troubleshooting

- **Listed, but only "Load Plugin":** usually installed on a `noexec` drive. Reinstall to the default `/sdcard/Synths`.
- **Gone from the list after a restart:** something rewrote MPC's plugin list. Causes include an edit made while MPC was
  running, or a tool that rebuilds the list. Run `install.sh` again.
- **Changed the skin?** MPC caches skins, so restart MPC (re-inserting the plugin is not enough).
- **SAVE says "Could not save":** the plugin folder isn't writable (for example, it was installed on a read-only
  location). Reinstall to `/sdcard/Synths`.
- **MPC crashes when the plugin loads:** SSH in, run `uninstall.sh` (or restore the `MPC.settings` backup), and open an
  issue with the MPC OS version.

## Rebuilding

```sh
sudo apt install g++-arm-linux-gnueabihf qemu-user python3-pil   # Debian/Ubuntu
git clone https://github.com/sd88me/mpc-vst-plugins ../mpc-vst-plugins
git -C ../mpc-vst-plugins checkout 3a54b72cd172c840881b0be4f887653cc0829bf0
mpc/build.sh 0.2.0          # tests, Force build, binary checks, skin, dist/SK-Filterbank-0.2.0-mpc-armv7.zip
```

Files:
- `sk_vst.cpp`: the VST2 wrapper (float audio, hand-written ABI, presets, state).
- `params.json` and `layout.conf`: the pages and Q-Links.
- `skin_post.py`: the colour-coded knobs and page texts.
- `check_params.py`: keeps the files above in step with the plugin, since saved projects bind parameters by index.
  Never reorder them, only append.
- `build.sh` and `bench.sh`: the build and the device CPU check.
