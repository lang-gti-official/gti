#include "cpp_failure_support_writer.h"

#include "gti/failure_metadata.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string_view>

namespace lang {
namespace {

class FailureArtifactDescriptorWriter {
public:
  explicit FailureArtifactDescriptorWriter(std::ostream &output)
      : output(output) {}

  [[nodiscard]] static std::string_view
  failureCodeAbiConstant(DefinedFailureCode code) {
    switch (code) {
    case DefinedFailureCode::IntegerOverflow:
      return "GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1";
    case DefinedFailureCode::DivisionByZero:
      return "GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1";
    case DefinedFailureCode::ModuloByZero:
      return "GTI_FAILURE_CODE_MODULO_BY_ZERO_V1";
    case DefinedFailureCode::NegativeShiftCount:
      return "GTI_FAILURE_CODE_NEGATIVE_SHIFT_COUNT_V1";
    case DefinedFailureCode::ShiftCountOutOfRange:
      return "GTI_FAILURE_CODE_SHIFT_COUNT_OUT_OF_RANGE_V1";
    case DefinedFailureCode::NumericConversionOutOfRange:
      return "GTI_FAILURE_CODE_NUMERIC_CONVERSION_OUT_OF_RANGE_V1";
    case DefinedFailureCode::IndexOutOfBounds:
      return "GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1";
    case DefinedFailureCode::EmptyOwnerAccess:
      return "GTI_FAILURE_CODE_EMPTY_OWNER_ACCESS_V1";
    case DefinedFailureCode::InvalidExpectedAccess:
      return "GTI_FAILURE_CODE_INVALID_EXPECTED_ACCESS_V1";
    case DefinedFailureCode::InvalidStorageState:
      return "GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1";
    case DefinedFailureCode::AllocationFailure:
      return "GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1";
    case DefinedFailureCode::InfallibleHostOperationFailed:
      return "GTI_FAILURE_CODE_INFALLIBLE_HOST_OPERATION_FAILED_V1";
    case DefinedFailureCode::HostedRuntimeContractFailure:
      return "GTI_FAILURE_CODE_HOSTED_RUNTIME_CONTRACT_FAILURE_V1";
    case DefinedFailureCode::KeyNotFound:
      return "GTI_FAILURE_CODE_KEY_NOT_FOUND_V1";
    case DefinedFailureCode::InvalidContainerArgument:
      return "GTI_FAILURE_CODE_INVALID_CONTAINER_ARGUMENT_V1";
    case DefinedFailureCode::Panic:
      return "GTI_FAILURE_CODE_PANIC_V2";
    case DefinedFailureCode::None:
    case DefinedFailureCode::Count:
      break;
    }
    throw std::logic_error(
        "verified failure metadata contains an unmapped ABI code");
  }

  [[nodiscard]] static std::string_view
  failureDetailAbiConstant(DefinedFailureDetail detail) {
    switch (detail) {
    case DefinedFailureDetail::Addition:
      return "GTI_FAILURE_DETAIL_ADDITION_V1";
    case DefinedFailureDetail::Subtraction:
      return "GTI_FAILURE_DETAIL_SUBTRACTION_V1";
    case DefinedFailureDetail::Multiplication:
      return "GTI_FAILURE_DETAIL_MULTIPLICATION_V1";
    case DefinedFailureDetail::Division:
      return "GTI_FAILURE_DETAIL_DIVISION_V1";
    case DefinedFailureDetail::Negation:
      return "GTI_FAILURE_DETAIL_NEGATION_V1";
    case DefinedFailureDetail::IntegerDivision:
      return "GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1";
    case DefinedFailureDetail::IntegerModulo:
      return "GTI_FAILURE_DETAIL_INTEGER_MODULO_V1";
    case DefinedFailureDetail::LeftShift:
      return "GTI_FAILURE_DETAIL_LEFT_SHIFT_V1";
    case DefinedFailureDetail::RightShift:
      return "GTI_FAILURE_DETAIL_RIGHT_SHIFT_V1";
    case DefinedFailureDetail::NumericCast:
      return "GTI_FAILURE_DETAIL_NUMERIC_CAST_V1";
    case DefinedFailureDetail::HostedArgumentCount:
      return "GTI_FAILURE_DETAIL_HOSTED_ARGUMENT_COUNT_V1";
    case DefinedFailureDetail::FixedArray:
      return "GTI_FAILURE_DETAIL_FIXED_ARRAY_V1";
    case DefinedFailureDetail::StringView:
      return "GTI_FAILURE_DETAIL_STRING_VIEW_V1";
    case DefinedFailureDetail::Vector:
      return "GTI_FAILURE_DETAIL_VECTOR_V1";
    case DefinedFailureDetail::String:
      return "GTI_FAILURE_DETAIL_STRING_V1";
    case DefinedFailureDetail::PrivateStorage:
      return "GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1";
    case DefinedFailureDetail::Dereference:
      return "GTI_FAILURE_DETAIL_DEREFERENCE_V1";
    case DefinedFailureDetail::MemberAccess:
      return "GTI_FAILURE_DETAIL_MEMBER_ACCESS_V1";
    case DefinedFailureDetail::ValueOnError:
      return "GTI_FAILURE_DETAIL_VALUE_ON_ERROR_V1";
    case DefinedFailureDetail::ErrorOnValue:
      return "GTI_FAILURE_DETAIL_ERROR_ON_VALUE_V1";
    case DefinedFailureDetail::DuplicateConstruction:
      return "GTI_FAILURE_DETAIL_DUPLICATE_CONSTRUCTION_V1";
    case DefinedFailureDetail::UninitializedAccess:
      return "GTI_FAILURE_DETAIL_UNINITIALIZED_ACCESS_V1";
    case DefinedFailureDetail::RelocationCapacity:
      return "GTI_FAILURE_DETAIL_RELOCATION_CAPACITY_V1";
    case DefinedFailureDetail::InvalidRelocationSource:
      return "GTI_FAILURE_DETAIL_INVALID_RELOCATION_SOURCE_V1";
    case DefinedFailureDetail::OccupiedRelocationDestination:
      return "GTI_FAILURE_DETAIL_OCCUPIED_RELOCATION_DESTINATION_V1";
    case DefinedFailureDetail::UniqueOwner:
      return "GTI_FAILURE_DETAIL_UNIQUE_OWNER_V1";
    case DefinedFailureDetail::ElementConstruction:
      return "GTI_FAILURE_DETAIL_ELEMENT_CONSTRUCTION_V1";
    case DefinedFailureDetail::HostedArguments:
      return "GTI_FAILURE_DETAIL_HOSTED_ARGUMENTS_V1";
    case DefinedFailureDetail::StdoutWrite:
      return "GTI_FAILURE_DETAIL_STDOUT_WRITE_V1";
    case DefinedFailureDetail::AutomaticJoin:
      return "GTI_FAILURE_DETAIL_AUTOMATIC_JOIN_V1";
    case DefinedFailureDetail::NegativeArgumentCount:
      return "GTI_FAILURE_DETAIL_NEGATIVE_ARGUMENT_COUNT_V1";
    case DefinedFailureDetail::RecursiveThreadLocalInitialization:
      return "GTI_FAILURE_DETAIL_RECURSIVE_THREAD_LOCAL_INITIALIZATION_V1";
    case DefinedFailureDetail::MutexLock:
      return "GTI_FAILURE_DETAIL_MUTEX_LOCK_V1";
    case DefinedFailureDetail::KeyLookup:
      return "GTI_FAILURE_DETAIL_KEY_LOOKUP_V1";
    case DefinedFailureDetail::ContainerArgument:
      return "GTI_FAILURE_DETAIL_CONTAINER_ARGUMENT_V1";
    case DefinedFailureDetail::SharedOwner:
      return "GTI_FAILURE_DETAIL_SHARED_OWNER_V1";
    case DefinedFailureDetail::ExplicitPanic:
      return "GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2";
    case DefinedFailureDetail::None:
    case DefinedFailureDetail::Count:
      break;
    }
    throw std::logic_error(
        "verified failure metadata contains an unmapped ABI detail");
  }

