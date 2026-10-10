// Static semantics of `constexpr for` and <std/meta>: expansion, deferral,
// diagnostics, access policy and the formatter. Runtime behaviour and the
// absence of reflection residue in emitted C++ are covered by
// reflection_runtime_test.py.
#include "gti/formatter.h"
#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

int failures = 0;

void expect(bool valid, const std::string &message) {
  if (!valid) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(const std::filesystem::path &root,
                             const std::filesystem::path &entry,
                             lang::FrontendPhase stopAfter = lang::FrontendPhase::Hir) {
  lang::FrontendOptions options;
  options.stopAfter = stopAfter;
  options.analyzeRecoveredProgram = true;
  return lang::Frontend(options).analyze(entry, std::nullopt,
                                         {root / "stdlib/prelude.gti"}, {},
                                         {root / "stdlib"});
}

lang::FrontendResult analyzeSource(const std::filesystem::path &root,
                                   const std::filesystem::path &directory,
                                   const std::string &name,
                                   const std::string &source) {
  const std::filesystem::path path = directory / name;
  std::ofstream(path) << source;
  return analyze(root, path);
}

bool hasCode(const lang::FrontendResult &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code;
                     });
}

std::size_t countCode(const lang::FrontendResult &result,
                      std::string_view code) {
  return static_cast<std::size_t>(
      std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code == code;
                    }));
}

const std::string kPlayer = R"(#include <std/meta>
struct Player {
  uint32_t health = 100;
  float speed = 1.5;
};
)";

void checkExpansion(const std::filesystem::path &root,
                    const std::filesystem::path &directory) {
  // Concrete expansion with a constexpr element binding and a reflected read.
  const auto concrete = analyzeSource(
      root, directory, "concrete.gti",
      kPlayer + R"(int main() {
  Player player = {.health = uint32_t(7), .speed = 2.0};
  mut uint64_t sum = 0;
  constexpr for (auto field : std::meta::fields_of<Player>()) {
    if constexpr (field.index() == uint64_t(0)) {
      sum = sum + uint64_t(field.get(player));
    }
    static_assert(field.index() < uint64_t(2), "two fields");
  }
  return int(sum);
}
)");
  expect(concrete.diagnostics.empty() && concrete.hirValid,
         "concrete expansion is accepted");

  // A dependent sequence defers in the primary and expands per instance.
  const auto deferred = analyzeSource(
      root, directory, "deferred.gti",
      kPlayer + R"(double total<T>(T& value) {
  mut double sum = 0.0;
  constexpr for (auto field : std::meta::fields_of<T>()) {
    if constexpr (field.type().is_integral()) {
      sum = sum + double(field.get(value));
    } else if constexpr (field.type().is_floating_point()) {
      sum = sum + double(field.get(value));
    }
  }
  return sum;
}
int main() {
  Player player = {.health = uint32_t(7), .speed = 2.0};
  return int(total(player));
}
)");
  expect(deferred.diagnostics.empty() && deferred.hirValid,
         "deferred expansion in a generic function is accepted");

  // Every copy keeps its own locals; a reflection-free sequence works too.
  const auto locals = analyzeSource(
      root, directory, "locals.gti",
      R"(#include <std/meta>
int main() {
  mut uint64_t total = 0;
  constexpr for (auto element : std::meta::list<int32_t, bool>()) {
    auto local = uint64_t(1);
    total = total + local;
  }
  constexpr for (auto nothing : std::meta::list<>()) {
    total = total + uint64_t(100);
  }
  return int(total);
}
)");
  expect(locals.diagnostics.empty() && locals.hirValid,
         "general sequences, per-copy locals and empty sequences are accepted");

  // Reflected access keeps the field's own mutability and access rules.
  const auto mutation = analyzeSource(
      root, directory, "mutation.gti",
      R"(#include <std/meta>
struct Counter { mut { uint32_t hits = 0; } };
int main() {
  mut Counter counter = {.hits = uint32_t(1)};
  Counter frozen = {.hits = uint32_t(1)};
  constexpr for (auto field : std::meta::fields_of<Counter>()) {
    field.get_mut(counter) = field.get(counter) + uint32_t(10);
    field.get_mut(frozen) = uint32_t(0);
  }
  return int(counter.hits);
}
)");
  expect(hasCode(mutation, "GTI-S2112"),
         "get_mut through a read-only owner is rejected");
  expect(!hasCode(mutation, "GTI-S2007"), "writable field access is accepted");

  const auto privateAccess = analyzeSource(
      root, directory, "private.gti",
      R"(#include <std/meta>
class Secret {
  uint32_t hidden = 1;
public:
  uint32_t shown = 2;
  constexpr uint64_t total() {
    mut uint64_t sum = 0;
    constexpr for (auto field : std::meta::fields_of<Secret>()) {
      sum = sum + uint64_t(field.get(this));
    }
    return sum;
  }
};
int main() {
  Secret secret{};
  mut uint32_t sum = 0;
  constexpr for (auto field : std::meta::fields_of<Secret>()) {
    sum = sum + field.get(secret);
  }
  return int(sum);
}
)");
  expect(hasCode(privateAccess, "GTI-S2007"),
         "a private field is inaccessible through reflection outside its class");
  expect(countCode(privateAccess, "GTI-S2007") == 1,
         "the private-field diagnostic is reported once per use site");
}

