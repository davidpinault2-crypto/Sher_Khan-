#!/usr/bin/env python3
"""One-off import: turns gearmulator's Virus TI parameter descriptions (GPL-3.0,
https://github.com/dsp56300/gearmulator, source/axel/osTIrusJucePlugin/parameterDescriptions_TI.json) into
virus/data/ti_params.json, the table this plugin is generated from (virus/gen.py).

    python3 virus/tools/import_gearmulator.py <gearmulator checkout> > virus/data/ti_params.json

What it keeps: one entry per byte of a Virus TI Single (sysex pages 0x70 A, 0x71 B, 0x6E, 0x6F) that the TI's editor
shows, plus the TI's global switches and levels (page 0x73) and the part volume/detune (page 0x72). Entries gearmulator
uses for its own editor's state ("Backup", "Select") and the patch name characters are left out (the name is shown on
the LINK page). Where one byte means different things in different modes (an oscillator in HyperSaw or Wavetable
mode, the vocoder, the tape delay, ...), the entry lists those meanings as aliases with the mode that selects them;
the mode rules are ours (ALIAS_RULES below), gearmulator's skin decides this in its UI."""
import json
import re
import sys

PAGES_SINGLE = (112, 113, 110, 111)       # dump order: A, B, then the TI's two extra pages
EXCLUDE = re.compile(r"Backup|/Select$|Slot Select|^deprecated|SingleName|Undefined|^Version$")

