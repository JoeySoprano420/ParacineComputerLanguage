r"""Validate diagnostic schema, provenance, warning controls and terminal output.

    python tools\test_diagnostics.py x64\Release\Paracine.exe
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile


def run(command, directory, expected=0, environment=None):
    result = subprocess.run(command, cwd=directory, capture_output=True, encoding="utf-8", errors="strict", timeout=90, env=environment)
    if result.returncode != expected:
        raise RuntimeError(f"Unexpected exit {result.returncode}, expected {expected}: {command}\n{result.stdout}\n{result.stderr}")
    return result


def validate(report, expected_ok):
    if (report["schema"], report["version"], report["coordinates"], report["ok"]) != (
        "paracine.diagnostics.v1", 1, "normalized-utf8-byte-1-based", expected_ok
    ):
        raise RuntimeError("Diagnostic schema or outcome mismatch")
    for diagnostic in report["diagnostics"]:
        required = {"severity", "code", "phase", "message", "hint", "line", "column", "warning_level", "promoted", "primary", "secondary", "traces", "fixes", "type_difference"}
        if set(diagnostic) != required:
            raise RuntimeError("Diagnostic record fields drifted")
        spans = [diagnostic["primary"], *(item["span"] for item in diagnostic["secondary"]), *(item["span"] for item in diagnostic["traces"]), *(item["span"] for item in diagnostic["fixes"])]
        for span in spans:
            if set(span) != {"file", "start", "end"}:
                raise RuntimeError("Span schema drifted")
            for position in (span["start"], span["end"]):
                if set(position) != {"line", "column", "offset"}:
                    raise RuntimeError("Position schema drifted")
                if position["offset"] is not None and position["offset"] < 0:
                    raise RuntimeError("Invalid byte offset")
        if diagnostic["severity"] not in {"error", "warning", "note"}:
            raise RuntimeError("Invalid severity")


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    cases = 0
    with tempfile.TemporaryDirectory(prefix="paracine-diagnostics-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "main.pcn"

        def write(text):
            source.write_text(text, encoding="utf-8")

        def check(text, expected=0, options=()):
            nonlocal cases
            write(text)
            result = run([compiler, "check", str(source), "--json", *options], root, expected)
            if result.stderr or "\x1b" in result.stdout:
                raise RuntimeError("JSON diagnostics contaminated by terminal output")
            report = json.loads(result.stdout)
            validate(report, expected == 0)
            cases += 1
            return report

        valid = "routine main() gives int\n    give 42\n"
        report = check(valid)
        if report["diagnostics"] or report["routines"] != 1:
            raise RuntimeError("Successful check lost its summary")
        report = check("routine one() gives int\n    give +\nroutine main() gives int\n    give *\n", 1)
        if len(report["diagnostics"]) != 2 or {item["phase"] for item in report["diagnostics"]} != {"parser"}:
            raise RuntimeError("Independent parser errors were not recovered")
        report = check("routine main() gives int\n\tgive 42\n", 1)
        if report["diagnostics"][0]["code"] != "E0100" or report["diagnostics"][0]["phase"] != "lexer":
            raise RuntimeError("Lexical exceptions do not use the shared schema")
        report = check("routine main() gives int\n    let value: int = true\n    give false\n", 1)
        mismatches = [item for item in report["diagnostics"] if item["type_difference"] == {"expected": "int", "actual": "bool"}]
        if len(mismatches) != 2 or not all(item["secondary"] for item in mismatches):
            raise RuntimeError("Type differences or related typed spans missing")
        report = check("routine main() gives int\n    give 42\nroutine main() gives int\n    give 0\n", 1)
        duplicate = next(item for item in report["diagnostics"] if item["code"] == "E0305")
        if duplicate["secondary"][0]["span"]["start"]["line"] != 1:
            raise RuntimeError("Duplicate definition lacks original declaration location")

        warning = valid + "    let unused: int = 1\n"
        report = check(warning)
        diagnostic = report["diagnostics"][0]
        if len(report["diagnostics"]) != 1 or (diagnostic["severity"], diagnostic["code"], diagnostic["promoted"]) != ("warning", "W0306", False):
            raise RuntimeError("Default warning classification failed")
        edit = diagnostic["fixes"][0]
        start, end = edit["span"]["start"]["offset"], edit["span"]["end"]["offset"]
        if start is None or end is None or start >= end:
            raise RuntimeError("Fix-it edit lacks an exact source range")
        data = warning.encode("utf-8")
        fixed = (data[:start] + edit["replacement"].encode("utf-8") + data[end:]).decode("utf-8")
        if check(fixed)["diagnostics"]:
            raise RuntimeError("Suggested warning removal did not repair the diagnostic")
        report = check(warning, 1, ("--warnings-as-errors",))
        if report["diagnostics"][0]["severity"] != "error" or not report["diagnostics"][0]["promoted"]:
            raise RuntimeError("Warning-as-error did not change outcome")
        check(warning, 1, ("--deny-warning", "W0306"))
        for options in (("--warning-level", "0"), ("--warning-level", "2", "--allow-warning", "W0306")):
            if check(warning, options=options)["diagnostics"]:
                raise RuntimeError("Warning filtering failed")
        if check("@allow(W0306)\n" + warning, options=("--warnings-as-errors",))["diagnostics"]:
            raise RuntimeError("Routine suppression did not survive warning promotion")
        check("@allow(E0303)\n" + valid, 1)
        check(valid, 1, ("--allow-warning", "E0303"))
        check(valid, 1, ("--color", "invalid"))

        (root / "a.pcn").write_text("import b\nroutine okay() gives int\n    give 1\n", encoding="utf-8")
        (root / "b.pcn").write_text("routine broken() gives int\n    give false\n", encoding="utf-8")
        report = check("import a\n" + valid, 1)
        imported = next(item for item in report["diagnostics"] if pathlib.Path(item["primary"]["file"]).name == "b.pcn")
        if [item["message"] for item in imported["traces"] if item["kind"] == "import"] != ["import a", "import b"]:
            raise RuntimeError("Nested imported error lost its real import chain")
        if imported["primary"]["start"]["offset"] is None:
            raise RuntimeError("Imported source snapshot is unavailable")
        (root / "a.pcn").write_text("routine first() gives int\n    give +\n", encoding="utf-8")
        (root / "b.pcn").write_text("routine second() gives int\n    give *\n", encoding="utf-8")
        report = check("import a\nimport b\n" + valid, 1)
        if len(report["diagnostics"]) != 2 or {pathlib.Path(item["primary"]["file"]).name for item in report["diagnostics"]} != {"a.pcn", "b.pcn"} or not all(item["traces"] for item in report["diagnostics"]):
            raise RuntimeError("Errors in independent imports were not collected with provenance")
        (root / "b.pcn").write_text("routine second() gives int\n\tgive 42\n", encoding="utf-8")
        report = check("import a\nimport b\n" + valid, 1)
        if {item["phase"] for item in report["diagnostics"]} != {"parser", "lexer"} or not all(item["traces"] for item in report["diagnostics"]):
            raise RuntimeError("Independent imported lexical/parser errors lost their context")
        report = check("routine expand<T>() gives int\n    give expand<option<T>>()\nroutine main() gives int\n    give expand<int>()\n", 1)
        generic = next(item for item in report["diagnostics"] if item["code"] == "E0345")
        if not generic["secondary"] or not any(item["kind"] == "generic" and item["span"]["start"]["line"] == 4 for item in generic["traces"]):
            raise RuntimeError("Generic expansion failure lacks real call-site ancestry")

        unicode_source = "routine main() gives int\n    give \\u03b1missing\n".replace("\\u03b1", chr(0x03b1))
        report = check(unicode_source, 1)
        primary = next(item["primary"] for item in report["diagnostics"] if item["code"] == "E0301")
        data = unicode_source.encode("utf-8")
        if data[primary["start"]["offset"]:primary["end"]["offset"]].decode("utf-8") != chr(0x03b1):
            raise RuntimeError("Fallback diagnostic span splits a Unicode scalar")
        report = check("routine id<T>(value: T) gives T\n    give value\nroutine main() gives int\n    give id<int>(42)\n    let unused: int = 1\n")
        if len(report["diagnostics"]) != 1:
            raise RuntimeError("Generic concrete revalidation duplicated a source warning")
        write(valid + "    let unused: int = 1 #" + chr(27) + "[31m\n")
        result = run([compiler, "check", str(source), "--color", "never"], root)
        if "\x1b" in result.stderr or "\\x1b[31m" not in result.stderr:
            raise RuntimeError("Source control bytes escaped into terminal output")
        cases += 1
        write(warning)
        for mode, color in (("never", False), ("auto", False), ("always", True)):
            result = run([compiler, "check", str(source), "--color", mode], root)
            if ("\x1b[" in result.stderr) != color or "warning[W0306]" not in result.stderr.replace("\x1b[33m", "").replace("\x1b[0m", ""):
                raise RuntimeError("Redirected terminal color handling failed")
            cases += 1
        result = run([compiler, "check", str(source), "--color", "auto"], root, environment={**os.environ, "NO_COLOR": "1"})
        if "\x1b" in result.stderr:
            raise RuntimeError("NO_COLOR was ignored")
        cases += 1
        cache = root / "cache"
        output = root / "main.exe"
        command = [compiler, "build", str(source), "--cache-dir", str(cache), "-o", str(output), "--color", "never"]
        first, second = run(command, root), run(command, root)
        if "cache hit" not in second.stdout or "warning[W0306]" not in first.stderr or "warning[W0306]" not in second.stderr:
            raise RuntimeError("Cache hit bypassed warning diagnostics")
        run([*command, "--warnings-as-errors"], root, 1)
        cases += 3
    print(f"PASS {cases} diagnostic schema, provenance, warning and terminal cases")


if __name__ == "__main__":
    main()
