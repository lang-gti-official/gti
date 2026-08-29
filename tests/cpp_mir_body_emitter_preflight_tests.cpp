#include "../src/compiler/cpp_mir_body_text_writer.h"
#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

void testDischargedCompoundOperatorVocabulary() {
  expect(
      lang::cppMirCompoundOperatorSpelling(lang::MirOperation::AddAssign) ==
              "+=" &&
          lang::cppMirCompoundOperatorSpelling(
              lang::MirOperation::SubtractAssign) == "-=" &&
          lang::cppMirCompoundOperatorSpelling(
              lang::MirOperation::MultiplyAssign) == "*=" &&
          lang::cppMirCompoundOperatorSpelling(lang::MirOperation::DivideAssign)
              .empty() &&
          lang::cppMirCompoundOperatorSpelling(
              lang::MirOperation::RemainderAssign)
              .empty() &&
          lang::cppMirCompoundOperatorSpelling(
              lang::MirOperation::ShiftLeftAssign)
              .empty() &&
          lang::cppMirCompoundOperatorSpelling(
              lang::MirOperation::ShiftRightAssign)
              .empty(),
      "native compound emission should remain limited to operations whose "
      "proved successful path exactly matches C++ semantics");
}

void testExhaustiveEnumClassification() {
  for (std::size_t value = 0;
       value <= static_cast<std::size_t>(lang::MirInstructionKind::Count);
       ++value) {
    const auto encoding = lang::classifyCppMirInstructionKind(
        static_cast<lang::MirInstructionKind>(value));
    expect((value == static_cast<std::size_t>(lang::MirInstructionKind::Count))
               ? encoding == lang::CppMirEmissionEncoding::Invalid
               : encoding != lang::CppMirEmissionEncoding::Invalid,
           "every named MIR instruction kind should have an exact class");
  }
  for (std::size_t value = 0;
       value <= static_cast<std::size_t>(lang::MirOperation::Count); ++value) {
    const auto encoding =
        lang::classifyCppMirOperation(static_cast<lang::MirOperation>(value));
    expect((value == static_cast<std::size_t>(lang::MirOperation::Count))
               ? encoding == lang::CppMirEmissionEncoding::Invalid
               : encoding != lang::CppMirEmissionEncoding::Invalid,
           "every named MIR operation should have an exact class");
  }

  constexpr std::array operandKinds{
      lang::MirOperandKind::Value,       lang::MirOperandKind::Constant,
      lang::MirOperandKind::Address,     lang::MirOperandKind::Copy,
      lang::MirOperandKind::Move,        lang::MirOperandKind::BorrowRead,
      lang::MirOperandKind::BorrowWrite, lang::MirOperandKind::Loan};
  for (const lang::MirOperandKind kind : operandKinds) {
    expect(lang::classifyCppMirOperandKind(kind) !=
               lang::CppMirEmissionEncoding::Invalid,
           "every named MIR operand kind should have an exact class");
  }
  constexpr std::array rootKinds{
      lang::MirPlaceRootKind::Binding, lang::MirPlaceRootKind::Symbol,
      lang::MirPlaceRootKind::This,    lang::MirPlaceRootKind::Temporary,
      lang::MirPlaceRootKind::Value,   lang::MirPlaceRootKind::Loan};
  for (const lang::MirPlaceRootKind kind : rootKinds) {
    expect(lang::classifyCppMirPlaceRootKind(kind) !=
               lang::CppMirEmissionEncoding::Invalid,
           "every named MIR place root should have an exact class");
  }
  constexpr std::array projections{
      lang::MirProjectionKind::Field, lang::MirProjectionKind::Index,
      lang::MirProjectionKind::Dereference, lang::MirProjectionKind::RawIndex,
      lang::MirProjectionKind::RawDereference};
  for (const lang::MirProjectionKind kind : projections) {
    expect(lang::classifyCppMirProjectionKind(kind) !=
               lang::CppMirEmissionEncoding::Invalid,
           "every named MIR projection should have an exact class");
  }
  constexpr std::array terminators{lang::MirTerminatorKind::None,
                                   lang::MirTerminatorKind::Goto,
                                   lang::MirTerminatorKind::Branch,
                                   lang::MirTerminatorKind::Switch,
                                   lang::MirTerminatorKind::Invoke,
                                   lang::MirTerminatorKind::Return,
                                   lang::MirTerminatorKind::PropagateFailure,
                                   lang::MirTerminatorKind::Unreachable,
                                   lang::MirTerminatorKind::Exit};
  for (const lang::MirTerminatorKind kind : terminators) {
    expect((kind == lang::MirTerminatorKind::None)
               ? lang::classifyCppMirTerminatorKind(kind) ==
                     lang::CppMirEmissionEncoding::Invalid
               : lang::classifyCppMirTerminatorKind(kind) !=
                     lang::CppMirEmissionEncoding::Invalid,
           "every executable MIR terminator should have an exact class");
  }
  constexpr std::array bodyKinds{lang::MirBodyKind::Module,
                                 lang::MirBodyKind::FieldInitializers,
                                 lang::MirBodyKind::StaticFieldInitializers,
                                 lang::MirBodyKind::Function,
                                 lang::MirBodyKind::Constructor,
                                 lang::MirBodyKind::Destructor,
                                 lang::MirBodyKind::Lambda,
                                 lang::MirBodyKind::HostedStartup};
  for (const lang::MirBodyKind kind : bodyKinds) {
    expect(lang::classifyCppMirBodyKind(kind) !=
               lang::CppMirEmissionEncoding::Invalid,
           "every named MIR body kind should have an exact class");
  }

  expect(
      lang::classifyCppMirInstructionKind(lang::MirInstructionKind::Modify) ==
              lang::CppMirEmissionEncoding::MissingMirAuthority &&
          lang::classifyCppMirOperation(lang::MirOperation::Index) ==
              lang::CppMirEmissionEncoding::NeedsCopiedRepresentation,
      "known schedule gaps and safe bounds should remain distinct");
  expect(
      lang::classifyCppMirOperandKind(static_cast<lang::MirOperandKind>(999)) ==
              lang::CppMirEmissionEncoding::Invalid &&
          lang::classifyCppMirPlaceRootKind(static_cast<lang::MirPlaceRootKind>(
              999)) == lang::CppMirEmissionEncoding::Invalid &&
          lang::classifyCppMirProjectionKind(
              static_cast<lang::MirProjectionKind>(999)) ==
              lang::CppMirEmissionEncoding::Invalid &&
          lang::classifyCppMirTerminatorKind(
              static_cast<lang::MirTerminatorKind>(999)) ==
              lang::CppMirEmissionEncoding::Invalid &&
          lang::classifyCppMirBodyKind(static_cast<lang::MirBodyKind>(999)) ==
              lang::CppMirEmissionEncoding::Invalid,
      "unknown future enum values should fail closed");
}

void testReadyBodyAndRepresentationFailures() {
  const lang::FrontendResult frontend = analyze("cpp-mir-body-ready.gti", R"(
int32_t identity(int32_t value) { return value; }
)");
  expect(frontend.canGenerateCode(),
         "the generic ready-body fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *function = firstSourceFunction(frontend.mir);
  expect(function != nullptr, "the ready fixture should lower one function");
  if (function == nullptr) {
    return;
  }
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function->id};

  lang::CppMirBodyEmissionMapRows rows = completeRows(frontend.mir);
  const lang::CppMirBodyEmissionAnalysis ready =
      lang::CppMirBodyEmitter(frontend.mir, lang::CppMirBodyEmissionMap(rows))
          .analyze(address);
  expect(ready.ready(),
         "a verified scalar identity body and complete copied map should be "
         "ready, including legitimately untyped lifecycle records");

  const lang::CppMirBodyEmissionAnalysis missing =
      lang::CppMirBodyEmitter(frontend.mir, lang::CppMirBodyEmissionMap{})
          .analyze(address);
  expect(missing.readiness ==
                 lang::CppMirBodyEmissionReadiness::MissingRepresentation &&
             hasIssue(
                 missing,
                 lang::CppMirBodyEmissionIssueKind::MissingTypeRepresentation),
         "an absent copied map should be classified as representation debt");

  rows.types.push_back(rows.types.front());
  const lang::CppMirBodyEmissionAnalysis duplicate =
      lang::CppMirBodyEmitter(frontend.mir,
                              lang::CppMirBodyEmissionMap(std::move(rows)))
          .analyze(address);
  expect(
      duplicate.readiness == lang::CppMirBodyEmissionReadiness::Incoherent &&
          hasIssue(
              duplicate,
              lang::CppMirBodyEmissionIssueKind::DuplicateTypeRepresentation),
      "duplicate copied type rows should fail closed as incoherent");
}

