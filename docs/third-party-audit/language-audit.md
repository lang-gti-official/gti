# Third-Party Language Architecture Audit

> **Status:** External review of the GTI *language*, not the compiler that
> implements it. Not canonical. Nothing here defines GTI semantics; the
> specification in [`docs/language/`](../language/index.md) does. Proposed work
> belongs under `docs/plans/` once accepted.

Reviewed at `origin/main` commit `5f4666c` (`VERSION` 0.92.0).
Review type: read-only. No source file was modified.
Companion documents: [`audit.md`](audit.md) (compiler architecture),
[`llvm-audit.md`](llvm-audit.md), [`implementation-plan.md`](implementation-plan.md).

Method: read the specification, then **test every claim against the built
compiler**. Roughly sixty probe programs were compiled; each verdict below
that says "verified" was produced by `gti --emit-cpp`, by running the
resulting binary, or by reading the optimized assembly. Where the
specification and the compiler disagreed, the compiler won, per
[`docs/language/index.md`](../language/index.md).

Every probe was re-run against `5f4666c` after that commit landed binary32
and MIR dominance mid-review; §4.9 is the finding that changed as a result.

---

## 1. Summary

**GTI has built the hard half of a systems language first.** Ownership,
borrows, reborrows, move tracking, deterministic cleanup, checked arithmetic,
and exact overload resolution are the design-heavy, easy-to-get-wrong parts,
and they are done coherently and enforced by the frontend. On safety GTI is
much closer to Rust than to C++, while reading like C++.

What is missing is mostly the *conventional* half: concurrency, allocators,
FFI breadth, sum types, operator overloading, and layout control. These are
well-understood designs requiring substantial but not novel work. That is a
good position — but they are load-bearing for the phrase "systems language,"
and today their absence is the difference between a language that demonstrates
a memory model and one that can write a server, a driver, or an engine.

Three structural observations drive the rest of this review:

1. **The safety story has no proportionate escape hatch.** `unsafe` covers raw
   pointers, but checked arithmetic has no opt-out at all, and the check
   survives `-O2` (§4.1). Every successful safe systems language pairs its
   guarantees with a way to step around them locally. Without one, hot code
   leaves the language rather than opting out inside it.
2. **Users cannot distinguish permanent design from temporary compiler
   limits.** `docs/plans/language-alignment.md` asks this question of itself
   and leaves it open. It is the single most valuable thing GTI could resolve
   before 1.0 (§5.1).
3. **The C++ backend is still visible in the language contract.** Argument
   evaluation order is inherited from the host compiler, and the
   two-argument-overlap borrow rule exists *because* of that (§5.2). A language
   rule that exists to compensate for a backend is not yet a language rule.

---

## 2. Where GTI correctly improves on C++

Every item was verified against the compiler. `REJECT` means GTI rejects a
program C++ accepts (or accepts with undefined behaviour).

### 2.1 Errors C++ cannot catch at all

| Program | GTI | C++ |
| --- | --- | --- |
| Read a moved-from value | `GTI-S2018` "Value 'a' has already been moved" | Compiles; unspecified state |
| Return a reference to a local | `GTI-S2017` | Compiles; dangling |
| Dereference a raw pointer outside `unsafe` | `GTI-S2055` | Compiles |
| Bind a reference to `nullptr` | `GTI-S2017` | Compiles via deref; UB |
| `int32_t` overflow | Runtime trap, exit 134, `"integer addition overflow"` | UB |
| Fixed-array index out of bounds | Runtime trap, `"fixed array index out of bounds"` | UB |

The move tracking is the standout. It is path-sensitive, covers fields and
loop backedges, and — unusually — applies to *copyable* types too, so
`std::move` means one thing everywhere. C++ has no equivalent at any warning
level.

The runtime traps deserve specific credit: they produce a named, actionable
message rather than a silent wrong answer. That is a real improvement over
both C++ UB and a bare `SIGSEGV`.

### 2.2 Defaults inverted in the right direction

