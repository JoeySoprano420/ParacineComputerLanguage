r"""Artifact-level CodeView/PDB, DWARF, unwind and ABI regression checks.

python tools\test_native_debug.py x64\Release\Paracine.exe
Requires existing LLVM tools on PATH and pyelftools (verification only).
"""
import hashlib
import io
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from elftools.elf.elffile import ELFFile
from elftools.dwarf.locationlists import LocationEntry, LocationParser


SOURCE = (
    "routine work(value: int) gives int\n"
    "    var result: int = value + 1\n"
    "    give result\n"
    "routine main() gives int\n"
    "    var answer: int = work(41)\n"
    "    give answer\n"
)


def run(command, root, expected=0):
    resolved = [shutil.which(str(command[0])) or str(command[0]), *(str(x) for x in command[1:])]
    result = subprocess.run(resolved, cwd=root, capture_output=True, encoding="utf-8", errors="strict", timeout=120)
    if result.returncode != expected:
        raise RuntimeError(f"Expected exit {expected}, got {result.returncode}: {command}\n{result.stdout}\n{result.stderr}")
    return result.stdout + result.stderr


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def coff_sections(data):
    count = struct.unpack_from("<H", data, 2)[0]
    result = {}
    for index in range(count):
        offset = 20 + index * 40
        name = data[offset:offset + 8].rstrip(b"\0").decode()
        size, pointer = struct.unpack_from("<II", data, offset + 16)
        result[name] = data[pointer:pointer + size]
    return result


