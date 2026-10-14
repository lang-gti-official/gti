#include "cpp_program_emission_plan.h"
#include "gti/support.h"

#include "cpp_failure_support_writer.h"
#include "cpp_mir_body_emitter.h"
#include "cpp_mir_body_text_writer.h"
#include "cpp_mir_program_plan.h"
#include "cpp_mir_representation_snapshot.h"
#include "cpp_representation.h"
#include "cpp_support_preamble.h"
#include "gti/backend.h"
#include "gti/lowered_program.h"
#include "gti/mir.h"
#include "gti/mir_effects.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace lang {

class CppProgramEmissionPlanner final {
public:
  explicit CppProgramEmissionPlanner(const VerifiedLoweredProgram &verified,
                                     CppStandard standard, bool debugInfo)
      : standard(standard), mir(verified.program().mir()),
        loweredProgram(verified.program()), debugInfo(debugInfo) {
    CppMirRepresentationSnapshotBuild snapshot = [&] {
      const PhaseTimeScope timeScope("backend-representation-snapshot");
      return buildCppMirRepresentationSnapshot(verified, standard, debugInfo);
    }();
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
    {
      const PhaseTimeScope timeScope("backend-program-plan");
      programPlan = planCppMirProgram(mir, std::move(*snapshot.snapshot));
    }
    if (selectCppMirBackendProgramRoute(programPlan) !=
            CppMirBackendProgramRoute::VerifiedMir ||
        !programPlan.complete()) {
      throw std::logic_error(
          "C++ emitter requires a complete generated-item program plan");
    }
    const PhaseTimeScope timeScope("backend-emission-map");
    generalEmissionMap.emplace(
        buildCppMirBodyEmissionMapRows(loweredProgram, standard));
  }

