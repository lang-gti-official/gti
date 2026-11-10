// Frozen C++ backend suite: C++ body-planner admission, template overlays,
// representation snapshots and native spellings for stored and owned
// callables. The semantic, MIR and native-emission proofs for the same
// fixtures live in stored_callable_tests.cpp.

#include "../src/compiler/cpp_mir_body_emitter.h"
#include "../src/compiler/cpp_mir_representation_snapshot.h"
#include "../src/compiler/cpp_representation.h"
#include "compiler_test_fixture.h"
#include "cpp_backend_test_support.h"
#include "gti/mir_effects.h"

int main() {
  for (const std::string policy :
       {"[[unsafe_owned]]", "[[unsafe_transfer]]", "[[unsafe_transfer(T)]]", "",
        "[[unsafe_share]]", "[[no_transfer]]"}) {
    const auto reviewed = analyzeTrustedPreludeFixture(
        "reviewed-callable-state.gti",
        "import std.string;\n" + policy + R"( class Reviewed<T> {
  T* pointer = nullptr;
public:
  Reviewed() {}
};
void retain<T>(T callback) {}
int main() {
  Reviewed<int> owner{};
  auto callback = [owner = std::move(owner)]() -> int { return 1; };
  retain(std::move(callback));
  return 0;
}
)");
    const bool permitted = policy == "[[unsafe_owned]]" ||
                           policy.find("unsafe_transfer") != std::string::npos;
    expect(reviewed.canGenerateCode() == permitted,
           "owned callable respects the exact wrapper transfer assertion: " +
               policy);
    if (permitted && reviewed.canGenerateCode())
      expect(!gti_test::emitCppText(reviewed).empty(),
             "reviewed raw-state captures reach verified MIR emission");
    if (!permitted)
      expect(hasDiagnosticCode(reviewed.diagnostics, "GTI-S2046"),
             "sharing or absence of transfer cannot authorize raw captures");
  }
  const auto sourceOwner =
      analyzeTrustedPreludeFixture("source-owner-callable.gti", R"(
import std.string;
void retain<T>(T callback) {}
int main() {
  std::string text{"owned"};
  auto callback = [text = std::move(text)]() -> int { return int(text.size()); };
  retain(std::move(callback));
  return 0;
}
)");
  expect(sourceOwner.canGenerateCode(),
         "source-owned string capture remains valid ordinary owned transport");
  if (sourceOwner.canGenerateCode())
    expect(!gti_test::emitCppText(sourceOwner).empty(),
           "source-owner capture preserves executable MIR cleanup");
  const auto copiedCallable =
      analyzeTrustedPreludeFixture("copied-owned-callable.gti", R"(
void retain<T>(T callback) {}
class Holder<T> {
  T callback;
public:
  Holder(T callback) : callback(std::move(callback)) {}
  void copy_twice() { retain(this.callback); retain(this.callback); }
};
void exercise<T>(T callback) {
  retain(callback);
  Holder<T> holder{std::move(callback)};
  holder.copy_twice();
}
int main() {
  int value = 3;
  auto callback = [value]() -> int { return value; };
  exercise(std::move(callback));
  auto second = [value]() -> int { return value; };
  exercise(std::move(second));
  return 0;
}
)");
  expect(copiedCallable.canGenerateCode(),
         "copyable locals and fields can initialize independent owned callable "
         "arguments without consuming the source");
  if (!copiedCallable.canGenerateCode()) {
    for (const auto &diagnostic : copiedCallable.diagnostics)
      std::cerr << diagnostic.message << '\n';
  }
  if (copiedCallable.canGenerateCode()) {
    const auto generated = gti_test::emitCppText(copiedCallable);
    std::size_t closureConstructors = 0;
    for (const auto &instance : copiedCallable.mir.constructorInstances()) {
      if (instance.parameterTypes.size() != 1 ||
          instance.parameterTypes.front().kind != lang::SemanticType::Lambda)
        continue;
      ++closureConstructors;
      const std::string marker =
          "// GTI verified-MIR body: scalar-cfg-v1 constructor-instance " +
          std::to_string(instance.id) + "\n";
      const auto first = generated.find(marker);
      expect(first != std::string::npos &&
                 generated.find(marker, first + 1) == std::string::npos,
             "coalesced constructor text retains each exact verified instance "
             "marker once");
    }
    expect(closureConstructors == 2,
           "distinct passive closures share a constructor representation");
  }
  const auto clonePath = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/stored_callable_clone.gti";
  std::ifstream cloneInput(clonePath);
  expect(cloneInput.good(), "owning callback clone fixture exists");
  const std::string cloneSource{std::istreambuf_iterator<char>(cloneInput), {}};
  const auto clone = analyzeTrustedPreludeFixture("clone.gti", cloneSource);
  expect(clone.canGenerateCode(), "erased callback clone reaches valid MIR");
  if (!clone.canGenerateCode()) {
    for (const auto &diagnostic : clone.diagnostics)
      std::cerr << diagnostic.message << '\n';
  }
  if (clone.canGenerateCode()) {
    const auto lowered = gti_test::lowerProgram(clone, clone.mir, clone.mir);
    const auto snapshot = lang::buildCppMirRepresentationSnapshot(
        lowered, lang::CppStandard::Cpp23);
    expect(snapshot.snapshot.has_value(),
           "clone, reset, replacement and constructor-local closure storage "
           "have complete representations");
    bool checkedLocalClosure = false;
    for (const auto &instance : lowered.mir().constructorInstances()) {
      const auto *source = lowered.findConstructorInstance(instance.id);
      const auto *declaration =
          source == nullptr
              ? nullptr
              : lowered.findConstructorDeclaration(source->declaration);
      if (declaration == nullptr || declaration->genericParameters.empty() ||
          instance.typeArguments.empty() ||
          instance.typeArguments.front().kind != lang::SemanticType::Lambda)
        continue;
      auto rows = lang::buildCppMirBodyEmissionMapRows(
          lowered, lang::CppStandard::Cpp23);
      const auto count = lang::cppMirApplyGenericOwnerConstructorTypeOverlays(
          rows, lowered, *declaration, instance);
      expect(count && *count != 0,
             "constructor-local closure types receive exact template overlays");
      checkedLocalClosure = true;
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto forged = instance;
        if (mutation == 0)
          forged.owner = 0;
        else if (mutation == 1)
          forged.typeArguments.clear();
        else
          forged.parameterTypes.clear();
        auto forgedRows = lang::buildCppMirBodyEmissionMapRows(
            lowered, lang::CppStandard::Cpp23);
        expect(!lang::cppMirApplyGenericOwnerConstructorTypeOverlays(
                   forgedRows, lowered, *declaration, forged),
               "constructor overlays reject mismatched owners, substitutions, "
               "and signatures");
      }
    }
    expect(checkedLocalClosure,
           "callback construction exercises a constructor-local closure");
  }
  const auto erasedVoidFixture = std::filesystem::path(__FILE__).parent_path() /
                                 "fixtures/stored_callable_signature_void.gti";
  std::ifstream erasedVoidInput(erasedVoidFixture);
  expect(erasedVoidInput.good(), "erased void signature fixture exists");
  const std::string erasedVoidSource{
      std::istreambuf_iterator<char>(erasedVoidInput), {}};
  const auto erasedVoid = analyzeTrustedPreludeFixture(
      "erased-void-signature.gti", erasedVoidSource);
  expect(erasedVoid.canGenerateCode(),
         "concrete overrides match empty and multi-element inherited packs");
  if (erasedVoid.canGenerateCode()) {
    const auto lowered =
        gti_test::lowerProgram(erasedVoid, erasedVoid.mir, erasedVoid.mir);
    const auto rows =
        lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23);
    std::size_t checkedOverrides = 0;
    for (std::size_t index = 0; index < rows.bodies.size(); ++index) {
      const auto &row = rows.bodies[index];
      if (row.address.kind != lang::MirBodyKind::Function)
        continue;
      const auto *function =
          lowered.mir().findFunctionInstance(row.address.owner);
      if (function == nullptr || !function->overrideMethod ||
          function->returnType != lang::SemanticType::Void)
        continue;
      ++checkedOverrides;
      expect(row.voidFailureResultParameter,
             "void overrides preserve their symbolic root's result channel");
      auto forgedRows = rows;
      forgedRows.bodies[index].voidFailureResultParameter = false;
      const lang::CppMirBodyEmissionMap forged(std::move(forgedRows));
      const auto analysis =
          lang::CppMirBodyEmitter(lowered.mir(), forged).analyze(row.address);
      expect(std::any_of(analysis.issues.begin(), analysis.issues.end(),
                         [](const auto &issue) {
                           return issue.kind ==
                                      lang::CppMirBodyEmissionIssueKind::
                                          InvalidRepresentationRow &&
                                  issue.detail.find("status-result channel") !=
                                      std::string::npos;
                         }),
             "a forged virtual result-channel mismatch is rejected");
    }
    expect(checkedOverrides >= 3,
           "void ABI tests cover erased and concrete overrides");
    expect(lang::buildCppMirRepresentationSnapshot(lowered,
                                                   lang::CppStandard::Cpp23)
               .valid(),
           "erased void signatures have a coherent representation snapshot");
  }
  const auto voidFixture = std::filesystem::path(__FILE__).parent_path() /
                           "fixtures/void_return_expression.gti";
  std::ifstream voidInput(voidFixture);
  expect(voidInput.good(), "void return cleanup fixture exists");
  const std::string voidSource{std::istreambuf_iterator<char>(voidInput), {}};
  const auto voidCleanup =
      analyzeTrustedPreludeFixture("void-return-cleanup.gti", voidSource);
  expect(voidCleanup.canGenerateCode(),
         "void return cleanup and failure reach verified MIR");
  if (voidCleanup.canGenerateCode()) {
    const auto lowered =
        gti_test::lowerProgram(voidCleanup, voidCleanup.mir, voidCleanup.mir);
    expect(lang::buildCppMirRepresentationSnapshot(lowered,
                                                   lang::CppStandard::Cpp23)
               .snapshot.has_value(),
           "a copied passive closure retains a materialized type for explicit "
           "generic pack arguments");
  }
  const auto polymorphicFixture =
      std::filesystem::path(__FILE__).parent_path() /
      "fixtures/polymorphic_owner_cleanup.gti";
  std::ifstream polymorphicInput(polymorphicFixture);
  expect(polymorphicInput.good(), "polymorphic cleanup fixture exists");
  const std::string polymorphicSource{
      std::istreambuf_iterator<char>(polymorphicInput), {}};
  const auto polymorphic = analyzeTrustedPreludeFixture(
      "polymorphic-cleanup.gti", polymorphicSource);
  expect(polymorphic.canGenerateCode(),
         "polymorphic cleanup reaches verified MIR");
  std::size_t constructedFields = 0;
  for (const auto &constructor : polymorphic.mir.constructorInstances()) {
    const auto *owner = polymorphic.mir.findClassInstance(constructor.owner);
    if (owner == nullptr || owner->bases.empty())
      continue;
    const auto fields = lang::cppMirConstructedFieldInitializerBindings(
        polymorphic.mir, constructor);
    if (fields.empty())
      continue;
    constructedFields += fields.size();
    auto forged = polymorphic.mir;
    auto *base = const_cast<lang::MirClassInstance *>(
        forged.findClassInstance(owner->bases.front().instance));
    base->requiresActiveCleanup = true;
    expect(lang::cppMirConstructedFieldInitializerBindings(forged, constructor)
               .empty(),
           "field construction cannot hoist across an unproved active base");
  }
  expect(constructedFields == 2,
         "passive interface bases preserve the two exact field constructions");
  const auto signatureFixture = std::filesystem::path(__FILE__).parent_path() /
                                "fixtures/stored_callable_signature.gti";
  std::ifstream signatureInput(signatureFixture);
  expect(signatureInput.good(), "stored signature fixture exists");
  const std::string signatureSource{
      std::istreambuf_iterator<char>(signatureInput), {}};
  const auto signatureModel = analyzeTrustedPreludeFixture(
      "stored-callable-signature.gti", signatureSource);
  expect(signatureModel.canGenerateCode(),
         "a signature-based erased model reaches verified MIR");
  if (signatureModel.canGenerateCode()) {
    const auto lowered = gti_test::lowerProgram(
        signatureModel, signatureModel.mir, signatureModel.mir);
    const auto snapshot = lang::buildCppMirRepresentationSnapshot(
        lowered, lang::CppStandard::Cpp23);
    expect(snapshot.snapshot.has_value(),
           "owner packs and staged field invocation retain native "
           "representations");
    bool checkedOwnerPack = false;
    for (const auto &instance : lowered.mir().constructorInstances()) {
      const auto *source = lowered.findConstructorInstance(instance.id);
      const auto *owner = lowered.findClassInstance(instance.owner);
      const auto *declaration =
          source == nullptr
              ? nullptr
              : lowered.findConstructorDeclaration(source->declaration);
      const auto *ownerDeclaration =
          owner == nullptr ? nullptr
                           : lowered.findClassDeclaration(owner->declaration);
      if (declaration == nullptr || ownerDeclaration == nullptr ||
          std::none_of(ownerDeclaration->genericParameters.begin(),
                       ownerDeclaration->genericParameters.end(),
                       [](const auto &parameter) { return parameter.pack; }))
        continue;
      auto rows = lang::buildCppMirBodyEmissionMapRows(
          lowered, lang::CppStandard::Cpp23);
      const auto overlays =
          lang::cppMirApplyGenericOwnerConstructorTypeOverlays(
              rows, lowered, *declaration, instance);
      if (!overlays || *overlays == 0)
        continue;
      checkedOwnerPack = true;
      auto forged = instance;
      forged.owner = 0;
      expect(!lang::cppMirApplyGenericOwnerConstructorTypeOverlays(
                 rows, lowered, *declaration, forged),
             "owner-pack projection rejects a mismatched constructor owner");
    }
    expect(checkedOwnerPack,
           "the erased model exercises its owner-pack constructor projection");
  }
  const std::string erasedSource = R"(
