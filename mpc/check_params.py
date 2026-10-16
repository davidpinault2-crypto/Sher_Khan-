#!/usr/bin/env python3
"""Check that mpc/params.json (what the skin is built from) matches the plugin itself: the parameter table in
mpc/sk_vst.cpp, and the ParamId order, switch position counts and defaults in dsp/ShermanCore.h. The VST
parameter index is what the skin, the Q-Links and saved projects bind to, so a mismatch would silently wire
controls to the wrong parameters."""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def braced(text, start_marker):
    """The {...} list after start_marker, without the outer braces."""
    s = text[text.index(start_marker):]
    s = s[s.index("{") + 1:]
    depth, out = 1, []
    for ch in s:
        depth += ch == "{"
        depth -= ch == "}"
        if depth == 0:
            break
        out.append(ch)
    return "".join(out)


def floats(body):
    vals = []
    for tok in body.split(","):
        tok = tok.strip()
        if tok:
            vals.append(eval(re.sub(r"(?<=[\d.])f\b", "", tok)))
    return vals


def main():
    params = json.load(open(os.path.join(HERE, "params.json")))["params"]
    vst = open(os.path.join(HERE, "sk_vst.cpp")).read()
    core = open(os.path.join(ROOT, "dsp", "ShermanCore.h")).read()

    table = braced(vst, "kParams[kNumVst] =")
    entries = re.findall(r'\{"([a-z0-9_]+)",\s*"([^"]+)",\s*k(\w+)\}', table)
    ids = re.findall(r"\bk([A-Z][A-Za-z0-9]+)\b", braced(core, "enum ParamId"))
    ids = [i for i in ids if i != "NumParams"]
    defaults = floats(braced(core, "constexpr float kDefaults"))
    options = [int(v) for v in floats(braced(core, "constexpr int kOptionCount"))]

    errors = []
    if len(params) != len(entries):
        errors.append("params.json has %d parameters, sk_vst.cpp %d" % (len(params), len(entries)))
    if not (len(ids) == len(defaults) == len(options)):
        errors.append("ShermanCore.h: %d ParamIds, %d defaults, %d option counts" % (len(ids), len(defaults), len(options)))
    sound = [e for e in entries if e[2] == "Sound"]
    if len(sound) != len(ids) or entries[:len(sound)] != sound:
        errors.append("sk_vst.cpp must list the %d sound parameters first, in ParamId order" % len(ids))
    for i, (p, (key, name, kind)) in enumerate(zip(params, entries)):
        if p["key"] != key:
            errors.append("index %d: params.json key %r, sk_vst.cpp key %r" % (i, p["key"], key))
        if p.get("name") != name:
            errors.append("index %d: params.json name %r, sk_vst.cpp name %r" % (i, p.get("name"), name))
        if kind == "Momentary" and not p.get("momentary"):
            errors.append("index %d (%s): a button in sk_vst.cpp, not momentary in params.json" % (i, key))
        if i >= len(ids):
            continue
        # ParamId names spell the key out in CamelCase (kFreq1 <-> freq_1, kLfoF2 <-> lfo_f2).
        if ids[i].lower() != key.replace("_", ""):
            errors.append("index %d: ParamId k%s vs key %r" % (i, ids[i], key))
        count = len(p.get("options") or [])
        if count != options[i]:
            errors.append("index %d (%s): params.json has %d options, kOptionCount %d" % (i, key, count, options[i]))
        want = p.get("default", 0)
        if count:
            want = want / (count - 1)
        if abs(want - defaults[i]) > 1e-3:
            errors.append("index %d (%s): params.json default %s, kDefaults %s" % (i, key, want, defaults[i]))
    for e in errors:
        print("error:", e)
    if not errors:
        print("params OK: %d parameters (%d sound) agree across params.json, sk_vst.cpp and ShermanCore.h"
              % (len(params), len(ids)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