  CppProgramEmissionPlan build() {
    output.str("");
    output.clear();
    indentation = 0;
    generalBodyEmitter.reset();
    constructorTemplatePlans.clear();
    destructorTemplatePlans.clear();
    callableTemplatePlans.clear();
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
    loweredDeferredMemberIds.clear();
    loweredDeferredCallableAdapters.clear();
    loweredDeferredLifecycleClasses.clear();
    loweredConcreteLifecycleInstances.clear();
    loweredNativePrimaryClasses.clear();
    loweredEmittedTypes.clear();
    loweredConstexprAggregateClasses.clear();

    const CppSupportPreambleFeatures supportFeatures{
        .standard = standard,
        .expected = loweredProgramContainsTypeKind(SemanticType::Expected),
        .storage = true,
        .managedThreads = loweredProgramContainsManagedThreads(),
        .cThreads = mir.requiresCThreadRuntime(),
        .sharedOwnerDeclared = loweredProgramDeclaresCapability(
            CompilerCapabilityTypeKind::SharedOwner),
        .atomicStorageDeclared = loweredProgramDeclaresCapability(
            CompilerCapabilityTypeKind::AtomicStorage),
        .mutexStorageDeclared = loweredProgramDeclaresCapability(
            CompilerCapabilityTypeKind::MutexStorage),
        .mutexLockTokenDeclared = loweredProgramDeclaresCapability(
            CompilerCapabilityTypeKind::MutexLockToken),
        .polymorphicStorage = loweredProgramDeclaresCapability(
            CompilerCapabilityTypeKind::PolymorphicStorage),
        .atomics = loweredProgramContainsAtomics(),
        .mutexes = loweredProgramContainsMutexes(),
        .simd = loweredProgramContainsSimd(),
        .definedFailure = true,
        .runtimeBinding = loweredProgramContainsRuntimeBinding(),
        .cAbiTextView = loweredProgramContainsCAbiTextView(),
    };
    CppProgramEmissionPlan plan;
    capture(plan, CppProgramTextRole::SupportPreamble, [&] {
      const PhaseTimeScope timeScope("backend-plan-support");
      writeCppSupportPreamble(output, supportFeatures);
      // Native declarations must implement the selected GTI target, not just
      // have equal-width arguments. Refuse a mismatched compiler data model.
      const TargetInfo &target = loweredProgram.target();
      const auto nativeLong = target.nativeCScalar(NativeCScalarKind::Long);
      const auto nativeChar = target.nativeCScalar(NativeCScalarKind::Char);
      const auto fixed64 = target.fixedCScalarIdentity(TargetScalarKind::Int64);
      if (!nativeLong || !nativeChar || !fixed64) {
        throw std::logic_error("verified target has no native C scalar ABI");
      }
      output
          << "static_assert(sizeof(long) == " << nativeLong->layout.sizeBytes
          << " && alignof(long) == " << nativeLong->layout.abiAlignmentBytes
          << ", \"native compiler long ABI differs from the GTI target\");\n";
      output << "static_assert(std::is_signed_v<char> == "
             << (nativeChar->valueKind == TargetScalarKind::Int8 ? "true"
                                                                 : "false")
             << ", \"native compiler char signedness differs from the GTI "
                "target\");\n";
      output << "static_assert(std::is_same_v<std::int64_t, "
             << nativeCScalarDeclarationName(*fixed64)
             << ">, \"native compiler int64_t identity differs from the GTI "
                "target\");\n";
    });
    capture(plan, CppProgramTextRole::FailureAndLifecycleSupport, [&] {
      writeCppFailureArtifactDescriptor(output, mir.failureMetadata());
    });
    capture(plan, CppProgramTextRole::NativeAndThreadAdapter,
            [&] { emitThreadTaskAdapterDeclarations(); });
    capture(plan, CppProgramTextRole::FailureAndLifecycleSupport,
            [&] { writeCppMirFailureHelpers(output); });
    capture(plan, CppProgramTextRole::NativeAndThreadAdapter,
            [&] { writeCppCAbiTextViewAdapter(output, supportFeatures); });
    capture(plan, CppProgramTextRole::DeclarationTypeMemberLifecycle, [&] {
      const PhaseTimeScope timeScope("backend-plan-declarations");
      emitLoweredUserMacroIsolation();
    });
    {
      const PhaseTimeScope timeScope("backend-plan-program");
      buildLoweredProgramTail(plan);
    }
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
    nativeClassRows.clear();
    lexicalClassChildren.clear();
    for (const auto &row : loweredProgram.declarations()) {
      const auto *type = std::get_if<LoweredClassDeclaration>(&row.payload);
      if (type == nullptr || row.ownerClass == 0)
        continue;
      lexicalClassChildren[row.ownerClass].push_back(&row);
      LoweredDeclaration native = row;
      native.name = cppClassDeclarationNameSpelling(loweredProgram, type->id);
      native.ownerClass = 0;
      const LoweredDeclaration *parent =
          loweredProgram.findDeclaration(row.parent);
      while (parent != nullptr &&
             std::holds_alternative<LoweredClassDeclaration>(parent->payload))
        parent = loweredProgram.findDeclaration(parent->parent);
      native.parent = parent == nullptr ? 0 : parent->id;
      nativeClassRows.emplace(type->id, std::move(native));
    }
    loweredDeclarationChildren.assign(loweredProgram.declarations().size() + 1,
                                      {});
    for (const LoweredDeclaration &declaration :
         loweredProgram.declarations()) {
      if (declaration.parent >= loweredDeclarationChildren.size()) {
        throw std::logic_error(
            "lowered declaration parent is outside the declaration tree");
      }
      const auto *type =
          std::get_if<LoweredClassDeclaration>(&declaration.payload);
      const auto found = type == nullptr ? nativeClassRows.end()
                                         : nativeClassRows.find(type->id);
      const auto *native =
          found == nativeClassRows.end() ? &declaration : &found->second;
      loweredDeclarationChildren[native->parent].push_back(native);
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

  // A retained capability class declaration is the structural fact that lets
  // an emitted declaration spell the backend representation of that class.
  [[nodiscard]] bool
  loweredProgramDeclaresCapability(CompilerCapabilityTypeKind kind) const {
    return std::any_of(
        loweredProgram.declarations().begin(),
        loweredProgram.declarations().end(),
        [kind](const LoweredDeclaration &row) {
          const auto *type = std::get_if<LoweredClassDeclaration>(&row.payload);
          return type != nullptr && type->compilerCapability == kind;
        });
  }

  [[nodiscard]] bool loweredProgramContainsManagedThreads() const {
    // The handle representation is not a template, so even an uninstantiated
    // generic definition names it non-dependently and needs it complete; a
    // retained declaration therefore selects the complete support.
    if (loweredProgramDeclaresCapability(
            CompilerCapabilityTypeKind::ThreadHandle)) {
      return true;
    }
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
          case IntrinsicKind::ThreadScope:
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

  [[nodiscard]] bool loweredProgramContainsAtomics() const {
    // Generic library declarations survive a dormant import; they need only
    // the forward declaration selected by loweredProgramDeclaresCapability.
    // Native atomic support is needed only once semantic instantiation creates
    // a concrete sealed storage capability (including
    // construct-without-operation).
    if (std::any_of(mir.classInstances().begin(), mir.classInstances().end(),
                    [](const MirClassInstance &instance) {
                      return instance.compilerCapability ==
                                 CompilerCapabilityTypeKind::AtomicStorage ||
                             instance.compilerCapability ==
                                 CompilerCapabilityTypeKind::SharedOwner;
                    })) {
      return true;
    }
    for (const MirBodyAddress address : enumerateMirBodyAddresses(mir)) {
      const MirBody *body = findMirBody(mir, address);
      if (body == nullptr) {
        continue;
      }
      for (const MirBlock &block : body->blocks) {
        for (const MirInstruction &instruction : block.instructions) {
          switch (instruction.intrinsic) {
          case IntrinsicKind::AtomicLoad:
          case IntrinsicKind::AtomicWait:
          case IntrinsicKind::AtomicNotifyOne:
          case IntrinsicKind::AtomicNotifyAll:
          case IntrinsicKind::AtomicStore:
          case IntrinsicKind::AtomicExchange:
          case IntrinsicKind::AtomicCompareExchange:
            return true;
          default:
            break;
          }
        }
      }
    }
    return false;
  }

  [[nodiscard]] bool loweredProgramContainsMutexes() const {
    // Generic standard-library declarations are present after a dormant
    // import; they need only the forward declarations selected by
    // loweredProgramDeclaresCapability, not native support. Concrete MIR
    // capability instances are created only once a mutex owner/guard is
    // instantiated, including the construct-without-lock case.
    if (std::any_of(mir.classInstances().begin(), mir.classInstances().end(),
                    [](const MirClassInstance &instance) {
                      return instance.compilerCapability ==
                                 CompilerCapabilityTypeKind::MutexStorage ||
                             instance.compilerCapability ==
                                 CompilerCapabilityTypeKind::MutexLockToken;
                    })) {
      return true;
    }
    for (const MirBodyAddress address : enumerateMirBodyAddresses(mir)) {
      const MirBody *body = findMirBody(mir, address);
      if (body == nullptr) {
        continue;
      }
      for (const MirBlock &block : body->blocks) {
        for (const MirInstruction &instruction : block.instructions) {
          switch (instruction.intrinsic) {
          case IntrinsicKind::MutexLock:
          case IntrinsicKind::MutexUnlock:
          case IntrinsicKind::MutexGuardActive:
          case IntrinsicKind::MutexGuardRead:
          case IntrinsicKind::MutexGuardReadMut:
          case IntrinsicKind::MutexLockFailure:
            return true;
          default:
            break;
          }
        }
      }
    }
    return false;
  }

  [[nodiscard]] bool loweredProgramContainsSimd() const {
    for (const MirBodyAddress address : enumerateMirBodyAddresses(mir)) {
      const MirBody *body = findMirBody(mir, address);
      if (body == nullptr)
        continue;
      for (const MirBlock &block : body->blocks) {
        for (const MirInstruction &instruction : block.instructions) {
          if (simdIntrinsic(instruction.intrinsic))
            return true;
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

  bool classBoundByteTransport(const SemanticType &type, ClassId owner) const {
    if (type.kind == SemanticType::Array && type.arguments.size() == 1)
      return classBoundByteTransport(type.arguments.front(), owner);
    if (type.kind != SemanticType::TypeParameter &&
        type.kind != SemanticType::TypePack)
      return false;
    const LoweredDeclaration *row = loweredClassRow(owner);
    const auto *declaration =
        row == nullptr ? nullptr
                       : std::get_if<LoweredClassDeclaration>(&row->payload);
    return declaration != nullptr &&
           std::any_of(declaration->genericParameters.begin(),
                       declaration->genericParameters.end(),
                       [&](const auto &parameter) {
                         return parameter.id == type.genericParameterId;
                       });
  }

  void emitDependentByteTransportPredicate(const SemanticType &type) {
    // A class-owned parameter or pack element may itself be a checked
    // reference. Preserve that exact type; only ordinary values use the
    // private byte transport ABI. Strip references before querying the native
    // representation trait, whose ADL probe forms a pointer to its argument.
    output << "(!std::is_reference_v<";
    emitSemanticType(type);
    output << "> && "
              "::gti_internal::backend::byte_transport<std::remove_cvref_t<";
    emitSemanticType(type);
    output << ">>::value)";
  }

  void emitDependentTransportParameter(const SemanticType &type,
                                       std::string_view name,
                                       bool pack = false) {
    output << "std::conditional_t<";
    emitDependentByteTransportPredicate(type);
    output << ", const ";
    emitSemanticType(type);
    output << " &, ";
    emitSemanticType(type);
    output << '>';
    if (pack)
      output << "...";
    output << ' ' << name;
  }

  void emitLoweredParameter(const LoweredParameter &parameter,
                            std::string_view name, ClassId transportOwner = 0) {
    if (classBoundByteTransport(parameter.type, transportOwner)) {
      emitDependentTransportParameter(parameter.type, name, parameter.pack);
      return;
    }
    if (cppRequiresIndirectValueTransport(mir, parameter.type)) {
      output << "const ";
      emitSemanticType(parameter.type);
      output << " &";
      if (!name.empty())
        output << ' ' << name;
      return;
    }
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

  void emitLoweredCallableTemplateParameter(const LoweredParameter &parameter,
                                            const SemanticType &projectedType,
                                            const SemanticType &concreteType,
                                            std::string_view name) {
    if (cppRequiresIndirectValueTransport(mir, concreteType)) {
      output << "const "
             << cppSemanticTypeSpelling(loweredProgram, standard, projectedType)
             << " &";
      if (!name.empty())
        output << ' ' << name;
      return;
    }
    const bool reference = projectedType.kind == SemanticType::Reference;
    const bool pointerRepresentation =
        projectedType.kind == SemanticType::RawPointer ||
        projectedType.kind == SemanticType::CString;
    const LoweredSymbol *symbol =
        parameter.symbol == 0 ? nullptr
                              : loweredProgram.findSymbol(parameter.symbol);
    const bool moveOnly =
        (symbol != nullptr && isMoveOnlyOwner(symbol->traits)) ||
        projectedType.kind == SemanticType::UniqueOwner ||
        parameter.type.kind == SemanticType::TypeParameter ||
        parameter.type.kind == SemanticType::TypePack;
    if (!reference && parameter.mutability == Mutability::Immutable &&
        !parameter.pack && !moveOnly && !pointerRepresentation &&
        !parameter.explicitlyMoved) {
      output << "const ";
    }
    const std::string spelling =
        cppSemanticTypeSpelling(loweredProgram, standard, projectedType);
    if (spelling.empty()) {
      throw std::logic_error(
          "lowered callable template has an unspellable projected parameter");
    }
    output << spelling;
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
                             bool mirArgumentNames = false,
                             ClassId transportOwner = 0) {
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      emitLoweredParameter(parameters[index],
                           mirArgumentNames
                               ? "__gti_mir_arg_" + std::to_string(index)
                               : parameters[index].name,
                           transportOwner);
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
      const bool indirect =
          cppRequiresIndirectValueTransport(mir, (*flattened)[index]);
      if (indirect)
        output << "const ";
      emitSemanticType((*flattened)[index]);
      if (indirect)
        output << " &";
      output << " __gti_mir_arg_" << index;
    }
    return flattened->size();
  }

  [[nodiscard]] const LoweredDeclaration *loweredClassRow(ClassId id) const {
    if (const auto found = nativeClassRows.find(id);
        found != nativeClassRows.end())
      return &found->second;
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

  [[nodiscard]] bool loweredConstructorInstanceIsNameable(
      const MirConstructorInstance &instance) const {
    const MirClassInstance *owner = mir.findClassInstance(instance.owner);
    const LoweredConstructorInstance *lowered =
        loweredProgram.findConstructorInstance(instance.id);
    return owner != nullptr && lowered != nullptr &&
           loweredTypeIsNameable(owner->type) &&
           std::all_of(instance.parameterTypes.begin(),
                       instance.parameterTypes.end(),
                       [&](const SemanticType &type) {
                         return loweredTypeIsNameable(type);
                       }) &&
           std::all_of(lowered->typeArguments.begin(),
                       lowered->typeArguments.end(),
                       [&](const SemanticType &type) {
                         return loweredTypeIsNameable(type);
                       }) &&
           std::all_of(lowered->valueArguments.begin(),
                       lowered->valueArguments.end(),
                       [](const CompileTimeValue &value) {
                         return value.kind == CompileTimeValue::UInt64;
                       });
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

  [[nodiscard]] std::optional<LoweredClassInitializerSelection>
  loweredClassInitializers(const MirClassInstance &instance) const {
    LoweredClassInitializerSelection result;
    result.instances.push_back(instance.id);
    const CppMirInitializerScheduleText fields = generalMirInitializerSchedule(
        {.kind = MirBodyKind::FieldInitializers, .owner = instance.id});
    result.fieldsSupported = fields.supported;
    if (fields.supported) {
      for (const CppMirFieldInitializerSpelling &field : fields.fields) {
        result.fields.emplace(field.field, field.spelling);
      }
    }
    const CppMirInitializerScheduleText statics = generalMirInitializerSchedule(
        {.kind = MirBodyKind::StaticFieldInitializers, .owner = instance.id});
    result.staticsVerifiedEmpty = statics.supported && statics.fields.empty();
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

  void emitLoweredFailureResultParameter(const SemanticType &returnType,
                                         bool callableTemplate = false,
                                         bool retainVoidChannel = false) {
    const auto emitType = [&](const SemanticType &type) {
      if (!callableTemplate) {
        emitSemanticType(type);
        return;
      }
      const std::string spelling =
          cppSemanticTypeSpelling(loweredProgram, standard, type);
      if (spelling.empty()) {
        throw std::logic_error(
            "lowered callable template has an unspellable projected result");
      }
      output << spelling;
    };
    if (returnType.kind == SemanticType::Reference &&
        returnType.arguments.size() == 1) {
      if (returnType.referenceAccess == AccessMode::ReadOnly) {
        output << "const ";
      }
      emitType(returnType.arguments.front());
      output << " **__gti_mir_out_result, ";
    } else if (returnType != SemanticType::Void) {
      emitType(returnType);
      output << " *__gti_mir_out_result, ";
    } else if (retainVoidChannel) {
      output << "void *__gti_mir_out_result, ";
    }
  }

  [[nodiscard]] std::string
  loweredFunctionInstanceSpelling(const LoweredDeclaration &row,
                                  const LoweredFunctionDeclaration &function,
                                  const MirFunctionInstance *instance) const {
    std::string name = cppFunctionSpelling(function, row.name);
    if (instance == nullptr) {
      return name;
    }
    const LoweredFunctionInstance *concrete =
        loweredFunctionInstance(instance->id);
    if (!instance->owner) {
      if (concrete != nullptr)
        if (const auto callable = cppCallableFreeFunctionSpelling(
                loweredProgram, row, function, *concrete, *instance))
          return *callable;
      return name;
    }
    if (concrete != nullptr) {
      if (const std::optional<std::string> direct =
              cppConcreteMemberFunctionSpelling(loweredProgram, row, function,
                                                *concrete, *instance)) {
        name = *direct;
      }
    }
    return name;
  }

  [[nodiscard]] std::size_t
  emitLoweredCallerLocationParameter(const LoweredFunctionDeclaration &function,
                                     std::size_t parameterCount) {
    if (!function.tracksCaller) {
      return parameterCount;
    }
    if (parameterCount != 0) {
      output << ", ";
    }
    output << "std::uint32_t __gti_mir_caller_site";
    return parameterCount + 1;
  }

  void emitLoweredFunctionSignature(
      const LoweredDeclaration &row, const LoweredFunctionDeclaration &function,
      const MirFunctionInstance *instance, bool failure,
      bool insideClass = false, std::string_view owner = {},
      bool sourceParameterTypes = false, bool explicitGenericArguments = false,
      std::optional<bool> dependentIndirect = std::nullopt) {
    if (!failure && !dependentIndirect && instance != nullptr &&
        sourceParameterTypes && !function.virtualMethod &&
        !function.genericParameters.empty() &&
        classBoundByteTransport(function.returnType, function.ownerClass)) {
      dependentIndirect =
          cppRequiresIndirectValueTransport(mir, instance->returnType);
    }
    std::optional<CppMirCallableTemplateSignature> callableSignature;
    if (instance != nullptr && sourceParameterTypes) {
      callableSignature =
          cppMirCallableTemplateSignature(loweredProgram, function, *instance);
      if (!callableSignature) {
        throw std::logic_error(
            "lowered callable template has no coherent projected signature");
      }
    }
    if (insideClass && function.staticMember) {
      output << "static ";
    }
    if (insideClass && function.virtualMethod && !function.overrideMethod &&
        !function.staticMember) {
      output << "virtual ";
    }
    const bool conditionalVirtualResult =
        cppVirtualFamilyHasConditionalResultChannel(loweredProgram, function);
    const SemanticType &signatureResult =
        callableSignature     ? callableSignature->returnType
        : instance == nullptr ? function.returnType
                              : instance->returnType;
    const bool indirectResult = dependentIndirect.value_or(
        (function.definitionKind == MirDefinitionKind::Source ||
         function.pureVirtual) &&
        function.linkage != LanguageLinkage::C &&
        cppRequiresIndirectValueTransport(
            mir, instance == nullptr ? signatureResult : instance->returnType));
    if (failure) {
      output << "bool ";
    } else if (conditionalVirtualResult && instance == nullptr &&
               classBoundByteTransport(signatureResult, function.ownerClass)) {
      output << "std::conditional_t<";
      emitDependentByteTransportPredicate(signatureResult);
      output << ", void, ";
      emitSemanticType(signatureResult);
      output << "> ";
    } else if (indirectResult) {
      output << "void ";
    } else {
      const SemanticType &returnType =
          callableSignature     ? callableSignature->returnType
          : instance == nullptr ? function.returnType
                                : instance->returnType;
      if (callableSignature) {
        const std::string spelling =
            cppSemanticTypeSpelling(loweredProgram, standard, returnType);
        if (spelling.empty()) {
          throw std::logic_error(
              "lowered callable template has an unspellable projected result");
        }
        output << spelling;
      } else {
        emitSemanticType(returnType);
      }
      if (returnType.kind != SemanticType::Reference) {
        output << ' ';
      }
    }
    if (!owner.empty()) {
      output << owner << "::";
    }
    const std::string name =
        loweredFunctionInstanceSpelling(row, function, instance);
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
    if (callableSignature) {
      for (std::size_t index = 0;
           index < callableSignature->parameterTypes.size(); ++index) {
        if (index != 0) {
          output << ", ";
        }
        emitLoweredCallableTemplateParameter(
            function.parameters[index],
            callableSignature->parameterTypes[index],
            instance->parameterTypes[index],
            "__gti_mir_arg_" + std::to_string(index));
      }
      parameterCount = callableSignature->parameterTypes.size();
    } else if (instance != nullptr && !sourceParameterTypes) {
      parameterCount = emitLoweredMirParameters(instance->parameterTypes);
    } else {
      emitLoweredParameters(function.parameters, sourceParameterTypes,
                            function.ownerClass);
      parameterCount = function.parameters.size();
    }
    parameterCount =
        emitLoweredCallerLocationParameter(function, parameterCount);
    if (failure) {
      if (parameterCount != 0) {
        output << ", ";
      }
      emitLoweredFailureResultParameter(
          callableSignature     ? callableSignature->returnType
          : instance == nullptr ? function.returnType
                                : instance->returnType,
          callableSignature.has_value(),
          cppFunctionHasFailureResultChannel(loweredProgram, function));
      output << "::gti_failure_record_v2 *__gti_mir_failure_record";
    }
    if (!failure && (indirectResult || conditionalVirtualResult)) {
      if (parameterCount != 0)
        output << ", ";
      if (conditionalVirtualResult)
        output << "std::add_pointer_t<";
      emitSemanticType(signatureResult);
      output << (conditionalVirtualResult ? "> __gti_mir_out_result"
                                          : " *__gti_mir_out_result");
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
    if (dependentIndirect) {
      output << " requires (";
      if (!*dependentIndirect)
        output << '!';
      emitDependentByteTransportPredicate(signatureResult);
      output << ')';
    }
  }

  void emitLoweredCAbiType(const SemanticType &type) {
    if (type == SemanticType::StringView) {
      output << "::gti_c_string_view";
    } else {
      output << cppNativeTypeSpelling(loweredProgram, standard, type);
    }
  }

  void emitLoweredCAbiSignature(const LoweredDeclaration &row,
                                const LoweredFunctionDeclaration &function) {
    emitLoweredCAbiType(function.returnType);
    output << ' ' << cppFunctionSpelling(function, row.name) << '(';
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
    const auto representedFailureBody = [&](const MirFunctionInstance
                                                &instance) {
      if (loweredFailureBodySupported(instance))
        return true;
      const auto *function =
          loweredProgram.findFunctionDeclaration(instance.declaration);
      return function != nullptr &&
             !loweredCallableTemplateBody(*function, {&instance}, true).empty();
    };
    for (const MirFunctionInstance &candidate : mir.functionInstances()) {
      if (candidate.declaration != declaration ||
          cppMirVirtualFailureContractRoot(mir, candidate) != &candidate) {
        continue;
      }
      if (!candidate.pureVirtual && !representedFailureBody(candidate)) {
        continue;
      }
      bool complete = true;
      for (const MirFunctionInstance &override : mir.functionInstances()) {
        if (override.id == candidate.id ||
            cppMirVirtualFailureContractRoot(mir, override) != &candidate) {
          continue;
        }
        if (!representedFailureBody(override)) {
          complete = false;
          break;
        }
      }
      // The abstract root owns the virtual ABI even when no override is
      // instantiated. A checked call through an empty owner still names that
      // declaration after its null check. Existing concrete overrides must
      // remain representable, but their existence cannot gate the contract.
      if (complete) {
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
        if (!type->genericParameters.empty() &&
            !loweredClassNeedsNativePrimary(*type)) {
          continue;
        }
        if (!type->genericArgumentPatterns.empty()) {
          writeIndent();
          output << "template <";
          for (std::size_t index = 0;
               index < type->genericArgumentPatterns.size(); ++index) {
            if (index != 0) {
              output << ", ";
            }
            output << "typename __gti_argument_" << index;
          }
          output << ">\n";
        } else if (!type->genericParameters.empty()) {
          emitLoweredTemplateDeclaration(type->genericParameters);
        }
        writeIndent();
        output << (type->kind == ClassKind::Struct  ? "struct "
                   : type->kind == ClassKind::Union ? "union "
                                                    : "class ")
               << declaration->name << ";\n";
        SemanticType nativeType;
        nativeType.kind = SemanticType::Class;
        nativeType.classId = type->id;
        if (type->cAbiRecord &&
            cppRequiresIndirectValueTransport(mir, nativeType)) {
          writeIndent();
          output << "std::true_type __gti_byte_transport_tag("
                 << declaration->name << " *);\n";
        }
        continue;
      }
      if (const auto *enumeration =
              std::get_if<LoweredEnumDeclaration>(&declaration->payload)) {
        const std::string name =
            cppConcreteEnumName(loweredProgram, enumeration->type)
                .value_or(declaration->name);
        writeIndent();
        if (enumeration->payload) {
          output << "struct " << name << ";\n";
        } else {
          output << "enum class " << name << " : ";
          emitSemanticType(enumeration->underlyingType);
          output << ";\n";
        }
      }
    }
  }

  [[nodiscard]] std::string loweredDeclaredClassArguments(
      const LoweredClassDeclaration &declaration) const {
    if (declaration.exactSpecializationPrimary == 0 &&
        declaration.genericArgumentPatterns.empty()) {
      return {};
    }
    std::string result = "<";
    bool separator = false;
    const auto &arguments = declaration.exactSpecializationPrimary != 0
                                ? declaration.exactTypeArguments
                                : declaration.genericArgumentPatterns;
    for (const SemanticType &argument : arguments) {
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
      if (!loweredConcreteClassInstances(type->id).empty()) {
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
             << row.name << loweredDeclaredClassArguments(*type) << ";\n";
      emitLoweredNamespaceClose(namespaceCount);
    }
  }

  void emitLoweredConcreteClassForwardDeclarations() {
    std::unordered_set<std::string> emitted;
    const auto emitType = [&](const auto &self,
                              const SemanticType &type) -> void {
      // Type-only aliases need the same nominal spelling as executable uses,
      // but do not require an instantiated layout or executable class body.
      // Visit only the already-resolved argument graph, not alias members of
      // the referenced class: recursive aliases must not expand endlessly.
      for (const SemanticType &argument : type.arguments) {
        self(self, argument);
      }
      const std::optional<std::string> name =
          cppConcreteClassName(loweredProgram, type);
      const LoweredDeclaration *row = type.kind == SemanticType::Class
                                          ? loweredClassRow(type.classId)
                                          : nullptr;
      const auto *declaration =
          row == nullptr ? nullptr
                         : std::get_if<LoweredClassDeclaration>(&row->payload);
      if (!name || row == nullptr || declaration == nullptr ||
          !emitted.insert(*name).second) {
        return;
      }
      const std::size_t namespaceCount =
          emitLoweredNamespaceOpen(row->namespaceScope);
      writeIndent();
      output << (declaration->kind == ClassKind::Struct  ? "struct "
                 : declaration->kind == ClassKind::Union ? "union "
                                                         : "class ")
             << *name << ";\n";
      emitLoweredNamespaceClose(namespaceCount);
    };
    for (const MirClassInstance &instance : mir.classInstances()) {
      emitType(emitType, instance.type);
      // A dependent member alias can become concrete only in this sealed
      // instance catalog. Its target still need not have an executable layout.
      for (const MirClassTypeAlias &alias : instance.typeAliases) {
        emitType(emitType, alias.type);
      }
    }
    for (const LoweredDeclaration &row : loweredProgram.declarations()) {
      if (const auto *alias =
              std::get_if<LoweredTypeAliasDeclaration>(&row.payload)) {
        emitType(emitType, alias->type);
      }
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

  [[nodiscard]] bool
  loweredClassUsesDirectRepresentation(const MirClassInstance &instance) const {
    return cppConcreteClassName(loweredProgram, instance.type).has_value();
  }

  [[nodiscard]] bool
  loweredClassUsesDirectRepresentation(HirClassInstanceId instance) const {
    const MirClassInstance *row = mir.findClassInstance(instance);
    return row != nullptr && loweredClassUsesDirectRepresentation(*row);
  }

  [[nodiscard]] bool loweredClassSeedsNativePrimary(
      const LoweredClassDeclaration &declaration) const {
    const auto containsLexicalClosure = [&](const auto &self,
                                            const SemanticType &type) -> bool {
      return type.kind == SemanticType::Lambda ||
             std::any_of(type.arguments.begin(), type.arguments.end(),
                         [&](const SemanticType &argument) {
                           return self(self, argument);
                         });
    };
    const bool lexicalCarrier = std::any_of(
        mir.functionInstances().begin(), mir.functionInstances().end(),
        [&](const MirFunctionInstance &function) {
          const auto lexical = [&](const SemanticType &type) {
            return containsLexicalClosure(containsLexicalClosure, type);
          };
          return lexical(function.returnType) ||
                 std::any_of(function.typeArguments.begin(),
                             function.typeArguments.end(), lexical) ||
                 std::any_of(function.parameterTypes.begin(),
                             function.parameterTypes.end(), lexical);
        });
    if (lexicalCarrier && !declaration.genericParameters.empty()) {
      return true;
    }
    bool discovered = false;
    for (const MirClassInstance &instance : mir.classInstances()) {
      if (instance.declaration != declaration.id) {
        continue;
      }
      discovered = true;
      if (!loweredClassUsesDirectRepresentation(instance)) {
        return true;
      }
    }
    // An unused generic declaration has no concrete execution ABI to migrate.
    // Keep its source template available for dependent declarations and
    // tooling-facing emitted artifacts; every discovered nameable instance
    // still takes the direct path below.
    return !discovered;
  }

  void collectLoweredNativePrimaryTypeDependencies(
      const SemanticType &type,
      std::unordered_set<ClassId> &dependencies) const {
    if (type.kind == SemanticType::Class ||
        type.kind == SemanticType::TypeName) {
      const LoweredClassDeclaration *referenced =
          loweredProgram.findClassDeclaration(type.classId);
      if (referenced != nullptr) {
        if (referenced->exactSpecializationPrimary != 0) {
          referenced = loweredProgram.findClassDeclaration(
              referenced->exactSpecializationPrimary);
        }
        if (referenced != nullptr && !referenced->genericParameters.empty()) {
          dependencies.insert(referenced->id);
        }
      }
    }
    for (const SemanticType &argument : type.arguments) {
      collectLoweredNativePrimaryTypeDependencies(argument, dependencies);
    }
    for (const SemanticType &argument : type.lambdaEnclosingClassTypes) {
      collectLoweredNativePrimaryTypeDependencies(argument, dependencies);
    }
    for (const SemanticType &argument : type.lambdaEnclosingFunctionTypes) {
      collectLoweredNativePrimaryTypeDependencies(argument, dependencies);
    }
  }

  void collectLoweredNativePrimaryDeclarationDependencies(
      const LoweredDeclaration &row,
      std::unordered_set<ClassId> &dependencies) const {
    if (const auto *type = std::get_if<LoweredClassDeclaration>(&row.payload)) {
      for (const LoweredClassBase &base : type->bases) {
        collectLoweredNativePrimaryTypeDependencies(base.type, dependencies);
      }
    } else if (const auto *function =
                   std::get_if<LoweredFunctionDeclaration>(&row.payload)) {
      collectLoweredNativePrimaryTypeDependencies(function->returnType,
                                                  dependencies);
      for (const LoweredParameter &parameter : function->parameters) {
        collectLoweredNativePrimaryTypeDependencies(parameter.type,
                                                    dependencies);
      }
      for (const LoweredConceptRequirement &requirement :
           function->requirements) {
        for (const SemanticType &argument : requirement.arguments) {
          collectLoweredNativePrimaryTypeDependencies(argument, dependencies);
        }
      }
      for (const LoweredCallableParameter &callable :
           function->callableParameters) {
        collectLoweredNativePrimaryTypeDependencies(callable.callableType,
                                                    dependencies);
        for (const LoweredCallableSignature &signature : callable.signatures) {
          collectLoweredNativePrimaryTypeDependencies(signature.returnType,
                                                      dependencies);
          for (const SemanticType &parameter : signature.parameterTypes) {
            collectLoweredNativePrimaryTypeDependencies(parameter,
                                                        dependencies);
          }
        }
      }
    } else if (const auto *constructor =
                   std::get_if<LoweredConstructorDeclaration>(&row.payload)) {
      for (const LoweredParameter &parameter : constructor->parameters) {
        collectLoweredNativePrimaryTypeDependencies(parameter.type,
                                                    dependencies);
      }
      for (const LoweredConceptRequirement &requirement :
           constructor->requirements) {
        for (const SemanticType &argument : requirement.arguments) {
          collectLoweredNativePrimaryTypeDependencies(argument, dependencies);
        }
      }
    } else if (const auto *storage =
                   std::get_if<LoweredStorageDeclaration>(&row.payload)) {
      collectLoweredNativePrimaryTypeDependencies(storage->type, dependencies);
    } else if (const auto *enumeration =
                   std::get_if<LoweredEnumDeclaration>(&row.payload)) {
      collectLoweredNativePrimaryTypeDependencies(enumeration->underlyingType,
                                                  dependencies);
      for (const LoweredEnumerator &enumerator : enumeration->enumerators) {
        for (const SemanticType &payload : enumerator.payloadTypes) {
          collectLoweredNativePrimaryTypeDependencies(payload, dependencies);
        }
      }
    } else if (const auto *alias =
                   std::get_if<LoweredTypeAliasDeclaration>(&row.payload)) {
      collectLoweredNativePrimaryTypeDependencies(alias->type, dependencies);
    }

    for (const LoweredDeclaration *child : loweredChildren(row.id)) {
      collectLoweredNativePrimaryDeclarationDependencies(*child, dependencies);
    }
  }

  void prepareLoweredNativePrimaryClasses() {
    loweredNativePrimaryClasses.clear();
    for (const LoweredDeclaration &row : loweredProgram.declarations()) {
      const auto *declaration =
          std::get_if<LoweredClassDeclaration>(&row.payload);
      if (declaration != nullptr &&
          declaration->exactSpecializationPrimary == 0 &&
          !declaration->genericParameters.empty() &&
          loweredClassSeedsNativePrimary(*declaration)) {
        loweredNativePrimaryClasses.insert(declaration->id);
      }
    }

    bool changed = true;
    while (changed) {
      changed = false;
      const std::vector<ClassId> nativeClasses(
          loweredNativePrimaryClasses.begin(),
          loweredNativePrimaryClasses.end());
      for (const ClassId classId : nativeClasses) {
        const LoweredDeclaration *row = loweredClassRow(classId);
        if (row == nullptr) {
          throw std::logic_error(
              "native generic carrier lost its lowered declaration");
        }
        std::unordered_set<ClassId> dependencies;
        collectLoweredNativePrimaryDeclarationDependencies(*row, dependencies);
        for (const ClassId dependency : dependencies) {
          changed =
              loweredNativePrimaryClasses.insert(dependency).second || changed;
        }
      }
    }
  }

  [[nodiscard]] bool loweredClassNeedsNativePrimary(
      const LoweredClassDeclaration &declaration) const {
    return loweredNativePrimaryClasses.contains(declaration.id);
  }

  [[nodiscard]] std::vector<const MirClassInstance *>
  loweredConcreteClassInstances(ClassId declaration) const {
    std::vector<const MirClassInstance *> result;
    for (const MirClassInstance &instance : mir.classInstances()) {
      if (instance.declaration == declaration &&
          loweredClassUsesDirectRepresentation(instance)) {
        result.push_back(&instance);
      }
    }
    return result;
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
    const bool indirectResult =
        cppRequiresIndirectValueTransport(mir, instance.returnType);
    if (failure) {
      output << "bool ";
    } else {
      output << "template <> ";
      if (indirectResult)
        output << "void";
      else
        emitSemanticType(instance.returnType);
      output << ' ';
    }
    const std::string name = cppFunctionSpelling(function, row.name);
    output << (failure ? cppMirFailureSiblingSpelling(name) : name);
    if (!failure) {
      emitLoweredExplicitArguments(*lowered);
    }
    output << '(';
    const std::size_t parameterCount = emitLoweredCallerLocationParameter(
        function, emitLoweredMirParameters(instance.parameterTypes));
    if (failure) {
      if (parameterCount != 0) {
        output << ", ";
      }
      emitLoweredFailureResultParameter(
          instance.returnType, false,
          cppFunctionHasFailureResultChannel(loweredProgram, function));
      output << "::gti_failure_record_v2 *__gti_mir_failure_record";
    }
    if (!failure && indirectResult) {
      if (parameterCount != 0)
        output << ", ";
      emitSemanticType(instance.returnType);
      output << " *__gti_mir_out_result";
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
    const bool indirectResult =
        cppRequiresIndirectValueTransport(mir, instance.returnType);
    if (failure) {
      output << "bool ";
    } else {
      if (indirectResult)
        output << "void";
      else
        emitSemanticType(instance.returnType);
      if (instance.returnType.kind != SemanticType::Reference) {
        output << ' ';
      }
    }
    output << (failure ? cppMirFailureSiblingSpelling(*name) : *name) << '(';
    const std::size_t parameterCount = emitLoweredCallerLocationParameter(
        function, emitLoweredMirParameters(instance.parameterTypes));
    if (failure) {
      if (parameterCount != 0) {
        output << ", ";
      }
      emitLoweredFailureResultParameter(
          instance.returnType, false,
          cppFunctionHasFailureResultChannel(loweredProgram, function));
      output << "::gti_failure_record_v2 *__gti_mir_failure_record";
    }
    if (!failure && indirectResult) {
      if (parameterCount != 0)
        output << ", ";
      emitSemanticType(instance.returnType);
      output << " *__gti_mir_out_result";
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
      const std::string symbol = function->externalSymbol.empty()
                                     ? declaration->name
                                     : function->externalSymbol;
      const std::string name =
          cppFunctionSpelling(*function, declaration->name);
      // Source semantics admits only portable ASCII C identifiers here, so no
      // source-provided quote, escape or directive can enter the link label.
      // The selected target fixes the object format: Mach-O symbols carry a
      // leading underscore, ELF symbols do not, and COFF transports the C-name
      // alias in a linker directive. Host preprocessor checks would leave an
      // unresolved target branch in the lowered program.
      const std::string &os = loweredProgram.target().os;
      if (os == "windows") {
        output << ";\n#pragma comment(linker, \"/alternatename:" << name << '='
               << symbol << "\")\n";
      } else {
        output << " __asm__(\"" << (os == "macos" ? "_" : "") << symbol
               << "\");\n";
      }
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
      const bool retainPrimaryDeclaration =
          generic && function->definitionKind != MirDefinitionKind::Source;
      if (!generic || retainPrimaryDeclaration) {
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
      if (!generic && !instances.empty() && hasFailure) {
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
        if (concrete != nullptr &&
            cppCallableFreeFunctionSpelling(loweredProgram, *declaration,
                                            *function, *concrete, *instance)) {
          for (const bool failure : {false, true}) {
            if (loweredCallableTemplateBody(*function, {instance}, failure)
                    .empty())
              continue;
            emitLoweredTemplateDeclaration(
                function->genericParameters,
                loweredFunctionNeedsThreadTaskTemplate({instance}));
            writeIndent();
            emitLoweredFunctionSignature(*declaration, *function, instance,
                                         failure, false, {}, true);
            output << ";\n";
          }
          continue;
        }
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

  // GTI has already computed these values. Native constexpr is only a
  // representation qualifier: C++20 expected-lite is non-literal, including
  // when nested in an array or source-defined record. Keep such storage const
  // without asking the native compiler to repeat constant evaluation.
  [[nodiscard]] bool nativeConstantStorage(const SemanticType &type,
                                           std::size_t depth = 0) const {
    if (standard != CppStandard::Cpp20)
      return true;
    if (depth > 128 || type.kind == SemanticType::Expected ||
        type.kind == SemanticType::Unexpected)
      return false;
    if (type.kind == SemanticType::Class) {
      const auto found = std::find_if(
          mir.classInstances().begin(), mir.classInstances().end(),
          [&](const auto &instance) { return instance.type == type; });
      if (found == mir.classInstances().end())
        return false;
      return std::all_of(found->declaredFields.begin(),
                         found->declaredFields.end(), [&](const auto &field) {
                           return nativeConstantStorage(field.type, depth + 1);
                         });
    }
    return std::all_of(type.arguments.begin(), type.arguments.end(),
                       [&](const auto &argument) {
                         return nativeConstantStorage(argument, depth + 1);
                       });
  }

  void emitLoweredStorageType(const LoweredStorageDeclaration &storage,
                              bool declarationOnly) {
    const bool runtime = loweredRuntimeProgramStorage(storage);
    const bool rawPointer = storage.type.kind == SemanticType::RawPointer;
    const bool moveOnly =
        isMoveOnlyOwner(storage.traits) || containsTypeParameter(storage.type);
    const bool representationConstexpr =
        storage.constexprStorage && nativeConstantStorage(storage.type);
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
    const std::string name =
        cppConcreteEnumName(loweredProgram, enumeration.type)
            .value_or(row.name);
    writeIndent();
    if (enumeration.payload) {
      output << "struct " << name << " {\n";
      ++indentation;
      for (const LoweredEnumerator &variant : enumeration.enumerators) {
        writeIndent();
        output << "struct __gti_variant_" << variant.variantIndex << " {\n";
        ++indentation;
        for (std::size_t index = 0; index < variant.payloadTypes.size();
             ++index) {
          writeIndent();
          const bool byteTransport = cppRequiresBytePreservingTransport(
              mir, variant.payloadTypes[index]);
          if (byteTransport)
            output << "::gti_internal::backend::representation_cell<";
          emitSemanticType(variant.payloadTypes[index]);
          if (byteTransport)
            output << '>';
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
      // The first alternative is a backend-private disengaged state. Source
      // payload enums still have exactly their declared closed alternatives;
      // this state exists only so a transformed failure-capable constructor
      // can default-create its observation-free native shell, publish fields
      // in verified MIR order, and destroy an unpublished shell safely.
      output << "std::variant<std::monostate";
      for (std::size_t index = 0; index < enumeration.enumerators.size();
           ++index) {
        output << ", __gti_variant_"
               << enumeration.enumerators[index].variantIndex;
      }
      output << "> __gti_value;\n";
      writeIndent();
      output << "friend bool operator==(const " << name << " &, const " << name
             << " &) = default;\n";
      --indentation;
      writeIndent();
      output << "};\n";
      return;
    }

    output << "enum class " << name << " : ";
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
    output << cppNativeTypeSpelling(loweredProgram, standard, *element);
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
        storage.constexprStorage && nativeConstantStorage(storage.type);

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
        !storage.staticStorage) {
      emitLoweredCAbiFieldType(storage.type, name);
    } else {
      const bool byteField = insideClass && owner && !storage.staticStorage &&
                             cppUsesDependentByteField(storage.type);
      if (byteField)
        output << "::gti_internal::backend::byte_field<";
      emitLoweredStorageRepresentationType(storage);
      if (byteField)
        output << '>';
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

  void emitLoweredCallableAdapter(
      const LoweredDeclaration &row, const LoweredFunctionDeclaration &function,
      const LoweredDeclaration &classRow, bool definition = false,
      std::optional<bool> dependentIndirect = std::nullopt) {
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
    const bool dependentOwner = loweredOwnerIsGeneric(classRow);
    if (!dependentIndirect && dependentOwner &&
        classBoundByteTransport(function.returnType, function.ownerClass)) {
      emitLoweredCallableAdapter(row, function, classRow, definition, false);
      emitLoweredCallableAdapter(row, function, classRow, definition, true);
      return;
    }
    const bool indirectResult = dependentIndirect.value_or(
        cppRequiresIndirectValueTransport(mir, function.returnType));
    const auto emitAdapterType = [&](const SemanticType &type) {
      // A retained native carrier can precede a concrete class used only in
      // its callable signature. Keep the exact resolved type, but make its
      // completeness check dependent on the injected owner. The hidden
      // friend's definition is then instantiated after layouts, without
      // introducing a false by-value layout edge through a call signature.
      if (dependentOwner) {
        output << "std::conditional_t<true, ";
      }
      emitSemanticType(type);
      if (dependentOwner) {
        output << ", " << classRow.name << '>';
      }
    };
    writeIndent();
    output << (definition ? "inline " : "friend ");
    if (indirectResult)
      output << "void";
    else
      emitAdapterType(function.returnType);
    output << " __gti_invoke(";
    if (function.receiverMutability == ReceiverMutability::ReadOnly) {
      output << "const ";
    }
    if (definition)
      emitLoweredPrimaryClassName(classRow);
    else
      output << classRow.name;
    output << (function.receiverMutability == ReceiverMutability::Consuming
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
      if (parameter.pack)
        output << "...";
    }
    output << ">";
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
      output << ", ";
      // The adapter transports its owned argument into the selected MIR
      // function. Source-level immutability does not make this internal
      // transport slot const (which would turn its move into a copy).
      LoweredParameter transport = function.parameters[index];
      transport.explicitlyMoved =
          transport.type.kind != SemanticType::Reference;
      if (classBoundByteTransport(transport.type, function.ownerClass)) {
        emitDependentTransportParameter(transport.type,
                                        "__gti_arg_" + std::to_string(index),
                                        transport.pack);
      } else if (dependentOwner) {
        const bool indirectParameter =
            cppRequiresIndirectValueTransport(mir, transport.type);
        if (indirectParameter)
          output << "const ";
        emitAdapterType(transport.type);
        if (indirectParameter)
          output << " &";
        if (transport.pack)
          output << "...";
        output << " __gti_arg_" << index;
      } else {
        emitLoweredParameter(transport, "__gti_arg_" + std::to_string(index));
      }
    }
    if (indirectResult) {
      output << ", ";
      emitAdapterType(function.returnType);
      output << " *__gti_mir_out_result";
    }
    if (!definition && !loweredOwnerIsGeneric(classRow) &&
        function.genericParameters.empty()) {
      // An ADL declaration needs only forward declarations of its by-value
      // types. Its forwarding definition needs complete types, including the
      // result. Emit it after all layouts, not as a false layout dependency:
      // a callable may return a class which itself stores that callable.
      output << ");\n";
      loweredDeferredCallableAdapters.push_back({row, function, classRow});
      return;
    }
    output << ')';
    if (dependentIndirect) {
      output << " requires (";
      if (!*dependentIndirect)
        output << '!';
      emitDependentByteTransportPredicate(function.returnType);
      output << ')';
    }
    output << " { ";
    if (!indirectResult && function.returnType != SemanticType::Void) {
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
      if (function.parameters[index].pack)
        output << "...";
    }
    if (indirectResult) {
      if (!function.parameters.empty())
        output << ", ";
      output << "__gti_mir_out_result";
    }
    output << "); }\n";
  }

  void emitLoweredCallableAdapterDefinitions() {
    for (const auto &adapter : loweredDeferredCallableAdapters) {
      const auto namespaceCount =
          emitLoweredNamespaceOpen(adapter.owner.namespaceScope);
      emitLoweredCallableAdapter(adapter.row, adapter.function, adapter.owner,
                                 true);
      emitLoweredNamespaceClose(namespaceCount);
    }
    loweredDeferredCallableAdapters.clear();
  }

  void deferLoweredMember(const LoweredDeclaration &row) {
    if (loweredDeferredMemberIds.insert(row.id).second) {
      loweredDeferredMembers.push_back(&row);
    }
  }

  [[nodiscard]] bool loweredMemberCallableTemplateBodyExists(
      const LoweredFunctionDeclaration &function,
      const std::vector<const MirFunctionInstance *> &instances, bool failure,
      bool genericOwner) const {
    if (!genericOwner) {
      return !loweredCallableTemplateBody(function, instances, failure).empty();
    }

    bool exists = false;
    std::vector<HirClassInstanceId> owners;
    for (const MirFunctionInstance *instance : instances) {
      if (instance == nullptr || !instance->owner) {
        throw std::logic_error(
            "lowered generic member callable instance lost its owner");
      }
      if (std::find(owners.begin(), owners.end(), *instance->owner) !=
          owners.end()) {
        continue;
      }
      owners.push_back(*instance->owner);
      std::vector<const MirFunctionInstance *> ownerInstances;
      std::copy_if(instances.begin(), instances.end(),
                   std::back_inserter(ownerInstances),
                   [&](const MirFunctionInstance *candidate) {
                     return candidate != nullptr &&
                            candidate->owner == instance->owner;
                   });
      exists = !loweredCallableTemplateBody(function, ownerInstances, failure)
                    .empty() ||
               exists;
    }
    return exists;
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
    const auto *owner = std::get_if<LoweredClassDeclaration>(&classRow.payload);
    const bool genericOwner =
        owner != nullptr && !owner->genericParameters.empty();
    const bool threadTaskTemplate =
        generic && loweredFunctionNeedsThreadTaskTemplate(instances);
    const bool hasCallableTemplateNormal =
        (generic || genericOwner) &&
        loweredMemberCallableTemplateBodyExists(function, instances, false,
                                                genericOwner);
    const bool retainPrimary =
        !generic || hasCallableTemplateNormal ||
        function.definitionKind != MirDefinitionKind::Source;
    if (generic && retainPrimary) {
      emitLoweredTemplateDeclaration(function.genericParameters,
                                     threadTaskTemplate);
    }
    if (retainPrimary) {
      const bool splitResult =
          !function.virtualMethod &&
          classBoundByteTransport(function.returnType, function.ownerClass);
      if (splitResult) {
        for (bool indirect : {false, true}) {
          if (indirect && generic)
            emitLoweredTemplateDeclaration(function.genericParameters,
                                           threadTaskTemplate);
          writeIndent();
          emitLoweredFunctionSignature(row, function, nullptr, false, true, {},
                                       generic, false, indirect);
          output << ";\n";
        }
      } else {
        writeIndent();
        emitLoweredFunctionSignature(row, function, nullptr, false, true, {},
                                     generic);
        if (function.pureVirtual) {
          output << " = 0;\n";
        } else {
          output << ";\n";
        }
      }
    }

    if (generic) {
      for (const MirFunctionInstance *instance : instances) {
        const LoweredFunctionInstance *concrete =
            loweredFunctionInstance(instance->id);
        if (concrete == nullptr || !instance->owner ||
            !cppConcreteMemberFunctionSpelling(loweredProgram, row, function,
                                               *concrete, *instance)) {
          continue;
        }
        writeIndent();
        emitLoweredFunctionSignature(row, function, instance, false, true);
        output << (function.pureVirtual ? " = 0;\n" : ";\n");
      }
    }
    if (!function.pureVirtual) {
      deferLoweredMember(row);
    }

    const bool hasCallableTemplateFailure =
        (generic || genericOwner) &&
        loweredMemberCallableTemplateBodyExists(function, instances, true,
                                                genericOwner);
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
        struct EmittedFailureSignature {
          std::string spelling;
          std::vector<SemanticType> parameters;
          SemanticType returnType = SemanticType::Unknown;
        };
        std::vector<EmittedFailureSignature> emitted;
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
          const EmittedFailureSignature signature{
              .spelling =
                  loweredFunctionInstanceSpelling(row, function, instance),
              .parameters = *parameters,
              .returnType = instance->returnType};
          if (std::any_of(emitted.begin(), emitted.end(),
                          [&](const EmittedFailureSignature &candidate) {
                            return candidate.spelling == signature.spelling &&
                                   candidate.parameters ==
                                       signature.parameters &&
                                   candidate.returnType == signature.returnType;
                          })) {
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

  [[nodiscard]] LoweredFunctionDeclaration loweredConcreteFunctionDeclaration(
      const LoweredFunctionDeclaration &function,
      const MirFunctionInstance &instance) const {
    LoweredFunctionDeclaration concrete = function;
    concrete.returnType = instance.returnType;
    concrete.genericParameters.clear();
    concrete.parameters.clear();
    const std::optional<std::vector<SemanticType>> parameters =
        cppMirFlattenConcreteParameterTypes(instance.parameterTypes);
    if (!parameters) {
      throw std::logic_error(
          "concrete generic member has an unspellable parameter pack");
    }
    concrete.parameters.reserve(parameters->size());
    for (std::size_t index = 0; index < parameters->size(); ++index) {
      concrete.parameters.push_back(
          {.name = "__gti_mir_arg_" + std::to_string(index),
           .type = (*parameters)[index]});
    }
    return concrete;
  }

  void emitLoweredConcreteMemberFunctionDeclaration(
      const LoweredDeclaration &row, const LoweredFunctionDeclaration &function,
      const LoweredDeclaration &classRow,
      const MirClassInstance &ownerInstance) {
    emitLoweredBoundaryMarkers(row, function);
    if (function.definitionKind == MirDefinitionKind::RuntimeBinding ||
        function.intrinsic != IntrinsicKind::None) {
      return;
    }

    bool emitted = false;
    for (const MirFunctionInstance *instance :
         loweredFunctionInstances(function.id)) {
      if (instance->owner != ownerInstance.id) {
        continue;
      }
      const LoweredFunctionDeclaration concrete =
          loweredConcreteFunctionDeclaration(function, *instance);
      writeIndent();
      emitLoweredFunctionSignature(row, function, instance, false, true);
      output << (function.pureVirtual ? " = 0;\n" : ";\n");
      if (loweredFailureBodySupported(*instance) ||
          loweredVirtualFailureContractRetained(function.id)) {
        writeIndent();
        emitLoweredFunctionSignature(row, function, instance, true, true);
        output << (function.pureVirtual ? " = 0;\n" : ";\n");
      }
      emitLoweredStructuralOperatorAdapter(row, concrete, classRow);
      emitLoweredCallableAdapter(row, concrete, classRow);
      emitted = true;
    }
    if (emitted && !function.pureVirtual) {
      deferLoweredMember(row);
    }
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
        loweredConstructorInstanceIsNameable(instance)) {
      return std::nullopt;
    }
    auto cached = constructorTemplatePlans.find(instance.id);
    if (cached == constructorTemplatePlans.end()) {
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
      const MirBodyAddress address{.kind = MirBodyKind::Constructor,
                                   .owner = instance.id};
      cached = constructorTemplatePlans
                   .emplace(instance.id,
                            std::make_unique<TemplateBodyPlan>(
                                mir, address, constructor.id, std::move(rows),
                                debugInfo ? std::span<const LoweredBody>(
                                                loweredProgram.bodies())
                                          : std::span<const LoweredBody>{}))
                   .first;
    }
    const auto &prepared = *cached->second;
    if (prepared.declaration != constructor.id)
      throw std::logic_error(
          "constructor template plan changed declaration identity");
    const CppMirBodyEmitter emitter(mir, prepared.representations);
    const CppMirBodyEmissionPlan &plan = prepared.plan;
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

  [[nodiscard]] std::string
  loweredGenericOwnerDestructorBodyText(const MirDestructorInstance &instance,
                                        bool failure) const {
    if (!generalEmissionMap)
      throw std::logic_error("retained destructor has no representation map");
    auto cached = destructorTemplatePlans.find(instance.id);
    if (cached == destructorTemplatePlans.end()) {
      CppMirBodyEmissionMapRows rows{
          .types = generalEmissionMap->types(),
          .bodies = generalEmissionMap->bodies(),
          .symbols = generalEmissionMap->symbols(),
          .enums = generalEmissionMap->enums(),
          .containedConstructors = generalEmissionMap->containedConstructors(),
          .capabilities = generalEmissionMap->capabilities()};
      const auto overlays = cppMirApplyGenericOwnerDestructorTypeOverlays(
          rows, loweredProgram, instance);
      if (!overlays || *overlays == 0)
        throw std::logic_error(
            "retained destructor has no exact owner overlay");
      cached =
          destructorTemplatePlans
              .emplace(instance.id,
                       std::make_unique<TemplateBodyPlan>(
                           mir,
                           MirBodyAddress{MirBodyKind::Destructor, instance.id},
                           instance.owner, std::move(rows),
                           debugInfo ? std::span<const LoweredBody>(
                                           loweredProgram.bodies())
                                     : std::span<const LoweredBody>{}))
              .first;
    }
    const auto &prepared = *cached->second;
    if (prepared.declaration != instance.owner)
      throw std::logic_error("retained destructor changed owner identity");
    const CppMirBodyEmitter emitter(mir, prepared.representations);
    const auto text =
        failure ? emitter.emitFailureBodyText(prepared.plan,
                                              "retained-destructor-failure-v1",
                                              indentation)
                : emitter.emitBodyText(prepared.plan, "retained-destructor-v1",
                                       indentation);
    if (!text.emitted())
      throw std::logic_error("retained destructor has no admitted MIR body");
    return text.text;
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
        emitLoweredParameters(constructor.parameters, true, constructor.owner);
      } else {
        emitLoweredMirParameters(instance.parameterTypes);
      }
    }
    output << ", " << representation->stateSpelling
           << " __gti_mir_contained_state";
  }

  void emitLoweredContainedConstructorBody(
      const LoweredDeclaration &owner, const MirConstructorInstance &instance,
      const CppMirBodyEmissionPlan &plan, std::string_view concreteName = {}) {
    if (loweredContainedConstructorRepresentation(instance.id) == nullptr ||
        plan.body() != MirBodyAddress{.kind = MirBodyKind::Constructor,
                                      .owner = instance.id} ||
        !plan.supportsFailureText()) {
      throw std::logic_error(
          "contained constructor definition lost its admitted failure body");
    }
    const std::optional<std::vector<SemanticType>> parameters =
        cppMirFlattenConcreteParameterTypes(instance.parameterTypes);
    if (!parameters) {
      throw std::logic_error(
          "contained constructor has an unspellable parameter pack");
    }
    output << " : " << (concreteName.empty() ? owner.name : concreteName) << '('
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
    emitIncomingCleanupFailureCheck("&__gti_mir_contained_state.record");
    writeIndent();
    output << "::gti_rt_failure_terminate_v2(\n";
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
    const std::vector<const MirConstructorInstance *> instances =
        loweredConstructorInstances(constructor.id);
    const bool generic = !constructor.genericParameters.empty();
    const bool retainPrimary =
        !generic || instances.empty() ||
        std::any_of(instances.begin(), instances.end(),
                    [&](const MirConstructorInstance *instance) {
                      return !loweredConstructorInstanceIsNameable(*instance);
                    });
    if (generic && retainPrimary) {
      emitLoweredTemplateDeclaration(constructor.genericParameters);
    }
    if (retainPrimary) {
      writeIndent();
      output << "explicit " << classRow.name << '(';
      emitLoweredParameters(constructor.parameters, false, constructor.owner);
      output << ");\n";
    }
    if (generic) {
      std::unordered_set<std::string> declaredParameters;
      for (const MirConstructorInstance *instance : instances) {
        if (!loweredConstructorInstanceIsNameable(*instance)) {
          continue;
        }
        // A retained primary can represent several concrete owners with the
        // same native parameter list. Declare that ordinary overload once;
        // its owner-specific definitions and failure-tag overloads stay exact.
        std::ostringstream saved = std::move(output);
        output = std::ostringstream{};
        emitLoweredMirParameters(instance->parameterTypes);
        const std::string parameters = output.str();
        output = std::move(saved);
        if (!declaredParameters.insert(parameters).second)
          continue;
        writeIndent();
        output << "explicit " << classRow.name << '(' << parameters << ");\n";
      }
    }
    deferLoweredMember(row);
    for (const MirConstructorInstance *instance : instances) {
      const bool genericOwnerPrimary =
          loweredGenericOwnerConstructorBodyText(constructor, *instance, true)
              .has_value();
      const bool concreteFailure =
          generalMirBodyPlan(
              {.kind = MirBodyKind::Constructor, .owner = instance->id})
              .supportsFailureText();
      emitBaseGateDeclaration(*instance, classRow.name);
      if (!instance->mayRaiseDefinedFailure ||
          (!concreteFailure && !genericOwnerPrimary)) {
        continue;
      }
      if (genericOwnerPrimary)
        emitLoweredTemplateDeclaration(constructor.genericParameters);
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
        emitLoweredParameters(constructor.parameters, true, constructor.owner);
      } else {
        emitLoweredMirParameters(instance->parameterTypes);
      }
      output << ", ";
      output << "bool *__gti_mir_constructor_success, "
                "::gti_failure_record_v2 *__gti_mir_failure_record);\n";
      emitBasePreparationDeclaration(*instance, classRow.name);
      if (loweredContainedConstructorRepresentation(instance->id) != nullptr) {
        if (genericOwnerPrimary)
          emitLoweredTemplateDeclaration(constructor.genericParameters);
        writeIndent();
        output << "explicit " << classRow.name << '(';
        emitLoweredContainedConstructorParameters(constructor, *instance,
                                                  genericOwnerPrimary);
        output << ");\n";
      }
    }
  }

  void emitLoweredConcreteConstructorDeclaration(
      const LoweredDeclaration &row,
      const LoweredConstructorDeclaration &constructor,
      const LoweredDeclaration &classRow,
      const MirClassInstance &ownerInstance) {
    if (constructor.specifier) {
      return;
    }
    bool emitted = false;
    for (const MirConstructorInstance *instance :
         loweredConstructorInstances(constructor.id)) {
      if (instance->owner != ownerInstance.id ||
          instance->definitionKind != MirDefinitionKind::Source) {
        continue;
      }
      writeIndent();
      output << "explicit " << classRow.name << '(';
      emitLoweredMirParameters(instance->parameterTypes);
      output << ");\n";
      emitBaseGateDeclaration(*instance, classRow.name);
      const MirBodyAddress address{.kind = MirBodyKind::Constructor,
                                   .owner = instance->id};
      if (instance->mayRaiseDefinedFailure &&
          generalMirBodyPlan(address).supportsFailureText()) {
        writeIndent();
        output << "explicit " << classRow.name << '('
               << cppMirFailureConstructorTagSpelling(instance->id);
        if (!instance->parameterTypes.empty()) {
          output << ", ";
        }
        emitLoweredMirParameters(instance->parameterTypes);
        output << ", bool *__gti_mir_constructor_success, "
                  "::gti_failure_record_v2 *__gti_mir_failure_record);\n";
        emitBasePreparationDeclaration(*instance, classRow.name);
        if (loweredContainedConstructorRepresentation(instance->id) !=
            nullptr) {
          writeIndent();
          output << "explicit " << classRow.name << '(';
          emitLoweredContainedConstructorParameters(constructor, *instance,
                                                    false);
          output << ");\n";
        }
      }
      emitted = true;
    }
    if (emitted) {
      deferLoweredMember(row);
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
    deferLoweredMember(row);
  }

  void emitLoweredConcreteDestructorDeclaration(
      const LoweredDeclaration &row,
      const LoweredDestructorDeclaration &destructor,
      const LoweredDeclaration &classRow,
      const LoweredClassDeclaration &classDeclaration,
      const MirClassInstance &ownerInstance) {
    if (destructor.specifier || !ownerInstance.destructor) {
      return;
    }
    writeIndent();
    if (classDeclaration.polymorphic) {
      output << "virtual ";
    }
    output << '~' << classRow.name << "() noexcept;\n";
    deferLoweredMember(row);
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
      const LoweredClassDeclaration &declaration,
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
        output << fields[index]->name
               << "(::gti_internal::backend::mir_empty_state_v1<"
                  "std::remove_cv_t<decltype("
               << fields[index]->name << ")>>())";
      }
    }
    output << " {";
    if (declaration.requiresActiveDropState)
      output << ' ' << loweredLifecycleActiveName(declaration.id)
             << " = false;";
    output << " }\n";
  }

  void emitLoweredMoveConstructor(
      const LoweredDeclaration &classRow,
      const LoweredClassDeclaration &declaration,
      const std::vector<const LoweredDeclaration *> &fields,
      const std::set<std::string> &byteFields,
      const std::set<std::string> &referenceFields) {
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
      output << field->name;
      if (byteFields.contains(field->name))
        output << "{}";
      else if (referenceFields.contains(field->name))
        // Move the binding, not the object it designates. std::move on a
        // native reference member casts its referent to an rvalue instead.
        output << "(other." << field->name << ')';
      else
        output << "(std::move(other." << field->name << "))";
      separator = true;
    }
    if (declaration.requiresActiveDropState) {
      if (separator)
        output << ", ";
      // Transfer the representation's lifecycle state, not just its fields.
      // A private empty-state shell is deliberately disarmed; relocating it
      // must not manufacture an observable GTI destructor obligation.
      output << loweredLifecycleActiveName(declaration.id) << "(other."
             << loweredLifecycleActiveName(declaration.id) << ")";
    }
    output << " {\n";
    ++indentation;
    for (const auto &field : byteFields) {
      writeIndent();
      output << "std::memcpy(std::addressof(" << field
             << "), std::addressof(other." << field << "), sizeof(" << field
             << "));\n";
    }
    if (declaration.requiresActiveDropState) {
      writeIndent();
      output << "other." << loweredLifecycleActiveName(declaration.id)
             << " = false;\n";
    }
    --indentation;
    writeIndent();
    output << "}\n";
  }

  void emitLoweredMoveAssignment(
      const LoweredDeclaration &classRow,
      const LoweredClassDeclaration &declaration,
      const std::vector<const LoweredDeclaration *> &fields,
      const std::set<std::string> &byteFields = {}) {
    writeIndent();
    output << classRow.name << " &operator=(" << classRow.name
           << " &&other) noexcept {\n";
    ++indentation;
    writeIndent();
    output << "if (this != &other) {\n";
    ++indentation;
    if (declaration.requiresActiveDropState) {
      writeIndent();
      output << loweredLifecycleCleanupName(declaration.id) << "();\n";
    }
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
      if (byteFields.contains(field->name))
        output << "std::memcpy(std::addressof(" << field->name
               << "), std::addressof(other." << field->name << "), sizeof("
               << field->name << "));\n";
      else
        output << field->name << " = std::move(other." << field->name << ");\n";
    }
    if (declaration.requiresActiveDropState) {
      writeIndent();
      output << loweredLifecycleActiveName(declaration.id) << " = other."
             << loweredLifecycleActiveName(declaration.id) << ";\n";
      writeIndent();
      output << "other." << loweredLifecycleActiveName(declaration.id)
             << " = false;\n";
    }
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
                                 const LoweredClassDeclaration &declaration,
                                 const MirClassInstance *concreteInstance) {
    const std::vector<const LoweredDeclaration *> fields =
        loweredClassFields(classRow);
    // Only passive native-layout subobjects need byte construction here.
    // Ordinary containing classes have their own fieldwise special members;
    // owning fields must never be copied wholesale merely for containing one.
    std::set<std::string> byteFields;
    std::set<std::string> referenceFields;
    for (const auto *field : fields) {
      const auto &storage = std::get<LoweredStorageDeclaration>(field->payload);
      SemanticType type = storage.type;
      if (concreteInstance) {
        for (const auto &concrete : concreteInstance->declaredFields)
          if (concrete.symbol == storage.symbol)
            type = concrete.type;
      }
      if (type.kind == SemanticType::Reference)
        referenceFields.insert(field->name);
      SemanticType element = type;
      while (element.kind == SemanticType::Array &&
             element.arguments.size() == 1) {
        // Copy the argument out before assigning: it lives inside the
        // vector that the assignment destroys (ASan container-overflow).
        SemanticType inner = element.arguments.front();
        element = std::move(inner);
      }
      const auto *native =
          element.kind == SemanticType::Class
              ? loweredProgram.findClassDeclaration(element.classId)
              : nullptr;
      if (native && native->cAbiRecord &&
          cppRequiresBytePreservingTransport(mir, type))
        byteFields.insert(field->name);
    }
    const bool incompletePointeeCarrier = std::any_of(
        fields.begin(), fields.end(), [](const LoweredDeclaration *field) {
          const auto *storage =
              std::get_if<LoweredStorageDeclaration>(&field->payload);
          return storage != nullptr &&
                 storage->type.kind == SemanticType::UniqueOwner;
        });
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
        output
            << "friend bool "
               "::gti_internal::backend::mir_unique_owner_drop_with_failure_v1<"
            << classRow.name << ">(std::unique_ptr<" << classRow.name
            << "> &, ::gti_failure_record_v2 *);\n";
        writeIndent();
        output << "bool " << lifecycleFailureDispatchName()
               << "(::gti_failure_record_v2 *);\n";
        writeIndent();
        output << "bool " << lifecycleFailureBodyName()
               << "(::gti_failure_record_v2 *);\n";
      }
      loweredDeferredLifecycleClasses.push_back(classRow);
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
    std::vector<const MirConstructorInstance *> generatedConstructors;
    for (const LoweredConstructorInstance &candidate :
         loweredProgram.constructorInstances()) {
      const MirClassInstance *owner = mir.findClassInstance(candidate.owner);
      if (candidate.declaration == 0 && owner != nullptr &&
          owner->declaration == declaration.id &&
          (concreteInstance != nullptr
               ? owner->id == concreteInstance->id
               : !loweredClassUsesDirectRepresentation(*owner))) {
        if (concreteInstance != nullptr && !generatedConstructors.empty())
          throw std::logic_error(
              "generated constructor needs one exact concrete owner");
        const auto *generated = mir.findConstructorInstance(candidate.id);
        if (generated == nullptr)
          throw std::logic_error("generated constructor lost its MIR body");
        generatedConstructors.push_back(generated);
      }
    }
    if (!generatedConstructors.empty()) {
      // A native primary template can serve several concrete MIR owners.
      // Declare its ordinary default once and each exact status overload;
      // the out-of-line specializations remain the executable authority.
      if (std::any_of(
              generatedConstructors.begin(), generatedConstructors.end(),
              [&](const auto *constructor) {
                return generalMirBodyPlan({.kind = MirBodyKind::Constructor,
                                           .owner = constructor->id})
                    .supportsPlainText();
              })) {
        writeIndent();
        output << classRow.name << "();\n";
      }
      for (const auto *constructor : generatedConstructors) {
        if (!generalMirBodyPlan(
                 {.kind = MirBodyKind::Constructor, .owner = constructor->id})
                 .supportsFailureText())
          continue;
        writeIndent();
        output << classRow.name << '(';
        emitLoweredConstructorParameters(*constructor, true);
        output << ");\n";
      }
    } else if (declaration.defaultConstructor !=
               SpecialMemberStatus::Declared) {
      writeIndent();
      output << classRow.name << "()";
      if (incompletePointeeCarrier &&
          declaration.defaultConstructor == SpecialMemberStatus::Generated)
        output << " noexcept {}\n";
      else
        emitStatus(declaration.defaultConstructor);
    }
    if (loweredClassNeedsEmptyState(declaration)) {
      emitLoweredEmptyStateConstructor(classRow, declaration, fields);
    }
    writeIndent();
    output << classRow.name << "(const " << classRow.name << " &other)";
    if (!byteFields.empty() &&
        declaration.copyConstructor == SpecialMemberStatus::Generated) {
      bool separator = false;
      for (const auto &base : declaration.bases) {
        if (base.interface)
          continue;
        output << (separator ? ", " : " : ");
        emitSemanticType(base.type);
        output << "(other)";
        separator = true;
      }
      for (const auto *field : fields) {
        output << (separator ? ", " : " : ") << field->name;
        if (byteFields.contains(field->name))
          output << "{}";
        else
          output << "(other." << field->name << ')';
        separator = true;
      }
      if (declaration.requiresActiveDropState)
        output << (separator ? ", " : " : ")
               << loweredLifecycleActiveName(declaration.id) << "(other."
               << loweredLifecycleActiveName(declaration.id) << ')';
      output << " {\n";
      ++indentation;
      for (const auto &field : byteFields) {
        writeIndent();
        output << "std::memcpy(std::addressof(" << field
               << "), std::addressof(other." << field << "), sizeof(" << field
               << "));\n";
      }
      --indentation;
      writeIndent();
      output << "}\n";
    } else
      emitStatus(declaration.copyConstructor);
    if ((declaration.requiresActiveDropState || incompletePointeeCarrier ||
         !byteFields.empty()) &&
        declaration.moveConstructor == SpecialMemberStatus::Generated) {
      emitLoweredMoveConstructor(classRow, declaration, fields, byteFields,
                                 referenceFields);
    } else {
      writeIndent();
      output << classRow.name << '(' << classRow.name << " &&)";
      emitStatus(declaration.moveConstructor);
    }
    writeIndent();
    output << classRow.name << " &operator=(const " << classRow.name
           << " &other)";
    if (!byteFields.empty() &&
        declaration.copyAssignment == SpecialMemberStatus::Generated) {
      output << " {\n";
      ++indentation;
      writeIndent();
      output << "if (this != &other) {\n";
      ++indentation;
      for (const auto &base : declaration.bases) {
        if (base.interface)
          continue;
        writeIndent();
        emitSemanticType(base.type);
        output << "::operator=(other);\n";
      }
      for (const auto *field : fields) {
        writeIndent();
        if (byteFields.contains(field->name))
          output << "std::memcpy(std::addressof(" << field->name
                 << "), std::addressof(other." << field->name << "), sizeof("
                 << field->name << "));\n";
        else
          output << field->name << " = other." << field->name << ";\n";
      }
      if (declaration.requiresActiveDropState) {
        writeIndent();
        output << loweredLifecycleActiveName(declaration.id) << " = other."
               << loweredLifecycleActiveName(declaration.id) << ";\n";
      }
      --indentation;
      writeIndent();
      output << "}\n";
      writeIndent();
      output << "return *this;\n";
      --indentation;
      writeIndent();
      output << "}\n";
    } else
      emitStatus(declaration.copyAssignment);
    if ((declaration.requiresActiveDropState || incompletePointeeCarrier ||
         !byteFields.empty()) &&
        declaration.moveAssignment == SpecialMemberStatus::Generated) {
      emitLoweredMoveAssignment(classRow, declaration, fields, byteFields);
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
      if (declaration.destructor == SpecialMemberStatus::Generated &&
          incompletePointeeCarrier) {
        // Keep this representation destructor non-constexpr. C++23's
        // constexpr unique_ptr destructor otherwise forces the dependent
        // deleter while mutually recursive pointees are still incomplete.
        // The empty body preserves the same native member cleanup order.
        output << " {}\n";
      } else {
        emitStatus(declaration.destructor);
      }
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
      if (!parameter.typeArgumentPath.empty()) {
        if (auto type = projectGenericTypeArgument(
                concreteType.arguments, parameter.typeArgumentPath,
                parameter.id, parameter.pack)) {
          substitution.types.emplace(parameter.id, std::move(*type));
        }
      } else if (parameter.value) {
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
      const LoweredClassDeclaration &declaration, AccessModifier originalAccess,
      const MirClassInstance *concreteInstance = nullptr) {
    const SemanticType exactType =
        concreteInstance != nullptr ? concreteInstance->type
                                    : SemanticType::classType(declaration.id);
    if (std::find(loweredConstexprAggregateClasses.begin(),
                  loweredConstexprAggregateClasses.end(),
                  exactType) == loweredConstexprAggregateClasses.end()) {
      return;
    }
    const std::vector<const LoweredDeclaration *> fields =
        loweredConstexprAggregateFields(classRow);
    const auto fieldType = [&](const LoweredDeclaration &field) {
      const auto &storage = std::get<LoweredStorageDeclaration>(field.payload);
      if (concreteInstance == nullptr) {
        return storage.type;
      }
      const auto found =
          std::find_if(concreteInstance->declaredFields.begin(),
                       concreteInstance->declaredFields.end(),
                       [&](const MirClassFieldInfo &candidate) {
                         return candidate.symbol == storage.symbol;
                       });
      return found == concreteInstance->declaredFields.end() ? storage.type
                                                             : found->type;
    };
    const auto emitAccess = [&](AccessModifier access) {
      if (indentation > 0) {
        --indentation;
      }
      writeIndent();
      output << accessModifierSpelling(access) << ":\n";
      ++indentation;
    };
    emitAccess(AccessModifier::Private);
    writeIndent();
    output << "struct __gti_constexpr_value_tag_v1 {};\n";
    writeIndent();
    const bool nativeConstexpr = nativeConstantStorage(exactType);
    if (nativeConstexpr)
      output << "constexpr ";
    output << classRow.name << "(__gti_constexpr_value_tag_v1";
    for (std::size_t index = 0; index < fields.size(); ++index) {
      output << ", ";
      emitSemanticType(fieldType(*fields[index]));
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
    output << (nativeConstexpr ? "static constexpr " : "static ")
           << classRow.name << " __gti_constexpr_value_v1(";
    for (std::size_t index = 0; index < fields.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      emitSemanticType(fieldType(*fields[index]));
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

  void emitUniqueCleanupMember(const LoweredDeclaration &classRow,
                               const LoweredClassDeclaration &declaration,
                               const MirClassInstance *concrete) {
    std::vector<MirDropType> descriptors;
    for (const auto &instance : mir.classInstances()) {
      if (instance.declaration != declaration.id ||
          (concrete != nullptr && concrete->id != instance.id))
        continue;
      if (auto descriptor = mirUniqueCleanupType(mir, instance.type))
        descriptors.push_back(*descriptor);
    }
    if (descriptors.empty())
      return;
    writeIndent();
    output << "public:\n";
    writeIndent();
    output << "template <std::size_t __gti_cleanup_instance>\n";
    writeIndent();
    output << "bool __gti_mir_unique_cleanup_failure(::gti_failure_record_v2 "
              "*record, bool unwinding) {\n";
    ++indentation;
    bool first = true;
    for (const auto &descriptor : descriptors) {
      writeIndent();
      output << (first ? "if" : "else if")
             << " constexpr (__gti_cleanup_instance == "
             << *descriptor.classInstance << ") {\n";
      first = false;
      ++indentation;
      writeIndent();
      output << "::gti_internal::backend::mir_cleanup_sequence_v1 sequence("
                "record, unwinding, &::gti_internal::backend::"
                "__gti_failure_artifact_descriptor_v1);\n";
      if (descriptor.destructor) {
        const auto *body = mir.findDestructorInstance(*descriptor.destructor);
        if (body == nullptr)
          throw std::logic_error("MIR cleanup lost its source destructor");
        writeIndent();
        if (body->mayRaiseDefinedFailure) {
          output << "if (!sequence.step(" << lifecycleFailureDispatchName()
                 << "(sequence.record()))) return false;\n";
        } else {
          output << loweredLifecycleCleanupName(declaration.id) << "();\n";
        }
      }
      for (const auto &step : descriptor.aggregate) {
        std::string receiver;
        if (step.uniqueOwnerField != 0) {
          for (const auto *field : loweredClassFields(classRow)) {
            const auto *storage =
                std::get_if<LoweredStorageDeclaration>(&field->payload);
            if (storage != nullptr &&
                storage->symbol == step.uniqueOwnerField) {
              receiver = "this->" + field->name;
              break;
            }
          }
        } else if (step.uniqueOwnerBase) {
          const auto *base = mir.findClassInstance(*step.uniqueOwnerBase);
          if (base != nullptr) {
            receiver = "(*this)." + loweredConcreteClassName(base->id) + "::";
          }
        }
        if (receiver.empty())
          throw std::logic_error(
              "unique cleanup lost its MIR-selected subobject");
        writeIndent();
        output << "if (!sequence.step(";
        if (step.uniqueOwnerBase) {
          if (!step.classInstance)
            throw std::logic_error(
                "unique cleanup lost its next aggregate step");
          output << receiver << "template __gti_mir_unique_cleanup_failure<"
                 << *step.classInstance
                 << ">(sequence.record(), sequence.unwinding())";
        } else {
          output << cppMirCleanupValueExpression(
              mir, step, receiver, "sequence.record()", "sequence.unwinding()");
        }
        output << ")) return false;\n";
      }
      writeIndent();
      output << "return sequence.success();\n";
      --indentation;
      writeIndent();
      output << "}\n";
    }
    writeIndent();
    output << "else { static_assert(__gti_cleanup_instance != "
              "__gti_cleanup_instance, \"unknown MIR cleanup instance\"); }\n";
    --indentation;
    writeIndent();
    output << "}\n";
  }

  void emitPolymorphicCleanupMember(const LoweredClassDeclaration &declaration,
                                    const MirClassInstance *concrete) {
    if (!declaration.polymorphic)
      return;
    writeIndent();
    output << "private:\n";
    writeIndent();
    output << "::gti_internal::backend::mir_polymorphic_cleanup_binding_v1 "
              "__gti_mir_dynamic_cleanup;\n";
    writeIndent();
    output << "public:\n";
    writeIndent();
    output << "template <std::size_t __gti_cleanup_instance>\n";
    writeIndent();
    output << "void __gti_mir_bind_polymorphic_cleanup() noexcept {\n";
    ++indentation;
    writeIndent();
    output << "using __gti_cleanup_self = "
              "std::remove_pointer_t<decltype(this)>;\n";
    bool first = true;
    for (const auto &instance : mir.classInstances()) {
      if (instance.declaration != declaration.id ||
          (concrete != nullptr && concrete->id != instance.id))
        continue;
      const auto cleanup = mirUniqueCleanupType(mir, instance.type);
      if (!cleanup && instance.requiresActiveCleanup)
        throw std::logic_error(
            "polymorphic allocation lost its complete MIR cleanup");
      writeIndent();
      output << (first ? "if" : "else if")
             << " constexpr (__gti_cleanup_instance == " << instance.id
             << ") {\n";
      first = false;
      ++indentation;
      writeIndent();
      if (cleanup) {
        output << "__gti_mir_dynamic_cleanup.bind([](void *object, "
                  "::gti_failure_record_v2 *record, bool unwinding) { return "
                  "static_cast<__gti_cleanup_self *>(object)->template "
                  "__gti_mir_unique_cleanup_failure<__gti_cleanup_instance>("
                  "record, "
                  "unwinding); });\n";
      } else {
        output << "__gti_mir_dynamic_cleanup.bind([](void *, "
                  "::gti_failure_record_v2 *, bool) { return true; });\n";
      }
      --indentation;
      writeIndent();
      output << "}\n";
    }
    writeIndent();
    output
        << (first ? "" : "else ")
        << "{ static_assert(__gti_cleanup_instance != __gti_cleanup_instance, "
           "\"unknown MIR polymorphic cleanup instance\"); }\n";
    --indentation;
    writeIndent();
    output << "}\n";
    writeIndent();
    output << "virtual bool __gti_mir_polymorphic_cleanup_failure("
              "::gti_failure_record_v2 *record, bool unwinding)";
    if (std::any_of(declaration.bases.begin(), declaration.bases.end(),
                    [&](const LoweredClassBase &base) {
                      const auto *parent = loweredProgram.findClassDeclaration(
                          base.type.classId);
                      return parent != nullptr && parent->polymorphic;
                    }))
      output << " override";
    output << " {\n";
    ++indentation;
    writeIndent();
    output
        << "return __gti_mir_dynamic_cleanup.run(this, record, unwinding);\n";
    --indentation;
    writeIndent();
    output << "}\n";
  }

  void emitLoweredLexicalClassFriends(ClassId owner) {
    const auto found = lexicalClassChildren.find(owner);
    if (found == lexicalClassChildren.end())
      return;
    for (const auto *row : found->second) {
      const auto &child = std::get<LoweredClassDeclaration>(row->payload);
      if (!child.genericParameters.empty() &&
          !loweredClassNeedsNativePrimary(child)) {
        for (const auto *instance : loweredConcreteClassInstances(child.id)) {
          writeIndent();
          output << "friend "
                 << cppSemanticTypeSpelling(loweredProgram, standard,
                                            instance->type)
                 << ";\n";
        }
      } else {
        emitLoweredTemplateDeclaration(child.genericParameters);
        writeIndent();
        output << "friend " << (child.genericParameters.empty() ? "" : "class ")
               << cppClassTemplateNameSpelling(loweredProgram, child.id)
               << ";\n";
      }
      emitLoweredLexicalClassFriends(child.id);
    }
  }

  void emitLoweredClassDefinitionBody(
      const LoweredDeclaration &classRow,
      const LoweredClassDeclaration &declaration,
      const MirClassInstance *concreteInstance = nullptr) {
    loweredEmittedTypes.insert(classRow.id);
    const std::optional<LoweredClassInitializerSelection> initializers =
        concreteInstance == nullptr
            ? loweredClassInitializers(declaration)
            : loweredClassInitializers(*concreteInstance);
    const bool hasInstance =
        concreteInstance != nullptr ||
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

    if (concreteInstance != nullptr) {
      // A concrete GTI class is an ordinary native class. All substitution and
      // exact selection have already happened in semantics/HIR/MIR.
    } else if (declaration.exactSpecializationPrimary != 0) {
      writeIndent();
      output << "template <>\n";
    } else {
      emitLoweredTemplateDeclaration(declaration.genericParameters);
    }
    writeIndent();
    output << (declaration.kind == ClassKind::Struct  ? "struct "
               : declaration.kind == ClassKind::Union ? "union "
                                                      : "class ")
           << classRow.name << loweredDeclaredClassArguments(declaration);
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
    // Preserve semantic-authorized lexical access without requiring native
    // physical nesting. Friendship carries no GTI permission or object state.
    emitLoweredLexicalClassFriends(declaration.id);
    AccessModifier access = declaration.kind == ClassKind::Class
                                ? AccessModifier::Private
                                : AccessModifier::Public;
    if (declaration.kind == ClassKind::Interface ||
        (declaration.reflectPrivate && access == AccessModifier::Private)) {
      writeIndent();
      output << "public:\n";
      access = AccessModifier::Public;
    }
    emitLoweredConstexprAggregateFactory(classRow, declaration, access,
                                         concreteInstance);
    for (const LoweredDeclaration *member : loweredChildren(classRow.id)) {
      if (const auto *value =
              std::get_if<LoweredAccessDeclaration>(&member->payload)) {
        access =
            declaration.reflectPrivate ? AccessModifier::Public : value->access;
        if (indentation > 0) {
          --indentation;
        }
        writeIndent();
        output << accessModifierSpelling(access) << ":\n";
        ++indentation;
        continue;
      }
      static_cast<void>(access);
      if (const auto *storage =
              std::get_if<LoweredStorageDeclaration>(&member->payload)) {
        LoweredStorageDeclaration concreteStorage = *storage;
        if (concreteInstance != nullptr && !storage->staticStorage) {
          const auto field =
              std::find_if(concreteInstance->declaredFields.begin(),
                           concreteInstance->declaredFields.end(),
                           [&](const MirClassFieldInfo &candidate) {
                             return candidate.symbol == storage->symbol;
                           });
          if (field == concreteInstance->declaredFields.end()) {
            throw std::logic_error(
                "concrete generic class lost a declared field type");
          }
          concreteStorage.type = field->type;
        }
        emitLoweredStorageDeclaration(*member, concreteStorage, true,
                                      &declaration,
                                      initializers ? &*initializers : nullptr);
      } else if (const auto *function = std::get_if<LoweredFunctionDeclaration>(
                     &member->payload)) {
        if (concreteInstance == nullptr) {
          emitLoweredMemberFunctionDeclaration(*member, *function, classRow);
        } else {
          emitLoweredConcreteMemberFunctionDeclaration(
              *member, *function, classRow, *concreteInstance);
        }
      } else if (const auto *constructor =
                     std::get_if<LoweredConstructorDeclaration>(
                         &member->payload)) {
        // Verified GTI construction uses lifetime-slot helpers outside this
        // native class. Semantic access remains authoritative; C++ access
        // must not reject an already-authorized constructor in those helpers.
        if (access != AccessModifier::Public) {
          writeIndent();
          output << "public:\n";
        }
        if (concreteInstance == nullptr) {
          emitLoweredConstructorDeclaration(*member, *constructor, classRow);
        } else {
          emitLoweredConcreteConstructorDeclaration(
              *member, *constructor, classRow, *concreteInstance);
        }
        if (access != AccessModifier::Public) {
          writeIndent();
          output << accessModifierSpelling(access) << ":\n";
        }
      } else if (const auto *destructor =
                     std::get_if<LoweredDestructorDeclaration>(
                         &member->payload)) {
        if (concreteInstance == nullptr) {
          emitLoweredDestructorDeclaration(*member, *destructor, classRow,
                                           declaration);
        } else {
          emitLoweredConcreteDestructorDeclaration(
              *member, *destructor, classRow, declaration, *concreteInstance);
        }
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
        SemanticType aliasType = alias->type;
        if (concreteInstance != nullptr) {
          const auto concrete = std::find_if(
              concreteInstance->typeAliases.begin(),
              concreteInstance->typeAliases.end(), [&](const auto &fact) {
                return fact.declaration == member->semanticIdentity;
              });
          if (concrete == concreteInstance->typeAliases.end()) {
            throw std::logic_error(
                "concrete class type alias lost its verified exact type");
          }
          aliasType = concrete->type;
        }
        emitSemanticType(aliasType);
        output << ";\n";
      } else if (member->kind == LoweredDeclarationKind::Empty) {
        writeIndent();
        output << ";\n";
      }
    }
    if (!declaration.cAbiRecord && declaration.kind != ClassKind::Union) {
      emitLoweredClassLifecycle(classRow, declaration, concreteInstance);
      emitUniqueCleanupMember(classRow, declaration, concreteInstance);
      emitPolymorphicCleanupMember(declaration, concreteInstance);
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

  void emitLoweredClassDefinition(const LoweredDeclaration &classRow,
                                  const LoweredClassDeclaration &declaration) {
    const bool genericFamily = !declaration.genericParameters.empty() ||
                               declaration.exactSpecializationPrimary != 0;
    if (!genericFamily) {
      emitLoweredClassDefinitionBody(classRow, declaration);
      return;
    }

    if (declaration.exactSpecializationPrimary == 0 &&
        loweredClassNeedsNativePrimary(declaration)) {
      emitLoweredClassDefinitionBody(classRow, declaration);
    } else if (declaration.exactSpecializationPrimary != 0 &&
               loweredConcreteClassInstances(declaration.id).empty()) {
      // A retained native template carrier needs the definition of its
      // already-selected exact specialization, not only a forward declaration.
      // Direct concrete instances are emitted by their separate schedule.
      emitLoweredClassDefinitionBody(classRow, declaration);
    }
  }

  void
  emitLoweredConcreteClassDefinition(const LoweredDeclaration &classRow,
                                     const LoweredClassDeclaration &declaration,
                                     const MirClassInstance &instance) {
    const std::optional<std::string> name =
        cppConcreteClassName(loweredProgram, instance.type);
    if (!name) {
      throw std::logic_error(
          "nameable concrete class lost its stable representation");
    }
    LoweredDeclaration concreteRow = classRow;
    concreteRow.name = *name;
    LoweredClassDeclaration concrete = declaration;
    concrete.exactSpecializationPrimary = 0;
    concrete.exactTypeArguments.clear();
    concrete.exactValueArguments.clear();
    concrete.genericParameters.clear();
    concrete.genericArgumentPatterns.clear();
    concrete.traits = instance.traits;
    concrete.defaultConstructor = instance.defaultConstructor;
    concrete.generatedDefaultRequiresSchedule =
        instance.generatedDefaultRequiresSchedule;
    concrete.copyConstructor = instance.copyConstructor;
    concrete.moveConstructor = instance.moveConstructor;
    concrete.copyAssignment = instance.copyAssignment;
    concrete.moveAssignment = instance.moveAssignment;
    concrete.destructor = instance.destructorStatus;
    concrete.requiresActiveDropState = instance.requiresActiveDropState;
    for (std::size_t index = 0;
         index < concrete.bases.size() && index < instance.bases.size();
         ++index) {
      concrete.bases[index].type = instance.bases[index].type;
    }
    concreteRow.payload = concrete;
    loweredConcreteLifecycleInstances.insert_or_assign(*name, instance.id);
    emitLoweredClassDefinitionBody(
        concreteRow, std::get<LoweredClassDeclaration>(concreteRow.payload),
        &instance);
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

  void emitLoweredScheduledConcreteTypeDefinition(
      const LoweredDeclaration &row, const LoweredClassDeclaration &declaration,
      const MirClassInstance &instance) {
    const std::size_t namespaceCount =
        emitLoweredNamespaceOpen(row.namespaceScope);
    emitLoweredConcreteClassDefinition(row, declaration, instance);
    emitLoweredNamespaceClose(namespaceCount);
    output << '\n';
  }

  void emitLoweredTypeDefinitions() {
    struct ClassEmissionNode {
      const LoweredDeclaration *row = nullptr;
      const MirClassInstance *concrete = nullptr;
    };

    std::vector<const LoweredDeclaration *> enumerations;
    std::vector<ClassEmissionNode> classes;
    std::unordered_map<ClassId, std::size_t> sourceClassSchedule;
    for (const LoweredDeclaration &sourceRow : loweredProgram.declarations()) {
      const auto *sourceClass =
          std::get_if<LoweredClassDeclaration>(&sourceRow.payload);
      const LoweredDeclaration &row = sourceClass == nullptr
                                          ? sourceRow
                                          : *loweredClassRow(sourceClass->id);
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
      classes.push_back({.row = &row});
      if (!type->forwardDeclaration &&
          type->compilerCapability == CompilerCapabilityTypeKind::None &&
          !type->cOpaqueHandle) {
        sourceClassSchedule.insert_or_assign(type->id, index);
      }
    }

    std::unordered_map<std::string, std::size_t> concreteClassSchedule;
    for (const MirClassInstance &instance : mir.classInstances()) {
      const std::optional<std::string> name =
          cppConcreteClassName(loweredProgram, instance.type);
      const LoweredDeclaration *row = loweredClassRow(instance.declaration);
      const auto *declaration =
          row == nullptr ? nullptr
                         : std::get_if<LoweredClassDeclaration>(&row->payload);
      const LoweredDeclaration *parent =
          row == nullptr ? nullptr
                         : loweredProgram.findDeclaration(row->parent);
      if (!name || row == nullptr || declaration == nullptr ||
          declaration->forwardDeclaration ||
          declaration->compilerCapability != CompilerCapabilityTypeKind::None ||
          declaration->cOpaqueHandle ||
          (parent != nullptr &&
           std::holds_alternative<LoweredClassDeclaration>(parent->payload))) {
        continue;
      }
      if (concreteClassSchedule.contains(*name)) {
        throw std::logic_error(
            "lowered concrete class schedule contains a duplicate stable "
            "representation");
      }
      const std::size_t index = classes.size();
      classes.push_back({.row = row, .concrete = &instance});
      concreteClassSchedule.emplace(*name, index);
    }

    std::unordered_map<EnumId, const LoweredDeclaration *>
        sourceEnumerationSchedule;
    for (const LoweredDeclaration *enumeration : enumerations) {
      const auto *declaration =
          std::get_if<LoweredEnumDeclaration>(&enumeration->payload);
      if (declaration == nullptr || declaration->id == 0 ||
          !sourceEnumerationSchedule.emplace(enumeration->id, enumeration)
               .second) {
        throw std::logic_error(
            "lowered enum schedule contains a duplicate or invalid identity");
      }
    }

    struct TypeDependencies {
      std::vector<std::size_t> classes;
      std::vector<EnumId> enumerations;
    };

    std::unordered_set<HirClassInstanceId> visitingLayoutArguments;
    const auto collectTypeDependencies = [&](const auto &self,
                                             const SemanticType &type,
                                             TypeDependencies &result) -> void {
      switch (type.kind) {
      case SemanticType::RawPointer:
      case SemanticType::Reference:
      case SemanticType::UniqueOwner:
      case SemanticType::SharedPointer:
        return;
      case SemanticType::Class: {
        // These declaration-bound capabilities embed their exact value even
        // though they have no source fields. Payload layout must precede every
        // containing class, unlike pointer-backed storage.
        const auto capability = std::find_if(
            mir.classInstances().begin(), mir.classInstances().end(),
            [&](const MirClassInstance &candidate) {
              return candidate.type == type &&
                     (candidate.compilerCapability ==
                          CompilerCapabilityTypeKind::OptionalStorage ||
                      candidate.compilerCapability ==
                          CompilerCapabilityTypeKind::UninitializedStorage ||
                      candidate.compilerCapability ==
                          CompilerCapabilityTypeKind::MutexStorage);
            });
        if (capability != mir.classInstances().end()) {
          for (const SemanticType &argument : type.arguments) {
            self(self, argument, result);
          }
          return;
        }
        if (const std::optional<std::string> concrete =
                cppConcreteClassName(loweredProgram, type)) {
          const auto found = concreteClassSchedule.find(*concrete);
          if (found == concreteClassSchedule.end()) {
            throw std::logic_error(
                "lowered class layout lost an exact concrete dependency");
          }
          result.classes.push_back(found->second);
          return;
        }
        if (const auto found = sourceClassSchedule.find(type.classId);
            found != sourceClassSchedule.end()) {
          result.classes.push_back(found->second);
        }
        if (type.arguments.empty())
          return;
        // A generic argument is not necessarily stored by value. Inspect the
        // exact MIR instance's representation-bearing fields/bases rather
        // than inventing T layout dependencies for pointer-only wrappers.
        // Otherwise two classes owning one another through unique_ptr form
        // a false native layout cycle even though each layout is finite.
        const MirClassInstance *instance = nullptr;
        for (const auto &candidate : mir.classInstances()) {
          if (candidate.type == type) {
            instance = &candidate;
            break;
          }
        }
        if (instance == nullptr) {
          // Symbolic source-template spellings have no concrete field graph.
          for (const auto &argument : type.arguments)
            self(self, argument, result);
          return;
        }
        if (!visitingLayoutArguments.insert(instance->id).second)
          return;
        for (const auto &base : instance->bases)
          self(self, base.type, result);
        for (const auto &field : instance->declaredFields)
          self(self, field.type, result);
        visitingLayoutArguments.erase(instance->id);
        return;
      }
      case SemanticType::Enum:
        for (const auto &[identity, row] : sourceEnumerationSchedule) {
          const auto *enumeration =
              std::get_if<LoweredEnumDeclaration>(&row->payload);
          if (enumeration != nullptr && enumeration->type == type) {
            result.enumerations.push_back(identity);
            break;
          }
        }
        return;
      case SemanticType::Array:
      case SemanticType::Expected:
      case SemanticType::Unexpected:
        for (const SemanticType &argument : type.arguments) {
          self(self, argument, result);
        }
        return;
      default:
        return;
      }
    };

    const auto collectInitializerDependencies = [&](const MirBody &body,
                                                    TypeDependencies &result) {
      for (const MirPlace &place : body.places) {
        collectTypeDependencies(collectTypeDependencies, place.type, result);
      }
      for (const MirValue &value : body.values) {
        collectTypeDependencies(collectTypeDependencies, value.info.type,
                                result);
      }
      for (const MirBlock &block : body.blocks) {
        for (const MirInstruction &instruction : block.instructions) {
          collectTypeDependencies(collectTypeDependencies,
                                  instruction.info.type, result);
          collectTypeDependencies(collectTypeDependencies,
                                  instruction.dispatchOwner, result);
          for (const SemanticType &parameter : instruction.parameterTypes) {
            collectTypeDependencies(collectTypeDependencies, parameter, result);
          }
          for (const SemanticType &capture : instruction.closureCaptureTypes) {
            collectTypeDependencies(collectTypeDependencies, capture, result);
          }
          if (instruction.receiver) {
            collectTypeDependencies(collectTypeDependencies,
                                    instruction.receiver->type, result);
          }
          for (const MirOperand &operand : instruction.operands) {
            collectTypeDependencies(collectTypeDependencies, operand.type,
                                    result);
          }
          if (instruction.functionTarget) {
            const MirFunctionInstance *function =
                mir.findFunctionInstance(*instruction.functionTarget);
            const MirClassInstance *owner =
                function != nullptr && function->owner
                    ? mir.findClassInstance(*function->owner)
                    : nullptr;
            if (owner != nullptr) {
              collectTypeDependencies(collectTypeDependencies, owner->type,
                                      result);
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
              collectTypeDependencies(collectTypeDependencies, owner->type,
                                      result);
            }
          }
        }
      }
    };

    const auto normalizeDependencies = [](TypeDependencies &result) {
      std::sort(result.classes.begin(), result.classes.end());
      result.classes.erase(
          std::unique(result.classes.begin(), result.classes.end()),
          result.classes.end());
      std::sort(result.enumerations.begin(), result.enumerations.end());
      result.enumerations.erase(
          std::unique(result.enumerations.begin(), result.enumerations.end()),
          result.enumerations.end());
    };

    const auto collectSourceDependencies =
        [&](const LoweredDeclaration &row,
            const LoweredClassDeclaration &declaration,
            TypeDependencies &result) {
          if (const auto found = sourceClassSchedule.find(
                  declaration.exactSpecializationPrimary);
              found != sourceClassSchedule.end()) {
            result.classes.push_back(found->second);
          }
          for (const LoweredClassBase &base : declaration.bases) {
            collectTypeDependencies(collectTypeDependencies, base.type, result);
          }
          for (const LoweredDeclaration *member : loweredChildren(row.id)) {
            const auto *storage =
                std::get_if<LoweredStorageDeclaration>(&member->payload);
            if (storage != nullptr) {
              collectTypeDependencies(collectTypeDependencies, storage->type,
                                      result);
            }
          }
          if (!declaration.genericParameters.empty()) {
            return;
          }
          for (const MirClassInstance &instance : mir.classInstances()) {
            if (instance.declaration != declaration.id) {
              continue;
            }
            for (const HirBaseInstance &base : instance.bases) {
              collectTypeDependencies(collectTypeDependencies, base.type,
                                      result);
            }
            for (const MirClassFieldInfo &field : instance.declaredFields) {
              collectTypeDependencies(collectTypeDependencies, field.type,
                                      result);
            }
            collectInitializerDependencies(instance.fieldInitializers, result);
          }
        };

    const auto collectConcreteDependencies =
        [&](const LoweredClassDeclaration &declaration,
            const MirClassInstance &instance, TypeDependencies &result) {
          if (loweredClassNeedsNativePrimary(declaration)) {
            if (const auto found = sourceClassSchedule.find(declaration.id);
                found != sourceClassSchedule.end()) {
              result.classes.push_back(found->second);
            }
          }
          for (const HirBaseInstance &base : instance.bases) {
            collectTypeDependencies(collectTypeDependencies, base.type, result);
          }
          for (const MirClassFieldInfo &field : instance.declaredFields) {
            collectTypeDependencies(collectTypeDependencies, field.type,
                                    result);
          }
          collectInitializerDependencies(instance.fieldInitializers, result);
        };

    std::vector<unsigned char> classState(classes.size(), 0);
    std::unordered_map<EnumId, unsigned char> enumerationState;
    for (const auto &[id, _] : sourceEnumerationSchedule) {
      enumerationState.emplace(id, 0);
    }

    std::function<void(std::size_t)> emitClass;
    std::function<void(EnumId)> emitEnumeration;
    const auto emitDependencies = [&](TypeDependencies dependencies,
                                      std::optional<std::size_t> currentClass,
                                      std::optional<EnumId> currentEnum) {
      if (currentClass) {
        std::erase(dependencies.classes, *currentClass);
      }
      if (currentEnum) {
        std::erase(dependencies.enumerations, *currentEnum);
      }
      normalizeDependencies(dependencies);
      for (const std::size_t dependency : dependencies.classes) {
        emitClass(dependency);
      }
      for (const EnumId dependency : dependencies.enumerations) {
        emitEnumeration(dependency);
      }
    };

    emitEnumeration = [&](EnumId id) {
      const auto scheduled = sourceEnumerationSchedule.find(id);
      const auto state = enumerationState.find(id);
      if (scheduled == sourceEnumerationSchedule.end() ||
          state == enumerationState.end()) {
        throw std::logic_error("lowered enum dependency is not scheduled");
      }
      if (state->second == 2) {
        return;
      }
      if (state->second == 1) {
        throw std::logic_error(
            "lowered type layout contains a recursive by-value enum "
            "dependency");
      }
      state->second = 1;
      const LoweredDeclaration &row = *scheduled->second;
      const auto *declaration =
          std::get_if<LoweredEnumDeclaration>(&row.payload);
      if (declaration == nullptr || row.id != id) {
        throw std::logic_error(
            "lowered enum schedule lost its declaration payload");
      }
      TypeDependencies dependencies;
      for (const LoweredEnumerator &enumerator : declaration->enumerators) {
        for (const SemanticType &payload : enumerator.payloadTypes) {
          collectTypeDependencies(collectTypeDependencies, payload,
                                  dependencies);
        }
      }
      emitDependencies(std::move(dependencies), std::nullopt, id);
      emitLoweredScheduledTypeDefinition(row);
      state->second = 2;
    };

    emitClass = [&](std::size_t index) {
      if (index >= classes.size() || classState[index] == 2) {
        return;
      }
      if (classState[index] == 1) {
        throw std::logic_error(
            "lowered type layout contains a recursive by-value class "
            "dependency");
      }
      classState[index] = 1;
      const ClassEmissionNode &node = classes[index];
      if (node.row == nullptr) {
        throw std::logic_error("lowered class schedule lost its source row");
      }
      const LoweredDeclaration &row = *node.row;
      const auto *declaration =
          std::get_if<LoweredClassDeclaration>(&row.payload);
      if (declaration == nullptr) {
        throw std::logic_error(
            "lowered class schedule lost its declaration payload");
      }
      TypeDependencies dependencies;
      if (node.concrete == nullptr) {
        collectSourceDependencies(row, *declaration, dependencies);
      } else {
        collectConcreteDependencies(*declaration, *node.concrete, dependencies);
      }
      emitDependencies(std::move(dependencies), index, std::nullopt);
      const std::size_t firstDeferredMember = loweredDeferredMembers.size();
      const std::size_t firstDeferredLifecycleClass =
          loweredDeferredLifecycleClasses.size();
      if (node.concrete == nullptr) {
        emitLoweredScheduledTypeDefinition(row);
      } else {
        emitLoweredScheduledConcreteTypeDefinition(row, *declaration,
                                                   *node.concrete);
      }
      const std::size_t lastDeferredMember = loweredDeferredMembers.size();
      const std::size_t lastDeferredLifecycleClass =
          loweredDeferredLifecycleClasses.size();
      classState[index] = 2;

      // A retained source-template specialization can instantiate its
      // complete owner. Complete the exact concrete field graph after the
      // primary becomes available, but do not collapse distinct applications
      // back to one class-family node: that invents cycles for finite nests
      // such as box<cell<box<int>>>.
      if (node.concrete == nullptr) {
        TypeDependencies specializationDependencies;
        for (const MirClassInstance &instance : mir.classInstances()) {
          if (instance.declaration != declaration->id) {
            continue;
          }
          for (const HirBaseInstance &base : instance.bases) {
            collectTypeDependencies(collectTypeDependencies, base.type,
                                    specializationDependencies);
          }
          for (const MirClassFieldInfo &field : instance.declaredFields) {
            collectTypeDependencies(collectTypeDependencies, field.type,
                                    specializationDependencies);
          }
          collectInitializerDependencies(instance.fieldInitializers,
                                         specializationDependencies);
        }
        emitDependencies(std::move(specializationDependencies), index,
                         std::nullopt);
      }
      emitLoweredMemberSpecializationDeclarations(
          firstDeferredMember, lastDeferredMember, firstDeferredLifecycleClass,
          lastDeferredLifecycleClass);
      if (node.concrete == nullptr)
        emitLoweredGeneratedConstructorSpecializationDeclarations(row);
    };
    for (std::size_t index = 0; index < classes.size(); ++index) {
      emitClass(index);
    }

    for (const LoweredDeclaration *enumeration : enumerations) {
      const auto *declaration =
          std::get_if<LoweredEnumDeclaration>(&enumeration->payload);
      if (declaration == nullptr) {
        throw std::logic_error(
            "lowered enum schedule lost its declaration payload");
      }
      emitEnumeration(enumeration->id);
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
          return std::any_of(
              mir.threadTaskAdapters().begin(), mir.threadTaskAdapters().end(),
              [&](const MirThreadTaskAdapter &adapter) {
                return adapter.sourceBody == source && !adapter.scoped;
              });
        });
  }

  [[nodiscard]] std::string loweredCallableSignatureCondition(
      const LoweredFunctionDeclaration &function,
      const MirFunctionInstance &instance,
      const std::vector<const MirFunctionInstance *> &family,
      bool nestedClosures = false) const {
    const auto signature =
        cppMirCallableTemplateSignature(loweredProgram, function, instance);
    if (!signature)
      throw std::logic_error(
          "callable selector lost its exact owner-projected signature");
    const auto *owner =
        instance.owner ? mir.findClassInstance(*instance.owner) : nullptr;
    const bool retainedOwner =
        owner != nullptr && loweredContainsLambdaType(owner->type);
    // A retained primary still declares its owner parameters. An explicit
    // concrete-owner specialization does not: use the same owner substitution
    // already proven for its emitted signature, leaving method parameters open.
    const auto &declaredReturn =
        retainedOwner ? function.returnType : signature->returnType;
    std::vector<std::string> terms;
    std::string arityCondition;
    std::vector<const MirFunctionInstance *> sameArityFamily;
    const auto pack =
        std::find_if(function.parameters.begin(), function.parameters.end(),
                     [](const auto &parameter) { return parameter.pack; });
    if (pack != function.parameters.end()) {
      const auto first = static_cast<std::size_t>(
          std::distance(function.parameters.begin(), pack));
      const auto *genericPack =
          loweredProgram.findGenericParameter(pack->type.genericParameterId);
      if (first + 1 != function.parameters.size() ||
          pack->type.kind != SemanticType::TypePack ||
          instance.parameterTypes.size() < first || genericPack == nullptr ||
          !genericPack->pack || genericPack->value || genericPack->name.empty())
        throw std::logic_error(
            "callable selector lost its exact trailing pack");
      arityCondition = "sizeof...(" + genericPack->name + ") == " +
                       std::to_string(instance.parameterTypes.size() - first);
      for (const auto *candidate : family)
        if (candidate->parameterTypes.size() == instance.parameterTypes.size())
          sameArityFamily.push_back(candidate);
    }
    const auto &selectorFamily =
        arityCondition.empty() ? family : sameArityFamily;
    // Native closure names are local, but their MIR identities are exact and
    // artifact-local. Use the same selector dimensions for every instance,
    // including zero for non-closure substitutions. A matching call signature
    // alone cannot merge bodies that target different concrete callees.
    std::vector<std::string> closureDimensions;
    std::vector<std::pair<std::string, HirLambdaId>> identities;
    const auto lambdaIdentity = [&](const SemanticType &type) {
      const auto lambda = std::find_if(
          mir.lambdaInstances().begin(), mir.lambdaInstances().end(),
          [&](const auto &value) { return value.type == type; });
      if (lambda == mir.lambdaInstances().end())
        throw std::logic_error(
            "callable selector lost its MIR lambda identity");
      return lambda->id;
    };
    // The first closure in a class argument's native template arguments, as
    // mir_nested_lambda_identity_v computes it. A class with value arguments
    // or a nested native pointer slot exposes no type-only argument list.
    const auto nestedIdentity = [&](const auto &self,
                                    const SemanticType &type) -> HirLambdaId {
      if (type.kind == SemanticType::Lambda)
        return lambdaIdentity(type);
      if ((type.kind == SemanticType::Class && !type.valueArguments.empty()) ||
          (type.kind == SemanticType::RawPointer && !type.arguments.empty() &&
           type.arguments.front().kind == SemanticType::RawPointer) ||
          (type.kind != SemanticType::Class &&
           type.kind != SemanticType::UniqueOwner &&
           type.kind != SemanticType::RawPointer &&
           type.kind != SemanticType::Reference &&
           type.kind != SemanticType::Expected &&
           type.kind != SemanticType::Unexpected))
        return 0;
      for (const SemanticType &argument : type.arguments)
        if (const HirLambdaId identity = self(self, argument))
          return identity;
      return 0;
    };
    const auto templateParameterName = [](const std::string &spelling) {
      return !spelling.empty() &&
             std::all_of(spelling.begin(), spelling.end(), [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
             });
    };
    // Nested dimensions are a fallback for a family whose bodies differ only
    // through a closure inside a class argument, such as a model type whose
    // inline eligibility depends on its captures.
    const std::string nestedPrefix = "::gti_internal::backend::"
                                     "mir_nested_lambda_identity_v<";
    for (const auto *candidate : selectorFamily) {
      CppMirBodyEmissionMapRows rows{.types = generalEmissionMap->types()};
      if (!cppMirApplyCallableTemplateTypeOverlays(
              rows, loweredProgram, standard, function, *candidate))
        continue;
      for (const auto &row : rows.types) {
        const bool closure = row.type.kind == SemanticType::Lambda;
        const bool nested = nestedClosures && !closure &&
                            row.type.kind == SemanticType::Class &&
                            loweredContainsLambdaType(row.type) &&
                            templateParameterName(row.spelling);
        if (!row.bodyTypeOverlay || (!closure && !nested))
          continue;
        const std::string dimension =
            (nested ? nestedPrefix
                    : std::string("::gti_internal::backend::"
                                  "mir_lambda_identity_v<")) +
            row.spelling + ">";
        if (std::find(closureDimensions.begin(), closureDimensions.end(),
                      dimension) == closureDimensions.end())
          closureDimensions.push_back(dimension);
        if (candidate->id != instance.id)
          continue;
        identities.emplace_back(
            dimension, nested ? nestedIdentity(nestedIdentity, row.type)
                              : lambdaIdentity(row.type));
      }
    }
    std::sort(closureDimensions.begin(), closureDimensions.end());
    for (const auto &dimension : closureDimensions) {
      const auto identity = std::find_if(
          identities.begin(), identities.end(),
          [&](const auto &entry) { return entry.first == dimension; });
      terms.push_back(
          "(" + dimension + " == " +
          std::to_string(identity == identities.end() ? 0 : identity->second) +
          ")");
    }
    const auto spelling = [&](const SemanticType &type) {
      const auto text = cppSemanticTypeSpelling(loweredProgram, standard, type);
      if (text.empty())
        throw std::logic_error(
            "callable signature selector has an unspellable type");
      return text;
    };
    // Every variant uses the same selector dimensions. Omitting an unnameable
    // dimension for only one instance would make its predicate overlap another
    // variant. A remaining indistinguishable body difference still fails
    // closed.
    if (std::all_of(selectorFamily.begin(), selectorFamily.end(),
                    [&](const auto *candidate) {
                      return loweredTypeIsNameable(candidate->returnType);
                    }))
      terms.push_back("std::is_same_v<" + spelling(declaredReturn) + ", " +
                      spelling(instance.returnType) + ">");
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
      const auto &parameter = function.parameters[index];
      const auto &declaredType =
          retainedOwner ? parameter.type : signature->parameterTypes[index];
      if (parameter.pack) {
        if (index + 1 != function.parameters.size())
          throw std::logic_error(
              "callable signature selector lost its final pack");
        const bool nameable = std::all_of(
            selectorFamily.begin(), selectorFamily.end(),
            [&](const auto *candidate) {
              return candidate->parameterTypes.size() >= index &&
                     std::all_of(candidate->parameterTypes.begin() + index,
                                 candidate->parameterTypes.end(),
                                 [&](const auto &type) {
                                   return loweredTypeIsNameable(type);
                                 });
            });
        if (!nameable)
          continue;
        std::string concrete = "std::tuple<";
        for (std::size_t element = index;
             element < instance.parameterTypes.size(); ++element) {
          if (element != index)
            concrete += ", ";
          concrete += spelling(instance.parameterTypes[element]);
        }
        concrete += '>';
        terms.push_back("std::is_same_v<std::tuple<" + spelling(declaredType) +
                        "...>, " + concrete + ">");
      } else if (std::all_of(selectorFamily.begin(), selectorFamily.end(),
                             [&](const auto *candidate) {
                               return index <
                                          candidate->parameterTypes.size() &&
                                      loweredTypeIsNameable(
                                          candidate->parameterTypes[index]);
                             })) {
        terms.push_back("std::is_same_v<" + spelling(declaredType) + ", " +
                        spelling(instance.parameterTypes[index]) + ">");
      }
    }
    std::string condition;
    for (const auto &term : terms) {
      if (!condition.empty())
        condition += " && ";
      condition += term;
    }
    if (arityCondition.empty())
      return condition;
    // A conjunction does not guard formation of tuple_element_t in an
    // indexed closure discriminator. Discard the entire selector expression
    // for other arities before those types are instantiated. Its dimensions
    // come only from same-arity instances, so every index is in range.
    return "([]() constexpr { if constexpr (" + arityCondition + ") { return " +
           (condition.empty() ? "true" : condition) +
           "; } else { return false; } }())";
  }

  [[nodiscard]] std::string loweredCallableTemplateBody(
      const LoweredFunctionDeclaration &function,
      const std::vector<const MirFunctionInstance *> &instances, bool failure,
      bool nestedClosures = false) const {
    if (!generalEmissionMap) {
      throw std::logic_error(
          "lowered callable template emission has no representation rows");
    }
    const std::string_view familyLabel =
        failure ? "deduced-callable-failure-v1" : "deduced-callable-v1";
    enum class ResultChannel { Value, Indirect, Failure };
    struct BodyVariant {
      // One canonical template body per parameter-cleanup shape: the bit at
      // each declared parameter index records whether that instance binding
      // carries a lexical drop obligation. Instances whose concrete callable
      // environments need different cleanup (a scalar-capturing lambda beside
      // a class-capturing one) form separate variants that the
      // instantiation-time destructibility dispatch below reunites under the
      // single emitted template.
      std::vector<bool> cleanupShape;
      ResultChannel resultChannel = ResultChannel::Value;
      std::string signatureCondition;
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
      const MirClassInstance *owner =
          instance->owner ? mir.findClassInstance(*instance->owner) : nullptr;
      const bool lambdaTransport =
          (owner != nullptr && loweredContainsLambdaType(owner->type)) ||
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
      auto cached = callableTemplatePlans.find(instance->id);
      if (cached == callableTemplatePlans.end()) {
        CppMirBodyEmissionMapRows rows{
            .types = generalEmissionMap->types(),
            .bodies = generalEmissionMap->bodies(),
            .symbols = generalEmissionMap->symbols(),
            .enums = generalEmissionMap->enums(),
            .containedConstructors =
                generalEmissionMap->containedConstructors(),
            .capabilities = generalEmissionMap->capabilities()};
        const std::optional<std::size_t> overlays =
            cppMirApplyCallableTemplateTypeOverlays(
                rows, loweredProgram, standard, function, *instance);
        if (!overlays || *overlays == 0) {
          throw std::logic_error(
              "lowered unnameable callable instance has no template type "
              "overlay");
        }
        const MirBodyAddress address{.kind = MirBodyKind::Function,
                                     .owner = instance->id};
        cached = callableTemplatePlans
                     .emplace(instance->id,
                              std::make_unique<TemplateBodyPlan>(
                                  mir, address, function.id, std::move(rows),
                                  debugInfo ? std::span<const LoweredBody>(
                                                  loweredProgram.bodies())
                                            : std::span<const LoweredBody>{}))
                     .first;
      }
      const auto &prepared = *cached->second;
      if (prepared.declaration != function.id)
        throw std::logic_error(
            "callable template plan changed declaration identity");
      const CppMirBodyEmitter emitter(mir, prepared.representations);
      const CppMirBodyEmissionPlan &plan = prepared.plan;
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
      const ResultChannel resultChannel =
          failure ? ResultChannel::Failure
          : cppRequiresIndirectValueTransport(mir, instance->returnType)
              ? ResultChannel::Indirect
              : ResultChannel::Value;
      const std::string signatureCondition = loweredCallableSignatureCondition(
          function, *instance, instances, nestedClosures);
      for (BodyVariant &row : variants) {
        if (row.cleanupShape == cleanupShape &&
            row.resultChannel == resultChannel &&
            row.signatureCondition == signatureCondition) {
          variant = &row;
          break;
        }
      }
      if (variant == nullptr) {
        variants.push_back({.cleanupShape = std::move(cleanupShape),
                            .resultChannel = resultChannel,
                            .signatureCondition = signatureCondition});
        variant = &variants.back();
      }
      variant->banners.push_back(emission.text.substr(
          lineStart, markerAt - lineStart + marker.size()));
      if (variant->canonical.empty()) {
        variant->canonical = std::move(body);
      } else if (variant->canonical != body) {
        if (!nestedClosures)
          return loweredCallableTemplateBody(function, instances, failure,
                                             true);
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
                return std::tie(left.signatureCondition, left.cleanupShape) <
                       std::tie(right.signatureCondition, right.cleanupShape);
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
      const bool varies = std::any_of(
          variants.begin(), variants.end(), [&](const BodyVariant &left) {
            return std::any_of(
                variants.begin(), variants.end(),
                [&](const BodyVariant &right) {
                  return left.signatureCondition == right.signatureCondition &&
                         left.cleanupShape[index] != right.cleanupShape[index];
                });
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
    if (varying.empty() &&
        std::any_of(variants.begin(), variants.end(), [](const auto &variant) {
          return variant.signatureCondition.empty();
        })) {
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
      if (variant.resultChannel == ResultChannel::Indirect ||
          (function.returnType == SemanticType::Void && !failure)) {
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
      std::string text = variant.signatureCondition;
      for (std::size_t position = 0; position < varying.size(); ++position) {
        if (!text.empty()) {
          text += " && ";
        }
        const std::size_t index = varying[position];
        if (variant.cleanupShape[index]) {
          text += "!";
        }
        text +=
            "std::is_trivially_destructible_v<" + varyingSpellings[index] + ">";
      }
      return text;
    };
    std::string assembled = "{\n";
    for (std::size_t index = 0; index < variants.size(); ++index) {
      const std::string label =
          (index == 0 ? "if constexpr (" : "else if constexpr (") +
          condition(variants[index]) + ")";
      assembled += branch(label, variants[index]);
    }
    assembled += branchIndent + "else {\n" + statementIndent + "static_assert(";
    for (std::size_t index = 0; index < variants.size(); ++index) {
      if (index != 0)
        assembled += " || ";
      assembled += "(" + condition(variants[index]) + ")";
    }
    assembled +=
        ", \"no verified MIR callable instance for this representation\");\n" +
        branchIndent + "}\n";
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

    for (const MirFunctionInstance *instance : instances) {
      const LoweredFunctionInstance *concrete =
          loweredFunctionInstance(instance->id);
      if (concrete != nullptr &&
          cppCallableFreeFunctionSpelling(loweredProgram, row, function,
                                          *concrete, *instance)) {
        for (const bool failure : {false, true}) {
          const std::string body =
              loweredCallableTemplateBody(function, {instance}, failure);
          if (body.empty())
            continue;
          emitLoweredTemplateDeclaration(
              function.genericParameters,
              loweredFunctionNeedsThreadTaskTemplate({instance}));
          writeIndent();
          emitLoweredFunctionSignature(row, function, instance, failure, false,
                                       {}, true);
          output << ' ' << body;
        }
        continue;
      }
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
    // This row may name a concrete carrier, not its canonical source primary.
    // Nested primary rows already carry their independent native spelling.
    output << classRow.name;
    if (declaration->exactSpecializationPrimary != 0 ||
        !declaration->genericArgumentPatterns.empty()) {
      output << loweredDeclaredClassArguments(*declaration);
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

  [[nodiscard]] std::string
  loweredConcreteClassLeafName(HirClassInstanceId instance) const {
    const MirClassInstance *owner = mir.findClassInstance(instance);
    if (owner == nullptr) {
      throw std::logic_error(
          "lowered concrete class name lost its MIR instance");
    }
    if (const std::optional<std::string> direct =
            cppConcreteClassName(loweredProgram, owner->type)) {
      return *direct;
    }
    const LoweredDeclaration *row = loweredClassRow(owner->declaration);
    if (row == nullptr) {
      throw std::logic_error(
          "lowered concrete class name lost its declaration row");
    }
    return row->name;
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

  void emitLoweredGeneratedConstructorSpecializationDeclarations(
      const LoweredDeclaration &owner) {
    const auto *declaration =
        std::get_if<LoweredClassDeclaration>(&owner.payload);
    if (declaration == nullptr || !loweredOwnerIsGeneric(owner))
      return;
    for (const LoweredConstructorInstance &candidate :
         loweredProgram.constructorInstances()) {
      if (candidate.declaration != 0)
        continue;
      const MirClassInstance *instance = mir.findClassInstance(candidate.owner);
      if (instance == nullptr || instance->declaration != declaration->id ||
          loweredClassUsesDirectRepresentation(*instance))
        continue;
      const MirConstructorInstance *constructor =
          mir.findConstructorInstance(candidate.id);
      if (constructor == nullptr)
        throw std::logic_error(
            "generated constructor declaration lost its MIR body");
      // Generated constructors have no source declaration row, so the ordinary
      // deferred-member pass cannot declare these specializations. Match every
      // emitted definition's exact signature before any body can instantiate
      // the retained primary; declaration order does not select GTI behavior.
      const std::size_t namespaceCount =
          emitLoweredNamespaceOpen(owner.namespaceScope);
      const auto &plan = generalMirBodyPlan(
          {.kind = MirBodyKind::Constructor, .owner = constructor->id});
      for (const bool failure : {false, true}) {
        if (!(failure ? plan.supportsFailureText() : plan.supportsPlainText()))
          continue;
        writeIndent();
        output << "template <> " << loweredConcreteClassName(instance->id)
               << "::" << owner.name << '(';
        emitLoweredConstructorParameters(*constructor, failure);
        output << ");\n";
      }
      emitLoweredNamespaceClose(namespaceCount);
    }
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
          if (loweredClassUsesDirectRepresentation(*ownerInstance)) {
            continue;
          }
          const LoweredFunctionInstance *concrete =
              loweredFunctionInstance(instance->id);
          const bool selectedMember =
              concrete != nullptr &&
              cppConcreteMemberFunctionSpelling(loweredProgram, *row, *function,
                                                *concrete, *instance)
                  .has_value();
          if (!loweredOwnerIsGeneric(*owner) && selectedMember) {
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
            emitLoweredFunctionSignature(
                *row, *function, instance, false, false, ownerSpelling, false,
                !selectedMember && !function->genericParameters.empty());
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
          if (loweredClassUsesDirectRepresentation(*ownerInstance)) {
            continue;
          }
          if (!loweredOwnerIsGeneric(*owner) &&
              !constructor->genericParameters.empty() &&
              loweredConstructorInstanceIsNameable(*instance)) {
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
          if (loweredClassUsesDirectRepresentation(instance)) {
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
          &loweredDeferredLifecycleClasses[index];
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
        if (loweredClassUsesDirectRepresentation(cleanup->classInstance)) {
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
                 << "(::gti_failure_record_v2 *);\n";
          writeIndent();
          output << "template <> bool " << ownerSpelling
                 << "::" << lifecycleFailureBodyName()
                 << "(::gti_failure_record_v2 *);\n";
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
      const auto *declaration =
          std::get_if<LoweredClassDeclaration>(&row.payload);
      if (declaration == nullptr) {
        return;
      }
      static_cast<void>(self);
      result += row.name;
      if (declaration->exactSpecializationPrimary != 0 ||
          !declaration->genericArgumentPatterns.empty()) {
        result += loweredDeclaredClassArguments(*declaration);
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

  void emitLoweredVirtualFailureBoundaryBody(
      const LoweredDeclaration &row, const LoweredFunctionDeclaration &function,
      const MirFunctionInstance &instance, bool sourceSignature) {
    const bool indirectResult =
        cppRequiresIndirectValueTransport(mir, instance.returnType);
    const bool placementResult =
        instance.returnType.kind == SemanticType::Class ||
        instance.returnType.kind == SemanticType::Lambda ||
        cppMirOwningArrayPlacementResult(mir, instance.returnType) ||
        cppMirExpectedClassPlacementResult(mir, *generalEmissionMap,
                                           instance.returnType);
    const MirFunctionInstance *root =
        cppMirVirtualFailureContractRoot(mir, instance);
    if (root == nullptr ||
        !loweredVirtualFailureContractRetained(root->declaration)) {
      throw std::logic_error(
          "lowered virtual failure body lost its contract root");
    }
    output << "{\n";
    ++indentation;
    writeIndent();
    output << "::gti_failure_record_v2 __gti_failure_record{};\n";
    if (indirectResult) {
      // The ordinary internal signature already supplies final result storage.
      // Forward it rather than returning a native record through C++ copying.
    } else if (instance.returnType.kind == SemanticType::Reference) {
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
    output << "if (!";
    if (function.receiverMutability == ReceiverMutability::Consuming) {
      output << "std::move(*this).";
    }
    output << cppMirFailureSiblingSpelling(
                  loweredFunctionInstanceSpelling(row, function, &instance))
           << '(';
    const std::size_t parameterCount = sourceSignature
                                           ? function.parameters.size()
                                           : instance.parameterTypes.size();
    for (std::size_t index = 0; index < parameterCount; ++index) {
      if (index != 0) {
        output << ", ";
      }
      if (sourceSignature) {
        // The signature has already been projected and exact-checked against
        // MIR. C++ forwarding here preserves its native value/reference ABI;
        // it does not infer a GTI ownership or overload decision.
        output << "std::forward<decltype(__gti_mir_arg_" << index
               << ")>(__gti_mir_arg_" << index << ')';
        if (function.parameters[index].pack)
          output << "...";
        continue;
      }
      const bool transfer =
          instance.parameterTypes[index].kind != SemanticType::Reference;
      if (transfer) {
        output << "std::move(";
      }
      output << "__gti_mir_arg_" << index;
      if (transfer) {
        output << ')';
      }
    }
    if (parameterCount != 0) {
      output << ", ";
    }
    if (instance.returnType != SemanticType::Void) {
      output << (indirectResult    ? "__gti_mir_out_result, "
                 : placementResult ? "__gti_result.construction_address(), "
                                   : "&__gti_result, ");
    } else if (cppFunctionHasFailureResultChannel(loweredProgram, function)) {
      output << "static_cast<void *>(nullptr), ";
    }
    output << "&__gti_failure_record)) {\n";
    ++indentation;
    emitHostedFailureTermination();
    --indentation;
    writeIndent();
    output << "}\n";
    if (placementResult && !indirectResult) {
      writeIndent();
      output << "__gti_result.mark_constructed();\n";
    }
    if (indirectResult) {
      writeIndent();
      output << "return;\n";
    } else if (instance.returnType.kind == SemanticType::Reference) {
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

  void emitLoweredVirtualFailureBoundaryWrapper(
      const LoweredDeclaration &row, const LoweredFunctionDeclaration &function,
      const MirFunctionInstance &instance, std::string_view ownerSpelling,
      bool explicitSpecialization, bool sourceSignature = false) {
    writeIndent();
    if (explicitSpecialization)
      output << "template <> ";
    emitLoweredFunctionSignature(
        row, function, sourceSignature ? nullptr : &instance, false, false,
        ownerSpelling, sourceSignature, !function.genericParameters.empty());
    output << ' ';
    emitLoweredVirtualFailureBoundaryBody(row, function, instance,
                                          sourceSignature);
  }

  void emitLoweredVirtualFailureBoundaryFamily(
      const LoweredDeclaration &row, const LoweredFunctionDeclaration &function,
      const std::vector<const MirFunctionInstance *> &instances,
      std::string_view ownerSpelling) {
    if (instances.empty())
      throw std::logic_error(
          "virtual boundary family has no concrete instance");
    std::vector<std::pair<std::string, const MirFunctionInstance *>> variants;
    for (const auto *instance : instances) {
      if (!cppMirCallableTemplateSignature(loweredProgram, function, *instance))
        throw std::logic_error(
            "virtual boundary family lost its exact signature projection");
      const auto condition =
          loweredCallableSignatureCondition(function, *instance, instances);
      const auto existing = std::find_if(
          variants.begin(), variants.end(),
          [&](const auto &variant) { return variant.first == condition; });
      if (existing == variants.end()) {
        variants.emplace_back(condition, instance);
      } else if (existing->second->returnType != instance->returnType ||
                 existing->second->parameterTypes != instance->parameterTypes) {
        throw std::logic_error(
            "virtual boundary signatures have no distinct native selectors");
      }
    }
    if (variants.size() == 1) {
      emitLoweredVirtualFailureBoundaryWrapper(
          row, function, *variants.front().second, ownerSpelling, false, true);
      return;
    }
    if (std::any_of(variants.begin(), variants.end(),
                    [](const auto &variant) { return variant.first.empty(); }))
      throw std::logic_error(
          "virtual boundary family has an unguarded signature variant");
    writeIndent();
    emitLoweredFunctionSignature(row, function, nullptr, false, false,
                                 ownerSpelling, true);
    output << " {\n";
    ++indentation;
    for (std::size_t index = 0; index < variants.size(); ++index) {
      writeIndent();
      output << (index == 0 ? "if constexpr (" : "else if constexpr (")
             << variants[index].first << ") ";
      emitLoweredVirtualFailureBoundaryBody(row, function,
                                            *variants[index].second, true);
    }
    writeIndent();
    output << "else {\n";
    ++indentation;
    writeIndent();
    output << "static_assert(";
    for (std::size_t index = 0; index < variants.size(); ++index) {
      if (index != 0)
        output << " || ";
      output << '(' << variants[index].first << ')';
    }
    output << ", \"no verified MIR virtual boundary for this signature\");\n";
    --indentation;
    writeIndent();
    output << "}\n";
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
    const bool indirectResult =
        cppRequiresIndirectValueTransport(mir, instance.returnType);
    const bool placementResult =
        instance.returnType.kind == SemanticType::Class ||
        instance.returnType.kind == SemanticType::Lambda ||
        cppMirOwningArrayPlacementResult(mir, instance.returnType) ||
        cppMirExpectedClassPlacementResult(mir, *generalEmissionMap,
                                           instance.returnType);
    writeIndent();
    if (explicitSpecialization) {
      output << "template <> ";
    }
    emitLoweredFunctionSignature(row, function, &instance, false, false,
                                 ownerSpelling);
    output << " {\n";
    ++indentation;
    writeIndent();
    output << "::gti_failure_record_v2 __gti_failure_record{};\n";
    if (indirectResult) {
      // Preserve the internal byte-transport result convention at the adapter.
    } else if (instance.returnType.kind == SemanticType::Reference) {
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
                  loweredFunctionInstanceSpelling(row, function, &instance))
           << '(';
    for (std::size_t index = 0; index < instance.parameterTypes.size();
         ++index) {
      if (index != 0) {
        output << ", ";
      }
      const bool transfer =
          instance.parameterTypes[index].kind != SemanticType::Reference;
      if (transfer) {
        output << "std::move(";
      }
      output << "__gti_mir_arg_" << index;
      if (transfer) {
        output << ')';
      }
    }
    if (!instance.parameterTypes.empty()) {
      output << ", ";
    }
    if (function.tracksCaller) {
      output << "__gti_mir_caller_site, ";
    }
    if (instance.returnType != SemanticType::Void) {
      output << (indirectResult    ? "__gti_mir_out_result, "
                 : placementResult ? "__gti_result.construction_address(), "
                                   : "&__gti_result, ");
    } else if (cppFunctionHasFailureResultChannel(loweredProgram, function)) {
      output << "static_cast<void *>(nullptr), ";
    }
    output << "&__gti_failure_record)) {\n";
    ++indentation;
    emitHostedFailureTermination();
    --indentation;
    writeIndent();
    output << "}\n";
    if (placementResult && !indirectResult) {
      writeIndent();
      output << "__gti_result.mark_constructed();\n";
    }
    if (indirectResult) {
      writeIndent();
      output << "return;\n";
    } else if (instance.returnType.kind == SemanticType::Reference) {
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
    if (genericOwner) {
      std::vector<const MirFunctionInstance *> dependentOwners;
      for (const MirFunctionInstance *instance : instances) {
        const MirClassInstance *ownerInstance =
            instance->owner ? mir.findClassInstance(*instance->owner) : nullptr;
        if (ownerInstance != nullptr &&
            !loweredTypeIsNameable(ownerInstance->type)) {
          dependentOwners.push_back(instance);
        }
      }
      for (const bool failure : {false, true}) {
        if (!failure && !function.virtualMethod &&
            classBoundByteTransport(function.returnType, function.ownerClass)) {
          for (bool indirect : {false, true}) {
            std::vector<const MirFunctionInstance *> selected;
            std::copy_if(dependentOwners.begin(), dependentOwners.end(),
                         std::back_inserter(selected),
                         [&](const auto *instance) {
                           return cppRequiresIndirectValueTransport(
                                      mir, instance->returnType) == indirect;
                         });
            const std::string body =
                loweredCallableTemplateBody(function, selected, failure);
            if (body.empty())
              continue;
            emitLoweredClassTemplateHeaders(owner);
            emitLoweredTemplateDeclaration(function.genericParameters);
            writeIndent();
            emitLoweredFunctionSignature(row, function, nullptr, failure, false,
                                         loweredPrimaryClassSpelling(owner),
                                         true, false, indirect);
            output << ' ' << body;
          }
          continue;
        }
        const std::string body =
            loweredCallableTemplateBody(function, dependentOwners, failure);
        if (body.empty()) {
          continue;
        }
        emitLoweredClassTemplateHeaders(owner);
        emitLoweredTemplateDeclaration(function.genericParameters);
        writeIndent();
        emitLoweredFunctionSignature(row, function, nullptr, failure, false,
                                     loweredPrimaryClassSpelling(owner), true);
        output << ' ' << body;
        if (failure && function.virtualMethod && !dependentOwners.empty()) {
          // This native vtable bridge contains failure at an ordinary C++
          // boundary; GTI callers use the verified status-returning sibling.
          // The closed family retains one bridge per exact signature selector;
          // every selected body still comes from its concrete verified MIR.
          emitLoweredClassTemplateHeaders(owner);
          emitLoweredVirtualFailureBoundaryFamily(
              row, function, dependentOwners,
              loweredPrimaryClassSpelling(owner));
        }
      }
    }
    if (genericOwner && !function.genericParameters.empty()) {
      std::vector<HirClassInstanceId> emittedOwners;
      for (const MirFunctionInstance *instance : instances) {
        const LoweredFunctionInstance *concrete =
            loweredFunctionInstance(instance->id);
        const MirClassInstance *ownerInstance =
            instance->owner ? mir.findClassInstance(*instance->owner) : nullptr;
        if (!instance->owner || concrete == nullptr ||
            (ownerInstance != nullptr &&
             !loweredTypeIsNameable(ownerInstance->type)) ||
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
      const auto *nameableInstance = loweredFunctionInstance(instance->id);
      if (nameableInstance == nullptr ||
          !loweredFunctionInstanceIsNameable(*nameableInstance, *instance))
        continue;
      if (!loweredNormalBodySupported(*instance) &&
          !loweredFailureBodySupported(*instance)) {
        continue;
      }
      std::string ownerSpelling = loweredPrimaryClassSpelling(owner);
      const bool directOwner =
          instance->owner &&
          loweredClassUsesDirectRepresentation(*instance->owner);
      const LoweredFunctionInstance *concreteInstance =
          loweredFunctionInstance(instance->id);
      const bool directMember =
          concreteInstance != nullptr &&
          cppConcreteMemberFunctionSpelling(loweredProgram, row, function,
                                            *concreteInstance, *instance)
              .has_value();
      // A selected generic method on an ordinary class is an ordinary hashed
      // member. The same selected method inside a retained source class
      // template still needs an explicit specialization of that native owner;
      // its stable name does not turn the owner into a direct class.
      const bool directRepresentation =
          directOwner || (directMember && !genericOwner);
      if (directOwner) {
        ownerSpelling = loweredConcreteClassName(*instance->owner);
      } else if (generic) {
        if (!instance->owner) {
          throw std::logic_error(
              "generic lowered member function lost its owner instance");
        }
        ownerSpelling = loweredConcreteClassName(*instance->owner);
      }
      const auto emitPrefix = [&](bool failureForm) {
        writeIndent();
        if (!directRepresentation &&
            (genericOwner ||
             (!failureForm && !function.genericParameters.empty()))) {
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
          emitLoweredVirtualFailureBoundaryWrapper(
              row, function, *instance, ownerSpelling,
              generic && !directRepresentation);
        } else if (!loweredNormalBodySupported(*instance) &&
                   ((cppMirBodyStatusCannotFail(instance->body) &&
                     cppMirNeedsFailureFreeBoundaryWrapper(
                         mir, *generalEmissionMap, instance->id)) ||
                    cppMirNeedsTerminalSubscriptBoundaryWrapper(
                        mir, *generalEmissionMap, instance->id))) {
          emitLoweredOrdinaryFailureBoundaryWrapper(
              row, function, *instance, ownerSpelling,
              generic && !directRepresentation);
        }
      }
      if (loweredNormalBodySupported(*instance) &&
          !(instance->virtualMethod &&
            loweredFailureBodySupported(*instance))) {
        emitPrefix(false);
        emitLoweredFunctionSignature(row, function, instance, false, false,
                                     ownerSpelling, false,
                                     !directRepresentation && !directMember &&
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
              "::gti_failure_record_v2 *__gti_mir_failure_record";
  }

  void emitBasePreparationDeclaration(const MirConstructorInstance &instance,
                                      std::string_view className) {
    const auto &plan =
        generalMirBodyPlan({MirBodyKind::Constructor, instance.id});
    if (!plan.supportsBasePreparationText())
      return;
    // Called only after the original signature has been closed by its caller.
    // The carrier is completed beside the constructor definitions, where the
    // class type of every temporary slot it holds is complete.
    writeIndent();
    output << "struct __gti_base_preparation_" << instance.id << ";\n";
    writeIndent();
    output << className << "(__gti_base_preparation_" << instance.id
           << " __gti_prepared, bool *__gti_mir_constructor_success, "
              "::gti_failure_record_v2 *__gti_mir_failure_record);\n";
  }

  // The carrier's constructor runs the verified pre-base partition. Passing
  // its prvalue to the delegated constructor builds the carrier in place as
  // that constructor's parameter, so a temporary slot keeps one address from
  // its construction in the preparation to its Drop after the native base.
  void emitBasePreparationDefinition(const MirConstructorInstance &instance,
                                     std::string_view ownerSpelling,
                                     const CppMirBasePreparation &preparation,
                                     const CppMirBodyEmissionPlan &plan) {
    const auto *stage = [&]() -> const MirInstruction * {
      for (const auto &block : instance.body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.id == preparation.stage)
            return &instruction;
      return nullptr;
    }();
    if (stage == nullptr)
      throw std::logic_error("prepared constructor lost its base stage");
    writeIndent();
    output << "struct " << ownerSpelling << "::__gti_base_preparation_"
           << instance.id << " {\n";
    ++indentation;
    writeIndent();
    output << "bool ready = false;\n";
    for (std::size_t index = 0; index < stage->parameterTypes.size(); ++index) {
      writeIndent();
      emitSemanticType(stage->parameterTypes[index]);
      output << " argument_" << index << "{};\n";
    }
    for (std::size_t index = 0; index < instance.parameterTypes.size();
         ++index) {
      writeIndent();
      emitSemanticType(instance.parameterTypes[index]);
      output << " parameter_" << index << "{};\n";
    }
    for (const auto id : preparation.carriedValues) {
      writeIndent();
      emitSemanticType(instance.body.findValue(id)->info.type);
      output << " value_" << id << "{};\n";
    }
    for (const auto id : preparation.places) {
      const auto *place = instance.body.findPlace(id);
      if (place->root == MirPlaceRootKind::Symbol)
        continue;
      writeIndent();
      emitSemanticType(place->type);
      output << " place_" << id << "{};\n";
    }
    for (const auto id : preparation.slots) {
      writeIndent();
      output << "::gti_internal::backend::mir_lifetime_slot<";
      emitSemanticType(instance.body.findPlace(id)->type);
      output << "> slot_" << id << ";\n";
    }
    writeIndent();
    output << "__gti_base_preparation_" << instance.id << '(';
    const auto count = emitLoweredMirParameters(instance.parameterTypes);
    if (count)
      output << ", ";
    output << "::gti_failure_record_v2 *__gti_mir_failure_record);\n";
    --indentation;
    writeIndent();
    output << "};\n";
    writeIndent();
    output << ownerSpelling << "::__gti_base_preparation_" << instance.id
           << "::__gti_base_preparation_" << instance.id << '(';
    if (emitLoweredMirParameters(instance.parameterTypes))
      output << ", ";
    output << "::gti_failure_record_v2 *__gti_mir_failure_record) "
           << CppMirBodyTextWriter::write(
                  plan, CppMirBodyTextForm::BasePreparation,
                  "scalar-cfg-base-preparation-v1", indentation)
                  .text;
  }

  void emitBaseGateDeclaration(const MirConstructorInstance &instance,
                               std::string_view className) {
    const auto &plan =
        generalMirBodyPlan({MirBodyKind::Constructor, instance.id});
    if (!plan.supportsBaseGateText())
      return;
    writeIndent();
    output << className << '(';
    emitLoweredConstructorParameters(instance, true);
    output << ", bool __gti_base_enabled);\n";
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
      emitLoweredParameters(constructor.parameters, true, constructor.owner);
      separator = true;
    }
    if (!failure) {
      return;
    }
    if (separator) {
      output << ", ";
    }
    output << "bool *__gti_mir_constructor_success, "
              "::gti_failure_record_v2 *__gti_mir_failure_record";
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

  [[nodiscard]] static std::string
  loweredConstructorRepresentationText(std::string body) {
    body = loweredBodyWithoutInstanceMarker(std::move(body));
    // Both owned class and closure parameters can publish exclusively in the
    // proved native member initializer list. Their MIR writers describe that
    // same no-op Initialize differently. Compare executable representation,
    // not those two diagnostic comments; every other line remains exact.
    constexpr std::string_view fieldPublication =
        "// field initialization publishes in the member initializer list";
    constexpr std::string_view closurePrefix = "// closure local ";
    constexpr std::string_view closureSuffix = " joins the fused chain";
    for (std::size_t start = 0; start < body.size();) {
      const std::size_t end = body.find('\n', start);
      const std::size_t length =
          (end == std::string::npos ? body.size() : end) - start;
      std::string_view line(body.data() + start, length);
      const std::size_t first = line.find_first_not_of(" \t");
      if (first != std::string_view::npos)
        line.remove_prefix(first);
      bool closurePublication = false;
      if (line.size() >= closurePrefix.size() + closureSuffix.size() &&
          line.starts_with(closurePrefix) && line.ends_with(closureSuffix)) {
        const std::string_view identity = line.substr(
            closurePrefix.size(),
            line.size() - closurePrefix.size() - closureSuffix.size());
        closurePublication =
            !identity.empty() &&
            std::all_of(identity.begin(), identity.end(),
                        [](char c) { return c >= '0' && c <= '9'; });
      }
      if (line == fieldPublication || closurePublication) {
        body.erase(start, length + (end != std::string::npos ? 1 : 0));
      } else {
        start += length + (end != std::string::npos ? 1 : 0);
      }
    }
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
    struct GenericNormalBody {
      const MirConstructorInstance *instance = nullptr;
      std::vector<bool> cleanup;
      std::string text;
      std::string initializers;
      std::vector<std::string> banners;
    };
    std::vector<GenericNormalBody> genericNormalBodies;
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
      const bool directOwner =
          ownerInstance != nullptr &&
          loweredClassUsesDirectRepresentation(*ownerInstance);
      const bool directConstructor =
          directOwner ||
          (!genericOwner && !constructor.genericParameters.empty() &&
           loweredConstructorInstanceIsNameable(*instance));
      const bool unnameableConstructor =
          !loweredConstructorInstanceIsNameable(*instance);
      if (unnameableConstructor) {
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
          // The selected failure definition above uses the verified dependent
          // type overlay when a concrete parameter contains an unnamed closure.
          // Its containment wrapper must use that same admission, not an
          // unrelated ordinary plan that cannot spell the closure's type.
          const auto prepared = constructorTemplatePlans.find(instance->id);
          if (prepared == constructorTemplatePlans.end() ||
              prepared->second->declaration != constructor.id) {
            throw std::logic_error(
                "contained generic-owner constructor lost its template plan");
          }
          emitLoweredContainedConstructorBody(owner, *instance,
                                              prepared->second->plan);
        }
        if (genericNormal) {
          const std::string marker =
              "// GTI verified-MIR body: scalar-cfg-v1 constructor-instance " +
              std::to_string(instance->id) + "\n";
          const std::size_t markerAt = genericNormal->find(marker);
          if (markerAt == std::string::npos) {
            throw std::logic_error(
                "generic-owner constructor lost its exact instance marker");
          }
          std::size_t lineStart = markerAt;
          while (lineStart > 0 && (*genericNormal)[lineStart - 1] != '\n') {
            --lineStart;
          }
          const std::string banner = genericNormal->substr(
              lineStart, markerAt - lineStart + marker.size());
          const std::string normalized =
              loweredConstructorRepresentationText(*genericNormal);
          std::vector<bool> cleanup;
          for (const HirBindingId binding : instance->parameterBindings) {
            cleanup.push_back(std::any_of(
                instance->body.dropObligations.begin(),
                instance->body.dropObligations.end(),
                [&](const MirDropObligation &drop) {
                  return drop.kind == MirDropObligationKind::Binding &&
                         drop.binding == binding;
                }));
          }
          std::ostringstream saved = std::move(output);
          output = std::ostringstream{};
          emitGeneralMirConstructorInitializerList(*instance);
          std::string initializers = output.str();
          output = std::move(saved);
          const auto previous = std::find_if(
              genericNormalBodies.begin(), genericNormalBodies.end(),
              [&](const GenericNormalBody &body) {
                return body.cleanup == cleanup;
              });
          if ((!genericNormalBodies.empty() &&
               genericNormalBodies.front().initializers != initializers) ||
              (previous != genericNormalBodies.end() &&
               previous->text != normalized)) {
            throw std::logic_error("generic-owner constructor instances have "
                                   "incompatible MIR representations");
          }
          if (previous == genericNormalBodies.end()) {
            genericNormalBodies.push_back({instance,
                                           std::move(cleanup),
                                           normalized,
                                           std::move(initializers),
                                           {banner}});
          } else {
            previous->banners.push_back(banner);
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
          directOwner ? loweredConcreteClassName(instance->owner)
          : generic   ? loweredConcreteClassName(instance->owner)
                      : loweredPrimaryClassSpelling(owner);
      const std::string constructorName =
          directOwner ? loweredConcreteClassLeafName(instance->owner)
                      : owner.name;
      const auto emitSignature = [&](bool failureForm) {
        writeIndent();
        if (!directConstructor && (genericOwner || (generic && !failureForm))) {
          output << "template <> ";
        }
        output << ownerSpelling << "::" << constructorName << '(';
        emitLoweredConstructorParameters(*instance, failureForm);
        output << ')';
      };
      const auto preparation = cppMirBasePreparation(mir, *instance);
      if (generalMirBodyPlan(address).supportsBaseGateText()) {
        writeIndent();
        output << ownerSpelling << "::" << constructorName << '(';
        emitLoweredConstructorParameters(*instance, true);
        output << ", bool __gti_base_enabled)";
        emitGeneralMirConstructorInitializerList(*instance, true);
        output << ' '
               << CppMirBodyTextWriter::write(
                      generalMirBodyPlan(address), CppMirBodyTextForm::BaseGate,
                      "scalar-cfg-base-gate-v1", indentation)
                      .text;
      }
      if (failure) {
        if (preparation)
          emitBasePreparationDefinition(*instance, ownerSpelling, *preparation,
                                        generalMirBodyPlan(address));
        emitSignature(true);
        if (preparation) {
          output << " : " << constructorName << "(__gti_base_preparation_"
                 << instance->id << '(';
          for (std::size_t index = 0; index < instance->parameterTypes.size();
               ++index) {
            if (index)
              output << ", ";
            output << "__gti_mir_arg_" << index;
          }
          if (!instance->parameterTypes.empty())
            output << ", ";
          output << "__gti_mir_failure_record), __gti_mir_constructor_success, "
                    "__gti_mir_failure_record) {}\n";
          writeIndent();
          output << ownerSpelling << "::" << constructorName
                 << "(__gti_base_preparation_" << instance->id
                 << " __gti_prepared, bool *__gti_mir_constructor_success, "
                    "::gti_failure_record_v2 *__gti_mir_failure_record)";
        }
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
        if (genericOwner && !directConstructor) {
          output << "template <> ";
        }
        output << ownerSpelling << "::" << constructorName << '(';
        emitLoweredContainedConstructorParameters(constructor, *instance,
                                                  false);
        output << ')';
        emitLoweredContainedConstructorBody(
            owner, *instance, generalMirBodyPlan(address), constructorName);
      }
      if (normal) {
        emitSignature(false);
        emitGeneralMirConstructorInitializerList(*instance);
        output << ' ';
        emitGeneralMirConstructorBodyText(*instance);
      }
    }
    if (!genericNormalBodies.empty()) {
      const auto assemble = [](const GenericNormalBody &body) {
        const std::size_t brace = body.text.find("{\n");
        if (brace == std::string::npos) {
          throw std::logic_error(
              "generic-owner constructor body has no opening brace");
        }
        std::string text = body.text.substr(0, brace + 2);
        for (const std::string &banner : body.banners) {
          text += banner;
        }
        text += body.text.substr(brace + 2);
        return text;
      };
      const GenericNormalBody &first = genericNormalBodies.front();
      emitLoweredClassTemplateHeaders(owner);
      emitLoweredTemplateDeclaration(constructor.genericParameters);
      writeIndent();
      output << loweredPrimaryClassSpelling(owner) << "::" << owner.name << '(';
      emitLoweredGenericOwnerConstructorParameters(constructor, *first.instance,
                                                   false);
      output << ')' << first.initializers << ' ';
      if (genericNormalBodies.size() == 1) {
        output << assemble(first);
      } else {
        std::vector<std::size_t> varying;
        for (std::size_t index = 0; index < first.cleanup.size(); ++index) {
          if (std::any_of(genericNormalBodies.begin(),
                          genericNormalBodies.end(),
                          [&](const GenericNormalBody &body) {
                            return body.cleanup[index] != first.cleanup[index];
                          })) {
            if (index >= constructor.parameters.size() ||
                constructor.parameters[index].type.kind !=
                    SemanticType::TypeParameter) {
              throw std::logic_error("generic constructor cleanup has no exact "
                                     "template type discriminator");
            }
            varying.push_back(index);
          }
        }
        if (varying.empty()) {
          throw std::logic_error(
              "generic constructor body variants lack a cleanup discriminator");
        }
        output << "{\n";
        ++indentation;
        for (std::size_t index = 0; index < genericNormalBodies.size();
             ++index) {
          const GenericNormalBody &body = genericNormalBodies[index];
          writeIndent();
          output << (index == 0 ? "if constexpr (" : "else if constexpr (");
          bool separator = false;
          for (const std::size_t parameter : varying) {
            if (separator) {
              output << " && ";
            }
            if (body.cleanup[parameter]) {
              output << '!';
            }
            output << "std::is_trivially_destructible_v<"
                   << cppSemanticTypeSpelling(
                          loweredProgram, standard,
                          constructor.parameters[parameter].type)
                   << '>';
            separator = true;
          }
          output << ") {\n";
          ++indentation;
          writeIndent();
          output << "[&]() -> void " << assemble(body) << "();\n";
          --indentation;
          writeIndent();
          output << "}\n";
        }
        const std::string discriminator = cppSemanticTypeSpelling(
            loweredProgram, standard,
            constructor.parameters[varying.front()].type);
        writeIndent();
        output << "else { static_assert(!std::is_same_v<" << discriminator
               << ", " << discriminator
               << ">, \"unrepresented GTI constructor instance\"); }\n";
        --indentation;
        writeIndent();
        output << "}\n";
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
    bool retainedPrimaryEmitted = false;
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
      const bool retainedPrimary = !loweredTypeIsNameable(classInstance.type);
      if (retainedPrimary && retainedPrimaryEmitted)
        continue;
      const std::string ownerSpelling =
          retainedPrimary ? loweredPrimaryClassSpelling(owner)
          : loweredClassUsesDirectRepresentation(classInstance)
              ? loweredConcreteClassName(classInstance.id)
          : generic ? loweredConcreteClassName(classInstance.id)
                    : loweredPrimaryClassSpelling(owner);
      const bool directOwner =
          loweredClassUsesDirectRepresentation(classInstance);
      const std::string destructorName =
          directOwner ? loweredConcreteClassLeafName(classInstance.id)
                      : owner.name;
      if (retainedPrimary)
        emitLoweredClassTemplateHeaders(owner);
      writeIndent();
      if (generic && !directOwner && !retainedPrimary) {
        output << "template <> ";
      }
      output << ownerSpelling << "::~" << destructorName << "() noexcept ";
      if (!classDeclaration->requiresActiveDropState) {
        throw std::logic_error(
            "declared lowered destructor has no lifecycle cleanup adapter");
      }
      output << "{ " << loweredLifecycleCleanupName(classDeclaration->id)
             << "(); }\n";
      emitted = true;
      retainedPrimaryEmitted = retainedPrimaryEmitted || retainedPrimary;
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
    for (const LoweredDeclaration &deferredClass :
         loweredDeferredLifecycleClasses) {
      const LoweredDeclaration *classRow = &deferredClass;
      const auto *classDeclaration =
          std::get_if<LoweredClassDeclaration>(&classRow->payload);
      if (classDeclaration == nullptr) {
        throw std::logic_error(
            "lowered lifecycle cleanup lost its class declaration");
      }
      const std::size_t namespaceCount =
          emitLoweredNamespaceOpen(classRow->namespaceScope);
      const auto direct =
          loweredConcreteLifecycleInstances.find(classRow->name);
      bool emitted = false;
      bool deferredToDirectClass = false;
      std::optional<std::pair<bool, std::string>> retainedBody;
      for (const CppMirGeneratedThunk &thunk : programPlan.thunks) {
        if (thunk.identity.kind != CppMirThunkKind::LifecycleCleanup) {
          continue;
        }
        const auto *cleanup =
            std::get_if<CppMirLifecycleCleanupThunk>(&thunk.payload);
        if (cleanup == nullptr || cleanup->ownerClass != classDeclaration->id) {
          continue;
        }
        if (direct != loweredConcreteLifecycleInstances.end() &&
            cleanup->classInstance != direct->second) {
          continue;
        }
        // A directly represented instance is an ordinary concrete class, not
        // a specialization of this primary. Its own deferred row defines its
        // lifecycle members, matching the declarations emitted for it.
        if (direct == loweredConcreteLifecycleInstances.end() &&
            loweredClassUsesDirectRepresentation(cleanup->classInstance)) {
          deferredToDirectClass = true;
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
        const bool retainedPrimary =
            cleanup->form == CppMirLifecycleCleanupForm::RetainedPrimary;
        std::optional<std::string> retainedText;
        if (retainedPrimary) {
          retainedText = loweredGenericOwnerDestructorBodyText(
              *destructor, destructor->mayRaiseDefinedFailure);
          const std::pair<bool, std::string> shape{
              destructor->mayRaiseDefinedFailure,
              loweredBodyWithoutInstanceMarker(*retainedText)};
          if (retainedBody) {
            if (*retainedBody != shape)
              throw std::logic_error(
                  "generic-owner destructor instances have incompatible MIR "
                  "representations");
            continue;
          }
          retainedBody = shape;
        }
        const bool directConcrete =
            direct != loweredConcreteLifecycleInstances.end();
        const std::string ownerSpelling =
            concrete ? loweredConcreteClassName(cleanup->classInstance)
                     : loweredPrimaryClassSpelling(*classRow);
        const auto emitPrefix = [&] {
          if (retainedPrimary)
            emitLoweredClassTemplateHeaders(*classRow);
          writeIndent();
          if (concrete && !directConcrete) {
            output << "template <> ";
          }
        };
        const MirBodyAddress address{.kind = MirBodyKind::Destructor,
                                     .owner = destructor->id};
        const bool failure =
            destructor->mayRaiseDefinedFailure &&
            (retainedPrimary ||
             generalMirBodyPlan(address).supportsFailureText());
        if (failure) {
          emitPrefix();
          output << "bool " << ownerSpelling
                 << "::" << lifecycleFailureBodyName()
                 << "(::gti_failure_record_v2 "
                    "*__gti_mir_failure_record) ";
          if (retainedText)
            output << *retainedText;
          else
            emitGeneralFailureDestructorBodyText(*destructor);

          emitPrefix();
          output << "bool " << ownerSpelling
                 << "::" << lifecycleFailureDispatchName()
                 << "(::gti_failure_record_v2 "
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
        writeIndent();
        output << "if (::gti_internal::backend::"
                  "mir_cleanup_emergency_abandonment()) { return; }\n";
        if (failure) {
          writeIndent();
          output << "::gti_failure_record_v2 __gti_failure_record{};\n";
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
          if (!retainedPrimary &&
              !generalMirBodyPlan(address).supportsPlainText()) {
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
          if (retainedText)
            output << *retainedText;
          else
            emitGeneralMirDestructorBodyText(*destructor, "scalar-cfg-v1");
        }
        --indentation;
        writeIndent();
        output << "}\n\n";
        emitted = true;
      }
      if (!emitted && !deferredToDirectClass &&
          (direct != loweredConcreteLifecycleInstances.end() ||
           !loweredOwnerIsGeneric(*classRow) ||
           loweredClassHasConcreteInstance(classDeclaration->id))) {
        throw std::logic_error(
            "lowered active-drop class has no lifecycle generated item");
      }
      emitLoweredNamespaceClose(namespaceCount);
    }
    loweredDeferredLifecycleClasses.clear();
  }

  void emitLoweredMemberDefinitions() {
    for (const LoweredConstructorInstance &candidate :
         loweredProgram.constructorInstances()) {
      if (candidate.declaration != 0)
        continue;
      const MirConstructorInstance *constructor =
          mir.findConstructorInstance(candidate.id);
      const MirClassInstance *instance = mir.findClassInstance(candidate.owner);
      const LoweredDeclaration *owner =
          instance == nullptr ? nullptr
                              : loweredClassRow(instance->declaration);
      if (constructor == nullptr || owner == nullptr)
        throw std::logic_error("generated constructor lost its concrete owner");
      const bool directOwner = loweredClassUsesDirectRepresentation(*instance);
      const bool genericOwner = loweredOwnerIsGeneric(*owner);
      const std::string ownerSpelling =
          directOwner || genericOwner
              ? loweredConcreteClassName(constructor->owner)
              : loweredPrimaryClassSpelling(*owner);
      const std::string constructorName =
          directOwner ? loweredConcreteClassLeafName(constructor->owner)
                      : owner->name;
      const std::size_t namespaceCount =
          emitLoweredNamespaceOpen(owner->namespaceScope);
      const auto &plan = generalMirBodyPlan(
          {.kind = MirBodyKind::Constructor, .owner = constructor->id});
      for (const bool failure : {false, true}) {
        if (!(failure ? plan.supportsFailureText() : plan.supportsPlainText()))
          continue;
        writeIndent();
        if (genericOwner && !directOwner)
          output << "template <> ";
        output << ownerSpelling << "::" << constructorName << '(';
        emitLoweredConstructorParameters(*constructor, failure);
        output << ')';
        emitGeneralMirConstructorInitializerList(*constructor, failure);
        output << ' ';
        if (failure)
          emitGeneralFailureConstructorBodyText(*constructor);
        else
          emitGeneralMirConstructorBodyText(*constructor);
      }
      emitLoweredNamespaceClose(namespaceCount);
    }
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
      const auto &type = *aggregate->value->exactType;
      if (std::find(loweredConstexprAggregateClasses.begin(),
                    loweredConstexprAggregateClasses.end(),
                    type) == loweredConstexprAggregateClasses.end()) {
        loweredConstexprAggregateClasses.push_back(type);
      }
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
    prepareLoweredNativePrimaryClasses();
    prepareLoweredConstexprAggregateClasses();
    capture(plan, CppProgramTextRole::DeclarationTypeMemberLifecycle,
            [&] { emitLoweredPublicOpaqueHandleForwardDeclarations(); });
    prepareLoweredModuleInitialization();

    capture(plan, CppProgramTextRole::DeclarationTypeMemberLifecycle, [&] {
      // Ordinary GTI identities are implementation details of this complete
      // lowered program, not a native C++ ABI. Keep them translation-unit-local
      // so the native compiler can discard unused bodies and data. Native
      // imports retain their separately selected external link labels.
      output << "namespace {\nnamespace __gti_program {\n";
      ++indentation;
      emitLoweredTypeForwardDeclarations(0);
      emitLoweredConcreteClassForwardDeclarations();
      emitLoweredExactClassSpecializationForwardDeclarations();
      emitLoweredAliasForwardDeclarations(0);
      emitLoweredFunctionForwardDeclarations(0);
    });
    capture(plan, CppProgramTextRole::NativeAndThreadAdapter,
            [&] { emitNativeCallbackAdapterDeclarations(); });
    capture(plan, CppProgramTextRole::DeclarationTypeMemberLifecycle, [&] {
      emitLoweredGlobalStorageForwardDeclarations(0);
      emitLoweredTypeDefinitions();
      emitLoweredCallableAdapterDefinitions();
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
      output << "} // anonymous namespace\n";
    });
    capture(plan, CppProgramTextRole::NativeAndThreadAdapter,
            [&] { emitThreadTaskAdapterDefinitions(); });
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
    const bool functionItem = adapter.taskType.hasFunctionItemShape();
    if (target == nullptr || declaration == nullptr || row == nullptr ||
        target->definitionKind != MirDefinitionKind::Source ||
        target->linkage != LanguageLinkage::Gti ||
        target->returnType != adapter.resultType ||
        !target->parameterTypes.empty() || target->pureVirtual ||
        (functionItem
             ? owner != nullptr ||
                   adapter.taskType.functionId != target->declaration ||
                   target->overloadedOperator.has_value() ||
                   adapter.targetInvocation !=
                       CallableInvocationCapability::Read
             : owner == nullptr || owner->type != adapter.taskType ||
                   target->overloadedOperator !=
                       std::optional{OverloadedOperator::Call} ||
                   adapter.targetInvocation !=
                       callableInvocationCapability(
                           target->receiverMutability)) ||
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
    const bool functionItem = adapter.taskType.hasFunctionItemShape();
    const auto emitTargetName = [&](bool failureForm) {
      output << "::__gti_program::";
      for (const std::string &scope : row->namespaceScope) {
        output << (scope == "std" ? emittedStandardNamespace
                                  : std::string_view(scope))
               << "::";
      }
      output << (failureForm ? cppMirFailureSiblingSpelling(name) : name);
    };
    const bool valueBearing = adapter.resultType != SemanticType::Void;
    if (target.mayRaiseDefinedFailure) {
      output << "      const bool success = ";
      if (functionItem) {
        emitTargetName(true);
      } else {
        emitThreadTaskReceiver(adapter.targetInvocation,
                               valueBearing || adapter.scoped);
        output << '.' << cppMirFailureSiblingSpelling(name);
      }
      output << '(';
      if (valueBearing) {
        output << "box->result.construction_address(), ";
      } else if (cppFunctionHasFailureResultChannel(loweredProgram,
                                                    *declaration)) {
        // A class-bound generic result keeps its uniform output parameter
        // even when this concrete task returns void. Use the same signature
        // contract as ordinary calls and wrappers, not only the result type.
        output << "static_cast<void *>(nullptr), ";
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
    if (functionItem) {
      emitTargetName(false);
    } else {
      emitThreadTaskReceiver(adapter.targetInvocation,
                             valueBearing || adapter.scoped);
      output << '.' << name;
    }
    output << "()";
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
      if (adapter.scoped) {
        if (valueBearing)
          throw std::logic_error("scoped task has a non-void result");
        output << "  template <typename Task>\n"
                  "  static void entry(void *raw) noexcept {\n"
                  "    auto *box = static_cast<managed_scoped_task_box<Task> "
                  "*>(raw);\n"
                  "    ::gti_runtime_detail::run_managed_task_adapter([&] {\n"
                  "      ::gti_failure_record_v2 failure{};\n";
        if (functionTarget != nullptr) {
          emitThreadTaskFunctionCall(adapter, *functionTarget);
        } else {
          output << "      if constexpr (requires { "
                    "std::move(box->task.get())(&failure); }) {\n"
                    "        if (!std::move(box->task.get())(&failure))\n"
                    "          "
                    "::gti_runtime_detail::handoff_managed_worker_failure(\n"
                    "              failure, "
                    "&__gti_failure_artifact_descriptor_v1);\n"
                    "      } else {\n"
                    "        read_only_receiver(box->task.get())();\n"
                    "      }\n";
        }
        output
            << "    });\n  }\n\n"
               "  template <typename Task>\n"
               "  static bool cleanup(mir_lifetime_slot<Task, true> &value,\n"
               "      ::gti_failure_record_v2 *failure, bool unwinding) {\n";
        const auto cleanup = mirUniqueCleanupType(mir, adapter.taskType);
        if (cleanup && cleanup->requiresActiveCleanup) {
          output << "    return value.destroy_cleanup_with_failure(failure, "
                    "false, unwinding,\n"
                    "        [](auto &element, ::gti_failure_record_v2 "
                    "*record, bool unwinding) { return "
                 << cppMirCleanupValueExpression(mir, *cleanup, "element",
                                                 "record", "unwinding")
                 << "; });\n";
        } else {
          output << "    value.destroy_if_engaged();\n    return true;\n";
        }
        output << "  }\n};\n\n";
        continue;
      }
      const auto emitCleanupHelper = [&](std::string_view name,
                                         const SemanticType &type, bool slot) {
        const auto cleanup = mirUniqueCleanupType(mir, type);
        // These helpers run inside run_managed_task_adapter. A verified
        // cleanup-emergency may use its private handoff to stop all remaining
        // task cleanup; the public entry adapter remains noexcept.
        output << "  template <typename Value>\n  static bool " << name
               << "(Value &value) {\n"
                  "    const auto pending = ::gti_runtime_detail::"
                  "managed_task_cleanup_failure_kind();\n"
                  "    if (pending == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1) "
                  "return false;\n";
        if (cleanup && cleanup->requiresActiveCleanup) {
          output
              << "    const bool unwinding = pending == "
                 "GTI_THREAD_WORKER_FAILURE_ORDINARY_V1;\n"
                 "    ::gti_failure_record_v2 primary{};\n"
                 "    const ::gti_failure_artifact_descriptor_v1 "
                 "*primary_artifact = nullptr;\n"
                 "    ::gti_internal::backend::mir_failure_cleanup_scope_v1 "
                 "cleanup_context;\n"
                 "    if (unwinding) {\n"
                 "      if "
                 "(!::gti_runtime_detail::copy_managed_task_cleanup_primary_v2("
                 "primary, primary_artifact)) std::abort();\n"
                 "      cleanup_context.activate(&primary, primary_artifact);\n"
                 "    }\n"
                 "    ::gti_failure_record_v2 failure{};\n    const bool "
                 "success = ";
          if (slot)
            output << "value.destroy_cleanup_with_failure(&failure, true, "
                      "unwinding, "
                      "[](auto &element, ::gti_failure_record_v2 *record, bool "
                      "unwinding) { return "
                   << cppMirCleanupValueExpression(mir, *cleanup, "element",
                                                   "record", "unwinding")
                   << "; });\n";
          else
            output << cppMirCleanupValueExpression(mir, *cleanup, "value",
                                                   "&failure", "unwinding")
                   << ";\n";
          output
              << "    if (!success) ::gti_internal::backend::"
                 "mir_check_incoming_cleanup_failure_v1(&failure, "
                 "&__gti_failure_artifact_descriptor_v1);\n"
              << "    if (!success && "
                 "!::gti_runtime_detail::record_managed_task_cleanup_failure(\n"
                 "        failure, &__gti_failure_artifact_descriptor_v1)) {\n"
                 "      ::gti_rt_failure_terminate_v2(&failure, "
                 "&__gti_failure_artifact_descriptor_v1, nullptr, nullptr);\n"
                 "    }\n";
        } else if (slot) {
          output << "    value.destroy_if_engaged();\n";
        }
        output << "    return ::gti_runtime_detail::"
                  "managed_task_cleanup_failure_kind() != "
                  "GTI_THREAD_WORKER_FAILURE_CLEANUP_V1;\n"
                  "  }\n\n";
      };
      emitCleanupHelper("cleanup_task", adapter.taskType, valueBearing);
      if (valueBearing)
        emitCleanupHelper("cleanup_result", adapter.resultType, true);
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
                "      ::gti_failure_record_v2 failure{};\n";
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
        [&] { cleanup_task(box->task); });
    return box;
  }

  template <typename Task, typename Result>
  static void abandon_task(void *raw) noexcept {
    auto *box =
        static_cast<managed_thread_value_box<Task, Result> *>(raw);
    ::gti_runtime_detail::run_managed_task_adapter([&] {
      if (!cleanup_result(box->result)) return;
      if (!cleanup_task(box->task)) return;
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
      if (!cleanup_result(box->result)) return;
      delete box;
    });
  }

  template <typename Task, typename Result>
  static void abandon_result(void *raw) noexcept {
    auto *box =
        static_cast<managed_thread_value_box<Task, Result> *>(raw);
    ::gti_runtime_detail::run_managed_task_adapter([&] {
      if (!cleanup_result(box->result)) return;
      delete box;
    });
  }
)";
      } else {
        output << "  template <typename Task>\n"
                  "  static void destroy(void *raw) noexcept {\n"
                  "    ::gti_runtime_detail::run_managed_task_adapter([&] {\n"
                  "      auto *box = static_cast<managed_thread_task_box<Task> "
                  "*>(raw);\n"
                  "      if (!cleanup_task(box->task)) return;\n"
                  "      delete box;\n"
                  "    });\n"
                  "  }\n";
      }
      output << "};\n\n";
    }
    if (opened) {
      output << "} // namespace gti_internal::backend\n\n";
    }
  }

  void emitThreadTaskAdapterDeclarations() {
    bool opened = false;
    for (const CppMirGeneratedThunk &thunk : programPlan.thunks) {
      if (thunk.identity.kind != CppMirThunkKind::ThreadTaskAdapter) {
        continue;
      }
      const CppMirThreadTaskThunk &thread = threadTaskPayload(thunk);
      if (mir.findThreadTaskAdapter(thread.adapter.id) == nullptr) {
        throw std::logic_error(
            "planned thread task declaration lost its exact adapter");
      }
      if (!opened) {
        output << "namespace gti_internal::backend {\n\n";
        opened = true;
      }
      output << "template <>\n"
                "struct managed_thread_task_adapter<"
             << thread.adapter.id << ">;\n";
    }
    if (opened) {
      output << "\n} // namespace gti_internal::backend\n\n";
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
    output << cppNativeTypeSpelling(loweredProgram, standard, *returnType);
    output << " __gti_native_callback_" << adapter.id << '(';
    const std::span<const SemanticType> parameters =
        adapter.type.nativeFunctionParameterTypes();
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << cppNativeTypeSpelling(loweredProgram, standard,
                                      parameters[index]);
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
      if (adapter.type.nativeFunctionParameterTypes()[index].kind ==
          SemanticType::StringView) {
        output << "std::string_view(__gti_callback_arg_" << index
               << ".data, static_cast<std::size_t>(__gti_callback_arg_" << index
               << ".length))";
      } else {
        output << "__gti_callback_arg_" << index;
      }
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
      if (callback.adapter.cThreadCallback ==
          CThreadCallbackKind::ThreadConfined) {
        const auto id = callback.adapter.id;
        output
            << "static std::atomic<uint64_t> __gti_callback_owner_" << id
            << "{0};\n"
            << "static auto __gti_bind_native_callback_" << id
            << "() noexcept {\n"
            << "  const auto owner = ::gti_rt_confined_callback_thread_v1();\n"
            << "  uint64_t expected = 0;\n"
            << "  if (__gti_callback_owner_" << id
            << ".load(std::memory_order_relaxed) != owner && "
               "!__gti_callback_owner_"
            << id
            << ".compare_exchange_strong(expected, owner, "
               "std::memory_order_relaxed) && expected != owner) "
               "std::terminate();\n"
            << "  return &__gti_native_callback_" << id << ";\n}\n";
      }
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

      const bool attached =
          adapter.cThreadCallback != CThreadCallbackKind::None;
      const auto emitAttachment = [&](bool entering) {
        const char *symbol = nullptr;
        switch (adapter.cThreadCallback) {
        case CThreadCallbackKind::None:
          return;
        case CThreadCallbackKind::ThreadEntry:
          symbol =
              entering ? "gti_rt_cthread_enter_v1" : "gti_rt_cthread_leave_v1";
          break;
        case CThreadCallbackKind::OnceInitializer:
          symbol = entering ? "gti_rt_once_enter_v1" : "gti_rt_once_leave_v1";
          break;
        case CThreadCallbackKind::ThreadConfined:
          writeIndent();
          if (entering) {
            output
                << "::gti_rt_confined_callback_enter_v1(__gti_callback_owner_"
                << adapter.id << ".load(std::memory_order_relaxed));\n";
          } else {
            output << "::gti_rt_confined_callback_leave_v1();\n";
          }
          return;
        }
        if (symbol == nullptr) {
          throw std::logic_error("invalid callback entry policy");
        }
        writeIndent();
        output << "::" << symbol << "(&__gti_native_callback_" << adapter.id
               << ");\n";
      };
      writeIndent();
      emitNativeCallbackAdapterSignature(callback, true);
      output << " {\n";
      ++indentation;
      writeIndent();
      output << "try {\n";
      ++indentation;
      emitAttachment(true);
      if (target.mayRaiseDefinedFailure) {
        writeIndent();
        output << "::gti_failure_record_v2 __gti_failure_record{};\n";
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
        emitIncomingCleanupFailureCheck("&__gti_failure_record");
        writeIndent();
        output << "::gti_rt_failure_terminate_v2(\n";
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
        emitAttachment(false);
        if (returnType != SemanticType::Void) {
          writeIndent();
          output << "return __gti_result;\n";
        } else {
          writeIndent();
          output << "return;\n";
        }
      } else {
        const bool indirect =
            cppRequiresIndirectValueTransport(mir, returnType);
        writeIndent();
        if (indirect) {
          emitSemanticType(returnType);
          output << " __gti_result{};\n";
          writeIndent();
        } else if (returnType != SemanticType::Void) {
          output << (attached ? "const auto __gti_result = " : "return ");
        }
        emitNativeCallbackTargetName(callback);
        output << '(';
        emitNativeCallbackArguments(callback);
        if (indirect) {
          if (!adapter.type.nativeFunctionParameterTypes().empty())
            output << ", ";
          output << "&__gti_result";
        }
        output << ");\n";
        emitAttachment(false);
        if ((attached || indirect) && returnType != SemanticType::Void) {
          writeIndent();
          output << "return __gti_result;\n";
        }
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
    const bool preparedBase =
        failureForm && cppMirBasePreparation(mir, constructor).has_value();
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
        // A separately admitted base initializer remains in the native list;
        // owned fields stay empty until their MIR Move/Initialize executes.
        failureForm && owner != nullptr &&
        cppMirFailureConstructorEmptyFieldsEligible(mir, owner->id) &&
        bindings && bindings->empty() && baseBindings && copiedBindings;
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
      std::vector<CppMirConstructedFieldInitializerArgument> baseArguments;
      std::vector<bool> movedParameters;
      std::vector<CppMirConstructedFieldInitializerArgument>
          constructedArguments;
      HirConstructorInstanceId statusConstructor = 0;
      bool prepared = false;
    };
    std::vector<InitializerBinding> initializers;
    initializers.reserve(baseBindings->size() + bindings->size() +
                         copiedBindings->size() +
                         (ownedBindings ? ownedBindings->size() : 0));
    for (const CppMirBaseParameterInitializerBinding &binding : *baseBindings) {
      initializers.push_back({.kind = BindingKind::Base,
                              .initializer = binding.initializer,
                              .baseType = binding.baseType,
                              .baseParameters = binding.parameters,
                              .baseArguments = binding.arguments,
                              .prepared = binding.prepared});
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
                              .constructedArguments = binding.arguments,
                              .statusConstructor = binding.failureStatus
                                                       ? binding.constructor
                                                       : 0});
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
    if (owner != nullptr) {
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
        // Both ordinary and failure-form bodies publish these fields from
        // the same verified MIR schedule. Neither may invoke an unrelated
        // source default constructor before that schedule begins.
        // Passive fields may retain their ordinary C++ default state until
        // verified MIR publishes them. A class field without a native default
        // constructor needs the exact private empty state proved for that
        // concrete type. This is deliberately per-field: a mutex owner is
        // move-constructible while its public move assignment stays deleted.
        const auto representation =
            cppMirExpectedTypeRepresentation(field->type);
        const bool passiveScalar =
            field->dropKind == DropKind::Trivial &&
            !field->requiresActiveCleanup && representation &&
            (*representation == CppMirTypeRepresentationKind::Scalar ||
             *representation == CppMirTypeRepresentationKind::Enum ||
             field->type.kind == SemanticType::RawPointer);
        if (!passiveScalar && field->type.kind != SemanticType::Array &&
            !cppMirFailureConstructorEmptyStateTypeEligible(mir, field->type)) {
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
        const MirInstruction *baseStage = nullptr;
        for (const auto &block : constructor.body.blocks)
          for (const auto &instruction : block.instructions)
            if (instruction.kind == MirInstructionKind::InitializeBase &&
                instruction.constructorInitializer ==
                    initializer.initializer + 1)
              baseStage = &instruction;
        const bool status =
            failureForm && baseStage != nullptr &&
            (!baseStage->definedFailure.empty() || initializer.prepared);
        if (status)
          output << cppMirFailureConstructorTagSpelling(
                        *baseStage->constructorTarget)
                 << "{}";
        if (initializer.prepared) {
          for (std::size_t index = 0; index < baseStage->parameterTypes.size();
               ++index)
            output << ", __gti_prepared.argument_" << index;
        }
        for (std::size_t index = 0; index < initializer.baseArguments.size();
             ++index) {
          if (status || index != 0)
            output << ", ";
          const auto &argument = initializer.baseArguments[index];
          if (argument.parameter)
            output << "__gti_mir_arg_" << *argument.parameter;
          else if (argument.literal)
            emitMirLiteral(*argument.literal, argument.type);
          else
            throw std::logic_error(
                "MIR base initializer lost its exact scalar argument");
        }
        if (status)
          output << ", __gti_mir_constructor_success, __gti_mir_failure_record";
        if (initializer.prepared)
          output << ", __gti_prepared.ready";
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
        const bool sequencedArguments = std::any_of(
            initializer.constructedArguments.begin(),
            initializer.constructedArguments.end(), [](const auto &argument) {
              return argument.constructor.has_value();
            });
        output << (sequencedArguments ? '{' : '(');
        if (initializer.statusConstructor != 0) {
          if (!failureForm)
            throw std::logic_error("raising native field requires status form");
          output << cppMirFailureConstructorTagSpelling(
                        initializer.statusConstructor)
                 << "{}";
        }
        const auto emitArgument =
            [&](const auto &self,
                const CppMirConstructedFieldInitializerArgument &source)
            -> void {
          const unsigned sources = unsigned(source.literal.has_value()) +
                                   unsigned(source.parameter.has_value()) +
                                   unsigned(source.constructor.has_value());
          if (sources != 1)
            throw std::logic_error(
                "constructed field initializer has an ambiguous argument");
          if (source.constructor) {
            if (source.failureStatus) {
              if (!failureForm)
                throw std::logic_error("nested status argument requires a "
                                       "failure-form constructor");
              output << "([&]() { bool __gti_nested_success = false; return ";
            }
            emitSemanticType(source.type);
            output << '{';
            if (source.failureStatus)
              output << cppMirFailureConstructorTagSpelling(*source.constructor)
                     << "{}";
            for (std::size_t index = 0; index < source.arguments.size();
                 ++index) {
              if (index || source.failureStatus)
                output << ", ";
              self(self, source.arguments[index]);
            }
            if (source.failureStatus)
              output << ", &__gti_nested_success, __gti_mir_failure_record";
            output << '}';
            if (source.failureStatus)
              output << "; }())";
          } else if (source.literal) {
            emitMirLiteral(*source.literal, source.type);
          } else {
            if (source.moved)
              output << "std::move(";
            output << (preparedBase ? "__gti_prepared.parameter_"
                                    : "__gti_mir_arg_")
                   << *source.parameter;
            if (source.moved)
              output << ')';
          }
        };
        for (std::size_t argument = 0;
             argument < initializer.constructedArguments.size(); ++argument) {
          if (argument != 0 || initializer.statusConstructor != 0) {
            output << ", ";
          }
          const CppMirConstructedFieldInitializerArgument &source =
              initializer.constructedArguments[argument];
          emitArgument(emitArgument, source);
        }
        if (initializer.statusConstructor != 0)
          output << ", __gti_mir_constructor_success, __gti_mir_failure_record";
        output << (sequencedArguments ? "})" : "))");
        continue;
      }
      if (initializer.kind == BindingKind::EmptyField) {
        output << fieldName
               << "(::gti_internal::backend::mir_empty_state_v1<"
                  "std::remove_cv_t<decltype("
               << fieldName << ")>>())";
        continue;
      }
      if (initializer.parameter < constructor.parameterTypes.size() &&
          cppRequiresIndirectValueTransport(
              mir, constructor.parameterTypes[initializer.parameter])) {
        // The shared binding proof still owns the source selection; passive
        // native fields publish their bytes at the matching MIR Initialize.
        output << fieldName << "{}";
        continue;
      }
      output << fieldName << '(';
      if (initializer.moved) {
        output << "std::move(";
      }
      output << (preparedBase ? "__gti_prepared.parameter_" : "__gti_mir_arg_")
             << initializer.parameter;
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

  void emitIncomingCleanupFailureCheck(std::string_view record) {
    writeIndent();
    output
        << "::gti_internal::backend::mir_check_incoming_cleanup_failure_v1("
        << record
        << ", "
           "&::gti_internal::backend::__gti_failure_artifact_descriptor_v1);\n";
  }

  void emitHostedFailureTermination() {
    emitIncomingCleanupFailureCheck("&__gti_failure_record");
    writeIndent();
    output << "::gti_rt_failure_terminate_v2(\n";
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
      output << "::gti_failure_record_v2 *__gti_mir_failure_record";
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
      output << "::gti_failure_record_v2 __gti_failure_record{};\n";
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
    // ReturnEntry maps the full GTI int result to a portable process status
    // only after the verified entry call and invocation cleanup. Never let a
    // host's truncation turn a nonzero GTI result into a successful exit.
    const auto emitEntryStatus = [&]() {
      writeIndent();
      output << "return (__gti_result >= 0 && __gti_result <= 255) "
                "? __gti_result : 1;\n";
    };
    const auto emitCThreadBoundary = [&](bool begin) {
      const auto targetId = begin ? hosted->cThreadBegin : hosted->cThreadEnd;
      if (targetId == 0) {
        return;
      }
      const MirFunctionInstance *target = mir.findFunctionInstance(targetId);
      if (target == nullptr ||
          target->cThreadOperation != (begin ? CThreadRuntimeOperation::Begin
                                             : CThreadRuntimeOperation::End)) {
        throw std::logic_error(
            "hosted C-thread boundary lost its exact target");
      }
      writeIndent();
      const LoweredFunctionDeclaration *declaration =
          loweredProgram.findFunctionDeclaration(target->declaration);
      const LoweredDeclaration *row = loweredFunctionRow(target->declaration);
      if (declaration == nullptr || row == nullptr) {
        throw std::logic_error(
            "hosted C-thread boundary lost its declaration identity");
      }
      emitLoweredQualifiedFunctionName(*row, *declaration);
      output << "();\n";
    };
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
        output << "::gti_failure_record_v2 __gti_failure_record{};\n";
        writeIndent();
        output << "std::int32_t __gti_result{};\n";
        writeIndent();
        output << "try {\n";
        ++indentation;
        emitRuntimeModuleInitializationCall();
        emitCThreadBoundary(true);
        writeIndent();
        output << "if (";
        emitLoweredQualifiedFunctionName(*entryRow, *entryDeclaration, true);
        output << "(&__gti_result, &__gti_failure_record)) {\n";
        ++indentation;
        emitCThreadBoundary(false);
        emitEntryStatus();
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
      if (!moduleRuntimeEmission && !mir.requiresCThreadRuntime()) {
        output << "\nint main() {\n";
        ++indentation;
        writeIndent();
        output << "const auto __gti_result = ";
        emitLoweredQualifiedFunctionName(*entryRow, *entryDeclaration);
        output << "();\n";
        emitEntryStatus();
        --indentation;
        output << "}\n";
        return;
      }
      output << "\nint main() {\n";
      ++indentation;
      if (moduleRuntimeMayRaise) {
        writeIndent();
        output << "::gti_failure_record_v2 __gti_failure_record{};\n";
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
      emitCThreadBoundary(true);
      writeIndent();
      output << "const auto __gti_result = ";
      emitLoweredQualifiedFunctionName(*entryRow, *entryDeclaration);
      output << "();\n";
      emitCThreadBoundary(false);
      emitEntryStatus();
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
    const std::string appendSpelling =
        loweredFunctionInstanceSpelling(*appendRow, *appendDeclaration, append);

    // Only the vector remains live when argument construction/append fails.
    // Its exact generated drop is owned by HostedStartup, including the
    // source-owner cleanup and secondary-failure terminal edge.
    const auto emitArgumentFailureCleanup = [&](MirHostedStartupOperationKind
                                                    producer) {
      const auto *body = mir.hostedStartup();
      bool foundProducer = false;
      const MirInstruction *dropInstruction = nullptr;
      for (const auto &operation : hosted->operations) {
        if (operation.kind == producer) {
          foundProducer = true;
          continue;
        }
        if (!foundProducer)
          continue;
        if (operation.kind !=
                MirHostedStartupOperationKind::RouteOperationFailure &&
            operation.kind != MirHostedStartupOperationKind::DropFailureCleanup)
          break;
        if (operation.kind != MirHostedStartupOperationKind::DropFailureCleanup)
          continue;
        const auto *block =
            body == nullptr ? nullptr : body->findBlock(operation.block);
        if (block != nullptr)
          for (const auto &instruction : block->instructions)
            if (instruction.id == operation.instruction)
              dropInstruction = &instruction;
        break;
      }
      const auto *drop = dropInstruction == nullptr || body == nullptr ||
                                 dropInstruction->lifecycle.size() != 1
                             ? nullptr
                             : body->findDropObligation(
                                   dropInstruction->lifecycle.front().source);
      if (drop == nullptr || drop->place != hosted->argumentVectorPlace ||
          dropInstruction->destination != drop->place ||
          drop->dropType != mirUniqueCleanupType(mir, entry->parameterTypes[1]))
        throw std::logic_error(
            "hosted argument cleanup lost its exact MIR recipe");
      writeIndent();
      output << "{\n";
      ++indentation;
      writeIndent();
      output << "::gti_failure_record_v2 __gti_cleanup_failure{};\n";
      writeIndent();
      output << "::gti_internal::backend::mir_failure_cleanup_scope_v1 "
                "__gti_cleanup_context(&__gti_failure_record, "
                "&::gti_internal::backend::__gti_failure_artifact_descriptor_"
                "v1);\n";
      writeIndent();
      output << "if (!"
             << cppMirCleanupValueExpression(mir, drop->dropType,
                                             "__gti_arguments",
                                             "&__gti_cleanup_failure", "true")
             << ") {\n";
      ++indentation;
      emitIncomingCleanupFailureCheck("&__gti_cleanup_failure");
      --indentation;
      writeIndent();
      output << "}\n";
      --indentation;
      writeIndent();
      output << "}\n";
    };

    output << "\nint main(int __gti_native_argc, "
              "char **__gti_native_argv) {\n";
    ++indentation;
    if (failure || moduleRuntimeMayRaise) {
      writeIndent();
      output << "::gti_failure_record_v2 __gti_failure_record{};\n";
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
    emitCThreadBoundary(true);

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
      emitArgumentFailureCleanup(
          MirHostedStartupOperationKind::ConstructArgumentString);
      emitHostedFailureTermination();
      --indentation;
      writeIndent();
      output << "}\n";
      writeIndent();
      output << "if (!__gti_arguments."
             << cppMirFailureSiblingSpelling(appendSpelling)
             << "(std::move(__gti_argument), &__gti_failure_record)) {\n";
      ++indentation;
      emitArgumentFailureCleanup(MirHostedStartupOperationKind::CallAppend);
      emitHostedFailureTermination();
      --indentation;
      writeIndent();
      output << "}\n";
    } else {
      writeIndent();
      output << "__gti_arguments." << appendSpelling << '(';
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
      emitCThreadBoundary(false);
      emitEntryStatus();
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
      output << "const auto __gti_result = ";
      emitLoweredQualifiedFunctionName(*entryRow, *entryDeclaration);
      output << "(__gti_stabilized_argc, std::move(__gti_arguments));\n";
      emitCThreadBoundary(false);
      emitEntryStatus();
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
      output << "char8_t{" << static_cast<unsigned int>(value->value) << '}';
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
      if (type->kind == SemanticType::Expected ||
          type->kind == SemanticType::Unexpected) {
        const auto checked = convertConstantToType(constant, *type);
        if (!checked || *checked.value != constant)
          throw std::logic_error(
              "result constant differs from its exact active alternative");
        const auto &node = *aggregate->value;
        const bool expected = type->kind == SemanticType::Expected;
        const bool success =
            node.kind == ConstantAggregateKind::ExpectedSuccess;
        if (expected) {
          emitSemanticType(*type);
          output << '{';
        }
        if (!success)
          output << (standard == CppStandard::Cpp23
                         ? "std::unexpected("
                         : "::nonstd::make_unexpected(");
        if (!node.elements.empty())
          emitConstant(node.elements[0],
                       &type->arguments[expected && !success ? 1 : 0]);
        if (!success)
          output << ')';
        if (expected)
          output << '}';
        return;
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
    } else {
      output << "nullptr";
    }
  }

  void emitMirLiteral(const Literal &literal, const SemanticType &type) {
    if (std::holds_alternative<std::nullptr_t>(literal)) {
      output << "nullptr";
      return;
    }
    if (const auto *integer = std::get_if<std::uint64_t>(&literal)) {
      output << "static_cast<";
      emitSemanticType(type);
      output << ">(";
      emitSwitchInteger(EnumConstant{.negative = false, .magnitude = *integer},
                        type);
      output << ')';
      return;
    }
    if (const auto *character = std::get_if<CharacterLiteral>(&literal)) {
      output << "char8_t{" << static_cast<unsigned int>(character->value)
             << '}';
      return;
    }
    if (const auto *boolean = std::get_if<bool>(&literal)) {
      output << (*boolean ? "true" : "false");
      return;
    }
    if (const auto *text = std::get_if<std::string>(&literal)) {
      output << cppMirStringViewLiteralSpelling(*text);
      return;
    }
    if (const auto *value = std::get_if<BinaryFloat>(&literal)) {
      emitFloat(*value);
      return;
    }
    throw std::logic_error(
        "constructed field initializer lost its proven scalar literal");
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
  bool debugInfo;
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
  // Text-planning views only: the sealed LoweredProgram retains its lexical
  // ownership/access tree. Native scopes must not create false layout edges.
  std::unordered_map<ClassId, LoweredDeclaration> nativeClassRows;
  std::unordered_map<ClassId, std::vector<const LoweredDeclaration *>>
      lexicalClassChildren;
  std::unordered_set<LoweredDeclarationId> loweredForwardedLinkage;
  std::unordered_set<SymbolId> loweredNamespaceStaticSymbols;
  std::vector<const LoweredDeclaration *> loweredDeferredStaticFields;
  std::vector<const LoweredDeclaration *> loweredDeferredMembers;
  struct DeferredCallableAdapter {
    LoweredDeclaration row;
    LoweredFunctionDeclaration function;
    LoweredDeclaration owner;
  };
  std::vector<DeferredCallableAdapter> loweredDeferredCallableAdapters;
  std::vector<LoweredDeclaration> loweredDeferredLifecycleClasses;
  std::unordered_map<std::string, HirClassInstanceId>
      loweredConcreteLifecycleInstances;
  std::unordered_set<ClassId> loweredNativePrimaryClasses;
  std::unordered_set<LoweredDeclarationId> loweredDeferredMemberIds;
  std::unordered_set<LoweredDeclarationId> loweredEmittedTypes;
  std::vector<SemanticType> loweredConstexprAggregateClasses;
  mutable std::optional<CppMirBodyEmitter> generalBodyEmitter;
  // A planner owns one immutable LoweredProgram and representation map. Keep
  // each fully validated overlay plan for declaration and definition emission
  // instead of re-verifying the whole program at every spelling request.
  // No plan crosses a planner/compilation boundary, and no validation is
  // skipped when a plan is first constructed.
  struct TemplateBodyPlan {
    std::size_t declaration;
    CppMirBodyEmissionMap representations;
    CppMirBodyEmissionPlan plan;
    TemplateBodyPlan(const MirProgram &program, MirBodyAddress address,
                     std::size_t declaration, CppMirBodyEmissionMapRows rows,
                     std::span<const LoweredBody> debugBodies)
        : declaration(declaration), representations(std::move(rows)),
          plan(CppMirBodyEmitter(program, representations, nullptr, debugBodies)
                   .buildPlan(address)) {}
  };
  mutable std::unordered_map<HirConstructorInstanceId,
                             std::unique_ptr<TemplateBodyPlan>>
      constructorTemplatePlans;
  mutable std::unordered_map<HirDestructorInstanceId,
                             std::unique_ptr<TemplateBodyPlan>>
      destructorTemplatePlans;
  mutable std::unordered_map<HirFunctionInstanceId,
                             std::unique_ptr<TemplateBodyPlan>>
      callableTemplatePlans;
};

CppProgramEmissionPlan
buildCppProgramEmissionPlan(const LoweredProgram &program, CppStandard standard,
                            bool debugInfo) {
  std::vector<LoweredProgramIssue> issues;
  const std::optional<VerifiedLoweredProgram> verified =
      verifiedLoweredProgram(program, issues);
  if (!verified) {
    throw std::logic_error("C++ emitter requires a verified lowered program: " +
                           issues.front().detail);
  }
  return buildCppProgramEmissionPlan(*verified, standard, debugInfo);
}

CppProgramEmissionPlan
buildCppProgramEmissionPlan(const VerifiedLoweredProgram &program,
                            CppStandard standard, bool debugInfo) {
  const std::optional<CppRepresentationScope> representation = [&] {
    const PhaseTimeScope timeScope("backend-representation");
    return std::optional<CppRepresentationScope>(std::in_place, program);
  }();
  std::optional<CppProgramEmissionPlanner> planner;
  {
    const PhaseTimeScope timeScope("backend-planner-init");
    planner.emplace(program, standard, debugInfo);
  }
  const PhaseTimeScope timeScope("backend-plan-build");
  return planner->build();
}

} // namespace lang
