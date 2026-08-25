#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <string>
#include <vector>

namespace lang {

// Backend-private, pointer-free inventory of the direct C++ control-flow
// representation for one sealed optimized MIR body. Edge ordinals use MIR's
// canonical slots: Goto/default/success are zero, Branch/Invoke else is one,
// and Switch cases are one-based after the default edge.
struct CppMirControlFlowEdge {
  MirBlockId target = 0;
  std::size_t ordinal = 0;

  friend bool operator==(const CppMirControlFlowEdge &,
                         const CppMirControlFlowEdge &) = default;
};

struct CppMirControlFlowBlock {
  MirBlockId block = 0;
  std::string label;
  std::vector<CppMirControlFlowEdge> outgoing;

  friend bool operator==(const CppMirControlFlowBlock &,
                         const CppMirControlFlowBlock &) = default;
};

struct CppMirControlFlowPlan {
  MirBlockId entry = 0;
  std::vector<CppMirControlFlowBlock> blocks;

  friend bool operator==(const CppMirControlFlowPlan &,
                         const CppMirControlFlowPlan &) = default;
};

enum class CppMirControlFlowIssueKind {
  InvalidMirBody,
  EntryCoverage,
  BlockCoverage,
  LabelCoverage,
  EdgeCoverage,
};

struct CppMirControlFlowIssue {
  CppMirControlFlowIssueKind kind = CppMirControlFlowIssueKind::InvalidMirBody;
  MirBlockId block = 0;
  std::size_t edgeOrdinal = 0;
  std::string detail;
};

struct CppMirControlFlowVerification {
  std::vector<CppMirControlFlowIssue> issues;

  [[nodiscard]] bool valid() const { return issues.empty(); }
};

[[nodiscard]] std::string cppMirControlFlowLabel(MirBlockId block);

// The builder is deliberately mechanical: it copies only target-representation
// facts from MIR. The verifier is a separate exact-cover gate so tests can
// mutate a plan and production emission can fail closed before spelling text.
[[nodiscard]] CppMirControlFlowPlan
buildCppMirControlFlowPlan(const MirBody &body);

[[nodiscard]] CppMirControlFlowVerification
verifyCppMirControlFlowPlan(const MirBody &body,
                            const CppMirControlFlowPlan &plan);

[[nodiscard]] const CppMirControlFlowBlock *
findCppMirControlFlowBlock(const CppMirControlFlowPlan &plan, MirBlockId block);

[[nodiscard]] const CppMirControlFlowEdge *
findCppMirControlFlowEdge(const CppMirControlFlowPlan &plan, MirBlockId source,
                          std::size_t ordinal);

} // namespace lang
