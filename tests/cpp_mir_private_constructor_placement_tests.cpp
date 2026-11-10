#include "cpp_mir_body_emitter_test_fixture.h"

// C++ spelling of private failure-tag construction: placement new in the
// member's lexical access context. The backend-neutral contract lives in
// private_constructor_placement_tests.cpp.
int main() {
  using namespace gti_test::cpp_mir_body_emitter_fixture;
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/private_constructor_placement.gti";
  std::ifstream input(path);
  expect(input.good(), "private constructor fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "same-class private factories retain valid MIR");
  if (!frontend.canGenerateCode())
    return 1;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t factories = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    if (!hir || !hir->source ||
        (hir->source->name().lexeme != "direct" &&
         hir->source->name().lexeme != "named"))
      continue;
    ++factories;
    const auto emitted =
        lang::CppMirBodyEmitter(frontend.mir, rows)
            .emitFailureBodyText({lang::MirBodyKind::Function, function.id},
                                 "test", 0);
    std::istringstream lines(emitted.text);
    std::string line;
    std::size_t constructors = 0;
    bool lexical = true;
    while (std::getline(lines, line)) {
      if (line.find("mir_failure_constructor_tag_v1<") == std::string::npos)
        continue;
      ++constructors;
      lexical &=
          line.find("::new (static_cast<void *>(") != std::string::npos &&
          line.find("std::construct_at(") == std::string::npos &&
          line.find("construct_representation_at(") == std::string::npos;
    }
    expect(emitted.emitted() && constructors != 0 && lexical,
           "failure-tag construction executes in the member's lexical access "
           "context");
  }
  expect(factories == 2,
         "fixture covers result storage and named-local storage");
  return failures ? 1 : 0;
}
