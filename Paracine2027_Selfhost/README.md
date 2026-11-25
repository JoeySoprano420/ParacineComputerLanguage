# Paracine 2027 — Self-hosting Compiler Bootstrap Project

**Write with intent. Compile with certainty.**  
Violet Aura Creations / Paracine Project

## What this download actually is

This repository is a **real Paracine-language source project**, derived from the user's existing `ParacineCompiler.pcn` Stage-1 implementation (about 2,091 lines). It preserves that source's lexer, parser, subset type checking, source emission, command-line driver, and Stage-0 native build bridge. It adds an Edition-2027 *structural auditor* written in Paracine and separate `.pcn` implementations of selected semantic, optimization, navigation, and x86-64 encoding kernels.

**It is not a fully implemented Paracine 2027 compiler.** It has not been built or run in this environment because the user's existing C++23 `Paracine.exe` bootstrap compiler and Windows test setup are not available here. Neither a passing 2027 audit nor the included algorithm kernels establishes full language correctness or native compilation. The existing Stage-1 subset compiler still forwards generated `.pcn` source to Stage 0 to produce a Windows executable.

The distinction is deliberate: do not treat a design specification as verification evidence.

## Repository layout

| Path | Purpose | Truthful status |
|---|---|---|
| `src/stage1_compiler.pcn` | Preserved Stage-1 compiler + 2027 lexical/structural audit | Implemented source; untested here |
| `src/semantics/ownership.pcn` | Small ownership borrow-state transfer kernel | Implemented local algorithm; not full NLL |
| `src/semantics/effects.pcn` | Five-effect joins and contract containment | Implemented local algorithm; no call-graph solver |
| `src/semantics/capsules.pcn` | Fail-closed isolation decisions and boundary predicates | Policy logic only; **no OS isolation** |
| `src/semantics/chains.pcn` | Dependency cycle check for <=60 stages | Implemented local algorithm; no Chain execution |
| `src/semantics/numerics.pcn` | Width validation and arithmetic rewrite guards | Limited checks; no big integers/decimal/posit engine |
| `src/semantics/state.pcn` | Local integer checkpoint/commit/rollback | Local reversible value only |
| `src/semantics/inheritance.pcn` | Necessary Family substitution obligations | Predicate only; no type resolver |
| `src/knowledge/polr.pcn` | Feasible-route filtering and deterministic cost choice | Bounded local scoring; no blueprint traversal |
| `src/knowledge/pitel.pcn` | Cache-key reuse conditions | Local validity checks; no scheduler |
| `src/knowledge/blueprint.pcn` | Bounded dependency reachability | Graph algorithm only |
| `src/optimization/aco.pcn` | Rewrite verification gate | Does **not** construct an E-graph or proof |
| `src/optimization/abstract_interpretation.pcn` | Conservative interval abstract interpretation | One numeric domain only |
| `src/backend/x64_encoding.pcn` | Correct byte formulas for selected x64 instructions | Partial encoder primitives only |
| `src/backend/coff_tiny.pcn` | Pure byte-by-byte 88-byte AMD64 COFF object routine | Real tiny object-format example; no binary writer |
| `tests/fixtures/main_return_42.obj` | Independent machine-code/COFF fixture | Parsed/disassembled with GNU objdump |
| `src/backend/layout.pcn` | COFF/PE constants, alignment, target facts | Partial native layout helpers only |
| `docs/BOOTSTRAP.md` | Windows Stage-0/Stage-1/Stage-2 progression | Recommended build sequence |
| `docs/NATIVE_OBJECT_EVIDENCE.md` | Tiny COFF format test and evidence limits | GNU objdump parsing/disassembly of reference fixture |
| `docs/COMPLETION_CONTRACT.md` | Exact remaining compiler systems and conformance gates | Engineering contract |
| `tests/positive`, `tests/negative` | Structural audit examples | Source fixtures; not executed here |

## Build the existing Stage-1 path on Windows

