# 6. Programs, Implementations, And Targets

Status: Normative scaffold

## 6.1 Program Formation

A program consists of one entry source unit, every source unit reachable
through valid includes and imports, the implicit prelude, and one selected target. Source
units are parsed independently and retain their identity through semantic
analysis.

Only the entry unit may define top-level `main`. Its definition uses one of
these entry signatures:

```gti
int main()
```

or:

```gti
#include <std/string>
#include <std/vector>

int main(int argc, std::vector<std::string> argv)
```

Here top-level means the global namespace: `tea::main` is an ordinary
function, not an entry point. An executable requires a global definition in
its entry source unit; a global `main` may forward to a namespaced function.
Missing entry definitions are diagnosed as `GTI-S2094` before backend/native
compilation. Library analysis and emit-only requests do not require an entry.

A closed executable/library request or C++ emission also requires a definition
for each selected ordinary GTI call target, including calls discovered by concrete generic
analysis and overloaded operators. Calls to bodyless ordinary declarations
are diagnosed as `GTI-S2106` at the call, with the declaration related. A missing
definition discovered through a concrete generic instance points at its outer
requesting call and relates the internal call and declaration. Unused
declarations and abstract virtual contracts remain valid. Foreign C declarations,
registered runtime bindings and compiler intrinsic declarations have their own
implementation contracts and do not require GTI bodies. Fragment analysis may
resolve bodyless signatures without claiming that the fragment is executable;
it does not authorize a native artifact with unresolved GTI calls.
MIR-only inspection preserves this open-fragment behavior; C++ emission checks
definition closure without requiring `main`.

The parameter names are not significant, and either by-value binding may use
ordinary `mut`. Aliases that resolve to the exact signature types are valid.
Other arities and parameter types are ill-formed; in particular, GTI does not
use the native `char**` entry signature or borrowed native argument storage
through `main`. Recursive raw pointers exist elsewhere; they are not a hosted
entry alternative.

For the owned-argument form, the implementation creates one owned
`std::string` for every hosted command-line argument and transfers the
resulting owned `std::vector` into the GTI entry function. `argc` is the exact
number of elements in `argv`, including element zero. Empty arguments are
preserved. When the host provides an executable-name argument, it occupies
`argv[0]`; programs shall not assume that this string is nonempty or a
canonical path. The native argument storage is not retained after the copy.

