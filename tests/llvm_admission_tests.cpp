#include "gti/frontend.h"
#include "gti/llvm_admission.h"
#include "gti/lowered_program_builder.h"

#include "../src/compiler/llvm/coverage.h"
#include "../src/compiler/llvm/placement.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testPlacementPlan() {
  using namespace lang;
  using native_llvm::Placement;
  // Component inputs exercise representation planning only. Public admission
  // and execution below still require a sealed, verified source program.
  SemanticType object = SemanticType::Unknown;
  object.kind = SemanticType::Class;
  SemanticType array = SemanticType::Unknown;
  array.kind = SemanticType::Array;
  array.arguments = {object};
  array.arrayLength = 1;
  MirBody body;
  body.returnType = object;
  body.places = {
      {.id = 1, .type = array},
      {.id = 2, .type = object},
      {.id = 3,
       .type = SemanticType::referenceTo(object, AccessMode::ReadOnly)}};
  MirBlock block;
  block.id = 1;
  block.instructions = {
      {.id = 1,
       .kind = MirInstructionKind::Construct,
       .result = 1,
       .info = {.type = object}},
      {.id = 2,
       .kind = MirInstructionKind::CallInput,
       .result = 2,
       .operands = {{.kind = MirOperandKind::Value,
                     .value = 1,
                     .type = object}},
       .info = {.type = object}},
      {.id = 3,
       .kind = MirInstructionKind::Compute,
       .result = 3,
       .operands = {{.kind = MirOperandKind::Value,
                     .value = 2,
                     .type = object}},
       .operation = MirOperation::Aggregate,
       .info = {.type = array}},
      {.id = 4,
       .kind = MirInstructionKind::Initialize,
       .destination = 1,
       .operands = {{.kind = MirOperandKind::Value,
                     .value = 3,
                     .type = array}}},
      {.id = 5,
       .kind = MirInstructionKind::Load,
       .result = 5,
       .operands = {{.kind = MirOperandKind::Copy, .place = 1, .type = array}},
       .info = {.type = array}},
      {.id = 6,
       .kind = MirInstructionKind::Call,
       .result = 6,
       .info = {.type = object}},
      {.id = 8,
       .kind = MirInstructionKind::Call,
       .result = 8,
       .intrinsic = IntrinsicKind::Move,
       .info = {.type = object}},
      {.id = 9,
       .kind = MirInstructionKind::Call,
       .result = 9,
       .loan = 1,
       .info = {.type = object}}};
  block.terminator = {.kind = MirTerminatorKind::Return,
                      .value = MirOperand{.kind = MirOperandKind::Value,
                                          .value = 6,
                                          .type = object}};
  body.blocks.push_back(block);
  const Placement placement(body);
  expect(placement.target(1) && placement.target(1)->aggregate == 3 &&
             placement.target(1)->element == 0 && placement.target(3) &&
             placement.target(3)->place == 1,
         "fresh argument staging preserves exact aggregate destination paths");
  expect(placement.target(6) && placement.target(6)->returned,
         "fresh returns use the caller's result destination");
  expect(!placement.fresh(5) && !placement.target(5),
         "explicit array copies never become fresh destination aliases");
  expect(!placement.fresh(8) && !placement.fresh(9),
         "intrinsic transfers and borrowed call results are not fresh objects");
  body.blocks[0].instructions.push_back(
      {.id = 10,
       .kind = MirInstructionKind::Initialize,
       .destination = 3,
       .operands = {
           {.kind = MirOperandKind::Value, .value = 1, .type = object}}});
  const Placement withReference(body);
  expect(withReference.target(1) && withReference.target(1)->aggregate == 3,
         "referent publication cannot replace object storage with a reference "
         "slot");
  body.blocks[0].instructions.push_back(
      {.id = 7,
       .kind = MirInstructionKind::Initialize,
       .destination = 2,
       .operands = {
           {.kind = MirOperandKind::Value, .value = 1, .type = object}}});
  bool rejected = false;
  try {
    const Placement invalid(body);
  } catch (const BackendUnsupportedError &) {
    rejected = true;
  }
  expect(rejected, "conflicting physical destinations fail closed");
}

