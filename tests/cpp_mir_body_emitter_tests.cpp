#include "../src/compiler/cpp_mir_body_emitter.h"
#include "../src/compiler/cpp_mir_representation_snapshot.h"

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(std::string name, std::string source) {
  lang::FrontendResult result =
      lang::Frontend().analyze(std::move(name), std::move(source));
  if (!result.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  return result;
}

lang::FrontendResult analyzeWithStandardLibrary(std::string name,
                                                std::string source) {
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  lang::FrontendResult result = lang::Frontend().analyze(
      std::move(name), std::move(source), {standardLibrary / "prelude.gti"}, {},
      {standardLibrary});
  if (!result.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  return result;
}

lang::CppMirBodyEmissionMapRows
buildRows(const lang::FrontendResult &frontend, const lang::MirProgram &mir,
          lang::CppStandard standard = lang::CppStandard::Cpp23) {
  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, mir);
  return lang::buildCppMirBodyEmissionMapRows(lowered, standard);
}

bool hasIssue(const lang::CppMirBodyEmissionAnalysis &analysis,
              lang::CppMirBodyEmissionIssueKind kind) {
  return std::any_of(analysis.issues.begin(), analysis.issues.end(),
                     [kind](const lang::CppMirBodyEmissionIssue &issue) {
                       return issue.kind == kind;
                     });
}

lang::CppMirTypeRepresentationKind
representationKind(const lang::SemanticType &type) {
  switch (type.kind) {
  case lang::SemanticType::Void:
    return lang::CppMirTypeRepresentationKind::Void;
  case lang::SemanticType::Int8:
  case lang::SemanticType::Int16:
  case lang::SemanticType::Int32:
  case lang::SemanticType::Int64:
  case lang::SemanticType::UInt8:
  case lang::SemanticType::UInt16:
  case lang::SemanticType::UInt32:
  case lang::SemanticType::UInt64:
  case lang::SemanticType::Float:
  case lang::SemanticType::Double:
  case lang::SemanticType::Bool:
  case lang::SemanticType::Char:
    return lang::CppMirTypeRepresentationKind::Scalar;
  case lang::SemanticType::StringView:
    return lang::CppMirTypeRepresentationKind::StringView;
  case lang::SemanticType::CString:
    return lang::CppMirTypeRepresentationKind::RawPointer;
  case lang::SemanticType::NullPtr:
    return lang::CppMirTypeRepresentationKind::NullPointer;
  case lang::SemanticType::RawPointer:
  case lang::SemanticType::NativeFunction:
    return lang::CppMirTypeRepresentationKind::RawPointer;
  case lang::SemanticType::Reference:
    return lang::CppMirTypeRepresentationKind::Reference;
  case lang::SemanticType::Array:
    return lang::CppMirTypeRepresentationKind::FixedArray;
  case lang::SemanticType::Class:
    return lang::CppMirTypeRepresentationKind::Class;
  case lang::SemanticType::Enum:
    return lang::CppMirTypeRepresentationKind::Enum;
  case lang::SemanticType::Function:
    return lang::CppMirTypeRepresentationKind::Function;
  case lang::SemanticType::Lambda:
    return lang::CppMirTypeRepresentationKind::Lambda;
  case lang::SemanticType::UniqueOwner:
    return lang::CppMirTypeRepresentationKind::UniqueOwner;
  case lang::SemanticType::SharedPointer:
    return lang::CppMirTypeRepresentationKind::SharedPointer;
  case lang::SemanticType::Storage:
  case lang::SemanticType::PrefixStorage:
    return lang::CppMirTypeRepresentationKind::Storage;
  case lang::SemanticType::Expected:
    return lang::CppMirTypeRepresentationKind::Expected;
  case lang::SemanticType::Unexpected:
    return lang::CppMirTypeRepresentationKind::Unexpected;
  case lang::SemanticType::TypeParameter:
  case lang::SemanticType::TypePack:
  case lang::SemanticType::TypeName:
  case lang::SemanticType::Unknown:
    return lang::CppMirTypeRepresentationKind::Meta;
  }
  return lang::CppMirTypeRepresentationKind::Meta;
}

void addType(lang::CppMirBodyEmissionMapRows &rows,
             const lang::SemanticType &type) {
  if (type == lang::SemanticType::Unknown) {
    return;
  }
  if (std::none_of(rows.types.begin(), rows.types.end(),
                   [&](const lang::CppMirTypeRepresentation &row) {
                     return row.type == type;
                   })) {
    rows.types.push_back({.type = type,
                          .kind = representationKind(type),
                          .spelling = "test_type"});
  }
  for (const lang::SemanticType &argument : type.arguments) {
    addType(rows, argument);
  }
  for (const lang::SemanticType &argument : type.lambdaEnclosingClassTypes) {
    addType(rows, argument);
  }
  for (const lang::SemanticType &argument : type.lambdaEnclosingFunctionTypes) {
    addType(rows, argument);
  }
}

void addSymbol(lang::CppMirBodyEmissionMapRows &rows,
               lang::CppMirSymbolRepresentation row) {
  const bool present = std::any_of(
      rows.symbols.begin(), rows.symbols.end(),
      [&](const lang::CppMirSymbolRepresentation &current) {
        return current.kind == row.kind && current.owner == row.owner &&
               current.symbol == row.symbol && current.ordinal == row.ordinal;
      });
  if (!present) {
    rows.symbols.push_back(std::move(row));
  }
}

void addBodyTypesAndStorage(lang::CppMirBodyEmissionMapRows &rows,
                            const lang::MirBody &body) {
  addType(rows, body.returnType);
  for (const lang::MirPlace &place : body.places) {
    addType(rows, place.type);
    if (place.root == lang::MirPlaceRootKind::Symbol && place.capture == 0 &&
        place.symbol != 0 && place.projections.empty()) {
      addSymbol(rows, {.kind = lang::CppMirSymbolRepresentationKind::Storage,
                       .owner = 0,
                       .symbol = place.symbol,
                       .type = place.type,
                       .spelling = "test_storage"});
    }
  }
  for (const lang::MirDropObligation &obligation : body.dropObligations) {
    addType(rows, obligation.dropType.type);
  }
  for (const lang::MirValue &value : body.values) {
    addType(rows, value.info.type);
  }
  for (const lang::MirBlock &block : body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      addType(rows, instruction.info.type);
      addType(rows, instruction.dispatchOwner);
      for (const lang::SemanticType &type : instruction.parameterTypes) {
        addType(rows, type);
      }
      for (const lang::SemanticType &type : instruction.closureCaptureTypes) {
        addType(rows, type);
      }
      if (instruction.receiver) {
        addType(rows, instruction.receiver->type);
      }
      for (const lang::MirOperand &operand : instruction.operands) {
        addType(rows, operand.type);
      }
    }
    if (block.terminator.value) {
      addType(rows, block.terminator.value->type);
    }
    for (const lang::MirSwitchTarget &target : block.terminator.switchTargets) {
      if (target.value) {
        addType(rows, target.value->type);
      }
    }
  }
}

lang::CppMirBodyEmissionMapRows completeRows(
    const lang::MirProgram &program,
    std::optional<lang::CppMirEmissionCapabilityKind> omitted = std::nullopt) {
  lang::CppMirBodyEmissionMapRows rows;
  for (const lang::MirBodyAddress address :
       lang::enumerateMirBodyAddresses(program)) {
    rows.bodies.push_back({.address = address, .spelling = "test_body"});
    const lang::MirBody *body = lang::findMirBody(program, address);
    if (body != nullptr) {
      addBodyTypesAndStorage(rows, *body);
    }
  }

  for (const lang::MirClassInstance &instance : program.classInstances()) {
    addType(rows, instance.type);
    for (const lang::MirClassFieldInfo &field : instance.declaredFields) {
      addType(rows, field.type);
      addSymbol(rows, {.kind = lang::CppMirSymbolRepresentationKind::Field,
                       .owner = instance.id,
                       .symbol = field.symbol,
                       .type = field.type,
                       .spelling = "test_field"});
    }
  }
  for (const lang::MirFunctionInstance &function :
       program.functionInstances()) {
    addType(rows, function.returnType);
    for (const lang::SemanticType &type : function.parameterTypes) {
      addType(rows, type);
    }
    for (const lang::MirCallableParameter &parameter :
         function.callableParameters) {
      addType(rows, parameter.callableType);
      for (const lang::MirCallableSignature &signature : parameter.signatures) {
        addType(rows, signature.returnType);
        for (const lang::SemanticType &type : signature.parameterTypes) {
          addType(rows, type);
        }
      }
    }
  }
  for (const lang::MirConstructorInstance &constructor :
       program.constructorInstances()) {
    for (const lang::SemanticType &type : constructor.parameterTypes) {
      addType(rows, type);
    }
    for (const lang::MirConstructorInitializer &initializer :
         constructor.initializers) {
      addType(rows, initializer.targetType);
    }
  }
  for (const lang::MirLambdaInstance &lambda : program.lambdaInstances()) {
    addType(rows, lambda.type);
    addType(rows, lambda.returnType);
    for (const lang::SemanticType &type : lambda.parameterTypes) {
      addType(rows, type);
    }
    for (std::size_t index = 0; index < lambda.captureTypes.size(); ++index) {
      addType(rows, lambda.captureTypes[index]);
      if (index < lambda.captureSymbols.size() &&
          lambda.captureSymbols[index] != 0) {
        addSymbol(rows, {.kind = lang::CppMirSymbolRepresentationKind::Capture,
                         .owner = lambda.id,
                         .symbol = lambda.captureSymbols[index],
                         .ordinal = index + 1,
                         .type = lambda.captureTypes[index],
                         .spelling = "test_capture"});
      }
    }
  }
  for (const lang::MirProgramInitializationStep &step :
       program.programInitializationPlan().steps) {
    const lang::MirPlace *storage =
        program.module().findPlace(step.storagePlace);
    if (storage != nullptr) {
      addSymbol(rows, {.kind = lang::CppMirSymbolRepresentationKind::Storage,
                       .owner = step.ownerClass,
                       .symbol = step.symbol,
                       .type = storage->type,
                       .spelling = "test_program_storage"});
    }
  }
  for (std::size_t value = 0;
       value <
       static_cast<std::size_t>(lang::CppMirEmissionCapabilityKind::Count);
       ++value) {
    const auto kind = static_cast<lang::CppMirEmissionCapabilityKind>(value);
    if (!omitted || *omitted != kind) {
      rows.capabilities.push_back(
          {.kind = kind, .spelling = "test_capability"});
    }
  }
  return rows;
}

