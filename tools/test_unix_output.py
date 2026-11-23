r"""Validate cross-emitted ELF64/Mach-O64 objects with LLVM and both linkers.

    python tools\test_unix_output.py x64\Release\Paracine.exe

Requires clang, llvm-readobj, ld.lld and ld64.lld on PATH. Does not execute
Linux/macOS images on Windows or claim complete platform runtime support.
"""
import pathlib
import shutil
import struct
import sys
import tempfile

from test_cpp_export import run as run_command


def run(command, directory, expected=0):
    executable = shutil.which(command[0])
    if executable is None:
        raise RuntimeError("Required executable is unavailable: " + command[0])
    return run_command([executable, *command[1:]], directory, expected)


LIBRARY = """foreign c routine external(value: int) gives int
routine relay(value: int) gives int
    give external(value) + 1
"""
PROVIDER = "int external(int value) { return value * 2; }\n"
ENTRY = """routine work(value: int) gives int
    var sum: int = 0
    from index = 1 to 6
        sum += value * index
    give sum
routine main() gives int
    if work(2) == 42
        give 42
    give 0
"""


def validate_elf(data, executable=False):
    if data[:8] != b"\x7fELF\x02\x01\x01\x00":
        raise RuntimeError("Invalid ELF64 identification")
    kind, machine, version = struct.unpack_from("<HHI", data, 16)
    if (kind, machine, version) != (2 if executable else 1, 62, 1):
        raise RuntimeError("Invalid ELF target or file type")
    if executable:
        entry, table = struct.unpack_from("<QQ", data, 24)
        width, count = struct.unpack_from("<HH", data, 54)
        executable_entry = False
        nonexec_stack = False
        for index in range(count):
            offset = table + index * width
            kind, flags, fileoff, address, _, filesz, memsz, align = struct.unpack_from("<IIQQQQQQ", data, offset)
            if fileoff + filesz > len(data) or filesz > memsz:
                raise RuntimeError("ELF segment extends beyond file or memory")
            if kind == 1:
                if fileoff % align != address % align or flags & 3 == 3:
                    raise RuntimeError("Invalid ELF alignment or writable executable segment")
                executable_entry |= bool(flags & 1 and address <= entry < address + memsz)
            if kind == 0x6474E551:
                nonexec_stack = not flags & 1
        if not executable_entry or not nonexec_stack:
            raise RuntimeError("ELF entry or nonexecutable-stack contract missing")
        return
    table = struct.unpack_from("<Q", data, 40)[0]
    width, count, names = struct.unpack_from("<HHH", data, 58)
    if width != 64 or names >= count or table + width * count > len(data):
        raise RuntimeError("Invalid ELF section graph")
    for index in range(count):
        kind = struct.unpack_from("<I", data, table + width * index + 4)[0]
        offset, size, link = struct.unpack_from("<QQI", data, table + width * index + 24)
        if offset + size > len(data) or link >= count:
            raise RuntimeError("Invalid ELF section extent/link")
        if kind == 4:
            for relocation in range(offset, offset + size, 24):
                address, info, addend = struct.unpack_from("<QQq", data, relocation)
                if info & 0xFFFFFFFF != 4 or addend != -4:
                    raise RuntimeError("Invalid ELF call relocation")


