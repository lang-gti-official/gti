#pragma once

#include "gti/mir_effects.h"

#include <map>
#include <utility>

namespace lang::mir_detail {

enum class FailureTarget { Function, Constructor, Destructor };

struct FailureInput {
  FailureTarget kind;
  std::size_t id;
  bool mayRaise;
};

// Body lowering can see the failure fixed point only through these queries.
// An absent summary is conservative. Record even a read of an absent summary:
// the next round may prove that target cannot raise.
class BodyFailureInputs {
public:
  BodyFailureInputs(const MirDefinedFailureEffects &effects,
                    std::vector<FailureInput> *reads)
      : effects(effects), reads(reads) {}

  bool mayRaise(FailureTarget kind, std::size_t id) const {
    const auto &table = kind == FailureTarget::Function ? effects.functions
                        : kind == FailureTarget::Constructor
                            ? effects.constructors
                            : effects.destructors;
    const bool result = !id || id > table.size() || table[id - 1];
    if (reads)
      reads->push_back({kind, id, result});
    return result;
  }

private:
  const MirDefinedFailureEffects &effects;
  std::vector<FailureInput> *reads;
};

// Lives inside one lowerVerified invocation: HIR, failure sites, signatures,
// cleanup-sharing mode and source identities stay fixed. Only the queried
// callee effects can change between rounds. An earlier valid program hands
// off its body by move, before any optimizer or editor can mutate it.
struct LoweringRounds {
  using Address = std::pair<MirBodyKind, std::size_t>;
  std::map<Address, std::vector<FailureInput>> dependencies;
  MirProgram *previous = nullptr;

  MirBody *reusable(MirBodyAddress address,
                    const MirDefinedFailureEffects &effects) const {
    if (!previous || !previous->valid())
      return nullptr;
    auto inputs = dependencies.find({address.kind, address.owner});
    if (inputs == dependencies.end())
      return nullptr;
    BodyFailureInputs current(effects, nullptr);
    for (const auto &input : inputs->second)
      if (current.mayRaise(input.kind, input.id) != input.mayRaise)
        return nullptr;
    return findMirBody(*previous, address);
  }
};
} // namespace lang::mir_detail
