#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <map>
#include <utility>
#include <vector>

namespace lang {

// Whole-program facts that hold only because a MIR program is closed: every
// runtime invocation of a closed function is a visible static MIR call in
// some program body. The facts are derived from verified MIR alone; they never
// recognize library names, count implementations, or trust backend emission.
//
// A function is open, and contributes no facts, when anything other than such
// a call may invoke it: an entry or hosted-startup root, an embedding export,
// a native-callback or thread-task adapter, a function-item value, a
// callable-parameter binding, an intrinsic or native-callee call, virtual
// dispatch, non-GTI linkage, or a non-source definition.
//
// For a reference parameter of a closed function whose referent is a class,
// `exactParameterClass` reports the one concrete class that every visible call
// passes. A call contributes that class only when its argument borrows a
// complete binding or temporary object, or a field of one, whose dynamic class
// is its static class; or when it forwards a caller's reference parameter that
// is itself exact. Any other argument, conflicting classes, a parameter with
// no visible call, or a function body that may rebind the reference makes the
// parameter unknown.
class MirClosedWorldFacts {
public:
  [[nodiscard]] bool closed(HirFunctionInstanceId function) const;
  [[nodiscard]] const SemanticType *
  exactParameterClass(HirFunctionInstanceId function,
                      std::size_t parameter) const;
  [[nodiscard]] std::size_t iterations() const { return iterations_; }

  friend bool operator==(const MirClosedWorldFacts &,
                         const MirClosedWorldFacts &) = default;

private:
  friend MirClosedWorldFacts
  analyzeMirClosedWorldFacts(const MirProgram &program);

  // Indexed by function instance ID - 1.
  std::vector<bool> closed_;
  std::map<std::pair<HirFunctionInstanceId, std::size_t>, SemanticType>
      exactParameters_;
  std::size_t iterations_ = 0;
};

[[nodiscard]] MirClosedWorldFacts
analyzeMirClosedWorldFacts(const MirProgram &program);

} // namespace lang
