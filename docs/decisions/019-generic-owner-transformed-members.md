# ADR 019: Generic-Owner Transformed-Member Boundary

Status: Accepted

## Context

ADR 017 migrates failure-capable bodies per body: the transformed private
ABI carries the body under a derived name, and a boundary wrapper carrying
the original name routes failure into the defined contract. For free
functions this works uniformly — a concrete declaration emits the pair
side by side, and a generic declaration emits the transformed body as a
concrete-signature overload with the wrapper as an explicit
specialization of the primary template (0.182.0).

Members split. A concrete class emits the transformed member and wrapper
side by side in class (0.179.0), and a generic owner's members emit
success-form explicit member specializations (0.184.0). But the failure
pair was excluded for generic owners: C++ does not permit adding a member
to one specialization of a class template, so the transformed sibling —
which must be a member, because the body text reads the owner's private
fields through `this` — appeared to have nowhere to live. The admission
fixpoint therefore drops every caller of a failure-capable generic-owner
member, and with the standard containers now generic (`vector<T>`) that
boundary blocks the storage-body cutover.

## Decision

1. **The primary class template declares the transformed sibling.** For
   every failure-capable member with at least one specialization-eligible
   concrete instance, the emitted primary template carries one extra
   member declaration: the derived transformed name with the member's
   generic parameter types, the transformed ABI's out-parameter for
   non-void returns, the failure-record pointer, and the receiver
   constness of the original member. The declaration is deliberately
   definition-free on the primary: an instantiation whose specialization
   is not emitted never references the name — the admission fixpoint
   guarantees callers exist only where the callee's transformed body
   emits — and a declared-but-undefined member of a class template is
   ill-formed only when odr-used.

2. **Concrete instances define the pair as explicit member
   specializations.** Each specialization-eligible instance emits
   `template <> <ret> Owner<Args>::<name>__gti_mir_failure(<substituted
   ABI>)` with the general failure body text, and the boundary wrapper as
   the explicit specialization of the original member,
   `template <> <ret> Owner<Args>::<name>(<substituted params>)`, calling
   the transformed member and routing failure through
   `::gti_rt_failure_terminate_v1` — exactly the free-generic wrapper
   body. Both declarations precede the first instantiating use, following
   the member-specialization ordering discipline already established.

3. **Eligibility mirrors the success-form member specializations.** The
   owner instance must be one concrete instantiation whose spelling is a
   genuine template-id, every substituted signature type must be concrete
   and representable at the transformed boundary (scalar or void result
   with a type row; parameters within the signature boundary), and the
   instance's failure body must be admitted by the general vocabulary.
   Static members, entry points, and pack-carrying declarations stay on
   compatibility.

4. **The admission fixpoint recognizes the boundary.** A member callee of
   a generic owner is available exactly when its instance passes the
   specialization eligibility above; a concrete owner's member keeps the
   declaration-keyed selector decision. Receiver calls to transformed
   generic-owner members spell through the existing owner-qualified body
   row unchanged.

## Consequences

- The transformed sibling stays a member: private field access needs no
  friend machinery, no receiver respelling, and no per-instance class
  specializations that would duplicate whole classes and fight implicit
  instantiation.
- Every instantiation of a generic owner carries one inert declaration
  per failure-capable member. This is the entire representational cost;
  no code is generated for uninvolved instances.
- The standard containers' mutation bodies become emittable per concrete
  element type, converting the storage pool's largest remaining boundary
  and letting the fixpoint admit their callers.
- A body-text-admissible instance whose signature cannot be concretely
  spelled (packs, parameter-typed signatures) remains on compatibility,
  fail-closed, exactly as the success form already behaves.
