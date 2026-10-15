#!/usr/bin/env python3
"""Check that unchanged tests reject selection faults in temporary copies."""
import argparse
import difflib
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path("kern/mm/best_fit_pmm.c")


def fingerprints(root):
    """Snapshot production sources and every existing test, excluding outputs."""
    files = [p for folder in ("kern", "libs", "tests", "tools")
             for p in (root / folder).rglob("*")
             if p.is_file() and "__pycache__" not in p.parts]
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="qemu-system-riscv64")
    parser.add_argument("--gcc-prefix", default="riscv64-unknown-elf-")
    args = parser.parse_args()
    logs = ROOT / "obj/check-best-fit-review"
    logs.mkdir(parents=True, exist_ok=True)
    before = fingerprints(ROOT)
    original = (ROOT / SOURCE).read_text()
    variants = {
        "baseline": original,
        "first-fit": original.replace("            best = p;\n",
                                       "            best = p;\n            break;\n", 1),
        "high-address-tie": original.replace("p->property < best->property",
                                             "p->property <= best->property", 1),
    }
    for name, text in variants.items():
        if name != "baseline" and text == original:
            raise RuntimeError(f"mutation did not match: {name}")
        with tempfile.TemporaryDirectory(prefix=f"lab2-review-{name}-") as tmp:
            copy = Path(tmp) / "code"
            shutil.copytree(ROOT, copy, ignore=shutil.ignore_patterns(
                "obj", "bin", ".git", "__pycache__", "*.pyc"))
            (copy / SOURCE).write_text(text)
            # This is the only changed file in the disposable copy.
            expected = dict(before)
            expected[str(SOURCE)] = hashlib.sha256(text.encode()).hexdigest()
            assert fingerprints(copy) == expected
            (logs / f"{name}.diff").write_text("".join(difflib.unified_diff(
                original.splitlines(keepends=True), text.splitlines(keepends=True),
                fromfile="baseline/best_fit_pmm.c", tofile=f"{name}/best_fit_pmm.c")))
            result = subprocess.run([
                sys.executable, "tools/check_best_fit.py", "--qemu", args.qemu,
                "--gcc-prefix", args.gcc_prefix], cwd=copy,
                capture_output=True, text=True, timeout=60)
            (logs / f"{name}-runner.log").write_text(result.stdout + result.stderr)
            test_logs = copy / "obj/check-best-fit"
            if test_logs.exists():
                destination = logs / name
                destination.mkdir(exist_ok=True)
                for file in test_logs.iterdir():
                    if file.is_file():
                        shutil.copy2(file, destination / file.name)
            console = "\n".join(p.read_text(errors="replace")
                                for p in test_logs.glob("console*.log"))
            if name == "baseline":
                if result.returncode != 0 or console.count("BEST_FIT_PASS") != 2:
                    raise RuntimeError(f"baseline failed; see {logs}")
                print("PASS baseline: unchanged tests passed at -O0 and -O2")
            else:
                if result.returncode == 0 or "BEST_FIT_FAIL" not in console:
                    raise RuntimeError(f"fault not rejected by runtime assertions: {name}")
                print(f"PASS negative control: {name} rejected by unchanged tests (-O0)")
    assert fingerprints(ROOT) == before
    (logs / "preservation.log").write_text(
        "PASS: all production sources, tests and tools unchanged during checks\n"
        "PASS: temporary copies differed only in the intended allocator mutation\n")
    print(f"PASS source/test preservation; logs: {logs}")
    print("These are isolated allocator tests; full production boot is not checked.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, AssertionError, subprocess.SubprocessError) as error:
        raise SystemExit(f"FAIL: {error}")
