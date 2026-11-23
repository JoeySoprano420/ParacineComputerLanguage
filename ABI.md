# Paracine native ABI: development revision 1

This document describes the **implemented development ABI**, not the aspirational
1.0 specification in README.md. Its identity is `paracine-dev-abi-1`. It is not a
promise of compatibility between arbitrary compiler versions, CPU architectures,
C++ exports, or other languages. Unsupported constructs still fail compilation.
The shared target registry and runtime service/failure contract are documented in
[TARGETS.md](TARGETS.md). ARM64 platform identities do not imply ARM64 codegen.

## Inspecting and enforcing the contract

```
paracine abi program.pcn --json -O0
paracine abi program.pcn --target linux-x86_64 --json
```

`paracine.abi.v1` reports the target contract, little-endian 64-bit pointers,
canonical type sizes/alignments, field offsets, reference captures, tag/payload
offsets, nominal discriminants, layout SHA-256 identities, function symbols,
source parameter modes, and hidden-result classification. `supported: false`
marks signatures outside the selected calling profile. The type inventory also
contains local storage types; a layout is not evidence that that type can cross
every target's call boundary. Ordering is deterministic. Offsets and sizes are
bytes. The format is a JSON object; consumers should ignore unknown fields.

COFF objects and Windows executables contain `.pcnabi` JSON. Debug ELF objects
and executables contain the corresponding System V manifest. COFF objects also
carry standard `/FAILIFMISMATCH` directives: one global ABI revision/profile key
and SHA-256 layout keys for nominal records, enums and variants. `lld-link`
rejects different values for a shared key, including changed enum discriminants.
These checks also apply to release COFF objects. They do not authenticate objects,
inspect foreign archives, check every external declaration against a definition,
or make unannotated third-party C objects compatible with rich Paracine values.
An external linker that ignores these directives does not enforce the contract.
ELF manifests are descriptive; there is no equivalent ELF link-time enforcement.

A layout fingerprint covers representation kind, size, alignment, direct versus
indirect classification, signedness, tag/payload positions, contained type names,
closure target, ordered fields with offsets and reference flags, and sorted
nominal tag names/values. Contained nominal types have their own fingerprints.
Consumers must compare the complete reachable type graph and target contract,
not just the size or a single parent fingerprint. Fingerprints are compatibility
checks, not signatures. Private root nominal names are not separately namespaced
by separate compilation: shared spellings must agree.

## Symbol identities and generic specialization

* Root routines retain their source names. Foreign declarations retain their
  native symbol names. These are not C++-style overloaded link identities.
* Imported declarations use `__pcn_module_v1_`, followed by lowercase hexadecimal
  UTF-8 bytes of the logical module identity, `_`, and the declaration name.
  Module identities include canonical package/version identity where applicable;
  physical checkout paths are not the module identity. The prefix is reserved.
* Concrete generics use `_PCG1_` followed by lowercase hexadecimal bytes of the
  compiler's length-framed template identity and concrete type arguments.
  Identity includes the declaration/module owner, signature, parameter modes,
  generic parameters, canonical constraints and sorted effects. The compiler
  canonicalizes concrete type arguments. This is monomorphization, not a runtime
  dictionary ABI. Const-generic specialization is unsupported.
* Specialization growth/recursion/identity limits remain enforced. Separate source
  units with repeated specializations require the checked source-LTO workflow;
  arbitrary COMDAT/ODR deduplication is not implemented.
* Closure-local numbering and synthesized routines are implementation identities,
  not public, cross-unit stable closure symbols.

## Scalar and aggregate storage

All quantities are little-endian. `bool` is one byte (0 or 1). Signed/unsigned
byte, short, int and long families occupy 1, 2, 4 and 8 bytes respectively.
`char` occupies four bytes and represents a Unicode scalar. `size`, `ssize`, raw
pointers and native function addresses occupy eight bytes. `float` and `double`
are IEEE binary32 and binary64. Alignment is the scalar's byte size, at most eight.
`void` has no storage. Native integer virtual-register/frame cells occupy eight
bytes even for a narrower source storage type; aggregate field layout uses the
source type's actual width. Do not equate frame-cell width with record-field width.