void testOwningPayloadEnumRepresentationAdmission() {
  const lang::FrontendResult frontend = analyze("owning-payload-map.gti", R"(
mut int32_t drops = 0;
class leaf { public: leaf() {} ~leaf() { drops = drops + 1; } mut int32_t value = 7; };
enum class holder { one(leaf value), none };
int main() {
  holder held = holder::one(leaf());
  switch (held) { case holder::one: break; case holder::none: break; }
  return drops;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning payload-enum map fixture should pass the frontend");
  const lang::MirFunctionInstance *function = firstSourceFunction(frontend.mir);
  if (!frontend.canGenerateCode() || function == nullptr) {
    return;
  }
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function->id};
  lang::CppMirBodyEmissionMapRows rows = buildRows(frontend, frontend.mir);
  const lang::CppMirBodyEmissionMap map(rows);
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::CppMirBodyEmissionAnalysis exact = emitter.analyze(address);
  if (!exact.ready()) {
    for (const lang::CppMirBodyEmissionIssue &issue : exact.issues) {
      std::cerr << "owning payload admission: " << issue.detail << '\n';
    }
  }
  expect(exact.ready(),
         "an exact owning payload-enum lifecycle row should admit verified "
         "MIR body analysis");

  const auto enumeration = std::find_if(
      rows.enums.begin(), rows.enums.end(),
      [](const lang::CppMirEnumRepresentation &row) {
        return row.requiresActiveCleanup && !row.payloadVariants.empty();
      });
  expect(enumeration != rows.enums.end(),
         "the sealed representation map should carry the owning enum row");
  if (enumeration == rows.enums.end() ||
      enumeration->payloadVariants.front().fieldRequiresActiveCleanup.empty()) {
    return;
  }
  enumeration->payloadVariants.front().fieldRequiresActiveCleanup.front() =
      false;
  const lang::CppMirBodyEmissionAnalysis forged =
      lang::CppMirBodyEmitter(frontend.mir,
                              lang::CppMirBodyEmissionMap(std::move(rows)))
          .analyze(address);
  expect(
      forged.readiness == lang::CppMirBodyEmissionReadiness::Incoherent &&
          hasIssue(forged,
                   lang::CppMirBodyEmissionIssueKind::InvalidRepresentationRow),
      "a forged payload cleanup row should fail closed against verified "
      "MIR");
}

void testImmutableBodyPlanOutlivesAdmissionInputs() {
  const lang::CppMirBodyEmissionPlan plan = [] {
    const lang::FrontendResult frontend =
        analyze("cpp-mir-body-immutable-plan.gti",
                "int32_t identity(int32_t value) { return value; }");
    expect(frontend.canGenerateCode(),
           "the immutable-plan fixture should pass the frontend");
    const lang::MirFunctionInstance *function =
        firstSourceFunction(frontend.mir);
    expect(function != nullptr,
           "the immutable-plan fixture should lower one source function");
    if (function == nullptr) {
      return lang::CppMirBodyEmissionPlan{};
    }
    const lang::CppMirBodyEmissionMap map(completeRows(frontend.mir));
    return lang::CppMirBodyEmitter(frontend.mir, map)
        .buildPlan(
            {.kind = lang::MirBodyKind::Function, .owner = function->id});
  }();

  expect(plan.ready() && plan.supportsPlainText(),
         "a complete scalar body should produce an admitted immutable plan");
  const lang::CppMirBodyEmissionText first = lang::CppMirBodyTextWriter::write(
      plan, lang::CppMirBodyTextForm::Plain, "immutable-plan-v1", 1);
  const lang::CppMirBodyEmissionText second = lang::CppMirBodyTextWriter::write(
      plan, lang::CppMirBodyTextForm::Plain, "immutable-plan-v1", 1);
  expect(first.emitted() && first.text == second.text &&
             first.text.find("immutable-plan-v1") != std::string::npos &&
             first.text.find(lang::cppMirBodyEmissionFamilyPlaceholder) ==
                 std::string::npos,
         "the formatting-only writer should remain deterministic after the "
         "MIR program and representation map have been destroyed");
}

void testHostedStartupOwnsRemainingAuthorityGap() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-entry.gti", "int main() { return 0; }");
  expect(frontend.canGenerateCode(),
         "the hosted-entry fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto entry =
      std::find_if(frontend.mir.functionInstances().begin(),
                   frontend.mir.functionInstances().end(),
                   [](const lang::MirFunctionInstance &function) {
                     return function.entryKind != lang::ProgramEntryKind::None;
                   });
  expect(entry != frontend.mir.functionInstances().end(),
         "the entry fixture should retain hosted-entry metadata");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(completeRows(frontend.mir));
  const lang::CppMirBodyEmissionAnalysis entryAnalysis =
      lang::CppMirBodyEmitter(frontend.mir, map)
          .analyze({.kind = lang::MirBodyKind::Function, .owner = entry->id});
  const lang::CppMirBodyEmissionAnalysis startupAnalysis =
      lang::CppMirBodyEmitter(frontend.mir, map)
          .analyze(
              {.kind = lang::MirBodyKind::HostedStartup, .owner = entry->id});
  expect(
      entryAnalysis.ready() &&
          !hasIssue(entryAnalysis,
                    lang::CppMirBodyEmissionIssueKind::MissingHostedStartupMir),
      "the source entry body should no longer claim ownership of generated "
      "hosted startup");
  expect(
      startupAnalysis.ready() &&
          !hasIssue(startupAnalysis,
                    lang::CppMirBodyEmissionIssueKind::MissingHostedStartupMir),
      "the generated failure-free startup body carries its complete "
      "verified two-operation schedule authority");

  const lang::FrontendResult owned =
      analyzeWithStandardLibrary("cpp-mir-body-owned-entry.gti", R"(
#include <std/string>
#include <std/vector>
int main(int argc, std::vector<std::string> argv) { return argc; }
)");
  expect(owned.canGenerateCode(),
         "the owned hosted-entry fixture should pass the frontend");
  if (!owned.canGenerateCode()) {
    return;
  }
  const auto ownedEntry = std::find_if(
      owned.mir.functionInstances().begin(),
      owned.mir.functionInstances().end(),
      [](const lang::MirFunctionInstance &function) {
        return function.entryKind == lang::ProgramEntryKind::OwnedArguments;
      });
  expect(ownedEntry != owned.mir.functionInstances().end(),
         "the owned entry fixture should retain its generated startup plan");
  if (ownedEntry == owned.mir.functionInstances().end()) {
    return;
  }
  const lang::CppMirBodyEmissionAnalysis ownedStartup =
      lang::CppMirBodyEmitter(
          owned.mir, lang::CppMirBodyEmissionMap(completeRows(owned.mir)))
          .analyze({.kind = lang::MirBodyKind::HostedStartup,
                    .owner = ownedEntry->id});
  expect(ownedStartup.ready() &&
             !hasIssue(
                 ownedStartup,
                 lang::CppMirBodyEmissionIssueKind::MissingFailureCleanupMir) &&
             !hasIssue(ownedStartup, lang::CppMirBodyEmissionIssueKind::
                                         MissingPartialConstructionRollbackMir),
         "the exact generated owned startup schedule is accepted: the verified "
         "marshaling plan carries its own failure-cleanup envelope and the "
         "emitted argc/argv main is its complete emission");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(owned.hir, lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(owned, owned.mir, owned.mir, optimizations);
  const auto marker = [&](std::string_view kind, std::size_t owner) {
    return artifact.contents.find(std::string(kind) + " " +
                                  std::to_string(owner) + "\n") !=
           std::string::npos;
  };
  const std::optional<lang::MirHostedStartupPlan> &ownedPlan =
      owned.mir.hostedStartupPlan();
  expect(ownedPlan && marker("function-instance", ownedPlan->entry) &&
             marker("function-instance", ownedPlan->appendFunction),
         "owned startup should publish its entry and class-valued append "
         "targets through verified MIR bodies");
  if (ownedPlan) {
    expect(artifact.contents.find("mir_failure_constructor_tag_v1<" +
                                  std::to_string(ownedPlan->vectorConstructor) +
                                  ">{") != std::string::npos &&
               artifact.contents.find(
                   "mir_failure_constructor_tag_v1<" +
                   std::to_string(ownedPlan->stringConstructor) + ">{") !=
                   std::string::npos &&
               artifact.contents.find(
                   "push_back__gti_mir_failure(std::move(__gti_argument)") !=
                   std::string::npos &&
               artifact.contents.find(
                   "::__gti_program::__gti_entry__gti_mir_failure(") !=
                   std::string::npos,
           "the native adapter should invoke the exact transformed vector, "
           "string, append, and entry surfaces selected by hosted MIR");
  }
}

void testVerifiedProgramInitializationPlanClassification() {
  const lang::FrontendResult dataOnly =
      analyze("cpp-mir-body-data-only-module.gti", R"(
constexpr int32_t seed = 4;
mut int32_t zeroed;
)");
  expect(dataOnly.canGenerateCode(),
         "the data-only Module classifier fixture should pass the frontend");
  if (!dataOnly.canGenerateCode()) {
    return;
  }
  lang::CppMirBodyEmissionMapRows dataRows = completeRows(dataOnly.mir);
  dataRows.bodies.erase(
      std::remove_if(dataRows.bodies.begin(), dataRows.bodies.end(),
                     [](const lang::CppMirBodyNameRepresentation &row) {
                       return row.address ==
                              lang::MirBodyAddress{
                                  .kind = lang::MirBodyKind::Module};
                     }),
      dataRows.bodies.end());
  dataRows.capabilities.erase(
      std::remove_if(
          dataRows.capabilities.begin(), dataRows.capabilities.end(),
          [](const lang::CppMirEmissionCapabilityRepresentation &row) {
            return row.kind ==
                   lang::CppMirEmissionCapabilityKind::ProgramInitialization;
          }),
      dataRows.capabilities.end());
  const lang::CppMirBodyEmissionAnalysis dataAnalysis =
      lang::CppMirBodyEmitter(dataOnly.mir,
                              lang::CppMirBodyEmissionMap(std::move(dataRows)))
          .analyze({.kind = lang::MirBodyKind::Module});
  expect(
      dataAnalysis.ready() && !dataOnly.mir.module().places.empty() &&
          !hasIssue(
              dataAnalysis,
              lang::CppMirBodyEmissionIssueKind::MissingBodyRepresentation) &&
          !hasIssue(dataAnalysis, lang::CppMirBodyEmissionIssueKind::
                                      MissingCapabilityRepresentation) &&
          !hasIssue(dataAnalysis, lang::CppMirBodyEmissionIssueKind::
                                      MissingProgramInitializationMir),
      "data-only tagged storage stages should require neither an "
      "executable Module name nor a runtime initialization adapter");

  const lang::FrontendResult dynamic =
      analyze("cpp-mir-body-dynamic-module.gti", R"(
int32_t initial_value() { return 5; }
class Registry {
public:
  static mut int32_t value = initial_value();
};
)");
  expect(dynamic.canGenerateCode(),
         "the dynamic Module classifier fixture should pass the frontend");
  if (!dynamic.canGenerateCode()) {
    return;
  }
  const auto staticStep = std::find_if(
      dynamic.mir.programInitializationPlan().steps.begin(),
      dynamic.mir.programInitializationPlan().steps.end(),
      [](const lang::MirProgramInitializationStep &step) {
        return step.storageKind == lang::ProgramStorageKind::StaticField &&
               step.role == lang::ProgramInitializationStepRole::Initializer;
      });
  expect(staticStep != dynamic.mir.programInitializationPlan().steps.end(),
         "the dynamic classifier fixture should retain one planned static "
         "initializer");

  lang::CppMirBodyEmissionMapRows dynamicRows = completeRows(dynamic.mir);
  const bool exactStaticStorage =
      staticStep != dynamic.mir.programInitializationPlan().steps.end() &&
      std::any_of(dynamicRows.symbols.begin(), dynamicRows.symbols.end(),
                  [&](const lang::CppMirSymbolRepresentation &row) {
                    return row.kind ==
                               lang::CppMirSymbolRepresentationKind::Storage &&
                           row.owner == staticStep->ownerClass &&
                           row.symbol == staticStep->symbol;
                  });
  const lang::CppMirBodyEmissionAnalysis dynamicAnalysis =
      lang::CppMirBodyEmitter(dynamic.mir,
                              lang::CppMirBodyEmissionMap(dynamicRows))
          .analyze({.kind = lang::MirBodyKind::Module});
  expect(
      exactStaticStorage &&
          !hasIssue(dynamicAnalysis, lang::CppMirBodyEmissionIssueKind::
                                         MissingProgramInitializationMir) &&
          !hasIssue(
              dynamicAnalysis,
              lang::CppMirBodyEmissionIssueKind::MissingSymbolRepresentation),
      "the verified Module plan should supply exact program-step and "
      "static-storage owner authority to the private classifier");

  dynamicRows.capabilities.erase(
      std::remove_if(
          dynamicRows.capabilities.begin(), dynamicRows.capabilities.end(),
          [](const lang::CppMirEmissionCapabilityRepresentation &row) {
            return row.kind ==
                   lang::CppMirEmissionCapabilityKind::ProgramInitialization;
          }),
      dynamicRows.capabilities.end());
  const lang::CppMirBodyEmissionAnalysis missingAdapter =
      lang::CppMirBodyEmitter(
          dynamic.mir, lang::CppMirBodyEmissionMap(std::move(dynamicRows)))
          .analyze({.kind = lang::MirBodyKind::Module});
  expect(missingAdapter.readiness ==
                 lang::CppMirBodyEmissionReadiness::MissingRepresentation &&
             hasIssue(missingAdapter, lang::CppMirBodyEmissionIssueKind::
                                          MissingCapabilityRepresentation) &&
             !hasIssue(missingAdapter, lang::CppMirBodyEmissionIssueKind::
                                           MissingProgramInitializationMir),
         "a verified dynamic plan should expose only the missing copied "
         "runtime adapter, not a fictitious MIR authority gap");

  lang::MirProgram stalePlan = dataOnly.mir;
  auto &steps = const_cast<lang::MirProgramInitializationPlan &>(
                    stalePlan.programInitializationPlan())
                    .steps;
  if (!steps.empty()) {
    steps.front().storageInitialization = 0;
  }
  const lang::CppMirBodyEmissionAnalysis incoherent =
      lang::CppMirBodyEmitter(
          stalePlan, lang::CppMirBodyEmissionMap(completeRows(stalePlan)))
          .analyze({.kind = lang::MirBodyKind::Module});
  expect(incoherent.readiness ==
                 lang::CppMirBodyEmissionReadiness::Incoherent &&
             hasIssue(incoherent,
                      lang::CppMirBodyEmissionIssueKind::InvalidMirProgram),
         "a stale program plan must fail before classifier readiness");
}

void testSafeIndexUsesBoundsNotRawMemory() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-safe-index.gti", R"(
int32_t at(int32_t values[2], uint64_t index) { return values[index]; }
)");
  expect(frontend.canGenerateCode(),
         "the safe fixed-array index fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *function = firstSourceFunction(frontend.mir);
  expect(function != nullptr, "the index fixture should lower one function");
  if (function == nullptr) {
    return;
  }
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function->id};
  const lang::CppMirBodyEmissionAnalysis noBounds =
      lang::CppMirBodyEmitter(
          frontend.mir,
          lang::CppMirBodyEmissionMap(completeRows(
              frontend.mir, lang::CppMirEmissionCapabilityKind::Bounds)))
          .analyze(address);
  const lang::CppMirBodyEmissionAnalysis noRawMemory =
      lang::CppMirBodyEmitter(
          frontend.mir,
          lang::CppMirBodyEmissionMap(completeRows(
              frontend.mir, lang::CppMirEmissionCapabilityKind::RawMemory)))
          .analyze(address);
  expect(
      hasIssue(
          noBounds,
          lang::CppMirBodyEmissionIssueKind::MissingCapabilityRepresentation) &&
          !hasIssue(noRawMemory, lang::CppMirBodyEmissionIssueKind::
                                     MissingCapabilityRepresentation),
      "safe Index should require a bounds/failure adapter, not raw-memory "
      "authority");
}

void testUnsafeRawPointerVocabulary() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-raw-pointer.gti", R"(
int64_t raw_distance() {
  mut int32_t value = 1;
  mut int32_t* pointer = nullptr;
  mut int64_t distance = 0;
  unsafe {
    pointer = &value;
    pointer[0] = 4;
    *(pointer + 0) = 5;
    distance = (pointer + 0) - pointer;
  }
  return distance;
}
)");
  expect(frontend.canGenerateCode(),
         "the raw-pointer text fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto functionIt = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &candidate) {
        return std::any_of(
            candidate.body.blocks.begin(), candidate.body.blocks.end(),
            [](const lang::MirBlock &block) {
              return std::any_of(block.instructions.begin(),
                                 block.instructions.end(),
                                 [](const lang::MirInstruction &instruction) {
                                   return instruction.operation ==
                                          lang::MirOperation::AddressOf;
                                 });
            });
      });
  const lang::MirFunctionInstance *function =
      functionIt == frontend.mir.functionInstances().end() ? nullptr
                                                           : &*functionIt;
  expect(function != nullptr,
         "the raw-pointer fixture should lower one source function");
  if (function == nullptr) {
    return;
  }
  expect(!function->mayRaiseDefinedFailure,
         "verified raw-pointer operations should not invent a GTI "
         "defined-failure effect");
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function->id};
  const lang::CppMirBodyEmissionAnalysis noRawMemory =
      lang::CppMirBodyEmitter(
          frontend.mir,
          lang::CppMirBodyEmissionMap(completeRows(
              frontend.mir, lang::CppMirEmissionCapabilityKind::RawMemory)))
          .analyze(address);
  expect(
      hasIssue(
          noRawMemory,
          lang::CppMirBodyEmissionIssueKind::MissingCapabilityRepresentation),
      "unsafe raw-pointer MIR should require its sealed representation "
      "capability");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyze(address).ready() && emitter.supportsBodyText(address),
         "the production snapshot should admit the exact raw-pointer MIR "
         "vocabulary");
  if (!emitter.supportsBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitBodyText(address, "raw-pointer-test-v0", 1);
  expect(text.text.find(" = (&__gti_mir_p_") != std::string::npos &&
             text.text.find("[__gti_mir_v_") != std::string::npos &&
             text.text.find("(*__gti_mir_v_") != std::string::npos &&
             text.text.find(" + ") != std::string::npos &&
             text.text.find(" - ") != std::string::npos,
         "raw-pointer MIR text should spell address formation, unchecked "
         "index/dereference lvalues, and pointer arithmetic directly");
}

