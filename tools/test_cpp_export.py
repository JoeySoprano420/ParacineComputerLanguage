r"""Differential checks of PCIR, native PE, and compiled C++ exports on Windows x64.

Run in an x64 Visual Studio developer shell:
    python tools\test_cpp_export.py path\to\Paracine.exe
"""
import pathlib
import subprocess
import sys
import tempfile

CASES = [
    ("owned-capture-snapshot", 42, """routine main() gives int
    var seed: int = 40
    let action = move routine(x: int) gives int => seed + x
    seed = 100
    let saved = action
    give action(1) + saved(1) - 40
"""),
    ("borrowed-closure-alias", 42, """routine main() gives int
    let seed: int = 40
    let action = routine(x: int) gives int => seed + x
    let saved = action
    give saved(2)
"""),
    ("inferred-closure-return", 42, """routine main() gives int
    let seed: int = 40
    let action = routine(x: int) => seed + x
    give action(2)
"""),
    ("grouped-closure-call", 42, """routine main() gives int
    let seed: int = 40
    let action = move routine(x: int) gives int => seed + x
    give (action)(2)
"""),
    ("indexed-closure-call", 42, """routine main() gives int
    let seed: int = 40
    let actions = [move routine(x: int) gives int => seed + x]
    give actions[0](2)
"""),
    ("closure-floating-argument", 42, """routine main() gives int
    let action = move routine(x: double) gives double => -x
    if action(2.5) == -2.5
        give 42
    give 0
"""),
    ("inferred-rich-closure-return", 42, """routine main() gives int
    let seed: text = "answer"
    let action = move routine() => seed
    if action().count() == 6
        give 42
    give 0
"""),
    ("generic-closures", 42, """routine relay<T>(value: T) gives T
    let action = routine() gives T => value
    let saved = action
    give saved()
routine main() gives int
    if relay<bool>(true)
        give relay<int>(42)
    give 0
"""),
    ("text-capture-return", 42, """routine count(value: text) gives size
    give value.count()
routine main() gives int
    let seed: text = "answer"
    let action = move routine() gives text => seed
    let saved = action
    if count(saved()) == 6
        give 42
    give 0
"""),
    ("move-string-capture-repeated-call", 42, """routine main() gives int
    let owner: string = "answer".to_string()
    let action = move routine() gives size => owner.to_text().count()
    let saved = action
    let first: size = saved()
    let second: size = saved()
    if first == 6 and second == 6
        give 42
    give 0
"""),
    ("move-string-capture-return", 42, """routine main() gives int
    let owner: string = "answer".to_string()
    let action = move routine() gives string => owner
    let result: string = action()
    if result.to_text().count() == 6
        give 42
    give 0
"""),
    ("borrowed-record-capture", 42, """record Counter
    value: int
routine bump(var state: Counter) gives int
    state.value = state.value + 20
    give state.value
routine main() gives int
    var state: Counter = Counter{value: 2}
    let action = routine() gives int => bump(state)
    let saved = action
    let first: int = saved()
    let second: int = action()
    give state.value
"""),
    ("borrowed-floating-capture", 42, """routine main() gives int
    let seed: double = 2.5
    let action = routine() gives double => -seed
    let saved = action
    if saved() == -2.5
        give 42
    give 0
"""),
    ("move-string-capture-mutation", 42, """routine main() gives int
    var owner: string = "a".to_string()
    let action = move routine() gives size with alloc, trap
        owner.append("b")
        give owner.to_text().count()
    let first: size = action()
    let second: size = action()
    if first == 2 and second == 3
        give 42
    give 0
"""),
    ("move-string-capture-loop-cleanup", 42, """routine main() gives int
    var total: size = 0
    from index = 1 to 7
        let owner: string = "answer".to_string()
        let action = move routine() gives size => owner.to_text().count()
        total += action()
    if total == 42
        give 42
    give 0
"""),
    ("borrowed-string-capture", 42, """routine main() gives int
    let owner: string = "answer".to_string()
    let action = routine() gives size => owner.to_text().count()
    let saved = action
    if action() == 6 and saved() == 6
        give 42
    give 0
"""),
    ("owned-text-move-mutation", 42, """routine main() gives int
    var owner: string = "first".to_string()
    let first: text = owner.to_text()
    let initial: size = first.count()
    owner.clear()
    owner.append("answer")
    let moved: string = owner
    if moved.to_text().count() == initial + 1
        give 42
    give 0
"""),
    ("rich-aggregate-copy", 42, """record Pair
    label: text
    value: int
routine label(value: Pair) gives text
    give value.label
routine main() gives int
    var pair: Pair = Pair{label: "before", value: 42}
    let before: text = pair.label
    var saved: Pair = pair
    saved.label = "after"
    if before == "before" and label(saved) == "after"
        give saved.value
    give 0
"""),
    ("rich-array-loops", 42, """routine main() gives int
    var words: array<text, 3> = ["a", "bb", "ccc"]
    words[1] = "answer"
    var count: size = 0
    for word in words
        count += word.count()
    if count == 10
        give 42
    give 0
"""),
    ("result-text", 42, """record Marker
    value: int
routine answer() gives result<text, int>
    give ok("answer")
routine measure(var state: Marker) gives result<int, int>
    let value: text = try answer()
    if value.count() == 6
        state.value = 42
        give ok(42)
    fail 1
routine main() gives int
    var state: Marker = Marker{value: 0}
    let outcome = measure(state)
    give state.value
"""),
    ("result-error-propagation", 42, """record Marker
    value: int
routine answer() gives result<text, int>
    fail 7
routine measure(var state: Marker) gives result<int, int>
    let value: text = try answer()
    state.value = 1
    give ok(0)
routine main() gives int
    var state: Marker = Marker{value: 42}
    let outcome = measure(state)
    give state.value
"""),
    ("text-search-and-slices", 42, """routine main() gives int
    let value: text = "bananas"
    if value.slice(1, 3) == "ana" and value.starts_with("ban") and value.ends_with("nas")
        if value.find("ana") == 1 and value.rfind("ana") == 3
            if value.byte_at(0) == 98 and value.find_byte(110) == 2
                give 42
    give 0
"""),
    ("floating-and-rich-text", 42, """routine difference(x: double) gives double
    give -x + 1.5
routine main() gives int
    let marker: text = "rich"
    if difference(3.5) == -2.0 and marker.count() == 4
        give 42
    give 0
"""),
    ("rich-stdout", 42, """import std.io
routine main() gives int with io
    std.io.println("backend parity")
    give 42
"""),
    ("utf8-file-roundtrip", 42, """import std.fs
routine main() gives int with io, alloc, trap
    if not std.fs.write_text("export-data.txt", "café")
        give 1
    if not std.fs.exists("export-data.txt")
        give 2
    let data: string = std.fs.read_text("export-data.txt")
    if data.to_text() == "café"
        give 42
    give 0
"""),
    ("unicode-identifier-and-text", 42, """routine 答え(value: text) gives text
    give value
routine main() gives int
    let value: text = 答え("café")
    if value.count() == 5
        give 42
    give 0
"""),
]

