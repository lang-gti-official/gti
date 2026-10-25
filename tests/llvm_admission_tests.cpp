#include "gti/frontend.h"
#include "gti/llvm_admission.h"
#include "gti/llvm_backend.h"
#include "gti/lowered_program_builder.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"

#include "../src/compiler/llvm/coverage.h"
#include "../src/compiler/llvm/placement.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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

void testIntegralEnumReferences() {
  using namespace lang;
  constexpr std::string_view source = R"(
enum class E { first, second };
E& read(E& value) { return value; }
mut E& edit(mut E& value) { return value; }
int main() {
  mut E value = E::first;
  edit(value) = E::second;
  return read(value) == E::second ? 0 : 1;
}
)";
  const auto plan = planLlvmAdmission(lower(source));
  expect(plan.admitted(), "sealed integral enum references are represented");
  unsigned borrows = 0;
  for (const auto &use : plan.requirements())
    if (use.required && use.entry->family == "MirInstructionKind" &&
        use.entry->name == "Borrow") {
      ++borrows;
      expect(use.implementedShape &&
                 use.implementedShape->name == "PassiveReference",
             "enum borrows retain the selected-address witness");
    }
  expect(borrows != 0, "integral enum fixture exercises actual MIR borrows");

  const auto payload = planLlvmAdmission(lower(R"(
enum class E { first, second };
enum Message { item(E value) };
E& read(Message& message) {
  switch (message) { case Message::item(auto& value): return value; }
}
int main() { Message message = Message::item(E::first); return read(message) == E::first ? 0 : 1; }
)"));
  unsigned enumFields = 0;
  for (const auto &use : payload.requirements()) {
    if (!use.required || use.entry->family != "MirInstructionKind" ||
        use.entry->name != "Borrow" || !use.body ||
        use.body->kind != MirBodyKind::Function)
      continue;
    const auto *owner =
        payload.program().mir().findFunctionInstance(use.body->owner);
    const auto *block = owner ? owner->body.findBlock(use.block) : nullptr;
    if (!block)
      continue;
    for (const auto &stage : block->instructions) {
      if (stage.id != use.instruction)
        continue;
      if (payload.program().mir().findIntegralEnum(stage.info.type)) {
        ++enumFields;
        expect(use.implementedShape &&
                   use.implementedShape->name == "PassiveReference",
               "integral enum payload fields have the same address witness");
      }
      if (payload.program().mir().findPayloadEnum(stage.info.type))
        expect(!use.implementedShape ||
                   use.implementedShape->name != "PassiveReference",
               "payload enum itself cannot borrow the scalar enum witness");
    }
  }
  expect(enumFields != 0, "payload regression reaches integral enum borrowing");
  // This asserts only the borrow's witness. O07 payload storage/execution is
  // still independently required by the complete combined execution fixture.

  // The enum's identity must be represented in the sealed integral inventory.
  // Test copies are corrupted before sealing, never accepted mutable inputs.
  const auto frontend =
      Frontend().analyze("enum-reference.gti", std::string(source));
  for (const bool missing : {false, true}) {
    auto mir = frontend.mir;
    auto &enums =
        const_cast<std::vector<MirIntegralEnum> &>(mir.integralEnums());
    expect(enums.size() == 1, "enum identity control has exactly one record");
    if (enums.empty())
      continue;
    if (missing)
      enums.clear();
    else
      enums.front().underlyingType = SemanticType::Unknown;
    auto build = LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, mir, mir, {},
        TargetInfo::host(), true);
    bool rejected = !build.valid();
    if (build.valid()) {
      std::vector<LoweredProgramIssue> issues;
      auto verified = verifiedLoweredProgram(std::move(*build.program), issues);
      rejected = !verified;
      if (verified)
        rejected = !planLlvmAdmission(*verified).admitted();
    }
    expect(
        rejected,
        "missing or malformed integral enum metadata cannot grant admission");
  }

  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  FrontendOptions options;
  options.requireEntryPoint = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto rawFrontend = Frontend(options).analyze(
      root / "tests/fixtures/llvm_enum_raw_borrow.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  expect(rawFrontend.canGenerateCode(),
         "raw enum control is valid source, not a semantic rejection");
  auto rawMir = rawFrontend.mir;
  unsigned marked = 0;
  // Current source staging does not mark restored-loan relays as raw reads.
  // Exercise the latent raw classifier with a conservative effect annotation
  // on an owned MIR copy, then require the complete program to verify and seal.
  // No type, permission, origin, producer or escape fact is changed.
  for (auto &owner : const_cast<std::vector<MirFunctionInstance> &>(
           rawMir.functionInstances()))
    for (auto &block : owner.body.blocks)
      for (auto &stage : block.instructions)
        if (stage.kind == MirInstructionKind::Borrow &&
            stage.operands.size() == 1 &&
            stage.operands[0].kind == MirOperandKind::Loan &&
            rawMir.findIntegralEnum(stage.info.type)) {
          stage.rawMemoryAccess = true;
          ++marked;
        }
  expect(marked != 0, "raw control retains an exact enum Borrow/Loan relay");
  const auto summaries = deriveMirProgramEffectSummaries(rawMir);
  for (std::size_t index = 0; index < summaries.functions.size(); ++index)
    const_cast<MirFunctionInstance &>(rawMir.functionInstances()[index])
        .effectSummary = summaries.functions[index];
  expect(verifyMirProgram(rawMir).valid(),
         "conservative relay effect does not change verified loan authority");
  auto rawBuild = LoweredProgramBuilder().build(
      rawFrontend.program, rawFrontend.semantics, rawFrontend.hir, rawMir,
      rawMir, {}, TargetInfo::host(), true);
  if (!rawBuild.valid())
    throw std::runtime_error(rawBuild.issues.front().detail);
  std::vector<LoweredProgramIssue> issues;
  auto rawVerified =
      verifiedLoweredProgram(std::move(*rawBuild.program), issues);
  if (!rawVerified)
    throw std::runtime_error(issues.front().detail);
  const auto raw = planLlvmAdmission(*rawVerified);
  expect(!raw.admitted(),
         "selected enum references do not grant raw enum access");
  unsigned rawLoans = 0;
  for (const auto &use : raw.requirements()) {
    if (!use.required || use.entry->family != "Shape" ||
        use.entry->name != "RawMemory" || !use.body ||
        use.body->kind != MirBodyKind::Function)
      continue;
    const auto *owner =
        raw.program().mir().findFunctionInstance(use.body->owner);
    const auto *block = owner ? owner->body.findBlock(use.block) : nullptr;
    if (!block)
      continue;
    for (const auto &stage : block->instructions)
      if (stage.id == use.instruction &&
          stage.kind == MirInstructionKind::Borrow &&
          stage.operands.size() == 1 &&
          stage.operands[0].kind == MirOperandKind::Loan) {
        ++rawLoans;
        expect(!use.implementedShape,
               "raw enum Borrow/Loan keeps its independently pending witness");
      }
  }
  expect(rawLoans != 0, "raw enum control exercises Borrow/Loan admission");
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
    if (entry.state == LlvmCoverageState::Implemented)
      expect(entry.positiveTest ==
                 (entry.milestone == 8   ? "llvm_memory_execution"
                  : entry.milestone == 6 ? "llvm_class_execution"
                  : entry.milestone == 5 ? "llvm_place_execution"
                                         : "llvm_scalar_execution"),
             "implemented families name real execution evidence");
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