  static void requireFailureUint64(std::size_t value) {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
      if (value > std::numeric_limits<std::uint64_t>::max()) {
        throw std::logic_error(
            "verified failure metadata span exceeds the runtime ABI");
      }
    }
  }

  void emitFailureByteArray(std::string_view name,
                            std::span<const std::uint8_t> bytes) {
    output << "[[maybe_unused]] static constexpr std::uint8_t " << name
           << "[] = {";
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      if (index % 12 == 0) {
        output << "\n  ";
      } else {
        output << ' ';
      }
      output << static_cast<unsigned int>(bytes[index]);
      if (index + 1 != bytes.size()) {
        output << ',';
      }
    }
    output << "\n};\n\n";
  }

  void emitFailureSourceArray(const FailureSiteDescriptor &site) {
    output << "[[maybe_unused]] static constexpr char "
           << "__gti_failure_source_" << site.id << "_v1[] = {";
    for (std::size_t index = 0; index < site.logicalSource.size(); ++index) {
      if (index % 6 == 0) {
        output << "\n  ";
      } else {
        output << ' ';
      }
      output << "static_cast<char>("
             << static_cast<unsigned int>(
                    static_cast<unsigned char>(site.logicalSource[index]))
             << ')';
      if (index + 1 != site.logicalSource.size()) {
        output << ',';
      }
    }
    output << "\n};\n\n";
  }

  void write(const FailureMetadata &metadata) {
    const FailureMetadataVerificationResult verification =
        verifyFailureMetadata(metadata);
    if (!verification.valid()) {
      throw std::logic_error(
          "production C++ emission requires coherent failure metadata");
    }
    if (metadata.sites().size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::logic_error(
          "verified failure site count exceeds the runtime ABI");
    }
    requireFailureUint64(metadata.descriptorBytes().size());

    output << R"(
namespace gti_internal::backend {

)";
    for (const FailureSiteDescriptor &site : metadata.sites()) {
      requireFailureUint64(site.logicalSource.size());
      requireFailureUint64(site.start);
      requireFailureUint64(site.end);
      if (site.outcomes.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::logic_error(
            "verified failure outcome count exceeds the runtime ABI");
      }
      emitFailureSourceArray(site);
      output << "[[maybe_unused]] static constexpr "
                "::gti_failure_outcome_descriptor_v1 "
             << "__gti_failure_outcomes_" << site.id << "_v1[] = {\n";
      for (const DefinedFailureOutcome outcome : site.outcomes) {
        output << "  {" << failureCodeAbiConstant(outcome.code) << ", "
               << failureDetailAbiConstant(outcome.detail)
               << ", UINT32_C(0)},\n";
      }
      output << "};\n\n";
    }

    if (!metadata.sites().empty()) {
      output << "[[maybe_unused]] static constexpr "
                "::gti_failure_site_descriptor_v1 "
                "__gti_failure_sites_v1[] = {\n";
      for (const FailureSiteDescriptor &site : metadata.sites()) {
        output << "  {{__gti_failure_source_" << site.id << "_v1, UINT64_C("
               << site.logicalSource.size() << ")}, UINT64_C(" << site.line
               << "), UINT64_C(" << site.start << "), UINT64_C(" << site.end
               << "), __gti_failure_outcomes_" << site.id << "_v1, UINT32_C("
               << site.outcomes.size() << "), UINT32_C(0)},\n";
      }
      output << "};\n\n";
    }

    emitFailureByteArray("__gti_failure_canonical_descriptor_v1",
                         metadata.descriptorBytes());
    output << "[[maybe_unused]] static constexpr "
              "::gti_failure_artifact_descriptor_v1\n"
              "    __gti_failure_artifact_descriptor_v1 = {\n"
              "        GTI_FAILURE_ABI_VERSION_V1,\n"
              "        UINT32_C(0),\n"
              "        {";
    for (std::size_t index = 0;
         index < metadata.artifactIdentity().bytes.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << static_cast<unsigned int>(
          metadata.artifactIdentity().bytes[index]);
    }
    output << "},\n        "
           << (metadata.sites().empty() ? "nullptr" : "__gti_failure_sites_v1")
           << ",\n        UINT32_C(" << metadata.sites().size()
           << "),\n        UINT32_C(0),\n"
              "        __gti_failure_canonical_descriptor_v1,\n"
              "        UINT64_C("
           << metadata.descriptorBytes().size()
           << ")};\n\n"
              "} // namespace gti_internal::backend\n\n";
  }

private:
  std::ostream &output;
};

} // namespace

void writeCppFailureArtifactDescriptor(std::ostream &output,
                                       const FailureMetadata &metadata) {
  FailureArtifactDescriptorWriter(output).write(metadata);
}