def dwarf_check(path, source, optimized=False):
    elf = ELFFile(io.BytesIO(path.read_bytes()))
    dwarf = elf.get_dwarf_info()
    units = list(dwarf.iter_CUs())
    require(len(units) == 1, "Expected one independently readable DWARF compilation unit")
    unit = units[0]
    functions = {die.attributes["DW_AT_name"].value.decode(): die for die in unit.iter_DIEs() if die.tag == "DW_TAG_subprogram"}
    require(set(functions) == {"main", "work"}, "DWARF function identity mismatch")
    states = [entry.state for entry in dwarf.line_program_for_CU(unit).get_entries() if entry.state and not entry.state.end_sequence]
    expected_lines = {2, 3, 5, 6} if not optimized else {2, 3, 6}
    require(expected_lines.issubset({state.line for state in states}), f"Missing actual machine instruction line mappings ({optimized=}): {[state.line for state in states]}")
    require(all(state.column > 0 for state in states), "Missing source columns")
    frame_table = dwarf.CFI_entries()
    require(len(frame_table) == 3, "Expected CIE and one frame description per emitted routine")
    for frame in frame_table[1:]:
        table = frame.get_decoded().table
        require(any(row["cfa"].reg == 6 and row["cfa"].offset == 16 for row in table), "Body CFA is not RBP+16")
        require(any(row["cfa"].reg == 7 and row["cfa"].offset == 8 for row in table), "Return CFA does not restore RSP+8")
    variables = {die.attributes["DW_AT_name"].value.decode(): die for die in unit.iter_DIEs() if die.tag in {"DW_TAG_variable", "DW_TAG_formal_parameter"}}
    require(set(variables) == {"value", "result", "answer"}, "Source variables lost their identities")
    for name, variable in variables.items():
        referenced = variable.get_DIE_from_attribute("DW_AT_type")
        require(referenced.attributes["DW_AT_name"].value == b"int", "Variable has incorrect source type")
        if optimized:
            require("DW_AT_location" not in variable.attributes, "Optimized variable incorrectly advertises a stale frame slot")
        else:
            require("DW_AT_location" in variable.attributes, f"Unoptimized variable {name} has no location")
            locations = LocationParser(dwarf.location_lists()).parse_from_attribute(variable.attributes["DW_AT_location"], unit["version"], die=variable)
            entries = [entry for entry in locations if isinstance(entry, LocationEntry)]
            require(entries and all(entry.loc_expr[0] == 0x91 for entry in entries), "Variable location does not use a bounded frame-base expression")
            if name != "value":
                declaration = variable.attributes["DW_AT_decl_line"].value
                for entry in entries:
                    covered = [state for state in states if entry.begin_offset <= state.address < entry.end_offset]
                    require(all(state.line > declaration for state in covered), "Local visible before its initializer completes")
    return elf, dwarf


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    for tool in ("llvm-readobj", "llvm-pdbutil", "llvm-objdump", "lld-link", "ld.lld"):
        require(shutil.which(tool), "Required LLVM verification tool missing: " + tool)
    cases = 0
    with tempfile.TemporaryDirectory(prefix="paracine native debug ") as temporary:
        root = pathlib.Path(temporary)
        source = root / "source.pcn"
        source.write_bytes(SOURCE.encode())
        obj, exe, elf_obj, elf_exe = (root / name for name in ("debug.obj", "debug.exe", "debug.o", "debug.elf"))
        run([compiler, "obj", source, "-g", "-O0", "-o", obj], root)
        decoded = run(["llvm-readobj", "--codeview", "--unwind", obj], root)
        for item in ("S_GPROC32", "S_LOCAL", "S_DEFRANGE_FRAMEPOINTER_REL", "work", "answer", "result", "value", "ALLOC_SMALL", "PUSH_NONVOL"):
            require(item in decoded, "Independent CodeView/unwind decoder did not find " + item)
        require("error:" not in decoded.lower(), "LLVM rejected CodeView records")
        cases += 1
        run([compiler, "build", source, "-g", "-O0", "-o", exe], root)
        run([compiler, "verify-bin", exe, obj], root)
        if os.name == "nt":
            run([exe], root, 42)
            from native_debug_windows import verify_debugger
            verify_debugger(exe, source)
            cases += 1
        pdb = exe.with_suffix(".pdb")
        decoded_pdb = run(["llvm-pdbutil", "dump", "--summary", "--symbols", "--types", "--files", "-l", pdb], root)
        for item in ("S_GPROC32", "S_LOCAL", "answer", "result", "value", "source.pcn", "LF_PROCEDURE"):
            require(item in decoded_pdb, "Linked PDB missing " + item)
        require("error:" not in decoded_pdb.lower(), "Independent PDB reader rejected artifact")
        cases += 1
        optimized_exe = root / "optimized.exe"
        run([compiler, "build", source, "-g", "-O3", "-o", optimized_exe], root)
        if os.name == "nt":
            verify_debugger(optimized_exe, source, optimized=True)
        optimized_pdb = run(["llvm-pdbutil", "dump", "--symbols", optimized_exe.with_suffix(".pdb")], root)
        require("optimized" in optimized_pdb.lower(), "PDB does not mark unproven optimized locals unavailable")
        cases += 1
        for level in (0, 3):
            run([compiler, "obj", source, "--target", "linux-x86_64", "-g", f"-O{level}", "-o", elf_obj], root)
            dwarf_check(elf_obj, source, optimized=level != 0)
            run([compiler, "verify-bin", elf_obj], root)
            run([compiler, "build", source, "--target", "linux-x86_64", "-g", f"-O{level}", "-o", elf_exe], root)
            dwarf_check(elf_exe, source, optimized=level != 0)
            frames = run(["llvm-objdump", "--dwarf=frames", elf_exe], root)
            require("CFA=RBP+16" in frames or "DW_CFA_def_cfa_register" in frames, "Independent LLVM CFI decoding failed")
            run([compiler, "verify-bin", elf_exe], root)
            if os.name != "nt":
                elf_exe.chmod(0o700)
                run([elf_exe], root, 42)
            cases += 2
        run([compiler, "obj", source, "--target", "linux-x86_64", "-g", "-O0", "-o", elf_obj], root)
        linked = root / "linked.elf"
        run(["ld.lld", "-e", "main", "-o", linked, elf_obj], root)
        dwarf_check(linked, source)
        cases += 1
        manifest = json.loads(run([compiler, "abi", source, "--json", "-O0"], root))
        require(manifest["schema"] == "paracine.abi.v1" and manifest["crossCompilerStable"] is False, "ABI manifest overstates compatibility")
        layouts = {item["name"]: item for item in manifest["types"]}
        require((layouts["text"]["size"], layouts["text"]["alignment"]) == (16, 8), "Text ABI manifest disagrees with legalization")
        require([member["offset"] for member in layouts["text"]["fields"]] == [0, 8], "Text ABI field offsets changed")
        elf = ELFFile(io.BytesIO(elf_obj.read_bytes()))
        embedded = json.loads(elf.get_section_by_name(".pcnabi").data())
        require("sysv-x64-scalar" in embedded["contract"], "ELF manifest reports Windows calling convention")
        cases += 1
        for target in ("windows-x86_64", "linux-x86_64"):
            a, b = root / "serial.bin", root / "parallel.bin"
            run([compiler, "obj", source, "-g", "-O0", "--target", target, "--jobs", "1", "-o", a], root)
            run([compiler, "obj", source, "-g", "-O0", "--target", target, "--jobs", "8", "-o", b], root)
            require(a.read_bytes() == b.read_bytes(), "Debug object changes with parallelism")
            cases += 1
        failure = run([compiler, "obj", source, "-g", "--target", "macos-x86_64", "-o", root / "unsupported.o"], root, 1)
        require("E0580" in failure and not (root / "unsupported.o").exists(), "Unsupported debug target failed open")
        failure = run([compiler, "build", source, "-g", "--cache", "-o", root / "cached.exe"], root, 1)
        require("E0580" in failure, "Multi-artifact debug build entered single-artifact cache")
        cases += 2
        run([compiler, "link", source, "-g", "-O0", "-o", root / "separate.exe"], root)
        separate_pdb = run(["llvm-pdbutil", "dump", "--symbols", root / "separate.pdb"], root)
        require("S_LOCAL" in separate_pdb and "answer" in separate_pdb, "Separate linking discarded source locals")
        if os.name == "nt":
            verify_debugger(root / "separate.exe", source)
        cases += 1
        source.write_bytes(SOURCE.replace("\n", "\r\n").encode())
        run([compiler, "obj", source, "-g", "-O0", "-o", obj], root)
        debug_symbols = coff_sections(obj.read_bytes())[".debug$S"]
        position, source_checksum = 4, None
        while position < len(debug_symbols):
            kind, size = struct.unpack_from("<II", debug_symbols, position)
            if kind == 0xF4:
                length, algorithm = struct.unpack_from("<BB", debug_symbols, position + 12)
                require((length, algorithm) == (32, 3), "CodeView source checksum is not SHA-256")
                source_checksum = debug_symbols[position + 14:position + 14 + length]
            position = (position + 8 + size + 3) & ~3
        require(source_checksum == hashlib.sha256(source.read_bytes()).digest(), "PDB source checksum hashes normalized rather than actual source bytes")
        cases += 1
        source.write_bytes(SOURCE.encode())
        first = root / "first.pcn"
        second = root / "second.pcn"
        first.write_text("record Shared\n    value: int\nroutine main() gives int\n    give 42\n", encoding="utf-8")
        second.write_text("record Shared\n    value: long\nroutine helper() gives int\n    give 0\n", encoding="utf-8")
        run([compiler, "obj", first, "-o", root / "first.obj"], root)
        run([compiler, "obj", second, "-o", root / "second.obj"], root)
        mismatch = run(["lld-link", "/nodefaultlib", "/subsystem:console", "/entry:main", "/out:" + str(root / "mismatch.exe"), root / "first.obj", root / "second.obj"], root, 1)
        require("mismatch" in mismatch.lower() and "PCNType_" in mismatch, "Standard linker accepted incompatible shared nominal layouts")
        second.write_text("record Shared\n    value: int\nroutine helper() gives int\n    give 0\n", encoding="utf-8")
        run([compiler, "obj", second, "-o", root / "second.obj"], root)
        run(["lld-link", "/nodefaultlib", "/subsystem:console", "/entry:main", "/out:" + str(root / "matched.exe"), root / "first.obj", root / "second.obj"], root)
        cases += 2
        cache = root / "cache"
        cache_args = [compiler, "obj", source, "-g", "-O0", "--cache-dir", cache, "-o", obj]
        require("cache miss" in run(cache_args, root), "First debug object build did not miss cache")
        require("cache hit" in run(cache_args, root), "Identical debug object build did not hit cache")
        copied = root / "relocated"
        copied.mkdir()
        copied_source = copied / "source.pcn"
        copied_source.write_bytes(source.read_bytes())
        require("cache miss" in run([compiler, "obj", copied_source, "-g", "-O0", "--cache-dir", cache, "-o", root / "relocated.obj"], root), "Debug cache reused absolute source locations from another checkout")
        relocated = run(["llvm-readobj", "--codeview", root / "relocated.obj"], root)
        require("relocated" in relocated, "Relocated debug object retained the old source identity")
        cases += 2
        helper = root / "math.pcn"
        helper.write_text("module math\npublic routine increment(value: int) gives int\n    give value + 1\n", encoding="utf-8")
        source.write_text("import math as M\nroutine main() gives int\n    give M.increment(41)\n", encoding="utf-8")
        run([compiler, "obj", source, "-g", "-O0", "-o", obj], root)
        imported = run(["llvm-readobj", "--codeview", obj], root)
        require("math.pcn" in imported and "source.pcn" in imported, "Debug file table lost imported source identities")
        run([compiler, "obj", source, "--target", "linux-x86_64", "-g", "-O0", "-o", elf_obj], root)
        units = ELFFile(io.BytesIO(elf_obj.read_bytes())).get_dwarf_info()
        unit = next(units.iter_CUs())
        require(len(units.line_program_for_CU(unit)["file_entry"]) == 2, "DWARF import source inventory is incomplete")
        cases += 2
        source.write_text("record Pair\n    flag: bool\n    value: long\nroutine main() gives long\n    let pair: Pair = Pair{flag: true, value: 42}\n    let action = routine() gives long => pair.value\n    give action()\n", encoding="utf-8")
        run([compiler, "obj", source, "-g", "-O0", "-o", obj], root)
        rich = run(["llvm-readobj", "--codeview", obj], root)
        require("LF_STRUCTURE" in rich and "LF_MEMBER" in rich and "Pair" in rich, "Rich storage type layout missing from CodeView")
        require("IsOptimizedOut" in rich, "Unproven rich locations should be unavailable")
        cases += 1
        source.write_text("routine crash(value: int) gives int\n    give 42 / value\nroutine main() gives int\n    give crash(0)\n", encoding="utf-8")
        run([compiler, "build", source, "-g", "-O0", "-o", root / "crash.exe"], root)
        if os.name == "nt":
            verify_debugger(root / "crash.exe", source, crash=True)
            cases += 1
    print(f"Native debug/ABI artifact checks: {cases} passed")


if __name__ == "__main__":
    main()