| Rule | Verified | Why it is right |
| --- | --- | --- |
| Bindings, parameters, fields, receivers immutable unless `mut` | `GTI-S2002` | C++'s `const`-by-omission is the single most common source of accidental mutation |
| No implicit conversions, including widening | `GTI-S2003` on `int32_t` → `int64_t` argument | Removes the entire integer-promotion hazard class |
| Unused non-`void` result must be discarded explicitly | `GTI-S2009` | `[[nodiscard]]` opt-in inverted to opt-out |
| No switch fallthrough | `GTI-S2037` | Removes a defect class C++ still only warns about |
| Every path must return | `GTI-S2031` | C++ leaves this UB |
| Initializer required | `GTI-S2000` | Removes uninitialized reads by construction |

The conversion rule is the most aggressive and, I think, correct. It costs
ergonomics (`f(int64_t(x))` everywhere) but it eliminates promotion, narrowing,
signedness, and ranking hazards in one stroke — and it is what makes exact
overload resolution possible.

### 2.3 Structural simplifications

- **No textual macros.** Removes C++'s largest tooling and reasoning obstacle.
- **Exact overload resolution** — no ADL, no ranking, no CTAD, no
  return-type overloading. Resolution is decidable by reading the call.
- **Generic bodies are checked before instantiation**, and concrete instances
  are rechecked after substitution. C++ templates diagnose at instantiation,
  which is why template errors are famous. Verified: generic validity is not
  delegated to C++ template machinery.
- **Scoped nominal enums** with no implicit integer conversion.
- **No object slicing**; single state-bearing base plus stateless interfaces
  removes the diamond/virtual-inheritance complex entirely.
- **Deterministic cleanup retained** — GTI keeps RAII, which is C++'s best
  idea, rather than replacing it with GC or defer.

### 2.4 Beyond C++ entirely: the borrow model

GTI implements exclusive reborrow with parent suspension, disjoint-field
coexistence, path-sensitive endpoint selection across `if`/loop/`switch`/
`break`, and one confined stored-reference form for iterators — with **no
lifetime syntax at all**. Origins are inferred from a single eligible
parameter or receiver.

This is genuinely ambitious and it is the language's most distinctive asset.
The no-annotation choice is a real bet: it buys enormous approachability
against Rust, and it pays for that with the expressiveness limits in §4.6.

---

## 3. What "systems language" currently means in practice

Before the gap list, the honest positive scope. GTI today can express:

- value types, generics with value parameters, interfaces and virtual dispatch;
- IEEE-754 binary32 arithmetic with a fully specified contract (§4.9);
- deterministic resource ownership with `unique_ptr` and move-only aggregates;
- a real `std::vector` written *in GTI* over a checked storage capability —
  this is the strongest evidence the model works, since it is ordinary source
  with no compiler privilege;
- read-only iteration, `expected<T, E>` error values, bounded `constexpr`
  evaluation with recursion and control flow;
- audited one-level raw-pointer code under `unsafe` for scalar C calls.

That is a competent applications-with-ownership language. The gaps below are
what separate it from the systems tier it aims at.

---

## 4. What the language lacks

Classified as **blocking** (cannot write ordinary systems software without
it), **major** (forces awkward workarounds), or **ergonomic**.

### 4.1 (blocking) No concurrency, and no memory model

`docs/language/execution.md` §4.9 states it plainly: threads, atomics, and
memory ordering "are not yet part of the language." Verified: `std::thread`,
`std::atomic`, `std::mutex` are all unknown types.

This is the largest single gap, and it is not merely a missing library. A
memory model is a *language* decision that retroactively constrains everything
already built:

- What does `mut` mean across threads? Today's borrow model is single-threaded
  by assumption; `Send`/`Sync`-style reasoning is exactly the kind of thing
  that is painful to retrofit onto an ownership system after 1.0.
- Is a data race ill-formed, UB, or defined? GTI has removed UB everywhere
  else; leaving it here would be inconsistent.
