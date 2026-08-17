#include "../src/compiler/cpp_mir_body_emitter.h"
#include "../src/compiler/cpp_mir_representation_snapshot.h"

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/optimizer.h"

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
  case lang::SemanticType::NullPtr:
    return lang::CppMirTypeRepresentationKind::NullPointer;
  case lang::SemanticType::RawPointer:
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
      for (const lang::MirPackFoldElement &element :
           instruction.packFoldElements) {
        addType(rows, element.elementType);
        for (const lang::SemanticType &type : element.parameterTypes) {
          addType(rows, type);
        }
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
          lang::classifyCppMirOperation(lang::MirOperation::PackExpansion) ==
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
  expect(
      missing.readiness ==
              lang::CppMirBodyEmissionReadiness::MissingRepresentation &&
          hasIssue(
              missing,
              lang::CppMirBodyEmissionIssueKind::MissingTypeRepresentation) &&
          !hasIssue(missing,
                    lang::CppMirBodyEmissionIssueKind::MissingPackExpansionMir),
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
      startupAnalysis.readiness ==
              lang::CppMirBodyEmissionReadiness::MissingMirAuthority &&
          hasIssue(
              startupAnalysis,
              lang::CppMirBodyEmissionIssueKind::MissingFailureCleanupMir) &&
          !hasIssue(startupAnalysis,
                    lang::CppMirBodyEmissionIssueKind::MissingHostedStartupMir),
      "the generated startup body should expose only its remaining Stage-E "
      "failure-containment authority gap");

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
  expect(
      ownedStartup.readiness ==
              lang::CppMirBodyEmissionReadiness::MissingMirAuthority &&
          hasIssue(
              ownedStartup,
              lang::CppMirBodyEmissionIssueKind::MissingFailureCleanupMir) &&
          hasIssue(ownedStartup, lang::CppMirBodyEmissionIssueKind::
                                     MissingPartialConstructionRollbackMir) &&
          !hasIssue(
              ownedStartup,
              lang::CppMirBodyEmissionIssueKind::MissingCallInputScheduleMir) &&
          !hasIssue(
              ownedStartup,
              lang::CppMirBodyEmissionIssueKind::MissingOrderedCompoundMir) &&
          std::all_of(ownedStartup.issues.begin(), ownedStartup.issues.end(),
                      [](const lang::CppMirBodyEmissionIssue &issue) {
                        return issue.kind == lang::CppMirBodyEmissionIssueKind::
                                                 MissingFailureCleanupMir ||
                               issue.kind ==
                                   lang::CppMirBodyEmissionIssueKind::
                                       MissingPartialConstructionRollbackMir;
                      }),
      "the exact generated owned startup schedule should expose only its "
      "Stage-E cleanup and partial-rollback debts");
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

void testOwningCheckedBodyNeedsWholeCleanupProof() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-owning-failure.gti", R"(
class FailureCleanup {
public:
  int32_t bit;
  FailureCleanup(int32_t value) : bit(value) { value + 1; }
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
  const lang::CppMirBodyEmissionAnalysis analysis =
      lang::CppMirBodyEmitter(
          frontend.mir, lang::CppMirBodyEmissionMap(completeRows(frontend.mir)))
          .analyze(
              {.kind = lang::MirBodyKind::Function, .owner = function->id});
  expect(
      analysis.readiness ==
              lang::CppMirBodyEmissionReadiness::MissingMirAuthority &&
          hasIssue(analysis,
                   lang::CppMirBodyEmissionIssueKind::MissingFailureCleanupMir),
      "local Invoke and lifetime helper rows must not promote a general "
      "owning failure-cleanup body");
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
    const lang::CppMirBodyEmissionMap map(completeRows(frontend.mir));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    for (const lang::MirBodyAddress address :
         lang::enumerateMirBodyAddresses(frontend.mir)) {
      const lang::CppMirBodyEmissionAnalysis analysis =
          emitter.analyze(address);
      ++bodies;
      if (analysis.ready()) {
        ++ready;
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
// general text step must reproduce, byte for byte, every scalar-cfg and
// scalar-direct-call body the transitional emitter publishes, for every
// shipped example. The artifact's own markers select the bodies, so this
// gate tracks production admission without modeling the selectors.
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
        lang::CppBackend().generate({.program = frontend.program,
                                     .semantics = frontend.semantics,
                                     .hir = frontend.hir,
                                     .mir = frontend.mir,
                                     .sourceMir = &frontend.mir,
                                     .optimizations = optimizations});

    lang::CppMirBodyEmissionMapRows rows = lang::buildCppMirBodyEmissionMapRows(
        frontend.semantics, frontend.mir, lang::CppStandard::Cpp23);
    const lang::CppMirBodyEmissionMapRows again =
        lang::buildCppMirBodyEmissionMapRows(frontend.semantics, frontend.mir,
                                             lang::CppStandard::Cpp23);
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
      if (label != "scalar-cfg-v1") {
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
      const lang::CppMirBodyEmissionText emission = emitter.emitBodyText(
          {.kind = lang::MirBodyKind::Function, .owner = instance}, label,
          markerIndent / 2 - 1);
      if (emission.text.find("::gti_internal::backend::wrapping_add(") !=
              std::string::npos ||
          emission.text.find("::gti_internal::backend::saturating_sub(") !=
              std::string::npos) {
        intrinsicBodies = true;
      }
      if (emission.text.find("__gti_mir_loan_") != std::string::npos &&
          emission.text.find("return *__gti_mir_loan_") != std::string::npos) {
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
         "every production scalar-cfg/direct body must byte-match the "
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
  const lang::CppMirBodyEmissionMap map(lang::buildCppMirBodyEmissionMapRows(
      frontend.semantics, frontend.mir, lang::CppStandard::Cpp23));
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
  const lang::CppMirBodyEmissionMap map(lang::buildCppMirBodyEmissionMapRows(
      frontend.semantics, frontend.mir, lang::CppStandard::Cpp23));
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
  const lang::CppMirBodyEmissionMap map(lang::buildCppMirBodyEmissionMapRows(
      frontend.semantics, frontend.mir, lang::CppStandard::Cpp23));
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
// Checked arithmetic inside the literal keeps the compatibility terminal
// helper spelling, so the lambda's failure edges are unreachable in text.
void testInlineClosureChainEmission() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-closure-chain.gti", R"(
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
  const lang::CppMirBodyEmissionMap map(lang::buildCppMirBodyEmissionMapRows(
      frontend.semantics, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready(),
         "the closure-chain entry should be analysis-Ready under production "
         "rows");
  expect(emitter.supportsFailureBodyText(address),
         "the closure-chain entry should prove its failure-form text");
  for (const lang::MirLambdaInstance &lambda :
       frontend.mir.lambdaInstances()) {
    expect(emitter.supportsBodyText({.kind = lang::MirBodyKind::Lambda,
                                     .owner = lambda.id}),
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
  expect(contains("::gti_internal::backend::add(") &&
             contains("::gti_internal::backend::multiply("),
         "checked arithmetic inside the literals should keep the "
         "compatibility terminal helper spelling");
  expect(!contains("mir_checked_multiply_v1") &&
             !contains("mir_checked_add_v1"),
         "the literal interiors must not adopt the transformed checked "
         "helpers");
  expect(contains("rejoins the fused closure chain") &&
             contains("joins the fused chain") &&
             contains("spells at its consuming invocation"),
         "the fused chain instructions should spell as comments only");
}

// Fusing the literal to a later invocation is sound only while every
// captured place stays frozen after the Closure; a capture rewritten
// between creation and invocation must decline fail-closed.
void testClosureCaptureFreezeDeclines() {
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
  const lang::CppMirBodyEmissionMap map(lang::buildCppMirBodyEmissionMapRows(
      frontend.semantics, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(!emitter.supportsBodyText(address) &&
             !emitter.supportsFailureBodyText(address),
         "a capture rewritten after the Closure must keep the body outside "
         "the fused-chain vocabulary");
}

int main() {
  testExhaustiveEnumClassification();
  testReadyBodyAndRepresentationFailures();
  testHostedStartupOwnsRemainingAuthorityGap();
  testVerifiedProgramInitializationPlanClassification();
  testSafeIndexUsesBoundsNotRawMemory();
  testConcreteGenericFieldOwner();
  testAmbiguousStaticStorageFailsClosed();
  testOwningCheckedBodyNeedsWholeCleanupProof();
  testExampleCorpusEmissionReadiness();
  testGeneralTextStepMatchesProductionEmission();
  testCleanupFixtureFunctionBodiesAreReady();
  testOwnedLifecycleConstructionBodiesReady();
  testDischargedStorageReadAnalysis();
  testInlineClosureChainEmission();
  testClosureCaptureFreezeDeclines();

  if (failures != 0) {
    std::cerr << failures << " cpp MIR body-emitter test(s) failed\n";
    return 1;
  }
  std::cout << "cpp MIR body-emitter tests passed\n";
  return 0;
}