void testConcreteGenericFieldOwner() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-generic-field.gti", R"(
class Box<T> {
  T value;
public:
  Box(T initial) : value(initial) {}
  T read() { return this.value; }
};

int32_t use_int() {
  Box<int32_t> value = Box<int32_t>(1);
  return value.read();
}

uint8_t use_byte() {
  Box<uint8_t> value = Box<uint8_t>(uint8_t(2));
  return value.read();
}
)");
  expect(frontend.canGenerateCode(),
         "the two-instantiation field fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  std::vector<const lang::MirClassInstance *> boxes;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    if (instance.declaredFields.size() == 1) {
      boxes.push_back(&instance);
    }
  }
  expect(boxes.size() == 2 &&
             boxes.front()->declaration == boxes.back()->declaration &&
             boxes.front()->declaredFields.front().symbol ==
                 boxes.back()->declaredFields.front().symbol,
         "the fixture should retain two concrete owners for one source field");
  if (boxes.size() != 2) {
    return;
  }

  const auto methodFor = [&](lang::HirClassInstanceId owner) {
    return std::find_if(
        frontend.mir.functionInstances().begin(),
        frontend.mir.functionInstances().end(),
        [&](const lang::MirFunctionInstance &function) {
          return function.owner == owner &&
                 std::any_of(
                     function.body.places.begin(), function.body.places.end(),
                     [](const lang::MirPlace &place) {
                       return std::any_of(
                           place.projections.begin(), place.projections.end(),
                           [](const lang::MirPlaceProjection &projection) {
                             return projection.kind ==
                                    lang::MirProjectionKind::Field;
                           });
                     });
        });
  };
  const auto firstMethod = methodFor(boxes.front()->id);
  const auto secondMethod = methodFor(boxes.back()->id);
  expect(firstMethod != frontend.mir.functionInstances().end() &&
             secondMethod != frontend.mir.functionInstances().end(),
         "both concrete class instances should have field-reading MIR bodies");
  if (firstMethod == frontend.mir.functionInstances().end() ||
      secondMethod == frontend.mir.functionInstances().end()) {
    return;
  }

  lang::CppMirBodyEmissionMapRows complete = completeRows(frontend.mir);
  const lang::CppMirBodyEmissionMap completeMap(complete);
  const lang::CppMirBodyEmissionAnalysis first =
      lang::CppMirBodyEmitter(frontend.mir, completeMap)
          .analyze(
              {.kind = lang::MirBodyKind::Function, .owner = firstMethod->id});
  const lang::CppMirBodyEmissionAnalysis second =
      lang::CppMirBodyEmitter(frontend.mir, completeMap)
          .analyze(
              {.kind = lang::MirBodyKind::Function, .owner = secondMethod->id});
  expect(
      !hasIssue(
          first,
          lang::CppMirBodyEmissionIssueKind::MissingSymbolRepresentation) &&
          !hasIssue(
              second,
              lang::CppMirBodyEmissionIssueKind::MissingSymbolRepresentation),
      "field lookup should use each projected place's concrete owner");

  const lang::HirClassInstanceId removedOwner = boxes.front()->id;
  const auto erased = std::remove_if(
      complete.symbols.begin(), complete.symbols.end(),
      [&](const lang::CppMirSymbolRepresentation &row) {
        return row.kind == lang::CppMirSymbolRepresentationKind::Field &&
               row.owner == removedOwner;
      });
  complete.symbols.erase(erased, complete.symbols.end());
  const lang::CppMirBodyEmissionMap missingOwnerMap(std::move(complete));
  const lang::CppMirBodyEmissionAnalysis missingOwner =
      lang::CppMirBodyEmitter(frontend.mir, missingOwnerMap)
          .analyze(
              {.kind = lang::MirBodyKind::Function, .owner = firstMethod->id});
  const lang::CppMirBodyEmissionAnalysis retainedOwner =
      lang::CppMirBodyEmitter(frontend.mir, missingOwnerMap)
          .analyze(
              {.kind = lang::MirBodyKind::Function, .owner = secondMethod->id});
  expect(
      hasIssue(
          missingOwner,
          lang::CppMirBodyEmissionIssueKind::MissingSymbolRepresentation) &&
          !hasIssue(
              retainedOwner,
              lang::CppMirBodyEmissionIssueKind::MissingSymbolRepresentation),
      "removing one generic field owner must not alias the same source "
      "SymbolId from another instantiation");
}

