# Language Coverage Corpus — Findings

The corpus in this directory exists to compile a large amount of *valid* GTI
across as much syntax and stdlib surface as possible, on the premise that
much of the language has never been put through the pipeline together. It is
not a demonstration set; `examples/` owns that role. Programs here are chosen
for combinatorial reach, not readability.

Run it with:

    tests/coverage/run_coverage.sh build/gti

Outcomes are classified as pass, ICE (`GTI-B0001`), silent failure (non-zero
exit with no diagnostic), or rejection.

## Integrated result

30 programs: **30 pass, 0 ICE, 0 silent failures, 0 rejections**.

The submitted corpus initially measured **24 pass, 4 ICE, 2 silent
failures**. Every historical finding below remains reproduced by a
self-contained program in `reproducers/`, and every reproducer now compiles
and runs. The corpus is registered as the `language_coverage_corpus` CTest so
these combinations remain release-gated.

| Finding | Resolution |
| --- | --- |
| F1 | HIR verification now compares concrete generic range facts and stable loan provenance rather than declaration-level types and per-analysis loan IDs. |
| F2 | Already fixed on the integration base by exact generic callable-reference lowering. |
| F3 | MIR full-expression verification distinguishes a loop's next dynamic evaluation from two reachable markers in one evaluation. |
| F4 | The C++ backend admits an adjacent, failure-free class prvalue at an exact confined callable parameter. |
| F5 | The backend retains a verified failure-free nested constructor/reference-return chain without manufacturing an owning local. |
| F6 | Semantic statement routing now attributes file-scope `static_assert` to its own source unit while retaining direct-include diagnostics for genuinely hidden declarations. |

The detailed sections below preserve the original symptoms and reductions as
regression rationale.

---

## F1 — Resolved: range-for over `std::vector<T>` in a generic function

`reproducers/f1-generic-range-for-silent.gti`

```gti
int32_t count_all<std::copyable T>(std::vector<T>& values) {
  mut int32_t total = 0;
  for (auto& v : values) { total = total + 1; }
  return total;
}
```

The driver exits **65 with no diagnostic on stdout or stderr** and writes no
artifact. Nothing tells the user what went wrong.

This is the most serious finding in the set for two reasons. A silent
non-zero exit is unconditionally a compiler defect regardless of whether the
program should be accepted, and the construct is completely ordinary — a
range-based `for` over a container parameter in a generic function is the
first thing anyone writes when they factor a loop into a reusable helper.

Scope, established by bisection:

| Shape | Result |
| --- | --- |
| Range-for over `std::vector<T>` in a generic function (parameter) | silent exit 65 |
| Range-for over `std::vector<T>` in a generic function (local) | silent exit 65 |
| Range-for over `std::vector<int32_t>` in a non-generic function | OK |
| Index loop over `std::vector<T>` in a generic function | OK |

The constraint on `T` is irrelevant; `std::copyable` and `std::numeric` both
fail. The generic context and the range-for are jointly required.

Corpus programs: `c29_generic_constraints_mix`, `c30_generic_range_for`.

## F2 — Resolved: generic callable parameter invoked with a by-reference predicate

`reproducers/f2-generic-callable-by-reference.gti`

```gti
std::vector<T> filtered<T, Predicate>(std::vector<T>& source, mut Predicate keep)
  requires std::copyable<T> { /* ... if (keep(element)) ... */ }

auto keep = [](int32_t& x) -> bool { return x > 0; };
mut auto out = filtered(v, std::move(keep));
```

```
error[GTI-B0001]: backend 'cpp' failed while generating an artifact:
verified MIR body is outside the complete C++ text vocabulary
for '::__gti_program::__gti_fn_300_filtered'
```

The discriminator is the **callable's parameter mode**, not the call site and
not the element type: an identical program whose lambda takes `int32_t x` by
value compiles and runs. Both a class element type and a built-in element
type reproduce it.

Corpus program: `c01_generic_container_pipeline`.

## F3 — Resolved: `?` propagation operator inside a loop body

`reproducers/f3-propagation-in-loop.gti`

```gti
expected<int32_t, parse_errc> sum_three() {
  mut int32_t total = 0;
  for (mut int32_t i = 0; i < 3; i++) {
    int32_t d = ?digit_value(i);
    total = total + d;
  }
  return total;
}
```

```
error[GTI-B0001]: failed to construct valid MIR.
full-expression boundary markers are not on mutually exclusive control-flow
paths (body owner 146, block 17, instruction 33).
program entry is missing hosted-startup MIR authority
```

The same `?digit_value(...)` in straight-line code lowers correctly; only the
loop body fails. Note the second sentence of the diagnostic — a spurious
"program entry is missing hosted-startup MIR authority" is reported alongside
the real failure, which will mislead anyone triaging it.

