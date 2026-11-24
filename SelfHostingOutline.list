paracine-selfhost/
├── paracine.toml                       # Root build manifest (edition = "2026" | "2027", Tier-1 msx64)
├── paracine.lock                       # Pinned local/trusted dependencies & SHA-256 digests
├── src/
│   ├── main.pcn                        # CLI router: check, build, test, sage, polr, obj, link, fmt
│   │
│   ├── support/                        # Shared primitives & bounded budgets (no semantic authority)
│   │   ├── budgets.pcn                 # Nesting caps (128 layout / 256 group), E-graph/POLR/search limits
│   │   ├── spans.pcn                   # Source spans, line/col coordinates, byte offsets, content hashes
│   │   ├── diagnostics.pcn             # Structured diagnostic records & paracine.diagnostics.v1 JSON
│   │   └── cache_store.pcn             # Content-addressed SHA-256 artifact cache & atomic staging
│   │
│   ├── lex/                            # Part I §3 & Appendix A.1–A.2 (Lexical + 4-space layout)
│   │   ├── utf8_nfc.pcn                # Strict UTF-8 scalar check, BOM/CRLF/LF, NFC & Hangul norm
│   │   ├── layout.pcn                  # Purely lexical 4-space stack, INDENT/DEDENT/NEWLINE, -> lines
│   │   ├── tokens.pcn                  # Token kinds, 2026/2027 keywords, literals, TokenEditDelta
│   │   └── lexer.pcn                   # Full lexer & safe incremental restart boundary logic
│   │
│   ├── syntax/                         # Appendix A.2–A.3 & Part II §18 (CST, AST, Parser)
│   │   ├── cst.pcn                     # Lossless CST with trivia & exact source reconstruction
│   │   ├── ast.pcn                     # Typed AST nodes, expressions, patterns, loops, declarations
│   │   ├── parser_2026.pcn             # Frozen Edition-2026 grammar & localized error recovery
│   │   ├── parser_2027.pcn             # Edition-gated 2027 constructs (capsule, family, chain, tx)
│   │   └── ast_diagram.pcn             # Human-readable AST hierarchy & source formatter (fmt)
│   │
│   ├── sage/                           # Parts IV–V & §6 (Authoritative SKM/VSJ, Blueprint, POLR, SCM)
│   │   ├── skm_types.pcn               # Canonical types, generics, capabilities, layouts, niches
│   │   ├── resolver.pcn                # Acyclic module/import graph, visibility, name/overload solver
│   │   ├── ownership.pcn               # Moves, partial moves, shared/var borrows, nonlexical regions
│   │   ├── effects_failure.pcn         # alloc/io/thread/trap/unsafe, result/option, no-unwind traps
│   │   ├── constructs_2027.pcn         # Capsule isolation (logical/memory/deep), Family, Chain, tx/state
│   │   ├── reflection_num.pcn          # bitint<N>, decimal, complex, posit, reflect<T>, @derive checks
│   │   ├── vsj_verifier.pcn            # Formal judgments & proof-obligation discharge (gates/checks)
│   │   ├── skm_graph.pcn               # 19-domain SAGEKnowledgeEntity & SAGEKnowledgeRelation store
│   │   ├── blueprint.pcn               # Whole-Program Blueprint: forward/reverse nav & change impact
│   │   ├── scm_conjectures.pcn         # Speculative Conjecture Model (unverified candidates only)
│   │   ├── advisory_lexicon.pcn        # Thesaurus logic & etymological lattice (strictly non-authoritative)
│   │   └── polr_engine.pcn             # Multi-dimensional cost vector, Pareto frontier & 9 profiles
│   │
│   ├── pitel/                          # Part VI §§30–31 (Progressive throughput & parallelism)
│   │   ├── stages.pcn                  # 6-stage progressive pipeline orchestration (Stages I–VI)
│   │   ├── invalidation.pcn            # Dependency-directed fact & proof cache invalidation
│   │   └── worker_pool.pcn             # Deterministic parallel execution & budget/precision fallback
│   │
│   ├── ir/                             # Part I §7 & Part III §§19–21 (PCIR, Oracle, ACO, PMIR)
│   │   ├── pcir_ir.pcn                 # Typed SSA-like PCIR instructions, provenance & effect tags
│   │   ├── pcir_verifier.pcn           # Pre/post-optimization PCIR structural & semantic verifier
│   │   ├── pcir_interpreter.pcn        # Independent PCIR reference execution oracle (Gate G18)
│   │   ├── aco_egraph.pcn              # Typed effect/provenance E-graph & equality saturation
│   │   ├── aco_transforms.pcn          # Closure scalarization, chain/loop fusion, refcount/drop checks
│   │   ├── pmir_ir.pcn                 # Target-aware PMIR, register classes, frames & stack probing
│   │   └── pmir_regalloc.pcn           # Register allocation, coalescing, spilling & ABI lowering
│   │
│   ├── backend/                        # Part I §§7.3–9 & Appendix A.9–A.15 (x86-64, COFF, PE32+, Link)
│   │   ├── abi_msx64.pcn               # Microsoft x64 ABI, aggregate/niche layouts, unwind (.pdata/.xdata)
│   │   ├── x86_64_encoder.pcn          # Direct x86-64 machine code encoder (REX/VEX/EVEX/ModRM/SIB)
│   │   ├── x86_64_decoder.pcn          # Validating x86-64 instruction decoder for asm/audit checks
│   │   ├── coff_obj.pcn                # COFF / bigobj reader/writer, symbols, COMDAT, relocations
│   │   ├── pe32_linker.pcn             # Integrated PE32+ .exe/.dll/.lib linker, ASLR/CFG/TLS/rsrc
│   │   ├── debug_info.pcn              # CodeView / PDB (-g0/-g1/-g2) & binary verifier (verify-bin)
│   │   └── archive_pkg.pcn             # Deterministic ZIP32 bundler, package lock/cache & registry ops
│   │
│   └── runtime/                        # Part I §§8–9 & Appendix A.11 (Tier-1 Windows runtime support)
│       ├── win_startup.pcn             # pcn_start / pcn_dll_start, UTF-16 <-> UTF-8 args, no CRT
│       ├── dyn_closure_rt.pcn          # Explicit dyn_closure<Args,R> & box_closure disposal runtime
│       └── capsule_isolation.pcn       # Isolation profile checks & fail-closed enforcement hooks
│
└── tests/                              # Gates G01–G24 conformance, differential & fuzz suites
    ├── lex_layout/                     # G01: UTF-8, NFC, 4-space layout, indentation errors
    ├── types_ownership/                # G02–G03: borrows, lifetimes, effects, traps, defer
    ├── constructs_2027/                # G04–G12: Capsules, Families, Chains, checkpoints, numerics
    ├── sage_polr_pitel/                # G13–G17, G21–G24: ACO, Blueprint, POLR, PITEL, cost honesty
    ├── native_oracle/                  # G18–G20: PCIR oracle vs O0–O3 native, COFF/PE32+, determinism
    └── fuzz_hostile/                   # Malformed binaries, budget exhaustion, negative diagnostics

How Modules Are Grouped by Responsibility:

Directory Primary Role Key Boundary Rule
support/ Cross-cutting spans, budgets, diagnostics, and SHA-256 cache utilities Must not introduce semantic rules or alter program meaning.
lex/ & syntax/ Unicode/NFC normalization, 4-space layout, lossless CST, AST, and edition-gated parsing Keep Edition-2026 rules untouched when edition = "2026" is selected.
sage/ Authoritative SKM/VSJ analysis, 19-domain knowledge graph, Blueprint, POLR, and advisory modules Never let scm_conjectures, advisory_lexicon, or polr_engine bypass vsj_verifier.
pitel/ Progressive 6-stage scheduling, dependency invalidation, and deterministic parallelism Accelerates work safely without changing semantic results or diagnostic order.
ir/ Typed PCIR, verifier, reference interpreter oracle, ACO E-graphs, and PMIR Every optimization pass must preserve PCIR verification and oracle equivalence.
backend/ & runtime/ x86-64 encoding/decoding, COFF/PE32+ linking, PDB/ZIP32/packaging, and Tier-1 runtime hooks isolate deep must fail closed if target enforcement is absent.
