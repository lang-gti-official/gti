#include "cpp_mir_body_emitter_test_fixture.h"

int main() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/nested_guarded_expected.gti";
  std::ifstream input(path);
  expect(input.good(), "nested guarded owner fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "nested guarded owner forwarding has valid MIR");
  if (!frontend.canGenerateCode())
    return 1;
  const lang::MirClassInstance *leaf = nullptr;
  const lang::MirClassInstance *owner = nullptr;
  const lang::MirFunctionInstance *factory = nullptr;
  for (const auto &item : frontend.hir.classInstances()) {
    if (!item.source)
      continue;
    if (item.source->name().lexeme == "Leaf")
      leaf = frontend.mir.findClassInstance(item.id);
    if (item.source->name().lexeme == "Envelope")
      owner = frontend.mir.findClassInstance(item.id);
  }
  for (const auto &item : frontend.hir.functionInstances())
    if (item.source && item.source->name().lexeme == "make_envelope")
      factory = frontend.mir.findFunctionInstance(item.id);
  expect(leaf && owner && factory, "fixture retains exact classes and factory");
  if (!leaf || !owner || !factory)
    return 1;
  expect(leaf->destructor && leaf->requiresActiveDropState &&
             !owner->destructor && !owner->requiresActiveDropState &&
             owner->requiresActiveCleanup &&
             owner->moveConstructor == lang::SpecialMemberStatus::Generated,
         "generated parent delegates moved-from disarming to guarded leaf");
  expect(!lang::mirTypeMoveIsDefinedFailureFree(frontend.mir, owner->type) &&
             lang::mirTypeRelocationIsDefinedFailureFree(frontend.mir,
                                                         owner->type),
         "relocation proves guarded transfer without pretending cleanup is "
         "passive");
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::MirBodyAddress factoryAddress{lang::MirBodyKind::Function,
                                            factory->id};
  expect(factory->returnType.kind == lang::SemanticType::Class &&
             lang::CppMirBodyEmitter(frontend.mir, rows)
                 .supportsFailureBodyText(factoryAddress),
         "ordinary source result placement admits exact nested guarded owner");
  const auto artifact = gti_test::emitCpp(frontend);
  const std::string active =
      "__gti_lifecycle_active_" + std::to_string(leaf->declaration);
  expect(artifact.contents.find(active + "(other." + active + ")") !=
                 std::string::npos &&
             artifact.contents.find(active + " = other." + active + ";") !=
                 std::string::npos &&
             artifact.contents.find("other." + active + " = false;") !=
                 std::string::npos,
         "generated moves must transfer then retire the source lifecycle bit, "
         "preserving both live owners and disarmed private empty shells");
  for (int mutation = 0; mutation != 7; ++mutation) {
    auto malformed = frontend.mir;
    auto *changedLeaf = const_cast<lang::MirClassInstance *>(
        malformed.findClassInstance(leaf->id));
    auto *changedOwner = const_cast<lang::MirClassInstance *>(
        malformed.findClassInstance(owner->id));
    if (mutation == 0)
      changedLeaf->requiresActiveDropState = false;
    if (mutation == 1)
      changedLeaf->destructorStatus = lang::SpecialMemberStatus::Generated;
    if (mutation == 2)
      changedLeaf->moveConstructor = lang::SpecialMemberStatus::Deleted;
    if (mutation == 3)
      changedOwner->moveConstructor = lang::SpecialMemberStatus::Deleted;
    if (mutation == 4)
      changedLeaf->declaredFields.front().type = owner->type;
    if (mutation == 5)
      changedLeaf->unionLayout.emplace();
    if (mutation == 6)
      changedOwner->abstract = true;
    if (mutation != 6)
      expect(
          !lang::mirTypeRelocationIsDefinedFailureFree(malformed, owner->type),
          "relocation rejects missing guards, non-generated moves, cycles and "
          "unions");
    expect(!lang::verifyMirProgram(malformed).valid(),
           "source result verification independently rejects malformed nested "
           "owner proof");
  }
  return failures ? 1 : 0;
}
