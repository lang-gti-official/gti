# Language Coverage Corpus — Findings

The corpus in this directory exists to compile a large amount of *valid* GTI
across as much syntax and stdlib surface as possible, on the premise that
much of the language has never been put through the pipeline together. It is
not a demonstration set; `examples/` owns that role. Programs here are chosen
for combinatorial reach, not readability.

Run it with:

    tests/coverage/run_coverage.sh build/gti

Outcomes are classified as pass, ICE (`GTI-B0001`), link failure (the GTI
front end accepts the program and the C++ linker then reports an undefined
generated symbol), silent failure (non-zero exit with no diagnostic), or
rejection.

## Result at time of writing

49 programs: **38 pass, 8 ICE, 1 link failure, 2 silent failures**.

A second script, `scaffold_probe.sh`, probes stdlib operations the surface
manifest marks as unimplemented and records how each one actually fails
(finding F7).

Every finding below is reproduced by a self-contained program in
`reproducers/`. None of them are fixed here — this pass is about locating
defects, not repairing them.

---

## F1 — Silent compile failure: range-for over `std::vector<T>` in a generic function

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

## F2 — ICE: generic callable parameter invoked with a by-reference predicate

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

## F3 — ICE: `?` propagation operator inside a loop body

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

## F4 — ICE: stdlib algorithms with a class functor

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

## F5 — ICE: nested generic unwrapped through chained reference-returning accessors

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

## F6 — Wrong diagnostic: file-scope `static_assert` over a file-scope `constexpr`

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

Corpus program: `c13_constexpr_static_assert` (passes only because it
declares a class).

---

## F7 — Unimplemented stdlib operations fail as C++ linker errors, never as GTI diagnostics

`reproducers/f7-scaffold-operation-link-error.gti`, `scaffold_probe.sh`

`tests/stdlib_surface_manifest.json` already records, per operation, which
parts of the standard library have no GTI body — **46 operations** across
thirteen headers, with `<std/optional>` and `<std/list>` marked scaffold in
their entirety and no executable entry at all. The information needed to
diagnose these calls exists, in machine-readable form, in the repository.

Calling one anyway produces this:

```
gti: native C++ compiler diagnostics:
Undefined symbols for architecture arm64:
  "__gti_program::__gti_std::optional<int>::__gti_fn_73_has_value() const",
      referenced from: ...
ld: symbol(s) not found for architecture arm64
```

The user is shown a mangled C++ symbol from generated code they never wrote,
for a header the standard library ships and the documentation lists.

`scaffold_probe.sh` runs eleven representative operations:

| Outcome | Count | Examples |
| --- | --- | --- |
| Linker error, C++ mangled symbol | 9 | `std::sqrt`, `std::floor`, `std::swap`, `std::exchange`, `std::string::swap`, `std::vector::swap`, `std::optional`, `std::list`, `std::hash<int32_t>` |
| ICE (`GTI-B0001`) | 1 | `std::make_shared` |
| Compiles and runs | 2 | `std::sort`, `std::transform` |

**No probe produces a GTI-level diagnostic.** There is no "this operation is
not implemented yet" error anywhere in the set.

Two observations beyond the missing diagnostic:

- The scaffolded surface includes what most programs reach for first. All of
  `std::sqrt`, `std::pow`, `std::sin`, `std::cos`, `std::floor`, `std::ceil`,
  `std::round` and the rest of the `<std/cmath>` arithmetic are bodyless, as
  are `std::swap`, `std::exchange`, `std::shared_ptr`, and every member of
  `std::optional`. Anyone writing ordinary code hits one of these early, and
  what they get back is a linker error.
- The last row is a manifest accuracy problem in the other direction:
  `std::sort` and `std::transform` are listed as scaffold but have bodies and
  run correctly. The manifest overstates what is missing as well as failing to
  act on what is.

A diagnostic naming the operation and its manifest status would convert the
worst failure mode in the standard library into an ordinary, actionable error.

## F8 — ICE: borrowing a view from a by-value returned owner

`reproducers/f8-view-from-returned-owner.gti`

```gti
mut std::filesystem::path p = std::filesystem::path("/a/b/thing.txt");
std::println(p.string().view());
```

```
error[GTI-B0001]: failed to construct valid MIR.
write of a place conflicts with active read-only loan
```

Binding the intermediate to a name first — `mut std::string s = p.string();`
then `s.view()` — compiles and runs.

Note what the message says. That text is a *borrow-checker diagnostic*, and a
reasonable one; it is simply being raised as an internal compiler error
instead of being reported against the source. Whichever answer is correct here
(the temporary lives to the end of the full expression, so this is arguably
well-formed), an ICE is not it.

The same shape over a user-defined class returning a plain value is fine, so
this is specific to borrowing from a returned heap-owning temporary.

Corpus program: `c32_filesystem_paths`.

## F9 — ICE: `?` propagation applied to a method call

`reproducers/f9-propagation-on-method-call.gti`

```gti
expected<int32_t, e> use() {
  mut Src s = Src();
  int32_t a = ?s.take();   // ICE
  return a;
}
```

The propagation operator works on a free function call and fails on a method
call, for any receiver — a local value, a reference, or a `unique_ptr`
through `->`. Two `?` operators on free functions in one body are fine, so
this is the receiver, not the count.

Together with F3 (`?` inside a loop), the two working shapes for `?` are
straight-line code over free functions. Every stdlib type that reports errors
through `expected` does so from methods — `FILE::write`, `FILE::read_some`,
`FILE::seek`, `FILE::tell` — so none of them can be used with `?`.

