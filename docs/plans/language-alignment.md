# Language Alignment Discussion

Status: Non-normative design review for pre-1.0 decisions.

This discussion is the input to restriction-ledger row `D-LANG-01` in the
maintained
[`implementation-sequence.md`](implementation-sequence.md). Until that row is
complete, an entry here records an open question rather than a permanent
language rule or implementation commitment.

GTI is not source-compatible C++. Its goal is a C++-familiar systems language
with explicit control, value semantics, RAII, predictable performance, and
native interoperability, while avoiding C++'s accidental complexity and unsafe
defaults.

Several departures clearly serve that goal: checked fixed-array access, scoped
nominal enums, defined integer edges, no textual macros, deterministic cleanup,
non-fallthrough switch, frontend-owned overload resolution, and public owners
separated from private allocation capabilities.

The following areas deserve review before their current staging limits become
stable language design:

| Area | Current direction | Pre-1.0 question |
| --- | --- | --- |
| Exact calls | No general argument conversions or ranking | Can a small specified set of safe widening and derived-to-base reference conversions improve ordinary APIs without recreating C++ ranking? |
| Reference spelling | `T&` is read-only; `mut T&` is writable | Does C++ familiarity help enough to offset the changed meaning, and are diagnostics/documentation sufficient? |
| Move spelling | `std::move` consumes a binding, including copyable values | Is the familiar spelling clearer than a stronger verb such as `take`? |
| Result use | Every non-void call result must be used unless discarded at the call | Would a declaration-side discard policy reduce noise without hiding errors? |
| Operators | Member-only, small allowlist, no ADL | Can generic numeric, formatting, iterator, and domain APIs remain natural without a capability-based customization mechanism? |
| Interfaces | `interface` coexists with `= 0`, virtual roots, and explicit public bases | Is this a useful bridge for C++ users or unnecessary ceremony? |
| Construction braces | Exact direct construction, not C++ list/aggregate/CTAD semantics | Can teaching and diagnostics make the deliberately narrower meaning obvious? |
| Includes | Familiar spelling with load-once, direct, non-textual visibility | Should a module vocabulary replace the spelling before compatibility freezes it? |
| Generic capabilities | Built-in/source concepts exist, but capabilities remain bounded | Are user-defined constraints expressive enough for allocators, hashing, formatting, ranges, and callables? |
| Borrowed values | Stored references, views, iterators, and escaping callables are confined | Which restrictions are sound design and which are temporary compiler limits? |
| Low-level control | One-level raw pointers under `unsafe`; no public allocator/manual lifetime | What allocator/provenance/initialization model permits engines and pools without importing `new`/`delete` hazards? |
| Internal capabilities | Public wrappers are ordinary GTI over trusted operations | How should the compiler enforce internal namespace visibility without leaking private types into public APIs? |
| Backend completeness | C++ emission still consumes AST/semantic/HIR side data | Which remaining evaluation, temporary, layout, ABI, and lifetime rules must move into MIR before backend behavior can no longer pressure semantics? |

A departure should survive when its safety, predictability, or simplicity
benefit outweighs lost familiarity and expressiveness. A restriction introduced
only because the current compiler cannot yet prove or lower a construct belongs
in a plan, not silently in the permanent language.