const lang::MirFunctionInstance *
firstSourceFunction(const lang::MirProgram &program) {
  const auto found = std::find_if(
      program.functionInstances().begin(), program.functionInstances().end(),
      [](const lang::MirFunctionInstance &function) {
        return function.definitionKind == lang::MirDefinitionKind::Source;
      });
  return found == program.functionInstances().end() ? nullptr : &*found;
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

void testOwningCheckedBodyEmitsWholeCleanupProof() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-owning-failure.gti", R"(
class FailureCleanup {
public:
  int32_t bit;
  FailureCleanup(int32_t value) : bit(value) {}
  ~FailureCleanup() { this.bit + 1; }
};

int32_t checked_increment(int32_t value) {
  FailureCleanup scope = FailureCleanup(1);
  return value + 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning checked-operation fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto function = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(),
      [](const lang::MirFunctionInstance &candidate) {
        return !candidate.body.failureRecords.empty() &&
               std::any_of(candidate.body.dropObligations.begin(),
                           candidate.body.dropObligations.end(),
                           [](const lang::MirDropObligation &obligation) {
                             return obligation.dropType.requiresActiveCleanup;
                           });
      });
  expect(function != frontend.mir.functionInstances().end(),
         "the owning fixture should retain failure records and cleanup "
         "obligations in one function body");
  if (function == frontend.mir.functionInstances().end()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(completeRows(frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function->id};
  const lang::CppMirBodyEmissionAnalysis analysis = emitter.analyze(address);
  expect(analysis.ready(),
         "an owning checked body should be ready once MIR carries its exact "
         "failure-cleanup drop");
  expect(emitter.supportsFailureBodyText(address),
         "an owning checked body should prove its failure-form cleanup text");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "owning-cleanup-test-v0", 1);
  expect(text.emitted() &&
             text.text.find(".destroy_with_failure(") != std::string::npos &&
             text.text.find("return false;") != std::string::npos,
         "the failure edge must run the engaged slot's fallible destructor "
         "before returning failure");
}

void testRichFieldInitializerSchedule() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-rich-field-initializers.gti", R"(
enum class Mode {
  Ready,
};

int32_t baseline = 11;

class Reading {
  int32_t value;
public:
  Reading(int32_t initial) : value(initial) {}
};

class Dashboard {
  Mode mode = Mode::Ready;
  int32_t baseline_copy = baseline;
  Reading initial{7};
  int32_t samples[2] = {2, 3};
};
)");
  expect(frontend.canGenerateCode(),
         "the rich field-initializer fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::CppMirInitializerScheduleText *selected = nullptr;
  std::optional<lang::CppMirInitializerScheduleText> schedule;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    lang::CppMirInitializerScheduleText candidate = emitter.initializerSchedule(
        {.kind = lang::MirBodyKind::FieldInitializers, .owner = instance.id});
    if (candidate.supported && candidate.fields.size() == 4) {
      schedule = std::move(candidate);
      selected = &*schedule;
      break;
    }
  }
  expect(selected != nullptr,
         "the dashboard initializer body should expose its four-field MIR "
         "schedule");
  if (selected == nullptr) {
    return;
  }
  const auto hasSpelling = [&](std::string_view first,
                               std::string_view second = {}) {
    return std::any_of(
        selected->fields.begin(), selected->fields.end(),
        [&](const lang::CppMirFieldInitializerSpelling &field) {
          return field.spelling.find(first) != std::string::npos &&
                 (second.empty() ||
                  field.spelling.find(second) != std::string::npos);
        });
  };
  expect(hasSpelling("Mode::Ready"),
         "the enum field should use the copied enumerator storage row");
  expect(hasSpelling("::__gti_program::baseline"),
         "the global-backed field should use the copied program-storage "
         "row");
  expect(hasSpelling("Reading", "(static_cast<std::int32_t>(7))"),
         "the direct field construction should be reconstructed from the "
         "MIR Construct and CallInput stages");
  expect(hasSpelling("std::array<std::int32_t, 2>",
                     "{static_cast<std::int32_t>(2), "
                     "static_cast<std::int32_t>(3)}"),
         "the fixed-array field aggregate should preserve its staged element "
         "order");
}

void testDependentFieldInitializerSchedule() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-dependent-field-initializers.gti", R"(
class StaticArray<T, uint64_t N> {
  T values[N] = {};
};

class WrappedArray<T, uint64_t N> {
  StaticArray<T, N> value = StaticArray<T, N>();
};

int main() {
  StaticArray<int32_t, 4> four = StaticArray<int32_t, 4>();
  StaticArray<int32_t, 8> eight = StaticArray<int32_t, 8>();
  WrappedArray<int32_t, 4> wrapped_four = WrappedArray<int32_t, 4>();
  WrappedArray<int32_t, 8> wrapped_eight = WrappedArray<int32_t, 8>();
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "dependent field-initializer fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t arrays = 0;
  std::size_t wrappers = 0;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    const lang::ClassTypeInfo *info =
        frontend.semantics.findClassType(instance.declaration);
    if (info == nullptr || info->declaration == nullptr ||
        (info->declaration->name().lexeme != "StaticArray" &&
         info->declaration->name().lexeme != "WrappedArray")) {
      continue;
    }
    const lang::CppMirInitializerScheduleText schedule =
        emitter.initializerSchedule(
            {.kind = lang::MirBodyKind::FieldInitializers,
             .owner = instance.id});
    expect(schedule.supported && schedule.fields.size() == 1,
           "each dependent class instance should retain one verified field "
           "initializer");
    if (!schedule.supported || schedule.fields.size() != 1) {
      continue;
    }
    const std::string &spelling = schedule.fields.front().spelling;
    if (info->declaration->name().lexeme == "StaticArray") {
      expect(spelling == "std::array<T, N>{}",
             "all concrete StaticArray instances should project one dependent "
             "aggregate spelling");
      ++arrays;
    } else {
      expect(spelling.find("StaticArray<T, N>{}") != std::string::npos,
             "all concrete WrappedArray instances should project one "
             "dependent default-construction spelling");
      ++wrappers;
    }
  }
  expect(arrays == 2 && wrappers == 2,
         "the dependent initializer fixture should exercise both concrete "
         "instances of each class template");
}

void testContainedOwningFieldConstructor() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-body-owning-field-constructor.gti", R"(
#include <std/vector>

class Holder {
  mut std::vector<int32_t> values = std::vector<int32_t>();

public:
  Holder() {}
};

int32_t main() {
  Holder value{};
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning field-constructor fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::HirClassInstance *holder = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "Holder") {
      holder = &instance;
      break;
    }
  }
  expect(holder != nullptr,
         "the fixture should retain the Holder HIR class instance");
  if (holder == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirClassInstance *owner =
      frontend.mir.findClassInstance(holder->id);
  expect(owner != nullptr, "the fixture should retain the Holder MIR instance");
  if (owner == nullptr) {
    return;
  }
  const lang::CppMirInitializerScheduleText schedule =
      emitter.initializerSchedule(
          {.kind = lang::MirBodyKind::FieldInitializers, .owner = owner->id});
  expect(schedule.supported && schedule.fields.size() == 1,
         "Holder's native in-class field schedule should be verified by MIR");
  expect(owner->fieldInitializers.dropObligations.size() == 2 &&
             !owner->fieldInitializers.failureRecords.empty(),
         "Holder's field-initializer MIR should retain the temporary, field "
         "rollback, and checked construction edge erased by native direct "
         "initialization");

  const lang::MirConstructorInstance *ownerConstructor = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (constructor.owner == owner->id) {
      ownerConstructor = &constructor;
    }
  }
  expect(ownerConstructor != nullptr &&
             ownerConstructor->mayRaiseDefinedFailure,
         "Holder's concrete constructor should retain the field-initializer "
         "failure effect even when its own source body is empty");
  if (ownerConstructor == nullptr) {
    return;
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto marker = [&](std::size_t constructor) {
    return artifact.contents.find("scalar-cfg-v1 constructor-instance " +
                                  std::to_string(constructor) + "\n") !=
           std::string::npos;
  };
  expect(marker(ownerConstructor->id),
         "the ordinary Holder constructor should publish from verified MIR "
         "after its checked field construction is contained at the native "
         "boundary");
}

void testContainedAbstractBaseConstructor() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-body-contained-abstract-base.gti", R"(
#include <std/vector>

class Item {
public:
  Item() {}
};

class Base {
  mut std::unique_ptr<Item> item;

public:
  Base() : item(std::make_unique<Item>()) {}
  virtual void run() mut = 0;
};

class Derived : public Base {
  mut bool ready = false;

public:
  Derived() : Base() {}

  void run() mut override {
    this.ready = true;
  }
};

void instantiate() {
  Derived value{};
}
)");
  expect(frontend.canGenerateCode(),
         "the contained abstract-base fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::HirClassInstance *baseHir = nullptr;
  const lang::HirClassInstance *derivedHir = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source == nullptr) {
      continue;
    }
    if (instance.source->name().lexeme == "Base") {
      baseHir = &instance;
    } else if (instance.source->name().lexeme == "Derived") {
      derivedHir = &instance;
    }
  }
  expect(baseHir != nullptr && derivedHir != nullptr,
         "the fixture should retain both concrete HIR class instances");
  if (baseHir == nullptr || derivedHir == nullptr) {
    return;
  }

  const lang::MirClassInstance *base =
      frontend.mir.findClassInstance(baseHir->id);
  const lang::MirClassInstance *derived =
      frontend.mir.findClassInstance(derivedHir->id);
  expect(base != nullptr && derived != nullptr,
         "the fixture should retain both MIR class instances");
  if (base == nullptr || derived == nullptr) {
    return;
  }
  expect(base->abstract && base->polymorphic && base->requiresActiveCleanup,
         "the base should retain its abstract polymorphic cleanup metadata");
  expect(derived->bases.size() == 1 &&
             derived->bases.front().instance == base->id,
         "the derived class should retain its exact concrete base instance");

  const lang::MirConstructorInstance *baseConstructor = nullptr;
  const lang::MirConstructorInstance *derivedConstructor = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (constructor.owner == base->id) {
      baseConstructor = &constructor;
    } else if (constructor.owner == derived->id) {
      derivedConstructor = &constructor;
    }
  }
  expect(baseConstructor != nullptr && derivedConstructor != nullptr,
         "the fixture should retain both source constructor instances");
  if (baseConstructor == nullptr || derivedConstructor == nullptr) {
    return;
  }
  expect(baseConstructor->mayRaiseDefinedFailure &&
             derivedConstructor->mayRaiseDefinedFailure,
         "the owning base initializer failure effect should reach both "
         "constructors");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(
      emitter.supportsNativeContainedBaseConstruction(derivedConstructor->id),
      "the derived constructor should prove its exact native base "
      "construction from MIR");

  const lang::CppMirBodyEmissionText baseText = emitter.emitBodyText(
      {.kind = lang::MirBodyKind::Constructor, .owner = baseConstructor->id},
      "contained-abstract-base-v0", 1);
  const lang::CppMirBodyEmissionText derivedText = emitter.emitBodyText(
      {.kind = lang::MirBodyKind::Constructor, .owner = derivedConstructor->id},
      "contained-derived-v0", 1);
  expect(baseText.emitted(),
         "an abstract base's source constructor must remain directly "
         "spellable from MIR");
  expect(derivedText.emitted(),
         "the derived source constructor must remain directly spellable "
         "from MIR");
  expect(baseText.text.find("__gti_mir_boundary_failure_") != std::string::npos,
         "the base field call should contain its transformed failure at the "
         "ordinary constructor boundary");
}

// Sweeps the shipped example corpus through the generic body-emission gate.
//
// This exists because the readiness figure is otherwise measured only by
// ad-hoc tooling, and a change that breaks compilation silently "improves" it:
// a rejected source contributes no bodies at all. Asserting that every example
// still reaches MIR keeps that failure mode visible, and asserting that no
// frontend-produced body is Incoherent keeps the gate honest about the
// difference between "needs more MIR authority" and "this MIR is wrong".
void testExampleCorpusEmissionReadiness() {
  const std::filesystem::path repository =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::filesystem::path examples = repository / "examples";
  std::vector<std::filesystem::path> sources;
  if (std::filesystem::is_directory(examples)) {
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(examples)) {
      if (entry.is_regular_file() && entry.path().extension() == ".gti") {
        sources.push_back(entry.path());
      }
    }
  }
  std::sort(sources.begin(), sources.end());
  expect(!sources.empty(), "the example corpus should be discoverable");

  std::size_t rejected = 0;
  std::size_t bodies = 0;
  std::size_t ready = 0;
  std::size_t incoherent = 0;
  std::size_t invalidIssues = 0;
  for (const std::filesystem::path &source : sources) {
    std::ifstream input(source);
    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::filesystem::path standardLibrary = repository / "stdlib";
    const lang::FrontendResult frontend = lang::Frontend().analyze(
        source.filename().string(), buffer.str(),
        {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
    if (!frontend.canGenerateCode()) {
      ++rejected;
      std::cerr << "corpus source no longer reaches MIR: "
                << source.filename().string() << '\n';
      for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
        std::cerr << "  " << diagnostic.code << ": " << diagnostic.message
                  << '\n';
      }
      continue;
    }
    lang::CppMirBodyEmissionMapRows rows = completeRows(frontend.mir);
    const lang::CppMirBodyEmissionMapRows productionRows =
        buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23);
    rows.enums = productionRows.enums;
    for (const lang::CppMirEnumRepresentation &enumeration : rows.enums) {
      addType(rows, enumeration.underlyingType);
      for (const lang::CppMirPayloadVariantRepresentation &variant :
           enumeration.payloadVariants) {
        for (const lang::SemanticType &field : variant.fieldTypes) {
          addType(rows, field);
        }
      }
    }
    const lang::CppMirBodyEmissionMap map(std::move(rows));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    for (const lang::MirBodyAddress address :
         lang::enumerateMirBodyAddresses(frontend.mir)) {
      const lang::CppMirBodyEmissionAnalysis analysis =
          emitter.analyze(address);
      ++bodies;
      if (analysis.ready()) {
        ++ready;
      } else {
        std::cerr << "not-ready body in " << source.filename().string()
                  << " kind=" << static_cast<int>(address.kind)
                  << " owner=" << address.owner;
        if (analysis.issues.empty()) {
          std::cerr << ": <no issue>\n";
        } else {
          std::cerr << ":\n";
          for (const lang::CppMirBodyEmissionIssue &issue : analysis.issues) {
            std::cerr << "  " << static_cast<int>(issue.kind) << ": "
                      << issue.detail << '\n';
          }
        }
      }
      if (analysis.readiness == lang::CppMirBodyEmissionReadiness::Incoherent) {
        ++incoherent;
        std::cerr << "incoherent body in " << source.filename().string()
                  << " kind=" << static_cast<int>(address.kind)
                  << " owner=" << address.owner << ": "
                  << (analysis.issues.empty() ? std::string("<no issue>")
                                              : analysis.issues.front().detail)
                  << '\n';
      }
      for (const lang::CppMirBodyEmissionIssue &issue : analysis.issues) {
        switch (issue.kind) {
        case lang::CppMirBodyEmissionIssueKind::InvalidMirProgram:
        case lang::CppMirBodyEmissionIssueKind::InvalidBodyAddress:
        case lang::CppMirBodyEmissionIssueKind::InvalidRepresentationEnum:
        case lang::CppMirBodyEmissionIssueKind::InvalidRepresentationRow:
        case lang::CppMirBodyEmissionIssueKind::InvalidBodyKind:
        case lang::CppMirBodyEmissionIssueKind::InvalidInstructionKind:
        case lang::CppMirBodyEmissionIssueKind::InvalidOperation:
        case lang::CppMirBodyEmissionIssueKind::InvalidOperandKind:
        case lang::CppMirBodyEmissionIssueKind::InvalidPlaceRootKind:
        case lang::CppMirBodyEmissionIssueKind::InvalidProjectionKind:
        case lang::CppMirBodyEmissionIssueKind::InvalidTerminatorKind:
          ++invalidIssues;
          std::cerr << "invalid-shape issue in " << source.filename().string()
                    << ": " << issue.detail << '\n';
          break;
        default:
          break;
        }
      }
    }
  }

  std::cout << "example-corpus emission readiness: sources=" << sources.size()
            << " rejected=" << rejected << " bodies=" << bodies
            << " ready=" << ready << " incoherent=" << incoherent << '\n';

  expect(rejected == 0,
         "every shipped example should still reach verified MIR; a rejected "
         "source silently inflates every readiness figure");
  expect(incoherent == 0,
         "no frontend-produced body should be structurally incoherent to the "
         "generic emission gate");
  expect(invalidIssues == 0,
         "frontend-produced MIR should raise no invalid-shape emission issue");
  // A floor rather than an exact count: the gate protects against regression
  // without forcing an update on every representation improvement.
  expect(ready >= 1900,
         "generic emission readiness across the example corpus should not "
         "regress below its recorded floor");
}

// ADR 016 phases 4-5 agreement gate: the production rows builder plus the
// general text step must reproduce, byte for byte, every plain and
// failure-propagating scalar-cfg body the transitional emitter publishes, for
// every shipped example. The artifact's own markers select the bodies, so
// this gate tracks production admission without modeling the selectors.
void testGeneralTextStepMatchesProductionEmission() {
  const std::filesystem::path repository =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::filesystem::path examples = repository / "examples";
  std::vector<std::filesystem::path> sources;
  if (std::filesystem::is_directory(examples)) {
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(examples)) {
      if (entry.is_regular_file() && entry.path().extension() == ".gti") {
        sources.push_back(entry.path());
      }
    }
  }
  std::sort(sources.begin(), sources.end());
  expect(!sources.empty(), "the example corpus should be discoverable");

  const std::string_view markerPrefix = "// GTI verified-MIR body: ";
  std::size_t generalBodies = 0;
  std::size_t matchedBodies = 0;
  bool deterministic = true;
  bool intrinsicBodies = false;
  bool checkedIntrinsicBodies = false;
  bool loanBodies = false;
  for (const std::filesystem::path &source : sources) {
    std::ifstream input(source);
    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::filesystem::path standardLibrary = repository / "stdlib";
    const lang::FrontendResult frontend = lang::Frontend().analyze(
        source.filename().string(), buffer.str(),
        {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
    if (!frontend.canGenerateCode()) {
      continue;
    }
    const lang::OptimizationResult optimizations =
        lang::OptimizationPipeline().run(frontend.hir,
                                         lang::OptimizationLevel::O0);
    const lang::BackendArtifact artifact =
        gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);

    lang::CppMirBodyEmissionMapRows rows =
        buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23);
    const lang::CppMirBodyEmissionMapRows again =
        buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23);
    deterministic =
        deterministic && rows.types == again.types &&
        rows.bodies == again.bodies && rows.symbols == again.symbols &&
        rows.enums == again.enums && rows.capabilities == again.capabilities;

    const lang::CppMirBodyEmissionMap map(std::move(rows));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    const std::string &generated = artifact.contents;
    for (std::size_t position = generated.find(markerPrefix);
         position != std::string::npos;
         position = generated.find(markerPrefix, position + 1)) {
      const std::size_t labelStart = position + markerPrefix.size();
      const std::size_t labelEnd = generated.find(' ', labelStart);
      if (labelEnd == std::string::npos) {
        continue;
      }
      const std::string label =
          generated.substr(labelStart, labelEnd - labelStart);
      const bool failureForm = label == "scalar-cfg-failure-v1";
      if (label != "scalar-cfg-v1" && !failureForm) {
        continue;
      }
      const std::string_view instanceMarker = "function-instance ";
      const std::size_t instanceStart =
          generated.find(instanceMarker, labelEnd);
      const std::size_t lineEnd = generated.find('\n', labelEnd);
      if (instanceStart == std::string::npos || lineEnd == std::string::npos ||
          instanceStart > lineEnd) {
        continue;
      }
      ++generalBodies;
      const std::size_t instance = static_cast<std::size_t>(std::stoull(
          generated.substr(instanceStart + instanceMarker.size(),
                           lineEnd - instanceStart - instanceMarker.size())));
      const std::size_t lineStart = generated.rfind('\n', position) + 1;
      const std::size_t markerIndent = position - lineStart;
      if (markerIndent < 2 || markerIndent % 2 != 0) {
        continue;
      }
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = instance};
      const lang::CppMirBodyEmissionText emission =
          failureForm
              ? emitter.emitFailureBodyText(address, label,
                                            markerIndent / 2 - 1)
              : emitter.emitBodyText(address, label, markerIndent / 2 - 1);
      if (emission.text.find("::gti_internal::backend::wrapping_add(") !=
              std::string::npos ||
          emission.text.find("::gti_internal::backend::saturating_sub(") !=
              std::string::npos) {
        intrinsicBodies = true;
      }
      if (emission.text.find("__gti_mir_loan_") != std::string::npos &&
          (emission.text.find("return *__gti_mir_loan_") != std::string::npos ||
           emission.text.find("__gti_mir_out_result = __gti_mir_loan_") !=
               std::string::npos)) {
        loanBodies = true;
      }
      if (emission.text.find("checked_add") != std::string::npos ||
          emission.text.find("checked_sub") != std::string::npos ||
          emission.text.find("checked_mul") != std::string::npos) {
        checkedIntrinsicBodies = true;
      }
      if (emission.emitted() &&
          generated.find(emission.text) != std::string::npos) {
        ++matchedBodies;
      } else {
        std::cerr << "general text step disagreed for " << label
                  << " function-instance " << instance << " in "
                  << source.filename().string() << '\n';
      }
    }
  }

  std::cout << "general text-step agreement: bodies=" << generalBodies
            << " matched=" << matchedBodies << '\n';
  expect(loanBodies,
         "the corpus should emit at least one loan-erasure body (ADR 018): "
         "a pointer loan local, an address-of borrow, and a dereferenced "
         "loan return");
  expect(intrinsicBodies,
         "the corpus should emit at least one wrapping/saturating intrinsic "
         "helper call from verified MIR");
  expect(checkedIntrinsicBodies,
         "the corpus should emit at least one checked-result intrinsic "
         "helper call from verified MIR now that Expected sums joined the "
         "signature boundary (0.183.0)");
  expect(deterministic,
         "the production rows builder should be run-to-run deterministic");
  expect(generalBodies >= 30,
         "the corpus should keep exercising the migrated per-body families");
  expect(matchedBodies == generalBodies,
         "every production scalar-cfg body must byte-match the "
         "general text step's emission");
}

