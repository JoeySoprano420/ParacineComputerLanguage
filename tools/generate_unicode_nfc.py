"""Generate deterministic NFC and XID tables from Unicode 15.1.0."""
import pathlib
import unicodedata as u

if u.unidata_version != "15.1.0":
    raise SystemExit("Unicode 15.1.0 is required; found " + u.unidata_version)

decompositions = []
combining = []
compositions = []
xid_start_ranges = []
xid_continue_ranges = []
for cp in range(0x110000):
    char = chr(cp)
    ccc = u.combining(char)
    if ccc:
        combining.append((cp, ccc))
    raw = u.decomposition(char)
    if raw and not raw.startswith("<"):
        parts = [int(x, 16) for x in raw.split()]
        if len(parts) > 2:
            raise SystemExit("Unexpected canonical decomposition length")
        decompositions.append((cp, parts[0], parts[1] if len(parts) == 2 else 0))
        if len(parts) == 2 and u.normalize("NFC", "".join(chr(x) for x in parts)) == char:
            compositions.append((parts[0], parts[1], cp))
    if char != "_" and char.isidentifier():
        if xid_start_ranges and xid_start_ranges[-1][1] + 1 == cp:
            xid_start_ranges[-1] = (xid_start_ranges[-1][0], cp)
        else:
            xid_start_ranges.append((cp, cp))
    if ("A" + char).isidentifier():
        if xid_continue_ranges and xid_continue_ranges[-1][1] + 1 == cp:
            xid_continue_ranges[-1] = (xid_continue_ranges[-1][0], cp)
        else:
            xid_continue_ranges.append((cp, cp))
compositions.sort()
output = pathlib.Path(__file__).resolve().parent.parent / "unicode_nfc_data.h"
with output.open("w", encoding="ascii", newline="\n") as f:
    f.write("// Generated from Unicode 15.1.0; see UNICODE-LICENSE.txt.\n#pragma once\n\n")
    f.write("struct UnicodeDecomposition { std::uint32_t code, first, second; };\n")
    f.write("struct UnicodeCombiningClass { std::uint32_t code; std::uint8_t value; };\n")
    f.write("struct UnicodeComposition { std::uint32_t first, second, result; };\n")
    f.write("struct UnicodeCodepointRange { std::uint32_t first, last; };\n\n")
    for name, kind, entries in (("kUnicodeDecompositions", "UnicodeDecomposition", decompositions),
                                 ("kUnicodeCombiningClasses", "UnicodeCombiningClass", combining),
                                 ("kUnicodeCompositions", "UnicodeComposition", compositions),
                                 ("kUnicodeXidStartRanges", "UnicodeCodepointRange", xid_start_ranges),
                                 ("kUnicodeXidContinueRanges", "UnicodeCodepointRange", xid_continue_ranges)):
        f.write("static constexpr " + kind + " " + name + "[] = {\n")
        for entry in entries:
            f.write("    { " + ", ".join("0x%X" % x for x in entry) + " },\n")
        f.write("};\n\n")
print("Wrote", output, "using Unicode", u.unidata_version)