void testAmbiguousStaticStorageFailsClosed() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-storage-owner.gti", R"(
mut int32_t state = 1;
int32_t read_state() { return state; }
)");
  expect(frontend.canGenerateCode(),
         "the global-storage owner fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *function = firstSourceFunction(frontend.mir);
  expect(function != nullptr, "the storage fixture should lower one function");
  if (function == nullptr) {
    return;
  }
  const auto place =
      std::find_if(function->body.places.begin(), function->body.places.end(),
                   [](const lang::MirPlace &candidate) {
                     return candidate.root == lang::MirPlaceRootKind::Symbol &&
                            candidate.capture == 0;
                   });
  expect(place != function->body.places.end(),
         "the storage fixture should retain a symbol-root place");
  if (place == function->body.places.end()) {
    return;
  }

  lang::CppMirBodyEmissionMapRows rows = completeRows(frontend.mir);
  rows.symbols.erase(
      std::remove_if(
          rows.symbols.begin(), rows.symbols.end(),
          [&](const lang::CppMirSymbolRepresentation &row) {
            return row.kind == lang::CppMirSymbolRepresentationKind::Storage &&
                   row.symbol == place->symbol;
          }),
      rows.symbols.end());
  rows.symbols.push_back({.kind = lang::CppMirSymbolRepresentationKind::Storage,
                          .owner = 101,
                          .symbol = place->symbol,
                          .type = place->type,
                          .spelling = "first_static_instance"});
  rows.symbols.push_back({.kind = lang::CppMirSymbolRepresentationKind::Storage,
                          .owner = 202,
                          .symbol = place->symbol,
                          .type = place->type,
                          .spelling = "second_static_instance"});
  const lang::CppMirBodyEmissionAnalysis analysis =
      lang::CppMirBodyEmitter(frontend.mir,
                              lang::CppMirBodyEmissionMap(std::move(rows)))
          .analyze(
              {.kind = lang::MirBodyKind::Function, .owner = function->id});
  expect(
      hasIssue(
          analysis,
          lang::CppMirBodyEmissionIssueKind::MissingProgramInitializationMir),
      "same-Symbol static rows from multiple owners must stay ambiguous "
      "without concrete initialization/storage authority");
}

void testGlobalClassStorageMemberProjection() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-global-class-storage.gti", R"(
class thing {
public:
  thing() {}
  mut int32_t value = 0;
};

mut thing g = thing();

int32_t read_global() { return g.value; }
void write_global(int32_t value) { g.value = value; }

int main() {
  write_global(int32_t(37));
  return read_global() == 37 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the global class-storage fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::CppMirBodyEmissionMapRows rows = buildRows(frontend, frontend.mir);
  const auto slot = std::find_if(
      rows.symbols.begin(), rows.symbols.end(),
      [](const lang::CppMirSymbolRepresentation &row) {
        return row.kind == lang::CppMirSymbolRepresentationKind::Storage &&
               row.type.kind == lang::SemanticType::Class &&
               row.runtimeLifetimeSlot;
      });
  expect(slot != rows.symbols.end(),
         "runtime-initialized class program storage should carry one exact "
         "lifetime-slot representation row");
  if (slot == rows.symbols.end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(rows);
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress module{.kind = lang::MirBodyKind::Module,
                                    .owner = 0};
  expect(emitter.analyze(module).ready() &&
             emitter.supportsFailureBodyText(module),
         "Module/0 should admit the class global's runtime construction "
         "through its copied lifetime-slot row");

  std::size_t projectedBodies = 0;
  bool projectedBodiesSupported = true;
  bool projectedBodiesUseSlotValue = true;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const bool projected = std::any_of(
        function.body.places.begin(), function.body.places.end(),
        [](const lang::MirPlace &place) {
          return place.root == lang::MirPlaceRootKind::Symbol &&
                 !place.projections.empty() &&
                 std::all_of(place.projections.begin(), place.projections.end(),
                             [](const lang::MirPlaceProjection &projection) {
                               return projection.kind ==
                                      lang::MirProjectionKind::Field;
                             });
        });
    if (!projected) {
      continue;
    }
    ++projectedBodies;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const bool supported = function.mayRaiseDefinedFailure
                               ? emitter.supportsFailureBodyText(address)
                               : emitter.supportsBodyText(address);
    projectedBodiesSupported = projectedBodiesSupported && supported;
    if (!supported) {
      continue;
    }
    const lang::CppMirBodyEmissionText text =
        function.mayRaiseDefinedFailure
            ? emitter.emitFailureBodyText(
                  address, "global-class-storage-regression-v1", 1)
            : emitter.emitBodyText(address,
                                   "global-class-storage-regression-v1", 1);
    projectedBodiesUseSlotValue =
        projectedBodiesUseSlotValue && text.emitted() &&
        text.text.find(".get().value") != std::string::npos;
  }
  expect(projectedBodies == 2 && projectedBodiesSupported &&
             projectedBodiesUseSlotValue,
         "global class field reads and writes should both emit through the "
         "engaged lifetime-slot value");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find(
             "mir_lifetime_slot<::__gti_program::thing, true> g") !=
                 std::string::npos &&
             artifact.contents.find("auto &__gti_mir_p_1 = "
                                    "::__gti_program::g;") !=
                 std::string::npos &&
             artifact.contents.find("::__gti_program::g.get().value") !=
                 std::string::npos,
         "the complete backend should declare, construct, and project the "
         "same class-global lifetime-slot representation");

  slot->runtimeLifetimeSlot = false;
  const lang::CppMirBodyEmissionMap missingSlotMap(std::move(rows));
  expect(!lang::CppMirBodyEmitter(frontend.mir, missingSlotMap)
              .supportsFailureBodyText(module),
         "removing the copied lifetime-slot fact must fail Module/0 text "
         "admission instead of emitting an undeclared slot carrier");
}

void testConcreteClassStaticStorage() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-class-static-storage.gti", R"(
class Counter {
public:
  static mut int32_t value = 3;
  static int32_t read() { return value; }
};

int32_t read_counter() { return Counter::value; }
)");
  expect(frontend.canGenerateCode(),
         "the concrete class-static storage fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t supported = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const bool readsStatic =
        std::any_of(function.body.places.begin(), function.body.places.end(),
                    [](const lang::MirPlace &place) {
                      return place.root == lang::MirPlaceRootKind::Symbol &&
                             place.capture == 0;
                    });
    if (!readsStatic) {
      continue;
    }
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const bool spellable =
        emitter.analyze(address).ready() && emitter.supportsBodyText(address);
    expect(spellable,
           "a unique class-static storage row should be spellable from its "
           "member and an unrelated concrete function");
    if (!spellable) {
      continue;
    }
    const lang::CppMirBodyEmissionText text =
        emitter.emitBodyText(address, "class-static-storage-test-v0", 1);
    expect(text.emitted() &&
               text.text.find("Counter::value") != std::string::npos,
           "class-static storage text should use its owner-qualified row");
    ++supported;
  }
  expect(supported == 2,
         "the class-static fixture should admit both concrete reader bodies");
}

void testDischargedStorageReadAnalysis() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-discharged-read.gti", R"(
#include <std/vector>

int main() {
  mut std::vector<int> values = std::vector<int>();
  values.push_back(7);
  return values.front();
}
)");
  expect(frontend.canGenerateCode(),
         "the discharged-read fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationPipeline pipeline;
  const lang::OptimizationResult compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O0);
  const lang::OptimizedProgram optimized =
      pipeline.run({.hir = frontend.hir,
                    .mir = frontend.mir,
                    .level = lang::OptimizationLevel::O3,
                    .compatibility = &compatibility});
  expect(optimized.valid(),
         "the discharged-read fixture should produce coherent O3 MIR");
  if (!optimized.valid()) {
    return;
  }
  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, optimized.sourceMir, optimized.mir, lang::TargetInfo::host(),
      optimized.report.transformations);
  const lang::CppMirBodyEmissionMap map(
      lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(optimized.mir, map);
  std::size_t dischargedInvokes = 0;
  bool exactUncheckedEmission = true;
  for (const lang::MirFunctionInstance &instance :
       optimized.mir.functionInstances()) {
    for (const lang::MirBlock &block : instance.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call ||
            (instruction.intrinsic != lang::IntrinsicKind::PrefixStorageRead &&
             instruction.intrinsic !=
                 lang::IntrinsicKind::PrefixStorageReadMut) ||
            !lang::allMirLocalFailuresDischarged(instruction) ||
            block.terminator.kind != lang::MirTerminatorKind::Invoke ||
            block.terminator.invokeInstruction != instruction.id) {
          continue;
        }
        ++dischargedInvokes;
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = instance.id};
        const lang::CppMirBodyEmissionText text =
            instance.mayRaiseDefinedFailure
                ? emitter.emitFailureBodyText(
                      address, "discharged-storage-read-regression-v1", 1)
                : emitter.emitBodyText(
                      address, "discharged-storage-read-regression-v1", 1);
        const std::string status =
            "__gti_mir_failure_status_" + std::to_string(instruction.id);
        const std::string checkedHelper =
            instruction.intrinsic == lang::IntrinsicKind::PrefixStorageRead
                ? "::gti_internal::backend::mir_prefix_read_v1"
                : "::gti_internal::backend::mir_prefix_read_mut_v1";
        const std::string uncheckedHelper =
            instruction.intrinsic == lang::IntrinsicKind::PrefixStorageRead
                ? "::gti_internal::backend::prefix_storage_read_unchecked"
                : "::gti_internal::backend::prefix_storage_read_mut_unchecked";
        const bool exact =
            text.emitted() &&
            text.text.find(uncheckedHelper) != std::string::npos &&
            text.text.find(checkedHelper) == std::string::npos &&
            text.text.find(status) == std::string::npos;
        if (!exact) {
          std::cerr << "discharged checked prefix read " << instruction.id
                    << " did not emit solely through its unchecked helper\n";
        }
        exactUncheckedEmission = exactUncheckedEmission && exact;
      }
    }
  }
  expect(dischargedInvokes > 0,
         "the vector fixture should retain at least one checked prefix read "
         "after its failure outcomes are safety-discharged");
  expect(exactUncheckedEmission,
         "every safety-discharged prefix read should use the unchecked helper "
         "without a dead failure-status carrier");
}

