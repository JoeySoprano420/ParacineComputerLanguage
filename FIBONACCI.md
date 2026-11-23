# Boolean inversion and Fibonacci intrinsics

## Boolean inversion

Boolean negation is available as either `!value` or `not value`. Both require a `bool` operand and produce a `bool`. They can be used in conditions and assignments, including state toggles such as `is_open = !is_open`. Boolean values retain ordinary logical semantics; active-low hardware signaling is a representation convention and does not change the language's `bool` type.

## Signed 64-bit Fibonacci

`fib(index)` is a runtime intrinsic with the signature `fib(index: long) -> long`. It returns exact integer values and uses the standard extension to negative indices:

- `F(0) = 0`, `F(1) = 1`
- `F(-n) = (-1)^(n+1) * F(n)`
- The supported index range is `-92..92`, because `F(92)` fits in signed 64-bit while `F(93)` does not.

A runtime index outside that range traps; a top-level constant using an out-of-range index is rejected during compilation. No floating-point approximation is used. Runtime evaluation takes at most 91 additions. Calls to `fib` in top-level constant initializers are evaluated during compilation; calls in routine bodies remain runtime operations, even when their argument is a literal.

## Compile-time arbitrary precision

`fib_big(index)` is an opt-in intrinsic for top-level constants only and produces decimal UTF-8 `text`:

```pcn
const F500: text = fib_big(500)
const NEGATIVE: text = fib_big(-10)
```

The result uses exact arbitrary-precision integer arithmetic and the same negative-index rule. Compile-time indices are limited to `-10000..10000` to bound compiler resource use; the result itself is not limited to 64 bits. `fib_big` in a runtime expression is diagnosed; use `fib` when the index is only known at runtime.

The arbitrary-precision constant evaluator uses fast doubling and never evaluates Binet's formula with floating-point values. It returns decimal text rather than introducing a general-purpose runtime big-integer type.