#include "../src/compiler/cpp_mir_body_emitter.h"
#include "../src/compiler/cpp_mir_representation_snapshot.h"
#include "../src/compiler/cpp_representation.h"
#include "compiler_test_fixture.h"
#include "gti/mir_effects.h"

int main() {
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
  std::size_t ownedCopies = 0;
  for (const auto &function : copiedCallable.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &input : block.instructions) {
        if (input.kind != lang::MirInstructionKind::CallInput ||
            input.callInputKind != lang::HirCallInputKind::CopyValue ||
            input.info.type.kind != lang::SemanticType::Lambda)
          continue;
        ++ownedCopies;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto forged = copiedCallable.mir;
          auto *body = lang::findMirBody(
              forged, {lang::MirBodyKind::Function, function.id});
          if (mutation == 0) {
            for (auto &place : body->places)
              if (place.id == input.operands.front().place)
                place.traits.copyable = false;
          } else {
            for (auto &candidateBlock : body->blocks)
              for (auto &candidate : candidateBlock.instructions)
                if (candidate.id == input.id) {
                  if (mutation == 1)
                    candidate.preparedParameterDrop.reset();
                  else
                    candidate.lifecycle.clear();
                }
          }
          expect(!lang::verifyMirProgram(forged).valid(),
                 "owned copies reject forged copyability or missing prepared "
                 "lifetime initialization");
        }
      }
    }
  }
  expect(ownedCopies >= 3,
         "owned callable copies cover a local and two field copies");
  const std::string voidReturnSource = R"(
