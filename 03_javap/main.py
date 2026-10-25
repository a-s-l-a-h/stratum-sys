import argparse
import json
import subprocess
import sys
from pathlib import Path


def print_header(title):
    print("==================================================")
    print(f" {title}")
    print("==================================================")


def run_javap(javap_path: str, classpath: str, fqn: str) -> tuple[bool, str]:
    """
    Run javap on a single class.
    Returns (success, output_text).
    """
    cmd = [
        javap_path,
        "-s",          # show JNI descriptors (critical for GetMethodID)
        "-p",          # show private members too
        "-constants",  # show static final constant values
        "-classpath", classpath,
        fqn
    ]
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=30
        )
        if result.returncode == 0:
            return True, result.stdout
        else:
            return False, result.stderr
    except subprocess.TimeoutExpired:
        return False, "TIMEOUT after 30 seconds"
    except Exception as e:
        return False, str(e)


def fqn_to_path(fqn: str) -> Path:
    """
    android.widget.Button -> android/widget/Button
    as a Path object (no extension)
    """
    return Path(*fqn.split("."))


def main():
    parser = argparse.ArgumentParser(description="Stratum Stage 03 - Run javap")
    parser.add_argument("--input",   required=True, help="Path to 01_extract/output/")
    parser.add_argument("--targets", required=True, help="Path to 02_inspect/targets.json")
    parser.add_argument("--setup",   required=True, help="Path to 00_setup/output/setup_report.json")
    parser.add_argument("--output",  required=True, help="Path to 03_javap/output/")
    args = parser.parse_args()

    print_header("STRATUM PIPELINE - STAGE 03 (JAVAP)")

    # ── Resolve paths (cross-platform & quote-safe) ───────────────────────────
    input_dir = Path(str(args.input).strip(' "\'')).resolve()
    
    targets_path = Path(str(args.targets).strip(' "\'')).resolve()
    targets_file = targets_path / "targets.json" if targets_path.is_dir() else targets_path

    setup_path = Path(str(args.setup).strip(' "\'')).resolve()
    setup_file = setup_path / "setup_report.json" if setup_path.is_dir() else setup_path

    output_dir = Path(str(args.output).strip(' "\'')).resolve()

    # ── Validate inputs ────────────────────────────────────────────────────────
    if not input_dir.is_dir():
        print(f"ERROR: Input directory not found: {input_dir}")
        sys.exit(1)

    if not targets_file.is_file():
        print(f"ERROR: targets.json not found: {targets_file}")
        sys.exit(1)

    if not setup_file.is_file():
        print(f"ERROR: setup_report.json not found: {setup_file}")
        sys.exit(1)

    # ── Read setup report (get javap path) ─────────────────────────────────────
    with open(setup_file, "r", encoding="utf-8") as f:
        setup = json.load(f)

    raw_javap = setup.get("javap_path")
    if not raw_javap:
        print("ERROR: 'javap_path' is missing or empty in setup_report.json.")
        sys.exit(1)

    javap_bin = Path(raw_javap).resolve()
    if not javap_bin.is_file():
        print(f"ERROR: Specified javap executable does not exist at: {javap_bin}")
        sys.exit(1)

    javap_path = str(javap_bin)
    print(f"-> javap path : {javap_path}")

    # ── Read targets.json ──────────────────────────────────────────────────────
    with open(targets_file, "r", encoding="utf-8") as f:
        targets_data = json.load(f)

    mode    = targets_data.get("mode", "manual")
    targets = targets_data.get("targets", [])
    print(f"-> Mode       : {mode}")

    # ── Decide which classes to process ───────────────────────────────────────
    if mode == "full":
        # Every .class file in the extract output
        class_files = list(input_dir.rglob("*.class"))
        to_process = []
        for cf in class_files:
            rel = cf.relative_to(input_dir).with_suffix("")
            fqn = str(rel).replace("\\", ".").replace("/", ".")
            to_process.append(fqn)
        to_process.sort()
        print(f"-> Full mode  : {len(to_process)} classes")
    else:
        # Only enabled entries
        to_process = [
            t["fqn"] for t in targets
            if t.get("enabled", False)
        ]
        print(f"-> Manual mode: {len(to_process)} enabled classes")

    if not to_process:
        print("ERROR: No classes to process. Check targets.json.")
        sys.exit(1)

    # ── Run javap on each class ────────────────────────────────────────────────
    output_dir.mkdir(parents=True, exist_ok=True)

    successful = []
    failed     = []

    print()
    for i, fqn in enumerate(to_process, 1):
        rel_path  = fqn_to_path(fqn)
        out_file  = output_dir / rel_path.with_suffix(".javap")
        out_file.parent.mkdir(parents=True, exist_ok=True)

        ok, text = run_javap(javap_path, str(input_dir), fqn)

        if ok:
            out_file.write_text(text, encoding="utf-8")
            successful.append(fqn)
            print(f"  [{i:3d}/{len(to_process)}] OK      {fqn}")
        else:
            failed.append({"fqn": fqn, "error": text.strip()})
            print(f"  [{i:3d}/{len(to_process)}] FAILED  {fqn}")
            print(f"           {text.strip()[:120]}")

    # ── Write summary ──────────────────────────────────────────────────────────
    summary = {
        "mode":            mode,
        "total_processed": len(to_process),
        "successful":      len(successful),
        "failed":          len(failed),
        "failed_classes":  failed,
    }
    summary_file = output_dir / "javap_summary.json"
    with open(summary_file, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)

    # ── Final report ───────────────────────────────────────────────────────────
    print()
    print_header("STAGE 03 COMPLETE")
    print(f"-> Processed  : {len(to_process)}")
    print(f"-> Successful : {len(successful)}")
    print(f"-> Failed     : {len(failed)}")
    print(f"-> Output     : {output_dir}")
    print(f"-> Summary    : {summary_file}")

    if failed:
        print()
        print("FAILED CLASSES:")
        for f in failed:
            print(f"  {f['fqn']}")
            print(f"    {f['error'][:100]}")
        print()
        print("These classes were skipped. Check the FQN spelling in targets.json.")
        print("All other classes succeeded. You can proceed to Stage 04.")
    else:
        print()
        print("All classes succeeded. Proceed to Stage 04.")


if __name__ == "__main__":
    main()