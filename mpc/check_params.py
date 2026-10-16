#!/usr/bin/env python3
"""Check that mpc/params.json (what the skin is built from) matches the plugin itself: the key/name table in
mpc/sk_vst.cpp, the ParamId order and defaults in dsp/ShermanCore.h. The VST parameter index is what the skin,
the Q-Links and saved projects bind to, so a mismatch would silently wire knobs to the wrong parameters."""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def main():
    params = json.load(open(os.path.join(HERE, "params.json")))["params"]
    vst = open(os.path.join(HERE, "sk_vst.cpp")).read()
    core = open(os.path.join(ROOT, "dsp", "ShermanCore.h")).read()

    table = vst[vst.index("kParams[sk::kNumParams] = {"):]
    table = table[:table.index("};")]
    entries = re.findall(r'\{"([a-z_]+)",\s*"([^"]+)"\}', table)

    ids = core[core.index("enum ParamId"):]
    ids = re.findall(r"\bk([A-Z][A-Za-z]+)\b", ids[:ids.index("};")])
    ids = [i for i in ids if i != "NumParams"]

    defaults = core[core.index("constexpr float kDefaults"):]
    defaults = defaults[defaults.index("{") + 1:defaults.index("};")]
    defaults = [eval(d.strip().rstrip("f").replace("f /", " /")) for d in defaults.split(",") if d.strip()]

    errors = []
    if not (len(params) == len(entries) == len(ids) == len(defaults)):
        errors.append("counts differ: params.json %d, sk_vst.cpp %d, ParamId %d, kDefaults %d"
                      % (len(params), len(entries), len(ids), len(defaults)))
    for i, (p, (key, name)) in enumerate(zip(params, entries)):
        if p["key"] != key:
            errors.append("index %d: params.json key %r, sk_vst.cpp key %r" % (i, p["key"], key))
        if p.get("name") != name:
            errors.append("index %d: params.json name %r, sk_vst.cpp name %r" % (i, p.get("name"), name))
        want = p.get("default", 0)
        if "options" in p:
            want = want / (len(p["options"]) - 1)
        if i < len(defaults) and abs(want - defaults[i]) > 1e-3:
            errors.append("index %d (%s): params.json default %s, kDefaults %s" % (i, key, want, defaults[i]))
    # ParamId names spell the key out in CamelCase (kFreqA <-> freq_a, kEnvAmount <-> env_amt).
    short = {"amount": "amt", "attack": "atk", "release": "rel"}
    for i, (key, ident) in enumerate(zip([k for k, _ in entries], ids)):
        words = [w.lower() for w in re.findall(r"[A-Z][a-z]*", ident)]
        if words[0] in ("env", "lfo"):
            words = [words[0]] + [short.get(w, w) for w in words[1:]]
        if "_".join(words) != key:
            errors.append("index %d: ParamId k%s vs key %r" % (i, ident, key))
    for e in errors:
        print("error:", e)
    if not errors:
        print("params OK: %d parameters, same order, names and defaults in params.json, sk_vst.cpp and ShermanCore.h"
              % len(params))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