void action() {}
void relay() { return action(); }
void generic<T>(T callback) { return callback(); }
int main() {
  relay();
  auto callback = []() -> void { return action(); };
  generic(callback);
  return 0;
}
)";
  const auto voidReturns = analyzeTrustedPreludeFixture(
      "void-expression-return.gti", voidReturnSource);
  expect(voidReturns.canGenerateCode(),
         "void functions and lambdas can return exact void expressions");
  std::size_t voidTerminals = 0;
  for (const auto &function : voidReturns.mir.functionInstances()) {
    if (function.returnType != lang::SemanticType::Void)
      continue;
    for (const auto &block : function.body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Return)
        continue;
      ++voidTerminals;
      expect(!block.terminator.value,
             "void expression returns have no runtime result operand");
      auto forged = voidReturns.mir;
      auto *body =
          lang::findMirBody(forged, {lang::MirBodyKind::Function, function.id});
      body->blocks[block.id - 1].terminator.value =
          lang::MirOperand{.kind = lang::MirOperandKind::Constant,
                           .literal = lang::Literal{std::uint64_t{7}},
                           .type = lang::SemanticType::Int32};
      expect(!lang::verifyMirProgram(forged).valid(),
             "a void return rejects a forged runtime value");
    }
  }
  expect(voidTerminals >= 2, "the void forwarding bodies retain return edges");
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
  auto wrongOverrideSource = erasedVoidSource;
  const auto parameterStart =
      wrongOverrideSource.find("int count, bool active");
  wrongOverrideSource.replace(parameterStart,
                              std::string("int count, bool active").size(),
                              "bool count, int active");
  const auto wrongOverride = analyzeTrustedPreludeFixture(
      "wrong-expanded-override.gti", wrongOverrideSource);
  expect(!wrongOverride.canGenerateCode() &&
             hasDiagnosticCode(wrongOverride.diagnostics, "GTI-S2042"),
         "expanded virtual override signatures retain exact element typing");
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
  for (const std::string source :
       {"void invalid() { return 7; } int main() { return 0; }",
        "int value() { return 7; } void invalid() { return value(); } "
        "int main() { return 0; }"}) {
    const auto invalid =
        analyzeTrustedPreludeFixture("nonvoid-return.gti", source);
    const auto diagnostic = std::find_if(
        invalid.diagnostics.begin(), invalid.diagnostics.end(),
        [](const auto &item) {
          return item.code == "GTI-S2000" &&
                 item.message.find("Void function cannot return a value") !=
                     std::string::npos;
        });
    const auto offset = source.find("return", source.find("void invalid"));
    expect(
        !invalid.canGenerateCode() && diagnostic != invalid.diagnostics.end() &&
            diagnostic->phase == lang::DiagnosticPhase::Semantics &&
            diagnostic->primary.source.ends_with(
                "gti-trusted-nonvoid-return.gti") &&
            diagnostic->primary.start == offset &&
            diagnostic->primary.end == offset + 6 && diagnostic->fixes.empty(),
        "nonvoid expressions retain the exact semantic return diagnostic");
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
  const std::string expandedLambdaSource = R"(
int apply<Args...>(Args... args) {
  auto callback = [](int value, bool enabled) -> int {
    return enabled ? value : 0;
  };
  return callback(args...);
}
int main() { return apply(7, true) - 7; }
)";
  const auto expandedLambda =
      analyzeTrustedPreludeFixture("expanded-lambda.gti", expandedLambdaSource);
  expect(
      expandedLambda.canGenerateCode(),
      "lambda invocation flattens the concrete pack and retains its receiver");
  bool expandedInvocation = false;
  for (const auto &function : expandedLambda.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call ||
            !instruction.lambdaTarget || instruction.callSite == 0)
          continue;
        expandedInvocation = true;
        expect(
            instruction.receiver && instruction.operands.size() == 2 &&
                instruction.parameterTypes.size() == 2,
            "expanded lambda has one receiver and two exact argument stages");
        for (int mutation = 0; mutation < 2; ++mutation) {
          auto forged = expandedLambda.mir;
          auto *body = lang::findMirBody(
              forged, {lang::MirBodyKind::Function, function.id});
          auto &changed = body->blocks[block.id - 1].instructions;
          auto target = std::find_if(
              changed.begin(), changed.end(),
              [&](const auto &item) { return item.id == instruction.id; });
          if (mutation == 0)
            target->receiver.reset();
          else
            target->parameterTypes.front() = lang::SemanticType::Bool;
          expect(!lang::verifyMirProgram(forged).valid(),
                 "staged lambda calls reject a missing receiver or forged "
                 "signature");
        }
      }
    }
  }
  expect(expandedInvocation,
         "the pack fixture exercises staged lambda invocation");
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
  auto wrongExpandedLambda = expandedLambdaSource;
  wrongExpandedLambda.replace(wrongExpandedLambda.find("7, true"), 7,
                              "true, 7");
  const auto wrongExpansion = analyzeTrustedPreludeFixture(
      "wrong-expanded-lambda.gti", wrongExpandedLambda);
  expect(!wrongExpansion.canGenerateCode() &&
             hasDiagnosticCode(wrongExpansion.diagnostics, "GTI-S2003"),
         "each expanded lambda argument must match its exact parameter");
  const auto returnFixture = std::filesystem::path(__FILE__).parent_path() /
                             "fixtures/return_result_cleanup.gti";
  std::ifstream returnInput(returnFixture);
  expect(returnInput.good(), "return-result cleanup fixture exists");
  const std::string returnSource{std::istreambuf_iterator<char>(returnInput),
                                 {}};
  const auto returns =
      analyzeTrustedPreludeFixture("return-result-cleanup.gti", returnSource);
  expect(returns.canGenerateCode(), "unpublished return lifetimes verify");
  std::size_t returnLifetimes = 0;
  bool callableFieldReturn = false;
  if (returns.canGenerateCode()) {
    for (const auto &function : returns.mir.functionInstances()) {
      for (const auto &drop : function.body.dropObligations) {
        if (drop.kind != lang::MirDropObligationKind::ReturnResult)
          continue;
        ++returnLifetimes;
        callableFieldReturn =
            callableFieldReturn ||
            std::any_of(
                function.callableParameters.begin(),
                function.callableParameters.end(), [](const auto &parameter) {
                  return parameter.ownedTransport &&
                         parameter.ownedTransport->kind ==
                             lang::CallableOwnedTransportKind::ExactField;
                });
        for (int mutation = 0; mutation < 5; ++mutation) {
          auto forged = returns.mir;
          auto *body = lang::findMirBody(
              forged, {lang::MirBodyKind::Function, function.id});
          bool changed = false;
          for (auto &block : body->blocks) {
            for (auto &member : block.instructions) {
              if (member.lifecycle.size() != 1)
                continue;
              auto &event = member.lifecycle.front();
              if (mutation == 0 && event.target == drop.id) {
                event.kind = lang::MirLifecycleEventKind::TransferOut;
                event.target = 0;
                changed = true;
              } else if (mutation == 1 && event.target == drop.id) {
                member.lifecycle.clear();
                changed = true;
              } else if (mutation == 2 && event.source == drop.id &&
                         event.kind ==
                             lang::MirLifecycleEventKind::TransferOut) {
                member.lifecycle.clear();
                changed = true;
              } else if (mutation == 3 && event.source == drop.id &&
                         event.kind == lang::MirLifecycleEventKind::Drop) {
                event.failureCleanup = false;
                changed = true;
              }
            }
          }
          if (mutation == 4) {
            body->dropObligations[drop.id - 1].fullExpression = 1;
            changed = true;
          }
          expect(changed && !lang::verifyMirProgram(forged).valid(),
                 "return rollback rejects early transfer, missing reparent or "
                 "publication, wrong cleanup phase, and temporary lifetime");
        }
      }
    }
  }
  expect(returnLifetimes >= 6 && callableFieldReturn,
         "direct-call, moved, constructed, wrapped and storage returns "
         "and exact callable-field transports retain unpublished ownership");
  const std::string carrierSource = R"(
