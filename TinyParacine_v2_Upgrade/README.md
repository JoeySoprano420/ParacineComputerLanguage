# TinyParacine v2 — all-Paracine subset compiler

The entire Stage-1 frontend is in `tiny_compiler.pcn` (no PowerShell, C++ or other language in its source).
It compiles a deliberately restricted Paracine program embedded in the `let input: text = "..."` statement in `main()` and prints an optimized Paracine file to standard output. The existing Stage-0 Paracine compiler still assembles the final Windows PE32+ program.

## Small additions since v1

- `var name: int = expression` for compile-time integer variables.
- `name = expression`, `+=`, `-=`, `*=`, `/=`, `%=` for variables declared with `var`.
- `let` remains immutable: trying to assign to it reports `E15`.
- Whole-line and trailing `#` comments and whitespace-only lines are accepted.
- Previously supported precedence, unary operators, four-slot symbol table, errors and constant folding are retained.

**Limits:** One `main()` routine, no user-defined functions, four names maximum, signed integer expression results constrained to +/- 2,147,483,647, and no runtime-variable or control-flow code generation. This is a compile-time expression evaluator that emits a constant-return Paracine program, not a self-hosting native backend. File input is embedded; copy example content into the quoted string in `main()` with `\\n` escapes.

## Windows commands (using your working Stage-0 Paracine)

```cmd
Paracine tiny_compiler.pcn -o tiny_compiler.exe
tiny_compiler.exe > generated.pcn
Paracine generated.pcn -o generated.exe
generated.exe
echo Return code: %ERRORLEVEL%
```

The included default embedded program has a variable that returns to 42, so the expected generated routine ends with `give 42`. On Linux, the supplied Stage-0 compiler's `test` command can compare emitted x64 execution with the PCIR interpreter.

## Verification (performed in this session)

- Stage-0 semantic and PCIR check passed.
- Stage-0 `test` at `-O0` and `-O3` passed with x64 execution and PCIR oracle agreement.
- 12 valid and invalid input cases, run at both O0 and O3 (24 checks), passed.
- A Windows PE32+ build was generated; its runtime on Windows has not been independently executed here.
