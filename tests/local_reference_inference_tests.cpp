#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(const std::string &source) {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("local_reference.gti", source,
                                  {library / "prelude.gti"}, {}, {library});
}

std::string substitute(std::string text, std::string_view type) {
  for (auto pos = text.find("TYPE"); pos != std::string::npos;
       pos = text.find("TYPE", pos + type.size()))
    text.replace(pos, 4, type);
  return text;
}

const std::string declarations = R"(
struct Record { mut int count = 1; };
class Owner {
  mut int count = 7;
public:
  Owner() = default;
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
  int read() { return count; }
  void set(int value) mut { count = value; }
  int& field() { return this.count; }
  mut int& field_mut() mut { return this.count; }
};
expected<Owner&, int> lookup(Owner& owner, bool fail) {
  if (fail) { return unexpected(9); }
  return owner;
}
)";

bool samePlace(lang::PlaceKey left, lang::PlaceKey right) {
  // Independent frontend snapshots deliberately have different domains.
  // Body/revision and all source-bound root/projection facts must still agree.
  left.domain.snapshot = 0;
  right.domain.snapshot = 0;
  return left == right;
}

bool sameDependencies(const std::vector<lang::SemanticLoanDependency> &left,
                      const std::vector<lang::SemanticLoanDependency> &right) {
  if (left.size() != right.size())
    return false;
  for (std::size_t i = 0; i < left.size(); ++i)
    if (!samePlace(left[i].place, right[i].place) ||
        left[i].access != right[i].access ||
        left[i].designatedFields != right[i].designatedFields ||
        left[i].protectsStorage != right[i].protectsStorage)
      return false;
  return true;
}

void pair(std::string_view label, std::string_view type,
          std::string_view program, bool valid) {
  auto explicitResult =
      analyze(declarations + substitute(std::string(program), type));
  auto inferredResult =
      analyze(declarations + substitute(std::string(program), "auto"));
  for (const auto *result : {&explicitResult, &inferredResult}) {
    if (result->canGenerateCode() != valid) {
      std::cerr << label
                << (result == &explicitResult ? " explicit" : " inferred")
                << '\n';
      for (const auto &diagnostic : result->diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      check(false, "reference admission matches the contract");
    }
    check(std::none_of(result->diagnostics.begin(), result->diagnostics.end(),
                       [](const auto &diagnostic) {
                         return diagnostic.code.starts_with("GTI-B");
                       }),
          "no source case reaches an internal backend error");
    if (!valid)
      check(!result->semanticValid, "invalid references stop in semantics");
  }
  if (!explicitResult.canGenerateCode() || !inferredResult.canGenerateCode())
    return;
  check(lang::verifyMirProgram(inferredResult.mir).valid(),
        "inferred MIR verifies");
  check(explicitResult.hir.functionInstances().size() ==
            inferredResult.hir.functionInstances().size(),
        "same concrete instance closure");
  for (const auto &function : explicitResult.hir.functionInstances()) {
    const auto *inferred = inferredResult.hir.findFunctionInstance(function.id);
    if (inferred == nullptr) {
      check(false, "same function identity");
      continue;
    }
    check(function.body.bindings.size() == inferred->body.bindings.size(),
          "same binding census");
    for (std::size_t i = 0; i < std::min(function.body.bindings.size(),
                                         inferred->body.bindings.size());
         ++i) {
      const auto &left = function.body.bindings[i];
      const auto &right = inferred->body.bindings[i];
      check(left.info.type == right.info.type &&
                left.info.access == right.info.access &&
                left.dropObligation == right.dropObligation,
            "exact binding type, access and drop parity");
    }
    check(function.body.loans.size() == inferred->body.loans.size(),
          "same loan census");
    for (std::size_t i = 0;
         i < std::min(function.body.loans.size(), inferred->body.loans.size());
         ++i) {
      const auto &left = function.body.loans[i];
      const auto &right = inferred->body.loans[i];
      check(left.kind == right.kind && left.parent == right.parent &&
                left.storedDependency == right.storedDependency &&
                samePlace(left.place, right.place) &&
                left.access == right.access &&
                left.carriers == right.carriers &&
                sameDependencies(left.additionalDependencies,
                                 right.additionalDependencies) &&
                left.lifecycleBound == right.lifecycleBound &&
                left.entry == right.entry,
            "exact HIR owner, access, dependency and carrier parity");
    }
  }
  for (const auto &function : explicitResult.mir.functionInstances()) {
    const auto *inferred = inferredResult.mir.findFunctionInstance(function.id);
    check(inferred != nullptr, "same MIR function identity");
    if (inferred == nullptr)
      continue;
    check(function.body.loans == inferred->body.loans,
          "same MIR owner, address, parent, access, escape and carrier facts");
    check(function.body.dropObligations == inferred->body.dropObligations,
          "same MIR cleanup obligations");
    for (const auto &loan : inferred->body.loans) {
      if (loan.kind != lang::MirLoanKind::Local || loan.source == 0)
        continue;
      auto detached = inferredResult.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          detached.findFunctionInstance(function.id));
      changed->body.loans[loan.id - 1].source = 0;
      check(!lang::verifyMirProgram(detached).valid(),
            "inferred local cannot lose its verified owner/address relation");
    }
  }
  for (auto level : {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3})
    for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
      check(!gti_test::emitCppText(inferredResult, standard,
                                   lang::TargetInfo::host(), level)
                 .empty(),
            "backend consumes resolved checked references");
}
} // namespace

