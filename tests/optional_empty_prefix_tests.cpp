#include "cpp_mir_body_emitter_test_fixture.h"

#include <regex>

void checkContainingClasses() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/constructor_nonassignable_fields.gti";
  std::ifstream stream(path);
  const std::string source{std::istreambuf_iterator<char>(stream), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "nonassignable constructor fields have valid MIR");
  if (!frontend.canGenerateCode())
    return;
  expect(!gti_test::emitCppText(frontend).empty(),
         "nested nonassignable and active-cleanup fields admit full emission");
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
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
  expect(emptyProofs >= 4 && publications >= 4,
         "checks cover unrelated wrappers plus passive and active optional "
         "fields");
}

void checkActiveStorage() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/optional_lifecycle.gti";
  std::ifstream stream(path);
  const std::string source{std::istreambuf_iterator<char>(stream), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "cleanup-owning optional passes frontend");
  if (!frontend.canGenerateCode())
    return;
  const auto cpp = gti_test::emitCppText(frontend);
  expect(
      std::regex_search(
          cpp,
          std::regex(
              R"(std::destroy_at\(std::addressof\((__gti_mir_p_[0-9]+)\)\);\s*::gti_internal::backend::construct_representation_at\(std::addressof\(\1\)\);\s*__gti_mir_construct_success_[0-9]+ = true;)")),
      "sealed native empty storage uses plain placement and explicit success");
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t checked = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *target = instruction.constructorTarget
                                 ? frontend.mir.findConstructorInstance(
                                       *instruction.constructorTarget)
                                 : nullptr;
        const auto *owner =
            target ? frontend.mir.findClassInstance(target->owner) : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Construct ||
            !instruction.successResultDrop || owner == nullptr ||
            owner->compilerCapability !=
                lang::CompilerCapabilityTypeKind::OptionalStorage)
          continue;
        ++checked;
        expect(lang::cppMirConstructedFieldInitializerBindings(frontend.mir,
                                                               constructor)
                   .empty(),
               "active storage keeps its MIR lifecycle, not the passive prefix "
               "route");
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          auto &producerBlock = *std::find_if(
              changed->body.blocks.begin(), changed->body.blocks.end(),
              [&](const auto &candidate) { return candidate.id == block.id; });
          if (mutation == 0) {
            producerBlock.terminator.successLifecycle.clear();
          } else if (mutation == 1) {
            for (auto &candidateBlock : changed->body.blocks)
              for (auto &candidate : candidateBlock.instructions)
                if (candidate.kind == lang::MirInstructionKind::Initialize &&
                    candidate.constructorInitializer != 0)
                  candidate.lifecycle.clear();
          } else if (mutation == 2) {
            auto *storage = const_cast<lang::MirClassInstance *>(
                malformed.findClassInstance(owner->id));
            storage->compilerCapability =
                lang::CompilerCapabilityTypeKind::None;
          } else {
            auto *empty = const_cast<lang::MirConstructorInstance *>(
                malformed.findConstructorInstance(target->id));
            empty->body.blocks.front().terminator.kind =
                lang::MirTerminatorKind::Unreachable;
            expect(!lang::cppMirFailureConstructorEmptyStateEligible(
                       malformed, constructor.owner) &&
                       !lang::cppMirFailureConstructorEmptyFieldsEligible(
                           malformed, constructor.owner),
                   "optional empty-state helpers retain the selected private "
                   "constructor Return proof");
          }
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Constructor, constructor.id}),
                 "native empty storage requires exact "
                 "activation/reparent/capability/return proof (mutation " +
                     std::to_string(mutation) + ")");
        }
      }
    }
  }
  expect(
      checked == 1,
      "active optional initializer exercises the native constructor boundary");
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
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/optional_value.gti";
  std::ifstream stream(path);
  const std::string source{std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "passive optional payload passes frontend");
  if (!frontend.canGenerateCode())
    return 1;
  std::size_t checked = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (constructor.body.blocks.size() < 2)
      continue;
    const auto bindings = lang::cppMirConstructedFieldInitializerBindings(
        frontend.mir, constructor);
    if (bindings.empty())
      continue;
    expect(bindings.size() == 1 && bindings.front().valueDrop == 0 &&
               bindings.front().fieldDrop == 0,
           "empty passive storage prefix has no invented cleanup identities");
    ++checked;
    for (int mutation = 0; mutation != 6; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      auto &entry = *std::find_if(
          changed->body.blocks.begin(), changed->body.blocks.end(),
          [&](const auto &block) { return block.id == changed->body.entry; });
      switch (mutation) {
      case 0:
        std::swap(entry.instructions[0], entry.instructions[1]);
        break;
      case 1:
        entry.instructions.back().operands.push_back(
            entry.instructions[1].operands.front());
        break;
      case 2: {
        auto *target =
            malformed.findConstructorInstance(bindings.front().constructor);
        auto *storage = const_cast<lang::MirClassInstance *>(
            malformed.findClassInstance(target->owner));
        storage->compilerCapability = lang::CompilerCapabilityTypeKind::None;
        break;
      }
      case 3:
        changed->initializers.front().arguments.front() += 10000;
        break;
      case 4:
        entry.instructions[2].fullExpressionEnd = 0;
        break;
      case 5: {
        auto *owner = const_cast<lang::MirClassInstance *>(
            malformed.findClassInstance(changed->owner));
        owner->declaredFields.front().requiresActiveCleanup = true;
        break;
      }
      }
      (void)lang::rebuildMirValueUses(changed->body);
      expect(
          lang::cppMirConstructedFieldInitializerBindings(malformed, *changed)
              .empty(),
          "empty prefix proof rejects ordering, extra use, capability, "
          "initializer, boundary, and cleanup mismatches");
    }
  }
  expect(checked == 1, "passive optional constructor exercises exact prefix "
                       "before fallible body");
  expect(!gti_test::emitCppText(frontend).empty(),
         "passive optional payload admits complete production emission");
  checkActiveStorage();
  checkContainingClasses();
  checkFieldReplacement();
  if (failures)
    return 1;
  std::cout << "optional empty prefix proof tests passed\n";
}
