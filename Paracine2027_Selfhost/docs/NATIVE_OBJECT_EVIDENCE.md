# Narrow native object-format evidence

`src/backend/coff_tiny.pcn` is Paracine source that defines the exact contents of an 88-byte AMD64 COFF object, containing a symbol named `main` whose body is `mov eax, 42; ret`. The byte functions are general for 32-bit integer return constants; the fixture uses 42.

The byte-identical reference fixture at `tests/fixtures/main_return_42.obj` was independently constructed for package verification in the container, **not produced by executing the Paracine source** (the Stage-0 compiler binary was not available here).

The reference fixture was parsed as `x86-64 COFF object file` and disassembled with GNU `objdump`. Its disassembly was:

```text
0000000000000000 <main>:
   0: b8 2a 00 00 00   mov $0x2a,%eax
   5: c3               ret
```

This independently establishes that the specified target bytes represent a valid tiny COFF object under the tested decoder. It does not establish that the Paracine compiler executed, generated an object, linked an executable, passed Win64 ABI conformance, or supports other Edition-2027 programs.

The next native milestone is a Paracine-owned binary output API, an extensible COFF symbol/section/relocation writer, an integrated linker, and independently verified PE32+ executables.