interface Invoker { int invoke(int value); };
class Model<T> : public Invoker {
  T callback;
public:
  Model(T callback) : callback(std::move(callback)) {}
  int invoke(int value) override { return this.callback(value); }
};
std::unique_ptr<Invoker> erase<T>(T callback) {
  auto model = std::make_unique<Model<T>>(std::move(callback));
  auto result = std::upcast_unique<Invoker, Model<T>>(std::move(model));
  return std::move(result);
}
int main() {
  int offset = 7;
  auto callback = [offset](int value) -> int { return offset + value; };
  auto owner = erase(std::move(callback));
  return owner->invoke(5) - 12;
}
 )";
  std::string activeErasedSource = R"(
class Capture {
  int value;
public:
  Capture(int value) : value(value) {}
  ~Capture() { int checked = 1 / this.value; }
  int read() { return this.value; }
};
)" + erasedSource;
  const std::string scalarLocal = "int offset = 7;";
  activeErasedSource.replace(activeErasedSource.find(scalarLocal),
                             scalarLocal.size(), "Capture offset{7};");
  const std::string scalarCapture =
      "[offset](int value) -> int { return offset + value; }";
  activeErasedSource.replace(activeErasedSource.find(scalarCapture),
                             scalarCapture.size(),
                             "[offset = std::move(offset)](int value) -> int { "
                             "return offset.read() + value; }");
  const auto activeErased = analyzeTrustedMemoryFixture(
      "active-erased-callable.gti", activeErasedSource);
  expect(activeErased.canGenerateCode(),
         "active erased model retains verified ownership and cleanup");
  bool checkedReturnCleanup = false;
  if (activeErased.canGenerateCode()) {
    const auto lowered = gti_test::lowerProgram(activeErased, activeErased.mir,
                                                activeErased.mir);
    for (const auto &instance : lowered.mir().functionInstances()) {
      if (instance.typeArguments.size() != 1 ||
          instance.typeArguments.front().kind != lang::SemanticType::Lambda ||
          instance.returnType.kind != lang::SemanticType::Class ||
          instance.returnType.arguments.size() != 1 ||
          instance.returnType.arguments.front().kind !=
              lang::SemanticType::Class ||
          !instance.returnType.arguments.front().arguments.empty())
        continue;
      const auto *declaration =
          lowered.findFunctionDeclaration(instance.declaration);
      auto rows = lang::buildCppMirBodyEmissionMapRows(
          lowered, lang::CppStandard::Cpp23);
      expect(declaration != nullptr &&
                 lang::cppMirApplyCallableTemplateTypeOverlays(
                     rows, lowered, lang::CppStandard::Cpp23, *declaration,
                     instance)
                     .has_value(),
             "active erasure factory retains its exact template overlay");
      const lang::CppMirBodyEmissionMap map(std::move(rows));
      expect(lang::CppMirBodyEmitter(lowered.mir(), map)
                 .supportsFailureBodyText({.kind = lang::MirBodyKind::Function,
                                           .owner = instance.id}),
             "return publication admits cleanup of multiple proven moved-from "
             "owners");
      checkedReturnCleanup = true;
    }
  }
  expect(checkedReturnCleanup,
         "active erasure regression checks the returning factory body");
  const auto heap = analyzeTrustedMemoryFixture("heap-callable.gti", R"(
class HeapModel<T> {
  T callback;
public:
  HeapModel(T callback) : callback(std::move(callback)) {}
  int invoke(int value) { return this.callback(value); }
};
std::unique_ptr<HeapModel<T>> heap_own<T>(T callback) {
  return std::make_unique<HeapModel<T>>(std::move(callback));
}
int main() {
  int offset = 7;
  auto callback = [offset](int value) -> int { return offset + value; };
  auto owner = heap_own(std::move(callback));
  return owner->invoke(5) - 12;
}
)");
  expect(heap.canGenerateCode(), "heap callback model reaches verified MIR");
  if (heap.canGenerateCode()) {
    const auto lowered = gti_test::lowerProgram(heap, heap.mir, heap.mir);
    const auto snapshot = lang::buildCppMirRepresentationSnapshot(
        lowered, lang::CppStandard::Cpp23);
    expect(snapshot.snapshot.has_value(),
           "heap callback factory and reference result have complete "
           "representations");
    bool allocatedTypeOverlay = false;
    for (const auto &instance : lowered.mir().functionInstances()) {
      const auto *declaration =
          lowered.findFunctionDeclaration(instance.declaration);
      if (declaration == nullptr || instance.typeArguments.empty() ||
          instance.typeArguments.front().kind != lang::SemanticType::Class ||
          instance.typeArguments.front().arguments.empty() ||
          instance.typeArguments.front().arguments.front().kind !=
              lang::SemanticType::Lambda)
        continue;
      auto rows = lang::buildCppMirBodyEmissionMapRows(
          lowered, lang::CppStandard::Cpp23);
      const auto count = lang::cppMirApplyCallableTemplateTypeOverlays(
          rows, lowered, lang::CppStandard::Cpp23, *declaration, instance);
      if (!count)
        continue;
      for (const auto &row : rows.types)
        if (row.type == instance.typeArguments.front() && row.bodyTypeOverlay)
          allocatedTypeOverlay = true;
      auto mismatchedInstance = instance;
      mismatchedInstance.typeArguments.front() = lang::SemanticType::Int32;
      auto forgedRows = lang::buildCppMirBodyEmissionMapRows(
          lowered, lang::CppStandard::Cpp23);
      expect(!lang::cppMirApplyCallableTemplateTypeOverlays(
                 forgedRows, lowered, lang::CppStandard::Cpp23, *declaration,
                 mismatchedInstance),
             "allocation overlays reject a forged concrete type argument");
    }
    expect(allocatedTypeOverlay,
           "explicit generic allocation type retains its body-scoped name");
  }
  const auto mixedAllocations =
      analyzeTrustedMemoryFixture("mixed-callable-allocation.gti", R"(
class First<T> {
  T callback;
public:
  First(T callback) : callback(std::move(callback)) {}
  int call(int value) { return this.callback(value); }
};
class Second<T> {
  T callback;
public:
  Second(T callback) : callback(std::move(callback)) {}
  int call(int value) { return this.callback(value); }
};
std::unique_ptr<First<T>> first<T>(T callback) {
  return std::make_unique<First<T>>(std::move(callback));
}
std::unique_ptr<Second<T>> second<T>(T callback) {
  return std::make_unique<Second<T>>(std::move(callback));
}
int main() {
  int offset = 7;
  auto left = [offset](int value) -> int { return offset + value; };
  auto right = [offset](int value) -> int { return offset - value; };
  auto one = first(std::move(left));
  auto two = second(std::move(right));
  return one->call(5) + two->call(3) - 16;
}
)");
  expect(mixedAllocations.canGenerateCode(),
         "different allocated callback models retain concrete MIR");
  if (mixedAllocations.canGenerateCode()) {
    const auto lowered = gti_test::lowerProgram(
        mixedAllocations, mixedAllocations.mir, mixedAllocations.mir);
    const auto rows =
        lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23);
    const std::string generated = gti_test::emitCppText(mixedAllocations);
    std::vector<std::pair<lang::FunctionId, std::string>> identities;
    bool repeatedDeclaration = false;
    for (const auto &row : lowered.declarations()) {
      const auto *function =
          std::get_if<lang::LoweredFunctionDeclaration>(&row.payload);
      if (function == nullptr)
        continue;
      for (const auto &instance : lowered.mir().functionInstances()) {
        if (instance.declaration != function->id)
          continue;
        const auto *concrete = lowered.findFunctionInstance(instance.id);
        if (concrete == nullptr)
          continue;
        const auto spelling = lang::cppCallableFreeFunctionSpelling(
            lowered, row, *function, *concrete, instance);
        if (!spelling)
          continue;
        for (const auto &[declaration, prior] : identities) {
          expect(
              prior != *spelling,
              "distinct callable MIR instances have distinct native symbols");
          repeatedDeclaration |= declaration == function->id;
        }
        identities.emplace_back(function->id, *spelling);
        expect(
            generated.find(*spelling) != std::string::npos &&
                std::any_of(rows.bodies.begin(), rows.bodies.end(),
                            [&](const auto &body) {
                              return body.address.kind ==
                                         lang::MirBodyKind::Function &&
                                     body.address.owner == instance.id &&
                                     body.spelling.find(*spelling) !=
                                         std::string::npos;
                            }),
            "native declarations and call rows retain the same instance name");
        auto forged = instance;
        forged.returnType = lang::SemanticType::Int32;
        expect(!lang::cppCallableFreeFunctionSpelling(lowered, row, *function,
                                                      *concrete, forged),
               "callable instance naming rejects a forged concrete signature");
        auto wrongIdentity = *concrete;
        wrongIdentity.id = 0;
        expect(!lang::cppCallableFreeFunctionSpelling(lowered, row, *function,
                                                      wrongIdentity, instance),
               "callable instance naming rejects a missing lowered identity");
      }
    }
    expect(repeatedDeclaration, "allocation regression covers different "
                                "instances of the same generic declaration");
  }
  const std::string packSource = R"(
