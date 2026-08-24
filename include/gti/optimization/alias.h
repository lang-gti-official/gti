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

[[nodiscard]] MirAliasRelation mirPlaceRelation(const MirPlace &left,
                                                const MirPlace &right);

[[nodiscard]] MirPlaceAliasEscapeAnalysis
analyzeMirPlaceAliasEscape(const MirProgram &program, MirBodyAddress address);

} // namespace lang
