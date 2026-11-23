r"""Measure generated Windows x64 code, not compiler execution time.

    python tools\benchmark_optimizer.py x64\Release\Paracine.exe

Results are workload-specific medians/MADs, not portable peak-speed guarantees.
This does not collect branch profiles or feed PGO data into the compiler.
"""
import pathlib
import statistics
import sys
import tempfile
import time

from test_cpp_export import run


def invariant_kernel(rounds):
    expected = 91 * 256 * (rounds // 7)
    return f"""routine work(seed: int) gives int
    var sum: int = 0
    var index: int = 0
    while index < 256
        sum += seed * seed
        index += 1
    give sum
routine main() gives int
    var total: int = 0
    var index: int = 0
    while index < {rounds}
        total += work(index % 7)
        index += 1
    if total == {expected}
        give 0
    give 1
"""


def array_kernel(rounds):
    values = ", ".join(["seed"] * 128)
    expected = 21 * 128 * (rounds // 7)
    return f"""routine work(seed: int) gives int
    let values: array<int,128> = [{values}]
    var sum: int = 0
    for value in values
        sum += value
    give sum
routine main() gives int
    var total: int = 0
    var index: int = 0
    while index < {rounds}
        total += work(index % 7)
        index += 1
    if total == {expected}
        give 0
    give 1
"""


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="paracine-optimizer-bench-") as temp:
        root = pathlib.Path(temp)
        for name, source in (("invariant-loop", invariant_kernel), ("array-reduction", array_kernel)):
            check = root / (name + "-check.pcn")
            check.write_bytes(source(7).encode("utf-8"))
            path = root / (name + ".pcn")
            path.write_bytes(source(70000).encode("utf-8"))
            executables = []
            for level in (0, 2, 3):
                flags = [f"-O{level}", "--jobs", "1"]
                result = run([str(compiler), "test", str(check), *flags], root)
                if b"PCIR oracle=0" not in result.stdout:
                    raise RuntimeError(name + ": unexpected oracle/native checksum")
                target = root / (name + f"-O{level}.exe")
                run([str(compiler), "build", str(path), *flags, "-o", str(target)], root)
                run([str(target)], root)
                executables.append((level, target))
            samples = {level: [] for level, _ in executables}
            # Rotate trial order to reduce systematic warm-up and ordering bias.
            for trial in range(7):
                shift = trial % len(executables)
                for level, target in executables[shift:] + executables[:shift]:
                    start = time.perf_counter()
                    run([str(target)], root)
                    samples[level].append((time.perf_counter() - start) * 1000)
            baseline = statistics.median(samples[2])
            print(name + " (includes process startup; every run checks its checksum)")
            print("level median_ms mad_ms speedup_vs_O2")
            for level, _ in executables:
                median = statistics.median(samples[level])
                mad = statistics.median(abs(value - median) for value in samples[level])
                print(f"O{level} {median:.3f} {mad:.3f} {baseline / median:.3f}x")


if __name__ == "__main__":
    main()
