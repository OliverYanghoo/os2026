#!/usr/bin/env python3
"""Isolated RISC-V page_init checks; the full lab2 kernel is not built here."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
CASES = [
    "default 128 MiB and supplied page-init test",
    "smaller 8 MiB and supplied page-init test",
    "256 MiB report clamped to framework limit",
    "unaligned memory end rounded down",
    "zero memory size rejected",
    "zero memory base rejected",
    "64-bit memory-range overflow rejected",
    "memory below DRAM rejected",
    "memory range excluding kernel rejected",
    "memory too short for kernel rejected",
    "insufficient descriptor space rejected before writes",
    "descriptors fit but no full free page: no callback",
    "non-default DRAM start leaves preceding pages reserved",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="qemu-system-riscv64")
    parser.add_argument("--gcc-prefix", default="riscv64-unknown-elf-")
    args = parser.parse_args()
    logs = ROOT / "obj" / "check-page-init"
    logs.mkdir(parents=True, exist_ok=True)
    flags = ["-mcmodel=medany", "-std=gnu99", "-Wall", "-Werror", "-Wno-unused",
             "-fno-builtin", "-nostdinc", "-fno-stack-protector", "-O2", "-g",
             "-ffunction-sections", "-fdata-sections", "-Ilibs", "-Ikern/mm",
             "-Ikern/driver", "-Ikern/debug", "-Itools"]
    with (logs / "build.log").open("w") as buildlog:
        def run(command):
            buildlog.write("$ " + shlex.join(map(str, command)) + "\n")
            buildlog.flush()
            subprocess.run(command, cwd=ROOT, stdout=buildlog, stderr=subprocess.STDOUT,
                           check=True, timeout=30)

        with tempfile.TemporaryDirectory(prefix="lab2-page-init-") as tmp:
            directory = Path(tmp)
            common = []
            sources = ["tests/page_init_boot.S", "kern/libs/stdio.c", "libs/printfmt.c",
                       "libs/string.c", "tools/test_lab2.c"]
            for index, source in enumerate(sources):
                obj = directory / f"common-{index}.o"
                run([args.gcc_prefix + "gcc", *flags, "-c", source, "-o", str(obj)])
                common.append(str(obj))
            for index, description in enumerate(CASES):
                obj = directory / "case.o"
                elf, image = directory / "kernel", directory / "ucore.img"
                run([args.gcc_prefix + "gcc", *flags, f"-DCASE_ID={index}", "-c",
                     "tests/page_init_check.c", "-o", str(obj)])
                run([args.gcc_prefix + "ld", "-m", "elf64lriscv", "-nostdlib",
                     "--gc-sections", "-T", "tools/kernel.ld", "-o", str(elf),
                     *common, str(obj)])
                run([args.gcc_prefix + "objcopy", "-O", "binary", str(elf), str(image)])
                log = logs / f"case-{index:02d}.log"
                with log.open("w") as output:
                    process = subprocess.Popen(shlex.split(args.qemu) + [
                        "-machine", "virt", "-nographic", "-bios", "default",
                        "-kernel", str(image)], cwd=ROOT, stdin=subprocess.DEVNULL,
                        stdout=output, stderr=subprocess.STDOUT)
                    try:
                        deadline = time.monotonic() + 10
                        while True:
                            text = log.read_text(errors="replace")
                            if f"PAGE_INIT_CASE_{index}_FAIL" in text:
                                raise RuntimeError(f"case {index} failed; see {log}")
                            if f"PAGE_INIT_CASE_{index}_PASS" in text:
                                if index in (0, 1, 3, 12) and "[Test 1] Page Init: PASS" not in text:
                                    raise RuntimeError(f"supplied page-init test failed; see {log}")
                                break
                            if process.poll() is not None or time.monotonic() >= deadline:
                                raise RuntimeError(f"case {index} stopped or timed out; see {log}")
                            time.sleep(0.05)
                    finally:
                        if process.poll() is None:
                            process.terminate()
                            try:
                                process.wait(timeout=3)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait()
                print(f"PASS {index:02d}: {description}")
    print(f"All {len(CASES)} isolated page_init checks passed; logs: {logs}")
    print("Fixtures supply DTB getter results and allocator callbacks; full kernel, "
          "DTB parsing and other allocators are not tested.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        raise SystemExit(f"FAIL: {error}")
