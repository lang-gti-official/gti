#include "cpp_support_preamble.h"

#include <ostream>
#include <stdexcept>
#include <string_view>

namespace lang {
namespace {

void writeCppBackendMacroIsolation(std::ostream &output) {
  // Native compiler arguments may force-include a header before this
  // translation unit, and generated includes may themselves provide legacy
  // macros.  This fixed token set is cleared on both sides of the includes.
  // Source symbol spellings are handled separately, immediately before
  // source declarations.
  static constexpr std::string_view helperNames[] = {
      "Args",
      "Interface",
      "Model",
      "Bytes",
      "Align",
      "Payload",
      "Head",
      "Tail",
      "payload_union",
      "payload_variant",
      "payload_get",
      "mir_closed_enum_unreachable_v1",
      "payload_equal_v",
      "storage_",
      "tag_",
      "vacant",
      "tag_type",
      "dispatch",
      "head",
      "tail",
      "tag",
      "equal",
      "operations",
      "payload",
      "object",
      "Allowed",
      "Argument",
      "Arguments",
      "Array",
      "Binding",
      "Bitwise",
      "Base",
      "Capability",
      "Cleanup",
      "Callable",
      "Count",
      "Derived",
      "Error",
      "Extent",
      "Index",
      "Left",
      "Parameters",
      "Operation",
      "Parameter",
      "Result",
      "Right",
      "Source",
      "SourceType",
      "T",
      "U",
      "Values",
      "Target",
      "Unsigned",
      "UnsignedCount",
      "UnsignedResult",
      "Value",
      "__gti_invoke",
      "__gti_small_source_inline",
      "__gti_mir_lambda_identity",
      "__gti_mir_source_result",
      "source_callable_result",
      "__gti_simd_vector",
      "__gti_simd_width",
      "__gti_simd_left",
      "__gti_simd_right",
      "__gti_simd_result",
      "__gti_simd_lane",
      "simd_binary",
      "vector_size",
      "mir_lambda_identity_v",
      "mir_nested_lambda_identity",
      "mir_nested_lambda_identity_v",
      "Template",
      "mir_cleanup_emergency_abandonment",
      "mir_failure_cleanup_context_v1",
      "mir_failure_cleanup_scope_v1",
      "mir_incoming_failure_cleanup_v1",
      "mir_check_incoming_cleanup_failure_v1",
      "mir_missing_drop_v1",
      "mir_terminate_cleanup_failure_v2",
      "primary",
      "secondary",
      "primary_artifact",
      "secondary_artifact",
      "memset",
      "activate",
      "context_",
      "active_",
      "current",
      "add",
      "add_assign",
      "allocate",
      "address",
      "alignment",
      "extent",
      "allocation",
      "allocation_",
      "allocation_bytes",
      "allocation_error",
      "aligned",
      "args",
      "array",
      "array_at",
      "array_bounds_error",
      "available",
      "backend",
      "bitwise_and_assign",
      "bitwise_or_assign",
      "bitwise_xor_assign",
      "block",
      "bits",
      "raw_address",
      "raw_address_traits",
      "is_raw_address_v",
      "address_bits",
      "native_slot_",
      "native_slot_address",
      "raw_pointer_array",
      "fixed_array_extent",
      "native_address",
      "native_pointer_value",
      "raw_pointer_slot_copy",
      "copy_to_native_slot",
      "copy_from_native_slot",
      "native",
      "raw_address_of",
      "construct_representation_at",
      "representation_cell",
      "byte_transport",
      "Candidate",
      "Input",
      "Other",
      "construct_from",
      "destroy_active",
      "emplace",
      "fallback",
      "has_value",
      "value_",
      "value_or",
      "value_type",
      "byte_field",
      "__gti_byte_transport_tag",
      "bytes_",
      "error_",
      "true_type",
      "false_type",
      "is_constructible_v",
      "is_move_constructible_v",
      "is_nothrow_move_constructible_v",
      "is_default_constructible_v",
      "is_copy_assignable_v",
      "is_move_assignable_v",
      "is_nothrow_move_assignable_v",
      "is_trivially_copy_assignable_v",
      "is_trivially_move_assignable_v",
      "assign_representation",
      "mir_restoring_exchange_into_v1",
      "mir_object_relocate_v1",
      "mir_object_relocate_range_v1",
      "from_storage_address",
      "bytes",
      "callable",
      "call_argument",
      "call_parameter",
      "callable_binding",
      "callable_capability",
      "capacity",
      "capacity_",
      "integer_add_overflow_flag",
      "checked_index",
      "checked_initialized_index",
      "integer_multiply_overflow_flag",
      "integer_subtract_overflow_flag",
      "cleanup",
      "cleanup_task",
      "cleanup_result",
      "element",
      "clone_into",
      "construct",
      "count",
      "data_",
      "destination",
      "destroy",
      "divide",
      "divide_assign",
      "empty_owner_error",
      "effective_alignment",
      "elements",
      "error",
      "exact_call",
      "exact_call_type_compatible",
      "exact_type",
      "function_item",
      "function_pointer",
      "function_pointer_parameter",
      "function_pointer_result",
      "forward_pack_argument",
      "fresh",
      "gti_internal",
      "integer_domain_error",
      "index",
      "initialized_",
      "invoke",
      "invoke_mutable",
      "invoke_once",
      "invoke_read",
      "invoke_selected",
      "left",
      "lower",
      "max",
      "maximum",
      "message",
      "min",
      "minimum",
      "minimum_alignment",
      "modulo",
      "multiply",
      "multiply_assign",
      "mutex_guard_active",
      "mutex_guard_read",
      "mutex_lock",
      "mutex_lock_token",
      "mutex_unlock",
      "mutex_value",
      "mutable_call",
      "mutable_reference",
      "mutable_receiver",
      "negate",
      "numeric_cast",
      "integer_comparison_type",
      "numeric_conversion_error",
      "once_call",
      "offset",
      "optional_storage",
      "uninitialized_storage",
      "optional_storage_construct",
      "optional_storage_has_value",
      "optional_storage_read",
      "optional_storage_read_mut",
      "optional_storage_read_mut_unchecked",
      "optional_storage_read_unchecked",
      "optional_storage_reset",
      "polymorphic_storage",
      "polymorphic_storage_operations",
      "polymorphic_storage_operations_for",
      "polymorphic_storage_construct",
      "polymorphic_storage_has_value",
      "polymorphic_storage_read",
      "polymorphic_storage_read_mut",
      "polymorphic_storage_reset",
      "operations_",
      "interface_",
      "relocate",
      "interface",
      "other",
      "outside",
      "overflow",
      "padding",
      "owner_access",
      "owner",
      "post_decrement",
      "post_increment",
      "pre_decrement",
      "pre_increment",
      "previous",
      "promote",
      "promoted",
      "promoted_left",
      "promoted_right",
      "read",
      "read_call",
      "read_reference",
      "read_mut",
      "read_only_receiver",
      "release",
      "release_with_failure",
      "released",
      "relocate_to",
      "remainder_assign",
      "retag_call",
      "requested",
      "requested_alignment",
      "result",
      "right",
      "saturating_add",
      "saturating_mul",
      "saturating_sub",
      "selected",
      "shared_owner",
      "shift_count",
      "shift_left",
      "shift_left_assign",
      "shift_right",
      "shift_right_assign",
      "shifted",
      "slot",
      "slots_",
      "status",
      "state_",
      "storage",
      "storage_error",
      "string_view_at",
      "string_view_bounds_error",
      "strong",
      "subtract",
      "subtract_assign",
      "source",
      "take",
      "target",
      "to_c_string_view",
      "truncated",
      "try_allocate",
      "try_construct",
      "unique_owner_is_null",
      "unique_owner_upcast",
      "unsigned_count",
      "use_count",
      "upper",
      "unchecked_read",
      "unchecked_read_mut",
      "value",
      "values",
      "wrapping_add",
      "wrapping_mul",
      "wrapping_sub",
  };
  static constexpr std::string_view usedLanguageAndLibraryNames[] = {
      "abort",
      "align",
      "align_val_t",
      "alignof",
      "add_const_t",
      "auto",
      "bad_alloc",
      "bit_cast",
      "bool",
      "byte",
      "catch",
      "char",
      "class",
      "cmp_greater_equal",
      "cmp_less",
      "conditional_t",
      "const",
      "constexpr",
      "construct_at",
      "addressof",
      "is_convertible_v",
      "is_trivially_copy_constructible_v",
      "data",
      "decltype",
      "delete",
      "destroy_at",
      "digits",
      "double",
      "else",
      "expected",
      "explicit",
      "false",
      "float",
      "for",
      "forward",
      "fputc",
      "fputs",
      "gti_c_string_view",
      "if",
      "in_range",
      "inline",
      "integral_constant",
      "is_arithmetic_v",
      "is_union_v",
      "is_copy_constructible_v",
      "is_floating_point_v",
      "is_iec559",
      "is_integral_v",
      "is_same_v",
      "is_signed_v",
      "is_trivially_copyable_v",
      "is_trivially_destructible_v",
      "is_trivially_move_constructible_v",
      "is_unsigned_v",
      "isfinite",
      "launder",
      "ldexp",
      "make_unsigned_t",
      "max_exponent",
      "memcpy",
      "move",
      "namespace",
      "new",
      "nodiscard",
      "noexcept",
      "noreturn",
      "nothrow",
      "nullptr",
      "numeric_limits",
      "operator",
      "private",
      "public",
      "radix",
      "reinterpret_cast",
      "remove_cvref_t",
      "remove_reference_t",
      "requires",
      "reset",
      "return",
      "size",
      "size_t",
      "sizeof",
      "static_assert",
      "static_cast",
      "std",
      "string_view",
      "struct",
      "template",
      "this",
      "trunc",
      "try",
      "tuple_size_v",
      "typename",
      "uint32_t",
      "uint64_t",
      "uint8_t",
      "unexpected",
      "unique_ptr",
      "union",
      "unsigned",
      "using",
      "void",
  };
  output
      << "\n// Forced native headers must not rewrite GTI backend helpers.\n";
  for (const std::string_view name : helperNames) {
    output << "#undef " << name << '\n';
  }
  for (const std::string_view name : usedLanguageAndLibraryNames) {
    output << "#undef " << name << '\n';
  }
  output << '\n';
}

} // namespace

void writeCppSupportPreamble(std::ostream &output,
                             CppSupportPreambleFeatures features) {
  if (!features.storage || !features.definedFailure) {
    throw std::logic_error(
        "the current C++ artifact contract requires storage and failure "
        "support");
  }
  output << "// Generated by GTI.\n";
  writeCppBackendMacroIsolation(output);
  output << "#include <array>\n"
            "#include <bit>\n"
            "#include <cmath>\n"
            "#include <cstddef>\n"
            "#include <cstdint>\n"
            "#include <cstdio>\n"
            "#include <cstdlib>\n"
            "#include <exception>\n"
            "#include <iterator>\n"
            "#include <limits>\n"
            "#include <memory>\n"
            "#include <cstring>\n"
            "#include <new>\n"
            "#include <string_view>\n"
            "#include <type_traits>\n"
            "#include <utility>\n"
            "#include <variant>\n";
  if (features.payloadReferences)
    output << "#include <functional>\n";
  const bool usesManagedThreads = features.managedThreads;
  const bool usesAtomics = features.atomics;
  const bool usesMutexes = features.mutexes;
  if (usesManagedThreads) {
    output << "#include <tuple>\n";
  }
  if (usesAtomics || features.cThreads) {
    output << "#include <atomic>\n";
  }
  if (usesMutexes) {
    output << "#include <mutex>\n"
              "#include <optional>\n"
              "#include <system_error>\n";
  }
  if (features.runtimeBinding) {
    output << "#include <gti/runtime.hpp>\n";
  }
  output << "#include <gti/runtime_failure.h>\n";
  if (features.cThreads || features.threadLocalStorage) {
    output << "#include <gti/cthread.h>\n";
  }
  if (usesManagedThreads || features.threadLocalStorage) {
    output << "#include <gti/runtime_thread.hpp>\n";
  }
  const bool usesCAbiTextView = features.cAbiTextView;
  if (usesCAbiTextView) {
    output << "#include <gti/c_abi.h>\n";
  }
  writeCppBackendMacroIsolation(output);
  output << R"(

#if defined(__OPTIMIZE__) && (defined(__GNUC__) || defined(__clang__))
#define __gti_small_source_inline inline __attribute__((__always_inline__))
#else
#define __gti_small_source_inline inline
#endif

// The lifetime-slot template only transports this ABI record by pointer, so
// the lowered translation unit needs the type name but not the definition.
struct gti_failure_record_v2;

namespace gti_internal::backend {

// Payload-enum carrier. MIR, not this native type, selects legal
// alternatives, transfers GTI obligations and runs source cleanup. The union
// only retires the resulting native member lifetimes. Unlike std::variant,
// owning destruction is a direct tag branch rather than a visitor-table call.
// Passive members retain trivial native special members and byte transport.
template <typename... Values> union payload_union;
template <> union payload_union<> {
  constexpr payload_union() noexcept {}
  ~payload_union() = default;
};
template <typename Head, typename... Tail>
union payload_union<Head, Tail...> {
  Head head;
  payload_union<Tail...> tail;
  constexpr payload_union() noexcept {}
  payload_union(const payload_union &) = default;
  payload_union(payload_union &&) = default;
  payload_union &operator=(const payload_union &) = default;
  payload_union &operator=(payload_union &&) = default;
  ~payload_union() requires (std::is_trivially_destructible_v<Head> &&
                            (std::is_trivially_destructible_v<Tail> && ...)) = default;
  ~payload_union() noexcept {}

  template <std::size_t Index, typename... Args>
  constexpr void construct(Args &&...args) {
    if constexpr (Index == 0) {
      std::construct_at(std::addressof(head), std::forward<Args>(args)...);
    } else {
      std::construct_at(std::addressof(tail));
      tail.template construct<Index - 1>(std::forward<Args>(args)...);
    }
  }
  template <std::size_t Index> void destroy() noexcept {
    if constexpr (Index == 0) {
      std::destroy_at(std::addressof(head));
    } else {
      tail.template destroy<Index - 1>();
      std::destroy_at(std::addressof(tail));
    }
  }
  template <std::size_t Index> decltype(auto) get() & noexcept {
    if constexpr (Index == 0) return (head);
    else return tail.template get<Index - 1>();
  }
  template <std::size_t Index> decltype(auto) get() const & noexcept {
    if constexpr (Index == 0) return (head);
    else return tail.template get<Index - 1>();
  }
};

template <typename T>
inline constexpr bool payload_equal_v = requires(const T &left, const T &right) {
  bool(left == right);
};

// Each source alternative occupies the native index equal to its verified MIR
// variant ordinal, so a two-alternative enum's live discriminants are 0 and 1.
// The final alternative is the backend-private empty shell that default
// construction starts; it is never a source variant. The tag follows the
// storage. When a small carrier travels whole, as one scalar, native
// optimizers then fold its tag tests back into the branches that selected
// the alternative; a leading tag or shifted ordinals defeat that folding.
template <typename... Values> class payload_variant {
  static_assert(sizeof...(Values) != 0);
  using tag_type = std::conditional_t<(sizeof...(Values) < 256), std::uint8_t,
      std::conditional_t<(sizeof...(Values) < 65536), std::uint16_t, std::size_t>>;
  static constexpr tag_type vacant = static_cast<tag_type>(-1);
  static constexpr std::size_t empty_index = sizeof...(Values) - 1;
  payload_union<Values...> storage_;
  tag_type tag_ = vacant;

  template <std::size_t Index = 0, typename Callable>
  static void dispatch(std::size_t tag, Callable &&callable) {
    if constexpr (Index < sizeof...(Values)) {
      if (tag == Index) callable.template operator()<Index>();
      else dispatch<Index + 1>(tag, std::forward<Callable>(callable));
    }
  }
  template <std::size_t Index, typename... Args>
  constexpr void construct(Args &&...args) {
    static_assert(Index < sizeof...(Values));
    // Publish only after complete construction. If native construction throws,
    // no alternative is live; any active recursive tails contain no resource.
    storage_.template construct<Index>(std::forward<Args>(args)...);
    tag_ = static_cast<tag_type>(Index);
  }
  void destroy_active() noexcept {
    dispatch(tag_, [this]<std::size_t Index>() {
      storage_.template destroy<Index>();
    });
    tag_ = vacant;
  }
public:
  constexpr payload_variant() noexcept(std::is_nothrow_default_constructible_v<
      std::tuple_element_t<empty_index, std::tuple<Values...>>>) {
    construct<empty_index>();
  }
  template <std::size_t Index, typename... Args>
  constexpr explicit payload_variant(std::in_place_index_t<Index>, Args &&...args) {
    construct<Index>(std::forward<Args>(args)...);
  }
  payload_variant(const payload_variant &)
      requires (std::is_trivially_copy_constructible_v<Values> && ...) = default;
  payload_variant(const payload_variant &other)
      requires ((std::is_copy_constructible_v<Values> && ...) &&
                !(std::is_trivially_copy_constructible_v<Values> && ...)) {
    dispatch(other.tag_, [this, &other]<std::size_t Index>() {
      construct<Index>(other.storage_.template get<Index>());
    });
  }
  payload_variant(payload_variant &&)
      requires (std::is_trivially_move_constructible_v<Values> && ...) = default;
  payload_variant(payload_variant &&other)
      noexcept((std::is_nothrow_move_constructible_v<Values> && ...))
      requires ((std::is_move_constructible_v<Values> && ...) &&
                !(std::is_trivially_move_constructible_v<Values> && ...)) {
    dispatch(other.tag_, [this, &other]<std::size_t Index>() {
      construct<Index>(std::move(other.storage_.template get<Index>()));
    });
  }
  payload_variant &operator=(const payload_variant &)
      requires ((std::is_trivially_copy_constructible_v<Values> && ...) &&
                (std::is_trivially_copy_assignable_v<Values> && ...) &&
                (std::is_trivially_destructible_v<Values> && ...)) = default;
  payload_variant &operator=(const payload_variant &other)
      requires ((std::is_copy_constructible_v<Values> && ...) &&
                (std::is_copy_assignable_v<Values> && ...) &&
                !((std::is_trivially_copy_constructible_v<Values> && ...) &&
                  (std::is_trivially_copy_assignable_v<Values> && ...) &&
                  (std::is_trivially_destructible_v<Values> && ...))) {
    if (this == std::addressof(other)) return *this;
    if (tag_ != other.tag_) {
      destroy_active();
      dispatch(other.tag_, [this, &other]<std::size_t Index>() {
        construct<Index>(other.storage_.template get<Index>());
      });
    } else {
      dispatch(tag_, [this, &other]<std::size_t Index>() {
        storage_.template get<Index>() = other.storage_.template get<Index>();
      });
    }
    return *this;
  }
  // Private SSA/binding transport can copy-construct a passive representation
  // even when its source type deliberately forbids assignment. This does not
  // grant GTI assignment: verified source traits still govern that operation.
  payload_variant &operator=(const payload_variant &other) noexcept
      requires ((std::is_nothrow_copy_constructible_v<Values> && ...) &&
                (std::is_trivially_destructible_v<Values> && ...) &&
                !(std::is_copy_assignable_v<Values> && ...)) {
    if (this == std::addressof(other)) return *this;
    destroy_active();
    dispatch(other.tag_, [this, &other]<std::size_t Index>() {
      construct<Index>(other.storage_.template get<Index>());
    });
    return *this;
  }
  payload_variant &operator=(payload_variant &&)
      requires ((std::is_trivially_move_constructible_v<Values> && ...) &&
                (std::is_trivially_move_assignable_v<Values> && ...) &&
                (std::is_trivially_destructible_v<Values> && ...)) = default;
  payload_variant &operator=(payload_variant &&other)
      noexcept((std::is_nothrow_move_constructible_v<Values> && ...) &&
               (std::is_nothrow_move_assignable_v<Values> && ...))
      requires ((std::is_move_constructible_v<Values> && ...) &&
                (std::is_move_assignable_v<Values> && ...) &&
                !((std::is_trivially_move_constructible_v<Values> && ...) &&
                  (std::is_trivially_move_assignable_v<Values> && ...) &&
                  (std::is_trivially_destructible_v<Values> && ...))) {
    if (this == std::addressof(other)) return *this;
    if (tag_ != other.tag_) {
      destroy_active();
      dispatch(other.tag_, [this, &other]<std::size_t Index>() {
        construct<Index>(std::move(other.storage_.template get<Index>()));
      });
    } else {
      dispatch(tag_, [this, &other]<std::size_t Index>() {
        storage_.template get<Index>() = std::move(other.storage_.template get<Index>());
      });
    }
    return *this;
  }
  // GTI owning-enum replacement has already run the destination's checked
  // source cleanup. Nonassignable payloads need only move construction here:
  // retire the now-clean native shell and publish the new tag after construction.
  // This overload is not a source cleanup mechanism or permission to relocate
  // the payload bytes (for example, a mutex recreates its native mutex).
  payload_variant &operator=(payload_variant &&other)
      noexcept((std::is_nothrow_move_constructible_v<Values> && ...))
      requires ((std::is_move_constructible_v<Values> && ...) &&
                !(std::is_move_assignable_v<Values> && ...)) {
    if (this == std::addressof(other)) return *this;
    destroy_active();
    dispatch(other.tag_, [this, &other]<std::size_t Index>() {
      construct<Index>(std::move(other.storage_.template get<Index>()));
    });
    return *this;
  }
  ~payload_variant() requires (std::is_trivially_destructible_v<Values> && ...) = default;
  ~payload_variant() noexcept { destroy_active(); }
  // Only verified enum cleanup calls this, after its complete source cleanup
  // recipe has drained the alternative. Retire the
  // native shell on success and failure so a later lexical drop cannot run
  // the same source cleanup twice. No GTI value observes the vacant interval.
  void retire_cleaned_alternative() noexcept { destroy_active(); }
  std::size_t index() const noexcept {
    return std::size_t(tag_);
  }
  // Access is selected only by a sealed MIR alternative and dominated by its
  // tag proof. No public GTI unchecked accessor or layout promise is created.
  template <std::size_t Index> decltype(auto) get() & noexcept {
    return storage_.template get<Index>();
  }
  template <std::size_t Index> decltype(auto) get() const & noexcept {
    return storage_.template get<Index>();
  }
  friend bool operator==(const payload_variant &left, const payload_variant &right)
      requires (payload_equal_v<Values> && ...) {
    if (left.tag_ != right.tag_) return false;
    bool equal = true;
    dispatch(left.tag_, [&]<std::size_t Index>() {
      equal = left.storage_.template get<Index>() == right.storage_.template get<Index>();
    });
    return equal;
  }
};

template <std::size_t Index, typename... Values>
decltype(auto) payload_get(payload_variant<Values...> &value) noexcept {
  return value.template get<Index>();
}
template <std::size_t Index, typename... Values>
decltype(auto) payload_get(const payload_variant<Values...> &value) noexcept {
  return value.template get<Index>();
}
template <std::size_t Index, typename... Values>
decltype(auto) payload_get(payload_variant<Values...> &&value) noexcept {
  return std::move(value.template get<Index>());
}
template <std::size_t Index, typename... Values>
decltype(auto) payload_get(const payload_variant<Values...> &&value) noexcept {
  return std::move(value.template get<Index>());
}
// Only the exact verified entry-boundary closed-domain proof may call this.
// Ordinary MIR Unreachable and every source defined-failure path still abort
// or propagate their original diagnostic; carrier shells are not this domain.
[[noreturn]] inline void mir_closed_enum_unreachable_v1() noexcept {
#if defined(__clang__) || defined(__GNUC__)
  __builtin_unreachable();
#elif defined(_MSC_VER)
  __assume(false);
#else
  std::abort();
#endif
}
// A union's implicit copy/move copies its object representation, rather than
// evaluating its pointer member ([class.copy.ctor], [class.copy.assign]).
// GTI transport and comparison therefore stay inert after a pointee expires,
// while an addressable pointer slot is an actual native T* object. This permits
// stored/forwarded T** values and foreign writeback without call-local staging.
// Native pointer values are evaluated only for MIR-admitted unsafe access,
// arithmetic or an explicit foreign boundary.
// GTI's supported hosted targets have integral object-pointer representations.
static_assert(sizeof(std::uintptr_t) == 8 &&
              sizeof(std::uintptr_t) == sizeof(void *) &&
              alignof(std::uintptr_t) == alignof(void *),
              "GTI raw addresses require an integral object-pointer ABI");
static_assert(std::endian::native == std::endian::little,
              "GTI raw addresses require the supported little-endian ABI");
#if !defined(__aarch64__) && !defined(_M_ARM64) && \
    !defined(__x86_64__) && !defined(_M_X64)
#error "GTI raw addresses require the supported arm64 or x86_64 native ABI"
#endif
#if defined(__CHERI_PURE_CAPABILITY__)
#error "GTI integer raw-address representation does not support capability pointers"
#endif
template <typename T>
union raw_address {
  template <typename> friend union raw_address;
  T *native_slot_;

  std::uintptr_t address_bits() const noexcept {
    std::uintptr_t result = 0;
    static_assert(sizeof(native_slot_) == sizeof(result));
    std::memcpy(&result, &native_slot_, sizeof(result));
    return result;
  }
public:
  constexpr raw_address() noexcept : native_slot_(nullptr) {}
  constexpr raw_address(std::nullptr_t) noexcept : native_slot_(nullptr) {}
  template <typename Source>
    requires ((!std::is_const_v<Source> || std::is_const_v<T>) &&
              (std::is_same_v<std::remove_const_t<Source>, std::remove_const_t<T>> ||
               std::is_void_v<T>))
  raw_address(Source *const &value) noexcept : native_slot_(nullptr) {
    // Read the native slot's representation, not its pointer value. Capturing
    // an expired C-compatible field remains inert, including qualification.
    // GTI's admitted native ABIs use flat 64-bit object addresses and all-zero
    // null pointers; this is not a promise for arbitrary C++ pointer models.
    static_assert(sizeof(Source *) == sizeof(native_slot_));
    std::memcpy(&native_slot_, &value, sizeof(native_slot_));
  }

  template <typename Source>
    requires (std::is_same_v<std::remove_const_t<Source>, std::remove_const_t<T>> &&
              (!std::is_const_v<Source> || std::is_const_v<T>))
  raw_address(raw_address<Source> value) noexcept : native_slot_(nullptr) {
    static_assert(sizeof(value) == sizeof(native_slot_));
    std::memcpy(&native_slot_, &value, sizeof(native_slot_));
  }

  template <typename Source>
    requires ((!std::is_const_v<Source> || std::is_const_v<T>) &&
              !std::is_same_v<std::remove_const_t<Source>, std::remove_const_t<T>> &&
              (std::is_void_v<Source> || std::is_void_v<T>))
  explicit raw_address(raw_address<Source> value) noexcept : native_slot_(nullptr) {
    static_assert(sizeof(value) == sizeof(native_slot_));
    std::memcpy(&native_slot_, &value, sizeof(native_slot_));
  }

  // Evaluation happens only at MIR-admitted unsafe access, arithmetic or a
  // foreign boundary, where the slot must hold a usable address. A typed load
  // of the slot keeps that pointer's provenance and its distinct type visible
  // to native alias analysis, unlike an integer round trip through its bytes.
  T *native_address() const noexcept { return native_slot_; }
  operator T *() const noexcept { return this->native_address(); }

  // Native pointer storage may contain an expired address. Transferring its
  // representation through references does not evaluate that pointer value.
  // Taking its address does not inspect the pointer stored in the slot.
  void copy_to_native_slot(T *&destination) const noexcept {
    static_assert(sizeof(destination) == sizeof(native_slot_));
    if (&destination != &native_slot_)
      std::memcpy(&destination, &native_slot_, sizeof(destination));
  }
  void copy_from_native_slot(T *const &source) noexcept {
    static_assert(sizeof(source) == sizeof(native_slot_));
    if (&source != &native_slot_)
      std::memcpy(&native_slot_, &source, sizeof(source));
  }

  T **native_slot_address() noexcept { return &native_slot_; }
  T *const *native_slot_address() const noexcept { return &native_slot_; }

  template <typename Source>
  bool operator==(raw_address<Source> other) const noexcept {
    return address_bits() == other.address_bits();
  }
  bool operator==(std::nullptr_t) const noexcept { return address_bits() == 0; }

  // These operators occur only at independently verified unsafe MIR sites.
  std::add_lvalue_reference_t<T> operator*() const requires (!std::is_void_v<T>) {
    return *this->native_address();
  }
  T *operator->() const requires (!std::is_void_v<T>) { return this->native_address(); }
  template <typename Index> requires (std::is_integral_v<Index> && !std::is_void_v<T>)
  std::add_lvalue_reference_t<T> operator[](Index index) const {
    return this->native_address()[index];
  }
  template <typename Index> requires (std::is_integral_v<Index> && !std::is_void_v<T>)
  raw_address operator+(Index index) const { return this->native_address() + index; }
  template <typename Index> requires (std::is_integral_v<Index> && !std::is_void_v<T>)
  friend raw_address operator+(Index index, raw_address value) { return value + index; }
  template <typename Index> requires (std::is_integral_v<Index> && !std::is_void_v<T>)
  raw_address operator-(Index index) const { return this->native_address() - index; }
  std::ptrdiff_t operator-(raw_address other) const requires (!std::is_void_v<T>) {
    return this->native_address() - other.native_address();
  }
  template <typename Index> requires (std::is_integral_v<Index> && !std::is_void_v<T>)
  raw_address &operator+=(Index index) { return *this = *this + index; }
  template <typename Index> requires (std::is_integral_v<Index> && !std::is_void_v<T>)
  raw_address &operator-=(Index index) { return *this = *this - index; }
  raw_address &operator++() requires (!std::is_void_v<T>) { return *this += 1; }
  raw_address operator++(int) requires (!std::is_void_v<T>) {
    auto previous = *this; ++*this; return previous;
  }
  raw_address &operator--() requires (!std::is_void_v<T>) { return *this -= 1; }
  raw_address operator--(int) requires (!std::is_void_v<T>) {
    auto previous = *this; --*this; return previous;
  }
};
static_assert(std::is_trivially_copyable_v<raw_address<int>> &&
              std::is_union_v<raw_address<int>> &&
              std::is_trivially_destructible_v<raw_address<int>> &&
              sizeof(raw_address<void>) == sizeof(void *) &&
              alignof(raw_address<void>) == alignof(void *));

// Backend representation trait only: MIR already selected a semantic scalar.
// The address carrier is a C++ class, not a native pointer.
template <typename T> struct raw_address_traits {
  static constexpr bool value = false;
};
template <typename T> struct raw_address_traits<raw_address<T>> {
  static constexpr bool value = true;
};
template <typename T>
inline constexpr bool is_raw_address_v = raw_address_traits<std::remove_cv_t<T>>::value;

template <typename T>
T *native_pointer_value(raw_address<T> value) noexcept { return value.native_address(); }
template <typename T>
T *native_pointer_value(T *value) noexcept { return value; }

template <typename T, typename U>
  requires std::is_convertible_v<U *, T *>
void raw_pointer_slot_copy(T *&destination, const raw_address<U> &source) noexcept {
  raw_address<T>(source).copy_to_native_slot(destination);
}
template <typename T, typename U>
  requires std::is_convertible_v<U *, T *>
void raw_pointer_slot_copy(raw_address<T> &destination, U *const &source) noexcept {
  destination = raw_address<T>(source);
}
template <typename T, typename U>
  requires (std::is_convertible_v<U *, T *> &&
            (std::is_void_v<T> ||
             std::is_same_v<std::remove_cv_t<T>, std::remove_cv_t<U>>))
void raw_pointer_slot_copy(T *&destination, U *const &source) noexcept {
  static_assert(sizeof(destination) == sizeof(source));
  if (static_cast<const void *>(&destination) !=
      static_cast<const void *>(&source))
    std::memcpy(&destination, &source, sizeof(destination));
}
template <typename T, typename U>
  requires std::is_convertible_v<U *, T *>
void raw_pointer_slot_copy(raw_address<T> &destination,
                           const raw_address<U> &source) noexcept {
  destination = raw_address<T>(source);
}
template <typename T>
void raw_pointer_slot_copy(T *&destination, std::nullptr_t) noexcept {
  destination = nullptr;
}
template <typename T>
void raw_pointer_slot_copy(raw_address<T> &destination, std::nullptr_t) noexcept {
  destination = nullptr;
}

template <typename T>
raw_address<T> raw_address_of(T *value) noexcept { return value; }
template <typename T>
raw_address<T *> raw_address_of(raw_address<T> *slot) noexcept {
  return slot->native_slot_address();
}
template <typename T>
raw_address<T *const> raw_address_of(const raw_address<T> *slot) noexcept {
  return slot->native_slot_address();
}

// The address of an array element must name an element of a real native
// pointer array: adjacent union members are not elements of one T* array.
// The enclosing union keeps whole-array transport representation-preserving,
// including after any individual pointee expires.
template <typename T, std::size_t Extent>
union raw_pointer_array {
  std::array<T *, Extent> slots_;
public:
  constexpr raw_pointer_array() noexcept : slots_{} {}
  template <typename... Values>
    requires (sizeof...(Values) != 0 && sizeof...(Values) <= Extent &&
              ((is_raw_address_v<Values> || std::is_pointer_v<Values> ||
                std::is_same_v<Values, std::nullptr_t>) && ...))
  raw_pointer_array(const Values &...values) noexcept : slots_{} {
    std::size_t index = 0;
    (raw_pointer_slot_copy(slots_[index++], values), ...);
  }
  T *&operator[](std::size_t index) noexcept { return slots_[index]; }
  T *const &operator[](std::size_t index) const noexcept { return slots_[index]; }
  static constexpr std::size_t size() noexcept { return Extent; }
  T **data() noexcept { return slots_.data(); }
  T *const *data() const noexcept { return slots_.data(); }
};
static_assert(std::is_trivially_copyable_v<raw_pointer_array<int, 2>> &&
              std::is_trivially_destructible_v<raw_pointer_array<int, 2>> &&
              sizeof(raw_pointer_array<int, 2>) == 2 * sizeof(int *));

template <typename Array> struct fixed_array_extent;
template <typename T, std::size_t Extent>
struct fixed_array_extent<std::array<T, Extent>>
    : std::integral_constant<std::size_t, Extent> {};
template <typename T, std::size_t Extent>
struct fixed_array_extent<raw_pointer_array<T, Extent>>
    : std::integral_constant<std::size_t, Extent> {};

// The MIR caller has selected construction into distinct final storage.
// Byte-copy an exact trivial copy/move without evaluating scalar subobjects:
// a native C-compatible record may contain inert, expired pointer addresses.
// All other constructor selections retain ordinary native construction. This
// helper neither grants source copyability nor transfers cleanup obligations.
template <typename T, typename... Args>
T *construct_representation_at(T *destination, Args &&...args) {
  if constexpr (sizeof...(Args) == 1 && std::is_trivially_copyable_v<T> &&
                std::is_trivially_constructible_v<T, Args...> &&
                (std::is_same_v<T, std::remove_cvref_t<Args>> && ...)) {
    (static_cast<void>(std::memcpy(static_cast<void *>(destination),
                                  static_cast<const void *>(std::addressof(args)),
                                  sizeof(T))), ...);
    return std::launder(destination);
  } else {
    return std::construct_at(destination, std::forward<Args>(args)...);
  }
}

// Assignment targets an existing representation, unlike construction above.
// Exact trivial assignment needs no pointer lvalue-to-rvalue conversions and
// self-assignment must not pass overlapping regions to memcpy.
template <typename T, typename Source>
void assign_representation(T &destination, Source &&source) {
  using SourceType = std::remove_cvref_t<Source>;
  if constexpr ((std::is_pointer_v<T> || is_raw_address_v<T>) &&
                (std::is_pointer_v<SourceType> || is_raw_address_v<SourceType> ||
                 std::is_same_v<SourceType, std::nullptr_t>)) {
    raw_pointer_slot_copy(destination, source);
  } else if constexpr (std::is_same_v<T, SourceType> &&
                       std::is_trivially_copyable_v<T> &&
                       std::is_trivially_assignable_v<T &, Source>) {
    if (std::addressof(destination) != std::addressof(source))
      std::memcpy(std::addressof(destination), std::addressof(source), sizeof(T));
  } else {
    destination = std::forward<Source>(source);
  }
}

// Called only for MIR-proven, failure-free structural relocation. Destruction
// here ends inactive representation shells, not GTI cleanup obligations.
template <typename T>
void mir_object_relocate_v1(T *destination, T *source) {
  construct_representation_at(destination, std::move(*source));
  std::destroy_at(source);
}

// MIR has verified bitwise relocation, not merely failure-free movement.
// memcpy begins the implicit-lifetime native representations; ending the old
// trivial shells never invokes GTI cleanup. Extent and nonoverlap are unsafe
// caller obligations, shared with the source API (including byte-size fit).
template <typename T>
void mir_object_relocate_range_v1(T *destination, T *source, std::uint64_t count) {
  static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
  if (count == 0) return;
  std::memcpy(destination, source, static_cast<std::size_t>(count) * sizeof(T));
  std::destroy_n(source, static_cast<std::size_t>(count));
}

template <typename T>
T mir_restoring_exchange_v1(T &destination, T &&replacement) {
  T previous(std::move(destination));
  std::destroy_at(std::addressof(destination));
  std::construct_at(std::addressof(destination), std::move(replacement));
  return previous;
}

// The caller supplies distinct final output storage for native records whose
// implicit C++ value return would evaluate expired pointer subobjects.
template <typename T>
void mir_restoring_exchange_into_v1(T &destination, T &&replacement, T *result) {
  construct_representation_at(result, std::move(destination));
  std::destroy_at(std::addressof(destination));
  construct_representation_at(std::addressof(destination), std::move(replacement));
}

template <typename T>
void mir_restoring_swap_v1(T &left, T &right) {
  if (std::addressof(left) == std::addressof(right)) return;
  if constexpr (std::is_trivially_copyable_v<T> &&
                std::is_trivially_move_constructible_v<T>) {
    alignas(T) unsigned char previous[sizeof(T)];
    std::memcpy(previous, std::addressof(left), sizeof(T));
    std::memcpy(std::addressof(left), std::addressof(right), sizeof(T));
    std::memcpy(std::addressof(right), previous, sizeof(T));
    return;
  }
  T previous(std::move(left));
  std::destroy_at(std::addressof(left));
  std::construct_at(std::addressof(left), std::move(right));
  std::destroy_at(std::addressof(right));
  std::construct_at(std::addressof(right), std::move(previous));
}

)";
  output << "inline bool mir_cleanup_emergency_abandonment() noexcept {\n"
         << (usesManagedThreads ? "  return "
                                  "::gti_runtime_detail::managed_cleanup_"
                                  "emergency_handoff_active();\n"
                                : "  return false;\n")
         << "}\n";
  output << R"(

template <typename T>
inline constexpr std::uint64_t mir_lambda_identity_v = [] {
  if constexpr (requires { typename T::__gti_mir_lambda_identity; })
    return T::__gti_mir_lambda_identity::value;
  else
    return std::uint64_t{0};
}();

// The first closure identity among a type's type-only template arguments, in
// order. It selects the verified body of a callable instance whose closure is
// nested in a class argument rather than being the argument itself.
template <typename T>
struct mir_nested_lambda_identity {
  static constexpr std::uint64_t value = mir_lambda_identity_v<T>;
};
template <typename T>
struct mir_nested_lambda_identity<T &> : mir_nested_lambda_identity<T> {};
template <typename T>
struct mir_nested_lambda_identity<const T> : mir_nested_lambda_identity<T> {};
template <template <typename...> class Template, typename... Arguments>
struct mir_nested_lambda_identity<Template<Arguments...>> {
  static constexpr std::uint64_t value = [] {
    std::uint64_t result = mir_lambda_identity_v<Template<Arguments...>>;
    ((result = result != 0
                   ? result
                   : mir_nested_lambda_identity<Arguments>::value),
     ...);
    return result;
  }();
};
template <typename T>
inline constexpr std::uint64_t mir_nested_lambda_identity_v =
    mir_nested_lambda_identity<T>::value;

// Defined with the failure-ABI helpers below. Capability representations may
// accept it by reference here so a transformed constructor can create an
// observation-free native empty state before publishing the MIR result.
struct mir_empty_state_constructor_tag_v1;
template <typename T>
struct mir_empty_state_factory_v1;
template <std::uint64_t Constructor>
struct mir_failure_constructor_tag_v1;
template <std::uint64_t Function, typename Result>
struct function_item final { using __gti_mir_source_result = Result; };

// Native callable signatures may use status/out-result channels. Preserve
// GTI's verified logical result as metadata instead of inspecting operator().
template <typename Callable>
struct source_callable_result {
  using type = typename Callable::__gti_mir_source_result;
};

template <typename T> std::false_type __gti_byte_transport_tag(T *);
template <typename T>
struct byte_transport : decltype(__gti_byte_transport_tag(static_cast<T *>(nullptr))) {};
template <typename T, std::size_t Size>
struct byte_transport<std::array<T, Size>> : byte_transport<T> {};

// GTI invocation pointers carry no environment and use one uniform checked
// failure ABI. This transport tag is the same representation fact used for
// ordinary verified GTI calls; C-ABI raw-slot records must not be native-copied.
template <typename T>
using function_pointer_parameter = std::conditional_t<
    !std::is_reference_v<T> && byte_transport<std::remove_cvref_t<T>>::value,
    const T &, T>;
// A reference result publishes its loan pointer through the `T **` channel
// shared with every verified GTI function body (ADR 018 §5).
template <typename Result>
using function_pointer_result =
    std::conditional_t<std::is_reference_v<Result>,
                       std::remove_reference_t<Result> *, Result>;
template <typename Signature> struct function_pointer;
template <typename Result, typename... Arguments>
struct function_pointer<Result(Arguments...)> final {
  using pointer = bool (*)(function_pointer_parameter<Arguments>...,
                           function_pointer_result<Result> *,
                           ::gti_failure_record_v2 *);
  pointer target;
};
template <typename... Arguments>
struct function_pointer<void(Arguments...)> final {
  using pointer = bool (*)(function_pointer_parameter<Arguments>...,
                           ::gti_failure_record_v2 *);
  pointer target;
};

// A container alternative owns exactly one ordinary representation lifetime.
// Exact trivial copies preserve bytes; nontrivial values retain their selected
// native special members, including GTI's generated active-drop transfer.
template <typename T>
class representation_cell {
  alignas(T) unsigned char bytes_[sizeof(T)];
  T *slot() noexcept { return reinterpret_cast<T *>(bytes_); }
public:
  representation_cell() requires std::is_default_constructible_v<T> {
    construct_representation_at(slot());
  }
  template <typename U> requires
      (!std::is_same_v<std::remove_cvref_t<U>, representation_cell> &&
       std::is_constructible_v<T, U>)
  representation_cell(U &&value) {
    construct_representation_at(slot(), std::forward<U>(value));
  }
  representation_cell(const representation_cell &other)
      requires std::is_copy_constructible_v<T> {
    construct_representation_at(slot(), other.get());
  }
  representation_cell(const representation_cell &)
      requires (!std::is_copy_constructible_v<T>) = delete;
  representation_cell(representation_cell &&other)
      noexcept(std::is_nothrow_move_constructible_v<T>)
      requires std::is_move_constructible_v<T> {
    construct_representation_at(slot(), std::move(other.get()));
  }
  representation_cell &operator=(const representation_cell &other)
      requires std::is_copy_assignable_v<T> {
    if (this != &other) {
      if constexpr (std::is_trivially_copyable_v<T> &&
                    std::is_trivially_copy_assignable_v<T>)
        std::memcpy(slot(), std::addressof(other.get()), sizeof(T));
      else
        get() = other.get();
    }
    return *this;
  }
  representation_cell &operator=(representation_cell &&other)
      noexcept(std::is_nothrow_move_assignable_v<T>)
      requires std::is_move_assignable_v<T> {
    if (this != &other) {
      if constexpr (std::is_trivially_copyable_v<T> &&
                    std::is_trivially_move_assignable_v<T>)
        std::memcpy(slot(), std::addressof(other.get()), sizeof(T));
      else
        get() = std::move(other.get());
    }
    return *this;
  }
  ~representation_cell() { std::destroy_at(std::addressof(get())); }
  T &get() & noexcept { return *std::launder(slot()); }
  const T &get() const & noexcept {
    return *std::launder(reinterpret_cast<const T *>(bytes_));
  }
  T &&get() && noexcept { return std::move(get()); }
  const T &&get() const && noexcept { return std::move(get()); }
  friend bool operator==(const representation_cell &left,
                         const representation_cell &right) {
    return left.get() == right.get();
  }
};
template <typename T>
using byte_field = std::conditional_t<byte_transport<T>::value,
                                       representation_cell<T>, T>;
)";
  output << R"(

static_assert(__gti_strict_ieee754 == 1,
              "compile GTI output with strict IEEE-754 flags and "
              "-D__gti_strict_ieee754=1");
static_assert(std::numeric_limits<char8_t>::digits == 8 &&
              std::is_unsigned_v<char8_t> &&
              !std::is_same_v<char8_t, std::uint8_t>,
              "GTI requires distinct unsigned eight-bit char code units");
static_assert(sizeof(float) == sizeof(std::uint32_t) &&
              std::numeric_limits<float>::is_iec559 &&
              std::numeric_limits<float>::radix == 2 &&
              std::numeric_limits<float>::digits == 24 &&
              std::numeric_limits<float>::max_exponent == 128,
              "GTI requires IEEE-754 binary32 host float semantics");
static_assert(sizeof(double) == sizeof(std::uint64_t) &&
              std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::radix == 2 &&
              std::numeric_limits<double>::digits == 53 &&
              std::numeric_limits<double>::max_exponent == 1024,
              "GTI requires IEEE-754 binary64 host double semantics");

[[noreturn]] inline void integer_domain_error(const char *message) {
  std::fputs("GTI runtime error: ", stderr);
  std::fputs(message, stderr);
  std::fputc('\n', stderr);
  std::abort();
}

[[noreturn]] inline void numeric_conversion_error() {
  std::fputs("GTI runtime error: numeric conversion is out of range\n", stderr);
  std::abort();
}

[[noreturn]] inline void array_bounds_error() {
  std::fputs("GTI runtime error: fixed array index out of bounds\n", stderr);
  std::abort();
}

[[noreturn]] inline void string_view_bounds_error() {
  std::fputs("GTI runtime error: string view index out of bounds\n", stderr);
  std::abort();
}

[[noreturn]] inline void allocation_error() {
  std::fputs("GTI runtime error: memory allocation failed\n", stderr);
  std::abort();
}

[[noreturn]] inline void empty_owner_error() {
  std::fputs("GTI runtime error: dereferenced an empty unique owner\n", stderr);
  std::abort();
}

[[noreturn]] inline void storage_error(const char *message) {
  std::fputs("GTI runtime error: ", stderr);
  std::fputs(message, stderr);
  std::fputc('\n', stderr);
  std::abort();
}

// The MIR unique-pointee descriptor proves a lifecycle-guarded leaf class.
// Run its source cleanup through the explicit record ABI, then destroy the
// disarmed representation and release its allocation exactly once.
template <typename T>
[[nodiscard]] bool mir_unique_owner_drop_with_failure_v1(
    std::unique_ptr<T> &owner, ::gti_failure_record_v2 *failure_record) {
  if (!owner) {
    return true;
  }
  bool success = false;
  try {
    success = owner->__gti_mir_destruct_failure(failure_record);
  } catch (...) {
    if (mir_cleanup_emergency_abandonment()) {
      (void)owner.release();
    } else {
      owner.reset();
    }
    throw;
  }
  owner.reset();
  return success;
}

// Inherited failure cleanup crosses ordinary source calls without changing
// their value ABI. The context belongs to one synchronous cleanup stack, not
// to an object or worker; its primary record stays alive in its owning frame.
struct mir_failure_cleanup_context_v1 {
  const ::gti_failure_record_v2 *record;
  const ::gti_failure_artifact_descriptor_v1 *artifact;
};
inline const mir_failure_cleanup_context_v1 *&mir_incoming_failure_cleanup_v1() {
  static thread_local const mir_failure_cleanup_context_v1 *current = nullptr;
  return current;
}
class mir_failure_cleanup_scope_v1 {
public:
  mir_failure_cleanup_scope_v1() = default;
  mir_failure_cleanup_scope_v1(const mir_failure_cleanup_scope_v1 &) = delete;
  mir_failure_cleanup_scope_v1 &operator=(const mir_failure_cleanup_scope_v1 &) = delete;
  mir_failure_cleanup_scope_v1(const ::gti_failure_record_v2 *record,
      const ::gti_failure_artifact_descriptor_v1 *artifact) {
    activate(record, artifact);
  }
  void activate(const ::gti_failure_record_v2 *record,
      const ::gti_failure_artifact_descriptor_v1 *artifact) {
    // A nested cleanup's local failure can never replace the earliest one.
    if (mir_incoming_failure_cleanup_v1() != nullptr) return;
    if (record == nullptr) std::abort();
    context_ = {record, artifact};
    mir_incoming_failure_cleanup_v1() = &context_;
    active_ = true;
  }
  ~mir_failure_cleanup_scope_v1() {
    if (active_) {
      if (mir_incoming_failure_cleanup_v1() != &context_) std::abort();
      mir_incoming_failure_cleanup_v1() = nullptr;
    }
  }
private:
  mir_failure_cleanup_context_v1 context_{};
  bool active_ = false;
};
// This envelope exists only on a terminal secondary-failure edge. Keeping it
// in a separate native frame bounds caller stack storage independently of the
// number of alternative cleanup paths. Managed-worker handoff may unwind out
// of this call, so it must not be noexcept.
[[noreturn]]
#if defined(__GNUC__) || defined(__clang__)
__attribute__((__cold__, __noinline__))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
inline void mir_terminate_cleanup_failure_v2(
    const ::gti_failure_record_v2 *primary,
    const ::gti_failure_record_v2 *secondary,
    const ::gti_failure_artifact_descriptor_v1 *primary_artifact,
    const ::gti_failure_artifact_descriptor_v1 *secondary_artifact) {
  if (primary == nullptr || secondary == nullptr) std::abort();
  const ::gti_failure_emergency_v2 emergency{
      GTI_FAILURE_ABI_VERSION_V2, UINT32_C(0), *primary, *secondary};
  ::gti_rt_failure_terminate_cleanup_v2(
      &emergency, primary_artifact, secondary_artifact);
}

// This helper is reached only after a MIR operation fails. Keep its emergency
// record/TLS work out of the successful caller's native inlining cost. It may
// return for a first failure or unwind through the managed-worker handoff;
// neither noexcept nor a terminal-function annotation would be sound here.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((__cold__, __noinline__))
#endif
inline void mir_check_incoming_cleanup_failure_v1(
    const ::gti_failure_record_v2 *secondary,
    const ::gti_failure_artifact_descriptor_v1 *artifact) {
  const auto *context = mir_incoming_failure_cleanup_v1();
  if (context == nullptr) return;
  if (secondary == nullptr) std::abort();
  const ::gti_failure_emergency_v2 emergency{
      GTI_FAILURE_ABI_VERSION_V2, UINT32_C(0), *context->record, *secondary};
  ::gti_rt_failure_terminate_cleanup_v2(&emergency, context->artifact, artifact);
}

// The aggregate schedule uses one record cursor across its ordered steps.
// An incoming failure stops on the next failure; otherwise the first failure
// is retained while remaining steps drain, and a second one terminates.
class mir_cleanup_sequence_v1 {
public:
  mir_cleanup_sequence_v1(::gti_failure_record_v2 *record, bool unwinding,
      const ::gti_failure_artifact_descriptor_v1 *artifact)
      : record_(record), unwinding_(unwinding), artifact_(artifact) {}
  ::gti_failure_record_v2 *record() { return success_ ? record_ : &secondary_; }
  bool unwinding() const { return unwinding_ || !success_; }
  bool step(bool success) {
    if (!success && unwinding_) return false;
    if (!success && !success_) {
      const ::gti_failure_emergency_v2 emergency{
          GTI_FAILURE_ABI_VERSION_V2, UINT32_C(0), *record_, secondary_};
      ::gti_rt_failure_terminate_cleanup_v2(&emergency, artifact_, artifact_);
    }
    success_ = success_ && success;
    if (!success_) context_.activate(record_, artifact_);
    return true;
  }
  bool success() const { return success_; }
private:
  ::gti_failure_record_v2 *record_;
  bool unwinding_;
  const ::gti_failure_artifact_descriptor_v1 *artifact_;
  bool success_ = true;
  ::gti_failure_record_v2 secondary_{};
  mir_failure_cleanup_scope_v1 context_;
};

// The verified array recipe visits immediate elements in reverse order. It
// retires semantic resources only; the enclosing lifetime operation ends the
// now-inert array representation after successful (or primary-failure) cleanup.
template <std::size_t Extent, typename Array, typename Cleanup>
[[nodiscard]] bool mir_array_cleanup_with_failure_v1(
    Array &array, ::gti_failure_record_v2 *record, bool unwinding,
    const ::gti_failure_artifact_descriptor_v1 *artifact, Cleanup cleanup) {
  static_assert(fixed_array_extent<std::remove_cvref_t<Array>>::value == Extent);
  mir_cleanup_sequence_v1 sequence(record, unwinding, artifact);
  for (std::size_t index = Extent; index != 0; --index) {
    if (!sequence.step(cleanup(array[index - 1], sequence.record(), sequence.unwinding())))
      return false;
  }
  return sequence.success();
}

template <std::size_t Instance, typename T>
[[nodiscard]] bool mir_unique_aggregate_owner_drop_with_failure_v1(
    std::unique_ptr<T> &owner, ::gti_failure_record_v2 *record, bool unwinding) {
  if (!owner) return true;
  const bool success =
      owner->template __gti_mir_unique_cleanup_failure<Instance>(record, unwinding);
  // A second failure terminates at the calling MIR edge. Do not let native
  // teardown run any remaining subobject cleanup before that terminal edge.
  if (!success && unwinding) return false;
  owner.reset();
  return success;
}

// Allocation binds the verified complete-object cleanup before publishing
// the owner. Upcasting retains that binding; the base type cannot replace it.
// This is representation metadata attached to an object's address, not a GTI
// field: constructing another object starts unbound, and value assignment
// preserves the destination allocation's existing selection.
class mir_polymorphic_cleanup_binding_v1 {
public:
  using function_type = bool (*)(void *, ::gti_failure_record_v2 *, bool);
  mir_polymorphic_cleanup_binding_v1() = default;
  mir_polymorphic_cleanup_binding_v1(const mir_polymorphic_cleanup_binding_v1 &) noexcept {}
  mir_polymorphic_cleanup_binding_v1(mir_polymorphic_cleanup_binding_v1 &&) noexcept {}
  mir_polymorphic_cleanup_binding_v1 &operator=(const mir_polymorphic_cleanup_binding_v1 &) noexcept { return *this; }
  mir_polymorphic_cleanup_binding_v1 &operator=(mir_polymorphic_cleanup_binding_v1 &&) noexcept { return *this; }
  void bind(function_type selected) noexcept {
    if (selected == nullptr || (function_ != nullptr && function_ != selected)) std::abort();
    function_ = selected;
  }
  bool run(void *object, ::gti_failure_record_v2 *record, bool unwinding) const {
    if (function_ == nullptr) std::abort();
    return function_(object, record, unwinding);
  }
private:
  function_type function_ = nullptr;
};

template <typename T>
[[nodiscard]] bool mir_polymorphic_owner_drop_with_failure_v1(
    std::unique_ptr<T> &owner, ::gti_failure_record_v2 *record, bool unwinding) {
  if (!owner) return true;
  const bool success =
      owner->__gti_mir_polymorphic_cleanup_failure(record, unwinding);
  if (!success && unwinding) return false;
  owner.reset();
  return success;
}

// An engaged semantic slot reaching native scope exit is either a verified
// cleanup-emergency handoff or a missing MIR Drop. Preserve both checks, but
// keep this exceptional path out of ordinary source wrapper inlining cost.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((__cold__, __noinline__))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
inline void mir_missing_drop_v1() noexcept {
  if (!mir_cleanup_emergency_abandonment())
    storage_error("verified MIR lifetime slot escaped without Drop");
}

// A verified MIR body owns the lifetime of values stored here. Unlike a
// normal C++ local or std::optional, this guard deliberately refuses to clean
// up an engaged value from its destructor: every successful construction must
// reach an explicit verified-MIR Drop instruction.
template <typename T, bool DestroyRepresentationAtScopeExit = false>
class mir_lifetime_slot {
public:
  mir_lifetime_slot() = default;
  mir_lifetime_slot(const mir_lifetime_slot &) = delete;
  mir_lifetime_slot &operator=(const mir_lifetime_slot &) = delete;