# Short names: under the knobs MPC has room for about 10 characters.
SHORT = {
    "Osc1 Mode": "O1 Mode", "Osc1 Shape": "O1 Shape", "Osc1 Pulsewidth": "O1 PW", "Osc1 Wave Select": "O1 Wave",
    "Osc1 Semitone": "O1 Semi", "Osc1 Keyfollow": "O1 KeyFol", "Osc1 Shape Velocity": "O1 ShpVel",
    "Osc1 Wavetable/Sync": "O1 WT Sync", "Osc1 Wavetable/FormantShift": "O1 Formnt",
    "Osc1 Wavetable/Internal Detune": "O1 IntDet", "Osc1 Wavetable/Interpolation": "O1 Interp",
    "Osc1 HyperSaw/Density": "O1 Density", "Osc1 HyperSaw/DetuneSpread": "O1 Spread",
    "Osc1 Wavetable/WaveTableIndex": "O1 Index", "Osc1 Wavetable/WaveTableSelect": "O1 Table",
    "Osc2 Mode": "O2 Mode", "Osc2 Shape": "O2 Shape", "Osc2 Pulsewidth": "O2 PW", "Osc2 Wave Select": "O2 Wave",
    "Osc2 Semitone": "O2 Semi", "Osc2 Detune": "O2 Detune", "Osc2 FM Amount": "FM Amount", "Osc2 Sync": "O2 Sync",
    "Osc2 Filt Env Amt": "FEnv>O2", "FM Filt Env Amt": "FEnv>FM", "Osc2 Keyfollow": "O2 KeyFol",
    "Osc FM Mode": "FM Mode", "Osc2 Shape Velocity": "O2 ShpVel", "Fm Amount Velocity": "FM Vel",
    "Osc2 Wavetable/Sync": "O2 WT Sync", "Osc2 Wavetable/FormantShift": "O2 Formnt",
    "Osc2 Wavetable/Interpolation": "O2 Interp", "Osc2 Wavetable/Internal Detune": "O2 IntDet",
    "Osc2 HyperSaw/Density": "O2 Density", "Osc2 HyperSaw/DetuneSpread": "O2 Spread",
    "Osc2 Wavetable/WaveTableIndex": "O2 Index", "Osc2 Wavetable/WaveTableSelect": "O2 Table",
    "Osc2 HyperSaw/CrossOscSyncFreq": "SyncFreq", "Osc2 Wavetable/FM Amount": "FM Amount",
    "Osc2 HyperSaw/FilterEnv > Pitch": "FEnv>O2", "Osc2 Wavetable/FilterEnv > Pitch": "FEnv>O2",
    "Osc2 Wavetable/FilterEnv > FM": "FEnv>FM", "Osc2 HyperSaw/FilterEnv > SyncFrequency": "FEnv>Sync",
    "Osc2 Wavetable/FM Mode WaveTable": "FM Mode",
    "Osc3 Mode": "O3 Mode", "Osc3 Volume": "O3 Volume", "Osc3 Semitone": "O3 Semi", "Osc3 Detune": "O3 Detune",
    "Osc Balance": "Osc Bal", "Suboscillator Volume": "Sub Vol", "Suboscillator Shape": "Sub Shape",
    "Osc Mainvolume": "Osc Vol", "Ringmodulator Volume": "Ring Mod", "Osc Init Phase": "Init Phase",
    "PulseWidth Velocity": "PW Vel", "Noise Volume": "Noise Vol", "Noise Color": "Noise Col",
    "Cutoff": "Cutoff 1", "Filter1 Resonance": "Reso 1", "Filter1 Env Amt": "F1 EnvAmt", "Filter1 Keyfollow": "F1 KeyFol",
    "Filter1 Mode": "F1 Mode", "Filter1 Env Polarity": "F1 EnvPol", "Flt1 EnvAmt Velocity": "F1 EnvVel",
    "Resonance1 Velocity": "Reso1 Vel", "Cutoff2": "Cutoff 2", "Filter2 Resonance": "Reso 2",
    "Filter2 Env Amt": "F2 EnvAmt", "Filter2 Keyfollow": "F2 KeyFol", "Offset for FilterLink": "Cut2 Offs",
    "Filter2 Mode": "F2 Mode", "Filter2 Env Polarity": "F2 EnvPol", "Filter2 Cutoff Link": "Cut Link",
    "Flt2 EnvAmt Velocity": "F2 EnvVel", "Resonance2 Velocity": "Reso2 Vel", "Filter Balance": "Flt Bal",
    "Saturation Curve": "Saturation", "Filter Routing": "Routing", "Filter Keytrack Base": "KeyBase",
    "Filter Env Attack": "FE Attack", "Filter Env Decay": "FE Decay", "Filter Env Sustain": "FE Sust",
    "Filter Env Sustain Time": "FE SusTime", "Filter Env Release": "FE Release",
    "Amp Env Attack": "AE Attack", "Amp Env Decay": "AE Decay", "Amp Env Sustain": "AE Sust",
    "Amp Env Sustain Time": "AE SusTime", "Amp Env Release": "AE Release",
    "Envelope 3/Attack": "E3 Attack", "Envelope 3/Decay": "E3 Decay", "Envelope 3/Sustain": "E3 Sust",
    "Envelope 3/Sustain Time": "E3 SusTime", "Envelope 3/Release": "E3 Release",
    "Envelope 4/Attack": "E4 Attack", "Envelope 4/Decay": "E4 Decay", "Envelope 4/Sustain": "E4 Sust",
    "Envelope 4/Sustain Time": "E4 SusTime", "Envelope 4/Release": "E4 Release",
    "Lfo1 Rate": "L1 Rate", "Lfo1 Shape": "L1 Shape", "Lfo1 Env Mode": "L1 EnvMod", "Lfo1 Mode": "L1 Mode",
    "Lfo1 Symmetry": "L1 Symm", "Lfo1 Keyfollow": "L1 KeyFol", "Lfo1 Keytrigger": "L1 KeyTrg",
    "Osc1 Lfo1 Amount": "L1>Osc1", "Osc2 Lfo1 Amount": "L1>Osc2", "PW Lfo1 Amount": "L1>PW",
    "Reso Lfo1 Amount": "L1>Reso", "FiltGain Lfo1 Amount": "L1>FltGain", "Lfo1 Clock": "L1 Clock",
    "LFO1 Assign Dest": "L1 Dest", "LFO1 Assign Amount": "L1 DstAmt",
    "Lfo2 Rate": "L2 Rate", "Lfo2 Shape": "L2 Shape", "Lfo2 Env Mode": "L2 EnvMod", "Lfo2 Mode": "L2 Mode",
    "Lfo2 Symmetry": "L2 Symm", "Lfo2 Keyfollow": "L2 KeyFol", "Lfo2 Keytrigger": "L2 KeyTrg",
    "Shape Lfo2 Amount": "L2>Shape", "FM Lfo2 Amount": "L2>FM", "Cutoff1 Lfo2 Amount": "L2>Cut1",
    "Cutoff2 Lfo2 Amount": "L2>Cut2", "Pan Lfo2 Amount": "L2>Pan", "Lfo2 Clock": "L2 Clock",
    "LFO2 Assign Dest": "L2 Dest", "LFO2 Assign Amount": "L2 DstAmt",
    "Lfo3 Rate": "L3 Rate", "Lfo3 Shape": "L3 Shape", "Lfo3 Mode": "L3 Mode", "Lfo3 Keyfollow": "L3 KeyFol",
    "Lfo3 Destination": "L3 Dest", "Osc Lfo3 Amount": "L3 Amount", "Lfo3 Fade-In Time": "L3 FadeIn",
    "Lfo3 Clock": "L3 Clock",
    "Patch Volume": "Patch Vol", "Amp Velocity": "Amp Vel", "Panorama Velocity": "Pan Vel",
    "Second Output Balance": "Out2 Bal", "Transpose": "Transpose", "Key Mode": "Key Mode",
    "Control Smooth Mode": "Smooth", "Bender Range Up": "Bend Up", "Bender Range Down": "Bend Down",
    "Bender Scale": "Bend Scale", "Unison Mode": "Unison", "Unison Detune": "Uni Detune",
    "Unison Pan Spread": "Uni Spread", "Unison Lfo Phase": "Uni LFOPh", "Portamento Time": "Portamnto",
    "Channel Volume": "Ch Volume", "Balance": "Balance", "Panorama": "Panorama", "Clock Tempo": "Tempo",
    "Punch Intensity": "Punch",
    "Chorus/Type": "Chor Type", "Chorus/Mix2": "Chor Mix2", "Chorus Mix": "Chor Mix", "Chorus Rate": "Chor Rate",
    "Chorus Depth": "Chor Depth", "Chorus Delay": "Chor Delay", "Chorus Feedback": "Chor FBack",
    "Chorus Lfo Shape": "Chor LFO", "Chorus/X Over": "Chor XOver", "Chorus/Speed": "Rot Speed",
    "Chorus/Distance": "Rot Dist", "Chorus/Amount": "Chor Amt", "Chorus/Mic Angle": "Mic Angle",
    "Chorus/LowHigh Bal": "Lo/Hi Bal",
    "Delay Mode": "Dly Mode", "Delay Send": "Dly Send", "Delay Time": "Dly Time", "Delay Feedback": "Dly FBack",
    "Dly Rate / Rev Decay": "Dly Rate", "Dly Depth ": "Dly Depth", "Delay Lfo Shape": "Dly LFO",
    "Delay Color": "Dly Color", "Delay Clock": "Dly Clock", "Delay Type": "Dly Type",
    "Delay Tape Delay Ratio": "Tape Ratio", "Delay Tape Delay Left Clock": "Tape ClkL",
    "Delay Tape Delay Right Clock": "Tape ClkR", "Delay Tape Delay Bandwidth": "Tape BW",
    "Delay/Tape Delay Time": "Tape Time", "Delay/Tape Delay Feedback": "Tape FBack",
    "Delay/Tape Delay Modulation": "Tape Mod", "Delay/Tape Delay Center Frequency": "Tape Freq",
    "Reverb Mode": "Rev Mode", "Reverb Send": "Rev Send", "Reverb Type": "Rev Type", "Reverb Time": "Rev Time",
    "Reverb Damping": "Rev Damp", "Reverb Color": "Rev Color", "Reverb Predelay": "Rev PreDly",
    "Reverb Clock": "Rev Clock", "Reverb Feedback": "Rev FBack",
    "Arp Mode": "Arp Mode", "Arp Pattern Selct": "Arp Pattrn", "Arp Octave Range": "Arp Oct",
    "Arp Hold Enable": "Arp Hold", "Arp Note Length": "Arp Length", "Arp Swing": "Arp Swing",
    "Arp Clock": "Arp Clock", "Arpeggiator/UserPatternLength": "Steps",
    "Input Mode": "In Mode", "Input Select": "In Select", "Input Ringmodulator": "In RingMod",
    "Phaser Mode": "Phs Stages", "Phaser Mix": "Phs Mix", "Phaser Rate": "Phs Rate", "Phaser Depth": "Phs Depth",
    "Phaser Frequency": "Phs Freq", "Phaser Feedback": "Phs FBack", "Phaser Spread": "Phs Spread",
    "LowEQ Gain": "Low Gain", "LowEQ Frequency": "Low Freq", "MidEQ Gain": "Mid Gain", "MidEQ Frequency": "Mid Freq",
    "MidEQ Q-Factor": "Mid Q", "HighEQ Gain": "High Gain", "HighEQ Frequency": "High Freq",
    "Bass Intensity": "Boost Int", "Bass Tune": "Boost Tune", "Distortion Curve": "Dist Type",
    "Distortion Intensity": "Dist Int", "Patch Distortion/Treble Booster": "Dist Treb",
    "Patch Distortion/High Cut": "Dist HiCut", "Patch Distortion/Mix": "Dist Mix",
    "Patch Distortion/Quality": "Dist Qual", "Patch Distortion/Tone127": "Dist Tone",
    "Vocoder Mode": "Voc Mode", "Filter Select": "Flt Select", "Vocoder/Carrier Center Frequency": "Car Freq",
    "Vocoder/Modulator Center Frequency": "Mod Freq", "Vocoder/Modulator Frequency Offset": "Mod Offset",
    "Vocoder/Carrier Q Factor": "Car Q", "Vocoder/Modulator Q Factor": "Mod Q",
    "Carrier Frequency Spread": "Car Spread", "Vocoder/Modulator Frequency Spread": "Mod Spread",
    "Vocoder/Balance": "Voc Bal", "Vocoder/Attack": "Voc Attack", "Vocoder/Release": "Voc Rel",
    "Vocoder/Spectral Balance": "Spec Bal", "Vocoder/Bands": "Voc Bands",
    "Input Follower Mode": "Follower", "Input Follower/Attack": "Fol Attack", "Input Follower/Level": "Fol Level",
    "Input Follower/Release": "Fol Rel",
    "Filter Bank/Type": "FB Type", "Filter Bank/Mix": "FB Mix", "Filter Bank/Frequency": "FB Freq",
    "Filter Bank/Comb Frequency": "Comb Note", "Filter Bank/Filter Frequency": "FB Freq",
    "Filter Bank/Vowel Frequency": "Vowel Frq", "Filter Bank/Stereo Phase": "FB Phase",
    "Filter Bank/Filter Type": "FB FltType", "Filter Bank/Poles": "FB Poles", "Filter Bank/Shape L": "Vowel L",
    "Filter Bank/Shape R": "Vowel R", "Filter Bank/Slope": "FB Slope", "Filter Bank/Resonance": "FB Reso",
    "Character Type": "Character", "Atomizer": "Atomizer",
    "Soft Knob1 ShortName": "SK1 Name", "Soft Knob2 ShortName": "SK2 Name", "Soft Knob3 ShortName": "SK3 Name",
    "Soft Knob-1 Single": "SK1 Dest", "Soft Knob-2 Single": "SK2 Dest", "Soft Knobs/Destination 3": "SK3 Dest",
    "Category1": "Category 1", "Category2": "Category 2",
    "Part Detune": "Part Det", "Part Volume": "Part Vol",
    "Switches/EQ Enable": "EQ On", "Switches/Arpeggiator Enable": "Arp On", "Switches/Delay Enable": "Delay On",
    "Switches/Reverb Enable": "Reverb On", "Master Tune": "Mstr Tune", "Master Volume": "Mstr Vol",
    "Midi Channel": "Glob Ch", "Soft Knob Configuration/Value Softknob0": "SK1 Value",
    "Soft Knob Configuration/Value Softknob1": "SK2 Value", "Soft Knob Configuration/Value Softknob2": "SK3 Value",
    "Soft Knob-1 Mode": "SK1 Mode", "Soft Knob-2 Mode": "SK2 Mode", "Soft Knob-3 Mode": "SK3 Mode",
    "Soft Knob-1 Dest": "SK1 GDest", "Soft Knob-2 Dest": "SK2 GDest", "Soft Knob-3 Dest": "SK3 GDest",
}
GLOBALS_KEEP = {"Switches/EQ Enable", "Switches/Arpeggiator Enable", "Switches/Delay Enable",
                "Switches/Reverb Enable", "Master Tune", "Master Volume"}