// The production rows now carry namespace-global storage names,
// constructor/destructor body names, and the sealed lifetime-slot helper,
// so every non-entry scalar function body of the class-default-cleanup
// fixture is analysis-Ready; its constructor and destructor bodies stay
// gated by the structural rollback/double-failure proofs, and the text
// vocabulary still declines Construct/Drop, so production emission of the
// family is unchanged until those land.
void testCleanupFixtureFunctionBodiesAreReady() {
  const std::filesystem::path fixture =
      std::filesystem::path(__FILE__).parent_path() / "fixtures" /
      "mir_backend_class_default_cleanup.gti";
  std::ifstream input(fixture);
  std::stringstream buffer;
  buffer << input.rdbuf();
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), buffer.str());
  expect(frontend.canGenerateCode(),
         "the class-default-cleanup fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  bool functionBodiesReady = true;
  std::size_t scalarFunctionBodies = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None ||
        function.mayRaiseDefinedFailure) {
      continue;
    }
    ++scalarFunctionBodies;
    const lang::CppMirBodyEmissionAnalysis analysis = emitter.analyze(
        {.kind = lang::MirBodyKind::Function, .owner = function.id});
    if (!analysis.ready()) {
      functionBodiesReady = false;
      std::cerr << "cleanup function-instance " << function.id
                << " is not ready: "
                << (analysis.issues.empty() ? std::string("<no issue>")
                                            : analysis.issues.front().detail)
                << '\n';
    }
  }
  expect(scalarFunctionBodies >= 3 && functionBodiesReady,
         "every non-entry failure-free cleanup-fixture function body should "
         "be analysis-Ready under production rows");

  // The destructor-definition route dissolved into the same per-body
  // admission: every failure-free declared destructor body is Ready and
  // inside the text vocabulary, and the checked-arithmetic near miss
  // (CheckedCleanup) stays outside admission.
  std::size_t admittedDestructorBodies = 0;
  std::size_t declinedDestructorBodies = 0;
  for (const lang::MirDestructorInstance &destructor :
       frontend.mir.destructorInstances()) {
    if (destructor.definitionKind != lang::MirDefinitionKind::Source) {
      continue;
    }
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Destructor,
                                       .owner = destructor.id};
    if (emitter.analyze(address).ready() && emitter.supportsBodyText(address)) {
      ++admittedDestructorBodies;
    } else {
      ++declinedDestructorBodies;
    }
  }
  expect(admittedDestructorBodies == 4 && declinedDestructorBodies == 1,
         "exactly the four ordinary declared destructor bodies should be "
         "admitted and the checked-arithmetic near miss declined");
}

// Milestone gate for the rollback campaign: every constructor and
// field-initializer body of the owned-lifecycle fixture is analysis-Ready
// under production rows, and the scalar user destructor summarizes
// provably failure-free since the effect-proof widening. Native-closing
// prelude destructors and the prelude function bodies legitimately keep
// their containment flags and withheld capability rows.
void testOwnedLifecycleConstructionBodiesReady() {
  const std::filesystem::path repository =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::filesystem::path fixture =
      repository / "tests" / "fixtures" / "mir_backend_owned_lifecycle.gti";
  const std::filesystem::path standardLibrary = repository / "stdlib";
  std::ifstream input(fixture);
  std::stringstream buffer;
  buffer << input.rdbuf();
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      fixture.string(), buffer.str(), {standardLibrary / "prelude.gti"}, {},
      {standardLibrary});
  expect(frontend.canGenerateCode(),
         "the owned-lifecycle fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::CppMirProgramEmissionAnalysis program = emitter.analyzeProgram();
  bool constructionReady = program.issues.empty();
  std::size_t constructionBodies = 0;
  std::size_t readyDestructors = 0;
  for (const lang::CppMirBodyEmissionAnalysis &body : program.bodies) {
    if (body.body.kind == lang::MirBodyKind::Destructor) {
      readyDestructors += body.ready() ? 1 : 0;
      continue;
    }
    if (body.body.kind != lang::MirBodyKind::Constructor &&
        body.body.kind != lang::MirBodyKind::FieldInitializers) {
      continue;
    }
    ++constructionBodies;
    if (!body.ready()) {
      constructionReady = false;
      std::cerr << "owned-lifecycle construction body kind="
                << static_cast<int>(body.body.kind)
                << " owner=" << body.body.owner << " not ready: "
                << (body.issues.empty() ? std::string("<no issue>")
                                        : body.issues.front().detail)
                << '\n';
    }
  }
  expect(constructionBodies >= 3 && constructionReady,
         "every owned-lifecycle constructor and field-initializer body must "
         "stay analysis-Ready under production rows");
  expect(readyDestructors >= 1,
         "the scalar user destructor should summarize provably failure-free "
         "and reach analysis-Ready; native-closing prelude destructors "
         "legitimately keep their containment flags");
}

// A prefix-storage read whose bounds proof the enclosing trusted container
// discharged (the logical-size check precedes it) records its site with no
// failure edge. The analysis accepts that shape instead of reporting
// missing checked-failure control flow; every OTHER checked operation
// still demands its exact Invoke successor.
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
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t dischargedReadBodies = 0;
  std::size_t checkedFlowIssues = 0;
  for (const lang::MirFunctionInstance &instance :
       frontend.mir.functionInstances()) {
    bool hasDischargedRead = false;
    for (const lang::MirBlock &block : instance.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call ||
            (instruction.intrinsic != lang::IntrinsicKind::PrefixStorageRead &&
             instruction.intrinsic !=
                 lang::IntrinsicKind::PrefixStorageReadMut) ||
            instruction.definedFailure.localOrigins.empty()) {
          continue;
        }
        bool invoked = false;
        for (const lang::MirBlock &owner : instance.body.blocks) {
          if (owner.terminator.kind == lang::MirTerminatorKind::Invoke &&
              owner.terminator.invokeInstruction == instruction.id &&
              owner.id == block.id) {
            invoked = true;
          }
        }
        if (!invoked) {
          hasDischargedRead = true;
        }
      }
    }
    if (!hasDischargedRead) {
      continue;
    }
    ++dischargedReadBodies;
    const lang::CppMirBodyEmissionAnalysis analysis = emitter.analyze(
        {.kind = lang::MirBodyKind::Function, .owner = instance.id});
    if (hasIssue(analysis, lang::CppMirBodyEmissionIssueKind::
                               MissingCheckedFailureControlFlow)) {
      ++checkedFlowIssues;
    }
  }
  expect(dischargedReadBodies > 0,
         "the vector fixture should reach at least one trusted body whose "
         "storage read is flow-discharged");
  expect(checkedFlowIssues == 0,
         "a flow-discharged storage read must not be reported as missing "
         "checked-failure control flow");
}

} // namespace

// The inline closure chain: lambda-typed places and values never declare
// (C++ closure types are unnameable), so the Closure compute fuses into
// its consuming invocations, which spell the full literal with capture
// names from the Capture rows and the recursively emitted verified body.
// Under the transformed parent convention each private generated literal
// also carries an out-result and failure-record channel, so its checked
// arithmetic preserves the enclosing frame's cleanup edge.
void testInlineClosureChainEmission() {
  const lang::FrontendResult frontend = analyze("cpp-mir-closure-chain.gti", R"(
int main() {
  int offset = 3;
  auto add_offset = [offset](int value) -> int {
    return offset + value;
  };
  auto copied = add_offset;
  int doubled = [](int value) -> int {
    return value * 2;
  }(4);
  return copied(doubled) - 11;
}
)");
  expect(frontend.canGenerateCode(),
         "the closure-chain fixture should pass the frontend");
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
         "the closure-chain fixture should retain its entry instance");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready(),
         "the closure-chain entry should be analysis-Ready under production "
         "rows");
  expect(emitter.supportsFailureBodyText(address),
         "the closure-chain entry should prove its failure-form text");
  for (const lang::MirLambdaInstance &lambda : frontend.mir.lambdaInstances()) {
    expect(emitter.supportsBodyText(
               {.kind = lang::MirBodyKind::Lambda, .owner = lambda.id}),
           "each lambda body should prove its plain-shape nested text");
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "closure-test-v0", 1);
  const auto contains = [&](std::string_view needle) {
    return text.text.find(needle) != std::string::npos;
  };
  expect(contains("// GTI verified-MIR body: closure-test-v0 "
                  "lambda-instance 1") &&
             contains("// GTI verified-MIR body: closure-test-v0 "
                      "lambda-instance 2"),
         "both lambda bodies should emit nested banners inside the entry");
  expect(contains("[offset = __gti_mir_p_"),
         "the capture should spell its Capture row name over the enclosing "
         "place expression");
  expect(contains("mir_checked_multiply_v1") &&
             contains("mir_checked_add_v1") &&
             contains("__gti_mir_call_success_"),
         "checked arithmetic inside transformed literals should forward "
         "status through the enclosing call edge");
  expect(contains("__gti_mir_out_result") &&
             contains("__gti_mir_failure_record"),
         "the generated literal signature should carry the transformed "
         "result and failure channels");
  expect(contains("rejoins the fused closure chain") &&
             contains("joins the fused chain") &&
             contains("spells at its consuming invocation") &&
             !contains(".construct("),
         "the fused chain instructions should spell as comments only and "
         "must not materialize an omitted lambda-typed slot");
}

// Fusing the literal to a later invocation is sound only while every
// captured place stays frozen after the Closure. When a capture is rewritten,
// the closure must instead materialize at the exact Closure step so it keeps
// value-capture snapshot semantics.
void testClosureCaptureFreezeMaterializes() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-closure-freeze.gti", R"(
int main() {
  mut int x = 1;
  auto f = [x](int value) -> int {
    return x + value;
  };
  x = 2;
  return f(0) - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the capture-freeze fixture should pass the frontend");
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
         "the capture-freeze fixture should retain its entry instance");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(!emitter.supportsBodyText(address) &&
             emitter.supportsFailureBodyText(address),
         "a rewritten capture should select materialized failure-form "
         "closure emission");
  const std::string text =
      emitter.emitFailureBodyText(address, "closure-freeze-test-v0", 1).text;
  const std::size_t slotType =
      text.find("mir_lifetime_slot<__gti_mir_closure_type_");
  const std::size_t slotNameAt = slotType == std::string::npos
                                     ? std::string::npos
                                     : text.find("__gti_mir_p_", slotType);
  const std::size_t slotNameEnd = slotNameAt == std::string::npos
                                      ? std::string::npos
                                      : text.find(';', slotNameAt);
  const std::string slot =
      slotNameEnd == std::string::npos
          ? std::string{}
          : text.substr(slotNameAt, slotNameEnd - slotNameAt);
  const std::size_t construction =
      slot.empty() ? std::string::npos
                   : text.find(slot + ".construct_from", slotNameEnd);
  const std::size_t rewrite = text.find("static_cast<std::int32_t>(2)");
  const std::size_t invocation = slot.empty() || rewrite == std::string::npos
                                     ? std::string::npos
                                     : text.find(slot + ".get()(", rewrite);
  expect(text.find("__gti_mir_closure_factory_") != std::string::npos &&
             text.find("using __gti_mir_closure_type_") != std::string::npos &&
             text.find("was constructed at its Closure step") !=
                 std::string::npos &&
             text.find("aliases the materialized closure slot") !=
                 std::string::npos &&
             text.find("joins the fused chain") == std::string::npos &&
             text.find("rejoins the fused closure chain") ==
                 std::string::npos &&
             construction != std::string::npos &&
             rewrite != std::string::npos && invocation != std::string::npos &&
             construction < rewrite && rewrite < invocation,
         "a rewritten capture should snapshot into a materialized slot before "
         "the source place changes and invoke that slot afterward");
}

// A class capture has lexical cleanup even when it is copied. MIR publishes
// that closure lifetime at the destination Initialize step; it does not
// duplicate an Initialize event on the Closure instruction. Passing the
// stored closure through a deduced-callable template must preserve that exact
// schedule through production failure-form emission.
void testLexicalCopyCaptureThroughCallableTemplate() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-lexical-copy-capture.gti", R"(
struct Scalar {
  int value = 0;
};

int inspect_transfer<std::transferable T>(T value) {
  return value();
}

int main() {
  Scalar scalar = Scalar();
  auto callable = [scalar]() -> int { return 1; };
  int result = inspect_transfer(callable);
  return result - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the lexical copy-capture fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirFunctionInstance *callableTemplate = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (!function.callableParameters.empty()) {
      callableTemplate = &function;
    }
  }
  expect(entry != nullptr && callableTemplate != nullptr,
         "the lexical copy-capture fixture should retain its entry and "
         "callable-template instances");
  if (entry == nullptr || callableTemplate == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = entry->id};
  expect(emitter.analyze(entryAddress).ready() &&
             emitter.supportsFailureBodyText(entryAddress),
         "a lexical class copy-capture should materialize from its "
         "destination Reparent schedule");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto productionMarker = [&](lang::HirFunctionInstanceId id) {
    return artifact.contents.find("function-instance " + std::to_string(id) +
                                  "\n") != std::string::npos;
  };
  expect(productionMarker(entry->id) &&
             productionMarker(callableTemplate->id) &&
             artifact.contents.find("__gti_mir_closure_factory_") !=
                 std::string::npos,
         "production emission should publish both the materialized caller and "
         "its callable-template body from verified MIR");
}

// A generated default construction can publish without first becoming a
// source binding. A by-value generic call uses its prepared-parameter slot;
// Expected<T, E> constructs its destination directly from T{}. Each route is
// admitted only by its recorded Initialize/TransferOut schedule.
void testDefaultConstructionPreparedParameterStaging() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-default-prepared-parameter.gti", R"(
struct Scalar {
  int value = 0;
};

T transfer_value<T>(T value) {
  return std::move(value);
}

int main() {
  Scalar scalar = transfer_value(Scalar());
  expected<Scalar, int> result = Scalar();
  return scalar.value;
}
)");
  expect(frontend.canGenerateCode(),
         "the default prepared-parameter fixture should pass the frontend");
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
         "the default prepared-parameter fixture should retain its entry");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a targetless default construction should publish through its exact "
         "prepared-parameter lifecycle");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("function-instance " +
                                std::to_string(entry->id) + "\n") !=
                 std::string::npos &&
             artifact.contents.find(".construct();") != std::string::npos &&
             artifact.contents.find("stages a placement class result") !=
                 std::string::npos &&
             artifact.contents.find("publishes at its Expected initializer") !=
                 std::string::npos &&
             artifact.contents.find(".construct(::__gti_program::Scalar{});") !=
                 std::string::npos,
         "production emission should publish targetless default values "
         "directly through both verified destination schedules");
}

