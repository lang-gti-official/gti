#pragma once

#include "gti/frontend.h"

#include <string>

namespace lang::driver {

// Deterministic JSON inventory of borrow dependencies for the RFC 0004
// signature-contract gate (`gti --emit-borrow-dependencies`). Main infers
// call-result contracts from bodies; signature-only contracts may widen them.
// The gate script checks that every dependency set only grows, and only by
// borrow-carrying inputs of the callee.
//
// Schema "gti-borrow-dependencies-v1":
//   {"schema": ..., "calls": [Call...], "contracts": [Contract...]}
//   Call: {"body": Identity, "ordinal": N, "callee": Identity,
//          "kind": "call" | "construct" | "call-body", "hir_value": N,
//          "dependencies": [Dependency...], "reference_inputs": Inputs}
//   Contract: {"callee": Identity, "kind": "function" | "constructor",
//              "explicit": bool, "dependencies": [...],
//              "reference_inputs": Inputs}
//   Dependency: {"root": "receiver" | "parameter:<i>" | "global:<name>",
//                "fields": [name...], "result_fields": [name...]}
//   Inputs: {"parameters": [i...], "receiver": bool}
// A body or callee identity is its kind, qualified name, owner class and
// parameter and type-argument spellings (semantic type printer), never a
// numeric ID; a lambda is its signature plus an ordinal among lambdas with
// that signature. A call is identified by body, callee and its ordinal among
// calls of that callee in the body, in MIR block order: the strictly
// compared normalized MIR fixes that order. No source span is recorded:
// HIR values keep only AST pointers, which procedural expansions do not
// outlive. "hir_value" is diagnostic only. A call is listed when its result
// carries borrowed state or it records a borrow contract, read from the
// canonical (pre-optimization) MIR. A dependency set is the union of every
// representation the row records (primary origin, extra parameters, field
// paths, complete summary); alternative conditions are not included.
// Reference inputs are the signature parameters whose type carries borrowed
// state (references, text views, borrowed-state classes, enums, closures and
// containers of them), and the receiver of a non-static member function.
[[nodiscard]] std::string
borrowDependencyReport(const FrontendResult &frontend);

} // namespace lang::driver