lang::VerifiedLoweredProgram lower(std::string_view source) {
  const auto frontend =
      lang::Frontend().analyze("llvm-admission.gti", std::string(source));
  if (!frontend.canGenerateCode()) {
    throw std::runtime_error(frontend.diagnostics.empty()
                                 ? "fixture did not pass frontend"
                                 : frontend.diagnostics.front().message);
  }
  auto build = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host(), true);
  if (!build.valid())
    throw std::runtime_error(build.issues.front().detail);
  std::vector<lang::LoweredProgramIssue> issues;
  auto verified =
      lang::verifiedLoweredProgram(std::move(*build.program), issues);
  if (!verified)
    throw std::runtime_error(issues.front().detail);
  return *verified;
}

bool has(const lang::LlvmAdmissionPlan &plan, std::string_view family,
         std::string_view name) {
  return std::any_of(plan.requirements().begin(), plan.requirements().end(),
                     [&](const auto &use) {
                       return use.entry->family == family &&
                              use.entry->name == name;
                     });
}

void testLedger() {
  using namespace lang;
  using llvm_detail::Shape;
  std::set<std::string> keys;
  std::size_t pending = 0;
  std::size_t nonexecuting = 0;
  for (const auto &entry : llvmCoverageLedger()) {
    expect(keys.insert(entry.key()).second, "coverage identities are unique");
    expect(entry.milestone >= 3 && entry.milestone <= 15,
           "each entry has a concrete implementation owner");
    expect(!entry.predicate.empty(),
           "each entry states its admission boundary");
    pending += entry.state == LlvmCoverageState::Pending;
    nonexecuting += entry.state == LlvmCoverageState::VerifiedNonExecuting;
    if (entry.state == LlvmCoverageState::Implemented) {
      const bool hasEvidence = !entry.positiveTest.empty() &&
                               !entry.positiveAssertion.empty() &&
                               entry.positiveTest != "llvm_admission";
      const bool hasGap = !entry.positiveEvidenceGap.empty();
      expect(hasEvidence != hasGap,
             "implemented rows name execution evidence or one explicit gap");
    } else if (entry.state == LlvmCoverageState::Pending) {
      expect(entry.positiveTest.empty() && entry.positiveAssertion.empty() &&
                 entry.positiveEvidenceGap.empty(),
             "pending rows cannot masquerade as accepted execution");
    } else {
      expect(entry.positiveTest == "llvm_admission" &&
                 entry.positiveAssertion == "ledger.nonexecuting" &&
                 entry.positiveEvidenceGap.empty() &&
                 entry.nonExecuting != LlvmNonExecutingRule::None,
             "nonexecuting rows retain structural evidence");
    }
    expect(!entry.negativeTest.empty() && !entry.negativeAssertion.empty(),
           "each row names its negative drift control");
    if (entry.key() == "MirOperation.None")
      expect(entry.state == LlvmCoverageState::VerifiedNonExecuting &&
                 entry.nonExecuting == LlvmNonExecutingRule::VerifiedMetadata,
             "the no-operation marker is structural metadata");
    if (entry.key() == "IntrinsicKind.ExactTypeParameterConstruction")
      expect(entry.state == LlvmCoverageState::VerifiedNonExecuting &&
                 entry.nonExecuting == LlvmNonExecutingRule::MustBeAbsent,
             "the HIR-only exact pack construction cannot reach executable "
             "MIR");
  }
  expect(pending > 250 && nonexecuting > 20,
         "the full input vocabulary is inventoried");

#define LLVM_COVERAGE_BEGIN(Type)                                              \
  {                                                                            \
    using Enum = Type;
#define LLVM_COVERAGE_ENTRY(Name, Milestone, State, Rule, Predicate)           \
  expect(llvm_detail::coverage(Enum::Name).name == #Name,                      \
         "each declared enumerator has a typed positive classification");
#define LLVM_COVERAGE_SENTINEL(Name)                                           \
  try {                                                                        \
    static_cast<void>(llvm_detail::coverage(Enum::Name));                      \
    expect(false, "Count cannot become an executable ledger entry");           \
  } catch (const std::logic_error &) {                                         \
  }
// The unscoped type-kind enum has no valid out-of-range cast; its exhaustive
// switch is the negative gate. Scoped enums have a fixed underlying type.
#define LLVM_COVERAGE_END()                                                    \
  if constexpr (!std::is_convertible_v<Enum, int>) {                           \
    try {                                                                      \
      static_cast<void>(llvm_detail::coverage(static_cast<Enum>(-1)));         \
      expect(false, "unknown vocabulary cannot silently gain coverage");       \
    } catch (const std::logic_error &) {                                       \
    }                                                                          \
  }                                                                            \
  }
#include "../src/compiler/llvm/coverage.def"
#undef LLVM_COVERAGE_BEGIN
#undef LLVM_COVERAGE_ENTRY
#undef LLVM_COVERAGE_SENTINEL
#undef LLVM_COVERAGE_END
}

