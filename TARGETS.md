# Implemented target and runtime contracts

This is the development compiler's capability contract, not a declaration of full
platform support. `target_contract.h` is the shared registry used by frontend/CLI
validation, scalar argument classification, native format selection and debug
selection. Both target and runtime contract revisions participate in native cache
identity. Target names are exact and unknown names fail with `E0540`.

```
paracine targets --json
paracine targets --target linux-arm64
paracine runtime-contract --target windows-x86_64 --json
```

The stable schemas are `paracine.targets.v1` and `paracine.runtime.v1`. Capabilities
have `implemented`, `partial` or `unsupported` status and explanatory details.
Consumers must inspect feature status, not infer support from a platform name,
CPU number, pointer size, ABI name or object format. JSON is deterministic and
locale-independent. The TLS model is a platform identity, not an implemented
language facility. No SDK, input source or external executable is needed to query
these contracts. Invalid/repeated query switches fail with `E0591`.

## Target matrix

| Target | Objects | Executables/startup | ABI | Debug/unwind |
|---|---|---|---|---|
| `windows-x86_64` | COFF | PE; optional UTF-8 argv adapter | Microsoft x64 scalars and Paracine indirect rich values | CodeView/PDB; OS x64 unwind records |
| `linux-x86_64` | ELF64 | Static scalar no-argument entry and exit syscall | System V scalar subset | DWARF 4 and debugger `.debug_frame` with `-g` |
| `macos-x86_64` (retained) | Mach-O64 | External macOS linker/startup required | System V scalar subset | Not implemented |
| `macos-arm64` | Not implemented | Not implemented | Darwin ARM64 classification not implemented | Not implemented |
| `linux-arm64` | Not implemented | Not implemented | AAPCS64 classification not implemented | Not implemented |
| `windows-arm64` | Not implemented | Not implemented | Windows ARM64 classification not implemented | Not implemented |

ARM64 entries permit target-independent `check`, `ast`, `sage` and `ir` work.
Requests for ABI reports, machine IR, assembly or native artifacts reject with
`E0590` before reading source or touching output. Registration never routes ARM64
through the x64 encoder. Full ARM64 instruction selection, aggregate ABI rules,
relocations, runtime services, unwind records, debug records and execution tests
are still necessary before enabling an ARM64 backend.

### Calling and linking boundaries

The shared argument classifier implements positional Microsoft x64 lanes, 32-byte
home space, hidden-result pointer lane consumption, ordered overflow slots and
16-byte outgoing alignment. Floating arguments in the first four lanes of a
variadic Windows call are duplicated into GP registers, **including named fixed
floating parameters**. Only floating arguments after the ellipsis receive default
float-to-double promotion. A native register probe tests fixed-float duplication
at both O0 and O3.

System V uses independent six-GP/eight-XMM banks, source-ordered overflow slots,
16-byte outgoing alignment and AL vector-register counts for variadics. Exhausting
one register bank does not exhaust the other. The classifier is used for both
parameter spills and call argument placement. Rich System V classification fails
explicitly; this is not the complete platform C aggregate/HFA classification.

COFF supports AMD64 call, RVA and debug relocations; ELF supports x86-64 PLT32
calls and absolute debug relocations; Mach-O supports x86-64 branch relocations.
These are bounded subsets, not complete relocation catalogs. Windows imports
supported runtime functions from KERNEL32/SHELL32; debug linking needs installed
`lld-link` and import libraries. The separate COFF linker adapter can produce DLLs.
Unix external calls require a system linker and matching startup/runtime. There is
no integrated Unix dynamic executable/shared-library writer, general runtime
loader API, TLS lowering or language exception propagation on any target.

See [ABI.md](ABI.md) for precise layout, ownership, naming, debug-location and
unwind rules and their remaining limitations.

## Runtime contract

The runtime contract describes **generated program behavior**, not host facilities
used by the compiler or by the PCIR interpreter. Compiler worker threads and cache
environment snapshots are not program threading/environment APIs.

* **Allocation/deallocation:** the implemented Windows owned-text paths use the
  process heap (`GetProcessHeap`, `HeapAlloc`, `HeapFree`). Allocation failure
  traps. Owned storage must return to the same runtime allocator; foreign free,
  cross-runtime ownership transfer and a general public allocator ABI are not
  provided. Direct owners have scoped cleanup; arbitrary recursive owning
  aggregates remain rejected where drop/move legalization is unavailable.
* **Panic/traps:** generated checked failures use x86 `UD2`. There is no recoverable
  panic object, guaranteed diagnostic message, exception cleanup or stable numeric
  trap exit code across operating systems. The PCIR oracle reports exceptions to
  its host rather than executing `UD2`.