class Token {
  int value;
public:
  Token(int value) : value(value) {}
  ~Token() { int checked = 1 / this.value; }
  int read() { return this.value; }
};
void accept<Args...>(int prefix, Args... args) {}
void relay<Args...>(Args... args) { accept(0, args...); }
int main() {
  relay();
  int offset = 7;
  auto single = [offset]() -> int { return offset; };
  relay(std::move(single));
  auto passive_left = [offset]() -> int { return offset; };
  auto passive_right = [offset]() -> int { return offset; };
  relay(std::move(passive_left), 7, std::move(passive_right));
  Token first{1};
  Token second{2};
  auto left = [first = std::move(first)]() -> int { return first.read(); };
  auto right = [second = std::move(second)]() -> int { return second.read(); };
  relay(std::move(left), 7, std::move(right));
  accept(0);
  return 0;
}
)";
  const auto pack = analyzeTrustedPreludeFixture("owned-pack.gti", packSource);
  expect(pack.canGenerateCode(),
         "owned callable pack forwarding reaches verified MIR");
  if (pack.canGenerateCode()) {
    expect(!gti_test::emitCppText(pack).empty(),
           "free callable instances with different pack arities and cleanup "
           "shapes emit independently from their exact MIR bodies");
  }
  const auto frontend = analyzeTrustedPreludeFixture("stored-callable.gti", R"(
class Owner<T> {
  T value;
public:
  Owner(T value) : value(std::move(value)) {}
  int call(int argument) { return this.value(argument); }
};
Owner<T> own<T>(T value) { return Owner<T>(std::move(value)); }
int main() {
  int offset = 7;
  auto operation = [offset](int value) -> int { return offset + value; };
  auto owner = own(std::move(operation));
  return owner.call(5) - 12;
}
)");
  expect(frontend.canGenerateCode(), "stored invocation reaches concrete MIR");
  if (frontend.canGenerateCode()) {
    const auto lowered =
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
    auto rows =
        lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23);
    lang::CppMirBodyEmissionMap map(rows);
    const auto analysis = lang::CppMirBodyEmitter(lowered.mir(), map)
                              .buildProgramPlan()
                              .analysis();
    for (const auto &body : analysis.bodies) {
      if (body.ready()) {
        continue;
      }
      if (body.body.kind == lang::MirBodyKind::Function) {
        const auto *instance =
            lowered.mir().findFunctionInstance(body.body.owner);
        const auto *decl =
            lowered.findFunctionDeclaration(instance->declaration);
        auto overlayRows = rows;
        const auto count = lang::cppMirApplyCallableTemplateTypeOverlays(
            overlayRows, lowered, lang::CppStandard::Cpp23, *decl, *instance);
        expect(count && *count != 0,
               "unnameable function has a resolved template overlay");
        lang::CppMirBodyEmissionMap overlay(std::move(overlayRows));
        const auto plan = lang::CppMirBodyEmitter(lowered.mir(), overlay)
                              .buildPlan(body.body);
        expect(
            plan.analysis().ready() &&
                (plan.supportsPlainText() || plan.supportsFailureText()),
            "stored receiver invocation is emitted from its verified MIR body");
      }
    }
    const auto snapshot = lang::buildCppMirRepresentationSnapshot(
        lowered, lang::CppStandard::Cpp23);
    expect(snapshot.snapshot.has_value(),
           "stored invocation has a complete MIR representation");
  }
  // A closure copied from its constructor parameter into a generic owner's
  // field has no explicit MIR Initialize: the constructor record publishes
  // the parameter's copy Load. The copy-parameter proof binds that exact Load
  // to a native member initializer for scalar-, record- and capture-free
  // closures alike, and every instance keeps its siblings body-staged so the
  // shared constructor template has one member-initializer list.
  const auto copiedField =
      analyzeTrustedPreludeFixture("copied-closure-field.gti", R"(
import std.string;
import std.utility;
struct Offset {
  int amount;
  Offset(int amount) : amount(amount) {}
};
class Holder<F> {
public:
  F callback;
  int value;
  Holder(F f) : callback(f), value(10) {}
  int invoke() { return callback(); }
};
int use<F>(F f) {
  Holder<F> h = Holder<F>(f);
  return h.value + h.invoke();
}
class Named<F> {
public:
  F callback;
  std::string name;
  Named(F f, std::string name) : callback(f), name(std::move(name)) {}
  int invoke() { return callback(); }
};
int named<F>(F f) {
  std::string label{"ab"};
  Named<F> n = Named<F>(f, std::move(label));
  return n.invoke();
}
int main() {
  int k = 3;
  Offset offset{4};
  int empty = use([]() -> int { return 1; });
  int scalar = use([k]() -> int { return k; });
  int record = use([offset]() -> int { return offset.amount; });
  int owned = named([k]() -> int { return k; }) +
              named([offset]() -> int { return offset.amount; });
  return empty + scalar + record + owned - 45;
}
)");
  expect(copiedField.canGenerateCode(), "copied closure fields verify");
  if (copiedField.canGenerateCode()) {
    expect(!gti_test::emitCpp(copiedField).contents.empty(),
           "copied closure fields reach native emission");
    const auto lowered =
        gti_test::lowerProgram(copiedField, copiedField.mir, copiedField.mir);
    std::size_t copiedClosures = 0;
    for (const auto &instance : lowered.mir().constructorInstances()) {
      if (instance.parameterTypes.size() != 1 ||
          instance.parameterTypes.front().kind != lang::SemanticType::Lambda)
        continue;
      ++copiedClosures;
      const auto bindings =
          lang::cppMirCopyParameterFieldBindings(lowered.mir(), instance);
      expect(bindings && bindings->size() == 1 &&
                 bindings->front().initializer == 0 &&
                 bindings->front().initializeInstruction == 0,
             "the record-published closure copy binds to its field");
      expect(lang::cppMirConstructedFieldInitializerBindings(lowered.mir(),
                                                             instance)
                 .empty(),
             "closure instances keep sibling fields body-staged");
      const auto *source = lowered.findConstructorInstance(instance.id);
      const auto *declaration =
          source == nullptr
              ? nullptr
              : lowered.findConstructorDeclaration(source->declaration);
      auto rows = lang::buildCppMirBodyEmissionMapRows(
          lowered, lang::CppStandard::Cpp23);
      const auto overlays =
          declaration == nullptr
              ? std::nullopt
              : lang::cppMirApplyGenericOwnerConstructorTypeOverlays(
                    rows, lowered, *declaration, instance);
      expect(overlays && *overlays != 0,
             "the closure type spells through the constructor overlay");
      const lang::CppMirBodyEmissionMap map(std::move(rows));
      const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                         instance.id};
      expect(
          lang::CppMirBodyEmitter(lowered.mir(), map).supportsBodyText(address),
          "the copied closure constructor body is in the C++ vocabulary");
      if (!bindings || bindings->size() != 1)
        continue;
      const auto &binding = bindings->front();
      const auto *lambda =
          lowered.mir().findLambdaForType(instance.parameterTypes.front());
      const bool recordCapture =
          lambda != nullptr && !lambda->captureTypes.empty() &&
          lambda->captureTypes.front().kind == lang::SemanticType::Class;
      for (int mutation = 0; mutation != 6; ++mutation) {
        if (mutation == 5 && !recordCapture)
          continue;
        auto forged = lowered.mir();
        auto *body = lang::findMirBody(forged, address);
        for (auto &block : body->blocks)
          for (auto &candidate : block.instructions)
            if (mutation == 0 && candidate.id == binding.loadInstruction)
              candidate.operands.front().kind = lang::MirOperandKind::Move;
        for (auto &value : body->values) {
          if (value.id != binding.loadedValue)
            continue;
          if (mutation == 1)
            value.info.traits.copyable = false;
          if (mutation == 2)
            value.info.traits.containsBorrowedState = true;
        }
        auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
            forged.classInstances());
        for (auto &type : classes) {
          if (mutation == 3 && type.id == instance.owner)
            type.declaredFields.front().requiresActiveCleanup = true;
          if (mutation == 5 && type.type == lambda->captureTypes.front())
            type.copyConstructor = lang::SpecialMemberStatus::Deleted;
        }
        auto *changed = const_cast<lang::MirConstructorInstance *>(
            forged.findConstructorInstance(instance.id));
        if (mutation == 4)
          changed->initializers.front().arguments.front() = 0;
        const auto rejected =
            lang::cppMirCopyParameterFieldBindings(forged, *changed);
        expect(!rejected || rejected->empty(),
               "a closure field copy rejects a moved or noncopyable value, "
               "borrowed state, active field cleanup, a lost record and a "
               "capture without a generated copy " +
                   std::to_string(mutation));
      }
    }
    expect(copiedClosures == 3,
           "three closure instances share one copying constructor");
    // A following owned field still moves in the native member-initializer
    // list: the closure copy's exact Load and boundary are an effect-free
    // prefix of the owned move, like a passive scalar sibling.
    std::size_t ownedSiblings = 0;
    for (const auto &instance : lowered.mir().constructorInstances()) {
      if (instance.parameterTypes.size() != 2 ||
          instance.parameterTypes.front().kind != lang::SemanticType::Lambda)
        continue;
      ++ownedSiblings;
      const auto copies =
          lang::cppMirCopyParameterFieldBindings(lowered.mir(), instance);
      const auto moves =
          lang::cppMirOwnedParameterFieldBindings(lowered.mir(), instance);
      expect(copies && copies->size() == 1 &&
                 copies->front().initializeInstruction == 0 && moves &&
                 moves->size() == 1 && moves->front().initializer == 1,
             "a closure copy prefix admits the following owned move");
      if (!copies || copies->size() != 1)
        continue;
      auto forged = lowered.mir();
      auto *body = lang::findMirBody(
          forged, {lang::MirBodyKind::Constructor, instance.id});
      auto &instructions = body->findBlock(body->entry)->instructions;
      const auto load = std::find_if(
          instructions.begin(), instructions.end(), [&](const auto &candidate) {
            return candidate.id == copies->front().loadInstruction;
          });
      expect(load != instructions.end() &&
                 std::next(load) != instructions.end(),
             "the closure copy Load precedes its boundary");
      if (load == instructions.end() || std::next(load) == instructions.end())
        continue;
      std::iter_swap(load, std::next(load));
      const auto *changed = forged.findConstructorInstance(instance.id);
      const auto rejected =
          lang::cppMirOwnedParameterFieldBindings(forged, *changed);
      expect(!rejected || rejected->empty(),
             "a reordered closure copy prefix cannot precede the owned move");
    }
    expect(ownedSiblings == 2,
           "both closure instances move their owned sibling natively");
  }
  // A closure-typed class constructed as another one's member initializer,
  // `inner(Holder<F>(f))`, publishes natively: the constructed-field proof
  // copies or moves the passive closure formal into that construction, and
  // the planner spells the field by its own declared type.
  const auto constructedField =
      analyzeTrustedPreludeFixture("constructed-closure-field.gti", R"(
import std.utility;
struct Offset {
  int amount;
  Offset(int amount) : amount(amount) {}
};
class Holder<F> {
public:
  F callback;
  Holder(F f) : callback(f) {}
  int invoke() { return callback(); }
};
class Moved<F> {
public:
  F callback;
  Moved(F f) : callback(std::move(f)) {}
  int invoke() { return callback(); }
};
class Outer<F> {
public:
  Holder<F> inner;
  int extra;
  Outer(F f) : inner(Holder<F>(f)), extra(1) {}
};
class MovingOuter<F> {
public:
  Moved<F> inner;
  int extra;
  MovingOuter(F f) : inner(Moved<F>(std::move(f))), extra(2) {}
};
int copied<F>(F f) {
  Outer<F> value = Outer<F>(f);
  return value.inner.invoke() + value.extra;
}
int moved<F>(F f) {
  MovingOuter<F> value = MovingOuter<F>(std::move(f));
  return value.inner.invoke() + value.extra;
}
int main() {
  int k = 3;
  Offset offset{4};
  return copied([k]() -> int { return k; }) +
         copied([offset]() -> int { return offset.amount; }) +
         moved([k]() -> int { return k; }) +
         moved([offset]() -> int { return offset.amount; }) - 20;
}
)");
  expect(constructedField.canGenerateCode(),
         "constructed closure fields verify");
  if (constructedField.canGenerateCode()) {
    const auto text = gti_test::emitCppText(constructedField);
    expect(text.find("inner(std::remove_cv_t<decltype(inner)>("
                     "__gti_mir_arg_0))") != std::string::npos &&
               text.find("inner(std::remove_cv_t<decltype(inner)>(std::move("
                         "__gti_mir_arg_0)))") != std::string::npos,
           "a closure-typed constructed field spells its declared type");
    const auto lowered = gti_test::lowerProgram(
        constructedField, constructedField.mir, constructedField.mir);
    std::size_t copiedFields = 0;
    std::size_t movedFields = 0;
    std::size_t mutations = 0;
    for (const auto &instance : lowered.mir().constructorInstances()) {
      const auto *owner = lowered.mir().findClassInstance(instance.owner);
      if (owner == nullptr || owner->declaredFields.size() != 2 ||
          instance.parameterTypes.size() != 1 ||
          instance.parameterTypes.front().kind != lang::SemanticType::Lambda)
        continue;
      const auto bindings = lang::cppMirConstructedFieldInitializerBindings(
          lowered.mir(), instance);
      expect(bindings.size() == 2 && bindings.front().arguments.size() == 1 &&
                 bindings.front().arguments.front().parameter == 0 &&
                 bindings.front().argumentInstructions.size() == 2 &&
                 !bindings.front().argumentValues.empty(),
             "the closure formal reaches the native field construction");
      if (bindings.size() != 2 || bindings.front().arguments.size() != 1 ||
          bindings.front().argumentInstructions.size() != 2)
        continue;
      const auto &binding = bindings.front();
      const bool movedArgument = binding.arguments.front().moved;
      ++(movedArgument ? movedFields : copiedFields);
      const auto *source = lowered.findConstructorInstance(instance.id);
      const auto *declaration =
          source == nullptr
              ? nullptr
              : lowered.findConstructorDeclaration(source->declaration);
      auto rows = lang::buildCppMirBodyEmissionMapRows(
          lowered, lang::CppStandard::Cpp23);
      const auto overlays =
          declaration == nullptr
              ? std::nullopt
              : lang::cppMirApplyGenericOwnerConstructorTypeOverlays(
                    rows, lowered, *declaration, instance);
      expect(overlays && *overlays != 0,
             "the outer constructor spells F through its overlay");
      const lang::CppMirBodyEmissionMap map(std::move(rows));
      const lang::CppMirBodyEmitter emitter(lowered.mir(), map);
      const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                         instance.id};
      expect(emitter.supportsBodyText(address) ||
                 emitter.supportsFailureBodyText(address),
             "the outer constructor body is in the C++ vocabulary");
      const lang::HirBindingId formal = instance.parameterBindings.front();
      for (int mutation = 0; mutation != 5; ++mutation) {
        auto forged = lowered.mir();
        auto *body = lang::findMirBody(forged, address);
        bool applied = false;
        for (auto &block : body->blocks)
          for (auto &candidate : block.instructions) {
            if (candidate.id == binding.argumentInstructions[1]) {
              if (mutation == 0) {
                candidate.lifecycle.clear();
                applied = true;
              }
              if (mutation == 1) {
                candidate.callInputIndex = 1;
                applied = true;
              }
              if (mutation == 3 && !movedArgument && candidate.ownership) {
                candidate.ownership->after = lang::OwnershipStateSet::Moved;
                applied = true;
              }
            }
            const auto *drop =
                candidate.kind == lang::MirInstructionKind::Drop &&
                        candidate.lifecycle.size() == 1
                    ? body->findDropObligation(
                          candidate.lifecycle.front().source)
                    : nullptr;
            if (mutation == 4 && !movedArgument && drop != nullptr &&
                drop->kind == lang::MirDropObligationKind::Binding &&
                drop->binding == formal &&
                !candidate.lifecycle.front().failureCleanup) {
              candidate.lifecycle.front().conditional = true;
              applied = true;
            }
          }
        if (mutation == 2)
          for (auto &value : body->values)
            if (std::find(binding.argumentValues.begin(),
                          binding.argumentValues.end(),
                          value.id) != binding.argumentValues.end()) {
              value.info.traits.copyable = false;
              applied = true;
            }
        if (!applied)
          continue;
        ++mutations;
        const auto rejected = lang::cppMirConstructedFieldInitializerBindings(
            forged, *forged.findConstructorInstance(instance.id));
        expect(rejected.empty(),
               "a constructed closure field rejects a dropped staging "
               "lifecycle, a foreign argument index, a noncopyable closure, "
               "a consuming copy and a conditional formal drop " +
                   std::to_string(mutation));
      }
    }
    expect(copiedFields == 2 && movedFields == 2 && mutations == 15,
           "copied and moved closure fields were each checked twice, the "
           "struct capture's formal drop included");
  }
  // The constructed closure field composes with a selected base initializer
  // and with owned-parameter siblings in the same native member-initializer
  // list. The impossible failure of the field's construction may retire the
  // native base and the consumed formals.
  const auto composedField =
      analyzeTrustedPreludeFixture("composed-closure-field.gti", R"(
import std.string;
import std.utility;
struct Offset {
  int amount;
  Offset(int amount) : amount(amount) {}
};
class Holder<F> {
public:
  F callback;
  Holder(F f) : callback(f) {}
  int invoke() { return callback(); }
};
class Base {
public:
  int amount;
  Base(int amount) : amount(amount) {}
};
class Derived<F> : public Base {
public:
  Holder<F> inner;
  Derived(F f) : Base(10), inner(Holder<F>(f)) {}
};
class Named<F> {
public:
  Holder<F> inner;
  std::string name;
  Named(F f, std::string name) : inner(Holder<F>(f)), name(std::move(name)) {}
};
class Twice<F> {
public:
  Holder<F> inner;
  F spare;
  Twice(F f, F g) : inner(Holder<F>(f)), spare(std::move(g)) {}
  int call_spare() { return spare(); }
};
int derived<F>(F f) {
  Derived<F> value = Derived<F>(f);
  return value.amount + value.inner.invoke();
}
int named<F>(F f) {
  std::string label{"abc"};
  Named<F> value = Named<F>(f, std::move(label));
  return value.inner.invoke() + (value.name.size() == 3 ? 1 : 0);
}
int twice<F>(F f) {
  Twice<F> value = Twice<F>(f, f);
  return value.inner.invoke() + value.call_spare();
}
int main() {
  int k = 3;
  Offset offset{4};
  return derived([k]() -> int { return k; }) +
         derived([offset]() -> int { return offset.amount; }) +
         named([k]() -> int { return k; }) +
         named([offset]() -> int { return offset.amount; }) +
         twice([k]() -> int { return k; }) +
         twice([offset]() -> int { return offset.amount; }) - 50;
}
)");
  expect(composedField.canGenerateCode(), "composed closure fields verify");
  if (composedField.canGenerateCode()) {
    const auto text = gti_test::emitCppText(composedField);
    expect(text.find(": ::__gti_program::Base(static_cast<std::int32_t>(10)), "
                     "inner(std::remove_cv_t<decltype(inner)>("
                     "__gti_mir_arg_0)) {") != std::string::npos &&
               text.find(": inner(std::remove_cv_t<decltype(inner)>("
                         "__gti_mir_arg_0)), name(std::move(__gti_mir_arg_1)) "
                         "{") != std::string::npos &&
               text.find(": inner(std::remove_cv_t<decltype(inner)>("
                         "__gti_mir_arg_0)), spare(std::move("
                         "__gti_mir_arg_1)) {") != std::string::npos,
           "the base, the constructed field and the owned siblings share "
           "one native member-initializer list");
    const auto lowered = gti_test::lowerProgram(
        composedField, composedField.mir, composedField.mir);
    std::size_t bases = 0;
    std::size_t ownedStrings = 0;
    std::size_t ownedClosures = 0;
    std::size_t mutations = 0;
    for (const auto &instance : lowered.mir().constructorInstances()) {
      const auto *owner = lowered.mir().findClassInstance(instance.owner);
      if (owner == nullptr || owner->declaredFields.empty() ||
          owner->declaredFields.front().type.kind !=
              lang::SemanticType::Class ||
          instance.parameterTypes.empty() ||
          instance.parameterTypes.front().kind != lang::SemanticType::Lambda)
        continue;
      const bool base = !owner->bases.empty();
      const bool owned = instance.parameterTypes.size() == 2;
      if (!base && !owned)
        continue;
      const bool ownedClosure = owned && instance.parameterTypes.back().kind ==
                                             lang::SemanticType::Lambda;
      const auto bindings = lang::cppMirConstructedFieldInitializerBindings(
          lowered.mir(), instance);
      expect(bindings.size() == 1 &&
                 bindings.front().initializer == (base ? 1u : 0u) &&
                 bindings.front().arguments.size() == 1 &&
                 bindings.front().arguments.front().parameter == 0,
             "the constructed closure field composes with its sibling");
      const auto baseBindings =
          lang::cppMirBaseParameterInitializerBindings(lowered.mir(), instance);
      const auto ownedBindings =
          lang::cppMirOwnedParameterFieldBindings(lowered.mir(), instance);
      expect(baseBindings && baseBindings->size() == (base ? 1u : 0u) &&
                 ownedBindings && ownedBindings->size() == (owned ? 1u : 0u),
             "the base and owned proofs publish their own initializers");
      if (bindings.size() != 1 || !ownedBindings ||
          ownedBindings->size() != (owned ? 1u : 0u))
        continue;
      if (owned) {
        const auto &move = ownedBindings->front();
        expect(
            move.initializer == 1 && move.parameter == 1 &&
                (ownedClosure
                     ? move.initializeInstruction == 0 && move.fieldDrop == 0
                     : move.initializeInstruction != 0 && move.fieldDrop != 0),
            "an owned string stages its field and a moved closure "
            "publishes from the constructor record");
      }
      ++(base ? bases : ownedClosure ? ownedClosures : ownedStrings);
      const auto *source = lowered.findConstructorInstance(instance.id);
      const auto *declaration =
          source == nullptr
              ? nullptr
              : lowered.findConstructorDeclaration(source->declaration);
      auto rows = lang::buildCppMirBodyEmissionMapRows(
          lowered, lang::CppStandard::Cpp23);
      const auto overlays =
          declaration == nullptr
              ? std::nullopt
              : lang::cppMirApplyGenericOwnerConstructorTypeOverlays(
                    rows, lowered, *declaration, instance);
      expect(overlays && *overlays != 0,
             "the composed constructor spells F through its overlay");
      const lang::CppMirBodyEmissionMap map(std::move(rows));
      const lang::CppMirBodyEmitter emitter(lowered.mir(), map);
      const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                         instance.id};
      expect(emitter.supportsBodyText(address) ||
                 emitter.supportsFailureBodyText(address),
             "the composed constructor body is in the C++ vocabulary");
      // The native base's rollback, or the owned move's consumption of its
      // formal: each must keep its exact MIR schedule.
      const auto *stage = [&]() -> const lang::MirInstruction * {
        for (const auto &block : instance.body.blocks)
          for (const auto &candidate : block.instructions)
            if (candidate.kind == lang::MirInstructionKind::InitializeBase)
              return &candidate;
        return nullptr;
      }();
      const lang::MirDropObligationId rollback =
          stage != nullptr && stage->successResultDrop
              ? *stage->successResultDrop
              : 0;
      const lang::MirInstructionId ownedMove =
          owned ? ownedBindings->front().moveInstruction : 0;
      const lang::MirDropObligationId formalDrop =
          owned ? ownedBindings->front().parameterDrop : 0;
      expect(base == (rollback != 0),
             "the native base arms its construction rollback");
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto forged = lowered.mir();
        auto *body = lang::findMirBody(forged, address);
        bool applied = false;
        for (auto &block : body->blocks)
          for (auto &candidate : block.instructions) {
            for (auto &event : candidate.lifecycle) {
              if (rollback == 0 ||
                  (event.source != rollback && event.target != rollback))
                continue;
              // The stage arms the rollback, the shared transfer retires
              // it, and only an impossible failure drops it.
              if (mutation == 0 &&
                  candidate.kind == lang::MirInstructionKind::InitializeBase) {
                event.conditional = true;
                applied = true;
              }
              if (mutation == 1 &&
                  event.kind == lang::MirLifecycleEventKind::TransferOut) {
                event.kind = lang::MirLifecycleEventKind::Drop;
                applied = true;
              }
              if (mutation == 2 && event.failureCleanup) {
                event.conditional = true;
                applied = true;
              }
            }
            if (ownedMove != 0 && candidate.id == ownedMove &&
                candidate.ownership) {
              if (mutation == 0) {
                candidate.ownership->after = lang::OwnershipStateSet::Available;
                applied = true;
              }
              if (mutation == 1) {
                candidate.hirValue = 0;
                applied = true;
              }
            }
            if (mutation == 2 && formalDrop != 0 &&
                candidate.kind == lang::MirInstructionKind::Drop &&
                candidate.lifecycle.size() == 1 &&
                candidate.lifecycle.front().source == formalDrop &&
                !candidate.lifecycle.front().failureCleanup) {
              candidate.lifecycle.front().conditional = true;
              applied = true;
            }
          }
        if (mutation == 2 && ownedClosure && formalDrop == 0) {
          // A scalar-capturing closure has no formal drop: give its record
          // publication a lifecycle event instead.
          for (auto &block : body->blocks)
            for (auto &candidate : block.instructions)
              if (candidate.id == ownedMove) {
                candidate.lifecycle.push_back(
                    {.kind = lang::MirLifecycleEventKind::Initialize});
                applied = true;
              }
        }
        if (!applied)
          continue;
        ++mutations;
        const auto *changed = forged.findConstructorInstance(instance.id);
        expect(lang::cppMirConstructedFieldInitializerBindings(forged, *changed)
                   .empty(),
               "a composed closure field rejects a conditional base "
               "activation, a retained base rollback, a conditional failure "
               "rollback, an unconsumed or foreign owned move, a conditional "
               "formal drop and a staged record publication " +
                   std::to_string(mutation));
      }
    }
    expect(bases == 2 && ownedStrings == 2 && ownedClosures == 2 &&
               mutations == 18,
           "each composition was checked for both closure instances");
  }
  return failures == 0 ? 0 : 1;
}