// A generic member on a non-generic class cannot use the generic-owner
// explicit-specialization spelling. Its transformed ABI is one concrete
// overload declared on the class and defined without a template<> prefix.
void testGenericMemberFailureOverloadOnOrdinaryOwner() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-generic-member-ordinary-owner.gti", R"(
class Total {
public:
  int add<uint64_t N>(int values[N]) {
    return values[0];
  }
};

int main() {
  Total total = Total();
  return total.add({7}) - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the generic-member ordinary-owner fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirFunctionInstance *member = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (function.owner && !function.parameterTypes.empty() &&
        function.parameterTypes.front().kind == lang::SemanticType::Array) {
      member = &function;
    }
  }
  expect(entry != nullptr && member != nullptr,
         "the generic-member fixture should retain its entry and concrete "
         "member instance");
  if (entry == nullptr || member == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  for (const lang::MirFunctionInstance *function : {entry, member}) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function->id};
    expect(emitter.analyze(address).ready() &&
               emitter.supportsFailureBodyText(address),
           "the generic member and its caller should support transformed MIR "
           "emission");
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const std::string transformedDefinition =
      "bool __gti_program::Total::__gti_fn_" +
      std::to_string(member->declaration) + "_add__gti_mir_failure(";
  expect(text.find("function-instance " + std::to_string(entry->id) + "\n") !=
                 std::string::npos &&
             text.find("function-instance " + std::to_string(member->id) +
                       "\n") != std::string::npos &&
             text.find(transformedDefinition) != std::string::npos &&
             text.find("template <> " + transformedDefinition) ==
                 std::string::npos,
         "production emission should define the concrete transformed member "
         "overload with ordinary-owner C++ syntax");
}

// The deduced-callable template vocabulary (task: CallableDispatch): a
// Function body with callable parameters uses a transformed template shape:
// its callable parameter place declares only under an overlay type row, and
// invoking it returns a success flag while forwarding the current failure
// record. The caller passes a transformed fused lambda with the same private
// convention. Without the overlay row the body stays outside the vocabulary.
void testCallableTemplateBodyVocabulary() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-callable-template.gti", R"(
void invoke<Operation>(Operation operation) {
  operation(42);
}

int main() {
  auto report = [](int value) -> void {
    if (value == 42) {
      return;
    }
  };
  invoke(report);
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the callable-template fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *invoke = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (!function.callableParameters.empty()) {
      invoke = &function;
    }
  }
  expect(invoke != nullptr && !invoke->parameterTypes.empty(),
         "the fixture should lower the callable-parameter instance");
  if (invoke == nullptr || invoke->parameterTypes.empty()) {
    return;
  }
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = invoke->id};
  lang::CppMirBodyEmissionMapRows production =
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23);
  {
    lang::CppMirBodyEmissionMapRows copy = production;
    const lang::CppMirBodyEmissionMap withoutOverlay{std::move(copy)};
    const lang::CppMirBodyEmitter emitter(frontend.mir, withoutOverlay);
    expect(!emitter.supportsBodyText(address) &&
               !emitter.supportsFailureBodyText(address),
           "without the overlay row the callable-template body must stay "
           "outside the vocabulary");
  }
  const std::optional<lang::CppMirTypeRepresentationKind> callableKind =
      lang::cppMirExpectedTypeRepresentation(invoke->parameterTypes.front());
  expect(callableKind.has_value(),
         "the callable type should classify for representation");
  if (!callableKind) {
    return;
  }
  production.types.push_back({.type = invoke->parameterTypes.front(),
                              .kind = *callableKind,
                              .spelling = "Operation"});
  const lang::CppMirBodyEmissionMap map(std::move(production));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(!emitter.supportsBodyText(address) &&
             emitter.supportsFailureBodyText(address),
         "under the overlay row the callable-template body should prove "
         "its transformed text");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "callable-template-test-v0", 1);
  const auto contains = [&](std::string_view needle) {
    return text.text.find(needle) != std::string::npos;
  };
  expect(contains("Operation __gti_mir_p_1 = __gti_mir_arg_0;"),
         "the callable parameter local should declare with the template "
         "parameter spelling");
  expect(contains("__gti_mir_call_success_") &&
             contains("__gti_mir_p_1(__gti_mir_v_") &&
             contains("__gti_mir_failure_record"),
         "the invocation should stage the callable parameter place "
         "directly through the transformed channel");
  expect(contains("std::abort();"),
         "the unreachable propagate block should spell abort");
}

// An explicitly moved callable receiver may resolve to a mutable operator()
// when no consuming overload has the requested signature. MIR records both
// facts independently: MoveValue transports the receiver, while the selected
// callable capability names the exact overload. The C++ body emitter must not
// collapse those facts into an equality check.
void testMovedCallableReceiverMutableFallback() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-moved-callable-fallback.gti", R"(
class fallback_counter {
public:
  int operator()(int value) mut {
    return value + 1;
  }

  int operator()() && {
    return 0;
  }
};

int invoke_once<Operation>(Operation operation) {
  return std::move(operation)(7);
}

int main() {
  fallback_counter operation{};
  return invoke_once(operation) - 8;
}
)");
  expect(frontend.canGenerateCode(),
         "the moved callable fallback fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *fallback = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (std::any_of(function.callableParameters.begin(),
                    function.callableParameters.end(),
                    [](const lang::MirCallableParameter &parameter) {
                      return std::any_of(
                          parameter.signatures.begin(),
                          parameter.signatures.end(),
                          [](const lang::MirCallableSignature &signature) {
                            return signature.selectedCapability ==
                                   lang::CallableInvocationCapability::Mutable;
                          });
                    })) {
      fallback = &function;
    }
  }
  expect(fallback != nullptr && entry != nullptr,
         "the fixture should retain the mutable fallback and entry bodies");
  if (fallback == nullptr || entry == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = fallback->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "MoveValue transport should admit the resolved mutable callable "
         "fallback");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const std::string body =
      emitter.emitFailureBodyText(address, "moved-fallback-test-v0", 1).text;
  expect(body.find("std::move(") != std::string::npos &&
             body.find("__gti_mir_call_success_") != std::string::npos &&
             body.find("std::move(__gti_mir_p_") == std::string::npos &&
             body.find(".get().") != std::string::npos,
         "the fallback body should transfer its parameter into the MIR slot "
         "but invoke the selected mutable overload on an lvalue receiver");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto productionMarker = [&](lang::HirFunctionInstanceId id) {
    return artifact.contents.find("// GTI verified-MIR body: ") !=
               std::string::npos &&
           artifact.contents.find("function-instance " + std::to_string(id) +
                                  "\n") != std::string::npos;
  };
  expect(productionMarker(fallback->id) && productionMarker(entry->id),
         "production emission should select both the fallback body and its "
         "entry caller from MIR");
}

// The declaration-level template route end to end: every monomorphized
// instance of a deduced-callable template proves byte-identical text
// under its own overlay row, and the production artifact carries exactly
// one template definition holding one banner per covered instance, with
// callers passing fused literals by deduction.
void testDeducedCallableTemplateEmission() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-deduced-template.gti", R"(
void apply<Operation>(Operation operation) {
  operation(1);
}

int main() {
  auto first = [](int value) -> void {
    if (value == 1) {
      return;
    }
  };
  auto second = [](int value) -> void {
    if (value == 2) {
      return;
    }
  };
  apply(first);
  apply(second);
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the deduced-template fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const auto count = [&](std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };
  expect(count("(Operation __gti_mir_arg_0, ::gti_failure_record_v1 "
               "*__gti_mir_failure_record) {") == 1,
         "exactly one template definition should carry the MIR body");
  expect(count("deduced-callable-failure-v1 function-instance") == 2,
         "the single template body should carry one banner per covered "
         "instance");
  expect(count("// GTI verified-MIR body: scalar-cfg-failure-v1 "
               "lambda-instance") == 2,
         "both fused literals should embed their verified lambda bodies at "
         "the call sites");
}

// The reference-return failure ABI (ADR 018 §5): a may-raise body whose
// return is a loan publishes its pointer through a `T **` out-parameter,
// the Return-with-loan spells the publication, and the whole artifact —
// transformed member and MIR-emitted caller carry the convention end to end;
// no compatibility wrapper may intercept failure ahead of caller cleanup.
void testReferenceReturnFailureAbi() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-reference-return.gti", R"(
class tally {
public:
  mut int total = 3;

  mut int& bump(int amount) mut {
    this.total += amount;
    return this.total;
  }
};

int main() {
  mut tally counter = tally();
  mut int& first = counter.bump(4);
  first = 17;
  return counter.bump(0) - 17;
}
)");
  expect(frontend.canGenerateCode(),
         "the reference-return fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *bump = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.returnType.kind == lang::SemanticType::Reference &&
        function.mayRaiseDefinedFailure) {
      bump = &function;
    }
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
  }
  expect(bump != nullptr && entry != nullptr,
         "the fixture should lower the loan-returning member and its caller");
  if (bump == nullptr || entry == nullptr) {
    return;
  }
  const lang::MirPlace *referenceCarrier = nullptr;
  for (const lang::MirPlace &place : entry->body.places) {
    const bool parameter =
        std::find(entry->parameterBindings.begin(),
                  entry->parameterBindings.end(),
                  place.binding) != entry->parameterBindings.end();
    if (!parameter && place.root == lang::MirPlaceRootKind::Binding &&
        place.projections.empty() &&
        place.type.kind == lang::SemanticType::Reference) {
      referenceCarrier = &place;
    }
  }
  const lang::MirLoan *retained = nullptr;
  std::size_t carrierLoans = 0;
  if (referenceCarrier != nullptr) {
    for (const lang::MirLoan &loan : entry->body.loans) {
      if (std::find(loan.carriers.begin(), loan.carriers.end(),
                    referenceCarrier->binding) != loan.carriers.end()) {
        ++carrierLoans;
        retained = &loan;
      }
    }
  }
  bool initializesFromRetainedLoan = false;
  if (referenceCarrier != nullptr && retained != nullptr) {
    for (const lang::MirBlock &block : entry->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        initializesFromRetainedLoan |=
            instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.destination == referenceCarrier->id &&
            instruction.operands.size() == 1 &&
            instruction.operands.front().kind == lang::MirOperandKind::Loan &&
            instruction.operands.front().loan == retained->id;
      }
    }
  }
  expect(referenceCarrier != nullptr && carrierLoans == 1 &&
             retained != nullptr &&
             retained->kind == lang::MirLoanKind::CallResult &&
             retained->parent == 0 && retained->semanticLoan != 0 &&
             initializesFromRetainedLoan,
         "a retained root call result should preserve its exact loan pointer "
         "as the local reference carrier");
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = bump->id};
  expect(emitter.supportsFailureBodyText(address),
         "the loan-returning body should prove its transformed text");
  const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = entry->id};
  expect(emitter.supportsFailureBodyText(entryAddress),
         "the retained-reference caller should prove its transformed text");
  if (!emitter.supportsFailureBodyText(address) ||
      !emitter.supportsFailureBodyText(entryAddress)) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const auto contains = [&](std::string_view needle) {
    return text.find(needle) != std::string::npos;
  };
  expect(contains("std::int32_t **__gti_mir_out_result"),
         "the transformed signature should carry the pointer out-parameter");
  expect(contains("*__gti_mir_out_result = __gti_mir_loan_"),
         "the Return-with-loan should publish through the out-parameter");
  expect(contains("std::int32_t *__gti_mir_p_") &&
             contains(" = __gti_mir_loan_") && contains("&__gti_mir_loan_"),
         "the caller should retain the published pointer in its reference "
         "carrier");
  expect(!contains("__gti_mir_boundary_result"),
         "the direct transformed call must not retain a terminal boundary "
         "wrapper");
}

void testGenericOwnerReferenceReturnFailureAbi() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-generic-reference-return.gti", R"(
class box<T> {
  T value;

public:
  box(T initial) : value(initial) {}

  T& get() {
    return this.value;
  }
};

int main() {
  box<int> value{7};
  return value.get() - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the generic reference-return fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *getter = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.returnType.kind == lang::SemanticType::Reference) {
      getter = &function;
    }
    if (function.entryKind == lang::ProgramEntryKind::NoArguments) {
      entry = &function;
    }
  }
  expect(getter != nullptr && entry != nullptr,
         "the fixture should lower the generic member and hosted entry");
  if (getter == nullptr || entry == nullptr) {
    return;
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto contains = [&](std::string_view needle) {
    return artifact.contents.find(needle) != std::string::npos;
  };
  expect(contains("const T **__gti_mir_out_result") &&
             contains("const std::int32_t **__gti_mir_out_result"),
         "the primary generic member and its concrete specialization should "
         "publish references through pointer carriers");
  expect(contains("scalar-cfg-failure-v1 function-instance " +
                  std::to_string(getter->id)) &&
             contains("scalar-cfg-failure-v1 function-instance " +
                      std::to_string(entry->id)),
         "the generic reference-return chain should remain closed through "
         "the hosted entry");
}

// The stores-reference constructor schedule (ADR 018): each reference
// field pairs bijectively with one Stored loan whose source is the
// dereference carrier of a reference parameter; the emitted C++ binds the
// field in the member initializer list, the Borrow spells as a comment,
// and no loan pointer materializes. A nonzero constructor borrow origin
// is that schedule's caller-side lifetime fact.
void testStoredReferenceConstructorSchedule() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-stored-reference.gti", R"(
class holder {
  int& target;

public:
  holder(int& value) : target(value) {}

  int read() { return this.target; }
};

int main() {
  mut int cell = 42;
  holder bound = holder(cell);
  return bound.read() - 42;
}
)");
  expect(frontend.canGenerateCode(),
         "the stored-reference fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *constructor = nullptr;
  for (const lang::MirConstructorInstance &candidate :
       frontend.mir.constructorInstances()) {
    if (!candidate.body.loans.empty()) {
      constructor = &candidate;
    }
  }
  expect(constructor != nullptr,
         "the fixture should lower the loan-carrying constructor");
  if (constructor == nullptr) {
    return;
  }
  const std::optional<std::vector<lang::CppMirStoredReferenceBinding>>
      bindings = lang::cppMirStoredReferenceBindings(*constructor);
  expect(bindings.has_value() && bindings->size() == 1 &&
             (*bindings)[0].parameter == 0,
         "the schedule should pair the reference field with the reference "
         "parameter");
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                     .owner = constructor->id};
  expect(emitter.analyze(address).ready() && emitter.supportsBodyText(address),
         "the stored-reference constructor should be ready and in the text "
         "vocabulary");
  if (!emitter.supportsBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitBodyText(address, "stored-reference-test-v0", 1);
  expect(text.text.find("binds its reference field in the initializer "
                        "list") != std::string::npos &&
             text.text.find("__gti_mir_loan_") == std::string::npos,
         "the Borrow should spell as a comment with no loan pointer local");

  // The full backend chain binds the field in the emitted member
  // initializer list of the selected constructor definition.
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find(" : target(__gti_mir_arg_0) ") !=
             std::string::npos,
         "the emitted constructor should bind the reference field in its "
         "member initializer list");
  expect(artifact.contents.find("constructor-instance " +
                                std::to_string(constructor->id)) !=
             std::string::npos,
         "the selected constructor should carry its verified-MIR banner");
}

