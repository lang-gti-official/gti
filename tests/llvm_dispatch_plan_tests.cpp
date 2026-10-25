#include "cpp_backend_test_support.h"
#include "gti/lowered_program_printer.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace {
using namespace lang;
int failures = 0;
void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

constexpr std::string_view source = R"(
interface Reader { int read(); };
interface Mirror { int read(); };
interface Scale<T> { T scale(T value); };
interface Label { int label(); };
class Base : public Reader {
public:
  Base() {}
  int read() override { return 2; }
  virtual int marker() { return 1; }
};
class Middle : public Base, public Scale<int> {
public:
  Middle() : Base() {}
  int read() override { return 3; }
  int scale(int value) override { return value; }
};
class Leaf : public Middle, public Label {
public:
  Leaf() : Middle() {}
  int marker() override { return 4; }
  int label() override { return 5; }
};
class Unified : public Reader, public Mirror {
public:
  int read() override { return 6; }
};
class Open : public Base {
public:
  virtual int pending() = 0;
};
class Generic<T> : public Scale<T> {
public:
  T scale(T value) override { return value; }
};
int through(Reader& value) { return value.read(); }
int main() {
  Leaf leaf{};
  Unified unified{};
  Generic<int> generic{};
  return through(leaf) + through(unified) + generic.scale(1);
}
)";

template <typename Program>
auto &namedClass(Program &program, const SemanticModel &semantics,
                 std::string_view name) {
  const auto &classes = program.classInstances();
  for (const auto &owner : classes) {
    const auto *declaration = semantics.findClassType(owner.declaration);
    if (declaration && declaration->qualifiedName == name) {
      using Owner =
          std::remove_const_t<std::remove_reference_t<decltype(owner)>>;
      return const_cast<Owner &>(owner);
    }
  }
  throw std::runtime_error("missing dispatch fixture class");
}

void rejectsMir(
    const FrontendResult &frontend,
    const std::function<void(MirProgram &, MirClassInstance &)> &edit,
    std::string_view message) {
  auto program = frontend.mir;
  auto &leaf = namedClass(program, frontend.semantics, "Leaf");
  edit(program, leaf);
  const auto result = verifyMirProgram(program);
  expect(!result.valid() &&
             std::any_of(result.errors.begin(), result.errors.end(),
                         [](const auto &error) {
                           return error.message.find("dispatch") !=
                                  std::string::npos;
                         }),
         message);
}

