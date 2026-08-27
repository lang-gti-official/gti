#pragma once

#include <array>
#include <cstddef>
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
  CopyConstructible,
  Copyable,
  Movable,
  Transferable,
  Shareable,
  DefaultInitializable,
  EqualityComparable,
  RelationallyOrdered,
  Count,
};

// Relational capabilities cannot be represented by the unary constraint
// bitset above.  They describe exact operations involving one or more types
// and are carried separately through generic semantic analysis.
enum class StructuralConstraintKind : std::uint8_t {
  None,
  InputIterator,
  SentinelFor,
  DereferencesTo,
  WritableIterator,
  WritesTo,
  BidirectionalIterator,
  RandomAccessIterator,
  SubscriptsToMut,
  SizedSentinelFor,
  Adds,
  Subtracts,
  Negates,
  ScalesBy,
};

using GenericConstraintSet = std::uint32_t;

static_assert(static_cast<std::uint8_t>(GenericConstraintKind::Count) <=
              sizeof(GenericConstraintSet) * 8);

struct CompilerConstraintRecord {
  std::string_view binding;
  GenericConstraintKind kind;
};

struct CompilerStructuralConstraintRecord {
  std::string_view binding;
  StructuralConstraintKind kind;
  std::size_t arity;
};

inline constexpr std::array<GenericConstraintKind, 14> genericConstraintKinds{{
    GenericConstraintKind::Numeric,
    GenericConstraintKind::SignedNumeric,
    GenericConstraintKind::Integral,
    GenericConstraintKind::SignedIntegral,
    GenericConstraintKind::UnsignedIntegral,
    GenericConstraintKind::FloatingPoint,
    GenericConstraintKind::CopyConstructible,
    GenericConstraintKind::Copyable,
    GenericConstraintKind::Movable,
    GenericConstraintKind::Transferable,
    GenericConstraintKind::Shareable,
    GenericConstraintKind::DefaultInitializable,
    GenericConstraintKind::EqualityComparable,
    GenericConstraintKind::RelationallyOrdered,
}};

inline constexpr std::array<CompilerConstraintRecord, 14> compilerConstraints{{
    {"numeric", GenericConstraintKind::Numeric},
    {"signed_numeric", GenericConstraintKind::SignedNumeric},
    {"integral", GenericConstraintKind::Integral},
    {"signed_integral", GenericConstraintKind::SignedIntegral},
    {"unsigned_integral", GenericConstraintKind::UnsignedIntegral},
    {"floating_point", GenericConstraintKind::FloatingPoint},
    {"copy_constructible", GenericConstraintKind::CopyConstructible},
    {"copyable", GenericConstraintKind::Copyable},
    {"movable", GenericConstraintKind::Movable},
    {"transferable", GenericConstraintKind::Transferable},
    {"shareable", GenericConstraintKind::Shareable},
    {"default_initializable", GenericConstraintKind::DefaultInitializable},
    {"equality_comparable", GenericConstraintKind::EqualityComparable},
    {"relationally_ordered", GenericConstraintKind::RelationallyOrdered},
}};

inline constexpr std::array<CompilerStructuralConstraintRecord, 13>
    compilerStructuralConstraints{{
        {"input_iterator", StructuralConstraintKind::InputIterator, 1},
        {"sentinel_for", StructuralConstraintKind::SentinelFor, 2},
        {"dereferences_to", StructuralConstraintKind::DereferencesTo, 2},
        {"writable_iterator", StructuralConstraintKind::WritableIterator, 1},
        {"writes_to", StructuralConstraintKind::WritesTo, 2},
        {"bidirectional_iterator",
         StructuralConstraintKind::BidirectionalIterator, 1},
        {"random_access_iterator",
         StructuralConstraintKind::RandomAccessIterator, 1},
        {"subscripts_to_mut", StructuralConstraintKind::SubscriptsToMut, 2},
        {"sized_sentinel_for", StructuralConstraintKind::SizedSentinelFor, 2},
        {"adds", StructuralConstraintKind::Adds, 1},
        {"subtracts", StructuralConstraintKind::Subtracts, 1},
        {"negates", StructuralConstraintKind::Negates, 1},
        {"scales_by", StructuralConstraintKind::ScalesBy, 2},
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

[[nodiscard]] constexpr std::optional<CompilerStructuralConstraintRecord>
compilerStructuralConstraint(std::string_view binding) {
  for (const CompilerStructuralConstraintRecord &constraint :
       compilerStructuralConstraints) {
    if (constraint.binding == binding) {
      return constraint;
    }
  }
  return std::nullopt;
}

} // namespace lang