constexpr std::string_view exactPackConstructionSource = R"(
T pack_construct<T, Args...>(Args... values) { return T(values...); }
int main() { return pack_construct<int>(7) - 7; }
)";

void testNonExecutingEvidence() {
  const auto packed = lower(exactPackConstructionSource);
  std::size_t identities = 0;
  std::size_t exactIntrinsics = 0;
  for (const auto &instance : packed.program().mir().functionInstances()) {
    for (const auto &block : instance.body.blocks) {
      for (const auto &instruction : block.instructions) {
        identities += instruction.operation == lang::MirOperation::Identity;
        exactIntrinsics += instruction.intrinsic ==
                           lang::IntrinsicKind::ExactTypeParameterConstruction;
      }
    }
  }
  const auto plan = lang::planLlvmAdmission(packed);
  expect(identities != 0 && exactIntrinsics == 0 &&
             has(plan, "MirOperation", "Identity") &&
             !has(plan, "IntrinsicKind", "ExactTypeParameterConstruction"),
         "exact pack construction lowers to Identity before LLVM admission");

  auto frontend = lang::Frontend().analyze(
      "llvm-exact-pack-forge.gti", std::string(exactPackConstructionSource));
  expect(frontend.canGenerateCode(),
         "exact pack forge fixture must pass the frontend");
  bool forged = false;
  for (const auto &instance : frontend.mir.functionInstances()) {
    auto *body = lang::findMirBody(frontend.mir,
                                   {lang::MirBodyKind::Function, instance.id});
    if (!body)
      continue;
    for (auto &block : body->blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::Identity)
          continue;
        instruction.intrinsic =
            lang::IntrinsicKind::ExactTypeParameterConstruction;
        forged = true;
        break;
      }
      if (forged)
        break;
    }
    if (forged)
      break;
  }
  expect(forged, "fixture must expose one lowered exact Identity operation");
  auto build = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host(), true);
  expect(build.valid(),
         "the current core MIR verifier delegates this HIR-only absence "
         "boundary to sealed backend admission");
  if (!build.valid())
    return;
  std::vector<lang::LoweredProgramIssue> issues;
  const auto verified =
      lang::verifiedLoweredProgram(std::move(*build.program), issues);
  expect(verified.has_value(),
         "the forged fixture must still pass the current construction seal");
  if (!verified)
    return;
  const auto rejected = lang::planLlvmAdmission(*verified);
  expect(!rejected.admitted() && rejected.firstUnsupported() &&
             rejected.firstUnsupported()->entry->key() ==
                 "IntrinsicKind.ExactTypeParameterConstruction",
         "LLVM admission rejects a forged executable HIR-only intrinsic");
}