class Carrier<T> {
  T callback;
public:
  Carrier(T callback) : callback(std::move(callback)) {}
  int call(int value) { return this.callback(value); }
};
Carrier<T> forward<T>(Carrier<T> owner) { return std::move(owner); }
Carrier<T> wrap<T>(T callback) { return Carrier<T>(std::move(callback)); }
int main() {
  int offset = 7;
  auto callback = [offset](int value) -> int { return offset + value; };
  auto first = wrap(std::move(callback));
  auto second = forward(std::move(first));
  return second.call(5) - 12;
}
)";
  const auto carrier =
      analyzeTrustedPreludeFixture("callable-carrier.gti", carrierSource);
  expect(carrier.canGenerateCode(),
         "nominal callable carrier reaches verified MIR");
  if (!carrier.canGenerateCode())
    for (const auto &diagnostic : carrier.diagnostics)
      std::cerr << diagnostic.message << '\n';
  auto copiedCarrierSource = carrierSource;
  const std::string movedCarrier = "forward(std::move(first))";
  copiedCarrierSource.replace(copiedCarrierSource.find(movedCarrier),
                              movedCarrier.size(), "forward(first)");
  const auto copiedCarrier = analyzeTrustedPreludeFixture(
      "copied-callable-carrier.gti", copiedCarrierSource);
  expect(!copiedCarrier.canGenerateCode() &&
             hasDiagnosticCode(copiedCarrier.diagnostics, "GTI-S2046") &&
             hasDiagnostic(copiedCarrier.diagnostics,
                           "requires an explicit std::move"),
         "nominal callable carriers do not silently copy at owned boundaries");
  auto hiddenCarrierSource = copiedCarrierSource;
  const std::string forwardingBody = " { return std::move(owner); }";
  hiddenCarrierSource.replace(hiddenCarrierSource.find(forwardingBody),
                              forwardingBody.size(), ";");
  const auto hiddenCarrier = analyzeTrustedPreludeFixture(
      "hidden-callable-carrier.gti", hiddenCarrierSource);
  const auto hiddenDiagnostic =
      std::find_if(hiddenCarrier.diagnostics.begin(),
                   hiddenCarrier.diagnostics.end(), [](const auto &diagnostic) {
                     return diagnostic.code == "GTI-S2046" &&
                            diagnostic.message.find("generic owning carrier") !=
                                std::string::npos;
                   });
  expect(!hiddenCarrier.canGenerateCode() &&
             hiddenDiagnostic != hiddenCarrier.diagnostics.end() &&
             hiddenDiagnostic->phase == lang::DiagnosticPhase::Semantics &&
             hiddenDiagnostic->primary.start ==
                 hiddenCarrierSource.rfind("first)") &&
             hiddenDiagnostic->primary.end ==
                 hiddenDiagnostic->primary.start + 5 &&
             !hiddenDiagnostic->hints.empty() &&
             hiddenDiagnostic->hints.front().find("Owner<Operation>") !=
                 std::string::npos &&
             hiddenDiagnostic->fixes.empty(),
         "hidden-body carrier rejection names the boundary at the argument "
         "without guessing a fix");
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
  const auto erased =
      analyzeTrustedMemoryFixture("erased-callable.gti", erasedSource);
  expect(erased.canGenerateCode(),
         "source-defined callable model supports ordinary polymorphic erasure");
  std::size_t dynamicDrops = 0;
  for (const auto address : lang::enumerateMirBodyAddresses(erased.mir)) {
    const auto *body = lang::findMirBody(erased.mir, address);
    for (const auto &drop : body->dropObligations) {
      if (!lang::mirDropHasPolymorphicCleanup(drop.dropType))
        continue;
      ++dynamicDrops;
      for (int mutation = 0; mutation < 2; ++mutation) {
        auto forged = erased.mir;
        auto *changed = lang::findMirBody(forged, address);
        auto alter = [&](const auto &self, lang::MirDropType &type) -> void {
          if (type.polymorphicCleanup) {
            if (mutation == 0)
              type.polymorphicCleanup = false;
            else
              type.uniquePointeeClass.reset();
          }
          for (auto &child : type.aggregate)
            self(self, child);
        };
        alter(alter, changed->dropObligations[drop.id - 1].dropType);
        expect(!lang::verifyMirProgram(forged).valid(),
               "polymorphic cleanup cannot lose dispatch or its exact base "
               "identity");
      }
    }
  }
  expect(dynamicDrops != 0,
         "erased owners retain dynamic cleanup even with a passive base");
  bool stagedCarrier = false;
  for (const auto &function : erased.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call)
          continue;
        for (const auto &boundary : call.callableArguments) {
          if (boundary.boundary != lang::CallableBoundary::Owned ||
              boundary.parameterIndex >= call.operands.size())
            continue;
          const auto &operand = call.operands[boundary.parameterIndex];
          if (operand.type.kind != lang::SemanticType::Class ||
              operand.kind != lang::MirOperandKind::Value)
            continue;
          const auto *value = function.body.findValue(operand.value);
          if (value == nullptr)
            continue;
          for (const auto &stage : block.instructions) {
            if (stage.id != value->definition ||
                stage.kind != lang::MirInstructionKind::CallInput ||
                stage.operands.size() != 1)
              continue;
            const auto *source =
                function.body.findValue(stage.operands.front().value);
            if (source == nullptr)
              continue;
            stagedCarrier = true;
            expect(stage.preparedParameterDrop.has_value() &&
                       stage.callInputKind == lang::HirCallInputKind::MoveValue,
                   "erased model transfer retains its prepared owner lifetime");
            for (int mutation = 0; mutation != 4; ++mutation) {
              auto forged = erased.mir;
              auto &functions =
                  const_cast<std::vector<lang::MirFunctionInstance> &>(
                      forged.functionInstances());
              for (auto &candidateBlock :
                   functions[function.id - 1].body.blocks)
                for (auto &instruction : candidateBlock.instructions) {
                  if (instruction.id == stage.id) {
                    if (mutation == 0)
                      instruction.callInputKind =
                          lang::HirCallInputKind::CopyValue;
                    if (mutation == 1)
                      instruction.lifecycle.clear();
                    if (mutation == 2)
                      instruction.callInputIndex += 1;
                  }
                  if (mutation == 3 && instruction.id == source->definition)
                    instruction.ownership.reset();
                }
              expect(!lang::verifyMirProgram(forged).valid(),
                     "MIR rejects forged carrier staging, lifetime, index and "
                     "source move");
            }
          }
        }
      }
    }
  }
  expect(stagedCarrier,
         "polymorphic erasure exercises staged nominal ownership");
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
  bool packContract = false;
  for (const auto &function : pack.mir.functionInstances()) {
    if (function.parameterTypes.size() != 4 ||
        function.callableParameters.size() != 2 ||
        std::none_of(function.body.dropObligations.begin(),
                     function.body.dropObligations.end(), [](const auto &drop) {
                       return drop.initiallyActive &&
                              drop.dropType.requiresActiveCleanup;
                     }))
      continue;
    packContract = true;
    expect(function.callableParameters[0].parameterIndex == 1 &&
               function.callableParameters[1].parameterIndex == 3,
           "pack contracts use physical indices past a fixed prefix and scalar "
           "element");
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto forged = pack.mir;
      auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
          forged.functionInstances());
      auto &target = functions[function.id - 1];
      if (mutation == 0) {
        target.callableParameters[1].parameterIndex = 2;
      } else if (mutation == 1) {
        target.body.dropObligations.front().initiallyActive = false;
      } else {
        for (auto &place : target.body.places) {
          if (place.projections.size() == 1 &&
              place.projections.front().kind ==
                  lang::MirProjectionKind::PackElement) {
            if (mutation == 2)
              place.traits.drop = lang::DropKind::Trivial;
            else
              place.projections.front().constantIndex = 1;
            break;
          }
        }
      }
      expect(!lang::verifyMirProgram(forged).valid(),
             "MIR rejects forged pack callable indices, cleanup, traits and "
             "projections");
    }
  }
  expect(packContract, "mixed pack records both owned closure elements");
  auto repeatedPackSource = packSource;
  const std::string relayBody = "{ accept(0, args...); }";
  repeatedPackSource.replace(repeatedPackSource.find(relayBody),
                             relayBody.size(),
                             "{ accept(0, args...); accept(0, args...); }");
  const auto repeatedPack =
      analyzeTrustedPreludeFixture("owned-pack-reuse.gti", repeatedPackSource);
  expect(!repeatedPack.canGenerateCode() &&
             hasDiagnosticCode(repeatedPack.diagnostics, "GTI-S2018"),
         "forwarding a move-only closure pack twice is rejected");
  const auto passivePack =
      analyzeTrustedPreludeFixture("passive-callable-pack.gti", R"(
void accept<Args...>(int prefix, Args... args) {}
void relay<Args...>(Args... args) { accept(0, args...); accept(0, args...); }
int main() {
  int first = 1;
  int second = 2;
  auto left = [first]() -> int { return first; };
  auto right = [second]() -> int { return second; };
  relay(std::move(left), 7, std::move(right));
  accept(0);
  return 0;
}
)");
  expect(passivePack.canGenerateCode(),
         "empty and mixed passive callable packs retain exact contracts");
  bool copiedPackElement = false;
  for (const auto &function : passivePack.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputKind != lang::HirCallInputKind::CopyValue ||
            instruction.info.type.kind != lang::SemanticType::Lambda)
          continue;
        copiedPackElement = true;
        expect(instruction.preparedParameterDrop &&
                   instruction.lifecycle.size() == 1 &&
                   instruction.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Initialize,
               "copyable closure pack creates a fresh lifetime without "
               "consuming its source");
        auto forged = passivePack.mir;
        auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
            forged.functionInstances());
        for (auto &candidate : functions[function.id - 1].body.places)
          if (candidate.id == instruction.operands.front().place)
            candidate.traits.copyable = false;
        expect(!lang::verifyMirProgram(forged).valid(),
               "MIR rejects copying a forged move-only pack element");
      }
    }
  }
  expect(copiedPackElement,
         "copyable closure forwarding retains per-element copy roles");
  const auto borrowedPack =
      analyzeTrustedPreludeFixture("borrowed-callable-pack.gti", R"(
void accept<Args...>(Args... args) {}
int main() {
  std::string_view text = "borrowed";
  auto operation = [text]() -> int { return 1; };
  accept(7, std::move(operation));
  return 0;
}
)");
  expect(!borrowedPack.canGenerateCode() &&
             hasDiagnosticCode(borrowedPack.diagnostics, "GTI-S2046") &&
             hasDiagnostic(borrowedPack.diagnostics,
                           "contains borrowed or raw-pointer capture state") &&
             !hasDiagnosticCode(borrowedPack.diagnostics, "GTI-B0001"),
         "owned pack rejects borrowed capture after an ordinary argument "
         "before MIR");
  const auto implicitPackCopy =
      analyzeTrustedPreludeFixture("implicit-callable-pack-copy.gti", R"(
void accept<Args...>(Args... args) {}
int main() {
  int value = 1;
  auto operation = [value]() -> int { return value; };
  accept(7, operation);
  return 0;
}
)");
  expect(!implicitPackCopy.canGenerateCode() &&
             hasDiagnosticCode(implicitPackCopy.diagnostics, "GTI-S2046") &&
             hasDiagnostic(implicitPackCopy.diagnostics,
                           "requires an explicit std::move"),
         "ordinary call into an owned pack still requires explicit closure "
         "transfer");
  const std::string ownedValueSource = R"(
