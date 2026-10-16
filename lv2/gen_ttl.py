#!/usr/bin/env python3
"""Write lv2/sherkhan.lv2/sherkhan.ttl from mpc/params.json: ports 0-3 audio, then one control port per sound
parameter (the ones before "trig"), normalized 0..1 like the plugin's own parameters."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
params = json.load(open(os.path.join(HERE, "..", "mpc", "params.json")))["params"]
sound = params[:[p["key"] for p in params].index("trig")]
ports = ['[ a lv2:AudioPort , lv2:%sPort ; lv2:index %d ; lv2:symbol "%s" ; lv2:name "%s" ]' % (io, i, sym, name)
         for i, (sym, name, io) in enumerate((("in_l", "In L", "Input"), ("in_r", "In R", "Input"),
                                             ("out_l", "Out L", "Output"), ("out_r", "Out R", "Output")))]
for i, p in enumerate(sound):
    opts = p.get("options")
    default = p.get("default", 0)
    if opts:
        default = default / (len(opts) - 1)
        name = "%s (%s)" % (p["name"], " / ".join(opts))
    else:
        name = p["name"]
    ports.append('[ a lv2:InputPort , lv2:ControlPort ; lv2:index %d ; lv2:symbol "%s" ; lv2:name "%s" ;\n'
                 '        lv2:default %s ; lv2:minimum 0.0 ; lv2:maximum 1.0 ]' % (i + 4, p["key"], name, round(default, 4)))
ttl = """@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .

<https://github.com/davidpinault2-crypto/Sher_Khan-#sherkhan>
    a lv2:Plugin , lv2:FilterPlugin ;
    doap:name "Sher Khan Filterbank" ;
    lv2:optionalFeature lv2:hardRTCapable ;
    lv2:port
        """ + " ,\n        ".join(ports) + " .\n"
open(os.path.join(HERE, "sherkhan.lv2", "sherkhan.ttl"), "w").write(ttl)
print("sherkhan.ttl: %d control ports" % len(sound))
