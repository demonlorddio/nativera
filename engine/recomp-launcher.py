#!/usr/bin/env python3
"""
recomp-launcher — the nativera manifest consumer.

Implements the RecompLauncher v1 contract against a title directory that
contains a `recomplauncher.json` + the native executable(s) it names. This is
the single program that turns "a folder of files" into a working native title:

    verify  -> locate/validate the user's BYO game copy
    prepare -> one-time native build/cache (headless, progress, timed out)
    config  -> write settings with new/ensure/always semantics
    start   -> exec the native program with args + injected env
    saves   -> after the session, collect the user-data globs

Stdlib-only, so it runs on Batocera (python3.12) with no deps. Phases are
individually callable so the Phase-2 ES provider can drive them separately.

The manifest is DATA, not code: this file only reads it and drives the
*user's own* native binary named in it. It never execs anything it extracted
from an untrusted archive.
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time

# --- placeholder resolution -------------------------------------------------

# The v1 placeholder set. On nativera (Batocera) these map onto the
# /userdata + $HOME layout; on a desktop install they map onto $HOME.
def _xdg(env, var, default):
    return env.get(var) or os.path.expanduser(default)


def build_placeholders(install_dir, game, env):
    home = _xdg(env, "HOME", "~")
    cfg_home = _xdg(env, "XDG_CONFIG_HOME", "~/.config")
    data_home = _xdg(env, "XDG_DATA_HOME", "~/.local/share")
    docs = _xdg(env, "NATIVERA_DOCUMENTS", home)
    return {
        "install": install_dir,
        "game": game,
        "home": home,
        "config": cfg_home,
        "local": data_home,
        "documents": docs,
        # RecompLauncher pads are a controller *type* (xbox/playstation), not an index.
        "pad": env.get("NATIVERA_PAD", "xbox"),
    }


def substitute(value, ph):
    """Resolve {placeholder} tokens in a string or nested structure.

    {input:<id>} is pulled from NATIVERA_INPUT_<id> (uppercase id). Anything
    unknown is left untouched so a manifest can still name it.
    """
    if isinstance(value, str):
        def rep(m):
            key = m.group(1)
            if key.startswith("input:"):
                iid = key[len("input:"):].upper()
                v = os.environ.get("NATIVERA_INPUT_" + iid)
                # RecompLauncher omits the arg entirely when the optional input is absent.
                return v if v is not None else ""
            return str(ph.get(key, m.group(0)))
        import re
        return re.sub(r"\{([a-z_]+(?::[a-z0-9_]+)?)\}", rep, value)
    if isinstance(value, list):
        return [substitute(v, ph) for v in value]
    if isinstance(value, dict):
        return {k: substitute(v, ph) for k, v in value.items()}
    return value


# start.env may set game-facing variables, but never the dynamic-loader / shell
# names — RecompLauncher blocks these so a manifest can't redirect execution.
_BLOCKED_ENV = {
    "PATH", "LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT", "LD_ORIGIN_PATH",
    "LD_DEBUG", "LD_BIND_NOW", "LD_BIND_NOLOAD", "LD_USE_LOAD_BIAS",
    "DYLD_INSERT_LIBRARIES", "DYLD_LIBRARY_PATH", "DYLD_FORCE_NODELETE",
    "NODE_OPTIONS", "PYTHONPATH", "PYTHONSTARTUP", "BASH_ENV",
    "HOME", "USER", "SHELL",
}


def _sanitize_env(raw):
    """Strip env keys a manifest may not override."""
    return {k: v for k, v in raw.items() if k not in _BLOCKED_ENV}


# --- a small config-file reader/writer for the three supported formats ------

def load_cfg(path, fmt):
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        text = f.read()
    try:
        if fmt == "json":
            return json.loads(text) if text.strip() else {}
        if fmt == "ini":
            import configparser
            cp = configparser.ConfigParser()
            cp.read_string(text)
            return {s: dict(cp[s]) for s in cp.sections()} or {}
        if fmt == "toml":
            try:
                import tomllib
                return tomllib.loads(text)
            except Exception:
                return {}
    except Exception as e:
        sys.stderr.write(f"[recomp] config read failed ({path}): {e}\n")
        return {}


def dump_cfg(obj, path, fmt):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        if fmt == "json":
            json.dump(obj, f, indent=2)
            f.write("\n")
        elif fmt == "ini":
            import configparser
            cp = configparser.ConfigParser()
            for k, v in obj.items():
                cp["recomp"] = dict((str(a), str(b)) for a, b in v.items()) if isinstance(v, dict) else {k: str(v)}
            cp.write(f)
        else:  # toml — minimal flat writer; nested keys are dotted
            for k, v in obj.items():
                f.write(f"{k} = {json.dumps(v)}\n")


def merge_deep(base, override):
    """Recursively merge `override` into `base` (returns new dict)."""
    if not isinstance(base, dict) or not isinstance(override, dict):
        return override
    out = dict(base)
    for k, v in override.items():
        out[k] = merge_deep(out.get(k), v) if isinstance(out.get(k), dict) or isinstance(v, dict) else v
    return out


# --- the consumer -----------------------------------------------------------

class Launcher:
    def __init__(self, install_dir, game=None, env=None, log=None):
        self.install = os.path.abspath(install_dir)
        self.env = dict(os.environ if env is None else env)
        self.log = log or sys.stdout
        self.manifest_path = os.path.join(self.install, "recomplauncher.json")
        with open(self.manifest_path) as f:
            self.manifest = json.load(f)
        self.game = game or self.env.get("NATIVERA_GAME")
        self.ph = build_placeholders(self.install, self.game or "", self.env)
        self.state_dir = os.path.join(self.install, ".recomp")

    def _line(self, msg):
        print(msg, file=self.log, flush=True)

    def _resolve_exe(self, prog):
        """A program entry is a path or a bare name; resolve to an exec target."""
        p = substitute(prog, self.ph)
        if os.path.isabs(p) and os.path.exists(p):
            return p
        cand = os.path.join(substitute("{install}", self.ph), os.path.basename(p))
        if os.path.exists(cand):
            return cand
        found = shutil.which(p)
        return found or cand

    # 1) verify the user's BYO copy -----------------------------------------
    def verify(self):
        g = self.manifest.get("game", {})
        check = g.get("check", {})
        if not self.game:
            self._fail(2, g.get("label"))
        # A `check` entry is {type: value}; type is one of file/iso9660/xdvdfs
        # and value is the path/bytes matched inside the player's own copy.
        for key in ("file", "iso9660", "xdvdfs"):
            rel = check.get(key)
            if rel:
                target = os.path.join(substitute(self.game, self.ph), substitute(rel, self.ph))
                if not os.path.exists(target):
                    self._fail(2, g.get("label"))
                # optional size/crc on the named dump
                want = check.get("size") or check.get("size_bytes")
                if want and os.path.isfile(target):
                    if os.path.getsize(target) != int(want):
                        self._fail(2, g.get("label"))
        self._line(f"[verify] OK — game copy at {self.game}")
        return True

    def _fail(self, code, label):
        msg = self.manifest.get("exit_codes", {}).get(str(code), "")
        self._line(f"[verify] MISSING — {label or 'BYO game copy'}")
        if msg:
            self._line(f"  → {msg}")
        sys.exit(code)

    # 2) one-time native build / cache ---------------------------------------
    def prepare(self, force=False):
        pre = self.manifest.get("prepare")
        if not pre:
            return True
        prog = self._resolve_exe(pre["program"][0])
        # stale-check: only rebuild when the game copy changed since last stamp
        stamp = os.path.join(self.state_dir, "prepare-state.json")
        if not force and os.path.exists(stamp) and self._stamp_matches(stamp):
            self._line(f"[prepare] up to date — skipping ({pre.get('label', 'native build')})")
            return True
        if os.path.exists(stamp):
            self._line(f"[prepare] {pre.get('label', 'building native module')} …")
        else:
            self._line(f"[prepare] {pre.get('label', 'building native module')} (first run) …")
        args = [substitute(a, self.ph) for a in pre.get("args", [])]
        cmd = [prog] + args
        timeout = pre.get("timeout")
        # RecompLauncher prepare emits `progress <0-1> <text>` lines on stdout; we
        # inherit the child's stdout so they reach the console and the wrapper's
        # /userdata/system/logs tee. Exit 0 = success; anything else is a failure.
        try:
            proc = subprocess.run(cmd, env={**self.env, "RECOMPLAUNCHER": "1"}, timeout=timeout)
        except subprocess.TimeoutExpired:
            self._line("[prepare] timed out")
            sys.exit(3)
        if proc.returncode != 0:
            self._fail(3, None)
        os.makedirs(self.state_dir, exist_ok=True)
        with open(stamp, "w") as f:
            json.dump({"game": self.game, "mtime": os.path.getmtime(self.game) if os.path.exists(self.game) else 0,
                       "size": os.path.getsize(self.game) if os.path.isfile(self.game) else None,
                       "at": time.time()}, f)
        self._line(f"[prepare] done")
        return True

    def _stamp_matches(self, stamp_path):
        try:
            with open(stamp_path) as f:
                s = json.load(f)
        except Exception:
            return False
        if s.get("game") != self.game:
            return False
        cur_m = os.path.getmtime(self.game) if os.path.exists(self.game) else 0
        return s.get("mtime") is not None and abs(cur_m - s["mtime"]) < 1

    # 3) write settings ------------------------------------------------------
    def config(self):
        for entry in self.manifest.get("config", []):
            fmt = entry.get("format", "json")
            target = self._first_existing(entry.get("file", []))
            cur = load_cfg(target, fmt)
            base = dict(cur)
            # apply the three write disciplines, lowest precedence first
            if "new" in entry:      # only if the file is absent / empty
                if not cur:
                    base = merge_deep(base, entry["new"])
            if "ensure" in entry:   # fill missing keys, keep existing
                for k, v in entry["ensure"].items():
                    base.setdefault(k, v)
            if "always" in entry:   # overwrite unconditionally
                base = merge_deep(base, entry["always"])
            if base != cur:
                dump_cfg(base, target, fmt)
                self._line(f"[config] wrote {os.path.basename(target)}")

    def _norm(self, f):
        return f.get("path") if isinstance(f, dict) else f

    def _first_existing(self, files):
        if not files:
            return "{install}/config.json"
        for f in files:
            p = substitute(self._norm(f), self.ph)
            if os.path.exists(p):
                return p
        return substitute(self._norm(files[0]), self.ph)

    # 4) launch --------------------------------------------------------------
    def start(self):
        st = self.manifest.get("start", {})
        prog = st["program"][0]
        target = self._resolve_exe(prog)
        # Omit any arg that resolves to empty (an absent {input:<id>}), matching
        # RecompLauncher. Static args are kept verbatim.
        args = [a for a in (substitute(x, self.ph) for x in st.get("args", [])) if a != ""]
        env = dict(self.env)
        env.update(_sanitize_env(substitute(st.get("env", {}), self.ph)))
        # The four standard variables RecompLauncher injects on every launch.
        env.setdefault("RECOMPLAUNCHER", "1")
        env.setdefault("RECOMPLAUNCHER_VERSION", "nativera-dev")
        env.setdefault("RECOMPLAUNCHER_MODE", "bigpicture")
        env["RECOMPLAUNCHER_PAD"] = str(self.ph.get("pad", "xbox"))
        env["RECOMPLAUNCHER_INSTALL"] = self.install
        if self.game:
            env["RECOMPLAUNCHER_GAME"] = self.game
        self._line(f"[start] exec {os.path.basename(target)} {' '.join(args)}")
        try:
            rc = subprocess.call([target] + args, env=env, cwd=self.install)
        except FileNotFoundError:
            self._line(f"[start] binary not found: {target}")
            sys.exit(3)
        self._saves_after()
        self._report_exit(rc)

    # 5) collect user data ---------------------------------------------------
    def _saves_after(self):
        save_root = os.path.join(self.state_dir, "saves")
        os.makedirs(save_root, exist_ok=True)
        kept = 0
        for i, pat in enumerate(self.manifest.get("saves", [])):
            p = substitute(pat, self.ph)
            matches = sorted(glob.glob(p))
            if not matches and os.path.exists(p):
                matches = [p]
            for m in matches:
                if not os.path.exists(m):
                    continue
                base = os.path.basename(m.rstrip("/")) or "root"
                dest = os.path.join(save_root, f"{i}_{base}")
                try:
                    if os.path.isdir(m):
                        shutil.copytree(m, dest, dirs_exist_ok=True)
                    else:
                        if os.path.getsize(m) > 64 * 1024 * 1024:
                            self._line(f"[saves] skip {os.path.basename(m)} (>64 MB)")
                            continue
                        shutil.copy2(m, dest)
                    kept += 1
                except Exception as e:
                    self._line(f"[saves] skip {m}: {e}")
        self._line(f"[saves] kept {kept} of {len(self.manifest.get('saves', []))} user-data target(s) under {save_root}")

    def _report_exit(self, rc):
        if rc == 0:
            return
        code = str(rc)
        msg = self.manifest.get("exit_codes", {}).get(code)
        if msg:
            self._line(f"[exit {rc}] {msg}")
        elif rc in (130, 137):
            self._line(f"[exit {rc}] closed by the user")
        else:
            self._line(f"[exit {rc}] the program stopped unexpectedly — see the Logs folder")

    # full sequence ----------------------------------------------------------
    def run(self, force_prepare=False):
        self.verify()
        self.prepare(force=force_prepare)
        self.config()
        self.start()


def main():
    ap = argparse.ArgumentParser(description="nativera manifest consumer")
    ap.add_argument("title", nargs="?", help="title dir containing recomplauncher.json")
    ap.add_argument("phase", nargs="?", default="run",
                    choices=["run", "verify", "prepare", "config", "start", "saves"])
    ap.add_argument("--game", help="path to the user's BYO game copy/folder/iso")
    ap.add_argument("--force-prepare", action="store_true", help="ignore the prepare stamp")
    ap.add_argument("--env", help="NATIVERA_* override, KEY=VAL (repeatable)", action="append")
    args = ap.parse_args()

    title = args.title or os.environ.get("NATIVERA_TITLE")
    if not title:
        ap.error("need a title dir (positional or NATIVERA_TITLE)")
    l = Launcher(title, game=args.game)
    for kv in args.env or []:
        k, _, v = kv.partition("=")
        l.env[k] = v

    if args.phase == "verify":
        l.verify()
    elif args.phase == "prepare":
        l.verify(); l.prepare(force=args.force_prepare)
    elif args.phase == "config":
        l.verify(); l.config()
    elif args.phase == "saves":
        l._saves_after()
    else:
        l.run(force_prepare=args.force_prepare)


if __name__ == "__main__":
    main()