// A constructor of a generic class publishes per admitted concrete
// instance as an explicit specialization carrying the same verified
// schedule — declaration after the class, definition beside the deferred
// template, stores-reference initializer list included.
void testGenericOwnerConstructorSpecialization() {
  const lang::FrontendResult frontend = analyze("cpp-mir-generic-ctor.gti", R"(
class wrapper<T> {
  T& target;

public:
  wrapper(T& value) : target(value) {}

  T read() { return this.target; }
};

int main() {
  mut int cell = 7;
  wrapper<int> bound = wrapper<int>(cell);
  return bound.read() - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the generic-owner fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto contains = [&](std::string_view needle) {
    return artifact.contents.find(needle) != std::string::npos;
  };
  expect(contains("template <> __gti_program::wrapper<std::int32_t>::"
                  "wrapper(const std::int32_t & __gti_mir_arg_0);"),
         "the specialization should declare after the class definition");
  expect(contains(" : target(__gti_mir_arg_0) "),
         "the specialization should bind the reference field in its member "
         "initializer list");
}

// Generic source constructors and generic members use concrete private
// overloads for their transformed ABI. The source templates remain available
// for compatibility emission, while each admitted MIR instance owns one
// independently nameable definition.
void testConcreteGenericConstructorAndMemberPackPublication() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-concrete-generic-overloads.gti", R"(
#include <std/vector>

class extent {
  uint64_t count;

public:
  extent<uint64_t N>(int32_t values[N]) : count(N) {}

  uint64_t size() { return this.count; }
};

int32_t main() {
  extent fixed = extent({1, 2});
  mut std::vector<int32_t> values{};
  [[discard]] values.emplace_back(4);
  return int32_t(fixed.size()) + values[std::size_t(0)] - 6;
}
)");
  expect(frontend.canGenerateCode(),
         "the concrete generic overload fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirConstructorInstance *genericConstructor = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    const lang::HirConstructorInstance *source =
        frontend.hir.findConstructorInstance(constructor.id);
    if (source != nullptr && source->source != nullptr &&
        !source->source->genericParameters().empty()) {
      genericConstructor = &constructor;
      break;
    }
  }
  const lang::MirFunctionInstance *genericMember = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    const lang::HirFunctionInstance *source =
        frontend.hir.findFunctionInstance(function.id);
    if (source != nullptr && source->source != nullptr && function.owner &&
        std::any_of(source->source->genericParameters().begin(),
                    source->source->genericParameters().end(),
                    [](const lang::GenericParameter &parameter) {
                      return parameter.pack;
                    })) {
      genericMember = &function;
    }
  }
  expect(genericConstructor != nullptr && genericMember != nullptr &&
             entry != nullptr,
         "the fixture should retain concrete generic constructor, member-pack, "
         "and entry bodies");
  if (genericConstructor == nullptr || genericMember == nullptr ||
      entry == nullptr) {
    return;
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto containsMarker = [&](std::string_view kind, std::size_t id) {
    return artifact.contents.find(std::string(kind) + " " + std::to_string(id) +
                                  "\n") != std::string::npos;
  };
  expect(containsMarker("constructor-instance", genericConstructor->id) &&
             containsMarker("function-instance", genericMember->id) &&
             containsMarker("function-instance", entry->id),
         "the concrete generic overloads and their entry caller should all "
         "publish verified-MIR bodies");
  expect(artifact.contents.find("mir_failure_constructor_tag_v1<" +
                                std::to_string(genericConstructor->id) + ">") !=
             std::string::npos,
         "the generic constructor should publish through its unique private "
         "tag overload");
}

void testPassiveConstructorFailureSummary() {
  const lang::FrontendResult frontend = analyze("cpp-mir-passive-ctor.gti", R"(
class counter {
  mut int value;

public:
  counter(int initial) : value(initial) {}

  int tick() mut {
    this.value += 1;
    return this.value;
  }
};

int main() {
  mut counter value{0};
  return value.tick() - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the passive-constructor fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *constructor =
      frontend.mir.constructorInstances().empty()
          ? nullptr
          : &frontend.mir.constructorInstances().front();
  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  const lang::MirDefinedFailureEffects effects =
      lang::deriveMirDefinedFailureEffects(frontend.mir);
  expect(constructor != nullptr && !constructor->mayRaiseDefinedFailure &&
             constructor->id <= effects.constructors.size() &&
             !effects.constructors[constructor->id - 1],
         "a constructor covering trivial scalar fields from matching scalar "
         "parameters should have an exact failure-free MIR summary");
  expect(entry != frontend.mir.functionInstances().end(),
         "the fixture should lower its no-argument entry");
  if (constructor == nullptr ||
      entry == frontend.mir.functionInstances().end()) {
    return;
  }
  bool exactConstruction = false;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      exactConstruction =
          exactConstruction ||
          (instruction.kind == lang::MirInstructionKind::Construct &&
           instruction.constructorTarget == constructor->id &&
           instruction.definedFailure.empty() &&
           instruction.localFailureSites.empty());
    }
  }
  expect(exactConstruction,
         "a call to the proved constructor should lower without a failure "
         "edge in its caller");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("constructor-instance " +
                                std::to_string(constructor->id)) !=
                 std::string::npos &&
             artifact.contents.find("function-instance " +
                                    std::to_string(entry->id)) !=
                 std::string::npos,
         "the proved constructor and its failure-capable caller should both "
         "publish from verified MIR");
}

void testPassiveFixedArrayFailureClosure() {
  const lang::FrontendResult frontend = analyze("cpp-mir-fixed-array.gti", R"(
int32_t sum(int32_t values[4]) {
  mut int32_t total = 0;
  for (mut int32_t index = 0; index < 4; index++) {
    total += values[index];
  }
  return total;
}

class pair<T> {
  T values[2];

public:
  pair(T left, T right) : values({left, right}) {}

  T first() { return this.values[0]; }
};

int32_t main() {
  int32_t values[4] = {1, 2, 3, 4};
  int32_t grid[2][2] = {{1, 2}, {3, 4}};
  pair<int32_t> value = pair<int32_t>(7, 8);
  return sum(values) + grid[1][0] + value.first() - 20;
}
)");
  expect(frontend.canGenerateCode(),
         "the passive fixed-array fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  expect(frontend.mir.constructorInstances().size() == 1,
         "the fixed-array fixture should lower one constructor");
  if (frontend.mir.constructorInstances().size() != 1) {
    return;
  }
  const lang::MirConstructorInstance &constructor =
      frontend.mir.constructorInstances().front();
  const lang::MirDefinedFailureEffects effects =
      lang::deriveMirDefinedFailureEffects(frontend.mir);
  expect(!constructor.mayRaiseDefinedFailure &&
             constructor.id <= effects.constructors.size() &&
             !effects.constructors[constructor.id - 1],
         "a complete passive fixed-array initializer should be proved "
         "failure-free");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto contains = [&](std::string_view needle) {
    return artifact.contents.find(needle) != std::string::npos;
  };
  expect(contains("scalar-cfg-v1 constructor-instance " +
                  std::to_string(constructor.id)),
         "the passive fixed-array constructor should publish from verified "
         "MIR");
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    expect(contains("scalar-cfg-failure-v1 function-instance " +
                    std::to_string(function.id)),
           "every function in the fixed-array failure component should use "
           "the transformed ABI");
  }
  expect(contains("[static_cast<std::size_t>(1)], UINT64_C(0), "
                  "&__gti_mir_v_"),
         "a nested array read should check the terminal index over its "
         "proved-safe parent projection");
  expect(contains("mir_checked_array_read_v1((*this).values, UINT64_C(0), "
                  "&__gti_mir_v_"),
         "a receiver fixed-array field read should use the checked failure "
         "operation");
}

void testOwningFixedArrayAggregateAndCheckedMove() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-owning-fixed-array.gti", R"(
struct record {
  int32_t value;
  record(int32_t initial) : value(initial) {}
};

int32_t partial_drop() {
  mut std::unique_ptr<record> owners[2] = {
      std::make_unique<record>(7), std::make_unique<record>(8)};
  auto first = std::move(owners[0]);
  return first->value;
}

int32_t main() {
  return partial_drop() == 7 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning fixed-array fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  const lang::MirInstruction *checkedMove = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    bool owningAggregate = false;
    const lang::MirInstruction *candidateMove = nullptr;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        owningAggregate =
            owningAggregate ||
            (instruction.kind == lang::MirInstructionKind::Compute &&
             instruction.operation == lang::MirOperation::Aggregate &&
             instruction.info.type.kind == lang::SemanticType::Array &&
             instruction.info.type.arguments.size() == 1 &&
             instruction.info.type.arguments.front().kind ==
                 lang::SemanticType::Class);
        if (instruction.kind == lang::MirInstructionKind::Move &&
            instruction.localFailureSites.size() == 1) {
          candidateMove = &instruction;
        }
      }
    }
    if (owningAggregate && candidateMove != nullptr) {
      selected = &function;
      checkedMove = candidateMove;
      break;
    }
  }
  expect(selected != nullptr && checkedMove != nullptr,
         "the owning fixed-array fixture should retain its aggregate and "
         "checked element move in MIR");
  if (selected == nullptr || checkedMove == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the owning aggregate and checked move should support transformed "
         "MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "owning-fixed-array-test-v0", 1)
          .text;
  expect(text.find("mir_lifetime_slot<") != std::string::npos &&
             text.find(".construction_address()") != std::string::npos &&
             text.find("std::array<") != std::string::npos &&
             text.find("std::move(__gti_mir_v_") != std::string::npos &&
             text.find("::gti_internal::backend::array_at(") !=
                 std::string::npos &&
             text.find("failure cleanup drop-obligation") != std::string::npos,
         "the transformed body should stage both owners in exact lifetime "
         "slots, roll back a partial aggregate, and bounds-check the move");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(selected->id) + "\n") !=
             std::string::npos,
         "production emission should select the owning fixed-array body from "
         "verified MIR");
}

void testPassiveClassArrayMoveAssignmentSchedule() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-passive-class-array-move.gti", R"(
struct record {
  int32_t value;
  record(int32_t initial) : value(initial) {}
};

int32_t main() {
  mut record values[2] = {record(1), record(2)};
  record first = std::move(values[0]);
  record second = std::move(values[1]);
  values[0] = std::move(second);
  values[1] = std::move(first);
  if (values[0].value == 2 and values[1].value == 1) {
    return 0;
  }
  return 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the passive class-array move fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  const lang::MirInstruction *aggregate = nullptr;
  std::vector<const lang::MirInstruction *> assignmentMoves;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const lang::MirInstruction *candidateAggregate = nullptr;
    std::vector<const lang::MirInstruction *> candidateMoves;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const lang::MirInstruction &instruction = block.instructions[index];
        if (instruction.kind == lang::MirInstructionKind::Compute &&
            instruction.operation == lang::MirOperation::Aggregate &&
            instruction.info.type.kind == lang::SemanticType::Array &&
            instruction.info.type.arguments.size() == 1 &&
            instruction.info.type.arguments.front().kind ==
                lang::SemanticType::Class) {
          candidateAggregate = &instruction;
        }
        if (index + 1 >= block.instructions.size() ||
            instruction.kind != lang::MirInstructionKind::Move ||
            !instruction.result) {
          continue;
        }
        const lang::MirInstruction &consumer = block.instructions[index + 1];
        const lang::MirPlace *destination =
            consumer.destination
                ? function.body.findPlace(*consumer.destination)
                : nullptr;
        if (consumer.kind == lang::MirInstructionKind::Assign &&
            consumer.operands.size() == 1 &&
            consumer.operands.front().kind == lang::MirOperandKind::Value &&
            consumer.operands.front().value == *instruction.result &&
            destination != nullptr &&
            std::any_of(destination->projections.begin(),
                        destination->projections.end(),
                        [](const lang::MirPlaceProjection &projection) {
                          return projection.kind ==
                                 lang::MirProjectionKind::Index;
                        })) {
          candidateMoves.push_back(&instruction);
        }
      }
    }
    if (candidateAggregate != nullptr && candidateMoves.size() == 2) {
      selected = &function;
      aggregate = candidateAggregate;
      assignmentMoves = std::move(candidateMoves);
      break;
    }
  }
  expect(selected != nullptr && aggregate != nullptr &&
             assignmentMoves.size() == 2,
         "the fixture should retain its passive aggregate and two adjacent "
         "class Move/Assign pairs");
  if (selected == nullptr || aggregate == nullptr ||
      assignmentMoves.size() != 2) {
    return;
  }

  const lang::MirPlace *arraySlot = nullptr;
  for (const lang::MirBlock &block : selected->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Initialize &&
          instruction.destination && instruction.operands.size() == 1 &&
          instruction.operands.front().kind == lang::MirOperandKind::Value &&
          instruction.operands.front().value == *aggregate->result) {
        arraySlot = selected->body.findPlace(*instruction.destination);
      }
    }
  }
  expect(arraySlot != nullptr,
         "the passive aggregate should initialize one exact array binding");
  if (arraySlot == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the passive class-array move schedule should support transformed "
         "MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter
          .emitFailureBodyText(address, "passive-class-array-move-test-v0", 1)
          .text;
  const std::string arrayStorage =
      "__gti_mir_p_" + std::to_string(arraySlot->id);
  bool erasedMoveLocals = true;
  for (const lang::MirInstruction *move : assignmentMoves) {
    erasedMoveLocals =
        erasedMoveLocals && move != nullptr && move->result &&
        text.find("__gti_mir_v_" + std::to_string(*move->result)) ==
            std::string::npos;
  }
  expect(text.find(arrayStorage + ".construction_address()") !=
                 std::string::npos &&
             text.find("stages into fixed-array assignment") !=
                 std::string::npos &&
             text.find("mir_checked_array_write_v1") != std::string::npos &&
             text.find(", std::move(__gti_mir_p_") != std::string::npos &&
             text.find(arrayStorage + ".destroy();") != std::string::npos &&
             text.find("].value") != std::string::npos && erasedMoveLocals,
         "the transformed body should construct the non-default array in its "
         "slot, move through checked assignments without SSA objects, read "
         "constant element fields, and destroy the slot on return");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(selected->id) + "\n") !=
             std::string::npos,
         "production emission should select the passive class-array body "
         "from verified MIR");
}

void testGeneratedSpecialMemberConstructionSchedule() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-generated-special-members.gti", R"(
class value {
  int32_t payload;

public:
  value(int32_t initial) : payload(initial) {}
  value(value& other) = default;
  value(value&& other) = default;
};

int32_t main() {
  value original = value(7);
  value copied = value(original);
  mut value movable = value(9);
  value moved = value(std::move(movable));
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the generated special-member fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the generated special-member fixture should lower its entry");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  std::size_t copies = 0;
  std::size_t moves = 0;
  bool copyUsesSourcePlace = false;
  bool moveUsesExplicitMove = false;
  bool everyResultActivatesBinding = true;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::Construct ||
          instruction.constructorKind == lang::ConstructorKind::Ordinary) {
        continue;
      }
      copies += instruction.constructorKind == lang::ConstructorKind::Copy;
      moves += instruction.constructorKind == lang::ConstructorKind::Move;
      if (instruction.operands.size() == 1) {
        const lang::MirOperand &operand = instruction.operands.front();
        copyUsesSourcePlace =
            copyUsesSourcePlace ||
            (instruction.constructorKind == lang::ConstructorKind::Copy &&
             operand.kind == lang::MirOperandKind::BorrowRead &&
             entry->body.findPlace(operand.place) != nullptr);
        if (instruction.constructorKind == lang::ConstructorKind::Move &&
            operand.kind == lang::MirOperandKind::Value) {
          const lang::MirValue *value = entry->body.findValue(operand.value);
          for (const lang::MirBlock &candidate : entry->body.blocks) {
            for (const lang::MirInstruction &definition :
                 candidate.instructions) {
              moveUsesExplicitMove =
                  moveUsesExplicitMove ||
                  (value != nullptr && definition.id == value->definition &&
                   definition.kind == lang::MirInstructionKind::Move);
            }
          }
        }
      }

      bool activated = false;
      if (instruction.result) {
        for (const lang::MirValueUse &use :
             entry->body.usesOf(*instruction.result)) {
          for (const lang::MirBlock &candidate : entry->body.blocks) {
            for (const lang::MirInstruction &consumer :
                 candidate.instructions) {
              activated =
                  activated ||
                  (consumer.id == use.instruction &&
                   consumer.kind == lang::MirInstructionKind::Initialize &&
                   consumer.destination && consumer.lifecycle.size() == 1 &&
                   consumer.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Initialize &&
                   consumer.lifecycle.front().target != 0);
            }
          }
        }
      }
      everyResultActivatesBinding = everyResultActivatesBinding && activated;
    }
  }
  expect(copies == 1 && moves == 1 && copyUsesSourcePlace &&
             moveUsesExplicitMove && everyResultActivatesBinding,
         "generated copy/move MIR should retain exact source, ownership, and "
         "destination-lifetime authority");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  const bool supported = entry->mayRaiseDefinedFailure
                             ? emitter.supportsFailureBodyText(address)
                             : emitter.supportsBodyText(address);
  expect(emitter.analyze(address).ready() && supported,
         "the fully scheduled generated special members should enter the "
         "general MIR text path");
  if (!supported) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      entry->mayRaiseDefinedFailure
          ? emitter.emitFailureBodyText(address,
                                        "generated-special-member-test-v0", 1)
          : emitter.emitBodyText(address, "generated-special-member-test-v0",
                                 1);
  expect(text.text.find("generated special-member construct") !=
                 std::string::npos &&
             text.text.find(".construct(std::move(") != std::string::npos,
         "generated copy/move emission should publish through the proved slot "
         "and move only the explicit move source");
}

