#!/usr/bin/env python3
"""Run the real Best-Fit and pmm_init with isolated boot/UART/DTB fixtures."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="qemu-system-riscv64")
    parser.add_argument("--gcc-prefix", default="riscv64-unknown-elf-")
    args = parser.parse_args()
    logs = ROOT / "obj/check-best-fit"
    logs.mkdir(parents=True, exist_ok=True)
    flags = ["-mcmodel=medany", "-std=gnu99", "-Wall", "-Werror", "-Wno-unused",
             "-fno-builtin", "-nostdinc", "-fno-stack-protector", "-g",
             "-ffunction-sections", "-fdata-sections", "-Ilibs", "-Ikern/mm",
             "-Ikern/driver", "-Ikern/debug", "-Itools", "-Ducore_test"]
    with tempfile.TemporaryDirectory(prefix="lab2-best-fit-") as tmp, \
            (logs / "build.log").open("w") as buildlog:
        directory = Path(tmp)

        def run(command):
            buildlog.write("$ " + shlex.join(map(str, command)) + "\n")
            buildlog.flush()
            subprocess.run(command, cwd=ROOT, stdout=buildlog, stderr=subprocess.STDOUT,
                           check=True, timeout=30)

        for optimization in ("-O0", "-O2"):
            objects = []
            sources = ["tests/page_init_boot.S", "tests/best_fit_check.c", "kern/mm/pmm.c",
                       "kern/libs/stdio.c", "libs/printfmt.c", "libs/string.c",
                       "tools/test_lab2.c"]
            for index, source in enumerate(sources):
                obj = directory / f"{index}.o"
                run([args.gcc_prefix + "gcc", *flags, optimization, "-c", source,
                     "-o", str(obj)])
                objects.append(str(obj))
            elf, image = directory / "kernel", directory / "ucore.img"
            run([args.gcc_prefix + "ld", "-m", "elf64lriscv", "-nostdlib",
                 "--gc-sections", "-T", "tools/kernel.ld", "-o", str(elf), *objects])
            run([args.gcc_prefix + "objcopy", "-O", "binary", str(elf), str(image)])
            log = logs / f"console{optimization}.log"
            with log.open("w") as output:
                process = subprocess.Popen(shlex.split(args.qemu) + [
                    "-machine", "virt", "-nographic", "-bios", "default", "-kernel",
                    str(image)], cwd=ROOT, stdin=subprocess.DEVNULL,
                    stdout=output, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 10
                    while True:
                        text = log.read_text(errors="replace")
                        if "BEST_FIT_FAIL" in text or ": FAIL" in text:
                            raise RuntimeError(f"{optimization} failed; see {log}")
                        if "BEST_FIT_PASS" in text:
                            for marker in ("memory management: best_fit_pmm_manager",
                                           "check_alloc_page() succeeded!",
                                           "[Test 1] Page Init: PASS",
                                           "[Test 2] Best-Fit: PASS"):
                                if marker not in text:
                                    raise RuntimeError(f"missing {marker}; see {log}")
                            break
                        if process.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError(f"{optimization} stopped or timed out; see {log}")
                        time.sleep(0.05)
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
            print(f"PASS {optimization}: framework checks, edge cases, 512 bitmap-oracle operations")
    print(f"All Best-Fit checks passed; logs: {logs}")
    print("Boot, UART and DTB getters are fixtures; production entry/SBI/DTB parsing "
          "and full-kernel boot are not covered.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        raise SystemExit(f"FAIL: {error}")
