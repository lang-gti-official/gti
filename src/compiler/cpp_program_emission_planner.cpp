#include "cpp_program_emission_plan.h"

#include "cpp_failure_support_writer.h"
#include "cpp_mir_body_emitter.h"
#include "cpp_mir_program_plan.h"
#include "cpp_mir_representation_snapshot.h"
#include "cpp_representation.h"
#include "cpp_support_preamble.h"
#include "gti/backend.h"
#include "gti/lowered_program.h"
#include "gti/mir.h"

#include <algorithm>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace lang {

class CppProgramEmissionPlanner final {
public:
  explicit CppProgramEmissionPlanner(const LoweredProgram &program,
                                     CppStandard standard)
      : standard(standard), mir(program.mir()), loweredProgram(program) {
    const std::vector<LoweredProgramIssue> loweredIssues =
        verifyLoweredProgram(loweredProgram);
    if (!loweredIssues.empty()) {
      throw std::logic_error(
          "C++ emitter requires a verified lowered program: " +
          loweredIssues.front().detail);
    }

    CppMirRepresentationSnapshotBuild snapshot =
        buildCppMirRepresentationSnapshot(loweredProgram, standard);
    if (!snapshot.valid()) {
      const auto internal =
          std::find_if(snapshot.issues.begin(), snapshot.issues.end(),
                       [](const CppMirRepresentationSnapshotIssue &issue) {
                         return issue.kind !=
                                CppMirRepresentationSnapshotIssueKind::
                                    KnownBackendLimitation;
                       });
      if (internal == snapshot.issues.end() && !snapshot.issues.empty()) {
        throw BackendUnsupportedError(snapshot.issues.front().detail);
      }
      const std::string detail = internal == snapshot.issues.end()
                                     ? "unknown snapshot-builder failure"
                                     : internal->detail;
      throw std::logic_error(
          "C++ emitter requires a coherent representation snapshot: " + detail);
    }
    generalEmissionPlan = std::move(*snapshot.emissionPlan);
    programPlan = planCppMirProgram(mir, std::move(*snapshot.snapshot));
    if (selectCppMirBackendProgramRoute(programPlan) !=
            CppMirBackendProgramRoute::VerifiedMir ||
        !programPlan.complete()) {
      throw std::logic_error(
          "C++ emitter requires a complete generated-item program plan");
    }
    generalEmissionMap.emplace(
        buildCppMirBodyEmissionMapRows(loweredProgram, standard));
  }

  CppProgramEmissionPlan build() {
    output.str("");
    output.clear();
    indentation = 0;
    generalBodyEmitter.reset();
    moduleGeneralInitializers.clear();
    moduleGeneralSupported = false;
    moduleRuntimeInitializedSymbols.clear();
    moduleRuntimeEmission = false;
    moduleRuntimeMayRaise = false;
    loweredDeclarationChildren.clear();
    loweredForwardedLinkage.clear();
    loweredNamespaceStaticSymbols.clear();
    loweredDeferredStaticFields.clear();
    loweredDeferredMembers.clear();
    loweredDeferredLifecycleClasses.clear();
    loweredEmittedTypes.clear();
    loweredConstexprAggregateClasses.clear();

    const CppSupportPreambleFeatures supportFeatures{
        .standard = standard,
        .expected = loweredProgramContainsTypeKind(SemanticType::Expected),
        .storage = true,
        .managedThreads = loweredProgramContainsManagedThreads(),
        .definedFailure = true,
        .runtimeBinding = loweredProgramContainsRuntimeBinding(),
        .cAbiTextView = loweredProgramContainsCAbiTextView(),
    };
    CppProgramEmissionPlan plan;
    capture(plan, CppProgramTextRole::SupportPreamble,
            [&] { writeCppSupportPreamble(output, supportFeatures); });
    capture(plan, CppProgramTextRole::FailureAndLifecycleSupport, [&] {
      writeCppFailureArtifactDescriptor(output, mir.failureMetadata());
    });
    capture(plan, CppProgramTextRole::NativeAndThreadAdapter,
            [&] { emitThreadTaskAdapterDefinitions(); });
    capture(plan, CppProgramTextRole::FailureAndLifecycleSupport,
            [&] { writeCppMirFailureHelpers(output); });
    capture(plan, CppProgramTextRole::NativeAndThreadAdapter,
            [&] { writeCppCAbiTextViewAdapter(output, supportFeatures); });
    capture(plan, CppProgramTextRole::DeclarationTypeMemberLifecycle,
            [&] { emitLoweredUserMacroIsolation(); });
    buildLoweredProgramTail(plan);
    return plan;
  }

  struct LoweredClassInitializerSelection {
    std::vector<HirClassInstanceId> instances;
    std::unordered_map<SymbolId, std::string> fields;
    bool fieldsSupported = false;
    bool staticsVerifiedEmpty = false;
  };

  [[nodiscard]] const std::vector<const LoweredDeclaration *> &
  loweredChildren(LoweredDeclarationId parent) const {
    static const std::vector<const LoweredDeclaration *> empty;
    return parent < loweredDeclarationChildren.size()
               ? loweredDeclarationChildren[parent]
               : empty;
  }

  void indexLoweredDeclarations() {
    loweredDeclarationChildren.assign(loweredProgram.declarations().size() + 1,
                                      {});
    for (const LoweredDeclaration &declaration :
         loweredProgram.declarations()) {
      if (declaration.parent >= loweredDeclarationChildren.size()) {
        throw std::logic_error(
            "lowered declaration parent is outside the declaration tree");
      }
      loweredDeclarationChildren[declaration.parent].push_back(&declaration);
    }
  }

  [[nodiscard]] static bool loweredTypeContainsKind(const SemanticType &type,
                                                    SemanticType::Kind kind) {
    if (type.kind == kind) {
      return true;
    }
    const auto contains = [&](const std::vector<SemanticType> &arguments) {
      return std::any_of(arguments.begin(), arguments.end(),
                         [&](const SemanticType &argument) {
                           return loweredTypeContainsKind(argument, kind);
                         });
    };
    return contains(type.arguments) ||
           contains(type.lambdaEnclosingClassTypes) ||
           contains(type.lambdaEnclosingFunctionTypes);
  }

  [[nodiscard]] bool
  loweredProgramContainsTypeKind(SemanticType::Kind kind) const {
    if (std::any_of(loweredProgram.symbols().begin(),
                    loweredProgram.symbols().end(),
                    [&](const LoweredSymbol &symbol) {
                      return loweredTypeContainsKind(symbol.type, kind);
                    })) {
      return true;
    }
    for (const LoweredDeclaration &row : loweredProgram.declarations()) {
      if (const auto *alias =
              std::get_if<LoweredTypeAliasDeclaration>(&row.payload);
          alias != nullptr && loweredTypeContainsKind(alias->type, kind)) {
        return true;
      }
      if (const auto *enumeration =
              std::get_if<LoweredEnumDeclaration>(&row.payload)) {
        if (loweredTypeContainsKind(enumeration->underlyingType, kind) ||
            std::any_of(enumeration->enumerators.begin(),
                        enumeration->enumerators.end(),
                        [&](const LoweredEnumerator &enumerator) {
                          return std::any_of(enumerator.payloadTypes.begin(),
                                             enumerator.payloadTypes.end(),
                                             [&](const SemanticType &type) {
                                               return loweredTypeContainsKind(
                                                   type, kind);
                                             });
                        })) {
          return true;
        }
      }
      if (const auto *function =
              std::get_if<LoweredFunctionDeclaration>(&row.payload)) {
        if (loweredTypeContainsKind(function->returnType, kind) ||
            std::any_of(function->parameters.begin(),
                        function->parameters.end(),
                        [&](const LoweredParameter &parameter) {
                          return loweredTypeContainsKind(parameter.type, kind);
                        })) {
          return true;
        }
      }
      if (const auto *constructor =
              std::get_if<LoweredConstructorDeclaration>(&row.payload);
          constructor != nullptr &&
          std::any_of(constructor->parameters.begin(),
                      constructor->parameters.end(),
                      [&](const LoweredParameter &parameter) {
                        return loweredTypeContainsKind(parameter.type, kind);
                      })) {
        return true;
      }
      if (const auto *storage =
              std::get_if<LoweredStorageDeclaration>(&row.payload);
          storage != nullptr && loweredTypeContainsKind(storage->type, kind)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool loweredProgramContainsRuntimeBinding() const {
    return std::any_of(
        loweredProgram.declarations().begin(),
        loweredProgram.declarations().end(), [](const LoweredDeclaration &row) {
          const auto *function =
              std::get_if<LoweredFunctionDeclaration>(&row.payload);
          return function != nullptr &&
                 function->definitionKind == MirDefinitionKind::RuntimeBinding;
        });
  }

  [[nodiscard]] bool loweredProgramContainsManagedThreads() const {
    for (const MirBodyAddress address : enumerateMirBodyAddresses(mir)) {
      const MirBody *body = findMirBody(mir, address);
      if (body == nullptr) {
        continue;
      }
      for (const MirBlock &block : body->blocks) {
        for (const MirInstruction &instruction : block.instructions) {
          switch (instruction.intrinsic) {
          case IntrinsicKind::ThreadHandleEmpty:
          case IntrinsicKind::ThreadSpawn:
          case IntrinsicKind::ThreadJoinable:
          case IntrinsicKind::ThreadJoin:
          case IntrinsicKind::ThreadAutoJoin:
            return true;
          default:
            break;
          }
        }
      }
    }
    return false;
  }

  [[nodiscard]] bool loweredProgramContainsCAbiTextView() const {
    return std::any_of(
        loweredProgram.declarations().begin(),
        loweredProgram.declarations().end(),
        [&](const LoweredDeclaration &row) {
          const auto *function =
              std::get_if<LoweredFunctionDeclaration>(&row.payload);
          return function != nullptr &&
                 function->linkage == LanguageLinkage::C &&
                 (loweredTypeContainsKind(function->returnType,
                                          SemanticType::StringView) ||
                  std::any_of(function->parameters.begin(),
                              function->parameters.end(),
                              [&](const LoweredParameter &parameter) {
                                return loweredTypeContainsKind(
                                    parameter.type, SemanticType::StringView);
                              }));
        });
  }

  void emitLoweredUserMacroIsolation() {
    std::vector<std::string> names;
    names.reserve(loweredProgram.symbols().size());
    for (const LoweredSymbol &symbol : loweredProgram.symbols()) {
      if (validMacroName(symbol.name) &&
          !std::string_view(symbol.name).starts_with("__gti_")) {
        names.push_back(symbol.name);
      }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    if (names.empty()) {
      return;
    }
    output << "\n// Host headers must not rewrite valid GTI identifiers.\n";
    for (const std::string &name : names) {
      output << "#undef " << name << '\n';
    }
    output << '\n';
  }

  [[nodiscard]] static std::string_view
  loweredNamespaceSpelling(std::string_view name, bool topLevel) {
    return topLevel && name == "std" ? cppEmittedStandardNamespace : name;
  }

  void emitLoweredTemplateDeclaration(
      const std::vector<LoweredGenericParameter> &parameters,
      bool threadTaskAdapter = false) {
    if (parameters.empty() && !threadTaskAdapter) {
      return;
    }
    writeIndent();
    output << "template <";
    bool separator = false;
    if (threadTaskAdapter) {
      output << "std::uint64_t __gti_thread_task_adapter";
      separator = true;
    }
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      if (separator) {
        output << ", ";
      }
      const LoweredGenericParameter &parameter = parameters[index];
      if (parameter.value) {
        output << "std::size_t " << parameter.name;
      } else {
        output << "typename" << (parameter.pack ? "... " : " ")
               << parameter.name;
      }
      separator = true;
    }
    output << ">\n";
  }

  void emitLoweredParameter(const LoweredParameter &parameter,
                            std::string_view name) {
    const bool reference = parameter.type.kind == SemanticType::Reference;
    const bool pointerRepresentation =
        parameter.type.kind == SemanticType::RawPointer ||
        parameter.type.kind == SemanticType::CString;
    const LoweredSymbol *symbol =
        parameter.symbol == 0 ? nullptr
                              : loweredProgram.findSymbol(parameter.symbol);
    const bool moveOnly =
        (symbol != nullptr && isMoveOnlyOwner(symbol->traits)) ||
        parameter.type.kind == SemanticType::UniqueOwner ||
        parameter.type.kind == SemanticType::Storage ||
        parameter.type.kind == SemanticType::PrefixStorage ||
        parameter.type.kind == SemanticType::TypeParameter ||
        parameter.type.kind == SemanticType::TypePack;
    if (!reference && parameter.mutability == Mutability::Immutable &&
        !parameter.pack && !moveOnly && !pointerRepresentation &&
        !parameter.explicitlyMoved) {
      output << "const ";
    }
    emitSemanticType(parameter.type);
    if (parameter.pack) {
      output << "...";
    }
    if (!name.empty()) {
      output << ' ' << name;
    }
  }

  void emitLoweredExactCallValueType(const SemanticType &type) {
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1) {
      emitSemanticType(type.arguments.front());
      return;
    }
    emitSemanticType(type);
  }

  void emitLoweredExactCallParameter(const SemanticType &type) {
    output << "::gti_internal::backend::call_parameter<"
              "::gti_internal::backend::callable_binding::";
    if (type.kind != SemanticType::Reference) {
      output << "value";
    } else if (type.referenceAccess == AccessMode::Mutable) {
      output << "mutable_reference";
    } else {
      output << "read_reference";
    }
    output << ", ";
    emitLoweredExactCallValueType(type);
    output << '>';
  }

  void emitLoweredParameters(const std::vector<LoweredParameter> &parameters,
                             bool mirArgumentNames = false) {
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      emitLoweredParameter(parameters[index],
                           mirArgumentNames
                               ? "__gti_mir_arg_" + std::to_string(index)
                               : parameters[index].name);
    }
  }

  std::size_t
  emitLoweredMirParameters(std::span<const SemanticType> parameterTypes) {
    const std::optional<std::vector<SemanticType>> flattened =
        cppMirFlattenConcreteParameterTypes(parameterTypes);
    if (!flattened) {
      throw std::logic_error(
          "lowered C++ signature contains a symbolic or misplaced type pack");
    }
    for (std::size_t index = 0; index < flattened->size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      emitSemanticType((*flattened)[index]);
      output << " __gti_mir_arg_" << index;
    }
    return flattened->size();
  }

  [[nodiscard]] const LoweredDeclaration *loweredClassRow(ClassId id) const {
    if (id == 0) {
      return nullptr;
    }
    const auto found =
        std::find_if(loweredProgram.declarations().begin(),
                     loweredProgram.declarations().end(),
                     [id](const LoweredDeclaration &declaration) {
                       const auto *value = std::get_if<LoweredClassDeclaration>(
                           &declaration.payload);
                       return value != nullptr && value->id == id;
                     });
    return found == loweredProgram.declarations().end() ? nullptr : &*found;
  }

  [[nodiscard]] const LoweredDeclaration *
  loweredFunctionRow(FunctionId id) const {
    if (id == 0) {
      return nullptr;
    }
    const auto found = std::find_if(
        loweredProgram.declarations().begin(),
        loweredProgram.declarations().end(),
        [id](const LoweredDeclaration &declaration) {
          const auto *value =
              std::get_if<LoweredFunctionDeclaration>(&declaration.payload);
          return value != nullptr && value->id == id;
        });
    return found == loweredProgram.declarations().end() ? nullptr : &*found;
  }

  [[nodiscard]] std::vector<const MirFunctionInstance *>
  loweredFunctionInstances(FunctionId declaration) const {
    std::vector<const MirFunctionInstance *> result;
    for (const MirFunctionInstance &instance : mir.functionInstances()) {
      if (instance.declaration == declaration) {
        result.push_back(&instance);
      }
    }
    return result;
  }

  [[nodiscard]] std::vector<const MirConstructorInstance *>
  loweredConstructorInstances(ConstructorId declaration) const {
    std::vector<const MirConstructorInstance *> result;
    for (const LoweredConstructorInstance &instance :
         loweredProgram.constructorInstances()) {
      if (instance.declaration != declaration) {
        continue;
      }
      const MirConstructorInstance *mirInstance =
          mir.findConstructorInstance(instance.id);
      if (mirInstance != nullptr) {
        result.push_back(mirInstance);
      }
    }
    return result;
  }

  [[nodiscard]] std::optional<LoweredClassInitializerSelection>
  loweredClassInitializers(const LoweredClassDeclaration &declaration) const {
    std::vector<const MirClassInstance *> instances;
    for (const MirClassInstance &candidate : mir.classInstances()) {
      if (candidate.declaration == declaration.id) {
        instances.push_back(&candidate);
      }
    }
    if (instances.empty()) {
      return std::nullopt;
    }

    const bool fixedRepresentation =
        declaration.kind == ClassKind::Union || declaration.cAbiRecord;
    LoweredClassInitializerSelection result;
    result.fieldsSupported = true;
    result.staticsVerifiedEmpty = true;
    bool first = true;
    for (const MirClassInstance *instance : instances) {
      result.instances.push_back(instance->id);
      const CppMirInitializerScheduleText fields =
          generalMirInitializerSchedule(
              {.kind = MirBodyKind::FieldInitializers, .owner = instance->id});
      std::unordered_map<SymbolId, std::string> current;
      if (fields.supported) {
        for (const CppMirFieldInitializerSpelling &field : fields.fields) {
          current.emplace(field.field, field.spelling);
        }
        if (first) {
          result.fields = std::move(current);
        } else if (current != result.fields) {
          result.fieldsSupported = false;
        }
      } else {
        result.fieldsSupported = false;
      }
      const CppMirInitializerScheduleText statics =
          generalMirInitializerSchedule(
              {.kind = MirBodyKind::StaticFieldInitializers,
               .owner = instance->id});
      if (!statics.supported || !statics.fields.empty()) {
        result.staticsVerifiedEmpty = false;
      }
      first = false;
    }
    if (fixedRepresentation && result.fieldsSupported &&
        !std::all_of(result.fields.begin(), result.fields.end(),
                     [](const auto &field) { return field.second.empty(); })) {
      result.fieldsSupported = false;
    }
    if (!result.fieldsSupported) {
      result.fields.clear();
    }
    return result;
  }

  void prepareLoweredModuleInitialization() {
    moduleGeneralInitializers.clear();
    moduleGeneralSupported = false;
    moduleRuntimeInitializedSymbols.clear();
    moduleRuntimeEmission = false;
    const MirBodyAddress moduleAddress{.kind = MirBodyKind::Module, .owner = 0};
    const bool hasExecutableProgramInitialization = std::any_of(
        mir.programInitializationPlan().steps.begin(),
        mir.programInitializationPlan().steps.end(), [](const auto &step) {
          return step.role == ProgramInitializationStepRole::Initializer;
        });
    moduleRuntimeMayRaise = cppMirModuleMayRaiseDefinedFailure(mir);
    const CppMirInitializerScheduleText schedule =
        generalMirInitializerSchedule(moduleAddress);
    if (!moduleRuntimeMayRaise && schedule.supported) {
      moduleGeneralSupported = true;
      for (const CppMirFieldInitializerSpelling &field : schedule.fields) {
        moduleGeneralInitializers.emplace(field.field, field.spelling);
      }
      return;
    }
    if (!hasExecutableProgramInitialization) {
      if (!schedule.supported) {
        throw std::logic_error(
            "lowered C++ emission has no MIR module initializer schedule");
      }
      return;
    }
    const CppMirBodyEmissionPlan &modulePlan =
        generalMirBodyPlan(moduleAddress);
    const CppMirBodyEmissionAnalysis &analysis = modulePlan.analysis();
    const bool supported = moduleRuntimeMayRaise
                               ? modulePlan.supportsFailureText()
                               : modulePlan.supportsPlainText();
    if (!analysis.ready() || !supported) {
      throw std::logic_error(
          "lowered C++ emission has no MIR runtime module body");
    }
    moduleRuntimeEmission = true;
    for (const MirProgramInitializationStep &step :
         mir.programInitializationPlan().steps) {
      if (step.role == ProgramInitializationStepRole::Initializer) {
        moduleRuntimeInitializedSymbols.insert(step.symbol);
      }
    }
  }

  [[nodiscard]] std::string
  loweredNativeQualifiedName(const LoweredDeclaration &declaration) const {
    std::string result = "::";
    for (std::size_t index = 0; index < declaration.namespaceScope.size();
         ++index) {
      result += std::string(loweredNamespaceSpelling(
          declaration.namespaceScope[index], index == 0));
      result += "::";
    }
    result += declaration.name;
    return result;
  }

  void emitLoweredNamePath(const std::vector<std::string> &path) {
    for (std::size_t index = 0; index < path.size(); ++index) {
      if (index != 0) {
        output << "::";
      }
      output << loweredNamespaceSpelling(path[index], index == 0);
    }
  }

  [[nodiscard]] bool loweredClassHasConsumingCallOperator(
      const LoweredDeclaration &classRow) const {
    return std::any_of(
        loweredChildren(classRow.id).begin(),
        loweredChildren(classRow.id).end(),
        [](const LoweredDeclaration *member) {
          const auto *function =
              member == nullptr
                  ? nullptr
                  : std::get_if<LoweredFunctionDeclaration>(&member->payload);
          return function != nullptr && function->overloadedOperator &&
                 *function->overloadedOperator == OverloadedOperator::Call &&
                 function->receiverMutability == ReceiverMutability::Consuming;
        });
  }

  void emitLoweredFailureResultParameter(const SemanticType &returnType) {
    if (returnType.kind == SemanticType::Reference &&
        returnType.arguments.size() == 1) {
      if (returnType.referenceAccess == AccessMode::ReadOnly) {
        output << "const ";
      }
      emitSemanticType(returnType.arguments.front());
      output << " **__gti_mir_out_result, ";
    } else if (returnType != SemanticType::Void) {
      emitSemanticType(returnType);
      output << " *__gti_mir_out_result, ";
    }
  }

  void emitLoweredFunctionSignature(const LoweredDeclaration &row,
                                    const LoweredFunctionDeclaration &function,
                                    const MirFunctionInstance *instance,
                                    bool failure, bool insideClass = false,
                                    std::string_view owner = {},
                                    bool sourceParameterTypes = false,
                                    bool explicitGenericArguments = false) {
    if (insideClass && function.staticMember) {
      output << "static ";
    }
    if (insideClass && function.virtualMethod && !function.overrideMethod &&
        !function.staticMember) {
      output << "virtual ";
    }
    if (failure) {
      output << "bool ";
    } else {
      const SemanticType &returnType =
          instance == nullptr ? function.returnType : instance->returnType;
      emitSemanticType(returnType);
      if (returnType.kind != SemanticType::Reference) {
        output << ' ';
      }
    }
    if (!owner.empty()) {
      output << owner << "::";
    }
    const std::string name = cppFunctionSpelling(function, row.name);
    output << (failure ? cppMirFailureSiblingSpelling(name) : name);
    if (!failure && explicitGenericArguments && instance != nullptr) {
      const LoweredFunctionInstance *concrete =
          loweredFunctionInstance(instance->id);
      if (concrete == nullptr || (concrete->typeArguments.empty() &&
                                  concrete->valueArguments.empty())) {
        throw std::logic_error(
            "lowered member specialization lost its generic arguments");
      }
      emitLoweredExplicitArguments(*concrete);
    }
    output << '(';
    std::size_t parameterCount = 0;
    if (instance != nullptr && !sourceParameterTypes) {
      parameterCount = emitLoweredMirParameters(instance->parameterTypes);
    } else {
      emitLoweredParameters(function.parameters, sourceParameterTypes);
      parameterCount = function.parameters.size();
    }
    if (failure) {
      if (parameterCount != 0) {
        output << ", ";
      }
      emitLoweredFailureResultParameter(
          instance == nullptr ? function.returnType : instance->returnType);
      output << "::gti_failure_record_v1 *__gti_mir_failure_record";
    }
    output << ')';
    const ReceiverMutability receiver = instance == nullptr
                                            ? function.receiverMutability
                                            : instance->receiverMutability;
    if ((insideClass || !owner.empty()) && !function.staticMember) {
      const LoweredDeclaration *classRow = loweredClassRow(function.ownerClass);
      const bool refQualified =
          function.overloadedOperator &&
          *function.overloadedOperator == OverloadedOperator::Call &&
          classRow != nullptr &&
          loweredClassHasConsumingCallOperator(*classRow);
      if (receiver == ReceiverMutability::ReadOnly) {
        output << " const";
        if (refQualified) {
          output << " &";
        }
      } else if (receiver == ReceiverMutability::Mutable && refQualified) {
        output << " &";
      } else if (receiver == ReceiverMutability::Consuming) {
        output << " &&";
      }
    }
    if (insideClass && function.overrideMethod) {
      output << " override";
    }
  }

  void emitLoweredCAbiType(const SemanticType &type) {
    if (type == SemanticType::StringView) {
      output << "::gti_c_string_view";
    } else {
      emitSemanticType(type);
    }
  }

  void emitLoweredCAbiSignature(const LoweredDeclaration &row,
                                const LoweredFunctionDeclaration &function) {
    emitLoweredCAbiType(function.returnType);
    output << ' '
           << (function.externalSymbol.empty() ? row.name
                                               : function.externalSymbol)
           << '(';
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      emitLoweredCAbiType(function.parameters[index].type);
      if (!function.parameters[index].name.empty()) {
        output << ' ' << function.parameters[index].name;
      }
    }
    output << ')';
  }

  [[nodiscard]] bool
  loweredNormalBodySupported(const MirFunctionInstance &instance) const {
    return instance.definitionKind == MirDefinitionKind::Source &&
           generalMirBodyPlan(
               {.kind = MirBodyKind::Function, .owner = instance.id})
               .supportsPlainText();
  }

  [[nodiscard]] bool
  loweredFailureBodySupported(const MirFunctionInstance &instance) const {
    return instance.definitionKind == MirDefinitionKind::Source &&
           generalMirBodyPlan(
               {.kind = MirBodyKind::Function, .owner = instance.id})
               .supportsFailureText();
  }