void testPlans(const FrontendResult &frontend) {
  expect(verifyHirProgramPlans(frontend.semantics, frontend.hir).valid(),
         "source selections verify in HIR");
  expect(verifyMirProgram(frontend.mir).valid(),
         "concrete hierarchy selections verify in MIR");
  const auto &leaf = namedClass(frontend.mir, frontend.semantics, "Leaf");
  expect(leaf.virtualDispatch.size() == 4,
         "multilevel leaf has each inherited and direct virtual root");
  bool inherited = false, concreteGeneric = false;
  for (const auto &row : leaf.virtualDispatch) {
    const auto *root = frontend.mir.findFunctionInstance(row.root);
    const auto *target = frontend.mir.findFunctionInstance(row.target);
    const auto *declaration =
        frontend.semantics.findFunction(root->declaration);
    if (declaration->declaration->name().lexeme == "read")
      inherited = row.rootPath.size() == 3 && row.targetPath.size() == 1 &&
                  target->owner == leaf.bases.front().instance;
    if (declaration->declaration->name().lexeme == "scale")
      concreteGeneric = root->parameterTypes ==
                            std::vector<SemanticType>{SemanticType::Int32} &&
                        row.rootPath.size() == 2 && row.targetPath.size() == 1;
  }
  expect(inherited, "inherited target retains the exact non-root owner path");
  expect(concreteGeneric, "generic interface root has its concrete signature");
  const auto &unified = namedClass(frontend.mir, frontend.semantics, "Unified");
  expect(unified.virtualDispatch.size() == 2 &&
             unified.virtualDispatch[0].root !=
                 unified.virtualDispatch[1].root &&
             unified.virtualDispatch[0].target ==
                 unified.virtualDispatch[1].target,
         "one semantic override can implement distinct interface roots");
  const auto &open = namedClass(frontend.mir, frontend.semantics, "Open");
  expect(open.abstract && std::any_of(open.virtualDispatch.begin(),
                                      open.virtualDispatch.end(),
                                      [&](const auto &row) {
                                        return frontend.mir
                                            .findFunctionInstance(row.target)
                                            ->pureVirtual;
                                      }),
         "abstract classes intentionally retain pure selections");

  auto hir = frontend.hir;
  namedClass(hir, frontend.semantics, "Leaf").virtualDispatch.front().target =
      0;
  expect(!verifyHirProgramPlans(frontend.semantics, hir).valid(),
         "HIR cannot replace the semantic-selected target");

  rejectsMir(
      frontend, [](auto &, auto &owner) { owner.virtualDispatch.pop_back(); },
      "missing root rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) {
        owner.virtualDispatch.push_back(owner.virtualDispatch.front());
      },
      "duplicate root rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) { owner.virtualDispatch.front().root = 0; },
      "unresolved root rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) { owner.virtualDispatch.front().target = 0; },
      "unresolved target rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) {
        owner.virtualDispatch.front().rootOwner = owner.id;
      },
      "mismatched root owner rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) {
        owner.virtualDispatch.front().targetOwner = 0;
      },
      "mismatched target owner rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) {
        owner.virtualDispatch.front().rootPath.clear();
      },
      "missing root base path rejects");
  rejectsMir(
      frontend,
      [](auto &, auto &owner) {
        owner.virtualDispatch.front().targetPath.push_back(owner.id);
      },
      "forged target base path rejects");
  rejectsMir(
      frontend,
      [](auto &program, auto &owner) {
        const auto target = owner.virtualDispatch.front().target;
        auto &function = const_cast<MirFunctionInstance &>(
            *program.findFunctionInstance(target));
        function.parameterTypes.push_back(SemanticType::Bool);
      },
      "mismatched target signature rejects");
  rejectsMir(
      frontend,
      [](auto &program, auto &owner) {
        const auto target = owner.virtualDispatch.front().target;
        auto &function = const_cast<MirFunctionInstance &>(
            *program.findFunctionInstance(target));
        function.definitionKind = MirDefinitionKind::Declaration;
      },
      "unimplemented concrete target rejects");
  rejectsMir(
      frontend,
      [](auto &program, auto &owner) {
        for (auto &row : owner.virtualDispatch) {
          const auto *root = program.findFunctionInstance(row.root);
          if (root->owner != row.targetOwner && !root->pureVirtual) {
            row.target = row.root;
            row.targetOwner = row.rootOwner;
            row.targetPath = row.rootPath;
            return;
          }
        }
        throw std::runtime_error("fixture needs an overridden concrete root");
      },
      "a signature-compatible ancestor cannot replace the final override");

  auto lowered = gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  const auto text = LoweredProgramPrinter().print(lowered);
  expect(text.starts_with("lowered-program-v27\n") &&
             text.find("virtual-dispatch=[{root=") != std::string::npos &&
             text == LoweredProgramPrinter().print(lowered) &&
             MirPrinter().print(frontend.mir).starts_with("mir-v115 "),
         "dispatch selection and paths serialize deterministically");
  auto &sealed = const_cast<LoweredClassInstance &>(*std::find_if(
      lowered.classInstances().begin(), lowered.classInstances().end(),
      [&](const auto &owner) { return owner.id == leaf.id; }));
  sealed.virtualDispatch.clear();
  expect(!verifyLoweredProgram(lowered).empty(),
         "sealed class instance cannot lose its dispatch vector");
}
} // namespace

int main() {
  const auto frontend =
      Frontend().analyze("llvm-dispatch-plan.gti", std::string(source));
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  testPlans(frontend);
  if (failures)
    return 1;
  std::cout << "LLVM dispatch prerequisite plans and mutations passed\n";
}