void testSnapshotAndWholeProgram() {
  auto verified = lower(R"(
int dormant(int value) { return value * 3; }
class Owned { public: Owned() {} ~Owned() {} };
int main() { Owned value{}; int data[1] = {1}; return data == data ? 0 : 1; }
)");
  const auto *identity = &verified.program();
  const auto plan = lang::planLlvmAdmission(
      verified, {.optimization = lang::LlvmCodegenOptimization::Aggressive,
                 .debugInfo = true});
  expect(&plan.program() == identity,
         "admission retains the exact immutable verified snapshot");
  expect(plan.options().optimization ==
                 lang::LlvmCodegenOptimization::Aggressive &&
             plan.options().debugInfo &&
             plan.program().target() == verified.program().target(),
         "coverage retains target and selected optimization/debug identity");
  expect(!plan.admitted() && plan.firstUnsupported() != nullptr,
         "unsupported reachable storage remains a classified limitation");
  expect(plan.closure() == lang::LlvmAdmissionClosure::SealedEntryDependencies,
         "sealed roots and dependencies determine required execution");
  const auto expected = std::count_if(
      plan.program().bodies().begin(), plan.program().bodies().end(),
      [](const auto &body) {
        return body.role == lang::LoweredBodyRole::SourceExecutable;
      });
  expect(plan.requiredBodies().size() < static_cast<std::size_t>(expected),
         "dormant helpers remain inventoried without requiring execution");
  const auto *first = plan.firstUnsupported();
  expect(first && first->source.source.ends_with("llvm-admission.gti") &&
             first->source.end > first->source.start,
         "unsupported input carries original half-open source provenance");
  const auto repeated = lang::planLlvmAdmission(verified, plan.options());
  expect(repeated.limitation() == plan.limitation() &&
             repeated.requirements().size() == plan.requirements().size(),
         "inventory and first limitation are deterministic");
  expect(plan.limitation().find("O-LLVM-") != std::string::npos && first &&
             plan.limitation().find(first->entry->key()) != std::string::npos,
         "the limitation identifies the exact requirement and its owner");
  lang::LlvmBackend backend(plan.options());
  try {
    testPlacementPlan();
    static_cast<void>(backend.generate(plan.program()));
    expect(false, "unsupported admission cannot produce an artifact");
  } catch (const lang::BackendUnsupportedError &error) {
    expect(error.location() && *error.location() == first->source,
           "generation preserves the admission diagnostic span");
  }
}