  [[nodiscard]] bool
  loweredVirtualFailureContractRetained(FunctionId declaration) const {
    for (const MirFunctionInstance &candidate : mir.functionInstances()) {
      if (candidate.declaration != declaration ||
          cppMirVirtualFailureContractRoot(mir, candidate) != &candidate) {
        continue;
      }
      bool implementation = !candidate.pureVirtual;
      if (!candidate.pureVirtual && !loweredFailureBodySupported(candidate)) {
        continue;
      }
      bool complete = true;
      for (const MirFunctionInstance &override : mir.functionInstances()) {
        if (override.id == candidate.id ||
            cppMirVirtualFailureContractRoot(mir, override) != &candidate) {
          continue;
        }
        implementation = true;
        if (!loweredFailureBodySupported(override)) {
          complete = false;
          break;
        }
      }
      if (implementation && complete) {
        return true;
      }
    }
    return false;
  }

  void emitLoweredPublicOpaqueHandleForwardDeclarations(
      LoweredDeclarationId parent = 0) {
    bool emitted = false;
    for (const LoweredDeclaration *declaration : loweredChildren(parent)) {
      if (declaration->kind == LoweredDeclarationKind::Namespace) {
        const bool contains = std::any_of(
            loweredChildren(declaration->id).begin(),
            loweredChildren(declaration->id).end(),
            [&](const LoweredDeclaration *child) {
              const auto *type =
                  std::get_if<LoweredClassDeclaration>(&child->payload);
              return child->kind == LoweredDeclarationKind::Namespace ||
                     (type != nullptr && type->cOpaqueHandle &&
                      !type->compilerPrivate);
            });
        if (!contains) {
          continue;
        }
        writeIndent();
        output << "namespace "
               << loweredNamespaceSpelling(declaration->name, parent == 0)
               << " {\n";
        ++indentation;
        emitLoweredPublicOpaqueHandleForwardDeclarations(declaration->id);
        --indentation;
        writeIndent();
        output << "}\n";
        emitted = true;
        continue;
      }
      const auto *type =
          std::get_if<LoweredClassDeclaration>(&declaration->payload);
      if (type == nullptr || !type->cOpaqueHandle || type->compilerPrivate) {
        continue;
      }
      writeIndent();
      output << "struct " << declaration->name << ";\n";
      emitted = true;
    }
    if (parent == 0 && emitted) {
      output << '\n';
    }
  }

  void emitLoweredTypeForwardDeclarations(LoweredDeclarationId parent) {
    for (const LoweredDeclaration *declaration : loweredChildren(parent)) {
      if (declaration->kind == LoweredDeclarationKind::Namespace) {
        writeIndent();
        output << "namespace "
               << loweredNamespaceSpelling(declaration->name, parent == 0)
               << " {\n";
        ++indentation;
        emitLoweredTypeForwardDeclarations(declaration->id);
        --indentation;
        writeIndent();
        output << "}\n";
        continue;
      }
      if (const auto *type =
              std::get_if<LoweredClassDeclaration>(&declaration->payload)) {
        if (type->compilerCapability != CompilerCapabilityTypeKind::None) {
          continue;
        }
        if (type->exactSpecializationPrimary != 0) {
          continue;
        }
        if (type->cOpaqueHandle) {
          writeIndent();
          output << "using " << declaration->name << " = "
                 << loweredNativeQualifiedName(*declaration) << ";\n";
          continue;
        }
        if (!type->genericParameters.empty()) {
          emitLoweredTemplateDeclaration(type->genericParameters);
        }
        writeIndent();
        output << (type->kind == ClassKind::Struct  ? "struct "
                   : type->kind == ClassKind::Union ? "union "
                                                    : "class ")
               << declaration->name << ";\n";
        continue;
      }
      if (const auto *enumeration =
              std::get_if<LoweredEnumDeclaration>(&declaration->payload)) {
        writeIndent();
        if (enumeration->payload) {
          output << "struct " << declaration->name << ";\n";
        } else {
          output << "enum class " << declaration->name << " : ";
          emitSemanticType(enumeration->underlyingType);
          output << ";\n";
        }
      }
    }
  }

  [[nodiscard]] std::string
  loweredExactClassArguments(const LoweredClassDeclaration &declaration) const {
    if (declaration.exactSpecializationPrimary == 0) {
      return {};
    }
    std::string result = "<";
    bool separator = false;
    for (const SemanticType &argument : declaration.exactTypeArguments) {
      if (separator) {
        result += ", ";
      }
      result += cppSemanticTypeSpelling(loweredProgram, standard, argument);
      separator = true;
    }
    for (const CompileTimeValue &argument : declaration.exactValueArguments) {
      if (separator) {
        result += ", ";
      }
      if (argument.kind != CompileTimeValue::UInt64) {
        throw std::logic_error(
            "exact class specialization retained a symbolic value argument");
      }
      result += std::to_string(argument.value);
      separator = true;
    }
    result += '>';
    return result;
  }

  void emitLoweredExactClassSpecializationForwardDeclarations() {
    for (const LoweredDeclaration &row : loweredProgram.declarations()) {
      const auto *type = std::get_if<LoweredClassDeclaration>(&row.payload);
      if (type == nullptr || type->exactSpecializationPrimary == 0) {
        continue;
      }
      const std::size_t namespaceCount =
          emitLoweredNamespaceOpen(row.namespaceScope);
      writeIndent();
      output << "template <>\n";
      writeIndent();
      output << (type->kind == ClassKind::Struct  ? "struct "
                 : type->kind == ClassKind::Union ? "union "
                                                  : "class ")
             << row.name << loweredExactClassArguments(*type) << ";\n";
      emitLoweredNamespaceClose(namespaceCount);
    }
  }

  void emitLoweredAliasForwardDeclarations(LoweredDeclarationId parent) {
    for (const LoweredDeclaration *declaration : loweredChildren(parent)) {
      if (declaration->kind == LoweredDeclarationKind::Namespace) {
        writeIndent();
        output << "namespace "
               << loweredNamespaceSpelling(declaration->name, parent == 0)
               << " {\n";
        ++indentation;
        emitLoweredAliasForwardDeclarations(declaration->id);
        --indentation;
        writeIndent();
        output << "}\n";
        continue;
      }
      if (const auto *alias = std::get_if<LoweredNamespaceAliasDeclaration>(
              &declaration->payload)) {
        writeIndent();
        output << "namespace " << declaration->name << " = ";
        emitLoweredNamePath(alias->target);
        output << ";\n";
      } else if (const auto *alias = std::get_if<LoweredTypeAliasDeclaration>(
                     &declaration->payload)) {
        writeIndent();
        output << "using " << declaration->name << " = ";
        emitSemanticType(alias->type);
        output << ";\n";
      }
    }
  }

  void emitLoweredBoundaryMarkers(const LoweredDeclaration &row,
                                  const LoweredFunctionDeclaration &function) {
    for (const MirFunctionInstance *instance :
         loweredFunctionInstances(function.id)) {
      if (instance->definitionKind == MirDefinitionKind::Source) {
        continue;
      }
      const MirBodyAddress address{.kind = MirBodyKind::Function,
                                   .owner = instance->id};
      if (!generalMirBodyPlan(address).boundaryDeclaration()) {
        throw std::logic_error(
            "lowered C++ emission has no boundary shell for function '" +
            row.name + "'");
      }
      writeIndent();
      output << "// GTI verified-MIR body: native-boundary-v1 "
                "function-instance "
             << instance->id << "\n";
    }
  }

  [[nodiscard]] const LoweredFunctionInstance *
  loweredFunctionInstance(HirFunctionInstanceId id) const {
    return loweredProgram.findFunctionInstance(id);
  }

  [[nodiscard]] bool loweredTypeIsNameable(const SemanticType &type) const {
    if (type == SemanticType::Unknown ||
        type.kind == SemanticType::TypeParameter ||
        type.kind == SemanticType::TypePack ||
        type.kind == SemanticType::Lambda) {
      return false;
    }
    return std::all_of(type.arguments.begin(), type.arguments.end(),
                       [&](const SemanticType &argument) {
                         return loweredTypeIsNameable(argument);
                       });
  }

  void emitLoweredExplicitArguments(const LoweredFunctionInstance &instance) {
    output << '<';
    bool separator = false;
    for (const SemanticType &argument : instance.typeArguments) {
      if (separator) {
        output << ", ";
      }
      emitSemanticType(argument);
      separator = true;
    }
    for (const CompileTimeValue &argument : instance.valueArguments) {
      if (separator) {
        output << ", ";
      }
      if (argument.kind == CompileTimeValue::UInt64) {
        output << argument.value;
      } else {
        const LoweredGenericParameter *parameter =
            loweredProgram.findGenericParameter(argument.parameterId);
        output << (parameter == nullptr ? "0" : parameter->name);
      }
      separator = true;
    }
    output << '>';
  }

  void emitLoweredFreeFunctionSpecializationSignature(
      const LoweredDeclaration &row, const LoweredFunctionDeclaration &function,
      const MirFunctionInstance &instance, bool failure) {
    const LoweredFunctionInstance *lowered =
        loweredFunctionInstance(instance.id);
    if (lowered == nullptr) {
      throw std::logic_error(
          "lowered C++ specialization lost its concrete instance");
    }
    if (failure) {
      output << "bool ";
    } else {
      output << "template <> ";
      emitSemanticType(instance.returnType);
      output << ' ';
    }
    const std::string name = cppFunctionSpelling(function, row.name);
    output << (failure ? cppMirFailureSiblingSpelling(name) : name);
    if (!failure) {
      emitLoweredExplicitArguments(*lowered);
    }
    output << '(';
    const std::size_t parameterCount =
        emitLoweredMirParameters(instance.parameterTypes);
    if (failure) {
      if (parameterCount != 0) {
        output << ", ";
      }
      emitLoweredFailureResultParameter(instance.returnType);
      output << "::gti_failure_record_v1 *__gti_mir_failure_record";
    }
    output << ')';
  }

  void emitLoweredConcreteFreeFunctionSignature(
      const LoweredDeclaration &row, const LoweredFunctionDeclaration &function,
      const MirFunctionInstance &instance, bool failure) {
    const LoweredFunctionInstance *lowered =
        loweredFunctionInstance(instance.id);
    const std::optional<std::string> name =
        lowered == nullptr
            ? std::nullopt
            : cppConcreteFreeFunctionSpelling(loweredProgram, row, function,
                                              *lowered, instance);
    if (!name) {
      throw std::logic_error(
          "lowered concrete free function lost its stable instance identity");
    }
    if (failure) {
      output << "bool ";
    } else {
      emitSemanticType(instance.returnType);
      if (instance.returnType.kind != SemanticType::Reference) {
        output << ' ';
      }
    }
    output << (failure ? cppMirFailureSiblingSpelling(*name) : *name) << '(';
    const std::size_t parameterCount =
        emitLoweredMirParameters(instance.parameterTypes);
    if (failure) {
      if (parameterCount != 0) {
        output << ", ";
      }
      emitLoweredFailureResultParameter(instance.returnType);
      output << "::gti_failure_record_v1 *__gti_mir_failure_record";
    }
    output << ')';
  }

  void emitLoweredExternCBlock(const LoweredDeclaration &linkage) {
    writeIndent();
    output << "extern \"C\" {\n";
    ++indentation;
    for (const LoweredDeclaration *declaration : loweredChildren(linkage.id)) {
      const auto *function =
          std::get_if<LoweredFunctionDeclaration>(&declaration->payload);
      if (function == nullptr) {
        continue;
      }
      writeIndent();
      emitLoweredCAbiSignature(*declaration, *function);
      output << ";\n";
      emitLoweredBoundaryMarkers(*declaration, *function);
    }
    --indentation;
    writeIndent();
    output << "}\n";
  }

  void emitLoweredFunctionForwardDeclarations(LoweredDeclarationId parent) {
    for (const LoweredDeclaration *declaration : loweredChildren(parent)) {
      if (declaration->kind == LoweredDeclarationKind::Namespace) {
        writeIndent();
        output << "namespace "
               << loweredNamespaceSpelling(declaration->name, parent == 0)
               << " {\n";
        ++indentation;
        emitLoweredFunctionForwardDeclarations(declaration->id);
        --indentation;
        writeIndent();
        output << "}\n";
        continue;
      }
      if (declaration->kind == LoweredDeclarationKind::LanguageLinkage) {
        emitLoweredExternCBlock(*declaration);
        loweredForwardedLinkage.insert(declaration->id);
        continue;
      }
      const auto *function =
          std::get_if<LoweredFunctionDeclaration>(&declaration->payload);
      if (function == nullptr || function->ownerClass != 0 ||
          function->linkage == LanguageLinkage::C) {
        continue;
      }
      emitLoweredBoundaryMarkers(*declaration, *function);
      if (function->definitionKind == MirDefinitionKind::RuntimeBinding ||
          function->intrinsic != IntrinsicKind::None) {
        continue;
      }
      const std::vector<const MirFunctionInstance *> instances =
          loweredFunctionInstances(function->id);
      const bool generic = !function->genericParameters.empty();
      const bool threadTaskTemplate =
          generic && loweredFunctionNeedsThreadTaskTemplate(instances);
      const bool hasTemplateNormal =
          generic && function->definitionKind == MirDefinitionKind::Source &&
          !loweredCallableTemplateBody(*function, instances, false).empty();
      const bool hasTemplateFailure =
          generic && function->definitionKind == MirDefinitionKind::Source &&
          !loweredCallableTemplateBody(*function, instances, true).empty();
      const bool retainPrimaryDeclaration =
          generic && function->definitionKind != MirDefinitionKind::Source;
      if (!generic || hasTemplateNormal || retainPrimaryDeclaration) {
        if (generic) {
          emitLoweredTemplateDeclaration(function->genericParameters,
                                         threadTaskTemplate);
        }
        writeIndent();
        emitLoweredFunctionSignature(*declaration, *function, nullptr, false,
                                     false, {}, generic);
        output << ";\n";
      }
      const bool hasFailure = std::any_of(
          instances.begin(), instances.end(), [&](const auto *instance) {
            return loweredFailureBodySupported(*instance);
          });
      if (hasTemplateFailure) {
        emitLoweredTemplateDeclaration(function->genericParameters,
                                       threadTaskTemplate);
        writeIndent();
        emitLoweredFunctionSignature(*declaration, *function, nullptr, true,
                                     false, {}, true);
        output << ";\n";
      } else if (!generic && !instances.empty() && hasFailure) {
        writeIndent();
        emitLoweredFunctionSignature(*declaration, *function, instances.front(),
                                     true);
        output << ";\n";
      }
      if (!generic) {
        continue;
      }
      for (const MirFunctionInstance *instance : instances) {
        const LoweredFunctionInstance *concrete =
            loweredFunctionInstance(instance->id);
        if (concrete == nullptr ||
            !std::all_of(concrete->typeArguments.begin(),
                         concrete->typeArguments.end(),
                         [&](const SemanticType &type) {
                           return loweredTypeIsNameable(type);
                         })) {
          continue;
        }
        if (loweredNormalBodySupported(*instance)) {
          writeIndent();
          emitLoweredConcreteFreeFunctionSignature(*declaration, *function,
                                                   *instance, false);
          output << ";\n";
        }
        if (loweredFailureBodySupported(*instance)) {
          writeIndent();
          emitLoweredConcreteFreeFunctionSignature(*declaration, *function,
                                                   *instance, true);
          output << ";\n";
        }
      }
    }
  }

  [[nodiscard]] bool
  loweredRuntimeProgramStorage(const LoweredStorageDeclaration &storage) const {
    return moduleRuntimeEmission && storage.symbol != 0 &&
           moduleRuntimeInitializedSymbols.contains(storage.symbol);
  }