void writeCppMirFailureHelpers(std::ostream &output) {
  output << R"(
namespace gti_internal::backend {

template <std::uint64_t Constructor>
struct mir_failure_constructor_tag_v1 {};

template <std::uint64_t Constructor>
struct mir_contained_constructor_tag_v1 {};

struct mir_contained_constructor_state_v1 {
  bool success = false;
  ::gti_failure_record_v2 record{};
};

struct mir_empty_state_constructor_tag_v1 {};

template <typename T>
struct mir_empty_state_factory_v1 {
  [[nodiscard]] static T make() noexcept {
    if constexpr (
        std::is_constructible_v<T, mir_empty_state_constructor_tag_v1>) {
      static_assert(
          std::is_nothrow_constructible_v<T,
                                          mir_empty_state_constructor_tag_v1>);
      return T(mir_empty_state_constructor_tag_v1{});
    } else {
      static_assert(std::is_nothrow_default_constructible_v<T>);
      return T{};
    }
  }
};

template <typename Element, std::size_t Size>
struct mir_empty_state_factory_v1<std::array<Element, Size>> {
  template <std::size_t... Index>
  [[nodiscard]] static std::array<Element, Size>
  make(std::index_sequence<Index...>) noexcept {
    return {((void)Index, mir_empty_state_factory_v1<Element>::make())...};
  }

  [[nodiscard]] static std::array<Element, Size> make() noexcept {
    return make(std::make_index_sequence<Size>{});
  }
};

template <typename T>
[[nodiscard]] inline T mir_empty_state_v1() noexcept {
  return mir_empty_state_factory_v1<T>::make();
}

// The MIR Drop ends a completed field's GTI lifetime now. Recreate only its
// proved observation-free representation so the enclosing unpublished C++
// object's later destruction sees an empty field. Ordinary fields are
// transparently replaceable; this helper must not be used for base subobjects.
template <typename T>
inline void mir_rollback_field_v1(T &field) noexcept {
  static_assert(!std::is_const_v<T>);
  T *address = std::addressof(field);
  std::destroy_at(address);
  ::new (static_cast<void *>(address)) T(mir_empty_state_v1<T>());
}

struct mir_failure_status_v1 {
  ::gti_failure_code_v1 code;
  ::gti_failure_detail_v1 detail;
  bool propagated = false;
};

inline constexpr mir_failure_status_v1 mir_failure_success_v1{
    GTI_FAILURE_CODE_NONE_V1, GTI_FAILURE_DETAIL_NONE_V1};

inline mir_failure_status_v1
mir_checked_contract_v1(bool condition, ::gti_failure_code_v1 code,
                        ::gti_failure_detail_v1 detail) noexcept {
  return condition ? mir_failure_success_v1 : mir_failure_status_v1{code, detail};
}

template <typename Unexpected>
inline decltype(auto) mir_unexpected_error_ref_v1(Unexpected &value) noexcept {
  if constexpr (requires { value.error(); })
    return value.error();
  else
    return value.value();
}

inline mir_failure_status_v1
mir_checked_index_bounds_v1(std::uint64_t index, std::uint64_t size,
                            ::gti_failure_detail_v1 detail) noexcept {
  if (index >= size) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1, detail};
  }
  return mir_failure_success_v1;
}

template <typename Index>
inline mir_failure_status_v1
mir_checked_string_view_at_v1(std::string_view value, Index index,
                              char8_t *result) noexcept {
  static_assert(std::is_integral_v<std::remove_cvref_t<Index>>);
  const auto numeric_index = static_cast<integer_comparison_type<Index>>(index);
  if (std::cmp_less(numeric_index, 0) || std::cmp_greater_equal(numeric_index, value.size())) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_STRING_VIEW_V1};
  }
  *result = static_cast<char8_t>(static_cast<unsigned char>(
      value[static_cast<std::size_t>(index)]));
  return mir_failure_success_v1;
}

template <typename Expected, typename Result>
inline mir_failure_status_v1 mir_expected_value_v1(Expected &expected,
                                                   Result *result) {
  if (!expected.has_value()) {
    return {GTI_FAILURE_CODE_INVALID_EXPECTED_ACCESS_V1,
            GTI_FAILURE_DETAIL_VALUE_ON_ERROR_V1};
  }
  // GTI's state check owns the failure. The host value() accessor additionally
  // requires a copyable error for bad_expected_access, even on the success path.
  *result = *expected;
  return mir_failure_success_v1;
}

template <typename Expected>
inline mir_failure_status_v1
mir_expected_void_value_v1(Expected &expected) noexcept {
  if (!expected.has_value()) {
    return {GTI_FAILURE_CODE_INVALID_EXPECTED_ACCESS_V1,
            GTI_FAILURE_DETAIL_VALUE_ON_ERROR_V1};
  }
  return mir_failure_success_v1;
}

template <typename Expected, typename ResultPointer>
inline mir_failure_status_v1
mir_expected_value_ref_v1(Expected &expected, ResultPointer *result) noexcept {
  if (!expected.has_value()) {
    return {GTI_FAILURE_CODE_INVALID_EXPECTED_ACCESS_V1,
            GTI_FAILURE_DETAIL_VALUE_ON_ERROR_V1};
  }
  *result = std::addressof(*expected);
  return mir_failure_success_v1;
}

template <typename Expected, typename ResultSlot>
inline mir_failure_status_v1
mir_expected_value_copy_v1(const Expected &expected, ResultSlot *result) {
  if (!expected.has_value()) {
    return {GTI_FAILURE_CODE_INVALID_EXPECTED_ACCESS_V1,
            GTI_FAILURE_DETAIL_VALUE_ON_ERROR_V1};
  }
  result->construct(*expected);
  return mir_failure_success_v1;
}

template <typename Expected, typename ResultSlot>
inline mir_failure_status_v1
mir_expected_value_move_v1(Expected &expected, ResultSlot *result) {
  if (!expected.has_value()) {
    return {GTI_FAILURE_CODE_INVALID_EXPECTED_ACCESS_V1,
            GTI_FAILURE_DETAIL_VALUE_ON_ERROR_V1};
  }
  result->construct(*std::move(expected));
  return mir_failure_success_v1;
}

template <typename Expected, typename Result>
inline mir_failure_status_v1 mir_expected_error_v1(Expected &expected,
                                                   Result *result) {
  if (expected.has_value()) {
    return {GTI_FAILURE_CODE_INVALID_EXPECTED_ACCESS_V1,
            GTI_FAILURE_DETAIL_ERROR_ON_VALUE_V1};
  }
  *result = expected.error();
  return mir_failure_success_v1;
}

