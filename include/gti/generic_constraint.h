#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lang {

enum class GenericConstraintKind : std::uint8_t {
  None,
  Invalid,
  Numeric,
  SignedNumeric,
  Integral,
  SignedIntegral,
  UnsignedIntegral,
  FloatingPoint,
  Copyable,
  Movable,
  DefaultInitializable,
  EqualityComparable,
  RelationallyOrdered,
  Count,
};

using GenericConstraintSet = std::uint32_t;

static_assert(static_cast<std::uint8_t>(GenericConstraintKind::Count) <=
              sizeof(GenericConstraintSet) * 8);

struct CompilerConstraintRecord {
  std::string_view binding;
  GenericConstraintKind kind;
};

inline constexpr std::array<GenericConstraintKind, 11> genericConstraintKinds{{
    GenericConstraintKind::Numeric,
    GenericConstraintKind::SignedNumeric,
    GenericConstraintKind::Integral,
    GenericConstraintKind::SignedIntegral,
    GenericConstraintKind::UnsignedIntegral,
    GenericConstraintKind::FloatingPoint,
    GenericConstraintKind::Copyable,
    GenericConstraintKind::Movable,
    GenericConstraintKind::DefaultInitializable,
    GenericConstraintKind::EqualityComparable,
    GenericConstraintKind::RelationallyOrdered,
}};

inline constexpr std::array<CompilerConstraintRecord, 11> compilerConstraints{{
    {"numeric", GenericConstraintKind::Numeric},
    {"signed_numeric", GenericConstraintKind::SignedNumeric},
    {"integral", GenericConstraintKind::Integral},
    {"signed_integral", GenericConstraintKind::SignedIntegral},
    {"unsigned_integral", GenericConstraintKind::UnsignedIntegral},
    {"floating_point", GenericConstraintKind::FloatingPoint},
    {"copyable", GenericConstraintKind::Copyable},
    {"movable", GenericConstraintKind::Movable},
    {"default_initializable", GenericConstraintKind::DefaultInitializable},
    {"equality_comparable", GenericConstraintKind::EqualityComparable},
    {"relationally_ordered", GenericConstraintKind::RelationallyOrdered},
}};

static_assert(compilerConstraints.size() + 2 ==
              static_cast<std::uint8_t>(GenericConstraintKind::Count));

[[nodiscard]] constexpr GenericConstraintSet
constraintBit(GenericConstraintKind constraint) {
  if (constraint == GenericConstraintKind::None ||
      constraint == GenericConstraintKind::Count) {
    return 0;
  }
  return GenericConstraintSet{1}
         << static_cast<GenericConstraintSet>(constraint);
}

[[nodiscard]] constexpr bool hasConstraint(GenericConstraintSet constraints,
                                           GenericConstraintKind constraint) {
  return (constraints & constraintBit(constraint)) != 0;
}

[[nodiscard]] constexpr bool
invalidConstraintSet(GenericConstraintSet constraints) {
  return hasConstraint(constraints, GenericConstraintKind::Invalid);
}

[[nodiscard]] constexpr std::optional<GenericConstraintKind>
compilerConstraint(std::string_view binding) {
  for (const CompilerConstraintRecord &constraint : compilerConstraints) {
    if (constraint.binding == binding) {
      return constraint.kind;
    }
  }
  return std::nullopt;
}

} // namespace lang
