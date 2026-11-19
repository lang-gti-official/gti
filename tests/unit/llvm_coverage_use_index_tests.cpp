// Stable coverage identities retain old key distinctions and first-use indices;
// validation detects a forged hit and an aliased static-entry identity.
#include "../../src/compiler/llvm/coverage.h"
#include "../../src/compiler/llvm/coverage_use_index.h"
#include "unit_test.h"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace lang::native_llvm {
struct CoverageUseIndexTestAccess {
  static void corrupt(CoverageUseIndex &index,
                      const CoverageUseIndex::Key &key) {
    index.indices.at(key) = 9999;
  }
};
} // namespace lang::native_llvm

namespace {
using namespace lang;
using namespace lang::native_llvm;
using llvm_detail::coverage;
using llvm_detail::Shape;
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
} // namespace

GTI_UNIT_TEST(llvm_coverage_use_index) {
  // Test the actual static entries, not llvmCoverageLedger's copied rows.
  std::map<std::string, const LlvmCoverageEntry *> entries;
#define LLVM_COVERAGE_BEGIN(Type)                                              \
  {                                                                            \
    using Enum = Type;
#define LLVM_COVERAGE_ENTRY(Name, Milestone, Claim, Rule, Predicate)           \
  {                                                                            \
    const auto *entry = &coverage(Enum::Name);                                 \
    require(entry == &coverage(Enum::Name), "coverage entry identity moved");  \
    require(entries.emplace(entry->key(), entry).second,                       \
            "two static entries have the same string identity");               \
  }
#define LLVM_COVERAGE_SENTINEL(Name)
#define LLVM_COVERAGE_END() }
#include "../../src/compiler/llvm/coverage.def"
#undef LLVM_COVERAGE_BEGIN
#undef LLVM_COVERAGE_ENTRY
#undef LLVM_COVERAGE_SENTINEL
#undef LLVM_COVERAGE_END
  require(entries.size() == llvmCoverageLedger().size(),
          "static entry inventory does not match the coverage ledger");

  for (bool validate : {false, true}) {
    CoverageUseIndex index(validate);
    std::vector<std::size_t> occurrences;
    std::vector<CoverageUseIndex::Key> keys;
    const auto *entry = &coverage(SemanticType::Class);
    const auto *shape = &coverage(Shape::NativeClassStorage);
    for (const auto body :
         {std::optional<MirBodyAddress>{},
          std::optional(MirBodyAddress{MirBodyKind::Module, 0}),
          std::optional(MirBodyAddress{MirBodyKind::Function, 0}),
          std::optional(MirBodyAddress{MirBodyKind::Function, 1})})
      for (bool required : {false, true})
        for (bool executable : {false, true})
          for (const auto *implementation :
               {static_cast<const LlvmCoverageEntry *>(nullptr), shape}) {
            const CoverageUseIndex::Key key{entry, body, required, executable,
                                            implementation};
            const auto [slot, first] = index.insert(key, occurrences.size());
            require(first && slot == occurrences.size(),
                    "distinct context reused an earlier coverage record");
            occurrences.push_back(1);
            keys.push_back(key);
          }
    for (std::size_t i = keys.size(); i-- > 0;) {
      const auto [slot, first] = index.insert(keys[i], occurrences.size());
      require(!first && slot == i, "repeat changed the first-use index");
      ++occurrences[slot];
    }
    for (auto count : occurrences)
      require(count == 2, "repeat lost an occurrence");
  }

  CoverageUseIndex checked(true);
  const CoverageUseIndex::Key key{
      &coverage(SemanticType::Int32), {}, true, false, nullptr};
  (void)checked.insert(key, 0);
  CoverageUseIndexTestAccess::corrupt(checked, key);
  bool rejected = false;
  try {
    (void)checked.insert(key, 1);
  } catch (const std::logic_error &error) {
    rejected = std::string(error.what()).find("coverage index mismatch") !=
               std::string::npos;
  }
  require(rejected, "forged coverage index was not an integrity failure");

  CoverageUseIndex aliases(true);
  (void)aliases.insert(key, 0);
  const auto copied = *key.entry;
  auto alias = key;
  alias.entry = &copied;
  rejected = false;
  try {
    (void)aliases.insert(alias, 1);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  require(rejected,
          "duplicate string identity was not caught by old-key check");
  return 0;
}