template <typename Expected, typename ResultPointer>
inline mir_failure_status_v1
mir_expected_error_ref_v1(Expected &expected, ResultPointer *result) noexcept {
  if (expected.has_value()) {
    return {GTI_FAILURE_CODE_INVALID_EXPECTED_ACCESS_V1,
            GTI_FAILURE_DETAIL_ERROR_ON_VALUE_V1};
  }
  *result = std::addressof(expected.error());
  return mir_failure_success_v1;
}

template <typename Expected, typename ResultSlot>
inline mir_failure_status_v1
mir_expected_error_copy_v1(const Expected &expected, ResultSlot *result) {
  if (expected.has_value()) {
    return {GTI_FAILURE_CODE_INVALID_EXPECTED_ACCESS_V1,
            GTI_FAILURE_DETAIL_ERROR_ON_VALUE_V1};
  }
  result->construct(expected.error());
  return mir_failure_success_v1;
}

template <typename Expected, typename ResultSlot>
inline mir_failure_status_v1
mir_expected_error_move_v1(Expected &expected, ResultSlot *result) {
  if (expected.has_value()) {
    return {GTI_FAILURE_CODE_INVALID_EXPECTED_ACCESS_V1,
            GTI_FAILURE_DETAIL_ERROR_ON_VALUE_V1};
  }
  result->construct(std::move(expected).error());
  return mir_failure_success_v1;
}

// One non-terminal dimension of a chained fixed-array element access. The
// terminal read and write helpers validate only the last index, so a
// multidimensional dynamic access checks every earlier dimension through this
// helper before subscripting it. The status carries the same defined
// index_out_of_bounds outcome the terminal helpers report.
template <typename Array>
inline mir_failure_status_v1
mir_checked_array_bound_v1(const Array &array, std::uint64_t index) noexcept {
  if (index >= std::size(array)) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_FIXED_ARRAY_V1};
  }
  return mir_failure_success_v1;
}

template <typename Array, typename Value>
inline mir_failure_status_v1
mir_checked_array_read_v1(const Array &array, std::uint64_t index,
                          Value *result) noexcept {
  if (index >= std::size(array)) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_FIXED_ARRAY_V1};
  }
  assign_representation(*result, array[static_cast<std::size_t>(index)]);
  return mir_failure_success_v1;
}

template <typename Array, typename Value>
  requires (!std::is_trivially_copyable_v<Value>)
inline mir_failure_status_v1
mir_checked_array_write_v1(Array &array, std::uint64_t index,
                           Value value) noexcept {
  if (index >= std::size(array)) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_FIXED_ARRAY_V1};
  }
  assign_representation(array[static_cast<std::size_t>(index)], std::move(value));
  return mir_failure_success_v1;
}

// Avoid copying native pointer subobjects through a C++ by-value parameter.
// For trivial values this adds no observable construction or cleanup event.
template <typename Array, typename Value>
  requires std::is_trivially_copyable_v<Value>
inline mir_failure_status_v1
mir_checked_array_write_v1(Array &array, std::uint64_t index,
                           const Value &value) noexcept {
  if (index >= std::size(array)) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_FIXED_ARRAY_V1};
  }
  assign_representation(array[static_cast<std::size_t>(index)], value);
  return mir_failure_success_v1;
}

struct mir_array_copy_assignment_v1_t {};
inline constexpr mir_array_copy_assignment_v1_t
    mir_array_copy_assignment_v1{};

template <typename Array, typename Value>
inline mir_failure_status_v1 mir_checked_array_write_v1(
    Array &array, std::uint64_t index, Value &value,
    mir_array_copy_assignment_v1_t) noexcept {
  if (index >= std::size(array)) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_FIXED_ARRAY_V1};
  }
  assign_representation(array[static_cast<std::size_t>(index)], value);
  return mir_failure_success_v1;
}

// A MIR allocation resource owns bytes, never a live T. Its destruction is an
// integrity check; only a verified Drop or ownership publication releases it.
template <typename T> class mir_object_allocation_v1 final {
  void *storage_ = nullptr;
public:
  mir_object_allocation_v1() = default;
  mir_object_allocation_v1(const mir_object_allocation_v1 &) = delete;
  mir_object_allocation_v1 &operator=(const mir_object_allocation_v1 &) = delete;
  ~mir_object_allocation_v1() {
    if (storage_ != nullptr && !mir_cleanup_emergency_abandonment()) std::abort();
  }
  mir_failure_status_v1 allocate() noexcept {
    if (storage_ != nullptr) std::abort();
    if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
      storage_ = ::operator new(sizeof(T), std::align_val_t(alignof(T)), std::nothrow);
    } else {
      storage_ = ::operator new(sizeof(T), std::nothrow);
    }
    return storage_ != nullptr ? mir_failure_success_v1
        : mir_failure_status_v1{GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1,
                               GTI_FAILURE_DETAIL_UNIQUE_OWNER_V1};
  }
  T *address() noexcept { return static_cast<T *>(storage_); }
  T *release() noexcept {
    T *result = address();
    storage_ = nullptr;
    return result;
  }
  void reset() noexcept {
    if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
      ::operator delete(storage_, std::align_val_t(alignof(T)));
    } else {
      ::operator delete(storage_);
    }
    storage_ = nullptr;
  }
};

// Scalar initialization cannot execute user code. Allocation is the sole
// detector here; the caller's MIR owns its record, cleanup, and publication.
template <typename T, typename... Args>
inline mir_failure_status_v1
mir_allocate_unique_scalar_v1(std::unique_ptr<T> *result, Args... args) noexcept {
  static_assert((std::is_scalar_v<T> || is_raw_address_v<T>) && sizeof...(Args) <= 1);
  // A direct allocation-function call cannot be elided as a C++ new-expression;
  // GTI's allocation outcome must remain observable at this MIR instruction.
  void *storage = ::operator new(sizeof(T), std::nothrow);
  if (storage == nullptr) {
    return {GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1,
            GTI_FAILURE_DETAIL_UNIQUE_OWNER_V1};
  }
  result->reset(std::construct_at(static_cast<T *>(storage), args...));
  return mir_failure_success_v1;
}

