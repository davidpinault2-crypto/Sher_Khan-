#!/usr/bin/env python3
"""Check a built plugin folder against the rules of Akai's MPC Plugin Builder SDK: docs/plugin-folder-format.md, the
plugin.json schema and the checks of its tools/validate.py, re-implemented here (nothing of the SDK is run). The
binaries themselves (exports, libraries, glibc) are checked by mpc/build.sh with readelf.

    check_folder.py <plugin folder> <number of VST parameters> [--legacy]

--legacy: the folder MPC.settings registers on MPC OS 3.10.0 and older (one armv7 .so at its root, no plugin.json
needed); the skin and version.xml rules are the same."""
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

ILLEGAL_IN_FOLDER = '"#@,;:<>*^|?\\/'   # what MPC strips when it builds the folder name from the plugin's strings
NOT_EXFAT = set('<>:"\\|?*')             # file names /nvme, SD cards and USB sticks refuse
REQUIRED = ("name", "manufacturer", "version", "format", "kind", "uid", "numInputs", "numOutputs", "file32", "file64")
PARAM_HANDLE = re.compile(r"Parameter (\d+)")
UIVERSION = "0.1.0.0"


def walk(value, path=""):
    if isinstance(value, dict):
        for k, v in value.items():
            yield from walk(v, path + "/" + str(k))
    elif isinstance(value, list):
        for i, v in enumerate(value):
            yield from walk(v, "%s[%d]" % (path, i))
    else:
        yield path, value


def pad_version(text):
    parts = text.split(".")[:4]
    return ".".join(parts + ["0"] * (4 - len(parts)))


def elf_kind(path):
    """(class bits, machine, type) of an ELF file, or None."""
    with open(path, "rb") as f:
        head = f.read(20)
    if len(head) < 20 or head[:4] != b"\x7fELF":
        return None
    bits = 32 if head[4] == 1 else 64
    little = head[5] == 1
    e_type = int.from_bytes(head[16:18], "little" if little else "big")
    e_machine = int.from_bytes(head[18:20], "little" if little else "big")
    return bits, e_machine, e_type


def check_plugin_json(folder, errors):
    path = os.path.join(folder, "plugin.json")
    try:
        pj = json.load(open(path, encoding="utf-8"))
    except (OSError, ValueError) as e:
        errors.append("plugin.json: %s" % e)
        return None
    missing = [k for k in REQUIRED if k not in pj]
    if missing:
        errors.append("plugin.json is missing %s" % ", ".join(missing))
        return pj
    for k in ("name", "manufacturer"):
        if not isinstance(pj[k], str) or not pj[k]:
            errors.append('plugin.json "%s" must be a non-empty string' % k)
    if not re.fullmatch(r"[0-9]+(\.[0-9]+){0,3}", str(pj["version"])):
        errors.append('plugin.json "version" %r is not like 1.2.3' % pj["version"])
    if pj["format"] != "vst2":
        errors.append('plugin.json "format" must be "vst2"')
    if pj["kind"] not in ("instrument", "effect"):
        errors.append('plugin.json "kind" must be "instrument" or "effect"')
    uid = pj["uid"]
    if not ((isinstance(uid, str) and (re.fullmatch(r"[ -~]{4}", uid) or re.fullmatch(r"0[xX][0-9a-fA-F]{1,8}", uid)))
            or (isinstance(uid, int) and 0 <= uid <= 0xFFFFFFFF)):
        errors.append('plugin.json "uid" %r is not a four-character code, 0x hex or an integer' % uid)
    if not isinstance(pj["numInputs"], int) or pj["numInputs"] < 0:
        errors.append('plugin.json "numInputs" must be an integer >= 0')
    if not isinstance(pj["numOutputs"], int) or pj["numOutputs"] < 1:
        errors.append('plugin.json "numOutputs" must be an integer >= 1 (MPC lists no plugin without outputs)')
    want = {"file32": (32, 40), "file64": (64, 183)}   # EM_ARM, EM_AARCH64
    for key, (bits, machine) in want.items():
        rel = pj[key]
        if not isinstance(rel, str) or not rel.endswith(".so") or rel.startswith("/") or ".." in rel:
            errors.append('plugin.json "%s" must be a relative path to a .so inside the folder, got %r' % (key, rel))
            continue
        so = os.path.join(folder, rel)
        if not os.path.isfile(so):
            errors.append('plugin.json "%s": %s is not in the folder' % (key, rel))
            continue
        kind = elf_kind(so)
        if kind != (bits, machine, 3):
            errors.append("%s: expected a %d-bit shared object for machine %d, got %s" % (rel, bits, machine, kind))
    expected = "%s - VST - %s" % tuple("".join(c for c in s if c not in ILLEGAL_IN_FOLDER)
                                       for s in (pj["manufacturer"], pj["name"]))
    if os.path.basename(os.path.normpath(folder)) != expected:
        errors.append('the folder must be named "%s" (MPC finds the skin by that name)' % expected)
    if len(expected) >= 128:
        errors.append("the folder name is too long (%d characters, under 128 please)" % len(expected))
    return pj