int main() {
  pair(
      "scalar", "int",
      R"(int main() { int owner = 3; TYPE& alias = owner; return alias - 3; })",
      true);
  pair("last use", "Record",
       R"(int main() { mut Record owner{}; TYPE& alias = owner;
    int count = alias.count; mut TYPE& writable = owner; writable.count = count + 1; return writable.count - 2; })",
       true);
  pair("projected field", "int",
       R"(int main() { mut Record owner{}; mut TYPE& alias = owner.count;
    alias = 2; return alias - 2; })",
       true);
  pair("indexed element", "int",
       R"(int main() { mut int owner[2] = {1, 2}; mut TYPE& alias = owner[0];
    alias = 3; return alias - 3; })",
       true);
  pair("whole array boundary", "int[2]",
       R"(int main() { int owner[2] = {1, 2}; TYPE& alias = owner;
    return alias[0] - 1; })",
       false);
  pair("reference result", "int",
       R"(int main() { mut Owner owner{}; mut TYPE& alias = owner.field_mut();
    alias = 8; return alias - 8; })",
       true);
  pair("borrowed expect", "Owner", R"(int main() { Owner owner{};
    TYPE& alias = lookup(owner, false).expect("required"); return alias.read() - 7; })",
       true);
  pair("borrowed propagate", "Owner", R"(expected<int, int> read(Owner& owner) {
    TYPE& alias = ?lookup(owner, false); return alias.read(); }
    int main() { Owner owner{}; return read(owner).expect("read") - 7; })",
       true);
  pair("generic", "T",
       R"(T read<T>(T& owner) { TYPE& alias = owner; return alias; }
    int main() { int owner = 7; return read(owner) - 7; })",
       true);
  pair("overlapping mutation", "Record",
       R"(int main() { mut Record owner{}; TYPE& alias = owner;
    owner.count = 2; return alias.count; })",
       false);
  pair("mutable upgrade", "Record",
       R"(int main() { Record owner{}; mut TYPE& alias = owner;
    alias.count = 2; return 0; })",
       false);
  pair("read-only returned borrow", "Owner", R"(int main() { mut Owner owner{};
    mut TYPE& alias = lookup(owner, false).expect("required"); alias.set(8); return 0; })",
       false);
  pair("move borrowed owner", "Owner",
       R"(int main() { Owner owner{}; TYPE& alias = owner;
    auto moved = std::move(owner); return alias.read() + moved.read(); })",
       false);
  pair("moved owner", "Owner",
       R"(int main() { Owner owner{}; auto moved = std::move(owner);
    TYPE& alias = owner; return alias.read() + moved.read(); })",
       false);
  pair("temporary owner", "Owner",
       R"(int main() { TYPE& alias = Owner(); return alias.read(); })", false);
  pair("temporary receiver", "int",
       R"(int main() { TYPE& alias = Owner().field(); return alias; })", false);
  pair("local escape", "Owner",
       R"(Owner& escape() { Owner owner{}; TYPE& alias = owner; return alias; }
    int main() { return 0; })",
       false);
  pair("retained reborrow", "int",
       R"(int main() { mut int owner = 3; mut int& parent = owner;
    TYPE& alias = parent; return alias; })",
       true);
  pair("reborrow suspends parent", "int",
       R"(int main() { mut int owner = 3; mut int& parent = owner;
    TYPE& alias = parent; parent = 4; return alias; })",
       false);
  pair("raw pointer slot", "int*",
       R"(int main() { mut int value = 3; int* owner = &value;
    TYPE& alias = owner; unsafe { return *alias - 3; } })",
       true);
  // A fixed pointer array holds native pointer objects, not pointer slots.
  pair("raw pointer array element boundary", "int*",
       R"(int main() { int* owner[1] = {nullptr};
    TYPE& alias = owner[0]; return 0; })",
       false);
  const auto lambda = analyze(R"(int main() {
    auto function = [](int value) -> int { return value + 1; };
    auto& alias = function; auto& child = alias; return child(4) - 5;
  })");
  check(lambda.canGenerateCode(),
        "lambda references retain a checked environment");
  if (lambda.canGenerateCode()) {
    for (auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3})
      for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
        check(!gti_test::emitCppText(lambda, standard, lang::TargetInfo::host(),
                                     level)
                   .empty(),
              "checked lambda references use the materialized MIR environment");
    for (const auto &function : lambda.mir.functionInstances())
      for (const auto &loan : function.body.loans) {
        if (loan.kind != lang::MirLoanKind::Local)
          continue;
        auto invalid = lambda.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            invalid.findFunctionInstance(function.id));
        changed->body.loans[loan.id - 1].source = 0;
        check(!lang::verifyMirProgram(invalid).valid(),
              "lambda references cannot discard their owner proof");
      }
  }
  for (
      const auto &source : {
          R"(int main() { auto fn = []() -> int { return 1; };
              auto& alias = fn; auto moved = std::move(fn); return alias(); })",
          R"(int main() { auto fn = []() -> int { return 1; };
              mut auto& alias = fn; return alias(); })",
          R"(int main() { auto& alias = []() -> int { return 1; }; return alias(); })"}) {
    const auto result = analyze(source);
    check(!result.semanticValid, "lambda aliasing cannot invalidate, upgrade "
                                 "or retain a temporary environment");
  }
  for (const auto &source :
       {"int main() { int owner = 3; auto&& alias = owner; return 0; }",
        "int owner = 3; auto& alias = owner; int main() { return 0; }",
        "struct Bad { auto& alias; }; int main() { return 0; }",
        "int main() { auto& alias; return 0; }",
        "int main() { auto& alias = {1, 2}; return 0; }"}) {
    auto result = analyze(source);
    check(!result.canGenerateCode(),
          "API-visible, missing or rvalue inference is rejected");
  }
  const auto copied = analyze(declarations + R"(int main() { Owner owner{};
    auto copied = lookup(owner, false).expect("required"); return 0; })");
  bool foundHint = false;
  for (const auto &diagnostic : copied.diagnostics) {
    if (diagnostic.code != "GTI-S2003")
      continue;
    foundHint = true;
    check(diagnostic.fixes.empty(),
          "no fix silently changes ownership into borrowing");
    check(std::any_of(diagnostic.hints.begin(), diagnostic.hints.end(),
                      [](const auto &hint) {
                        return hint.find("auto&") != std::string::npos;
                      }),
          "borrowed copy error explains reference spelling");
    check(std::none_of(diagnostic.hints.begin(), diagnostic.hints.end(),
                       [](const auto &hint) {
                         return hint.find("std::move") != std::string::npos;
                       }),
          "borrowed result is not advised to move its referent");
  }
  check(foundHint,
        "plain auto still rejects move-only borrowed-result copying");
  return failures == 0 ? 0 : 1;
}