// The defined-failure storage families: allocation uses the capability's
// no-throw primitive so the status exactly reflects exhaustion instead of
// escaping through the compatibility abort path.
template <typename T>
inline mir_failure_status_v1
mir_allocate_storage_v1(std::uint64_t count,
                        ::gti_internal::backend::storage<T> *result) noexcept {
  if (::gti_internal::backend::storage<T>::try_allocate(count, 0, result) != 0) {
    return {GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  }
  return mir_failure_success_v1;
}

template <typename T, typename... Args>
inline mir_failure_status_v1
mir_storage_construct_v1(::gti_internal::backend::storage<T> &storage,
                         std::uint64_t index, Args &&...args) {
  if (index >= storage.capacity()) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  }
  if (storage.initialized(index)) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_DUPLICATE_CONSTRUCTION_V1};
  }
  if (!storage.try_construct(index, std::forward<Args>(args)...)) {
    return {GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1,
            GTI_FAILURE_DETAIL_ELEMENT_CONSTRUCTION_V1};
  }
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1 mir_storage_preflight_v1(
    const ::gti_internal::backend::storage<T> &storage, std::uint64_t index) {
  if (index >= storage.capacity()) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  }
  if (storage.initialized(index)) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_DUPLICATE_CONSTRUCTION_V1};
  }
  return mir_failure_success_v1;
}

template <typename T, typename Constructor>
inline mir_failure_status_v1 mir_storage_construct_with_v1(
    ::gti_internal::backend::storage<T> &storage, std::uint64_t index,
    ::gti_failure_record_v2 *record, Constructor construct) {
  if (!storage.construct_with_constructor(index, construct)) {
    return {record->code, record->detail, true};
  }
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1
mir_storage_read_v1(const ::gti_internal::backend::storage<T> &storage,
                    std::uint64_t index, const T **result) noexcept {
  if (index >= storage.capacity()) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  }
  if (!storage.initialized(index)) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_UNINITIALIZED_ACCESS_V1};
  }
  *result = &storage.read(index);
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1
mir_storage_read_mut_v1(::gti_internal::backend::storage<T> &storage,
                        std::uint64_t index, T **result) noexcept {
  if (index >= storage.capacity()) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  }
  if (!storage.initialized(index)) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_UNINITIALIZED_ACCESS_V1};
  }
  *result = &storage.read_mut(index);
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1
mir_storage_destroy_v1(::gti_internal::backend::storage<T> &storage,
                       std::uint64_t index) {
  if (index >= storage.capacity()) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  }
  if (!storage.initialized(index)) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_UNINITIALIZED_ACCESS_V1};
  }
  storage.destroy(index);
  return mir_failure_success_v1;
}

template <typename T, typename Cleanup>
inline mir_failure_status_v1
mir_storage_destroy_v1(::gti_internal::backend::storage<T> &storage,
                       std::uint64_t index, ::gti_failure_record_v2 *record,
                       Cleanup cleanup) {
  if (index >= storage.capacity()) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  }
  if (!storage.initialized(index)) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_UNINITIALIZED_ACCESS_V1};
  }
  if (!storage.destroy_with_failure(index, record, cleanup)) {
    return {record->code, record->detail, true};
  }
  return mir_failure_success_v1;
}

template <typename T, std::size_t N>
inline mir_failure_status_v1
mir_restoring_indexed_swap_v1(std::array<T, N> &storage,
                              std::uint64_t left, std::uint64_t right) {
  if (left >= N || right >= N)
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  if (left != right) mir_restoring_swap_v1(storage[left], storage[right]);
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1
mir_restoring_indexed_swap_v1(prefix_storage<T> &storage,
                              std::uint64_t left, std::uint64_t right) {
  if (left >= storage.length() || right >= storage.length())
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  if (left != right) mir_restoring_swap_v1(storage.read_mut(left), storage.read_mut(right));
  return mir_failure_success_v1;
}

template <bool Bitwise = false, typename T>
inline mir_failure_status_v1
mir_storage_relocate_v1(::gti_internal::backend::storage<T> &source,
                        ::gti_internal::backend::storage<T> &destination,
                        std::uint64_t count) {
  if (count > source.capacity() || count > destination.capacity()) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_RELOCATION_CAPACITY_V1};
  }
  for (std::uint64_t index = 0; index < count; ++index) {
    if (!source.initialized(index)) {
      return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
              GTI_FAILURE_DETAIL_INVALID_RELOCATION_SOURCE_V1};
    }
    if (destination.initialized(index)) {
      return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
              GTI_FAILURE_DETAIL_OCCUPIED_RELOCATION_DESTINATION_V1};
    }
  }
  source.template relocate_to<Bitwise>(destination, count);
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1
mir_storage_shift_right_v1(::gti_internal::backend::storage<T> &storage,
                           std::uint64_t first, std::uint64_t last) {
  if (first > last || last >= storage.capacity()) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_RELOCATION_CAPACITY_V1};
  }
  if (storage.initialized(last)) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_OCCUPIED_RELOCATION_DESTINATION_V1};
  }
  for (std::uint64_t index = first; index < last; ++index) {
    if (!storage.initialized(index)) {
      return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
              GTI_FAILURE_DETAIL_INVALID_RELOCATION_SOURCE_V1};
    }
  }
  storage.shift_right(first, last);
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1
mir_storage_shift_left_v1(::gti_internal::backend::storage<T> &storage,
                          std::uint64_t first, std::uint64_t last) {
  if (first >= last || last > storage.capacity()) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_RELOCATION_CAPACITY_V1};
  }
  if (storage.initialized(first)) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_OCCUPIED_RELOCATION_DESTINATION_V1};
  }
  for (std::uint64_t index = first + 1; index < last; ++index) {
    if (!storage.initialized(index)) {
      return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
              GTI_FAILURE_DETAIL_INVALID_RELOCATION_SOURCE_V1};
    }
  }
  storage.shift_left(first, last);
  return mir_failure_success_v1;
}

