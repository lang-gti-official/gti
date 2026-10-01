#include "cpp_mir_body_emitter_test_fixture.h"

int main() {
  const auto fixtures =
      std::filesystem::path(__FILE__).parent_path() / "fixtures";
  const auto path = fixtures / "structural_relocation.gti";
  std::ifstream input(path);
  expect(input.good(), "structural relocation fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "structural concepts and public relocation analyze");
  if (!frontend.canGenerateCode())
    return 1;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "relocation MIR verifies");
  const lang::MirClassInstance *particle = nullptr;
  const lang::MirClassInstance *resource = nullptr;
  for (const auto &item : frontend.hir.classInstances()) {
    if (!item.source)
      continue;
    if (item.source->name().lexeme == "Particle")
      particle = frontend.mir.findClassInstance(item.id);
    if (item.source->name().lexeme == "Resource")
      resource = frontend.mir.findClassInstance(item.id);
  }
  expect(particle && resource,
         "unrelated user classes have concrete MIR identities");
  if (!particle || !resource)
    return 1;
  expect(particle->traits.relocation == lang::RelocationKind::Bitwise &&
             resource->traits.relocation == lang::RelocationKind::Structural,
         "semantic relocation distinction survives HIR and MIR");
  expect(
      lang::mirTypeRelocationKind(
          frontend.mir, lang::SemanticType::arrayOf(particle->type, 4)) ==
              lang::RelocationKind::Bitwise &&
          lang::mirTypeRelocationKind(
              frontend.mir, lang::SemanticType::arrayOf(resource->type, 4)) ==
              lang::RelocationKind::Structural,
      "fixed arrays inherit the exact element relocation rule");
  const auto cpp = gti_test::emitCppText(frontend);
  expect(cpp.find("mir_storage_relocate_v1<true>(") != std::string::npos &&
             cpp.find("mir_storage_relocate_v1<false>(") != std::string::npos &&
             cpp.find("mir_prefix_relocate_v1<true>(") != std::string::npos &&
             cpp.find("mir_prefix_relocate_v1<false>(") != std::string::npos,
         "production sparse storage and vector growth consume verified "
         "eligibility");
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto malformed = frontend.mir;
    auto *passive = const_cast<lang::MirClassInstance *>(
        malformed.findClassInstance(particle->id));
    auto *owner = const_cast<lang::MirClassInstance *>(
        malformed.findClassInstance(resource->id));
    if (mutation == 0)
      owner->traits.relocation = lang::RelocationKind::Bitwise;
    if (mutation == 1)
      passive->moveConstructor = lang::SpecialMemberStatus::Deleted;
    if (mutation == 2)
      passive->declaredFields.front().type = resource->type;
    if (mutation == 3)
      passive->traits.relocation = lang::RelocationKind::Unavailable;
    if (mutation == 4)
      passive->polymorphic = true;
    expect(!lang::verifyMirProgram(malformed).valid(),
           "forged relocation facts cannot authorize the bulk path");
  }
  for (const auto fixture :
       {"stored_callable_clone.gti", "native_opaque_pointer_results.gti"}) {
    const auto compositionPath = fixtures / fixture;
    std::ifstream compositionInput(compositionPath);
    expect(compositionInput.good(), "relocation composition fixture exists");
    const std::string compositionSource{
        std::istreambuf_iterator<char>(compositionInput),
        std::istreambuf_iterator<char>()};
    const auto composition =
        analyzeWithStandardLibrary(compositionPath.string(), compositionSource);
    expect(composition.canGenerateCode() &&
               lang::verifyMirProgram(composition.mir).valid(),
           "interface bases, erased captures, and pointer-only native classes "
           "retain coherent relocation metadata");
    if (!composition.canGenerateCode())
      continue;
    expect(!gti_test::emitCppText(composition).empty(),
           "composed relocation metadata reaches the production backend");
    for (const auto &instance : composition.mir.classInstances()) {
      if (!instance.cOpaqueHandle)
        continue;
      expect(instance.traits.relocation == lang::RelocationKind::Unavailable,
             "opaque native pointees are not movable GTI values");
      auto malformed = composition.mir;
      auto *opaque = const_cast<lang::MirClassInstance *>(
          malformed.findClassInstance(instance.id));
      opaque->cOpaqueHandle = false;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "losing native-opaque identity invalidates relocation metadata");
    }
  }
  const auto requireRejected = [&](std::string text,
                                   std::string_view conceptName) {
    const auto result =
        analyzeWithStandardLibrary("relocation-denied.gti", text);
    const auto failure = std::find_if(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto &diagnostic) {
          return diagnostic.code == "GTI-S2029" &&
                 diagnostic.message.find(conceptName) != std::string::npos;
        });
    expect(!result.canGenerateCode() && failure != result.diagnostics.end(),
           "invalid structural relocation is rejected before lowering");
    if (failure != result.diagnostics.end()) {
      expect(!failure->hints.empty(),
             "constraint diagnostic explains eligibility");
      expect(!failure->related.empty(),
             "constraint diagnostic relates the declaration");
      expect(std::filesystem::path(failure->primary.source).filename() ==
                     "relocation-denied.gti" &&
                 failure->primary.start < failure->primary.end,
             "constraint diagnostic anchors the rejected source use");
    }
  };
  requireRejected(R"(
    class Fixed { public: Fixed() {} Fixed(Fixed&& source) = delete; };
    void accept<std::relocatable T>() {}
    int main() { accept<Fixed>(); return 0; }
  )",
                  "relocatable");
  requireRejected(R"(
    class Resource { public: Resource() {} ~Resource() {} };
    void accept<std::bitwise_relocatable T>() {}
    int main() { accept<Resource>(); return 0; }
  )",
                  "bitwise_relocatable");
  requireRejected(R"(
    class View { int& value; public: View(int& value) : value(value) {} };
    void accept<std::relocatable T>() {}
    int main() { accept<View>(); return 0; }
  )",
                  "relocatable");
  for (const std::string replacement :
       {"source.relocate_to(source, std::size_t(0));",
        "Particle& loan = source.get(std::size_t(0)); "
        "source.relocate_to(destination, std::size_t(0)); [[discard]] "
        "loan.id();",
        "destination.construct(std::size_t(0), Particle(90, 9.0)); "
        "Particle& loan = destination.get(std::size_t(0)); "
        "source.relocate_to(destination, std::size_t(0)); [[discard]] "
        "loan.id();"}) {
    auto invalid = source;
    const std::string original =
        "source.relocate_to(destination, std::size_t(0));";
    invalid.replace(invalid.find(original), original.size(), replacement);
    const auto denied =
        analyzeWithStandardLibrary("relocation-loan-denied.gti", invalid);
    expect(
        !denied.canGenerateCode() &&
            std::none_of(denied.diagnostics.begin(), denied.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-B0001"; }),
        "self-aliasing and outstanding source/destination loans remain "
        "semantic errors");
  }
  return failures ? 1 : 0;
}