  ~mir_lifetime_slot() {
    if (!engaged_) {
      return;
    }
    // A verified cleanup-emergency edge explicitly stopped source cleanup.
    // The private worker handoff only retires native stack frames; neither
    // representation policy may manufacture an omitted GTI Drop here.
    if constexpr (DestroyRepresentationAtScopeExit) {
      if (mir_cleanup_emergency_abandonment()) return;
      // Some GTI values need a non-trivial C++ representation even though MIR
      // assigns no semantic Drop here, or has already transferred the value.
      // End only that backend representation lifetime.
      destroy();
    } else {
      mir_missing_drop_v1();
    }
  }

  template <typename... Args>
  void construct(Args &&...args) {
    if (engaged_) {
      storage_error("verified MIR lifetime slot was constructed twice");
    }
    construct_representation_at(slot(), std::forward<Args>(args)...);
    engaged_ = true;
  }

  template <typename Factory>
  void construct_from(Factory &&factory) {
    if (engaged_) {
      storage_error("verified MIR lifetime slot was constructed twice");
    }
    ::new (static_cast<void *>(slot()))
        T(std::forward<Factory>(factory)());
    engaged_ = true;
  }

  // A verified TransferOut hands an already prepared argument to its consumer.
  // Native pointer-bearing records use an internal reference/output ABI: leave
  // their trivial source shell alive until storage reuse so the consumer can
  // byte-copy it without a C++ by-value return evaluating pointer subobjects.
  [[nodiscard]] decltype(auto) take() {
    if constexpr (byte_transport<T>::value) {
      static_assert(std::is_trivially_copyable_v<T> &&
                    std::is_trivially_destructible_v<T>);
      T &value = get();
      engaged_ = false;
      return std::move(value);
    } else {
      T value(std::move(get()));
      destroy();
      return value;
    }
  }

