#include "cpp_mir_body_emitter_test_fixture.h"
#include "gti/mir_lowering.h"

namespace lang {
struct CppMirRepresentationSnapshotTestAccess {
  static void reseal(CppMirRepresentationSnapshot &snapshot) {
    snapshot.inventorySeal_ = CppMirRepresentationSnapshot::InventorySeal{
        snapshot.mir, snapshot.bodies, snapshot.data, snapshot.declarationRoots,
        snapshot.thunks};
  }
};
} // namespace lang

namespace {
std::string source(std::string_view destructor) {
  return R"gti(
#include <std/string>
class Token {
public:
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
      auto rows = lang::buildCppMirBodyEmissionMapRows(
          lowered, lang::CppStandard::Cpp23);
      const auto overlay = lang::cppMirApplyGenericOwnerDestructorTypeOverlays(
          rows, lowered, *destructorInstance);
      expect(overlay && *overlay != 0,
             "valid concrete owner receives a dependent destructor overlay");
      for (int mutation = 0; mutation != 2; ++mutation) {
        auto malformed = lowered;
        if (mutation == 0) {
          auto *owner = const_cast<lang::LoweredClassInstance *>(
              malformed.findClassInstance(cleanup->classInstance));
          owner->typeArguments.front() = lang::SemanticType::Bool;
        } else {
          auto *owner = const_cast<lang::MirClassInstance *>(
              malformed.mir().findClassInstance(cleanup->classInstance));
          owner->type.valueArguments.emplace_back();
        }
        auto mutatedRows = lang::buildCppMirBodyEmissionMapRows(
            lowered, lang::CppStandard::Cpp23);
        expect(!lang::cppMirApplyGenericOwnerDestructorTypeOverlays(
                   mutatedRows, malformed, *destructorInstance),
               "overlay independently rejects mismatched concrete owner "
               "arguments");
      }
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
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const auto built =
        lang::buildCppMirRepresentationSnapshot(lowered, standard);
    expect(built.valid(),
           "retained owner needs a complete representation snapshot");
    if (!built.valid())
      continue;
    for (const auto &thunk : built.snapshot->thunks) {
      const auto *cleanup =
          std::get_if<lang::CppMirLifecycleCleanupThunk>(&thunk.payload);
      if (cleanup == nullptr ||
          cleanup->form != lang::CppMirLifecycleCleanupForm::RetainedPrimary)
        continue;
      for (const auto form :
           {lang::CppMirLifecycleCleanupForm::OrdinaryClass,
            lang::CppMirLifecycleCleanupForm::ConcreteSpecialization}) {
        auto malformed = *built.snapshot;
        for (auto &candidate : malformed.thunks)
          if (candidate.identity == thunk.identity)
            std::get<lang::CppMirLifecycleCleanupThunk>(candidate.payload)
                .form = form;
        lang::CppMirRepresentationSnapshotTestAccess::reseal(malformed);
        const auto plan =
            lang::planCppMirProgram(frontend.mir, std::move(malformed));
        expect(
            std::any_of(plan.issues.begin(), plan.issues.end(),
                        [](const auto &issue) {
                          return issue.kind ==
                                 lang::CppMirPlanIssueKind::InvalidThunkPayload;
                        }),
            "C++ planner independently rejects wrong cleanup form after "
            "resealing");
      }
    }
    try {
      const auto text = lang::CppBackend(standard).generate(lowered).contents;
      const std::string declaration = "Owner<Callable>::~Owner() noexcept";
      const auto first = text.find(declaration);
      expect(first != std::string::npos &&
                 text.find(declaration, first + 1) == std::string::npos,
             "compatible instances share exactly one dependent destructor");
      expect(
          text.find("Owner<void>") == std::string::npos,
          "unnameable closure must never become a void owner specialization");
      const std::string constructor =
          "Owner<Callable>::Owner(std::conditional_t<"
          "(!std::is_reference_v<Callable> && "
          "::gti_internal::backend::byte_transport<"
          "std::remove_cvref_t<Callable>>::value), const Callable &, Callable>";
      const auto firstConstructor = text.find(constructor);
      expect(firstConstructor != std::string::npos &&
                 text.find(constructor, firstConstructor + 1) ==
                     std::string::npos,
             "closure and nominal field publication comments do not split "
             "constructor representation");
      for (const auto &instance : lowered.mir().constructorInstances()) {
        if (instance.parameterTypes.size() != 1 ||
            (instance.parameterTypes.front().kind !=
                 lang::SemanticType::Lambda &&
             instance.parameterTypes.front().kind != lang::SemanticType::Class))
          continue;
        const std::string marker =
            "// GTI verified-MIR body: scalar-cfg-v1 constructor-instance " +
            std::to_string(instance.id) + "\n";
        const auto firstMarker = text.find(marker);
        expect(firstMarker != std::string::npos &&
                   text.find(marker, firstMarker + 1) == std::string::npos,
               "canonicalized closure and nominal publication retains every "
               "exact constructor marker once");
      }
      for (const auto &item : lowered.generatedItems()) {
        const auto *cleanup =
            std::get_if<lang::LoweredLifecycleCleanupItem>(&item.payload);
        if (cleanup == nullptr ||
            cleanup->form != lang::LoweredLifecycleCleanupForm::RetainedPrimary)
          continue;
        const std::string branch = "__gti_cleanup_instance == " +
                                   std::to_string(cleanup->classInstance);
        const auto start = text.find(branch);
        expect(start != std::string::npos,
               "source body grouping preserves each concrete aggregate cleanup "
               "branch");
        const auto *owner =
            frontend.mir.findClassInstance(cleanup->classInstance);
        if (start != std::string::npos && owner != nullptr) {
          const auto end = text.find("return sequence.success();", start);
          const auto branchText = text.substr(start, end - start);
          const bool closure =
              owner->type.arguments.front().kind == lang::SemanticType::Lambda;
          expect(branchText.find(closure
                                     ? "__gti_mir_closure_cleanup_failure"
                                     : "__gti_mir_unique_cleanup_failure<") !=
                     std::string::npos,
                 "capture and nominal field cleanup retain distinct exact "
                 "representations");
        }
      }
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      expect(false, "compatible nested owner lifecycle must emit");
    }
  }
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
  expect(lang::buildCppMirRepresentationSnapshot(lowered).valid(),
         "distinct cleanup reaches representation grouping after snapshot "
         "admission");
  bool rejected = false;
  try {
    static_cast<void>(lang::CppBackend().generate(lowered));
  } catch (const std::logic_error &error) {
    rejected = std::string_view(error.what())
                   .find("generic-owner destructor instances have incompatible "
                         "MIR representations") != std::string_view::npos;
    if (!rejected)
      std::cerr << "unexpected boundary: " << error.what() << '\n';
  }
  expect(
      rejected,
      "distinct concrete cleanup bodies cannot silently share the first body");
}

void checkDestructorCannotConsumeFieldAgain() {
  for (const auto *body : {"[[discard]] std::move(this.callable)();",
                           "[[discard]] std::move(callable)();",
                           "Callback again = std::move(this.callable);"}) {
    const std::string program = R"gti(
class Callback {
public:
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
  ~Callback() {}
  int operator()() && { return 1; }
};
class Owner {
public:
  ~Owner() {
    Callback local{};
    [[discard]] std::move(local)();
  }
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