def validate_macho(data):
    magic, cpu, subtype, kind, count, size, flags, reserved = struct.unpack_from("<8I", data)
    if (magic, cpu, subtype, kind) != (0xFEEDFACF, 0x01000007, 3, 1):
        raise RuntimeError("Invalid Mach-O target/file type")
    cursor = 32
    for _ in range(count):
        command, width = struct.unpack_from("<II", data, cursor)
        if width < 8 or cursor + width > 32 + size:
            raise RuntimeError("Invalid Mach-O load-command graph")
        if command == 0x19:
            sections = struct.unpack_from("<I", data, cursor + 64)[0]
            for section in range(sections):
                record = cursor + 72 + section * 80
                address, length, offset, alignment, relocoff, reloccount = struct.unpack_from("<QQIIII", data, record + 32)
                if offset + length > len(data) or relocoff + reloccount * 8 > len(data):
                    raise RuntimeError("Invalid Mach-O section/relocation extent")
                for relocation in range(relocoff, relocoff + reloccount * 8, 8):
                    address, bits = struct.unpack_from("<II", data, relocation)
                    if bits >> 24 != 0x2D:
                        raise RuntimeError("Invalid Mach-O branch relocation encoding")
        cursor += width
    if cursor != 32 + size:
        raise RuntimeError("Mach-O command size mismatch")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    compiler = pathlib.Path(sys.argv[1]).resolve()
    passed = 0
    with tempfile.TemporaryDirectory(prefix="paracine-unix-output-") as temp:
        root = pathlib.Path(temp)
        source = root / "library.pcn"
        source.write_bytes(LIBRARY.encode("utf-8"))
        provider = root / "provider.c"
        provider.write_bytes(PROVIDER.encode("utf-8"))
        for target, command, triple in (("linux-x86_64", "elf", "x86_64-unknown-linux-gnu"),
                                         ("macos-x86_64", "macho", "x86_64-apple-macos10.15")):
            for level in (0, 3):
                obj = root / f"{target}-O{level}.o"
                run([str(compiler), command, str(source), f"-O{level}", "--jobs", "1", "-o", str(obj)], root)
                data = obj.read_bytes()
                (validate_elf if command == "elf" else validate_macho)(data)
                run([str(compiler), "verify-bin", str(obj)], root)
                read = run(["llvm-readobj", "--file-headers", "--sections", "--symbols", "--relocations", str(obj)], root)
                if b"external" not in read.stdout or b"relay" not in read.stdout or read.stderr:
                    raise RuntimeError("LLVM did not validate expected symbols/relocations")
                other = root / "parallel.o"
                run([str(compiler), "obj", str(source), "--target", target, f"-O{level}", "--jobs", "4", "-o", str(other)], root)
                if other.read_bytes() != data:
                    raise RuntimeError("Unix output changed with worker count")
                damaged = bytearray(data)
                if command == "elf":
                    struct.pack_into("<Q", damaged, 40, len(data) + 1)
                else:
                    struct.pack_into("<I", damaged, 36, 7)
                invalid = root / "invalid.o"
                invalid.write_bytes(damaged)
                failure = run([str(compiler), "verify-bin", str(invalid)], root, 1)
                if b"E0542" not in failure.stderr:
                    raise RuntimeError("Malformed Unix table was not rejected with E0542")
                cobj = root / "provider.o"
                run(["clang", "-target", triple, "-ffreestanding", "-c", str(provider), "-o", str(cobj)], root)
                linked = root / "linked-image"
                if command == "elf":
                    run(["ld.lld", "-e", "relay", str(obj), str(cobj), "-o", str(linked)], root)
                else:
                    run(["ld64.lld", "-arch", "x86_64", "-platform_version", "macos", "10.15", "10.15",
                         "-e", "_relay", str(obj), str(cobj), "-o", str(linked)], root)
                run(["llvm-readobj", "--file-headers", str(linked)], root)
                passed += 1
                print(f"PASS {target} O{level}: object graph, external C relocation, native linker, deterministic workers")
        entry = root / "entry.pcn"
        entry.write_bytes(ENTRY.encode("utf-8"))
        for level in (0, 3):
            image = root / "program.elf"
            run([str(compiler), "build", str(entry), "--target", "linux-x86_64", f"-O{level}", "-o", str(image)], root)
            validate_elf(image.read_bytes(), True)
            alias = root / "alias.elf"
            run([str(compiler), "emit", "exe", str(entry), "--target", "linux-x86_64", f"-O{level}", "-o", str(alias)], root)
            if alias.read_bytes() != image.read_bytes():
                raise RuntimeError("Generic executable export differs from Linux build output")
            run([str(compiler), "verify-bin", str(image)], root)
            run(["llvm-readobj", "--file-headers", "--program-headers", str(image)], root)
            damaged = bytearray(image.read_bytes())
            struct.pack_into("<Q", damaged, 24, 0)
            invalid = root / "invalid.elf"
            invalid.write_bytes(damaged)
            failure = run([str(compiler), "verify-bin", str(invalid)], root, 1)
            if b"E0542" not in failure.stderr:
                raise RuntimeError("Unmapped ELF entry was not rejected")
            passed += 1
        unsupported = run([str(compiler), "build", str(entry), "--target", "macos-x86_64"], root, 1)
        if b"E0541" not in unsupported.stderr:
            raise RuntimeError("Mach-O executable support was silently assumed")
        passed += 1
    print(f"PASS {passed} cross-output/linker cases; target OS execution not performed")


if __name__ == "__main__":
    main()