  [[nodiscard]] T *construction_address() {
    if (engaged_) {
      storage_error("verified MIR lifetime slot requested construction while engaged");
    }
    return slot();
  }

  void mark_constructed() {
    if (engaged_) {
      storage_error("verified MIR lifetime slot marked constructed twice");
    }
    engaged_ = true;
  }

  void destroy() {
    if (!engaged_) {
      storage_error("verified MIR lifetime slot was dropped while empty");
    }
    engaged_ = false;
    // Retiring a trivial representation needs no destructor call. Its C++
    // lifetime may end when construction reuses the storage or storage is
    // released; get() still rejects the retired MIR slot immediately. Avoid
    // imposing an unnecessary lifetime-end barrier on native optimization.
    if constexpr (!std::is_trivially_destructible_v<T>) {
      std::destroy_at(std::launder(slot()));
    }
  }

  [[nodiscard]] bool
  destroy_with_failure(::gti_failure_record_v2 *failure_record) {
    if (!engaged_) {
      storage_error("verified MIR lifetime slot was dropped while empty");
    }
    engaged_ = false;
    T *value = std::launder(slot());
    bool success = false;
    try {
      success = value->__gti_mir_destruct_failure(failure_record);
    } catch (...) {
      if (!mir_cleanup_emergency_abandonment()) std::destroy_at(value);
      throw;
    }
    // The transformed helper ran the source destructor body and disarmed its
    // lifecycle state. End the C++ object lifetime so generated field/base
    // teardown still runs; the ordinary destructor wrapper now observes the
    // disarmed state and cannot run the source body twice.
    std::destroy_at(value);
    return success;
  }

