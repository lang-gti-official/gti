#include "gti/mir_lowering.h"

#include "llvm_backend_test_support.h"
#include "mir_test_fixture.h"

using namespace gti_test::mir_fixture;

namespace {
std::string source(std::string_view destructor) {
  return R"gti(
import std.string;
class Token {
public:
  Token() = default;
  ~Token() { std::println(7); }
  int read() { return 1; }
};
class Owner<Callable> {
  Callable callable;
public:
  Owner(Callable callable) : callable(std::move(callable)) {}
  ~Owner() { )gti" +
         std::string(destructor) + R"gti( }
  int operator()() { return this.callable(); }
};
int nest<Callable>(Callable callable) {
  Owner<Callable> inner{std::move(callable)};
  Owner<Owner<Callable>> outer{std::move(inner)};
  return 0;
}
int main() {
  Token token{};
  auto callback = [token = std::move(token)]() -> int { return token.read(); };
  return nest(std::move(callback));
}
)gti";
}

void checkCompatible(std::string_view destructor) {
  const auto frontend = analyzeWithStandardLibrary(
      "retained-generic-lifecycle.gti", source(destructor));
  expect(frontend.canGenerateCode(), "nested closure owner must verify");
  if (!frontend.canGenerateCode())
    return;
  const auto lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  std::size_t retained = 0;
  for (const auto &item : lowered.generatedItems()) {
    const auto *cleanup =
        std::get_if<lang::LoweredLifecycleCleanupItem>(&item.payload);
    if (cleanup == nullptr ||
        cleanup->form != lang::LoweredLifecycleCleanupForm::RetainedPrimary)
      continue;
    ++retained;
    const auto *destructorInstance =
        frontend.mir.findDestructorInstance(cleanup->destructorInstance);
    expect(destructorInstance != nullptr,
           "retained cleanup preserves the concrete source destructor");
    if (destructorInstance != nullptr) {
      expect(destructorInstance->mayRaiseDefinedFailure == !destructor.empty(),
             "positive fixtures exercise both raising and nonraising source "
             "cleanup");
    }
    for (const auto form :
         {lang::LoweredLifecycleCleanupForm::OrdinaryClass,
          lang::LoweredLifecycleCleanupForm::ConcreteSpecialization}) {
      auto malformed = lowered;
      auto &items = const_cast<std::vector<lang::LoweredGeneratedItem> &>(
          malformed.generatedItems());
      for (auto &candidate : items)
        if (candidate.identity == item.identity)
          std::get<lang::LoweredLifecycleCleanupItem>(candidate.payload).form =
              form;
      expect(!lang::verifyLoweredProgram(malformed).empty(),
             "lowered closure owner rejects a forged cleanup form");
    }
  }
  expect(retained == 2,
         "both closure and nested owner retain exact cleanup items");
  // Each fixture Owner constructor, for the closure and the nominal
  // instance, is its own MIR constructor body in the verified program.
  std::size_t constructors = 0;
  for (const auto &instance : lowered.mir().constructorInstances()) {
    const auto *source = frontend.hir.findConstructorInstance(instance.id);
    if (source == nullptr ||
        source->sourceUnit != frontend.sourceGraph.entryUnit() ||
        instance.parameterTypes.size() != 1 ||
        (instance.parameterTypes.front().kind != lang::SemanticType::Lambda &&
         instance.parameterTypes.front().kind != lang::SemanticType::Class))
      continue;
    ++constructors;
    expect(!instance.body.blocks.empty(),
           "each closure and nominal owner constructor has its own MIR body");
  }
  expect(constructors == 2,
         "the closure and the nested owner each select one constructor");
  expect(lang::verifyMirProgram(lowered.mir()).valid() &&
             lang::verifyLoweredProgram(lowered).empty(),
         "compatible nested owner lifecycle lowers to a verified program");
  std::string error;
  const bool emitted = gti_test::llvmEmits(
      frontend, lang::OptimizationLevel::O0, lang::TargetInfo::host(), &error);
  if (!emitted)
    std::cerr << error << '\n';
  expect(emitted, "compatible nested owner lifecycle must emit");
}

