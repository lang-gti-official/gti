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
  options.requireEntryPoint = false;
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