- Does `unique_ptr` cross threads? Does a borrow?

GTI's ownership model is *unusually well positioned* to give strong data-race
guarantees — that is the payoff Rust got from the same foundation. But the
decision has to be made before the borrow rules calcify.

**This should be designed before 1.0 even if it ships after**, because the
answer changes the ownership contract.

### 4.2 (blocking) No layout control and no `sizeof`

Verified rejected: `sizeof`, `alignof`, `alignas`, packed attributes,
bitfields, `union`.

A systems language must let you reason about memory layout. Without these you
cannot:

- match a hardware or wire-format struct;
- write an allocator that honours alignment;
- implement serialization, memory-mapped I/O, or a driver;
- reason about cache-line behaviour.

Note the compiler *now* knows pointer width and endianness in `TargetInfo`
(added during the compiler work), but the **language has no way to ask**. That
is a small, high-value gap to close: `sizeof`/`alignof` as frontend-evaluated
constants would fit the existing bounded-`constexpr` machinery directly.

### 4.3 (blocking) No public allocator model

`gti_internal::allocate_storage` is compiler-private by design. There is no
public allocator interface, no arena, no pool, no per-container allocator
parameter, and no placement construction.

Systems software is largely *about* allocation strategy: arenas for parsers,
pools for entities, stack allocators for frames, custom alignment for SIMD.
Today every GTI container allocates exactly one way.