  [[nodiscard]] bool
  destroy_with_failure_if_engaged(
      ::gti_failure_record_v2 *failure_record) {
    return !engaged_ || destroy_with_failure(failure_record);
  }

  [[nodiscard]] bool destroy_unique_with_failure(
      ::gti_failure_record_v2 *failure_record) {
    if (!engaged_) {
      storage_error("verified MIR unique owner was dropped while empty");
    }
    engaged_ = false;
    T *owner = std::launder(slot());
    bool success = false;
    try {
      success = mir_unique_owner_drop_with_failure_v1(*owner, failure_record);
    } catch (...) {
      if (!mir_cleanup_emergency_abandonment()) std::destroy_at(owner);
      throw;
    }
    std::destroy_at(owner);
    return success;
  }

  [[nodiscard]] bool destroy_unique_with_failure_if_engaged(
      ::gti_failure_record_v2 *failure_record) {
    return !engaged_ || destroy_unique_with_failure(failure_record);
  }

  template <std::size_t Instance>
  [[nodiscard]] bool destroy_unique_aggregate_with_failure(
      ::gti_failure_record_v2 *failure_record, bool conditional, bool unwinding) {
    if (!engaged_) {
      if (conditional) return true;
      storage_error("verified MIR unique aggregate was dropped while empty");
    }
    engaged_ = false;
    T *value = std::launder(slot());
    bool success = false;
    try {
      success = value->template __gti_mir_unique_cleanup_failure<Instance>(failure_record, unwinding);
    } catch (...) {
      if (!mir_cleanup_emergency_abandonment()) std::destroy_at(value);
      throw;
    }
    if (!success && unwinding) return false;
    std::destroy_at(value);
    return success;
  }

  template <std::size_t Instance>
  [[nodiscard]] bool destroy_unique_aggregate_owner_with_failure(
      ::gti_failure_record_v2 *record, bool conditional, bool unwinding) {
    if (!engaged_) {
      if (conditional) return true;
      storage_error("verified MIR unique aggregate owner was dropped while empty");
    }
    engaged_ = false;
    T *value = std::launder(slot());
    const bool success = mir_unique_aggregate_owner_drop_with_failure_v1<Instance>(
        *value, record, unwinding);
    if (!success && unwinding) return false;
    std::destroy_at(value);
    return success;
  }

  template <typename Cleanup>
  [[nodiscard]]
#if defined(_MSC_VER)
  __forceinline
#elif defined(__GNUC__) || defined(__clang__)
  __attribute__((always_inline))
#endif
  bool destroy_cleanup_with_failure(
      ::gti_failure_record_v2 *record, bool conditional, bool unwinding,
      Cleanup cleanup) {
    if (!engaged_) {
      if (conditional) return true;
      storage_error("verified MIR collection was dropped while empty");
    }
    engaged_ = false;
    T *value = std::launder(slot());
    const bool success = cleanup(*value, record, unwinding);
    if (!success && unwinding) return false;
    std::destroy_at(value);
    return success;
  }

  // MIR supplies the exact fixed-array extents. This slot validates those
  // extents against its C++ representation, then completes each leaf lifetime
  // in reverse order without invoking std::array's recursive destructor.
  template <std::size_t... Extents>
  [[nodiscard]] bool destroy_fixed_array_with_failure(
      ::gti_failure_record_v2 *failure_record, bool cleanup_failure,
      const ::gti_failure_artifact_descriptor_v1 *artifact) {
    static_assert(sizeof...(Extents) != 0);
    if (!engaged_) {
      storage_error("verified MIR lifetime slot was dropped while empty");
    }
    engaged_ = false;
    bool success = true;
    if (!destroy_fixed_array_level<Extents...>(
            *std::launder(slot()), failure_record, cleanup_failure, artifact,
            success)) {
      return false;
    }
    return success;
  }

  void destroy_if_engaged() {
    if (engaged_) {
      destroy();
    }
  }

  [[nodiscard]] T &get() {
    if (!engaged_) {
      storage_error("verified MIR lifetime slot was read while empty");
    }
    return *std::launder(slot());
  }

private:
  template <std::size_t Extent, std::size_t... Rest, typename Array>
  [[nodiscard]] static bool destroy_fixed_array_level(
      Array &array, ::gti_failure_record_v2 *failure_record,
      bool cleanup_failure,
      const ::gti_failure_artifact_descriptor_v1 *artifact, bool &success) {
    static_assert(fixed_array_extent<std::remove_cvref_t<Array>>::value == Extent);
    mir_failure_cleanup_scope_v1 context;
    for (std::size_t index = Extent; index != 0; --index) {
      auto &element = array[index - 1];
      // The leaf arm must be the discarded statement of this constexpr
      // branch: a trailing statement after `continue` would still
      // instantiate the leaf destructor call against a nested array level.
      if constexpr (sizeof...(Rest) != 0) {
        if (!destroy_fixed_array_level<Rest...>(
                element, failure_record, cleanup_failure, artifact, success)) {
          return false;
        }
      } else {
        if (!success) context.activate(failure_record, artifact);
        ::gti_failure_record_v2 secondary{};
        const bool already_cleaning = !success;
        ::gti_failure_record_v2 *element_record =
            already_cleaning ? &secondary : failure_record;
        const bool element_success =
            element.__gti_mir_destruct_failure(element_record);
        std::destroy_at(&element);
        if (!element_success && cleanup_failure) {
          return false;
        }
        if (!element_success && already_cleaning) {
          const ::gti_failure_emergency_v2 emergency{
              GTI_FAILURE_ABI_VERSION_V2, UINT32_C(0), *failure_record,
              secondary};
          ::gti_rt_failure_terminate_cleanup_v2(&emergency, artifact,
                                                artifact);
        }
        success = success && element_success;
      }
    }
    return true;
  }

