# ADR 033: Checked Immediate Argument Construction

Status: Accepted (2026-11-03, issue #169)

## Decision

A constant expression can initialize a by-value class parameter through an
`implicit consteval` constructor taking its exact type, by value or read-only
reference, with trailing defaults. This is a final overload tier after exact
matching and temporary borrowing. More than one function or constructor is
ambiguous. There is one construction, no conversion ranking or chaining, and no
inference through a parameter satisfied by construction. Assignment and operators
are unchanged. This revises ADR 027's blanket statement that calls never convert;
conversion operators still never participate in argument conversion.

Semantics selects and validates the constructor using staged canonical verified
MIR and publishes its exact passive value. The original expression keeps its
source identity. The checked value is an ordinary value afterward and can be
forwarded through generic wrappers without rechecking. No compiler rule names a
public library type. User-defined checked ports and durations prove the rule.

## Companion decisions

- Dynamic formatting uses distinct `runtime_*` and `try_runtime_*` function
  names. For example, `try_runtime_format(pattern, value)` accepts a dynamic
  `string_view` directly and preserves its existing owner dependency without
  copying the text. A wrapper holding a by-value dynamic view would encounter
  the current aggregate escape restrictions; no ownership exception is added.
- Exact overloads retain precedence. `GTI-S2125` warns about same-shaped
  declaration pairs where an exact source parameter bypasses a checked class
  parameter. A separate runtime name avoids this hazard entirely. Raw text
  print/println/panic overloads with no formatting arguments keep their separate
  literal-text contract.
- A literal/constant-message panic executed in an immediate body produces a
  caller-span `GTI-S2057` with the library message and existing trace. The
  identity-bound terminal panic operation supplies that message. This is the
  prerequisite slice of #174; formatted compile-time messages remain there.
- Immediate requests run under the request's configurable step and call-depth
  limits (`CompileLimits`: `constexpr-steps`, `constexpr-depth`), the same
  limits as every other constant context. Nested requests inherit their
  enclosing budget. The existing request-local staged result cache keys exact
  callable identity, substitutions, constants and completed definitions. Reuse
  charges original work and checks depth, so cache warmth never changes
  acceptance.
- Hover reports the argument index, checked type and selected implicit immediate
  constructor. No new syntax or Tree-sitter rule is required.

## Formatting client and compatibility

The source-defined parser is shared by checked and runtime formatting. The
`format_pattern<Args...>` constructor validates grammar, indexes, count and
built-in presentation compatibility. A custom formatter can opt in through a
constexpr `validate(format_spec&)` method and constructor. Value-dependent
failures remain runtime errors. `format`, its `try_` twin, formatted output and
formatted panic accept the checked carrier; explicit runtime entry points retain
the old dynamic error categories and cleanup behavior. Raw text output and
assertions keep their existing evaluation contracts.

LLVM materializes the proved aggregate. The frozen C++ backend reports
`GTI-B0003` for aggregate publication. Existing runtime format oracle programs
call explicit runtime entry points, retaining their execution and sanitizer
checks; new checked paths have independent LLVM execution and rejection evidence.

## Rejected alternatives

Constant parameters would add values to instantiation identity or lose the proof
when forwarding. A validator attribute would create a second contract mechanism.
Macros would lose ordinary function-item behavior and pay expansion cost at each
call. A nominal compiler format parser would duplicate source policy. General
implicit conversions or ranking would weaken exact matching far beyond the
checked-literal use case. An unchecked runtime fallback would silently stop
validation; explicit runtime entry points make that decision visible.

Validation is library policy. Unsafe code cannot treat a user-written validator
as its only proof of a memory-safety invariant.
