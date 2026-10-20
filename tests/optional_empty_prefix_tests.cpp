#include "cpp_mir_body_emitter_test_fixture.h"

void checkContainingClasses() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/constructor_nonassignable_fields.gti";
  std::ifstream stream(path);
  const std::string source =
      std::string{std::istreambuf_iterator<char>(stream), {}} + R"gti(
#include <std/mutex>
class MutexSlot {
  std::mutex<std::string> value;
public:
  MutexSlot() : value(std::mutex<std::string>(std::string("value"))) {}
};
class MutexHolder {
  MutexSlot slot;
public:
  MutexHolder() : slot(MutexSlot()) {}
};
class MutexOuter {
  MutexHolder holder;
public:
  MutexOuter() : holder(MutexHolder()) {}
};
void nonassignable_control() { MutexOuter outer{}; }
)gti";
  auto target = lang::TargetInfo::host();
  target.executionProfile = lang::ExecutionProfile::Concurrent;
  target.runtimeCapabilities.threads = true;
  const auto standardLibrary =
      path.parent_path().parent_path().parent_path() / "stdlib";
  const auto frontend =
      lang::Frontend({.target = target})
          .analyze(path.string(), source, {standardLibrary / "prelude.gti"}, {},
                   {standardLibrary});
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "nonassignable constructor fields have valid MIR");
  if (!frontend.canGenerateCode())
    return;
  expect(!gti_test::emitCppText(frontend, lang::CppStandard::Cpp23, target)
              .empty(),
         "nested nonassignable and active-cleanup fields admit full emission");
  const lang::CppMirBodyEmissionMap rows(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23, target));
  std::size_t emptyProofs = 0;
  std::size_t publications = 0;
  for (const auto &owner : frontend.mir.classInstances()) {
    if (owner.compilerCapability != lang::CompilerCapabilityTypeKind::None ||
        owner.traits.moveAssignable || owner.declaredFields.empty() ||
        !lang::cppMirFailureConstructorEmptyStateTypeEligible(frontend.mir,
                                                              owner.type))
      continue;
    ++emptyProofs;
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirClassInstance *>(
          malformed.findClassInstance(owner.id));
      if (mutation == 0)
        changed->traits.containsBorrowedState = true;
      else if (mutation == 1)
        changed->cAbiRecord = true;
      else
        changed->declaredFields.front().symbol = 0;
      expect(!lang::cppMirFailureConstructorEmptyStateTypeEligible(malformed,
                                                                   owner.type),
             "construction-only empty state retains borrow, layout and field "
             "identity checks");
    }
  }
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (!lang::CppMirBodyEmitter(frontend.mir, rows)
             .supportsFailureBodyText(
                 {lang::MirBodyKind::Constructor, constructor.id}))
      continue;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            !instruction.constructorInitializer || !instruction.destination)
          continue;
        const auto *field =
            constructor.body.findPlace(*instruction.destination);
        if (!field || field->type.kind != lang::SemanticType::Class ||
            field->traits.moveAssignable)
          continue;
        ++publications;
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(constructor.id));
        for (auto &changedBlock : changed->body.blocks)
          for (auto &changedInstruction : changedBlock.instructions)
            if (changedInstruction.id == instruction.id)
              changedInstruction.constructorInitializer = 0;
        expect(!lang::CppMirBodyEmitter(malformed, rows)
                    .supportsFailureBodyText(
                        {lang::MirBodyKind::Constructor, constructor.id}),
               "in-place publication requires the selected constructor "
               "initializer");
      }
    }
  }
  // The mutex plus its three ordinary wrappers prove empty construction;
  // the three wrapper fields exercise the fallible publication classifier.
  expect(emptyProofs >= 4 && publications >= 3,
         "checks cover genuine nonassignable mutex-bearing wrappers alongside "
         "passive and active source optional fields (empty=" +
             std::to_string(emptyProofs) +
             ", publications=" + std::to_string(publications) + ")");
}

