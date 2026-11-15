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

std::size_t countCode(const lang::FrontendResult &result,
                      std::string_view code) {
  std::size_t count = 0;
  for (const auto &diagnostic : result.diagnostics)
    count += diagnostic.code == code ? 1 : 0;
  return count;
}

// Issue #322: a type written where a value is required says so, instead of a
// contradictory mismatch or "Undefined name".
void checkTypeUsedAsValue() {
  const struct {
    const char *name;
    const char *source;
    const char *expected;
  } cases[] = {
      {"return-class",
       "import std.string;\n"
       "std::string make() { return std::string; }\n"
       "int main() { return 0; }\n",
       "'std::string' names a type, not a value."},
      {"auto-class",
       "struct Point { int x; };\n"
       "int main() { auto p = Point; return 0; }\n",
       "'Point' names a type, not a value."},
      {"argument-class",
       "struct Point { int x; };\n"
       "int use(Point p) { return p.x; }\n"
       "int main() { return use(Point); }\n",
       "'Point' names a type, not a value."},
      {"type-parameter",
       "bool check<T>() { return T; }\n"
       "int main() { return check<int32_t>() ? 1 : 0; }\n",
       "Type parameter 'T' names a type, not a value."},
  };
  for (const auto &probe : cases) {
    const auto result =
        analyzeQuietly(std::string(probe.name) + ".gti", probe.source);
    const auto *diagnostic = withMessage(result, probe.expected);
    expect(!result.canGenerateCode() && diagnostic != nullptr &&
               !diagnostic->hints.empty() &&
               withMessage(result, "Undefined name") == nullptr &&
               withMessage(result, "Cannot return a value of type") == nullptr,
           std::string("#322 ") + probe.name +
               " names the type as not a value");
    if (diagnostic == nullptr)
      for (const auto &other : result.diagnostics)
        std::cerr << probe.name << ": " << other.code << " " << other.message
                  << '\n';
  }
}

// Issue #396: when exactly one overload accepts the argument count, its own
// argument mismatch and that parameter's guidance are the diagnosis. Two
// overloads that both accept the count still report the overload set.
void checkSingleShapeCandidate() {
  const std::string prefix = "import std.string;\n"
                             "class Sink {\n"
                             "  mut { std::size_t total; }\n"
                             "public:\n"
                             "  Sink() : total(0) {}\n"
                             "  void append(std::string_view text) mut {}\n"
                             "  void append(std::string_view text, std::size_t "
                             "first, std::size_t count) mut {}\n";
  const std::string body = "};\n"
                           "int scale(int value) { return value; }\n"
                           "int scale(int value, int factor, int offset) {\n"
                           "  return value * factor + offset;\n"
                           "}\n"
                           "int main() {\n"
                           "  std::string word = std::string(\"abc\");\n"
                           "  mut Sink sink = Sink();\n"
                           "  sink.append(word);\n"
                           "  bool flag = true;\n"
                           "  return scale(flag);\n"
                           "}\n";
  const auto single = analyzeQuietly("single-shape.gti", prefix + body);
  std::size_t mismatches = 0;
  bool viewHint = false;
  bool onlyCandidateNote = false;
  for (const auto &diagnostic : single.diagnostics) {
    if (diagnostic.code != "GTI-S2003")
      continue;
    ++mismatches;
    for (const auto &hint : diagnostic.hints)
      viewHint = viewHint || hint.find("value.view()") != std::string::npos;
    for (const auto &related : diagnostic.related)
      onlyCandidateNote =
          onlyCandidateNote ||
          related.message.starts_with("Only candidate accepting 1 argument: ");
  }
  expect(!single.canGenerateCode() && mismatches == 2 &&
             countCode(single, "GTI-S2012") == 0 && viewHint &&
             onlyCandidateNote,
         "#396 the single overload accepting the argument count reports its "
         "own mismatch and guidance");
  if (mismatches != 2)
    for (const auto &other : single.diagnostics)
      std::cerr << "single-shape: " << other.code << " " << other.message
                << '\n';

  const auto both = analyzeQuietly(
      "two-shape.gti", prefix + "  void append(int value) mut {}\n" + body);
  expect(!both.canGenerateCode() && countCode(both, "GTI-S2012") == 1,
         "#396 two overloads accepting the argument count still report the "
         "overload set");
}

} // namespace

int main() {
  checkTypeUsedAsValue();
  checkSingleShapeCandidate();
  if (failures != 0)
    return 1;
  std::cout << "diagnostic placement tests passed\n";
}