Records and tuples place fields in declaration order. Each field begins at the
next multiple of its alignment; final size rounds up to maximum field alignment.
Fixed arrays use contiguous elements with that element's padded size as stride.
Empty nominal rich objects have at least one byte of native backing storage.
Padding is not a semantic field and must not be used for equality, hashing,
serialization or interchange. No packed-record or bitfield ABI is promised.

Enums store an unsigned discriminant at offset zero. The minimum width is 1, 2,
4 or 8 bytes according to the greatest tag value; an explicit storage annotation
may widen it, not truncate it. Implicit enum tags start at zero and follow the
previous value. Duplicate/out-of-range tags fail validation.

Variants use declaration-order tags starting at zero. Tag width accommodates the
case count. Payload starts at `align_up(tag_size, maximum_payload_alignment)`;
payload space is the largest case's storage size, with overall size rounded to
maximum tag/payload alignment. Case fields overlap deliberately. The manifest's
case-qualified field names and `tags` define the active-case interpretation.
Inactive payload bytes have no defined value. No C union compatibility is implied.

## Options and results

* `option<bool>` is one direct byte: false=0, true=1, none=2.
* `option<char>` is a direct four-byte value: none=`0x00110000`, outside the
  Unicode scalar range. Some preserves the scalar value.
* `option<text>`, `option<view<T>>`, `option<span<T>>` and `option<buffer<T>>`
  preserve the underlying rich layout and use a null first pointer for none.
* Other options use a one-byte tag (none=0, some=1), followed by an aligned value.
* `result<T,E>` uses a one-byte tag (ok=0, error=1), followed by a payload aligned
  for both alternatives and large enough for the larger alternative.

For tagged options/results, total size is rounded to payload alignment. Only the
active alternative may be read. Niche validity is a compiler invariant, not an
invitation to construct unchecked values at a foreign boundary. Native Windows x64
move/drop glue recursively handles owned strings inside fixed arrays, tuples and
ordinary records, with a 4096-owned-leaf expansion limit. Owner-bearing tagged
options/results, variants/unions and nested owning closure environments remain
unsupported and are rejected during native legalization rather than shallow-copied.

## Text, references and ownership

Borrowed `text` is `{pointer@0, byte_count@8}`, size 16, alignment 8. Count excludes
any terminator. Storage is UTF-8 and need not be NUL-terminated. The borrower does
not free or extend the lifetime of the bytes. Owned `string` is
`{pointer@0, byte_count@8, capacity@16}`, size 24, alignment 8. Capacity and count
are bytes; moving transfers ownership and invalidates the previous owning value.
Destruction uses the compiler's runtime allocation family, not a foreign allocator.
Allocator interoperability and cross-runtime deallocation are not promised.

`view<T>`, `span<T>`, `ref<T>` and `mut<T>` currently use a 16-byte pointer/count
storage representation. `buffer<T>` uses the 24-byte pointer/count/capacity shape.
This describes current legalization, not a general C reference ABI. The lifetime,
mutability and exclusivity restrictions remain semantic requirements; a matching
bit pattern does not establish a valid borrow or owner. Foreign code must not
retain borrowed pointers or independently free owned runtime allocations.

## Closure environments

A closure environment contains captures only; the call target is statically known
from the concrete closure type (`closure$N$call`). There is no universal
`{code_pointer, environment_pointer}` pair. Captures appear in analyzed capture
order. Owned captures use their value layout. Borrowed captures use one aligned
8-byte referent pointer. Size rounds to maximum capture alignment; an empty
environment still has one byte of backing storage. Invocation expands captures
before explicit parameters. Direct owned-string capture cleanup belongs to the
environment's owning scope, not each invocation. Arbitrary escaping/nested owning
environments and cross-unit closure interchange remain unsupported.

## Calling conventions and stack unwinding

Windows uses Microsoft x64 register positions, 32-byte caller home space,
8-byte overflow argument lanes and 16-byte call-site stack alignment. Scalar
integer/pointer values use positional RCX/RDX/R8/R9 lanes; positional floating
arguments use XMM0..3. Scalar integer results use RAX and floating results XMM0.
Rich parameters use indirect pointers in ordinary argument lanes. Rich results
use a hidden caller-owned result pointer before explicit arguments, consuming the
first lane; the returned address is also placed in RAX. These rich rules are
Paracine rules, not the complete Microsoft C aggregate-classification algorithm.
Parameter modes and ownership remain part of the checked source signature.
For variadic calls, first-four-lane floating arguments are duplicated in the
corresponding GP registers, including named fixed floating parameters. Only
post-ellipsis float arguments receive float-to-double promotion. Shared argument
classification controls ordinary Windows call emission and System V parameter
and call placement; native probes verify the Windows register duplication.