Corpus program: `c33_file_io`.

## F10 — Assignment through a method returning `mut T&` is rejected by the parser

`reproducers/f10-assign-through-mut-ref-method.gti`

```gti
class Holder {
  mut int32_t n = 0;
public:
  Holder() {}
  mut int32_t& slot() mut { return this.n; }
};

h.slot() = 4;   // error[GTI-P0001]: Invalid assignment target.
```

`operator[]` returning `mut T&` **is** an assignable target; a named method
returning the identical type is not. The rejection is `GTI-P0001`, from the
parser, so no type information is consulted — a call expression is simply not
accepted on the left of `=`.

This makes a large part of the standard library's mutable surface
unreachable. `std::array::at`, `std::vector::at`, `front`, `back`, and
`std::optional::value` all return `mut T&` and none can be assigned through:

```gti
a.at(std::size_t(0)) = 5;   // rejected
a[std::size_t(0)] = 5;      // accepted
```

Corpus program: `c38_array_span` (works only by avoiding `at`).

## F11 — ICE: a generic instantiated over its own instantiation

`reproducers/f11-pair-nested-in-pair.gti`

```gti
mut auto inner = std::make_pair(1, 2);
mut auto nested = std::make_pair(std::move(inner), 3);   // ICE
```

Emitter-vocabulary ICE. `std::vector<std::pair<int32_t, int32_t>>` is fine, so
it is not generic nesting in general — it is the same generic appearing inside
itself. Naming the inner value first does not help.

This is likely the same underlying defect as F5 (`Box<Box<T>>`), reached
through construction rather than through chained accessors.

Corpus program: `c36_utility_pairs`.

---

## F12 — ICE: returning a `std::span` parameter by plain copy

`reproducers/f12-span-returned-by-plain-copy.gti`

```gti
std::span<int32_t> relay(std::span<int32_t> view) { return view; }
```

Emitter-vocabulary ICE. Writing `return std::move(view);` instead compiles and
runs. A span is a copyable view, so the plain return is the natural spelling —
and it is the one that fails. Generic and non-generic forms both reproduce it.

This one is worth dwelling on. `tests/fixtures/span_owner_dependent.gti` is a
shipped fixture covering exactly this function shape, and it passes because it
happens to be written `return std::move(view);`. Changing that one word to
`return view;` makes the compiler ICE. The construct is covered, the test is
green, and the defect is still there — which is the premise this corpus was
built to test.

Corpus program: `c46_span_views`.

## F13 — Link failure: `std::string`'s constructor in a member initializer list

`reproducers/f13-string-ctor-in-member-init-list.gti`

```gti
class Holder {
  mut std::string s;
public:
  Holder() : s(std::string("lit")) {}
};
```

```
Undefined symbols for architecture arm64:
  "__gti_program::__gti_std::string::string(
       std::__1::basic_string_view<char, std::__1::char_traits<char>>)"
```

The same constructor call inside a function body is fine, and assigning to the
field in the constructor body is fine. Other types in a member initializer
list are fine — a user-defined class works, and so does
`std::vector<int32_t>()`. It is `std::string`'s constructor, reached from an
initializer list, that emits a call to a symbol that is never defined.

Member initializer lists are the idiomatic way to initialize fields, and a
`std::string` field is about as common as fields get.

Corpus program: `c48_ownership_breadth`.

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
- `std::upcast_unique` does not deduce `Derived`; a `unique_ptr<Derived>` needs
  an explicit `std::upcast_unique<Base, Derived>(std::move(p))` to become a
  `unique_ptr<Base>`.
- Implementing an interface requires both `public` on the base and `override`
  on every method. Both are diagnosed clearly (GTI-S2040, GTI-S2042).
- Configuration flags are valueless: `#define TIER 2` is rejected by GTI-I0011,
  which directs the user to a typed `constexpr` selected by `#ifdef`.
- Preprocessor directives cannot be indented after the `#`; `#  if` is
  GTI-L0003.
- `std::find` and `std::count` take the searched value as `T&`, so a literal
  argument is rejected (GTI-S2017) and must be bound to a name first.
- `std::fopen` yields `unique_ptr<FILE>`, so its members are reached with `->`.
- `std::string` has no `substr`.
- `Type{...}` is valid only as direct initialization (`mut Point a{5};`), never
  after `=`. `mut Point b = Point{6};` is a parse error.
- A `[[c_abi]]` record cannot be constructed in GTI at all: it may not declare
  constructors and its fields may not have initializers (both GTI-S2064), while
  a struct variable requires explicit construction (GTI-S2000). The design
  intent is that these records come from a native factory, which the GTI-S2064
  message states — but the messages you actually hit first do not, and one of
  them (GTI-S2038, "place the braces directly") points at a form that then
  fails for a different reason. `examples/48-native-c-records.gti` only queries
  layout, so it never meets this.
- Payload enums cannot carry a type needing cleanup, such as `std::string`
  (GTI-S2067); variant-aware drop state is not implemented.
- Payload enums cannot be generic; `enum class Result<T>` is a parse error.
- `sizeof` accepts only a type, not an expression.
- `sizeof...(pack)` is not supported.
- A fixed array binding must be declared `mut` before its elements can be
  assigned.

## Coverage gaps still open

Not yet exercised by this corpus, in rough order of expected yield: native C
records and bridge headers, `tcp`, `<std/random>` distribution surface,
generic classes with value (non-type) parameters, and variadic packs beyond
`emplace`.

Two headers cannot be exercised at all until F7's underlying work lands:
`<std/optional>` and `<std/list>` have no executable operations.
