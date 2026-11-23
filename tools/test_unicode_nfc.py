r"""Compare source normalization to Unicode 15.1 NormalizationTest.txt through the AST CLI.

Usage: python tools\test_unicode_nfc.py path\to\Paracine.exe path\to\NormalizationTest.txt
"""
import pathlib
import subprocess
import sys
import tempfile

if len(sys.argv) != 3:
    raise SystemExit(__doc__)
exe = pathlib.Path(sys.argv[1]).resolve()
test_file = pathlib.Path(sys.argv[2])
lines = test_file.read_text(encoding="utf-8").splitlines()
if not lines or lines[0].strip() != "# NormalizationTest-15.1.0.txt":
    raise SystemExit("Expected Unicode 15.1.0 NormalizationTest.txt")
cases = []
for line in lines:
    body = line.split("#", 1)[0].strip()
    if not body or body.startswith("@"):
        continue
    fields = body.split(";")
    if len(fields) < 5:
        raise SystemExit("Malformed test row: " + line)
    columns = ["".join(chr(int(code, 16)) for code in field.split()) for field in fields[:5]]
    for i, value in enumerate(columns):
        expected = columns[1] if i < 3 else columns[3]
        if any(c in value or c in expected for c in "\r\n\x00"):
            continue
        cases.append((value, expected))
if not cases:
    raise SystemExit("No Unicode conformance cases found")

def escape(value):
    return value.replace("\\", "\\\\").replace('"', '\\"')

with tempfile.TemporaryDirectory(prefix="paracine-nfc-") as temp:
    path = pathlib.Path(temp) / "test.pcn"
    for start in range(0, len(cases), 200):
        batch = cases[start:start + 200]
        source = "routine main() gives int\n" + "".join(
            f'    let v{i} = "{escape(value)}"\n' for i, (value, _) in enumerate(batch)
        ) + "    give 0\n"
        path.write_bytes(source.encode("utf-8"))
        process = subprocess.run([str(exe), "ast", str(path), "-O0"], capture_output=True)
        if process.returncode:
            raise SystemExit("Compiler failed at case " + str(start) + ": " + process.stderr.decode("utf-8", "replace"))
        lines = process.stdout.replace(b"\r\n", b"\n").decode("utf-8").split("\n")
        actual = [line for line in lines if line.startswith("    let v")]
        expected = [f'    let v{i} = "{value}"' for i, (_, value) in enumerate(batch)]
        if actual != expected:
            for i, (got, want) in enumerate(zip(actual, expected)):
                if got != want:
                    raise SystemExit(f"NFC mismatch at case {start + i}: {got!r} != {want!r}")
            raise SystemExit(f"Wrong result count at case {start}: {len(actual)} != {len(expected)}")
print(f"PASS: {len(cases)} Unicode normalization conformance cases")
