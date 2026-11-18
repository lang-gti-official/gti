// Whole-query memo hits are checked against an uncached oracle; forged answers
// fail as integrity errors, and real plan replay preserves ordered use counts.
#include "../../src/compiler/llvm/admission_memo.h"
#include "../lowered_program_test_support.h"
#include "gti/llvm_admission.h"
#include "unit_test.h"

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace lang::native_llvm {
struct AdmissionMemoTestAccess {
  template <class Key, class Answer, class Hash>
  static void corrupt(AdmissionMemo<Key, Answer, Hash> &memo, const Key &key,
                      Answer answer) {
    memo.answers.at(key) = std::move(answer);
  }
};
} // namespace lang::native_llvm

namespace {
using namespace lang;
using namespace lang::native_llvm;
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct ValidationMode {
  bool previous = loweredProgramSealValidation();
  ~ValidationMode() { setLoweredProgramSealValidation(previous); }
};
} // namespace

GTI_UNIT_TEST(llvm_admission_memo) {
  const auto type =
      SemanticType::classType(1, {}, {CompileTimeValue::uint64(1)});
  const auto collision =
      SemanticType::classType(1, {}, {CompileTimeValue::uint64(2)});
  require(type != collision &&
              TypeBucketHash{}(type) == TypeBucketHash{}(collision),
          "memo collision control is not a collision");
  AdmissionMemo<SemanticType, bool, TypeBucketHash> memo("native-value");
  std::size_t calls = 0, hits = 0;
  const auto compute = [&] {
    ++calls;
    return true;
  };
  require(memo.query(type, type, false, &hits, compute), "first query failed");
  require(memo.query(type, type, false, &hits, compute) && calls == 1 &&
              hits == 1,
          "ordinary hit recomputed a whole query");
  require(!memo.query(collision, collision, false, &hits, [] { return false; }),
          "hash collision reused a different type's answer");
  require(memo.query(type, type, true, &hits, compute) && calls == 2,
          "validation did not recompute a memo hit");
  AdmissionMemoTestAccess::corrupt(memo, type, false);
  bool rejected = false;
  try {
    (void)memo.query(type, type, true, &hits, compute);
  } catch (const std::logic_error &error) {
    rejected = std::string(error.what())
                   .find("LLVM admission memo mismatch for native-value") !=
               std::string::npos;
  }
  require(rejected, "forged memo answer was not an integrity failure");

  AdmissionMemo<SemanticType, std::vector<int>, TypeBucketHash> trace(
      "type-walk");
  const auto walk = [] { return std::vector<int>{1, 2, 1, 3}; };
  require(trace.query(type, type, true, nullptr, walk) == walk(),
          "first trace lost ordered repeated entries");
  AdmissionMemoTestAccess::corrupt(trace, type, std::vector<int>{1, 1, 2, 3});
  rejected = false;
  try {
    (void)trace.query(type, type, true, nullptr, walk);
  } catch (const std::logic_error &error) {
    rejected = std::string(error.what()).find("type-walk") != std::string::npos;
  }
  require(rejected, "reordered trace was not an integrity failure");

  AdmissionMemo<SemanticType, bool, TypeBucketHash> failure("failed-query");
  try {
    (void)failure.query(type, type, false, nullptr, []() -> bool {
      throw std::runtime_error("original failure");
    });
  } catch (const std::runtime_error &error) {
    require(std::string(error.what()) == "original failure", "failure changed");
  }
  require(failure.query(type, type, true, nullptr, compute),
          "failed computation populated the memo");
  return 0;
}

GTI_UNIT_TEST(llvm_admission_walk_replay) {
  ValidationMode restore;
  const auto frontend = Frontend().analyze(
      "admission-replay.gti",
      "struct Box { int value; Box(int n) : value(n) {} ~Box() {} }; "
      "int main() { Box a{3}; Box b{4}; return a.value + b.value; }");
  require(frontend.canGenerateCode(), "replay fixture did not compile");
  auto lowered = gti_test::lowerOptimizedProgram(frontend);
  std::vector<LoweredProgramIssue> issues;
  auto verified = verifiedLoweredProgram(lowered, issues);
  require(verified.has_value(), "replay fixture did not verify");
  setLoweredProgramSealValidation(true);
  const auto checked = planLlvmAdmission(*verified);
  require(checked.admitted() && checked.statistics(),
          "replay fixture rejected");
  const auto &counts = *checked.statistics();
  require(counts.nativeValueMemoHits > 0 && counts.typeWalkReplays > 0 &&
              counts.cleanupWalkReplays > 0,
          "fixture did not exercise all three memo tables");
  std::ostringstream expected;
  printLlvmAdmissionPlan(expected, checked);
  setLoweredProgramSealValidation(false);
  std::ostringstream actual;
  printLlvmAdmissionPlan(actual, planLlvmAdmission(*verified));
  require(actual.str() == expected.str(),
          "validation recomputation changed ordered plan uses or counts");
  return 0;
}
