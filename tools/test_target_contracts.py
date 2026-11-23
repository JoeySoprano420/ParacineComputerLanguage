r"""Seeded target, grammar, behavioral, incremental and reproducibility checks.

python tools\test_target_contracts.py x64\Release\Paracine.exe
Use --cases 1000 --modules 128 --stress-rounds 20 for a longer campaign.
Every subprocess is bounded. Failures report the seed and complete source.
"""
import argparse
import json
import os
import pathlib
import platform
import random
import re
import subprocess
import tempfile
import zipfile

from test_diagnostics import validate


def bounded(minimum, maximum):
    def parse(value):
        number = int(value)
        if not minimum <= number <= maximum:
            raise argparse.ArgumentTypeError(f"Expected {minimum}..{maximum}")
        return number
    return parse


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--seed", type=bounded(0, 0xFFFFFFFF), default=0x5EED)
    parser.add_argument("--cases", type=bounded(1, 10000), default=16)
    parser.add_argument("--modules", type=bounded(1, 256), default=24)
    parser.add_argument("--stress-rounds", type=bounded(1, 1000), default=2)
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    randomizer = random.Random(arguments.seed)
    can_execute = platform.machine().lower() in {"amd64", "x86_64"} and platform.system() in {"Windows", "Linux"}
    passed = 0
    active_source = ""

    def require(condition, message):
        if not condition:
            raise AssertionError(message)

    with tempfile.TemporaryDirectory(prefix="paracine-target-contracts-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "main.pcn"

        def run(*options, expected=0, directory=root):
            nonlocal passed
            result = subprocess.run([str(compiler), *(str(option) for option in options)], cwd=directory,
                                    capture_output=True, encoding="utf-8", errors="strict", timeout=90)
            if expected is not None:
                require(result.returncode == expected,
                        f"Exit {result.returncode}, expected {expected}: {options}\n{result.stdout}\n{result.stderr}")
            passed += 1
            return result

        def write(text):
            nonlocal active_source
            active_source = text
            source.write_bytes(text.encode("utf-8"))

        def check(text, ok=True, options=()):
            write(text)
            result = run("check", source, "--json", *options, expected=0 if ok else 1)
            require(not result.stderr, "JSON diagnostics contaminated by stderr")
            report = json.loads(result.stdout)
            validate(report, ok)
            return report

        def differential(text, value):
            write(text)
            for level in (0, 3):
                if can_execute:
                    result = run("test", source, f"-O{level}")
                    require(f"oracle={value}" in result.stdout, f"Native/PCIR value is not independently expected {value}: {result.stdout}")
                else:
                    run("obj", source, f"-O{level}", "-o", root / "hostless.obj")

        try:
            registry_bytes = run("targets", "--json").stdout
            registry = json.loads(registry_bytes)
            require(registry["schema"] == "paracine.targets.v1", "Target schema changed")
            targets = {item["name"]: item for item in registry["targets"]}
            require(len(targets) == 6 and set(targets) == {"windows-x86_64", "linux-x86_64", "macos-x86_64", "windows-arm64", "linux-arm64", "macos-arm64"}, "Missing/duplicate target identity")
            features = {"abiClassification", "objectFormat", "relocations", "unwind", "debug", "tls", "exceptions", "systemLibraries", "startup", "dynamicLinking"}
            services = {"allocation", "deallocation", "panic", "stackOverflow", "text", "printing", "fileIO", "environment", "threads", "synchronization", "startup", "dynamicLibraries"}
            valid = "routine main() gives int\n    give 42\n"
            for name, target in targets.items():
                inventory = {item["name"]: item for item in target["features"]}
                require(set(inventory) == features and len(target["features"]) == len(features), "Target feature inventory drifted")
                require(target["pointerBytes"] == 8 and target["stackAlignment"] == 16 and target["endianness"] == "little", "Data layout mismatch")
                require(target["codegen"] == name.endswith("x86_64"), "Unimplemented emitter advertised")
                selected = json.loads(run("targets", "--target", name, "--json").stdout)
                require(selected["targets"] == [target], "Filtered target differs from registry")
                contract = json.loads(run("runtime-contract", "--target", name, "--json").stdout)
                require(contract["schema"] == "paracine.runtime.v1" and contract["target"] == name, "Runtime schema/identity drifted")
                require((contract["maxFileBytes"], contract["maxCommandBytes"], contract["maxStdinBytes"], contract["fileChunkBytes"]) == (67108864, 32760, 1048576, 65536), "Runtime limits drifted")
                require({item["name"] for item in contract["services"]} == services and len(contract["services"]) == len(services), "Runtime service inventory drifted")
                require(all(item["support"] in {"implemented", "partial", "unsupported"} and item["detail"] for item in [*inventory.values(), *contract["services"]]), "Invalid capability classification")
                check(valid, options=("--target", name))
                if not target["codegen"]:
                    require(all(item["support"] == "unsupported" for item in [*inventory.values(), *contract["services"]]), "ARM64 support overclaim")
                    output = root / "untouched.artifact"
                    output.write_bytes(b"preserve existing artifact")
                    for command in ("obj", "build", "asm", "abi"):
                        result = run(command, source, "--target", name, "-o", output, expected=1)
                        require("E0590" in result.stderr and output.read_bytes() == b"preserve existing artifact", "Missing architecture guard or output was modified")
                    run("obj", source, "--target", name, "-g", expected=1)

            require(run("targets", "--json").stdout == registry_bytes, "Registry is nondeterministic")
            for command in (("targets", "--target", "unknown"), ("targets", "--target"), ("targets", "--json", "--json"), ("runtime-contract", "--unknown")):
                result = run(*command, expected=1)
                require(re.search(r"E05(?:40|91)", result.stderr), "Invalid contract query lacks coded failure")

            for invalid, code in (
                ("routine main() gives int\n\tgive 42\n", "E0100"),
                ("routine main() gives int\n    give missing\n", None),
                ("routine main() gives int\n    give true\n", None),
                ("routine main() gives int\n    let = 1\n    give 0\n", None),
                ("routine one() gives int\n    give +\nroutine main() gives int\n    give *\n", None),
            ):
                report = check(invalid, False)
                require(report["diagnostics"] and all(item["severity"] == "error" for item in report["diagnostics"]), "Compile-fail case has no errors")
                if code:
                    require(report["diagnostics"][0]["code"] == code, "Compile-fail diagnostic changed")
            for ending in ("\n", "\r\n", "\r"):
                check(valid.replace("\n", ending))
            check("# preserved comment\n" + valid + "\n")

            corpus = json.loads(pathlib.Path(__file__).with_name("conformance_corpus.json").read_text(encoding="utf-8"))
            require(corpus["schema"] == "paracine.conformance-corpus.v1", "Corpus schema mismatch")
            for case in corpus["cases"]:
                differential(case["source"], case["expected"])
                if case["scalar"]:
                    for target in ("linux-x86_64", "macos-x86_64"):
                        run("obj", source, "--target", target, "-O3", "-o", root / "corpus.o")
                        run("verify-bin", root / "corpus.o")

            for index in range(arguments.cases):
                seed, factor, delta, limit = (randomizer.randint(0, 30), randomizer.randint(1, 5), randomizer.randint(-9, 9), randomizer.randint(1, 9))
                total = seed
                for step in range(1, limit + 1):
                    total += step * factor if step < 4 else delta
                expected = total & 127
                text = (f"routine work(seed: int) gives int\n    var result: int = seed\n    from step = 1 to {limit}\n"
                        f"        if step < 4\n            result += step * {factor}\n        else\n            result += {delta}\n"
                        f"    give result & 127\nroutine main() gives int\n    give work({seed})\n")
                differential(text, expected)
                for target in ("windows-x86_64", "linux-x86_64", "macos-x86_64"):
                    output = root / "fuzz.obj"
                    run("obj", source, "--target", target, "--jobs", "1", "-O3", "-o", output)
                    before = output.read_bytes()
                    run("obj", source, "--target", target, "--jobs", "8", "-O3", "-o", output)
                    require(output.read_bytes() == before, f"Worker scheduling changed {target} output (case {index})")
                    if index == 0:
                        for length in (0, 1, 7, 19, len(before) - 1):
                            malformed = root / "truncated.obj"
                            malformed.write_bytes(before[:length])
                            result = run("verify-bin", malformed, expected=1)
                            require(re.search(r"E0(?:542|910)", result.stderr), "Malformed binary lacks coded structural failure")
                mutated = bytearray(text.encode())
                position = randomizer.randrange(len(mutated))
                mutated[position:position + 1] = randomizer.choice((b"\t", b"+", b"\xff", b"\n", b"(", b""))
                source.write_bytes(mutated)
                active_source = repr(bytes(mutated))
                result = run("check", source, "--json", expected=None)
                require(result.returncode in (0, 1) and not result.stderr, "Frontend mutation crashed or polluted JSON")
                validate(json.loads(result.stdout), result.returncode == 0)

            projects = (root / "checkout-one", root / "checkout-two")
            main_text = "".join(f"import m{index}\n" for index in range(arguments.modules))
            main_text += "routine main() gives int\n    give (" + " + ".join(f"part{index}()" for index in range(arguments.modules)) + ") & 127\n"
            for project in projects:
                project.mkdir()
                (project / "main.pcn").write_bytes(main_text.encode())
                for index in range(arguments.modules):
                    (project / f"m{index}.pcn").write_bytes(f"module m{index}\npublic routine part{index}() gives int\n    give {index}\n".encode())
            active_source = main_text
            for target in ("windows-x86_64", "linux-x86_64", "macos-x86_64"):
                objects = []
                for index, project in enumerate(projects):
                    output = project / "large.obj"
                    run("obj", project / "main.pcn", "--target", target, "-O0", "--jobs", str(1 if index == 0 else 8), "-o", output)
                    objects.append(output.read_bytes())
                require(objects[0] == objects[1], f"Checkout paths changed nondebug {target} output")

            project = projects[0]
            dependency = project / "m0.pcn"
            timestamp = dependency.stat().st_mtime_ns
            output = project / "incremental.obj"
            options = ["obj", project / "main.pcn", "-O0", "-o", output]
            if platform.system() == "Windows":
                options += ["--cache-dir", project / "cache"]
            run(*options)
            original = output.read_bytes()
            unchanged = run(*options)
            require(output.read_bytes() == original, "Unchanged rebuild changed artifact bytes")
            if platform.system() == "Windows":
                require("cache hit" in unchanged.stdout, "Unchanged build did not use persistent cache")
            dependency.write_bytes(dependency.read_bytes().replace(b"give 0", b"give 1"))
            os.utime(dependency, ns=(dependency.stat().st_atime_ns, timestamp))
            run(*options)
            require(output.read_bytes() != original, "Same-size/timestamp imported edit reused stale native artifact")
            if can_execute:
                expected = (sum(range(arguments.modules)) + 1) & 127
                result = run("test", project / "main.pcn", "-O0")
                require(f"oracle={expected}" in result.stdout, "Incremental import changed artifact but not behavior")
            stable = output.read_bytes()
            if platform.system() == "Windows":
                envelopes = list((project / "cache").glob("*.pcnc"))
                require(len(envelopes) == 2, "Imported edit did not create an isolated cache identity")
                for envelope in envelopes:
                    envelope.write_bytes(b"corrupt cache envelope")
                repaired = run(*options)
                require("cache miss" in repaired.stdout and output.read_bytes() == stable, "Corrupt cache was trusted or repair changed behavior")
            for _ in range(arguments.stress_rounds):
                run(*options)
                require(output.read_bytes() == stable, "Repeated compilation drifted")

            write(valid)
            bundle = root / "distribution.zip"
            run("package", source, "-O0", "-o", bundle)
            original = bundle.read_bytes()
            run("package", source, "-O0", "-o", bundle)
            require(bundle.read_bytes() == original, "Repeated distribution bytes changed")
            with zipfile.ZipFile(bundle) as archive:
                expected = {"manifest.json", "README.txt", "src/main.pcn", "obj/program.obj", "bin/program.exe"}
                require(set(archive.namelist()) == expected and len(archive.infolist()) == len(expected), "Unexpected or duplicate archive members")
                require(archive.testzip() is None and archive.read("src/main.pcn") == valid.encode(), "Independent ZIP CRC/source check failed")
                require(all(info.date_time == (1980, 1, 1, 0, 0, 0) and info.compress_type == zipfile.ZIP_STORED and info.flag_bits & 0x800 for info in archive.infolist()), "ZIP reproducibility/encoding contract drifted")
                manifest = json.loads(archive.read("manifest.json"))
                require(manifest["target"] == "windows-x86_64-pe32plus", "Archive manifest target mismatch")
                for name in ("obj/program.obj", "bin/program.exe"):
                    artifact = root / pathlib.PurePosixPath(name).name
                    artifact.write_bytes(archive.read(name))
                    run("verify-bin", artifact)
        except Exception:
            print(f"FAIL seed={arguments.seed}; active source follows:\n{active_source}", flush=True)
            raise

    print(f"PASS {passed} target/runtime/conformance checks; seed={arguments.seed}; generated cases={arguments.cases}; modules={arguments.modules}; stress rounds={arguments.stress_rounds}")
    if not can_execute:
        print("Native/PCIR execution not performed on this host; only frontend/artifact checks ran.")


if __name__ == "__main__":
    main()
