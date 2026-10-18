#!/usr/bin/env python3 -I
"""v3k_session.py SPEC.json : Vita3K runs of the release candidate while holding the harness lock the whole time.

The emulated ux0:data/infected is shared with other users of the harness, so a session stages what a run needs (config.txt, settings.txt,
pad script, memory stick, shader cache, ...) by moving the existing file aside, runs, collects the results, and puts everything back
before the lock is released. Uses the harness module (/home/claude/ws/v3k/bin/v3k.py) as its own hdmov twin does.

SPEC.json:
 { "title": "INFC00001", "vpk": "...vpk" (installed first; optional), "elf": "...elf" (symbolise crashes; optional), "out": "dir",
   "lock_wait": 3300,
   "runs": [ { "name": "A", "timeout": 600, "config": "text of config.txt", "settings": "text of settings.txt" (optional),
               "files": { "pad.script": "/host/path", "ms0": "/host/dir", "dumps": "/host/dir" },   # staged (copied) into the data dir
               "remove": ["settings.txt", "shader_cache"],                                            # moved aside for this run
               "keys": "v3k key script text" or "keysfile": "path of a key script", "wait_log": "regex", "shots": "N@SECONDS", "sets": ["k=v"], "tail": 8 } ] }
"""
import importlib.util, json, os, shutil, sys, time, zipfile

H = "/home/claude/ws/v3k/bin/v3k.py"
spec = importlib.util.spec_from_file_location("v3k", H)
v = importlib.util.module_from_spec(spec)
sys.modules["v3k"] = v
spec.loader.exec_module(v)

S = json.load(open(sys.argv[1]))
DATA = f"{v.FS}/ux0/data/infected"
BAK = f"{DATA}/.rc_session_backup"
os.makedirs(S["out"], exist_ok=True)


def aside(name):
    """Moves DATA/name into the backup (if it exists)."""
    src = f"{DATA}/{name}"
    if os.path.lexists(src):
        os.makedirs(BAK, exist_ok=True)
        dst = f"{BAK}/{name}"
        assert not os.path.lexists(dst), f"backup of {name} already exists: {dst}"
        os.rename(src, dst)
        return True
    return False


def restore_all():
    if not os.path.isdir(BAK):
        return
    for name in os.listdir(BAK):
        dst = f"{DATA}/{name}"
        if os.path.lexists(dst):
            if os.path.isdir(dst) and not os.path.islink(dst):
                shutil.rmtree(dst, ignore_errors=True)
            else:
                os.unlink(dst)
        os.rename(f"{BAK}/{name}", dst)
    os.rmdir(BAK)


def copy_in(src, name):
    dst = f"{DATA}/{name}"
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.isdir(src):
        shutil.copytree(src, dst)
    else:
        shutil.copyfile(src, dst)


with v.Lock("rc session (release candidate)", S.get("lock_wait", 3300)):
    assert not os.path.exists(BAK), f"{BAK} exists from an interrupted session: restore it by hand first (mv its contents back)"
    title = S.get("title", "INFC00001")
    if S.get("vpk"):
        dest = f"{v.FS}/ux0/app/{title}"
        shutil.rmtree(dest, ignore_errors=True)
        zipfile.ZipFile(S["vpk"]).extractall(dest)
        print(f"installed {S['vpk']} as {title}", flush=True)
    try:
        for r in S["runs"]:
            name = r["name"]
            out_dir = f"{S['out']}/{name}"
            shutil.rmtree(out_dir, ignore_errors=True)          # nothing of an earlier run of the same name may be mistaken for this run's result
            os.makedirs(out_dir, exist_ok=True)
            touched = ["config.txt", "log.txt"] + list(r.get("files", {})) + list(r.get("remove", []))
            if r.get("settings") is not None:
                touched.append("settings.txt")
            for t in dict.fromkeys(touched):
                aside(t)
            open(f"{DATA}/config.txt", "w").write(r.get("config", ""))
            if r.get("settings") is not None:
                open(f"{DATA}/settings.txt", "w").write(r["settings"])
            for dst, src in r.get("files", {}).items():
                copy_in(src, dst)
            print(f"=== run {name} (timeout {r['timeout']} s)", flush=True)
            t0 = time.time()
            try:
                res = v.run_once(None, title, r["timeout"], keys=(open(r["keysfile"]).read() if r.get("keysfile") else r.get("keys", "")), wait_log=r.get("wait_log"),
                                 shots=r.get("shots"), sets=r.get("sets", []), elf=S.get("elf"), trace=r.get("trace", False),
                                 extra_env=r.get("env", []), crash_re=None if r.get("no_crash_detect") else v.CRASH_RE)
                v.print_result(res, tail=r.get("tail", 8))
                rd = os.path.realpath(f"{v.RUNS}/last")
                print("run dir:", rd, f"wall {time.time() - t0:.0f} s", flush=True)
                for f in ("emulator.log", "result.env", "crash.txt", "launcher.out", "guest.log", "runner.log"):
                    if os.path.exists(f"{rd}/{f}"):
                        shutil.copyfile(f"{rd}/{f}", f"{out_dir}/{f}")
                if os.path.isdir(f"{rd}/shots"):
                    shutil.copytree(f"{rd}/shots", f"{out_dir}/shots", dirs_exist_ok=True)
            finally:
                if os.path.exists(f"{DATA}/log.txt"):
                    shutil.copyfile(f"{DATA}/log.txt", f"{out_dir}/log.txt")
                # collect files the run produced that the spec asks for (shader cache, saves)
                for keep in r.get("collect", []):
                    p = f"{DATA}/{keep}"
                    if os.path.exists(p):
                        d = f"{out_dir}/collected/{keep}"
                        shutil.rmtree(d, ignore_errors=True) if os.path.isdir(d) else None
                        os.makedirs(os.path.dirname(d), exist_ok=True)
                        if os.path.isdir(p):
                            shutil.copytree(p, d, dirs_exist_ok=True)
                        else:
                            shutil.copyfile(p, d)
                # take away what the run left under the staged names and put the originals back
                for t in dict.fromkeys(touched):
                    p = f"{DATA}/{t}"
                    if os.path.lexists(p):
                        shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) and not os.path.islink(p) else os.unlink(p)
                    if os.path.lexists(f"{BAK}/{t}"):
                        os.rename(f"{BAK}/{t}", p)
            v.prune_runs()
    finally:
        restore_all()
        print("data directory restored", flush=True)
sys.exit(0)
