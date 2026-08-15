#pragma once

#include "gti/source_graph.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace lang {

enum class DefinedFailureCode : std::uint16_t {
  None = 0,
  IntegerOverflow = 1,
  DivisionByZero = 2,
  ModuloByZero = 3,
  NegativeShiftCount = 4,
  ShiftCountOutOfRange = 5,
  NumericConversionOutOfRange = 6,
  IndexOutOfBounds = 7,
  EmptyOwnerAccess = 8,
  InvalidExpectedAccess = 9,
  InvalidStorageState = 10,
  AllocationFailure = 11,
  InfallibleHostOperationFailed = 12,
  HostedRuntimeContractFailure = 13,
  Count,
};

enum class DefinedFailureDetail : std::uint8_t {
  None,
  Addition,
  Subtraction,
  Multiplication,
  Division,
  Negation,
  IntegerDivision,
  IntegerModulo,
  LeftShift,
  RightShift,
  NumericCast,
  HostedArgumentCount,
  FixedArray,
  StringView,
  Vector,
  String,
  PrivateStorage,
  Dereference,
  MemberAccess,
  ValueOnError,
  ErrorOnValue,
  DuplicateConstruction,
  UninitializedAccess,
  RelocationCapacity,
  InvalidRelocationSource,
  OccupiedRelocationDestination,
  UniqueOwner,
  ElementConstruction,
  HostedArguments,
  StdoutWrite,
  AutomaticJoin,
  NegativeArgumentCount,
  RecursiveThreadLocalInitialization,
  Count,
};

struct DefinedFailureOutcome {
  DefinedFailureCode code = DefinedFailureCode::None;
  DefinedFailureDetail detail = DefinedFailureDetail::None;

  friend bool operator==(const DefinedFailureOutcome &,
                         const DefinedFailureOutcome &) = default;
};

struct DefinedFailureOrigin {
  std::vector<DefinedFailureOutcome> outcomes;
  SourceUnitId sourceUnit = 0;
  std::size_t start = 0;
  std::size_t end = 0;
  int line = 1;

  friend bool operator==(const DefinedFailureOrigin &,
                         const DefinedFailureOrigin &) = default;
};

enum class FailurePropagationKind : std::uint8_t {
  None,
  DirectCall,
  VirtualCall,
  Constructor,
  Callable,
  TaskJoin,
  BodyCall,
  Destructor,
  Count,
};

struct DefinedFailureOperation {
  std::vector<DefinedFailureOrigin> localOrigins;
  FailurePropagationKind propagation = FailurePropagationKind::None;

  [[nodiscard]] bool empty() const {
    return localOrigins.empty() && propagation == FailurePropagationKind::None;
  }

  friend bool operator==(const DefinedFailureOperation &,
                         const DefinedFailureOperation &) = default;
};

[[nodiscard]] constexpr bool validDefinedFailureCode(DefinedFailureCode code) {
  return code > DefinedFailureCode::None && code < DefinedFailureCode::Count;
}

[[nodiscard]] constexpr bool
validDefinedFailureDetail(DefinedFailureDetail detail) {
  return detail > DefinedFailureDetail::None &&
         detail < DefinedFailureDetail::Count;
}

[[nodiscard]] constexpr bool
validDefinedFailureOutcome(DefinedFailureOutcome outcome) {
  if (!validDefinedFailureCode(outcome.code) ||
      !validDefinedFailureDetail(outcome.detail)) {
    return false;
  }
  switch (outcome.code) {
  case DefinedFailureCode::IntegerOverflow:
    return outcome.detail == DefinedFailureDetail::Addition ||
           outcome.detail == DefinedFailureDetail::Subtraction ||
           outcome.detail == DefinedFailureDetail::Multiplication ||
           outcome.detail == DefinedFailureDetail::Division ||
           outcome.detail == DefinedFailureDetail::Negation;
  case DefinedFailureCode::DivisionByZero:
    return outcome.detail == DefinedFailureDetail::IntegerDivision;
  case DefinedFailureCode::ModuloByZero:
    return outcome.detail == DefinedFailureDetail::IntegerModulo;
  case DefinedFailureCode::NegativeShiftCount:
  case DefinedFailureCode::ShiftCountOutOfRange:
    return outcome.detail == DefinedFailureDetail::LeftShift ||
           outcome.detail == DefinedFailureDetail::RightShift;
  case DefinedFailureCode::NumericConversionOutOfRange:
    return outcome.detail == DefinedFailureDetail::NumericCast ||
           outcome.detail == DefinedFailureDetail::HostedArgumentCount;
  case DefinedFailureCode::IndexOutOfBounds:
    return outcome.detail == DefinedFailureDetail::FixedArray ||
           outcome.detail == DefinedFailureDetail::StringView ||
           outcome.detail == DefinedFailureDetail::Vector ||
           outcome.detail == DefinedFailureDetail::String ||
           outcome.detail == DefinedFailureDetail::PrivateStorage;
  case DefinedFailureCode::EmptyOwnerAccess:
    return outcome.detail == DefinedFailureDetail::Dereference ||
           outcome.detail == DefinedFailureDetail::MemberAccess;
  case DefinedFailureCode::InvalidExpectedAccess:
    return outcome.detail == DefinedFailureDetail::ValueOnError ||
           outcome.detail == DefinedFailureDetail::ErrorOnValue;
  case DefinedFailureCode::InvalidStorageState:
    return outcome.detail == DefinedFailureDetail::DuplicateConstruction ||
           outcome.detail == DefinedFailureDetail::UninitializedAccess ||
           outcome.detail == DefinedFailureDetail::RelocationCapacity ||
           outcome.detail == DefinedFailureDetail::InvalidRelocationSource ||
           outcome.detail ==
               DefinedFailureDetail::OccupiedRelocationDestination;
  case DefinedFailureCode::AllocationFailure:
    return outcome.detail == DefinedFailureDetail::UniqueOwner ||
           outcome.detail == DefinedFailureDetail::PrivateStorage ||
           outcome.detail == DefinedFailureDetail::ElementConstruction ||
           outcome.detail == DefinedFailureDetail::HostedArguments;
  case DefinedFailureCode::InfallibleHostOperationFailed:
    return outcome.detail == DefinedFailureDetail::StdoutWrite ||
           outcome.detail == DefinedFailureDetail::AutomaticJoin;
  case DefinedFailureCode::HostedRuntimeContractFailure:
    return outcome.detail == DefinedFailureDetail::NegativeArgumentCount ||
           outcome.detail ==
               DefinedFailureDetail::RecursiveThreadLocalInitialization;
  case DefinedFailureCode::None:
  case DefinedFailureCode::Count:
    return false;
  }
  return false;
}