Linux/macOS x86-64 currently implement a **restricted System V scalar profile**:
independent RDI/RSI/RDX/RCX/R8/R9 and XMM0..7 argument banks, ordered stack overflow
arguments, scalar return registers and supported C variadic promotions/AL counts.
Rich/reference call boundaries, platform text/ownership runtimes and arbitrary
aggregate classification are unsupported. Rich local fixed-layout operations do
not change that restriction. Other CPU architectures are not covered.

Native routines preserve RBP, allocate a fixed frame and use recognized
`add rsp, frame; pop rbp; ret` epilogues. Windows PE/PDB builds have address-ordered
runtime-function entries and allocation/push unwind codes, including the argv
adapter and both large-allocation encodings. Tests invoke `RtlVirtualUnwind` at
prologue/body/epilogue boundaries for frames up to 1 MiB. This verifies metadata,
not safe execution of arbitrarily large unprobed stacks. Stack probing/overflow
hardening and exception-based cleanup remain separate limitations. Debug Linux
artifacts contain DWARF `.debug_frame` CFI; they do not contain runtime `.eh_frame`
or implement language exception propagation. Language traps do not unwind owned
values as C++ exceptions would.

## Native debug profile

```
paracine build program.pcn -g -O0 -o program.exe
paracine obj program.pcn -g -O0 -o program.obj
paracine link program.pcn library.obj -g -O0 --linker lld-link -o program.exe
paracine build program.pcn --target linux-x86_64 -g -O0 -o program.elf
paracine obj program.pcn --target linux-x86_64 -g -O0 -o program.o
```

Windows debug objects contain CodeView types, function scopes, SHA-256 checksums
of actual source file bytes, line/column contributions and bounded scalar local
locations. Debug executables use installed `lld-link` (or `--debug-linker PATH`)
to produce an ordinary PE with an RSDS reference and companion full PDB. No PDB
writer or debugger is bundled. Win32 imports require an SDK-configured environment;
use `link --libpath/--defaultlib` for explicit library selection. Files and source
paths may contain directory spaces. Package/module filename policy still applies.

Linux debug objects and static executables contain DWARF 4 compilation units,
source files, final machine-address line tables, storage-oriented type/member
DIEs, subprograms, bounded scalar variable location lists and call-frame records.
Absolute debug relocations survive linking with `ld.lld`. Source byte columns are
1-based after compiler newline/BOM normalization, not visual grapheme columns.

At `-O0`, ordinary named scalar parameters and let/var bindings have proven stable
frame-cell locations only on covered source instruction ranges after initialization
and within the lexical scope. Rich values, non-shared reference parameters,
synthesized loop bindings and values lacking a proven location are unavailable.
At higher optimization levels all source variable locations are conservatively
unavailable; names and types remain. Surviving optimized instructions retain
source locations, but eliminated statements are not guaranteed breakpoint sites.
Full inline call stacks, expression-piece/register tracking, active-variant
visualizers, macro frames and optimized local reconstruction are not implemented.
This is not a complete optimized-debugging pipeline for every language construct.

`-g` on Mach-O is explicitly rejected (`E0580`), as is debug source-concatenating
LTO. Debug objects use separate cache identities including canonical source paths
and exact dependency bytes. Debug executable/PDB pair caching is rejected rather
than returning an executable without symbols. Debug objects are reproducible for
fixed source paths and configuration regardless of worker count; PDBs contain
linker-specific path/environment records and are not promised reproducible across
machines. Release builds do not gain debug sections implicitly.

Verification: `tools\test_native_debug.py` requires existing LLVM tools and
`pyelftools` as a test-only dependency. On Windows it additionally uses the OS debug
API and DbgHelp to set source breakpoints, inspect actual local values and types,
single-step unoptimized/optimized instructions and walk real symbolic call and crash stacks.
Independent readers decode CodeView, PDB, DWARF and CFI; ELF output is linked but
not executed on Windows. These tests are evidence for this bounded profile, not a
claim that all ordinary debuggers or target operating systems have been validated.
