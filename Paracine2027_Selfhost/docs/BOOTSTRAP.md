# Native Paracine self-hosting bootstrap sequence

## Bootstrapping principle

Self-hosted means the compiler's compiler implementation is written in the same language it compiles, after an initial trusted stage has built it. The first executable necessarily comes from a pre-existing compiler/toolchain or a trusted executable seed. The normative C++23 Stage-0 foundation is not contradicted by a later Paracine-written Stage-2 compiler; it is the bootstrap of trust.

```text
C++23 Stage-0 compiler.exe  [existing, version-pinned]
             |
             | compile ParacineCompiler2027.pcn
             v
Paracine Stage-1 executable [subset + 2027 structural audit]
             |
             | future: add full Edition-2027 semantics, PCIR, PMIR, COFF, PE
             v
Paracine Stage-2 native compiler [NOT YET PRESENT]
             |
             | self-compile under pinned target/profile
             v
Stage-3 binary [NOT YET PRESENT]
             |
             | compare Stage-2/Stage-3 semantics and output
             v
Reproducible self-host confirmation [NOT YET PRESENT]
```

## Build Stage 1

1. Use your existing Stage-0 executable that previously built `ParacineCompiler.pcn`.
2. Build `src/stage1_compiler.pcn` with the same source and standard-library interfaces used by that source.
3. Run `--capabilities-2027`; it explicitly reports missing native and semantic components.
4. Run `audit-2027` against positive and negative fixtures. A negative fixture must fail.
5. Run the prior Edition-2026 subset regression suite and check no behavior changed.

## Build Stage 2 (completion criteria, not an existing capability)

- Resolve the normative language grammar and names/precedence/trait rules.
- Replace the Stage-1 source-to-source output with typed AST -> SAGE -> verified PCIR.
- Add a PCIR interpreter and semantic differential tests before optimizations.
- Add PMIR legalization, native register allocation, x86-64 encoder, COFF writer, linker and PE32+ validation.
- Add library binary I/O interfaces with typed, recoverable errors.
- Support library startup, runtime and foreign Win64 ABI.
- Only then allow the new self-host binary to accept `check`, `obj`, `build`, `link` as true native operations.

## Security and proof rule

A source-level policy test does not qualify deep isolation. A compiler-generated result does not prove correctness by itself. Every native backend transformation needs either its applicable formal proof obligation or differential validation plus the separately required correctness arguments. Do not claim high-assurance equivalence solely from test passes.
