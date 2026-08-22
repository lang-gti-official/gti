# Read-only Receiver Diagnostic Precision

Status: Implemented in GTI 0.294.0.

## Outcome

When a method tries to mutate one of its own mutable fields but the method has
no trailing `mut` receiver qualifier, GTI should say that directly. This lets a
library author distinguish an immutable field from a mutable field reached
through a read-only `this` without having to infer the cause from the language
rules.

The motivating workflow is implementing observer-like library methods with
cached state, such as `std::filesystem::path::extension()`. The language rule
does not change: methods remain read-only by default, and a field declared
`mut` is writable only through a mutable receiver.

## Former Baseline

Before 0.294.0, this reduced program was rejected with an imprecise message:

```gti
struct Component {
  mut int offset = 0;
};

class Path {
public:
  void update() {
    component.offset = 1;
  }

private:
  mut Component component{};
};
```

The primary diagnostic pointed at `offset` but reported only:

```text
error[GTI-S2000]: Cannot mutate through a read-only receiver.
```

That statement is true, but it omits the actionable cause: `update()` is a
read-only method because its declaration has no trailing `mut`. In an editor
that renders only the primary message as virtual text, a hint would not repair
that omission.

Semantic analysis owns every fact needed to distinguish this case:

- `currentReceiverMutability` identifies the current method as read-only;
- `currentFunctionDeclaration` identifies the method declaration;
- resolved symbols identify mutable non-static fields of the current class;
- expression information identifies whether the attempted target is a place
  reached through mutable or read-only access.

The former implementation had two generic emission sites in
`SemanticAnalyzer`: assignment to an implicit receiver field and assignment
through a member expression. Fixed-array element assignment already has a
special receiver-root classifier and emits structured `GTI-S2002` information,
including a trailing-`mut` hint. The result is diagnostic drift between
equivalent mutation operations.

## Diagnostic Contract

### Trigger

Use the receiver-specific diagnostic when all of the following are true:

1. analysis is inside a non-static class or struct method;
2. the current method has a read-only receiver;
3. the attempted operation requires mutable access;
4. the target is rooted in `this`, either explicitly or through an implicit
   current-class field; and
5. the relevant field is declared mutable, so field immutability is not the
   owning error.

The root trace should admit ordinary grouping, named member projection, and
checked indexing. It should therefore recognize all of these as the same
cause:

```gti
field = value;
this.field = value;
component.offset = value;
this.component.offset = value;
components.entries[0].offset = value;
```

The trace must stop at calls, overloaded projections, raw-pointer access,
unrelated locals or parameters, and other expressions that do not prove the
current receiver as the storage root. Those cases keep their existing
read-only-object or borrow diagnostic.

### Identity and wording

Use `GTI-S2002`, the existing immutable-access diagnostic family, rather than
the generic `GTI-S2000` fallback or a new code. Fixed-array receiver writes
already use `GTI-S2002`, so this consolidates one semantic rule rather than
creating another identity.

For the motivating example, the primary diagnostic should be equivalent to:

```text
error[GTI-S2002]: Cannot modify field 'offset' because method 'update' has no
trailing 'mut' receiver qualifier.
```

The primary span remains the field token at the attempted mutation. The
primary message deliberately includes the missing receiver qualifier because
many editor clients display only that message.

Attach structured related information when source spans are available:

- at the method name: `Method 'update' is read-only by default.`
- at the receiver-root field declaration: `Field 'component' is declared
  mutable, but it is writable only through a mutable receiver.`

For a direct field assignment, the root and written field may be the same and
should produce only one field-related entry. Nested projections should not
emit one related entry for every intermediate field.

Attach this correction-oriented hint:

```text
Add trailing 'mut' only if callers should require a mutable 'Path'; otherwise
avoid changing receiver state in this method.
```

The concrete receiver type should replace `Path`. This wording makes the API
tradeoff visible: changing the method qualifier is not always the correct
design for a logically read-only query.

### No automatic fix-it

Do not attach a compiler fix-it or LSP code action. Although the insertion
point can be found, adding trailing `mut` changes the callable surface:

- callers must provide a mutable receiver;
- overload selection may change;
- interface, virtual, or override compatibility may need corresponding edits;
- an observer may be better repaired by removing the cache mutation.