void testShapeWitnesses() {
  const auto indirect = lang::planLlvmAdmission(lower(R"(
int dormant(int value) { return value * 3; }
int main() { auto function = [](int value) -> int { return value; }; return function(0); }
)"));
  expect(
      indirect.closure() == lang::LlvmAdmissionClosure::AllExecutableBodies &&
          !indirect.admitted(),
      "unknown callable dispatch conservatively retains all executable bodies");
  const auto arrays = lang::planLlvmAdmission(lower(R"(
int main() { mut int values[2] = {1, 2}; return values[0] - 1; }
)"));
  expect(has(arrays, "Shape", "FixedArray") &&
             has(arrays, "MirProjectionKind", "Index"),
         "arrays retain both type and checked operation requirements");
  expect(arrays.admitted(), "passive arrays satisfy a structural refinement "
                            "without promoting the broad aggregate family");
  const auto arrayEquality = lang::planLlvmAdmission(lower(R"(
int main() { int left[2] = {1, 2}; int right[2] = {1, 2}; return left == right ? 0 : 1; }
)"));
  expect(!arrayEquality.admitted() && arrayEquality.firstUnsupported() &&
             arrayEquality.firstUnsupported()->entry->key() ==
                 "Shape.ArrayValueComparison",
         "scalar equality admission cannot hide an unimplemented array "
         "comparison");
  for (const auto &use : arrays.requirements())
    if (use.entry->family == "MirOperation" && use.entry->name == "Aggregate")
      expect(use.entry->state == lang::LlvmCoverageState::Pending &&
                 use.implementedShape &&
                 use.implementedShape->name == "PassiveArrayValue",
             "a passive-array proof does not claim class aggregate lowering");
  const auto references = lang::planLlvmAdmission(lower(R"(
int& identity(int& value) { return value; }
int main() { int value = 3; int& alias = identity(value); return alias - 3; }
)"));
  expect(has(references, "Shape", "BorrowReturn") &&
             has(references, "MirLoanKind", "Return"),
         "reference results retain address and loan requirements");
  const auto stored = lang::planLlvmAdmission(lower(R"(
mut int visits = 0;
mut int& selected(mut int& input) { visits += 1; return input; }
class View {
  mut int& value;
public:
  View(mut int& input) : value(selected(input)) {}
  void write() mut { this.value = 42; }
};
int main() {
  mut int value = 1;
  { mut View view{value}; view.write(); }
  return value - 42;
}
)"));
  expect(stored.admitted() && has(stored, "MirLoanKind", "Stored"),
         "evaluated stored-reference addresses are admitted through the sealed "
         "loan stage");
  const auto lifecycle = lang::planLlvmAdmission(lower(R"(
class Value { public: Value() {} ~Value() {} int read() { return 1; } };
int main() { Value value{}; return value.read() - 1; }
)"));
  expect(
      has(lifecycle, "Shape", "UserDestructor") &&
          has(lifecycle, "MirInstructionKind", "Construct"),
      "class construction and selected cleanup are independently inventoried");
  expect(lifecycle.admitted(),
         "exact native class witnesses admit selected lifecycle execution");
  expect(std::any_of(lifecycle.requiredBodies().begin(),
                     lifecycle.requiredBodies().end(),
                     [](auto body) {
                       return body.kind == lang::MirBodyKind::Destructor;
                     }),
         "cleanup recipes retain their exact selected destructor body");
  const auto unsafeScalar = lang::planLlvmAdmission(lower(R"(
unsafe int identity(int value) { return value; }
int main() { unsafe { return identity(0); } }
)"));
  expect(unsafeScalar.admitted(),
         "exact unsafe source calls keep ordinary body admission");
  const auto unsafeOwner = lang::planLlvmAdmission(lower(R"(
class Owner { public: virtual int read() { return 0; } };
unsafe int create() { Owner value{}; return 0; }
int main() { unsafe { return create(); } }
)"));
  expect(!unsafeOwner.admitted(),
         "caller permission cannot admit an unsupported source callee body");
  const auto classArray = lang::planLlvmAdmission(lower(R"(
class Owner { public: Owner() {} ~Owner() {} };
int main() { Owner values[2] = {Owner(), Owner()}; return 0; }
)"));
  expect(classArray.admitted(),
         "native class arrays have explicit lifecycle admission");
  for (const auto &use : classArray.requirements())
    if (use.entry->family == "SemanticType::Kind" && use.entry->name == "Array")
      expect(use.implementedShape &&
                 use.implementedShape->name == "NativeClassStorage",
             "class arrays require the class, not primitive-array witness");
  const auto native = lang::planLlvmAdmission(lower(R"(
extern "C" { int foreign_call(int value); }
int main() { return foreign_call(0); }
)"));
  expect(has(native, "Shape", "NativeCall") &&
             has(native, "Shape", "LibraryDeclaration"),
         "ABI declarations are distinguished from source definitions");
  const auto tls = lang::planLlvmAdmission(lower(R"(
thread_local mut int visits = 0;
int main() { visits += 1; return visits - 1; }
)"));
  expect(has(tls, "Shape", "ThreadLocalPassive") &&
             has(tls, "MirInstructionKind", "BeginThreadLocals"),
         "new thread-local storage and lifetime operations cannot drift behind "
         "coverage");
  expect(tls.admitted(), "passive TLS is explicitly admitted");
  const auto pinned = lang::planLlvmAdmission(lower(R"(
class Pinned {
public:
  Pinned() {}
  Pinned(Pinned&) = delete;
  Pinned(Pinned&&) = delete;
};
int main() { Pinned value{}; return 0; }
)"));
  expect(pinned.admitted(), "immovable ordinary objects use final destination "
                            "placement rather than byte transport");
}

} // namespace

int main() {
  try {
    testLedger();
    testNonExecutingEvidence();
    testSnapshotAndWholeProgram();
    testShapeWitnesses();
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    ++failures;
  }
  if (failures)
    return 1;
  std::cout << "LLVM coverage and admission tests passed\n";
}
