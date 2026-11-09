"""The port's home screen: iOS's default layout without widgets, an empty Today page, and dark mode.

Writes two files that HackiOS's init copies into /var at every boot (userland/var/), made from iOS's own
DefaultIconState~iphone.plist in the rootfs that HackiOS extracts on your machine (nothing of Apple's is
in this repository):
    HACKIOS=~/hackios python3 -I home-screen.py
"""
import os
import plistlib

H = os.environ.get("HACKIOS", os.path.expanduser("~/hackios"))
SRC = f"{H}/ios165/rootfs/System/Library/CoreServices/SpringBoard.app/DefaultIconState~iphone.plist"
V = f"{H}/userland/var/mobile/Library"

d = plistlib.load(open(SRC + ".orig" if os.path.exists(SRC + ".orig") else SRC, "rb"))


def keep(e):
    # apps (bundle IDs) and folders stay; widgets and widget stacks go
    return not (isinstance(e, dict) and ("elements" in e or e.get("elementType") in ("widget", "batteries")))


state = {
    "buttonBar": d["buttonBar"],
    "iconLists": [p for p in ([e for e in page if keep(e)] for page in d["iconLists"]) if p],
    "today": [],
}
os.makedirs(f"{V}/SpringBoard", exist_ok=True)
os.makedirs(f"{V}/Preferences", exist_ok=True)
with open(f"{V}/SpringBoard/IconState.plist", "wb") as f:
    plistlib.dump(state, f)
with open(f"{V}/Preferences/com.apple.uikitservices.userInterfaceStyleMode.plist", "wb") as f:
    plistlib.dump({"UserInterfaceStyleMode": 2}, f)  # 1 light, 2 dark
print("home-screen: pages", [len(p) for p in state["iconLists"]], "dock", len(state["buttonBar"]))