* **Stack overflow:** probing, alternate stacks and recoverable overflow handling
  are **not implemented**. Fixed-frame unwind metadata does not make large frames
  safe to execute or constitute a stack-overflow policy.
* **Text:** UTF-8 byte sequences; borrowed text is pointer/count (16 bytes), owned
  string is pointer/count/capacity (24 bytes), alignment eight. Counts exclude a
  terminator. Lifetimes and allocator ownership remain semantic requirements.
  This is implemented for Windows; Unix rich/text runtime operations reject.
* **Printing:** Windows uses byte-counted `GetStdHandle`/`WriteFile` operations,
  without a C stdio buffer. `println` lowers to content plus newline writes, not
  an atomic multi-threaded log record. No synchronization guarantee is implied.
* **Files:** Windows paths convert strict UTF-8 to UTF-16. Covered operations are
  read, exists and write. Reads reject malformed UTF-8; operational read failures
  trap, write failures return `bool`, invalid input/limits trap. There is no
  transactional filesystem or durability guarantee. Reads/writes are bounded to
  64 MiB with 64 KiB transfer chunks. Complete output bytes are tested beyond a
  chunk boundary at O0/O3, including actual Windows PE execution.
* **Input:** stream read-all is bounded to 1 MiB. The native allocation reserves an
  extra byte to detect over-limit input. Oracle/native bounds come from the same
  runtime constants. This bound does not establish UTF-8 validity for arbitrary
  stdin bytes; stream read-all does not promise strict UTF-8 validation.
* **Processes/startup:** Windows supports scalar entry or a UTF-8 argv adapter,
  without CRT global initialization. The process-run adapter rejects empty/NUL
  commands, validates UTF-8 and bounds the command to 32760 bytes; operational
  spawn/wait failure returns -1. Linux static startup accepts no arguments and
  exits with the scalar result. macOS startup is external. No cross-platform
  process environment, cancellation or child-isolation contract is implemented.
* **Environment, threads, synchronization, dynamic libraries:** no language API
  or memory-order/load-unload/lifetime ABI is implemented. Native Windows import
  resolution and externally linked DLLs are not a language dynamic-loader API.

## Conformance and reproducibility evidence

```
paracine selftest
python tools\test_target_contracts.py x64\Release\Paracine.exe
python tools\test_target_contracts.py x64\Release\Paracine.exe --seed 12648430 --cases 256 --modules 128 --stress-rounds 20
```

The embedded suite includes seeded lexer/parser edits with exact lossless source
reconstruction and token/span comparisons against a full lex, transactional
rejected edits, and 192 IR mutations of registers, branches, terminators, opcodes
and CFG identities. Unknown opcodes and duplicate block identities fail before
optimizer/backend consumption.

The Python campaign adds strict contract-schema checks, unsupported-target/output
preservation tests, compile-fail diagnostics, newline variants, small
application-style corpus programs, independent expected results plus PCIR/native
O0/O3 comparison, mutated malformed input, three-format worker-count comparisons,
malformed binary truncations, multi-module projects, checkout-path reproducibility,
same-size/same-timestamp import invalidation, corrupt-cache repair on Windows and
bounded repeated builds. Every subprocess has a timeout and failures report the
seed and active source. Seeded runs are reproducible; they are not exhaustive
proofs or coverage-guided fuzzing. Corpus programs are small examples, not evidence
of a large real-world application ecosystem.

ZIP bundle tests independently decode records/CRC with Python's ZIP reader.
Generated archive member paths are restricted to portable ASCII components;
absolute/traversal/control/NUL/device aliases, ASCII case collisions and
file/directory prefix conflicts reject with `E0593`. This is a safe-name writer
policy, not an archive extraction sandbox, cryptographic signature or malware
scanner. Existing package selftests cover integrity, namespace ownership, locks,
offline restore, corruption and bounded parallel installs; no network credentials
or real registries are required for these tests.

`.github\workflows\conformance.yml` defines Windows, Linux x86-64 and macOS ARM64
**host** builds, cross-emission checks for the three implemented x86-64 formats,
and explicit ARM64 rejection checks. Scheduled jobs request a larger seeded
campaign. Windows additionally runs the full embedded suite. A macOS ARM64 host
can compile the compiler and inspect x64 artifacts; it does not thereby gain a
native ARM64 backend. Native/PCIR execution is reported unavailable on unsupported
hosts, rather than counted as executed tests. CI configuration still needs actual
runner execution; local Windows verification does not establish cross-host or
cross-machine success.