# Mode rules for bytes with several meanings: alias name -> (selector name, op, value). First match wins; the base
# name (no rule) is the fallback. Selector names are gearmulator's.
O1, O2 = "Osc1 Mode", "Osc2 Mode"
ALIAS_RULES = {
    "Osc1 HyperSaw/": (O1, "eq", 1), "Osc1 Wavetable/": (O1, "ge", 2),
    "Osc2 HyperSaw/": (O2, "eq", 1), "Osc2 Wavetable/": (O2, "ge", 2),
    "Vocoder/": ("Vocoder Mode", "ne", 0), "Carrier Frequency Spread": ("Vocoder Mode", "ne", 0),
    "Input Follower/": ("Input Follower Mode", "ne", 0),
    "Offset for FilterLink": ("Filter2 Cutoff Link", "ne", 0),
    "Chorus/Speed": ("Chorus/Type", "eq", 6), "Chorus/Distance": ("Chorus/Type", "eq", 6),
    "Chorus/Mic Angle": ("Chorus/Type", "eq", 6), "Chorus/LowHigh Bal": ("Chorus/Type", "eq", 6),
    "Chorus/Amount": ("Chorus/Type", "ge", 2),
    "Delay/Tape": ("Delay Type", "ne", 0),
    "Filter Bank/Frequency": ("Filter Bank/Type", "le", 2), "Filter Bank/Vowel Frequency": ("Filter Bank/Type", "eq", 3),
    "Filter Bank/Comb Frequency": ("Filter Bank/Type", "eq", 4),
    "Filter Bank/Shape L": ("Filter Bank/Type", "eq", 3), "Filter Bank/Poles": ("Filter Bank/Type", "ge", 9),
    "Filter Bank/Shape R": ("Filter Bank/Type", "eq", 3),
}
# The base (no-rule) meaning of each shared byte, where gearmulator lists another first.
BASE = {"Osc1 Shape", "Osc1 Pulsewidth", "Osc1 Wave Select", "Osc2 Shape", "Osc2 Pulsewidth", "Osc2 Wave Select",
        "Osc2 FM Amount", "Osc2 Filt Env Amt", "FM Filt Env Amt", "Cutoff", "Cutoff2", "Filter1 Resonance",
        "Filter2 Resonance", "Filter1 Keyfollow", "Filter2 Keyfollow", "Filter Balance", "Filter Env Attack",
        "Filter Env Decay", "Filter Env Sustain", "Filter Env Sustain Time", "Filter Env Release", "Chorus/Type",
        "Chorus Rate", "Chorus Depth", "Chorus Delay", "Chorus Feedback", "Delay Time", "Delay Feedback", "Dly Depth ",
        "Delay Color", "Filter2 Cutoff Link", "Osc FM Mode", "Soft Knob1 ShortName", "Soft Knob2 ShortName",
        "Soft Knob3 ShortName", "Filter Bank/Filter Frequency", "Filter Bank/Filter Type", "Filter Bank/Slope",
        "Patch Distortion/Tone127"}