  [[nodiscard]] T *slot() {
    return reinterpret_cast<T *>(storage_);
  }

  alignas(T) std::byte storage_[sizeof(T)];
  bool engaged_ = false;
};

// The identity-bound public logical-size check (P-STORAGE-01): container
// indexing reports the logical bound instead of leaking the private
// storage invariant.
inline void index_bounds_check(std::uint64_t index, std::uint64_t size) {
  if (index >= size) {
    storage_error("container index out of logical bounds");
  }
}

// Layout-only storage. These booleans are verified GTI traits, never native
// type-trait guesses. Copy/move transfers bytes without reading a live T and
// destruction does not inspect the bytes or invoke T's destructor.
template <typename T, bool Copyable, bool Movable>
class uninitialized_storage {
  alignas(T) std::byte bytes_[sizeof(T)];
public:
  uninitialized_storage() noexcept {}
  uninitialized_storage(const uninitialized_storage &other) noexcept requires(Copyable) {
    std::memcpy(bytes_, other.bytes_, sizeof(T));
  }
  uninitialized_storage(const uninitialized_storage &) requires(!Copyable) = delete;
  uninitialized_storage(uninitialized_storage &&other) noexcept requires(Movable) {
    std::memcpy(bytes_, other.bytes_, sizeof(T));
  }
  uninitialized_storage(uninitialized_storage &&) requires(!Movable) = delete;
  uninitialized_storage &operator=(const uninitialized_storage &other) noexcept requires(Copyable) {
    if (this != &other) std::memcpy(bytes_, other.bytes_, sizeof(T));
    return *this;
  }
  uninitialized_storage &operator=(const uninitialized_storage &) requires(!Copyable) = delete;
  uninitialized_storage &operator=(uninitialized_storage &&other) noexcept requires(Movable) {
    if (this != &other) std::memcpy(bytes_, other.bytes_, sizeof(T));
    return *this;
  }
  uninitialized_storage &operator=(uninitialized_storage &&) requires(!Movable) = delete;
  ~uninitialized_storage() = default;
};

// Inline zero-or-one storage used only by the declaration-bound
// <std/optional> capability. It has no allocation state: engagement and one
// correctly aligned T lifetime are carried directly in the containing object.
template <typename T>
class optional_storage {
public:
  optional_storage() = default;
  optional_storage(const optional_storage &) = delete;
  optional_storage &operator=(const optional_storage &) = delete;

  optional_storage(optional_storage &&other) {
    if (other.engaged_) {
      construct_representation_at(slot(), std::move(*std::launder(other.slot())));
      engaged_ = true;
      other.reset();
    }
  }

  optional_storage &operator=(optional_storage &&) = delete;

  ~optional_storage() {
    if (!mir_cleanup_emergency_abandonment()) reset();
  }

  [[nodiscard]] bool has_value() const noexcept { return engaged_; }

  template <typename... Args>
  void construct(Args &&...args) {
    if (engaged_) {
      storage_error("constructed an engaged optional storage slot");
    }
    try {
      construct_representation_at(slot(), std::forward<Args>(args)...);
    } catch (const std::bad_alloc &) {
      allocation_error();
    }
    engaged_ = true;
  }

  template <typename... Args>
  [[nodiscard]] bool try_construct(Args &&...args) {
    if (engaged_) {
      return false;
    }
    try {
      construct_representation_at(slot(), std::forward<Args>(args)...);
    } catch (const std::bad_alloc &) {
      return false;
    }
    engaged_ = true;
    return true;
  }

  [[nodiscard]] const T &read() const {
    if (!engaged_) {
      empty_owner_error();
    }
    return *std::launder(slot());
  }

  [[nodiscard]] T &read_mut() {
    if (!engaged_) {
      empty_owner_error();
    }
    return *std::launder(slot());
  }

  [[nodiscard]] const T &unchecked_read() const {
    return *std::launder(slot());
  }

  [[nodiscard]] T &unchecked_read_mut() { return *std::launder(slot()); }

  template <typename Cleanup>
  [[nodiscard]] bool reset_with_failure(::gti_failure_record_v2 *record,
                                        Cleanup cleanup,
                                        bool unwinding = false) {
    if (!engaged_) {
      return true;
    }
    // Retire engagement before executing user cleanup, just as sparse and
    // prefix storage retire the selected element before its destructor.
    engaged_ = false;
    T *element = std::launder(slot());
    const bool success = cleanup(*element, record, unwinding);
    // A second failure returns directly to the calling MIR terminal edge.
    // Native teardown must not run payload subobjects whose cleanup the
    // verified schedule deliberately stopped before reaching.
    if (!success && unwinding) {
      return false;
    }
    std::destroy_at(element);
    return success;
  }

  void reset() {
    if (engaged_) {
      engaged_ = false;
      std::destroy_at(std::launder(slot()));
    }
  }

private:
  [[nodiscard]] T *slot() {
    return reinterpret_cast<T *>(storage_);
  }

  [[nodiscard]] const T *slot() const {
    return reinterpret_cast<const T *>(storage_);
  }

  alignas(T) std::byte storage_[sizeof(T)];
  bool engaged_ = false;
};

template <typename T>
inline bool optional_storage_has_value(const optional_storage<T> &value) {
  return value.has_value();
}

template <typename T, typename... Args>
inline void optional_storage_construct(optional_storage<T> &value,
                                       Args &&...args) {
  value.construct(std::forward<Args>(args)...);
}

template <typename T>
inline const T &optional_storage_read(const optional_storage<T> &value) {
  return value.read();
}

template <typename T>
inline T &optional_storage_read_mut(optional_storage<T> &value) {
  return value.read_mut();
}

template <typename T>
inline const T &
optional_storage_read_unchecked(const optional_storage<T> &value) {
  return value.unchecked_read();
}

template <typename T>
inline T &optional_storage_read_mut_unchecked(optional_storage<T> &value) {
  return value.unchecked_read_mut();
}

template <typename T>
inline void optional_storage_reset(optional_storage<T> &value) {
  value.reset();
}

)";
  if (features.polymorphicStorage) {
    output << R"(
// Per-payload operations of the declaration-bound <std/polymorphic> slot.
// One constant table exists per payload type, so the slot stores only its
// address. Semantics admits payloads with failure-free generated relocation
// and inert cleanup only: none of these operations runs GTI source code.
template <typename Interface>
struct polymorphic_storage_operations {
  // Moves the live payload into vacant storage, then ends the source.
  void (*relocate)(void *destination, void *source);
  void (*destroy)(void *object);
  // Recovers the interface from the payload's current address.
  Interface *(*interface)(void *object);
};

template <typename Interface, typename Model>
inline constexpr polymorphic_storage_operations<Interface>
    polymorphic_storage_operations_for{
        [](void *destination, void *source) {
          mir_object_relocate_v1(static_cast<Model *>(destination),
                                 std::launder(static_cast<Model *>(source)));
        },
        [](void *object) {
          std::destroy_at(std::launder(static_cast<Model *>(object)));
        },
        [](void *object) -> Interface * {
          return std::launder(static_cast<Model *>(object));
        }};

// Inline zero-or-one polymorphic storage. Moving relocates the payload
// through its own move constructor, never copies a polymorphic object's
// bytes, and recomputes the cached interface from the destination address.
// The cache only avoids an indirect call per read; it is never moved.
template <typename Interface, std::size_t Bytes, std::size_t Align>
class polymorphic_storage {
public:
  polymorphic_storage() = default;
  polymorphic_storage(const polymorphic_storage &) = delete;
  polymorphic_storage &operator=(const polymorphic_storage &) = delete;
  polymorphic_storage(polymorphic_storage &&other) noexcept { take(other); }
  polymorphic_storage &operator=(polymorphic_storage &&other) noexcept {
    if (this != &other) {
      reset();
      take(other);
    }
    return *this;
  }
  ~polymorphic_storage() {
    if (!mir_cleanup_emergency_abandonment()) reset();
  }

  [[nodiscard]] bool has_value() const noexcept {
    return interface_ != nullptr;
  }

  template <typename Model> void construct(Model &&value) {
    using Payload = std::remove_cvref_t<Model>;
    // Every concrete payload instantiates this; the library selects the slot
    // only for a payload whose native layout fits, so the other arm is a
    // private-invariant trap rather than invalid native code.
    if constexpr (sizeof(Payload) <= Bytes && alignof(Payload) <= Align) {
      if (operations_ != nullptr) {
        storage_error("constructed an engaged polymorphic storage slot");
      }
      construct_representation_at(reinterpret_cast<Payload *>(bytes_),
                                  std::forward<Model>(value));
      operations_ = &polymorphic_storage_operations_for<Interface, Payload>;
      interface_ = operations_->interface(bytes_);
    } else {
      storage_error("polymorphic storage payload exceeds its inline footprint");
    }
  }

  [[nodiscard]] const Interface &read() const {
    if (interface_ == nullptr) {
      empty_owner_error();
    }
    return *interface_;
  }

  [[nodiscard]] Interface &read_mut() {
    if (interface_ == nullptr) {
      empty_owner_error();
    }
    return *interface_;
  }

  void reset() noexcept {
    if (operations_ != nullptr) {
      const auto *operations = operations_;
      operations_ = nullptr;
      interface_ = nullptr;
      operations->destroy(bytes_);
    }
  }

private:
  void take(polymorphic_storage &other) noexcept {
    if (other.operations_ != nullptr) {
      other.operations_->relocate(bytes_, other.bytes_);
      operations_ = other.operations_;
      interface_ = operations_->interface(bytes_);
      other.operations_ = nullptr;
      other.interface_ = nullptr;
    }
  }

  const polymorphic_storage_operations<Interface> *operations_ = nullptr;
  Interface *interface_ = nullptr;
  alignas(Align) std::byte bytes_[Bytes];
};

template <typename Interface, std::size_t Bytes, std::size_t Align>
inline bool polymorphic_storage_has_value(
    const polymorphic_storage<Interface, Bytes, Align> &value) {
  return value.has_value();
}

template <typename Interface, std::size_t Bytes, std::size_t Align,
          typename Model>
inline void
polymorphic_storage_construct(polymorphic_storage<Interface, Bytes, Align> &value,
                              Model &&payload) {
  value.construct(std::forward<Model>(payload));
}

template <typename Interface, std::size_t Bytes, std::size_t Align>
inline const Interface &polymorphic_storage_read(
    const polymorphic_storage<Interface, Bytes, Align> &value) {
  return value.read();
}

template <typename Interface, std::size_t Bytes, std::size_t Align>
inline Interface &
polymorphic_storage_read_mut(polymorphic_storage<Interface, Bytes, Align> &value) {
  return value.read_mut();
}

template <typename Interface, std::size_t Bytes, std::size_t Align>
inline void
polymorphic_storage_reset(polymorphic_storage<Interface, Bytes, Align> &value) {
  value.reset();
}
)";
  }
  output << R"(

template <typename T>
inline T &owner_access(std::unique_ptr<T> &owner) {
  if (!owner) {
    empty_owner_error();
  }
  return *owner;
}

template <typename T>
inline const T &owner_access(const std::unique_ptr<T> &owner) {
  if (!owner) {
    empty_owner_error();
  }
  return *owner;
}

template <typename T>
inline bool unique_owner_is_null(const std::unique_ptr<T> &owner) noexcept {
  return !owner;
}

template <typename Base, typename Derived>
inline std::unique_ptr<Base>
unique_owner_upcast(std::unique_ptr<Derived> &&owner) noexcept {
  return std::unique_ptr<Base>(std::move(owner));
}

)";
  // C++ resolves a qualified template name where a template is defined, so an
  // uninstantiated generic definition (for example, one retained by a dormant
  // import) needs these names declared. A declaration adds no native header
  // or support; only a concrete instance or verified operation selects the
  // definitions below.
  if (features.sharedOwnerDeclared) {
    output << "template <typename T> class shared_owner;\n";
  }
  if (features.atomicStorageDeclared) {
    output << "template <typename T> class atomic_value;\n";
  }
  if (features.mutexStorageDeclared) {
    output << "template <typename T> class mutex_value;\n";
  }
  if (features.mutexLockTokenDeclared) {
    output << "template <typename T> class mutex_lock_token;\n";
  }
  if (usesAtomics) {
    output << R"(

// Every typed token carries a view pointer into one allocation and shares this
// non-templated block. A polymorphic upcast changes only that token's view; it
// never allocates a second block and never changes the strong count. The block
// intentionally owns no native destructor policy. Verified MIR supplies the
// exact pointee recipe on the last release, including dynamic GTI cleanup.
struct shared_owner_block final {
  std::atomic<std::uint64_t> strong{1};
  void *allocation = nullptr;
};

template <typename T> class shared_owner final {
  template <typename> friend class shared_owner;

  shared_owner_block *state_ = nullptr;
  T *view_ = nullptr;
public:
  shared_owner() noexcept = default;
  shared_owner(const shared_owner &) = delete;
  shared_owner &operator=(const shared_owner &) = delete;
  shared_owner(shared_owner &&other) noexcept
      : state_(std::exchange(other.state_, nullptr)),
        view_(std::exchange(other.view_, nullptr)) {}
  shared_owner &operator=(shared_owner &&other) noexcept {
    if (this == &other) return *this;
    if (state_ != nullptr || view_ != nullptr) std::abort();
    state_ = std::exchange(other.state_, nullptr);
    view_ = std::exchange(other.view_, nullptr);
    return *this;
  }
  ~shared_owner() {
    if (mir_cleanup_emergency_abandonment()) return;
    if (state_ != nullptr || view_ != nullptr) std::abort();
  }
  explicit operator bool() const noexcept { return state_ != nullptr; }
  const T *get() const noexcept { return view_; }
  std::uint64_t use_count() const noexcept {
    return state_ == nullptr ? 0 : state_->strong.load(std::memory_order_relaxed);
  }
  void swap(shared_owner &other) noexcept {
    std::swap(state_, other.state_);
    std::swap(view_, other.view_);
  }
  // Exhaustion leaves the input intact until the MIR allocation detector
  // aborts, without cleanup. Ownership detaches only after allocation succeeds.
  static bool promote(std::unique_ptr<T> &source,
                      shared_owner &destination) noexcept {
    if (destination.state_ != nullptr || destination.view_ != nullptr)
      std::abort();
    if (!source) return true;
    shared_owner_block *fresh = new (std::nothrow) shared_owner_block;
    if (fresh == nullptr) return false;
    T *value = source.release();
    fresh->allocation = value;
    destination.state_ = fresh;
    destination.view_ = value;
    return true;
  }
  bool clone_into(shared_owner &destination) const noexcept {
    if (destination.state_ != nullptr || destination.view_ != nullptr)
      std::abort();
    if (state_ == nullptr) return true;
    auto count = state_->strong.load(std::memory_order_relaxed);
    do {
      if (count == std::numeric_limits<std::uint64_t>::max())
        return false;
    } while (!state_->strong.compare_exchange_weak(
        count, count + 1, std::memory_order_relaxed, std::memory_order_relaxed));
    destination.state_ = state_;
    destination.view_ = view_;
    return true;
  }
  template <typename Derived>
  static shared_owner upcast_from(shared_owner<Derived> &&source) noexcept {
    shared_owner destination;
    destination.state_ = std::exchange(source.state_, nullptr);
    destination.view_ = static_cast<T *>(source.view_);
    source.view_ = nullptr;
    return destination;
  }
  template <typename Cleanup>
  bool release_with_failure(::gti_failure_record_v2 *record, bool unwinding,
                            Cleanup cleanup) {
    shared_owner_block *released = std::exchange(state_, nullptr);
    T *released_view = std::exchange(view_, nullptr);
    if (released == nullptr ||
        released->strong.fetch_sub(1, std::memory_order_acq_rel) != 1)
      return true;
    if (released_view == nullptr || released->allocation == nullptr)
      std::abort();
    std::unique_ptr<T> owner(released_view);
    const bool success = cleanup(owner, record, unwinding);
    // A secondary failure terminates at the explicit MIR edge: do not let
    // native deletion run remaining subobjects before that terminal edge.
    if (!success && unwinding) {
      released->allocation = owner.release();
      return false;
    }
    if (owner) std::abort();
    released->allocation = nullptr;
    delete released;
    return success;
  }
};

template <typename T> class atomic_value final {
public:
  explicit atomic_value(T value) noexcept : value_(value) {}
  atomic_value(const atomic_value &) = delete;
  atomic_value &operator=(const atomic_value &) = delete;

  atomic_value(atomic_value &&other) noexcept
      : value_(other.value_.load(std::memory_order_relaxed)) {}

  atomic_value &operator=(atomic_value &&) = delete;

  [[nodiscard]] T load(std::memory_order order) const noexcept {
    return value_.load(order);
  }

  void store(T value, std::memory_order order) const noexcept {
    value_.store(value, order);
  }

  void wait(T old, std::memory_order order) const noexcept {
    value_.wait(old, order);
  }

  void notify_one() const noexcept { value_.notify_one(); }
  void notify_all() const noexcept { value_.notify_all(); }

  [[nodiscard]] T exchange(T value, std::memory_order order) const noexcept {
    return value_.exchange(value, order);
  }

  [[nodiscard]] bool compare_exchange_strong(
      T &observed, T desired, std::memory_order success,
      std::memory_order failure) const noexcept {
    return value_.compare_exchange_strong(observed, desired, success, failure);
  }

private:
  mutable std::atomic<T> value_;
};

template <typename T>
[[nodiscard]] inline bool atomic_compare_exchange(
    const atomic_value<T> &value, T expected, T desired, T &observed,
    std::memory_order success, std::memory_order failure) {
  observed = expected;
  return value.compare_exchange_strong(observed, desired, success, failure);
}

)";
  }
  if (usesMutexes) {
    output << R"(

template <typename T> class mutex_value final {
public:
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "GTI mutex payload relocation must not throw");

  explicit mutex_value(
      const mir_empty_state_constructor_tag_v1 &) noexcept
      : value_(std::nullopt) { mutex_.construct(); }

  explicit mutex_value(T value) noexcept
      : value_(std::in_place, std::move(value)) { mutex_.construct(); }

  template <std::uint64_t Constructor>
  explicit mutex_value(
      const mir_failure_constructor_tag_v1<Constructor> &, T value,
      bool *success, ::gti_failure_record_v2 *) noexcept
      : mutex_value(std::move(value)) {
    if (success == nullptr) {
      std::terminate();
    }
    *success = true;
  }
  mutex_value(const mutex_value &) = delete;
  mutex_value &operator=(const mutex_value &) = delete;

  mutex_value(mutex_value &&other) noexcept
      : value_(std::move(other.value_)) {
    mutex_.construct();
    if (!value_.has_value()) {
      std::terminate();
    }
    other.value_.reset();
  }

  mutex_value &operator=(mutex_value &&other) noexcept {
    if (this == &other || value_.has_value() || !other.value_.has_value()) {
      std::terminate();
    }
    value_.emplace(std::move(*other.value_));
    other.value_.reset();
    return *this;
  }

  void lock() const {
    if (!value_.has_value()) {
      std::terminate();
    }
    mutex_.get().lock();
  }
  void unlock() const noexcept {
    try {
      mutex_.get().unlock();
    } catch (...) {
      std::terminate();
    }
  }

  [[nodiscard]] T &value() const noexcept {
    if (!value_.has_value()) {
      std::terminate();
    }
    return *value_;
  }

  template <typename Cleanup>
  [[nodiscard]] bool destroy_value_with_failure(
      ::gti_failure_record_v2 *record, bool unwinding,
      Cleanup &&cleanup) {
    if (!value_.has_value()) {
      std::terminate();
    }
    bool success = false;
    try {
      success = static_cast<Cleanup &&>(cleanup)(*value_, record, unwinding);
    } catch (...) {
      if (mir_cleanup_emergency_abandonment()) throw;
      std::terminate();
    }
    // A second defined failure is terminal at the calling MIR edge.  Keep the
    // native shell engaged so std::optional teardown cannot run any payload
    // subobjects that remain live after the source cleanup stopped.
    if (!success && unwinding) {
      return false;
    }
    value_.reset();
    return success;
  }

private:
  // A skipped lock Drop on a cleanup emergency leaves the native mutex
  // locked. Its representation must not destroy that mutex during handoff.
  mutable mir_lifetime_slot<std::mutex, true> mutex_;
  mutable std::optional<T> value_;
};