void checkDiagnostics(const std::filesystem::path &root,
                      const std::filesystem::path &directory) {
  const auto sequence = analyzeSource(
      root, directory, "sequence.gti",
      R"(int main() {
  constexpr for (auto x : 42) { }
  return 0;
}
)");
  expect(hasCode(sequence, "GTI-S2107"), "a non-sequence operand is rejected");

  const auto index = analyzeSource(
      root, directory, "index.gti",
      kPlayer + R"(int main() {
  Player player{};
  std::meta::field<Player, 5> missing{};
  return int(missing.get(player));
}
)");
  expect(hasCode(index, "GTI-S2111"), "a field index out of range is rejected");

  // Diagnostics inside a copy name the element and repeat once per element
  // only when they differ.
  const auto related = analyzeSource(
      root, directory, "related.gti",
      kPlayer + R"(int main() {
  constexpr for (auto field : std::meta::fields_of<Player>()) {
    undefined_name = 1;
  }
  return 0;
}
)");
  expect(countCode(related, "GTI-S2001") == 1,
         "an identical diagnostic from every copy is reported once");
  const auto named = std::find_if(
      related.diagnostics.begin(), related.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2001";
      });
  expect(named != related.diagnostics.end() && !named->related.empty() &&
             named->related.front().message.find("element 0") !=
                 std::string::npos,
         "a diagnostic from a copy names its element");

  std::string many = "#include <std/meta>\nint main() {\n  constexpr for (auto x : std::meta::list<";
  for (int count = 0; count < 1025; ++count) {
    many += count == 0 ? "int32_t" : ", int32_t";
  }
  many += ">()) { }\n  return 0;\n}\n";
  const auto budget = analyzeSource(root, directory, "budget.gti", many);
  expect(hasCode(budget, "GTI-S2108"), "the expansion budget is enforced");

  const auto binding = analyzeSource(
      root, directory, "binding.gti",
      R"(@compiler_type("meta_list") struct mine<Elements...> {};
int main() { return 0; }
)");
  expect(hasCode(binding, "GTI-S2049"),
         "@compiler_type is rejected outside the trusted unit");
}

void checkFormatter() {
  const std::string source = R"(#include <std/meta>

int main() {
  constexpr for (auto field : std::meta::fields_of<int>()) {
    return 1;
  }
  return 0;
}
)";
  const lang::Formatter formatter;
  const std::string formatted = formatter.format(source);
  expect(formatted == source, "constexpr for formats as written");
  expect(formatter.format(formatted) == formatted, "formatting is idempotent");
}

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("gti-reflection-" + std::to_string(::getpid()));
  std::filesystem::create_directories(directory);
  checkExpansion(root, directory);
  checkDiagnostics(root, directory);
  checkFormatter();
  std::filesystem::remove_all(directory);
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  return 0;
}
