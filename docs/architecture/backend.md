# Backend And Native Handoff

Status: Current transitional C++ backend.

`include/gti/backend.h` defines a target-independent `Backend` interface.
`BackendInput` carries the checked `Program`, `SemanticModel`, typed HIR,
optimized MIR, HIR compatibility replacements, and selected target.
`BackendArtifact` is source or object content plus its extension.

M-FAIL-01 must extend this handoff with compiler-owned `FailureMetadata`: the
immutable descriptor, artifact identity, and detector-to-`FailureSiteId`
mapping built before MIR optimization. Building it requires `SourceGraph`,
`SourceManager`, and the driver-provided direct/project logical root; a backend
must not reconstruct those inputs from absolute token paths. This metadata
product is not present in `BackendInput` today.

## C++ Backend

`CppBackend` currently creates `CppEmitter`, which traverses the checked AST and
consults semantic facts, HIR, optimization replacements, and the target. It
does not currently consume `BackendInput::mir`. The generated C++ is a
representation artifact, not GTI semantics.

The emitter is responsible for choices such as:

- mapping the root GTI namespace `std` to the compiler-reserved C++ namespace
  `__gti_std`, while leaving an ordinary user namespace named `gti_std`
  distinct;
- emitting already-selected mangled calls, C-linkage symbols, dispatch, and
  lifecycle operations;
- representing fixed arrays, unique ownership, storage, classes, and virtual
  dispatch in C++;
- realizing checked arithmetic, conversion, indexing, pointer, and runtime
  operations;
- isolating the hosted native `argc`/`char**` boundary in an adapter for the
  semantic owned-argument entry kind, copying each argument into the exact
  GTI string/vector types and invoking the already-resolved append callable;
- emitting every GTI `float` or `double` literal and proven replacement from
  its exact binary32 or binary64 bits with `std::bit_cast`, and rejecting a
  host without matching IEEE-754 `float` and `double` representations;
- realizing the selected fixed-width wrapping operations through unsigned
  modulo arithmetic and the selected saturating operations through guarded
  bounds checks, and constructing checked-result expected values through the
  same guarded arithmetic, without executing signed native overflow;
- emitting a source layout query as its retained unsigned-64 frontend constant,
  never as native C++ `sizeof` or `alignof`;
- emitting a `[[c_abi]]` record as a passive standard-layout C++ struct and
  asserting its semantic size, alignment, and every field offset with native
  `static_assert`s rather than accepting native layout as language authority;
- emitting the same native-record definition through `NativeHeaderBackend` as
  a dual C17/C++20-or-C++23 header. The C++ branch preserves exact source
  namespaces and `extern "C"` function identity; the C branch uses
  deterministic flattened names where namespaces cannot be represented. Both
  consume semantic field types/layouts and never reconstruct ABI facts from
  generated C++;
- preserving `[[c_opaque]]` types as declarations only: generated GTI C++ and
  the bridge header's C++ branch emit the exact namespaced forward declaration,
  while the C branch emits a deterministic incomplete `typedef struct`. The
  native implementation may complete that type privately in C or C++, but the
  backend never asks for its layout or emits a GTI definition;
- selecting C++20 versus C++23 expected support.

GTI constant evaluation remains authoritative for checked-result constants.
C++23 may emit their representation as native `constexpr std::expected`.
Because the vendored C++20 expected representation is not a literal type, the
C++20 backend emits an immutable `const` object after the frontend has already
resolved every constant observer; native C++ constant evaluation is not used
as a substitute proof.

It must not perform GTI lookup, overload resolution, constraint checking,
ownership validation, or infer an intrinsic from spelling.

For `[[c_abi]]` only, `CppEmitter` uses canonical resolved field spellings and
does not emit GTI lifecycle policy members into the record definition. This
keeps its class definition token-equivalent to the generated header's C++
definition across translation units. Semantics still decides which GTI
construction operations are available. Initializers are prohibited on the ABI
record itself; safe wrappers and native factory functions own construction
policy.

Restricted non-virtual methods other than native comparison implementations
retain selected function-identity names. Public iterator-protocol declarations
additionally emit backend-private hidden-friend adapters for dereference,
prefix increment, and sentinel inequality. A symbolic generic body calls the
adapter through unqualified argument-dependent lookup. This makes inherited
adapters available without C++ member hiding or public `using` declarations and
lets each adapter forward to its declaration's already-selected implementation
identity.

The adapters use stable representation names such as
`__gti_operator_dereference` and `__gti_operator_pre_increment`; these are not
source names or a GTI ABI. They are emitted only for the public, exact shapes
admitted by the bounded `input_iterator` and `sentinel_for` contracts. Private
operators, constrained operators, invalid protocol shapes, and inactive
conditional declarations do not expose adapters. Abstract/interface
declarations emit the same adapter, whose forwarding call retains virtual
dispatch.

Sentinel inequality adapters carry an additional backend-private
`exact_type<S>` tag, and a symbolic call derives that tag from the exact
sentinel argument type. The tag prevents native conversions from making a
different base/derived sentinel overload viable while argument-dependent
lookup still discovers adapters inherited from iterator base classes. The tag
is GTI-owned rather than a standard-library type so its associated namespace
does not expand argument-dependent lookup into `std`.

