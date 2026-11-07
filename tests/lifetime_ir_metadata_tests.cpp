#include "../src/compiler/mir_lifetimes.h"
#include "../src/compiler/mir_verification_queries.h"
#include "../src/compiler/mir_verification_support.h"
#include "gti/mir_printer.h"
#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_editor.h"
#include "gti/mir_lowering.h"
#include "gti/mir_verifier.h"
#include "gti/callable_lifetimes.h"
#include "gti/nominal_variance.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace lang {
struct MirProgramTestAccess {
  static auto &classes(MirProgram &program) { return program.classes; }
  static auto &constructors(MirProgram &program) { return program.constructors; }
  static auto &functions(MirProgram &program) { return program.functions; }
  static auto &payloads(MirProgram &program) { return program.payloadEnums_; }
  static auto &integrals(MirProgram &program) { return program.integralEnums_; }
  static auto &lambdas(MirProgram &program) { return program.lambdas; }
  static auto &nominalRecipes(MirProgram &program) { return program.nominalVarianceRecipes_; }
};
} // namespace lang
using namespace lang;
namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
SemanticType reference(GenericParameterId id) {
  auto type = SemanticType::referenceTo(SemanticType(SemanticType::Int32));
  type.referenceLifetime = Lifetime::parameter(id);
  return type;
}
void stagedReferenceContentsMetadata() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "staged-reference-contents.gti", R"gti(
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Pair<lifetime a, lifetime b> : public Base<a> {
  int&<b> other;
  Pair(int&<a> first, int&<b> second) : Base<a>(first), other(second) {}
};
int&<a> external<lifetime a>(int&<a> input) {
  int local = 9;
  Pair pair{input, local};
  return pair.value;
}
int main() { int value = 7; return external(value) - 7; }
)gti");
  expect(checked.canGenerateCode(), "ordinary references remain valid without contents metadata");
  const auto original = MirPrinter().print(checked.mir);
  const auto composite = std::find_if(checked.mir.classInstances().begin(),
      checked.mir.classInstances().end(), [](const auto &candidate) {
        return !candidate.structuralBases.empty();
      });
  expect(composite != checked.mir.classInstances().end(), "fixture needs its concrete inherited carrier");
  const auto suppliers = mir_detail::referenceSupplierPaths(checked.mir, composite->type);
  expect(suppliers.status == ReferenceSupplierQueryStatus::Complete &&
             suppliers.paths.size() == 2 &&
             std::all_of(suppliers.paths.begin(), suppliers.paths.end(), [](const auto &path) {
               return !path.fields.empty() && path.crossings.empty() &&
                      path.reference.kind == SemanticType::Reference &&
                      path.reference.referenceLifetime.specified();
             }) && suppliers.paths[0].reference.referenceLifetime !=
                       suppliers.paths[1].reference.referenceLifetime,
         "the MIR selector query preserves the independent instantiated regions of inherited and direct fields");
  expect(mir_detail::referenceSupplierPaths(checked.mir, SemanticType::Unknown).status ==
             ReferenceSupplierQueryStatus::Unknown,
         "unknown MIR type inventory cannot become a known empty supplier set");
  const auto stagedFailure = [](const MirVerificationResult &result) {
    return !result.valid() && std::any_of(result.errors.begin(), result.errors.end(),
        [](const auto &error) {
          return error.message ==
              "MIR reference contents require checked opening replay before publication";
        });
  };
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto forged = checked.mir;
    const auto function = std::find_if(forged.functionInstances().begin(),
        forged.functionInstances().end(), [](const auto &candidate) {
          return !candidate.body.loans.empty() && !candidate.body.values.empty();
        });
    expect(function != forged.functionInstances().end(), "fixture needs executable reference authority");
    auto &body = const_cast<MirFunctionInstance &>(*function).body;
    auto &loan = body.loans.front();
    const MirReferenceContentsSource selected{
        .openingLoan = loan.id,
        .fields = {{.kind = PlaceProjectionKind::Field, .field = 1}}};
    if (mutation == 0)
      loan.referenceContents = selected;
    if (mutation == 1)
      loan.contentsOpening = MirReferenceContentsOpening{
          .reference = {.kind = MirOperandKind::Loan, .loan = loan.id,
                        .type = SemanticType::Int32},
          .referenceType = reference(991), .readLoans = {loan.id}};
    if (mutation == 2)
      loan.additionalDependencies.push_back({.referenceContents = selected});
    if (mutation == 3)
      body.values.front().borrowDependencies = MirBorrowDependencySet{
          .dependencies = {{.referenceContents = selected}}};
    if (mutation == 4) {
      const auto constructor = std::find_if(forged.constructorInstances().begin(),
          forged.constructorInstances().end(), [](const auto &candidate) {
            return !candidate.initializers.empty();
          });
      expect(constructor != forged.constructorInstances().end(), "fixture needs initializer metadata");
      const_cast<MirConstructorInstance &>(*constructor).initializers.front().completeBorrow =
          MirBorrowDependencySet{.dependencies = {{.referenceContents = selected}}};
    }
    expect(stagedFailure(verifyMirProgram(forged)),
           "unproved contents identity cannot reach place-only program proofs, including unused metadata");
    if (mutation != 4) {
      expect(stagedFailure(verifyMirBody(forged, body, function->id)) &&
                 stagedFailure(verifyMirBodyStructure(body)),
             "contents metadata stays closed in full and structural body verification");
    }
    const auto serialized = MirPrinter().print(forged);
    expect(serialized != original && serialized == MirPrinter().print(forged) &&
               serialized.starts_with("mir-v127 "),
           "contents proof identity survives deterministic versioned MIR serialization");
  }
}
void stagedReferenceContentsTransport() {
  for (const bool copyContents : {false, true}) {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "staged-reference-contents-transport.gti", R"gti(
struct View<lifetime a, lifetime b> {
  int&<a> first;
  int&<b> second;
  View(int&<a> x, int&<b> y) : first(x), second(y) {}
};
int read<lifetime a, lifetime b>(View<a, b>& input) {
)gti" + std::string(copyContents
          ? "View<a, b> copied = input; return copied.first + copied.second;"
          : "return input.first + input.second;") + R"gti(
}
int main() { int x = 2; int y = 5; View view{x, y}; return read(view) - 7; }
)gti");
  // The copy variant has an existing MIR lowering prerequisite. It exercises
  // transport from checked HIR only; neither variant claims opening admission.
  expect(checked.hirValid && (copyContents || checked.canGenerateCode()),
         "source/HIR are valid before staged transport; the direct-read control also verifies MIR");
  const auto function = std::find_if(checked.hir.functionInstances().begin(),
      checked.hir.functionInstances().end(), [](const auto &candidate) {
        return candidate.source && candidate.source->name().lexeme == "read";
      });
  expect(function != checked.hir.functionInstances().end() &&
             function->parameterBindings.size() == 1, "fixture has one exact reference formal");
  const auto selectedBinding = std::find_if(function->body.bindings.begin(),
      function->body.bindings.end(), [&](const auto &candidate) {
        return candidate.id == function->parameterBindings.front();
      });
  const auto *binding = selectedBinding == function->body.bindings.end() ? nullptr : &*selectedBinding;
  expect(binding && binding->info.type.kind == SemanticType::Reference,
         "fixture formal retains its full checked reference type");
  const auto value = std::find_if(function->body.values.begin(), function->body.values.end(),
      [&](const auto &candidate) {
        return candidate.kind == HirValueKind::Variable &&
               candidate.symbol == binding->info.symbol;
      });
  const auto read = std::find_if(function->body.loans.begin(), function->body.loans.end(),
      [&](const auto &loan) {
        return loan.entry && loan.kind == SemanticLoanKind::Reference &&
               loan.place.root == binding->info.symbol;
      });
  const auto stored = std::find_if(function->body.loans.begin(), function->body.loans.end(),
      [&](const auto &loan) {
        return loan.entry && loan.kind == SemanticLoanKind::StoredValue &&
               loan.place.root == binding->info.symbol;
      });
  expect(value != function->body.values.end() && read != function->body.loans.end() &&
             stored != function->body.loans.end() && stored->additionalDependencies.size() == 1,
         "fixture has independent address and two-field contents authority");
  SemanticLoanId openingId = 1;
  for (const auto &loan : function->body.loans)
    openingId = std::max(openingId, loan.semanticLoan + 1);
  for (int mutation = 0; mutation != 8; ++mutation) {
    auto staged = checked.hir;
    auto &body = const_cast<HirFunctionInstance *>(staged.findFunctionInstance(function->id))->body;
    HirLoan opening{.semanticLoan = openingId, .kind = SemanticLoanKind::StoredValue,
                     .referenceContents = SemanticReferenceContentsSource{
                         openingId, stored->designatedFields},
                     .access = AccessMode::ReadOnly,
                     .designatedFields = stored->designatedFields,
                     .contentsOpening = HirReferenceContentsOpening{
                         value->id, binding->info.type, {read->semanticLoan}}};
    for (const auto &field : stored->additionalDependencies)
      opening.additionalDependencies.push_back({
          .referenceContents = SemanticReferenceContentsSource{openingId, field.designatedFields},
          .designatedFields = field.designatedFields, .access = AccessMode::ReadOnly});
    if (mutation == 1) opening.contentsOpening->reference = body.values.size() + 1;
    if (mutation == 2) opening.contentsOpening->referenceType.arguments.front() = SemanticType::Int32;
    if (mutation == 3) opening.contentsOpening->readLoans = {openingId + 10};
    if (mutation == 4) opening.place = stored->place;
    if (mutation == 5) opening.referenceContents->openingLoan = read->semanticLoan;
    if (mutation == 6) opening.additionalDependencies.front() = {
        .referenceContents = opening.referenceContents,
        .designatedFields = opening.designatedFields, .access = AccessMode::ReadOnly};
    if (mutation == 7) opening.contentsOpening->readLoans.push_back(read->semanticLoan);
    body.loans.push_back(std::move(opening));
    auto &evaluated = *const_cast<HirValue *>(body.findValue(value->id));
    evaluated.referenceContentsLoan = openingId;
    if (copyContents) {
      SemanticBorrowDependencySet contents;
      contents.dependencies.push_back({
          .referenceContents = SemanticReferenceContentsSource{openingId, stored->designatedFields},
          .designatedFields = stored->designatedFields, .access = AccessMode::ReadOnly,
          .parent = openingId});
      for (const auto &field : stored->additionalDependencies)
        contents.dependencies.push_back({
            .referenceContents = SemanticReferenceContentsSource{openingId, field.designatedFields},
            .designatedFields = field.designatedFields, .access = AccessMode::ReadOnly,
            .parent = openingId});
      evaluated.borrowDependencies = std::move(contents);
    }
    const auto lowered = MirLowerer{}.lower(staged, checked.failureMetadata);
    expect(!lowered.valid(), "staged contents lowering cannot bypass closed MIR replay gates");
    const auto *instance = lowered.program.findFunctionInstance(function->id);
    expect(instance != nullptr, "staged lowering retains the exact function body for diagnostics");
    const auto found = std::find_if(instance->body.loans.begin(), instance->body.loans.end(),
        [&](const auto &loan) { return loan.semanticLoan == openingId; });
    if (mutation != 0) {
      expect(found == instance->body.loans.end(),
             "malformed staged contents cannot publish even a partial opening identity");
      continue;
    }
    expect(found != instance->body.loans.end() && found->contentsOpening &&
               found->kind == MirLoanKind::Stored && found->source == 0 && found->parent == 0 &&
               !found->storedDependency && !found->lifecycleBound &&
               found->producedBy == value->id && found->additionalDependencies.size() == 1 &&
               found->referenceContents == MirReferenceContentsSource{found->id, stored->designatedFields} &&
               found->contentsOpening->referenceType == binding->info.type,
           "transport maps tagged field sources to one body-local opening without inventing physical origins");
    const auto shape = mir_detail::referenceContentsOpeningShape(lowered.program, instance->body, *found);
    expect(shape && shape->fields.size() == 2,
           "independent typed shape replay recognizes the lowered exact evaluated-reference opening");
    const auto &contents = *found->contentsOpening;
    const auto *place = instance->body.findPlace(contents.reference.place);
    const auto *authority = instance->body.findLoan(contents.readLoans.front());
    expect(contents.reference.kind == MirOperandKind::BorrowRead &&
               contents.reference.type == binding->info.type.arguments.front() &&
               place && place->type == contents.reference.type &&
               place->root == MirPlaceRootKind::Binding && place->binding == binding->id &&
               place->projections.size() == 1 &&
               place->projections.front().kind == MirProjectionKind::Dereference &&
               contents.readLoans.size() == 1 && authority &&
               authority->semanticLoan == read->semanticLoan,
           "opening retains the evaluated referent address and exact independent read capability");
    std::size_t definitions = 0;
    for (const auto &block : instance->body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == MirInstructionKind::Borrow && instruction.loan == found->id) {
          ++definitions;
          expect(instruction.hirValue == value->id && instruction.operands.size() == 1 &&
                     instruction.operands.front() == contents.reference,
                 "opening is explicitly anchored to its evaluated source instruction");
        }
    expect(definitions == 1, "one evaluated expression creates exactly one staged opening");
    if (copyContents)
      expect(std::any_of(instance->body.values.begin(), instance->body.values.end(),
          [&](const auto &copied) {
            return copied.sourceValue == value->id && copied.borrowDependencies &&
                copied.borrowDependencies->dependencies.size() == 2 &&
                std::all_of(copied.borrowDependencies->dependencies.begin(),
                    copied.borrowDependencies->dependencies.end(), [&](const auto &dependency) {
                      return dependency.source == 0 && dependency.parent == found->id &&
                          dependency.referenceContents &&
                          dependency.referenceContents->openingLoan == found->id &&
                          dependency.referenceContents->fields == dependency.designatedFields;
                    });
          }), "copied value facts transport every tagged field without substituting the shell or read loan");
    expect(!verifyMirBody(instance->body, instance->id).valid(),
           "transported contents remain closed to context-free body verification");
  }
  }
}
void nestedCarrierPublication() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "nested-carrier-publication.gti", R"gti(
struct Inner<lifetime a> {
  int&<a> value;
  Inner(int&<a> input) : value(input) {}
};
struct Pair<lifetime a, lifetime b> {
  Inner<a> first;
  int&<b> other;
  Pair(int&<a> firstValue, int&<b> secondValue)
      : first(Inner<a>(firstValue)), other(secondValue) {}
};
int&<a> external<lifetime a>(int&<a> input) {
  int local = 9;
  Pair pair{input, local};
  return pair.first.value;
}
int main() { int value = 7; return external(value) - 7; }
)gti");
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.message << '\n';
  expect(checked.canGenerateCode(),
         "unconditional nested carrier publication preserves its complete field provenance");
  const auto constructor = std::find_if(checked.mir.constructorInstances().begin(),
      checked.mir.constructorInstances().end(), [](const auto &candidate) {
        return candidate.parameterTypes.size() == 2;
      });
  expect(constructor != checked.mir.constructorInstances().end(),
         "nested fixture has its exact two-source constructor");
  const auto &body = constructor->body;
  const auto publication = std::find_if(body.loans.begin(), body.loans.end(),
      [](const auto &loan) {
        return loan.escapes && !loan.storedPath.empty() && !loan.designatedFields.empty();
      });
  expect(publication != body.loans.end() && publication->conditions.empty(),
         "an unconditional publication still names its selected reference field");
  const MirInstruction *producer = nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == MirInstructionKind::Borrow && instruction.loan == publication->id)
        producer = &instruction;
  expect(producer && producer->operands.size() == 1 &&
             producer->operands.front().kind == MirOperandKind::Loan,
         "nested publication transfers one exact constructed group");
  const auto *input = body.findLoan(producer->operands.front().loan);
  expect(input && !input->entry && !input->escapes &&
             input->source == publication->source &&
             input->designatedFields == publication->designatedFields &&
             input->additionalDependencies == publication->additionalDependencies,
         "constructed and published groups retain the same complete referents");
  const auto other = std::find_if(body.loans.begin(), body.loans.end(),
      [&](const auto &loan) {
        return loan.escapes && loan.id != publication->id && loan.source != input->source;
      });
  expect(other != body.loans.end(), "mutation fixture has an independent same-typed source");
  for (int mutation = 0; mutation != 6; ++mutation) {
    auto forged = checked.mir;
    auto &changed = MirProgramTestAccess::constructors(forged)[constructor->id - 1];
    auto &published = changed.body.loans[publication->id - 1];
    if (mutation == 0) published.designatedFields.clear();
    if (mutation == 1) published.designatedFields = other->storedPath;
    if (mutation == 2) published.source = other->source;
    if (mutation == 3) published.access = AccessMode::Mutable;
    if (mutation == 4) {
      // Coordinated annotation changes still cannot rewrite the actual
      // constructed value's input. The executable producer is authoritative.
      changed.body.loans[input->id - 1].source = other->source;
      published.source = other->source;
      for (auto &initializer : changed.initializers)
        if (initializer.field == published.storedPath.front().field && initializer.completeBorrow)
          for (auto &dependency : initializer.completeBorrow->dependencies)
            dependency.source = other->source;
      for (auto &dependency : changed.completeBorrow->dependencies)
        if (!dependency.resultFields.empty() &&
            dependency.resultFields.front() == published.storedPath.front())
          dependency.parameter = 1;
    }
    if (mutation == 5) {
      MirInstructionId next = 1;
      for (const auto &block : changed.body.blocks)
        for (const auto &instruction : block.instructions)
          next = std::max(next, instruction.id + 1);
      for (auto &block : changed.body.blocks)
        if (block.terminator.kind == MirTerminatorKind::Return)
          block.instructions.push_back({.id = next++,
              .kind = MirInstructionKind::EndBorrow, .loan = published.id});
      expect(rebuildMirValueUses(changed.body), "rebuild the precise early-retirement mutation");
    }
    const auto result = verifyMirProgram(forged);
    expect(!result.valid(),
           "nested publication rejects changed paths, source, permission and coordinated false ownership");
    if (mutation == 5)
      expect(std::any_of(result.errors.begin(), result.errors.end(), [](const auto &error) {
               return error.message == "constructor field dependency cannot retire on a successful construction path";
             }), "unconditional constructor publication must remain live at successful exit");
  }
}

