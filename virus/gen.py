#!/usr/bin/env python3
"""Generates SK TI Link's parameter table and its MPC pages from virus/data/ti_params.json:

    virus/ti_table.h        the C++ table the plugin is built from (parameters, value texts, mode-dependent names)
    virus/mpc/params.json   the VST parameter list the skin is built from (index = VST parameter index)
    virus/mpc/layout.conf   the touchscreen pages (mpc-vst-plugins' shadow layout syntax)

    python3 virus/gen.py            (re-run after editing PAGES or the data; the outputs are committed)

VST parameter order: the LINK controls (CONTROLS), then every Virus parameter in ti_params.json order. Saved projects
bind parameters by index, so only ever append to either list."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# key, name, kind, extra. kinds: readout, option (list of names), stepper (count), button.
CONTROLS = [
    ("status", "Status", "readout", None),
    ("part", "Part", "option", ["Single"] + ["Part %d" % i for i in range(1, 17)]),
    ("midi_ch", "MIDI Ch", "option", ["Ch %d" % i for i in range(1, 17)]),
    ("notes", "Notes", "option", ["Off", "On"]),
    ("auto_send", "AutoSend", "option", ["Off", "On"]),
    ("bank", "Bank", "stepper", 32),
    ("program", "Program", "stepper", 128),
    ("read", "Read", "button", None),
    ("send", "Send", "button", None),
    ("load", "Load", "button", None),
    ("store", "Store", "button", None),
    ("panic", "Panic", "button", None),
    ("patch", "Patch", "readout", None),
    ("bank_prev", "Bank -", "button", None),
    ("bank_next", "Bank +", "button", None),
    ("program_prev", "Prog -", "button", None),
    ("program_next", "Prog +", "button", None),
]
CONTROL_DEFAULTS = {"notes": 1, "auto_send": 1}

# Pages: tab -> rows (top to bottom, 8 knobs at most) of (frame title, keys). Q-Links: rows 1-2 are the tab's first
# sub-page (knob banks 1 and 2), rows 3-4 its second. Tab and sub-page names are short: MPC's tab strip is narrow.
PAGES = [
    ("OSC", ["Osc 1", "Osc 2"], [
        [("OSCILLATOR 1", ["osc1_mode", "osc1_shape", "osc1_pulsewidth", "osc1_wave_select", "osc1_semitone",
                           "osc1_keyfollow", "osc1_shape_velocity", "pulsewidth_velocity"])],
        [("OSC 1 WAVETABLE / GRAIN / FORMANT", ["osc1_wavetable_sync", "osc1_wavetable_formantshift",
                                                "osc1_wavetable_internal_detune", "osc1_wavetable_interpolation"]),
         ("OSC 2 WAVETABLE / GRAIN / FORMANT", ["osc2_wavetable_sync", "osc2_wavetable_formantshift",
                                                "osc2_wavetable_internal_detune", "osc2_wavetable_interpolation"])],
        [("OSCILLATOR 2", ["osc2_mode", "osc2_shape", "osc2_pulsewidth", "osc2_wave_select", "osc2_semitone",
                           "osc2_detune", "osc2_keyfollow", "osc2_shape_velocity"])],
        [("FM / SYNC", ["osc2_fm_amount", "osc_fm_mode", "fm_amount_velocity", "osc2_sync", "osc2_filt_env_amt",
                        "fm_filt_env_amt"]), ("PHASE", ["osc_init_phase"])],
    ]),
    ("MIX", ["Mix", "Play"], [
        [("OSCILLATOR 3", ["osc3_mode", "osc3_volume", "osc3_semitone", "osc3_detune"]),
         ("SUB / NOISE", ["suboscillator_volume", "suboscillator_shape", "noise_volume", "noise_color"])],
        [("MIXER", ["osc_balance", "osc_mainvolume", "ringmodulator_volume", "patch_volume", "punch_intensity",
                    "panorama", "channel_volume", "balance"])],
        [("UNISON", ["unison_mode", "unison_detune", "unison_pan_spread", "unison_lfo_phase"]),
         ("OUTPUT", ["second_output_balance", "amp_velocity", "panorama_velocity"])],
        [("PLAY", ["transpose", "key_mode", "portamento_time", "control_smooth_mode", "bender_range_up",
                   "bender_range_down", "bender_scale", "clock_tempo"])],
    ]),
    ("FILTER", ["Filters", "Flt Env"], [
        [("FILTER 1", ["cutoff", "filter1_resonance", "filter1_env_amt", "filter1_keyfollow", "filter1_mode",
                       "filter1_env_polarity", "flt1_envamt_velocity", "resonance1_velocity"])],
        [("FILTER 2", ["cutoff2", "filter2_resonance", "filter2_env_amt", "filter2_keyfollow", "filter2_mode",
                       "filter2_env_polarity", "flt2_envamt_velocity", "resonance2_velocity"])],
        [("FILTERS", ["filter_routing", "filter_balance", "saturation_curve", "filter2_cutoff_link",
                      "filter_keytrack_base", "filter_select"])],
        [("FILTER ENVELOPE", ["filter_env_attack", "filter_env_decay", "filter_env_sustain",
                              "filter_env_sustain_time", "filter_env_release"])],
    ]),
    ("ENV", ["Amp+Env3", "Env4+LFO3"], [
        [("AMP ENVELOPE", ["amp_env_attack", "amp_env_decay", "amp_env_sustain", "amp_env_sustain_time",
                           "amp_env_release"])],
        [("ENVELOPE 3", ["envelope_3_attack", "envelope_3_decay", "envelope_3_sustain", "envelope_3_sustain_time",
                         "envelope_3_release"])],
        [("ENVELOPE 4", ["envelope_4_attack", "envelope_4_decay", "envelope_4_sustain", "envelope_4_sustain_time",
                         "envelope_4_release"])],
        [("LFO 3", ["lfo3_rate", "lfo3_shape", "lfo3_mode", "lfo3_keyfollow", "lfo3_destination", "osc_lfo3_amount",
                    "lfo3_fade_in_time", "lfo3_clock"])],
    ]),
    ("LFO", ["LFO 1", "LFO 2"], [
        [("LFO 1", ["lfo1_rate", "lfo1_shape", "lfo1_env_mode", "lfo1_mode", "lfo1_symmetry", "lfo1_keyfollow",
                    "lfo1_keytrigger", "lfo1_clock"])],
        [("LFO 1 AMOUNTS", ["osc1_lfo1_amount", "osc2_lfo1_amount", "pw_lfo1_amount", "reso_lfo1_amount",
                            "filtgain_lfo1_amount"]), ("LFO 1 ASSIGN", ["lfo1_assign_dest", "lfo1_assign_amount"])],
        [("LFO 2", ["lfo2_rate", "lfo2_shape", "lfo2_env_mode", "lfo2_mode", "lfo2_symmetry", "lfo2_keyfollow",
                    "lfo2_keytrigger", "lfo2_clock"])],
        [("LFO 2 AMOUNTS", ["shape_lfo2_amount", "fm_lfo2_amount", "cutoff1_lfo2_amount", "cutoff2_lfo2_amount",
                            "pan_lfo2_amount"]), ("LFO 2 ASSIGN", ["lfo2_assign_dest", "lfo2_assign_amount"])],
    ]),
    ("MOD", ["Slot 1-2", "Slot 3-4"], [
        [("SLOT 1", ["assign1_source", "assign1_destination", "assign1_amount", "assign_1_destination_2",
                     "assign_1_amount_2", "assign_1_destination_3", "assign_1_amount_3"])],
        [("SLOT 2", ["assign2_source", "assign2_destination1", "assign2_amount1", "assign2_destination2",
                     "assign2_amount2", "assign_2_destination_3", "assign_2_amount_3"])],
        [("SLOT 3", ["assign3_source", "assign3_destination1", "assign3_amount1", "assign3_destination2",
                     "assign3_amount2", "assign3_destination3", "assign3_amount3"])],
        [("SLOT 4", ["assign_4_source", "assign_4_destination", "assign_4_amount", "assign_4_destination_2",
                     "assign_4_amount_2", "assign_4_destination_3", "assign_4_amount_3"])],
    ]),
    ("MOD 2", ["Slot 5-6", "Knobs"], [
        [("SLOT 5", ["assign_5_source", "assign_5_destination", "assign_5_amount", "assign_5_destination_2",
                     "assign_5_amount_2", "assign_5_destination_3", "assign_5_amount_3"])],
        [("SLOT 6", ["assign_6_source", "assign_6_destination", "assign_6_amount", "assign_6_destination_2",
                     "assign_6_amount_2", "assign_6_destination_3", "assign_6_amount_3"])],
        [("SOFT KNOBS", ["soft_knob1_shortname", "soft_knob_1_single", "soft_knob2_shortname",
                         "soft_knob_2_single", "soft_knob3_shortname", "soft_knobs_destination_3"])],
        [("CATEGORY", ["category1", "category2"])],
    ]),
    ("FX 1", ["Chorus", "Delay"], [
        [("CHORUS", ["chorus_type", "chorus_mix", "chorus_mix2", "chorus_rate", "chorus_depth", "chorus_delay",
                     "chorus_feedback", "chorus_lfo_shape"])],
        [("CHORUS", ["chorus_x_over"]), ("PHASER", ["phaser_mode", "phaser_mix", "phaser_rate", "phaser_depth",
                                                    "phaser_frequency", "phaser_feedback", "phaser_spread"])],
        [("DELAY", ["delay_mode", "delay_send", "delay_time", "delay_feedback", "dly_rate_rev_decay", "dly_depth",
                    "delay_lfo_shape", "delay_color"])],
        [("DELAY", ["delay_type", "delay_clock"]), ("TAPE DELAY", ["delay_tape_delay_ratio",
                                                                  "delay_tape_delay_left_clock",
                                                                  "delay_tape_delay_right_clock",
                                                                  "delay_tape_delay_bandwidth"])],
    ]),
    ("FX 2", ["Reverb", "EQ"], [
        [("REVERB", ["reverb_mode", "reverb_send", "reverb_type", "reverb_time", "reverb_damping", "reverb_color",
                     "reverb_predelay", "reverb_feedback"])],
        [("REVERB", ["reverb_clock"]), ("DISTORTION", ["distortion_curve", "distortion_intensity",
                                                       "patch_distortion_treble_booster",
                                                       "patch_distortion_high_cut", "patch_distortion_mix",
                                                       "patch_distortion_quality", "patch_distortion_tone127"])],
        [("EQUALIZER", ["loweq_gain", "loweq_frequency", "mideq_gain", "mideq_frequency", "mideq_q_factor",
                        "higheq_gain", "higheq_frequency"])],
        [("ANALOG BOOST", ["bass_intensity", "bass_tune", "character_type"]), ("ATOMIZER", ["atomizer"])],
    ]),
    ("FX 3", ["F.Bank", "Input"], [
        [("FILTER BANK", ["filter_bank_type", "filter_bank_mix", "filter_bank_filter_frequency",
                          "filter_bank_stereo_phase", "filter_bank_filter_type", "filter_bank_slope",
                          "filter_bank_resonance"])],
        [],
        [("VOCODER", ["vocoder_mode"]), ("INPUT", ["input_mode", "input_select", "input_ringmodulator",
                                                   "input_follower_mode"])],
        [],
    ]),
    ("ARP", ["Arp", "Arp"], [
        [("ARPEGGIATOR", ["arp_mode", "arp_pattern_selct", "arp_octave_range", "arp_hold_enable",
                          "arp_note_length", "arp_swing", "arp_clock", "arpeggiator_userpatternlength"])],
        [], [], [],
    ]),
]
for kind, title in (("length", "LEN"), ("velocity", "VEL"), ("bitfield", "BITS")):
    PAGES.append(("STEP " + title, ["1-16", "17-32"], [
        [("STEPS %d-%d" % (r * 8 + 1, r * 8 + 8), ["step_%d_%s" % (r * 8 + s + 1, kind) for s in range(8)])]
        for r in range(4)]))

# The LINK page comes first; it is laid out by hand in layout() (readouts, steppers, buttons).
LINK_SETUP = ["part", "midi_ch", "notes", "auto_send", "part_volume", "part_detune"]
LINK_GLOBAL = ["switches_eq_enable", "switches_arpeggiator_enable", "switches_delay_enable",
               "switches_reverb_enable", "master_tune", "master_volume"]

OPS = {"eq": 0, "ne": 1, "ge": 2, "le": 3}


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def short_text(s):
    """Value texts fit under a knob: drop the space before a unit."""
    for unit in (" %", " ms", " dB", " cm", " bpm"):
        s = s.replace(unit, unit.strip())
    return s.replace("^", "deg")


def main():
    data = json.load(open(os.path.join(HERE, "data", "ti_params.json"), encoding="utf-8"))
    params, lists = data["params"], data["lists"]
    keys = [p["key"] for p in params]
    index = {k: i for i, k in enumerate(keys)}
    ctl_keys = [c[0] for c in CONTROLS]
    assert not set(ctl_keys) & set(keys)

    # ---- check the pages place every Virus parameter exactly once
    placed = []
    for tab, subs, rows in PAGES:
        assert len(rows) == 4 and len(subs) == 2, tab
        for row in rows:
            assert sum(len(k) for _, k in row) <= 8, (tab, row)
            for _, ks in row:
                placed += ks
    placed += [k for k in LINK_SETUP + LINK_GLOBAL if k in index]
    missing = sorted(set(keys) - set(placed))
    dup = sorted({k for k in placed if placed.count(k) > 1})
    unknown = sorted(set(placed) - set(keys))
    if missing or dup or unknown:
        sys.exit("pages: missing %s, twice %s, unknown %s" % (missing, dup, unknown))

    # ---- ti_table.h
    list_names = sorted(lists)
    list_id = {n: i + 1 for i, n in enumerate(list_names)}   # 0: plain number
    out = ["// Generated by virus/gen.py from virus/data/ti_params.json (derived from gearmulator, GPL-3.0). Do not edit.",
           "#pragma once", "#include <cstdint>", "", "namespace ti {", ""]
    for n in list_names:
        items = ", ".join(cstr(short_text(s)) for s in lists[n])
        out.append("constexpr const char* kL%d[] = {%s};" % (list_id[n], items))
    out += ["", "struct List { const char* const* items; int count; };",
            "constexpr List kLists[%d] = {{nullptr, 0}, %s};" % (len(list_names) + 1, ", ".join(
                "{kL%d, %d}" % (list_id[n], len(lists[n])) for n in list_names)), ""]
    out += ["// One meaning of a parameter byte: its name, short name (under the knob), value texts and range.",
            "struct Meaning { const char* name; const char* shortName; uint8_t list, min, max; };",
            "// Another meaning, in force while parameter `sel` (an index into kParams) compares `op` against `value`.",
            "enum Op : uint8_t { kEq, kNe, kGe, kLe };",
            "struct Alias { Meaning m; int16_t sel; Op op; uint8_t value; };",
            "struct Param { const char* key; uint8_t page, index, def, rawMax; Meaning base; uint8_t numAliases; Alias aliases[3]; };",
            "", "constexpr int kNumParams = %d;" % len(params), "constexpr Param kParams[kNumParams] = {"]

    def meaning(m):
        return "{%s, %s, %d, %d, %d}" % (cstr(m["name"]), cstr(m["short"]), list_id.get(m["fmt"], 0), m["min"], m["max"])
    for p in params:
        als = p.get("aliases", [])
        assert len(als) <= 3
        raw_max = max([p["max"]] + [a["max"] for a in als])
        alias_txt = ", ".join("{%s, %d, %s, %d}" % (meaning(a), index[a["when"][0]], "k" + a["when"][1].capitalize(),
                                                   a["when"][2]) for a in als)
        out.append("    {%s, 0x%02x, %d, %d, %d, %s, %d, {%s}}," % (
            cstr(p["key"]), p["page"], p["index"], min(p["default"], raw_max), raw_max, meaning(p), len(als), alias_txt))
    out += ["};", "", "}  // namespace ti", ""]
    open(os.path.join(HERE, "ti_table.h"), "w", encoding="utf-8").write("\n".join(out))

    # ---- params.json (VST order: controls, then the Virus parameters)
    plist = []
    for key, name, kind, extra in CONTROLS:
        e = {"key": key, "name": name}
        if kind == "readout":
            e.update(min=0, max=1, display="string", type="readout")
        elif kind == "option":
            e.update(options=extra, default=CONTROL_DEFAULTS.get(key, 0))
        elif kind == "stepper":
            e.update(min=0, max=extra - 1, display="int", type="stepper")
        else:
            e.update(min=0, max=1, momentary=True)
        plist.append(e)
    for p in params:
        plist.append({"key": p["key"], "name": p["short"], "min": 0, "max": 1,
                      "default": round(min(p["default"], 127) / max(1, max([p["max"]] + [a["max"] for a in p.get("aliases", [])])), 4)})
    json.dump({"name": "SK TI Link", "params": plist}, open(os.path.join(HERE, "mpc", "params.json"), "w"), indent=1)

    # ---- layout.conf
    short = {p["key"]: p["short"] for p in params}
    L = ["# SK TI Link pages for MPC OS. Generated by virus/gen.py: edit PAGES there, not this file.",
         "# Plugin area 1280 x 628 (layout y 86..714): four rows of up to 8 knobs per tab; rows 1-2 are the tab's first",
         "# Q-Link sub-page (Force knob banks 1 and 2), rows 3-4 its second. MPC draws the names and values live.",
         "theme_bg=1d1f22", "theme_panel=26292d", "theme_line=4a4f55", "theme_ink=e9ecef", "theme_ink_dim=aab1b8",
         "theme_ink_faint=6c737a", "theme_accent=e8412c", "theme_accent_hi=ff6a52", "theme_knob_face=111214",
         "theme_knob_ring=5b6168", "theme_knob_dot=ff5a40", "theme_seg_active=e8412c", "theme_seg_inactive=3a3e43",
         "theme_seg_active_tx=ffffff", "theme_btn_bg=34383d", "theme_btn_text=f2f2f2", "theme_btn_on=e8412c",
         "theme_tab_on=e8412c", "theme_tab_on_tx=ffffff", "theme_lcd=0d1a10", "theme_display_bg=0b140d",
         "theme_display_cell=13261a", "theme_display_ink=7dff9a", "theme_display_off=17301f", "theme_display_bezel=050806",
         "qlinks_track = cutoff,filter1_resonance,filter1_env_amt,filter_env_attack,filter_env_decay,"
         "filter_env_sustain,filter_env_release,patch_volume,osc1_shape,osc2_shape,osc2_detune,osc2_fm_amount,"
         "lfo1_rate,lfo1_symmetry,delay_send,reverb_send", ""]
    ROW_Y = [88, 245, 402, 559]   # frame tops; each row frame is 153 tall (a knob with its texts is 128)
    COL_X = [87 + 158 * i for i in range(8)]
    KNOB_DY = 67   # knob centre below the frame top: under the frame title, its value box inside the frame

    def frame(x, y, w, title):
        return ['frame x=%d y=%d w=%d h=153 title="%s"' % (x, y, w, title)]

    def knob(cx, cy, key, label):
        return 'knob cx=%d cy=%d r=20 label="%s" key=%s' % (cx, cy, label.upper().replace('"', ""), key)

    # LINK
    L += ["[tab LINK]",
          ] + frame(10, 88, 1258, "VIRUS TI OVER USB") + [
          'readout cx=640 cy=144 w=1200 h=44 label="" key=status style=dotmatrix',
          'readout cx=640 cy=200 w=1200 h=44 label="" key=patch style=dotmatrix',
          ] + frame(10, 245, 1258, "PATCH: READ FROM / SEND TO THE VIRUS, LOAD / STORE A PROGRAM") + [
          'button cx=87 cy=326 w=130 h=60 label="READ" key=read',
          'button cx=245 cy=326 w=130 h=60 label="SEND" key=send',
          'stepper cx=490 cy=326 w=300 h=56 label="" key=bank style=dotmatrix',
          'stepper cx=820 cy=326 w=300 h=56 label="" key=program style=dotmatrix',
          'button cx=1035 cy=296 w=130 h=50 label="LOAD" key=load',
          'button cx=1193 cy=296 w=130 h=50 label="STORE" key=store',
          'button cx=1114 cy=358 w=130 h=44 label="PANIC" key=panic',
          ] + frame(10, 402, 1258, "SETUP")
    for i, k in enumerate(LINK_SETUP):
        L.append(knob(COL_X[i], ROW_Y[2] + KNOB_DY, k, dict((c[0], c[1]) for c in CONTROLS).get(k) or short[k]))
    L += frame(10, 559, 1258, "VIRUS GLOBAL SETTINGS (NOT PART OF THE SOUND)")
    for i, k in enumerate(LINK_GLOBAL):
        L.append(knob(COL_X[i], ROW_Y[3] + KNOB_DY, k, short[k]))
    L.append('qlinks "Link" = ' + ",".join(["part", "midi_ch", "bank", "program", "notes", "auto_send", "part_volume",
                                            "part_detune"] + LINK_GLOBAL + ["-", "-"]))
    L.append("")
    for tab, subs, rows in PAGES:
        L.append("[tab %s]" % tab)
        for r, row in enumerate(rows):
            col = 0
            for title, ks in row:
                x0 = 10 + 158 * col
                w = 158 * len(ks) - 6 if col + len(ks) < 8 else 1268 - x0
                L += frame(x0, ROW_Y[r], w, title)
                for k in ks:
                    L.append(knob(COL_X[col], ROW_Y[r] + KNOB_DY, k, short[k]))
                    col += 1
        for s in range(2):
            ks = []
            for row in rows[2 * s:2 * s + 2]:
                flat = [k for _, kk in row for k in kk]
                ks += flat + ["-"] * (8 - len(flat))
            if any(k != "-" for k in ks):
                L.append('qlinks "%s" = %s' % (subs[s], ",".join(ks)))
        L.append("")
    open(os.path.join(HERE, "mpc", "layout.conf"), "w").write("\n".join(L))
    print("gen: %d Virus parameters + %d controls, %d tabs" % (len(params), len(CONTROLS), len(PAGES) + 1))


main()
