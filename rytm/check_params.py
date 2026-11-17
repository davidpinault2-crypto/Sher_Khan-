#!/usr/bin/env python3
"""Check that rytm/{track,send}/params.json (what the skins are built from) match the parameter tables in
rytm/rytm_vst.cpp: same keys in the same order (the VST index the skin, the Q-Links and saved projects bind to),
same option counts and option names."""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def table(src, variant):
    """The kParams entries of one variant (#ifndef SK_RYTM_SEND = track, #else = send)."""
    blocks = re.findall(r"constexpr ParamInfo kParams\[kNumVst\] = \{(.*?)\n\};", src, re.S)
    body = blocks[0 if variant == "track" else 1]
    return re.findall(r'\{"(\w+)", "([^"]+)", (k\w+), ([\w]+), [^}]*\}', body)


def names(src, array):
    body = re.search(r"constexpr \w+ %s\[\] = \{(.*?)\n\};" % array, src, re.S).group(1)
    return re.findall(r'\{"([^"]+)",', body)


def main():
    src = open(os.path.join(HERE, "rytm_vst.cpp")).read()
    opts = {"source": names(src, "kSources"), "dest": names(src, "kDests"),
            "midi_ch": ["Off"] + ["Ch %d" % i for i in range(1, 17)], "phase": ["Normal", "Invert"], "thru": ["Thru", "Mute"]}
    bad = 0
    for variant in ("track", "send"):
        params = json.load(open(os.path.join(HERE, variant, "params.json")))["params"]
        rows = table(src, variant)
        if [p["key"] for p in params] != [r[0] for r in rows]:
            print("%s: keys differ: %s vs %s" % (variant, [p["key"] for p in params], [r[0] for r in rows]))
            bad += 1
        for p, (key, name, kind, count) in zip(params, rows):
            if p["name"] != name:
                print("%s: %s is named %r in params.json, %r in the plugin" % (variant, key, p["name"], name))
                bad += 1
            if kind == "kChoice":
                want = opts.get(key)
                if p.get("options") != want:
                    print("%s: %s options differ: %s vs %s" % (variant, key, p.get("options"), want))
                    bad += 1
    print("params.json matches rytm_vst.cpp" if not bad else "%d mismatches" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