// The inline closure chain: lambda-typed places and values never declare
// (C++ closure types are unnameable), so the Closure compute fuses into
// its consuming invocations, which spell the full literal with capture
// names from the Capture rows and the recursively emitted verified body.
// Under the transformed parent convention each private generated literal
// also carries an out-result and failure-record channel, so its checked
// arithmetic preserves the enclosing frame's cleanup edge.

void testExpectedExtractionStateAdmissionFailsClosed() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-extraction-state.gti", R"(
expected<int32_t, int32_t> first_state = int32_t(7);
expected<int32_t, int32_t> second_state = int32_t(8);

expected<int32_t, int32_t>
forward(expected<int32_t, int32_t> result) {
  return ?result;
}

expected<int32_t, int32_t> forward_globals() {
  bool ignored = second_state.has_value();
  return ?first_state;
}

int32_t main() {
  expected<int32_t, int32_t> result = int32_t(7);
  expected<int32_t, int32_t> forwarded = forward(result);
  expected<int32_t, int32_t> global_forwarded = forward_globals();
  return forwarded.has_value() && global_forwarded.has_value()
             ? forwarded.value() + global_forwarded.value() - int32_t(14)
             : int32_t(1);
}
)");
  expect(frontend.canGenerateCode(),
         "the Expected extraction-state fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto forwardHir =
      std::find_if(frontend.hir.functionInstances().begin(),
                   frontend.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &candidate) {
                     return candidate.source != nullptr &&
                            candidate.source->name().lexeme == "forward";
                   });
  const lang::MirFunctionInstance *forward =
      forwardHir == frontend.hir.functionInstances().end()
          ? nullptr
          : frontend.mir.findFunctionInstance(forwardHir->id);
  const auto globalForwardHir = std::find_if(
      frontend.hir.functionInstances().begin(),
      frontend.hir.functionInstances().end(),
      [](const lang::HirFunctionInstance &candidate) {
        return candidate.source != nullptr &&
               candidate.source->name().lexeme == "forward_globals";
      });
  const lang::MirFunctionInstance *globalForward =
      globalForwardHir == frontend.hir.functionInstances().end()
          ? nullptr
          : frontend.mir.findFunctionInstance(globalForwardHir->id);
  const lang::MirBlock *stateBranch = nullptr;
  const auto findInstruction = [](const lang::MirBody &body,
                                  lang::MirInstructionId id) {
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == id) {
          return &instruction;
        }
      }
    }
    return static_cast<const lang::MirInstruction *>(nullptr);
  };
  const auto findMutableInstruction = [](lang::MirBody &body,
                                         lang::MirInstructionId id) {
    for (lang::MirBlock &block : body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == id) {
          return &instruction;
        }
      }
    }
    return static_cast<lang::MirInstruction *>(nullptr);
  };
  if (forward != nullptr) {
    for (const lang::MirBlock &block : forward->body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Branch ||
          !block.terminator.value ||
          block.terminator.value->kind != lang::MirOperandKind::Value) {
        continue;
      }
      const lang::MirValue *condition =
          forward->body.findValue(block.terminator.value->value);
      const lang::MirInstruction *definition =
          condition == nullptr
              ? nullptr
              : findInstruction(forward->body, condition->definition);
      if (definition != nullptr &&
          definition->kind == lang::MirInstructionKind::Compute &&
          definition->operation == lang::MirOperation::ExpectedHasValue) {
        stateBranch = &block;
        break;
      }
    }
  }
  bool zeroSiteExtractions = false;
  if (forward != nullptr && stateBranch != nullptr) {
    for (const lang::MirBlockId target :
         {stateBranch->terminator.target, stateBranch->terminator.elseTarget}) {
      const lang::MirBlock *extraction = forward->body.findBlock(target);
      if (extraction == nullptr) {
        continue;
      }
      zeroSiteExtractions =
          zeroSiteExtractions ||
          std::any_of(
              extraction->instructions.begin(), extraction->instructions.end(),
              [](const lang::MirInstruction &instruction) {
                return instruction.kind == lang::MirInstructionKind::Call &&
                       (instruction.intrinsic ==
                            lang::IntrinsicKind::ExpectedValue ||
                        instruction.intrinsic ==
                            lang::IntrinsicKind::ExpectedError) &&
                       instruction.localFailureSites.empty();
              });
    }
  }
  expect(forward != nullptr && stateBranch != nullptr && zeroSiteExtractions,
         "propagation should lower zero-site Expected extractions behind one "
         "matching state branch");
  if (forward == nullptr || stateBranch == nullptr || !zeroSiteExtractions) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = forward->id};
  const lang::CppMirBodyEmitter canonical(frontend.mir, map);
  expect(canonical.supportsFailureBodyText(address),
         "the canonical state-proven propagation body should remain admitted");

  lang::MirProgram extraPredecessor = frontend.mir;
  auto &extraFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      extraPredecessor.functionInstances());
  lang::MirFunctionInstance &extraForward = extraFunctions.at(forward->id - 1);
  extraForward.body.blocks.push_back(
      {.id = static_cast<lang::MirBlockId>(extraForward.body.blocks.size() + 1),
       .terminator = {.kind = lang::MirTerminatorKind::Goto,
                      .target = stateBranch->terminator.target},
       .reachable = true});
  const lang::CppMirBodyEmitter extraPredecessorEmitter(extraPredecessor, map);
  expect(!extraPredecessorEmitter.supportsFailureBodyText(address),
         "a zero-site Expected extraction with an unproven incoming edge "
         "must fail backend admission");

  lang::MirProgram reversedState = frontend.mir;
  auto &reversedFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          reversedState.functionInstances());
  lang::MirFunctionInstance &reversedForward =
      reversedFunctions.at(forward->id - 1);
  lang::MirBlock *reversedBranch =
      reversedForward.body.findBlock(stateBranch->id);
  expect(reversedBranch != nullptr,
         "the propagation state branch should survive the forged copy");
  if (reversedBranch != nullptr) {
    std::swap(reversedBranch->terminator.target,
              reversedBranch->terminator.elseTarget);
    const lang::CppMirBodyEmitter reversedStateEmitter(reversedState, map);
    expect(!reversedStateEmitter.supportsFailureBodyText(address),
           "a zero-site Expected extraction without its matching state edge "
           "must fail backend admission");
  }

  const lang::MirValue *stateConditionValue =
      stateBranch->terminator.value
          ? forward->body.findValue(stateBranch->terminator.value->value)
          : nullptr;
  const lang::MirInstruction *stateCondition =
      stateConditionValue == nullptr
          ? nullptr
          : findInstruction(forward->body, stateConditionValue->definition);
  const lang::MirValue *carrierValue =
      stateCondition != nullptr && stateCondition->operands.size() == 1 &&
              stateCondition->operands.front().kind ==
                  lang::MirOperandKind::Value
          ? forward->body.findValue(stateCondition->operands.front().value)
          : nullptr;
  const lang::MirInstruction *carrierLoad =
      carrierValue != nullptr
          ? findInstruction(forward->body, carrierValue->definition)
          : nullptr;
  const lang::MirBlock *successStateBlock =
      forward->body.findBlock(stateBranch->terminator.target);
  const lang::MirBlock *errorStateBlock =
      forward->body.findBlock(stateBranch->terminator.elseTarget);
  const auto findExtraction = [](const lang::MirBlock *block,
                                 lang::IntrinsicKind intrinsic) {
    if (block == nullptr) {
      return static_cast<const lang::MirInstruction *>(nullptr);
    }
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const lang::MirInstruction &instruction) {
                       return instruction.intrinsic == intrinsic &&
                              instruction.localFailureSites.empty();
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  };
  const lang::MirInstruction *successStateExtraction =
      findExtraction(successStateBlock, lang::IntrinsicKind::ExpectedValue);
  const lang::MirInstruction *errorStateExtraction =
      findExtraction(errorStateBlock, lang::IntrinsicKind::ExpectedError);
  const lang::MirPlace *carrierSource =
      carrierLoad != nullptr && carrierLoad->operands.size() == 1
          ? forward->body.findPlace(carrierLoad->operands.front().place)
          : nullptr;
  expect(carrierLoad != nullptr && carrierSource != nullptr &&
             successStateExtraction != nullptr &&
             successStateExtraction->receiver &&
             errorStateExtraction != nullptr && errorStateExtraction->receiver,
         "the named propagation fixture should expose its exact checked and "
         "extracted carrier schedule");
  if (carrierLoad != nullptr && carrierSource != nullptr &&
      successStateExtraction != nullptr && successStateExtraction->receiver &&
      errorStateExtraction != nullptr && errorStateExtraction->receiver) {
    const auto forgeLoanCarriers =
        [&](std::optional<lang::IntrinsicKind> mismatchedExtraction) {
          lang::MirProgram forged = frontend.mir;
          auto &functions =
              const_cast<std::vector<lang::MirFunctionInstance> &>(
                  forged.functionInstances());
          lang::MirBody &body = functions.at(forward->id - 1).body;
          lang::MirInstruction *load =
              findMutableInstruction(body, carrierLoad->id);
          lang::MirInstruction *success =
              findMutableInstruction(body, successStateExtraction->id);
          lang::MirInstruction *error =
              findMutableInstruction(body, errorStateExtraction->id);

          const lang::MirLoanId firstLoan =
              body.loans.empty() ? 1 : body.loans.back().id + 1;
          const lang::MirLoanId secondLoan = firstLoan + 1;
          body.loans.push_back({.id = firstLoan,
                                .kind = lang::MirLoanKind::Local,
                                .source = carrierSource->id,
                                .access = lang::AccessMode::ReadOnly});
          body.loans.push_back({.id = secondLoan,
                                .kind = lang::MirLoanKind::Local,
                                .source = carrierSource->id,
                                .access = lang::AccessMode::ReadOnly});

          const lang::MirPlaceId firstPlace =
              body.places.empty() ? 1 : body.places.back().id + 1;
          const lang::MirPlaceId secondPlace = firstPlace + 1;
          const auto loanPlace = [&](lang::MirPlaceId id,
                                     lang::MirLoanId loan) {
            lang::MirPlace place = *carrierSource;
            place.id = id;
            place.root = lang::MirPlaceRootKind::Loan;
            place.binding = 0;
            place.symbol = 0;
            place.capture = 0;
            place.temporary = 0;
            place.value = 0;
            place.loan = loan;
            place.projections.clear();
            place.key.reset();
            return place;
          };
          body.places.push_back(loanPlace(firstPlace, firstLoan));
          body.places.push_back(loanPlace(secondPlace, secondLoan));

          load->operands.front().place = firstPlace;
          success->receiver->place =
              mismatchedExtraction == lang::IntrinsicKind::ExpectedValue
                  ? secondPlace
                  : firstPlace;
          error->receiver->place =
              mismatchedExtraction == lang::IntrinsicKind::ExpectedError
                  ? secondPlace
                  : firstPlace;
          return forged;
        };

    const lang::MirProgram matchingLoanCarrier =
        forgeLoanCarriers(std::nullopt);
    expect(lang::CppMirBodyEmitter(matchingLoanCarrier, map)
               .supportsFailureBodyText(address),
           "matching loan-root carriers should preserve propagation "
           "admission");
    const lang::MirProgram distinctValueLoan =
        forgeLoanCarriers(lang::IntrinsicKind::ExpectedValue);
    expect(!lang::CppMirBodyEmitter(distinctValueLoan, map)
                .supportsFailureBodyText(address),
           "a state proof for one loan must not admit ExpectedValue from a "
           "different loan root");
    const lang::MirProgram distinctErrorLoan =
        forgeLoanCarriers(lang::IntrinsicKind::ExpectedError);
    expect(!lang::CppMirBodyEmitter(distinctErrorLoan, map)
                .supportsFailureBodyText(address),
           "a state proof for one loan must not admit ExpectedError from a "
           "different loan root");
  }

  expect(globalForward != nullptr,
         "the symbol-carrier propagation fixture should lower to MIR");
  if (globalForward == nullptr) {
    return;
  }
  const lang::MirBodyAddress globalAddress{.kind = lang::MirBodyKind::Function,
                                           .owner = globalForward->id};
  const lang::CppMirBodyEmitter globalCanonical(frontend.mir, map);
  expect(globalCanonical.supportsFailureBodyText(globalAddress),
         "canonical propagation from a global Expected should be admitted");

  lang::MirProgram distinctSymbols = frontend.mir;
  auto &symbolFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      distinctSymbols.functionInstances());
  lang::MirBody &symbolBody = symbolFunctions.at(globalForward->id - 1).body;
  lang::MirInstruction *successExtraction = nullptr;
  lang::MirInstruction *errorExtraction = nullptr;
  for (lang::MirBlock &block : symbolBody.blocks) {
    if (block.terminator.kind != lang::MirTerminatorKind::Branch) {
      continue;
    }
    lang::MirBlock *success = symbolBody.findBlock(block.terminator.target);
    lang::MirBlock *error = symbolBody.findBlock(block.terminator.elseTarget);
    if (success == nullptr || error == nullptr) {
      continue;
    }
    const auto findExtraction = [](lang::MirBlock &candidate,
                                   lang::IntrinsicKind intrinsic) {
      const auto found = std::find_if(
          candidate.instructions.begin(), candidate.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.intrinsic == intrinsic &&
                   instruction.localFailureSites.empty();
          });
      return found == candidate.instructions.end() ? nullptr : &*found;
    };
    successExtraction =
        findExtraction(*success, lang::IntrinsicKind::ExpectedValue);
    errorExtraction =
        findExtraction(*error, lang::IntrinsicKind::ExpectedError);
    if (successExtraction != nullptr && errorExtraction != nullptr) {
      break;
    }
  }
  const lang::MirPlace *provenPlace =
      successExtraction != nullptr && successExtraction->receiver
          ? symbolBody.findPlace(successExtraction->receiver->place)
          : nullptr;
  const auto alternate =
      provenPlace == nullptr
          ? symbolBody.places.end()
          : std::find_if(symbolBody.places.begin(), symbolBody.places.end(),
                         [&](const lang::MirPlace &place) {
                           return place.root ==
                                      lang::MirPlaceRootKind::Symbol &&
                                  place.type == provenPlace->type &&
                                  place.symbol != provenPlace->symbol;
                         });
  expect(successExtraction != nullptr && errorExtraction != nullptr &&
             provenPlace != nullptr && alternate != symbolBody.places.end(),
         "the global propagation fixture should retain two distinct symbol "
         "carriers and both zero-site extractions");
  if (successExtraction != nullptr && errorExtraction != nullptr &&
      provenPlace != nullptr && alternate != symbolBody.places.end()) {
    successExtraction->receiver->place = alternate->id;
    const lang::CppMirBodyEmitter distinctSymbolEmitter(distinctSymbols, map);
    expect(!distinctSymbolEmitter.supportsFailureBodyText(globalAddress),
           "a has-value proof for one symbol must not admit extraction from "
           "a distinct symbol-root Expected carrier");

    lang::MirProgram distinctErrorSymbols = frontend.mir;
    auto &errorFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        distinctErrorSymbols.functionInstances());
    lang::MirBody &errorBody = errorFunctions.at(globalForward->id - 1).body;
    lang::MirInstruction *forgedError =
        findMutableInstruction(errorBody, errorExtraction->id);
    expect(forgedError != nullptr && forgedError->receiver,
           "the copied global propagation body should retain ExpectedError");
    if (forgedError != nullptr && forgedError->receiver) {
      forgedError->receiver->place = alternate->id;
      const lang::CppMirBodyEmitter distinctErrorEmitter(distinctErrorSymbols,
                                                         map);
      expect(!distinctErrorEmitter.supportsFailureBodyText(globalAddress),
             "a has-value proof for one symbol must not admit ExpectedError "
             "from a distinct symbol-root carrier");
    }
  }
}