The hosted containment boundary is active before GTI module/static
initialization and remains active through checked entry adaptation, `main`, and
invocation cleanup. Dependencies initialize before requesters, direct
dependencies follow lexical include-directive order, and globals/static fields
within a unit follow source order as specified in
[Execution Section 4.2.4](execution.md#424-program-wide-initialization). The
verified `Module/0` body and program-initialization thunk carry that plan into
the hosted adapter. Data-only constant and implicit-zero stages may use native
static representation, but GTI-visible dynamic work and failure do not execute
as an uncontained C++ pre-`main` policy.

For the owned entry form, safely detected native-state validation and checked
count conversion occur first, followed by program-wide initialization and then
construction of the owned argument vector in host order. The count and vector
then initialize `main`'s parameters left to right and the vector ownership
transfers into the second parameter. This order is part of the generated GTI
operation, not native C++ adapter policy.

Failure to represent the native argument count as GTI `int` is
`GTI-R0006`; failure to allocate the owned values is `GTI-R0011`; and a
malformed impossible host state such as a negative native count is
`GTI-R0013`. Adapter-owned count and native-state checks are anchored to the
selected source `main`; failures from invoked string/vector construction keep
their callee's category, detail and site. These all occur before the entry body.
Ordinary defined failures clean initialized startup values; infallible
exhaustion aborts without cleanup. Environment variables and a
zero-copy borrowed argument view are not part of this entry contract.

Reaching the closing brace of either `main` form returns zero. Parameters and
their owned contents receive ordinary deterministic cleanup when `main`
returns.

After this cleanup, the hosted entry boundary converts the returned `int` to
a portable process status: values from 0 through 255 are preserved; every
other value becomes status 1. In particular, a nonzero return can never become
success through native status truncation. This conversion applies only at the
hosted boundary, not to ordinary GTI function results.

For a cleanup-preserving defined failure in hosted setup or user execution, the
hosted program boundary completes required GTI cleanup, reports the structured
record, and terminates with status 70. Infallible allocation exhaustion instead
reports best-effort and aborts at its detector, as specified in
[Execution And Runtime Semantics](execution.md#410-defined-runtime-failure).

### Annotated test suites

`[[test]]` marks a defined, safe, non-generic namespace-scope function with no
parameters and an exact `void` result. Namespaced functions are eligible;
file-static functions, instance/static methods, operators, hosted `main`, foreign/runtime
declarations, unsafe functions, bodyless declarations and attribute arguments
are rejected with `GTI-S2099`. Duplicate annotations are also rejected.
Returning normally indicates success; a defined runtime failure, panic or
abnormal process exit fails that test.

A manifest target with `kind = "test"` selects one explicit suite root. Its
source graph supplies the tests in the selected package; dependency and trusted
standard-library tests are not automatically run. Qualified names determine
stable discovery ordering and result identities. One native executable is built for the suite,
then each test runs in a fresh process with fresh globals and normal hosted
initialization/cleanup. A panic terminates its child, and later tests continue.
The command reports every result and returns the first failing child status in
stable target/name order. `--filter <text>`, optional `--exact`, and repeated
`--skip <text>` select qualified names; `--list` lists the selection without
executing it. An empty selection fails unless `--allow-empty` is explicit.
`--jobs N` bounds parallel isolated tests (default one). Each test has a
five-minute deadline, overridden by `--timeout Nms|Ns|Nm`; `0` disables it.
Timeouts kill the child process group and return 124 without guaranteeing source
cleanup. Normal panic cleanup is unchanged. `--format json` emits versioned JSON
Lines inventory, launch, completion and summary events on stdout, with build
progress and diagnostics on stderr. See the [driver contract](../architecture/build-and-driver.md)
for status and completion accounting.

Annotated suites supply their entry point automatically and cannot also define
their own hosted `main`. A legacy target with `main` and no annotations still
runs once as before. A root with neither form is diagnosed before native
compilation. Ordinary executable/fragment compilation does not discover or
invoke tests merely because a function is annotated.

`<std/testing>` provides `std::testing::check(bool, string_view message =
"Test check failed")`, a source-defined caller-tracked wrapper over `std::panic`.
Failure reports the user's check location and preserves normal deterministic
cleanup. The library does not catch or resume a panic.
Source-defined `check_eq`/`check_ne` macros evaluate operands once and format
values only on failure; scalar operands accept literals while object operands
remain borrowed. `check_string_eq`, `check_approx`, and `check_elements_eq`
provide string byte/length, floating tolerance and borrowed contiguous collection
diagnostics. See the [standard-library contract](standard-library.md) for exact
type and borrowing boundaries.
See [`examples/annotated-tests`](../../examples/annotated-tests/README.md).

## 6.2 Target And Configuration Selection

`#if`, `#elif`, and `#else` select declarations, class members, or block items
using target properties and valueless configuration flags. `#ifdef NAME` is
equivalent to `#if defined(NAME)`; `#ifndef NAME` is equivalent to
`#if !defined(NAME)`. Conditions compose target comparisons and `defined(...)`
with `!`, `&&`, `||`, and parentheses using ordinary boolean precedence.

Every branch is parsed and must be syntactically valid; only the selected
branch participates in semantic analysis, HIR/MIR lowering, and execution.
Conditional compilation is not a textual macro processor and no directive
lowers to a native preprocessor directive.

`#define NAME` defines a case-sensitive flag from that point in source order;
`#undef NAME` removes it. Repeating a definition and undefining an absent flag
are no-ops. A definition in an inactive branch has no effect. Definitions
before an include are visible while that unit is loaded, and active definitions
made by the included unit remain visible afterwards. Includes themselves
remain forbidden inside a conditional, and each included unit is loaded once.

Flags carry no value and never replace tokens. A replacement list such as
`#define BUFFER_SIZE 4096` is ill-formed; a typed `constexpr` selected by a
flag is the value-bearing alternative. Function-like macros, stringification,
token pasting, arithmetic flag expressions, and predefined compiler-identity
flags are not part of GTI.

An invocation may seed the initial flag set with repeatable `-D NAME` options.
A project may seed it for every target/profile plan in `gti.toml`:

```toml
[build]
defines = ["DEBUG", "USE_EPOLL"]
```

Flags can also be scoped to the selected profile or named build target:

```toml
[profiles.dev]
defines = ["PSYCH_ASSERTIONS"]

[targets.game]
kind = "executable"
root = "src/main.gti"
defines = ["PSYCH_GAME"]
```

`gti run game` selects the default dev profile and both flags. A release build
selects the game flag but not the dev-only assertion flag. Profiles do not
implicitly define `DEBUG`; declare the flag explicitly when needed.

For a project invocation, manifest flags are the sorted, deduplicated union
of `[build]` flags from the selected package and its transitive dependencies,
plus the selected consumer profile's and target's `defines`. Dependency
target/profile flags and unselected targets/profiles do not contribute.
They seed the one
program-wide configuration, not package-private configurations; use distinct
flag names to avoid unintended interactions. Unrelated workspace members do
not contribute flags. Direct one-file compilation does not resolve dependency
manifests.

CLI flags are added to manifest flags. Source directives are then applied in
load order, so a later source `#undef` can deliberately remove either input.
Flag names use ordinary GTI identifier syntax. Flags declared in another
consumer profile or target are known but inactive, so testing a dev-only flag
in release does not produce a typo warning. Referencing a flag declared neither
in source nor in the applicable manifest configuration produces warning
`GTI-S2075` and still evaluates false.

`#error "message"` is a compile-time diagnostic directive. It is syntactically
valid wherever compile-time conditionals are valid and makes the program
ill-formed only when it appears outside a conditional or in the selected
branch. Its message is reported against the GTI directive; it is never
forwarded to a native preprocessor.

Compiler warnings have stable group names and may be disabled per invocation.
For a standalone source, pass `-Wno-<group>` or `--disable-warning <group>`:

```sh
gti main.gti -Wno-unused-mut -o app
```

A project applies the same policy from its selected profile:

```toml
[profiles.dev]
disabled-warnings = ["unused-mut"]
```

Command-line groups are additive to the profile list. Current warning groups
are `configuration` (`GTI-S2075`), `unreachable-code` (`GTI-S2095`),
`unused-mut` (`GTI-S2096`), `unused-unsafe` (`GTI-S2100`), and the optional
raw-audit groups `raw-pointer-violation`, `raw-pointer-potential` and
`raw-pointer-incomplete` (`GTI-S2101` through `GTI-S2103`). Unknown names are rejected, and errors cannot be
suppressed through this mechanism. Successful and failed build/check commands
end their status output with the remaining error and warning counts.

The selected project profile may also set `dependency-warnings = false` to
silence warnings whose primary source belongs to another registered package.
It defaults to `true`; selected-package warnings and all errors remain visible.
This is package-identity policy, not a `build/gti/deps` path filter, and does not
suppress native-tool output. See [dependency warnings](../architecture/build-and-driver.md#dependency-warnings)
for ownership, workspace and cache behavior.

The frontend, optimizer, and backend shall consume equivalent target facts.

Target comparisons have exactly three case-sensitive properties:

| Property | Current values produced by a supported triple |
| --- | --- |
| `target.os` | `"macos"`, `"linux"`, or `"windows"` |
| `target.vendor` | `"apple"`, `"pc"`, or `"unknown"` |
| `target.arch` | `"arm64"` or `"x86_64"` |

An unknown property name is a parse error even in an otherwise inactive
branch. A property may be compared with any string literal by `==` or `!=`;
an unlisted string is not a new target value and simply does not equal the
selected value.

An explicit target triple is normalized before these facts are selected.
`aarch64` and `arm64` select `target.arch == "arm64"`; Darwin and macOS triple
spellings select `target.os == "macos"`. The environment and object-format
components do not become source-visible properties. A malformed triple, a
non-64-bit or unsupported architecture, a big-endian architecture, or an
operating system outside the table is rejected with a distinct target error.
It never falls back to host facts or an `"unknown"` operating system.

Every currently supported triple selects the same GTI-owned scalar data
layout. A byte is eight bits, and the size, ABI alignment, and preferred
alignment below are measured in bytes:

| GTI representation domain | Size | ABI alignment | Preferred alignment |
| --- | ---: | ---: | ---: |
| `bool`, `char`, `int8_t`, `uint8_t` | 1 | 1 | 1 |
| `int16_t`, `uint16_t` | 2 | 2 | 2 |
| `int32_t`, `uint32_t`, `float` | 4 | 4 | 4 |
| `int64_t`, `uint64_t`, `double`, pointer | 8 | 8 | 8 |

The layout is little-endian and has 64-bit pointers. `float` and `double`
retain the exact binary32 and binary64 contracts in
[Execution Section 4.3](execution.md#43-numeric-execution).
The pointer row is a representation category for compiler facts; it does not
grant layout to ordinary classes, interfaces, owners, references, or other
aggregate source types. A record explicitly declared `[[c_abi]] struct` has
the layout defined below, and a valid passive union has the separate bounded
layout defined below. Ordinary class layout, vtables, packing, payload-enum
layout, bit-fields, and user-selected alignment remain outside this bounded
contract.

The type-only operators `sizeof(type)` and `alignof(type)` expose a bounded
projection of these selected facts as exact `uint64_t` frontend constants.
`alignof` selects the ABI-alignment column; preferred alignment is not
source-queryable. Primitive scalar types and recursive raw pointers use the
table directly after transparent alias resolution; pointer layout does not
depend on whether the pointee is itself queryable. An integral scoped enum
uses the layout of its exact fixed-width underlying integer type. A supported
positive fixed array is derived recursively:

```text
sizeof(T[N])  = checked(N * sizeof(T))
alignof(T[N]) = alignof(T)
```

Every extent must be concrete and greater than zero, and the size product must
fit `uint64_t`. A valid `[[c_abi]]` record is laid out in source field order.
Each field begins at the smallest offset aligned to that field's ABI alignment;
the record alignment is the greatest field alignment; and the final size is
rounded up to that record alignment. Nested `[[c_abi]]` records use their
already-computed size and alignment. Every intermediate offset and final size
must fit `uint64_t`.

A valid passive union uses the greatest field alignment and the greatest field
size, with its final size rounded up to that alignment. Its fields may
recursively use the admitted scalar, raw-pointer, integral-enum, fixed-array,
passive-union, and `[[c_abi]]` record layouts. The union is nonempty, so both
maxima are defined, and every intermediate and final size must fit `uint64_t`.

References, ordinary classes and structs, interfaces, payload enums,
`expected`, bare `void`, `nullptr_t`, compiler-private types, symbolic type
parameters or extents, and every other backend-dependent representation are
rejected before lowering when queried directly. Structurally valid passive
unions and `[[c_abi]]` records are the bounded aggregate-layout opt-ins;
integral scoped enums inherit their declared integer layout.
The query grammar is parenthesized and type-only; it does not evaluate an
expression, and a query is not directly part of the restricted array-extent
grammar. An earlier `constexpr uint64_t` initialized from a query may name an
extent.

The selected layout and every source layout-query result are frontend facts,
not LLVM or C++ layout objects. A compiler configuration without a supported
layout is rejected as `GTI-S2062`
before parsing or semantic analysis can consume a selected compile-time branch
and before any backend runs. Installed-toolchain tests compare the host
selection against the native scalar, pointer, positive-array, and `[[c_abi]]`
record ABI on every supported build target.

Target selection does not itself promise that a native artifact can be
produced or executed. A compiler-library client may analyze a program with any
supported normalized target facts. The direct one-file compiler selects the
host target. Project `build`, `check`, `run`, and `test` commands accept
`--target <triple>`; omission selects the host. `check` can analyze any
supported target without a native toolchain, while a non-host native build
requires its normalized `[toolchains.<triple>]` manifest entry. `run` and
`test` reject a non-host target because the reference driver has no target
runner contract. Cross-target runtime headers and archives must be supplied
explicitly through the runtime path overrides; the manifest does not yet
select them per target, and the language makes no automatic cross-runtime
guarantee.

## 6.3 Implementation Requirements

A conforming implementation may use C++, LLVM, another native backend, or an
interpreter. Its representation may differ from the reference compiler if it
preserves the specified observable behaviour.

In particular, an implementation shall not expose as GTI semantics:

- C++ name mangling or namespace representation;
- C++ object, vtable, smart-pointer, template, or exception ABI;
- native overload resolution;
- native integer undefined behaviour;
- native temporary lifetime or evaluation order; or
- private compiler-generated identifiers and helper types.

Resource exhaustion may prevent successful translation, but an implementation
should diagnose it distinctly from an ill-formed program.

All loaded declarations receive semantic checking, including unused application
functions. An error there still makes the program ill-formed; dead-code removal
is not a way to suppress it. Application and package declarations also retain
complete concrete HIR discovery. Artifact requests skip concrete HIR work for
unreached **compiler-trusted library** declarations only; `gti check` retains
complete library lowering. Do not infer lazy semantic analysis or zero frontend
cost for all unreachable code.

The executable need not contain code the program cannot reach. Its verified
MIR/backend inventory is selected from the program roots:

- the hosted entry;
- library exports;
- global and static initialization;
- the C boundary, meaning every `extern "C"` declaration and C record.

Everything those roots reach is emitted too, including virtual members of the
classes that are used. Only the selected artifact bodies proceed to optimization
and native emission; complete application/package validation remains independent
of that inventory.

## 6.4 Diagnostics

An implementation shall diagnose an ill-formed program. It should identify the
GTI source span and the phase or rule responsible. Related declarations and
mechanically correct fix-its are quality-of-implementation features unless a
future tooling profile makes them normative.

Diagnostics produced solely against generated C++ do not satisfy a frontend
diagnostic requirement for a GTI language rule.

## 6.5 Runtime And Native Boundary

Portable public APIs are written in GTI where possible. Operations requiring
the host use source-defined wrappers over a narrow native ABI. A public GTI
class, generic instance, owner, or reference does not automatically acquire a
stable native representation.

An `extern "C"` linkage block declares calls to existing C ABI functions. Its
language string is exactly `C`, and it contains only bodyless namespace-scope
free-function declarations. Each function identifier is the exact
program-global native symbol; a containing GTI namespace affects source lookup
but does not mangle or qualify that symbol. Definitions, native variables,
methods, generics, overloading, redeclaration, and static or virtual qualifiers
are ill-formed. The C symbol `main` is reserved for the GTI entry point, and a
C-linkage function shall not reuse the name of root-namespace GTI storage.

The bounded ABI permits `void` results, fixed-width integers, `bool`, `float`
and `double`, plus the target-correct `c_*` scalar families. The latter retain
native rank/identity when equal width is not equal C type. It also admits
NUL-terminated `c_string`, passive `[[c_abi]]` records, exact named native
function types, and recursive raw pointers ending in an admitted scalar,
`void`, record, opaque handle, `c_string` or named callback. Per-layer `const`
qualification is retained. Scalar, record, raw-pointer and callback parameters
are immutable by-value bindings. `[[c_array(count)]]` supplies a separate
pointer-plus-count contract rather than granting pointer depth.

`OpaqueHandle**` and other admitted nested out-pointers are ordinary pointer
values. Their slot addresses may be formed, stored and forwarded under the
general address rules; the unsafe native-call contract owns slot lifetime,
replacement and any eventual ownership transfer. There is no call-local staging
buffer or automatically created cleanup obligation.

GTI `char`, enums, references, fixed-array parameters, ordinary classes and
structs, owners, recoverable-result types, and anonymous/general
function-pointer declarators do not acquire C ABI forms. Prefer native `c_char`
for C plain `char`. The complete allowlist and target-correct scalar examples
are in [Native C Interoperation](native-c-interop.md).
`std::string_view` is additionally permitted as a parameter only and lowers to
the explicit record:

```c
typedef struct gti_c_string_view {
  const char *data;
  uint64_t length;
} gti_c_string_view;
```

It is an immutable counted input valid only for the duration of the call. The
native callee shall not write through or retain `data`, assume NUL termination,
or read beyond `length`. This compiler-private record does not opt into source
`[[c_abi]]` record rules or make its private pointer source-accessible.

Calling a C-linkage function whose return type or any parameter type contains a
raw pointer or native callback, including an opaque-handle pointer or either
kind nested in a `[[c_abi]]` record, requires lexical unsafe context. The
caller shall meet the pointer/callback validity conditions and the native
function's nullability, bounds, retention, aliasing, initialization, and
ownership contract. A declaration itself is not unsafe. A call whose
source signature contains only the scalar allowlist, pointer-free `[[c_abi]]`
records, or the special counted string-view parameter remains valid in safe
code.

The raw-pointer type and unsafe obligations are incorporated from
[`raw-pointers.md`](raw-pointers.md).

The compiler-owned `@runtime` mechanism remains a closed compatibility surface
for known host identities and is not a general user FFI. The standard runtime
may declare its entry symbols through the bounded C-linkage surface while
keeping public policy in ordinary GTI wrappers.

A generated embedding wrapper is a distinct future containment boundary. ADR
026 now fixes its version-1 C contract, and the compiler has a dormant
artifact request that resolves exact exports and carries their verified roots
through lowered-program construction. It must still convert GTI's explicit
failure channel into a structured host result only after invocation-owned
cleanup; a direct call to a generated C++ symbol does not establish that
boundary. Released manifests therefore still reject `kind = "library"`, and
the current language has neither a published wrapper/shared-library artifact
nor a stable callable GTI ABI. `E-EMBED-01B/C` and `B-LIB-02` own the context
runtime, generated boundary, activation, and same-process re-entry evidence.

**Specification gap:** Annotated opaque-handle ownership transfer, captured or
userdata callbacks, arbitrary foreign-thread GTI callback entry, array
parameters at the C boundary, native error conventions, alternate calling
conventions, packing, C ABI unions (distinct from passive source unions) and
bit-fields require additional contracts. `gti bindgen` imports supported C
enums as target-correct integer aliases/constants, not a native GTI enum ABI.
General nested pointers, typed/void pointer conversion, byte allocation and
manual GTI object lifetime are already implemented under their focused
contracts; they do not infer a foreign library's ownership or object lifetime.

## 6.6 Hosted And Future Execution Environments

The current draft assumes a hosted environment capable of supplying allocation
and standard output. A future freestanding profile may define a smaller prelude
and required runtime service set without changing core expression and ownership
semantics.

The concurrency and memory-model boundary is specified in
[Execution section 4.9](execution.md#49-concurrency-boundary) and
[ADR 008](../decisions/008-safe-concurrency-memory-model.md). The source-defined
`std::thread` and `std::thread_result<Result>` provide public owned
managed-thread operations. `std::atomic<T>` provides `bool` and fixed-width
integer atomics with sequentially consistent defaults and explicit static
legal orders. `std::mutex<T>` provides owner-tied scoped exclusive access.
The separate unsafe `<std/cthread>` surface provides registered manual
create/join, once initialization, manual mutexes, and manual condition
variables. A safe scoped condition-variable wrapper, signal interaction,
detached/asynchronous execution, thread-specific storage, and arbitrary
foreign-thread entry are not currently implemented. An implementation shall
not infer the
concurrent profile or a `threads` capability solely from the target operating
system, backend flags, link arguments, or native-library behavior. The profile
must be selected before semantic analysis by the target/runtime contract and
retained in program facts. Its implementation prerequisites remain tracked in
[`implementation-sequence.md`](../plans/implementation-sequence.md).

The reference direct compiler selects that fact with
`--execution-profile single-threaded|concurrent`. Project mode resolves the
optional `execution-profile` field of the selected `[profiles.<name>]` table,
with the same two values, and accepts the command-line option as an explicit
override. Omission selects `single-threaded`. Concurrent selection enables the
adopted static global/storage policy and permits implemented managed-thread,
atomic, scoped-mutex, and unsafe C-style create/join, once, manual-mutex, and
condition-variable operations when the selected target advertises its
independent `threads` runtime capability. It grants no detach, safe scoped
condition-variable wrapper, arbitrary foreign-thread entry, or other
unimplemented operation.

The reference driver resolves execution policy and runtime support into one
immutable analysis configuration before frontend entry. A raw compiler-library
target advertises no runtime capabilities; selecting the installed hosted
runtime derives `threads` only for a supported target. `gti_lsp` accepts the
same `--execution-profile` option for standalone documents; an explicit startup
selection overrides a nearest manifest's `dev` profile, while omission uses
that profile when present and otherwise remains single-threaded. Neither the
compiler nor the LSP derives capability from `std::thread` spelling.

## 6.7 Build Systems And Packages

Manifest discovery, dependency acquisition, caches, profiles, and artifact
placement are toolchain concerns, not source-language semantics. Direct
compilation remains valid without a project manifest.

A native declaration does not select or link a library. The reference direct
compiler forwards accepted native toolchain options after `--` (for example
`-- -lfoo`); driver-owned build invariants remain reserved.
Project manifests may declare structured native C and C++ sources, search
paths, exact link files, library and framework names, and native argument
vectors at package, profile, or executable-target scope. Optional platform
fragments select by exact resolved operating-system, vendor, and architecture
fields. Manifest paths are relative to and contained by the package; selected
paths shall have the declared file kind. Selected `.c`, `.cpp`, `.cc`, and
`.cxx` files are compiled to managed objects before the existing final C++
link. The implementation invokes native tools directly from argument vectors
and shall not perform shell splitting or environment-variable interpolation.
Project `run -- ...` remains reserved for executed-program arguments rather
than native compiler options.

Native lists are additive and ordered. C sources, C++ sources, search paths,
and link operands prefer the selected target, then profile, then package; C,
C++, linker, and explicit raw arguments apply package, profile, then target so
more-specific flags occur later. Within one fragment, exact link files precede
libraries, which precede frameworks. A matching platform fragment is applied
within its declaring scope. The most-specific base-table C standard wins;
declared C++ sources use the project profile or CLI C++ standard. Recognized
output, compilation-mode, language-standard, optimization, target, sysroot,
data-layout, and response-file option families are reserved to the driver and
are invalid in manifest argument lists. Raw compiler-driver/cc1 escapes and
unjoined forwarded-linker escapes are also invalid because their following
payload cannot be classified independently against those invariants.
Compiler, linker, and raw argument arrays are trusted exact-argument escape
hatches; embedded paths in them are not interpreted or package-contained, and
the driver cannot classify every vendor-specific ABI option. An admitted
argument must not contradict the resolved target/data-layout contract. Only
structured path fields receive containment validation. These settings are not
a source language feature.

Project and workspace manifests provide declared package source roots and
direct-dependency visibility to the same source graph and frontend used by
direct compilation. Package resolution remains driver policy; it does not
replace include resolution, parsing, or source-language semantics.