Error propagation inside a loop is the primary reason `expected` and `?`
exist, so this closes off most practical use of the feature.

Corpus program: `c03_expected_error_flow`.

## F4 — Resolved: stdlib algorithms with a class functor

`reproducers/f4-algorithm-class-functor.gti`

```gti
class Positive {
public:
  Positive() {}
  bool operator()(int32_t value) { return value > 1; }
};

std::ptrdiff_t big = std::count_if(values.begin(), values.end(), Positive());
```

Same "outside the complete C++ text vocabulary" ICE, this time naming
`__gti_entry`.

The same algorithm called with an equivalent **lambda** compiles and runs, so
user-defined function objects are simply not usable with the algorithm
surface. Statefulness is not the trigger — a stateless functor with a
non-`mut` `operator()` reproduces it just as reliably as a `mut` one, so this
is distinct from F2 despite the shared diagnostic text.

Corpus program: `c08_lambda_captures`.

## F5 — Resolved: nested generic unwrapped through chained reference-returning accessors

`reproducers/f5-nested-generic-chained-get.gti`

```gti
class Box<T> {
  mut T value;
public:
  Box(T value) : value(std::move(value)) {}
  T& get() { return this.value; }
};

T unwrap_twice<T>(Box<Box<T>>& outer) requires std::copyable<T> {
  return outer.get().get();
}
```

Same emitter-vocabulary ICE. Constructing and reading `Box<Box<int32_t>>`
through a plain field is fine; chaining two reference-returning accessors
inside a generic function is what fails.

Corpus program: `c15_multidim_and_nested_generics`.

## F6 — Resolved: file-scope `static_assert` over a file-scope `constexpr`

`reproducers/f6-file-scope-static-assert.gti`

```gti
constexpr int32_t limit = 8;
static_assert(limit == 8, "limit must be 8");
int main() { std::println(limit); return 0; }
```

```
error[GTI-S2024]: Declaration 'limit' is not visible from this source file;
                 include its file directly.
note: Declaration is in this source unit.
```

The diagnostic contradicts itself inside a single message — it tells the user
to include the file that the accompanying note confirms is already the
current one, and the suggested remedy (a file including itself) is not
actionable.

The visibility result depends on unrelated declarations elsewhere in the file:

| Rest of file | Result |
| --- | --- |
| Nothing else | GTI-S2024 |
| A plain class declared (even unused) | OK |
| A generic class declared (even unused) | OK |
| A generic function declared (even unused) | GTI-S2024 |

`examples/68-static-assertions.gti` passes only because it happens to declare
a class. Anyone writing a header of `constexpr` limits and assertions — with
no class in it — hits this immediately.

Corpus program: `c13_constexpr_static_assert` now exercises the shape without
an unrelated class declaration.

---

## Language behaviour recorded along the way

These are not filed as defects. They are diagnosed correctly and are either
documented limits or deliberate design, but each cost time to discover and is
worth capturing for whoever writes the language reference.

- `char` is neither ordered nor convertible to `int32_t`. Comparisons and
  arithmetic must go through `uint8_t(c)`.
- `uint8_t` arithmetic promotes to `int32_t`, so accumulating into a `uint8_t`
  needs an explicit narrowing cast on store.
- Extracting an owner from `expected` is `std::move(result).value()`. The
  natural-looking `std::move(result.value())` is rejected by GTI-S2018, whose
  message ("cannot partially move a field, indexed element, or borrowed
  place") does not hint at the working form. This one shape accounted for two
  of the corpus rejections.
- Lambda reference captures are rejected; captures are immutable value
  snapshots. The diagnostic is clear.
- `virtual ~X()` is rejected — use `interface`.
- References to fixed arrays are unsupported (GTI-S2017).
- A scoped-enum `switch` covering every enumerator does **not** satisfy
  definite-return analysis, while a payload-enum `switch` does. The
  asymmetry is surprising.
- `std::advance` cannot take a `std::vector` iterator (GTI-S2045, "parameters
  cannot nest borrowed state").
- Class fields must be declared `mut` to be assignable from a `mut` member
  function; there is no whole-object mutability shorthand.
- Managed threads require `--execution-profile concurrent`. Corpus programs
  opt in with a `// @coverage-profile: concurrent` marker comment.
- `std::upcast_unique` does not deduce `Derived`.

## Coverage gaps still open

Not yet exercised by this corpus, in rough order of expected yield:
filesystem paths, `cstdio`, `optional`, native C records and bridge headers,
`tcp`, qualified template specializations, and richer conditional-compilation
nesting.