One insertion is therefore not correct for every triggering program. The hint
provides the exact spelling without presenting an API change as mechanical.

## Operation Coverage

The implementation applies the same cause and structured payload
to receiver-rooted operations that already require mutable access:

- plain and compound field assignment;
- nested field assignment;
- fixed-array element assignment; and
- initialization of a local `mut T&` from a receiver-rooted field.

The mutable-reference form should retain `GTI-S2017` as the owning reference
diagnostic code, but enrich its primary message, method relation, and hint with
the same missing-trailing-`mut` cause. This preserves the stable distinction
between an invalid reference binding and an invalid assignment.

Calls to mutable methods, explicit moves from receiver fields, and future
exclusive-borrow operations may reuse the same cause classifier when their
existing diagnostics are revised. They are not required to complete the first
slice, and their established diagnostic identities must remain intact.

## Compiler and LSP Ownership

Semantic analysis remains the only owner of the diagnosis. A small helper may
return a receiver-restriction cause containing:

- the current read-only method declaration;
- the current receiver type;
- the receiver-root mutable field symbol; and
- the written field token or initializer expression used for the primary span.

Assignment and reference-validation paths consume that cause to build their
own stable diagnostic family. The helper classifies provenance only; it does
not change expression access, borrow state, overload selection, HIR, MIR, or
backend behavior.

The LSP remains a protocol adapter. It serializes the compiler-provided code,
message, primary range, related information, and hint from the current
`FrontendResult`. It must not detect field paths or infer a missing `mut` from
source spelling. Because no fix-it is emitted, no code action is published.

## Recovery and Precedence

The revised diagnostic must preserve the current recovery behavior and disable
code generation for the invalid source. More specific errors retain
precedence:

- an immutable field reports field immutability, not receiver mutability;
- a read-only local, parameter, or borrowed object reports its own access
  restriction rather than blaming the current method;
- an unknown member or type error is not replaced by a receiver diagnostic;
- an overlapping live loan remains a borrow conflict once mutable receiver
  access is otherwise valid; and
- unsafe raw-pointer operations remain owned by the raw-pointer diagnostic
  family.

Only one receiver-cause diagnostic should be produced for one attempted
mutation. Enriching it must not add a second generic `GTI-S2000` report.

## Verification

Compiler coverage should assert, without whole-rendered snapshots:

- `GTI-S2002` for a direct mutable field write in a read-only method;
- `GTI-S2002` for the motivating nested `component.offset` write;
- the exact primary field span;
- the method and root-field related spans and messages;
- the receiver/API hint and absence of fix-its;
- `GTI-S2017` with the same cause for a receiver-rooted mutable-reference
  initializer;
- successful compilation after adding trailing `mut`; and
- unchanged diagnostics for an immutable field and an unrelated read-only
  local object.

LSP protocol coverage should open the nested-field example and assert the
published code, UTF-16 range, related information, hint, and absence of fixes.
Existing current-snapshot publication owns stale-version behavior; this
diagnostic refinement does not require a new LSP inference or cache path.

## Completed Implementation Sequence

1. Generalized the existing receiver-root field classifier to recognize nested
   named projections while remaining fail-closed at unproven roots.
2. Added one structured diagnostic builder for receiver-rooted assignment
   failures and replaced the two generic `GTI-S2000` reports.
3. Routed fixed-array receiver writes through the shared builder without
   changing their established `GTI-S2002` identity.
4. Enriched receiver-rooted mutable-reference initialization while retaining
   `GTI-S2017`.
5. Added focused compiler and LSP assertions and updated the canonical
   diagnostics documentation with the implemented behavior.
6. Included the user-visible diagnostic change in the 0.294.0 language and
   tooling release.

## Acceptance Evidence

The implementation demonstrates that:

- the motivating nested write identifies the missing trailing `mut` in its
  primary message;
- the compiler, not the LSP, owns the cause and all structured information;
- receiver-rooted assignment forms no longer drift between generic and
  fixed-array diagnostics;
- unrelated read-only-object errors are not mislabeled as method-qualifier
  errors;
- no unsafe or API-changing fix-it is offered; and
- valid mutable-receiver programs have unchanged semantics and generated code.
