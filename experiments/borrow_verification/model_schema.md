# Independent model profile (schema 1)

This is an opt-in experimental reference model, not a second production checker
or a formally proved implementation. It imports no compiler ownership decisions.
Run its independent self-tests with:

```sh
python3 experiments/borrow_verification/model_tests.py -v
```

`model.check(program)` explores abstract states; `model.execute(program, inputs=...)`
executes one concrete input/schedule. Both return `Result` with `to_dict()` and
`status`, `violation`, `trace`, `observations`, `steps`, `states`. Status is
`accepted`, `rejected`, `unsupported`, or `timeout`. Observations are integer
reads from concrete execution. Accepted defined failure carries
`violation="defined-failure"`; a violation on an invalid program is rejected.

## Representation and scope

A program is JSON-compatible:

```json
{
  "schema": 1,
  "body": [
    {"op": "alloc", "name": "o", "fields": {"value": 7}},
    {"op": "borrow", "name": "r", "target": "o", "mode": "shared"},
    {"op": "read", "target": "r"},
    {"op": "end", "name": "r"},
    {"op": "drop", "target": "o"}
  ]
}
```

`alloc` represents owned storage identity, including a bounded allocation slot;
it does not claim equivalence of stack and heap allocation policies. The concrete
machine assigns allocation ids and increasing generations when a slot is reused.
This makes stale-generation access observable even with the free-precondition
check deliberately disabled. Scalar values are mathematical integers; this model
is about storage/permissions, not GTI integer overflow semantics. Source adapters
must limit values to representable GTI integers or report unsupported.

Places are an owner or reference name followed by at most one named field.
Record fields are disjoint, a whole-owner place overlaps all its fields, and
unknown projections are unsupported. All user names are identifiers; internal
call and temporary namespaces cannot collide with them. Reference parent chains
represent reborrowing; sibling mutable overlap and incompatible parent access
are rejected. Immutable owners and shared references cannot grant mutable access.

The full operation vocabulary is:

| Operation | Required payload / behavior |
| --- | --- |
| `alloc` | `name`, optional `fields` mapping (default `value: 0`), `mutable` (default true) |
| `borrow` | `name`, `target`, `mode` (`shared` or `mut`) |
| `end` | `name`; active children must end first |
| `read` | `target`; observe its scalar field(s) in declaration order |
| `write` | `target`, optional integer `value`; requires initialized writable storage |
| `init` | `target`, optional integer `value`; reinitialize moved field/owner |
| `move` | `source`, `name`; move initialized owned storage into an unoccupied binding |
| `drop` | `target`; retire initialized whole owner or field; no overlapping live loan |
| `if` | `condition` (bool or input name), `then`, `else` statement lists |
| `loop` | `condition`, `body`, optional concrete `iterations` schedule |
| `scope` | `body`, `cleanup`; cleanup runs after normal, return, or defined-failure outcome |
| `fail` | defined failure, skipping remaining normal statements |
| `call` | `function`, `args`, optional `name` for returned reference |
| `return` | `target`, inside a function only |

Functions are supplied as `functions: {name: {params: [{name, mode}], body: [...]}}`.
They are direct, first-order and nonrecursive. The body is analyzed, not trusted
because of a declared return-origin summary. Reference arguments become scoped
formal borrows. A returned reference must derive from an actual argument; local
owner escape is rejected. Literal integer arguments become call-only shared
borrowed temporaries; mutable borrowing or retained reference results from those
temporaries are rejected. Callee-local loans end before local owners at ordinary
frame exit. Arbitrary destructor callbacks and inter-thread behavior are outside
this profile. Returning a reference creates a provisional return loan before cleanup. Ending
a local reference binding transfers that provisional dependency to its parent;
the actual owner stays protected until the caller result is established. Cleanup
that mutates overlapping storage is rejected even if the returned local binding
has already ended. This models returned-loan transport rather than treating a
return as an untracked address capture.

## Static versus concrete control flow

The analysis has its own transfer implementation with owner liveness,
definitely-initialized fields and loan graphs. Unknown branches preserve a set
of path states. Loops compute a finite powerset fixed point; `max_states` and
`max_steps` bound resource use and produce `timeout`, not acceptance. It does not
replace loops with a fixed number of iterations. Scalar values are abstracted
away, so input-dependent branches are conservative, and correlation between
separate input tests is not retained.

The concrete machine separately maintains memory cells, allocation generations,
reference permission tags and concrete branch choices. Its transfer functions
do not call the static checker's transfer/overlap routines. A named or `*` loop
runs its `iterations` count, or the integer input associated with its condition
(default zero). `condition: true` is unconditional and runs until defined failure,
return, or the execution budget, regardless of a supplied finite schedule.
Even an empty iteration consumes budget. A clean concrete run proves nothing
about unexecuted inputs. Cleanup traces remain in `trace`; they are not appended
to the observed read values.

## Enumeration, sampling and faults

`generation.core_examples()` provides named hand-derived programs and `EXPECTED`
records their expected static outcomes. `enumerate_programs(max_length=n)` is
exhaustive only for the public ten-operation linear alphabet following one
allocation: exactly `sum(10**i for i in range(n+1))` programs. Declaration/lifetime
violations are included intentionally; unsupported syntax is separate.
`enumerate_structured(max_body_length=n)` independently enumerates an eight-operation
grammar over two record owners and two fields: all pairs of branch bodies and
all loop bodies of length zero through n. With S = sum(8**i, i=0..n), the exact
count is S*S + S: 2, 90, or 5,402 programs at the supported bounds 0, 1, or 2.
This finite grammar makes control-flow coverage explicit without claiming
exhaustive coverage of functions, reborrows, or the entire schema.
`seeded_programs(seed, count)` samples structured combinations and mutations,
including branches, loops, calls and failure cases. It is not an exhaustive
structured-program grammar. `alpha_rename` supplies a justified metamorphic test;
field names and control inputs retain their meaning.

The five intentional faults (`overlap`, `owner_liveness`, `escape`, `init`,
`cleanup`) disable specifically named rules in either implementation. Self-tests
require each designated witness to fail with the intended violation normally and
to be accepted under its mutation. These are test-only fault switches, never
trusted verification modes. The static and concrete engines share schema parsing
and result types only. Shared grammar assumptions are still part of the trust
boundary and require review.

## Independent MIR dump adapter

`mir_adapter.verify_mir_dump(text)` checks a separate narrow production artifact
profile: `mir-v120`, no module initialization, one parameterless entry function,
scalar integers/bools and scalar reference bindings, dereference projections,
SSA constants/arithmetic/loads, initialization/assignment, local borrow/end
operations, and scalar CFG return/defined-failure paths. It independently resolves
addresses through reference cells and compares loan metadata with the executable
borrow operands. `valid=1`, compiler reachability and ownership/effect claims do
not authorize operations. Both invocation successors are explored so an
unexecuted failure cleanup is still examined.

The adapter's `observations` are reachable normal return integers, not source
`read` observations. Compare equivalent return encodings explicitly. It does not
validate a whole compiler frontend, runtime, LLVM, arbitrary calls, records,
allocation, block parameters, integer overflow, or unknown MIR vocabulary. Such
inputs produce `unsupported`. It is an initial compiler-artifact connection,
not complete model-to-MIR correspondence for the whole schema. Public evidence
must list model coverage, source-adapter coverage and MIR-adapter coverage
separately. Formal proofs and an independently reviewed complete adapter remain
phase 3 work.