[[nodiscard]] constexpr std::string_view
definedFailureCodeName(DefinedFailureCode code) {
  switch (code) {
  case DefinedFailureCode::IntegerOverflow:
    return "integer_overflow";
  case DefinedFailureCode::DivisionByZero:
    return "division_by_zero";
  case DefinedFailureCode::ModuloByZero:
    return "modulo_by_zero";
  case DefinedFailureCode::NegativeShiftCount:
    return "negative_shift_count";
  case DefinedFailureCode::ShiftCountOutOfRange:
    return "shift_count_out_of_range";
  case DefinedFailureCode::NumericConversionOutOfRange:
    return "numeric_conversion_out_of_range";
  case DefinedFailureCode::IndexOutOfBounds:
    return "index_out_of_bounds";
  case DefinedFailureCode::EmptyOwnerAccess:
    return "empty_owner_access";
  case DefinedFailureCode::InvalidExpectedAccess:
    return "invalid_expected_access";
  case DefinedFailureCode::InvalidStorageState:
    return "invalid_storage_state";
  case DefinedFailureCode::AllocationFailure:
    return "allocation_failure";
  case DefinedFailureCode::InfallibleHostOperationFailed:
    return "infallible_host_operation_failed";
  case DefinedFailureCode::HostedRuntimeContractFailure:
    return "hosted_runtime_contract_failure";
  case DefinedFailureCode::None:
  case DefinedFailureCode::Count:
    return "none";
  }
  return "none";
}

[[nodiscard]] constexpr std::string_view
definedFailureDetailName(DefinedFailureDetail detail) {
  switch (detail) {
  case DefinedFailureDetail::Addition:
    return "addition";
  case DefinedFailureDetail::Subtraction:
    return "subtraction";
  case DefinedFailureDetail::Multiplication:
    return "multiplication";
  case DefinedFailureDetail::Division:
    return "division";
  case DefinedFailureDetail::Negation:
    return "negation";
  case DefinedFailureDetail::IntegerDivision:
    return "integer_division";
  case DefinedFailureDetail::IntegerModulo:
    return "integer_modulo";
  case DefinedFailureDetail::LeftShift:
    return "left_shift";
  case DefinedFailureDetail::RightShift:
    return "right_shift";
  case DefinedFailureDetail::NumericCast:
    return "numeric_cast";
  case DefinedFailureDetail::HostedArgumentCount:
    return "hosted_argument_count";
  case DefinedFailureDetail::FixedArray:
    return "fixed_array";
  case DefinedFailureDetail::StringView:
    return "string_view";
  case DefinedFailureDetail::Vector:
    return "vector";
  case DefinedFailureDetail::String:
    return "string";
  case DefinedFailureDetail::PrivateStorage:
    return "private_storage";
  case DefinedFailureDetail::Dereference:
    return "dereference";
  case DefinedFailureDetail::MemberAccess:
    return "member_access";
  case DefinedFailureDetail::ValueOnError:
    return "value_on_error";
  case DefinedFailureDetail::ErrorOnValue:
    return "error_on_value";
  case DefinedFailureDetail::DuplicateConstruction:
    return "duplicate_construction";
  case DefinedFailureDetail::UninitializedAccess:
    return "uninitialized_access";
  case DefinedFailureDetail::RelocationCapacity:
    return "relocation_capacity";
  case DefinedFailureDetail::InvalidRelocationSource:
    return "invalid_relocation_source";
  case DefinedFailureDetail::OccupiedRelocationDestination:
    return "occupied_relocation_destination";
  case DefinedFailureDetail::UniqueOwner:
    return "unique_owner";
  case DefinedFailureDetail::ElementConstruction:
    return "element_construction";
  case DefinedFailureDetail::HostedArguments:
    return "hosted_arguments";
  case DefinedFailureDetail::StdoutWrite:
    return "stdout_write";
  case DefinedFailureDetail::AutomaticJoin:
    return "automatic_join";
  case DefinedFailureDetail::NegativeArgumentCount:
    return "negative_argument_count";
  case DefinedFailureDetail::RecursiveThreadLocalInitialization:
    return "recursive_thread_local_initialization";
  case DefinedFailureDetail::None:
  case DefinedFailureDetail::Count:
    return "none";
  }
  return "none";
}

} // namespace lang
