#pragma once

#include "gti/mir.h"

#include <string>
#include <vector>

namespace lang {

// One MIR-derived finding of a borrow audit that semantics also performs
// (RFC 0004 phase 2 re-host). It names the executable instruction whose access
// overlaps a live checked loan and the loans it overlaps. Source mapping is the
// caller's: `hirValue`/`hirStatement`/`callSite` name the instruction's HIR
// origin in the body's HIR counterpart.
struct MirBorrowAuditFinding {
  MirBodyAddress body;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  HirValueId hirValue = 0;
  HirStatementId hirStatement = 0;
  HirValueId callSite = 0;
  // The binding an instruction without an HIR value acts on (a scope-exit
  // drop names the destroyed local).
  HirBindingId binding = 0;
  std::vector<MirLoanId> loans;
  std::string detail;
};

// Raw-pointer audit: a raw dereference/index access whose address may
// designate storage protected by a live, incompatible checked loan. Loan
// liveness is MIR's own: a loan is live between the instruction that starts it
// (or body entry) and its materialized `EndBorrow`. Raw address provenance is
// a flow-sensitive MIR analysis of `AddressOf`, pointer arithmetic, loads and
// stores; raw pointers carry no loans.
[[nodiscard]] std::vector<MirBorrowAuditFinding>
auditMirRawAccesses(const MirProgram &program);

// Cross-function persistent storage: an invocation whose transitive callees
// (program effect closure over executable MIR) access a global or thread-local
// place that a live loan of the calling body protects with an incompatible
// access. Reuses the persistent-storage effect closure and the body loan-flow
// engine of `verifyMirPersistentStorageEffects`, collecting every rejected
// invocation instead of stopping at the first. Defined in
// mir_storage_effects.cpp.
[[nodiscard]] std::vector<MirBorrowAuditFinding>
auditMirPersistentStorageBorrows(const MirProgram &program);

} // namespace lang
