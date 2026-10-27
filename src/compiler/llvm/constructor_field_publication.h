#pragma once

#include "gti/mir.h"

namespace lang::native_llvm {

// A constructor record may leave one field initializer unstaged in its body.
// The shared MIR query identifies that source record; this plan additionally
// proves that the record has one exact SSA producer and one exact destination
// field. Admission decides whether the producer's concrete storage transport
// is implemented, while body emission consumes the same exact plan.
enum class UnstagedConstructorFieldPublicationTransport {
  None,
  ExactValue,
  // A source null literal published into a field represented by one pointer:
  // a unique owner of a complete object, a C string or a raw pointer.
  NullPointer,
};

struct UnstagedConstructorFieldPublicationPlan final {
  const MirConstructorInitializer *initializer = nullptr;
  MirValueId value = 0;
  SemanticType sourceType = SemanticType::Unknown;
  SemanticType targetType = SemanticType::Unknown;
  UnstagedConstructorFieldPublicationTransport transport =
      UnstagedConstructorFieldPublicationTransport::None;
  bool exact = false;

  [[nodiscard]] bool matched() const { return initializer != nullptr; }
};

enum class StoredFieldPublicationKind {
  None,
  ReferenceAddress,
  ProvenanceOnly,
};

// Stored loans can either publish a physical reference field or retain the
// provenance carried inside an already-published by-value object. This exact
// constructor query keeps that distinction out of opcode/type heuristics at
// emission sites.
struct StoredFieldPublicationPlan final {
  const MirConstructorInitializer *initializer = nullptr;
  const MirClassFieldInfo *field = nullptr;
  StoredFieldPublicationKind kind = StoredFieldPublicationKind::None;
  bool requested = false;
  bool exact = false;

  [[nodiscard]] bool matched() const { return requested; }
};

[[nodiscard]] UnstagedConstructorFieldPublicationPlan
unstagedConstructorFieldPublication(const MirProgram &program,
                                    const MirBody &body,
                                    HirConstructorInstanceId constructor,
                                    const MirInstruction &instruction);

[[nodiscard]] StoredFieldPublicationPlan
storedFieldPublication(const MirProgram &program, const MirBody &body,
                       HirConstructorInstanceId constructor,
                       const MirLoan &loan);

} // namespace lang::native_llvm