template <typename T, typename... Args>
inline mir_failure_status_v1 mir_optional_construct_v1(
    ::gti_internal::backend::optional_storage<T> &storage, Args &&...args) {
  if (storage.has_value()) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_DUPLICATE_CONSTRUCTION_V1};
  }
  if (!storage.try_construct(std::forward<Args>(args)...)) {
    return {GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1,
            GTI_FAILURE_DETAIL_ELEMENT_CONSTRUCTION_V1};
  }
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1 mir_optional_read_v1(
    const ::gti_internal::backend::optional_storage<T> &storage,
    const T **result) noexcept {
  if (!storage.has_value()) {
    return {GTI_FAILURE_CODE_EMPTY_OWNER_ACCESS_V1,
            GTI_FAILURE_DETAIL_MEMBER_ACCESS_V1};
  }
  *result = &storage.unchecked_read();
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1 mir_optional_read_mut_v1(
    ::gti_internal::backend::optional_storage<T> &storage,
    T **result) noexcept {
  if (!storage.has_value()) {
    return {GTI_FAILURE_CODE_EMPTY_OWNER_ACCESS_V1,
            GTI_FAILURE_DETAIL_MEMBER_ACCESS_V1};
  }
  *result = &storage.unchecked_read_mut();
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1
mir_prefix_allocate_v1(std::uint64_t count,
                       ::gti_internal::backend::prefix_storage<T> *result)
    noexcept {
  if (!::gti_internal::backend::prefix_storage<T>::try_allocate(count,
                                                                result)) {
    return {GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  }
  return mir_failure_success_v1;
}

template <typename T, typename... Args>
inline mir_failure_status_v1
mir_prefix_append_v1(::gti_internal::backend::prefix_storage<T> &storage,
                     Args &&...args) {
  if (storage.length() >= storage.capacity()) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_RELOCATION_CAPACITY_V1};
  }
  storage.append(std::forward<Args>(args)...);
  return mir_failure_success_v1;
}

template <typename T, typename Constructor>
inline mir_failure_status_v1 mir_prefix_construct_v1(
    ::gti_internal::backend::prefix_storage<T> &storage,
    ::gti_failure_record_v2 *record, Constructor construct) {
  if (!storage.append_with_constructor(construct)) {
    return {record->code, record->detail, true};
  }
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1 mir_prefix_preflight_v1(
    const ::gti_internal::backend::prefix_storage<T> &storage) {
  if (storage.length() >= storage.capacity()) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_RELOCATION_CAPACITY_V1};
  }
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1
mir_prefix_pop_v1(::gti_internal::backend::prefix_storage<T> &storage) {
  if (storage.length() == 0) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_UNINITIALIZED_ACCESS_V1};
  }
  storage.pop();
  return mir_failure_success_v1;
}

template <typename T, typename Cleanup>
inline mir_failure_status_v1
mir_prefix_pop_v1(::gti_internal::backend::prefix_storage<T> &storage,
                  ::gti_failure_record_v2 *record, Cleanup cleanup) {
  if (storage.length() == 0) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_UNINITIALIZED_ACCESS_V1};
  }
  if (!storage.pop_with_failure(record, cleanup)) {
    return {record->code, record->detail, true};
  }
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1
mir_prefix_read_v1(const ::gti_internal::backend::prefix_storage<T> &storage,
                   std::uint64_t index, const T **result) {
  if (index >= storage.length()) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  }
  *result = &storage.read(index);
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1
mir_prefix_read_mut_v1(::gti_internal::backend::prefix_storage<T> &storage,
                       std::uint64_t index, T **result) {
  if (index >= storage.length()) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  }
  *result = &storage.read_mut(index);
  return mir_failure_success_v1;
}

template <typename T, typename... Args>
inline mir_failure_status_v1
mir_prefix_insert_v1(::gti_internal::backend::prefix_storage<T> &storage,
                     std::uint64_t index, Args &&...args) {
  if (index > storage.length()) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  }
  if (storage.length() >= storage.capacity()) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_RELOCATION_CAPACITY_V1};
  }
  storage.insert_at(index, std::forward<Args>(args)...);
  return mir_failure_success_v1;
}

template <typename T>
inline mir_failure_status_v1
mir_prefix_erase_v1(::gti_internal::backend::prefix_storage<T> &storage,
                    std::uint64_t index) {
  if (index >= storage.length()) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  }
  storage.erase_at(index);
  return mir_failure_success_v1;
}

template <typename T, typename Cleanup>
inline mir_failure_status_v1
mir_prefix_erase_v1(::gti_internal::backend::prefix_storage<T> &storage,
                    std::uint64_t index, ::gti_failure_record_v2 *record,
                    const ::gti_failure_artifact_descriptor_v1 *artifact,
                    Cleanup cleanup) {
  if (index >= storage.length()) {
    return {GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
            GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1};
  }
  if (!storage.erase_with_failure(index, record, artifact, cleanup)) {
    return {record->code, record->detail, true};
  }
  return mir_failure_success_v1;
}

template <bool Bitwise = false, typename T>
inline mir_failure_status_v1
mir_prefix_relocate_v1(::gti_internal::backend::prefix_storage<T> &source,
                       ::gti_internal::backend::prefix_storage<T>
                           &destination) {
  if (destination.length() != 0) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_OCCUPIED_RELOCATION_DESTINATION_V1};
  }
  if (destination.capacity() < source.length()) {
    return {GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1,
            GTI_FAILURE_DETAIL_RELOCATION_CAPACITY_V1};
  }
  source.template relocate_to<Bitwise>(destination);
  return mir_failure_success_v1;
}

template <typename Target, typename Left, typename Right>
inline mir_failure_status_v1 mir_checked_add_v1(Left left, Right right,
                                                Target *result) noexcept {
  static_assert(std::is_integral_v<Target> && std::is_integral_v<Left> &&
                std::is_integral_v<Right>);
  const Target promoted_left = static_cast<Target>(left);
  const Target promoted_right = static_cast<Target>(right);
  bool overflow = false;
  if constexpr (std::is_unsigned_v<Target>) {
    overflow = promoted_left >
               (std::numeric_limits<Target>::max)() - promoted_right;
  } else {
    overflow =
        (promoted_right > 0 &&
         promoted_left >
             (std::numeric_limits<Target>::max)() - promoted_right) ||
        (promoted_right < 0 &&
         promoted_left <
             (std::numeric_limits<Target>::min)() - promoted_right);
  }
  if (overflow) {
    return {GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
            GTI_FAILURE_DETAIL_ADDITION_V1};
  }
  *result = static_cast<Target>(promoted_left + promoted_right);
  return mir_failure_success_v1;
}

template <typename Target, typename Left, typename Right>
inline mir_failure_status_v1 mir_checked_subtract_v1(Left left, Right right,
                                                     Target *result) noexcept {
  static_assert(std::is_integral_v<Target> && std::is_integral_v<Left> &&
                std::is_integral_v<Right>);
  const Target promoted_left = static_cast<Target>(left);
  const Target promoted_right = static_cast<Target>(right);
  bool overflow = false;
  if constexpr (std::is_unsigned_v<Target>) {
    overflow = promoted_left < promoted_right;
  } else {
    overflow =
        (promoted_right > 0 &&
         promoted_left <
             (std::numeric_limits<Target>::min)() + promoted_right) ||
        (promoted_right < 0 &&
         promoted_left >
             (std::numeric_limits<Target>::max)() + promoted_right);
  }
  if (overflow) {
    return {GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
            GTI_FAILURE_DETAIL_SUBTRACTION_V1};
  }
  *result = static_cast<Target>(promoted_left - promoted_right);
  return mir_failure_success_v1;
}

