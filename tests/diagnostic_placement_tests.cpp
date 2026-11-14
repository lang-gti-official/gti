// Diagnostics name the actual problem and point at the code to change.
#include "mir_test_fixture.h"

namespace {

lang::FrontendResult analyzeQuietly(std::string name, std::string source) {
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze(std::move(name), std::move(source),
                                  {standardLibrary / "prelude.gti"}, {},
                                  {standardLibrary});
}

const lang::Diagnostic *withMessage(const lang::FrontendResult &result,
                                    std::string_view text) {
  for (const auto &diagnostic : result.diagnostics)
    if (diagnostic.message.find(text) != std::string::npos)
      return &diagnostic;
  return nullptr;
}

// Issue #322: a type written where a value is required says so, instead of a
// contradictory mismatch or "Undefined name".
void checkTypeUsedAsValue() {
  const struct {
    const char *name;
    const char *source;
    const char *expected;
  } cases[] = {
      {"return-class", "import std.string;\n"
                       "std::string make() { return std::string; }\n"
                       "int main() { return 0; }\n",
       "'std::string' names a type, not a value."},
      {"auto-class", "struct Point { int x; };\n"
                     "int main() { auto p = Point; return 0; }\n",
       "'Point' names a type, not a value."},
      {"argument-class", "struct Point { int x; };\n"
                         "int use(Point p) { return p.x; }\n"
                         "int main() { return use(Point); }\n",
       "'Point' names a type, not a value."},
      {"type-parameter", "bool check<T>() { return T; }\n"
                         "int main() { return check<int32_t>() ? 1 : 0; }\n",
       "Type parameter 'T' names a type, not a value."},
  };
  for (const auto &probe : cases) {
    const auto result = analyzeQuietly(std::string(probe.name) + ".gti",
                                       probe.source);
    const auto *diagnostic = withMessage(result, probe.expected);
    expect(!result.canGenerateCode() && diagnostic != nullptr &&
               !diagnostic->hints.empty() &&
               withMessage(result, "Undefined name") == nullptr &&
               withMessage(result, "Cannot return a value of type") ==
                   nullptr,
           std::string("#322 ") + probe.name + " names the type as not a value");
    if (diagnostic == nullptr)
      for (const auto &other : result.diagnostics)
        std::cerr << probe.name << ": " << other.code << " " << other.message
                  << '\n';
  }
}

} // namespace

int main() {
  checkTypeUsedAsValue();
  if (failures != 0)
    return 1;
  std::cout << "diagnostic placement tests passed\n";
}
