#!/usr/bin/env python3
"""Self-test for the recomp-launcher consumer.

Proves the RecompLauncher v1 contract against the real Asura's Wrath manifest
WITHOUT launching the engine: verify (present/missing), config new/ensure
idempotence, placeholder resolution, exit-code mapping, saves collection.

Run:  python3 -I engine/test-recomp-launcher.py [path-to-asura-package]
Defaults to the on-host proven package.
"""
import json, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CONSUMER = os.path.join(HERE, "recomp-launcher.py")
DEFAULT_PKG = "/home/dio/Games/AsurasWrathRecomp-linux-x86_64"

def run(launcher, install, phase, extra=(), env=None):
    cmd = [sys.executable, "-I", CONSUMER, install, phase, *extra]
    e = dict(os.environ)
    if env: e.update(env)
    return subprocess.run(cmd, capture_output=True, text=True, env=e, cwd="/tmp")

passed = 0
def check(name, cond):
    global passed
    print(f"  [{'ok' if cond else 'XX'}] {name}")
    assert cond, name
    passed += 1

pkg = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PKG
tmp = tempfile.mkdtemp(prefix="recomp-test-")
install = os.path.join(tmp, "install")
cfghome = os.path.join(tmp, "xdg")
os.makedirs(install); os.makedirs(cfghome)
shutil.copy(os.path.join(pkg, "recomplauncher.json"), install)
env = {"XDG_CONFIG_HOME": cfghome}
disc = os.path.join(pkg, "disc_extracted")
m = json.load(open(os.path.join(install, "recomplauncher.json")))
print(f"manifest: {m['name']}  (format {m.get('format')})")

# 1) verify present
r = run(CONSUMER, install, "verify", ["--game", disc], env)
check("verify present -> rc 0", r.returncode == 0)
check("verify logs OK", "OK" in r.stdout)

# 2) verify missing -> exit code 2 + manifest message
r = run(CONSUMER, install, "verify", ["--game", os.path.join(tmp, "nope")], env)
check("verify missing -> rc 2", r.returncode == 2)
check("missing surfaces exit_codes[2]", m["exit_codes"]["2"] in r.stdout)

# 3) config new: first run writes defaults, second run does NOT clobber user edits
r = run(CONSUMER, install, "config", ["--game", disc], env)
check("config first run -> rc 0", r.returncode == 0)
s = json.load(open(os.path.join(install, "settings.json")))
check("new defaults written", s.get("resolution_scale") == "150")
s["fullscreen"] = False; s["vblank_rate"] = 120
json.dump(s, open(os.path.join(install, "settings.json"), "w"))
r = run(CONSUMER, install, "config", ["--game", disc], env)
s2 = json.load(open(os.path.join(install, "settings.json")))
check("new does not clobber user edits", s2["fullscreen"] is False and s2["vblank_rate"] == 120)

# 4) saves collection (rpcs3 savedata + settings + recomp.cfg pin)
rpc = os.path.join(cfghome, "rpcs3", "dev_hdd0", "home", "00000001", "savedata")
os.makedirs(rpc, exist_ok=True); open(os.path.join(rpc, "SAVE1"), "w").write("x")
shutil.copy(os.path.join(pkg, "recomp.cfg"), install)
r = run(CONSUMER, install, "saves", ["--game", disc], env)
collected = os.path.join(install, ".recomp", "saves")
names = sorted(os.listdir(collected)) if os.path.isdir(collected) else []
check("saves kept 3/3", "kept 3 of 3" in r.stdout, )
check("saves copied savedata+settings+cfg", len(names) == 3 and any("settings" in n for n in names))

# 5) placeholder resolution: {config} -> XDG_CONFIG_HOME, proven by the savedata
#    target (defined as {config}/rpcs3/.../savedata in the manifest) landing there
check("saves found {config}/rpcs3 savedata", any("savedata" in n for n in names))

shutil.rmtree(tmp, ignore_errors=True)
print(f"\n{passed} checks passed — RecompLauncher v1 contract holds against the real manifest.")