void testDeclarationOnlyCallBoundary() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-declaration-call.gti", R"(
int32_t native_like(int32_t value);

int main() {
  return native_like(7);
}
)");
  expect(frontend.canGenerateCode(),
         "an ordinary declaration-only call should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirFunctionInstance *declaration = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    } else if (function.definitionKind ==
               lang::MirFunctionInstance::DefinitionKind::Declaration) {
      declaration = &function;
    }
  }
  expect(entry != nullptr && declaration != nullptr &&
             declaration->mayRaiseDefinedFailure,
         "MIR should retain the conservative declaration-only failure "
         "summary");
  if (entry == nullptr || declaration == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a declaration-only plain boundary should remain MIR-emittable");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "declaration-call-test-v0", 1);
  const auto targetRow =
      std::find_if(map.bodies().begin(), map.bodies().end(),
                   [&](const lang::CppMirBodyNameRepresentation &row) {
                     return row.address.kind == lang::MirBodyKind::Function &&
                            row.address.owner == declaration->id;
                   });
  const std::string target =
      targetRow == map.bodies().end() ? std::string() : targetRow->spelling;
  expect(!target.empty() && text.text.find(target + "(") != std::string::npos &&
             text.text.find("__gti_mir_call_success_") == std::string::npos,
         "the declaration-only boundary should use its plain symbol and "
         "treat a native return as success");
}

void testDeclarationOnlyMemberCallBoundary() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-declaration-member-call.gti", R"(
class ExternalBox<T> {
public:
  ExternalBox() {}
  bool ready();
};

int main() {
  mut ExternalBox<int32_t> box = ExternalBox<int32_t>();
  return box.ready() ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "an ordinary declaration-only member call should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirFunctionInstance *declaration = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    } else if (function.owner &&
               function.definitionKind ==
                   lang::MirFunctionInstance::DefinitionKind::Declaration) {
      declaration = &function;
    }
  }
  expect(entry != nullptr && declaration != nullptr &&
             declaration->mayRaiseDefinedFailure,
         "MIR should retain the conservative bodyless-member failure "
         "summary");
  if (entry == nullptr || declaration == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a bodyless member boundary should remain MIR-emittable");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
      address, "member-declaration-call-test-v0", 1);
  const auto targetRow =
      std::find_if(map.bodies().begin(), map.bodies().end(),
                   [&](const lang::CppMirBodyNameRepresentation &row) {
                     return row.address.kind == lang::MirBodyKind::Function &&
                            row.address.owner == declaration->id;
                   });
  const std::string target =
      targetRow == map.bodies().end() ? std::string() : targetRow->spelling;
  expect(!target.empty() && text.text.find(target + "(") != std::string::npos &&
             text.text.find("__gti_mir_call_success_") == std::string::npos,
         "the bodyless member boundary should use its plain member symbol "
         "and treat a native return as success");

  try {
    const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
    expect(!artifact.contents.empty(),
           "production C++ emission should accept a bodyless member call");
  } catch (const std::logic_error &) {
    expect(false, "production C++ emission should not reject a bodyless member "
                  "call");
  }
}

