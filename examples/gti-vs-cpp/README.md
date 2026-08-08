# GTI versus C++ evidence examples

This folder contains paired GTI and C++ programs that demonstrate two claims:

1. GTI deliberately keeps familiar C++-style syntax and value-oriented code.
2. GTI makes selected safety and predictability rules compiler-enforced rather
   than optional conventions.

The examples are designed as reproducible evidence, not universal proof that
GTI is better than C++ in every dimension. Modern C++ can express many of the
same guarantees with `const`, checked APIs, warnings, sanitizers, guidelines,
and careful review. The distinction shown here is that GTI makes these chosen
rules the language default and rejects or checks violations consistently.

No performance claim is made by this suite. Runtime benchmarking belongs to
[`../../docs/performance-tooling-proposal.md`](../../docs/performance-tooling-proposal.md).

## Verified cases

| Case | Shared shape | Evidence |
| --- | --- | --- |
| `01-familiar-control-flow` | typed function, fixed array, `for`, `if`, and `return` | both programs compile and return success; GTI adds `mut` only where state changes |
| `02-immutable-by-default` | ordinary local declaration and assignment | C++ accepts mutation; GTI rejects mutation because bindings are immutable unless marked `mut` |
| `03-exact-call-types` | typed function call | C++ performs an implicit narrowing conversion; GTI rejects the non-exact argument type |
| `04-checked-narrowing` | explicit numeric conversion | C++'s explicit unsigned conversion produces a modulo-reduced value; GTI reports a defined runtime range failure |
| `05-bounds-checked-arrays` | fixed-size array indexing | C++ `operator[]` compiles an unchecked access; GTI reports a defined runtime bounds failure |
| `06-consumed-moves` | `std::unique_ptr`, `std::make_unique`, and `std::move` | C++ permits observing a valid moved-from owner; GTI treats the source binding as consumed and rejects later use |
| `07-explicit-overrides` | interface/base class and virtual method | C++ accepts an implicit override; GTI requires the inherited implementation to say `override` |
| `08-no-switch-fallthrough` | familiar `switch`, `case`, and `default` | C++ permits implicit fallthrough; GTI rejects an executable arm that does not terminate explicitly |

The C++ bounds example is compiled but deliberately not executed because its
out-of-range `operator[]` access would violate the container precondition. The
other C++ programs are safe to run and return zero when their expected behavior
is observed.

## Run the evidence suite

Build GTI first, then run:

```sh
python3 examples/gti-vs-cpp/verify.py
```

The verifier defaults to `build/gti` and the first `c++` compiler on `PATH`.
Override either tool explicitly when needed:

```sh
python3 examples/gti-vs-cpp/verify.py \
  --gti /path/to/gti \
  --cxx /path/to/clang++
```

Run or list selected cases:

```sh
python3 examples/gti-vs-cpp/verify.py --list
python3 examples/gti-vs-cpp/verify.py --case 04-checked-narrowing
```

The runner uses temporary output paths, GTI `-O0`, C++20, native `-O0`, and no
shell evaluation. Each expectation is declared in [`cases.json`](cases.json):
compile success, compile rejection with a diagnostic fragment, successful
execution, or defined runtime failure.

## Read the examples directly

Each numbered directory contains `gti.gti` and `cpp.cpp`. The paired files stay
small and intentionally use comparable names and control flow. Compile one GTI
case manually from the repository root with:

```sh
./build/gti examples/gti-vs-cpp/01-familiar-control-flow/gti.gti \
  -O0 -o /tmp/gti-familiar
/tmp/gti-familiar
```

Compile its C++ counterpart with:

```sh
c++ -std=c++20 -O0 \
  examples/gti-vs-cpp/01-familiar-control-flow/cpp.cpp \
  -o /tmp/cpp-familiar
/tmp/cpp-familiar
```

The GTI rules demonstrated here are specified in
[`../../.agents/skills/gti-language/references/language-contract.md`](../../.agents/skills/gti-language/references/language-contract.md)
and [`../../docs/grammar.ebnf`](../../docs/grammar.ebnf).

## Evidence contract for new cases

A new comparison must:

- state one narrow claim that the current compiler implements;
- keep GTI and C++ source structure comparable where the languages overlap;
- distinguish a safer default from something C++ cannot express;
- declare a machine-verifiable outcome in `cases.json`;
- avoid executing C++ undefined behavior;
- verify GTI behavior at `-O0` so native optimization cannot create the claim;
- avoid performance or portability claims without the benchmark evidence and
  environment metadata required by the performance tooling proposal;
- update this table and keep `verify.py` passing.

These constraints keep the showcase credible as GTI evolves: an example that
stops matching implemented semantics fails visibly instead of remaining as
stale marketing text.