class Capture {
  int value;
public:
  Capture(int value) : value(value) {}
  ~Capture() { int checked = 1 / this.value; }
  int read() { return this.value; }
};
class Owner<T> {
  T callback;
public:
  Owner(T callback) : callback(std::move(callback)) {}
  int call(int argument) { return this.callback(argument); }
};
Owner<T> own<T>(T callback, bool early) {
  if (early) { return Owner<T>(std::move(callback)); }
  auto local = Owner<T>(std::move(callback));
  return std::move(local);
}
int main() {
  Capture offset{7};
  auto callback = [offset = std::move(offset)](int value) -> int { return offset.read() + value; };
  auto owner = own(std::move(callback), false);
  return owner.call(5) - 12;
}
)";
  const auto ownedValue =
      analyzeTrustedPreludeFixture("owned-value.gti", ownedValueSource);
  expect(ownedValue.canGenerateCode(),
         "ordinary owned callable flow reaches MIR");
  bool valueContract = false;
  for (const auto &function : ownedValue.mir.functionInstances()) {
    for (const auto &parameter : function.callableParameters) {
      if (!parameter.ownedTransport ||
          parameter.ownedTransport->kind !=
              lang::CallableOwnedTransportKind::Value)
        continue;
      valueContract = true;
      expect(
          parameter.ownedTransport->destinationType == parameter.callableType,
          "owned value contract describes parameter entry, not return shape");
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto forged = ownedValue.mir;
        auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
            forged.functionInstances());
        auto &target = functions[function.id - 1];
        if (mutation == 0) {
          target.callableParameters.front().ownedTransport->destinationType =
              lang::SemanticType::Int32;
        } else if (mutation == 1) {
          for (auto &place : target.body.places)
            if (place.binding ==
                target.parameterBindings[parameter.parameterIndex])
              place.initiallyAvailable = false;
        } else {
          for (auto &drop : target.body.dropObligations)
            if (drop.binding ==
                target.parameterBindings[parameter.parameterIndex])
              drop.initiallyActive = false;
        }
        expect(
            !lang::verifyMirProgram(forged).valid(),
            "MIR rejects forged owned entry types, availability, and cleanup");
      }
    }
  }
  expect(valueContract,
         "multi-statement callable factory retains an owned value contract");
  auto missingMoveSource = ownedValueSource;
  const std::string transfer = "own(std::move(callback), false)";
  missingMoveSource.replace(missingMoveSource.find(transfer), transfer.size(),
                            "own(callback, false)");
  const auto missingMove =
      analyzeTrustedPreludeFixture("owned-value-copy.gti", missingMoveSource);
  expect(!missingMove.canGenerateCode() &&
             hasDiagnosticCode(missingMove.diagnostics, "GTI-S2018") &&
             hasDiagnostic(missingMove.diagnostics,
                           "would copy a move-only value"),
         "ordinary owned callable entry still requires explicit transfer");
  auto reusedSource = ownedValueSource;
  const std::string result = "return owner.call(5) - 12;";
  reusedSource.replace(reusedSource.find(result), result.size(),
                       "return callback(5);");
  const auto reused =
      analyzeTrustedPreludeFixture("owned-value-reuse.gti", reusedSource);
  expect(!reused.canGenerateCode() &&
             hasDiagnosticCode(reused.diagnostics, "GTI-S2018"),
         "ordinary owned callable transfer retains use-after-move checking");
  const auto capturedCleanup =
      analyzeTrustedPreludeFixture("capture-cleanup.gti", R"(
class Token {
  int divisor;
public:
  Token(int divisor) : divisor(divisor) {}
  ~Token() { int checked = 1 / this.divisor; }
  int read() { return this.divisor; }
};
int main() {
  Token first{1};
  Token second{1};
  auto callback = [first = std::move(first), second = std::move(second)]() -> int {
    return first.read() + second.read();
  };
  return callback() - 2;
}
)");
  expect(capturedCleanup.canGenerateCode(),
         "captured cleanup has verified MIR");
  bool capturedRecipe = false;
  bool captureFailureEdge = false;
  for (const auto &function : capturedCleanup.mir.functionInstances()) {
    for (const auto &drop : function.body.dropObligations) {
      if (!drop.dropType.lambdaInstance || drop.dropType.aggregate.size() != 2)
        continue;
      capturedRecipe = true;
      expect(drop.dropType.aggregate[0].captureIndex == 1 &&
                 drop.dropType.aggregate[1].captureIndex == 0 &&
                 lang::mirDropMayRaiseDefinedFailure(capturedCleanup.mir,
                                                     drop.dropType),
             "MIR owns reverse capture cleanup and its defined-failure effect");
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto forged = capturedCleanup.mir;
        auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
            forged.functionInstances());
        auto &recipe = functions[function.id - 1]
                           .body.dropObligations[drop.id - 1]
                           .dropType;
        if (mutation == 0)
          recipe.aggregate.front().captureIndex = 99;
        if (mutation == 1)
          recipe.aggregate.pop_back();
        if (mutation == 2)
          std::swap(recipe.aggregate[0], recipe.aggregate[1]);
        if (mutation == 3)
          recipe.aggregate.front().destructor.reset();
        expect(!lang::verifyMirProgram(forged).valid(),
               "MIR rejects forged capture identity, missing cleanup, order, "
               "and destructor");
      }
      auto forged = capturedCleanup.mir;
      auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
          forged.functionInstances());
      bool changed = false;
      for (auto &block : functions[function.id - 1].body.blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::Drop &&
              instruction.destination == drop.place &&
              instruction.definedFailure.propagation ==
                  lang::FailurePropagationKind::Destructor) {
            instruction.definedFailure.propagation =
                lang::FailurePropagationKind::None;
            changed = true;
          }
        }
      }
      if (changed) {
        captureFailureEdge = true;
        expect(!lang::verifyMirProgram(forged).valid(),
               "MIR rejects removing a capture cleanup failure edge");
      }
    }
  }
  expect(capturedRecipe,
         "captured ownership retains an executable cleanup recipe");
  expect(captureFailureEdge, "captured cleanup retains a failure edge");
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
    bool storedRequirement = false;
    const auto *ownerClass = findTopLevelClass(frontend.program, "Owner");
    for (const auto &member : ownerClass->members()) {
      const auto *method = lang::stmtAs<lang::FunctionDecl>(member.get());
      if (method == nullptr || method->name().lexeme != "call") {
        continue;
      }
      const auto *returned = lang::stmtAs<lang::ReturnStmt>(
          method->body()->statements().front().get());
      const auto *call = lang::exprAs<lang::Call>(returned->value().get());
      const auto *requirement =
          frontend.semantics.findDeferredCallableCall(*call);
      if (requirement && requirement->receiverField != 0) {
        storedRequirement = true;
        expect(!requirement->boundary &&
                   requirement->returnType == lang::SemanticType::Int32,
               "stored invocation does not masquerade as a confined parameter "
               "boundary");
      }
    }
    expect(storedRequirement,
           "symbolic field invocation retains its exact result requirement");
    bool mutated = false;
    for (const auto &function : frontend.mir.functionInstances()) {
      auto body = function.body;
      for (auto &block : body.blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.lambdaTarget && instruction.receiver) {
            instruction.receiver->type = lang::SemanticType::Int32;
            expect(!lang::verifyMirBody(body).valid(),
                   "MIR rejects a callable receiver with a forged type");
            mutated = true;
            break;
          }
        }
        if (mutated) {
          break;
        }
      }
      if (mutated) {
        break;
      }
    }
    expect(
        mutated,
        "stored invocation regression includes an executable lambda receiver");
  }
  const std::string owner = R"(