void testPassiveReturnSignedMinimumAndRepresentationSlot() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-passive-return-and-slot.gti", R"(
#include <std/numeric>

constexpr int8_t wrapped_boundary =
    std::wrapping_add(int8_t(127), int8_t(1));

union NumberBits {
  mut uint32_t integer;
  mut float real;
};

enum class Command {
  stop,
  translate(int32_t x, int32_t y),
  exit_code(int32_t code),
};

int32_t read_integer_bits() {
  NumberBits bits{uint32_t(42)};
  unsafe {
    return int32_t(bits.integer);
  }
}

Command next_command(mut int32_t& calls) {
  calls++;
  return Command::translate(20, 22);
}

int main() {
  Command stopped = Command::stop;
  switch (stopped) {
  case Command::stop:
    break;
  case Command::translate(x, y):
    return x + y;
  case Command::exit_code(code):
    return code;
  }

  mut int32_t calls = 0;
  switch (next_command(calls)) {
  case Command::stop:
    return 1;
  case Command::translate(x, y):
    if (wrapped_boundary != int8_t(-128)) {
      return 2;
    }
    return read_integer_bits() - (x + y);
  case Command::exit_code(code):
    return code;
  }
}
)");
  expect(frontend.canGenerateCode(),
         "the passive-return and representation-slot fixture should pass "
         "the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *signedMinimum = nullptr;
  const lang::MirFunctionInstance *representationSlot = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    bool hasSignedMinimum = false;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        hasSignedMinimum =
            hasSignedMinimum ||
            (instruction.kind == lang::MirInstructionKind::Compute &&
             instruction.operation == lang::MirOperation::Negate &&
             instruction.programConstantSubstitution);
      }
    }
    if (hasSignedMinimum) {
      signedMinimum = &function;
    }
    if (function.returnType == lang::SemanticType::Int32 &&
        function.parameterTypes.empty() &&
        function.body.dropObligations.empty() &&
        std::any_of(function.body.places.begin(), function.body.places.end(),
                    [](const lang::MirPlace &place) {
                      return place.root == lang::MirPlaceRootKind::Binding &&
                             place.projections.empty() &&
                             place.type.kind == lang::SemanticType::Class &&
                             place.traits.drop == lang::DropKind::Trivial;
                    })) {
      representationSlot = &function;
    }
  }
  const bool constantReturn =
      entry != nullptr &&
      std::any_of(entry->body.blocks.begin(), entry->body.blocks.end(),
                  [](const lang::MirBlock &block) {
                    return block.terminator.kind ==
                               lang::MirTerminatorKind::Return &&
                           block.terminator.value &&
                           block.terminator.value->kind ==
                               lang::MirOperandKind::Constant;
                  });
  expect(signedMinimum != nullptr && representationSlot != nullptr &&
             entry != nullptr && constantReturn,
         "the fixture should lower a fused signed minimum, a trivial class "
         "slot, and a passive constant return");
  if (signedMinimum == nullptr || representationSlot == nullptr ||
      entry == nullptr || !constantReturn) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const auto failureText = [&](const lang::MirFunctionInstance &function,
                               std::string_view label) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    expect(emitter.analyze(address).ready() &&
               emitter.supportsFailureBodyText(address),
           "the passive leaf fixture should enter transformed failure-body "
           "emission");
    return emitter.supportsFailureBodyText(address)
               ? emitter.emitFailureBodyText(address, label, 1).text
               : std::string{};
  };

  const std::string signedText =
      failureText(*signedMinimum, "signed-minimum-test-v0");
  expect(signedText.find("signed-minimum magnitude spells at its negation") !=
                 std::string::npos &&
             signedText.find("static_cast<std::int8_t>(-127 - 1)") !=
                 std::string::npos,
         "the signed minimum should fuse its otherwise unrepresentable "
         "positive magnitude into the typed negation");

  const std::string slotText =
      failureText(*representationSlot, "representation-slot-test-v0");
  expect(slotText.find("mir_lifetime_slot<") != std::string::npos &&
             slotText.find(", true>") != std::string::npos,
         "a trivial GTI value without a Drop obligation should select the "
         "representation-only lifetime-slot policy");

  const std::string entryText = failureText(*entry, "passive-return-test-v0");
  expect(
      entryText.find("*__gti_mir_out_result = static_cast<std::int32_t>(0);") !=
          std::string::npos,
      "a passive constant Return operand should publish directly through "
      "the transformed result pointer");
}

void testSingleUseClassSsaRepresentationSlot() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-class-ssa-slot.gti", R"(
#include <std/algorithm>
#include <std/vector>

class Positive {
  mut bool observed = false;

public:
  bool operator()(int32_t& value) mut {
    this.observed = true;
    return value > 0;
  }
};

int main() {
  mut std::vector<int32_t> values{};
  values.push_back(7);
  return int32_t(std::count_if(values.begin(), values.end(), Positive())) - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the single-use class SSA fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  lang::MirValueId temporary = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &producer : block.instructions) {
        if (producer.kind != lang::MirInstructionKind::Call ||
            !producer.result || producer.functionTarget ||
            producer.constructorTarget || producer.receiver ||
            !producer.operands.empty() ||
            producer.info.type.kind != lang::SemanticType::Class) {
          continue;
        }
        bool directCallArgument = false;
        for (const lang::MirBlock &candidate : function.body.blocks) {
          for (const lang::MirInstruction &consumer : candidate.instructions) {
            directCallArgument =
                directCallArgument ||
                (consumer.kind == lang::MirInstructionKind::Call &&
                 std::any_of(consumer.operands.begin(), consumer.operands.end(),
                             [&](const lang::MirOperand &operand) {
                               return operand.kind ==
                                          lang::MirOperandKind::Value &&
                                      operand.value == *producer.result;
                             }));
          }
        }
        if (directCallArgument) {
          selected = &function;
          temporary = *producer.result;
        }
      }
    }
  }
  expect(selected != nullptr,
         "the fixture should lower a default-constructed class SSA value used "
         "by one call");
  if (selected == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the single-use class SSA fixture should enter transformed emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "class-ssa-slot-test-v0", 1).text;
  const std::string name = "__gti_mir_v_" + std::to_string(temporary);
  expect(text.find(", true> " + name + ";") != std::string::npos &&
             text.find(name + ".construct();") != std::string::npos &&
             text.find("std::move(" + name + ".get())") != std::string::npos &&
             text.find(name + ".destroy();") != std::string::npos,
         "the class temporary should construct, transfer, and retire through "
         "one representation-only lifetime slot");
}