DROP = {"Cutoff2 Offset", "Soft Knob1 ShortName Main", "Soft Knob2 ShortName Main", "Soft Knob3 ShortName Main",
        "Patch Distortion/Tone64"}


def load(path):
    t = open(path, encoding="utf-8").read().replace("'\"'", '"\\""')
    out, i, ins = [], 0, False
    while i < len(t):   # strip // comments outside strings
        c = t[i]
        if ins:
            out.append(c)
            if c == "\\":
                out.append(t[i + 1]); i += 2; continue
            ins = c != '"'
        elif c == '"':
            ins = True; out.append(c)
        elif t.startswith("//", i):
            j = t.find("\n", i); i = j if j >= 0 else len(t); continue
        else:
            out.append(c)
        i += 1
    return json.loads(re.sub(r",\s*([\]}])", r"\1", "".join(out)))


def slug(name):
    s = re.sub(r"[^a-z0-9]+", "_", name.lower().replace(">", " to ").replace("+", " plus ")).strip("_")
    return s


def short_name(name):
    if name in SHORT:
        return SHORT[name]
    m = re.match(r"Assign ?(\d) ?(Source|Destination|Amount) ?(\d?)$", name)
    if m:
        return "A%s %s%s" % (m.group(1), {"Source": "Source", "Destination": "Dest", "Amount": "Amt"}[m.group(2)],
                             m.group(3) or ("" if m.group(2) == "Source" else "1"))
    m = re.match(r"Step (\d+) (Length|Velocity|Bitfield)$", name)
    if m:
        return "S%s %s" % (m.group(1), {"Length": "Len", "Velocity": "Vel", "Bitfield": "Bits"}[m.group(2)])
    return name.strip()[:10]


