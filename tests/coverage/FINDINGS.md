# Language Coverage Corpus — Findings

The corpus in this directory exists to compile a large amount of *valid* GTI
across as much syntax and stdlib surface as possible, on the premise that
much of the language has never been put through the pipeline together. It is
not a demonstration set; `examples/` owns that role. Programs here are chosen
for combinatorial reach, not readability.

The CTest gate `language_coverage_corpus` runs every corpus program, every
reproducer, and every must-reject program in `rejections/`:

    python3 tests/coverage/gate.py build/gti <output-dir>

`gate.py` classifies each outcome as pass, runtime failure, ICE (`GTI-B0001`),
backend failure (generated C++ does not compile), link failure (the native
linker reports an undefined generated symbol), silent failure (non-zero exit
with no diagnostic), hang, crash, bad location (a diagnostic at the synthetic
`<source>` location), or located rejection. `expectations.json` states the
expected outcome. Corpus programs must pass unless listed. A listed entry with
a `finding` (and, where one exists, an `issue`) is a known defect: it records
the outcome the defect produces today, and the gate fails when that outcome
changes. A failing test therefore stays in place without turning the gate red,
and a fix cannot go unrecorded: update this file, the expectation and the issue
together. `run_coverage.sh build/gti` remains available for a quick sweep of
the corpus programs alone that does not consult the expectations.

## Audit result

The complete 65-program PR head initially measured **50 pass, 10 ICE, 1 link
failure, 2 silent failures, and 2 rejections of valid code**. The first
30-program tranche initially measured **24 pass, 4 ICE, and 2 silent
failures**; after integrating F1–F6 fixes, that fixed regression tranche is
**30 pass, 0 ICE, 0 silent failures, and 0 rejections** and is registered as
`language_coverage_fixed_corpus`.

The later c32–c66 programs preserved the then-unresolved F7–F16 findings. Four
further defects (F17–F20) came from sanitizer, malformed-input, and
resource-stress runs rather than from the sweep.

A second script, `scaffold_probe.sh`, probes stdlib operations the surface
manifest marks as unimplemented and records how each one actually fails
(finding F7). `stress/` holds inputs that crash or exhaust the compiler
(F18–F20); they are deliberately outside the sweep, and `stress/README.md`
explains how to run them safely.

Every historical finding below remains reproduced by a self-contained program
in `reproducers/`, and every reproducer is gated by `expectations.json`.

### Re-audit at 0.379.2

An optimized 0.379.2 build compiles and runs **63 of 65** corpus programs; only
`c52_higher_order_lambdas` (F14) and `c55_iterator_utilities` (F16) are still
rejected. Before this re-audit only c01–c30 were gated, so 33 passing programs
had no regression protection.