void checkDifferentBodiesFailClosed() {
  auto frontend = analyzeWithStandardLibrary(
      "retained-generic-lifecycle-distinct.gti", source("std::println(8);"));
  expect(frontend.canGenerateCode(),
         "different concrete destructor calls must verify");
  if (!frontend.canGenerateCode())
    return;
  // Change one concrete HIR body's executable literal, then lower MIR again:
  // this deliberately models different concrete generic behavior without
  // forging a stale MIR snapshot or failing an unrelated vocabulary gate.
  std::size_t changed = 0;
  for (const auto &instance : frontend.hir.destructorInstances()) {
    const auto *owner = frontend.mir.findClassInstance(instance.owner);
    if (owner == nullptr || owner->type.arguments.empty() ||
        owner->type.arguments.front().kind != lang::SemanticType::Class)
      continue;
    auto &body = const_cast<lang::HirDestructorInstance &>(instance).body;
    for (auto &value : body.values) {
      if (value.literal && *value.literal == lang::Literal{std::uint64_t{8}}) {
        value.literal = lang::Literal{std::uint64_t{9}};
        ++changed;
      }
    }
  }
  expect(changed == 1,
         "only the nested owner's source cleanup changes behavior");
  const auto loweredMir =
      lang::MirLowerer().lower(frontend.hir, frontend.failureMetadata);
  expect(loweredMir.valid() &&
             lang::verifyMirProgram(loweredMir.program).valid(),
         "distinct cleanup behavior must still have independently valid MIR");
  if (!loweredMir.valid())
    return;
  frontend.mir = loweredMir.program;
  const auto lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "distinct cleanup behavior lowers to a verified program");
  std::size_t distinct = 0;
  for (const auto &instance : lowered.mir().destructorInstances()) {
    const auto *owner = lowered.mir().findClassInstance(instance.owner);
    if (owner != nullptr && owner->type.arguments.size() == 1 &&
        (owner->type.arguments.front().kind == lang::SemanticType::Lambda ||
         owner->type.arguments.front().kind == lang::SemanticType::Class))
      ++distinct;
  }
  expect(distinct == 2,
         "each concrete owner keeps its own destructor instance");
  // The native backend emits each concrete destructor instance from its own
  // MIR body; the frozen C++ backend's refusal to group incompatible bodies
  // is checked by cpp_mir_retained_generic_lifecycle.
  std::string error;
  const bool emitted = gti_test::llvmEmits(
      frontend, lang::OptimizationLevel::O0, lang::TargetInfo::host(), &error);
  if (!emitted)
    std::cerr << error << '\n';
  expect(emitted,
         "distinct concrete cleanup bodies compile from their own MIR bodies");
}

void checkDestructorCannotConsumeFieldAgain() {
  for (const auto *body : {"[[discard]] std::move(this.callable)();",
                           "[[discard]] std::move(callable)();",
                           "Callback again = std::move(this.callable);"}) {
    const std::string program = R"gti(
class Callback {
public:
  Callback() = default;
  ~Callback() {}
  int operator()() && { return 1; }
};
class Owner {
  mut Callback callable;
public:
  Owner(Callback callable) : callable(std::move(callable)) {}
  ~Owner() { )gti" + std::string(body) +
                                R"gti( }
  int operator()() && { return std::move(this.callable)(); }
};
int main() {
  Callback callback{};
  Owner owner{std::move(callback)};
  return std::move(owner)();
}
)gti";
    const auto frontend =
        analyzeWithStandardLibrary("retained-destructor-reuse.gti", program);
    const auto fieldStart = program.find("callable", program.find("~Owner()"));
    expect(!frontend.canGenerateCode() &&
               std::any_of(frontend.diagnostics.begin(),
                           frontend.diagnostics.end(),
                           [&](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2018" &&
                                    diagnostic.message.find("partially move") !=
                                        std::string::npos &&
                                    diagnostic.phase ==
                                        lang::DiagnosticPhase::Semantics &&
                                    diagnostic.severity ==
                                        lang::DiagnosticSeverity::Error &&
                                    diagnostic.primary.source ==
                                        std::filesystem::absolute(
                                            "retained-destructor-reuse.gti")
                                            .string() &&
                                    diagnostic.primary.start == fieldStart &&
                                    diagnostic.primary.end == fieldStart + 8 &&
                                    diagnostic.fixes.empty();
                           }) &&
               std::none_of(frontend.diagnostics.begin(),
                            frontend.diagnostics.end(),
                            [](const auto &diagnostic) {
                              return diagnostic.code == "GTI-S2009" ||
                                     diagnostic.code == "GTI-B0001";
                            }),
           "a destructor cannot acquire a second consuming field invocation");
  }
  const auto local =
      analyzeWithStandardLibrary("destructor-local-move.gti", R"gti(
class Callback {
public:
  Callback() = default;
  ~Callback() {}
  int operator()() && { return 1; }
};
class Owner {
public:
  ~Owner() {
    Callback local{};
    [[discard]] std::move(local)();
  }
public:
  Owner() = default;
};
int main() { Owner owner{}; return 0; }
)gti");
  expect(local.canGenerateCode(),
         "destructor receiver protection must not prevent consuming a fresh "
         "local owner");
}
} // namespace

int main() {
  checkCompatible("std::println(8);");
  checkCompatible("");
  checkDifferentBodiesFailClosed();
  checkDestructorCannotConsumeFieldAgain();
  if (failures != 0)
    return 1;
  std::cout << "retained generic lifecycle tests passed\n";
  return 0;
}
