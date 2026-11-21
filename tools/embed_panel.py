"""Turns plugins/<name>.html into plugins/<name>.inc, so a panel ships inside its dll (default: mt_net_panel).

build.bat runs this before compiling mt_net.cpp and cb_radio.cpp. A byte list because MSVC caps string literals.
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGINS = os.path.join(HERE, "..", "plugins")

NAME = sys.argv[1] if len(sys.argv) > 1 else "mt_net_panel"
html = open(os.path.join(PLUGINS, NAME + ".html"), "rb").read()
with open(os.path.join(PLUGINS, NAME + ".inc"), "w") as out:
    for i in range(0, len(html), 32):
        out.write(",".join(str(b) for b in html[i:i + 32]) + ",\n")
print(NAME, "embedded:", len(html), "bytes")
