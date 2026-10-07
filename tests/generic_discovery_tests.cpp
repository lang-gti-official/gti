#include "gti/frontend.h"
#include "gti/hir.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;

void expect(bool valid, std::string_view message) {
  if (!valid) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(std::string_view source) {
  const auto standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("generic-discovery.gti", std::string(source),
                                  {standardLibrary / "prelude.gti"}, {},
                                  {standardLibrary});
}

void checkAliases() {
  constexpr std::string_view declarations = R"gti(
class Key {
  int value;
public:
  Key(int value) : value(value) {}
  bool operator==(Key& other) { return this.value == other.value; }
  bool operator!=(Key& other) { return this.value != other.value; }
};
class Policy<std::equality_comparable T> {
public:
  Policy() {}
};
)gti";
  const auto valid = analyze(std::string(declarations) + R"gti(
using Alias = Policy<Key>;
class Holder {
public:
  using Member = Policy<Key>;
  Member item{};
};
int main() { Alias a{}; Holder h{}; return 0; }
)gti");
  for (const auto &diagnostic : valid.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(valid.canGenerateCode(),
         "namespace and member aliases validate completed class facts");
  const auto invalid = analyze(std::string(declarations) + R"gti(
class Missing { public: Missing() {} };
using Unused = Policy<Missing>;
int main() { return 0; }
)gti");
  bool constraint = false;
  for (const auto &diagnostic : invalid.diagnostics)
    constraint = constraint ||
                 (diagnostic.code == "GTI-S2029" &&
                  diagnostic.message.find("Missing") != std::string::npos &&
                  std::filesystem::path(diagnostic.primary.source).filename() ==
                      "generic-discovery.gti" &&
                  diagnostic.primary.end > diagnostic.primary.start);
  expect(!invalid.canGenerateCode() && constraint,
         "an unused alias cannot bypass its concrete class constraint");
}

void checkRanges() {
  const auto valid = analyze(R"gti(
int sum<T>(T seed) {
  int[2] local = {1, 2};
  mut int total = 0;
  for (auto& value : local) { total += value; }
  return total;
}
int main() { return sum(1) + sum(true) - 6; }
)gti");
  expect(valid.canGenerateCode(),
         "non-dependent ranges retain each concrete body's loan domain");
  if (!valid.canGenerateCode()) {
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  std::size_t ranges = 0;
  for (const auto &function : valid.hir.functionInstances())
    for (const auto &statement : function.body.statements)
      ranges += statement.rangeIteration.has_value();
  expect(ranges == 2, "both generic instances contain their own range plan");
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto malformed = valid.hir;
    auto &functions = const_cast<std::vector<lang::HirFunctionInstance> &>(
        malformed.functionInstances());
    bool changed = false;
    for (auto &function : functions) {
      for (auto &statement : function.body.statements) {
        if (!statement.rangeIteration)
          continue;
        auto &range = *statement.rangeIteration;
        if (mutation == 0)
          range.elementAccess = lang::AccessMode::Mutable;
        if (mutation == 1)
          range.rangeLoan = 0;
        if (mutation == 2)
          range.arrayLength += 1;
        if (mutation == 3)
          range.rangeCarrier = range.element;
        if (mutation == 4)
          range.strategy = lang::RangeIterationStrategy::Structural;
        changed = true;
        break;
      }
      if (changed)
        break;
    }
    expect(changed &&
               !lang::verifyHirProgramPlans(valid.semantics, malformed).valid(),
           "concrete range plans still reject corrupted access, loans, shape "
           "and carrier");
  }
  const auto invalid = analyze(R"gti(
int change<T>(T seed) {
  int[2] local = {1, 2};
  for (mut auto& value : local) { value += 1; }
  return 0;
}
int main() { return change(1); }
)gti");
  expect(!invalid.canGenerateCode(),
         "generic range reanalysis does not authorize mutable immutable-owner "
         "access");
}

std::string finite(std::size_t count) {
  return R"gti(
int bounded<uint64_t N>() {
  if constexpr (N == 0) { return 0; }
  else { return 1 + bounded<N - 1>(); }
}
int main() { return bounded<)gti" +
         std::to_string(count) + ">() - " + std::to_string(count) + "; }";
}

void checkLimits() {
  expect(analyze(finite(62)).canGenerateCode(),
         "a finite chain at the 64-level discovery boundary remains valid");
  const auto excessive = analyze(finite(63));
  bool found = false;
  for (const auto &diagnostic : excessive.diagnostics) {
    if (diagnostic.code != "GTI-S2105")
      continue;
    found = diagnostic.phase == lang::DiagnosticPhase::Semantics &&
            std::filesystem::path(diagnostic.primary.source).filename() ==
                "generic-discovery.gti" &&
            diagnostic.primary.end > diagnostic.primary.start &&
            diagnostic.message.find("64") != std::string::npos &&
            !diagnostic.related.empty() && !diagnostic.hints.empty() &&
            diagnostic.fixes.empty();
  }
  expect(!excessive.canGenerateCode() && found,
         "the first excessive discovery level reports a located resource "
         "diagnostic");
  const auto growing = analyze(R"gti(
class Box<T> {
  T value;
public:
  Box(T value) : value(value) {}
};
int depth<T>(T value, int count) {
  if (count == 0) { return 0; }
  return 1 + depth(Box<T>(value), count - 1);
}
int main() { return depth(1, 3) - 3; }
)gti");
  bool bounded = false;
  for (const auto &diagnostic : growing.diagnostics)
    bounded = bounded || diagnostic.code == "GTI-S2105";
  expect(!growing.canGenerateCode() && bounded,
         "runtime-guarded type-growing recursion terminates with a diagnostic");
  expect(analyze(R"gti(
int exact<T>(T value, int count) {
  if (count == 0) { return 0; }
  return 1 + exact(value, count - 1);
}
int main() { return exact(1, 200) - 200; }
)gti")
             .canGenerateCode(),
         "exact recursive calls reuse a canonical instance");
}
} // namespace

int main() {
  checkAliases();
  checkRanges();
  checkLimits();
  return failures == 0 ? 0 : 1;
}