def check_version_xml(folder, version, errors):
    path = os.path.join(folder, "version.xml")
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as e:
        errors.append("version.xml: %s" % e)
        return
    ui = root.find("uiversion")
    if ui is None or (ui.text or "").strip() != UIVERSION:
        errors.append("version.xml needs <uiversion>%s</uiversion> (the MPC's skin format; not 1)" % UIVERSION)
    ver = root.find("version")
    if version and (ver is None or (ver.text or "").strip() != pad_version(version)):
        errors.append("version.xml <version> should be %s" % pad_version(version))


def check_skins(folder, nparams, errors):
    skins = os.path.join(folder, "Plugin Skins")
    try:
        tui = json.load(open(os.path.join(skins, "TUI.json"), encoding="utf-8"))
    except (OSError, ValueError) as e:
        errors.append("Plugin Skins/TUI.json: %s" % e)
        return
    page = tui.get("pageData") if isinstance(tui, dict) else None
    if not isinstance(page, dict) or page.get("version") != 1:
        errors.append("TUI.json: needs {\"pageData\": {\"version\": 1, ...}}")
        return
    tabs = page.get("tabs")
    if not isinstance(tabs, list) or not tabs or len(tabs) > 32:
        errors.append("TUI.json: pageData.tabs must hold 1 to 32 tabs")
        tabs = []
    keys = [(t.get("fnKeyIndex"), t.get("fnKeySubIndex")) for t in tabs if isinstance(t, dict)]
    if len(set(keys)) != len(keys):
        errors.append("TUI.json: two tabs share a function key")
    images = set()
    for path, v in walk(tui):
        if not isinstance(v, str):
            continue
        for m in PARAM_HANDLE.finditer(v):
            if int(m.group(1)) >= nparams:
                errors.append('TUI.json: "%s" at %s: the plugin has parameters 0..%d' % (v, path, nparams - 1))
        if path.endswith(("/filmStrip", "/image", "/onImage", "/offImage")) and v:
            images.add(v)
    for img in sorted(images):
        if not os.path.isfile(os.path.join(skins, img)):
            errors.append("TUI.json uses %s, which is not in Plugin Skins/" % img)
    qlinks = [f for f in os.listdir(skins) if f == "Q-Links.json" or (f.startswith("Q-Links - ") and f.endswith(".json"))]
    if "Q-Links.json" not in qlinks:
        errors.append("Plugin Skins/Q-Links.json is missing")
    for name in sorted(qlinks):
        try:
            q = json.load(open(os.path.join(skins, name), encoding="utf-8"))
        except (OSError, ValueError) as e:
            errors.append("%s: %s" % (name, e))
            continue
        if q.get("version") != 4:
            errors.append("%s: version must be 4" % name)
        screen = q.get("Screen Mode Q-Links", {})
        if not isinstance(screen.get("map"), list):
            errors.append('%s: "Screen Mode Q-Links".map must be a list' % name)
        for path, v in walk(q):
            if "/Q-Links/Q-Link " in path or path.startswith("/Program Mode Q-Links/"):
                if not isinstance(v, int) or not (v == -1 or 0 <= v < nparams):
                    errors.append("%s: %s = %r is not -1 or a parameter index" % (name, path, v))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    legacy = "--legacy" in sys.argv
    if len(args) != 2:
        print(__doc__)
        return 2
    folder, nparams = args[0], int(args[1])
    errors = []
    version = None
    if not legacy:
        pj = check_plugin_json(folder, errors)
        version = str(pj.get("version")) if pj else None
    check_version_xml(folder, version, errors)
    check_skins(folder, nparams, errors)
    for dirpath, dirnames, filenames in os.walk(folder):
        for f in dirnames + filenames:
            if NOT_EXFAT & set(f):
                errors.append("%s: not a valid file name on exFAT drives" % os.path.join(dirpath, f))
    for e in errors:
        print("error:", e)
    if not errors:
        print("%s: OK (%s)" % (folder, "legacy MPC.settings folder" if legacy else "Akai SDK plugin folder"))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
