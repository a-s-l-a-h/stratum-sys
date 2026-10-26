#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Stratum - one-command pipeline runner.

    python run_pipeline.py                      # run every enabled job in pipeline.json
    python run_pipeline.py --job 35             # only API 35 (name or api_version; repeatable)
    python run_pipeline.py --only 04,05         # only stages whose id starts with these
    python run_pipeline.py --from 04_parse      # start at a stage, run to the end
    python run_pipeline.py --dry-run            # print the plan, download/run nothing
    python run_pipeline.py --list               # show jobs + stage toggles
    python run_pipeline.py --force-download     # ignore cached downloads

Per job, everything lives in  <build_root>/API_<n>/ :
    download/        platform zip, JDK archive, hiddenapi csv, ...   (resumable, verified, cached)
    tools/android/  extracted platform (android.jar, data/api-versions.xml)
    tools/jdk/      extracted JDK (used only by this pipeline, nothing installed system-wide)
    00_setup ... 21_cpp_emit/   stage outputs
Final SDK (the package.py `stratum/` folder) is zipped into the folder given by the job's
"zip_dir" / "zip_name" (both required in pipeline.json, no hardcoded default).
Run it from the project root (the folder that contains 00_setup/, 01_extract/ ...).
"""
import argparse
import http.client  # noqa: F401  (IncompleteRead is caught via Exception)
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path
from urllib.error import HTTPError
from urllib.parse import unquote, urlparse
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parent
UA = "stratum-pipeline/1.0"
IS_WIN = os.name == "nt"

STAGE_IDS = [
    "00_setup", "01_extract", "02_inspect", "03_javap", "04_parse", "04_5_sanitize",
    "05_resolve_pass1", "05_5_abstract", "05_resolve_pass2",
    "20_engine_core", "21_cpp_emit", "21b_package", "zip_package",
]


class PipelineError(Exception):
    pass


class _Fatal(Exception):
    """Non-retryable download error (404, 403 ...)."""


def log(msg=""):
    print(msg, flush=True)


def human(n):
    n = float(n)
    for u in ("B", "KB", "MB", "GB"):
        if n < 1024 or u == "GB":
            return f"{n:.0f} {u}" if u == "B" else f"{n:.1f} {u}"
        n /= 1024


def arg(p):
    """Command-line form of a path: relative to the project root with forward slashes
    (same as the manual commands), absolute only if it lives outside the project."""
    if not isinstance(p, Path):
        return str(p)
    try:
        return Path(p).resolve().relative_to(ROOT).as_posix()
    except Exception:
        return str(p)


def rel(p):
    try:
        return Path(p).resolve().relative_to(ROOT).as_posix()
    except Exception:
        return str(p)


# ─────────────────────────────── downloading ────────────────────────────────

def _sidecar(path: Path) -> Path:
    return path.with_name(path.name + ".ok")


def _sha256(path: Path) -> str:
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_file(path: Path, sha256=None) -> bool:
    try:
        if not path.exists() or path.stat().st_size == 0:
            return False
        if sha256 and _sha256(path).lower() != sha256.lower():
            return False
        n = path.name.lower()
        if n.endswith((".zip", ".jar")):
            with zipfile.ZipFile(path) as z:
                return z.testzip() is None
        if n.endswith((".tar.gz", ".tgz", ".tar")):
            with tarfile.open(path) as t:
                for _ in t:
                    pass
            return True
        return True
    except Exception:
        return False


def is_verified(path: Path, sha256=None) -> bool:
    ok = _sidecar(path)
    if path.exists() and ok.exists() and not sha256:
        try:
            if ok.read_text().strip() == str(path.stat().st_size):
                return True
        except OSError:
            pass
    if verify_file(path, sha256):
        ok.write_text(str(path.stat().st_size))
        return True
    return False


def _fetch(url: str, part: Path, timeout: float):
    start = part.stat().st_size if part.exists() else 0
    headers = {"User-Agent": UA}
    if start:
        headers["Range"] = f"bytes={start}-"
    try:
        resp = urlopen(Request(url, headers=headers), timeout=timeout)
    except HTTPError as e:
        if e.code == 416 and start:
            return  # the .part already holds the full file; integrity check decides
        if 400 <= e.code < 500 and e.code not in (408, 429):
            raise _Fatal(f"HTTP {e.code} {e.reason} for {url}")
        raise
    with resp:
        status = getattr(resp, "status", 200)
        clen = resp.headers.get("Content-Length")
        clen = int(clen) if clen and clen.isdigit() else None
        if start and status == 206:
            mode, total = "ab", (start + clen if clen is not None else None)
            log(f"   resuming from {human(start)}")
        else:
            mode, start, total = "wb", 0, clen
        got, t0, last = start, time.time(), 0.0
        tty = sys.stdout.isatty()
        with open(part, mode) as f:
            while True:
                chunk = resp.read(1 << 18)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                now = time.time()
                if now - last >= (0.25 if tty else 5.0):
                    last = now
                    speed = (got - start) / max(now - t0, 1e-6)
                    pct = f"{got * 100 / total:5.1f}%" if total else "  ?  "
                    line = f"   {pct}  {human(got)}" + (f" / {human(total)}" if total else "") + f"  {human(speed)}/s"
                    if tty:
                        sys.stdout.write("\r" + line + "   ")
                        sys.stdout.flush()
                    else:
                        log(line)
        if tty:
            sys.stdout.write("\n")
        if total is not None and got != total:
            raise IOError(f"incomplete download ({got}/{total} bytes)")


def download(url, dest: Path, g: dict, force=False, sha256=None, dry=False) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    ok, part = _sidecar(dest), dest.with_name(dest.name + ".part")
    if dry:
        state = "cached" if dest.exists() and not force else "would download"
        log(f"   [dry-run] {state}: {url} -> {rel(dest)}")
        return dest
    if force:
        for p in (dest, ok, part):
            p.unlink(missing_ok=True)
    if dest.exists():
        if is_verified(dest, sha256):
            log(f"   [cached] {rel(dest)} ({human(dest.stat().st_size)}) - skipping download")
            return dest
        log(f"   [corrupt] {rel(dest)} failed verification - downloading again")
        dest.unlink(missing_ok=True)
        ok.unlink(missing_ok=True)

    retries = int(g.get("retries", 5))
    timeout = float(g.get("timeout_seconds", 60))
    log(f"   downloading {url}")
    last_err = None
    for attempt in range(1, retries + 1):
        try:
            _fetch(url, part, timeout)
            if not part.exists():
                raise IOError("server returned no data")
            os.replace(part, dest)
            if not verify_file(dest, sha256):
                dest.unlink(missing_ok=True)
                raise IOError("file failed integrity check (will restart from zero)")
            ok.write_text(str(dest.stat().st_size))
            log(f"   [done] {rel(dest)} ({human(dest.stat().st_size)})")
            return dest
        except _Fatal as e:
            raise PipelineError(str(e))
        except Exception as e:  # network errors, truncation, corrupt file
            last_err = e
            log(f"   ! attempt {attempt}/{retries} failed: {e}")
            if attempt < retries:
                wait = min(2 ** attempt, 30)
                log(f"     retrying in {wait}s (partial data is kept and resumed)")
                time.sleep(wait)
    raise PipelineError(f"download failed after {retries} attempts: {url} ({last_err})")


# ─────────────────────────────── extracting ─────────────────────────────────

def extract(archive: Path, dest: Path, dry=False):
    if dry:
        log(f"   [dry-run] would extract {rel(archive)} -> {rel(dest)}")
        return
    marker = dest / ".stratum_extracted"
    sig = f"{archive.name}:{archive.stat().st_size}"
    if marker.exists() and marker.read_text() == sig:
        log(f"   [cached] already extracted in {rel(dest)}")
        return
    if dest.exists():
        shutil.rmtree(dest)
    tmp = dest.with_name(dest.name + ".tmp")
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    log(f"   extracting {archive.name} -> {rel(dest)}")
    root = tmp.resolve()

    def safe(name):
        t = (tmp / name).resolve()
        if t != root and root not in t.parents:
            raise PipelineError(f"unsafe path in archive: {name}")

    try:
        if zipfile.is_zipfile(archive):
            with zipfile.ZipFile(archive) as z:
                infos = z.infolist()
                for i in infos:
                    safe(i.filename)
                for i in infos:
                    z.extract(i, tmp)
                    mode = (i.external_attr >> 16) & 0o777
                    if mode and not IS_WIN:
                        try:
                            os.chmod(tmp / i.filename, mode)
                        except OSError:
                            pass
        else:
            with tarfile.open(archive) as t:
                for m in t.getmembers():
                    safe(m.name)
                try:
                    t.extractall(tmp, filter="data")
                except TypeError:  # Python < 3.12 without filter support
                    t.extractall(tmp)
        os.replace(tmp, dest)
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    marker.write_text(sig)


# ───────────────────────────── tool discovery ───────────────────────────────

def host():
    osn = {"Windows": "windows", "Linux": "linux", "Darwin": "darwin"}.get(platform.system(), "linux")
    m = platform.machine().lower()
    return osn, ("aarch64" if m in ("arm64", "aarch64") else "x64")


def pick_url(spec):
    if isinstance(spec, str):
        return spec
    osn, arch = host()
    for k in (f"{osn}-{arch}", osn, "default"):
        if spec.get(k):
            return spec[k]
    raise PipelineError(f"jdk.url has no entry for '{osn}-{arch}' (add one, or a '{osn}' / 'default' key)")


def find_jdk_home(root: Path):
    exe = "javap.exe" if IS_WIN else "javap"
    cands = [root]
    if root.exists():
        for p in root.iterdir():
            if p.is_dir():
                cands += [p, p / "Contents" / "Home"]
    for c in cands:
        if (c / "bin" / exe).exists():
            return c
    return None


def make_jdk_executable(jdk_home: Path):
    """chmod +x the JDK launchers (zip extraction / odd archives can lose the exec bit)."""
    if IS_WIN:
        return
    n = 0
    targets = [p for p in (jdk_home / "bin").glob("*") if p.is_file()]
    for extra in ("jspawnhelper", "jexec"):
        targets += [p for p in (jdk_home / "lib").glob(extra) if p.is_file()]
    for p in targets:
        try:
            p.chmod(p.stat().st_mode | 0o111)
            n += 1
        except OSError:
            pass
    log(f"   chmod +x applied to {n} JDK files")


def javap_version(jdk_home: Path) -> str:
    exe = jdk_home / "bin" / ("javap.exe" if IS_WIN else "javap")
    r = subprocess.run([str(exe), "-version"], capture_output=True, text=True)
    return (r.stdout.strip() or r.stderr.strip())


def find_android_jar(tools_android: Path, api: str):
    jars = sorted(tools_android.rglob("android.jar"))
    if not jars:
        return None
    for j in jars:
        if f"android-{api}" in j.parts:
            return j
    return jars[0]


# ─────────────────────────────────── job ────────────────────────────────────

class Job:
    def __init__(self, cfg, g):
        for k in ("api_version", "android_jar_url", "jdk"):
            if k not in cfg:
                raise PipelineError(f"job '{cfg.get('name', '?')}' is missing '{k}'")
        self.cfg, self.g = cfg, g
        self.api = str(cfg["api_version"])
        self.name = cfg.get("name", f"android-{self.api}")
        self.opt = cfg.get("options", {})
        self.extra = cfg.get("extra_args", {})
        self.base = (ROOT / g.get("build_root", "build")).resolve() / f"API_{self.api}"
        b = self.base
        self.dl, self.tools = b / "download", b / "tools"
        self.s00, self.s01, self.s02, self.s03 = b / "00_setup", b / "01_extract", b / "02_inspect", b / "03_javap"
        self.s04, self.s045 = b / "04_parse", b / "04_5_sanitize"
        self.s05a, self.s055 = b / "05_resolve" / "output", b / "05_5_abstract" / "output"
        self.s05b = b / "05_resolve" / "output_patched"
        self.e20, self.e21 = b / "20_engine_core" / "generated", b / "21_cpp_emit" / "output"
        self.ctx = {}
        # zip location comes ONLY from the json (placeholders: {api}, {api_dir}, {build_root})
        for k in ("zip_dir", "zip_name"):
            if not cfg.get(k):
                raise PipelineError(f"job '{self.name}' is missing '{k}' (where to write the final zip)")
        fmt = dict(api=self.api, api_dir=str(self.base), build_root=str((ROOT / g.get("build_root", "build")).resolve()))
        self.zip_dir = (ROOT / cfg["zip_dir"].format(**fmt)).resolve()
        self.zip_path = self.zip_dir / cfg["zip_name"].format(**fmt)

    def on(self, stage):
        return bool(self.cfg.get("stages", {}).get(stage, True))

    # --- sanitizer / abstract / pass2 wiring depends on the toggles -----------
    @property
    def pass1_input(self):
        return self.s045 if self.on("04_5_sanitize") else self.s04

    @property
    def resolved(self):
        return self.s05b if self.on("05_resolve_pass2") else self.s05a

    # --- tools: download + extract + locate -----------------------------------
    def prepare(self, force=False, dry=False):
        cfg, g = self.cfg, self.g
        log("-- preparing tools (download folder: " + rel(self.dl) + ")")

        # Android platform
        url = cfg["android_jar_url"]
        fname = cfg.get("android_file") or Path(unquote(urlparse(url).path)).name or f"platform-{self.api}.zip"
        arc = download(url, self.dl / fname, g, force, cfg.get("android_sha256"), dry)
        tools_android = self.tools / "android"
        if fname.lower().endswith(".jar"):
            target = tools_android / f"android-{self.api}" / "android.jar"
            if not dry:
                target.parent.mkdir(parents=True, exist_ok=True)
                if not target.exists() or target.stat().st_size != arc.stat().st_size:
                    shutil.copy2(arc, target)
            jar = target
        else:
            extract(arc, tools_android, dry)
            jar = find_android_jar(tools_android, self.api) if not dry else \
                (find_android_jar(tools_android, self.api) if tools_android.exists() else None) or \
                tools_android / f"android-{self.api}" / "android.jar"
        if jar is None or (not dry and not jar.exists()):
            raise PipelineError(f"android.jar not found under {rel(tools_android)} after extraction")
        log(f"   android.jar : {rel(jar)}")

        # api-versions.xml
        xml_spec = cfg.get("api_versions_xml", "auto")
        xml = None
        if xml_spec == "skip":
            log("   api-versions.xml : skipped (config)")
        elif xml_spec == "auto":
            xml = jar.parent / "data" / "api-versions.xml"
            if not dry and not xml.exists():
                raise PipelineError(
                    f"api-versions.xml not found at {rel(xml)}. Set \"api_versions_xml\" to a URL / local path, "
                    "or to \"skip\" to continue without it.")
            log(f"   api-versions.xml : {rel(xml)}")
        elif str(xml_spec).lower().startswith(("http://", "https://")):
            xml = download(xml_spec, self.dl / "api-versions.xml", g, force, None, dry)
            log(f"   api-versions.xml : {rel(xml)}")
        else:
            xml = (ROOT / xml_spec).resolve()
            if not xml.exists():
                raise PipelineError(f"api_versions_xml file not found: {xml}")
            log(f"   api-versions.xml : {xml}")

        # hiddenapi flags
        h_spec = cfg.get("hiddenapi_flags", "skip")
        hidden = None
        if h_spec == "skip":
            log("   hiddenapi-flags  : skipped (config)")
        elif str(h_spec).lower().startswith(("http://", "https://")):
            hidden = download(h_spec, self.dl / "hiddenapi-flags.csv", g, force, cfg.get("hiddenapi_sha256"), dry)
            log(f"   hiddenapi-flags  : {rel(hidden)}")
        else:
            hidden = (ROOT / h_spec).resolve()
            if not hidden.exists():
                raise PipelineError(f"hiddenapi_flags file not found: {hidden}")
            log(f"   hiddenapi-flags  : {hidden}")

        # JDK (private to this API)
        jdk = cfg["jdk"]
        jver = str(jdk["version"])
        if jdk.get("path"):
            home = Path(jdk["path"]).expanduser().resolve()
            if not dry and find_jdk_home(home) is None:
                raise PipelineError(f"jdk.path has no bin/javap: {home}")
            home = find_jdk_home(home) or home
        else:
            osn, arch = host()
            jfile = jdk.get("file") or f"jdk-{jver}-{osn}-{arch}." + ("zip" if osn == "windows" else "tar.gz")
            jarc = download(pick_url(jdk["url"]), self.dl / jfile, g, force, jdk.get("sha256"), dry)
            jdir = self.tools / "jdk"
            extract(jarc, jdir, dry)
            home = find_jdk_home(jdir) if not dry else None
            if home is None:
                if dry:
                    home = jdir
                else:
                    raise PipelineError(f"no bin/javap found inside extracted JDK at {rel(jdir)}")
        if not dry:
            make_jdk_executable(home)
            ver = javap_version(home)
            log(f"   JDK         : {rel(home)}  ({ver})")
            if ver and not ver.split()[-1].startswith(jver):
                log(f"   ! warning: javap reports '{ver}', config says JDK {jver}")
        else:
            log(f"   JDK         : {rel(home)}")
        self.ctx = dict(jar=jar, xml=xml, hidden=hidden, jdk=home, jdk_version=jver)


# ─────────────────────────────────── stages ─────────────────────────────────

class Stage:
    def __init__(self, sid, requires, cmd=None, func=None):
        self.id, self.requires, self.cmd, self.func = sid, requires, cmd, func


def build_stages(j: Job):
    py = sys.executable
    o = j.opt
    x = lambda sid: list(j.extra.get(sid, []))

    def s00():
        c = j.ctx
        cmd = [py, "00_setup/main.py", "--jar-path", c["jar"], "--api-version", j.api,
               "--javap-path", c["jdk"], "--jdk-version", c["jdk_version"]]
        if c["xml"]:
            cmd += ["--api-versions-xml-path", c["xml"]]
        if c["hidden"]:
            cmd += ["--hiddenapi-path", c["hidden"]]
        ndk = j.cfg.get("ndk_path") or j.g.get("ndk_path")
        if ndk:
            cmd += ["--ndk-path", ndk]
        return cmd + ["--output", j.s00] + x("00_setup")

    def s04_5():
        cmd = [py, "04_5_sanitize/main.py", "--input", j.s04, "--output", j.s045]
        if o.get("sanitize_strict", False):
            cmd.append("--strict")
        if o.get("min_sdk") is not None:
            cmd += ["--min-sdk", str(o["min_sdk"])]
        return cmd + x("04_5_sanitize")

    def s05a():
        cmd = [py, "05_resolve/main.py", "--input", j.pass1_input, "--output", j.s05a]
        if o.get("closure_mode"):
            cmd += ["--closure-mode", o["closure_mode"]]
        return cmd + x("05_resolve_pass1")

    def s05b():
        cmd = [py, "05_resolve/main.py", "--input", j.s055 / "patched", "--output", j.s05b]
        if o.get("closure_mode"):
            cmd += ["--closure-mode", o["closure_mode"]]
        return cmd + x("05_resolve_pass2")

    def s21():
        cmd = [py, "21_cpp_emit/main.py", "--input", j.resolved, "--output", j.e21]
        if o.get("emit_prefix"):
            cmd += ["--emit-prefix", o["emit_prefix"]]
        return cmd + x("21_cpp_emit")

    def s21b():
        cmd = [py, "21_cpp_emit/package.py", "--input", j.e21, "--generated", j.e20,
               "--api-version", j.api,
               "--min-sdk", str(o.get("runtime_min_sdk", 24)),
               "--java-version", str(o.get("runtime_java_version", "1.8"))]
        adapters = j.s055 / "java" / "com" / "stratum" / "adapters"
        if adapters.exists() or j.on("05_5_abstract"):
            cmd += ["--adapters", adapters]
        return cmd + ["--output", j.e21 / "stratum"] + x("21b_package")

    def zip_package():
        import hashlib
        src = j.e21 / "stratum"
        j.zip_dir.mkdir(parents=True, exist_ok=True)
        tmp = j.zip_path.with_name(j.zip_path.name + ".tmp")
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for f in sorted(src.rglob("*")):
                if f.is_file():
                    z.write(f, ("stratum/" + f.relative_to(src).as_posix()))
        os.replace(tmp, j.zip_path)
        h = hashlib.sha256(j.zip_path.read_bytes()).hexdigest()
        j.zip_path.with_name(j.zip_path.name + ".sha256").write_text(f"{h}  {j.zip_path.name}\n", encoding="utf-8")
        log(f"   zip    : {rel(j.zip_path)} ({human(j.zip_path.stat().st_size)})")
        log(f"   sha256 : {h}")
        log("   -> unzip gives stratum/: follow stratum/README.md "
            "(cpp/ -> app/src/main/cpp/, stratum-runtime/ -> project root)")

    tf = lambda: o.get("targets_file", "02_inspect/targets.json")
    return [
        Stage("00_setup", lambda: [], s00),
        Stage("01_extract", lambda: [j.s00 / "setup_report.json"],
              lambda: [py, "01_extract/main.py", "--setup", j.s00, "--output", j.s01] + x("01_extract")),
        Stage("02_inspect", lambda: [j.s01],
              lambda: [py, "02_inspect/main.py", "--input", j.s01, "--output", j.s02] + x("02_inspect")),
        Stage("03_javap", lambda: [j.s01, j.s00 / "setup_report.json"],
              lambda: [py, "03_javap/main.py", "--input", j.s01, "--targets", tf(),
                       "--setup", j.s00 / "setup_report.json", "--output", j.s03] + x("03_javap")),
        Stage("04_parse", lambda: [j.s03],
              lambda: [py, "04_parse/main.py", "--input", j.s03, "--output", j.s04] + x("04_parse")),
        Stage("04_5_sanitize", lambda: [j.s04], s04_5),
        Stage("05_resolve_pass1", lambda: [j.pass1_input], s05a),
        Stage("05_5_abstract", lambda: [j.s05a],
              lambda: [py, "05_5_abstract/main.py", "--input", j.s05a, "--output", j.s055,
                       "--mode", o.get("abstract_mode", "on")] + x("05_5_abstract")),
        Stage("05_resolve_pass2", lambda: [j.s055 / "patched"], s05b),
        Stage("20_engine_core", lambda: [j.resolved],
              lambda: [py, "20_engine_core/emit.py", "--input", j.resolved, "--output", j.e20] + x("20_engine_core")),
        Stage("21_cpp_emit", lambda: [j.resolved], s21),
        Stage("21b_package", lambda: [j.e20, j.e21], s21b),
        Stage("zip_package", lambda: [j.e21 / "stratum"], func=zip_package),
    ]


def fmt_cmd(cmd):
    parts = [str(c) for c in cmd]
    return subprocess.list2cmdline(parts) if IS_WIN else shlex.join(parts)


def select_stages(job: Job, args):
    if args.only:
        toks = [t.strip() for t in args.only.split(",") if t.strip()]
        return [s for s in STAGE_IDS if any(s == t or s.startswith(t) for t in toks)]
    sel = [s for s in STAGE_IDS if job.on(s)]
    if args.from_stage:
        first = next((s for s in STAGE_IDS if s == args.from_stage or s.startswith(args.from_stage)), None)
        if not first:
            raise PipelineError(f"unknown --from stage '{args.from_stage}'")
        sel = [s for s in sel if STAGE_IDS.index(s) >= STAGE_IDS.index(first)]
    return sel


def run_job(job: Job, args):
    """Returns (ok, rows[(stage, status, seconds)])."""
    rows = []
    selected = select_stages(job, args)
    bar = "=" * 72
    log(f"\n{bar}\n JOB {job.name}  (API {job.api})   build dir: {rel(job.base)}\n{bar}")
    skipped = [s for s in STAGE_IDS if s not in selected]
    for s in skipped:
        rows.append((s, "skipped", 0.0))

    t0 = time.time()
    try:
        if "00_setup" in selected:
            job.prepare(force=args.force_download, dry=args.dry_run)
            rows.append(("prepare tools", "OK" if not args.dry_run else "dry-run", time.time() - t0))
    except PipelineError as e:
        log(f"\n[FAIL] prepare tools: {e}")
        rows.append(("prepare tools", "FAILED", time.time() - t0))
        return False, rows

    stages = {s.id: s for s in build_stages(job)}
    env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUTF8="1", PYTHONUNBUFFERED="1")
    for n, sid in enumerate(selected, 1):
        st = stages[sid]
        log(f"\n[{n}/{len(selected)}] {sid}\n" + "-" * 72)
        t = time.time()
        try:
            if not args.dry_run:
                missing = [p for p in st.requires() if not Path(p).exists()]
                if missing:
                    raise PipelineError("missing input: " + ", ".join(rel(m) for m in missing) +
                                        "  (run the earlier stage first)")
            if st.cmd:
                cmd = [arg(c) for c in st.cmd()]
                log("$ " + fmt_cmd(cmd))
                if not args.dry_run:
                    rc = subprocess.run(cmd, cwd=ROOT, env=env).returncode
                    if rc != 0:
                        raise PipelineError(f"{sid} exited with code {rc}")
            else:
                log(f"   (internal step: {sid})")
                if not args.dry_run:
                    st.func()
            rows.append((sid, "OK" if not args.dry_run else "dry-run", time.time() - t))
            log(f"[OK] {sid}  ({time.time() - t:.1f}s)")
        except PipelineError as e:
            log(f"\n[FAIL] {sid}: {e}")
            rows.append((sid, "FAILED", time.time() - t))
            return False, rows
        except KeyboardInterrupt:
            raise
        except Exception as e:
            log(f"\n[FAIL] {sid}: {type(e).__name__}: {e}")
            rows.append((sid, "FAILED", time.time() - t))
            return False, rows
    return True, rows


def print_summary(results):
    log("\n" + "=" * 72 + "\n SUMMARY\n" + "=" * 72)
    for name, ok, rows in results:
        log(f" {name}: {'SUCCESS' if ok else 'FAILED'}")
        for sid, status, secs in rows:
            log(f"    {sid:<20} {status:<9} {secs:7.1f}s" if status != "skipped" else f"    {sid:<20} {status}")


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    ap = argparse.ArgumentParser(description="Stratum pipeline runner")
    ap.add_argument("--config", default="pipeline.json")
    ap.add_argument("--job", action="append", help="job name or api_version (repeatable / comma separated)")
    ap.add_argument("--only", help="comma list of stage ids/prefixes to run (overrides config toggles)")
    ap.add_argument("--from", dest="from_stage", help="start at this stage")
    ap.add_argument("--force-download", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    cfg_path = Path(args.config)
    cfg_path = cfg_path if cfg_path.is_absolute() else ROOT / cfg_path
    if not cfg_path.exists():
        sys.exit(f"config not found: {cfg_path}")
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    g = cfg.get("global", {})

    wanted = {w.strip() for a in (args.job or []) for w in a.split(",") if w.strip()}
    jobs_cfg = []
    for jc in cfg.get("jobs", []):
        named = bool(wanted) and (jc.get("name") in wanted or str(jc.get("api_version")) in wanted)
        if wanted and not named:
            continue
        if not wanted and not jc.get("enabled", True):
            continue
        jobs_cfg.append(jc)
    if args.list:
        for jc in cfg.get("jobs", []):
            off = [s for s in STAGE_IDS if not jc.get("stages", {}).get(s, True)]
            log(f"{jc.get('name')}  api={jc.get('api_version')}  enabled={jc.get('enabled', True)}  disabled stages={off or 'none'}")
        return 0
    if not jobs_cfg:
        sys.exit("no jobs selected (check 'enabled' in the config or --job)")

    results = []
    try:
        for jc in jobs_cfg:
            try:
                job = Job(jc, g)
                ok, rows = run_job(job, args)
                results.append((job.name, ok, rows))
            except PipelineError as e:
                log(f"\n[FAIL] job {jc.get('name')}: {e}")
                results.append((jc.get("name", "?"), False, []))
    except KeyboardInterrupt:
        log("\ninterrupted - partial downloads are kept; run again to resume.")
        print_summary(results)
        return 130
    print_summary(results)
    return 0 if all(ok for _, ok, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())