Using a Stage-0 compiler with the same language and standard library interfaces as the original `ParacineCompiler.pcn`:

```powershell
Paracine pe src\stage1_compiler.pcn -O0 -o ParacineCompiler2027.exe
.\ParacineCompiler2027.exe --version
.\ParacineCompiler2027.exe --capabilities-2027
.\ParacineCompiler2027.exe audit-2027 tests\positive\edition2027_features.pcn
.\ParacineCompiler2027.exe audit-2027 tests\negative\bad_width.pcn
```

If your Stage-0 executable is named differently or uses a different CLI syntax, adapt that one compilation command. Check your current installed standard-library names (`std.fs.read_text`, `.write_text`, `std.process.run`, `std.io.println`) against the provided source. **These are expected commands, not recorded successful runs.**

The existing bounded Stage-1 source-to-source mode remains:

```powershell
.\ParacineCompiler2027.exe check tests\positive\legacy_subset.pcn
.\ParacineCompiler2027.exe build tests\positive\legacy_subset.pcn -o legacy_subset.exe --stage0 Paracine -O0
```

`audit-2027` intentionally cannot produce `.exe` or claim semantic correctness. A 2027 program cannot be compiled merely by passing its audit.

## How to reach an actual fully self-hosted compiler

1. **Formalize Edition-2027 grammar and static semantics** — parse all constructs to a lossless CST and typed AST, not token matching.
2. **Complete SAGE** — full name, type, generic, capability, ownership, effect, contract, failure, isolation and ABI analysis with verified judgments.
3. **Implement PCIR + reference interpreter** — explicit instruction semantics, validation, negative tests and differential execution.
4. **Implement full native backend** — PMIR, x86-64 instruction selection, register allocation, frame layout, COFF writer, integrated PE linker, PDB and exception/unwind requirements.
5. **Implement native runtime and security** — owned file and binary I/O, allocator, FFI, threading, compartment process launcher, authenticated messaging, fail-closed deep isolation.
6. **Complete advanced systems** — Family substitution, Chain graphs and execution, transactional log semantics, numeric formats, rendering/training contracts, SAGE knowledge model, ACO/PITEL/POLR.
7. **Bootstrap** — Stage-0 C++23 builds Stage-1/2 Paracine compiler, Stage-1 builds itself, Stage-2 independently builds itself, compare native artifacts under a pinned build identity.
8. **Conformance qualification** — execute all G01–G30 with positive and negative tests, differential execution, ABI and security audits, controlled performance benchmarks.

The Edition-2027 specification is a target; `docs/COMPLETION_CONTRACT.md` enumerates missing requirements. **Self-hosting alone is not proof of full language conformance.**

## Known specification/implementation boundaries

- The Stage-1 source scanner currently handles ASCII identifier starts rather than complete Unicode 15.1 XID/NFC. Its delimiter nesting is also below the 2027 stated limits.
- `audit-2027` validates top-level 2027 names, Capsule isolation selectors, Chain-stage placement and `bitint`/`ubitint` width literals, plus UTF-8/indentation through the original scanner. It is not a general 2027 parser.
- The ownership kernel models one binding's borrow counts but has no region graph, path-sensitive lifetime solver, alias proof or destructor handling.
- `x64_encoding.pcn` implements only selected individual instruction bytes, not all REX/VEX/EVEX or object/linker emission.
- Strong security claims require actual Windows OS compartment enforcement; source-level `isolate deep` declarations do nothing without that runtime.
- Licensing in Specification 2.0 identifies GPL-3.0, a runtime exception, and Apache-2.0 documentation. Add the **authoritative corresponding license texts** before distribution; this project does not silently fabricate a runtime exception.


## Provenance

Source origin: the user's Library file `ParacineCompiler.pcn`, imported as the foundation of this project. Changes were additive to the compiler except for its metadata strings and Edition-2027 keyword recognition; user-authored input source was not overwritten.