void testForwardedConcretePackLayout() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-forwarded-concrete-pack.gti", R"(
void consume<Args...>(Args... values) {}

void forward<Args...>(Args... values) {
  consume(values...);
  consume(0, values...);
}

T keep_first<T, Rest...>(T first, Rest... rest) {
  forward(rest...);
  return first;
}

int32_t main() {
  forward();
  forward(1, true, "gti");
  int32_t inferred = keep_first(7, false, "tail");
  int32_t explicit_types =
      keep_first<int32_t, std::string_view>(9, "tail");
  return inferred + explicit_types - 16;
}
)");
  expect(frontend.canGenerateCode(),
         "the forwarded concrete-pack fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *forwarded = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const bool projectsPackElement = std::any_of(
        function.body.places.begin(), function.body.places.end(),
        [](const lang::MirPlace &candidate) {
          return std::any_of(candidate.projections.begin(),
                             candidate.projections.end(), [](const auto &item) {
                               return item.kind ==
                                      lang::MirProjectionKind::PackElement;
                             });
        });
    if (!projectsPackElement) {
      continue;
    }
    for (const lang::MirPlace &place : function.body.places) {
      if (place.root != lang::MirPlaceRootKind::Binding ||
          !place.projections.empty() ||
          place.type.kind != lang::SemanticType::TypePack ||
          !place.type.concretePack) {
        continue;
      }
      const auto binding =
          std::find(function.parameterBindings.begin(),
                    function.parameterBindings.end(), place.binding);
      if (binding == function.parameterBindings.end()) {
        continue;
      }
      const std::size_t index = static_cast<std::size_t>(
          std::distance(function.parameterBindings.begin(), binding));
      if (index < function.parameterTypes.size() &&
          function.parameterTypes[index].kind == lang::SemanticType::TypePack &&
          function.parameterTypes[index].concretePack &&
          place.type.arguments.size() >= 2 &&
          function.parameterTypes[index].arguments == place.type.arguments &&
          function.parameterTypes[index].genericParameterId !=
              place.type.genericParameterId) {
        forwarded = &function;
      }
    }
  }
  expect(forwarded != nullptr,
         "forwarding should retain distinct source pack identities over one "
         "concrete element layout");
  if (forwarded == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = forwarded->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a forwarded concrete pack should enter the general failure-body "
         "path from its exact element layout");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
      address, "forwarded-concrete-pack-test-v0", 1);
  expect(text.text.find("__gti_mir_arg_0") != std::string::npos &&
             text.text.find("__gti_mir_arg_1") != std::string::npos,
         "forwarded pack elements should spell as their flattened native "
         "arguments");
}

void testConditionalClassReturnAndPreparedResultStage() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-conditional-class-return.gti", R"(
struct Reading {
  int32_t value;

  Reading(int32_t initial) : value(initial) {}
};

std::unique_ptr<Reading> choose_owner(
    bool use_left,
    std::unique_ptr<Reading> left,
    std::unique_ptr<Reading> right) {
  return use_left ? std::move(left) : std::move(right);
}

int32_t main() {
  auto reading = choose_owner(
      true,
      std::make_unique<Reading>(7),
      std::make_unique<Reading>(11));
  return reading->value - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the conditional class-return fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *conditional = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    const bool hasBranch = std::any_of(
        function.body.blocks.begin(), function.body.blocks.end(),
        [](const lang::MirBlock &block) {
          return block.terminator.kind == lang::MirTerminatorKind::Branch;
        });
    if (function.parameterTypes.size() == 3 &&
        function.parameterTypes.front().kind == lang::SemanticType::Bool &&
        function.returnType.kind == lang::SemanticType::Class && hasBranch) {
      conditional = &function;
    }
  }
  expect(conditional != nullptr && entry != nullptr,
         "the fixture should lower its conditional owner return and entry");
  if (conditional == nullptr || entry == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);

  const lang::MirBodyAddress conditionalAddress{
      .kind = lang::MirBodyKind::Function, .owner = conditional->id};
  expect(emitter.analyze(conditionalAddress).ready() &&
             emitter.supportsFailureBodyText(conditionalAddress),
         "the conditional class return should enter the transformed "
         "failure-body path");
  if (emitter.supportsFailureBodyText(conditionalAddress)) {
    const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
        conditionalAddress, "conditional-class-return-test-v0", 1);
    expect(text.emitted() &&
               text.text.find("mir_lifetime_slot<") != std::string::npos &&
               text.text.find("std::construct_at(") != std::string::npos &&
               text.text.find("conditional return transfer-out") !=
                   std::string::npos,
           "the conditional arms should converge through one owned slot "
           "whose final move explicitly transfers its drop");
  }

  const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = entry->id};
  expect(emitter.analyze(entryAddress).ready() &&
             emitter.supportsFailureBodyText(entryAddress),
         "the caller should enter the transformed failure-body path");
  if (emitter.supportsFailureBodyText(entryAddress)) {
    const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
        entryAddress, "prepared-class-result-test-v0", 1);
    expect(text.emitted() &&
               text.text.find(".construction_address()") != std::string::npos &&
               text.text.find(".mark_constructed()") != std::string::npos &&
               text.text.find("stages a placement class result") !=
                   std::string::npos,
           "nested class results should construct in their prepared call "
           "slots and activate those slots only after success");
  }
}

void testConditionalFailureDestructorDrop() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-conditional-drop.gti", R"(
class Cleanup {
public:
  Cleanup() {}
  ~Cleanup() { std::println("cleanup"); }
  operator bool() { return true; }
};

int main() {
  bool skipped = false && Cleanup();
  return skipped ? 1 : 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the conditional class-drop fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirInstruction *conditionalDrop = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None) {
      continue;
    }
    entry = &function;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Drop &&
            instruction.destination && instruction.lifecycle.size() == 1 &&
            instruction.lifecycle.front().kind ==
                lang::MirLifecycleEventKind::Drop &&
            instruction.lifecycle.front().conditional) {
          conditionalDrop = &instruction;
        }
      }
    }
  }
  expect(entry != nullptr && conditionalDrop != nullptr,
         "short-circuit cleanup should lower one conditional MIR Drop");
  if (entry == nullptr || conditionalDrop == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the conditional class-drop fixture should enter transformed MIR "
         "emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "conditional-drop-test-v0", 1).text;
  const std::string conditionalDestroy =
      "__gti_mir_p_" + std::to_string(*conditionalDrop->destination) +
      ".destroy_with_failure_if_engaged(";
  expect(text.find(conditionalDestroy) != std::string::npos,
         "a conditional fallible Drop should succeed without touching an "
         "unengaged lifetime slot");
}

void testRetainedReferenceAddressAndConcreteDefaultReturn() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-retained-reference-address.gti", R"(
struct Sentinel {};

class BorrowingIterator<T> {
  T& value;

public:
  BorrowingIterator(T& source) : value(source) {}

  T& operator*() { return this.value; }
};

class SingleRange<T> {
  mut T value;

public:
  SingleRange(T initial) : value(initial) {}

  BorrowingIterator<T> begin() {
    return BorrowingIterator<T>(this.value);
  }

  Sentinel end() { return Sentinel(); }
};

int32_t main() {
  SingleRange<int32_t> range = SingleRange<int32_t>(7);
  mut BorrowingIterator<int32_t> iterator = range.begin();
  int32_t& value = *iterator;
  Sentinel sentinel = range.end();
  return value - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the retained-reference fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirFunctionInstance *defaultReturn = nullptr;
  lang::MirPlaceId referencePlace = 0;
  lang::MirLoanId retainedLoan = 0;
  lang::MirLoanId addressLoan = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
      for (const lang::MirBlock &block : function.body.blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Initialize ||
              !instruction.destination || instruction.operands.size() != 1 ||
              instruction.operands.front().kind != lang::MirOperandKind::Loan) {
            continue;
          }
          const lang::MirPlace *destination =
              function.body.findPlace(*instruction.destination);
          if (destination == nullptr ||
              destination->type.kind != lang::SemanticType::Reference) {
            continue;
          }
          const lang::MirLoanId parent = instruction.operands.front().loan;
          const auto child = std::find_if(
              function.body.loans.begin(), function.body.loans.end(),
              [&](const lang::MirLoan &loan) {
                return loan.kind == lang::MirLoanKind::CallResult &&
                       loan.parent == parent;
              });
          if (child != function.body.loans.end()) {
            referencePlace = destination->id;
            retainedLoan = parent;
            addressLoan = child->id;
          }
        }
      }
    }

    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.result && !instruction.functionTarget &&
            !instruction.constructorTarget && !instruction.receiver &&
            instruction.intrinsic == lang::IntrinsicKind::None &&
            instruction.operands.empty() &&
            instruction.info.type.kind == lang::SemanticType::Class) {
          defaultReturn = &function;
        }
      }
    }
  }
  expect(entry != nullptr && defaultReturn != nullptr && referencePlace != 0 &&
             retainedLoan != 0 && addressLoan != 0 &&
             retainedLoan != addressLoan,
         "the fixture should retain distinct provenance and address loans");
  if (entry == nullptr || defaultReturn == nullptr || referencePlace == 0 ||
      retainedLoan == 0 || addressLoan == 0 || retainedLoan == addressLoan) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);

  const lang::MirBodyAddress defaultAddress{.kind = lang::MirBodyKind::Function,
                                            .owner = defaultReturn->id};
  expect(emitter.analyze(defaultAddress).ready() &&
             emitter.supportsFailureBodyText(defaultAddress),
         "a concrete default construction should publish through the "
         "transformed class-return boundary");
  if (emitter.supportsFailureBodyText(defaultAddress)) {
    const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
        defaultAddress, "concrete-default-return-test-v0", 1);
    expect(text.text.find("std::construct_at(__gti_mir_out_result);") !=
               std::string::npos,
           "the concrete default return should construct directly in its "
           "uninitialized result storage");
  }

  const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = entry->id};
  expect(emitter.analyze(entryAddress).ready() &&
             emitter.supportsFailureBodyText(entryAddress),
         "the retained-reference caller should enter the transformed path");
  if (emitter.supportsFailureBodyText(entryAddress)) {
    const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
        entryAddress, "retained-reference-address-test-v0", 1);
    const std::string destination =
        "__gti_mir_p_" + std::to_string(referencePlace) + " = ";
    expect(text.text.find(destination + "__gti_mir_loan_" +
                          std::to_string(addressLoan) + ";") !=
                   std::string::npos &&
               text.text.find(destination + "__gti_mir_loan_" +
                              std::to_string(retainedLoan) + ";") ==
                   std::string::npos,
           "a retained reference should copy the child call-result address, "
           "not its provenance-only parent loan");
  }
}

void testVirtualFailureInterfaceFamily() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-virtual-failure-family.gti", R"(
interface Iterator<T> {
  T& operator*();
  void operator++() mut;
  int32_t stable_value();
};

class Counter : public Iterator<int32_t> {
  mut int32_t value;

public:
  Counter(int32_t initial) : value(initial) {}
  int32_t& operator*() override { return this.value; }
  void operator++() mut override { this.value++; }
  int32_t stable_value() override { return 7; }
};

int32_t main() {
  mut Counter counter{1};
  mut Iterator<int32_t>& iterator = counter;
  int32_t first = *iterator;
  ++iterator;
  return *iterator - first - 1 + iterator.stable_value() - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the virtual failure-family fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *dereferenceRoot = nullptr;
  const lang::MirFunctionInstance *dereferenceOverride = nullptr;
  const lang::MirFunctionInstance *incrementRoot = nullptr;
  const lang::MirFunctionInstance *incrementOverride = nullptr;
  const lang::MirFunctionInstance *stableRoot = nullptr;
  const lang::MirFunctionInstance *stableOverride = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (!function.overloadedOperator) {
      if (function.pureVirtual) {
        stableRoot = &function;
      } else if (function.overrideMethod) {
        stableOverride = &function;
      }
      continue;
    }
    const bool dereference =
        *function.overloadedOperator == lang::OverloadedOperator::Dereference;
    if (function.pureVirtual) {
      (dereference ? dereferenceRoot : incrementRoot) = &function;
    } else if (function.overrideMethod) {
      (dereference ? dereferenceOverride : incrementOverride) = &function;
    }
  }
  expect(dereferenceRoot != nullptr && dereferenceOverride != nullptr &&
             incrementRoot != nullptr && incrementOverride != nullptr &&
             stableRoot != nullptr && stableOverride != nullptr &&
             entry != nullptr,
         "the fixture should lower all pure contracts, all overrides, and "
         "its entry body");
  if (dereferenceRoot == nullptr || dereferenceOverride == nullptr ||
      incrementRoot == nullptr || incrementOverride == nullptr ||
      stableRoot == nullptr || stableOverride == nullptr || entry == nullptr) {
    return;
  }
  expect(
      lang::cppMirVirtualFailureContractRoot(frontend.mir, *dereferenceRoot) ==
              dereferenceRoot &&
          lang::cppMirVirtualFailureContractRoot(
              frontend.mir, *dereferenceOverride) == dereferenceRoot &&
          lang::cppMirVirtualFailureContractRoot(
              frontend.mir, *incrementRoot) == incrementRoot &&
          lang::cppMirVirtualFailureContractRoot(
              frontend.mir, *incrementOverride) == incrementRoot &&
          lang::cppMirVirtualFailureContractRoot(frontend.mir, *stableRoot) ==
              stableRoot &&
          lang::cppMirVirtualFailureContractRoot(frontend.mir,
                                                 *stableOverride) == stableRoot,
      "each override should resolve to its exact pure interface contract");
  expect(!stableOverride->mayRaiseDefinedFailure,
         "the stable override should exercise a failure-free implementation "
         "of a conservatively failure-capable virtual contract");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  for (const lang::MirFunctionInstance *function :
       {dereferenceOverride, incrementOverride, stableOverride, entry}) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function->id};
    expect(emitter.analyze(address).ready() &&
               emitter.supportsFailureBodyText(address),
           "the complete virtual family should support failure-form MIR "
           "emission");
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  expect(text.find("virtual bool "
                   "__gti_operator_dereference__gti_mir_failure(") !=
                 std::string::npos &&
             text.find("__gti_operator_dereference__gti_mir_failure("
                       "const std::int32_t **__gti_mir_out_result, "
                       "::gti_failure_record_v1 *__gti_mir_failure_record) "
                       "const override;") != std::string::npos,
         "the interface and concrete class should publish one virtual status "
         "ABI");
  expect(text.find("const std::int32_t &Counter::"
                   "__gti_operator_dereference() const {") !=
                 std::string::npos &&
             text.find("void Counter::__gti_operator_pre_increment() {") !=
                 std::string::npos,
         "ordinary virtual entries should be defined as terminal boundary "
         "wrappers for non-MIR callers");
  expect(text.find("scalar-cfg-failure-v1 function-instance " +
                   std::to_string(entry->id)) != std::string::npos &&
             text.find(".__gti_operator_dereference__gti_mir_failure(") !=
                 std::string::npos &&
             text.find("stable_value__gti_mir_failure(") != std::string::npos &&
             text.find(".Iterator<std::int32_t>::"
                       "__gti_operator_dereference__gti_mir_failure(") ==
                 std::string::npos,
         "the MIR entry should call the unqualified status member so native "
         "virtual dispatch remains active");
}