void testRawFieldWitnesses() {
  const std::string source = R"(
class Slot { public: mut int value = 0; int read() { return this.value; } };
int main() {
  mut Slot object{};
  unsafe { Slot* pointer = &object; pointer->value = 7; return pointer->value - 7; }
}
)";
  const auto plan = lang::planLlvmAdmission(lower(source));
  bool witnessed = false;
  for (const auto &use : plan.requirements())
    if (use.required && use.entry->family == "UnsafeOperationKind" &&
        use.entry->name == "RawMember") {
      witnessed = true;
      expect(use.implementedShape &&
                 use.implementedShape->name == "NativeClassRawAccess",
             "raw field loads/stores require the complete typed field witness");
    }
  expect(plan.admitted() && witnessed,
         "ordinary mutable raw field access is admitted");
  for (const auto &valid :
       {"class Readonly { public: int value = 7; }; int main() { Readonly "
        "object{}; unsafe { const Readonly* pointer = &object; return "
        "pointer->value - 7; } }",
        "class Padded { public: int8_t tag = int8_t(1); double tail = "
        "double(42.5); }; int main() { Padded object{}; unsafe { const Padded* "
        "pointer = &object; return pointer->tail == double(42.5) ? 0 : 1; } }",
        "class Nested { public: mut int values[3] = {1, 2, 3}; }; int main() { "
        "mut Nested object{}; unsafe { Nested* pointer = &object; "
        "pointer->values[1] = 9; return pointer->values[1] - 9; } }",
        "class Parent { public: int value = 7; }; class Child : public Parent "
        "{ public: int extra = 2; }; int main() { Child object{}; unsafe { "
        "const Child* pointer = &object; return pointer->value - 7; } }"}) {
    const auto frontend =
        lang::Frontend().analyze("valid-raw-field.gti", valid);
    expect(frontend.canGenerateCode() &&
               lang::verifyMirProgram(frontend.mir).valid(),
           "raw projection type verification preserves readonly, padded, "
           "nested-array and base fields");
  }
  const auto method = lang::planLlvmAdmission(lower(R"(
class Slot { public: int value = 7; int read() { return this.value; } };
int main() { Slot object{}; unsafe { const Slot* pointer = &object; return pointer->read(); } }
)"));
  bool methodRejected = false;
  for (const auto &use : method.requirements())
    methodRejected |= use.required &&
                      use.entry->family == "UnsafeOperationKind" &&
                      use.entry->name == "RawMember" && !use.implementedShape;
  expect(methodRejected,
         "a supported raw field path does not authorize member invocation");
  const auto unsupported = lang::planLlvmAdmission(lower(R"(
class Dynamic { public: int value = 7; virtual int read() { return this.value; } };
int main() { Dynamic object{}; unsafe { const Dynamic* pointer = &object; return pointer->value; } }
)"));
  bool unsupportedRejected = false;
  for (const auto &use : unsupported.requirements())
    unsupportedRejected |=
        use.required && use.entry->family == "UnsafeOperationKind" &&
        use.entry->name == "RawMember" && !use.implementedShape;
  expect(unsupportedRejected,
         "unknown dynamic owner layout cannot borrow the field witness");
  for (const auto &invalid :
       {"class Slot { public: mut int value = 0; }; int main() { Slot "
        "object{}; unsafe { const Slot* pointer = &object; pointer->value = 1; "
        "} return 0; }",
        "class Slot { public: int value = 0; }; int main() { Slot object{}; "
        "unsafe { const Slot* pointer = &object; return pointer->missing; } }"})
    expect(
        !lang::Frontend()
             .analyze("invalid-raw-field.gti", invalid)
             .canGenerateCode(),
        "read-only writes and nonexistent fields reject at the semantic owner");
  const auto frontend = lang::Frontend().analyze("raw-field.gti", source);
  const auto output =
      std::filesystem::temp_directory_path() /
      ("gti-malformed-field-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  struct RemoveOutput {
    std::filesystem::path path;
    ~RemoveOutput() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } removeOutput{output};
  for (unsigned mutation = 0; mutation < 3; ++mutation) {
    {
      std::ofstream sentinel(output);
      sentinel << "preserved";
    }
    auto mir = frontend.mir;
    bool changed = false;
    std::string location;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             mir.functionInstances()))
      for (auto &place : function.body.places)
        if (!changed &&
            std::any_of(place.projections.begin(), place.projections.end(),
                        [](const auto &projection) {
                          return projection.kind ==
                                 lang::MirProjectionKind::RawDereference;
                        }))
          for (auto &projection : place.projections)
            if (!changed && projection.kind == lang::MirProjectionKind::Field) {
              location = "Function/" + std::to_string(function.id) +
                         " place=" + std::to_string(place.id) +
                         " raw-root=" + std::to_string(place.value) +
                         " field=" + std::to_string(projection.field);
              if (mutation == 0)
                projection.field = 0;
              if (mutation == 1)
                place.type = lang::SemanticType::Int64;
              if (mutation == 2)
                place.value = 0;
              changed = true;
            }
    const auto verification = lang::verifyMirProgram(mir);
    bool rejected = !verification.valid();
    expect(changed && rejected,
           "malformed raw class field is rejected by program verification");
    if (mutation == 1)
      expect(std::any_of(verification.errors.begin(), verification.errors.end(),
                         [](const auto &error) {
                           return error.bodyKind ==
                                      lang::MirBodyKind::Function &&
                                  error.owner != 0 && error.instruction != 0 &&
                                  error.message.find("place 3") !=
                                      std::string::npos &&
                                  error.message.find(
                                      "exact raw field projection type") !=
                                      std::string::npos;
                         }),
             "field-type integrity error retains its body, root producer and "
             "place");
    const auto mirrored = lang::LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, mir, mir, {},
        lang::TargetInfo::host(), true);
    expect(
        !mirrored.valid(),
        "mirrored malformed snapshots cannot enter verified lowered sealing");
    std::string rejectedBy = "MIR verifier";
    if (!rejected) {
      auto build = lang::LoweredProgramBuilder().build(
          frontend.program, frontend.semantics, frontend.hir, mir, mir, {},
          lang::TargetInfo::host(), true);
      rejected = !build.valid();
      rejectedBy = "LoweredProgramBuilder";
      if (!build.valid() && !build.issues.empty())
        rejectedBy += "/" + build.issues[0].detail.substr(0, 300);
      if (build.valid()) {
        std::vector<lang::LoweredProgramIssue> issues;
        auto verified =
            lang::verifiedLoweredProgram(std::move(*build.program), issues);
        rejected = !verified;
        rejectedBy = "lowered verifier";
        if (verified) {
          const auto admission = lang::planLlvmAdmission(*verified);
          rejected = !admission.admitted();
          rejectedBy = "native admission";
          if (const auto *unsupported = admission.firstUnsupported())
            rejectedBy += "/" + unsupported->entry->key();
          bool generated = false;
          try {
            const auto artifact =
                lang::LlvmBackend().generate(verified->program());
            std::ofstream published(output, std::ios::binary);
            published << artifact.contents;
            generated = true;
          } catch (const lang::BackendUnsupportedError &) {
          }
          expect(!generated,
                 "malformed projected type cannot produce native object bytes");
        }
      }
    }
    if (mutation == 1)
      std::cout << "raw field type control: " << location
                << " int32_t->int64_t; rejected by " << rejectedBy << '\n';
    std::ifstream sentinel(output);
    std::string contents{std::istreambuf_iterator<char>(sentinel), {}};
    expect(contents == "preserved",
           "malformed field preflight preserves the existing artifact");
    expect(changed && rejected, "malformed raw field must fail no later than "
                                "native artifact preflight: " +
                                    std::to_string(mutation));
  }
}

} // namespace

int main() {
  try {
    testLedger();
    testSnapshotAndWholeProgram();
    testShapeWitnesses();
    testIntegralEnumReferences();
    testRawFieldWitnesses();
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    ++failures;
  }
  if (failures)
    return 1;
  std::cout << "LLVM coverage and admission tests passed\n";
}