template <typename Target, typename Left, typename Right>
inline mir_failure_status_v1 mir_checked_multiply_v1(Left left, Right right,
                                                     Target *result) noexcept {
  static_assert(std::is_integral_v<Target> && std::is_integral_v<Left> &&
                std::is_integral_v<Right>);
  const Target promoted_left = static_cast<Target>(left);
  const Target promoted_right = static_cast<Target>(right);
  bool overflow = false;
  if constexpr (std::is_unsigned_v<Target>) {
    overflow = promoted_right != 0 &&
               promoted_left >
                   (std::numeric_limits<Target>::max)() / promoted_right;
  } else if (promoted_left != 0 && promoted_right != 0) {
    if ((promoted_left == -1 &&
         promoted_right == (std::numeric_limits<Target>::min)()) ||
        (promoted_right == -1 &&
         promoted_left == (std::numeric_limits<Target>::min)())) {
      overflow = true;
    } else if (promoted_left > 0) {
      overflow =
          (promoted_right > 0 &&
           promoted_left >
               (std::numeric_limits<Target>::max)() / promoted_right) ||
          (promoted_right < 0 &&
           promoted_right <
               (std::numeric_limits<Target>::min)() / promoted_left);
    } else {
      overflow =
          (promoted_right > 0 &&
           promoted_left <
               (std::numeric_limits<Target>::min)() / promoted_right) ||
          (promoted_right < 0 &&
           promoted_left <
               (std::numeric_limits<Target>::max)() / promoted_right);
    }
  }
  if (overflow) {
    return {GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
            GTI_FAILURE_DETAIL_MULTIPLICATION_V1};
  }
  *result = static_cast<Target>(promoted_left * promoted_right);
  return mir_failure_success_v1;
}

template <typename Target, typename Left, typename Right>
inline mir_failure_status_v1 mir_checked_divide_v1(Left left, Right right,
                                                   Target *result) noexcept {
  static_assert(std::is_integral_v<Target> && std::is_integral_v<Left> &&
                std::is_integral_v<Right>);
  const Target promoted_left = static_cast<Target>(left);
  const Target promoted_right = static_cast<Target>(right);
  if (promoted_right == 0) {
    return {GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1,
            GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1};
  }
  if constexpr (std::is_signed_v<Target>) {
    if (promoted_left == (std::numeric_limits<Target>::min)() &&
        promoted_right == -1) {
      return {GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
              GTI_FAILURE_DETAIL_DIVISION_V1};
    }
  }
  *result = static_cast<Target>(promoted_left / promoted_right);
  return mir_failure_success_v1;
}

template <typename Target, typename Left, typename Right>
inline mir_failure_status_v1 mir_checked_remainder_v1(Left left, Right right,
                                                      Target *result) noexcept {
  static_assert(std::is_integral_v<Target> && std::is_integral_v<Left> &&
                std::is_integral_v<Right>);
  const Target promoted_left = static_cast<Target>(left);
  const Target promoted_right = static_cast<Target>(right);
  if (promoted_right == 0) {
    return {GTI_FAILURE_CODE_MODULO_BY_ZERO_V1,
            GTI_FAILURE_DETAIL_INTEGER_MODULO_V1};
  }
  if constexpr (std::is_signed_v<Target>) {
    if (promoted_left == (std::numeric_limits<Target>::min)() &&
        promoted_right == -1) {
      *result = Target{0};
      return mir_failure_success_v1;
    }
  }
  *result = static_cast<Target>(promoted_left % promoted_right);
  return mir_failure_success_v1;
}

template <typename Target, typename Left, typename Right>
inline Target mir_nonzero_remainder_v1(Left left, Right right) noexcept {
  static_assert(std::is_integral_v<Target> && std::is_integral_v<Left> &&
                std::is_integral_v<Right>);
  const Target promoted_left = static_cast<Target>(left);
  const Target promoted_right = static_cast<Target>(right);
  if constexpr (std::is_signed_v<Target>) {
    if (promoted_left == (std::numeric_limits<Target>::min)() &&
        promoted_right == -1) {
      return Target{0};
    }
  }
  return static_cast<Target>(promoted_left % promoted_right);
}

template <typename Target, typename Value>
inline mir_failure_status_v1 mir_checked_negate_v1(Value value,
                                                   Target *result) noexcept {
  static_assert(std::is_integral_v<Target> && std::is_signed_v<Target> &&
                std::is_integral_v<Value>);
  if constexpr (std::is_signed_v<Value> && sizeof(Value) > sizeof(Target)) {
    // A wider signed operand negates in its own domain and then
    // range-checks the conversion: -(int64 2147483648) is int32 min and
    // must succeed, while any value outside the target's negated range
    // reports.
    if (value == (std::numeric_limits<Value>::min)()) {
      return {GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
              GTI_FAILURE_DETAIL_NEGATION_V1};
    }
    const Value negated = static_cast<Value>(-value);
    if (negated < static_cast<Value>((std::numeric_limits<Target>::min)()) ||
        negated > static_cast<Value>((std::numeric_limits<Target>::max)())) {
      return {GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
              GTI_FAILURE_DETAIL_NEGATION_V1};
    }
    *result = static_cast<Target>(negated);
    return mir_failure_success_v1;
  } else {
    const Target promoted = static_cast<Target>(value);
    if (promoted == (std::numeric_limits<Target>::min)()) {
      return {GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
              GTI_FAILURE_DETAIL_NEGATION_V1};
    }
    *result = static_cast<Target>(-promoted);
    return mir_failure_success_v1;
  }
}

