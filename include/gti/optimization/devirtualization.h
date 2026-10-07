#pragma once

#include "gti/mir.h"

#include <optional>
#include <string>

namespace lang {

// A bounded exact-receiver proof. This is not whole-program type speculation:
// the object is a non-escaping, fully constructed local, optionally viewed
// through one checked local interface reference.
struct MirExactReceiverProof {
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  MirInstructionId receiverInput = 0;
  MirValueId receiverValue = 0;
  MirPlaceId concretePlace = 0;
  HirFunctionInstanceId target = 0;

  friend bool operator==(const MirExactReceiverProof &,
                         const MirExactReceiverProof &) = default;
};

[[nodiscard]] std::optional<MirExactReceiverProof>
proveMirExactReceiver(const MirProgram &program, MirBodyAddress address,
                      MirInstructionId instruction,
                      std::string *reason = nullptr);

// Re-derives the proof before refining the receiver input and selected call.
// No CFG, failure origin, operand evaluation, allocation or cleanup is removed.
[[nodiscard]] std::optional<MirTransformRecord>
rewriteMirExactReceiver(MirProgram &program, MirBodyAddress address,
                        MirInstructionId instruction, std::string pass);

} // namespace lang