def rule_for(name):
    for prefix, rule in ALIAS_RULES.items():
        if name.startswith(prefix):
            return rule
    return None


def main():
    j = load(sys.argv[1] + "/source/axel/osTIrusJucePlugin/parameterDescriptions_TI.json")
    descs = [p for p in j["parameterdescriptions"] if p.get("isPublic", True) and p["name"] not in DROP]
    groups = {}
    for p in descs:
        if EXCLUDE.search(p["name"]):
            continue
        if p["page"] == 115 and p["name"] not in GLOBALS_KEEP:
            continue
        groups.setdefault((p["page"], p["index"]), []).append(p)
    used_lists, entries = set(), []
    by_name = {p["name"]: p for p in descs}
    for (page, index), ps in sorted(groups.items(), key=lambda kv: (
            {112: 0, 113: 1, 110: 2, 111: 3, 114: 4, 115: 5}[kv[0][0]], kv[0][1])):
        base = [p for p in ps if p["name"] in BASE] or [p for p in ps if not rule_for(p["name"])] or ps[:1]
        base = base[0]

        def meaning(p):
            fmt = p.get("toText", "unsignedZero")
            used_lists.add(fmt)
            return {"name": p["name"].strip(), "short": short_name(p["name"]),
                    "fmt": fmt, "min": p.get("min", 0), "max": p.get("max", 127),
                    "bipolar": bool(p.get("isBipolar", False))}
        e = {"key": slug(base["name"]), "page": page, "index": index, "default": base.get("default", 0)}
        e.update(meaning(base))
        aliases = []
        for p in ps:
            if p is base:
                continue
            sel, op, v = rule_for(p["name"]) or (None, None, None)
            if sel is None:
                sys.stderr.write("note: no rule for %r (shares %d.%d with %r): dropped\n" % (p["name"], page, index, base["name"]))
                continue
            a = meaning(p)
            a["when"] = [slug(by_name[sel]["name"]), op, v]
            aliases.append(a)
        if aliases:   # exact modes before ranges, so a ge rule doesn't hide an eq one
            e["aliases"] = sorted(aliases, key=lambda a: a["when"][1] != "eq")
        entries.append(e)
    keys = [e["key"] for e in entries]
    assert len(keys) == len(set(keys)), "duplicate keys"
    for e in entries:
        for a in e.get("aliases", []):
            assert a["when"][0] in keys, a
    lists = {}
    for k in sorted(used_lists):
        v = j["valuelists"].get(k)
        if v is None:
            continue
        if isinstance(v, dict):
            v = {int(a): b for a, b in v.items()}
            v = [v.get(i, str(i)) for i in range(max(v) + 1)]
        lists[k] = [s.strip() for s in v]
    json.dump({"source": "gearmulator parameterDescriptions_TI.json (GPL-3.0), via virus/tools/import_gearmulator.py",
               "params": entries, "lists": lists}, sys.stdout, indent=1, ensure_ascii=False)
    sys.stdout.write("\n")


main()