Read-only symbolic calls pass the receiver through a generated const-view
helper, so a mutable source binding cannot cause native overload resolution to
prefer a mutable operator after GTI selected a read-only operation. Concrete
restricted-method calls continue to name their resolved declaration directly.
Native comparison methods are an existing transitional exception: they retain
C++ `operator...` spellings, although the emitter pins their semantically
selected dispatch owner and receiver mutability to prevent derived member
hiding from changing the target. This comparison representation should not be
treated as the semantic model or extended to new generic protocols.

The AST-based C++ emitter still cannot encode every concrete HIR target into a
separate instantiation of one emitted template. The hidden-friend boundary is a
narrow bridge for the implemented structural contracts; it is not permission
for the backend or native C++ to define new GTI overload semantics.

The emitter also does not yet implement the accepted
[evaluation/full-expression contract](../language/execution.md#42-evaluation-order).
It emits ordinary calls, constructors, checked helpers, and several operator
operands inline in native C++ argument lists; emits target places directly;
relies on native temporary rules; and leaves module/static initialization to
native static initialization. The owned-entry adapter currently constructs its
argument vector before performing the count conversion, contrary to the
accepted startup sequence. Although some native constructs such as `&&`, `||`,
and `?:` happen to preserve the selected source control flow, that is not a
complete or verified GTI schedule. Emitter-local IIFEs or statement hoisting
must not become a second lifetime authority.

M-LIFE-01 now provides verified temporary obligations; M-EXEC-01 must provide
ordered MIR. Production conformance then lands only through matching M-BACK
closed-body migrations. The compatibility emitter remains conservative, and
semantics must not relax the both-argument transient-loan restriction for an
operation family until its production path consumes that MIR schedule.

The emitter also currently chooses failure messages in seven generated helper
families and terminates with `std::abort()`. Wrong-state `expected` observers
are emitted as native C++ calls. These paths predate the
[defined-failure contract](../language/execution.md#410-defined-runtime-failure)
and are known nonconformities: they erase the stable GTI category/source
record, skip failure cleanup, expose signal-derived status, and import native
exception/assertion behavior. Once the applicable closed call-graph family
migrates through M-EXEC-01/M-FAIL-01/M-BACK-02, a conforming backend must lower
MIR-owned failure successors and pass the completed record to the selected
program, embedding, or task boundary. The current compatibility emitter remains
explicitly nonconforming until that migration; it shall not invent a second
partial failure authority. The backend shall not synthesize categories or use
C++ exception unwinding to emulate the contract.

A failure-capable MIR-emission slice must be closed across every GTI caller,
callee, constructor edge, and virtual override between a local failure origin
and its containment boundary. A legacy AST/HIR caller cannot erase a MIR
callee's record or skip cleanup, and a migrated caller cannot treat a legacy
aborting callee as propagation. M-BACK-01 may therefore start with a genuinely
failure-free closed family; M-BACK-02 owns the family-by-family closed-call-graph
migration and removal of compatibility helpers.

For `int main(int, std::vector<std::string>)`, the source entry function is
emitted under its ordinary GTI identity. A separate native C++ `main` performs
the checked count conversion and owned startup copy, then moves the resulting
vector into the source function. `ProgramEntryKind` and the append
declaration are semantic facts; HIR concretizes the append target and MIR
retains that concrete identity for verification. The native adapter is a
representation choice and never exposes `char**` to GTI source.

Today the emitter itself also synthesizes the negative-count check, checked
GTI count conversion, and allocation-bearing argument copy. Under M-FAIL-01,
the semantic program-entry record and compiler-generated hosted-startup HIR/MIR
operation instead own those three local failure origins and the `main` anchor.
The backend only realizes that verified operation; it does not select startup
categories or manufacture a source site.

The current emitter may also rely on native C++ static initialization for GTI
module/static initializer bodies. A conforming hosted backend instead enters
the GTI containment boundary before those bodies execute and lowers them under
the language-selected initialization order inside the generated adapter.
M-BACK-02 owns that migration; native pre-`main` failure is not an alternate
containment policy.

## Driver Handoff

`lang::driver::compileToCpp` in `src/driver/compilation.cpp` runs the frontend,
HIR compatibility optimization, owned-MIR pipeline, and backend. At `-O1+` the
MIR pipeline may produce its verified primitive literal-identity shadow
rewrite, but `CppBackend` still ignores MIR bodies and emits from the HIR
compatibility result. The driver refuses backend generation unless every
frontend validity gate and MIR verification succeeds.

The resulting C++ artifact is handed to `gti_driver`, which owns temporary
files, native tool discovery, exact argument vectors, process execution, and
atomic artifact publication. Those concerns do not belong in `gti_compiler` or
the backend semantic contract.

The native driver makes the portable contract authoritative for its supported
GNU-style toolchain interface by appending `-fno-fast-math` and
`-ffp-contract=off` after forwarded compiler arguments. It then defines
`__gti_strict_ieee754=1`, an opt-in marker required by a generated C++
`static_assert`. Library consumers compiling a `BackendArtifact` themselves
must impose the same no-reassociation/no-contraction policy and define that
marker; otherwise the artifact does not compile. The marker records the
consumer's assertion rather than trying to infer arbitrary native compiler
flags. These controls align runtime operations with the frontend's one-rounding
step `llvm::APFloat` evaluation; they do not make C++ the source of the rule.

## Future Backends

A new backend should implement `Backend` rather than adding target branches to
frontend layers. LLVM emission remains premature until MIR owns the missing
temporary, lifecycle, layout, ABI, and runtime rules described in
[`mir.md`](mir.md) and [`docs/plans/optimization.md`](../plans/optimization.md).