template <typename T> class mutex_lock_token final {
public:
  mutex_lock_token() noexcept = default;
  mutex_lock_token(const mutex_lock_token &) = delete;
  mutex_lock_token &operator=(const mutex_lock_token &) = delete;

  mutex_lock_token(mutex_lock_token &&other) noexcept
      : owner_(std::exchange(other.owner_, nullptr)) {}

  mutex_lock_token &operator=(mutex_lock_token &&) = delete;

  ~mutex_lock_token() {
    if (mir_cleanup_emergency_abandonment()) return;
    if (owner_ != nullptr) {
      std::terminate();
    }
  }

  [[nodiscard]] bool active() const noexcept { return owner_ != nullptr; }
  [[nodiscard]] bool owns(const mutex_value<T> &owner) const noexcept {
    return owner_ == &owner;
  }
  void activate(const mutex_value<T> &owner) noexcept {
    if (owner_ != nullptr) {
      std::terminate();
    }
    owner_ = &owner;
  }
  void release(const mutex_value<T> &owner) noexcept {
    if (!owns(owner)) {
      std::terminate();
    }
    owner_ = nullptr;
  }

private:
  const mutex_value<T> *owner_ = nullptr;
};

template <typename T>
[[nodiscard]] inline std::int32_t
mutex_lock(const mutex_value<T> &storage,
           mutex_lock_token<T> &token) noexcept {
  if (token.active()) {
    std::terminate();
  }
  try {
    storage.lock();
  } catch (const std::system_error &) {
    return INT32_C(1);
  } catch (...) {
    return INT32_C(1);
  }
  token.activate(storage);
  return INT32_C(0);
}

template <typename T>
inline void mutex_unlock(const mutex_value<T> &storage,
                         mutex_lock_token<T> &token) noexcept {
  if (!token.owns(storage)) {
    std::terminate();
  }
  storage.unlock();
  token.release(storage);
}

template <typename T>
[[nodiscard]] inline bool
mutex_guard_active(const mutex_lock_token<T> &token) noexcept {
  return token.active();
}

template <typename T>
[[nodiscard]] inline T &mutex_guard_read(
    const mutex_value<T> &storage,
    const mutex_lock_token<T> &token) noexcept {
  if (!token.owns(storage)) {
    std::terminate();
  }
  return storage.value();
}

)";
  }
  if (usesManagedThreads) {
    output << R"(

class thread_handle final {
public:
  thread_handle() noexcept = default;
  thread_handle(const thread_handle &) = delete;
  thread_handle &operator=(const thread_handle &) = delete;

  thread_handle(thread_handle &&other) noexcept
      : state_(std::exchange(other.state_, nullptr)),
        pending_context_(std::exchange(other.pending_context_, nullptr)),
        pending_take_(std::exchange(other.pending_take_, nullptr)),
        pending_abandon_(std::exchange(other.pending_abandon_, nullptr)) {}

  thread_handle &operator=(thread_handle &&other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (state_ != nullptr || pending_context_ != nullptr) {
      std::terminate();
    }
    state_ = std::exchange(other.state_, nullptr);
    pending_context_ = std::exchange(other.pending_context_, nullptr);
    pending_take_ = std::exchange(other.pending_take_, nullptr);
    pending_abandon_ = std::exchange(other.pending_abandon_, nullptr);
    return *this;
  }

  ~thread_handle() {
    // A typed join's result that the source wrapper did not take is still
    // owned here; release it exactly once.
    abandon_pending_result();
    if (mir_cleanup_emergency_abandonment()) {
      // Preserve mandatory-join ownership until the terminal report. This
      // does not detach or invoke the abandoned source join operation.
      if (state_ != nullptr) quarantine_for_terminal_failure();
      return;
    }
    // The source-defined thread destructor must discharge this obligation.
    // Reaching the representation destructor with a live state would detach
    // ownership from the language's mandatory-join policy.
    if (state_ != nullptr) {
      std::terminate();
    }
  }

  [[nodiscard]] bool joinable() const noexcept { return state_ != nullptr; }
  [[nodiscard]] ::gti_thread_state_v1 **state_address() noexcept {
    return &state_;
  }
  void adopt(::gti_thread_state_v1 *state) noexcept {
    if (state_ != nullptr || state == nullptr) {
      std::terminate();
    }
    state_ = state;
  }
  void quarantine_for_terminal_failure() noexcept {
    ::gti_runtime_detail::quarantine_managed_thread_state(&state_);
  }

  // A successful typed join stores the worker's result here; the take
  // operation then moves it out without default-constructing Result.
  void stash_result(::gti_thread_value_join_result_v2 &joined) noexcept {
    if (pending_context_ != nullptr || joined.result_context == nullptr ||
        joined.take_result == nullptr || joined.abandon_result == nullptr) {
      std::terminate();
    }
    pending_context_ = std::exchange(joined.result_context, nullptr);
    pending_take_ = std::exchange(joined.take_result, nullptr);
    pending_abandon_ = std::exchange(joined.abandon_result, nullptr);
  }

  template <typename Result> Result take_result() {
    if (pending_context_ == nullptr || pending_take_ == nullptr) {
      std::terminate();
    }
    mir_lifetime_slot<Result, true> result;
    void *context = std::exchange(pending_context_, nullptr);
    const auto take = std::exchange(pending_take_, nullptr);
    pending_abandon_ = nullptr;
    take(context, result.construction_address());
    result.mark_constructed();
    return result.take();
  }

private:
  void abandon_pending_result() noexcept {
    if (pending_context_ == nullptr) {
      return;
    }
    void *context = std::exchange(pending_context_, nullptr);
    const auto abandon = std::exchange(pending_abandon_, nullptr);
    pending_take_ = nullptr;
    if (abandon != nullptr) {
      abandon(context);
    }
  }

  ::gti_thread_state_v1 *state_ = nullptr;
  void *pending_context_ = nullptr;
  ::gti_thread_result_take_v1 pending_take_ = nullptr;
  ::gti_thread_result_abandon_v1 pending_abandon_ = nullptr;
};

template <typename Task> struct managed_thread_task_box final {
  Task task;
};

template <typename Task> struct managed_scoped_task_box final {
  mir_lifetime_slot<Task, true> task;
};

template <typename Task, typename Result>
struct managed_thread_value_box final {
  mir_lifetime_slot<Task, true> task;
  mir_lifetime_slot<Result, true> result;

  template <typename Input>
  explicit managed_thread_value_box(Input &&input) {
    task.construct(std::forward<Input>(input));
  }
};

template <std::uint64_t Adapter> struct managed_thread_task_adapter;

[[noreturn]] inline void malformed_thread_runtime_result() noexcept {
  std::terminate();
}

inline bool zero_thread_failure_record(
    const ::gti_failure_record_v2 &record) noexcept {
  if (record.abi_version != 0 || record.code != GTI_FAILURE_CODE_NONE_V1 ||
      record.detail != GTI_FAILURE_DETAIL_NONE_V1 || record.site_index != 0 ||
      record.reserved != 0 || record.message_size != 0 ||
      record.message_flags != 0) {
    return false;
  }
  for (const std::uint8_t byte : record.artifact_identity) {
    if (byte != 0) {
      return false;
    }
  }
  for (const std::uint8_t byte : record.message) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

inline bool zero_thread_cleanup_failure(
    const ::gti_failure_emergency_v2 &failure) noexcept {
  return failure.abi_version == 0 && failure.reserved == 0 &&
         zero_thread_failure_record(failure.primary) &&
         zero_thread_failure_record(failure.secondary);
}

inline bool valid_thread_spawn_status(
    ::gti_thread_spawn_status_v1 status) noexcept {
  return status == GTI_THREAD_SPAWN_OK_V1 ||
         status == GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1 ||
         status == GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1;
}

inline bool valid_thread_join_status(
    ::gti_thread_join_status_v1 status) noexcept {
  return status == GTI_THREAD_JOIN_OK_V1 ||
         status == GTI_THREAD_JOIN_NOT_JOINABLE_V1 ||
         status == GTI_THREAD_JOIN_FAILED_V1 ||
         status == GTI_THREAD_JOIN_WORKER_FAILURE_V1;
}

inline bool valid_thread_join_payload(
    const ::gti_thread_join_result_v2 &result) noexcept {
  if (result.status != GTI_THREAD_JOIN_WORKER_FAILURE_V1) {
    return result.failure_kind == GTI_THREAD_WORKER_FAILURE_NONE_V1 &&
           zero_thread_failure_record(result.failure) &&
           zero_thread_cleanup_failure(result.cleanup_failure) &&
           result.primary_artifact == nullptr &&
           result.secondary_artifact == nullptr;
  }
  if (result.failure_kind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1) {
    return ::gti_rt_failure_validate_v2(&result.failure,
                                      result.primary_artifact) ==
               GTI_FAILURE_REPORT_OK_V1 &&
           zero_thread_cleanup_failure(result.cleanup_failure) &&
           result.secondary_artifact == nullptr;
  }
  if (result.failure_kind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1) {
    return zero_thread_failure_record(result.failure) &&
           result.cleanup_failure.abi_version ==
               GTI_FAILURE_ABI_VERSION_V2 &&
           result.cleanup_failure.reserved == 0 &&
           ::gti_rt_failure_validate_v2(&result.cleanup_failure.primary,
                                       result.primary_artifact) ==
               GTI_FAILURE_REPORT_OK_V1 &&
           ::gti_rt_failure_validate_v2(&result.cleanup_failure.secondary,
                                       result.secondary_artifact) ==
               GTI_FAILURE_REPORT_OK_V1;
  }
  return false;
}

// The verified scope owns every task slot through join-all and reverse cleanup.
// Native thread creation failure cannot destroy a task while earlier workers
// are live. No task box allocation or native RAII destructor runs GTI cleanup.
struct managed_thread_scope_outcome {
  std::int32_t spawn_status;
  ::gti_thread_join_result_v2 worker;
};

template <std::uint64_t... Adapters, typename... Tasks>
inline managed_thread_scope_outcome managed_thread_scope(
    const ::gti_failure_artifact_descriptor_v1 *artifact, Tasks &&...tasks) {
  static_assert(sizeof...(Adapters) == sizeof...(Tasks));
  std::tuple<managed_scoped_task_box<std::remove_cvref_t<Tasks>>...> boxes;
  return [&]<std::size_t... I>(std::index_sequence<I...>) {
    (std::get<I>(boxes).task.construct(std::forward<Tasks>(tasks)), ...);
    const std::array<::gti_thread_scoped_task_v1, sizeof...(Tasks)> entries{{
        {&managed_thread_task_adapter<Adapters>::template entry<std::remove_cvref_t<Tasks>>,
         &std::get<I>(boxes)}...}};
    std::array<::gti_thread_state_v1 *, sizeof...(Tasks)> scratch{};
    const auto joined = ::gti_rt_thread_scope_v1(entries.data(), scratch.data(), entries.size());
    if (joined.abi_version != GTI_THREAD_ABI_VERSION_V1 ||
        !valid_thread_spawn_status(joined.spawn_status) ||
        joined.started > entries.size() ||
        (joined.spawn_status == GTI_THREAD_SPAWN_OK_V1 && joined.started != entries.size()) ||
        joined.worker.abi_version != GTI_THREAD_ABI_VERSION_V2 ||
        joined.worker.reserved != 0 || !valid_thread_join_payload(joined.worker) ||
        (joined.worker.status != GTI_THREAD_JOIN_OK_V1 &&
         joined.worker.status != GTI_THREAD_JOIN_WORKER_FAILURE_V1) ||
        ((joined.worker.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1)
             ? joined.failed_worker >= joined.started
             : joined.failed_worker != UINT64_MAX))
      malformed_thread_runtime_result();
    for (const auto *state : scratch)
      if (state != nullptr) malformed_thread_runtime_result();
    managed_thread_scope_outcome result{static_cast<std::int32_t>(joined.spawn_status), joined.worker};
    if (result.worker.failure_kind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1)
      ::gti_rt_failure_terminate_cleanup_v2(&result.worker.cleanup_failure,
          result.worker.primary_artifact, result.worker.secondary_artifact);

    // All workers have joined. Reuse the ordinary failure-cleanup context:
    // preserve an existing worker/outer failure; otherwise retain the first
    // task-cleanup failure while draining remaining task slots.
    mir_failure_cleanup_scope_v1 cleanup_context;
    if (result.worker.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1)
      cleanup_context.activate(&result.worker.failure, result.worker.primary_artifact);
    auto cleanup_one = [&]<std::size_t Index>() {
      constexpr auto adapter = std::array<std::uint64_t, sizeof...(Adapters)>{Adapters...}[Index];
      ::gti_failure_record_v2 failure{};
      const bool unwinding = mir_incoming_failure_cleanup_v1() != nullptr;
      if (!managed_thread_task_adapter<adapter>::cleanup(
              std::get<Index>(boxes).task, &failure, unwinding)) {
        mir_check_incoming_cleanup_failure_v1(&failure, artifact);
        result.worker.status = GTI_THREAD_JOIN_WORKER_FAILURE_V1;
        result.worker.failure_kind = GTI_THREAD_WORKER_FAILURE_ORDINARY_V1;
        result.worker.failure = failure;
        result.worker.primary_artifact = artifact;
        cleanup_context.activate(&result.worker.failure, result.worker.primary_artifact);
      }
    };
    (cleanup_one.template operator()<sizeof...(I) - 1 - I>(), ...);
    return result;
  }(std::index_sequence_for<Tasks...>{});
}

template <std::uint64_t Adapter, typename Task>
inline std::int32_t managed_thread_spawn(thread_handle &handle, Task &&task) {
  if (handle.joinable()) {
    return static_cast<std::int32_t>(
        GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1);
  }
  using Stored = std::remove_cvref_t<Task>;
  auto *context = new (std::nothrow)
      managed_thread_task_box<Stored>{std::forward<Task>(task)};
  if (context == nullptr) {
    return static_cast<std::int32_t>(
        GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1);
  }
  const ::gti_thread_spawn_result_v1 result = ::gti_rt_thread_spawn_v1(
      &managed_thread_task_adapter<Adapter>::template entry<Stored>, context,
      &managed_thread_task_adapter<Adapter>::template destroy<Stored>);
  if (result.abi_version != GTI_THREAD_ABI_VERSION_V1 ||
      !valid_thread_spawn_status(result.status) ||
      ((result.status == GTI_THREAD_SPAWN_OK_V1) !=
       (result.state != nullptr))) {
    malformed_thread_runtime_result();
  }
  if (result.status == GTI_THREAD_SPAWN_OK_V1) {
    handle.adopt(result.state);
  }
  return static_cast<std::int32_t>(result.status);
}

template <std::uint64_t Adapter, typename Result, typename Task>
inline std::int32_t
managed_thread_spawn_value(thread_handle &handle, Task &&task) {
  if (handle.joinable()) {
    return static_cast<std::int32_t>(
        GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1);
  }
  using Stored = std::remove_cvref_t<Task>;
  using Box = managed_thread_value_box<Stored, Result>;
  auto *context = new (std::nothrow) Box(std::forward<Task>(task));
  if (context == nullptr) {
    return static_cast<std::int32_t>(
        GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1);
  }
  const ::gti_thread_spawn_result_v1 result =
      ::gti_rt_thread_spawn_value_v1(
          &managed_thread_task_adapter<Adapter>::
              template entry_value<Stored, Result>,
          context,
          &managed_thread_task_adapter<Adapter>::
              template complete_task<Stored, Result>,
          &managed_thread_task_adapter<Adapter>::
              template abandon_task<Stored, Result>,
          &managed_thread_task_adapter<Adapter>::
              template take_result<Stored, Result>,
          &managed_thread_task_adapter<Adapter>::
              template abandon_result<Stored, Result>);
  if (result.abi_version != GTI_THREAD_ABI_VERSION_V1 ||
      !valid_thread_spawn_status(result.status) ||
      ((result.status == GTI_THREAD_SPAWN_OK_V1) !=
       (result.state != nullptr))) {
    malformed_thread_runtime_result();
  }
  if (result.status == GTI_THREAD_SPAWN_OK_V1) {
    handle.adopt(result.state);
  }
  return static_cast<std::int32_t>(result.status);
}

inline ::gti_thread_join_result_v2
managed_thread_join(thread_handle &handle) noexcept {
  const bool wasJoinable = handle.joinable();
  const ::gti_thread_join_result_v2 result =
      ::gti_rt_thread_join_v2(handle.state_address());
  const bool isJoinable = handle.joinable();
  if (result.abi_version != GTI_THREAD_ABI_VERSION_V2 ||
      result.reserved != 0 || !valid_thread_join_status(result.status) ||
      !valid_thread_join_payload(result)) {
    malformed_thread_runtime_result();
  }
  if ((!wasJoinable &&
       (result.status != GTI_THREAD_JOIN_NOT_JOINABLE_V1 || isJoinable)) ||
      (wasJoinable &&
       ((result.status == GTI_THREAD_JOIN_NOT_JOINABLE_V1) ||
        ((result.status == GTI_THREAD_JOIN_OK_V1 ||
          result.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1) ==
         isJoinable) ||
        (result.status == GTI_THREAD_JOIN_FAILED_V1 && !isJoinable)))) {
    malformed_thread_runtime_result();
  }
  return result;
}

inline bool valid_thread_value_join_payload(
    const ::gti_thread_value_join_result_v2 &result) noexcept {
  if (!valid_thread_join_payload(result.join)) {
    return false;
  }
  const bool ownsResult = result.result_context != nullptr &&
                          result.take_result != nullptr &&
                          result.abandon_result != nullptr;
  const bool ownsNothing = result.result_context == nullptr &&
                           result.take_result == nullptr &&
                           result.abandon_result == nullptr;
  return result.join.status == GTI_THREAD_JOIN_OK_V1 ? ownsResult
                                                     : ownsNothing;
}

inline ::gti_thread_value_join_result_v2
managed_thread_join_value(thread_handle &handle) noexcept {
  const bool wasJoinable = handle.joinable();
  ::gti_thread_value_join_result_v2 result =
      ::gti_rt_thread_join_value_v2(handle.state_address());
  const bool isJoinable = handle.joinable();
  if (result.join.abi_version != GTI_THREAD_ABI_VERSION_V2 ||
      result.join.reserved != 0 ||
      !valid_thread_join_status(result.join.status) ||
      !valid_thread_value_join_payload(result)) {
    malformed_thread_runtime_result();
  }
  if ((!wasJoinable &&
       (result.join.status != GTI_THREAD_JOIN_NOT_JOINABLE_V1 ||
        isJoinable)) ||
      (wasJoinable &&
       ((result.join.status == GTI_THREAD_JOIN_NOT_JOINABLE_V1) ||
        ((result.join.status == GTI_THREAD_JOIN_OK_V1 ||
          result.join.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1) ==
         isJoinable) ||
        (result.join.status == GTI_THREAD_JOIN_FAILED_V1 && !isJoinable)))) {
    malformed_thread_runtime_result();
  }
  return result;
}

inline void managed_thread_abandon_join_result(
    ::gti_thread_value_join_result_v2 &joined) noexcept {
  if (joined.join.status != GTI_THREAD_JOIN_OK_V1 ||
      joined.result_context == nullptr || joined.take_result == nullptr ||
      joined.abandon_result == nullptr) {
    malformed_thread_runtime_result();
  }
  void *context = std::exchange(joined.result_context, nullptr);
  joined.take_result = nullptr;
  const auto abandon = std::exchange(joined.abandon_result, nullptr);
  abandon(context);
}

inline void
managed_thread_quarantine_for_terminal_failure(thread_handle &handle) noexcept {
  handle.quarantine_for_terminal_failure();
}

)";
  }
  output << R"(