| Status at 0.379.2 | Findings |
| --- | --- |
| Resolved, now gated as passing | F8, F9, F11, F12, F13, F15 (its `(a + b).value()` form), S03, S05, S07, S10, S11 |
| Improved | S08: the 40-link chain compiles in about 1.2s (was 6.5s) |
| Still reproduced, gated as known | F10, F14, F16, F17, S01, S02, S04, S06, S09, S12, S15, tracked together in [#103](https://git.internal.howetotech.dev/gti-lang/gti/issues/103) (F14's wrong diagnostic is now GTI-S2015 rather than GTI-S2046) |
| Fixed in the issue campaign, gated as located rejection | F7 ([#51](https://git.internal.howetotech.dev/gti-lang/gti/issues/51)): selected bodyless ordinary GTI functions, declaration-only `std::weak_ptr` members and primary JSON codecs now produce `GTI-S2106` in closed programs before backend/native entry; the original optional reproducer remains passing |
| Not gated (resource or tooling inputs) | F18, F19, F20, S13, S14 |

## Test-gap audit (A-series, 0.379.2)

### Fixed-array construction repair

A17 (#63), A42 (#85), and A52 (#93) are resolved and their six original
reproducers now require native success. Signed literal elements preserve their
contextual type, passive record copies construct final array storage without
extra default construction, and verified passive union movement is
failure-free. The C ABI reproducer links an independent C17 fixture rather
than merely passing emission. The focused array gate also checks no-default
records, observable default-constructor counts, representability/unsafe
negatives, malformed publication/type proofs, both C++ standards at O0/O1/O3,
and ASan/UBSan. Other A-series outcomes below remain historical until their
own fixes land; this is not full-campaign verification.

A third campaign looked for areas where the existing suites do not reach, using
features added since the 0.329.0 audit. Seven probes each wrote ordinary,
self-checking programs and must-reject programs for one area:
- ownership and borrowing;
- generics and compile-time evaluation;
- classes, operators and sum types;
- standard-library collections, text and data formats;
- memory, `unsafe` and defined runtime failure;
- diagnostics and optimization consistency;
- multi-file projects and modules.

Every candidate finding was re-checked against the specification, reduced, and
reproduced on an optimized 0.379.2 build.

All passing work is now gated:
- **78 programs** become corpus programs c67–c144. The memory programs check
  exit status 70, the exact runtime report, and cleanup order with `cases`.
- **139 must-reject programs** in `rejections/` each record their diagnostic code.
- **25 multi-step projects** in `projects/` cover modules, path dependencies,
  defines, workspaces, test targets, and initialization order.

Every defect has a reproducer gated as a known finding and a Gitea issue.

| Kind | Findings |
| --- | --- |
| Silent miscompile | A25: an `expected<T&, E>` relay returns the parameter, not the referent |
| Runtime-semantics or tooling result | A02: `gti test` passes a test whose `main` returns 256; A13: nondeterministic `--emit-mir` |
| Compiler hang or silent failure | A06 and A57 resolved; A41 now reports a located internal failure but its forwarding defect remains unresolved |
| ICE or invalid generated C++ on valid code | A01, A14–A17, A26–A32, A37–A40, A42, A45–A52, A55, A56, A58, A59 |
| Valid code rejected | A03, A18, A19, A23, A34, A36 |
| Specification gap | A04: a dependency's own `[build] defines` is ignored |
| Diagnostic quality | A05, A07–A12, A20–A22, A24, A33, A35, A43, A44, A53, A60, A61 |

| Finding | Issue | Gated outcome today | Tests |
| --- | --- | --- | --- |
| A01 | [#53](https://git.internal.howetotech.dev/gti-lang/gti/issues/53) C++ backend ICE assigning a class value into a class-typed field | fixed; focused native and representation checks | `reproducers/a01-call-result-into-field`<br>`reproducers/a01-class-value-into-field`<br>`reproducers/a01-field-assign-from-call-result-method` |
| A02 | [#54](https://git.internal.howetotech.dev/gti-lang/gti/issues/54) hosted entry maps results outside 0..255 to failure status 1 after cleanup | fixed; passing regression | `projects/a02-test-status-truncation` |
| A03 | [#55](https://git.internal.howetotech.dev/gti-lang/gti/issues/55) Program-wide initializer proof rejects any generic call (GTI-S2068) | rejected | `reproducers/a03-generic-call-in-global-initializer` |
| A04 | [#56](https://git.internal.howetotech.dev/gti-lang/gti/issues/56) dependency defines participate in the selected program's shared configuration | fixed; passing regression | `projects/a04-dependency-defines` |
| A05 | [#69](https://git.internal.howetotech.dev/gti-lang/gti/issues/69) Diagnostics suggest invalid fixes and misdescribe explicit generic arguments | bad-diagnostic | `reproducers/a05-string-view-help-suggests-invalid-form` |
| A06 | [#57](https://git.internal.howetotech.dev/gti-lang/gti/issues/57) Resolved: unbounded polymorphic recursion is resource-bounded | rejected (GTI-S2105) | `reproducers/a06-polymorphic-recursion-hang` |
| A07 | [#58](https://git.internal.howetotech.dev/gti-lang/gti/issues/58) Unknown function attribute is misparsed as a class attribute (GTI-P0001 instead of GTI-S2099) | rejected | `rejections/diag-p10-unknown-attribute`<br>`reproducers/a07-unknown-function-attribute` |
| A08 | [#68](https://git.internal.howetotech.dev/gti-lang/gti/issues/68) Diagnostic cascades after an earlier error: unknown-type leaks, false #endif, false missing-return, duplicates | bad-diagnostic | `reproducers/a08-unknown-type-cascade` |
| A09 | [#68](https://git.internal.howetotech.dev/gti-lang/gti/issues/68) Diagnostic cascades after an earlier error: unknown-type leaks, false #endif, false missing-return, duplicates | bad-diagnostic | `reproducers/a09-bad-if-condition-cascade` |
| A10 | [#68](https://git.internal.howetotech.dev/gti-lang/gti/issues/68) Diagnostic cascades after an earlier error: unknown-type leaks, false #endif, false missing-return, duplicates | bad-diagnostic | `reproducers/a10-payload-arity-cascade` |
| A11 | [#69](https://git.internal.howetotech.dev/gti-lang/gti/issues/69) Diagnostics suggest invalid fixes and misdescribe explicit generic arguments | bad-diagnostic | `reproducers/a11-explicit-generic-argument-messages` |
| A12 | [#68](https://git.internal.howetotech.dev/gti-lang/gti/issues/68) Diagnostic cascades after an earlier error: unknown-type leaks, false #endif, false missing-return, duplicates | bad-diagnostic | `reproducers/a12-extern-c-overload-duplicate` |
| A13 | [#59](https://git.internal.howetotech.dev/gti-lang/gti/issues/59) --emit-mir output is not deterministic (field symbol IDs vary between runs) | nondeterministic | `reproducers/a13-mir-field-symbol-order` |
| A14 | [#60](https://git.internal.howetotech.dev/gti-lang/gti/issues/60) C++ backend ICE: constructor-less struct field initializer inside a class with a user constructor | fixed; focused native and representation checks | `reproducers/a14-generated-ctor-field-in-user-ctor` |
| A15 | [#61](https://git.internal.howetotech.dev/gti-lang/gti/issues/61) ICE: local declared inside a dependent if constexpr branch (absent lowered symbol) | ice | `reproducers/a15-dependent-if-constexpr-local` |
| A16 | [#62](https://git.internal.howetotech.dev/gti-lang/gti/issues/62) Documented relay<T> example fails in the C++ backend for a copyable lambda | backend | `reproducers/a16-relay-copyable-lambda` |
| A17 | [#63](https://git.internal.howetotech.dev/gti-lang/gti/issues/63) Contextual signed fixed-array literals | resolved: pass | `reproducers/a17-narrow-negative-array-literal` |
| A18 | [#64](https://git.internal.howetotech.dev/gti-lang/gti/issues/64) Function item rejected as a callable argument invoked with a parameter pack | rejected | `reproducers/a18-function-item-pack-invoke` |
| A19 | [#65](https://git.internal.howetotech.dev/gti-lang/gti/issues/65) Trailing requires-clause facts are ignored when validating a dependent compound argument | rejected | `reproducers/a19-trailing-clause-dependent-compound` |
| A20 | [#66](https://git.internal.howetotech.dev/gti-lang/gti/issues/66) Advisory warnings decided from one generic instance are wrong (GTI-S2096, GTI-S2095) | bad-diagnostic | `reproducers/a20-unnecessary-mut-per-instance` |
| A21 | [#69](https://git.internal.howetotech.dev/gti-lang/gti/issues/69) Diagnostics suggest invalid fixes and misdescribe explicit generic arguments | bad-diagnostic | `reproducers/a21-value-argument-resolved-as-type` |
| A22 | [#68](https://git.internal.howetotech.dev/gti-lang/gti/issues/68) Diagnostic cascades after an earlier error: unknown-type leaks, false #endif, false missing-return, duplicates | bad-diagnostic | `reproducers/a22-spurious-s2068-after-constexpr-failure` |
| A23 | [#67](https://git.internal.howetotech.dev/gti-lang/gti/issues/67) Finite value-generic recursion ended by a specialization is rejected as self-containing | rejected | `reproducers/a23-recursive-value-generic-layout` |
| A24 | [#66](https://git.internal.howetotech.dev/gti-lang/gti/issues/66) Advisory warnings decided from one generic instance are wrong (GTI-S2096, GTI-S2095) | bad-diagnostic | `reproducers/a24-unreachable-warning-per-instance` |
| A25 | [#70](https://git.internal.howetotech.dev/gti-lang/gti/issues/70) Miscompile: returning a local reference through expected<T&, E> yields the parameter, not the referent | ice, runtime | `reproducers/a25-expected-ref-relay-wrong-referent`<br>`reproducers/a25-ref-return-from-expected-value` |
| A26 | [#71](https://git.internal.howetotech.dev/gti-lang/gti/issues/71) MIR ICE: overloaded dereference store whose right side reads the same owner | ice | `reproducers/a26-overloaded-deref-store-reads-owner` |
| A27 | [#72](https://git.internal.howetotech.dev/gti-lang/gti/issues/72) C++ backend ICE: compound assignment through an overloaded operator* or a subscript on a call result | ice | `reproducers/a27-compound-assign-overloaded-deref` |
| A28 | [#73](https://git.internal.howetotech.dev/gti-lang/gti/issues/73) C++ backend ICE: overloaded operator on a field of a range-for element | ice | `reproducers/a28-map-range-for-key-view`<br>`reproducers/a28-range-element-field-overloaded-operator`<br>`reproducers/a28-range-element-field-ref-method` |
| A29 | [#74](https://git.internal.howetotech.dev/gti-lang/gti/issues/74) C++ backend ICE: calling a method on a string_view returned by a call | ice | `reproducers/a29-method-on-returned-string-view`<br>`reproducers/a29-string-view-size-on-dynamic-view` |
| A30 | [#75](https://git.internal.howetotech.dev/gti-lang/gti/issues/75) Consuming member results stage before replacing existing owning bindings, retaining receiver and replacement failure cleanup | pass | `reproducers/a30-assign-consuming-call-result` |
| A31 | [#76](https://git.internal.howetotech.dev/gti-lang/gti/issues/76) MIR ICE: move on a loop's break path, then reinitialization after the loop | ice | `reproducers/a31-move-on-break-path-then-reinit` |
| A32 | [#77](https://git.internal.howetotech.dev/gti-lang/gti/issues/77) move_only_function cannot be stored in vector (invalid C++) or optional (MIR ICE) | backend, ice | `reproducers/a32-optional-emplace-move-only-function`<br>`reproducers/a32-vector-push-back-move-only-function` |
| A33 | [#66](https://git.internal.howetotech.dev/gti-lang/gti/issues/66) Advisory warnings decided from one generic instance are wrong (GTI-S2096, GTI-S2095) | bad-diagnostic | `reproducers/a33-unnecessary-mut-deref-store` |
| A34 | [#78](https://git.internal.howetotech.dev/gti-lang/gti/issues/78) string_view relayed through a receiver-tied subscript is rejected (GTI-S2045) | rejected | `reproducers/a34-view-relay-through-subscript` |
| A35 | [#68](https://git.internal.howetotech.dev/gti-lang/gti/issues/68) Diagnostic cascades after an earlier error: unknown-type leaks, false #endif, false missing-return, duplicates | bad-diagnostic | `reproducers/a35-expected-ref-field-stale-message` |
| A36 | [#79](https://git.internal.howetotech.dev/gti-lang/gti/issues/79) Resolved: nominal fields use the same exact nullptr construction as declared bindings | pass | `reproducers/a36-unique-ptr-member-init-nullptr`, `initialization_and_native_identity`, `constructor_null_native_runtime` |
| A37 | [#80](https://git.internal.howetotech.dev/gti-lang/gti/issues/80) Resolved: private native declaration identity retains the exact external C symbol without support-header prototype collision | pass | `reproducers/a37-libc-qsort-link-name-conflict`<br>`reproducers/a37-libc-strlen-prototype-conflict`, `initialization_and_native_identity`, `constructor_null_native_runtime` |
| A38 | [#81](https://git.internal.howetotech.dev/gti-lang/gti/issues/81) Checked class payload extraction publishes the exact return result, preserving wrong-state detection and unpublished-result cleanup | pass | `reproducers/a38-return-call-value-class`<br>`reproducers/a38-return-expected-value-class` |
| A39 | [#82](https://git.internal.howetotech.dev/gti-lang/gti/issues/82) C++ backend ICE: taking the address of a struct-typed field inside a constructor body | ice | `reproducers/a39-ctor-address-of-struct-field` |
| A40 | [#83](https://git.internal.howetotech.dev/gti-lang/gti/issues/83) C++ backend ICE: indexing a fixed-array field through a raw pointer | ice | `reproducers/a40-c-abi-record-pointer-array-field`<br>`reproducers/a40-raw-pointer-fixed-array-field-index` |
| A41 | [#84](https://git.internal.howetotech.dev/gti-lang/gti/issues/84) Unresolved fixed-array forwarding through storage::emplace or vector::emplace_back; invalid HIR is now diagnosed | ice (located GTI-B0001, no longer silent) | `reproducers/a41-storage-emplace-fixed-array-silent`<br>`reproducers/a41-vector-emplace-back-fixed-array-silent` |
| A42 | [#85](https://git.internal.howetotech.dev/gti-lang/gti/issues/85) Fixed arrays of named passive records | resolved: pass | `reproducers/a42-c-abi-record-array-from-named-values`<br>`reproducers/a42-struct-array-from-named-values`<br>`reproducers/a42-struct-array-two-named-values` |
| A43 | [#68](https://git.internal.howetotech.dev/gti-lang/gti/issues/68) Diagnostic cascades after an earlier error: unknown-type leaks, false #endif, false missing-return, duplicates | bad-diagnostic | `reproducers/a43-callback-argument-unknown-type` |
| A44 | [#68](https://git.internal.howetotech.dev/gti-lang/gti/issues/68) Diagnostic cascades after an earlier error: unknown-type leaks, false #endif, false missing-return, duplicates | bad-diagnostic | `reproducers/a44-raw-audit-duplicate-warning` |
| A45 | [#86](https://git.internal.howetotech.dev/gti-lang/gti/issues/86) Derived classes with state-bearing bases: GTI-B0003, backend ICEs, and an undefined Base::Base() at link | ice, link, rejected | `reproducers/a45-base-plus-string-field-unsupported`<br>`reproducers/a45-base-state-rollback-unsupported`<br>`reproducers/a45-derived-destructor-explicit-base`<br>`reproducers/a45-derived-destructor-generated-ctor`<br>`reproducers/a45-literal-base-argument` |
| A46 | [#87](https://git.internal.howetotech.dev/gti-lang/gti/issues/87) Chained user subscripts g[i][j]: compound assignment and self-read are ICEs | ice | `reproducers/a46-chained-subscript-compound`<br>`reproducers/a46-chained-subscript-self-read` |
| A47 | [#88](https://git.internal.howetotech.dev/gti-lang/gti/issues/88) MIR ICE: 'mut auto&' payload binding when the switch subject is a mut T& parameter or range element | ice | `reproducers/a47-mut-payload-binding-through-mut-ref` |
| A48 | [#89](https://git.internal.howetotech.dev/gti-lang/gti/issues/89) C++ backend ICE: switch on an owning payload enum held in a field (obj.field) | ice | `reproducers/a48-switch-on-owning-enum-field` |
| A49 | [#90](https://git.internal.howetotech.dev/gti-lang/gti/issues/90) C++ backend ICE: an owning payload enum call result passed directly as an argument | ice | `reproducers/a49-owning-enum-call-result-argument` |
| A50 | [#91](https://git.internal.howetotech.dev/gti-lang/gti/issues/91) C++ backend ICE: designated construction omitting a field whose default is 'Inner inner{};' | fixed; focused native and representation checks | `reproducers/a50-designated-omitted-brace-default` |
| A51 | [#92](https://git.internal.howetotech.dev/gti-lang/gti/issues/92) Structured binding over a fixed array of class elements: ICE, or invalid C++ emitted | backend, ice | `reproducers/a51-binding-over-class-array`<br>`reproducers/a51-binding-over-class-array-invalid-cpp` |
| A52 | [#93](https://git.internal.howetotech.dev/gti-lang/gti/issues/93) Fixed arrays of passive unions and union-containing records | resolved: pass | `reproducers/a52-struct-with-union-array`<br>`reproducers/a52-union-array` |
| A53 | [#69](https://git.internal.howetotech.dev/gti-lang/gti/issues/69) Diagnostics suggest invalid fixes and misdescribe explicit generic arguments | bad-diagnostic | `reproducers/a53-operator-temporary-ref-diagnostic` |
| A54 | [#95](https://git.internal.howetotech.dev/gti-lang/gti/issues/95) Resolved: alias constraints inspect completed class facts | pass | `reproducers/a54-alias-constrained-generic-class-argument`<br>`reproducers/a54-alias-constrained-generic-user-class` |
| A55 | [#96](https://git.internal.howetotech.dev/gti-lang/gti/issues/96) C++ backend ICE: list/deque/queue emplacement forwarding a move-only constructor argument | ice | `reproducers/a55-ctor-pack-move-only-field-init`<br>`reproducers/a55-list-emplace-move-only-argument` |
| A56 | [#97](https://git.internal.howetotech.dev/gti-lang/gti/issues/97) C++ backend ICE: std::make_unique<T>(T value) | ice | `reproducers/a56-make-unique-same-type`<br>`reproducers/a56-make-unique-string-from-string` |
| A57 | [#98](https://git.internal.howetotech.dev/gti-lang/gti/issues/98) Resolved: non-dependent generic ranges preserve instance-local loans | pass | `reproducers/a57-generic-member-field-range-for-silent`<br>`reproducers/a57-generic-nondependent-range-for-silent` |
| A58 | [#99](https://git.internal.howetotech.dev/gti-lang/gti/issues/99) MIR ICE: reference to a unique_ptr field's pointee reached through a local reference | ice | `reproducers/a58-map-unique-ptr-value-deref`<br>`reproducers/a58-ref-to-owner-field-pointee-via-local-ref` |
| A59 | [#100](https://git.internal.howetotech.dev/gti-lang/gti/issues/100) MIR ICE: read-only reference into an element of a mutable nested container | ice | `reproducers/a59-map-at-then-element-ref`<br>`reproducers/a59-readonly-ref-into-mut-nested-container` |
| A60 | [#69](https://git.internal.howetotech.dev/gti-lang/gti/issues/69) Diagnostics suggest invalid fixes and misdescribe explicit generic arguments | bad-diagnostic | `reproducers/a60-println-format-missing-include` |
| A61 | [#69](https://git.internal.howetotech.dev/gti-lang/gti/issues/69) Diagnostics suggest invalid fixes and misdescribe explicit generic arguments | rejected | `reproducers/a61-designated-argument-diagnostic` |
| F7 | [#51](https://git.internal.howetotech.dev/gti-lang/gti/issues/51) Calls to declared-but-undefined GTI functions fail at the native link instead of with a diagnostic | ice, link | `reproducers/f7-declaration-only-weak-ptr`<br>`reproducers/f7-json-decode-without-codec`<br>`reproducers/f7-undefined-user-function` |

Optimization-level and C++-standard consistency held throughout. Eight diverse
examples gave identical output and exit status at O0–O3 under C++20 and C++23,
and `--emit-cpp` was byte-identical across runs. The coverage matrices
(`matrices/`) were re-run. Their remaining failure cells are all existing
findings (S01, S02, S04, S09, S15), but the matrices themselves are still not
gated, because they report rather than assert.

| Finding | Resolution |
| --- | --- |
| A06 | Canonical HIR discovery stops at 64 new-instance levels or 16,384 distinct generic instances with located GTI-S2105; ordinary roots, exact recursion and finite chains through the depth boundary remain valid. The original growing Box recursion rejects in about 4.5 seconds on the optimized macOS arm64 build, within its unchanged 20-second gate limit. |
| A54 | Alias identities remain available during registration; target constraints are obligatorily rechecked after complete class/member/lifecycle publication, including unused aliases. |
| A57 | Non-dependent range types remain exact, while numeric loan IDs are compared within the correct concrete reanalysis domain. Access/loan/carrier/protocol mutations remain rejected. Every silent invalid-HIR result now produces a located internal diagnostic. |
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

## Issue #103 campaign status

All eleven findings remain individually tracked. The umbrella is not resolved
by completing only a subset; focused checks do not replace the final combined
verification.

| Finding | Current campaign status |
| --- | --- |
| F10 | Reproduces; direct call-result assignment needs general typed destination syntax/places. Main123's projected assignment repair is a separate prerequisite. |
| F14 | Reproduces; nested generic callable-result arguments need coherent exact signature inference, not a brace diagnostic or guessed result type. |
| F16 | Reproduces; canonical iterator comparison still rejects the temporary sentinel. |
| F17 | Reproduces; unnamed-parameter related spans need declaration-owned anchors. |
| S01 | Reproduces; grouped class construction is outside the current production representation proof. |
| S02 | Reproduces; class-valued conditional publication is outside the current production representation proof. |
| S04 | Reproduces; bodyless unnamed formals leave a noncanonical declaration body. |
| S06 | Reproduces; unnamed constexpr formal lacks its diagnostic anchor. |
| S09 | Resolved in focused source checks: initializer-less ordinary mutable locals report located GTI-S2000 before MIR/backend. Program-storage and field policies remain separate. |
| S12 | Documented rounding policy still reproduces; owner policy decision pending before changing implicit conversions. |
| S15 | Reproduces on canonical main; PR110's enum lifecycle prerequisite must be integrated/revalidated rather than duplicated. |

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

## F14 — Wrong diagnostic: a callable parameter invoked in argument position

`reproducers/f14-nested-callable-invocation.gti`

```gti
int32_t apply_twice<Op>(mut Op op, int32_t v) { return op(op(v)); }
```

```
error[GTI-S2046]: Callable parameter 'op' must return 'void' for this
                  invocation, but the selected callable returns 'int32_t'.
```

Nothing in `op(op(v))` requires a `void` return. Splitting the expression —
`int32_t once = op(v); return op(once);` — compiles and runs, producing the
value the rejected form asked for. The inner invocation is evidently being
classified as statement-position, and the diagnostic then reports a rule that
does not apply.

Corpus program: `c52_higher_order_lambdas`.

## F15 — ICE: calling a method on the result of an overloaded operator

**Superseded and mischaracterized — see S01.** The reproducer was written
`(a + b).value()`, and later work established that the *parentheses* are the
defect, not the method call. `a.plus(b).get()` and
`mut Val c = a + b; c.get();` both compile and run. Keeping the entry so the
correction is on the record rather than silently edited away.

## F16 — The canonical C++ iterator loop does not compile over `std::vector`

`reproducers/f16-canonical-iterator-loop.gti`

```gti
for (mut auto it = v.begin(); it != v.end(); ++it) { total = total + *it; }
```

```
error[GTI-S2022]: No exact overload of operator!= for argument types
                  (std::detail::vector_sentinel).
```

`vector_iterator<T>::operator!=` takes `vector_sentinel&` — a reference — and
`v.end()` is a temporary, so it cannot bind. Hoisting the sentinel into a
variable first compiles and runs:

```gti
mut auto last = v.end();
for (mut auto it = v.begin(); it != last; ++it) { ... }
```

This is not an ICE and the rejection is internally consistent, but it deserves
a finding rather than a footnote. It is the single most familiar loop in C++,
the language GTI takes its syntax from; every user will write it, and the
diagnostic says only that no overload matched. It never mentions the temporary,
which is the entire problem and the whole of the fix.

The same reference-binding rule is what makes `std::find(v.begin(), v.end(), 6)`
fail on its literal argument, so this is a pattern rather than an isolated
signature choice.

Corpus program: `c55_iterator_utilities`.

---

## F17 — Diagnostic with an empty name and an out-of-source span

`reproducers/f17-unnamed-parameter-bad-span.gti`

Unnamed parameters are legal GTI — `int32_t ignore_arg(int32_t) { return 7; }`
compiles and runs. But when a diagnostic's related information refers to one:

```gti
Holder build(int32_t&) {          // parameter has a type but no name
  mut int32_t local = 2;
  return Holder(local);
}
```

```
error[GTI-S2045]: Returned stored-reference value must derive from
                  read-only parameter ''.
<source>:1:1: note: Required owner dependency source is here.
```

Two defects in one diagnostic. The name renders as `''`, and the note points
at `<source>:1:1` — a synthetic location that is not a position in the user's
file at all. Give the same parameter a name and the diagnostic is perfect: it
says `'seed'` and the note carries a real line, column, and caret.

The out-of-source span is not merely cosmetic. The pipeline harness in `fuzz/`
asserts that every diagnostic range lies inside its source, and this input
trips that assertion — which is how it was found. Anything consuming ranges
programmatically rather than reading the rendered text, the LSP above all,
receives a range that does not exist.

The mechanism is visible at
[`semantic_type_resolution.cpp:2611`](../../src/compiler/semantic_type_resolution.cpp:2611):
the message interpolates `source.name.lexeme` and the note uses
`tokenSpan(source.name)`. For an unnamed parameter `source.name` is a
default-constructed token, so the lexeme is empty and the span is offset 0 in
no file. A fallback to the parameter's type or declaration span, and wording
that does not require a name, would cover it.

## F18 — Crash: a long operator chain overflows the compiler stack

`stress/operator-chain-500.gti`

```gti
int main() { std::println(1+1+1+ ... +1); return 0; }   // 500 terms
```

**SIGSEGV, exit 139.** The crash appears between 450 and 470 terms and is
reproducible. `lldb` stops in `___chkstk_darwin` on a guard page, so it is
recursion exhausting the stack rather than a bad pointer; ASan prints nothing
at all, which is consistent with the stack being gone before its handler runs.

The parser *does* have a depth limit, and for nested constructs it works
perfectly — 100 nested parentheses, blocks, or `if` statements all produce a
clean `GTI-P0005` in 0.3s at any depth tested. The guard is
`maximumNestingDepth = 64` in
[`parser.cpp:21`](../../src/compiler/parser.cpp:21), taken at six
recursive-descent sites.

A left-associative binary chain never touches it. `1+1+1+...` is parsed by a
loop, so `nestingDepth` stays flat while the resulting AST grows one level
deeper per term. The guard is counting parser recursion, not the depth of the
tree it produces, and a later recursive walk over that tree — which has no
guard of its own — runs out of stack.

Two observations that narrow it further:

- The chain crashes inside a function that is never called, so this is not
  code generation for `main`.
- `||` chains crash identically, so it is not arithmetic-specific.
- 500 separate `t = t + 1;` statements are fine, so it is expression depth and
  not program size.

## F19 — Unbounded memory: one level past the nesting limit

`stress/namespace-depth-63.gti`, `stress/namespace-depth-62-control.gti`

| Nested namespaces | Result |
| --- | --- |
| 62 | `GTI-P0005` in **0.3s**, exit 65 |
| 63 | **no diagnostic, no termination**, memory climbs without bound |

Watched over 75 seconds, resident size at depth 63 grows steadily past 3.2 GB
and is still climbing when killed. Nothing is printed. The only reason the
measurement stops is the external cap.

This is a one-line cliff. At depth 62 the parser's nesting guard fires and
reports the right thing immediately; at depth 63 the compiler runs away. The
shape of it — a guard that throws, followed by a recovery path that re-enters
and generates unbounded work — is the same recovery-spin family as the parser
defect fixed in PR #160.

It is specific to namespaces. Parentheses, blocks, `if` chains, and class
nesting were all tested at depths 62, 63, and 70 and every one is rejected
cleanly and instantly.

Worth stating plainly: this is a compiler that can be made to eat all
available memory by a 63-line file, with no output explaining why. It is the
most likely explanation for a compiler memory spike during ordinary work.

## F20 — The formatter is not idempotent

`stress/formatter-blank-lines.gti`

The project's own formatter harness asserts `format(format(x)) == format(x)`.
It does not hold.

A file containing nothing but two newlines:

```
input  (2 bytes): "\n\n"
format once      : "\n"
format twice     : ""
```

Each pass removes one more newline, so the file erodes to empty. Every other
whitespace-only shape tested is stable — a single newline, spaces only, a tab,
a comment followed by blank lines. It is specifically a token-free file with
two or more newlines.

A longer run on 0.329.0 produced 35 artifacts across **two** properties: 32
non-idempotence, and 3 where **formatting changes the significant token
stream**. `stress/formatter-token-resplit.gti`:

| Input | Formatted | Tokens |
| --- | --- | --- |
| `a<<<=b;` | `a << <= b;` | `[<<=][<]` → `[<<][<=]` |
| `a>>>=b;` | `a >> >= b;` | `[>>=][>]` → `[>>][>=]` |

Maximal munch lexes `<<<=` as `<<=` then `<`; the formatter re-splits it as `<<`
then `<=`. Every valid operator sequence probed is clean, so this arises only on
ill-formed input — but the formatter's reachable path is format-on-save, which
runs on mid-edit buffers by definition.

All three symptoms — blank-file erosion, angle-bracket drift, and this
re-splitting — are consistent with the formatter re-deriving token boundaries
from its own spacing decisions rather than preserving the ones the lexer found.

Three further non-idempotent inputs turned up on lexically invalid bytes,
where the divergence is angle-bracket spacing: the first pass emits
`std::name < int32_t >` and the second collapses it to `std::name<int32_t>`.
The formatter's decision about whether `<` opens a generic argument list or is
a comparison depends on whitespace that the formatter itself has just
rewritten.

The formatter has no file-formatting CLI command, so the reachable path is the
LSP — that is, format-on-save. A formatter that is not a fixed point is a
formatter that can keep producing diffs, and in the blank-file case can delete
content.

---

# Stabilization pass (0.329.0)

A second campaign against `main` at 0.329.0, aimed at v1.0 readiness. Seven
of the findings above were fixed in the interim — F1, F2, F3, F4, F5, F6 and
F9 all compile now, and the corpus sweep went from 50 to 56 passing.

The defects below were found with combination matrices
(`matrices/`), the sanitizer harnesses, and structural stress inputs.

## S01 — ICE: parenthesizing a constructor call or operator result

`reproducers/s01-parenthesized-constructor.gti`

```gti
mut Val c = Val(4);     // ok, prints 4
mut Val c = (Val(4));   // ICE
```

One redundant bracket pair. It is fatal only around expressions that
*materialize* a class value — a constructor call or an operator result.
Parenthesizing a variable, a method call, or a free call is fine, and
parenthesizing a built-in operator result is fine.

This is what F15 actually was.

## S02 — ICE: a ternary producing a class value

`reproducers/s02-ternary-class-value.gti`

```gti
mut Val x = flag ? Val(1) : Val(2);   // ICE
int32_t x = flag ? 1 : 2;             // ok
```

ICEs in all eight consumer positions tried, so it is the ternary itself and
not what is done with the result.

## S03 — ICE: a method call directly on `std::move(x)`

`reproducers/s03-method-on-moved-value.gti`

```gti
std::println(std::move(a).get());        // ICE
mut Val b = std::move(a); b.get();       // ok
```

The inconsistency is the interesting part: `std::move(r).value()` is the
**required** idiom for extracting an owner from an `expected`, and works. The
same shape on a user class or a `unique_ptr` is an ICE — and the `unique_ptr`
case fails in MIR construction while the user-class case fails in the emitter,
so there may be two defects here.

## S04 — ICE: any bodyless declaration with an unnamed parameter

`reproducers/s04-bodyless-decl-unnamed-param.gti`

```gti
interface I { int32_t f(int32_t); };
int main() { return 0; }
```

Two lines. `declaration_matrix.py` puts the whole family on one page: **14 ICE
cells**, every combination of a bodyless declaration form (free declaration,
`extern "C"`, class method declaration, interface method) with an unnamed
parameter of any shape. A *defined* function with an unnamed parameter is fine,
which is why nothing shipped hits it.

The `extern "C"` row matters most: omitting parameter names is normal C style,
so hand-transcribing a C API hits this immediately. `gti bindgen` is unaffected
— it synthesizes `argument0`, `argument1`, and its output compiles.

## S05 — Invalid C++ emitted for an owning payload enum assignment

`reproducers/s05-owning-enum-assign-invalid-cpp.gti`

```gti
mut slot s = slot::empty;
s = slot::one(res(1));      // res has a destructor
```

```
error: use of undeclared identifier '__gti_mir_v_2'
```

The front end accepts the program and emits C++ that does not compile. The
body declares `v_4`, `v_5`, `v_7` — there is no `v_2`. It needs all three of an
assignment (not an initialization), a payload with a destructor, and a
construction expression on the right; `examples/78-owning-payload-enums.gti`
only ever assigns from a named value, which is the gap.

Worth separating from the ICEs: the verified-MIR contract is supposed to
guarantee the emitter can spell what it accepts, or decline with `GTI-B0001`.
Here it did neither.

## S06 — A primary diagnostic with no location at all

`reproducers/s06-constexpr-unnamed-param-no-span.gti`

```gti
constexpr int32_t reads(int32_t*) { return 0; }
```

```
<source>:1:1: error[GTI-S2057]: Constexpr function parameters must be
              non-pack, by-value supported values.
```

F17 was a related *note* pointing outside the source; this is the **primary
diagnostic** with no line, column or caret anywhere. Naming the parameter
restores a correct location.

Both are one root cause: a diagnostic anchored on a parameter's name token,
which is default-constructed for a legal unnamed parameter. There are **9 uses
of `tokenSpan(parameter.name)` across 5 files**, so this is a pattern worth
auditing as a whole.

## S07 — A type alias cannot be used as a scope qualifier

`reproducers/s07-alias-as-scope-qualifier.gti`

```gti
using Alias = Traits;
using ViaAlias = Alias::value_type;        // GTI-S2000: Unknown type
using Direct = Traits::value_type;         // ok
```

And separately, a class-scoped alias on a *generic* class cannot be named from
outside the class at all — `Box<int32_t>::value_type` is a parse error in a
`using`, in a variable declaration, and in a parameter list. The alias works
inside the class body, which is all
`examples/79-class-scoped-type-aliases.gti` exercises.

## S08 — Compile time is exponential in method-chain length

`reproducers/s08-exponential-method-chain.gti`

| chain length | compile time |
| --- | --- |
| 20 | 0.3s |
| 30 | 1.7s |
| 40 | 6.5s |
| 50 | 21.4s |
| 60 | 59.7s |

Each ten links multiplies the time by about three. Three variants at length 40
isolate it: a **reference**-returning `self()` is 0.3s, a free function chained
by value is 0.3s, and the same 40 calls written as 40 separate statements is
0.3s. Only a chained expression whose receiver is the by-value result of a
method call is slow, which points at receiver re-analysis without memoization.

Fluent and builder APIs are mainstream, and the immutable-builder style returns
by value deliberately.

## S15 — ICE: a payload enum carrying a class without a destructor

`reproducers/s15-payload-enum-plain-class.gti`

```gti
class P { mut int32_t n = 0; public: P(int32_t n) : n(n) {} };
enum class E { a(P p), b, };
mut E e = E::a(P(1));            // ICE
```

Adding `~P() {}` to the payload class makes the identical program compile and
run. From `payload_enum_matrix.py`, the destructor-free class payload fails all
ten operations tried, while tag-only, `int32_t` and two-field payloads all pass
every one.

`examples/78-owning-payload-enums.gti` is the only shipped file with a class
payload and its class has a destructor, so the cleanup-free path has no
coverage. A plain data-carrying class is the simpler case and the more likely
thing to write first.

Opposite half of S05, which needs a payload *with* a destructor and emits
invalid C++ rather than an ICE.

## S12 — Implicit integer-to-float conversions silently lose value

`reproducers/s12-lossy-implicit-int-to-float.gti`

```gti
mut int32_t exact = 16777217;   // 2^24 + 1
mut float widened = exact;      // accepted implicitly
int32_t(widened) == 16777216    // the value changed
```

`conversion_matrix.py` maps the permitted set: 35 implicit conversions, exactly
the widening ones plus every integer type to `float` and `double` — including
`int64_t` → `float`, which can lose 40 bits. Meanwhile integer *narrowing* is
rejected (`GTI-S2003`), as are out-of-range literals.

Not a crash, but the one implicit conversion that can change a value in a
language that otherwise refuses conversions that might. Cheaper to settle
before 1.0 than after.

## S13 — The memory exhaustion is reachable through the LSP

Following up F19: `gti_lsp` opening `stress/namespace-depth-63.gti` grows
without bound and is killed at the cap, while the 62-level control answers
normally. The compiler is invoked deliberately; the language server runs on
file open, so this is a path where *looking at* a file exhausts memory.

The input that segfaults the compiler (`operator-chain-500.gti`) is handled
fine by the LSP, so the two subsystems do not share this failure generally.

## S14 — The `--target` triple parser ignores the environment field

`x86_64-unknown-linux-gnu` and `x86_64-unknown-linux-musl` both resolve to
`x86_64-unknown-linux`, and arbitrary trailing text is accepted —
`aarch64-apple-macos-total-nonsense-here` builds as `arm64-apple-macos`.
Unsupported architectures and operating systems *are* rejected properly, so
the parser validates the first two fields and then discards the rest.

## S09 — ICE: an uninitialized `mut` scalar local

Resolved: ordinary local values require an explicit initializer independently
of mutability. `GTI-S2000` points at the binding and prevents invalid local
storage from reaching MIR/backend. Pointer, reference, array and enum-specific
diagnostics and field/program-storage policies are unchanged. Focused controls
also reject a later assignment as a substitute for initialization and accept
an explicitly initialized mutable scalar.

`reproducers/s09-uninitialized-mut-scalar.gti`

```gti
int main(){mut int32_t r;}
```

Twenty-two bytes. GTI already has the right diagnostic for an uninitialized
declaration and fires it correctly for an uninitialized array (`GTI-S2015`), an
uninitialized pointer (`GTI-S2056`), an immutable scalar and a class type
(`GTI-S2000`). The `mut` scalar is the single path that falls through to the
emitter instead.

`elementary_forms_matrix.py` sweeps 33 basic declaration and statement forms;
this is the only hard failure in the set. `mut int32_t total;` followed by
assignment is also one of the first things a C++ programmer types.

## S10 — ICE: passing a named value to a constructor's by-value class parameter

`reproducers/s10-ctor-by-value-copy.gti`

A free function and a constructor with the identical by-value class parameter
disagree:

| Argument | `take(...)` free function | `Holder(...)` constructor |
| --- | --- | --- |
| `named` (a variable) | ok | **ICE** |
| `named.twin()` (method result) | ok | **ICE** |
| `Val(9)` (temporary) | ok | ok |
| `std::move(named)` | ok | ok |

Wrapping a value in a type is most of what constructors do, and `Holder(named)`
is how anyone writes it. If an implicit copy into a by-value constructor
parameter should be rejected, the free function needs the same treatment and
the answer has to be a diagnostic.

## S11 — ICE: a generic instantiated over its own instantiation

`reproducers/s11-nested-generic-class-arg.gti`

`Box<Box<Plain>>` ICEs; `Box<Box<int32_t>>` is fine. From
`generic_shape_matrix.py`, self-nesting fails for every class-typed argument
tried and passes for every built-in. Every non-nested shape in the same sweep
passes for all seven arguments.

This is F11, now filed. The related F5 — the same nesting unwrapped through
chained accessors — **is fixed on main**.

---

## What was checked and found sound

Recording the negative results as well, since they bound where the defects
above are not:

- **No miscompilation.** All 50 passing corpus programs produce byte-identical
  stdout and exit status at `-O0`, `-O1`, `-O2`, `-O3` and under both C++20 and
  C++23 — 250 configurations. The optimizer does not change observable
  behaviour anywhere this corpus reaches.
- **No runtime undefined behaviour.** All 56 programs that build on 0.329.0
  were compiled with ASan and UBSan applied to the *generated C++* and run:
  zero reports. This covers miscompilation that produces UB, which an output
  differential cannot see if every optimization level is wrong the same way.
- **Deterministic output.** Emitted C++ and emitted MIR are byte-identical
  across repeated compiles of all 57 programs.
- **The build system is solid.** Twenty malformed manifests — missing and
  wrong `manifest-version`, absent `[package]`, wrong value types, unknown
  target kinds, duplicate targets, invalid TOML, absolute and `../` traversal
  roots, empty and non-ASCII package names — each produce a distinct, specific
  diagnostic code. Include paths reject traversal, absolute paths, self-include
  and embedded NUL the same way. Nothing crashed, hung, or escaped the project
  directory.
- **Ownership and control flow agree.** `ownership_flow_matrix.py` crosses six
  ownership operations — moving out, borrowing, reassigning, constructing,
  pushing into a container, and a nested scope — against ten control-flow
  contexts including both arms of an `if`, `for`, `while`, `break`,
  `continue`, nested loops and a `switch` arm. All 60 cells compile and run.
- **The elementary surface is otherwise sound.** 33 basic declaration and
  statement forms, of which only S09 fails.
- **Threading is sound.** Ten constructs — spawn and join, joinable before and
  after, value capture, two concurrent threads, spawning in a loop, dropping
  without joining, a thread in a nested scope, moving a thread, and an owner
  alive across a join — all compile and run under
  `--execution-profile concurrent`, and all are correctly rejected under the
  default profile with `GTI-S2084`.
- **The LSP survives what crashes the compiler.** `operator-chain-500.gti`
  segfaults `gti` but `gti_lsp` opens it, answers `documentSymbol`, and stays
  alive. `lsp_probe.py` passes for UTF-16 position encoding across ASCII,
  Latin-1, CJK and emoji, and for edit staleness.
- **Arithmetic is correct.** 400 randomly generated, provably-defined integer
  expressions over the full operator set agree exactly between an independent
  evaluator, C++ at `-O0`/`-O2`, and GTI at `-O0`/`-O3`.
- **No memory errors on well-formed input.** An ASan+UBSan build of the
  compiler processed all 81 corpus programs and reproducers and all 74 shipped
  examples with zero reports.
- **The existing matrices are clean.** `container_element_matrix.py` (65
  container × element × operation cells) and `place_shape_matrix.py` both pass
  with no unspellable shapes.

The defects in this document were found by malformed input through the `fuzz/`
harnesses (F17, F20), by structural stress inputs (F18, F19), and by writing
ordinary programs (F1–F16) — not by the differential and matrix tooling, which
was clean throughout.

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
- `std::string` has no `substr`. (Outdated at 0.379.2: `substr` now exists.)
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

After the A-series audit, the largest remaining gaps are:
- the combination matrices are not gated (they print tables and always exit 0);
- Linux and Windows execution of the corpus is not exercised by this gate;
- LSP behaviour is not covered by the corpus;
- the stress inputs (F18–F20) still run outside CTest.

The earlier list below remains relevant.


Not yet exercised by this corpus, in rough order of expected yield:
concurrency beyond a single spawn/join, connected `tcp` traffic (the socket
surface currently stops at open/close), and receiving a `[[c_abi]]` record
from a linked native factory, which needs a companion C translation unit the
single-file runner does not build.

Two headers cannot be exercised at all until F7's underlying work lands:
`<std/optional>` and `<std/list>` have no executable operations.
