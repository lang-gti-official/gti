#pragma once

#include "gti/diagnostic.h"
#include "gti/hir.h"
#include "gti/mir.h"

#include <string>
#include <vector>

namespace lang {

// User diagnostics for MIR's borrow audits (mir_borrow_audits.h). MIR is the
// authority for both: the raw-pointer audit's live-borrow finding
// (GTI-S2102, an audit warning that strict mode promotes) and the
// cross-function persistent-storage rejection (GTI-S2045). Every finding is
// anchored at its instruction's HIR source; `fallbackSource` names the file
// for the rare instruction with no source at all.
[[nodiscard]] std::vector<Diagnostic>
mirRawAuditDiagnostics(const MirProgram &program, const HirProgram &hir,
                       const std::string &fallbackSource);

[[nodiscard]] std::vector<Diagnostic>
mirPersistentStorageDiagnostics(const MirProgram &program,
                                const HirProgram &hir,
                                const std::string &fallbackSource);

} // namespace lang