template <typename T>
inline decltype(auto) forward_pack_argument(T &value) noexcept {
  if constexpr (std::is_copy_constructible_v<T>) {
    return static_cast<const T &>(value);
  } else {
    return static_cast<T &&>(value);
  }
}

template <typename T>
inline const std::remove_reference_t<T> &read_only_receiver(T &&value) noexcept {
  return value;
}

template <typename T>
inline std::remove_reference_t<T> &mutable_receiver(T &&value) noexcept {
  return static_cast<std::remove_reference_t<T> &>(value);
}

template <typename T> struct exact_type {};

enum class callable_capability { read_call, mutable_call, once_call };

enum class callable_binding : std::uint8_t {
  value = 1,
  read_reference = 2,
  mutable_reference = 4,
};

template <std::uint8_t Allowed, typename T> struct call_argument {};

template <typename Parameter, typename Argument>
inline constexpr bool exact_call_type_compatible =
    std::is_same_v<Parameter, Argument> ||
    (std::is_pointer_v<Parameter> &&
     std::is_same_v<Argument, std::nullptr_t>) ||
    (std::is_pointer_v<Parameter> && std::is_pointer_v<Argument> &&
     std::is_same_v<std::remove_cv_t<std::remove_pointer_t<Parameter>>,
                    std::remove_cv_t<std::remove_pointer_t<Argument>>> &&
     (std::is_const_v<std::remove_pointer_t<Parameter>> ||
      !std::is_const_v<std::remove_pointer_t<Argument>>));

template <callable_binding Binding, typename T> struct call_parameter {
  template <std::uint8_t Allowed, typename Argument>
    requires((Allowed & static_cast<std::uint8_t>(Binding)) != 0 &&
             exact_call_type_compatible<T, Argument>)
  call_parameter(call_argument<Allowed, Argument>) {}
};

template <callable_capability Capability, typename... Parameters>
struct exact_call {
  exact_call() = default;

  template <typename... Arguments>
    requires(sizeof...(Parameters) == sizeof...(Arguments) &&
             (std::is_constructible_v<Parameters, Arguments> && ...))
  exact_call(const exact_call<Capability, Arguments...> &) {}
};

template <callable_capability Target, callable_capability Source,
          typename... Parameters>
inline exact_call<Target, Parameters...>
retag_call(exact_call<Source, Parameters...>) {
  return {};
}

template <typename Callable, typename... Parameters, typename... Args>
inline decltype(auto)
invoke_read(Callable &&callable,
            exact_call<callable_capability::read_call, Parameters...> selected,
            Args &&...args) {
  if constexpr (requires {
                  __gti_invoke(
                      read_only_receiver(std::forward<Callable>(callable)),
                      selected,
                      std::forward<Args>(args)...);
                }) {
    return __gti_invoke(
        read_only_receiver(std::forward<Callable>(callable)),
        selected, std::forward<Args>(args)...);
  } else {
    return read_only_receiver(std::forward<Callable>(callable))(
        std::forward<Args>(args)...);
  }
}

template <typename Callable, typename... Parameters, typename... Args>
inline decltype(auto) invoke_mutable(
    Callable &&callable,
    exact_call<callable_capability::mutable_call, Parameters...> selected,
    Args &&...args) {
  if constexpr (requires {
                  __gti_invoke(
                      mutable_receiver(std::forward<Callable>(callable)),
                      selected,
                      std::forward<Args>(args)...);
                }) {
    return __gti_invoke(
        mutable_receiver(std::forward<Callable>(callable)),
        selected, std::forward<Args>(args)...);
  } else if constexpr (requires {
                         __gti_invoke(
                             read_only_receiver(
                                 std::forward<Callable>(callable)),
                             retag_call<callable_capability::read_call>(
                                 selected),
                             std::forward<Args>(args)...);
                       }) {
    return __gti_invoke(
        read_only_receiver(std::forward<Callable>(callable)),
        retag_call<callable_capability::read_call>(selected),
        std::forward<Args>(args)...);
  } else {
    return read_only_receiver(std::forward<Callable>(callable))(
        std::forward<Args>(args)...);
  }
}

template <typename Callable, typename... Parameters, typename... Args>
inline decltype(auto)
invoke_once(Callable &&callable,
            exact_call<callable_capability::once_call, Parameters...> selected,
            Args &&...args) {
  if constexpr (requires {
                  __gti_invoke(
                      std::move(callable),
                      selected,
                      std::forward<Args>(args)...);
                }) {
    return __gti_invoke(
        std::move(callable), selected, std::forward<Args>(args)...);
  } else if constexpr (requires {
                         __gti_invoke(
                             mutable_receiver(callable),
                             retag_call<callable_capability::mutable_call>(
                                 selected),
                             std::forward<Args>(args)...);
                       }) {
    return __gti_invoke(
        mutable_receiver(callable),
        retag_call<callable_capability::mutable_call>(selected),
        std::forward<Args>(args)...);
  } else if constexpr (requires {
                         __gti_invoke(
                             read_only_receiver(callable),
                             retag_call<callable_capability::read_call>(
                                 selected),
                             std::forward<Args>(args)...);
                       }) {
    return __gti_invoke(
        read_only_receiver(callable),
        retag_call<callable_capability::read_call>(selected),
        std::forward<Args>(args)...);
  } else {
    return read_only_receiver(callable)(std::forward<Args>(args)...);
  }
}

template <typename Callable, callable_capability Capability,
          typename... Parameters, typename... Args>
inline decltype(auto)
invoke_selected(Callable &&callable,
                exact_call<Capability, Parameters...> selected,
                Args &&...args) {
  if constexpr (Capability == callable_capability::once_call) {
    return __gti_invoke(std::forward<Callable>(callable), selected,
                        std::forward<Args>(args)...);
  } else if constexpr (Capability == callable_capability::mutable_call) {
    return __gti_invoke(mutable_receiver(callable), selected,
                        std::forward<Args>(args)...);
  } else if constexpr (requires {
                         __gti_invoke(read_only_receiver(callable), selected,
                                      std::forward<Args>(args)...);
                       }) {
    return __gti_invoke(read_only_receiver(callable), selected,
                        std::forward<Args>(args)...);
  } else {
    return read_only_receiver(callable)(std::forward<Args>(args)...);
  }
}

)";
  if (features.simd) {
    output << R"(
// A semantic fixed-lane operation. Native vectors are a private lowering
// choice; copied chunks impose neither aliasing nor alignment requirements on
// the source arrays. Other C++20 hosts execute the identical scalar contract.
template <int Operation, typename Value, typename Result, std::size_t Count>
inline void simd_binary(const std::array<Value, Count> &left,
                        const std::array<Value, Count> &right,
                        std::array<Result, Count> &destination) {
  static_assert(std::is_same_v<Value, float> || std::is_same_v<Value, double>);
  static_assert(Count >= 1 && Count <= 64 && Operation >= 0 && Operation <= 5);
  static_assert(std::is_same_v<Result,
                std::conditional_t<(Operation >= 4), bool, Value>>);
  std::size_t index = 0;
#if (defined(__clang__) || defined(__GNUC__)) && !defined(GTI_SIMD_SCALAR_REFERENCE)
  typedef Value __gti_simd_vector __attribute__((vector_size(16)));
  constexpr std::size_t __gti_simd_width = 16 / sizeof(Value);
  for (; index + __gti_simd_width <= Count; index += __gti_simd_width) {
    __gti_simd_vector __gti_simd_left, __gti_simd_right;
    std::memcpy(&__gti_simd_left, left.data() + index, 16);
    std::memcpy(&__gti_simd_right, right.data() + index, 16);
    if constexpr (Operation < 4) {
      __gti_simd_vector __gti_simd_result;
      if constexpr (Operation == 0) __gti_simd_result = __gti_simd_left + __gti_simd_right;
      if constexpr (Operation == 1) __gti_simd_result = __gti_simd_left - __gti_simd_right;
      if constexpr (Operation == 2) __gti_simd_result = __gti_simd_left * __gti_simd_right;
      if constexpr (Operation == 3) __gti_simd_result = __gti_simd_left / __gti_simd_right;
      std::memcpy(destination.data() + index, &__gti_simd_result, 16);
    } else {
      const auto __gti_simd_result = [&] {
        if constexpr (Operation == 4) return __gti_simd_left == __gti_simd_right;
        else return __gti_simd_left < __gti_simd_right;
      }();
      for (std::size_t __gti_simd_lane = 0; __gti_simd_lane < __gti_simd_width; ++__gti_simd_lane)
        destination[index + __gti_simd_lane] = __gti_simd_result[__gti_simd_lane] != 0;
    }
  }
#endif
  for (; index < Count; ++index) {
    if constexpr (Operation == 0) destination[index] = left[index] + right[index];
    if constexpr (Operation == 1) destination[index] = left[index] - right[index];
    if constexpr (Operation == 2) destination[index] = left[index] * right[index];
    if constexpr (Operation == 3) destination[index] = left[index] / right[index];
    if constexpr (Operation == 4) destination[index] = left[index] == right[index];
    if constexpr (Operation == 5) destination[index] = left[index] < right[index];
  }
}

)";
  }
  output << R"(