void testConcreteOnlyGenericVirtualContractRoots() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-concrete-only-virtual-roots.gti", R"(
interface Iterator<T> {
  T& operator*();
  void operator++() mut;
};

class Counter : public Iterator<int32_t> {
  mut int32_t value;

public:
  Counter(int32_t initial) : value(initial) {}
  int32_t& operator*() override { return this.value; }
  void operator++() mut override { this.value++; }
  bool operator!=(Counter& other) {
    return this.value != other.value;
  }
};

class Range {
  int32_t first;
  int32_t last;

public:
  Range(int32_t first, int32_t last) : first(first), last(last) {}
  Counter begin() { return Counter(this.first); }
  Counter end() { return Counter(this.last); }
};

int32_t main() {
  Range values{1, 4};
  mut int32_t total = 0;
  for (auto& value : values) {
    total += value;
  }
  return total - 6;
}
)");
  expect(frontend.canGenerateCode(),
         "the concrete-only virtual contract fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *dereferenceRoot = nullptr;
  const lang::MirFunctionInstance *incrementRoot = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (!function.pureVirtual || !function.overloadedOperator) {
      continue;
    }
    if (*function.overloadedOperator == lang::OverloadedOperator::Dereference) {
      dereferenceRoot = &function;
    } else if (*function.overloadedOperator ==
               lang::OverloadedOperator::PreIncrement) {
      incrementRoot = &function;
    }
  }
  expect(dereferenceRoot != nullptr && incrementRoot != nullptr &&
             entry != nullptr,
         "concrete overrides should materialize their exact generic pure "
         "contract roots without an interface-typed source use");
  if (dereferenceRoot == nullptr || incrementRoot == nullptr ||
      entry == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = entry->id};
  expect(emitter.analyze(entryAddress).ready() &&
             emitter.supportsFailureBodyText(entryAddress),
         "the concrete-only range caller should retain its transformed MIR "
         "body once the virtual contract family is complete");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(entry->id)) !=
                 std::string::npos &&
             artifact.contents.find(
                 ".__gti_operator_dereference__gti_mir_failure(") !=
                 std::string::npos,
         "the production backend should emit the concrete-only range caller "
         "through the virtual failure ABI");
}

void testAbstractBaseFailureFamilyAndInheritedMembers() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-abstract-base-failure-family.gti", R"(
class Base {
public:
  mut int32_t state = 0;

  virtual bool advance() mut = 0;
  void stop() mut { this.state = 9; }
  bool run() mut { return this.advance(); }
};

class Derived : public Base {
public:
  Derived() {}

  bool advance() mut override {
    this.state++;
    this.stop();
    return true;
  }

  bool complete() { return this.state == 9; }
};

int32_t main() {
  mut Derived value{};
  if (!value.run()) {
    return 1;
  }
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the abstract-base failure-family fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *root = nullptr;
  const lang::MirFunctionInstance *override = nullptr;
  const lang::MirFunctionInstance *inheritedCaller = nullptr;
  const lang::MirFunctionInstance *fieldReader = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    } else if (function.pureVirtual) {
      root = &function;
    } else if (function.overrideMethod) {
      override = &function;
      inheritedCaller = &function;
    } else if (function.owner) {
      const bool callsBaseMember = std::any_of(
          function.body.blocks.begin(), function.body.blocks.end(),
          [&](const lang::MirBlock &block) {
            return std::any_of(block.instructions.begin(),
                               block.instructions.end(),
                               [&](const lang::MirInstruction &instruction) {
                                 if (!instruction.functionTarget) {
                                   return false;
                                 }
                                 const lang::MirFunctionInstance *target =
                                     frontend.mir.findFunctionInstance(
                                         *instruction.functionTarget);
                                 return target != nullptr && target->owner &&
                                        *target->owner != *function.owner;
                               });
          });
      if (callsBaseMember) {
        inheritedCaller = &function;
      } else if (override != nullptr && function.owner == override->owner &&
                 std::any_of(
                     function.body.places.begin(), function.body.places.end(),
                     [](const lang::MirPlace &place) {
                       return place.root == lang::MirPlaceRootKind::This &&
                              !place.projections.empty() &&
                              place.projections.front().kind ==
                                  lang::MirProjectionKind::Field;
                     })) {
        fieldReader = &function;
      }
    }
  }
  const lang::MirConstructorInstance *constructor = nullptr;
  if (override != nullptr && override->owner) {
    for (const lang::MirConstructorInstance &candidate :
         frontend.mir.constructorInstances()) {
      if (candidate.owner == *override->owner) {
        constructor = &candidate;
        break;
      }
    }
  }
  expect(root != nullptr && override != nullptr && inheritedCaller != nullptr &&
             fieldReader != nullptr && entry != nullptr &&
             constructor != nullptr,
         "the fixture should lower its abstract contract, override, inherited "
         "member call, inherited field read, constructor, and entry");
  if (root == nullptr || override == nullptr || inheritedCaller == nullptr ||
      fieldReader == nullptr || entry == nullptr || constructor == nullptr) {
    return;
  }
  expect(lang::cppMirVirtualFailureContractRoot(frontend.mir, *root) == root &&
             lang::cppMirVirtualFailureContractRoot(frontend.mir, *override) ==
                 root,
         "a pure virtual declared on an abstract class should own the same "
         "status ABI as its concrete override");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  for (const lang::MirFunctionInstance *function :
       {override, inheritedCaller, fieldReader, entry}) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function->id};
    expect(emitter.analyze(address).ready() &&
               (emitter.supportsBodyText(address) ||
                emitter.supportsFailureBodyText(address)),
           "abstract-base family body " + std::to_string(function->id) +
               " should have verified MIR text");
  }
  const lang::MirBodyAddress constructorAddress{
      .kind = lang::MirBodyKind::Constructor, .owner = constructor->id};
  expect(emitter.analyze(constructorAddress).ready() &&
             emitter.supportsFailureBodyText(constructorAddress),
         "the passive default-base constructor should have verified failure "
         "MIR text");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("virtual bool advance__gti_mir_failure(") !=
                 std::string::npos &&
             artifact.contents.find("advance__gti_mir_failure(") !=
                 artifact.contents.rfind("advance__gti_mir_failure(") &&
             artifact.contents.find("constructor-instance " +
                                    std::to_string(constructor->id) + "\n") !=
                 std::string::npos &&
             artifact.contents.find("function-instance " +
                                    std::to_string(entry->id) + "\n") !=
                 std::string::npos,
         "an abstract class and its override should publish the virtual "
         "failure contract, passive constructor, and entry implementation");
}

void testUniqueOwnerUpcastFailureBody() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-unique-owner-upcast.gti", R"(
interface Base {
  int32_t value();
};

class Derived : public Base {
public:
  Derived() {}
  int32_t value() override { return 7; }
};

class Other : public Base {
public:
  Other() {}
  int32_t value() override { return 5; }
};

int32_t main() {
  mut std::unique_ptr<Derived> derived = std::make_unique<Derived>();
  mut std::unique_ptr<Base> base =
      std::upcast_unique<Base, Derived>(std::move(derived));
  mut std::unique_ptr<Other> other = std::make_unique<Other>();
  mut std::unique_ptr<Base> other_base =
      std::upcast_unique<Base, Other>(std::move(other));
  return base->value() + other_base->value() - 12;
}
)");
  expect(frontend.canGenerateCode(),
         "the unique-owner upcast fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *upcast = nullptr;
  const lang::MirInstruction *upcastIntrinsic = nullptr;
  const lang::MirInstruction *discardedNullAssignment = nullptr;
  const lang::MirInstruction *returnMove = nullptr;
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
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.intrinsic == lang::IntrinsicKind::UniqueOwnerUpcast) {
          upcast = &function;
          upcastIntrinsic = &instruction;
        }
        if (instruction.kind == lang::MirInstructionKind::Assign &&
            instruction.result && instruction.operands.size() == 1 &&
            instruction.operands.front().type.kind ==
                lang::SemanticType::NullPtr &&
            function.body.usesOf(*instruction.result).empty()) {
          discardedNullAssignment = &instruction;
        }
      }
      if (upcast == &function &&
          block.terminator.kind == lang::MirTerminatorKind::Return &&
          block.terminator.value &&
          block.terminator.value->kind == lang::MirOperandKind::Value) {
        const lang::MirValue *returned =
            function.body.findValue(block.terminator.value->value);
        const lang::MirInstruction *construct =
            returned == nullptr
                ? nullptr
                : findInstruction(function.body, returned->definition);
        if (construct != nullptr &&
            construct->kind == lang::MirInstructionKind::Construct &&
            construct->operands.size() == 1 &&
            construct->operands.front().kind == lang::MirOperandKind::Value) {
          const lang::MirValue *moved =
              function.body.findValue(construct->operands.front().value);
          const lang::MirInstruction *definition =
              moved == nullptr
                  ? nullptr
                  : findInstruction(function.body, moved->definition);
          if (definition != nullptr &&
              definition->kind == lang::MirInstructionKind::Move) {
            returnMove = definition;
          }
        }
      }
    }
  }
  expect(upcast != nullptr && upcastIntrinsic != nullptr &&
             discardedNullAssignment != nullptr && returnMove != nullptr,
         "the fixture should lower the consuming intrinsic, discarded null "
         "assignment, and fused owner return");
  if (upcast == nullptr || upcastIntrinsic == nullptr ||
      discardedNullAssignment == nullptr || returnMove == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = upcast->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the unique-owner upcast body should support transformed MIR "
         "emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "unique-owner-upcast-test-v0", 1);
  const std::size_t upcastCall = text.text.find("unique_owner_upcast<");
  const std::size_t movedArgument = text.text.find(">(std::move(", upcastCall);
  expect(text.text.find("unique_owner_upcast<") != std::string::npos &&
             movedArgument != std::string::npos &&
             text.text.find("__gti_mir_v_" +
                            std::to_string(*discardedNullAssignment->result) +
                            " =") == std::string::npos &&
             text.text.find("std::move(__gti_mir_v_" +
                            std::to_string(*returnMove->result)) ==
                 std::string::npos &&
             text.text.find(
                 "std::construct_at(__gti_mir_out_result, std::move(") !=
                 std::string::npos,
         "the upcast should consume its owner, omit the discarded assignment "
         "readback, and publish the fused move from its source place");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::size_t primary = artifact.contents.find("class unique_ptr");
  const std::size_t nextMember = artifact.contents.find("const T &", primary);
  std::size_t declarations = 0;
  if (primary != std::string::npos && nextMember != std::string::npos) {
    std::size_t cursor = primary;
    while ((cursor = artifact.contents.find("upcast__gti_mir_failure(",
                                            cursor)) != std::string::npos &&
           cursor < nextMember) {
      ++declarations;
      ++cursor;
    }
  }
  expect(declarations == 1,
         "concrete owner specializations sharing one substituted upcast "
         "signature should publish one primary-template declaration");
}

void testExpectedLifetimeSlotObserverFusion() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-slot-observer.gti", R"(
#include <std/format>
#include <std/string>

int32_t main() {
  auto formatted = std::format("value={}", int32_t(7));
  if (!formatted) {
    return 1;
  }
  std::println(formatted.value());
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the expected-slot observer fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  lang::MirValueId observerValue = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &load : block.instructions) {
        if (load.kind != lang::MirInstructionKind::Load || !load.result ||
            load.info.type.kind != lang::SemanticType::Expected ||
            load.info.type.arguments.size() != 2 ||
            load.info.type.arguments.front().kind !=
                lang::SemanticType::Class ||
            load.operands.size() != 1 || load.operands.front().place == 0) {
          continue;
        }
        const bool ownedSlot =
            std::any_of(function.body.dropObligations.begin(),
                        function.body.dropObligations.end(),
                        [&](const lang::MirDropObligation &drop) {
                          return drop.place == load.operands.front().place;
                        });
        bool observer = false;
        bool extraction = false;
        for (const lang::MirBlock &candidate : function.body.blocks) {
          for (const lang::MirInstruction &instruction :
               candidate.instructions) {
            observer = observer ||
                       (instruction.kind == lang::MirInstructionKind::Compute &&
                        instruction.operation ==
                            lang::MirOperation::ExpectedHasValue &&
                        instruction.operands.size() == 1 &&
                        instruction.operands.front().kind ==
                            lang::MirOperandKind::Value &&
                        instruction.operands.front().value == *load.result);
            extraction =
                extraction ||
                (instruction.kind == lang::MirInstructionKind::Call &&
                 instruction.receiver &&
                 instruction.receiver->place == load.operands.front().place &&
                 (instruction.intrinsic == lang::IntrinsicKind::ExpectedValue ||
                  instruction.intrinsic == lang::IntrinsicKind::ExpectedError));
          }
        }
        if (ownedSlot && observer && extraction) {
          selected = &function;
          observerValue = *load.result;
        }
      }
    }
  }
  expect(selected != nullptr,
         "the fixture should lower an owned Expected slot with an observer "
         "and extraction");
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
         "the owned Expected slot should enter the transformed failure-body "
         "path");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "expected-slot-observer-test-v0", 1);
  const std::string observerLocal =
      "__gti_mir_v_" + std::to_string(observerValue) + "{}";
  const bool extractionUsesLiveValue =
      text.text.find(".get().value()") != std::string::npos ||
      text.text.find(".get().error()") != std::string::npos ||
      (text.text.find("mir_expected_") != std::string::npos &&
       text.text.find(".get(), &__gti_mir_") != std::string::npos);
  expect(text.text.find(".get().has_value()") != std::string::npos &&
             extractionUsesLiveValue,
         "Expected observers and extraction helpers should read the live "
         "value inside its lifetime slot");
  expect(text.text.find(observerLocal) == std::string::npos,
         "the Expected observer's explicit MIR load must not materialize a "
         "copying SSA local");
}