void erasedCarrierReturnPublication() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto prelude = std::filesystem::weakly_canonical(root / "prelude.gti");
  const auto capability = std::filesystem::weakly_canonical(root / "std/detail/memory_capabilities.gti");
  const auto entry = std::filesystem::weakly_canonical(root / "__trusted_tests__/erased-carrier-return.gti");
  const std::string declarations = R"gti(export module detail.memory_capabilities;
namespace gti_internal {
export class erased_object {};
export erased_object& erase_reference<T>(T& input);
export unsafe T&<a> restore_reference<T, lifetime a>(erased_object&<a> input) requires outlives<T, a>;
}
)gti";
  const std::string source = R"gti(import "../std/detail/memory_capabilities.gti";
using Opaque = gti_internal::erased_object;
Opaque& erase<T>(T& source) { return gti_internal::erase_reference(source); }
unsafe T&<a> restore<T, lifetime a>(Opaque&<a> source) requires outlives<T, a> {
  unsafe { return gti_internal::restore_reference<T>(source); }
}
T&<a> round_trip<T, lifetime a>(T&<a> source) requires outlives<T, a> {
  unsafe { return restore<T, a>(erase(source)); }
}
struct Borrowed<lifetime storage> {
  int&<storage> value;
  Borrowed(int&<storage> source) : value(source) {}
};
int main() {
  int number = 7;
  Borrowed view{number};
  Borrowed& same = round_trip(view);
  return same.value - 7;
}
)gti";
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      entry, source, {prelude, entry},
      {{prelude.string(), ""}, {capability.string(), declarations}, {entry.string(), source}}, {root});
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked.canGenerateCode(), "transparent erasure returns the exact carrier shell and its original external owners");
  HirFunctionInstanceId functionId = 0;
  for (const auto &function : checked.hir.functionInstances())
    if (function.source && function.source->name().lexeme == "round_trip") functionId = function.id;
  const auto *function = checked.mir.findFunctionInstance(functionId);
  expect(function, "erased return fixture has its concrete returning body");
  const MirInstruction *publication = nullptr;
  for (const auto &block : function->body.blocks)
    for (const auto &instruction : block.instructions) {
      const auto *loan = instruction.loan ? function->body.findLoan(*instruction.loan) : nullptr;
      if (instruction.kind == MirInstructionKind::Borrow && loan && loan->kind == MirLoanKind::Return)
        publication = &instruction;
    }
  expect(publication && publication->operands.size() == 1, "return publishes the evaluated restored pointer");
  const auto *restored = function->body.findLoan(publication->operands.front().loan);
  const auto projection = restored ? mir_detail::verifiedSharedErasedReferenceReturn(
      checked.mir, function->body, *restored, functionId) : std::nullopt;
  expect(projection && projection->publication.dependencies.size() == 2,
         "independent erasure proof distinguishes shell permission from the stored reference owner");
  const auto &returned = *function->body.findLoan(*publication->loan);
  expect(returned.storedDependency == projection->stored && returned.parent == 0 &&
             returned.additionalDependencies.size() == 1 &&
             returned.additionalDependencies.front().parent == projection->stored &&
             restored->storedDependency != projection->stored,
         "only outgoing authority is projected; transient erased/restored loans retain their original witnesses");
  expect(!verifyMirBody(function->body).valid(),
         "context-free verification cannot replace independent erasure helper proof");
  for (int mutation = 0; mutation != 8; ++mutation) {
    auto forged = checked.mir;
    auto &body = MirProgramTestAccess::functions(forged)[functionId - 1].body;
    auto &published = body.loans[returned.id - 1];
    auto &opaque = body.loans[restored->storedDependency - 1];
    auto &restoreLoan = body.loans[restored->id - 1];
    if (mutation == 0) published.storedDependency = opaque.id;
    if (mutation == 1) published.additionalDependencies.clear();
    if (mutation == 2) published.additionalDependencies.front().parent = 0;
    if (mutation == 3) published.source = opaque.source;
    if (mutation == 4) published.additionalDependencies.front().access = AccessMode::Mutable;
    if (mutation == 5 || mutation == 7) {
      // All attached sets agree on the same lie. The actual erasure argument
      // still has the real external field supplier, so executable replay must
      // reject even without optional annotations or a consuming return.
      const auto old = opaque.source;
      const auto wrong = published.source;
      opaque.source = wrong;
      for (auto &dependency : opaque.additionalDependencies)
        if (dependency.source == old) dependency.source = wrong;
      restoreLoan.source = wrong;
      for (auto &dependency : restoreLoan.additionalDependencies)
        if (dependency.source == old) dependency.source = wrong;
      for (auto &dependency : published.additionalDependencies)
        if (dependency.source == old) dependency.source = wrong;
      if (mutation == 7) {
        for (auto &value : body.values) value.borrowDependencies.reset();
        for (auto &block : body.blocks)
          if (block.terminator.kind == MirTerminatorKind::Return)
            block.terminator = {.kind = MirTerminatorKind::Unreachable};
      }
    }
    if (mutation == 6) {
      MirInstructionId next = 1;
      for (const auto &block : body.blocks)
        for (const auto &instruction : block.instructions) next = std::max(next, instruction.id + 1);
      for (auto &block : body.blocks)
        for (auto it = block.instructions.begin(); it != block.instructions.end(); ++it)
          if (it->id == publication->id) {
            block.instructions.insert(it, {.id = next, .kind = MirInstructionKind::EndBorrow,
                                          .loan = projection->stored});
            break;
          }
    }
    expect(rebuildMirValueUses(body), "rebuild exact erased-return mutation use records");
    expect(!verifyMirProgram(forged).valid(),
           "erased return cannot substitute shell identity, lose owners, upgrade access or bypass executable replay");
  }
}