// std::in_range/cmp_* exclude plain char, but GTI's C char has a selected
// signed or unsigned numeric domain. Normalize only for range comparison.
template <typename T>
using integer_comparison_type = std::conditional_t<std::is_same_v<T, char>,
    std::conditional_t<std::is_signed_v<char>, signed char, unsigned char>, T>;

template <typename Array, typename Index>
inline decltype(auto) array_at(Array &&array, Index index) {
  static_assert(std::is_integral_v<std::remove_cvref_t<Index>>);
  const auto numeric_index = static_cast<integer_comparison_type<Index>>(index);
  if (std::cmp_less(numeric_index, 0) ||
      std::cmp_greater_equal(numeric_index, std::size(array))) {
    array_bounds_error();
  }
  return std::forward<Array>(array)[static_cast<std::size_t>(index)];
}

template <typename Index>
inline char8_t string_view_at(std::string_view value, Index index) {
  static_assert(std::is_integral_v<std::remove_cvref_t<Index>>);
  const auto numeric_index = static_cast<integer_comparison_type<Index>>(index);
  if (std::cmp_less(numeric_index, 0) || std::cmp_greater_equal(numeric_index, value.size())) {
    string_view_bounds_error();
  }
  return static_cast<char8_t>(static_cast<unsigned char>(
      value[static_cast<std::size_t>(index)]));
}

template <typename Target, typename Source>
inline Target numeric_cast(Source value) {
  static_assert(std::is_arithmetic_v<Target> && std::is_arithmetic_v<Source>);
  // Alias/type-parameter calls share this helper with numeric conversions.
  // GTI's only code-unit conversions are lossless char <-> uint8_t; C++'s
  // integer comparison utilities do not admit char8_t.
  if constexpr (std::is_same_v<Target, char8_t> ||
                std::is_same_v<Source, char8_t>) {
    static_assert((std::is_same_v<Target, char8_t> &&
                   std::is_same_v<Source, std::uint8_t>) ||
                  (std::is_same_v<Target, std::uint8_t> &&
                   std::is_same_v<Source, char8_t>));
  } else if constexpr (std::is_integral_v<Target> &&
                std::is_integral_v<Source>) {
    if (!std::in_range<integer_comparison_type<Target>>(
            static_cast<integer_comparison_type<Source>>(value))) {
      numeric_conversion_error();
    }
  } else if constexpr (std::is_integral_v<Target> &&
                       std::is_floating_point_v<Source>) {
    if (!std::isfinite(value)) {
      numeric_conversion_error();
    }
    const Source truncated = std::trunc(value);
    const Source upper =
        std::ldexp(Source{1}, std::numeric_limits<Target>::digits);
    const Source lower = std::is_unsigned_v<Target> ? Source{0} : -upper;
    if (truncated < lower || truncated >= upper) {
      numeric_conversion_error();
    }
  }
  return static_cast<Target>(value);
}

// Exact integer overflow detection shared by every checked, saturating and
// expected-returning integer operation. GCC and Clang lower the builtins to
// the target's overflow flags; the portable comparisons compute the same
// mathematical predicate. `*result` is written only when no overflow occurs
// on the portable path and is unspecified after an overflow on either path.
// Define GTI_PORTABLE_OVERFLOW_CHECKS to select the portable path.
#if !defined(GTI_PORTABLE_OVERFLOW_CHECKS) &&                                  \
    (defined(__GNUC__) || defined(__clang__))
#define GTI_OVERFLOW_BUILTINS_V1 1
#else
#define GTI_OVERFLOW_BUILTINS_V1 0
#endif

template <typename Value>
inline constexpr bool integer_add_overflows(Value left, Value right,
                                            Value *result) noexcept {
  static_assert(std::is_integral_v<Value> && !std::is_same_v<Value, bool>);
#if GTI_OVERFLOW_BUILTINS_V1
  return __builtin_add_overflow(left, right, result);
#else
  bool overflow = false;
  if constexpr (std::is_unsigned_v<Value>) {
    overflow = left > (std::numeric_limits<Value>::max)() - right;
  } else {
    overflow =
        (right > 0 && left > (std::numeric_limits<Value>::max)() - right) ||
        (right < 0 && left < (std::numeric_limits<Value>::min)() - right);
  }
  if (!overflow) {
    *result = static_cast<Value>(left + right);
  }
  return overflow;
#endif
}

template <typename Value>
inline constexpr bool integer_subtract_overflows(Value left, Value right,
                                                 Value *result) noexcept {
  static_assert(std::is_integral_v<Value> && !std::is_same_v<Value, bool>);
#if GTI_OVERFLOW_BUILTINS_V1
  return __builtin_sub_overflow(left, right, result);
#else
  bool overflow = false;
  if constexpr (std::is_unsigned_v<Value>) {
    overflow = left < right;
  } else {
    overflow =
        (right > 0 && left < (std::numeric_limits<Value>::min)() + right) ||
        (right < 0 && left > (std::numeric_limits<Value>::max)() + right);
  }
  if (!overflow) {
    *result = static_cast<Value>(left - right);
  }
  return overflow;
#endif
}

template <typename Value>
inline constexpr bool integer_multiply_overflows(Value left, Value right,
                                                 Value *result) noexcept {
  static_assert(std::is_integral_v<Value> && !std::is_same_v<Value, bool>);
#if GTI_OVERFLOW_BUILTINS_V1
  return __builtin_mul_overflow(left, right, result);
#else
  constexpr Value minimum = (std::numeric_limits<Value>::min)();
  constexpr Value maximum = (std::numeric_limits<Value>::max)();
  bool overflow = false;
  if (left != 0 && right != 0) {
    if constexpr (std::is_unsigned_v<Value>) {
      overflow = left > maximum / right;
    } else if ((left == -1 && right == minimum) ||
               (right == -1 && left == minimum)) {
      overflow = true;
    } else if (left > 0) {
      overflow = right > 0 ? left > maximum / right : right < minimum / left;
    } else {
      overflow = right > 0 ? left < minimum / right : left < maximum / right;
    }
  }
  if (!overflow) {
    *result = static_cast<Value>(left * right);
  }
  return overflow;
#endif
}

template <typename Left, typename Right>
inline auto add(Left left, Right right) {
  using Result = decltype(left + right);
  const Result promoted_left = static_cast<Result>(left);
  const Result promoted_right = static_cast<Result>(right);
  if constexpr (std::is_integral_v<Result>) {
    Result value{};
    if (integer_add_overflows(promoted_left, promoted_right, &value)) {
      integer_domain_error("integer addition overflow");
    }
    return value;
  } else {
    return static_cast<Result>(promoted_left + promoted_right);
  }
}

template <typename Left, typename Right>
inline auto subtract(Left left, Right right) {
  using Result = decltype(left - right);
  const Result promoted_left = static_cast<Result>(left);
  const Result promoted_right = static_cast<Result>(right);
  if constexpr (std::is_integral_v<Result>) {
    Result value{};
    if (integer_subtract_overflows(promoted_left, promoted_right, &value)) {
      integer_domain_error("integer subtraction overflow");
    }
    return value;
  } else {
    return static_cast<Result>(promoted_left - promoted_right);
  }
}

template <typename Left, typename Right>
inline auto multiply(Left left, Right right) {
  using Result = decltype(left * right);
  const Result promoted_left = static_cast<Result>(left);
  const Result promoted_right = static_cast<Result>(right);
  if constexpr (std::is_integral_v<Result>) {
    Result value{};
    if (integer_multiply_overflows(promoted_left, promoted_right, &value)) {
      integer_domain_error("integer multiplication overflow");
    }
    return value;
  } else {
    return static_cast<Result>(promoted_left * promoted_right);
  }
}

template <typename Value>
inline constexpr Value wrapping_add(Value left, Value right) {
  static_assert(std::is_integral_v<Value>);
  using Unsigned = std::make_unsigned_t<Value>;
  const Unsigned result =
      static_cast<Unsigned>(left) + static_cast<Unsigned>(right);
  return std::bit_cast<Value>(result);
}

template <typename Value>
inline constexpr Value wrapping_sub(Value left, Value right) {
  static_assert(std::is_integral_v<Value>);
  using Unsigned = std::make_unsigned_t<Value>;
  const Unsigned result =
      static_cast<Unsigned>(left) - static_cast<Unsigned>(right);
  return std::bit_cast<Value>(result);
}

template <typename Value>
inline constexpr Value wrapping_mul(Value left, Value right) {
  static_assert(std::is_integral_v<Value>);
  using Unsigned = std::make_unsigned_t<Value>;
  const Unsigned result =
      static_cast<Unsigned>(left) * static_cast<Unsigned>(right);
  return std::bit_cast<Value>(result);
}

template <typename Value>
inline constexpr Value saturating_add(Value left, Value right) {
  static_assert(std::is_integral_v<Value>);
  Value value{};
  if (integer_add_overflows(left, right, &value)) {
    if constexpr (std::is_unsigned_v<Value>) {
      return (std::numeric_limits<Value>::max)();
    } else {
      return right > 0 ? (std::numeric_limits<Value>::max)()
                       : (std::numeric_limits<Value>::min)();
    }
  }
  return value;
}

template <typename Value>
inline constexpr Value saturating_sub(Value left, Value right) {
  static_assert(std::is_integral_v<Value>);
  Value value{};
  if (integer_subtract_overflows(left, right, &value)) {
    if constexpr (std::is_unsigned_v<Value>) {
      return (std::numeric_limits<Value>::min)();
    } else {
      return right > 0 ? (std::numeric_limits<Value>::min)()
                       : (std::numeric_limits<Value>::max)();
    }
  }
  return value;
}

template <typename Value>
inline constexpr Value saturating_mul(Value left, Value right) {
  static_assert(std::is_integral_v<Value>);
  Value value{};
  if (integer_multiply_overflows(left, right, &value)) {
    if constexpr (std::is_unsigned_v<Value>) {
      return (std::numeric_limits<Value>::max)();
    } else {
      return (left < 0) != (right < 0) ? (std::numeric_limits<Value>::min)()
                                       : (std::numeric_limits<Value>::max)();
    }
  }
  return value;
}
)";
  output << R"(

template <typename Value>
inline constexpr bool integer_add_overflow_flag(Value left, Value right) {
  static_assert(std::is_integral_v<Value>);
  Value value{};
  return integer_add_overflows(left, right, &value);
}

template <typename Value>
inline constexpr bool integer_subtract_overflow_flag(Value left, Value right) {
  static_assert(std::is_integral_v<Value>);
  Value value{};
  return integer_subtract_overflows(left, right, &value);
}

template <typename Value>
inline constexpr bool integer_multiply_overflow_flag(Value left, Value right) {
  static_assert(std::is_integral_v<Value>);
  Value value{};
  return integer_multiply_overflows(left, right, &value);
}
)";
  output << R"(

template <typename Left, typename Right>
inline auto divide(Left left, Right right) {
  using Result = decltype(left / right);
  const Result promoted_left = static_cast<Result>(left);
  const Result promoted_right = static_cast<Result>(right);
  if constexpr (std::is_integral_v<Result>) {
    if (promoted_right == 0) {
      integer_domain_error("division by zero");
    }
    if constexpr (std::is_signed_v<Result>) {
      if (promoted_left == (std::numeric_limits<Result>::min)() &&
          promoted_right == -1) {
        integer_domain_error("integer division overflow");
      }
    }
  }
  return static_cast<Result>(promoted_left / promoted_right);
}

template <typename Value>
inline auto negate(Value value) {
  using Result = decltype(-value);
  const Result promoted = static_cast<Result>(value);
  if constexpr (std::is_integral_v<Result> && std::is_signed_v<Result>) {
    if (promoted == (std::numeric_limits<Result>::min)()) {
      integer_domain_error("integer negation overflow");
    }
  }
  return static_cast<Result>(-promoted);
}

template <typename Left, typename Right>
inline auto modulo(Left left, Right right) {
  using Result = decltype(left % right);
  const Result promoted_left = static_cast<Result>(left);
  const Result promoted_right = static_cast<Result>(right);
  if (promoted_right == 0) {
    integer_domain_error("modulo by zero");
  }
  if constexpr (std::is_signed_v<Result>) {
    if (promoted_left == (std::numeric_limits<Result>::min)() &&
        promoted_right == -1) {
      return Result{0};
    }
  }
  return static_cast<Result>(promoted_left % promoted_right);
}

template <typename Result, typename Count>
inline std::uint32_t shift_count(Count count) {
  if constexpr (std::is_signed_v<Count>) {
    if (count < 0) {
      integer_domain_error("negative shift count");
    }
  }
  using UnsignedCount = std::make_unsigned_t<Count>;
  using UnsignedResult = std::make_unsigned_t<Result>;
  const auto unsigned_count = static_cast<UnsignedCount>(count);
  if (static_cast<std::uint64_t>(unsigned_count) >=
      std::numeric_limits<UnsignedResult>::digits) {
    integer_domain_error("shift count exceeds operand width");
  }
  return static_cast<std::uint32_t>(unsigned_count);
}

template <typename Left, typename Right>
inline auto shift_left(Left left, Right right) {
  using Result = decltype(+left);
  using UnsignedResult = std::make_unsigned_t<Result>;
  const std::uint32_t count = shift_count<Result>(right);
  const auto bits = static_cast<UnsignedResult>(static_cast<Result>(left));
  const auto shifted = static_cast<UnsignedResult>(bits << count);
  return std::bit_cast<Result>(shifted);
}

template <typename Left, typename Right>
inline auto shift_right(Left left, Right right) {
  using Result = decltype(+left);
  const std::uint32_t count = shift_count<Result>(right);
  return static_cast<Result>(static_cast<Result>(left) >> count);
}

template <typename Target, typename Value>
inline Target &add_assign(Target &target, Value value) {
  target = numeric_cast<Target>(add(target, value));
  return target;
}

template <typename Target, typename Value>
inline Target &subtract_assign(Target &target, Value value) {
  target = numeric_cast<Target>(subtract(target, value));
  return target;
}

template <typename Target, typename Value>
inline Target &multiply_assign(Target &target, Value value) {
  target = numeric_cast<Target>(multiply(target, value));
  return target;
}

template <typename Target, typename Value>
inline Target &divide_assign(Target &target, Value value) {
  target = numeric_cast<Target>(divide(target, value));
  return target;
}

template <typename Target, typename Value>
inline Target &remainder_assign(Target &target, Value value) {
  target = numeric_cast<Target>(modulo(target, value));
  return target;
}

template <typename Target, typename Value>
inline Target &bitwise_and_assign(Target &target, Value value) {
  target = numeric_cast<Target>(target & value);
  return target;
}

template <typename Target, typename Value>
inline Target &bitwise_or_assign(Target &target, Value value) {
  target = numeric_cast<Target>(target | value);
  return target;
}

template <typename Target, typename Value>
inline Target &bitwise_xor_assign(Target &target, Value value) {
  target = numeric_cast<Target>(target ^ value);
  return target;
}

template <typename Target, typename Value>
inline Target &shift_left_assign(Target &target, Value value) {
  target = numeric_cast<Target>(shift_left(target, value));
  return target;
}

template <typename Target, typename Value>
inline Target &shift_right_assign(Target &target, Value value) {
  target = numeric_cast<Target>(shift_right(target, value));
  return target;
}

template <typename Target>
inline Target &pre_increment(Target &target) {
  return add_assign(target, 1);
}

template <typename Target>
inline Target &pre_decrement(Target &target) {
  return subtract_assign(target, 1);
}

template <typename Target>
inline Target post_increment(Target &target) {
  const Target previous = target;
  add_assign(target, 1);
  return previous;
}

template <typename Target>
inline Target post_decrement(Target &target) {
  const Target previous = target;
  subtract_assign(target, 1);
  return previous;
}

} // namespace gti_internal::backend

)";
}

void writeCppCAbiTextViewAdapter(std::ostream &output,
                                 CppSupportPreambleFeatures features) {
  if (features.cAbiTextView) {
    output << R"(
namespace gti_internal::backend {

inline ::gti_c_string_view to_c_string_view(std::string_view value) noexcept {
  return ::gti_c_string_view{value.data(),
                             static_cast<std::uint64_t>(value.size())};
}

} // namespace gti_internal::backend

)";
  }
}

} // namespace lang
