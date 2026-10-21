#pragma once

#include "../src/compiler/cpp_mir_body_emitter.h"
#include "../src/compiler/cpp_mir_representation_snapshot.h"

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/mir_dominance.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
#include "gti/optimization/loops.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace gti_test::cpp_mir_body_emitter_fixture {

int failures = 0;
std::optional<std::filesystem::path> corpusSource;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::vector<std::filesystem::path> exampleCorpusSources() {
  if (corpusSource.has_value()) {
    if (std::filesystem::is_regular_file(*corpusSource) &&
        corpusSource->extension() == ".gti") {
      return {*corpusSource};
    }
    return {};
  }

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
  return sources;
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
          lang::CppStandard standard = lang::CppStandard::Cpp23,
          const lang::TargetInfo &target = lang::TargetInfo::host()) {
  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, mir, target);
  return lang::buildCppMirBodyEmissionMapRows(lowered, standard);
}

// A function the structural failure-freedom proof accepts has only its plain
// form; query the body form the function actually selects.
bool supportsSelectedBodyText(const lang::MirProgram &program,
                              const lang::CppMirBodyEmissionMap &rows,
                              lang::HirFunctionInstanceId id) {
  const lang::MirFunctionInstance *function = program.findFunctionInstance(id);
  const lang::CppMirBodyEmitter emitter(program, rows);
  const lang::MirBodyAddress address{lang::MirBodyKind::Function, id};
  return function != nullptr && (function->mayRaiseDefinedFailure
                                     ? emitter.supportsFailureBodyText(address)
                                     : emitter.supportsBodyText(address));
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
  case lang::SemanticType::FunctionItem:
    return lang::CppMirTypeRepresentationKind::FunctionItem;
  case lang::SemanticType::Lambda:
    return lang::CppMirTypeRepresentationKind::Lambda;
  case lang::SemanticType::UniqueOwner:
    return lang::CppMirTypeRepresentationKind::UniqueOwner;
  case lang::SemanticType::SharedPointer:
    return lang::CppMirTypeRepresentationKind::SharedPointer;
  case lang::SemanticType::TypeParameter:
  case lang::SemanticType::TypePack:
  case lang::SemanticType::TypeName:
  case lang::SemanticType::FunctionSignature:
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
  for (const lang::MirClassInstance &instance : program.classInstances()) {
    const std::array<const lang::MirBody *, 2> initializerBodies{
        &instance.fieldInitializers, &instance.staticFieldInitializers};
    for (const lang::MirBody *body : initializerBodies) {
      for (const lang::MirBlock &block : body->blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Construct ||
              !instruction.constructorTarget ||
              instruction.constructorKind != lang::ConstructorKind::Ordinary) {
            continue;
          }
          const lang::MirConstructorInstance *constructor =
              program.findConstructorInstance(*instruction.constructorTarget);
          const lang::MirClassInstance *owner =
              constructor == nullptr
                  ? nullptr
                  : program.findClassInstance(constructor->owner);
          if (constructor == nullptr || owner == nullptr ||
              constructor->definitionKind != lang::MirDefinitionKind::Source ||
              !constructor->mayRaiseDefinedFailure ||
              std::any_of(
                  rows.containedConstructors.begin(),
                  rows.containedConstructors.end(),
                  [&](const lang::CppMirContainedConstructorRepresentation
                          &row) {
                    return row.constructor == constructor->id;
                  })) {
            continue;
          }
          rows.containedConstructors.push_back(
              {.constructor = constructor->id,
               .ownerType = owner->type,
               .parameterTypes = constructor->parameterTypes,
               .tagSpelling =
                   lang::cppMirContainedConstructorTagSpelling(constructor->id),
               .stateSpelling = "::gti_internal::backend::"
                                "mir_contained_constructor_state_v1"});
        }
      }
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

struct NamedTest {
  std::string_view name;
  void (*run)();
};

template <std::size_t N>
int runNamedTests(int argc, char **argv, const NamedTest (&tests)[N],
                  std::string_view suite) {
  if (argc == 2 && std::string_view(argv[1]) == "--list") {
    for (const NamedTest &test : tests) {
      std::cout << test.name << '\n';
    }
    return 0;
  }
  if (argc == 3 && std::string_view(argv[1]) == "--test") {
    const std::string_view requested = argv[2];
    for (const NamedTest &test : tests) {
      if (test.name == requested) {
        test.run();
        return failures == 0 ? 0 : 1;
      }
    }
    std::cerr << "unknown " << suite << " test: " << requested << '\n';
    return 2;
  }
  if (argc != 1) {
    std::cerr << "usage: " << argv[0] << " [--list | --test NAME]\n";
    return 2;
  }
  for (const NamedTest &test : tests) {
    test.run();
  }
  if (failures != 0) {
    std::cerr << failures << ' ' << suite << " emitter test(s) failed\n";
    return 1;
  }
  std::cout << suite << " emitter tests passed (" << N << ")\n";
  return 0;
}

} // namespace gti_test::cpp_mir_body_emitter_fixture

using namespace gti_test::cpp_mir_body_emitter_fixture;
