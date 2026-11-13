#pragma once

#include "gti/diagnostic.h"
#include "gti/frontend.h"
#include "gti/hir.h"
#include "gti/mir.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace lang {

// Development measurement for RFC 0004 phase 2: the raw-pointer audit's
// live-borrow finding and the cross-function persistent-storage GTI-S2045 are
// still decided by semantic loan flow, and their MIR re-hosts
// (mir_borrow_audits.h) run alongside them. With GTI_PARITY_RAW_AUDIT set
// ("1" for stderr, otherwise a file to append to), one frontend analysis
// compares the semantic findings with the MIR findings by source line and
// prints every agreement and mismatch. Without it nothing here runs and
// compilation is unchanged.
//
// A semantic persistent-storage rejection would stop the pipeline before MIR;
// while measuring, such a program continues to MIR with those diagnostics set
// aside, and `finish` restores them and the semantic rejection.
class MirBorrowAuditParityScope {
public:
  explicit MirBorrowAuditParityScope(const FrontendOptions &options);
  MirBorrowAuditParityScope(const MirBorrowAuditParityScope &) = delete;
  MirBorrowAuditParityScope &
  operator=(const MirBorrowAuditParityScope &) = delete;
  ~MirBorrowAuditParityScope();

  // The scope of the analysis on this thread, or null when not measuring.
  [[nodiscard]] static MirBorrowAuditParityScope *active();

  // After semantic analysis: records the semantic findings in
  // diagnostics[begin, end). Returns true when the only semantic errors are
  // persistent-storage findings, which it then removes for measurement.
  bool semanticFindings(std::vector<Diagnostic> &diagnostics,
                        std::size_t begin);
  // The first lowered MIR program covering the application bodies.
  void observe(const MirProgram &program, const HirProgram &hir);
  void finish(FrontendResult &result);

private:
  struct Observed {
    std::string kind;
    SourceSpan span;
    std::string detail;
  };

  bool rawAudit = false;
  bool reportLibrary = false;
  bool observed = false;
  bool forced = false;
  std::string destination;
  std::vector<Diagnostic> semantic;
  std::vector<Diagnostic> original;
  std::vector<Observed> mir;
  MirBorrowAuditParityScope *previous = nullptr;
};

} // namespace lang
