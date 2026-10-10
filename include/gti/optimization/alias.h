#pragma once

#include "gti/mir.h"

#include <cstdint>
#include <vector>

namespace lang {

enum class MirAliasRelation : std::uint8_t {
  MustAlias,
  NoAlias,
  MayAlias,
};

enum class MirPlaceEscapeReason : std::uint8_t {
  AddressTaken = 1U << 0U,
  Returned = 1U << 1U,
  RetainedBorrow = 1U << 2U,
  StoredOutsideLocal = 1U << 3U,
  UnknownCall = 1U << 4U,
  ExactCall = 1U << 5U,
  RawMemory = 1U << 6U,
};

struct MirPlaceEscapeFact {
  MirPlaceId place = 0;
  std::uint8_t reasons = 0;

  [[nodiscard]] bool has(MirPlaceEscapeReason reason) const {
    return (reasons & static_cast<std::uint8_t>(reason)) != 0;
  }

  [[nodiscard]] bool escapesBody() const {
    constexpr std::uint8_t escaping =
        static_cast<std::uint8_t>(MirPlaceEscapeReason::Returned) |
        static_cast<std::uint8_t>(MirPlaceEscapeReason::RetainedBorrow) |
        static_cast<std::uint8_t>(MirPlaceEscapeReason::StoredOutsideLocal) |
        static_cast<std::uint8_t>(MirPlaceEscapeReason::UnknownCall) |
        static_cast<std::uint8_t>(MirPlaceEscapeReason::ExactCall) |
        static_cast<std::uint8_t>(MirPlaceEscapeReason::RawMemory);
    return (reasons & escaping) != 0;
  }

  friend bool operator==(const MirPlaceEscapeFact &,
                         const MirPlaceEscapeFact &) = default;
};

// Snapshot-scoped place facts. The dense relation table is deterministic and
// intentionally body-local; it does not assert interprocedural pointer
// identity. Exact call summaries contribute escape facts only.
struct MirPlaceAliasEscapeAnalysis {
  std::size_t placeCount = 0;
  std::vector<MirAliasRelation> relations;
  std::vector<MirPlaceEscapeFact> escapes;

  [[nodiscard]] MirAliasRelation relation(MirPlaceId left,
                                          MirPlaceId right) const;
  [[nodiscard]] const MirPlaceEscapeFact *escape(MirPlaceId place) const;

  friend bool operator==(const MirPlaceAliasEscapeAnalysis &,
                         const MirPlaceAliasEscapeAnalysis &) = default;
};

[[nodiscard]] MirAliasRelation mirPlaceRelation(const MirProgram &program,
                                                const MirPlace &left,
                                                const MirPlace &right);

[[nodiscard]] MirPlaceAliasEscapeAnalysis
analyzeMirPlaceAliasEscape(const MirProgram &program, MirBodyAddress address);

// What an exact source body may do with an input's identity: the receiver's
// own storage, or the referents of a reference or borrow-carrying parameter.
// ReturnOnly reaches the caller only through the body's result, which the
// caller tracks as its own value or loan. Captured covers every other route:
// a raw address, a store into any place, a closure, synchronization or an
// unknown, native, dynamic or polymorphic callee.
enum class MirCaptureClass : std::uint8_t {
  None,
  ReturnOnly,
  Captured,
};

struct MirBodyCaptureSummary {
  // Captures on paths that return normally. A caller applies them on the
  // successful continuation of its call.
  MirCaptureClass receiver = MirCaptureClass::None;
  std::vector<MirCaptureClass> parameters;
  // Captures on any path, including failure paths that only clean up. A
  // caller applies them on its call's failure edge.
  MirCaptureClass failureReceiver = MirCaptureClass::None;
  std::vector<MirCaptureClass> failureParameters;
  // The object this constructor builds, or the object a function returns by
  // value from such a construction, may have had its own address captured.
  // A caller adopting it as a local inherits that capture.
  bool capturesResult = false;

  friend bool operator==(const MirBodyCaptureSummary &,
                         const MirBodyCaptureSummary &) = default;
};

// Least-fixpoint capture summaries of the program's exact function,
// constructor, destructor and field-initializer bodies. A body that is
// missing here is unknown to callers and captures every input.
struct MirProgramCaptureSummaries {
  std::vector<std::pair<MirBodyAddress, MirBodyCaptureSummary>> bodies;

  [[nodiscard]] const MirBodyCaptureSummary *find(MirBodyAddress body) const;
};

[[nodiscard]] MirProgramCaptureSummaries
deriveMirProgramCaptureSummaries(const MirProgram &program);

// Flow-sensitive capture of one body's local bindings. A binding is captured
// before an instruction when a capturing instruction (or an escaping loan of
// it) may execute earlier on some path, including around loop back edges.
// Code that has not received a binding's identity cannot reach its storage.
// Queries about unknown instructions or an invalid analysis answer true.
struct MirLocalCaptureAnalysis {
  bool valid = false;
  std::vector<std::pair<MirInstructionId, std::vector<HirBindingId>>>
      capturedBefore;
  // Bindings whose storage or identity the instruction itself takes as an
  // input, receiver or destination, by value, address or loan.
  std::vector<std::pair<MirInstructionId, std::vector<HirBindingId>>> involved;

  [[nodiscard]] bool mayBeCapturedBefore(HirBindingId binding,
                                         MirInstructionId instruction) const;
  [[nodiscard]] bool involves(HirBindingId binding,
                              MirInstructionId instruction) const;
};

[[nodiscard]] MirLocalCaptureAnalysis
analyzeMirLocalCaptures(const MirProgram &program, MirBodyAddress address,
                        const MirProgramCaptureSummaries &summaries);

} // namespace lang