`docs/plans/language-alignment.md` asks precisely the right question here
("What allocator/provenance/initialization model permits engines and pools
without importing `new`/`delete` hazards?"). It is unanswered, and it blocks
the language's target domain more than any other library gap.

### 4.4 (blocking) FFI cannot bind real C APIs

Verified rejected inside `extern "C"`: struct declarations, function-pointer
parameters, varargs. `void*` is opaque with no round-trip.

The consequence is concrete: GTI cannot bind `qsort`, `printf`, `pthread_*`,
`ioctl`, or any API that passes a struct by value, takes a callback, or
returns a pointer to an aggregate. What works today is scalar-in/scalar-out
plus counted text — enough for `write(2)`, not enough for a platform.

For a language whose stated strategy is native interoperation, this is the gap
most likely to block adoption. `docs/language/native-c-interop.md` is honest
that the surface is bounded; the point here is how much of the real C world
sits outside that bound.

### 4.5 (blocking) No escaping callables or function pointers

Lambdas are lexical and non-escaping; function pointers are ill-formed;
callbacks cannot be stored. That rules out event loops, callback registration,
plugin dispatch, comparator injection, and state machines built from function
tables. Combined with §4.4 it also means C APIs that call back into GTI are
unreachable in both directions.

### 4.6 (major) No sum types or pattern matching

There is no `union`, no enum payload (verified rejected), and no `match`. The
only sum type is `expected<T, E>`.

Systems code is full of tagged unions: tokens, AST nodes, protocol frames,
state machines, instruction encodings. The evidence is close to hand — **GTI's
own compiler uses `std::variant` in C++ for exactly these**, and could not be
written in GTI today.

This interacts well with everything GTI already has: exhaustive `switch` with
no fallthrough is *already* the right matching construct, and move tracking
already understands consuming a value. Payload-carrying enums plus exhaustive
destructuring would be a natural extension rather than a foreign one.

### 4.7 (major) Checked arithmetic has no opt-out

Verified: the overflow branch survives `-O2` in the final ARM64 assembly, and
no wrapping, saturating, or unchecked operation exists anywhere in the
language or standard library.

Checked-by-default is the right default. Having *no* alternative is not:
hashing, checksums, PRNGs, crypto, DSP, and bit-twiddling all require defined
wrapping, and today they must either accept a branch per operation or be
written outside GTI. Rust's `wrapping_add`/`checked_add`/`unchecked_add`
triple is the proven shape.

This is the clearest instance of the missing-escape-hatch theme: GTI made a
safety decision without providing the local, visible way to opt out — which
is exactly what `unsafe` does successfully for pointers.

### 4.8 (major) Operator overloading is a small allowlist

Verified: `operator+` is rejected; the permitted set is `*`, `->`, `++`,
`[]`, call, comparisons, and contextual `bool`.

So user types cannot be arithmetic. No vectors, matrices, complex numbers,
fixed-point, big integers, durations, or units. For a language targeting
engines, graphics, and numerics this is severe, and it is not a small
restriction dressed as simplicity — arithmetic on domain types is a primary
reason systems languages have operators at all.

### 4.9 (major) Only one floating-point width

**The semantics gap is closed.** `docs/language/execution.md` §4.3 now
specifies `float` as IEEE-754 binary32 in full: round-to-nearest ties-to-even
on every operation and conversion, no reassociation or contraction, signed
infinities, quiet NaN on invalid operations, gradual underflow, unordered NaN
comparisons, and a defined float-to-integer rejection-then-truncation rule.
Constant evaluation retains exact binary32 bits rather than computing through
host `double`, and the native driver enforces matching strict-float flags.
This is a complete, well-drafted floating-point contract and it closes the
Milestone 0 blocker that earlier versions of this section would have cited.

What remains is narrower but still real: **there is exactly one float type.**
`double` is not a type and there is no `f64`. A systems language needs both
binary32 and binary64 — double precision is not optional for numerics,
physics, geodesy, financial math, or most scientific code, and it is the
default float width in practically every systems language.

Since the binary32 contract is already written, adding binary64 is now mostly
a matter of applying the same rules at a second width, and the compiler's
`BinaryFloat` representation and `APFloat`-backed evaluator were built in a
way that should extend to it. The literal grammar will also need a way to
spell each width.

There are still no source spellings for infinity or NaN (the specification
says so explicitly), so those values are only reachable through arithmetic or
native calls.

### 4.10 (major) No mutable iteration

Verified: `for (mut auto& e : v)` is rejected — a local borrowed-state value
cannot retain a mutable loan. Reading a container works; modifying its
elements in place does not.

This is the most visible day-to-day consequence of the borrow model's current
staging, and it will be the first thing every new user hits.

### 4.11 (ergonomic, but compounding) Error propagation is manual

`expected<T, E>` works, but there is no `?`/`try` operator (verified). Every
propagation is an explicit four-line check. Combined with the mandatory
result-use rule (§2.2), error-heavy code becomes noticeably verbose — and
verbosity in error paths is how error paths get skipped.

### 4.12 (ergonomic) Assorted confinements

Verified rejected, each with a reasonable current justification but a real
cost: user-defined concepts with `requires` (so no "has method" constraint —
blocking allocator/hash/format abstraction), value generics other than
`uint64_t`, custom copy/move constructor bodies, block-scope `static`,
mutable stored references, global references, `static_assert`, and
integers wider than 64 bits.

---

## 5. Cross-cutting concerns

### 5.1 The permanent/temporary distinction is unresolved

`docs/plans/language-alignment.md` asks: *"Which restrictions are sound design
and which are temporary compiler limits?"* This audit cannot answer it from
outside, and neither can users.

It matters more than any individual gap. A user who hits "no mutable
iteration" cannot tell whether to design around it permanently or wait a
release. A library author cannot tell whether operator restrictions will
loosen. Every restriction in §4 has a different answer, and the answers are
currently distributed across plans, checkpoints, and diagnostic text.

**Recommendation:** publish one language-level ledger — each restriction, its
reason (*design* / *not yet provable* / *not yet lowered*), and its 1.0
disposition. This is documentation work, not implementation, and it would do
more for GTI's credibility as a language than any single feature.

### 5.2 Backend limitations are visible in the language contract

Two rules exist because of the C++ backend, not because of GTI:

- Argument evaluation order is unspecified and inherited from the host
  compiler, contradicting `execution.md` §4.1's own statement that translating
  to C++ "does not import C++ evaluation order."
- The rule that two call arguments may not overlap when one borrows and one
  mutates is explicitly conservative "in both written orders because the
  transitional C++ backend does not yet own native call-argument evaluation
  order."

The second is a *language* restriction users must obey, caused by a *backend*
gap. That inversion should not survive to 1.0: either GTI specifies an
evaluation order and the backend implements it, or the restriction is
acknowledged as permanent design.

### 5.3 Failure is process abort with no contract

Verified: checked failures call `abort()` (exit 134) after a message on
stderr. There is no panic hook, no unwinding, no source location, no exit-code
contract — `execution.md` §4.10 records the specification gap.

For a systems language this has a sharp consequence: **a GTI library cannot be
embedded in a process that must not die.** A server, a plugin, an audio
callback, or a kernel module cannot accept a dependency that aborts on integer
overflow. The library ecosystem question is downstream of this decision.

The compiler already has what it needs (MIR knows the span of each trapping
operation); the missing piece is the language contract for what failure *is*.

### 5.4 No modules, no separate compilation, no freestanding mode

`#include` with load-once, non-textual, graph-based visibility is a genuine
improvement over C++ textual inclusion. But the language has no module
vocabulary, and the toolchain compiles whole programs into a single C++
translation unit with counter-derived symbol names (see `audit.md` §5.1).

Language-level consequences: no binary libraries, no stable ABI, no
incremental builds, and no way to ship a GTI package. There is also no
freestanding/no-runtime mode — the runtime (`println`, file handles) is
assumed — which rules out embedded and kernel targets that are otherwise a
natural fit for the ownership model.

---

## 6. Priorities

If the goal is "proper systems language," these are the ordered gaps. The
ordering weights *how much other work each unblocks*, not implementation cost.

| # | Gap | Why first |
| --- | --- | --- |
| 1 | **Concurrency and memory model** (§4.1) | Retroactively constrains the ownership rules; must be *designed* before they calcify, even if it ships later |
| 2 | **`sizeof`/`alignof` and layout control** (§4.2) | Small, fits existing constexpr machinery, unblocks allocators, FFI, and serialization |
| 3 | **Failure contract** (§5.3) | Decides whether GTI libraries are embeddable at all |
| 4 | **FFI breadth: structs, callbacks, varargs** (§4.4) | The adoption gate for a native-interop language |
| 5 | **Allocator model** (§4.3) | The target domain's defining concern |
| 6 | **Arithmetic escape hatch** (§4.7) | Cheap; prevents hot code leaving the language |
| 7 | **Sum types + exhaustive matching** (§4.6) | Composes naturally with existing `switch` and move tracking |
| 8 | **A second float width (binary64)** (§4.9) | The binary32 contract is written; a second width mostly reapplies it |
| 9 | **Mutable iteration** (§4.10) | Highest visibility per unit of work |
| 10 | **The restriction ledger** (§5.1) | Documentation only; largest credibility return |

Items 2, 6, and 10 are small enough to be worth doing soon regardless of
where the larger items land.

---

## 7. Assessment

GTI is not a C++ dialect with extra checks. It is a coherent language design
that makes a specific bet: *C++'s spelling, Rust-class ownership, no lifetime
annotations, and no undefined behaviour.* On the parts it has built, that bet
is being executed well — the safety rules in §2 are real, enforced by the
frontend, and verified here rather than merely documented. The borrow model
with inferred origins is a genuine contribution, and a standard `vector`
written in ordinary GTI over a checked capability is strong evidence the
foundation holds.

The honest position is that GTI is currently an **excellent memory-safety
prototype and an incomplete systems language.** Nothing in §4 contradicts the
design; every item is additive. But "systems language" is a claim about
domain, and the domain requires concurrency, layout control, allocators, and
FFI that reaches real C. Until those exist, the language can express its own
standard library — which it does impressively — but not the software it is
aimed at.

The most valuable near-term action is not a feature. It is deciding, and
publishing, which of §4's restrictions are permanent design and which are the
compiler catching up (§5.1). That single document would tell users what
language they are actually adopting.