  [[nodiscard]] const CppMirSymbolRepresentation *
  loweredStorageRepresentation(const LoweredStorageDeclaration &storage) const {
    if (!generalEmissionMap || storage.symbol == 0) {
      return nullptr;
    }
    const MirProgramInitializationStep *step =
        mir.programInitializationPlan().findStepForSymbol(storage.symbol);
    const std::size_t owner = step == nullptr ? 0 : step->ownerClass;
    const auto found = std::find_if(
        generalEmissionMap->symbols().begin(),
        generalEmissionMap->symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == CppMirSymbolRepresentationKind::Storage &&
                 row.owner == owner && row.symbol == storage.symbol &&
                 row.ordinal == 0 && row.type == storage.type;
        });
    return found == generalEmissionMap->symbols().end() ? nullptr : &*found;
  }

  [[nodiscard]] bool
  loweredRuntimeLifetimeSlot(const LoweredStorageDeclaration &storage) const {
    const CppMirSymbolRepresentation *row =
        loweredStorageRepresentation(storage);
    return row != nullptr && row->runtimeLifetimeSlot;
  }

  void emitLoweredStorageRepresentationType(
      const LoweredStorageDeclaration &storage) {
    if (loweredRuntimeLifetimeSlot(storage)) {
      output << "::gti_internal::backend::mir_lifetime_slot<";
      emitSemanticType(storage.type);
      output << ", true>";
      return;
    }
    emitSemanticType(storage.type);
  }

  void emitLoweredStorageType(const LoweredStorageDeclaration &storage,
                              bool declarationOnly) {
    const bool runtime = loweredRuntimeProgramStorage(storage);
    const bool rawPointer = storage.type.kind == SemanticType::RawPointer;
    const bool moveOnly =
        isMoveOnlyOwner(storage.traits) || containsTypeParameter(storage.type);
    const bool representationConstexpr =
        storage.constexprStorage &&
        !(standard == CppStandard::Cpp20 &&
          storage.type.kind == SemanticType::Expected);
    if (representationConstexpr && !runtime) {
      output << (declarationOnly ? "const " : "constexpr ");
    } else if (!runtime && (storage.constexprStorage ||
                            (storage.mutability == Mutability::Immutable &&
                             !moveOnly && !rawPointer))) {
      output << "const ";
    }
    emitLoweredStorageRepresentationType(storage);
  }

  void
  emitLoweredGlobalStorageForwardDeclarations(LoweredDeclarationId parent) {
    bool emitted = false;
    for (const LoweredDeclaration *declaration : loweredChildren(parent)) {
      if (declaration->kind == LoweredDeclarationKind::Namespace) {
        writeIndent();
        output << "namespace "
               << loweredNamespaceSpelling(declaration->name, parent == 0)
               << " {\n";
        ++indentation;
        emitLoweredGlobalStorageForwardDeclarations(declaration->id);
        --indentation;
        writeIndent();
        output << "}\n";
        continue;
      }
      const auto *storage =
          std::get_if<LoweredStorageDeclaration>(&declaration->payload);
      if (storage == nullptr || storage->ownerClass != 0) {
        continue;
      }
      writeIndent();
      if (storage->internalLinkage) {
        output << "struct "
               << cppStaticStorageBaseSpelling(storage->symbol,
                                               declaration->name)
               << " { static ";
        emitLoweredStorageType(*storage, true);
        output << " value; };\n";
        loweredNamespaceStaticSymbols.insert(storage->symbol);
      } else {
        output << "extern ";
        emitLoweredStorageType(*storage, true);
        output << ' ' << declaration->name << ";\n";
      }
      emitted = true;
    }
    if (parent == 0 && emitted) {
      output << '\n';
    }
  }

  void emitLoweredEnumDefinition(const LoweredDeclaration &row,
                                 const LoweredEnumDeclaration &enumeration) {
    writeIndent();
    if (enumeration.payload) {
      output << "struct " << row.name << " {\n";
      ++indentation;
      for (const LoweredEnumerator &variant : enumeration.enumerators) {
        writeIndent();
        output << "struct __gti_variant_" << variant.variantIndex << " {\n";
        ++indentation;
        for (std::size_t index = 0; index < variant.payloadTypes.size();
             ++index) {
          writeIndent();
          emitSemanticType(variant.payloadTypes[index]);
          output << " __gti_field_" << index << ";\n";
        }
        writeIndent();
        output << "friend bool operator==(const __gti_variant_"
               << variant.variantIndex << " &, const __gti_variant_"
               << variant.variantIndex << " &) = default;\n";
        --indentation;
        writeIndent();
        output << "};\n";
      }
      writeIndent();
      output << "std::variant<";
      for (std::size_t index = 0; index < enumeration.enumerators.size();
           ++index) {
        if (index != 0) {
          output << ", ";
        }
        output << "__gti_variant_"
               << enumeration.enumerators[index].variantIndex;
      }
      output << "> __gti_value;\n";
      writeIndent();
      output << "friend bool operator==(const " << row.name << " &, const "
             << row.name << " &) = default;\n";
      --indentation;
      writeIndent();
      output << "};\n";
      return;
    }

    output << "enum class " << row.name << " : ";
    emitSemanticType(enumeration.underlyingType);
    output << " {\n";
    ++indentation;
    for (std::size_t index = 0; index < enumeration.enumerators.size();
         ++index) {
      const LoweredEnumerator &enumerator = enumeration.enumerators[index];
      writeIndent();
      output << enumerator.name << " = ";
      emitSwitchInteger(enumerator.value, enumeration.underlyingType);
      output << (index + 1 == enumeration.enumerators.size() ? "\n" : ",\n");
    }
    --indentation;
    writeIndent();
    output << "};\n";
  }

  void emitLoweredStorageInitializer(
      const LoweredStorageDeclaration &storage,
      const LoweredClassInitializerSelection *classInitializers = nullptr) {
    if (loweredRuntimeProgramStorage(storage)) {
      return;
    }
    if (moduleGeneralSupported && !storage.constexprStorage) {
      const auto found = moduleGeneralInitializers.find(storage.symbol);
      if (found != moduleGeneralInitializers.end()) {
        if (!found->second.empty()) {
          output << " = " << found->second;
        }
        return;
      }
    }
    if (classInitializers != nullptr && !storage.staticStorage &&
        !storage.constexprStorage && classInitializers->fieldsSupported) {
      const auto found = classInitializers->fields.find(storage.symbol);
      if (found != classInitializers->fields.end() && !found->second.empty()) {
        output << " = " << found->second;
      }
      return;
    }
    if (storage.constexprStorage && storage.constant) {
      output << " = ";
      emitConstant(*storage.constant, &storage.type);
      return;
    }
    if (storage.constant) {
      output << " = ";
      emitConstant(*storage.constant, &storage.type);
    }
  }

  void emitLoweredCAbiFieldType(const SemanticType &type,
                                std::string_view name) {
    const SemanticType *element = &type;
    std::vector<std::uint64_t> extents;
    while (element->kind == SemanticType::Array &&
           element->arguments.size() == 1) {
      extents.push_back(element->arrayLength);
      element = &element->arguments.front();
    }
    emitSemanticType(*element);
    output << ' ' << name;
    for (const std::uint64_t extent : extents) {
      output << '[' << extent << ']';
    }
  }

  void emitLoweredStorageDeclaration(
      const LoweredDeclaration &row, const LoweredStorageDeclaration &storage,
      bool insideClass, const LoweredClassDeclaration *owner = nullptr,
      const LoweredClassInitializerSelection *classInitializers = nullptr) {
    const bool runtime = loweredRuntimeProgramStorage(storage);
    const bool rawPointer = storage.type.kind == SemanticType::RawPointer;
    const bool moveOnly =
        isMoveOnlyOwner(storage.traits) || containsTypeParameter(storage.type);
    const bool representationConstexpr =
        storage.constexprStorage &&
        !(standard == CppStandard::Cpp20 &&
          storage.type.kind == SemanticType::Expected);

    writeIndent();
    if (!insideClass && storage.internalLinkage) {
      output << "inline ";
    } else if (storage.staticStorage) {
      if (!representationConstexpr && storage.constexprStorage && insideClass) {
        output << "inline ";
      }
      output << "static ";
    }
    if (representationConstexpr && !runtime) {
      output << "constexpr ";
    } else if (!runtime &&
               (storage.constexprStorage ||
                (storage.mutability == Mutability::Immutable && !moveOnly &&
                 !rawPointer && (!insideClass || storage.staticStorage)))) {
      output << "const ";
    }

    const std::string name =
        !insideClass && storage.internalLinkage
            ? cppStaticStorageValueSpelling(storage.symbol, row.name)
            : row.name;
    if (insideClass && owner != nullptr && owner->cAbiRecord &&
        storage.type.kind == SemanticType::Array) {
      emitLoweredCAbiFieldType(storage.type, name);
    } else {
      emitLoweredStorageRepresentationType(storage);
      output << ' ' << name;
    }
    if (insideClass && storage.staticStorage && !storage.constexprStorage) {
      loweredDeferredStaticFields.push_back(&row);
      output << ";\n";
      return;
    }
    emitLoweredStorageInitializer(storage, classInitializers);
    output << ";\n";
  }

  void emitLoweredStructuralOperatorAdapter(
      const LoweredDeclaration &row, const LoweredFunctionDeclaration &function,
      const LoweredDeclaration &classRow) {
    const auto found =
        std::find_if(programPlan.thunks.begin(), programPlan.thunks.end(),
                     [&](const CppMirGeneratedThunk &thunk) {
                       return thunk.identity.kind ==
                                  CppMirThunkKind::StructuralOperatorAdapter &&
                              thunk.identity.owner == function.id;
                     });
    if (found == programPlan.thunks.end()) {
      return;
    }
    const auto *adapter =
        std::get_if<CppMirStructuralOperatorThunk>(&found->payload);
    if (adapter == nullptr) {
      throw std::logic_error(
          "lowered structural adapter lost its sealed payload");
    }
    writeIndent();
    output << "friend ";
    emitSemanticType(function.returnType);
    output << ' ' << operatorFunctionName(adapter->operation) << '(';
    if (function.receiverMutability == ReceiverMutability::ReadOnly) {
      output << "const ";
    }
    output << classRow.name << " &__gti_receiver";
    if (adapter->operation == OverloadedOperator::NotEqual &&
        !function.parameters.empty()) {
      output << ", ::gti_internal::backend::exact_type<";
      emitSemanticType(function.parameters.front().type);
      output << ">";
    }
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
      output << ", ";
      emitLoweredParameter(function.parameters[index],
                           "__gti_argument_" + std::to_string(index));
    }
    output << ") { ";
    if (function.returnType != SemanticType::Void) {
      output << "return ";
    }
    output << "__gti_receiver." << cppFunctionSpelling(function, row.name)
           << '(';
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      if (function.parameters[index].type.kind != SemanticType::Reference) {
        output << "std::move(";
      }
      output << "__gti_argument_" << index;
      if (function.parameters[index].type.kind != SemanticType::Reference) {
        output << ')';
      }
    }
    output << "); }\n";
  }

  void emitLoweredCallableAdapter(const LoweredDeclaration &row,
                                  const LoweredFunctionDeclaration &function,
                                  const LoweredDeclaration &classRow) {
    const auto found = std::find_if(
        programPlan.thunks.begin(), programPlan.thunks.end(),
        [&](const CppMirGeneratedThunk &thunk) {
          return thunk.identity.kind == CppMirThunkKind::CallableAdapter &&
                 thunk.identity.owner == function.id;
        });
    if (found == programPlan.thunks.end()) {
      return;
    }
    const auto *adapter = std::get_if<CppMirCallableThunk>(&found->payload);
    if (adapter == nullptr) {
      throw std::logic_error(
          "lowered callable adapter lost its sealed payload");
    }
    writeIndent();
    output << "friend ";
    emitSemanticType(function.returnType);
    output << " __gti_invoke(";
    if (function.receiverMutability == ReceiverMutability::ReadOnly) {
      output << "const ";
    }
    output << classRow.name
           << (function.receiverMutability == ReceiverMutability::Consuming
                   ? " &&__gti_callable"
                   : " &__gti_callable")
           << ", ::gti_internal::backend::exact_call<";
    switch (adapter->capability) {
    case CallableInvocationCapability::Read:
      output << "::gti_internal::backend::callable_capability::read_call";
      break;
    case CallableInvocationCapability::Mutable:
      output << "::gti_internal::backend::callable_capability::mutable_call";
      break;
    case CallableInvocationCapability::Once:
      output << "::gti_internal::backend::callable_capability::once_call";
      break;
    }
    for (const LoweredParameter &parameter : function.parameters) {
      output << ", ";
      emitLoweredExactCallParameter(parameter.type);
    }
    output << ">";
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
      output << ", ";
      emitLoweredParameter(function.parameters[index],
                           "__gti_arg_" + std::to_string(index));
    }
    output << ") { ";
    if (function.returnType != SemanticType::Void) {
      output << "return ";
    }
    if (function.receiverMutability == ReceiverMutability::Consuming) {
      output << "std::move(__gti_callable).";
    } else {
      output << "__gti_callable.";
    }
    output << cppFunctionSpelling(function, row.name) << '(';
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      if (function.parameters[index].type.kind != SemanticType::Reference) {
        output << "std::move(";
      }
      output << "__gti_arg_" << index;
      if (function.parameters[index].type.kind != SemanticType::Reference) {
        output << ')';
      }
    }
    output << "); }\n";
  }

  void emitLoweredMemberFunctionDeclaration(
      const LoweredDeclaration &row, const LoweredFunctionDeclaration &function,
      const LoweredDeclaration &classRow) {
    emitLoweredBoundaryMarkers(row, function);
    if (function.definitionKind == MirDefinitionKind::RuntimeBinding ||
        function.intrinsic != IntrinsicKind::None) {
      return;
    }
    const std::vector<const MirFunctionInstance *> instances =
        loweredFunctionInstances(function.id);
    const bool generic = !function.genericParameters.empty();
    const bool threadTaskTemplate =
        generic && loweredFunctionNeedsThreadTaskTemplate(instances);
    if (generic) {
      emitLoweredTemplateDeclaration(function.genericParameters,
                                     threadTaskTemplate);
    }
    writeIndent();
    emitLoweredFunctionSignature(row, function, nullptr, false, true, {},
                                 generic);
    const auto *owner = std::get_if<LoweredClassDeclaration>(&classRow.payload);
    const bool genericOwner =
        owner != nullptr && !owner->genericParameters.empty();
    if (function.pureVirtual ||
        (owner != nullptr && owner->kind == ClassKind::Interface)) {
      output << " = 0;\n";
    } else {
      output << ";\n";
      loweredDeferredMembers.push_back(&row);
    }

    const bool hasCallableTemplateFailure =
        generic &&
        !loweredCallableTemplateBody(function, instances, true).empty();
    const bool hasFailure =
        hasCallableTemplateFailure ||
        loweredVirtualFailureContractRetained(function.id) ||
        std::any_of(instances.begin(), instances.end(),
                    [&](const auto *instance) {
                      return loweredFailureBodySupported(*instance);
                    });
    if (hasFailure) {
      if (!generic) {
        writeIndent();
        emitLoweredFunctionSignature(row, function,
                                     genericOwner ? nullptr : instances.front(),
                                     true, true, {}, genericOwner);
        output << (function.pureVirtual ? " = 0;\n" : ";\n");
      } else {
        if (hasCallableTemplateFailure) {
          emitLoweredTemplateDeclaration(function.genericParameters,
                                         threadTaskTemplate);
          writeIndent();
          emitLoweredFunctionSignature(row, function, nullptr, true, true, {},
                                       true);
          output << ";\n";
        }
        std::vector<std::pair<std::vector<SemanticType>, SemanticType>> emitted;
        for (const MirFunctionInstance *instance : instances) {
          if (!loweredFailureBodySupported(*instance) || !instance->owner) {
            continue;
          }
          const MirClassInstance *ownerInstance =
              mir.findClassInstance(*instance->owner);
          const std::optional<std::vector<SemanticType>> parameters =
              cppMirFlattenConcreteParameterTypes(instance->parameterTypes);
          if (ownerInstance == nullptr ||
              !loweredTypeIsNameable(ownerInstance->type) || !parameters ||
              !loweredTypeIsNameable(instance->returnType) ||
              !std::all_of(parameters->begin(), parameters->end(),
                           [&](const SemanticType &type) {
                             return loweredTypeIsNameable(type);
                           })) {
            continue;
          }
          const auto signature =
              std::make_pair(*parameters, instance->returnType);
          if (std::find(emitted.begin(), emitted.end(), signature) !=
              emitted.end()) {
            continue;
          }
          emitted.push_back(signature);
          writeIndent();
          emitLoweredFunctionSignature(row, function, instance, true, true);
          output << ";\n";
        }
      }
    }
    emitLoweredStructuralOperatorAdapter(row, function, classRow);
    emitLoweredCallableAdapter(row, function, classRow);
  }

  [[nodiscard]] std::optional<std::string>
  loweredGenericOwnerConstructorBodyText(
      const LoweredConstructorDeclaration &constructor,
      const MirConstructorInstance &instance, bool failure) const {
    if (!generalEmissionMap ||
        instance.definitionKind != MirDefinitionKind::Source) {
      return std::nullopt;
    }
    const MirClassInstance *owner = mir.findClassInstance(instance.owner);
    const LoweredDeclaration *ownerRow = loweredClassRow(constructor.owner);
    if (owner == nullptr || ownerRow == nullptr ||
        !loweredOwnerIsGeneric(*ownerRow) ||
        loweredTypeIsNameable(owner->type)) {
      return std::nullopt;
    }
    CppMirBodyEmissionMapRows rows{
        .types = generalEmissionMap->types(),
        .bodies = generalEmissionMap->bodies(),
        .symbols = generalEmissionMap->symbols(),
        .enums = generalEmissionMap->enums(),
        .containedConstructors = generalEmissionMap->containedConstructors(),
        .capabilities = generalEmissionMap->capabilities()};
    const std::optional<std::size_t> overlays =
        cppMirApplyGenericOwnerConstructorTypeOverlays(rows, loweredProgram,
                                                       constructor, instance);
    if (!overlays || *overlays == 0) {
      return std::nullopt;
    }
    const CppMirBodyEmissionMap overlay(std::move(rows));
    const CppMirBodyEmitter emitter(mir, overlay);
    const MirBodyAddress address{.kind = MirBodyKind::Constructor,
                                 .owner = instance.id};
    const CppMirBodyEmissionPlan plan = emitter.buildPlan(address);
    const bool supported =
        failure ? plan.supportsFailureText() : plan.supportsPlainText();
    if (!supported) {
      return std::nullopt;
    }
    const CppMirBodyEmissionText emission =
        failure ? emitter.emitFailureBodyText(
                      plan, "generic-owner-constructor-failure-v1", indentation)
                : emitter.emitBodyText(plan, "scalar-cfg-v1", indentation);
    if (!emission.emitted()) {
      throw std::logic_error(
          "lowered generic-owner constructor lost its overlay body");
    }
    return emission.text;
  }

  [[nodiscard]] const CppMirContainedConstructorRepresentation *
  loweredContainedConstructorRepresentation(
      HirConstructorInstanceId constructor) const {
    if (!generalEmissionMap) {
      return nullptr;
    }
    const auto found = std::find_if(
        generalEmissionMap->containedConstructors().begin(),
        generalEmissionMap->containedConstructors().end(),
        [constructor](
            const CppMirContainedConstructorRepresentation &candidate) {
          return candidate.constructor == constructor;
        });
    return found == generalEmissionMap->containedConstructors().end() ? nullptr
                                                                      : &*found;
  }

  void emitLoweredContainedConstructorParameters(
      const LoweredConstructorDeclaration &constructor,
      const MirConstructorInstance &instance, bool sourceParameters) {
    const CppMirContainedConstructorRepresentation *representation =
        loweredContainedConstructorRepresentation(instance.id);
    const MirClassInstance *owner = mir.findClassInstance(instance.owner);
    if (representation == nullptr || owner == nullptr ||
        representation->ownerType != owner->type ||
        representation->parameterTypes != instance.parameterTypes) {
      throw std::logic_error(
          "contained constructor lost its sealed representation row");
    }
    output << representation->tagSpelling;
    const std::size_t count = sourceParameters ? constructor.parameters.size()
                                               : instance.parameterTypes.size();
    if (count != 0) {
      output << ", ";
      if (sourceParameters) {
        emitLoweredParameters(constructor.parameters, true);
      } else {
        emitLoweredMirParameters(instance.parameterTypes);
      }
    }
    output << ", " << representation->stateSpelling
           << " __gti_mir_contained_state";
  }

  void
  emitLoweredContainedConstructorBody(const LoweredDeclaration &owner,
                                      const MirConstructorInstance &instance) {
    if (loweredContainedConstructorRepresentation(instance.id) == nullptr ||
        !generalMirBodyPlan(
             {.kind = MirBodyKind::Constructor, .owner = instance.id})
             .supportsFailureText()) {
      throw std::logic_error(
          "contained constructor definition lost its admitted failure body");
    }
    const std::optional<std::vector<SemanticType>> parameters =
        cppMirFlattenConcreteParameterTypes(instance.parameterTypes);
    if (!parameters) {
      throw std::logic_error(
          "contained constructor has an unspellable parameter pack");
    }
    output << " : " << owner.name << '('
           << cppMirFailureConstructorTagSpelling(instance.id) << "{}";
    for (std::size_t index = 0; index < parameters->size(); ++index) {
      output << ", ";
      if ((*parameters)[index].kind != SemanticType::Reference) {
        output << "std::move(";
      }
      output << "__gti_mir_arg_" << index;
      if ((*parameters)[index].kind != SemanticType::Reference) {
        output << ')';
      }
    }
    output << ", &__gti_mir_contained_state.success, "
              "&__gti_mir_contained_state.record) {\n";
    ++indentation;
    writeIndent();
    output << "if (!__gti_mir_contained_state.success) {\n";
    ++indentation;
    writeIndent();
    output << "::gti_rt_failure_terminate_v1(\n";
    ++indentation;
    writeIndent();
    output << "&__gti_mir_contained_state.record,\n";
    writeIndent();
    output << "&::gti_internal::backend::"
              "__gti_failure_artifact_descriptor_v1,\n";
    writeIndent();
    output << "nullptr, nullptr);\n";
    --indentation;
    --indentation;
    writeIndent();
    output << "}\n";
    --indentation;
    writeIndent();
    output << "}\n";
  }

  void emitLoweredConstructorDeclaration(
      const LoweredDeclaration &row,
      const LoweredConstructorDeclaration &constructor,
      const LoweredDeclaration &classRow) {
    if (constructor.specifier) {
      return;
    }
    if (!constructor.genericParameters.empty()) {
      emitLoweredTemplateDeclaration(constructor.genericParameters);
    }
    writeIndent();
    output << "explicit " << classRow.name << '(';
    emitLoweredParameters(constructor.parameters);
    output << ");\n";
    loweredDeferredMembers.push_back(&row);
    for (const MirConstructorInstance *instance :
         loweredConstructorInstances(constructor.id)) {
      const bool genericOwnerPrimary =
          loweredGenericOwnerConstructorBodyText(constructor, *instance, true)
              .has_value();
      const bool concreteFailure =
          generalMirBodyPlan(
              {.kind = MirBodyKind::Constructor, .owner = instance->id})
              .supportsFailureText();
      if (!instance->mayRaiseDefinedFailure ||
          (!concreteFailure && !genericOwnerPrimary)) {
        continue;
      }
      writeIndent();
      output << "explicit " << classRow.name << '('
             << cppMirFailureConstructorTagSpelling(instance->id);
      const std::size_t count = genericOwnerPrimary
                                    ? constructor.parameters.size()
                                    : instance->parameterTypes.size();
      if (count != 0) {
        output << ", ";
      }
      if (genericOwnerPrimary) {
        emitLoweredParameters(constructor.parameters, true);
      } else {
        emitLoweredMirParameters(instance->parameterTypes);
      }
      output << ", ";
      output << "bool *__gti_mir_constructor_success, "
                "::gti_failure_record_v1 *__gti_mir_failure_record);\n";
      if (loweredContainedConstructorRepresentation(instance->id) != nullptr) {
        writeIndent();
        output << "explicit " << classRow.name << '(';
        emitLoweredContainedConstructorParameters(constructor, *instance,
                                                  genericOwnerPrimary);
        output << ");\n";
      }
    }
  }

  void emitLoweredDestructorDeclaration(
      const LoweredDeclaration &row,
      const LoweredDestructorDeclaration &destructor,
      const LoweredDeclaration &classRow,
      const LoweredClassDeclaration &classDeclaration) {
    if (destructor.specifier) {
      return;
    }
    writeIndent();
    if (classDeclaration.polymorphic) {
      output << "virtual ";
    }
    output << '~' << classRow.name << "() noexcept;\n";
    loweredDeferredMembers.push_back(&row);
  }

  [[nodiscard]] std::vector<const MirDestructorInstance *>
  loweredClassDestructorInstances(ClassId declaration) const {
    std::vector<const MirDestructorInstance *> result;
    for (const MirClassInstance &instance : mir.classInstances()) {
      if (instance.declaration != declaration || !instance.destructor) {
        continue;
      }
      if (const MirDestructorInstance *destructor =
              mir.findDestructorInstance(*instance.destructor)) {
        result.push_back(destructor);
      }
    }
    return result;
  }

  [[nodiscard]] static std::string loweredLifecycleActiveName(ClassId id) {
    return "__gti_lifecycle_active_" + std::to_string(id);
  }

  [[nodiscard]] static std::string loweredLifecycleCleanupName(ClassId id) {
    return "__gti_lifecycle_cleanup_" + std::to_string(id);
  }

  [[nodiscard]] std::vector<const LoweredDeclaration *>
  loweredClassFields(const LoweredDeclaration &classRow) const {
    std::vector<const LoweredDeclaration *> result;
    for (const LoweredDeclaration *member : loweredChildren(classRow.id)) {
      const auto *storage =
          std::get_if<LoweredStorageDeclaration>(&member->payload);
      if (storage != nullptr && !storage->staticStorage) {
        result.push_back(member);
      }
    }
    return result;
  }

  [[nodiscard]] bool loweredClassNeedsEmptyState(
      const LoweredClassDeclaration &declaration) const {
    return std::any_of(mir.classInstances().begin(), mir.classInstances().end(),
                       [&](const MirClassInstance &instance) {
                         return instance.declaration == declaration.id &&
                                cppMirFailureConstructorEmptyStateEligible(
                                    mir, instance.id);
                       });
  }

  void emitLoweredEmptyStateConstructor(
      const LoweredDeclaration &classRow,
      const std::vector<const LoweredDeclaration *> &fields) {
    writeIndent();
    output << "explicit " << classRow.name
           << "(::gti_internal::backend::mir_empty_state_constructor_tag_v1) "
              "noexcept";
    if (!fields.empty()) {
      output << " : ";
      for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) {
          output << ", ";
        }
        const auto *storage =
            std::get_if<LoweredStorageDeclaration>(&fields[index]->payload);
        if (storage == nullptr) {
          throw std::logic_error(
              "lowered empty-state constructor lost a field payload");
        }
        output << fields[index]->name;
        if (storage->type.kind == SemanticType::Array) {
          output << "{}";
        } else {
          output << "(::gti_internal::backend::mir_empty_state_v1<"
                    "std::remove_cv_t<decltype("
                 << fields[index]->name << ")>>())";
        }
      }
    }
    output << " {}\n";
  }

  void emitLoweredMoveConstructor(
      const LoweredDeclaration &classRow,
      const LoweredClassDeclaration &declaration,
      const std::vector<const LoweredDeclaration *> &fields) {
    writeIndent();
    output << classRow.name << '(' << classRow.name << " &&other) noexcept : ";
    bool separator = false;
    const auto base = std::find_if(
        declaration.bases.begin(), declaration.bases.end(),
        [](const LoweredClassBase &candidate) { return !candidate.interface; });
    if (base != declaration.bases.end()) {
      emitSemanticType(base->type);
      output << "(std::move(other))";
      separator = true;
    }
    for (const LoweredDeclaration *field : fields) {
      if (separator) {
        output << ", ";
      }
      output << field->name << "(std::move(other." << field->name << "))";
      separator = true;
    }
    if (separator) {
      output << ", ";
    }
    output << loweredLifecycleActiveName(declaration.id) << "(true) {\n";
    ++indentation;
    writeIndent();
    output << "other." << loweredLifecycleActiveName(declaration.id)
           << " = false;\n";
    --indentation;
    writeIndent();
    output << "}\n";
  }

  void emitLoweredMoveAssignment(
      const LoweredDeclaration &classRow,
      const LoweredClassDeclaration &declaration,
      const std::vector<const LoweredDeclaration *> &fields) {
    writeIndent();
    output << classRow.name << " &operator=(" << classRow.name
           << " &&other) noexcept {\n";
    ++indentation;
    writeIndent();
    output << "if (this != &other) {\n";
    ++indentation;
    writeIndent();
    output << loweredLifecycleCleanupName(declaration.id) << "();\n";
    const auto base = std::find_if(
        declaration.bases.begin(), declaration.bases.end(),
        [](const LoweredClassBase &candidate) { return !candidate.interface; });
    if (base != declaration.bases.end()) {
      writeIndent();
      emitSemanticType(base->type);
      output << "::operator=(std::move(other));\n";
    }
    for (const LoweredDeclaration *field : fields) {
      writeIndent();
      output << field->name << " = std::move(other." << field->name << ");\n";
    }
    writeIndent();
    output << loweredLifecycleActiveName(declaration.id) << " = true;\n";
    writeIndent();
    output << "other." << loweredLifecycleActiveName(declaration.id)
           << " = false;\n";
    --indentation;
    writeIndent();
    output << "}\n";
    writeIndent();
    output << "return *this;\n";
    --indentation;
    writeIndent();
    output << "}\n";
  }

  void emitLoweredClassLifecycle(const LoweredDeclaration &classRow,
                                 const LoweredClassDeclaration &declaration) {
    const std::vector<const LoweredDeclaration *> fields =
        loweredClassFields(classRow);
    const std::vector<const MirDestructorInstance *> destructors =
        loweredClassDestructorInstances(declaration.id);
    if (declaration.requiresActiveDropState) {
      if (indentation > 0) {
        --indentation;
      }
      writeIndent();
      output << "private:\n";
      ++indentation;
      writeIndent();
      output << "bool " << loweredLifecycleActiveName(declaration.id)
             << " = true;\n";
      writeIndent();
      output << "void " << loweredLifecycleCleanupName(declaration.id)
             << "() noexcept;\n";
      const bool failure = std::any_of(
          destructors.begin(), destructors.end(), [&](const auto *instance) {
            return instance->mayRaiseDefinedFailure &&
                   generalMirBodyPlan(
                       {.kind = MirBodyKind::Destructor, .owner = instance->id})
                       .supportsFailureText();
          });
      if (failure) {
        writeIndent();
        output << "template <typename, bool> friend class "
                  "::gti_internal::backend::mir_lifetime_slot;\n";
        writeIndent();
        output << "bool " << lifecycleFailureDispatchName()
               << "(::gti_failure_record_v1 *);\n";
        writeIndent();
        output << "bool " << lifecycleFailureBodyName()
               << "(::gti_failure_record_v1 *);\n";
      }
      loweredDeferredLifecycleClasses.push_back(&classRow);
    }

    if (indentation > 0) {
      --indentation;
    }
    writeIndent();
    output << "public:\n";
    ++indentation;
    const auto emitStatus = [&](SpecialMemberStatus status) {
      output << (status == SpecialMemberStatus::Generated ? " = default;\n"
                                                          : " = delete;\n");
    };
    if (declaration.defaultConstructor != SpecialMemberStatus::Declared) {
      writeIndent();
      output << classRow.name << "()";
      emitStatus(declaration.defaultConstructor);
    }
    if (loweredClassNeedsEmptyState(declaration)) {
      emitLoweredEmptyStateConstructor(classRow, fields);
    }
    writeIndent();
    output << classRow.name << "(const " << classRow.name << " &)";
    emitStatus(declaration.copyConstructor);
    if (declaration.requiresActiveDropState &&
        declaration.moveConstructor == SpecialMemberStatus::Generated) {
      emitLoweredMoveConstructor(classRow, declaration, fields);
    } else {
      writeIndent();
      output << classRow.name << '(' << classRow.name << " &&)";
      emitStatus(declaration.moveConstructor);
    }
    writeIndent();
    output << classRow.name << " &operator=(const " << classRow.name << " &)";
    emitStatus(declaration.copyAssignment);
    if (declaration.requiresActiveDropState &&
        declaration.moveAssignment == SpecialMemberStatus::Generated) {
      emitLoweredMoveAssignment(classRow, declaration, fields);
    } else {
      writeIndent();
      output << classRow.name << " &operator=(" << classRow.name << " &&)";
      emitStatus(declaration.moveAssignment);
    }
    if (declaration.destructor != SpecialMemberStatus::Declared) {
      writeIndent();
      if (declaration.polymorphic) {
        output << "virtual ";
      }
      output << '~' << classRow.name << "() noexcept";
      emitStatus(declaration.destructor);
    }
  }

  void emitLoweredClassAssertions(const LoweredDeclaration &classRow,
                                  const LoweredClassDeclaration &declaration) {
    if (declaration.cAbiRecord && declaration.cAbiLayout) {
      const MirCAbiRecordLayout &layout = *declaration.cAbiLayout;
      writeIndent();
      output << "static_assert(std::is_standard_layout_v<" << classRow.name
             << "> && std::is_trivially_copyable_v<" << classRow.name
             << ">, \"GTI [[c_abi]] records must remain passive C-compatible "
                "values\");\n";
      writeIndent();
      output << "static_assert(sizeof(" << classRow.name
             << ") == " << layout.sizeBytes
             << ", \"native compiler disagrees with GTI record size\");\n";
      writeIndent();
      output << "static_assert(alignof(" << classRow.name
             << ") == " << layout.abiAlignmentBytes
             << ", \"native compiler disagrees with GTI record alignment\");\n";
      for (const MirCAbiRecordFieldLayout &field : layout.fields) {
        const LoweredSymbol *symbol = loweredProgram.findSymbol(field.field);
        if (symbol == nullptr) {
          throw std::logic_error("lowered C ABI layout lost a field symbol");
        }
        writeIndent();
        output << "static_assert(__builtin_offsetof(" << classRow.name << ", "
               << symbol->name << ") == " << field.offsetBytes
               << ", \"native compiler disagrees with GTI field offset\");\n";
      }
    }
    if (declaration.kind == ClassKind::Union && declaration.unionLayout) {
      writeIndent();
      output << "static_assert(std::is_union_v<" << classRow.name
             << "> && std::is_trivially_copyable_v<" << classRow.name
             << ">, \"GTI unions must remain passive native unions\");\n";
      writeIndent();
      output << "static_assert(sizeof(" << classRow.name
             << ") == " << declaration.unionLayout->sizeBytes
             << ", \"native compiler disagrees with GTI union size\");\n";
      writeIndent();
      output << "static_assert(alignof(" << classRow.name
             << ") == " << declaration.unionLayout->abiAlignmentBytes
             << ", \"native compiler disagrees with GTI union alignment\");\n";
    }
  }

  [[nodiscard]] static SemanticType
  substituteLoweredConstantType(SemanticType type,
                                const GenericSubstitution &substitution) {
    if (type.kind == SemanticType::TypeParameter) {
      const auto found = substitution.types.find(type.genericParameterId);
      return found == substitution.types.end() ? type : found->second;
    }
    for (SemanticType &argument : type.arguments) {
      argument = substituteLoweredConstantType(argument, substitution);
    }
    for (CompileTimeValue &argument : type.valueArguments) {
      if (argument.kind != CompileTimeValue::Parameter) {
        continue;
      }
      const auto found = substitution.values.find(argument.parameterId);
      if (found != substitution.values.end()) {
        argument = found->second;
      }
    }
    if (type.arrayLengthParameterId != 0) {
      const auto found = substitution.values.find(type.arrayLengthParameterId);
      if (found != substitution.values.end() &&
          found->second.kind == CompileTimeValue::UInt64) {
        type.arrayLength = found->second.value;
        type.arrayLengthParameterId = 0;
      }
    }
    return type;
  }

  [[nodiscard]] static GenericSubstitution
  loweredConstantClassSubstitution(const LoweredClassDeclaration &declaration,
                                   const SemanticType &concreteType) {
    GenericSubstitution substitution;
    std::size_t typeIndex = 0;
    std::size_t valueIndex = 0;
    for (const LoweredGenericParameter &parameter :
         declaration.genericParameters) {
      if (parameter.value) {
        if (valueIndex < concreteType.valueArguments.size()) {
          substitution.values.insert_or_assign(
              parameter.id, concreteType.valueArguments[valueIndex++]);
        }
      } else if (typeIndex < concreteType.arguments.size()) {
        substitution.types.insert_or_assign(
            parameter.id, concreteType.arguments[typeIndex++]);
      }
    }
    return substitution;
  }

  [[nodiscard]] std::vector<const LoweredDeclaration *>
  loweredConstexprAggregateFields(const LoweredDeclaration &classRow) const {
    std::vector<const LoweredDeclaration *> result;
    for (const LoweredDeclaration *member : loweredChildren(classRow.id)) {
      const auto *storage =
          std::get_if<LoweredStorageDeclaration>(&member->payload);
      if (storage != nullptr && !storage->staticStorage) {
        result.push_back(member);
      }
    }
    return result;
  }

  void emitLoweredConstexprAggregateFactory(
      const LoweredDeclaration &classRow,
      const LoweredClassDeclaration &declaration,
      AccessModifier originalAccess) {
    if (!loweredConstexprAggregateClasses.contains(declaration.id)) {
      return;
    }
    const std::vector<const LoweredDeclaration *> fields =
        loweredConstexprAggregateFields(classRow);
    const auto emitAccess = [&](AccessModifier access) {
      if (indentation > 0) {
        --indentation;
      }
      writeIndent();
      output << (access == AccessModifier::Private ? "private:\n"
                                                   : "public:\n");
      ++indentation;
    };
    emitAccess(AccessModifier::Private);
    writeIndent();
    output << "struct __gti_constexpr_value_tag_v1 {};\n";
    writeIndent();
    output << "constexpr " << classRow.name << "(__gti_constexpr_value_tag_v1";
    for (std::size_t index = 0; index < fields.size(); ++index) {
      const auto &storage =
          std::get<LoweredStorageDeclaration>(fields[index]->payload);
      output << ", ";
      emitSemanticType(storage.type);
      output << " __gti_constexpr_field_" << index;
    }
    output << ')';
    if (!fields.empty()) {
      output << " : ";
      for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) {
          output << ", ";
        }
        output << fields[index]->name << "(std::move(__gti_constexpr_field_"
               << index << "))";
      }
    }
    output << " {}\n";
    emitAccess(AccessModifier::Public);
    writeIndent();
    output << "static constexpr " << classRow.name
           << " __gti_constexpr_value_v1(";
    for (std::size_t index = 0; index < fields.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      const auto &storage =
          std::get<LoweredStorageDeclaration>(fields[index]->payload);
      emitSemanticType(storage.type);
      output << " __gti_constexpr_field_" << index;
    }
    output << ") { return " << classRow.name
           << "(__gti_constexpr_value_tag_v1{}";
    for (std::size_t index = 0; index < fields.size(); ++index) {
      output << ", std::move(__gti_constexpr_field_" << index << ')';
    }
    output << "); }\n";
    emitAccess(originalAccess);
  }

  void emitLoweredClassDefinition(const LoweredDeclaration &classRow,
                                  const LoweredClassDeclaration &declaration) {
    loweredEmittedTypes.insert(classRow.id);
    const std::optional<LoweredClassInitializerSelection> initializers =
        loweredClassInitializers(declaration);
    const bool hasInstance =
        std::any_of(mir.classInstances().begin(), mir.classInstances().end(),
                    [&](const MirClassInstance &instance) {
                      return instance.declaration == declaration.id;
                    });
    if (hasInstance && (!initializers || !initializers->fieldsSupported ||
                        !initializers->staticsVerifiedEmpty)) {
      throw std::logic_error(
          "lowered C++ emission has no complete class initializer schedule "
          "for '" +
          classRow.name + "'");
    }
    if (declaration.compilerCapability != CompilerCapabilityTypeKind::None ||
        declaration.cOpaqueHandle) {
      if (initializers) {
        for (const HirClassInstanceId instance : initializers->instances) {
          if (initializers->fieldsSupported &&
              std::all_of(
                  initializers->fields.begin(), initializers->fields.end(),
                  [](const auto &field) { return field.second.empty(); })) {
            writeIndent();
            output << "// GTI verified-MIR body: scalar-cfg-v1 "
                      "field-initializers-instance "
                   << instance << "\n";
          }
          if (initializers->staticsVerifiedEmpty) {
            writeIndent();
            output << "// GTI verified-MIR body: scalar-cfg-v1 "
                      "static-field-initializers-instance "
                   << instance << "\n";
          }
        }
      }
      return;
    }
    if (declaration.forwardDeclaration) {
      return;
    }

    if (declaration.exactSpecializationPrimary != 0) {
      writeIndent();
      output << "template <>\n";
    } else {
      emitLoweredTemplateDeclaration(declaration.genericParameters);
    }
    writeIndent();
    output << (declaration.kind == ClassKind::Struct  ? "struct "
               : declaration.kind == ClassKind::Union ? "union "
                                                      : "class ")
           << classRow.name << loweredExactClassArguments(declaration);
    if (!declaration.bases.empty()) {
      output << " : ";
      for (std::size_t index = 0; index < declaration.bases.size(); ++index) {
        if (index != 0) {
          output << ", ";
        }
        output << (declaration.bases[index].access == AccessModifier::Private
                       ? "private "
                       : "public ");
        emitSemanticType(declaration.bases[index].type);
      }
    }
    output << " {\n";
    ++indentation;
    AccessModifier access = declaration.kind == ClassKind::Class
                                ? AccessModifier::Private
                                : AccessModifier::Public;
    if (declaration.kind == ClassKind::Interface) {
      writeIndent();
      output << "public:\n";
      access = AccessModifier::Public;
    }
    emitLoweredConstexprAggregateFactory(classRow, declaration, access);
    for (const LoweredDeclaration *member : loweredChildren(classRow.id)) {
      if (const auto *value =
              std::get_if<LoweredAccessDeclaration>(&member->payload)) {
        access = value->access;
        if (indentation > 0) {
          --indentation;
        }
        writeIndent();
        output << (access == AccessModifier::Private ? "private:\n"
                                                     : "public:\n");
        ++indentation;
        continue;
      }
      static_cast<void>(access);
      if (const auto *storage =
              std::get_if<LoweredStorageDeclaration>(&member->payload)) {
        emitLoweredStorageDeclaration(*member, *storage, true, &declaration,
                                      initializers ? &*initializers : nullptr);
      } else if (const auto *function = std::get_if<LoweredFunctionDeclaration>(
                     &member->payload)) {
        emitLoweredMemberFunctionDeclaration(*member, *function, classRow);
      } else if (const auto *constructor =
                     std::get_if<LoweredConstructorDeclaration>(
                         &member->payload)) {
        emitLoweredConstructorDeclaration(*member, *constructor, classRow);
      } else if (const auto *destructor =
                     std::get_if<LoweredDestructorDeclaration>(
                         &member->payload)) {
        emitLoweredDestructorDeclaration(*member, *destructor, classRow,
                                         declaration);
      } else if (const auto *enumeration =
                     std::get_if<LoweredEnumDeclaration>(&member->payload)) {
        emitLoweredEnumDefinition(*member, *enumeration);
        loweredEmittedTypes.insert(member->id);
      } else if (const auto *nested =
                     std::get_if<LoweredClassDeclaration>(&member->payload)) {
        emitLoweredClassDefinition(*member, *nested);
      } else if (const auto *alias = std::get_if<LoweredTypeAliasDeclaration>(
                     &member->payload)) {
        writeIndent();
        output << "using " << member->name << " = ";
        emitSemanticType(alias->type);
        output << ";\n";
      } else if (member->kind == LoweredDeclarationKind::Empty) {
        writeIndent();
        output << ";\n";
      }
    }
    if (!declaration.cAbiRecord && declaration.kind != ClassKind::Union) {
      emitLoweredClassLifecycle(classRow, declaration);
    }
    --indentation;
    writeIndent();
    output << "};\n";
    if (initializers) {
      for (const HirClassInstanceId instance : initializers->instances) {
        if (initializers->fieldsSupported) {
          writeIndent();
          output << "// GTI verified-MIR body: scalar-cfg-v1 "
                    "field-initializers-instance "
                 << instance << "\n";
        }
        if (initializers->staticsVerifiedEmpty) {
          writeIndent();
          output << "// GTI verified-MIR body: scalar-cfg-v1 "
                    "static-field-initializers-instance "
                 << instance << "\n";
        }
      }
    }
    emitLoweredClassAssertions(classRow, declaration);
  }

  void
  loweredCollectRepresentationDependencies(const SemanticType &type,
                                           std::vector<ClassId> &result) const {
    switch (type.kind) {
    case SemanticType::RawPointer:
    case SemanticType::Reference:
    case SemanticType::UniqueOwner:
    case SemanticType::SharedPointer:
    case SemanticType::Storage:
    case SemanticType::PrefixStorage:
      return;
    case SemanticType::Class:
      if (type.classId != 0) {
        result.push_back(type.classId);
      }
      for (const SemanticType &argument : type.arguments) {
        loweredCollectRepresentationDependencies(argument, result);
      }
      return;
    case SemanticType::Array:
    case SemanticType::Expected:
    case SemanticType::Unexpected:
      for (const SemanticType &argument : type.arguments) {
        loweredCollectRepresentationDependencies(argument, result);
      }
      return;
    default:
      return;
    }
  }

  void
  loweredCollectInitializerDependencies(const MirBody &body,
                                        std::vector<ClassId> &result) const {
    for (const MirPlace &place : body.places) {
      loweredCollectRepresentationDependencies(place.type, result);
    }
    for (const MirValue &value : body.values) {
      loweredCollectRepresentationDependencies(value.info.type, result);
    }
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        loweredCollectRepresentationDependencies(instruction.info.type, result);
        loweredCollectRepresentationDependencies(instruction.dispatchOwner,
                                                 result);
        for (const SemanticType &parameter : instruction.parameterTypes) {
          loweredCollectRepresentationDependencies(parameter, result);
        }
        for (const SemanticType &capture : instruction.closureCaptureTypes) {
          loweredCollectRepresentationDependencies(capture, result);
        }
        if (instruction.receiver) {
          loweredCollectRepresentationDependencies(instruction.receiver->type,
                                                   result);
        }
        for (const MirOperand &operand : instruction.operands) {
          loweredCollectRepresentationDependencies(operand.type, result);
        }
        if (instruction.functionTarget) {
          const MirFunctionInstance *function =
              mir.findFunctionInstance(*instruction.functionTarget);
          const MirClassInstance *owner =
              function != nullptr && function->owner
                  ? mir.findClassInstance(*function->owner)
                  : nullptr;
          if (owner != nullptr) {
            loweredCollectRepresentationDependencies(owner->type, result);
          }
        }
        if (instruction.constructorTarget) {
          const MirConstructorInstance *constructor =
              mir.findConstructorInstance(*instruction.constructorTarget);
          const MirClassInstance *owner =
              constructor == nullptr
                  ? nullptr
                  : mir.findClassInstance(constructor->owner);
          if (owner != nullptr) {
            loweredCollectRepresentationDependencies(owner->type, result);
          }
        }
      }
    }
  }

  [[nodiscard]] std::vector<ClassId> loweredClassDefinitionDependencies(
      const LoweredDeclaration &row,
      const LoweredClassDeclaration &declaration) const {
    std::vector<ClassId> result;
    if (declaration.exactSpecializationPrimary != 0) {
      result.push_back(declaration.exactSpecializationPrimary);
    }
    for (const LoweredClassBase &base : declaration.bases) {
      loweredCollectRepresentationDependencies(base.type, result);
    }
    for (const LoweredDeclaration *member : loweredChildren(row.id)) {
      const auto *storage =
          std::get_if<LoweredStorageDeclaration>(&member->payload);
      if (storage != nullptr) {
        loweredCollectRepresentationDependencies(storage->type, result);
      }
    }
    // A generic definition is scheduled from its dependent source fields.
    // Folding every concrete instantiation back to the primary declaration
    // invents cycles such as box -> cell -> box for an acyclic nested type.
    if (declaration.genericParameters.empty()) {
      for (const MirClassInstance &instance : mir.classInstances()) {
        if (instance.declaration != declaration.id) {
          continue;
        }
        for (const MirClassFieldInfo &field : instance.declaredFields) {
          loweredCollectRepresentationDependencies(field.type, result);
        }
        loweredCollectInitializerDependencies(instance.fieldInitializers,
                                              result);
      }
    }
    std::erase(result, declaration.id);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
  }

  [[nodiscard]] std::vector<ClassId> loweredConcreteSpecializationDependencies(
      const LoweredClassDeclaration &declaration) const {
    std::vector<ClassId> result;
    for (const MirClassInstance &instance : mir.classInstances()) {
      if (instance.declaration != declaration.id) {
        continue;
      }
      for (const MirClassFieldInfo &field : instance.declaredFields) {
        loweredCollectRepresentationDependencies(field.type, result);
      }
    }
    std::erase(result, declaration.id);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
  }

  void emitLoweredScheduledTypeDefinition(const LoweredDeclaration &row) {
    const std::size_t namespaceCount =
        emitLoweredNamespaceOpen(row.namespaceScope);
    if (const auto *enumeration =
            std::get_if<LoweredEnumDeclaration>(&row.payload)) {
      emitLoweredEnumDefinition(row, *enumeration);
      loweredEmittedTypes.insert(row.id);
    } else if (const auto *type =
                   std::get_if<LoweredClassDeclaration>(&row.payload)) {
      emitLoweredClassDefinition(row, *type);
    }
    emitLoweredNamespaceClose(namespaceCount);
    output << '\n';
  }

  void emitLoweredTypeDefinitions() {
    std::vector<const LoweredDeclaration *> enumerations;
    std::vector<const LoweredDeclaration *> classes;
    std::unordered_map<ClassId, std::size_t> classSchedule;
    for (const LoweredDeclaration &row : loweredProgram.declarations()) {
      const LoweredDeclaration *parent =
          loweredProgram.findDeclaration(row.parent);
      if (parent != nullptr &&
          std::holds_alternative<LoweredClassDeclaration>(parent->payload)) {
        continue;
      }
      if (std::holds_alternative<LoweredEnumDeclaration>(row.payload)) {
        enumerations.push_back(&row);
        continue;
      }
      const auto *type = std::get_if<LoweredClassDeclaration>(&row.payload);
      if (type == nullptr) {
        continue;
      }
      const std::size_t index = classes.size();
      classes.push_back(&row);
      if (!type->forwardDeclaration &&
          type->compilerCapability == CompilerCapabilityTypeKind::None &&
          !type->cOpaqueHandle) {
        classSchedule.insert_or_assign(type->id, index);
      }
    }

    // A variant alternative embeds its class payloads by value, so an enum
    // with class payload fields must follow every class definition. Field
    // embedding of such enums inside classes is semantically rejected until
    // layout scheduling interleaves enums with the class dependency walk.
    std::vector<const LoweredDeclaration *> classPayloadEnumerations;
    for (const LoweredDeclaration *enumeration : enumerations) {
      const auto *declaration =
          std::get_if<LoweredEnumDeclaration>(&enumeration->payload);
      const bool classPayload =
          declaration != nullptr && declaration->payload &&
          std::any_of(
              declaration->enumerators.begin(), declaration->enumerators.end(),
              [](const LoweredEnumerator &enumerator) {
                return std::any_of(enumerator.payloadTypes.begin(),
                                   enumerator.payloadTypes.end(),
                                   [](const SemanticType &type) {
                                     return type.kind == SemanticType::Class;
                                   });
              });
      if (classPayload) {
        classPayloadEnumerations.push_back(enumeration);
        continue;
      }
      emitLoweredScheduledTypeDefinition(*enumeration);
    }

    std::vector<unsigned char> state(classes.size(), 0);
    const auto emitClass = [&](const auto &self, std::size_t index) -> void {
      if (index >= classes.size() || state[index] == 2) {
        return;
      }
      if (state[index] == 1) {
        throw std::logic_error(
            "lowered class layout contains a recursive by-value dependency");
      }
      state[index] = 1;
      const LoweredDeclaration &row = *classes[index];
      const auto *declaration =
          std::get_if<LoweredClassDeclaration>(&row.payload);
      if (declaration == nullptr) {
        throw std::logic_error(
            "lowered class schedule lost its declaration payload");
      }
      std::vector<ClassId> dependencies =
          loweredClassDefinitionDependencies(row, *declaration);
      std::sort(dependencies.begin(), dependencies.end(),
                [&](ClassId left, ClassId right) {
                  const auto leftAt = classSchedule.find(left);
                  const auto rightAt = classSchedule.find(right);
                  return (leftAt == classSchedule.end()
                              ? std::numeric_limits<std::size_t>::max()
                              : leftAt->second) <
                         (rightAt == classSchedule.end()
                              ? std::numeric_limits<std::size_t>::max()
                              : rightAt->second);
                });
      for (const ClassId dependency : dependencies) {
        if (const auto found = classSchedule.find(dependency);
            found != classSchedule.end()) {
          self(self, found->second);
        }
      }
      const std::size_t firstDeferredMember = loweredDeferredMembers.size();
      const std::size_t firstDeferredLifecycleClass =
          loweredDeferredLifecycleClasses.size();
      emitLoweredScheduledTypeDefinition(row);
      const std::size_t lastDeferredMember = loweredDeferredMembers.size();
      const std::size_t lastDeferredLifecycleClass =
          loweredDeferredLifecycleClasses.size();
      state[index] = 2;

      // A concrete member specialization can instantiate its complete owner
      // even though the primary definition only depends on source-level field
      // types. Complete any concrete field dependencies first. Marking this
      // primary complete above makes mutually nested generic arguments
      // converge without treating them as recursive by-value source layouts.
      std::vector<ClassId> specializationDependencies =
          loweredConcreteSpecializationDependencies(*declaration);
      std::sort(specializationDependencies.begin(),
                specializationDependencies.end(),
                [&](ClassId left, ClassId right) {
                  const auto leftAt = classSchedule.find(left);
                  const auto rightAt = classSchedule.find(right);
                  return (leftAt == classSchedule.end()
                              ? std::numeric_limits<std::size_t>::max()
                              : leftAt->second) <
                         (rightAt == classSchedule.end()
                              ? std::numeric_limits<std::size_t>::max()
                              : rightAt->second);
                });
      for (const ClassId dependency : specializationDependencies) {
        if (const auto found = classSchedule.find(dependency);
            found != classSchedule.end()) {
          self(self, found->second);
        }
      }
      emitLoweredMemberSpecializationDeclarations(
          firstDeferredMember, lastDeferredMember, firstDeferredLifecycleClass,
          lastDeferredLifecycleClass);
    };
    for (std::size_t index = 0; index < classes.size(); ++index) {
      emitClass(emitClass, index);
    }

    for (const LoweredDeclaration *enumeration : classPayloadEnumerations) {
      emitLoweredScheduledTypeDefinition(*enumeration);
    }
  }

  void emitLoweredNormalFunctionBody(
      const MirFunctionInstance &instance,
      std::string_view familyLabel = "scalar-cfg-v1") {
    const CppMirBodyEmissionPlan &plan = generalMirBodyPlan(
        {.kind = MirBodyKind::Function, .owner = instance.id});
    const CppMirBodyEmissionText emission =
        generalMirBodyEmitter().emitBodyText(plan, familyLabel, indentation);
    if (!emission.emitted()) {
      throw std::logic_error(
          "selected lowered function body is not emit-ready (instance " +
          std::to_string(instance.id) + ")");
    }
    output << emission.text;
  }

  void emitLoweredFailureFunctionBody(
      const MirFunctionInstance &instance,
      std::string_view familyLabel = "scalar-cfg-failure-v1") {
    const CppMirBodyEmissionPlan &plan = generalMirBodyPlan(
        {.kind = MirBodyKind::Function, .owner = instance.id});
    const CppMirBodyEmissionText emission =
        generalMirBodyEmitter().emitFailureBodyText(plan, familyLabel,
                                                    indentation);
    if (!emission.emitted()) {
      throw std::logic_error(
          "selected lowered failure function body is not emit-ready "
          "(instance " +
          std::to_string(instance.id) + ")");
    }
    output << emission.text;
  }

  [[nodiscard]] static bool
  loweredContainsLambdaType(const SemanticType &type) {
    if (type.kind == SemanticType::Lambda) {
      return true;
    }
    return std::any_of(type.arguments.begin(), type.arguments.end(),
                       loweredContainsLambdaType);
  }

  [[nodiscard]] bool loweredFunctionInstanceIsNameable(
      const LoweredFunctionInstance &instance,
      const MirFunctionInstance &mirInstance) const {
    return cppConcreteFunctionInstanceIsNameable(loweredProgram, instance,
                                                 mirInstance);
  }

  [[nodiscard]] bool loweredFunctionNeedsThreadTaskTemplate(
      const std::vector<const MirFunctionInstance *> &instances) const {
    return std::any_of(
        instances.begin(), instances.end(),
        [&](const MirFunctionInstance *instance) {
          const LoweredFunctionInstance *lowered =
              instance == nullptr ? nullptr
                                  : loweredFunctionInstance(instance->id);
          if (lowered == nullptr ||
              loweredFunctionInstanceIsNameable(*lowered, *instance)) {
            return false;
          }
          const MirBodyAddress source{.kind = MirBodyKind::Function,
                                      .owner = instance->id};
          return std::any_of(mir.threadTaskAdapters().begin(),
                             mir.threadTaskAdapters().end(),
                             [&](const MirThreadTaskAdapter &adapter) {
                               return adapter.sourceBody == source;
                             });
        });
  }

  [[nodiscard]] std::string loweredCallableTemplateBody(
      const LoweredFunctionDeclaration &function,
      const std::vector<const MirFunctionInstance *> &instances,
      bool failure) const {
    if (!generalEmissionMap) {
      throw std::logic_error(
          "lowered callable template emission has no representation rows");
    }
    const std::string_view familyLabel =
        failure ? "deduced-callable-failure-v1" : "deduced-callable-v1";
    struct BodyVariant {
      // One canonical template body per parameter-cleanup shape: the bit at
      // each declared parameter index records whether that instance binding
      // carries a lexical drop obligation. Instances whose concrete callable
      // environments need different cleanup (a scalar-capturing lambda beside
      // a class-capturing one) form separate variants that the
      // instantiation-time destructibility dispatch below reunites under the
      // single emitted template.
      std::vector<bool> cleanupShape;
      std::vector<std::string> banners;
      std::string canonical;
    };
    std::vector<BodyVariant> variants;
    const bool threadTaskTemplate =
        loweredFunctionNeedsThreadTaskTemplate(instances);
    std::optional<std::size_t> taskParameterIndex;
    const auto threadTaskParameter =
        [](const MirFunctionInstance &instance) -> std::optional<std::size_t> {
      std::optional<std::size_t> selected;
      for (const MirCallableParameter &parameter :
           instance.callableParameters) {
        if (!parameter.ownedTransport ||
            parameter.ownedTransport->kind !=
                CallableOwnedTransportKind::ThreadTask) {
          continue;
        }
        if (selected) {
          return std::nullopt;
        }
        selected = parameter.parameterIndex;
      }
      return selected;
    };
    const auto parameterHasLexicalDrop = [](const MirFunctionInstance &instance,
                                            std::size_t parameterIndex) {
      if (parameterIndex >= instance.parameterBindings.size()) {
        return false;
      }
      const HirBindingId binding = instance.parameterBindings[parameterIndex];
      return std::any_of(instance.body.dropObligations.begin(),
                         instance.body.dropObligations.end(),
                         [&](const MirDropObligation &obligation) {
                           return obligation.kind ==
                                      MirDropObligationKind::Binding &&
                                  obligation.binding == binding;
                         });
    };
    for (const MirFunctionInstance *instance : instances) {
      const LoweredFunctionInstance *concrete =
          loweredFunctionInstance(instance->id);
      const bool lambdaTransport =
          std::any_of(instance->typeArguments.begin(),
                      instance->typeArguments.end(),
                      loweredContainsLambdaType) ||
          std::any_of(instance->parameterTypes.begin(),
                      instance->parameterTypes.end(),
                      loweredContainsLambdaType) ||
          loweredContainsLambdaType(instance->returnType);
      if (concrete == nullptr || !lambdaTransport ||
          loweredFunctionInstanceIsNameable(*concrete, *instance)) {
        continue;
      }
      CppMirBodyEmissionMapRows rows{
          .types = generalEmissionMap->types(),
          .bodies = generalEmissionMap->bodies(),
          .symbols = generalEmissionMap->symbols(),
          .enums = generalEmissionMap->enums(),
          .containedConstructors = generalEmissionMap->containedConstructors(),
          .capabilities = generalEmissionMap->capabilities()};
      const std::optional<std::size_t> overlays =
          cppMirApplyCallableTemplateTypeOverlays(
              rows, loweredProgram, standard, function, *instance);
      if (!overlays || *overlays == 0) {
        throw std::logic_error(
            "lowered unnameable callable instance has no template type "
            "overlay");
      }
      const CppMirBodyEmissionMap overlay{std::move(rows)};
      const CppMirBodyEmitter emitter(mir, overlay);
      const MirBodyAddress address{.kind = MirBodyKind::Function,
                                   .owner = instance->id};
      const CppMirBodyEmissionPlan plan = emitter.buildPlan(address);
      const bool supported =
          failure ? plan.supportsFailureText() : plan.supportsPlainText();
      if (!supported) {
        continue;
      }
      const CppMirBodyEmissionText emission =
          failure ? emitter.emitFailureBodyText(plan, familyLabel, indentation)
                  : emitter.emitBodyText(plan, familyLabel, indentation);
      if (!emission.emitted()) {
        throw std::logic_error(
            "lowered callable template body emission failed closed");
      }
      const std::string marker =
          std::string("// GTI verified-MIR body: ") + std::string(familyLabel) +
          " function-instance " + std::to_string(instance->id) + "\n";
      const std::size_t markerAt = emission.text.find(marker);
      if (markerAt == std::string::npos) {
        throw std::logic_error(
            "lowered callable template body lost its instance marker");
      }
      std::size_t lineStart = markerAt;
      while (lineStart > 0 && emission.text[lineStart - 1] != '\n') {
        --lineStart;
      }
      std::string body = emission.text;
      body.erase(lineStart, markerAt - lineStart + marker.size());
      if (threadTaskTemplate) {
        const std::optional<std::size_t> parameter =
            threadTaskParameter(*instance);
        if (!parameter || *parameter >= function.parameters.size() ||
            (taskParameterIndex && taskParameterIndex != parameter)) {
          throw std::logic_error(
              "lowered thread-task template lost its exact task parameter");
        }
        taskParameterIndex = parameter;
      }
      // The overlay admission above already proved the instance parameter
      // list matches the declaration parameter list exactly, so the shape
      // indexes the declared parameters directly.
      std::vector<bool> cleanupShape(function.parameters.size(), false);
      for (std::size_t index = 0; index < cleanupShape.size(); ++index) {
        cleanupShape[index] = parameterHasLexicalDrop(*instance, index);
      }
      BodyVariant *variant = nullptr;
      for (BodyVariant &row : variants) {
        if (row.cleanupShape == cleanupShape) {
          variant = &row;
          break;
        }
      }
      if (variant == nullptr) {
        variants.push_back({.cleanupShape = std::move(cleanupShape)});
        variant = &variants.back();
      }
      variant->banners.push_back(emission.text.substr(
          lineStart, markerAt - lineStart + marker.size()));
      if (variant->canonical.empty()) {
        variant->canonical = std::move(body);
      } else if (variant->canonical != body) {
        throw std::logic_error(
            "lowered callable instances do not share one source template "
            "representation");
      }
    }
    if (variants.empty()) {
      return {};
    }
    const auto assemble = [](const BodyVariant &variant) {
      if (variant.canonical.empty()) {
        return std::string{};
      }
      const std::size_t brace = variant.canonical.find("{\n");
      if (brace == std::string::npos) {
        throw std::logic_error(
            "lowered callable template body has no opening brace");
      }
      std::string assembled = variant.canonical.substr(0, brace + 2);
      for (const std::string &banner : variant.banners) {
        assembled += banner;
      }
      assembled += variant.canonical.substr(brace + 2);
      return assembled;
    };
    std::sort(variants.begin(), variants.end(),
              [](const BodyVariant &left, const BodyVariant &right) {
                return left.cleanupShape < right.cleanupShape;
              });
    if (variants.size() == 1) {
      return assemble(variants.front());
    }
    // Distinct cleanup shapes cannot share one body text. Reunite them under
    // the single emitted template through an instantiation-time dispatch:
    // every parameter whose drop presence varies across instances must be
    // declared directly as a template type parameter (or be the exact
    // thread-task parameter), so std::is_trivially_destructible_v over its
    // declared spelling reproduces exactly the MIR drop decision each covered
    // instance was built from.
    std::vector<std::size_t> varying;
    std::vector<std::string> varyingSpellings(function.parameters.size());
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
      const bool first = variants.front().cleanupShape[index];
      const bool varies = std::any_of(variants.begin(), variants.end(),
                                      [&](const BodyVariant &row) {
                                        return row.cleanupShape[index] != first;
                                      });
      if (!varies) {
        continue;
      }
      const SemanticType &declared = function.parameters[index].type;
      const bool threadTask = threadTaskTemplate && taskParameterIndex &&
                              *taskParameterIndex == index;
      const std::string spelling =
          cppSemanticTypeSpelling(loweredProgram, standard, declared);
      if ((declared.kind != SemanticType::TypeParameter && !threadTask) ||
          spelling.empty()) {
        throw std::logic_error(
            "lowered callable template has incompatible cleanup variants");
      }
      varying.push_back(index);
      varyingSpellings[index] = spelling;
    }
    if (varying.empty()) {
      throw std::logic_error(
          "lowered callable template has incompatible cleanup variants");
    }
    const std::string returnType =
        failure ? "bool"
                : cppSemanticTypeSpelling(loweredProgram, standard,
                                          function.returnType);
    const std::string branchIndent((indentation + 1) * 2, ' ');
    const std::string statementIndent((indentation + 2) * 2, ' ');
    const auto branch = [&](std::string_view condition,
                            const BodyVariant &variant) {
      std::string text = branchIndent + std::string(condition) + " {\n";
      if (function.returnType == SemanticType::Void && !failure) {
        text += statementIndent + "[&]() -> void " + assemble(variant) +
                "();\n" + statementIndent + "return;\n";
      } else {
        text += statementIndent + "return [&]() -> " + returnType + " " +
                assemble(variant) + "();\n";
      }
      text += branchIndent + "}\n";
      return text;
    };
    const auto condition = [&](const BodyVariant &variant) {
      std::string text = "if constexpr (";
      for (std::size_t position = 0; position < varying.size(); ++position) {
        if (position != 0) {
          text += " && ";
        }
        const std::size_t index = varying[position];
        if (variant.cleanupShape[index]) {
          text += "!";
        }
        text +=
            "std::is_trivially_destructible_v<" + varyingSpellings[index] + ">";
      }
      text += ")";
      return text;
    };
    std::string assembled = "{\n";
    for (std::size_t index = 0; index < variants.size(); ++index) {
      const bool last = index + 1 == variants.size();
      const std::string label = last ? std::string("else")
                                : index == 0
                                    ? condition(variants[index])
                                    : "else " + condition(variants[index]);
      assembled += branch(label, variants[index]);
    }
    assembled += std::string(indentation * 2, ' ') + "}\n";
    return assembled;
  }

  void emitLoweredFreeFunctionDefinition(
      const LoweredDeclaration &row,
      const LoweredFunctionDeclaration &function) {
    if (function.ownerClass != 0 ||
        function.definitionKind != MirDefinitionKind::Source ||
        function.linkage == LanguageLinkage::C ||
        function.intrinsic != IntrinsicKind::None) {
      return;
    }
    const std::vector<const MirFunctionInstance *> instances =
        loweredFunctionInstances(function.id);
    if (instances.empty()) {
      return;
    }
    const bool generic = !function.genericParameters.empty();
    if (!generic) {
      if (instances.size() != 1) {
        throw std::logic_error(
            "non-generic lowered function has multiple MIR instances");
      }
      const MirFunctionInstance &instance = *instances.front();
      bool emitted = false;
      if (loweredFailureBodySupported(instance)) {
        writeIndent();
        emitLoweredFunctionSignature(row, function, &instance, true);
        output << ' ';
        emitLoweredFailureFunctionBody(instance);
        emitted = true;
      }
      if (loweredNormalBodySupported(instance)) {
        writeIndent();
        emitLoweredFunctionSignature(row, function, &instance, false);
        output << ' ';
        emitLoweredNormalFunctionBody(instance);
        emitted = true;
      }
      if (!emitted) {
        throw std::logic_error(
            "source lowered function has no admitted MIR body");
      }
      return;
    }

    const std::string callableFailure =
        loweredCallableTemplateBody(function, instances, true);
    const std::string callableNormal =
        loweredCallableTemplateBody(function, instances, false);
    const bool threadTaskTemplate =
        loweredFunctionNeedsThreadTaskTemplate(instances);
    if (!callableFailure.empty()) {
      emitLoweredTemplateDeclaration(function.genericParameters,
                                     threadTaskTemplate);
      writeIndent();
      emitLoweredFunctionSignature(row, function, nullptr, true, false, {},
                                   true);
      output << ' ' << callableFailure;
    }
    if (!callableNormal.empty()) {
      emitLoweredTemplateDeclaration(function.genericParameters,
                                     threadTaskTemplate);
      writeIndent();
      emitLoweredFunctionSignature(row, function, nullptr, false, false, {},
                                   true);
      output << ' ' << callableNormal;
    }
    for (const MirFunctionInstance *instance : instances) {
      const LoweredFunctionInstance *concrete =
          loweredFunctionInstance(instance->id);
      if (concrete == nullptr ||
          !loweredFunctionInstanceIsNameable(*concrete, *instance)) {
        continue;
      }
      if (loweredFailureBodySupported(*instance)) {
        writeIndent();
        emitLoweredConcreteFreeFunctionSignature(row, function, *instance,
                                                 true);
        output << ' ';
        emitLoweredFailureFunctionBody(*instance);
      }
      if (loweredNormalBodySupported(*instance)) {
        writeIndent();
        emitLoweredConcreteFreeFunctionSignature(row, function, *instance,
                                                 false);
        output << ' ';
        emitLoweredNormalFunctionBody(*instance);
      }
    }
  }

  void emitLoweredSourceDeclarations(LoweredDeclarationId parent) {
    for (const LoweredDeclaration *declaration : loweredChildren(parent)) {
      if (declaration->kind == LoweredDeclarationKind::Namespace) {
        writeIndent();
        output << "namespace "
               << loweredNamespaceSpelling(declaration->name, parent == 0)
               << " {\n";
        ++indentation;
        emitLoweredSourceDeclarations(declaration->id);
        --indentation;
        writeIndent();
        output << "}\n";
        continue;
      }
      if (declaration->kind == LoweredDeclarationKind::LanguageLinkage) {
        if (!loweredForwardedLinkage.contains(declaration->id)) {
          emitLoweredExternCBlock(*declaration);
        }
        continue;
      }
      if (const auto *storage =
              std::get_if<LoweredStorageDeclaration>(&declaration->payload)) {
        if (storage->ownerClass == 0) {
          emitLoweredStorageDeclaration(*declaration, *storage, false);
        }
        continue;
      }
      if (const auto *function =
              std::get_if<LoweredFunctionDeclaration>(&declaration->payload)) {
        emitLoweredFreeFunctionDefinition(*declaration, *function);
        continue;
      }
      if (declaration->kind == LoweredDeclarationKind::Empty) {
        writeIndent();
        output << ";\n";
      }
    }
  }

  std::size_t emitLoweredNamespaceOpen(const std::vector<std::string> &scope) {
    for (std::size_t index = 0; index < scope.size(); ++index) {
      writeIndent();
      output << "namespace "
             << loweredNamespaceSpelling(scope[index], index == 0) << " {\n";
      ++indentation;
    }
    return scope.size();
  }

  void emitLoweredNamespaceClose(std::size_t count) {
    while (count != 0) {
      --indentation;
      writeIndent();
      output << "}\n";
      --count;
    }
  }

  void emitLoweredPrimaryClassName(const LoweredDeclaration &classRow) {
    const auto *declaration =
        std::get_if<LoweredClassDeclaration>(&classRow.payload);
    if (declaration == nullptr) {
      throw std::logic_error("lowered class name lost its declaration");
    }
    const LoweredDeclaration *parent =
        loweredProgram.findDeclaration(classRow.parent);
    if (parent != nullptr &&
        std::holds_alternative<LoweredClassDeclaration>(parent->payload)) {
      emitLoweredPrimaryClassName(*parent);
      output << "::";
    }
    output << classRow.name;
    if (declaration->exactSpecializationPrimary != 0) {
      output << loweredExactClassArguments(*declaration);
      return;
    }
    if (declaration->genericParameters.empty()) {
      return;
    }
    output << '<';
    for (std::size_t index = 0; index < declaration->genericParameters.size();
         ++index) {
      if (index != 0) {
        output << ", ";
      }
      const LoweredGenericParameter &parameter =
          declaration->genericParameters[index];
      output << parameter.name;
      if (parameter.pack) {
        output << "...";
      }
    }
    output << '>';
  }

  void emitLoweredClassTemplateHeaders(const LoweredDeclaration &classRow) {
    const LoweredDeclaration *parent =
        loweredProgram.findDeclaration(classRow.parent);
    if (parent != nullptr &&
        std::holds_alternative<LoweredClassDeclaration>(parent->payload)) {
      emitLoweredClassTemplateHeaders(*parent);
    }
    const auto *declaration =
        std::get_if<LoweredClassDeclaration>(&classRow.payload);
    if (declaration != nullptr) {
      emitLoweredTemplateDeclaration(declaration->genericParameters);
    }
  }

  [[nodiscard]] std::string
  loweredConcreteClassName(HirClassInstanceId instance) const {
    const MirClassInstance *owner = mir.findClassInstance(instance);
    if (owner == nullptr) {
      throw std::logic_error(
          "lowered member definition lost its concrete class instance");
    }
    std::string spelling =
        cppSemanticTypeSpelling(loweredProgram, standard, owner->type);
    if (spelling.rfind("::", 0) == 0) {
      spelling.erase(0, 2);
    }
    return spelling;
  }

  void emitLoweredStaticFieldDefinitions() {
    for (const LoweredDeclaration *row : loweredDeferredStaticFields) {
      const auto *storage =
          std::get_if<LoweredStorageDeclaration>(&row->payload);
      const LoweredDeclaration *owner =
          storage == nullptr ? nullptr : loweredClassRow(storage->ownerClass);
      if (storage == nullptr || owner == nullptr) {
        throw std::logic_error(
            "lowered static field definition lost its owner");
      }
      const std::size_t namespaceCount =
          emitLoweredNamespaceOpen(row->namespaceScope);
      emitLoweredClassTemplateHeaders(*owner);
      writeIndent();
      output << "inline ";
      emitLoweredStorageType(*storage, false);
      output << ' ';
      emitLoweredPrimaryClassName(*owner);
      output << "::" << row->name;
      emitLoweredStorageInitializer(*storage);
      output << ";\n";
      emitLoweredNamespaceClose(namespaceCount);
      output << '\n';
    }
    loweredDeferredStaticFields.clear();
  }

  void emitLoweredMemberSpecializationDeclarations(
      std::size_t firstDeferredMember, std::size_t lastDeferredMember,
      std::size_t firstDeferredLifecycleClass,
      std::size_t lastDeferredLifecycleClass) {
    for (std::size_t index = firstDeferredMember; index < lastDeferredMember;
         ++index) {
      const LoweredDeclaration *row = loweredDeferredMembers[index];
      const LoweredDeclaration *owner = loweredClassRow(row->ownerClass);
      if (owner == nullptr ||
          (!loweredOwnerIsGeneric(*owner) && !row->generic)) {
        continue;
      }
      const std::size_t namespaceCount =
          emitLoweredNamespaceOpen(row->namespaceScope);
      if (const auto *function =
              std::get_if<LoweredFunctionDeclaration>(&row->payload)) {
        if (function->pureVirtual ||
            function->definitionKind != MirDefinitionKind::Source) {
          emitLoweredNamespaceClose(namespaceCount);
          continue;
        }
        for (const MirFunctionInstance *instance :
             loweredFunctionInstances(function->id)) {
          if (!instance->owner) {
            continue;
          }
          const MirClassInstance *ownerInstance =
              mir.findClassInstance(*instance->owner);
          if (ownerInstance == nullptr ||
              !loweredTypeIsNameable(ownerInstance->type)) {
            continue;
          }
          const std::string ownerSpelling =
              loweredConcreteClassName(*instance->owner);
          if (loweredFailureBodySupported(*instance) &&
              loweredOwnerIsGeneric(*owner)) {
            writeIndent();
            output << "template <> ";
            emitLoweredFunctionSignature(*row, *function, instance, true, false,
                                         ownerSpelling);
            output << ";\n";
          }
          const bool ordinaryBoundaryWrapper =
              loweredFailureBodySupported(*instance) &&
              !loweredNormalBodySupported(*instance) &&
              ((cppMirBodyStatusCannotFail(instance->body) &&
                cppMirNeedsFailureFreeBoundaryWrapper(mir, *generalEmissionMap,
                                                      instance->id)) ||
               cppMirNeedsTerminalSubscriptBoundaryWrapper(
                   mir, *generalEmissionMap, instance->id));
          if (loweredNormalBodySupported(*instance) ||
              ordinaryBoundaryWrapper) {
            writeIndent();
            output << "template <> ";
            emitLoweredFunctionSignature(*row, *function, instance, false,
                                         false, ownerSpelling, false,
                                         !function->genericParameters.empty());
            output << ";\n";
          }
        }
      } else if (const auto *constructor =
                     std::get_if<LoweredConstructorDeclaration>(
                         &row->payload)) {
        for (const MirConstructorInstance *instance :
             loweredConstructorInstances(constructor->id)) {
          if (instance->definitionKind != MirDefinitionKind::Source) {
            continue;
          }
          const MirClassInstance *ownerInstance =
              mir.findClassInstance(instance->owner);
          if (ownerInstance == nullptr ||
              !loweredTypeIsNameable(ownerInstance->type)) {
            continue;
          }
          const std::string ownerSpelling =
              loweredConcreteClassName(instance->owner);
          const bool genericOwner = loweredOwnerIsGeneric(*owner);
          const MirBodyAddress address{.kind = MirBodyKind::Constructor,
                                       .owner = instance->id};
          if (instance->mayRaiseDefinedFailure &&
              generalMirBodyPlan(address).supportsFailureText() &&
              genericOwner) {
            writeIndent();
            output << "template <> " << ownerSpelling << "::" << owner->name
                   << '(';
            emitLoweredConstructorParameters(*instance, true);
            output << ");\n";
          }
          if (generalMirBodyPlan(address).supportsPlainText()) {
            writeIndent();
            output << "template <> " << ownerSpelling << "::" << owner->name
                   << '(';
            emitLoweredConstructorParameters(*instance, false);
            output << ");\n";
          }
        }
      } else if (std::holds_alternative<LoweredDestructorDeclaration>(
                     row->payload)) {
        for (const MirClassInstance &instance : mir.classInstances()) {
          if (instance.declaration != row->ownerClass || !instance.destructor ||
              !loweredTypeIsNameable(instance.type)) {
            continue;
          }
          writeIndent();
          output << "template <> " << loweredConcreteClassName(instance.id)
                 << "::~" << owner->name << "() noexcept;\n";
        }
      }
      emitLoweredNamespaceClose(namespaceCount);
    }

    for (std::size_t index = firstDeferredLifecycleClass;
         index < lastDeferredLifecycleClass; ++index) {
      const LoweredDeclaration *classRow =
          loweredDeferredLifecycleClasses[index];
      const auto *declaration =
          std::get_if<LoweredClassDeclaration>(&classRow->payload);
      if (declaration == nullptr || !loweredOwnerIsGeneric(*classRow)) {
        continue;
      }
      const std::size_t namespaceCount =
          emitLoweredNamespaceOpen(classRow->namespaceScope);
      for (const CppMirGeneratedThunk &thunk : programPlan.thunks) {
        const auto *cleanup =
            thunk.identity.kind == CppMirThunkKind::LifecycleCleanup
                ? std::get_if<CppMirLifecycleCleanupThunk>(&thunk.payload)
                : nullptr;
        if (cleanup == nullptr || cleanup->ownerClass != declaration->id ||
            cleanup->form !=
                CppMirLifecycleCleanupForm::ConcreteSpecialization) {
          continue;
        }
        const std::string ownerSpelling =
            loweredConcreteClassName(cleanup->classInstance);
        writeIndent();
        output << "template <> void " << ownerSpelling
               << "::" << loweredLifecycleCleanupName(declaration->id)
               << "() noexcept;\n";
        if (cleanup->mayRaiseDefinedFailure) {
          writeIndent();
          output << "template <> bool " << ownerSpelling
                 << "::" << lifecycleFailureDispatchName()
                 << "(::gti_failure_record_v1 *);\n";
          writeIndent();
          output << "template <> bool " << ownerSpelling
                 << "::" << lifecycleFailureBodyName()
                 << "(::gti_failure_record_v1 *);\n";
        }
      }
      emitLoweredNamespaceClose(namespaceCount);
    }
    output << '\n';
  }

  [[nodiscard]] std::string
  loweredPrimaryClassSpelling(const LoweredDeclaration &classRow) const {
    std::string result;
    const auto append = [&](const auto &self,
                            const LoweredDeclaration &row) -> void {
      const LoweredDeclaration *parent =
          loweredProgram.findDeclaration(row.parent);
      if (parent != nullptr &&
          std::holds_alternative<LoweredClassDeclaration>(parent->payload)) {
        self(self, *parent);
        result += "::";
      }
      result += row.name;
      const auto *declaration =
          std::get_if<LoweredClassDeclaration>(&row.payload);
      if (declaration == nullptr) {
        return;
      }
      if (declaration->exactSpecializationPrimary != 0) {
        result += loweredExactClassArguments(*declaration);
        return;
      }
      if (declaration->genericParameters.empty()) {
        return;
      }
      result += '<';
      for (std::size_t index = 0; index < declaration->genericParameters.size();
           ++index) {
        if (index != 0) {
          result += ", ";
        }
        result += declaration->genericParameters[index].name;
        if (declaration->genericParameters[index].pack) {
          result += "...";
        }
      }
      result += '>';
    };
    append(append, classRow);
    return result;
  }

  [[nodiscard]] bool
  loweredOwnerIsGeneric(const LoweredDeclaration &classRow) const {
    const LoweredDeclaration *current = &classRow;
    while (current != nullptr) {
      const auto *declaration =
          std::get_if<LoweredClassDeclaration>(&current->payload);
      if (declaration != nullptr && !declaration->genericParameters.empty()) {
        return true;
      }
      current = loweredProgram.findDeclaration(current->parent);
      if (current != nullptr &&
          !std::holds_alternative<LoweredClassDeclaration>(current->payload)) {
        current = nullptr;
      }
    }
    return false;
  }

  [[nodiscard]] bool
  loweredClassHasConcreteInstance(ClassId declaration) const {
    return std::any_of(mir.classInstances().begin(), mir.classInstances().end(),
                       [declaration](const MirClassInstance &instance) {
                         return instance.declaration == declaration &&
                                (!instance.type.arguments.empty() ||
                                 !instance.type.valueArguments.empty());
                       });
  }

  void emitLoweredVirtualFailureBoundaryWrapper(
      const LoweredDeclaration &row, const LoweredFunctionDeclaration &function,
      const MirFunctionInstance &instance, std::string_view ownerSpelling,
      bool explicitSpecialization) {
    const MirFunctionInstance *root =
        cppMirVirtualFailureContractRoot(mir, instance);
    if (root == nullptr ||
        !loweredVirtualFailureContractRetained(root->declaration)) {
      throw std::logic_error(
          "lowered virtual failure body lost its contract root");
    }
    writeIndent();
    if (explicitSpecialization) {
      output << "template <> ";
    }
    emitLoweredFunctionSignature(row, function, &instance, false, false,
                                 ownerSpelling, false,
                                 !function.genericParameters.empty());
    output << " {\n";
    ++indentation;
    writeIndent();
    output << "::gti_failure_record_v1 __gti_failure_record{};\n";
    if (instance.returnType.kind == SemanticType::Reference) {
      writeIndent();
      if (instance.returnType.referenceAccess == AccessMode::ReadOnly) {
        output << "const ";
      }
      emitSemanticType(instance.returnType.arguments.front());
      output << " *__gti_result = nullptr;\n";
    } else if (instance.returnType != SemanticType::Void) {
      writeIndent();
      emitSemanticType(instance.returnType);
      output << " __gti_result{};\n";
    }
    writeIndent();
    output << "if (!";
    if (function.receiverMutability == ReceiverMutability::Consuming) {
      output << "std::move(*this).";
    }
    output << cppMirFailureSiblingSpelling(
                  cppFunctionSpelling(function, row.name))
           << '(';
    for (std::size_t index = 0; index < instance.parameterTypes.size();
         ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << "__gti_mir_arg_" << index;
    }
    if (!instance.parameterTypes.empty()) {
      output << ", ";
    }
    if (instance.returnType != SemanticType::Void) {
      output << "&__gti_result, ";
    }
    output << "&__gti_failure_record)) {\n";
    ++indentation;
    emitHostedFailureTermination();
    --indentation;
    writeIndent();
    output << "}\n";
    if (instance.returnType.kind == SemanticType::Reference) {
      writeIndent();
      output << "return *__gti_result;\n";
    } else if (instance.returnType != SemanticType::Void) {
      writeIndent();
      output << "return __gti_result;\n";
    }
    --indentation;
    writeIndent();
    output << "}\n";
  }

  // A source member can need an ordinary signature even when only its failure
  // body is directly representable. This covers both a proven failure-free
  // nested producer and the class-subscript place compatibility path, which
  // has no call result on which to encode a failure edge. Publish a narrow
  // adapter over the admitted transformed body and contain any failure
  // terminally; the caller never observes an invented success value.
  void emitLoweredOrdinaryFailureBoundaryWrapper(
      const LoweredDeclaration &row, const LoweredFunctionDeclaration &function,
      const MirFunctionInstance &instance, std::string_view ownerSpelling,
      bool explicitSpecialization) {
    const bool placementResult =
        instance.returnType.kind == SemanticType::Class ||
        instance.returnType.kind == SemanticType::Lambda;
    writeIndent();
    if (explicitSpecialization) {
      output << "template <> ";
    }
    emitLoweredFunctionSignature(row, function, &instance, false, false,
                                 ownerSpelling);
    output << " {\n";
    ++indentation;
    writeIndent();
    output << "::gti_failure_record_v1 __gti_failure_record{};\n";
    if (instance.returnType.kind == SemanticType::Reference) {
      writeIndent();
      if (instance.returnType.referenceAccess == AccessMode::ReadOnly) {
        output << "const ";
      }
      emitSemanticType(instance.returnType.arguments.front());
      output << " *__gti_result = nullptr;\n";
    } else if (placementResult) {
      writeIndent();
      output << "::gti_internal::backend::mir_lifetime_slot<";
      emitSemanticType(instance.returnType);
      output << ", true> __gti_result;\n";
    } else if (instance.returnType != SemanticType::Void) {
      writeIndent();
      emitSemanticType(instance.returnType);
      output << " __gti_result{};\n";
    }
    writeIndent();
    output << "if (!"
           << cppMirFailureSiblingSpelling(
                  cppFunctionSpelling(function, row.name))
           << '(';
    for (std::size_t index = 0; index < instance.parameterTypes.size();
         ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << "__gti_mir_arg_" << index;
    }
    if (!instance.parameterTypes.empty()) {
      output << ", ";
    }
    if (instance.returnType != SemanticType::Void) {
      output << (placementResult ? "__gti_result.construction_address(), "
                                 : "&__gti_result, ");
    }
    output << "&__gti_failure_record)) {\n";
    ++indentation;
    emitHostedFailureTermination();
    --indentation;
    writeIndent();
    output << "}\n";
    if (placementResult) {
      writeIndent();
      output << "__gti_result.mark_constructed();\n";
    }
    if (instance.returnType.kind == SemanticType::Reference) {
      writeIndent();
      output << "return *__gti_result;\n";
    } else if (placementResult) {
      writeIndent();
      output << "return std::move(__gti_result.get());\n";
    } else if (instance.returnType != SemanticType::Void) {
      writeIndent();
      output << "return __gti_result;\n";
    }
    --indentation;
    writeIndent();
    output << "}\n";
  }

  void emitLoweredMemberFunctionDefinition(
      const LoweredDeclaration &row, const LoweredFunctionDeclaration &function,
      const LoweredDeclaration &owner) {
    if (function.definitionKind != MirDefinitionKind::Source ||
        function.intrinsic != IntrinsicKind::None || function.pureVirtual) {
      return;
    }
    const std::vector<const MirFunctionInstance *> instances =
        loweredFunctionInstances(function.id);
    const bool genericOwner = loweredOwnerIsGeneric(owner);
    const bool generic = genericOwner || !function.genericParameters.empty();
    const bool threadTaskTemplate =
        !genericOwner && loweredFunctionNeedsThreadTaskTemplate(instances);
    // A callable member template whose concrete instance contains an
    // unnameable closure type must emit from the source-template overlay,
    // just like a free callable template. Concrete-specialization emission
    // cannot name that closure in a C++ signature.
    if (!genericOwner && !function.genericParameters.empty()) {
      const std::string callableFailure =
          loweredCallableTemplateBody(function, instances, true);
      const std::string callableNormal =
          loweredCallableTemplateBody(function, instances, false);
      const std::string ownerSpelling = loweredPrimaryClassSpelling(owner);
      if (!callableFailure.empty()) {
        emitLoweredTemplateDeclaration(function.genericParameters,
                                       threadTaskTemplate);
        writeIndent();
        emitLoweredFunctionSignature(row, function, nullptr, true, false,
                                     ownerSpelling, true);
        output << ' ' << callableFailure;
      }
      if (!callableNormal.empty()) {
        emitLoweredTemplateDeclaration(function.genericParameters,
                                       threadTaskTemplate);
        writeIndent();
        emitLoweredFunctionSignature(row, function, nullptr, false, false,
                                     ownerSpelling, true);
        output << ' ' << callableNormal;
      }
    }
    if (genericOwner && !function.genericParameters.empty()) {
      std::vector<HirClassInstanceId> emittedOwners;
      for (const MirFunctionInstance *instance : instances) {
        const LoweredFunctionInstance *concrete =
            loweredFunctionInstance(instance->id);
        if (!instance->owner || concrete == nullptr ||
            loweredFunctionInstanceIsNameable(*concrete, *instance) ||
            std::find(emittedOwners.begin(), emittedOwners.end(),
                      *instance->owner) != emittedOwners.end()) {
          continue;
        }

        std::vector<const MirFunctionInstance *> ownerInstances;
        std::copy_if(instances.begin(), instances.end(),
                     std::back_inserter(ownerInstances),
                     [&](const MirFunctionInstance *candidate) {
                       return candidate->owner == instance->owner;
                     });
        const std::string callableFailure =
            loweredCallableTemplateBody(function, ownerInstances, true);
        const std::string callableNormal =
            loweredCallableTemplateBody(function, ownerInstances, false);
        if (callableFailure.empty() && callableNormal.empty()) {
          continue;
        }

        emittedOwners.push_back(*instance->owner);
        const std::string ownerSpelling =
            loweredConcreteClassName(*instance->owner);
        const bool ownerThreadTaskTemplate =
            loweredFunctionNeedsThreadTaskTemplate(ownerInstances);
        const auto emitCallableBody = [&](const std::string &body,
                                          bool failure) {
          if (body.empty()) {
            return;
          }
          writeIndent();
          output << "template <>\n";
          emitLoweredTemplateDeclaration(function.genericParameters,
                                         ownerThreadTaskTemplate);
          writeIndent();
          emitLoweredFunctionSignature(row, function, instance, failure, false,
                                       ownerSpelling, true);
          output << ' ' << body;
        };
        emitCallableBody(callableFailure, true);
        emitCallableBody(callableNormal, false);
      }
    }
    for (const MirFunctionInstance *instance : instances) {
      if (!loweredNormalBodySupported(*instance) &&
          !loweredFailureBodySupported(*instance)) {
        continue;
      }
      std::string ownerSpelling = loweredPrimaryClassSpelling(owner);
      if (generic) {
        if (!instance->owner) {
          throw std::logic_error(
              "generic lowered member function lost its owner instance");
        }
        ownerSpelling = loweredConcreteClassName(*instance->owner);
      }
      const auto emitPrefix = [&](bool failureForm) {
        writeIndent();
        if (genericOwner ||
            (!failureForm && !function.genericParameters.empty())) {
          output << "template <> ";
        }
      };
      if (loweredFailureBodySupported(*instance)) {
        emitPrefix(true);
        emitLoweredFunctionSignature(row, function, instance, true, false,
                                     ownerSpelling);
        output << ' ';
        emitLoweredFailureFunctionBody(*instance);
        if (instance->virtualMethod) {
          emitLoweredVirtualFailureBoundaryWrapper(row, function, *instance,
                                                   ownerSpelling, generic);
        } else if (!loweredNormalBodySupported(*instance) &&
                   ((cppMirBodyStatusCannotFail(instance->body) &&
                     cppMirNeedsFailureFreeBoundaryWrapper(
                         mir, *generalEmissionMap, instance->id)) ||
                    cppMirNeedsTerminalSubscriptBoundaryWrapper(
                        mir, *generalEmissionMap, instance->id))) {
          emitLoweredOrdinaryFailureBoundaryWrapper(row, function, *instance,
                                                    ownerSpelling, generic);
        }
      }
      if (loweredNormalBodySupported(*instance) &&
          !(instance->virtualMethod &&
            loweredFailureBodySupported(*instance))) {
        emitPrefix(false);
        emitLoweredFunctionSignature(row, function, instance, false, false,
                                     ownerSpelling, false,
                                     !function.genericParameters.empty());
        output << ' ';
        emitLoweredNormalFunctionBody(*instance);
      }
    }
  }

  void emitLoweredConstructorParameters(const MirConstructorInstance &instance,
                                        bool failure) {
    if (failure) {
      output << cppMirFailureConstructorTagSpelling(instance.id);
      output << ", ";
    }
    const std::size_t count = emitLoweredMirParameters(instance.parameterTypes);
    if (!failure) {
      return;
    }
    if (count != 0) {
      output << ", ";
    }
    output << "bool *__gti_mir_constructor_success, "
              "::gti_failure_record_v1 *__gti_mir_failure_record";
  }

  void emitLoweredGenericOwnerConstructorParameters(
      const LoweredConstructorDeclaration &constructor,
      const MirConstructorInstance &instance, bool failure) {
    bool separator = false;
    if (failure) {
      output << cppMirFailureConstructorTagSpelling(instance.id);
      separator = true;
    }
    if (!constructor.parameters.empty()) {
      if (separator) {
        output << ", ";
      }
      emitLoweredParameters(constructor.parameters, true);
      separator = true;
    }
    if (!failure) {
      return;
    }
    if (separator) {
      output << ", ";
    }
    output << "bool *__gti_mir_constructor_success, "
              "::gti_failure_record_v1 *__gti_mir_failure_record";
  }

  [[nodiscard]] static std::string
  loweredBodyWithoutInstanceMarker(std::string body) {
    const std::size_t marker = body.find("// GTI verified-MIR body:");
    if (marker == std::string::npos) {
      return body;
    }
    std::size_t lineStart = marker;
    while (lineStart != 0 && body[lineStart - 1] != '\n') {
      --lineStart;
    }
    const std::size_t lineEnd = body.find('\n', marker);
    body.erase(lineStart, lineEnd == std::string::npos
                              ? std::string::npos
                              : lineEnd - lineStart + 1);
    return body;
  }

  void emitLoweredConstructorDefinition(
      const LoweredDeclaration &row,
      const LoweredConstructorDeclaration &constructor,
      const LoweredDeclaration &owner) {
    const std::vector<const MirConstructorInstance *> instances =
        loweredConstructorInstances(constructor.id);
    const bool generic =
        loweredOwnerIsGeneric(owner) || !constructor.genericParameters.empty();
    const bool genericOwner = loweredOwnerIsGeneric(owner);
    std::optional<std::string> genericNormalBody;
    for (const MirConstructorInstance *instance : instances) {
      if (instance->definitionKind != MirDefinitionKind::Source) {
        continue;
      }
      const MirBodyAddress address{.kind = MirBodyKind::Constructor,
                                   .owner = instance->id};
      const std::optional<std::string> genericNormal =
          loweredGenericOwnerConstructorBodyText(constructor, *instance, false);
      const std::optional<std::string> genericFailure =
          instance->mayRaiseDefinedFailure
              ? loweredGenericOwnerConstructorBodyText(constructor, *instance,
                                                       true)
              : std::nullopt;
      const MirClassInstance *ownerInstance =
          mir.findClassInstance(instance->owner);
      const bool unnameableOwner = ownerInstance != nullptr &&
                                   !loweredTypeIsNameable(ownerInstance->type);
      if (unnameableOwner) {
        if (!genericNormal && !genericFailure) {
          throw std::logic_error(
              "unnameable generic-owner constructor has no source-template "
              "MIR representation");
        }
        if (genericFailure) {
          emitLoweredClassTemplateHeaders(owner);
          emitLoweredTemplateDeclaration(constructor.genericParameters);
          writeIndent();
          output << loweredPrimaryClassSpelling(owner) << "::" << owner.name
                 << '(';
          emitLoweredGenericOwnerConstructorParameters(constructor, *instance,
                                                       true);
          output << ')';
          emitGeneralMirConstructorInitializerList(*instance, true);
          output << ' ' << *genericFailure;
        }
        if (loweredContainedConstructorRepresentation(instance->id) !=
            nullptr) {
          if (!genericFailure) {
            throw std::logic_error(
                "contained generic-owner constructor has no admitted failure "
                "body");
          }
          emitLoweredClassTemplateHeaders(owner);
          emitLoweredTemplateDeclaration(constructor.genericParameters);
          writeIndent();
          output << loweredPrimaryClassSpelling(owner) << "::" << owner.name
                 << '(';
          emitLoweredContainedConstructorParameters(constructor, *instance,
                                                    true);
          output << ')';
          emitLoweredContainedConstructorBody(owner, *instance);
        }
        if (genericNormal) {
          const std::string normalized =
              loweredBodyWithoutInstanceMarker(*genericNormal);
          if (genericNormalBody && *genericNormalBody != normalized) {
            throw std::logic_error(
                "generic-owner constructor instances do not share one "
                "source-template MIR representation");
          }
          if (!genericNormalBody) {
            genericNormalBody = normalized;
            emitLoweredClassTemplateHeaders(owner);
            emitLoweredTemplateDeclaration(constructor.genericParameters);
            writeIndent();
            output << loweredPrimaryClassSpelling(owner) << "::" << owner.name
                   << '(';
            emitLoweredGenericOwnerConstructorParameters(constructor, *instance,
                                                         false);
            output << ')';
            emitGeneralMirConstructorInitializerList(*instance);
            output << ' ' << *genericNormal;
          }
        }
        continue;
      }
      const bool normal = generalMirBodyPlan(address).supportsPlainText();
      const bool failure = instance->mayRaiseDefinedFailure &&
                           generalMirBodyPlan(address).supportsFailureText();
      if (!normal && !failure) {
        throw std::logic_error(
            "source lowered constructor has no admitted MIR body");
      }
      const std::string ownerSpelling =
          generic ? loweredConcreteClassName(instance->owner)
                  : loweredPrimaryClassSpelling(owner);
      const auto emitSignature = [&](bool failureForm) {
        writeIndent();
        if (genericOwner || (generic && !failureForm)) {
          output << "template <> ";
        }
        output << ownerSpelling << "::" << owner.name << '(';
        emitLoweredConstructorParameters(*instance, failureForm);
        output << ')';
      };
      if (failure) {
        emitSignature(true);
        emitGeneralMirConstructorInitializerList(*instance, true);
        output << ' ';
        emitGeneralFailureConstructorBodyText(*instance);
      }
      if (loweredContainedConstructorRepresentation(instance->id) != nullptr) {
        if (!failure) {
          throw std::logic_error(
              "contained constructor has no admitted failure body");
        }
        writeIndent();
        if (genericOwner) {
          output << "template <> ";
        }
        output << ownerSpelling << "::" << owner.name << '(';
        emitLoweredContainedConstructorParameters(constructor, *instance,
                                                  false);
        output << ')';
        emitLoweredContainedConstructorBody(owner, *instance);
      }
      if (normal) {
        emitSignature(false);
        emitGeneralMirConstructorInitializerList(*instance);
        output << ' ';
        emitGeneralMirConstructorBodyText(*instance);
      }
    }
    static_cast<void>(row);
  }

  void emitLoweredDestructorDefinition(
      const LoweredDeclaration &row,
      const LoweredDestructorDeclaration &destructor,
      const LoweredDeclaration &owner) {
    const auto *classDeclaration =
        std::get_if<LoweredClassDeclaration>(&owner.payload);
    if (classDeclaration == nullptr) {
      throw std::logic_error(
          "lowered destructor definition lost its class declaration");
    }
    const bool generic = loweredOwnerIsGeneric(owner);
    bool emitted = false;
    const bool hasConcreteOwnerInstance =
        loweredClassHasConcreteInstance(destructor.owner);
    for (const MirClassInstance &classInstance : mir.classInstances()) {
      if (classInstance.declaration != destructor.owner) {
        continue;
      }
      const bool concrete = !classInstance.type.arguments.empty() ||
                            !classInstance.type.valueArguments.empty();
      if (generic && !concrete) {
        continue;
      }
      if (!classInstance.destructor) {
        continue;
      }
      const std::string ownerSpelling =
          generic ? loweredConcreteClassName(classInstance.id)
                  : loweredPrimaryClassSpelling(owner);
      writeIndent();
      if (generic) {
        output << "template <> ";
      }
      output << ownerSpelling << "::~" << owner.name << "() noexcept ";
      if (!classDeclaration->requiresActiveDropState) {
        throw std::logic_error(
            "declared lowered destructor has no lifecycle cleanup adapter");
      }
      output << "{ " << loweredLifecycleCleanupName(classDeclaration->id)
             << "(); }\n";
      emitted = true;
      if (!generic) {
        break;
      }
    }
    if (!emitted && (!generic || hasConcreteOwnerInstance)) {
      throw std::logic_error(
          "lowered destructor declaration has no concrete MIR instance");
    }
    static_cast<void>(row);
  }

  void emitLoweredLifecycleDefinitions() {
    for (const LoweredDeclaration *classRow : loweredDeferredLifecycleClasses) {
      const auto *classDeclaration =
          std::get_if<LoweredClassDeclaration>(&classRow->payload);
      if (classDeclaration == nullptr) {
        throw std::logic_error(
            "lowered lifecycle cleanup lost its class declaration");
      }
      const std::size_t namespaceCount =
          emitLoweredNamespaceOpen(classRow->namespaceScope);
      bool emitted = false;
      for (const CppMirGeneratedThunk &thunk : programPlan.thunks) {
        if (thunk.identity.kind != CppMirThunkKind::LifecycleCleanup) {
          continue;
        }
        const auto *cleanup =
            std::get_if<CppMirLifecycleCleanupThunk>(&thunk.payload);
        if (cleanup == nullptr || cleanup->ownerClass != classDeclaration->id) {
          continue;
        }
        const MirDestructorInstance *destructor =
            mir.findDestructorInstance(cleanup->destructorInstance);
        if (destructor == nullptr ||
            destructor->owner != cleanup->classInstance ||
            destructor->mayRaiseDefinedFailure !=
                cleanup->mayRaiseDefinedFailure) {
          throw std::logic_error(
              "lowered lifecycle cleanup lost its exact destructor body");
        }
        const bool concrete =
            cleanup->form == CppMirLifecycleCleanupForm::ConcreteSpecialization;
        const std::string ownerSpelling =
            concrete ? loweredConcreteClassName(cleanup->classInstance)
                     : loweredPrimaryClassSpelling(*classRow);
        const auto emitPrefix = [&] {
          writeIndent();
          if (concrete) {
            output << "template <> ";
          }
        };
        const MirBodyAddress address{.kind = MirBodyKind::Destructor,
                                     .owner = destructor->id};
        const bool failure = destructor->mayRaiseDefinedFailure &&
                             generalMirBodyPlan(address).supportsFailureText();
        if (failure) {
          emitPrefix();
          output << "bool " << ownerSpelling
                 << "::" << lifecycleFailureBodyName()
                 << "(::gti_failure_record_v1 "
                    "*__gti_mir_failure_record) ";
          emitGeneralFailureDestructorBodyText(*destructor);

          emitPrefix();
          output << "bool " << ownerSpelling
                 << "::" << lifecycleFailureDispatchName()
                 << "(::gti_failure_record_v1 "
                    "*__gti_mir_failure_record) {\n";
          ++indentation;
          writeIndent();
          output << "if (!" << loweredLifecycleActiveName(classDeclaration->id)
                 << ") { return true; }\n";
          writeIndent();
          output << loweredLifecycleActiveName(classDeclaration->id)
                 << " = false;\n";
          writeIndent();
          output << "return " << lifecycleFailureBodyName()
                 << "(__gti_mir_failure_record);\n";
          --indentation;
          writeIndent();
          output << "}\n";
        }

        emitPrefix();
        output << "void " << ownerSpelling
               << "::" << loweredLifecycleCleanupName(classDeclaration->id)
               << "() noexcept {\n";
        ++indentation;
        if (failure) {
          writeIndent();
          output << "::gti_failure_record_v1 __gti_failure_record{};\n";
          writeIndent();
          output << "if (!" << lifecycleFailureDispatchName()
                 << "(&__gti_failure_record)) {\n";
          ++indentation;
          if (!mir.threadTaskAdapters().empty()) {
            writeIndent();
            output << "if (!::gti_runtime_detail::"
                      "record_managed_task_cleanup_failure(\n";
            ++indentation;
            writeIndent();
            output << "__gti_failure_record,\n";
            writeIndent();
            output << "&::gti_internal::backend::"
                      "__gti_failure_artifact_descriptor_v1)) {\n";
            ++indentation;
            emitHostedFailureTermination();
            --indentation;
            writeIndent();
            output << "}\n";
            --indentation;
          } else {
            emitHostedFailureTermination();
          }
          --indentation;
          writeIndent();
          output << "}\n";
        } else {
          if (!generalMirBodyPlan(address).supportsPlainText()) {
            throw std::logic_error(
                "lowered lifecycle cleanup has no admitted destructor "
                "body");
          }
          writeIndent();
          output << "if (!" << loweredLifecycleActiveName(classDeclaration->id)
                 << ") { return; }\n";
          writeIndent();
          output << loweredLifecycleActiveName(classDeclaration->id)
                 << " = false;\n";
          emitGeneralMirDestructorBodyText(*destructor, "scalar-cfg-v1");
        }
        --indentation;
        writeIndent();
        output << "}\n\n";
        emitted = true;
      }
      if (!emitted && (!loweredOwnerIsGeneric(*classRow) ||
                       loweredClassHasConcreteInstance(classDeclaration->id))) {
        throw std::logic_error(
            "lowered active-drop class has no lifecycle generated item");
      }
      emitLoweredNamespaceClose(namespaceCount);
    }
    loweredDeferredLifecycleClasses.clear();
  }

  void emitLoweredMemberDefinitions() {
    for (const LoweredDeclaration *row : loweredDeferredMembers) {
      const LoweredDeclaration *owner = loweredClassRow(row->ownerClass);
      if (owner == nullptr) {
        throw std::logic_error(
            "lowered deferred member definition lost its class owner");
      }
      const std::size_t namespaceCount =
          emitLoweredNamespaceOpen(row->namespaceScope);
      if (const auto *function =
              std::get_if<LoweredFunctionDeclaration>(&row->payload)) {
        emitLoweredMemberFunctionDefinition(*row, *function, *owner);
      } else if (const auto *constructor =
                     std::get_if<LoweredConstructorDeclaration>(
                         &row->payload)) {
        emitLoweredConstructorDefinition(*row, *constructor, *owner);
      } else if (const auto *destructor =
                     std::get_if<LoweredDestructorDeclaration>(&row->payload)) {
        emitLoweredDestructorDefinition(*row, *destructor, *owner);
      }
      emitLoweredNamespaceClose(namespaceCount);
      output << '\n';
    }
    loweredDeferredMembers.clear();
    emitLoweredLifecycleDefinitions();
  }

  void collectLoweredConstexprAggregateClasses(const ConstantValue &constant) {
    const auto *aggregate = std::get_if<ConstantAggregateValue>(&constant);
    if (aggregate == nullptr || aggregate->value == nullptr) {
      return;
    }
    if (aggregate->value->kind == ConstantAggregateKind::Object) {
      if (aggregate->value->exactType == nullptr ||
          aggregate->value->exactType->kind != SemanticType::Class ||
          aggregate->value->exactType->classId != aggregate->value->identity) {
        throw std::logic_error(
            "object constant lost its exact concrete class type");
      }
      loweredConstexprAggregateClasses.insert(aggregate->value->identity);
      for (const ConstantAggregateField &field : aggregate->value->fields) {
        collectLoweredConstexprAggregateClasses(field.value);
      }
      return;
    }
    for (const ConstantValue &element : aggregate->value->elements) {
      collectLoweredConstexprAggregateClasses(element);
    }
  }

  void prepareLoweredConstexprAggregateClasses() {
    loweredConstexprAggregateClasses.clear();
    for (const LoweredDeclaration &declaration :
         loweredProgram.declarations()) {
      const auto *storage =
          std::get_if<LoweredStorageDeclaration>(&declaration.payload);
      if (storage != nullptr && storage->constant) {
        collectLoweredConstexprAggregateClasses(*storage->constant);
      }
    }
  }

  void buildLoweredProgramTail(CppProgramEmissionPlan &plan) {
    indexLoweredDeclarations();
    prepareLoweredConstexprAggregateClasses();
    capture(plan, CppProgramTextRole::DeclarationTypeMemberLifecycle,
            [&] { emitLoweredPublicOpaqueHandleForwardDeclarations(); });
    prepareLoweredModuleInitialization();

    capture(plan, CppProgramTextRole::DeclarationTypeMemberLifecycle, [&] {
      output << "namespace __gti_program {\n";
      ++indentation;
      emitLoweredTypeForwardDeclarations(0);
      emitLoweredExactClassSpecializationForwardDeclarations();
      emitLoweredAliasForwardDeclarations(0);
      emitLoweredFunctionForwardDeclarations(0);
    });
    capture(plan, CppProgramTextRole::NativeAndThreadAdapter,
            [&] { emitNativeCallbackAdapterDeclarations(); });
    capture(plan, CppProgramTextRole::DeclarationTypeMemberLifecycle, [&] {
      emitLoweredGlobalStorageForwardDeclarations(0);
      emitLoweredTypeDefinitions();
      emitLoweredSourceDeclarations(0);
      emitLoweredStaticFieldDefinitions();
      emitLoweredMemberDefinitions();
    });
    capture(plan, CppProgramTextRole::NativeAndThreadAdapter,
            [&] { emitNativeCallbackAdapterDefinitions(); });
    capture(plan, CppProgramTextRole::InitializerAndModuleAdapter, [&] {
      emitRuntimeModuleInitializer();
      if (!moduleRuntimeEmission) {
        writeIndent();
        output << "// GTI verified-MIR body: scalar-cfg-v1 module-instance 0\n";
      }
      --indentation;
      output << "} // namespace __gti_program\n";
    });
    capture(plan, CppProgramTextRole::HostedEntryAdapter,
            [&] { emitLoweredProgramEntryAdapter(); });
  }

  template <typename Callback>
  void capture(CppProgramEmissionPlan &plan, CppProgramTextRole role,
               Callback &&callback) {
    output.str("");
    output.clear();
    callback();
    plan.append(role, output.str());
  }

  [[nodiscard]] static bool validMacroName(std::string_view name) {
    if (name.empty() ||
        !(name.front() == '_' || (name.front() >= 'A' && name.front() <= 'Z') ||
          (name.front() >= 'a' && name.front() <= 'z'))) {
      return false;
    }
    return std::all_of(name.begin() + 1, name.end(), [](char character) {
      return character == '_' || (character >= 'A' && character <= 'Z') ||
             (character >= 'a' && character <= 'z') ||
             (character >= '0' && character <= '9');
    });
  }

  [[nodiscard]] static constexpr std::string_view
  lifecycleFailureDispatchName() {
    return "__gti_mir_destruct_failure";
  }

  [[nodiscard]] static constexpr std::string_view lifecycleFailureBodyName() {
    return "__gti_mir_destruct_body_failure";
  }

  [[nodiscard]] const CppMirThreadTaskThunk &
  threadTaskPayload(const CppMirGeneratedThunk &thunk) const {
    const auto *thread = std::get_if<CppMirThreadTaskThunk>(&thunk.payload);
    if (thunk.identity.kind != CppMirThunkKind::ThreadTaskAdapter ||
        thread == nullptr || thread->adapter.id != thunk.identity.owner ||
        mir.findThreadTaskAdapter(thread->adapter.id) == nullptr) {
      throw std::logic_error(
          "planned thread task lost its exact generated-item payload");
    }
    return *thread;
  }

  [[nodiscard]] const MirFunctionInstance *
  threadTaskFunctionTarget(const MirThreadTaskAdapter &adapter) const {
    if (!adapter.functionTarget) {
      return nullptr;
    }
    const MirFunctionInstance *target =
        mir.findFunctionInstance(*adapter.functionTarget);
    const MirClassInstance *owner = target == nullptr || !target->owner
                                        ? nullptr
                                        : mir.findClassInstance(*target->owner);
    const LoweredFunctionDeclaration *declaration =
        target == nullptr
            ? nullptr
            : loweredProgram.findFunctionDeclaration(target->declaration);
    const LoweredDeclaration *row =
        declaration == nullptr ? nullptr : loweredFunctionRow(declaration->id);
    const bool supportedBody =
        target != nullptr &&
        (target->mayRaiseDefinedFailure ? loweredFailureBodySupported(*target)
                                        : loweredNormalBodySupported(*target));
    if (target == nullptr || owner == nullptr || declaration == nullptr ||
        row == nullptr || owner->type != adapter.taskType ||
        target->definitionKind != MirDefinitionKind::Source ||
        target->overloadedOperator != std::optional{OverloadedOperator::Call} ||
        target->returnType != adapter.resultType ||
        !target->parameterTypes.empty() || target->pureVirtual ||
        adapter.targetInvocation !=
            callableInvocationCapability(target->receiverMutability) ||
        !supportedBody) {
      throw std::logic_error(
          "verified thread task adapter lost its exact function entry");
    }
    return target;
  }

  [[nodiscard]] const MirLambdaInstance *
  threadTaskLambdaTarget(const MirThreadTaskAdapter &adapter) const {
    if (!adapter.lambdaTarget) {
      return nullptr;
    }
    const MirLambdaInstance *target = mir.findLambda(*adapter.lambdaTarget);
    if (target == nullptr || target->type != adapter.taskType ||
        target->returnType != adapter.resultType ||
        !target->parameterTypes.empty() ||
        adapter.targetInvocation != CallableInvocationCapability::Read) {
      throw std::logic_error(
          "verified thread task adapter lost its exact lambda entry");
    }
    return target;
  }

  void emitThreadTaskReceiver(CallableInvocationCapability capability,
                              bool valueBearing) {
    const std::string_view task =
        valueBearing ? "box->task.get()" : "box->task";
    switch (capability) {
    case CallableInvocationCapability::Read:
      output << "read_only_receiver(" << task << ')';
      break;
    case CallableInvocationCapability::Mutable:
      output << "mutable_receiver(" << task << ')';
      break;
    case CallableInvocationCapability::Once:
      output << "std::move(" << task << ')';
      break;
    }
  }

  void emitThreadTaskFunctionCall(const MirThreadTaskAdapter &adapter,
                                  const MirFunctionInstance &target) {
    const LoweredFunctionDeclaration *declaration =
        loweredProgram.findFunctionDeclaration(target.declaration);
    const LoweredDeclaration *row = loweredFunctionRow(target.declaration);
    if (declaration == nullptr || row == nullptr) {
      throw std::logic_error(
          "verified thread task function lost its declaration spelling");
    }
    const std::string name = cppFunctionSpelling(*declaration, row->name);
    const bool valueBearing = adapter.resultType != SemanticType::Void;
    if (target.mayRaiseDefinedFailure) {
      output << "      const bool success = ";
      emitThreadTaskReceiver(adapter.targetInvocation, valueBearing);
      output << '.' << cppMirFailureSiblingSpelling(name) << '(';
      if (valueBearing) {
        output << "box->result.construction_address(), ";
      }
      output << "&failure);\n";
      if (valueBearing) {
        output << "      if (success) {\n"
                  "        box->result.mark_constructed();\n"
                  "      }\n";
      }
      output << "      if (!success) {\n"
                "        ::gti_runtime_detail::"
                "handoff_managed_worker_failure(\n"
                "            failure, "
                "&__gti_failure_artifact_descriptor_v1);\n"
                "      }\n";
      return;
    }
    output << "      ";
    if (valueBearing) {
      output << "box->result.construct_from([&] { return ";
    }
    emitThreadTaskReceiver(adapter.targetInvocation, valueBearing);
    output << '.' << name << "()";
    output << (valueBearing ? "; });\n" : ";\n");
  }

  void emitThreadTaskAdapterDefinitions() {
    bool opened = false;
    for (const CppMirGeneratedThunk &thunk : programPlan.thunks) {
      if (thunk.identity.kind != CppMirThunkKind::ThreadTaskAdapter) {
        continue;
      }
      const CppMirThreadTaskThunk &thread = threadTaskPayload(thunk);
      const MirThreadTaskAdapter &adapter = thread.adapter;
      if (adapter.invocation != CallableInvocationCapability::Once ||
          adapter.functionTarget.has_value() ==
              adapter.lambdaTarget.has_value() ||
          !adapter.catchesWorkerFailure || !adapter.catchesNativeExceptions) {
        throw std::logic_error(
            "verified thread task adapter lost its containment contract");
      }
      const MirFunctionInstance *functionTarget =
          threadTaskFunctionTarget(adapter);
      const MirLambdaInstance *lambdaTarget = threadTaskLambdaTarget(adapter);
      if ((functionTarget != nullptr) == (lambdaTarget != nullptr)) {
        throw std::logic_error(
            "verified thread task adapter has no unique entry target");
      }
      if (!opened) {
        output << "namespace gti_internal::backend {\n\n";
        opened = true;
      }
      const bool valueBearing = adapter.resultType != SemanticType::Void;
      output << "template <>\n"
                "struct managed_thread_task_adapter<"
             << adapter.id << "> final {\n";
      if (valueBearing) {
        output << "  template <typename Task, typename Result>\n"
                  "  static void entry_value(void *raw) noexcept {\n"
                  "    auto *box = static_cast<"
                  "managed_thread_value_box<Task, Result> *>(raw);\n";
      } else {
        output << "  template <typename Task>\n"
                  "  static void entry(void *raw) noexcept {\n"
                  "    auto *box = static_cast<managed_thread_task_box<Task> "
                  "*>(raw);\n";
      }
      output << "    ::gti_runtime_detail::run_managed_task_adapter([&] {\n"
                "      ::gti_failure_record_v1 failure{};\n";
      if (functionTarget != nullptr) {
        emitThreadTaskFunctionCall(adapter, *functionTarget);
      } else if (valueBearing) {
        output << "      if constexpr (requires { "
                  "std::move(box->task.get())("
                  "box->result.construction_address(), &failure); }) {\n"
                  "        const bool success = "
                  "std::move(box->task.get())("
                  "box->result.construction_address(), &failure);\n"
                  "        if (success) {\n"
                  "          box->result.mark_constructed();\n"
                  "        } else {\n"
                  "          ::gti_runtime_detail::"
                  "handoff_managed_worker_failure(\n"
                  "              failure, "
                  "&__gti_failure_artifact_descriptor_v1);\n"
                  "        }\n"
                  "      } else {\n"
                  "        box->result.construct_from([&] { return "
                  "read_only_receiver(box->task.get())(); });\n"
                  "      }\n";
      } else {
        output << "      if constexpr (requires { "
                  "std::move(box->task)(&failure); }) {\n"
                  "        const bool success = "
                  "std::move(box->task)(&failure);\n"
                  "        if (!success) {\n"
                  "          ::gti_runtime_detail::"
                  "handoff_managed_worker_failure(\n"
                  "              failure, "
                  "&__gti_failure_artifact_descriptor_v1);\n"
                  "        }\n"
                  "      } else {\n"
                  "        read_only_receiver(box->task)();\n"
                  "      }\n";
      }
      output << "    });\n"
                "  }\n\n";
      if (valueBearing) {
        output << R"(  template <typename Task, typename Result>
  static void *complete_task(void *raw) noexcept {
    auto *box =
        static_cast<managed_thread_value_box<Task, Result> *>(raw);
    ::gti_runtime_detail::run_managed_task_adapter(
        [&] { box->task.destroy(); });
    return box;
  }

  template <typename Task, typename Result>
  static void abandon_task(void *raw) noexcept {
    auto *box =
        static_cast<managed_thread_value_box<Task, Result> *>(raw);
    ::gti_runtime_detail::run_managed_task_adapter([&] {
      box->result.destroy_if_engaged();
      box->task.destroy_if_engaged();
      delete box;
    });
  }

  template <typename Task, typename Result>
  static void take_result(void *raw, void *destination) noexcept {
    auto *box =
        static_cast<managed_thread_value_box<Task, Result> *>(raw);
    ::gti_runtime_detail::run_managed_task_adapter([&] {
      std::construct_at(static_cast<Result *>(destination),
                        std::move(box->result.get()));
      box->result.destroy();
      delete box;
    });
  }

  template <typename Task, typename Result>
  static void abandon_result(void *raw) noexcept {
    auto *box =
        static_cast<managed_thread_value_box<Task, Result> *>(raw);
    ::gti_runtime_detail::run_managed_task_adapter([&] {
      box->result.destroy();
      delete box;
    });
  }
)";
      } else {
        output << "  template <typename Task>\n"
                  "  static void destroy(void *raw) noexcept {\n"
                  "    ::gti_runtime_detail::run_managed_task_adapter([&] {\n"
                  "      delete static_cast<managed_thread_task_box<Task> "
                  "*>(raw);\n"
                  "    });\n"
                  "  }\n";
      }
      output << "};\n\n";
    }
    if (opened) {
      output << "} // namespace gti_internal::backend\n\n";
    }
  }

  [[nodiscard]] const CppMirNativeCallbackThunk &
  nativeCallbackPayload(const CppMirGeneratedThunk &thunk) const {
    const auto *callback =
        std::get_if<CppMirNativeCallbackThunk>(&thunk.payload);
    if (thunk.identity.kind != CppMirThunkKind::NativeInteropAdapter ||
        callback == nullptr || callback->adapter.id != thunk.identity.owner) {
      throw std::logic_error(
          "planned native callback lost its exact generated-item payload");
    }
    return *callback;
  }

  [[nodiscard]] const MirFunctionInstance &
  nativeCallbackTarget(const CppMirNativeCallbackThunk &callback) const {
    const MirNativeCallbackAdapter &adapter = callback.adapter;
    const MirFunctionInstance *target =
        mir.findFunctionInstance(adapter.target);
    const LoweredFunctionDeclaration *declaration =
        target == nullptr
            ? nullptr
            : loweredProgram.findFunctionDeclaration(target->declaration);
    const LoweredDeclaration *row =
        declaration == nullptr ? nullptr : loweredFunctionRow(declaration->id);
    if (target == nullptr || declaration == nullptr || row == nullptr ||
        target->owner || target->linkage != LanguageLinkage::Gti ||
        target->definitionKind != MirDefinitionKind::Source ||
        adapter.targetMayRaiseDefinedFailure !=
            target->mayRaiseDefinedFailure) {
      throw std::logic_error(
          "verified native callback adapter lost its exact function target");
    }
    return *target;
  }

  void
  emitNativeCallbackAdapterSignature(const CppMirNativeCallbackThunk &callback,
                                     bool parameterNames) {
    const MirNativeCallbackAdapter &adapter = callback.adapter;
    const SemanticType *returnType = adapter.type.nativeFunctionReturnType();
    if (returnType == nullptr) {
      throw std::logic_error(
          "verified native callback adapter has no return type");
    }
    output << "extern \"C\" ";
    emitSemanticType(*returnType);
    output << " __gti_native_callback_" << adapter.id << '(';
    const std::span<const SemanticType> parameters =
        adapter.type.nativeFunctionParameterTypes();
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      emitSemanticType(parameters[index]);
      if (parameterNames) {
        output << " __gti_callback_arg_" << index;
      }
    }
    output << ") noexcept";
  }

  void emitNativeCallbackTargetName(const CppMirNativeCallbackThunk &callback,
                                    bool failureForm = false) {
    const MirFunctionInstance &target = nativeCallbackTarget(callback);
    const LoweredFunctionDeclaration *declaration =
        loweredProgram.findFunctionDeclaration(target.declaration);
    const LoweredDeclaration *row = loweredFunctionRow(target.declaration);
    if (declaration == nullptr || row == nullptr) {
      throw std::logic_error(
          "lowered native callback target lost its declaration row");
    }
    output << "::__gti_program::";
    for (std::size_t index = 0; index < row->namespaceScope.size(); ++index) {
      const std::string &scope = row->namespaceScope[index];
      output << (scope == "std" ? emittedStandardNamespace
                                : std::string_view(scope))
             << "::";
    }
    const std::string name = cppFunctionSpelling(*declaration, row->name);
    output << (failureForm ? cppMirFailureSiblingSpelling(name) : name);
    if (target.typeArguments.size() != 0) {
      throw std::logic_error(
          "native callback target unexpectedly retained generic arguments");
    }
  }

  void emitNativeCallbackArguments(const CppMirNativeCallbackThunk &callback) {
    const MirNativeCallbackAdapter &adapter = callback.adapter;
    const std::size_t count =
        adapter.type.nativeFunctionParameterTypes().size();
    for (std::size_t index = 0; index < count; ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << "__gti_callback_arg_" << index;
    }
  }

  void emitNativeCallbackAdapterDeclarations() {
    bool emitted = false;
    for (const CppMirGeneratedThunk &thunk : programPlan.thunks) {
      if (thunk.identity.kind != CppMirThunkKind::NativeInteropAdapter) {
        continue;
      }
      const CppMirNativeCallbackThunk &callback = nativeCallbackPayload(thunk);
      (void)nativeCallbackTarget(callback);
      writeIndent();
      emitNativeCallbackAdapterSignature(callback, false);
      output << ";\n";
      emitted = true;
    }
    if (emitted) {
      output << '\n';
    }
  }

  void emitNativeCallbackAdapterDefinitions() {
    for (const CppMirGeneratedThunk &thunk : programPlan.thunks) {
      if (thunk.identity.kind != CppMirThunkKind::NativeInteropAdapter) {
        continue;
      }
      const CppMirNativeCallbackThunk &callback = nativeCallbackPayload(thunk);
      const MirNativeCallbackAdapter &adapter = callback.adapter;
      const MirFunctionInstance &target = nativeCallbackTarget(callback);
      const SemanticType &returnType = *adapter.type.nativeFunctionReturnType();
      if (adapter.failurePolicy !=
              MirNativeCallbackFailurePolicy::TerminateInvocation ||
          !adapter.catchesNativeExceptions ||
          (target.mayRaiseDefinedFailure &&
           !loweredFailureBodySupported(target))) {
        throw std::logic_error(
            "verified native callback adapter has no admitted containment "
            "path");
      }

      writeIndent();
      emitNativeCallbackAdapterSignature(callback, true);
      output << " {\n";
      ++indentation;
      writeIndent();
      output << "try {\n";
      ++indentation;
      if (target.mayRaiseDefinedFailure) {
        writeIndent();
        output << "::gti_failure_record_v1 __gti_failure_record{};\n";
        if (returnType != SemanticType::Void) {
          writeIndent();
          emitSemanticType(returnType);
          output << " __gti_result{};\n";
        }
        writeIndent();
        output << "if (!";
        emitNativeCallbackTargetName(callback, true);
        output << '(';
        emitNativeCallbackArguments(callback);
        if (!adapter.type.nativeFunctionParameterTypes().empty()) {
          output << ", ";
        }
        if (returnType != SemanticType::Void) {
          output << "&__gti_result, ";
        }
        output << "&__gti_failure_record)) {\n";
        ++indentation;
        writeIndent();
        output << "::gti_rt_failure_terminate_v1(\n";
        ++indentation;
        writeIndent();
        output << "&__gti_failure_record,\n";
        writeIndent();
        output << "&::gti_internal::backend::"
                  "__gti_failure_artifact_descriptor_v1,\n";
        writeIndent();
        output << "nullptr, nullptr);\n";
        --indentation;
        --indentation;
        writeIndent();
        output << "}\n";
        if (returnType != SemanticType::Void) {
          writeIndent();
          output << "return __gti_result;\n";
        }
      } else {
        writeIndent();
        if (returnType != SemanticType::Void) {
          output << "return ";
        }
        emitNativeCallbackTargetName(callback);
        output << '(';
        emitNativeCallbackArguments(callback);
        output << ");\n";
        if (returnType == SemanticType::Void) {
          writeIndent();
          output << "return;\n";
        }
      }
      --indentation;
      writeIndent();
      output << "} catch (...) {\n";
      ++indentation;
      writeIndent();
      output << "std::terminate();\n";
      --indentation;
      writeIndent();
      output << "}\n";
      --indentation;
      writeIndent();
      output << "}\n\n";
    }
  }

  [[nodiscard]] const CppMirBodyEmitter &generalMirBodyEmitter() const {
    if (!generalEmissionMap) {
      throw std::logic_error(
          "general MIR body emitter requires verified MIR rows");
    }
    if (!generalBodyEmitter) {
      generalBodyEmitter.emplace(mir, *generalEmissionMap);
    }
    return *generalBodyEmitter;
  }

  [[nodiscard]] const CppMirBodyEmissionPlan &
  generalMirBodyPlan(MirBodyAddress address) const {
    const CppMirBodyEmissionPlan *plan = generalEmissionPlan.find(address);
    if (plan == nullptr) {
      throw std::logic_error(
          "general MIR emission plan lost an exact body address");
    }
    return *plan;
  }

  [[nodiscard]] CppMirInitializerScheduleText
  generalMirInitializerSchedule(MirBodyAddress address) const {
    const CppMirBodyEmissionPlan &plan = generalMirBodyPlan(address);
    return {.analysis = plan.analysis(),
            .supported = plan.supportsInitializerSchedule(),
            .fields = plan.initializerFields()};
  }

  void emitGeneralFailureConstructorBodyText(
      const MirConstructorInstance &instance) {
    if (!generalEmissionMap) {
      throw std::logic_error("general MIR body emission requires verified MIR");
    }
    const CppMirBodyEmissionPlan &plan = generalMirBodyPlan(
        {.kind = MirBodyKind::Constructor, .owner = instance.id});
    const CppMirBodyEmissionText emission =
        generalMirBodyEmitter().emitFailureBodyText(
            plan, "scalar-cfg-constructor-failure-v1", indentation);
    if (!emission.emitted()) {
      throw std::logic_error(
          "selected failure-form verified-MIR constructor body is not ready "
          "for general emission (constructor-instance " +
          std::to_string(instance.id) + ")");
    }
    output << emission.text;
  }

  void
  emitGeneralFailureDestructorBodyText(const MirDestructorInstance &instance) {
    if (!generalEmissionMap) {
      throw std::logic_error("general MIR body emission requires verified MIR");
    }
    const CppMirBodyEmissionPlan &plan = generalMirBodyPlan(
        {.kind = MirBodyKind::Destructor, .owner = instance.id});
    const CppMirBodyEmissionText emission =
        generalMirBodyEmitter().emitFailureBodyText(
            plan, "scalar-cfg-destructor-failure-v1", indentation);
    if (!emission.emitted()) {
      throw std::logic_error(
          "selected failure-form verified-MIR destructor body is not ready "
          "for general emission (destructor-instance " +
          std::to_string(instance.id) + ")");
    }
    output << emission.text;
  }

  void emitGeneralMirConstructorInitializerList(
      const MirConstructorInstance &constructor, bool failureForm = false) {
    const MirClassInstance *owner = mir.findClassInstance(constructor.owner);
    const std::optional<std::vector<CppMirStoredReferenceBinding>> bindings =
        cppMirStoredReferenceBindings(constructor);
    const std::optional<std::vector<CppMirBaseParameterInitializerBinding>>
        baseBindings = cppMirBaseParameterInitializerBindings(mir, constructor);
    const std::optional<std::vector<CppMirOwnedParameterFieldBinding>>
        ownedBindings = cppMirOwnedParameterFieldBindings(mir, constructor);
    const std::optional<std::vector<CppMirCopyParameterFieldBinding>>
        copiedBindings = cppMirCopyParameterFieldBindings(mir, constructor);
    const std::vector<CppMirConstructedFieldInitializerBinding>
        constructedBindings =
            cppMirConstructedFieldInitializerBindings(mir, constructor);
    const bool bodyStagedOwnedParameters =
        failureForm && owner != nullptr &&
        cppMirFailureConstructorEmptyStateEligible(mir, owner->id) &&
        bindings && bindings->empty() && baseBindings &&
        baseBindings->empty() && copiedBindings && copiedBindings->empty();
    if (!bindings || !baseBindings || !copiedBindings ||
        (!ownedBindings && !bodyStagedOwnedParameters)) {
      return;
    }
    enum class BindingKind { Base, Field, EmptyField, ConstructedField };
    struct InitializerBinding {
      BindingKind kind = BindingKind::Field;
      std::size_t initializer = 0;
      SymbolId field = 0;
      std::size_t parameter = 0;
      bool moved = false;
      SemanticType fieldType = SemanticType::Unknown;
      SemanticType baseType = SemanticType::Unknown;
      std::vector<std::size_t> baseParameters;
    };
    std::vector<InitializerBinding> initializers;
    initializers.reserve(baseBindings->size() + bindings->size() +
                         copiedBindings->size() +
                         (ownedBindings ? ownedBindings->size() : 0));
    for (const CppMirBaseParameterInitializerBinding &binding : *baseBindings) {
      initializers.push_back({.kind = BindingKind::Base,
                              .initializer = binding.initializer,
                              .baseType = binding.baseType,
                              .baseParameters = binding.parameters});
    }
    for (const CppMirStoredReferenceBinding &binding : *bindings) {
      initializers.push_back({.initializer = binding.initializer,
                              .field = binding.field,
                              .parameter = binding.parameter});
    }
    for (const CppMirCopyParameterFieldBinding &binding : *copiedBindings) {
      initializers.push_back({.initializer = binding.initializer,
                              .field = binding.field,
                              .parameter = binding.parameter});
    }
    for (const CppMirConstructedFieldInitializerBinding &binding :
         constructedBindings) {
      initializers.push_back({.kind = BindingKind::ConstructedField,
                              .initializer = binding.initializer,
                              .field = binding.field,
                              .fieldType = binding.fieldType,
                              .baseParameters = binding.parameters});
    }
    if (ownedBindings) {
      for (const CppMirOwnedParameterFieldBinding &binding : *ownedBindings) {
        if (binding.initializer >= constructor.initializers.size() ||
            std::any_of(initializers.begin(), initializers.end(),
                        [&](const InitializerBinding &candidate) {
                          return candidate.kind == BindingKind::Field &&
                                 candidate.field == binding.field;
                        })) {
          continue;
        }
        initializers.push_back({.initializer = binding.initializer,
                                .field = binding.field,
                                .parameter = binding.parameter,
                                .moved = true});
      }
    }
    if (failureForm && owner != nullptr &&
        cppMirFailureConstructorEmptyStateEligible(mir, owner->id)) {
      for (std::size_t index = 0; index < constructor.initializers.size();
           ++index) {
        const MirConstructorInitializer &initializer =
            constructor.initializers[index];
        if (initializer.kind != ConstructorInitializerTargetKind::Field) {
          continue;
        }
        const auto field = std::find_if(
            owner->declaredFields.begin(), owner->declaredFields.end(),
            [&](const MirClassFieldInfo &candidate) {
              return candidate.symbol == initializer.field &&
                     candidate.type == initializer.targetType;
            });
        if (field == owner->declaredFields.end()) {
          throw std::logic_error(
              "failure constructor lost an explicit field initializer");
        }
        if (std::any_of(initializers.begin(), initializers.end(),
                        [&](const InitializerBinding &candidate) {
                          return candidate.kind != BindingKind::Base &&
                                 candidate.field == field->symbol;
                        })) {
          continue;
        }
        initializers.push_back({.kind = BindingKind::EmptyField,
                                .initializer = index,
                                .field = field->symbol,
                                .fieldType = field->type});
      }
    }
    if (initializers.empty()) {
      return;
    }
    std::sort(
        initializers.begin(), initializers.end(),
        [](const InitializerBinding &left, const InitializerBinding &right) {
          return left.initializer < right.initializer;
        });
    output << " : ";
    for (std::size_t index = 0; index < initializers.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      const InitializerBinding &initializer = initializers[index];
      if (initializer.kind == BindingKind::Base) {
        emitSemanticType(initializer.baseType);
        output << '(';
        for (std::size_t argument = 0;
             argument < initializer.baseParameters.size(); ++argument) {
          if (argument != 0) {
            output << ", ";
          }
          output << "__gti_mir_arg_" << initializer.baseParameters[argument];
        }
        output << ')';
        continue;
      }
      const LoweredSymbol *loweredSymbol =
          loweredProgram.findSymbol(initializer.field);
      const std::string_view fieldName =
          loweredSymbol == nullptr ? std::string_view{}
                                   : std::string_view(loweredSymbol->name);
      if (fieldName.empty()) {
        throw std::logic_error(
            "verified MIR field initializer lost its field name");
      }
      if (initializer.kind == BindingKind::ConstructedField) {
        // C++17 guaranteed elision constructs the field directly from this
        // prvalue: one construction, no intermediate object, and no invented
        // copy or move. The proof already fixed declaration order.
        output << fieldName << '(';
        emitSemanticType(initializer.fieldType);
        output << '(';
        for (std::size_t argument = 0;
             argument < initializer.baseParameters.size(); ++argument) {
          if (argument != 0) {
            output << ", ";
          }
          output << "__gti_mir_arg_" << initializer.baseParameters[argument];
        }
        output << "))";
        continue;
      }
      if (initializer.kind == BindingKind::EmptyField) {
        output << fieldName;
        if (initializer.fieldType.kind == SemanticType::Array) {
          output << "{}";
        } else {
          output << "(::gti_internal::backend::mir_empty_state_v1<"
                    "std::remove_cv_t<decltype("
                 << fieldName << ")>>())";
        }
        continue;
      }
      output << fieldName << '(';
      if (initializer.moved) {
        output << "std::move(";
      }
      output << "__gti_mir_arg_" << initializer.parameter;
      if (initializer.moved) {
        output << ')';
      }
      output << ')';
    }
  }

  void
  emitGeneralMirConstructorBodyText(const MirConstructorInstance &constructor) {
    if (!generalEmissionMap) {
      throw std::logic_error("general MIR body emission requires verified MIR");
    }
    const CppMirBodyEmissionPlan &plan = generalMirBodyPlan(
        {.kind = MirBodyKind::Constructor, .owner = constructor.id});
    const CppMirBodyEmissionText emission =
        generalMirBodyEmitter().emitBodyText(plan, "scalar-cfg-v1",
                                             indentation);
    if (!emission.emitted()) {
      throw std::logic_error(
          "selected verified-MIR constructor body is not ready for general "
          "emission (constructor-instance " +
          std::to_string(constructor.id) + ")");
    }
    output << emission.text;
  }

  void emitGeneralMirDestructorBodyText(const MirDestructorInstance &destructor,
                                        std::string_view familyLabel) {
    if (!generalEmissionMap) {
      throw std::logic_error("general MIR body emission requires verified MIR");
    }
    const CppMirBodyEmissionPlan &plan = generalMirBodyPlan(
        {.kind = MirBodyKind::Destructor, .owner = destructor.id});
    const CppMirBodyEmissionText emission =
        generalMirBodyEmitter().emitBodyText(plan, familyLabel, indentation);
    if (!emission.emitted()) {
      std::string message =
          "selected verified-MIR body is not ready for general emission";
      for (const CppMirBodyEmissionIssue &issue : emission.analysis.issues) {
        message += "; ";
        message += issue.detail;
        message += " (destructor-instance " + std::to_string(destructor.id) +
                   " block " + std::to_string(issue.block) + " instruction " +
                   std::to_string(issue.instruction) + ")";
      }
      throw std::logic_error(message);
    }
    writeIndent();
    output << emission.text;
  }
  void emitHostedStartupMarker() {
    if (!mir.hostedStartupPlan()) {
      return;
    }
    if (!generalEmissionMap ||
        (!cppMirHostedStartupNoArgumentsSchedule(mir) &&
         !cppMirHostedStartupFailureFreeSchedule(mir) &&
         !cppMirHostedStartupOwnedArgumentsSchedule(mir))) {
      throw std::logic_error(
          "production C++ emission has no verified-MIR hosted-startup "
          "schedule");
    }
    // The adapter below is the verified hosted-startup schedule's complete
    // emission: for the no-argument form, call-entry with propagated
    // failure, operation-failure routing, terminal containment, and
    // return; for the owned-arguments form, the count
    // validation/conversion, vector construction, per-argument
    // view/string/append loop with its failure-cleanup envelope, and the
    // entry call the schedule authority verified.
    output << "// GTI verified-MIR body: hosted-entry-v1 "
              "hosted-startup-instance "
           << mir.hostedStartupPlan()->entry << "\n";
  }

  void emitHostedFailureTermination() {
    writeIndent();
    output << "::gti_rt_failure_terminate_v1(\n";
    ++indentation;
    writeIndent();
    output << "&__gti_failure_record,\n";
    writeIndent();
    output << "&::gti_internal::backend::"
              "__gti_failure_artifact_descriptor_v1,\n";
    writeIndent();
    output << "nullptr, nullptr);\n";
    --indentation;
  }

  void emitRuntimeModuleInitializer() {
    if (!moduleRuntimeEmission) {
      return;
    }
    const MirBodyAddress address{.kind = MirBodyKind::Module, .owner = 0};
    const CppMirBodyEmissionPlan &plan = generalMirBodyPlan(address);
    const CppMirBodyEmissionText emission =
        moduleRuntimeMayRaise
            ? generalMirBodyEmitter().emitFailureBodyText(
                  plan, "program-initialization-v1", indentation)
            : generalMirBodyEmitter().emitBodyText(
                  plan, "program-initialization-v1", indentation);
    if (!emission.emitted()) {
      throw std::logic_error(
          "selected verified-MIR runtime module body is not emit-ready");
    }
    writeIndent();
    output << "static " << (moduleRuntimeMayRaise ? "bool" : "void")
           << " __gti_program_initialize(";
    if (moduleRuntimeMayRaise) {
      output << "::gti_failure_record_v1 *__gti_mir_failure_record";
    }
    output << ") " << emission.text << '\n';

    // A hosted program invokes Module/0 at the exact HostedStartup operation.
    // Translation units without an entry still need their dynamic program
    // storage initialized, so a private guard performs the same verified body
    // during native startup.
    if (mir.hostedStartupPlan()) {
      return;
    }
    writeIndent();
    output << "static const bool __gti_program_initialize_once = [] {\n";
    ++indentation;
    if (moduleRuntimeMayRaise) {
      writeIndent();
      output << "::gti_failure_record_v1 __gti_failure_record{};\n";
      writeIndent();
      output << "try {\n";
      ++indentation;
      writeIndent();
      output << "if (!__gti_program_initialize(&__gti_failure_record)) {\n";
      ++indentation;
      emitHostedFailureTermination();
      --indentation;
      writeIndent();
      output << "}\n";
      --indentation;
      writeIndent();
      output << "} catch (...) {\n";
      ++indentation;
      writeIndent();
      output << "std::_Exit(GTI_FAILURE_EXIT_STATUS);\n";
      --indentation;
      writeIndent();
      output << "}\n";
    } else {
      writeIndent();
      output << "__gti_program_initialize();\n";
    }
    writeIndent();
    output << "return true;\n";
    --indentation;
    writeIndent();
    output << "}();\n\n";
  }

  void emitRuntimeModuleInitializationCall() {
    if (!moduleRuntimeEmission) {
      return;
    }
    writeIndent();
    if (moduleRuntimeMayRaise) {
      output << "if (!::__gti_program::__gti_program_initialize("
                "&__gti_failure_record)) {\n";
      ++indentation;
      emitHostedFailureTermination();
      --indentation;
      writeIndent();
      output << "}\n";
      return;
    }
    output << "::__gti_program::__gti_program_initialize();\n";
  }

  void
  emitLoweredQualifiedFunctionName(const LoweredDeclaration &row,
                                   const LoweredFunctionDeclaration &function,
                                   bool failure = false) {
    output << "::__gti_program::";
    for (std::size_t index = 0; index < row.namespaceScope.size(); ++index) {
      output << loweredNamespaceSpelling(row.namespaceScope[index], index == 0)
             << "::";
    }
    const std::string name = cppFunctionSpelling(function, row.name);
    output << (failure ? cppMirFailureSiblingSpelling(name) : name);
  }

  void emitLoweredProgramEntryAdapter() {
    emitHostedStartupMarker();
    const std::optional<MirHostedStartupPlan> &hosted = mir.hostedStartupPlan();
    if (!hosted || hosted->kind == ProgramEntryKind::None) {
      return;
    }
    const MirFunctionInstance *entry = mir.findFunctionInstance(hosted->entry);
    const LoweredFunctionDeclaration *entryDeclaration =
        entry == nullptr
            ? nullptr
            : loweredProgram.findFunctionDeclaration(entry->declaration);
    const LoweredDeclaration *entryRow =
        entryDeclaration == nullptr ? nullptr
                                    : loweredFunctionRow(entryDeclaration->id);
    if (entry == nullptr || entryDeclaration == nullptr ||
        entryRow == nullptr || entry->entryKind != hosted->kind ||
        entryDeclaration->entryKind != hosted->kind) {
      throw std::logic_error(
          "lowered hosted startup lost its exact entry declaration");
    }
    const bool failure = entry->mayRaiseDefinedFailure;
    const MirBodyAddress entryAddress{.kind = MirBodyKind::Function,
                                      .owner = entry->id};
    const CppMirBodyEmissionPlan &entryPlan = generalMirBodyPlan(entryAddress);
    if (failure ? !entryPlan.supportsFailureText()
                : !entryPlan.supportsPlainText()) {
      throw std::logic_error("lowered hosted entry has no admitted MIR body");
    }

    if (hosted->kind == ProgramEntryKind::NoArguments) {
      if (failure) {
        if (!cppMirHostedStartupNoArgumentsSchedule(mir)) {
          throw std::logic_error(
              "lowered no-argument hosted entry has no verified schedule");
        }
        output << "\nint main() {\n";
        ++indentation;
        writeIndent();
        output << "::gti_failure_record_v1 __gti_failure_record{};\n";
        writeIndent();
        output << "std::int32_t __gti_result{};\n";
        writeIndent();
        output << "try {\n";
        ++indentation;
        emitRuntimeModuleInitializationCall();
        writeIndent();
        output << "if (";
        emitLoweredQualifiedFunctionName(*entryRow, *entryDeclaration, true);
        output << "(&__gti_result, &__gti_failure_record)) {\n";
        ++indentation;
        writeIndent();
        output << "return __gti_result;\n";
        --indentation;
        writeIndent();
        output << "}\n";
        --indentation;
        writeIndent();
        output << "} catch (...) {\n";
        ++indentation;
        writeIndent();
        output << "std::_Exit(GTI_FAILURE_EXIT_STATUS);\n";
        --indentation;
        writeIndent();
        output << "}\n";
        emitHostedFailureTermination();
        --indentation;
        output << "}\n";
        return;
      }

      if (!cppMirHostedStartupFailureFreeSchedule(mir)) {
        throw std::logic_error(
            "lowered no-argument entry has no failure-free startup schedule");
      }
      if (!moduleRuntimeEmission) {
        output << "\nint main() { return ";
        emitLoweredQualifiedFunctionName(*entryRow, *entryDeclaration);
        output << "(); }\n";
        return;
      }
      output << "\nint main() {\n";
      ++indentation;
      if (moduleRuntimeMayRaise) {
        writeIndent();
        output << "::gti_failure_record_v1 __gti_failure_record{};\n";
        writeIndent();
        output << "try {\n";
        ++indentation;
        emitRuntimeModuleInitializationCall();
        --indentation;
        writeIndent();
        output << "} catch (...) {\n";
        ++indentation;
        writeIndent();
        output << "std::_Exit(GTI_FAILURE_EXIT_STATUS);\n";
        --indentation;
        writeIndent();
        output << "}\n";
      } else {
        emitRuntimeModuleInitializationCall();
      }
      writeIndent();
      output << "return ";
      emitLoweredQualifiedFunctionName(*entryRow, *entryDeclaration);
      output << "();\n";
      --indentation;
      output << "}\n";
      return;
    }

    if (hosted->kind != ProgramEntryKind::OwnedArguments ||
        entry->parameterTypes.size() != 2 ||
        entry->parameterTypes[1].kind != SemanticType::Class ||
        entry->parameterTypes[1].arguments.size() != 1 ||
        !cppMirHostedStartupOwnedArgumentsSchedule(mir)) {
      throw std::logic_error(
          "lowered owned-arguments entry has no verified startup schedule");
    }
    const MirFunctionInstance *append =
        mir.findFunctionInstance(hosted->appendFunction);
    const LoweredFunctionDeclaration *appendDeclaration =
        append == nullptr
            ? nullptr
            : loweredProgram.findFunctionDeclaration(append->declaration);
    const LoweredDeclaration *appendRow =
        appendDeclaration == nullptr
            ? nullptr
            : loweredFunctionRow(appendDeclaration->id);
    const MirConstructorInstance *vectorConstructor =
        mir.findConstructorInstance(hosted->vectorConstructor);
    const MirConstructorInstance *stringConstructor =
        mir.findConstructorInstance(hosted->stringConstructor);
    if (append == nullptr || appendDeclaration == nullptr ||
        appendRow == nullptr || vectorConstructor == nullptr ||
        stringConstructor == nullptr) {
      throw std::logic_error(
          "lowered owned-arguments startup lost an exact target");
    }

    output << "\nint main(int __gti_native_argc, "
              "char **__gti_native_argv) {\n";
    ++indentation;
    if (failure || moduleRuntimeMayRaise) {
      writeIndent();
      output << "::gti_failure_record_v1 __gti_failure_record{};\n";
    }
    if (failure) {
      writeIndent();
      output << "std::int32_t __gti_result{};\n";
      writeIndent();
      output << "try {\n";
      ++indentation;
    }
    writeIndent();
    output << "if (__gti_native_argc < 0) {\n";
    ++indentation;
    writeIndent();
    output << "::gti_internal::backend::integer_domain_error("
              "\"negative program argument count\");\n";
    --indentation;
    writeIndent();
    output << "}\n";
    writeIndent();
    output << "const std::int32_t __gti_stabilized_argc = "
              "::gti_internal::backend::numeric_cast<std::int32_t>("
              "__gti_native_argc);\n";
    emitRuntimeModuleInitializationCall();

    if (failure) {
      writeIndent();
      output << "bool __gti_arguments_success = false;\n";
      writeIndent();
      emitSemanticType(entry->parameterTypes[1]);
      output << " __gti_arguments("
             << cppMirFailureConstructorTagSpelling(vectorConstructor->id)
             << "{}, &__gti_arguments_success, &__gti_failure_record);\n";
      writeIndent();
      output << "if (!__gti_arguments_success) {\n";
      ++indentation;
      emitHostedFailureTermination();
      --indentation;
      writeIndent();
      output << "}\n";
    } else {
      writeIndent();
      emitSemanticType(entry->parameterTypes[1]);
      output << " __gti_arguments{};\n";
    }
    writeIndent();
    output << "for (int __gti_index = 0; __gti_index < "
              "__gti_native_argc; ++__gti_index) {\n";
    ++indentation;
    if (failure) {
      writeIndent();
      output << "bool __gti_argument_success = false;\n";
      writeIndent();
      emitSemanticType(entry->parameterTypes[1].arguments.front());
      output << " __gti_argument("
             << cppMirFailureConstructorTagSpelling(stringConstructor->id)
             << "{}, std::string_view(__gti_native_argv[__gti_index]), "
                "&__gti_argument_success, &__gti_failure_record);\n";
      writeIndent();
      output << "if (!__gti_argument_success) {\n";
      ++indentation;
      emitHostedFailureTermination();
      --indentation;
      writeIndent();
      output << "}\n";
      writeIndent();
      output << "if (!__gti_arguments."
             << cppMirFailureSiblingSpelling(
                    cppFunctionSpelling(*appendDeclaration, appendRow->name))
             << "(std::move(__gti_argument), &__gti_failure_record)) {\n";
      ++indentation;
      emitHostedFailureTermination();
      --indentation;
      writeIndent();
      output << "}\n";
    } else {
      writeIndent();
      output << "__gti_arguments."
             << cppFunctionSpelling(*appendDeclaration, appendRow->name) << '(';
      emitSemanticType(entry->parameterTypes[1].arguments.front());
      output << "(std::string_view(__gti_native_argv[__gti_index])));\n";
    }
    --indentation;
    writeIndent();
    output << "}\n";
    writeIndent();
    if (failure) {
      output << "if (";
      emitLoweredQualifiedFunctionName(*entryRow, *entryDeclaration, true);
      output << "(__gti_stabilized_argc, std::move(__gti_arguments), "
                "&__gti_result, &__gti_failure_record)) {\n";
      ++indentation;
      writeIndent();
      output << "return __gti_result;\n";
      --indentation;
      writeIndent();
      output << "}\n";
      emitHostedFailureTermination();
      --indentation;
      writeIndent();
      output << "} catch (...) {\n";
      ++indentation;
      writeIndent();
      output << "std::_Exit(GTI_FAILURE_EXIT_STATUS);\n";
      --indentation;
      writeIndent();
      output << "}\n";
    } else {
      output << "return ";
      emitLoweredQualifiedFunctionName(*entryRow, *entryDeclaration);
      output << "(__gti_stabilized_argc, std::move(__gti_arguments));\n";
    }
    --indentation;
    output << "}\n";
  }

  void emitSemanticType(const SemanticType &type) {
    output << cppSemanticTypeSpelling(loweredProgram, standard, type);
  }

  [[nodiscard]] static bool isMoveOnlyOwner(const SemanticTypeTraits &traits) {
    return !traits.copyable && traits.movable;
  }
  [[nodiscard]] static bool containsTypeParameter(const SemanticType &type) {
    if (type.kind == SemanticType::TypeParameter ||
        type.kind == SemanticType::TypePack) {
      return true;
    }
    return std::any_of(type.arguments.begin(), type.arguments.end(),
                       [](const SemanticType &argument) {
                         return containsTypeParameter(argument);
                       });
  }
  void emitSwitchInteger(const EnumConstant &value, const SemanticType &type) {
    if (!value.negative) {
      output << value.magnitude;
      if (type == SemanticType::UInt64 &&
          value.magnitude > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max())) {
        output << "ULL";
      }
      return;
    }

    std::uint64_t signedLimit = 0;
    switch (type.kind) {
    case SemanticType::Int8:
      signedLimit = std::uint64_t{1} << 7U;
      break;
    case SemanticType::Int16:
      signedLimit = std::uint64_t{1} << 15U;
      break;
    case SemanticType::Int32:
      signedLimit = std::uint64_t{1} << 31U;
      break;
    case SemanticType::Int64:
      signedLimit = std::uint64_t{1} << 63U;
      break;
    default:
      break;
    }
    if (signedLimit != 0 && value.magnitude == signedLimit) {
      output << "(-" << signedLimit - 1 << "LL - 1LL)";
      return;
    }
    output << '-' << value.magnitude;
  }

  void emitConstant(const ConstantValue &constant,
                    const SemanticType *type = nullptr) {
    if (const auto *integer = std::get_if<ConstantInteger>(&constant)) {
      if (type != nullptr && type->kind == SemanticType::Enum) {
        output << "static_cast<";
        emitSemanticType(*type);
        output << ">(";
        emitSwitchInteger(EnumConstant{.negative = integer->negative,
                                       .magnitude = integer->magnitude},
                          semanticIntegerType(integer->domain));
        output << ')';
        return;
      }
      const SemanticType type = semanticIntegerType(integer->domain);
      output << "static_cast<";
      emitSemanticType(type);
      output << ">(";
      emitSwitchInteger(EnumConstant{.negative = integer->negative,
                                     .magnitude = integer->magnitude},
                        type);
      output << ')';
    } else if (const auto *value = std::get_if<BinaryFloat>(&constant)) {
      emitFloat(*value);
    } else if (const auto *value = std::get_if<CharacterLiteral>(&constant)) {
      output << "std::uint8_t{" << static_cast<unsigned int>(value->value)
             << '}';
    } else if (const auto *value = std::get_if<std::string>(&constant)) {
      output << cppMirStringViewLiteralSpelling(*value);
    } else if (const auto *value = std::get_if<bool>(&constant)) {
      output << (*value ? "true" : "false");
    } else if (const auto *enumerator = std::get_if<ConstantEnum>(&constant)) {
      if (type == nullptr || type->kind != SemanticType::Enum ||
          type->enumId == 0 || type->enumId != enumerator->enumId) {
        throw std::logic_error(
            "enum constant requires its exact semantic enum type");
      }
      output << "static_cast<";
      emitSemanticType(*type);
      output << ">(";
      emitSwitchInteger(EnumConstant{.negative = enumerator->value.negative,
                                     .magnitude = enumerator->value.magnitude},
                        semanticIntegerType(enumerator->value.domain));
      output << ')';
    } else if (const auto *aggregate =
                   std::get_if<ConstantAggregateValue>(&constant)) {
      if (type == nullptr || aggregate->value == nullptr) {
        throw std::logic_error(
            "aggregate constant requires its exact semantic type");
      }
      if (aggregate->value->exactType == nullptr ||
          *aggregate->value->exactType != *type) {
        throw std::logic_error(
            "aggregate constant differs from its exact semantic type");
      }
      if (aggregate->value->kind == ConstantAggregateKind::Array) {
        if (type->kind != SemanticType::Array || type->arguments.size() != 1 ||
            type->arrayLengthParameterId != 0 ||
            type->arrayLength != aggregate->value->elements.size()) {
          throw std::logic_error(
              "array constant differs from its exact semantic type");
        }
        emitSemanticType(*type);
        output << '{';
        for (std::size_t index = 0; index < aggregate->value->elements.size();
             ++index) {
          if (index != 0) {
            output << ", ";
          }
          emitConstant(aggregate->value->elements[index],
                       &type->arguments.front());
        }
        output << '}';
        return;
      }
      if (type->kind != SemanticType::Class || type->classId == 0 ||
          type->classId != aggregate->value->identity) {
        throw std::logic_error(
            "object constant differs from its exact semantic class type");
      }
      const LoweredDeclaration *classRow = loweredClassRow(type->classId);
      const auto *declaration =
          classRow == nullptr
              ? nullptr
              : std::get_if<LoweredClassDeclaration>(&classRow->payload);
      if (declaration == nullptr) {
        throw std::logic_error(
            "object constant lost its lowered class declaration");
      }
      const std::vector<const LoweredDeclaration *> fields =
          loweredConstexprAggregateFields(*classRow);
      if (fields.size() != aggregate->value->fields.size()) {
        throw std::logic_error(
            "object constant differs from its lowered field inventory");
      }
      const GenericSubstitution substitution =
          loweredConstantClassSubstitution(*declaration, *type);
      emitSemanticType(*type);
      output << "::__gti_constexpr_value_v1(";
      for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto &storage =
            std::get<LoweredStorageDeclaration>(fields[index]->payload);
        const ConstantAggregateField &field = aggregate->value->fields[index];
        if (field.symbol != storage.symbol) {
          throw std::logic_error(
              "object constant field order differs from its lowered class");
        }
        if (index != 0) {
          output << ", ";
        }
        const SemanticType fieldType =
            substituteLoweredConstantType(storage.type, substitution);
        emitConstant(field.value, &fieldType);
      }
      output << ')';
    } else if (const auto *checked =
                   std::get_if<ConstantCheckedIntegerResult>(&constant)) {
      if (type == nullptr || type->kind != SemanticType::Expected ||
          type->arguments.size() != 2 ||
          constantIntegerDomain(type->arguments[0]) != checked->domain ||
          type->arguments[1].kind != SemanticType::Enum) {
        throw std::logic_error(
            "checked-integer constant requires its exact expected type");
      }
      emitSemanticType(*type);
      output << '{';
      if (checked->value) {
        emitConstant(ConstantValue{*checked->value}, &type->arguments[0]);
      } else {
        output << (standard == CppStandard::Cpp23
                       ? "std::unexpected("
                       : "::nonstd::make_unexpected(");
        output << "static_cast<";
        emitSemanticType(type->arguments[1]);
        output << ">(0))";
      }
      output << '}';
    } else {
      output << "nullptr";
    }
  }

  void writeIndent() {
    for (std::size_t index = 0; index < indentation; ++index) {
      output << "  ";
    }
  }

  void emitFloat(BinaryFloat value) {
    // Shared byte-exact authority with the verified-MIR text step.
    output << cppMirBinaryFloatLiteralSpelling(value);
  }

  [[nodiscard]] static std::string_view operatorSpelling(const Token &oper) {
    if (oper.kind == TokenKind::AND) {
      return "&&";
    }
    if (oper.kind == TokenKind::OR) {
      return "||";
    }
    return oper.lexeme;
  }

  [[nodiscard]] static std::string_view
  compoundAssignmentHelper(TokenKind operation) {
    switch (operation) {
    case TokenKind::PLUS_EQUAL:
      return "add_assign";
    case TokenKind::MINUS_EQUAL:
      return "subtract_assign";
    case TokenKind::STAR_EQUAL:
      return "multiply_assign";
    case TokenKind::SLASH_EQUAL:
      return "divide_assign";
    case TokenKind::PERCENT_EQUAL:
      return "remainder_assign";
    case TokenKind::AMPERSAND_EQUAL:
      return "bitwise_and_assign";
    case TokenKind::PIPE_EQUAL:
      return "bitwise_or_assign";
    case TokenKind::CARET_EQUAL:
      return "bitwise_xor_assign";
    case TokenKind::SHIFT_LEFT_EQUAL:
      return "shift_left_assign";
    case TokenKind::SHIFT_RIGHT_EQUAL:
      return "shift_right_assign";
    default:
      return "invalid_compound_assignment";
    }
  }

  [[nodiscard]] static std::string quote(std::string_view value) {
    std::string result = "\"";
    for (char character : value) {
      switch (character) {
      case '\\':
        result += "\\\\";
        break;
      case '"':
        result += "\\\"";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      case '\0':
        result += "\\000";
        break;
      default: {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32 || byte >= 127) {
          result += '\\';
          result += static_cast<char>('0' + ((byte >> 6U) & 0x07U));
          result += static_cast<char>('0' + ((byte >> 3U) & 0x07U));
          result += static_cast<char>('0' + (byte & 0x07U));
        } else {
          result += character;
        }
        break;
      }
      }
    }
    result += '"';
    return result;
  }

  static constexpr std::string_view emittedStandardNamespace =
      cppEmittedStandardNamespace;

  std::ostringstream output;
  CppStandard standard;
  const MirProgram &mir;
  const LoweredProgram &loweredProgram;
  CppMirProgramPlan programPlan;
  CppMirProgramEmissionPlan generalEmissionPlan;
  std::unordered_map<SymbolId, std::string> moduleGeneralInitializers;
  bool moduleGeneralSupported = false;
  std::unordered_set<SymbolId> moduleRuntimeInitializedSymbols;
  bool moduleRuntimeEmission = false;
  bool moduleRuntimeMayRaise = false;
  std::size_t indentation = 0;
  std::optional<CppMirBodyEmissionMap> generalEmissionMap;
  std::vector<std::vector<const LoweredDeclaration *>>
      loweredDeclarationChildren;
  std::unordered_set<LoweredDeclarationId> loweredForwardedLinkage;
  std::unordered_set<SymbolId> loweredNamespaceStaticSymbols;
  std::vector<const LoweredDeclaration *> loweredDeferredStaticFields;
  std::vector<const LoweredDeclaration *> loweredDeferredMembers;
  std::vector<const LoweredDeclaration *> loweredDeferredLifecycleClasses;
  std::unordered_set<LoweredDeclarationId> loweredEmittedTypes;
  std::unordered_set<ClassId> loweredConstexprAggregateClasses;
  mutable std::optional<CppMirBodyEmitter> generalBodyEmitter;
};

CppProgramEmissionPlan
buildCppProgramEmissionPlan(const LoweredProgram &program,
                            CppStandard standard) {
  return CppProgramEmissionPlanner(program, standard).build();
}

} // namespace lang
