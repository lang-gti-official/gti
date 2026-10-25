#pragma once

#include "gti/mir.h"

#include <map>

namespace lang::native_llvm {

// Class-containing aggregates carry object identity, not LLVM SSA snapshots.
[[nodiscard]] inline bool addressValue(const SemanticType &type) {
  return type.kind == SemanticType::Class ||
         type.kind == SemanticType::Lambda ||
         (type.kind == SemanticType::Array && type.arguments.size() == 1 &&
          addressValue(type.arguments[0]));
}

// Fresh values have one physical destination. This plan follows only verified
// value transport; it never elides a MIR Copy/Move or aliases a named local to
// the caller's return storage.
class Placement final {
public:
  struct Target {
    MirPlaceId place = 0;
    MirValueId aggregate = 0;
    std::size_t element = 0;
    bool returned = false;
    friend bool operator==(const Target &, const Target &) = default;
  };
  explicit Placement(const MirBody &body);
  [[nodiscard]] const Target *target(MirValueId value) const;
  [[nodiscard]] bool fresh(MirValueId value) const;

private:
  std::map<MirValueId, const MirInstruction *> producers;
  std::map<MirValueId, Target> targets;
  void bind(MirValueId value, Target target);
};

} // namespace lang::native_llvm