RUNTIME_CHECKS = r"""
int main(int argc, char**) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    using namespace pcn_runtime;
    auto literal = text(std::string("A\xc3\xa9\xf0\x9f\x98\x80", 7));
    if (argc == 2) { (void)slice(literal, 2, 1); return 99; }
    if (argc == 3) { (void)codepoint(literal, 2); return 99; }
    if (argc == 4) { (void)byte_at(literal, -1); return 99; }
    if (argc == 5) { auto owner = text("loan", true); auto view = borrow(owner, 0, 4); dispose(owner); (void)bytes(view); return 99; }
    if (argc == 6) { auto invalid = text(std::string("\xed\xa0\x80", 3)); validate_utf8(invalid); return 99; }
    if (codepoint(literal, 1) != 233 || codepoint(literal, 3) != 128512 || codepoints(literal) != 3 || ascii(literal)) return 1;
    auto part = slice(literal, 1, 2);
    if (bytes(part).data() != bytes(literal).data() + 1 || bytes(part) != "\xc3\xa9") return 2;
    auto owner = text("abc", true); auto view = borrow(owner, 0, 3);
    if (bytes(view).data() != bytes(owner).data()) return 3;
    auto moved = string_move(owner);
    if (!bytes(owner).empty() || bytes(moved) != "abc" || bytes(view) != "abc") return 4;
    append(moved, text("d")); clear(moved); append(moved, text("answer"));
    if (bytes(moved) != "answer") return 5;
    dispose(moved); if (!bytes(moved).empty()) return 6;
    auto record = aggregate("Pair", {"text", "number"}, {literal, Value(42)});
    auto saved = copy(record); field(record, "number") = 99;
    if (std::int64_t(field(saved, "number")) != 42) return 7;
    auto action = closure("test", {Value(40)}, [](const std::vector<Value>& a) { return Value(add(a.at(0), a.at(1))); });
    if (std::int64_t(invoke(action, {Value(2)})) != 42) return 8;
    auto result = tagged("result<text,int>", 0, {literal});
    if (tag(result) != 0 || bytes(payload(result)) != bytes(literal)) return 9;
    auto some = tagged("option<text>", 1, {literal}); auto none = tagged("option<text>", 0, {});
    if (tag(some) != 1 || tag(none) != 0 || bytes(payload(some)) != bytes(literal)) return 10;
    if (find_text(text("banana"), text("ana")) != 1 || find_text(text("banana"), text("ana"), true) != 3) return 11;
    if (compare(text("a"), text("b")) != -1 || find_byte(literal, 300) != -1 || hash(text("")) != std::bit_cast<std::int64_t>(14695981039346656037ull)) return 12;
    return 42;
}
"""