void testRepresentationOnlyPublicationRearmsBeforeConstruction() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-representation-slot-rearm.gti", R"(
[[c_abi]] struct Pair {
  mut float x;
  mut float y;
};

extern "C" {
  Pair make_pair_c(float x, float y);
}

int main() {
  for (mut int32_t iteration = 0; iteration < 3; iteration++) {
    Pair pair = make_pair_c(1.0, 2.0);
    if (pair.x < 0.0) {
      return 1;
    }
  }
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the repeated passive-record publication fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the repeated passive-record fixture should lower its entry");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  const bool failureBody = emitter.supportsFailureBodyText(address);
  expect(emitter.analyze(address).ready() &&
             (failureBody || emitter.supportsBodyText(address)),
         "the repeated passive-record publication should enter transformed "
         "emission");
  if (!failureBody && !emitter.supportsBodyText(address)) {
    return;
  }

  const std::string text =
      failureBody
          ? emitter.emitFailureBodyText(address, "slot-rearm-test-v0", 1).text
          : emitter.emitBodyText(address, "slot-rearm-test-v0", 1).text;
  const std::string constructMarker =
      ".construct_from([&]() { return make_pair_c(";
  const std::size_t construction = text.find(constructMarker);
  const std::size_t slotStart = construction == std::string::npos
                                    ? std::string::npos
                                    : text.rfind("__gti_mir_p_", construction);
  const std::string slot =
      slotStart == std::string::npos
          ? std::string{}
          : text.substr(slotStart, construction - slotStart);
  const std::string rearmMarker = slot + ".destroy_if_engaged();";
  const std::string publicationMarker = slot + constructMarker;
  const std::size_t rearm =
      slot.empty() ? std::string::npos : text.find(rearmMarker);
  const std::size_t publication =
      slot.empty() ? std::string::npos : text.find(publicationMarker);
  const std::string separator =
      rearm == std::string::npos || publication == std::string::npos ||
              rearm + rearmMarker.size() > publication
          ? std::string{"not-adjacent"}
          : text.substr(rearm + rearmMarker.size(),
                        publication - (rearm + rearmMarker.size()));
  expect(!slot.empty() &&
             text.find(", true> " + slot + ";") != std::string::npos &&
             rearm != std::string::npos && publication != std::string::npos &&
             rearm < publication &&
             separator.find_first_not_of(" \t\r\n") == std::string::npos,
         "an exact repeated Initialize should rearm its representation-only "
         "slot immediately before construct_from");
}

void testCopyParameterFieldPublicationAdmission() {
  // A constructor member initializer may copy a passive generated-copy class
  // directly from its exact by-value parameter. This MIR shape carries the
  // parameter Load and full-expression boundary but no field Initialize; the
  // member-initializer metadata is the publication authority.
  lang::FrontendResult copyForm =
      analyzeWithStandardLibrary("cpp-mir-copy-parameter-field.gti", R"(
class Payload {
public:
  int32_t value;
  Payload(int32_t value) : value(value) {}
};

class Holder {
public:
  Payload inner;
  Holder(Payload payload) : inner(payload) {}
};

int main() {
  Holder holder = Holder(Payload(3));
  return holder.inner.value - 3;
}
)");
  expect(copyForm.canGenerateCode(),
         "the copy-parameter field fixture should pass the frontend");
  if (!copyForm.canGenerateCode()) {
    return;
  }
  const auto findHolderConstructor =
      [](lang::FrontendResult &frontend) -> lang::MirConstructorInstance * {
    lang::MirConstructorInstance *selected = nullptr;
    auto &constructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            frontend.mir.constructorInstances());
    for (lang::MirConstructorInstance &constructor : constructors) {
      if (constructor.parameterTypes.size() == 1 &&
          constructor.parameterTypes.front().kind ==
              lang::SemanticType::Class &&
          !constructor.initializers.empty()) {
        selected = &constructor;
      }
    }
    return selected;
  };
  lang::MirConstructorInstance *copyConstructor =
      findHolderConstructor(copyForm);
  expect(copyConstructor != nullptr,
         "the copy fixture should lower the class-parameter constructor");
  if (copyConstructor == nullptr) {
    return;
  }
  {
    const lang::CppMirBodyEmissionMap map(
        buildRows(copyForm, copyForm.mir, lang::CppStandard::Cpp23));
    const lang::CppMirBodyEmitter emitter(copyForm.mir, map);
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                       .owner = copyConstructor->id};
    const auto bindings =
        lang::cppMirCopyParameterFieldBindings(copyForm.mir, *copyConstructor);
    expect(bindings && bindings->size() == 1 &&
               bindings->front().initializeInstruction == 0,
           "the copy-parameter field publication should prove the exact "
           "metadata-only passive class shape");
    expect(emitter.supportsBodyText(address),
           "the exact passive class copy must enter the ordinary constructor "
           "form");
    expect(emitter.supportsFailureBodyText(address),
           "the exact passive class copy must enter the failure constructor "
           "form");
    if (emitter.supportsBodyText(address)) {
      const std::string text =
          emitter.emitBodyText(address, "copy-parameter-field-v0", 1).text;
      expect(text.find("parameter copy publishes in the member initializer "
                       "list") != std::string::npos,
             "the class parameter Load should be consumed by the exact "
             "member-initializer publication");
    }
  }

  lang::MirInstruction *copyLoad = nullptr;
  for (lang::MirBlock &block : copyConstructor->body.blocks) {
    for (lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Load &&
          instruction.result && instruction.ownership) {
        copyLoad = &instruction;
      }
    }
  }
  expect(copyLoad != nullptr,
         "the copy fixture should retain its parameter Load proof input");
  if (copyLoad != nullptr) {
    copyLoad->ownership->reachable = false;
    const auto malformedBindings =
        lang::cppMirCopyParameterFieldBindings(copyForm.mir, *copyConstructor);
    expect(malformedBindings && malformedBindings->empty(),
           "unreachable forged ownership must invalidate the passive class "
           "copy binding");
    const lang::CppMirBodyEmissionMap malformedMap(
        buildRows(copyForm, copyForm.mir, lang::CppStandard::Cpp23));
    const lang::CppMirBodyEmitter malformedEmitter(copyForm.mir, malformedMap);
    const lang::MirBodyAddress malformedAddress{
        .kind = lang::MirBodyKind::Constructor, .owner = copyConstructor->id};
    expect(!malformedEmitter.supportsBodyText(malformedAddress) &&
               !malformedEmitter.supportsFailureBodyText(malformedAddress),
           "the passive class copy route must fail closed when its exact "
           "ownership event drifts");
  }

  // A scalar parameter published through a normal MIR Initialize remains a
  // body-local place when the constructor reads it again after the member
  // initializer. Only the metadata-only passive class copy above may elide
  // that carrier in favor of the native parameter.
  lang::FrontendResult scalarReuse =
      analyzeWithStandardLibrary("cpp-mir-scalar-parameter-reuse.gti", R"(
class ScalarReuse {
public:
  int32_t stored;
  mut int32_t observed = 0;

  ScalarReuse(int32_t value) : stored(value) {
    observed = value;
  }
};

int main() {
  ScalarReuse value = ScalarReuse(3);
  return value.observed - 3;
}
)");
  expect(scalarReuse.canGenerateCode(),
         "the scalar parameter reuse fixture should pass the frontend");
  if (scalarReuse.canGenerateCode()) {
    const lang::MirConstructorInstance *reuseConstructor = nullptr;
    for (const lang::MirConstructorInstance &constructor :
         scalarReuse.mir.constructorInstances()) {
      const lang::MirClassInstance *owner =
          scalarReuse.mir.findClassInstance(constructor.owner);
      const lang::ClassTypeInfo *info =
          owner == nullptr
              ? nullptr
              : scalarReuse.semantics.findClassType(owner->declaration);
      if (info != nullptr && info->declaration != nullptr &&
          info->declaration->name().lexeme == "ScalarReuse") {
        reuseConstructor = &constructor;
        break;
      }
    }
    expect(reuseConstructor != nullptr,
           "the scalar fixture should lower its constructor instance");
    if (reuseConstructor != nullptr) {
      const auto bindings = lang::cppMirCopyParameterFieldBindings(
          scalarReuse.mir, *reuseConstructor);
      expect(bindings && bindings->size() == 1 &&
                 bindings->front().initializeInstruction != 0,
             "the scalar field copy should retain its ordinary MIR "
             "Initialize publication");
      const lang::CppMirBodyEmissionMap map(
          buildRows(scalarReuse, scalarReuse.mir, lang::CppStandard::Cpp23));
      const lang::CppMirBodyEmitter emitter(scalarReuse.mir, map);
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                         .owner = reuseConstructor->id};
      expect(emitter.supportsFailureBodyText(address),
             "the scalar parameter reuse should enter failure-body "
             "emission");
      if (bindings && bindings->size() == 1 &&
          emitter.supportsFailureBodyText(address)) {
        const std::string text =
            emitter.emitFailureBodyText(address, "scalar-parameter-reuse-v0", 1)
                .text;
        expect(text.find("std::int32_t __gti_mir_p_" +
                         std::to_string(bindings->front().sourcePlace) +
                         " = __gti_mir_arg_0;") != std::string::npos,
               "a scalar constructor parameter reused in the body must keep "
               "its MIR place declaration");
      }
    }
  }

  // The moved flavor of the same constructor keeps its admitted
  // member-initializer publication.
  lang::FrontendResult moveForm =
      analyzeWithStandardLibrary("cpp-mir-move-parameter-field.gti", R"(
class Payload {
public:
  int32_t value;
  Payload(int32_t value) : value(value) {}
};

class Holder {
public:
  Payload inner;
  Holder(Payload payload) : inner(std::move(payload)) {}
};

int main() {
  Holder holder = Holder(Payload(3));
  return holder.inner.value - 3;
}
)");
  expect(moveForm.canGenerateCode(),
         "the move-parameter field fixture should pass the frontend");
  if (!moveForm.canGenerateCode()) {
    return;
  }
  lang::MirConstructorInstance *moveConstructor =
      findHolderConstructor(moveForm);
  expect(moveConstructor != nullptr,
         "the move fixture should lower the class-parameter constructor");
  if (moveConstructor == nullptr) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(
      buildRows(moveForm, moveForm.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(moveForm.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                     .owner = moveConstructor->id};
  expect(emitter.supportsFailureBodyText(address),
         "the moved-parameter field publication must keep its admitted "
         "failure form");
}

void testFailureFormSubscriptCompatibilityBoundary() {
  lang::FrontendResult frontend = analyze("subscript-boundary.gti", R"(
class Indexed {
  mut int elements[1] = {1};

public:
  int& operator[](uint64_t index) { return this.elements[index]; }
  mut int& operator[](uint64_t index) mut { return this.elements[index]; }
};

int mutate(mut Indexed& indexed) {
  indexed[uint64_t(0)] *= 2;
  return indexed[uint64_t(0)];
}

int main() {
  mut Indexed indexed = Indexed();
  indexed[uint64_t(0)] += 1;
  return mutate(indexed) - 4;
}
)");
  expect(frontend.canGenerateCode(),
         "the reduced failure-capable subscript fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  std::size_t selectedBoundaries = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.overloadedOperator &&
        *function.overloadedOperator == lang::OverloadedOperator::Subscript &&
        lang::cppMirNeedsTerminalSubscriptBoundaryWrapper(frontend.mir, map,
                                                          function.id)) {
      ++selectedBoundaries;
    }
  }
  expect(selectedBoundaries == 2,
         "the compatibility places should select the exact read-only and "
         "mutable subscript boundary instances");

  const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
  std::size_t normalDefinitions = 0;
  bool checkedReferenceCompound = false;
  std::istringstream lines(artifact.contents);
  for (std::string line; std::getline(lines, line);) {
    normalDefinitions +=
        line.find("Indexed::__gti_fn_") != std::string::npos &&
        line.find("___gti_operator_subscript(") != std::string::npos &&
        line.find(" {") != std::string::npos;
    checkedReferenceCompound =
        checkedReferenceCompound ||
        (line.find("mir_checked_compound_multiply_v1(") != std::string::npos &&
         line.find("___gti_operator_subscript(") != std::string::npos);
  }
  expect(normalDefinitions == 2,
         "the C++ artifact should define both ordinary subscript boundaries "
         "instead of leaving compatibility calls unresolved");
  expect(checkedReferenceCompound,
         "a reference-projected class subscript should preserve the checked "
         "compound operation instead of degrading it to assignment");
}

constexpr NamedTest kTests[] = {
    {"testOwningPayloadEnumRepresentationAdmission",
     testOwningPayloadEnumRepresentationAdmission},
    {"testDischargedCompoundOperatorVocabulary",
     testDischargedCompoundOperatorVocabulary},
    {"testExhaustiveEnumClassification", testExhaustiveEnumClassification},
    {"testReadyBodyAndRepresentationFailures",
     testReadyBodyAndRepresentationFailures},
    {"testImmutableBodyPlanOutlivesAdmissionInputs",
     testImmutableBodyPlanOutlivesAdmissionInputs},
    {"testHostedStartupOwnsRemainingAuthorityGap",
     testHostedStartupOwnsRemainingAuthorityGap},
    {"testVerifiedProgramInitializationPlanClassification",
     testVerifiedProgramInitializationPlanClassification},
    {"testSafeIndexUsesBoundsNotRawMemory",
     testSafeIndexUsesBoundsNotRawMemory},
    {"testUnsafeRawPointerVocabulary", testUnsafeRawPointerVocabulary},
    {"testConcreteGenericFieldOwner", testConcreteGenericFieldOwner},
    {"testAmbiguousStaticStorageFailsClosed",
     testAmbiguousStaticStorageFailsClosed},
    {"testGlobalClassStorageMemberProjection",
     testGlobalClassStorageMemberProjection},
    {"testConcreteClassStaticStorage", testConcreteClassStaticStorage},
    {"testDischargedStorageReadAnalysis", testDischargedStorageReadAnalysis},
    {"testExpectedExtractionStateAdmissionFailsClosed",
     testExpectedExtractionStateAdmissionFailsClosed},
    {"testDeclarationOnlyCallBoundary", testDeclarationOnlyCallBoundary},
    {"testDeclarationOnlyMemberCallBoundary",
     testDeclarationOnlyMemberCallBoundary},
    {"testPassiveReturnSignedMinimumAndRepresentationSlot",
     testPassiveReturnSignedMinimumAndRepresentationSlot},
    {"testSingleUseClassSsaRepresentationSlot",
     testSingleUseClassSsaRepresentationSlot},
    {"testRepresentationOnlyPublicationRearmsBeforeConstruction",
     testRepresentationOnlyPublicationRearmsBeforeConstruction},
    {"testCopyParameterFieldPublicationAdmission",
     testCopyParameterFieldPublicationAdmission},
    {"testFailureFormSubscriptCompatibilityBoundary",
     testFailureFormSubscriptCompatibilityBoundary},
};

} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "preflight");
}
