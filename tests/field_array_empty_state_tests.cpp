#include "cpp_mir_body_emitter_test_fixture.h"

#include <regex>

int main() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/issue_144_class_array_field.gti";
  std::ifstream stream(path);
  const std::string source{std::istreambuf_iterator<char>(stream), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "class array fields have valid MIR");
  if (!frontend.canGenerateCode())
    return 1;
  std::size_t arrays = 0;
  std::size_t interfaces = 0;
  for (const auto &owner : frontend.mir.classInstances()) {
    if (owner.declaredFields.size() != 6)
      continue;
    expect(lang::cppMirFailureConstructorEmptyStateEligible(frontend.mir,
                                                            owner.id),
           "array owner has recursively proved private empty state");
    for (const auto &field : owner.declaredFields) {
      expect(lang::cppMirFailureConstructorEmptyStateTypeEligible(frontend.mir,
                                                                  field.type),
             "each fixed array has recursively proved empty elements");
      ++arrays;
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto wrong = field.type;
        if (mutation == 0)
          wrong.arrayLength = 0;
        else if (mutation == 1)
          wrong.arrayLengthParameterId = 999;
        else
          wrong.arguments.clear();
        expect(!lang::cppMirFailureConstructorEmptyStateTypeEligible(
                   frontend.mir, wrong),
               "empty array proof requires an exact concrete nonzero extent "
               "and element");
      }
      const auto &elementType = field.type.arguments.front();
      const auto element = std::find_if(
          frontend.mir.classInstances().begin(),
          frontend.mir.classInstances().end(),
          [&](const auto &candidate) { return candidate.type == elementType; });
      if (element == frontend.mir.classInstances().end() ||
          !element->polymorphic)
        continue;
      ++interfaces;
      for (int mutation = 0; mutation != 6; ++mutation) {
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirClassInstance *>(
            malformed.findClassInstance(element->id));
        auto *base = const_cast<lang::MirClassInstance *>(
            malformed.findClassInstance(changed->bases.front().instance));
        switch (mutation) {
        case 0:
          changed->bases.front().interface = false;
          break;
        case 1:
          changed->structuralBases.clear();
          break;
        case 2:
          base->kind = lang::ClassKind::Class;
          break;
        case 3:
          base->requiresActiveCleanup = true;
          break;
        case 4:
          base->defaultConstructor = lang::SpecialMemberStatus::Declared;
          break;
        case 5:
          changed->abstract = true;
          break;
        }
        expect(!lang::cppMirFailureConstructorEmptyStateTypeEligible(
                   malformed, field.type),
               "array element rejects state-bearing, stale, non-interface, "
               "cleanup, source-default, or abstract bases");
      }
    }
  }
  expect(arrays == 6 && interfaces == 2,
         "fixture exercises scalar and five class arrays including both "
         "interface-only cleanup forms");
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t publications = 0;
  for (const auto &constructor : frontend.mir.constructorInstances())
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.info.type.kind != lang::SemanticType::Array ||
            instruction.info.type.arguments.front().kind !=
                lang::SemanticType::Class)
          continue;
        ++publications;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          for (auto &candidateBlock : changed->body.blocks)
            for (auto &candidate : candidateBlock.instructions) {
              if (candidate.id != instruction.id)
                continue;
              if (mutation == 0)
                candidate.lifecycle.clear();
              else if (mutation == 1)
                candidate.constructorInitializer = 0;
              else
                candidate.lifecycle.front().target =
                    candidate.lifecycle.front().source;
            }
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Constructor, constructor.id}),
                 "field array publication rejects missing transfer, stale "
                 "initializer, and wrong rollback identity");
        }
      }
  expect(publications == 5,
         "all five class arrays retain exact MIR publication identities");
  try {
    const auto cpp = gti_test::emitCppText(frontend);
    expect(
        !std::regex_search(
            cpp,
            std::regex(
                R"(std::array<::__gti_program::[a-z_]+, [0-9]+> __gti_mir_v_[0-9]+\{\};)")),
        "class array aggregates never invent default-constructed scalar SSA "
        "arrays");
    const std::regex placement(
        R"(false, true> __gti_mir_p_[0-9]+\(std::addressof\(\(__gti_mir_p_[0-9]+\)\[[0-9]+\]\)\))");
    const auto fieldElements = [&] {
      std::size_t count = 0;
      for (const auto &owner : frontend.mir.classInstances())
        if (owner.declaredFields.size() == 6)
          for (const auto &field : owner.declaredFields)
            if (field.type.arguments.front().kind == lang::SemanticType::Class)
              count += field.type.arrayLength;
      return count;
    }();
    expect(static_cast<std::size_t>(std::distance(
               std::sregex_iterator(cpp.begin(), cpp.end(), placement),
               std::sregex_iterator())) == fieldElements,
           "each fresh class element aliases its final field address rather "
           "than moving from independent staging");
    for (const std::string_view name :
         {"passive_polymorphic_item", "counted_polymorphic_item",
          "field_array_owner"})
      expect(cpp.find("explicit " + std::string(name) +
                      "(::gti_internal::backend::mir_empty_state_constructor_"
                      "tag_v1) noexcept") != std::string::npos,
             "every proved interface element and array owner publishes its "
             "native private tag");
  } catch (const std::exception &error) {
    expect(false, error.what());
  }
  if (failures)
    return 1;
  std::cout << "field array empty-state proof tests passed\n";
}