template <typename Target, typename Count>
inline mir_failure_status_v1 mir_checked_shift_count_v1(
    Count count, ::gti_failure_detail_v1 detail,
    std::uint32_t *result) noexcept {
  static_assert(std::is_integral_v<Target> && std::is_integral_v<Count>);
  if constexpr (std::is_signed_v<Count>) {
    if (count < 0) {
      return {GTI_FAILURE_CODE_NEGATIVE_SHIFT_COUNT_V1, detail};
    }
  }
  using UnsignedCount = std::make_unsigned_t<Count>;
  const UnsignedCount unsigned_count = static_cast<UnsignedCount>(count);
  if (std::cmp_greater_equal(
          unsigned_count,
          std::numeric_limits<std::make_unsigned_t<Target>>::digits)) {
    return {GTI_FAILURE_CODE_SHIFT_COUNT_OUT_OF_RANGE_V1, detail};
  }
  *result = static_cast<std::uint32_t>(unsigned_count);
  return mir_failure_success_v1;
}

template <typename Target, typename Left, typename Right>
inline mir_failure_status_v1 mir_checked_shift_left_v1(Left left, Right right,
                                                       Target *result) noexcept {
  static_assert(std::is_integral_v<Target> && std::is_integral_v<Left> &&
                std::is_integral_v<Right>);
  std::uint32_t count = 0;
  const mir_failure_status_v1 status = mir_checked_shift_count_v1<Target>(
      right, GTI_FAILURE_DETAIL_LEFT_SHIFT_V1, &count);
  if (status.code != GTI_FAILURE_CODE_NONE_V1) {
    return status;
  }
  using Unsigned = std::make_unsigned_t<Target>;
  const Unsigned bits = static_cast<Unsigned>(static_cast<Target>(left));
  const Unsigned shifted = static_cast<Unsigned>(bits << count);
  if constexpr (std::is_signed_v<Target>) {
    *result = std::bit_cast<Target>(shifted);
  } else {
    *result = shifted;
  }
  return mir_failure_success_v1;
}

template <typename Target, typename Left, typename Right>
inline mir_failure_status_v1 mir_checked_shift_right_v1(
    Left left, Right right, Target *result) noexcept {
  static_assert(std::is_integral_v<Target> && std::is_integral_v<Left> &&
                std::is_integral_v<Right>);
  std::uint32_t count = 0;
  const mir_failure_status_v1 status = mir_checked_shift_count_v1<Target>(
      right, GTI_FAILURE_DETAIL_RIGHT_SHIFT_V1, &count);
  if (status.code != GTI_FAILURE_CODE_NONE_V1) {
    return status;
  }
  const Target promoted = static_cast<Target>(left);
  *result = static_cast<Target>(promoted >> count);
  return mir_failure_success_v1;
}

template <typename Target, typename Source>
inline mir_failure_status_v1 mir_checked_convert_v1(Source value,
                                                    Target *result) noexcept {
  static_assert(std::is_arithmetic_v<Target> &&
                std::is_arithmetic_v<Source>);
  if constexpr (std::is_integral_v<Target> &&
                std::is_integral_v<Source>) {
    if (!std::in_range<integer_comparison_type<Target>>(
            static_cast<integer_comparison_type<Source>>(value))) {
      return {GTI_FAILURE_CODE_NUMERIC_CONVERSION_OUT_OF_RANGE_V1,
              GTI_FAILURE_DETAIL_NUMERIC_CAST_V1};
    }
  } else if constexpr (std::is_integral_v<Target> &&
                       std::is_floating_point_v<Source>) {
    if (!std::isfinite(value)) {
      return {GTI_FAILURE_CODE_NUMERIC_CONVERSION_OUT_OF_RANGE_V1,
              GTI_FAILURE_DETAIL_NUMERIC_CAST_V1};
    }
    const Source truncated = std::trunc(value);
    const Source upper =
        std::ldexp(Source{1}, std::numeric_limits<Target>::digits);
    const Source lower = std::is_unsigned_v<Target> ? Source{0} : -upper;
    if (truncated < lower || truncated >= upper) {
      return {GTI_FAILURE_CODE_NUMERIC_CONVERSION_OUT_OF_RANGE_V1,
              GTI_FAILURE_DETAIL_NUMERIC_CAST_V1};
    }
  }
  *result = static_cast<Target>(value);
  return mir_failure_success_v1;
}

// The status-returning compound assignment family composes exactly the
// terminal add_assign contract: the base operation checks in the C++
// common type and the narrowing conversion checks the commit, so each
// failure mode carries its own code. The target is written only when
// both steps succeed.
#define GTI_MIR_CHECKED_COMPOUND_V1(name, base)                                  template <typename Target, typename Value>                                    inline mir_failure_status_v1 name(Target &target, Value value) noexcept {       using Common = decltype(target + value);                                      Common computed{};                                                            const mir_failure_status_v1 checked =                                             base<Common>(target, value, &computed);                                   if (checked.code != GTI_FAILURE_CODE_NONE_V1) {                                 return checked;                                                             }                                                                             return mir_checked_convert_v1(computed, &target);                           }

GTI_MIR_CHECKED_COMPOUND_V1(mir_checked_compound_add_v1, mir_checked_add_v1)
GTI_MIR_CHECKED_COMPOUND_V1(mir_checked_compound_subtract_v1,
                            mir_checked_subtract_v1)
GTI_MIR_CHECKED_COMPOUND_V1(mir_checked_compound_multiply_v1,
                            mir_checked_multiply_v1)
GTI_MIR_CHECKED_COMPOUND_V1(mir_checked_compound_divide_v1,
                            mir_checked_divide_v1)
GTI_MIR_CHECKED_COMPOUND_V1(mir_checked_compound_remainder_v1,
                            mir_checked_remainder_v1)
#undef GTI_MIR_CHECKED_COMPOUND_V1

#define GTI_MIR_CHECKED_SHIFT_COMPOUND_V1(name, base)                            \
  template <typename Target, typename Value>                                    \
  inline mir_failure_status_v1 name(Target &target, Value value) noexcept {      \
    using Promoted = decltype(+target);                                          \
    Promoted computed{};                                                         \
    const mir_failure_status_v1 checked =                                        \
        base<Promoted>(target, value, &computed);                                \
    if (checked.code != GTI_FAILURE_CODE_NONE_V1) {                              \
      return checked;                                                            \
    }                                                                            \
    return mir_checked_convert_v1(computed, &target);                            \
  }

GTI_MIR_CHECKED_SHIFT_COMPOUND_V1(mir_checked_compound_shift_left_v1,
                                  mir_checked_shift_left_v1)
GTI_MIR_CHECKED_SHIFT_COMPOUND_V1(mir_checked_compound_shift_right_v1,
                                  mir_checked_shift_right_v1)
#undef GTI_MIR_CHECKED_SHIFT_COMPOUND_V1

} // namespace gti_internal::backend

)";
}

} // namespace lang
