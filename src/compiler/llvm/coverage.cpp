#include "coverage.h"

#include <stdexcept>
#include <type_traits>
#include <vector>

namespace lang::llvm_detail {

namespace {
constexpr bool dispatchExecutionWitness(std::string_view family,
                                        std::string_view name) {
  return family == "Shape" && (name == "NativeStaticDispatchStorage" ||
                               name == "NativeBorrowedDispatchView" ||
                               name == "NativeFreshHierarchyTransfer" ||
                               name == "NativeVirtualInvocation");
}
} // namespace

static_assert(
    std::is_same_v<Literal, std::variant<std::monostate, std::nullptr_t,
                                         std::uint64_t, BinaryFloat,
                                         CharacterLiteral, std::string, bool>>,
    "review LLVM literal coverage when the contract changes");
static_assert(std::variant_size_v<LoweredDeclarationPayload> == 11,
              "review LLVM declaration-payload coverage");
static_assert(std::variant_size_v<LoweredGeneratedItemPayload> == 7,
              "review LLVM generated-payload coverage");

// A new field can introduce an executable shape without extending an enum.
// Force that change through the admission review as well.
#define LLVM_INPUT_FIELDS(Type, ...)                                           \
  [[maybe_unused]] void checkInputFields(Type &inputForCoverage) {             \
    [[maybe_unused]] auto &[__VA_ARGS__] = inputForCoverage;                   \
  }
#include "input_fields.def"
#undef LLVM_INPUT_FIELDS

// Unlike an enum-count comparison this also catches insertions into enums
// without a Count sentinel. Keep this diagnostic local to the ledger switches.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wswitch"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
#endif

#define LLVM_COVERAGE_BEGIN(Type)                                              \
  const LlvmCoverageEntry &coverage(Type value) {                              \
    using Enum = Type;                                                         \
    constexpr std::string_view family = #Type;                                 \
    switch (value) {
#define LLVM_COVERAGE_ENTRY(Name, Milestone, State, Rule, Predicate)           \
  case Enum::Name: {                                                           \
    static constexpr LlvmCoverageEntry entry{                                  \
        family,                                                                \
        #Name,                                                                 \
        Milestone,                                                             \
        LlvmCoverageState::State,                                              \
        LlvmNonExecutingRule::Rule,                                            \
        Predicate,                                                             \
        LlvmCoverageState::State == LlvmCoverageState::Implemented             \
            ? (dispatchExecutionWitness(family, #Name)                         \
                   ? "llvm_dispatch_execution"                                 \
               : Milestone == 6 ? "llvm_class_execution"                       \
               : Milestone == 5 ? "llvm_place_execution"                       \
                                : "llvm_scalar_execution")                     \
            : "llvm_admission"};                                               \
    return entry;                                                              \
  }
#define LLVM_COVERAGE_SENTINEL(Name)                                           \
  case Enum::Name:                                                             \
    break;
#define LLVM_COVERAGE_END()                                                    \
  }                                                                            \
  throw std::logic_error("invalid or unclassified LLVM coverage value");       \
  }
#include "coverage.def"
#undef LLVM_COVERAGE_BEGIN
#undef LLVM_COVERAGE_ENTRY
#undef LLVM_COVERAGE_SENTINEL
#undef LLVM_COVERAGE_END

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

} // namespace lang::llvm_detail

namespace lang {

std::span<const LlvmCoverageEntry> llvmCoverageLedger() {
  static const std::vector<LlvmCoverageEntry> entries = [] {
    std::vector<LlvmCoverageEntry> rows;
#define LLVM_COVERAGE_BEGIN(Type)                                              \
  {                                                                            \
    using Enum = Type;
#define LLVM_COVERAGE_ENTRY(Name, Milestone, State, Rule, Predicate)           \
  rows.push_back(llvm_detail::coverage(Enum::Name));
#define LLVM_COVERAGE_SENTINEL(Name)
#define LLVM_COVERAGE_END() }
    using llvm_detail::Shape;
#include "coverage.def"
#undef LLVM_COVERAGE_BEGIN
#undef LLVM_COVERAGE_ENTRY
#undef LLVM_COVERAGE_SENTINEL
#undef LLVM_COVERAGE_END
    return rows;
  }();
  return entries;
}

} // namespace lang