void closureEnvironmentReplay() {
  const auto first = Lifetime::parameter(841), second = Lifetime::parameter(842);
  const auto actualFirst = Lifetime::parameter(843), actualSecond = Lifetime::parameter(844);
  MirLambdaInstance lambda;
  lambda.id = 1;
  lambda.declaration = 84;
  lambda.environmentLifetimes = {first, second};
  lambda.environmentContract = {.parameters = {first, second},
      .bounds = {{first, second}}, .complete = true};
  lambda.type = SemanticType::lambdaType(84, SemanticType::Void, {}, {});
  lambda.type.lifetimeArguments = {first, second};
  lambda.returnType = SemanticType::Void;
  lambda.lifetimeContract = {.parameters = {first, second}, .complete = true};
  lambda.body.kind = MirBodyKind::Lambda;
  lambda.body.returnType = SemanticType::Void;
  lambda.body.lifetimeBody.complete = true;
  MirProgram program;
  MirProgramTestAccess::lambdas(program) = {lambda};
  MirFunctionInstance context;
  context.id = 1;
  context.lifetimeContract = {.parameters = {actualFirst, actualSecond},
      .bounds = {{actualFirst, actualSecond}}, .complete = true};
  MirProgramTestAccess::functions(program) = {context};
  const std::vector<LifetimeBinding> bindings{{first, actualFirst}, {second, actualSecond}};
  const auto actual = *substituteTypeLifetimes(lambda.type, bindings);
  MirBody body;
  body.kind = MirBodyKind::Function;
  body.returnType = SemanticType::Void;
  body.lifetimeBody = {.obligations = {{actualFirst, actualSecond}}, .complete = true};
  MirInstruction creation{.id = 1, .kind = MirInstructionKind::Compute,
      .operation = MirOperation::Closure, .lambdaTarget = 1,
      .info = {.type = actual}};
  creation.lifetimeApplication = {.bindings = bindings,
      .obligations = {{actualFirst, actualSecond}}, .complete = true};
  body.blocks = {{.id = 1, .instructions = {creation}}};
  expect(verifyMirLifetimeApplications(program, body, 1).valid(),
         "closure formation independently proves its fixed environment in the enclosing graph");
  for (int mutation = 0; mutation != 6; ++mutation) {
    auto forged = body;
    auto &claim = forged.blocks.front().instructions.front();
    MirProgramTestAccess::lambdas(program) = {lambda};
    MirProgramTestAccess::functions(program) = {context};
    if (mutation == 0) claim.lifetimeApplication = {};
    if (mutation == 1) claim.lifetimeApplication.bindings.front().second = Lifetime::staticLifetime();
    if (mutation == 2) claim.info.type.lifetimeArguments.front() = actualSecond;
    if (mutation == 3) {
      MirProgramTestAccess::functions(program).front().lifetimeContract.bounds.clear();
      MirProgramTestAccess::lambdas(program).front().lifetimeContract.bounds = {{first, second}};
    }
    if (mutation == 4)
      MirProgramTestAccess::lambdas(program).front().environmentContract.typeBounds = {{reference(899), second}};
    if (mutation == 5)
      MirProgramTestAccess::lambdas(program).front().environmentContract.parameters.front() = Lifetime::local(841);
    expect(!verifyMirLifetimeApplications(program, forged, 1).valid(),
           "closure creation rejects missing proof, changed fixed types, invocation-only premises and foreign scopes");
  }
  MirProgramTestAccess::functions(program) = {context};
  lambda.environmentContract.bounds.clear();
  lambda.environmentContract.requirements = {
      .nodes = {{.kind = LifetimeRequirementNode::Kind::Region, .bound = {first, second}},
                {.kind = LifetimeRequirementNode::Kind::Region, .bound = {second, first}},
                {.kind = LifetimeRequirementNode::Kind::Any, .left = 0, .right = 1}}, .root = 2};
  MirProgramTestAccess::lambdas(program) = {lambda};
  expect(verifyMirLifetimeApplications(program, body, 1).valid(),
         "closure formation can prove one arm of its complete environment disjunction");
  lambda.body.lifetimeBody.obligations = {{first, second}};
  MirProgramTestAccess::lambdas(program) = {lambda};
  expect(!verifyMirLifetimeApplications(program, lambda.body, 1).valid(),
         "lambda body assumptions retain OR instead of granting both environment alternatives");
  lambda.environmentContract.requirements.nodes.back().kind = LifetimeRequirementNode::Kind::All;
  MirProgramTestAccess::lambdas(program) = {lambda};
  expect(verifyMirLifetimeApplications(program, lambda.body, 1).valid(),
         "lambda body receives both premises only from an actual conjunctive environment");

  lambda.returnType = reference(842);
  lambda.parameterTypes = {reference(841)};
  lambda.type = SemanticType::lambdaType(84, lambda.returnType, lambda.parameterTypes, {});
  lambda.type.lifetimeArguments = {first, second};
  lambda.lifetimeContract.bounds = {{first, second}};
  lambda.environmentContract.requirements = {};
  lambda.environmentContract.bounds = {{first, second}};
  const auto actualTarget = *substituteTypeLifetimes(lambda.type, bindings);
  CallableLifetimeSignature requested;
  requested.signature = SemanticType::functionSignature(reference(851), {reference(852)});
  requested.contract = *callableLifetimeContract(requested.signature);
  requested.signatureBindings = {{Lifetime::parameter(851), actualSecond},
                                 {Lifetime::parameter(852), actualFirst}};
  expect(mirLambdaCallableLifetimeTargetMatches(program, requested, lambda, actualTarget),
         "exact fixed environment facts prove a closed callable target's invocation requirements");
  auto missingEnvironment = lambda;
  missingEnvironment.environmentContract.bounds.clear();
  expect(!mirLambdaCallableLifetimeTargetMatches(program, requested, missingEnvironment, actualTarget),
         "a callable target identifier alone cannot supply a missing environment premise");
  auto changedFixed = requested;
  changedFixed.signatureBindings.front().second = Lifetime::staticLifetime();
  expect(!mirLambdaCallableLifetimeTargetMatches(program, changedFixed, lambda, actualTarget),
         "a fixed environment fact cannot be attached to a different requested region");
  auto changedInvocation = requested;
  changedInvocation.signature.quantifiedLifetimes = {851};
  expect(!mirLambdaCallableLifetimeTargetMatches(program, changedInvocation, lambda, actualTarget),
         "fixed environment facts cannot specialize a fresh requested invocation binder");
  lambda.environmentContract.bounds.clear();
  lambda.environmentContract.requirements = {
      .nodes = {{.kind = LifetimeRequirementNode::Kind::Region, .bound = {first, second}},
                {.kind = LifetimeRequirementNode::Kind::Region, .bound = {second, first}},
                {.kind = LifetimeRequirementNode::Kind::Any, .left = 0, .right = 1}}, .root = 2};
  expect(!mirLambdaCallableLifetimeTargetMatches(program, requested, lambda, actualTarget),
         "target implication proves every environment alternative rather than selecting one");
}
void rawPointerStorageOutlives() {
  MirProgram program;
  const auto generic = SemanticType::typeParameter(810);
  const auto pointer = SemanticType::rawPointerTo(generic);
  const auto forever = Lifetime::staticLifetime();
  const auto shorter = Lifetime::parameter(811);
  LifetimeSignatureContract assumptions{.parameters = {shorter}, .complete = true};
  MirBody body;
  body.lifetimeBody = {.typeObligations = {{pointer, forever}}, .complete = true};
  expect(verifyMirLifetimeBodyFacts(program, body, assumptions).valid(),
         "raw pointer storage outlives any region without a pointee lifetime premise");
  assumptions.typeBounds = {{pointer, forever}};
  body.lifetimeBody.typeObligations = {{generic, forever}};
  expect(!verifyMirLifetimeBodyFacts(program, body, assumptions).valid(),
         "a pointer storage premise cannot establish its generic pointee as static");
  body.lifetimeBody.typeObligations = {{
      SemanticType::referenceTo(generic, AccessMode::ReadOnly, shorter), forever}};
  expect(!verifyMirLifetimeBodyFacts(program, body, assumptions).valid(),
         "a pointer premise cannot strengthen a checked reference to its pointee");
  MirClassInstance borrowed;
  borrowed.id = 1;
  borrowed.declaration = 81;
  borrowed.type = SemanticType::classType(81);
  borrowed.type.lifetimeArguments = {shorter};
  borrowed.lifetimeContract = assumptions;
  borrowed.lifetimeContract.typeBounds.clear();
  borrowed.declaredFields = {{.symbol = 812, .type = reference(811)}};
  MirProgramTestAccess::classes(program) = {borrowed};
  const auto nominalPointer = SemanticType::rawPointerTo(borrowed.type);
  assumptions.typeBounds = {{nominalPointer, forever}};
  body.lifetimeBody.typeObligations = {{nominalPointer, forever}};
  expect(verifyMirLifetimeBodyFacts(program, body, assumptions).valid(),
         "raw nominal pointers do not own their pointee's stored checked references");
  body.lifetimeBody.typeObligations = {{borrowed.type, forever}};
  expect(!verifyMirLifetimeBodyFacts(program, body, assumptions).valid(),
         "pointer assumptions cannot expand into nominal pointee storage bounds");
  body.lifetimeBody.typeObligations = {{reference(811), forever}};
  expect(!verifyMirLifetimeBodyFacts(program, body, assumptions).valid() &&
             !mirDeclaredLifetimeRelationship(program, assumptions, shorter, forever),
         "pointer assumptions cannot make a nested checked region static");
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto malformed = pointer;
    if (mutation == 0) malformed.arguments.clear();
    if (mutation == 1) malformed.arguments.push_back(SemanticType::Int32);
    if (mutation == 2) malformed.arguments.front() = SemanticType::Unknown;
    if (mutation == 3) malformed.arguments.front() =
        SemanticType::rawPointerTo(SemanticType::Unknown);
    if (mutation == 4) malformed.arguments.front() = reference(999);
    body.lifetimeBody.typeObligations = {{malformed, forever}};
    assumptions.typeBounds.clear();
    expect(!verifyMirLifetimeBodyFacts(program, body, assumptions).valid(),
           "raw pointer outlives proof retains shape, known type and nested region scope checks");
    assumptions.typeBounds = {{malformed, forever}};
    expect(!verifyMirLifetimeBodyFacts(program, body, assumptions).valid(),
           "a malformed pointer premise cannot bypass independent type validation");
  }
  MirFunctionInstance context;
  context.id = 1;
  context.lifetimeContract = {.parameters = {shorter}, .complete = true};
  MirProgramTestAccess::functions(program) = {context};
  MirBody conversion;
  conversion.kind = MirBodyKind::Function;
  conversion.lifetimeBody.complete = true;
  expect(mirLifetimeValueConversion(program, conversion, 1, SemanticType::NullPtr, nominalPointer),
         "null initializes a named-region nominal pointer without a pointee outlives premise");
  expect(mirLifetimeValueConversion(program, conversion, 1, SemanticType::NullPtr, pointer),
         "null initializes an opaque generic pointer without opening its pointee");
  auto fixedPointer = nominalPointer;
  fixedPointer.arguments.front().lifetimeArguments.front() = forever;
  expect(!mirLifetimeValueConversion(program, conversion, 1, nominalPointer, fixedPointer) &&
             !mirLifetimeValueConversion(program, conversion, 1, fixedPointer, nominalPointer),
         "non-null pointer conversion keeps checked pointee regions invariant");
  auto foreignPointer = nominalPointer;
  foreignPointer.arguments.front().lifetimeArguments.front() = Lifetime::parameter(999);
  expect(!mirLifetimeValueConversion(program, conversion, 1, SemanticType::NullPtr, foreignPointer),
         "null cannot introduce an undeclared destination region");
  auto malformedPointer = pointer;
  malformedPointer.arguments.front() = SemanticType::Unknown;
  expect(!mirLifetimeValueConversion(program, conversion, 1, SemanticType::NullPtr, malformedPointer),
         "null cannot authorize a malformed destination pointee");
  expect(!mirLifetimeValueConversion(program, conversion, 1,
             SemanticType::rawPointerTo(SemanticType::NullPtr),
             SemanticType::rawPointerTo(nominalPointer)),
         "whole-value null conversion never rewrites nested pointer argument types");
  conversion.places = {{.id = 1, .root = MirPlaceRootKind::Binding, .binding = 1,
                        .type = nominalPointer}};
  conversion.values = {{.id = 1, .info = {.type = SemanticType::NullPtr}}};
  MirInstruction initialize;
  initialize.id = 1;
  initialize.kind = MirInstructionKind::Initialize;
  initialize.destination = 1;
  initialize.operands = {{.kind = MirOperandKind::Value, .value = 1, .type = SemanticType::NullPtr}};
  conversion.blocks = {{.id = 1, .instructions = {initialize}}};
  expect(verifyMirLifetimeApplications(program, conversion, 1).valid(),
         "stored null replay derives the exact source type before accepting its scoped pointer destination");
  conversion.values.front().info.type = fixedPointer;
  conversion.blocks.front().instructions.front().operands.front().type = SemanticType::NullPtr;
  expect(!verifyMirLifetimeApplications(program, conversion, 1).valid(),
         "a forged null operand tag cannot hide a non-null pointer source with different nested regions");
}
void packEntrySourceIdentity() {
  const auto reference = SemanticType::referenceTo(SemanticType::classType(88),
      AccessMode::ReadOnly, Lifetime::parameter(881));
  auto nested = SemanticType(SemanticType::TypePack);
  nested.concretePack = true;
  nested.arguments = {reference};
  auto grouped = nested;
  grouped.arguments = {reference, nested};
  MirBody body;
  body.places = {{.id = 1, .root = MirPlaceRootKind::Binding, .binding = 1,
      .symbol = 88, .type = grouped, .initiallyAvailable = true}};
  for (std::size_t index = 0; index != 2; ++index) {
    MirPlace entry{.id = index + 2, .root = MirPlaceRootKind::Binding, .binding = 1,
        .symbol = 88, .projections = {{.kind = MirProjectionKind::PackElement, .constantIndex = index}},
        .type = reference, .traits = semanticTraits(reference), .initiallyAvailable = true};
    if (index) entry.projections.push_back({.kind = MirProjectionKind::PackElement, .constantIndex = 0});
    body.places.push_back(entry);
    body.loans.push_back({.id = index + 1, .kind = MirLoanKind::Parameter,
        .source = entry.id, .carriers = {1}, .entry = true});
  }
  expect(mir_detail::borrowSourceForPlace(body, 2, 0) == 2 &&
             mir_detail::borrowSourceForPlace(body, 3, 0) == 3 &&
             !mir_detail::borrowSourceForPlace(body, 1, 0),
         "equal-typed grouped entries retain distinct full lane origins and no whole-pack owner");
  auto referent = body.places.back();
  referent.id = 4;
  referent.projections.push_back({.kind = MirProjectionKind::Dereference});
  referent.type = reference.arguments.front();
  body.places.push_back(referent);
  expect(mir_detail::borrowSourceForPlace(body, 4, 0) == 3,
         "the exact whole referent retains its selected nested entry");
  referent.id = 5;
  referent.projections.push_back({.kind = MirProjectionKind::Field, .field = 880});
  referent.projections.push_back({.kind = MirProjectionKind::Dereference});
  referent.type = SemanticType::Int32;
  body.places.push_back(referent);
  expect(mir_detail::borrowSourceForPlace(body, 5, 0) == 5,
         "a deeper stored field keeps its exact projection instead of the physical pack shell");
  expect(mir_detail::wholeEntryPlaceMatches(body, body.places[2], body.places[2]) &&
             mir_detail::wholeEntryPlaceMatches(body, body.places[2], body.places[3]) &&
             !mir_detail::wholeEntryPlaceMatches(body, body.places[1], body.places[3]) &&
             !mir_detail::wholeEntryPlaceMatches(body, body.places[2], body.places[4]),
         "carrier dependency selection keeps a whole reference-pack referent but rejects another lane or stored field");
  MirBody ordinary;
  MirPlace referenceEntry{.id = 1, .root = MirPlaceRootKind::Binding, .binding = 8,
      .symbol = 808, .type = reference, .initiallyAvailable = true};
  auto selected = referenceEntry;
  selected.id = 2;
  selected.type = reference.arguments.front();
  selected.projections = {{.kind = MirProjectionKind::Dereference}};
  expect(mir_detail::wholeEntryPlaceMatches(ordinary, referenceEntry, selected),
         "an ordinary carrier reference preserves the dependency of its exact whole referent");
  auto wrong = selected;
  wrong.type = SemanticType::classType(89);
  expect(!mir_detail::wholeEntryPlaceMatches(ordinary, referenceEntry, wrong),
         "the physical crossing must retain the exact referent type");
  wrong = selected;
  wrong.projections.push_back({.kind = MirProjectionKind::Field, .field = 880});
  expect(!mir_detail::wholeEntryPlaceMatches(ordinary, referenceEntry, wrong),
         "an equal-typed field is not the whole physical referent");
  wrong = selected;
  wrong.projections.push_back({.kind = MirProjectionKind::Dereference});
  expect(!mir_detail::wholeEntryPlaceMatches(ordinary, referenceEntry, wrong),
         "a second reference crossing cannot inherit the physical shell dependency");
  wrong = selected;
  wrong.binding = 9;
  expect(!mir_detail::wholeEntryPlaceMatches(ordinary, referenceEntry, wrong),
         "another binding cannot substitute an equal-typed reference entry");
  auto forged = body;
  forged.places[2].projections.back().constantIndex = 1;
  expect(!mir_detail::borrowSourceForPlace(forged, 3, 0) &&
             !mir_detail::wholeEntryPlaceMatches(forged, forged.places[2], body.places[3]),
         "a wrong nested index cannot select an equal-typed entry loan");
  forged = body;
  forged.places.front().type = SemanticType::referenceTo(grouped);
  expect(!mir_detail::borrowSourceForPlace(forged, 2, 0) &&
             !mir_detail::wholeEntryPlaceMatches(forged, forged.places[2], body.places[3]),
         "reference-to-pack storage remains atomic during entry origin selection");
  forged = body;
  forged.loans.back().source = 2;
  expect(mir_detail::borrowSourceForPlace(forged, 3, 0) != 2,
         "substituting another equal-typed lane never grants that lane's source identity");
}
void packElementLifetimeReplay() {
  const auto first = Lifetime::parameter(821);
  const auto second = Lifetime::parameter(822);
  const auto firstUse = Lifetime::inference(823);
  const auto secondUse = Lifetime::inference(824);
  const auto firstReference = SemanticType::referenceTo(SemanticType::Int32, AccessMode::ReadOnly, first);
  const auto secondReference = SemanticType::referenceTo(SemanticType::Int32, AccessMode::ReadOnly, second);
  auto pack = SemanticType(SemanticType::TypePack);
  pack.concretePack = true;
  pack.arguments = {firstReference, secondReference};
  MirProgram program;
  MirFunctionInstance context;
  context.id = 1;
  context.lifetimeContract = {.parameters = {first, second}, .complete = true};
  MirProgramTestAccess::functions(program) = {context};
  MirBody body;
  body.kind = MirBodyKind::Function;
  body.lifetimeBody = {.variables = {firstUse, secondUse},
      .obligations = {{first, firstUse}, {second, secondUse}}, .complete = true};
  body.places = {{.id = 1, .root = MirPlaceRootKind::Binding, .binding = 1, .type = pack}};
  MirBlock block;
  block.id = 1;
  for (std::size_t index = 0; index != 2; ++index) {
    body.places.push_back({.id = index + 2, .root = MirPlaceRootKind::Binding, .binding = 1,
        .projections = {{.kind = MirProjectionKind::PackElement, .constantIndex = index}},
        .type = pack.arguments[index]});
    MirInstruction input;
    input.id = index + 1;
    input.kind = MirInstructionKind::CallInput;
    input.hirValue = 1;
    input.callInputKind = HirCallInputKind::ReadBorrow;
    input.info.type = SemanticType::referenceTo(SemanticType::Int32, AccessMode::ReadOnly,
                                               index == 0 ? firstUse : secondUse);
    input.operands = {{.kind = MirOperandKind::BorrowRead, .place = index + 2, .type = input.info.type}};
    block.instructions.push_back(input);
  }
  body.blocks = {block};
  expect(verifyMirLifetimeApplications(program, body, 1).valid(),
         "each exact pack lane retains its declared reference region during independent stage replay");
  auto missing = body;
  missing.lifetimeBody.obligations.erase(missing.lifetimeBody.obligations.begin());
  expect(!verifyMirLifetimeApplications(program, missing, 1).valid(),
         "the first lane cannot borrow the second lane's distinct outlives edge");
  auto wrongLane = body;
  wrongLane.blocks.front().instructions.front().operands.front().place = 3;
  expect(!verifyMirLifetimeApplications(program, wrongLane, 1).valid(),
         "a matching operand tag cannot substitute another equal-referent pack lane");
  auto nested = body;
  auto inner = pack;
  inner.arguments = {secondReference, firstReference};
  nested.places.front().type.arguments = {firstReference, inner};
  nested.places.back().projections.push_back({.kind = MirProjectionKind::PackElement, .constantIndex = 0});
  expect(verifyMirLifetimeApplications(program, nested, 1).valid(),
         "nested first-child projection retains the exact independent reference region");
  nested.places.back().projections.back().constantIndex = 1;
  expect(!verifyMirLifetimeApplications(program, nested, 1).valid(),
         "a wrong nested lane cannot gain the selected lane's reference lifetime");
  auto atomicReference = body;
  atomicReference.places.front().type = SemanticType::referenceTo(pack, AccessMode::ReadOnly, first);
  expect(!verifyMirLifetimeApplications(program, atomicReference, 1).valid(),
         "a Reference<TypePack> does not become a grouped formal without an exact dereference");
}
void exactReceiverProjectionRegion() {
  const auto external = Lifetime::parameter(1601), shell = Lifetime::parameter(1602);
  const auto receiver = Lifetime::parameter(1603), use = Lifetime::inference(1604);
  MirClassInstance inner;
  inner.id = 1;
  inner.declaration = 160;
  inner.type = SemanticType::classType(160);
  inner.type.lifetimeArguments = {external};
  inner.lifetimeContract = {.parameters = {external}, .complete = true};
  inner.declaredFields = {{.symbol = 1605, .type = reference(external.id)}};
  MirClassInstance outer;
  outer.id = 2;
  outer.declaration = 161;
  outer.type = SemanticType::classType(161);
  outer.type.lifetimeArguments = {external, shell};
  outer.lifetimeContract = {.parameters = {external, shell}, .complete = true};
  outer.declaredFields = {{.symbol = 1606, .type =
      SemanticType::referenceTo(inner.type, AccessMode::ReadOnly, shell)}};
  MirProgram program;
  MirProgramTestAccess::classes(program) = {inner, outer};
  MirFunctionInstance function;
  function.id = 1;
  function.lifetimeContract = {.parameters = {external, shell, receiver}, .complete = true};
  MirProgramTestAccess::functions(program) = {function};
  MirBody body;
  body.kind = MirBodyKind::Function;
  body.lifetimeBody = {.variables = {use}, .obligations = {{shell, use}}, .complete = true};
  MirPlace root{.id = 1, .root = MirPlaceRootKind::Binding, .binding = 1,
      .type = SemanticType::referenceTo(outer.type, AccessMode::ReadOnly, receiver)};
  auto selected = root;
  selected.id = 2;
  selected.projections = {{.kind = MirProjectionKind::Dereference},
      {.kind = MirProjectionKind::Field, .field = 1606}, {.kind = MirProjectionKind::Dereference}};
  selected.type = inner.type;
  selected.sourceValue = 31;
  auto nested = selected;
  nested.id = 3;
  nested.projections.push_back({.kind = MirProjectionKind::Field, .field = 1605});
  nested.projections.push_back({.kind = MirProjectionKind::Dereference});
  nested.type = SemanticType::Int32;
  body.places = {root, selected, nested};
  MirInstruction input;
  input.id = 1;
  input.kind = MirInstructionKind::CallInput;
  input.hirValue = 31;
  input.callInputKind = HirCallInputKind::ReadBorrow;
  input.info.type = SemanticType::referenceTo(inner.type, AccessMode::ReadOnly, use);
  input.operands = {{.kind = MirOperandKind::BorrowRead, .place = selected.id, .type = inner.type}};
  body.blocks = {{.id = 1, .instructions = {input}}};
  expect(verifyMirLifetimeApplications(program, body, 1).valid(),
         "an exact addressed receiver keeps its stored shell region despite deeper projections from the same expression");
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto forged = body;
    if (mutation == 0) forged.lifetimeBody.obligations.clear();
    if (mutation == 1) forged.blocks.front().instructions.front().operands.front().place = nested.id;
    if (mutation == 2) forged.places[1].projections[1].field = 1699;
    if (mutation == 3) forged.blocks.front().instructions.front().info.type.referenceLifetime = Lifetime::staticLifetime();
    expect(!verifyMirLifetimeApplications(program, forged, 1).valid(),
           "receiver replay rejects a missing edge, another projected place, unknown field and longer lifetime");
  }
}
void cleanupLifetimeRecipes() {
  const auto region = Lifetime::parameter(801);
  const auto actualRegion = Lifetime::local(901);
  const auto instantiate = [&](const SemanticType &type) {
    return *substituteTypeLifetimes(type, std::array{LifetimeBinding{region, actualRegion}});
  };
  MirProgram program;
  MirClassInstance leaf;
  leaf.id = 1;
  leaf.declaration = 11;
  leaf.type = SemanticType::classType(11);
  leaf.type.lifetimeArguments = {region};
  leaf.lifetimeContract = {.parameters = {region}, .complete = true};
  leaf.destructor = 1;
  leaf.destructorStatus = SpecialMemberStatus::Declared;
  leaf.requiresActiveDropState = true;
  leaf.requiresActiveCleanup = true;
  leaf.declaredFields = {{.symbol = 101, .type = reference(801)}};
  MirClassInstance outer;
  outer.id = 2;
  outer.declaration = 12;
  outer.type = SemanticType::classType(12);
  outer.type.lifetimeArguments = {region};
  outer.lifetimeContract = leaf.lifetimeContract;
  outer.requiresActiveCleanup = true;
  outer.declaredFields = {{.symbol = 102,
      .type = SemanticType::uniqueOwnerOf(leaf.type), .requiresActiveCleanup = true}};
  outer.bases = {{.instance = leaf.id, .type = leaf.type}};
  MirProgramTestAccess::classes(program) = {leaf, outer};
  const auto actualLeaf = instantiate(leaf.type);
  const auto actualOuter = instantiate(outer.type);
  const auto record = mirUniqueCleanupType(program, actualOuter);
  expect(record && record->type == actualOuter && record->classInstance == 2 &&
             record->aggregate.size() == 2 &&
             record->aggregate[0].type == SemanticType::uniqueOwnerOf(actualLeaf) &&
             record->aggregate[0].uniqueOwnerField == 102 &&
             record->aggregate[0].uniquePointeeClass == 1 &&
             record->aggregate[1].type == actualLeaf &&
             record->aggregate[1].uniqueOwnerBase == 1,
         "cleanup substitutes exact field and base regions while retaining physical declaration IDs");
  const auto arrayType = SemanticType::arrayOf(actualOuter, std::size_t{2});
  const auto array = mirUniqueCleanupType(program, arrayType);
  expect(array && array->fixedArray && array->fixedArray->leafType == actualOuter &&
             array->aggregate.size() == 1 && array->aggregate.front() == *record,
         "array cleanup retains its actual leaf recipe and exact child lifetime types");
  MirPayloadEnum payload;
  payload.declaration = 21;
  payload.type = SemanticType::enumType(21);
  payload.type.lifetimeArguments = {region};
  payload.lifetimeContract = leaf.lifetimeContract;
  payload.requiresActiveCleanup = true;
  payload.variants = {{.variantIndex = 0, .payloadTypes = {outer.type},
                       .payloadSymbols = {103}, .payloadRequiresActiveCleanup = {true}}};
  MirProgramTestAccess::payloads(program) = {payload};
  const auto actualPayload = instantiate(payload.type);
  const auto enumeration = mirUniqueCleanupType(program, actualPayload);
  expect(enumeration && enumeration->type == actualPayload &&
             enumeration->aggregate.size() == 1 &&
             enumeration->aggregate.front().type == actualOuter &&
             enumeration->aggregate.front().payloadField == 0,
         "enum cleanup instantiates the actual payload recipe");
  expect(!mirUniqueCleanupType(program, SemanticType::uniqueOwnerOf(actualPayload)) &&
             !mirUniqueCleanupType(program, SemanticType::uniqueOwnerOf(SemanticType::enumType(99))),
         "a failed payload view must never be treated as an integral enum cleanup");
  MirIntegralEnum integral;
  integral.declaration = 22;
  integral.type = SemanticType::enumType(22);
  integral.underlyingType = SemanticType::Int32;
  MirProgramTestAccess::integrals(program) = {integral};
  expect(mirUniqueCleanupType(program, SemanticType::uniqueOwnerOf(integral.type)).has_value(),
         "a known integral enum retains its supported owner cleanup");
  MirLambdaInstance closure;
  closure.id = 1;
  closure.declaration = 31;
  closure.environmentLifetimes = {region};
  closure.environmentContract = {.parameters = {region}, .complete = true};
  closure.type = SemanticType::lambdaType(31, SemanticType::Int32, {}, std::array{outer.type});
  closure.type.lifetimeArguments = {region};
  closure.returnType = SemanticType::Int32;
  closure.lifetimeContract = leaf.lifetimeContract;
  closure.captureTypes = {outer.type};
  closure.captureRequiresActiveCleanup = {true};
  MirProgramTestAccess::lambdas(program) = {closure};
  const auto actualClosure = instantiate(closure.type);
  const auto capture = mirUniqueCleanupType(program, actualClosure);
  expect(capture && capture->type == actualClosure && capture->lambdaInstance == 1 &&
             capture->aggregate.size() == 1 &&
             capture->aggregate.front().type == actualOuter &&
             capture->aggregate.front().captureIndex == 0,
         "closure cleanup substitutes its fixed environment without changing invocation binders");
  MirProgramTestAccess::classes(program).push_back(leaf);
  expect(!mirUniqueCleanupType(program, actualLeaf),
         "ambiguous physical instances cannot select a cleanup target by erased type equality");
}
void owningCleanupShape() {
  auto type = SemanticType::classType(110);
  type.lifetimeArguments = {Lifetime::parameter(1101)};
  auto shortened = type;
  shortened.lifetimeArguments = {Lifetime::inference(1102)};
  MirDropType source{
      .type = SemanticType::arrayOf(type, std::size_t{2}),
      .fixedArray = MirFixedArrayDropSchedule{{2}, type, 1},
      .aggregate = {{.type = type, .classInstance = 1, .destructor = 2,
                     .uniqueOwnerField = 7, .requiresActiveCleanup = true}},
      .requiresActiveCleanup = true};
  auto target = source;
  target.type = SemanticType::arrayOf(shortened, std::size_t{2});
  target.fixedArray->leafType = shortened;
  target.aggregate.front().type = shortened;
  expect(mir_detail::sameLifetimeDropShape(source, target),
         "owning region conversion retains exact cleanup shape");
  for (int mutation = 0; mutation != 8; ++mutation) {
    auto broken = target;
    switch (mutation) {
    case 0: broken.aggregate.front().destructor = 3; break;
    case 1: broken.aggregate.front().uniqueOwnerField = 8; break;
    case 2: broken.aggregate.front().recursiveCleanup = true; break;
    case 3: broken.fixedArray->extents = {3}; break;
    case 4: broken.fixedArray->leafClassInstance = 2; break;
    case 5: broken.aggregate.front().payloadVariant = 1; break;
    case 6: broken.aggregate.clear(); break;
    case 7: broken.aggregate.front().requiresActiveCleanup = false; break;
    }
    expect(!mir_detail::sameLifetimeDropShape(source, broken),
           "region conversion cannot change cleanup identity, selectors or effects");
  }
  target.aggregate.front().type.lifetimeArguments = {Lifetime::staticLifetime()};
  expect(mir_detail::sameLifetimeDropShape(source, target),
         "cleanup shape is not an outlives proof; the mandatory stage gate rejects strengthening");
}
void structuralBorrowPathShape() {
  const BorrowPath path{{.kind = PlaceProjectionKind::Base, .index = 1, .baseOwner = 170},
                        {.kind = PlaceProjectionKind::Field, .field = 171},
                        {.kind = PlaceProjectionKind::ConstantIndex, .index = 2}};
  expect(isWellFormedBorrowPath(path) && isWellFormedBorrowPath({}),
         "borrow paths retain typed base ordinals, stable fields and bounded index shapes");
  for (int mutation = 0; mutation != 8; ++mutation) {
    auto changed = path;
    if (mutation == 0) changed.front().baseOwner = 0;
    else if (mutation == 1) changed.front().field = 171;
    else if (mutation == 2) changed.front().selection = 1;
    else if (mutation == 3) changed[1].baseOwner = 170;
    else if (mutation == 4) changed[1].field = 0;
    else if (mutation == 5) changed.back().kind = PlaceProjectionKind::DynamicIndex;
    else if (mutation == 6) changed.back().kind = PlaceProjectionKind::Dereference;
    else changed.back().kind = static_cast<PlaceProjectionKind>(999);
    expect(!isWellFormedBorrowPath(changed),
           "exportable borrow paths reject body-only operations and mixed projection identities");
  }
  PlaceKey whole{.domain = {.snapshot = 1, .body = 2}, .root = 3};
  auto base = whole;
  base.projections = {path.front()};
  auto field = base;
  field.projections.push_back(path[1]);
  auto different = base;
  different.projections.front().index = 2;
  expect(placeRelation(base, base).relation == PlaceRelation::Equal &&
             placeRelation(whole, field).relation == PlaceRelation::LeftStrictPrefix &&
             placeRelation(field, base).relation == PlaceRelation::RightStrictPrefix &&
             placeRelation(base, different).relation == PlaceRelation::MayAlias,
         "base paths preserve ancestor overlap without inferring unchecked sibling disjointness");
  auto malformed = base;
  malformed.projections.front().baseOwner = 0;
  expect(placeRelation(malformed, malformed).relation == PlaceRelation::MayAlias &&
             placeRelation(malformed, field).relation == PlaceRelation::MayAlias,
         "malformed base metadata cannot prove exact identity or nonoverlap");
}
void projectedOnlyNominalRecipe() {
  NominalVarianceRecipe recipe{
      .declaration = 140,
      .argumentPatterns = {reference(1401)},
      .argumentPatternLifetimes = {{.id = 1401, .lifetime = true}},
      .stored = {{.symbol = 1402, .type = reference(1401)}},
      .complete = true};
  MirClassInstance instance;
  instance.id = 1;
  instance.declaration = recipe.declaration;
  instance.type = SemanticType::classType(recipe.declaration, {reference(1403)});
  instance.liftedLifetimes = {{.id = 1403, .lifetime = true, .implicit = true}};
  instance.lifetimeContract = {.parameters = {Lifetime::parameter(1403)}, .complete = true};
  instance.declaredFields = {{.symbol = 1402, .type = reference(1403)}};
  instance.variance = {.types = {TypeVariance::Invariant}, .complete = true};
  MirProgram program;
  MirProgramTestAccess::classes(program) = {instance};
  MirProgramTestAccess::nominalRecipes(program) = {recipe};
  expect(verifyMirNominalLifetimeRecipes(program).valid(),
         "a projected-only pattern binds its public type argument without an ordinary type binder");
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto invalid = program;
    auto &changed = MirProgramTestAccess::classes(invalid).front();
    if (mutation == 0)
      changed.type.arguments.clear();
    else if (mutation == 1)
      changed.type.arguments.front().arguments.front() = SemanticType::Bool;
    else if (mutation == 2)
      changed.type.lifetimeArguments = {Lifetime::parameter(1403)};
    else if (mutation == 3)
      changed.declaredFields.front().type.referenceLifetime = Lifetime::staticLifetime();
    else
      changed.type.arguments.push_back(reference(1403));
    expect(!verifyMirNominalLifetimeRecipes(invalid).valid(),
           "projected-only patterns still require exact argument arity, shape and stored region");
  }
}
void storedEnumFieldRegion() {
  MirPayloadEnum payload;
  payload.declaration = 150;
  payload.type = SemanticType::enumType(150);
  payload.type.lifetimeArguments = {Lifetime::parameter(1501)};
  payload.lifetimeContract = {.parameters = {Lifetime::parameter(1501)}, .complete = true};
  payload.variants = {{.variantIndex = 0, .payloadTypes = {reference(1501)},
                       .payloadSymbols = {1502}}};
  MirProgram program;
  MirProgramTestAccess::payloads(program) = {payload};
  auto actual = payload.type;
  actual.lifetimeArguments = {Lifetime::staticLifetime()};
  MirBody body;
  body.returnType = SemanticType::referenceTo(SemanticType::Int32, AccessMode::ReadOnly,
                                             Lifetime::staticLifetime());
  MirPlace root;
  root.id = 1;
  root.root = MirPlaceRootKind::Binding;
  root.binding = 1;
  root.type = SemanticType::referenceTo(actual, AccessMode::ReadOnly, Lifetime::local(1503));
  auto field = root;
  field.id = 2;
  field.type = SemanticType::Int32;
  field.projections = {{.kind = MirProjectionKind::Dereference},
                       {.kind = MirProjectionKind::Field, .field = 1502},
                       {.kind = MirProjectionKind::Dereference}};
  body.places = {root, field};
  MirLoan returned;
  returned.id = 1;
  returned.kind = MirLoanKind::Return;
  returned.source = field.id;
  body.loans = {returned};
  MirBlock block;
  block.id = 1;
  block.terminator.kind = MirTerminatorKind::Return;
  block.terminator.value = MirOperand{.kind = MirOperandKind::Loan, .loan = 1,
                                      .type = SemanticType::Int32};
  body.blocks = {block};
  expect(verifyMirLifetimeApplications(program, body).valid(),
         "an enum field's external reference region is independent of carrier storage");
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto invalid = program;
    auto changed = body;
    if (mutation == 0)
      changed.places.front().type.arguments.front().lifetimeArguments = {Lifetime::local(1503)};
    else if (mutation == 1)
      changed.places.back().projections[1].field = 9999;
    else if (mutation == 2) {
      auto duplicate = payload.variants.front();
      duplicate.variantIndex = 1;
      MirProgramTestAccess::payloads(invalid).front().variants.push_back(duplicate);
    } else
      MirProgramTestAccess::payloads(invalid).front().variants.front().payloadTypes.clear();
    expect(!verifyMirLifetimeApplications(invalid, changed).valid(),
           "enum field region proof rejects missing, ambiguous, malformed and strengthened paths");
  }
}
void scopedCallableNominalRecipe() {
  const auto result = SemanticType::typeParameter(1601);
  const auto arguments = SemanticType::typePack(1602);
  const auto pattern = SemanticType::functionSignature(result, {arguments});
  auto storedPattern = pattern;
  storedPattern.kind = SemanticType::NativeFunction;
  auto signature = SemanticType::functionSignature(reference(1611),
                                                    {reference(1611), reference(1612)});
  signature.quantifiedLifetimes = {1611, 1612, 1613};
  auto storedSignature = signature;
  storedSignature.kind = SemanticType::NativeFunction;
  NominalVarianceRecipe recipe{
      .declaration = 160,
      .argumentPatterns = {pattern},
      .parameters = {{.id = 1601, .typeArgumentPath = {0, 0}},
                     {.id = 1602, .pack = true, .typeArgumentPath = {0, 1}},
                     {.id = 1621, .lifetime = true},
                     {.id = 1622, .lifetime = true}},
      .stored = {{.symbol = 1631, .type = storedPattern},
                 {.index = 1, .symbol = 1632, .type = reference(1621)},
                 {.index = 2, .symbol = 1633, .type = reference(1622)}},
      .complete = true};
  MirClassInstance instance;
  instance.id = 1;
  instance.declaration = recipe.declaration;
  instance.type = SemanticType::classType(recipe.declaration, {signature}, {},
                                         {Lifetime::parameter(1622), Lifetime::parameter(1623)});
  instance.liftedLifetimes = {{.id = 1611, .lifetime = true, .implicit = true},
                            {.id = 1612, .lifetime = true, .implicit = true},
                            {.id = 1613, .lifetime = true, .implicit = true},
                            {.id = 1623, .lifetime = true, .implicit = true}};
  instance.lifetimeContract = {.parameters = {Lifetime::parameter(1622), Lifetime::parameter(1623)},
                               .complete = true};
  instance.declaredFields = {{.symbol = 1631, .type = storedSignature},
                             {.symbol = 1632, .type = reference(1622)},
                             {.symbol = 1633, .type = reference(1623)}};
  instance.variance = {.types = {TypeVariance::Invariant},
                      .lifetimes = {TypeVariance::Covariant, TypeVariance::Covariant},
                      .complete = true};
  MirProgram program;
  MirProgramTestAccess::classes(program) = {instance};
  MirProgramTestAccess::nominalRecipes(program) = {recipe};
  expect(verifyMirNominalLifetimeRecipes(program).valid(),
         "MIR reconstructs a closed callable with unused binders and applies ordinary region maps once");
  auto alpha = program;
  auto &alphaField = MirProgramTestAccess::classes(alpha).front().declaredFields.front().type;
  alphaField.quantifiedLifetimes = {1641, 1642, 1643};
  alphaField.arguments[0].referenceLifetime = Lifetime::parameter(1641);
  alphaField.arguments[1].referenceLifetime = Lifetime::parameter(1641);
  alphaField.arguments[2].referenceLifetime = Lifetime::parameter(1642);
  expect(verifyMirNominalLifetimeRecipes(alpha).valid(),
         "closed stored callable scopes compare by exact alpha correspondence");
  for (int mutation = 0; mutation != 9; ++mutation) {
    auto invalid = program;
    auto &changed = MirProgramTestAccess::classes(invalid).front();
    auto &source = MirProgramTestAccess::nominalRecipes(invalid).front();
    if (mutation == 0)
      changed.declaredFields.front().type.quantifiedLifetimes.pop_back();
    else if (mutation == 1)
      changed.declaredFields.front().type.arguments.front().referenceLifetime = Lifetime::parameter(1612);
    else if (mutation == 2)
      changed.declaredFields.front().type.arguments.front().referenceLifetime = Lifetime::staticLifetime();
    else if (mutation == 3)
      changed.declaredFields[1].type.referenceLifetime = Lifetime::parameter(1623);
    else if (mutation == 4)
      changed.declaredFields[1].type.referenceLifetime = Lifetime::parameter(1621);
    else if (mutation == 5) {
      source.stored.front().type = result;
      changed.declaredFields.front().type = reference(1611);
    } else if (mutation == 6) {
      source.stored.front().type = result;
      changed.declaredFields.front().type = SemanticType::Unknown;
    } else if (mutation == 7)
      changed.type.arguments.front().quantifiedLifetimes.push_back(1611);
    else
      changed.declaredFields.front().type.arguments.back().referenceLifetime = Lifetime::parameter(1699);
    expect(!verifyMirNominalLifetimeRecipes(invalid).valid(),
           "scoped recipes reject lost binders, changed correlations, free escape and chained substitutions");
  }
  auto named = program;
  auto &namedRecipe = MirProgramTestAccess::nominalRecipes(named).front();
  auto shell = SemanticType::referenceTo(result, AccessMode::ReadOnly, Lifetime::parameter(1651));
  namedRecipe.argumentPatterns = {SemanticType::functionSignature(shell, {arguments})};
  namedRecipe.argumentPatternLifetimes = {{.id = 1651, .lifetime = true}};
  namedRecipe.parameters.front().typeArgumentPath = {0, 0, 0};
  namedRecipe.stored.front().type = namedRecipe.argumentPatterns.front();
  namedRecipe.stored.front().type.kind = SemanticType::NativeFunction;
  expect(verifyMirNominalLifetimeRecipes(named).valid(),
         "a projected named region remains scoped to its complete callable recipe");
  auto fixed = named;
  MirProgramTestAccess::nominalRecipes(fixed).front().argumentPatterns.front()
      .arguments.front().referenceLifetime = Lifetime::staticLifetime();
  MirProgramTestAccess::nominalRecipes(fixed).front().argumentPatternLifetimes.clear();
  expect(!verifyMirNominalLifetimeRecipes(fixed).valid(),
         "fixed static callable regions cannot match arbitrary invocation binders");
  auto repeated = named;
  auto &repeatedRecipe = MirProgramTestAccess::nominalRecipes(repeated).front();
  repeatedRecipe.parameters.erase(repeatedRecipe.parameters.begin() + 1);
  repeatedRecipe.argumentPatterns = {SemanticType::functionSignature(shell, {shell})};
  repeatedRecipe.stored.front().type = repeatedRecipe.argumentPatterns.front();
  repeatedRecipe.stored.front().type.kind = SemanticType::NativeFunction;
  auto &repeatedClass = MirProgramTestAccess::classes(repeated).front();
  repeatedClass.type.arguments.front().arguments.pop_back();
  repeatedClass.declaredFields.front().type.arguments.pop_back();
  expect(verifyMirNominalLifetimeRecipes(repeated).valid(),
         "repeated named slots preserve the same invocation relationship");
  repeatedClass.type.arguments.front().arguments.back().referenceLifetime = Lifetime::parameter(1612);
  repeatedClass.declaredFields.front().type.arguments.back().referenceLifetime = Lifetime::parameter(1612);
  expect(!verifyMirNominalLifetimeRecipes(repeated).valid(),
         "a selected structural family cannot weaken a repeated named region relationship");
}
void quantifiedNominalLiftInventory() {
  auto signature = SemanticType::functionSignature(reference(1001), {reference(1001)});
  signature.quantifiedLifetimes = {1001};
  NominalVarianceRecipe recipe{
      .declaration = 100, .parameters = {{.id = 101}, {.id = 102}}, .complete = true};
  MirClassInstance wrapper;
  wrapper.id = 1;
  wrapper.declaration = recipe.declaration;
  wrapper.type = SemanticType::classType(recipe.declaration, {signature, SemanticType::Int32});
  wrapper.liftedLifetimes = {{.id = 1001, .lifetime = true, .implicit = true}};
  wrapper.lifetimeContract.complete = true;
  wrapper.variance = {.types = {TypeVariance::Bivariant, TypeVariance::Bivariant}, .complete = true};
  MirProgram program;
  MirProgramTestAccess::classes(program) = {wrapper};
  MirProgramTestAccess::nominalRecipes(program) = {recipe};
  expect(verifyMirNominalLifetimeRecipes(program).valid(),
         "a lifted callable binder stays scoped to its exact nominal type argument");
  auto &instance = MirProgramTestAccess::classes(program).front();
  instance.liftedLifetimes.front().id = 1002;
  expect(!verifyMirNominalLifetimeRecipes(program).valid(),
         "an unused lifted identity cannot claim an unrelated nested callable scope");
  instance = wrapper;
  instance.lifetimeContract.parameters = {Lifetime::parameter(1001)};
  expect(!verifyMirNominalLifetimeRecipes(program).valid(),
         "a nested callable binder cannot become an owner contract assumption");
  instance = wrapper;
  instance.type.arguments.back() = reference(1001);
  expect(!verifyMirNominalLifetimeRecipes(program).valid(),
         "a captured invocation binder cannot escape its callable type argument");
}
} // namespace
int main() {
  stagedReferenceContentsMetadata();
  stagedReferenceContentsTransport();
  nestedCarrierPublication();
  erasedCarrierReturnPublication();
  closureEnvironmentReplay();
  rawPointerStorageOutlives();
  packEntrySourceIdentity();
  exactReceiverProjectionRegion();
  packElementLifetimeReplay();
  cleanupLifetimeRecipes();
  owningCleanupShape();
  quantifiedNominalLiftInventory();
  scopedCallableNominalRecipe();
  projectedOnlyNominalRecipe();
  structuralBorrowPathShape();
  storedEnumFieldRegion();
  const auto fixedEnvironment = Lifetime::parameter(701);
  const std::array closureParameters{reference(702)};
  const std::array closureCaptures{reference(701)};
  MirLambdaInstance closure{
      .id = 1, .declaration = 70, .environmentLifetimes = {fixedEnvironment},
      .environmentContract = {.parameters = {fixedEnvironment}, .complete = true},
      .type = SemanticType::lambdaType(70, reference(702), closureParameters, closureCaptures),
      .returnType = reference(702), .parameterTypes = {reference(702)},
      .lifetimeContract = {.parameters = {fixedEnvironment, Lifetime::parameter(702)}, .complete = true},
      .captureTypes = {reference(701)}};
  closure.type.lifetimeArguments = {fixedEnvironment};
  closure.type.quantifiedLifetimes = {702};
  const auto actualClosure = substituteTypeLifetimes(
      closure.type, std::array{LifetimeBinding{fixedEnvironment, Lifetime::staticLifetime()}});
  expect(actualClosure && mirLambdaLifetimeView(closure, *actualClosure),
         "closure environment choices instantiate every occurrence while invocation stays quantified");
  auto changedCapture = *actualClosure;
  changedCapture.arguments.back().referenceLifetime = Lifetime::local(71);
  expect(!mirLambdaLifetimeView(closure, changedCapture),
         "closure matching rejects a capture inconsistent with its fixed environment slot");
  auto changedInvocation = *actualClosure;
  changedInvocation.arguments.front().referenceLifetime = Lifetime::staticLifetime();
  expect(!mirLambdaLifetimeView(closure, changedInvocation),
         "a fixed environment choice cannot specialize a quantified invocation result");
  auto quantified = SemanticType::functionSignature(reference(101), {reference(101)});
  quantified.quantifiedLifetimes = {101};
  const auto quantifiedContract = callableLifetimeContract(quantified);
  expect(quantifiedContract && quantifiedContract->parameters ==
                                  std::vector<Lifetime>{Lifetime::parameter(101)} &&
             quantifiedContract->typeBounds.size() == 1,
         "callable contracts derive scoped universal inputs and exact referent bounds");
  CallableLifetimeSignature quantifiedProof{.signature = quantified,
                                             .contract = *quantifiedContract};
  auto alpha = SemanticType::functionSignature(reference(202), {reference(202)});
  alpha.quantifiedLifetimes = {202};
  expect(callableLifetimeCarrierMatches(quantifiedProof, alpha),
         "higher-ranked carrier identities compare by scoped alpha equivalence");
  quantifiedProof.signatureBindings = {{Lifetime::parameter(101), Lifetime::staticLifetime()}};
  expect(!callableLifetimeCarrierMatches(quantifiedProof, alpha),
         "fixed carrier substitutions cannot instantiate a quantified binder");
  auto fixedSignature = SemanticType::functionSignature(reference(303), {reference(303)});
  CallableLifetimeSignature fixedProof{
      .signature = fixedSignature,
      .contract = *callableLifetimeContract(fixedSignature),
      .signatureBindings = {{Lifetime::parameter(303), Lifetime::local(404)}}};
  const auto fixedCarrier = substituteTypeLifetimes(fixedSignature, fixedProof.signatureBindings);
  expect(fixedCarrier && callableLifetimeCarrierMatches(fixedProof, *fixedCarrier),
         "a universal carrier slot reconstructs its exact local region without publishing it");
  fixedProof.signatureBindings[0].second = Lifetime::local(405);
  expect(!callableLifetimeCarrierMatches(fixedProof, *fixedCarrier),
         "changing a fixed carrier region fails exact signature identity");
  fixedProof.signatureBindings[0].second = Lifetime::local(404);
  fixedProof.contract.typeBounds.clear();
  expect(!callableLifetimeCarrierMatches(fixedProof, *fixedCarrier),
         "a complete flag cannot replace the derived callable type-outlives obligations");
  const auto targetBindings = callableLifetimeTargetBindings(
      reference(303), std::array{reference(303)},
      *callableLifetimeContract(fixedSignature), quantified);
  expect(targetBindings && *targetBindings ==
              std::vector<LifetimeBinding>{{Lifetime::parameter(303), Lifetime::parameter(101)}},
         "a universal target may implement an alpha-renamed higher-ranked signature");
  auto requested = SemanticType::functionSignature(reference(101), {reference(101), reference(102)});
  requested.quantifiedLifetimes = {101, 102};
  CallableLifetimeSignature requestedProof{
      .signature = requested, .contract = *callableLifetimeContract(requested)};
  auto constrainedSignature = SemanticType::functionSignature(reference(303), {reference(303), reference(304)});
  auto constrained = *callableLifetimeContract(constrainedSignature);
  using RequirementKind = LifetimeRequirementNode::Kind;
  constrained.requirements.nodes = {
      {.kind = RequirementKind::Region,
       .bound = {Lifetime::parameter(303), Lifetime::parameter(304)}},
      {.kind = RequirementKind::Region,
       .bound = {Lifetime::parameter(304), Lifetime::parameter(303)}},
      {.kind = RequirementKind::Any, .left = 0, .right = 1}};
  constrained.requirements.root = 2;
  MirProgram callableProgram;
  expect(!mirCallableLifetimeTargetMatches(callableProgram, requestedProof, reference(303),
                                           std::array{reference(303), reference(304)}, constrained),
         "a target disjunction cannot disappear behind empty guaranteed bounds");
  requested.arguments[2] = reference(101);
  requestedProof = {.signature = requested, .contract = *callableLifetimeContract(requested)};
  expect(mirCallableLifetimeTargetMatches(callableProgram, requestedProof, reference(303),
                                          std::array{reference(303), reference(304)}, constrained),
         "a proved disjunct suffices without assuming every leaf");
  constrained.requirements.nodes[0] = {.kind = RequirementKind::SemanticPredicate, .predicate = 0};
  expect(!mirCallableLifetimeTargetMatches(callableProgram, requestedProof, reference(303),
                                           std::array{reference(303), reference(304)}, constrained),
         "unresolved target predicate truth cannot reach executable proof");
  NominalVarianceRecipe shared{
      .declaration = 1, .parameters = {{.id = 501}, {.id = 502, .lifetime = true}},
      .stored = {{.symbol = 601, .type = SemanticType::referenceTo(
          SemanticType::typeParameter(501), AccessMode::ReadOnly, Lifetime::parameter(502))}},
      .complete = true};
  auto exclusive = shared;
  exclusive.declaration = 2;
  exclusive.stored[0].type.referenceAccess = AccessMode::Mutable;
  const auto inferred = inferNominalVarianceRecipes(std::array{shared, exclusive});
  expect(inferred && (*inferred)[0].types == std::vector{TypeVariance::Covariant} &&
             (*inferred)[1].types == std::vector{TypeVariance::Invariant} &&
             (*inferred)[1].lifetimes == std::vector{TypeVariance::Covariant},
         "shared symbolic recipes distinguish mutable referents from covariant borrow regions");
  auto specialization = exclusive;
  specialization.exactSpecializationPrimary = 1;
  specialization.parameters.erase(specialization.parameters.begin());
  specialization.exactTypeArguments = {SemanticType::Int32};
  specialization.stored[0].type = SemanticType::rawPointerTo(reference(502));
  const auto family = inferNominalVarianceRecipes(std::array{shared, specialization});
  expect(family && (*family)[0].lifetimes == std::vector{TypeVariance::Invariant} &&
             (*family)[1].types == std::vector{TypeVariance::Invariant},
         "an exact specialization retains public type slots and constrains family lifetime variance");
  auto specializedType = SemanticType::classType(2, {reference(701)});
  specializedType.lifetimeArguments = {Lifetime::parameter(702)};
  const auto expandedSpec = expandNominalVarianceRecipe(specializedType, specialization,
      (*family)[1], std::array{shared, specialization});
  expect(expandedSpec.complete && expandedSpec.types.size() == 1 &&
             expandedSpec.lifetimes.size() == 1,
         "specialized public arguments remain visible to nested-region variance proof");
  specialization.parameters.push_back(specialization.parameters.front());
  expect(!inferNominalVarianceRecipes(std::array{shared, specialization}),
         "duplicate symbolic parameter identities cannot enter variance proof");
  NominalVarianceRecipe projected{
      .declaration = 3,
      .argumentPatterns = {SemanticType::referenceTo(
          SemanticType::typeParameter(801), AccessMode::ReadOnly,
          Lifetime::parameter(802))},
      .argumentPatternLifetimes = {{.id = 802, .lifetime = true, .implicit = true}},
      .parameters = {{.id = 801, .typeArgumentPath = {0, 0}}},
      .stored = {{.symbol = 803, .type = SemanticType::rawPointerTo(reference(802))}},
      .complete = true};
  const auto projectedVariance = inferNominalVarianceRecipes(std::array{projected});
  expect(projectedVariance && projectedVariance->front().types ==
             std::vector{TypeVariance::Invariant} &&
             projectedVariance->front().lifetimes.empty(),
         "projected pattern regions do not add top-level arity or grant unchecked variance");
  auto namedProjected = projected;
  namedProjected.argumentPatternLifetimes.front().implicit = false;
  const auto namedVariance = inferNominalVarianceRecipes(std::array{namedProjected});
  expect(namedVariance && *namedVariance == *projectedVariance,
         "named pattern binders retain the same exact scope and public arity as anonymous slots");
  auto unboundNamedProjection = namedProjected;
  unboundNamedProjection.argumentPatternLifetimes.front().id = 804;
  expect(!inferNominalVarianceRecipes(std::array{unboundNamedProjection}),
         "a named pattern binder must occur exactly in its declared recipe scope");
  const std::array projectedPatterns{reference(802), reference(802)};
  const std::array projectedActual{reference(901), reference(901)};
  const std::array projectedRegions{Lifetime::parameter(802)};
  const auto projectedBindings = matchUniversalTypeArgumentLifetimes(
      projectedPatterns, projectedActual, projectedRegions);
  expect(projectedBindings && *projectedBindings == std::vector<LifetimeBinding>{
             {Lifetime::parameter(802), Lifetime::parameter(901)}},
         "a projected region binding must agree across all public type arguments");
  expect(!matchUniversalTypeArgumentLifetimes(
             projectedPatterns, std::array{reference(901), reference(902)}, projectedRegions),
         "a repeated projected binder cannot receive inconsistent actual regions");
  auto duplicateProjected = projected;
  duplicateProjected.argumentPatternLifetimes.push_back(
      duplicateProjected.argumentPatternLifetimes.front());
  expect(!inferNominalVarianceRecipes(std::array{duplicateProjected}),
         "duplicate projected lifetime binder identity is invalid");
  auto foreignProjected = projected;
  foreignProjected.argumentPatterns.front().referenceLifetime = Lifetime::parameter(804);
  expect(!inferNominalVarianceRecipes(std::array{foreignProjected}),
         "an argument pattern cannot reference an undeclared region binder");
  auto unusedProjected = projected;
  unusedProjected.argumentPatterns.front().referenceLifetime = Lifetime::staticLifetime();
  expect(!inferNominalVarianceRecipes(std::array{unusedProjected}),
         "a projected lifetime binder requires an exact pattern occurrence");
  MirBody body;
  body.returnType = reference(1);
  body.entry = 1;
  MirBlock block;
  block.id = 1;
  MirInstruction call;
  call.id = 1;
  call.kind = MirInstructionKind::Call;
  call.info.type = reference(1);
  call.parameterTypes = {reference(1)};
  call.lifetimeArguments = {Lifetime::parameter(1)};
  call.lifetimeApplication.bindings = {
      {Lifetime::parameter(2), Lifetime::parameter(1)}};
  call.lifetimeApplication.variables = {Lifetime::inference(3)};
  call.lifetimeApplication.obligations = {
      {Lifetime::parameter(1), Lifetime::inference(3)}};
  call.lifetimeApplication.typeObligations = {
      {SemanticType::typeParameter(4), Lifetime::inference(3)}};
  call.lifetimeApplication.origins = {{.region = Lifetime::local(5),
                                       .place = 1,
                                       .key = {.domain = {1, 2, 3}, .root = 6},
                                       .source = 7}};
  call.lifetimeApplication.complete = true;
  block.instructions.push_back(call);
  body.blocks.push_back(block);
  const MirPrinter printer;
  const auto original = printer.print(body);
  expect(
      original.starts_with("mir-body-v96\n"),
      "lifetime-bearing structural metadata requires its own format version");
  auto changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.callableSignature = fixedProof;
  const auto callableBytes = printer.print(changed);
  changed.blocks[0].instructions[0].lifetimeApplication.callableSignature->signatureBindings[0].second =
      Lifetime::local(406);
  expect(printer.print(changed) != callableBytes,
         "fixed callable regions participate in structural MIR identity");
  changed = body;
  changed.returnType.referenceLifetime = Lifetime::staticLifetime();
  expect(
      printer.print(changed) != original,
      "changing a return region must invalidate a structural body fingerprint");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeArguments[0] =
      Lifetime::staticLifetime();
  expect(printer.print(changed) != original,
         "generic lifetime arguments must participate in structural identity");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.bindings[0].second =
      Lifetime::staticLifetime();
  expect(printer.print(changed) != original,
         "changing an actual contract substitution must invalidate the proof "
         "fingerprint");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.bindings[0].first =
      Lifetime::parameter(3);
  expect(printer.print(changed) != original,
         "a contract binding's declaration identity is part of its proof");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.variables[0] =
      Lifetime::inference(8);
  expect(printer.print(changed) != original,
         "inference variable allocation must participate in proof identity");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.obligations[0].longer =
      Lifetime::staticLifetime();
  expect(printer.print(changed) != original,
         "required outlives edges must participate in proof identity");
  changed = body;
  changed.blocks[0]
      .instructions[0]
      .lifetimeApplication.typeObligations[0]
      .type = SemanticType::typeParameter(9);
  expect(printer.print(changed) != original,
         "type outlives obligations retain the exact bounded type");
  changed = body;
  changed.blocks[0]
      .instructions[0]
      .lifetimeApplication.typeObligations[0]
      .region = Lifetime::staticLifetime();
  expect(printer.print(changed) != original,
         "type outlives obligations retain the required region");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.origins[0].region =
      Lifetime::local(10);
  expect(printer.print(changed) != original,
         "local region witnesses retain their checked region identity");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.origins[0].place = 2;
  expect(printer.print(changed) != original,
         "origin storage identity must participate in proof identity");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.origins[0].key.root =
      11;
  expect(printer.print(changed) != original,
         "origin semantic place keys must participate in proof identity");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.origins[0].source = 12;
  expect(printer.print(changed) != original,
         "temporary origin source identity must participate in proof identity");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.complete = false;
  expect(printer.print(changed) != original,
         "incomplete lifetime applications cannot reuse checked fingerprints");
  changed = body;
  changed.lifetimeBody.complete = true;
  expect(printer.print(changed) != original,
         "body lifetime proof completeness must participate in fingerprints");
  changed.lifetimeBody.variables = call.lifetimeApplication.variables;
  const auto inventoryOnly = printer.print(changed);
  changed.lifetimeBody.obligations = call.lifetimeApplication.obligations;
  expect(printer.print(changed) != inventoryOnly,
         "body assignment and return constraints must participate in "
         "fingerprints");
  auto callable = SemanticType::functionSignature(reference(4), {reference(4)});
  callable.quantifiedLifetimes = {4};
  body.returnType = callable;
  changed = body;
  changed.returnType.quantifiedLifetimes.clear();
  expect(printer.print(changed) != printer.print(body),
         "a quantified callable and a free-region callable cannot share a "
         "fingerprint");
  SemanticType view(SemanticType::StringView);
  view.lifetimeArguments = {Lifetime::parameter(1)};
  body.returnType = view;
  changed = body;
  changed.returnType.lifetimeArguments = {Lifetime::staticLifetime()};
  expect(
      printer.print(changed) != printer.print(body),
      "borrowing value regions must participate in structural type identity");
  auto projection = SemanticType::typeParameter(0);
  projection.projectionConcept = 77;
  projection.projectionMember = 0;
  projection.arguments = {SemanticType::typeParameter(5)};
  body.returnType = projection;
  changed = body;
  changed.returnType.projectionMember = 1;
  expect(printer.print(changed) != printer.print(body),
         "distinct associated members in exact type bounds need distinct "
         "fingerprints");
  changed = body;
  changed.returnType.projectionConcept = 78;
  expect(printer.print(changed) != printer.print(body),
         "associated concept identity cannot be replaced by its argument "
         "spelling");
  HirLifetimeApplication source;
  source.bindings = call.lifetimeApplication.bindings;
  source.variables = call.lifetimeApplication.variables;
  source.obligations = call.lifetimeApplication.obligations;
  source.typeObligations = call.lifetimeApplication.typeObligations;
  source.complete = true;
  source.origins = {{.region = Lifetime::local(5),
                     .place = {.domain = {1, 2, 3}, .root = 6},
                     .source = 7}};
  auto application = lowerMirLifetimeApplication(source);
  MirBody originBody;
  MirPlace named;
  named.id = 1;
  named.root = MirPlaceRootKind::Binding;
  named.binding = 1;
  named.sourceValue = 7;
  named.key = source.origins[0].place;
  originBody.places.push_back(named);
  expect(resolveMirLifetimeOrigins(application, originBody) &&
             application.origins[0].place == named.id &&
             preservesHirLifetimeApplication(source, application),
         "an exact named origin must survive HIR to MIR mapping");
  HirLifetimeBody bodyFacts{.variables = source.variables,
                            .obligations = source.obligations,
                            .typeObligations = source.typeObligations,
                            .origins = source.origins,
                            .complete = true};
  auto loweredBodyFacts = lowerMirLifetimeBody(bodyFacts);
  expect(resolveMirLifetimeOrigins(loweredBodyFacts, originBody) &&
             preservesHirLifetimeBody(bodyFacts, loweredBodyFacts),
         "whole-body lifetime facts retain their exact mapped origins");
  loweredBodyFacts.typeObligations.clear();
  expect(!preservesHirLifetimeBody(bodyFacts, loweredBodyFacts),
         "whole-body lowering cannot drop exact type obligations");
  auto damaged = application;
  damaged.obligations.clear();
  expect(!preservesHirLifetimeApplication(source, damaged),
         "lowering must not drop a required outlives edge");
  damaged = application;
  damaged.origins[0].source = 8;
  expect(!preservesHirLifetimeApplication(source, damaged),
         "lowering must preserve the origin source exactly");
  auto wrongBody = originBody;
  wrongBody.places[0].key->domain.revision += 1;
  expect(
      !resolveMirLifetimeOrigins(application, wrongBody),
      "equal symbol IDs in a different place domain are not origin evidence");
  source.origins[0].place = {};
  application = lowerMirLifetimeApplication(source);
  expect(!resolveMirLifetimeOrigins(application, originBody),
         "a temporary source cannot be inferred from a named binding");
  originBody.places[0].root = MirPlaceRootKind::Temporary;
  originBody.places[0].key.reset();
  originBody.places[0].temporary = 1;
  expect(resolveMirLifetimeOrigins(application, originBody) &&
             preservesHirLifetimeApplication(source, application),
         "a unique temporary materialization retains its exact source");
  auto secondTemporary = originBody.places[0];
  secondTemporary.id = 2;
  secondTemporary.temporary = 2;
  originBody.places.push_back(secondTemporary);
  expect(!resolveMirLifetimeOrigins(application, originBody),
         "ambiguous temporary materializations must not guess an origin");
  LifetimeSignatureContract contract{
      .parameters = {Lifetime::parameter(1), Lifetime::parameter(2)},
      .bounds = {{Lifetime::parameter(1), Lifetime::parameter(2)}},
      .typeBounds = {{SemanticType::Int32, Lifetime::parameter(2)}},
      .complete = true};
  MirLifetimeApplication applied{
      .bindings = {{Lifetime::parameter(1), Lifetime::parameter(11)},
                   {Lifetime::parameter(2), Lifetime::inference(12)}},
      .variables = {Lifetime::inference(12)},
      .obligations = {{Lifetime::parameter(11), Lifetime::inference(12)}},
      .typeObligations = {{SemanticType::Int32, Lifetime::inference(12)}},
      .complete = true};
  const std::vector<SemanticType> declaredParameters{reference(1)};
  const auto instantiated = instantiateMirLifetimeSignature(
      contract, reference(2), declaredParameters, applied);
  expect(instantiated &&
             instantiated->returnType.referenceLifetime ==
                 Lifetime::inference(12) &&
             instantiated->parameterTypes[0].referenceLifetime ==
                 Lifetime::parameter(11),
         "MIR must instantiate every declared region without changing its "
         "signature shape");
  auto invalidApplication = applied;
  invalidApplication.bindings.pop_back();
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "a missing universal binding is not an instantiated contract");
  invalidApplication = applied;
  invalidApplication.bindings[1].first = Lifetime::parameter(1);
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "duplicate binding keys cannot cover a different universal");
  invalidApplication = applied;
  invalidApplication.bindings[1].first = Lifetime::parameter(3);
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "unknown binding keys cannot belong to a selected declaration");
  invalidApplication = applied;
  invalidApplication.variables.clear();
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "inference IDs need an explicit allocation inventory");
  invalidApplication = applied;
  invalidApplication.obligations.clear();
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "callee bounds remain required obligations after instantiation");
  invalidApplication = applied;
  invalidApplication.typeObligations[0].type = SemanticType::Int64;
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "an exact type bound cannot be replaced by another type's bound");
  invalidApplication = applied;
  invalidApplication.complete = false;
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "incomplete applications cannot authorize lifetime instantiation");
  auto badDeclaration = contract;
  badDeclaration.parameters.pop_back();
  expect(!instantiateMirLifetimeSignature(badDeclaration, reference(2),
                                          declaredParameters, applied),
         "free signature regions require declaration-owned binders");
  auto quantifiedResult =
      SemanticType::functionSignature(reference(1), {reference(13)});
  quantifiedResult.quantifiedLifetimes = {13};
  invalidApplication = applied;
  invalidApplication.bindings[0].second = Lifetime::parameter(13);
  invalidApplication.obligations[0].longer = Lifetime::parameter(13);
  expect(!instantiateMirLifetimeSignature(contract, quantifiedResult,
                                          declaredParameters,
                                          invalidApplication),
         "call substitution cannot capture a higher-ranked binder");
  expect(instantiateMirLifetimeSignature({}, SemanticType::Int32, {}, {})
             .has_value(),
         "region-free legacy signatures retain their exact behavior");
  MirProgram emptyProgram;
  LifetimeSignatureContract caller{
      .parameters = {Lifetime::parameter(21), Lifetime::parameter(22)},
      .complete = true};
  MirBody graphBody;
  graphBody.lifetimeBody = {
      .variables = {Lifetime::inference(23)},
      .obligations = {{Lifetime::parameter(21), Lifetime::inference(23)},
                      {Lifetime::parameter(22), Lifetime::inference(23)}},
      .complete = true};
  expect(verifyMirLifetimeBodyFacts(emptyProgram, graphBody, caller).valid(),
         "two independent inputs may constrain one existential meet");
  LifetimeSignatureContract cases{
      .parameters = {Lifetime::parameter(21), Lifetime::parameter(22),
                     Lifetime::parameter(24), Lifetime::parameter(25)},
      .complete = true};
  using CaseKind = LifetimeRequirementNode::Kind;
  cases.requirements.nodes = {
      {.kind = CaseKind::Region, .bound = {Lifetime::parameter(21), Lifetime::parameter(22)}},
      {.kind = CaseKind::Region, .bound = {Lifetime::parameter(22), Lifetime::parameter(25)}},
      {.kind = CaseKind::All, .left = 0, .right = 1},
      {.kind = CaseKind::Region, .bound = {Lifetime::parameter(21), Lifetime::parameter(24)}},
      {.kind = CaseKind::Region, .bound = {Lifetime::parameter(24), Lifetime::parameter(25)}},
      {.kind = CaseKind::All, .left = 3, .right = 4},
      {.kind = CaseKind::Any, .left = 2, .right = 5}};
  cases.requirements.root = 6;
  MirBody caseBody;
  caseBody.lifetimeBody = {
      .obligations = {{Lifetime::parameter(21), Lifetime::parameter(25)}},
      .complete = true};
  expect(verifyMirLifetimeBodyFacts(emptyProgram, caseBody, cases).valid(),
         "each caller disjunct may prove the same transitive required relation");
  caseBody.lifetimeBody.obligations.push_back({Lifetime::parameter(22), Lifetime::parameter(25)});
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, caseBody, cases).valid(),
         "a relation true in one disjunct cannot become an unconditional body edge");
  auto unboundTypeContract = caller;
  unboundTypeContract.typeBounds = {{reference(999), Lifetime::parameter(21)}};
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, graphBody, unboundTypeContract).valid(),
         "type-outlives assumptions cannot introduce an unbound nested region");
  unboundTypeContract.typeBounds.clear();
  unboundTypeContract.requirements.nodes = {{.kind = CaseKind::Type,
      .bound = {{}, Lifetime::parameter(21)}, .type = reference(999)}};
  unboundTypeContract.requirements.root = 0;
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, graphBody, unboundTypeContract).valid(),
         "a requirement type leaf must be scoped before it becomes a branch assumption");
  auto forgedGraph = graphBody;
  forgedGraph.lifetimeBody.obligations.push_back(
      {Lifetime::inference(23), Lifetime::parameter(22)});
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, forgedGraph, caller).valid(),
         "an inference meet must not smuggle an unrelated universal outlives "
         "proof");
  auto relatedCaller = caller;
  relatedCaller.bounds = {{Lifetime::parameter(21), Lifetime::parameter(22)}};
  expect(verifyMirLifetimeBodyFacts(emptyProgram, forgedGraph, relatedCaller)
             .valid(),
         "a declared universal relationship may discharge the required path");
  forgedGraph = graphBody;
  forgedGraph.lifetimeBody.variables.clear();
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, forgedGraph, caller).valid(),
         "body graph edges cannot invent inference identities");
  forgedGraph = graphBody;
  forgedGraph.lifetimeBody.obligations[0].longer = Lifetime::local(24);
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, forgedGraph, caller).valid(),
         "a local region requires an exact origin witness");
  graphBody.lifetimeBody.typeObligations = {
      {SemanticType::typeParameter(25), Lifetime::inference(23)}};
  caller.typeBounds = {
      {SemanticType::typeParameter(25), Lifetime::parameter(21)}};
  expect(verifyMirLifetimeBodyFacts(emptyProgram, graphBody, caller).valid(),
         "an exact caller type bound may flow into an existential region");
  graphBody.lifetimeBody.typeObligations[0].type.projectionConcept = 26;
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, graphBody, caller).valid(),
         "a bound on T does not establish a bound on an associated projection "
         "of T");
  MirBody initializing;
  initializing.placeDomain = {1, 2, 0};
  MirPlace owned;
  owned.id = 1;
  owned.root = MirPlaceRootKind::Binding;
  owned.binding = 1;
  owned.type = SemanticType::Int32;
  owned.sourceValue = 7;
  owned.key = PlaceKey{.domain = initializing.placeDomain, .root = 1};
  MirPlace alias = owned;
  alias.id = 2;
  alias.binding = 2;
  alias.type = SemanticType::referenceTo(SemanticType::Int32);
  alias.type.referenceLifetime = Lifetime::inference(8);
  alias.sourceValue = 9;
  alias.key->root = 2;
  initializing.places = {owned, alias};
  initializing.lifetimeBody = {
      .variables = {Lifetime::inference(8)},
      .obligations = {{Lifetime::local(11), Lifetime::inference(8)}},
      .origins = {{.region = Lifetime::local(11),
                   .place = 1,
                   .key = *owned.key,
                   .source = 7}},
      .complete = true};
  auto staticReference = alias.type;
  staticReference.referenceLifetime = Lifetime::staticLifetime();
  expect(mirLifetimeReferenceConversion(emptyProgram, initializing, 0,
                                         staticReference, alias.type),
         "reference conversion may shorten a proven static region");
  auto wrongReferent = alias.type;
  wrongReferent.arguments.front() = SemanticType::Int64;
  expect(!mirLifetimeReferenceConversion(emptyProgram, initializing, 0,
                                          staticReference, wrongReferent),
         "lifetime shortening never substitutes a different referent type");
  auto mutableReference = alias.type;
  mutableReference.referenceAccess = AccessMode::Mutable;
  expect(!mirLifetimeReferenceConversion(emptyProgram, initializing, 0,
                                          staticReference, mutableReference),
         "lifetime shortening cannot invent mutable permission");
  auto unknownReference = alias.type;
  unknownReference.referenceLifetime = Lifetime::parameter(999);
  expect(!mirLifetimeReferenceConversion(emptyProgram, initializing, 0,
                                          staticReference, unknownReference),
         "reference shortening cannot invent an undeclared region");
  MirInstruction initialize;
  initialize.id = 1;
  initialize.kind = MirInstructionKind::Initialize;
  initialize.hirValue = 7;
  initialize.destination = 2;
  initialize.operands = {
      {.kind = MirOperandKind::BorrowRead, .place = 1, .type = alias.type}};
  MirBlock initializationBlock;
  initializationBlock.id = 1;
  initializationBlock.instructions = {initialize};
  initializing.blocks = {initializationBlock};
  expect(verifyMirLifetimeApplications(emptyProgram, initializing).valid(),
         "reference initialization must replay its exact "
         "source-to-declared-region edge");
  auto missingInitializationEdge = initializing;
  missingInitializationEdge.lifetimeBody.obligations.clear();
  expect(!verifyMirLifetimeApplications(emptyProgram, missingInitializationEdge)
              .valid(),
         "dropping a reference initializer edge must invalidate MIR proof");
  auto escapedLocal = initializing;
  escapedLocal.returnType = SemanticType::referenceTo(SemanticType::Int32);
  escapedLocal.returnType.referenceLifetime = Lifetime::staticLifetime();
  escapedLocal.blocks[0].terminator.kind = MirTerminatorKind::Return;
  escapedLocal.blocks[0].terminator.hirValue = 7;
  escapedLocal.blocks[0].terminator.value =
      MirOperand{.kind = MirOperandKind::BorrowRead,
                 .place = 1,
                 .type = escapedLocal.returnType};
  expect(!verifyMirLifetimeApplications(emptyProgram, escapedLocal).valid(),
         "MIR return reconstruction cannot promote a local origin to static");
  std::cout << "lifetime IR structural metadata passed\n";
}
