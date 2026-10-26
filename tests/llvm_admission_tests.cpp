#include "gti/frontend.h"
#include "gti/llvm_admission.h"
#include "gti/lowered_program_builder.h"

#include "../src/compiler/llvm/coverage.h"
#include "../src/compiler/llvm/placement.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace lang {
struct MirProgramTestAccess {
  static MirProgram storageInventory(std::vector<MirClassInstance> classes) {
    MirProgram program;
    program.classes = std::move(classes);
    return program;
  }
};
} // namespace lang

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

lang::VerifiedLoweredProgram lower(std::string_view source,
                                   bool withPrelude = false) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto frontend =
      withPrelude
          ? lang::Frontend().analyze("llvm-admission.gti", std::string(source),
                                     {root / "stdlib/prelude.gti"}, {},
                                     {root / "stdlib"})
          : lang::Frontend().analyze("llvm-admission.gti", std::string(source));
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

void testHierarchyStorageExclusion() {
  using namespace lang;
  using native_llvm::containsHierarchyStorage;
  const auto plain = SemanticType::classType(1);
  const auto polymorphic = SemanticType::classType(2);
  const auto repeated = SemanticType::classType(3);
  const auto wrapper = SemanticType::classType(4);
  const auto derived = SemanticType::classType(5);
  std::vector<MirClassInstance> classes{
      {.id = 1,
       .type = plain,
       .declaredFields = {{.type = SemanticType::Int32}}},
      {.id = 2, .type = polymorphic, .polymorphic = true},
      {.id = 3,
       .type = repeated,
       .declaredFields = {{.type = plain}, {.type = plain}}},
      {.id = 4, .type = wrapper, .declaredFields = {{.type = polymorphic}}},
      {.id = 5,
       .type = derived,
       .bases = {{.instance = 2, .type = polymorphic}}}};
  // Query-only component fixtures never acquire a source seal or become
  // executable backend input; malformed variants below must fail closed.
  const auto program = MirProgramTestAccess::storageInventory(classes);
  expect(!containsHierarchyStorage(program, plain) &&
             !containsHierarchyStorage(program, repeated),
         "repeated acyclic ordinary fields are not recursive hierarchy state");
  expect(containsHierarchyStorage(program, polymorphic) &&
             containsHierarchyStorage(program, wrapper) &&
             containsHierarchyStorage(program, derived),
         "hierarchy exclusion follows inline fields and exact state bases");
  SemanticType array(SemanticType::Array, {wrapper});
  array.arrayLength = 2;
  expect(containsHierarchyStorage(program, array),
         "hierarchy exclusion follows inline array elements");
  expect(!containsHierarchyStorage(
             program,
             SemanticType::referenceTo(polymorphic, AccessMode::ReadOnly)) &&
             !containsHierarchyStorage(program,
                                       SemanticType::rawPointerTo(polymorphic)),
         "borrowed and raw addresses do not copy their referent storage");
  const auto rejects = [&](const MirProgram &input, const SemanticType &type) {
    try {
      static_cast<void>(containsHierarchyStorage(input, type));
      return false;
    } catch (const BackendUnsupportedError &) {
      return true;
    }
  };
  expect(rejects(program, SemanticType::Unknown) &&
             rejects(program, SemanticType::typeParameter(1)) &&
             rejects(program, SemanticType::classType(99)),
         "unknown and missing storage never grant object-byte transport");
  auto wrongBase = classes;
  wrongBase.back().bases.front().type = plain;
  expect(rejects(MirProgramTestAccess::storageInventory(wrongBase), derived),
         "base identity and concrete type must agree even after finding state");
  wrongBase.back().bases.front() = {.instance = 99, .type = polymorphic};
  expect(rejects(MirProgramTestAccess::storageInventory(wrongBase), derived),
         "missing base identities never grant object-byte transport");
  wrongBase.back().bases.front() = {.instance = 5, .type = derived};
  expect(rejects(MirProgramTestAccess::storageInventory(wrongBase), derived),
         "recursive base storage fails closed");
  classes[0].declaredFields.front().type = repeated;
  const auto cyclic = MirProgramTestAccess::storageInventory(classes);
  expect(rejects(cyclic, plain),
         "recursive inline storage rejects without an unbounded walk");
  classes[0].declaredFields.front().type = SemanticType::Unknown;
  const auto unknown = MirProgramTestAccess::storageInventory(classes);
  expect(rejects(unknown, repeated),
         "unknown nested storage cannot be cached as an ordinary sibling");
  const auto sealed = lower(R"(
    struct Plain { int value = 1; };
    class P { public: virtual int read() { return 1; } };
    struct Pair { Plain left = Plain(); Plain right = Plain(); };
    struct Wrapper { P value = P(); };
    int main() { Pair pair{}; Wrapper wrapper{}; return 0; }
  )");
  bool foundPair = false, foundWrapper = false;
  for (const auto &owner : sealed.program().mir().classInstances()) {
    if (owner.declaredFields.size() == 2) {
      foundPair = true;
      expect(!containsHierarchyStorage(sealed.program().mir(), owner.type),
             "source-sealed repeated ordinary subobjects remain outside "
             "hierarchy-storage exclusion");
    }
    if (!owner.polymorphic && owner.declaredFields.size() == 1 &&
        owner.declaredFields[0].type.kind == SemanticType::Class &&
        containsHierarchyStorage(sealed.program().mir(), owner.type))
      foundWrapper = true;
  }
  expect(foundPair && foundWrapper,
         "source-sealed field inventory preserves nested hierarchy exclusion");
  expect(!Frontend()
              .analyze("inline-cycle.gti", R"(
             struct Recursive { Recursive value; };
             int main() { Recursive value{}; return 0; }
           )")
              .canGenerateCode(),
         "cyclic inline source cannot acquire a verified executable seal");
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
    if (entry.state == LlvmCoverageState::Implemented)
      expect(entry.positiveTest ==
                 (entry.family == "Shape" &&
                          (entry.name == "NativeStaticDispatchStorage" ||
                           entry.name == "NativeBorrowedDispatchView" ||
                           entry.name == "NativeVirtualInvocation" ||
                           entry.name == "NativeFreshHierarchyTransfer")
                      ? "llvm_dispatch_execution"
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
unsafe int create() { mut Owner value{}; Owner other{}; value = other; return 0; }
int main() { unsafe { return create(); } }
)"));
  expect(!unsafeOwner.admitted() &&
             has(unsafeOwner, "Shape", "PolymorphicTransfer"),
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

void testDispatchStateDependencies() {
  using namespace lang;
  const auto verified = lower(R"(
int selected_helper() { return 7; }
int unused_helper() { auto call = []() -> int { return 9; }; return call(); }
class Generated {
public:
  virtual int read() { return selected_helper(); }
  int unused() { return unused_helper(); }
};
class Source {
public:
  Source() {}
  virtual int read() { return 8; }
};
class Base {
public:
  Base() {}
  virtual int phase() { return 1; }
};
class Derived : public Base {
public:
  Derived() : Base() {}
  int phase() override { return 2; }
  ~Derived() {}
};
class Dormant { public: virtual int read() { return unused_helper(); } };
class EmptyBase { public: virtual int phase() { return 3; } };
class EmptyLeaf : public EmptyBase {
public:
  EmptyLeaf() : EmptyBase() {}
  int phase() override { return 4; }
};
void dormant(Dormant& value) {}
int main() {
  Generated generated{};
  Source source{};
  Derived derived{};
  EmptyLeaf empty{};
  return 0;
}
)");
  const auto plan = planLlvmAdmission(verified);
  const auto required = [&](MirBodyAddress body) {
    return std::find(plan.requiredBodies().begin(), plan.requiredBodies().end(),
                     body) != plan.requiredBodies().end();
  };
  expect(plan.closure() == LlvmAdmissionClosure::SealedEntryDependencies,
         "no-call dispatch states retain the exact sealed dependency closure");
  unsigned activeClasses = 0;
  for (const auto &object : verified.program().mir().classInstances()) {
    const auto *declaration =
        verified.program().findClassDeclaration(object.declaration);
    if (!declaration)
      continue;
    if (declaration->qualifiedName == "Dormant") {
      for (const auto &selection : object.virtualDispatch)
        expect(!required({MirBodyKind::Function, selection.target}),
               "an unreachable class does not root its virtual implementation");
    } else if (declaration->qualifiedName == "Generated" ||
               declaration->qualifiedName == "Source" ||
               declaration->qualifiedName == "Base" ||
               declaration->qualifiedName == "Derived" ||
               declaration->qualifiedName == "EmptyBase" ||
               declaration->qualifiedName == "EmptyLeaf") {
      ++activeClasses;
      for (const auto &selection : object.virtualDispatch)
        expect(
            required({MirBodyKind::Function, selection.target}),
            "generated/source construction and static cleanup require each "
            "exact base/derived phase implementation without a virtual call");
    }
  }
  expect(activeClasses == 6,
         "all no-call dispatch-state controls are instantiated");
  bool helper = false;
  for (const auto &function : verified.program().mir().functionInstances()) {
    const auto *declaration =
        verified.program().findFunctionDeclaration(function.declaration);
    if (!declaration)
      continue;
    if (declaration->qualifiedName == "selected_helper") {
      helper = true;
      expect(required({MirBodyKind::Function, function.id}),
             "a selected table target retains its transitive ordinary helper");
    }
    if (declaration->qualifiedName == "unused_helper" ||
        declaration->qualifiedName == "Generated::unused" ||
        declaration->qualifiedName == "dormant")
      expect(!required({MirBodyKind::Function, function.id}),
             "dispatch-state demand does not root unrelated class methods or "
             "helpers");
  }
  expect(helper, "the transitive helper has a sealed source instance");
  const auto repeated = planLlvmAdmission(verified);
  expect(std::equal(plan.requiredBodies().begin(), plan.requiredBodies().end(),
                    repeated.requiredBodies().begin(),
                    repeated.requiredBodies().end()),
         "dispatch-state closure is deterministic in sealed inventory order");
  const auto unsupported = planLlvmAdmission(lower(R"(
int unsupported_helper() { auto call = []() -> int { return 9; }; return call(); }
class Value { public: virtual int read() { return unsupported_helper(); } };
int main() { Value value{}; return 0; }
)"));
  expect(unsupported.closure() == LlvmAdmissionClosure::AllExecutableBodies &&
             !unsupported.admitted() &&
             std::any_of(unsupported.requirements().begin(),
                         unsupported.requirements().end(),
                         [](const auto &use) {
                           return use.required &&
                                  use.entry->family == "MirBodyKind" &&
                                  use.entry->name == "Lambda";
                         }),
         "a table target's unsupported helper retains existing unknown-call "
         "conservative closure and its checked requirements");
}

void testConservativeClassWitnesses() {
  using namespace lang;
  const auto classesProved = [](const LlvmAdmissionPlan &plan) {
    bool found = false;
    for (const auto &use : plan.requirements())
      if (use.required && use.entry->family == "ClassKind" &&
          use.entry->name == "Class") {
        found = true;
        if (!use.implementedShape ||
            use.implementedShape->name != "NativeClassStorage" ||
            use.entry->state != LlvmCoverageState::Pending)
          return false;
      }
    return found;
  };
  const auto ordinary = planLlvmAdmission(lower(R"(
class Value { int data; public: Value(int input):data(input){} int read(){return this.data;} };
int inspect(int input) { Value value{input}; return value.read(); }
)"));
  expect(ordinary.admitted() && classesProved(ordinary) &&
             ordinary.closure() == LlvmAdmissionClosure::AllExecutableBodies,
         "ordinary rootless inventory uses a per-use class proof without "
         "omission or family promotion");
  expect(std::any_of(ordinary.requiredBodies().begin(),
                     ordinary.requiredBodies().end(),
                     [](auto address) {
                       return address.kind == MirBodyKind::FieldInitializers;
                     }),
         "conservative class admission retains field declaration bodies");
  const auto body = planLlvmAdmission(lower(R"(
class Value {
  int data = 1;
public:
  int unsupported() { auto call = [](int value)->int{return value;}; return call(this.data); }
};
int inspect(Value& value) { return value.unsupported(); }
)"));
  expect(!body.admitted() && classesProved(body) && body.firstUnsupported() &&
             body.firstUnsupported()->entry->family != "ClassKind" &&
             std::any_of(body.requirements().begin(), body.requirements().end(),
                         [](const auto &use) {
                           return use.required && use.body &&
                                  use.body->kind == MirBodyKind::Function &&
                                  use.entry->state ==
                                      LlvmCoverageState::Pending &&
                                  !use.implementedShape;
                         }),
         "representable class storage cannot hide a required unsupported "
         "method body");
  const auto cleanup = planLlvmAdmission(lower(R"(
int unsupported(int input) { auto call=[](int value)->int{return value;}; return call(input); }
class Value {
public:
  Value() {}
  ~Value() { int observed = unsupported(1); }
};
void inspect() { Value value{}; }
)"));
  expect(!cleanup.admitted() && classesProved(cleanup) &&
             std::any_of(cleanup.requiredBodies().begin(),
                         cleanup.requiredBodies().end(),
                         [](auto address) {
                           return address.kind == MirBodyKind::Destructor;
                         }) &&
             cleanup.firstUnsupported() &&
             cleanup.firstUnsupported()->entry->family != "ClassKind",
         "source cleanup and its unsupported call dependency stay required "
         "after class storage is proved");
  const auto itemCall = planLlvmAdmission(lower(R"(
int identity(int value) { return value; }
class Value { public: int read(){auto callback=identity; return callback(1);} };
int inspect(Value& value) { return value.read(); }
)"));
  expect(!itemCall.admitted() && classesProved(itemCall) &&
             itemCall.firstUnsupported() &&
             itemCall.firstUnsupported()->entry->family != "ClassKind",
         "required function-item creation/call capabilities cannot hide behind "
         "class storage");
  for (
      const auto source :
      {R"(interface Empty {}; void accept(Empty& value) {})",
       R"(class Base {public: Base(){}}; class Derived:public Base {public: Derived():Base(){}}; void accept(Derived& value){})"}) {
    const auto borrowed = planLlvmAdmission(lower(source));
    expect(borrowed.admitted() &&
               std::any_of(borrowed.requirements().begin(),
                           borrowed.requirements().end(),
                           [](const auto &use) {
                             return use.required && use.implementedShape &&
                                    (use.implementedShape->name ==
                                         "NativeBorrowedDispatchView" ||
                                     use.implementedShape->name ==
                                         "NativeStaticDispatchStorage");
                           }),
           "exact interface and state-base metadata has bounded public "
           "representation witnesses");
  }
  for (
      const auto source :
      {R"(class Poly {public: virtual int read(){return 1;}}; class Outer {public: Poly value;}; void accept(Outer& value){})",
       R"(enum class Choice { empty, held(int value) }; class Outer {public: Choice value=Choice::empty;}; void accept(Outer& value){})",
       R"(union Payload { int value; bool flag; }; void accept(Payload& value){})"}) {
    const auto excluded = planLlvmAdmission(lower(source));
    expect(!excluded.admitted() && std::any_of(excluded.requirements().begin(),
                                               excluded.requirements().end(),
                                               [](const auto &use) {
                                                 return use.required &&
                                                        use.entry->family ==
                                                            "ClassKind" &&
                                                        !use.implementedShape;
                                               }),
           "nested polymorphic, payload and union classes retain "
           "unproved class uses");
  }
  const auto nestedForeign = planLlvmAdmission(lower(R"(
[[c_abi]] struct Record { int value; };
class Outer { public: Record value; Outer(Record input) : value(input) {} };
void accept(Outer& value) {}
)"));
  expect(!nestedForeign.admitted() &&
             std::any_of(nestedForeign.requirements().begin(),
                         nestedForeign.requirements().end(),
                         [](const auto &use) {
                           return use.required &&
                                  use.entry->key() == "ClassKind.Struct" &&
                                  !use.implementedShape;
                         }),
         "an ordinary containing class cannot bypass nested foreign-layout "
         "inventory");
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto owned =
      Frontend().analyze(root / "llvm-owned-field-control.gti", R"(
#include <std/memory>
class Outer {
  std::unique_ptr<int> owner = std::unique_ptr<int>(nullptr);
public:
  Outer() : owner(std::unique_ptr<int>(nullptr)) {}
};
void accept(Outer& value) {}
)",
                         {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  expect(owned.canGenerateCode(),
         "ownership-field exclusion uses valid source-defined policy");
  if (owned.canGenerateCode()) {
    const auto built = LoweredProgramBuilder().build(
        owned.program, owned.semantics, owned.hir, owned.mir, owned.mir, {},
        TargetInfo::host(), true);
    std::vector<LoweredProgramIssue> issues;
    const auto verified = built.valid()
                              ? verifiedLoweredProgram(*built.program, issues)
                              : std::nullopt;
    expect(verified.has_value(),
           "ownership-field control retains its exact source seal");
    if (verified) {
      const auto ownerPlan = planLlvmAdmission(*verified);
      expect(!ownerPlan.admitted() &&
                 std::any_of(ownerPlan.requirements().begin(),
                             ownerPlan.requirements().end(),
                             [](const auto &use) {
                               return use.required &&
                                      use.entry->key() ==
                                          "SemanticType::Kind.UniqueOwner";
                             }),
             "recursive ordinary storage proof cannot admit a private owning "
             "field");
    }
  }
  ProgramRequest request{.kind = ProgramKind::Library};
  request.exports.push_back({.function = "inspect", .symbol = "gti_inspect"});
  const auto frontend =
      Frontend({.program = std::move(request)})
          .analyze("llvm-conservative-library.gti",
                   "int inspect(int value) { return value; }");
  expect(frontend.canGenerateCode(),
         "library negative control has valid semantic export facts");
  if (frontend.canGenerateCode()) {
    const auto built = LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, frontend.mir,
        frontend.mir, {}, TargetInfo::host(), true);
    std::vector<LoweredProgramIssue> issues;
    const auto verified = built.valid()
                              ? verifiedLoweredProgram(*built.program, issues)
                              : std::nullopt;
    expect(verified.has_value(),
           "library negative retains its exact source seal");
    if (verified) {
      const auto library = planLlvmAdmission(*verified);
      expect(!library.admitted() && has(library, "ProgramKind", "Library") &&
                 has(library, "Shape", "EmbeddingExports"),
             "rootless ordinary object storage does not admit library/export "
             "program kinds");
      try {
        static_cast<void>(LlvmBackend().generate(*built.program));
        expect(false, "pending library/export plans cannot return a public "
                      "object artifact");
      } catch (const BackendUnsupportedError &) {
      }
    }
  }
}

void testDispatchPerUseWitnesses() {
  using namespace lang;
  const auto input = lower(R"(
    class P {
    public:
      mut int value;
      P() : value(1) {}
      virtual int read() { return this.value; }
    };
    void take(P value) {}
    int main() {
      mut P first{};
      P second{};
      first.value = 5;
      P copied = P(first);
      take(P());
      first = second;
      return copied.read();
    }
  )");
  const auto plan = planLlvmAdmission(input);
  bool goodConstruct = false, goodFreshCopy = false, badTransfer = false,
       badConstruct = false;
  for (const auto &use : plan.requirements()) {
    if (!use.required)
      continue;
    if (use.entry->key() == "MirInstructionKind.Construct") {
      goodConstruct |=
          use.implementedShape &&
          use.implementedShape->name == "NativeStaticDispatchStorage";
      goodFreshCopy |=
          use.implementedShape &&
          use.implementedShape->name == "NativeFreshHierarchyTransfer";
      badConstruct |= !use.implementedShape;
    }
    badTransfer |= use.entry->key() == "Shape.PolymorphicTransfer" &&
                   !use.implementedShape && use.instruction != 0;
  }
  expect(!plan.admitted() && goodConstruct && goodFreshCopy && badConstruct &&
             badTransfer,
         "fresh-local copy and prepared by-value construction in one body keep "
         "distinct witnesses and explicit pending transfer requirements");
  const auto copyInput = lower(R"(
    class P {
    public:
      int value;
      P(int input) : value(input) {}
      virtual int read() { return this.value; }
    };
    int main() {
      P original{7};
      P explicit_copy{original};
      P implicit_copy = original;
      return original.read() + explicit_copy.read() + implicit_copy.read() - 21;
    }
  )");
  const auto copyPlan = planLlvmAdmission(copyInput);
  bool explicitCopy = false, implicitCopy = false;
  for (const auto &use : copyPlan.requirements())
    if (use.required && use.implementedShape &&
        use.implementedShape->name == "NativeFreshHierarchyTransfer") {
      explicitCopy |= use.entry->key() == "MirInstructionKind.Construct";
      implicitCopy |= use.entry->key() == "MirInstructionKind.Load";
    }
  expect(copyPlan.admitted() && explicitCopy && implicitCopy,
         "exact generated Copy Construct and implicit Copy Load into fresh "
         "whole locals have operation-specific admission: " +
             copyPlan.limitation());
  const auto moveInput = lower(R"(
    mut int destroyed = 0;
    class P {
      int value;
    public:
      P(int input) : value(input) {}
      virtual int read() { return this.value; }
      ~P() { destroyed += 1; }
    };
    int main() {
      {
        P original{7};
        P transferred = std::move(original);
        if (transferred.read() != 7) { return 1; }
      }
      return destroyed == 1 ? 0 : 2;
    }
  )",
                               true);
  const auto movePlan = planLlvmAdmission(moveInput);
  if (!movePlan.admitted()) {
    std::size_t reported = 0;
    for (const auto &use : movePlan.requirements()) {
      if (!use.required || use.implementedShape ||
          use.entry->key() != "SemanticType::Kind.Class" || reported >= 8)
        continue;
      ++reported;
      std::cerr << "fresh-move-unwitnessed-class: body=";
      if (use.body)
        std::cerr << static_cast<int>(use.body->kind) << '/' << use.body->owner;
      else
        std::cerr << "inventory";
      std::cerr << " block=" << use.block << " instruction=" << use.instruction
                << " occurrences=" << use.occurrences << " source="
                << use.source.source << ':' << use.source.line << '@'
                << use.source.start << ".." << use.source.end << '\n';
      for (const auto &owner : moveInput.program().mir().classInstances()) {
        const auto *row = moveInput.program().findClassInstance(owner.id);
        if (!row || row->source != use.source)
          continue;
        const auto *declaration =
            moveInput.program().findClassDeclaration(owner.declaration);
        std::cerr << "fresh-move-class-row: id=" << owner.id
                  << " declaration=" << owner.declaration << " name="
                  << (declaration ? declaration->qualifiedName : "<missing>")
                  << " kind=" << static_cast<int>(owner.kind)
                  << " fields=" << owner.declaredFields.size()
                  << " bases=" << owner.bases.size()
                  << " structural-bases=" << owner.structuralBases.size()
                  << " borrowed=" << owner.traits.containsBorrowedState
                  << " active=" << owner.requiresActiveCleanup
                  << " own-flag=" << owner.requiresActiveDropState << '\n';
      }
    }
  }
  bool wholeMove = false;
  for (const auto &use : movePlan.requirements())
    wholeMove |= use.required &&
                 use.entry->key() == "MirInstructionKind.Move" &&
                 use.implementedShape &&
                 use.implementedShape->name == "NativeFreshHierarchyTransfer";
  expect(movePlan.admitted() && wholeMove,
         "an exact whole-binding Move into its unique fresh local has bounded "
         "admission without by-value or replacement support: " +
             movePlan.limitation());
  const auto mutations = lower(R"(
    class P {
    public:
      mut int value;
      P() : value(1) {}
      virtual int read() { return this.value; }
    };
    int main() {
      mut P first{}; P second{};
      first.value = 5;
      first = second;
      return first.read();
    }
  )");
  const auto mutationPlan = planLlvmAdmission(mutations);
  bool scalarMutation = false, objectMutation = false, objectCopy = false;
  for (const auto address : mutationPlan.requiredBodies()) {
    const auto *body = findMirBody(mutations.program().mir(), address);
    std::size_t transfers = 0;
    for (const auto &use : mutationPlan.requirements())
      if (use.required && use.body == address &&
          use.entry->key() == "Shape.PolymorphicTransfer")
        transfers += use.occurrences;
    for (const auto &block : body->blocks)
      for (const auto &stage : block.instructions) {
        const bool rejected = std::any_of(
            mutationPlan.requirements().begin(),
            mutationPlan.requirements().end(), [&](const auto &use) {
              return use.required && use.body == address &&
                     use.block == block.id && use.instruction == stage.id &&
                     use.entry->key() == "Shape.PolymorphicTransfer";
            });
        if (stage.kind == MirInstructionKind::Load &&
            stage.info.type.kind == SemanticType::Class)
          objectCopy |= rejected;
        if (stage.kind != MirInstructionKind::Assign || !stage.destination)
          continue;
        const auto *place = body->findPlace(*stage.destination);
        if (place->type == SemanticType::Int32)
          scalarMutation |= !rejected;
        if (place->type.kind == SemanticType::Class)
          objectMutation |= transfers == 2;
      }
  }
  expect(scalarMutation && objectCopy && objectMutation,
         "scalar writes remain distinct from the coalesced source copy and "
         "whole-object assignment exclusions in the same body");
  const auto global = planLlvmAdmission(lower(R"(
    class P { public: P() {} virtual int read() { return 1; } };
    P value{};
    int main() { return value.read(); }
  )"));
  expect(!global.admitted() &&
             std::any_of(
                 global.requirements().begin(), global.requirements().end(),
                 [](const auto &use) {
                   return use.required &&
                          use.entry->key() == "Shape.PolymorphicTransfer" &&
                          !use.implementedShape;
                 }),
         "nonlocal storage keeps an explicit transfer exclusion even when an "
         "unsupported global receiver has the preferred diagnostic span");
}

} // namespace

int main() {
  try {
    testLedger();
    testHierarchyStorageExclusion();
    testSnapshotAndWholeProgram();
    testShapeWitnesses();
    testDispatchStateDependencies();
    testConservativeClassWitnesses();
    testDispatchPerUseWitnesses();
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    ++failures;
  }
  if (failures)
    return 1;
  std::cout << "LLVM coverage and admission tests passed\n";
}
