#pragma once

#include "gti/mir.h"
#include "gti/optimization/closed_world.h"

#include <cstddef>
#include <optional>
#include <string>

namespace lang {

// A bounded exact-receiver proof. It is not speculation from the number of
// implementations. The local form proves a non-escaping, fully constructed
// local object, optionally viewed through one checked local interface
// reference. The parameter form proves a function's own reference parameter
// whose referent class is exact on every visible call of a closed function
// (`MirClosedWorldFacts`); the receiver keeps its interface place and the
// call records the exact class it is presented at.
struct MirExactReceiverProof {
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  MirInstructionId receiverInput = 0;
  MirValueId receiverValue = 0;
  // The concrete local object, or the dereferenced receiver parameter.
  MirPlaceId concretePlace = 0;
  HirFunctionInstanceId target = 0;
  std::optional<std::size_t> parameter;
  SemanticType exactClass = SemanticType::Unknown;

  friend bool operator==(const MirExactReceiverProof &,
                         const MirExactReceiverProof &) = default;
};

[[nodiscard]] std::optional<MirExactReceiverProof>
proveMirExactReceiver(const MirProgram &program, MirBodyAddress address,
                      MirInstructionId instruction,
                      std::string *reason = nullptr,
                      const MirClosedWorldFacts *closedWorld = nullptr);

// Re-derives the proof before refining the receiver input and selected call.
// No CFG, failure origin, operand evaluation, allocation or cleanup is removed.
// Parameter proofs are available only when `closedWorld` was derived from the
// same program snapshot.
[[nodiscard]] std::optional<MirTransformRecord>
rewriteMirExactReceiver(MirProgram &program, MirBodyAddress address,
                        MirInstructionId instruction, std::string pass,
                        const MirClosedWorldFacts *closedWorld = nullptr);

} // namespace lang