void testExpectedValueOrBorrowedReceiver() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-value-or.gti", R"(
expected<void, std::string_view> complete() {
  return;
}

int32_t main() {
  expected<int32_t, std::string_view> result = int32_t(1);
  expected<void, std::string_view> completion = complete();
  completion.value();
  return result.value_or(int32_t(0)) - int32_t(1);
}
)");
  expect(frontend.canGenerateCode(),
         "the expected value_or fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirInstruction *valueOr = nullptr;
  const lang::MirInstruction *voidExtraction = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None) {
      continue;
    }
    entry = &function;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.intrinsic == lang::IntrinsicKind::ExpectedValueOr) {
          valueOr = &instruction;
        }
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.intrinsic == lang::IntrinsicKind::ExpectedValue &&
            instruction.info.type == lang::SemanticType::Void) {
          voidExtraction = &instruction;
        }
      }
    }
  }
  expect(entry != nullptr && valueOr != nullptr && valueOr->receiver &&
             voidExtraction != nullptr && voidExtraction->receiver &&
             !voidExtraction->result &&
             valueOr->receiver->kind == lang::MirOperandKind::BorrowRead &&
             valueOr->receiver->place != 0,
         "Expected observers should retain their exact read-borrowed receiver "
         "and void value() should remain resultless");
  if (entry == nullptr || valueOr == nullptr || !valueOr->receiver ||
      voidExtraction == nullptr || !voidExtraction->receiver) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the borrowed value_or receiver should support transformed MIR "
         "emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "expected-value-or-test-v0", 1);
  const std::string receiver =
      "__gti_mir_p_" + std::to_string(valueOr->receiver->place) + ".value_or(";
  const std::string voidReceiver =
      "::gti_internal::backend::mir_expected_void_value_v1(__gti_mir_p_" +
      std::to_string(voidExtraction->receiver->place) + ");";
  expect(text.text.find(receiver) != std::string::npos &&
             text.text.find(voidReceiver) != std::string::npos,
         "Expected observers should read their live places without an SSA "
         "copy or a fabricated void result");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("function-instance " +
                                std::to_string(entry->id) + "\n") !=
             std::string::npos,
         "production emission should retain the borrowed value_or body on "
         "the verified MIR route");
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

void testExpectedOwningPayloadPublicationAndForwarding() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-owning-payload.gti", R"(
#include <std/tcp>

int main() {
  auto opened = std::tcp::open();
  if (!opened) {
    return 1;
  }
  return opened.value().is_open() ? 0 : 2;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning Expected payload fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *payloadProducer = nullptr;
  const lang::MirFunctionInstance *forwarder = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (function.returnType.kind != lang::SemanticType::Expected ||
        function.returnType.arguments.size() != 2 ||
        function.returnType.arguments.front().kind !=
            lang::SemanticType::Class) {
      continue;
    }
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.result &&
            instruction.info.type == function.returnType.arguments.front()) {
          payloadProducer = &function;
        }
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.result &&
            instruction.info.type == function.returnType) {
          forwarder = &function;
        }
      }
    }
  }
  expect(payloadProducer != nullptr && forwarder != nullptr && entry != nullptr,
         "the fixture should lower a payload constructor, an unchanged "
         "Expected forwarder, and a hosted caller");
  if (payloadProducer == nullptr || forwarder == nullptr || entry == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  for (const lang::MirFunctionInstance *function :
       {payloadProducer, forwarder, entry}) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function->id};
    expect(emitter.analyze(address).ready() &&
               emitter.supportsFailureBodyText(address),
           "each owning Expected call-chain body should support transformed "
           "failure emission");
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const auto hasMarker = [&](const lang::MirFunctionInstance &function) {
    return text.find("scalar-cfg-failure-v1 function-instance " +
                     std::to_string(function.id)) != std::string::npos;
  };
  expect(hasMarker(*payloadProducer) && hasMarker(*forwarder) &&
             hasMarker(*entry),
         "production emission should select the complete owning Expected "
         "call chain");
  expect(text.find("handle(std::move(__gti_mir_arg_0))") != std::string::npos &&
             text.find(".construct_from([&]() { return ") !=
                 std::string::npos &&
             text.find(", true> __gti_mir_v_") != std::string::npos,
         "the private owned field should initialize in place and the payload "
         "should publish through one representation-cleanup slot");
  expect(text.find("placement value published at its call") !=
                 std::string::npos &&
             text.find("(__gti_mir_out_result, __gti_mir_failure_record)") !=
                 std::string::npos,
         "the unchanged Expected forwarder should pass its caller's result "
         "storage directly to the transformed callee");
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

void testContextualSignedMinimumInFailureCaller() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-contextual-signed-minimum.gti", R"(
int32_t checked_modulo(int32_t value, int32_t divisor) {
  return value % divisor;
}

bool matches_parenthesized_minimum(int64_t value) {
  return value == -(9223372036854775808);
}

int32_t main() {
  int32_t minimum = -2147483648;
  int32_t remainder = checked_modulo(4, 2);
  if (minimum == -2147483648) {
    return remainder;
  }
  return 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the contextual signed-minimum fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirFunctionInstance *parenthesized = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    const lang::HirFunctionInstance *hir =
        frontend.hir.findFunctionInstance(function.id);
    if (hir != nullptr && hir->source != nullptr &&
        hir->source->name().lexeme == "matches_parenthesized_minimum") {
      parenthesized = &function;
    }
  }
  expect(entry != nullptr && parenthesized != nullptr,
         "the contextual signed-minimum fixture should lower its entry and "
         "parenthesized comparison");
  if (entry == nullptr || parenthesized == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a failure caller should retain a contextually narrowed signed "
         "minimum in transformed MIR emission");
  if (emitter.supportsFailureBodyText(address)) {
    const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
        address, "contextual-signed-minimum-test-v0", 1);
    expect(text.text.find("static_cast<std::int32_t>(-2147483647 - 1)") !=
               std::string::npos,
           "the wider literal carrier should fuse into the safe int32 minimum "
           "spelling at negation");
  }

  const lang::MirBodyAddress parenthesizedAddress{
      .kind = lang::MirBodyKind::Function, .owner = parenthesized->id};
  expect(emitter.analyze(parenthesizedAddress).ready() &&
             emitter.supportsFailureBodyText(parenthesizedAddress),
         "an exact identity inserted by parentheses should remain inside the "
         "signed-minimum MIR fusion proof");
  if (emitter.supportsFailureBodyText(parenthesizedAddress)) {
    const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
        parenthesizedAddress, "parenthesized-signed-minimum-test-v0", 1);
    expect(
        text.text.find("static_cast<std::int64_t>(-9223372036854775807 - 1)") !=
            std::string::npos,
        "the parenthesized uint64 carrier should fuse into the safe int64 "
        "minimum spelling");
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(entry->id)) !=
                 std::string::npos &&
             artifact.contents.find("checked_modulo__gti_mir_failure(") !=
                 std::string::npos,
         "production emission should keep the caller and checked callee on "
         "the transformed failure ABI");
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

void testFailureCapableClassReturnTemporaryReceiver() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-class-return-temporary-receiver.gti", R"(
#include <std/string>

int32_t main() {
  if (std::to_string(int8_t(0)) != "0") {
    return 1;
  }
  return 0;
}
)");
  expect(
      frontend.canGenerateCode(),
      "the class-return temporary-receiver fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the fixture should lower its no-argument entry");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a fallible class-return call used as a fallible operator receiver "
         "should enter transformed MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "class-return-receiver-test-v0", 1)
          .text;
  expect(text.find("mir_lifetime_slot<") != std::string::npos &&
             text.find("to_string__gti_mir_failure(") != std::string::npos &&
             text.find(".construction_address()") != std::string::npos &&
             text.find(".mark_constructed();") != std::string::npos &&
             text.find("string::__gti_mir_op_ne__gti_mir_failure(") !=
                 std::string::npos &&
             text.find(".get().") != std::string::npos &&
             text.find(".destroy();") != std::string::npos &&
             text.find("failure cleanup drop-obligation") != std::string::npos,
         "the returned class should publish into one MIR lifetime slot, serve "
         "as the operator receiver, and retire on success and failure");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(entry->id) + "\n") !=
             std::string::npos,
         "production emission should select the temporary-receiver entry from "
         "verified MIR");
}

void testAdjacentClassCopyAssignmentFusion() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-class-copy-assignment.gti", R"(
struct CopyValue {
  int32_t value = 0;
  CopyValue(int32_t initial) : value(initial) {}
  int32_t read() { return this.value; }
};

int main() {
  mut CopyValue target = CopyValue();
  CopyValue source = CopyValue(7);
  target = source;
  return target.read() - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the adjacent class-copy fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  const lang::MirInstruction *load = nullptr;
  const lang::MirInstruction *assignment = nullptr;
  const lang::MirPlace *source = nullptr;
  const lang::MirPlace *destination = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (std::size_t index = 0; index + 1 < block.instructions.size();
           ++index) {
        const lang::MirInstruction &candidate = block.instructions[index];
        const lang::MirInstruction &consumer = block.instructions[index + 1];
        if (candidate.kind != lang::MirInstructionKind::Load ||
            !candidate.result || candidate.operands.size() != 1 ||
            candidate.operands.front().kind != lang::MirOperandKind::Copy ||
            candidate.info.type.kind != lang::SemanticType::Class ||
            consumer.kind != lang::MirInstructionKind::Assign ||
            !consumer.destination || consumer.operands.size() != 1 ||
            consumer.operands.front().kind != lang::MirOperandKind::Value ||
            consumer.operands.front().value != *candidate.result) {
          continue;
        }
        selected = &function;
        load = &candidate;
        assignment = &consumer;
        source = function.body.findPlace(candidate.operands.front().place);
        destination = function.body.findPlace(*consumer.destination);
      }
    }
  }
  expect(selected != nullptr && load != nullptr && assignment != nullptr &&
             source != nullptr && destination != nullptr,
         "the fixture should lower one adjacent class Load/Assign pair");
  if (selected == nullptr || load == nullptr || assignment == nullptr ||
      source == nullptr || destination == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the adjacent class copy should support transformed MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "class-copy-assignment-test-v0", 1)
          .text;
  const std::string copyValue = "__gti_mir_v_" + std::to_string(*load->result);
  const std::string assignmentValue =
      "__gti_mir_v_" + std::to_string(*assignment->result);
  const std::string directCopy =
      "__gti_mir_p_" + std::to_string(destination->id) +
      ".get() = __gti_mir_p_" + std::to_string(source->id) + ".get();";
  expect(text.find("class copy load " + std::to_string(*load->result) +
                   " fuses into assignment " +
                   std::to_string(assignment->id)) != std::string::npos &&
             text.find(directCopy) != std::string::npos &&
             text.find(copyValue) == std::string::npos &&
             text.find(assignmentValue) == std::string::npos,
         "the class copy should spell directly between MIR-owned slots without "
         "inventing either discarded SSA lifetime");
}

void testNativeCallbackFactoryBody() {
  lang::FrontendResult frontend =
      analyzeWithStandardLibrary("native_callback_factory.gti", R"(
[[c_opaque]] struct NativeHandle;

using callback = (int32_t) -> int32_t;
using notification = (NativeHandle*) -> void;
using callback_factory = () -> callback;

[[c_abi]] struct NativeCallbacks {
  callback unary;
  notification notify;
  void* user;
};

extern "C" {
  callback set_callback(callback value);
  callback_factory set_factory(callback_factory value);
  void install_callbacks(const NativeCallbacks* callbacks);
}

int32_t add_one(int32_t value) {
  return value + 1;
}

callback make_callback() {
  return add_one;
}
)");
  expect(frontend.canGenerateCode(),
         "the native callback factory should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O0,
                                        .compatibility = &compatibility});
  expect(optimized.valid(),
         "the callback factory should preserve a valid optimized MIR program");
  if (!optimized.valid()) {
    return;
  }
  const lang::MirProgram &mir = optimized.mir;

  const auto selected = std::find_if(
      mir.functionInstances().begin(), mir.functionInstances().end(),
      [](const lang::MirFunctionInstance &function) {
        return function.definitionKind == lang::MirDefinitionKind::Source &&
               function.returnType.kind == lang::SemanticType::NativeFunction;
      });
  expect(selected != mir.functionInstances().end(),
         "the fixture should lower a callback-valued source function");
  if (selected == mir.functionInstances().end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(mir, map);
  const lang::CppMirProgramEmissionAnalysis programAnalysis =
      emitter.analyzeProgram();
  for (const lang::CppMirBodyEmissionIssue &issue : programAnalysis.issues) {
    std::cerr << "native callback program issue: " << issue.detail << '\n';
  }
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  const lang::CppMirBodyEmissionAnalysis analysis = emitter.analyze(address);
  for (const lang::CppMirBodyEmissionIssue &issue : analysis.issues) {
    std::cerr << "native callback body issue: " << issue.detail << '\n';
  }
  expect(!selected->mayRaiseDefinedFailure,
         "forming and returning a native callback should not raise a defined "
         "failure");
  expect(analysis.ready(),
         "the callback factory should have complete MIR representation rows");
  expect(programAnalysis.issues.empty(),
         "nested callback aliases should preserve a coherent representation "
         "map");
  expect(emitter.supportsBodyText(address),
         "the callback factory should support plain verified-MIR emission");
  if (!analysis.ready() || !emitter.supportsBodyText(address)) {
    return;
  }
  const std::string text =
      emitter.emitBodyText(address, "native-callback-factory-test-v0", 1).text;
  expect(text.find("__gti_native_callback_") != std::string::npos,
         "the callback factory should return its exact adapter thunk");
}

int main() {
  testExhaustiveEnumClassification();
  testReadyBodyAndRepresentationFailures();
  testHostedStartupOwnsRemainingAuthorityGap();
  testVerifiedProgramInitializationPlanClassification();
  testSafeIndexUsesBoundsNotRawMemory();
  testUnsafeRawPointerVocabulary();
  testConcreteGenericFieldOwner();
  testAmbiguousStaticStorageFailsClosed();
  testConcreteClassStaticStorage();
  testOwningCheckedBodyEmitsWholeCleanupProof();
  testRichFieldInitializerSchedule();
  testDependentFieldInitializerSchedule();
  testContainedOwningFieldConstructor();
  testContainedAbstractBaseConstructor();
  testExampleCorpusEmissionReadiness();
  testGeneralTextStepMatchesProductionEmission();
  testCleanupFixtureFunctionBodiesAreReady();
  testOwnedLifecycleConstructionBodiesReady();
  testDischargedStorageReadAnalysis();
  testInlineClosureChainEmission();
  testClosureCaptureFreezeMaterializes();
  testLexicalCopyCaptureThroughCallableTemplate();
  testDefaultConstructionPreparedParameterStaging();
  testGenericMemberFailureOverloadOnOrdinaryOwner();
  testCallableTemplateBodyVocabulary();
  testMovedCallableReceiverMutableFallback();
  testStoredReferenceConstructorSchedule();
  testGenericOwnerConstructorSpecialization();
  testConcreteGenericConstructorAndMemberPackPublication();
  testPassiveConstructorFailureSummary();
  testPassiveFixedArrayFailureClosure();
  testOwningFixedArrayAggregateAndCheckedMove();
  testPassiveClassArrayMoveAssignmentSchedule();
  testGeneratedSpecialMemberConstructionSchedule();
  testForwardedConcretePackLayout();
  testConditionalClassReturnAndPreparedResultStage();
  testConditionalFailureDestructorDrop();
  testRetainedReferenceAddressAndConcreteDefaultReturn();
  testVirtualFailureInterfaceFamily();
  testConcreteOnlyGenericVirtualContractRoots();
  testAbstractBaseFailureFamilyAndInheritedMembers();
  testUniqueOwnerUpcastFailureBody();
  testExpectedLifetimeSlotObserverFusion();
  testExpectedValueOrBorrowedReceiver();
  testDeclarationOnlyCallBoundary();
  testExpectedOwningPayloadPublicationAndForwarding();
  testPassiveReturnSignedMinimumAndRepresentationSlot();
  testContextualSignedMinimumInFailureCaller();
  testSingleUseClassSsaRepresentationSlot();
  testFailureCapableClassReturnTemporaryReceiver();
  testAdjacentClassCopyAssignmentFusion();
  testNativeCallbackFactoryBody();
  testDeducedCallableTemplateEmission();
  testReferenceReturnFailureAbi();
  testGenericOwnerReferenceReturnFailureAbi();

  if (failures != 0) {
    std::cerr << failures << " cpp MIR body-emitter test(s) failed\n";
    return 1;
  }
  std::cout << "cpp MIR body-emitter tests passed\n";
  return 0;
}
