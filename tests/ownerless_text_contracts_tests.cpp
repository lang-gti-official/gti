#include "compiler_test_fixture.h"
#include "gti/hir.h"
#include "gti/mir_verifier.h"

namespace {
void printDiagnostics(const lang::FrontendResult &result) {
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}
lang::FrontendResult analyzeText(std::string source) {
  return lang::Frontend().analyze("ownerless-text-contracts.gti",
                                  std::move(source), {standardLibraryPrelude()},
                                  {}, {standardLibraryRoot()});
}

void testOwnerlessForwarding() {
  const auto result = analyzeText(R"gti(
#include <std/string>
#include <std/utility>
struct Label<T> {
  mut T text;
  Label(T input) : text(std::move(input)) {}
  T take() && { return std::move(this.text); }
};
Label<T> forward<T>(T input) { return later(std::move(input)); }
Label<T> later<T>(T input) { return Label<T>(std::move(input)); }
Label<std::string_view> uncalled(std::string_view input) { return later(input); }
int main() {
  auto label = forward("literal");
  auto integer = forward(42);
  return std::move(label).take() == "literal" && std::move(integer).take() == 42 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "conditional ownerless forwarding is generic and order independent");
  if (!result.canGenerateCode())
    return;
  std::size_t contracts = 0;
  for (const auto &function : result.mir.functionInstances()) {
    if (function.ownerlessTextParameters.empty())
      continue;
    ++contracts;
    auto changed = result.mir;
    auto *target = const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(function.id));
    target->ownerlessTextParameters.clear();
    expect(!lang::verifyMirProgram(changed).valid(),
           "omitted ownerless formal contract is rejected");
  }
  expect(contracts >= 2,
         "forwarding and storing functions retain requirements");
  for (const auto &function : result.hir.functionInstances()) {
    if (function.ownerlessTextParameters.empty())
      continue;
    auto changed = result.hir;
    auto *target = const_cast<lang::HirFunctionInstance *>(
        changed.findFunctionInstance(function.id));
    target->ownerlessTextContractProof.reset();
    expect(!lang::verifyHirProgramPlans(result.semantics, changed).valid(),
           "omitted semantic ownerless contract seal is rejected");
  }
}

void testOwnerlessTextAliasWrites() {
  const std::string declarations = R"gti(
#include <std/string>
#include <std/utility>
struct Label {
  mut std::string_view text;
  Label() : text("literal") {}
  mut std::string_view& slot() mut { return this.text; }
  std::string_view take() && { return this.text; }
};
void write(mut std::string_view& output, std::string_view input) { output = input; }
)gti";
  for (const char *write :
       {"label.slot() = owner.view();",
        "mut std::string_view& alias = label.slot(); alias = owner.view();",
        "write(label.slot(), owner.view());"}) {
    const auto result = analyzeText(
        declarations +
        "int main() { mut Label label{}; std::string owner{\"dynamic\"}; " +
        write + " return 0; }");
    expect(!result.canGenerateCode(),
           "dynamic text cannot enter a field through mutable aliases");
    expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [](const auto &diagnostic) {
                         return diagnostic.code == "GTI-S2045";
                       }),
           "dynamic alias writes are diagnosed at source");
    expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           "dynamic alias rejection does not defer to an internal compiler "
           "failure");
  }
  const auto literal = analyzeText(declarations + R"gti(
int main() {
  mut Label label{};
  label.slot() = "replacement";
  write(label.slot(), "another");
  return std::move(label).take() == "another" ? 0 : 1;
}
)gti");
  if (!literal.canGenerateCode())
    printDiagnostics(literal);
  expect(literal.canGenerateCode(),
         "mutable text accessors remain usable with ownerless replacement");
}

void testNativeCStringProducer() {
  const auto result = analyzeText(R"gti(
extern "C" {
  c_string native_text(int32_t kind);
  void native_release(c_string text);
}
class NativeText {
  c_string text;
public:
  unsafe NativeText(c_string input) : text(input) {}
  NativeText(NativeText&) = delete;
  NativeText(NativeText&&) = default;
  ~NativeText() { native_release(this.text); }
};
void retain(int32_t kind) {
  unsafe { auto text = NativeText(native_text(kind)); }
}
int main() { retain(0); return 0; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "native C-string results can enter an externally managed owner");
  if (!result.canGenerateCode())
    return;
  const auto producer = std::find_if(
      result.mir.functionInstances().begin(),
      result.mir.functionInstances().end(), [](const auto &function) {
        return function.linkage == lang::LanguageLinkage::C &&
               function.returnType.kind == lang::SemanticType::CString;
      });
  expect(producer != result.mir.functionInstances().end(),
         "the regression retains its native C-string producer");
  if (producer == result.mir.functionInstances().end())
    return;
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto changed = result.mir;
    auto *target = const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(producer->id));
    if (mutation == 0)
      target->linkage = lang::LanguageLinkage::Gti;
    else if (mutation == 1)
      target->definitionKind = lang::MirDefinitionKind::Source;
    else
      target->returnBorrowOrigin = lang::BorrowOriginKind::Argument;
    const auto verification = lang::verifyMirProgram(changed);
    expect(std::any_of(verification.errors.begin(), verification.errors.end(),
                       [](const auto &error) {
                         return error.message ==
                                "ownerless text storage or call lacks exact "
                                "producer evidence";
                       }),
           "native text evidence requires the exact external untracked C "
           "producer contract");
  }

  const auto dependent = analyzeText(R"gti(
#include <std/string>
class NativeText {
  c_string text;
public:
  unsafe NativeText(c_string input) : text(input) {}
  ~NativeText() {}
};
int main() {
  std::string owner{"dynamic"};
  unsafe { auto text = NativeText(owner.c_string()); }
  return 0;
}
)gti");
  expect(!dependent.canGenerateCode() &&
             std::any_of(
                 dependent.diagnostics.begin(), dependent.diagnostics.end(),
                 [](const auto &error) { return error.code == "GTI-S2045"; }) &&
             std::none_of(dependent.diagnostics.begin(),
                          dependent.diagnostics.end(),
                          [](const auto &error) {
                            return error.code.starts_with("GTI-B");
                          }),
         "GTI owner-dependent C strings still fail at source, including "
         "inside unsafe code");
}

constexpr NamedTest kTests[] = {
    {"testOwnerlessForwarding", testOwnerlessForwarding},
    {"testNativeCStringProducer", testNativeCStringProducer},
    {"testOwnerlessTextAliasWrites", testOwnerlessTextAliasWrites},
};
} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "ownerless text contracts");
}
