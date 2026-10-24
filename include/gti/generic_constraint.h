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
  Relocatable,
  BitwiseRelocatable,
  Transferable,
  Shareable,
  DefaultInitializable,
  AtomicScalar,
  MoveConstructible,
  IntegralEnum,
  // Every instance field of a struct or class is readable through
  // reflection where the constraint is checked (std::meta::reflectable).
  Reflectable,
  Boolean,
  ObjectLayout,
  BorrowFree,
  Count,
};

// Relational capabilities cannot be represented by the unary constraint
// bitset above.  They describe exact operations involving one or more types
// and are carried separately through generic semantic analysis.
enum class StructuralConstraintKind : std::uint8_t {
  None,
  ReadMember,
  Iteration,
  SizedRange,
  EnumUnderlying,
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

inline constexpr std::array<GenericConstraintKind, 21> genericConstraintKinds{{
    GenericConstraintKind::Numeric,
    GenericConstraintKind::SignedNumeric,
    GenericConstraintKind::Integral,
    GenericConstraintKind::SignedIntegral,
    GenericConstraintKind::UnsignedIntegral,
    GenericConstraintKind::FloatingPoint,
    GenericConstraintKind::CopyConstructible,
    GenericConstraintKind::Copyable,
    GenericConstraintKind::Movable,
    GenericConstraintKind::MoveConstructible,
    GenericConstraintKind::Relocatable,
    GenericConstraintKind::BitwiseRelocatable,
    GenericConstraintKind::Transferable,
    GenericConstraintKind::Shareable,
    GenericConstraintKind::DefaultInitializable,
    GenericConstraintKind::AtomicScalar,
    GenericConstraintKind::IntegralEnum,
    GenericConstraintKind::Reflectable,
    GenericConstraintKind::Boolean,
    GenericConstraintKind::ObjectLayout,
    GenericConstraintKind::BorrowFree,
}};

// Only facts still bound by private concept declarations belong here. The
// complete fact inventory above also serves reflection and generic proofs.
inline constexpr std::array<CompilerConstraintRecord, 0> compilerConstraints{};

inline constexpr std::array<CompilerStructuralConstraintRecord, 1>
    compilerStructuralConstraints{{
        {"sized_range", StructuralConstraintKind::SizedRange, 1},
    }};

static_assert(genericConstraintKinds.size() + 2 ==
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