def run(command, directory, expected=0):
    result = subprocess.run(command, cwd=directory, capture_output=True, timeout=120)
    if result.returncode != expected:
        raise RuntimeError(
            f"{command!r}: exit {result.returncode}, expected {expected}\n"
            + result.stdout.decode("utf-8", "replace")
            + result.stderr.decode("utf-8", "replace")
        )
    return result


def compile_cpp(source, target, directory):
    run(["cl", "/nologo", "/std:c++20", "/EHsc", "/O2", "/W3", "/permissive-",
         "/D_CRT_SECURE_NO_WARNINGS", str(source), "/Fe:" + str(target),
         "/Fo:" + str(target.with_suffix(".obj"))], directory)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    compiler = pathlib.Path(sys.argv[1]).resolve()
    count = 0
    with tempfile.TemporaryDirectory(prefix="paracine-cpp-export-") as temp:
        root = pathlib.Path(temp)
        runtime_source = None
        for name, expected, source in CASES:
            path = root / (name + ".pcn")
            path.write_bytes(source.encode("utf-8"))
            for level in (0, 3):
                flags = ["-O" + str(level)]
                differential = run([str(compiler), "test", str(path), *flags], root)
                if f"PCIR oracle={expected}".encode() not in differential.stdout:
                    raise RuntimeError(name + ": oracle/native returned an unexpected result")
                cpp = root / (name + f"-O{level}.cpp")
                target = cpp.with_suffix(".exe")
                run([str(compiler), "cpp", str(path), *flags, "-o", str(cpp)], root)
                compile_cpp(cpp, target, root)
                actual = run([str(target)], root, expected)
                if name == "rich-stdout" and actual.stdout.replace(b"\r\n", b"\n") != b"backend parity\n":
                    raise RuntimeError("C++ export changed stdout bytes")
                if name == "owned-capture-snapshot":
                    runtime_source = cpp.read_text(encoding="utf-8")
                count += 1
                print(f"PASS {name} -O{level}")
        if runtime_source is None:
            raise RuntimeError("No generated rich runtime available")
        boundary = "int main() {"
        if runtime_source.count(boundary) != 1:
            raise RuntimeError("Cannot identify the exported entry point")
        runtime_source = runtime_source.replace(boundary, "int exported_main() {", 1) + RUNTIME_CHECKS
        cpp = root / "runtime-checks.cpp"
        cpp.write_bytes(runtime_source.encode("utf-8"))
        target = cpp.with_suffix(".exe")
        compile_cpp(cpp, target, root)
        run([str(target)], root, 42)
        count += 1
        for arguments in range(1, 6):
            result = subprocess.run([str(target), *(["trap"] * arguments)], cwd=root,
                                    capture_output=True, timeout=30)
            if result.returncode in (0, 42, 99) or b"Paracine trap" not in result.stderr:
                raise RuntimeError(f"Runtime guard {arguments} did not report an intentional trap")
            count += 1
        print(f"PASS {count} export differential/runtime cases")


if __name__ == "__main__":
    main()
