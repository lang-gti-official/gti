// Admission telemetry is observational; dumps retain ordered use identity and
// exact counts, and nested hierarchy scopes report cache hits and rejections.
#include "../../src/compiler/llvm/placement.h"
#include "../lowered_program_test_support.h"
#include "../temporary_directory.h"
#include "gti/llvm_admission.h"
#include "gti/support.h"
#include "unit_test.h"

#include <fstream>
#include <iomanip>
#include <llvm/Support/JSON.h>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct ValidationMode {
  bool previous = lang::loweredProgramSealValidation();
  ~ValidationMode() { lang::setLoweredProgramSealValidation(previous); }
};
} // namespace

GTI_UNIT_TEST(llvm_admission_evidence) {
  ValidationMode restore;
  lang::setLoweredProgramSealValidation(true);
  const auto frontend = lang::Frontend().analyze(
      "admission-evidence.gti",
      "struct Box { int value; Box(int n) : value(n) {} }; "
      "int main() { Box box{3}; return box.value; }");
  require(frontend.canGenerateCode(), "evidence fixture did not compile");
  auto lowered = gti_test::lowerOptimizedProgram(frontend);
  std::vector<lang::LoweredProgramIssue> issues;
  auto verified = lang::verifiedLoweredProgram(lowered, issues);
  require(verified.has_value(), "evidence input did not verify");
  gti_test::TemporaryDirectory temporary("gti-admission-trace");
  const auto tracePath = temporary.root() / "trace.json";
  lang::beginTimeTrace("gti-admission-test");
  const auto measured = lang::planLlvmAdmission(*verified);
  require(lang::endTimeTrace(tracePath.string()), "trace publication failed");
  std::ifstream traceFile(tracePath);
  const std::string traceText((std::istreambuf_iterator<char>(traceFile)), {});
  const auto trace = llvm::json::parse(traceText);
  require(trace && trace->getAsObject(), "invalid trace JSON");
  const auto *events = trace->getAsObject()->getArray("traceEvents");
  require(events != nullptr, "missing trace events");
  for (const char *name :
       {"llvm-admission", "llvm-admission-closure",
        "llvm-admission-dispatch-proofs", "llvm-admission-inventory"}) {
    std::size_t count = 0;
    for (const auto &event : *events) {
      const auto *object = event.getAsObject();
      if (object && object->getString("name") == name &&
          object->getString("ph") == "X")
        ++count;
    }
    require(count == 1, "missing or double-counted admission trace region");
  }
  require(measured.admitted(), "evidence input was rejected");
  require(measured.statistics().has_value(), "validation counters are missing");
  require(measured.statistics()->nativeValueQueries > 0 &&
              measured.statistics()->typeWalks > 0,
          "validation counters did not observe work");
  std::ostringstream expected;
  lang::printLlvmAdmissionPlan(expected, measured);
  require(expected.str().starts_with("llvm-admission-v1\nclosure "),
          "missing dump schema/closure");
  require(expected.str().find("admitted 1\nlimitation \"\"\n") !=
              std::string::npos,
          "missing dump result");
  std::size_t ordinal = 0;
  for (const auto &use : measured.requirements()) {
    const auto start =
        expected.str().find("\nuse " + std::to_string(ordinal++) + " ");
    require(start != std::string::npos, "ordered coverage use missing");
    const auto end = expected.str().find('\n', start + 1);
    const auto row = expected.str().substr(start, end - start);
    require(row.find(use.entry->key()) != std::string::npos,
            "coverage identity missing");
    require(row.find(" occurrences " + std::to_string(use.occurrences) + " ") !=
                std::string::npos,
            "coverage occurrence count changed");
  }
  lang::setLoweredProgramSealValidation(false);
  const auto unmeasured = lang::planLlvmAdmission(*verified);
  require(!unmeasured.statistics(), "ordinary admission collected counters");
  std::ostringstream actual;
  actual << std::hex << std::boolalpha;
  lang::printLlvmAdmissionPlan(actual, unmeasured);
  require(actual.str() == expected.str(),
          "telemetry/stream flags changed the admission dump");
  // A forged unsupported record tests the inspection contract, without
  // depending on a source feature that a future backend may implement.
  auto rejected = lang::planLlvmAdmission(*verified);
  require(!rejected.requirements().empty(), "missing forgery storage");
  const lang::LlvmCoverageEntry unknown{
      .family = "Fixture", .name = "Unknown", .milestone = 7};
  auto &use =
      const_cast<lang::LlvmCoverageUse &>(rejected.requirements().front());
  use.entry = &unknown;
  use.implementedShape = nullptr;
  use.required = true;
  use.source = {"quoted\"source.gti", 19, 25, 3};
  use.occurrences = 17;
  std::ostringstream rejection;
  lang::printLlvmAdmissionPlan(rejection, rejected);
  require(rejection.str().find("use 0 Fixture.Unknown ") != std::string::npos &&
              rejection.str().find(" occurrences 17 ") != std::string::npos &&
              rejection.str().find("source \"quoted\\\"source.gti\" 19 25 3 "
                                   "first-unsupported 1") !=
                  std::string::npos &&
              rejection.str().find("admitted 0\nlimitation \"") !=
                  std::string::npos &&
              rejection.str().find(rejected.limitation()) != std::string::npos,
          "unsupported inspection lost the first use, span, count or reason");
  lang::setLoweredProgramSealValidation(true);
  {
    const lang::MirProgram program;
    lang::native_llvm::HierarchyStorageScope outer(program);
    require(!lang::native_llvm::containsHierarchyStorage(
                program, lang::SemanticType::Int32),
            "scalar exclusion changed");
    lang::native_llvm::HierarchyStorageScope inner(program);
    require(!lang::native_llvm::containsHierarchyStorage(
                program, lang::SemanticType::Int32),
            "cached scalar exclusion changed");
    std::string reason;
    for (int i = 0; i < 2; ++i) {
      bool rejected = false;
      try {
        static_cast<void>(lang::native_llvm::containsHierarchyStorage(
            program, lang::SemanticType::Unknown));
      } catch (const lang::BackendUnsupportedError &failure) {
        rejected = true;
        if (i == 0)
          reason = failure.what();
        else
          require(reason == failure.what(), "cached rejection message changed");
      }
      require(rejected, "unknown storage did not fail closed");
    }
    const auto counts = inner.statistics();
    require(counts.queries == 4 && counts.hits == 2 && counts.rejections == 2,
            "hierarchy cache telemetry lost an invocation");
    require(outer.statistics().queries == counts.queries,
            "nested hierarchy scope did not share counters");
  }
  return 0;
}
