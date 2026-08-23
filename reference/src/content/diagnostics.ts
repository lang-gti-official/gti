// Authored prose for diagnostic pages. The verified reproducer and its real
// compiler output come from generated data; this file only carries the human
// explanation. A code with no entry here still gets a page — a stable URL
// with the census facts — badged as not yet documented.

export interface DiagnosticProse {
  summary: string;
  rule: string;
  why: string;
  fix: string;
}

export const diagnosticProse: Record<string, DiagnosticProse> = {
  "GTI-L0009": {
    summary: "Unterminated character literal.",
    rule: "A character literal opened with ' must be closed by ' on the same line.",
    why: "Letting a character literal run across lines would swallow the rest of the file into one token and turn every later error into noise. Failing at the first unterminated quote keeps diagnostics local.",
    fix: "Close the literal: 'a'. To spell the quote character itself, escape it as '\\''.",
  },
  "GTI-P0001": {
    summary: "The parser expected different syntax at this position.",
    rule: "GTI-P0001 is the general parse-error code: the token stream stopped matching the grammar, and the message names what was expected.",
    why: "One stable code for grammar mismatches keeps parse errors greppable while the message carries the specifics — what was expected and where.",
    fix: "Read the expectation in the message; the caret marks the first token that no longer fits. The help line proposes the smallest insertion when one exists.",
  },
  "GTI-S2001": {
    summary: "Undefined name.",
    rule: "Every identifier must resolve to a declaration visible from its use — a binding, parameter, function, type, or member reachable through the include graph.",
    why: "GTI resolves names during semantic analysis rather than at link time, so a typo fails where it is written instead of surfacing later as a confusing downstream error.",
    fix: "Declare the name before use, fix the spelling, or include the unit that declares it (a visible-but-not-included declaration is reported as GTI-S2024 with the include to add).",
  },
  "GTI-S2002": {
    summary: "Cannot assign to an immutable binding.",
    rule: "Bindings are immutable by default. Assignment, compound mutation, and mutable borrows all require the binding to be declared with `mut`.",
    why: "Immutability by default makes the mutable state in a function visible at its declarations — reviewers see exactly which locals can change, and the compiler can reason about the rest.",
    fix: "Declare the binding `mut` if mutation is intended: `mut int32_t value = 1;`. If it is not, restructure so the value is computed once.",
  },
  "GTI-S2003": {
    summary: "Argument type does not exactly match the parameter.",
    rule: "Calls require exact argument types. GTI performs no implicit numeric conversions at call boundaries, including widenings that C++ would accept silently.",
    why: "Implicit conversions are one of C++'s largest sources of silent surprise — a uint8_t quietly widening, a signed value quietly reinterpreting. GTI makes every conversion visible at the call site.",
    fix: "Convert explicitly when the conversion is intended: `takes_wide(uint64_t(narrow))`. The conversion is checked: a value that cannot be represented is a defined runtime failure, not wraparound.",
  },
  "GTI-S2029": {
    summary: "The argument does not satisfy the generic constraint.",
    rule: "A generic parameter declared with a concept, such as `std::integral T`, only accepts arguments whose types satisfy that concept.",
    why: "Constraints fail at the call with the concept named, instead of deep inside the callee's body the way unconstrained templates fail in C++.",
    fix: "Pass a type the concept admits, or convert first. The note under the error points at the declaration that imposed the constraint.",
  },
};