class Owner<T> {
  T value;
public:
  Owner(T value) : value(std::move(value)) {}
  int call(int argument) { return this.value(argument); }
};
Owner<T> own<T>(T value) { return Owner<T>(std::move(value)); }
)";
  for (const std::string input :
       {"int main() { auto op = [](int value) -> bool { return true; }; auto "
        "owner = own(std::move(op)); return owner.call(1); }",
        "int main() { Owner<int> owner{1}; return owner.call(2); }",
        "class Mut { public: int operator()(int value) mut { return value; } "
        "}; int main() { Owner<Mut> owner = Owner<Mut>(Mut()); return "
        "owner.call(1); }"}) {
    const auto invalid =
        analyzeTrustedPreludeFixture("bad-stored-callable.gti", owner + input);
    if (input.find("-> bool") != std::string::npos) {
      const auto diagnostic = std::find_if(
          invalid.diagnostics.begin(), invalid.diagnostics.end(),
          [](const lang::Diagnostic &item) {
            return item.code == "GTI-S2046" &&
                   item.message.find("Callable field 'value' must return") !=
                       std::string::npos;
          });
      expect(diagnostic != invalid.diagnostics.end() &&
                 diagnostic->phase == lang::DiagnosticPhase::Semantics &&
                 diagnostic->primary.start == owner.find("this.value") + 5 &&
                 !diagnostic->related.empty() &&
                 diagnostic->related.front().span.start ==
                     owner.find("T value;") + 2 &&
                 diagnostic->fixes.empty(),
             "inexact stored result names the field and links its declaration "
             "without a guessed fix");
    }
    expect(!invalid.canGenerateCode() &&
               !hasDiagnosticCode(invalid.diagnostics, "GTI-B0001"),
           "concrete field callable rejects inexact result, non-callable, and "
           "mutable-only target");
  }
  return failures == 0 ? 0 : 1;
}
