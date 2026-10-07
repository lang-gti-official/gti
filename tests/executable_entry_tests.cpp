#include "gti/driver/compilation.h"
#include "gti/frontend.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "gti-entry-diagnostic.gti";
  const std::string source = "// UTF-8: \xf0\x9f\x98\x80\nnamespace tea { int "
                             "main() { return 0; } }\n";
  lang::FrontendOptions options;
  options.requireEntryPoint = true;
  options.stopAfter = lang::FrontendPhase::Semantics;
  auto invalid = lang::Frontend(options).analyze(path, source);
  expect(!invalid.semanticValid && !invalid.canGenerateCode(),
         "missing entry accepted");
  expect(invalid.diagnostics.size() == 1, "missing entry cascaded");
  const auto &diagnostic = invalid.diagnostics.front();
  expect(diagnostic.code == "GTI-S2094" &&
             diagnostic.phase == lang::DiagnosticPhase::Semantics &&
             diagnostic.severity == lang::DiagnosticSeverity::Error,
         "wrong entry diagnostic classification");
  expect(diagnostic.primary.source ==
                 std::filesystem::weakly_canonical(path).string() &&
             diagnostic.primary.start == source.find("main") &&
             diagnostic.primary.end == source.find("main") + 4,
         "wrong entry source span");
  expect(diagnostic.related.size() == 1 &&
             diagnostic.related.front().span == diagnostic.primary &&
             diagnostic.related.front().message.find("tea::main") !=
                 std::string::npos &&
             diagnostic.hints.size() == 1 && diagnostic.fixes.empty(),
         "entry diagnostic missing explanation or guessing a fix");
  const auto fixed = lang::Frontend(options).analyze(
      path, source + "int main() { return tea::main(); }\n");
  expect(fixed.semanticValid && fixed.diagnostics.empty(),
         "global forwarding shim rejected");
  const auto empty =
      lang::Frontend(options).analyze(path, "int helper() { return 1; }\n");
  expect(empty.diagnostics.size() == 1 &&
             empty.diagnostics.front().primary.start == 0,
         "no-candidate diagnostic needs entry anchor");
  const auto inactive = lang::Frontend(options).analyze(
      path,
      "#if target.os == \"not-an-os\"\nint main() { return 0; }\n#endif\n");
  expect(!inactive.semanticValid && inactive.diagnostics.size() == 1 &&
             inactive.diagnostics.front().code == "GTI-S2094",
         "inactive entry counted as executable definition");
  options.completionOffset = source.find("main");
  const auto completion = lang::Frontend(options).analyze(path, source);
  for (const auto &item : completion.diagnostics) {
    expect(item.code != "GTI-S2094",
           "completion recovery invented missing entry");
  }
  options.completionOffset.reset();
  const std::string undefinedSource =
      "int helper(int value);\nint main() { return helper(7); }\n";
  const auto undefined = lang::Frontend(options).analyze(path, undefinedSource);
  expect(!undefined.semanticValid && undefined.diagnostics.size() == 1,
         "closed executable must reject a called declaration without a body");
  const auto &missingDefinition = undefined.diagnostics.front();
  expect(missingDefinition.code == "GTI-S2106" &&
             missingDefinition.phase == lang::DiagnosticPhase::Semantics &&
             missingDefinition.primary.source ==
                 std::filesystem::weakly_canonical(path).string() &&
             missingDefinition.primary.start ==
                 undefinedSource.rfind("helper") &&
             missingDefinition.primary.end ==
                 undefinedSource.rfind("helper") + 6 &&
             missingDefinition.related.size() == 1 &&
             missingDefinition.related.front().span.start ==
                 undefinedSource.find("helper") &&
             missingDefinition.fixes.empty() &&
             missingDefinition.hints.size() == 1,
         "missing definition diagnostic must locate the call and declaration");
  const auto unused = lang::Frontend(options).analyze(
      path, "int helper(int value);\nint main() { return 0; }\n");
  expect(unused.semanticValid, "unused bodyless declarations remain valid");
  const auto missingMethod = lang::Frontend(options).analyze(
      path, "class Box { public: Box() {} int read(); };\n"
            "int main() { Box value{}; return value.read(); }\n");
  expect(!missingMethod.semanticValid &&
             missingMethod.diagnostics.size() == 1 &&
             missingMethod.diagnostics.front().code == "GTI-S2106",
         "ordinary bodyless methods require the same definition check");
  const auto missingOperator = lang::Frontend(options).analyze(
      path, "class Box { public: Box() {} int operator+(int other); };\n"
            "int main() { Box value{}; return value + 1; }\n");
  expect(!missingOperator.semanticValid &&
             missingOperator.diagnostics.size() == 1 &&
             missingOperator.diagnostics.front().code == "GTI-S2106",
         "overloaded operators require definitions just like explicit calls");
  const auto foreign = lang::Frontend(options).analyze(
      path, "extern \"C\" { int32_t native(int32_t value); }\n"
            "int main() { return native(7); }\n");
  expect(foreign.semanticValid, "foreign declarations do not need GTI bodies");
  const auto defined = lang::Frontend(options).analyze(
      path, "int main() { return helper(7); }\n"
            "int helper(int value) { return value; }\n");
  expect(defined.semanticValid,
         "later definitions must resolve before use checking");
  options.requireEntryPoint = false;
  const auto fragment = lang::Frontend(options).analyze(path, undefinedSource);
  expect(fragment.semanticValid,
         "fragment analysis may resolve signatures without executable closure");
  const auto library = lang::Frontend(options).analyze(path, source);
  expect(library.semanticValid && library.diagnostics.empty(),
         "ordinary library rejected");
  const lang::driver::CompilationRequest request(
      path, {}, lang::TargetInfo::host(), lang::OptimizationLevel::O0,
      lang::CppStandard::Cpp23);
  const auto executable = request.forExecutable();
  expect(!request.requiresEntryPoint() && executable.requiresEntryPoint(),
         "executable policy must be explicit and immutable");
}