void checkEnumStorage(const char *fixture, bool owning) {
  const auto path =
      std::filesystem::path(__FILE__).parent_path() / "fixtures" / fixture;
  std::ifstream stream(path);
  const std::string source{std::istreambuf_iterator<char>(stream), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "source optional passes frontend");
  if (!frontend.canGenerateCode())
    return;
  expect(!gti_test::emitCppText(frontend).empty(),
         "source optional admits complete production emission");
  expect(
      std::none_of(frontend.mir.classInstances().begin(),
                   frontend.mir.classInstances().end(),
                   [](const auto &owner) {
                     return owner.compilerCapability ==
                            lang::CompilerCapabilityTypeKind::OptionalStorage;
                   }),
      "source optional does not instantiate private optional storage");
  std::size_t checked = 0;
  std::size_t producers = 0;
  const auto checkProducers = [&](const lang::MirBody &body,
                                  lang::MirBodyAddress address) {
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::PayloadConstruct ||
            instruction.info.type.kind != lang::SemanticType::Enum ||
            instruction.operands.empty())
          continue;
        ++producers;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = lang::findMirBody(malformed, address);
          for (auto &changedBlock : changed->blocks)
            for (auto &candidate : changedBlock.instructions)
              if (candidate.id == instruction.id) {
                if (mutation == 0)
                  candidate.enumOwner = 0;
                else if (mutation == 1)
                  candidate.enumVariant = std::size_t(-1);
                else
                  candidate.operands.front().type = lang::SemanticType::Unknown;
              }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "ordinary enum construction retains exact owner, variant and "
                 "payload type");
        }
      }
  };
  for (const auto &function : frontend.mir.functionInstances())
    checkProducers(function.body, {lang::MirBodyKind::Function, function.id});
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    checkProducers(constructor.body,
                   {lang::MirBodyKind::Constructor, constructor.id});
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *field =
            instruction.destination
                ? constructor.body.findPlace(*instruction.destination)
                : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            !instruction.constructorInitializer || !field ||
            field->type.kind != lang::SemanticType::Enum ||
            instruction.operands.size() != 1)
          continue;
        ++checked;
        expect(owning == !instruction.lifecycle.empty(),
               "enum publication has cleanup exactly when its payload owns "
               "cleanup");
        for (int mutation = 0; mutation != (owning ? 6 : 4); ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          auto &producerBlock = *std::find_if(
              changed->body.blocks.begin(), changed->body.blocks.end(),
              [&](const auto &candidate) { return candidate.id == block.id; });
          auto &publication = *std::find_if(
              producerBlock.instructions.begin(),
              producerBlock.instructions.end(), [&](const auto &candidate) {
                return candidate.id == instruction.id;
              });
          if (mutation == 0)
            publication.constructorInitializer = 0;
          else if (mutation == 1)
            publication.operands.front().type = lang::SemanticType::Int32;
          else if (mutation == 2)
            publication.operands.push_back(publication.operands.front());
          else if (mutation == 3) {
            auto *place = const_cast<lang::MirPlace *>(
                changed->body.findPlace(*publication.destination));
            if (place->root == lang::MirPlaceRootKind::Symbol)
              place->symbol = 0;
            else
              place->projections.front().field = 0;
          } else if (mutation == 4)
            publication.lifecycle.clear();
          else
            publication.lifecycle.front().source = 0;
          expect(lang::rebuildMirValueUses(changed->body),
                 "enum publication mutation retains SSA use records");
          const bool rejected =
              mutation == 0
                  ? !lang::CppMirBodyEmitter(
                         malformed, lang::CppMirBodyEmissionMap(
                                        buildRows(frontend, frontend.mir)))
                         .supportsFailureBodyText(
                             {lang::MirBodyKind::Constructor, constructor.id})
                  : !lang::verifyMirProgram(malformed).valid();
          expect(rejected,
                 "enum publication rejects missing initializer, wrong type, "
                 "extra operand, field identity and cleanup transfer drift "
                 "(mutation " +
                     std::to_string(mutation) + ")");
        }
      }
    }
  }
  expect(producers != 0 && (!owning || checked != 0),
         "source optional exercises ordinary enum production and owning field "
         "publication");
}

void checkFieldReplacement() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/constructor_field_call_assignment.gti";
  std::ifstream stream(path);
  const std::string source{std::istreambuf_iterator<char>(stream), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "constructor replacements have valid MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t assignments = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto &owners = frontend.hir.classInstances();
    const auto owner =
        std::find_if(owners.begin(), owners.end(), [&](const auto &candidate) {
          return candidate.id == constructor.owner;
        });
    if (owner == owners.end() || !owner->source ||
        owner->source->name().lexeme != "Holder")
      continue;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            instruction.info.type.kind != lang::SemanticType::Class ||
            !instruction.destination || instruction.lifecycle.empty())
          continue;
        ++assignments;
        expect(lang::CppMirBodyEmitter(frontend.mir, rows)
                   .supportsFailureBodyText(
                       {lang::MirBodyKind::Constructor, constructor.id}),
               "qualified and unqualified replacement share the field proof");
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(constructor.id));
        for (auto &changedBlock : changed->body.blocks)
          for (auto &candidate : changedBlock.instructions)
            if (candidate.id == instruction.id)
              candidate.lifecycle.front().source = 0;
        expect(
            !lang::CppMirBodyEmitter(malformed, rows)
                 .supportsFailureBodyText(
                     {lang::MirBodyKind::Constructor, constructor.id}),
            "field replacement requires the exact result ownership transfer");
        malformed = frontend.mir;
        changed = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(constructor.id));
        auto *place = const_cast<lang::MirPlace *>(
            changed->body.findPlace(*instruction.destination));
        if (place->root == lang::MirPlaceRootKind::Symbol)
          place->symbol = 0;
        else
          place->projections.front().field = 0;
        expect(!lang::CppMirBodyEmitter(malformed, rows)
                    .supportsFailureBodyText(
                        {lang::MirBodyKind::Constructor, constructor.id}),
               "an unknown symbol or projection cannot be treated as a "
               "receiver field");
      }
    }
  }
  expect(assignments == 2, "fixture covers both receiver field spellings");
}

int main() {
  checkEnumStorage("optional_value.gti", false);
  checkEnumStorage("optional_lifecycle.gti", true);
  checkContainingClasses();
  checkFieldReplacement();
  if (failures)
    return 1;
  std::cout << "optional enum publication proof tests passed\n";
}
