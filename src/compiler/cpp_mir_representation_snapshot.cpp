#include "cpp_mir_representation_snapshot.h"

#include "cpp_representation.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace lang {

class CppMirRepresentationSnapshotBuilderAccess {
public:
  static void seal(CppMirRepresentationSnapshot &snapshot) {
    snapshot.inventorySeal_ = CppMirRepresentationSnapshot::InventorySeal{
        .mir = snapshot.mir,
        .bodies = snapshot.bodies,
        .data = snapshot.data,
        .declarationRoots = snapshot.declarationRoots,
        .thunks = snapshot.thunks};
  }
};

namespace {

template <typename Symbol>
[[nodiscard]] std::string cppProgramStorageSpelling(const Symbol &record,
                                                    SymbolId symbol) {
  if (record.kind != SymbolKind::GlobalVariable || record.name.empty()) {
    return {};
  }

  std::string_view qualified = record.qualifiedName;
  if (qualified.empty()) {
    qualified = record.name;
  }
  if (qualified.size() < record.name.size() ||
      qualified.substr(qualified.size() - record.name.size()) != record.name) {
    return {};
  }

  std::string spelling = "::__gti_program";
  if (qualified.size() != record.name.size()) {
    const std::size_t nameOffset = qualified.size() - record.name.size();
    if (nameOffset < 2 || qualified.substr(nameOffset - 2, 2) != "::") {
      return {};
    }
    const std::string_view scopes = qualified.substr(0, nameOffset - 2);
    std::size_t begin = 0;
    std::size_t index = 0;
    while (begin < scopes.size()) {
      const std::size_t end = scopes.find("::", begin);
      const std::string_view scope = scopes.substr(
          begin,
          end == std::string_view::npos ? std::string_view::npos : end - begin);
      if (scope.empty()) {
        return {};
      }
      spelling += "::";
      spelling += index == 0 && scope == "std"
                      ? std::string(cppEmittedStandardNamespace)
                      : std::string(scope);
      ++index;
      if (end == std::string_view::npos) {
        break;
      }
      begin = end + 2;
    }
  }

  spelling += "::";
  spelling += record.internalLinkage
                  ? cppStaticStorageValueSpelling(symbol, record.name)
                  : record.name;
  return spelling;
}

[[nodiscard]] bool containsDependentType(const SemanticType &type) {
  if (type.kind == SemanticType::TypeParameter ||
      type.kind == SemanticType::TypePack || type.arrayLengthParameterId != 0 ||
      std::any_of(type.valueArguments.begin(), type.valueArguments.end(),
                  [](const CompileTimeValue &value) {
                    return value.kind == CompileTimeValue::Parameter;
                  })) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     containsDependentType);
}

void addIssue(CppMirRepresentationSnapshotBuild &build,
              CppMirRepresentationSnapshotIssueKind kind, std::string detail) {
  build.issues.push_back({.kind = kind, .detail = std::move(detail)});
}

[[nodiscard]] bool containsTemplateTypeParameter(const SemanticType &type) {
  if (type.kind == SemanticType::TypeParameter ||
      type.kind == SemanticType::TypePack) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     containsTemplateTypeParameter);
}

[[nodiscard]] bool containsLambdaType(const SemanticType &type) {
  if (type.kind == SemanticType::Lambda) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     containsLambdaType);
}

[[nodiscard]] bool applyTemplateTypeOverlay(
    CppMirBodyEmissionMapRows &rows,
    std::vector<std::pair<SemanticType, std::string>> &overlays,
    const SemanticType &concrete, std::string_view parameterName) {
  if (parameterName.empty() || concrete.kind == SemanticType::TypeParameter ||
      concrete.kind == SemanticType::TypePack) {
    return false;
  }
  const std::optional<CppMirTypeRepresentationKind> concreteKind =
      cppMirExpectedTypeRepresentation(concrete);
  if (!concreteKind) {
    return false;
  }
  const auto prior =
      std::find_if(overlays.begin(), overlays.end(), [&](const auto &overlay) {
        return overlay.first == concrete;
      });
  if (prior != overlays.end()) {
    return prior->second == parameterName;
  }
  const auto existing = std::find_if(rows.types.begin(), rows.types.end(),
                                     [&](const CppMirTypeRepresentation &row) {
                                       return row.type == concrete;
                                     });
  if (existing == rows.types.end()) {
    rows.types.push_back({.type = concrete,
                          .kind = *concreteKind,
                          .spelling = std::string(parameterName)});
  } else {
    if (existing->kind != *concreteKind) {
      return false;
    }
    existing->spelling = parameterName;
  }
  overlays.emplace_back(concrete, parameterName);
  return true;
}

} // namespace

std::optional<std::size_t> cppMirApplyCallableTemplateTypeOverlays(
    CppMirBodyEmissionMapRows &rows, const LoweredProgram &program,
    CppStandard standard, const LoweredFunctionDeclaration &declaration,
    const MirFunctionInstance &instance) {
  if (declaration.id == 0 || instance.declaration != declaration.id ||
      instance.parameterTypes.size() != declaration.parameters.size()) {
    return std::nullopt;
  }

  std::vector<std::pair<SemanticType, std::string>> overlays;
  for (std::size_t index = 0; index < declaration.parameters.size(); ++index) {
    const SemanticType &declared = declaration.parameters[index].type;
    const SemanticType &concrete = instance.parameterTypes[index];
    if (declared.kind == SemanticType::TypeParameter) {
      const LoweredGenericParameter *parameter =
          program.findGenericParameter(declared.genericParameterId);
      if (parameter == nullptr ||
          !applyTemplateTypeOverlay(rows, overlays, concrete,
                                    parameter->name)) {
        return std::nullopt;
      }
      continue;
    }
    if (declared != concrete && containsTemplateTypeParameter(declared) &&
        (concrete.kind == SemanticType::Lambda ||
         concrete.kind == SemanticType::Class) &&
        !applyTemplateTypeOverlay(
            rows, overlays, concrete,
            cppSemanticTypeSpelling(program, standard, declared))) {
      return std::nullopt;
    }
  }
  if (declaration.returnType != instance.returnType &&
      containsTemplateTypeParameter(declaration.returnType) &&
      (instance.returnType.kind == SemanticType::Lambda ||
       instance.returnType.kind == SemanticType::Class) &&
      !applyTemplateTypeOverlay(
          rows, overlays, instance.returnType,
          cppSemanticTypeSpelling(program, standard, declaration.returnType))) {
    return std::nullopt;
  }
  return overlays.size();
}

std::optional<std::size_t> cppMirApplyGenericOwnerConstructorTypeOverlays(
    CppMirBodyEmissionMapRows &rows, const LoweredProgram &program,
    const LoweredConstructorDeclaration &declaration,
    const MirConstructorInstance &instance) {
  const LoweredConstructorInstance *source =
      program.findConstructorInstance(instance.id);
  const LoweredClassInstance *owner = program.findClassInstance(instance.owner);
  const LoweredClassDeclaration *ownerInfo =
      owner == nullptr ? nullptr
                       : program.findClassDeclaration(owner->declaration);
  const MirClassInstance *mirOwner =
      program.mir().findClassInstance(instance.owner);
  if (source == nullptr || source->declaration != declaration.id ||
      source->owner != instance.owner || owner == nullptr ||
      ownerInfo == nullptr || mirOwner == nullptr ||
      owner->id != mirOwner->id ||
      owner->declaration != mirOwner->declaration ||
      declaration.owner != ownerInfo->id ||
      ownerInfo->genericParameters.empty() || owner->typeArguments.empty() ||
      !owner->valueArguments.empty()) {
    return std::nullopt;
  }

  std::vector<std::pair<SemanticType, std::string>> overlays;
  std::size_t typeIndex = 0;
  bool lambdaArgument = false;
  for (const LoweredGenericParameter &parameter :
       ownerInfo->genericParameters) {
    if (parameter.pack || parameter.value ||
        typeIndex >= owner->typeArguments.size()) {
      return std::nullopt;
    }
    const SemanticType &argument = owner->typeArguments[typeIndex++];
    lambdaArgument = lambdaArgument || containsLambdaType(argument);
    if (!applyTemplateTypeOverlay(rows, overlays, argument, parameter.name)) {
      return std::nullopt;
    }
  }
  if (!lambdaArgument || typeIndex != owner->typeArguments.size() ||
      overlays.empty()) {
    return std::nullopt;
  }
  return overlays.size();
}

namespace {

[[nodiscard]] bool
hasOnlyUnsupportedTextVocabulary(const CppMirBodyEmissionAnalysis &analysis) {
  return !analysis.issues.empty() &&
         std::all_of(
             analysis.issues.begin(), analysis.issues.end(),
             [](const CppMirBodyEmissionIssue &issue) {
               return issue.kind ==
                      CppMirBodyEmissionIssueKind::UnsupportedTextVocabulary;
             });
}

[[nodiscard]] bool
specializedBodyTextIsReady(const LoweredProgram &program, CppStandard standard,
                           const CppMirBodyEmissionMapRows &baseRows,
                           const CppMirBodyEmissionAnalysis &analysis) {
  if (!hasOnlyUnsupportedTextVocabulary(analysis)) {
    return false;
  }

  CppMirBodyEmissionMapRows rows = baseRows;
  std::optional<std::size_t> overlayCount;
  switch (analysis.body.kind) {
  case MirBodyKind::Function: {
    const MirFunctionInstance *instance =
        program.mir().findFunctionInstance(analysis.body.owner);
    const LoweredFunctionDeclaration *declaration =
        instance == nullptr
            ? nullptr
            : program.findFunctionDeclaration(instance->declaration);
    if (instance == nullptr || declaration == nullptr ||
        instance->definitionKind != MirDefinitionKind::Source) {
      return false;
    }
    overlayCount = cppMirApplyCallableTemplateTypeOverlays(
        rows, program, standard, *declaration, *instance);
    if (!overlayCount || *overlayCount == 0) {
      return false;
    }
    const CppMirBodyEmissionMap overlay(std::move(rows));
    const CppMirBodyEmitter emitter(program.mir(), overlay);
    const CppMirBodyEmissionAnalysis overlayAnalysis =
        emitter.analyze(analysis.body);
    if (!overlayAnalysis.ready()) {
      return false;
    }
    const bool failure = emitter.supportsFailureBodyText(analysis.body);
    const bool plain = emitter.supportsBodyText(analysis.body);
    return instance->mayRaiseDefinedFailure ? failure || plain : plain;
  }
  case MirBodyKind::Constructor: {
    const MirConstructorInstance *instance =
        program.mir().findConstructorInstance(analysis.body.owner);
    const LoweredConstructorInstance *source =
        instance == nullptr ? nullptr
                            : program.findConstructorInstance(instance->id);
    const LoweredConstructorDeclaration *declaration =
        source == nullptr
            ? nullptr
            : program.findConstructorDeclaration(source->declaration);
    if (instance == nullptr || declaration == nullptr ||
        instance->definitionKind != MirDefinitionKind::Source ||
        !instance->mayRaiseDefinedFailure) {
      return false;
    }
    overlayCount = cppMirApplyGenericOwnerConstructorTypeOverlays(
        rows, program, *declaration, *instance);
    if (!overlayCount || *overlayCount == 0) {
      return false;
    }
    const CppMirBodyEmissionMap overlay(std::move(rows));
    const CppMirBodyEmitter emitter(program.mir(), overlay);
    return emitter.analyze(analysis.body).ready() &&
           emitter.supportsFailureBodyText(analysis.body);
  }
  case MirBodyKind::Module:
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
  case MirBodyKind::Destructor:
  case MirBodyKind::Lambda:
  case MirBodyKind::HostedStartup:
    return false;
  }
  return false;
}

} // namespace

namespace {

[[nodiscard]] CppMirBodyDefinitionKind
cppBodyDefinition(LoweredBodyDefinitionKind definition) {
  switch (definition) {
  case LoweredBodyDefinitionKind::ImplicitSource:
    return CppMirBodyDefinitionKind::ImplicitSource;
  case LoweredBodyDefinitionKind::Source:
    return CppMirBodyDefinitionKind::Source;
  case LoweredBodyDefinitionKind::CompilerGenerated:
    return CppMirBodyDefinitionKind::CompilerGenerated;
  case LoweredBodyDefinitionKind::RuntimeBinding:
    return CppMirBodyDefinitionKind::RuntimeBinding;
  case LoweredBodyDefinitionKind::Declaration:
    return CppMirBodyDefinitionKind::Declaration;
  case LoweredBodyDefinitionKind::Count:
    return CppMirBodyDefinitionKind::Count;
  }
  return CppMirBodyDefinitionKind::Count;
}

[[nodiscard]] CppMirBodyRole cppBodyRole(LoweredBodyRole role) {
  switch (role) {
  case LoweredBodyRole::SourceExecutable:
    return CppMirBodyRole::SourceExecutable;
  case LoweredBodyRole::AbiDeclaration:
    return CppMirBodyRole::AbiDeclaration;
  case LoweredBodyRole::DataOnly:
    return CppMirBodyRole::DataOnly;
  case LoweredBodyRole::Count:
    return CppMirBodyRole::Count;
  }
  return CppMirBodyRole::Count;
}

[[nodiscard]] CppMirThunkKind cppThunkKind(LoweredGeneratedItemKind kind) {
  switch (kind) {
  case LoweredGeneratedItemKind::ProgramInitialization:
    return CppMirThunkKind::ProgramInitialization;
  case LoweredGeneratedItemKind::HostedEntry:
    return CppMirThunkKind::HostedEntry;
  case LoweredGeneratedItemKind::StructuralOperatorAdapter:
    return CppMirThunkKind::StructuralOperatorAdapter;
  case LoweredGeneratedItemKind::CallableAdapter:
    return CppMirThunkKind::CallableAdapter;
  case LoweredGeneratedItemKind::LifecycleCleanup:
    return CppMirThunkKind::LifecycleCleanup;
  case LoweredGeneratedItemKind::NativeInteropAdapter:
    return CppMirThunkKind::NativeInteropAdapter;
  case LoweredGeneratedItemKind::ConcreteInstanceAdapter:
    return CppMirThunkKind::ConcreteInstanceAdapter;
  case LoweredGeneratedItemKind::Count:
    return CppMirThunkKind::Count;
  }
  return CppMirThunkKind::Count;
}

[[nodiscard]] CppMirThunkIdentity
cppThunkIdentity(const LoweredGeneratedItemIdentity &identity) {
  return {.kind = cppThunkKind(identity.kind),
          .owner = identity.owner,
          .ordinal = identity.ordinal};
}

[[nodiscard]] CppMirGeneratedThunkSourceKind
cppThunkSourceKind(LoweredGeneratedItemSourceKind kind) {
  switch (kind) {
  case LoweredGeneratedItemSourceKind::Body:
    return CppMirGeneratedThunkSourceKind::Body;
  case LoweredGeneratedItemSourceKind::Declaration:
    return CppMirGeneratedThunkSourceKind::Declaration;
  case LoweredGeneratedItemSourceKind::Count:
    return CppMirGeneratedThunkSourceKind::Count;
  }
  return CppMirGeneratedThunkSourceKind::Count;
}

[[nodiscard]] CppMirLifecycleCleanupForm
cppLifecycleCleanupForm(LoweredLifecycleCleanupForm form) {
  switch (form) {
  case LoweredLifecycleCleanupForm::OrdinaryClass:
    return CppMirLifecycleCleanupForm::OrdinaryClass;
  case LoweredLifecycleCleanupForm::ConcreteSpecialization:
    return CppMirLifecycleCleanupForm::ConcreteSpecialization;
  case LoweredLifecycleCleanupForm::Count:
    return CppMirLifecycleCleanupForm::Count;
  }
  return CppMirLifecycleCleanupForm::Count;
}

[[nodiscard]] CppMirConcreteInstanceAdapterKind
cppConcreteInstanceKind(LoweredConcreteInstanceAdapterKind kind) {
  switch (kind) {
  case LoweredConcreteInstanceAdapterKind::Function:
    return CppMirConcreteInstanceAdapterKind::Function;
  case LoweredConcreteInstanceAdapterKind::Constructor:
    return CppMirConcreteInstanceAdapterKind::Constructor;
  case LoweredConcreteInstanceAdapterKind::Count:
    return CppMirConcreteInstanceAdapterKind::Count;
  }
  return CppMirConcreteInstanceAdapterKind::Count;
}

[[nodiscard]] std::optional<CppMirDataKind>
cppSourceDataKind(LoweredDeclarationKind kind) {
  switch (kind) {
  case LoweredDeclarationKind::Namespace:
    return CppMirDataKind::NamespaceDeclaration;
  case LoweredDeclarationKind::NamespaceAlias:
    return CppMirDataKind::NamespaceAliasDeclaration;
  case LoweredDeclarationKind::TypeAlias:
    return CppMirDataKind::TypeAliasDeclaration;
  case LoweredDeclarationKind::Class:
    return CppMirDataKind::ClassDeclaration;
  case LoweredDeclarationKind::Function:
  case LoweredDeclarationKind::Constructor:
  case LoweredDeclarationKind::Destructor:
    return CppMirDataKind::CallableDeclaration;
  case LoweredDeclarationKind::Storage:
    return CppMirDataKind::StorageDeclaration;
  case LoweredDeclarationKind::Access:
    return CppMirDataKind::AccessDeclaration;
  case LoweredDeclarationKind::LanguageLinkage:
    return CppMirDataKind::LanguageLinkageDeclaration;
  case LoweredDeclarationKind::Empty:
    return CppMirDataKind::EmptyDeclaration;
  case LoweredDeclarationKind::Concept:
  case LoweredDeclarationKind::Other:
    return CppMirDataKind::OtherDeclaration;
  case LoweredDeclarationKind::Enum:
  case LoweredDeclarationKind::Count:
    return std::nullopt;
  }
  return std::nullopt;
}

void appendLoweredDeclarationRows(const LoweredProgram &program,
                                  CppMirRepresentationSnapshot &snapshot,
                                  CppMirRepresentationSnapshotBuild &build) {
  std::size_t sourceOrdinal = 0;
  std::vector<const LoweredEnumDeclaration *> enums;
  std::vector<const LoweredClassDeclaration *> classes;
  std::vector<const LoweredStorageDeclaration *> constexprBindings;

  for (const LoweredDeclaration &declaration : program.declarations()) {
    if (const auto *enumeration =
            std::get_if<LoweredEnumDeclaration>(&declaration.payload)) {
      enums.push_back(enumeration);
    }
    if (const auto *classDeclaration =
            std::get_if<LoweredClassDeclaration>(&declaration.payload)) {
      classes.push_back(classDeclaration);
    }
    if (const auto *storage =
            std::get_if<LoweredStorageDeclaration>(&declaration.payload);
        storage != nullptr && storage->constexprStorage) {
      constexprBindings.push_back(storage);
    }

    const std::optional<CppMirDataKind> kind =
        cppSourceDataKind(declaration.kind);
    if (!kind) {
      if (declaration.kind == LoweredDeclarationKind::Count) {
        addIssue(build,
                 CppMirRepresentationSnapshotIssueKind::InvalidLoweredProgram,
                 "lowered declaration inventory contains a sentinel kind");
      }
      continue;
    }
    ++sourceOrdinal;
    const std::size_t identity = declaration.semanticIdentity == 0
                                     ? sourceOrdinal
                                     : declaration.semanticIdentity;
    snapshot.data.push_back({.identity = {.kind = *kind,
                                          .declaration = identity,
                                          .owner = declaration.ownerClass,
                                          .ordinal = sourceOrdinal},
                             .support = CppMirSurfaceSupport::Supported});

    bool callableTemplate = false;
    if (const auto *function =
            std::get_if<LoweredFunctionDeclaration>(&declaration.payload)) {
      callableTemplate = !function->genericParameters.empty();
    } else if (const auto *constructor =
                   std::get_if<LoweredConstructorDeclaration>(
                       &declaration.payload)) {
      callableTemplate = !constructor->genericParameters.empty();
    }
    if (callableTemplate) {
      snapshot.data.push_back(
          {.identity = {.kind = CppMirDataKind::CallableTemplateDeclaration,
                        .declaration = identity,
                        .owner = declaration.ownerClass,
                        .ordinal = sourceOrdinal},
           .support = CppMirSurfaceSupport::Supported});
    }
  }

  for (const LoweredEnumDeclaration *enumeration : enums) {
    if (enumeration == nullptr || enumeration->id == 0) {
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::InvalidLoweredProgram,
               "lowered enum declaration lacks an exact identity");
      continue;
    }
    snapshot.data.push_back(
        {.identity = {.kind = CppMirDataKind::EnumDefinition,
                      .declaration = enumeration->id},
         .support = CppMirSurfaceSupport::Supported});
  }

  for (const LoweredClassDeclaration *classDeclaration : classes) {
    if (classDeclaration == nullptr || classDeclaration->id == 0) {
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::InvalidLoweredProgram,
               "lowered class declaration lacks an exact identity");
      continue;
    }
    const bool abi = classDeclaration->cAbiRecord ||
                     classDeclaration->cOpaqueHandle ||
                     classDeclaration->kind == ClassKind::Union ||
                     classDeclaration->unionLayout.has_value();
    if (abi) {
      snapshot.data.push_back(
          {.identity = {.kind = CppMirDataKind::AbiTypeDeclaration,
                        .declaration = classDeclaration->id},
           .support = CppMirSurfaceSupport::Supported});
    } else if (!classDeclaration->genericParameters.empty()) {
      snapshot.data.push_back(
          {.identity = {.kind = CppMirDataKind::ClassTemplateDeclaration,
                        .declaration = classDeclaration->id},
           .support = CppMirSurfaceSupport::Supported});
    }
  }

  for (std::size_t index = 0; index < constexprBindings.size(); ++index) {
    const LoweredStorageDeclaration &storage = *constexprBindings[index];
    snapshot.data.push_back(
        {.identity = {.kind = CppMirDataKind::ConstexprBinding,
                      .declaration = storage.symbol,
                      .owner = storage.ownerClass,
                      .ordinal = index + 1},
         .support = CppMirSurfaceSupport::Supported});
  }
}

} // namespace

CppMirRepresentationSnapshotBuild
buildCppMirRepresentationSnapshot(const LoweredProgram &program,
                                  CppStandard standard) {
  CppMirRepresentationSnapshotBuild build;
  const std::vector<LoweredProgramIssue> loweredIssues =
      verifyLoweredProgram(program);
  if (!loweredIssues.empty()) {
    addIssue(build,
             CppMirRepresentationSnapshotIssueKind::InvalidLoweredProgram,
             "invalid LoweredProgram: " + loweredIssues.front().detail);
    return build;
  }

  CppMirRepresentationSnapshot snapshot;
  snapshot.mir = program.mir();
  for (const LoweredBody &body : program.bodies()) {
    // The C++-private plan predates constructor declaration identities in the
    // shared lowered boundary. Preserve its established identity contract
    // until the private plan itself is retired.
    const std::size_t declaration =
        body.identity.address.kind == MirBodyKind::Constructor
            ? 0
            : body.identity.declaration;
    CppMirBodyRepresentation row{
        .identity = {.address = body.identity.address,
                     .placeDomain = body.identity.placeDomain,
                     .definition = cppBodyDefinition(body.identity.definition),
                     .declaration = declaration,
                     .concreteOwner = body.identity.concreteOwner},
        .role = cppBodyRole(body.role),
        .family = body.role == LoweredBodyRole::SourceExecutable
                      ? CppMirExecutionFamily::GeneralV1
                      : CppMirExecutionFamily::None};
    row.requiredThunks.reserve(body.requiredGeneratedItems.size());
    for (const LoweredGeneratedItemIdentity &required :
         body.requiredGeneratedItems) {
      row.requiredThunks.push_back(cppThunkIdentity(required));
    }
    snapshot.bodies.push_back(std::move(row));
  }

  appendLoweredDeclarationRows(program, snapshot, build);
  for (const LoweredDeclaration &declaration : program.declarations()) {
    if (declaration.requiredGeneratedItems.empty()) {
      continue;
    }
    CppMirDeclarationThunkRoots roots{.declaration = declaration.id};
    roots.requiredThunks.reserve(declaration.requiredGeneratedItems.size());
    for (const LoweredGeneratedItemIdentity &required :
         declaration.requiredGeneratedItems) {
      roots.requiredThunks.push_back(cppThunkIdentity(required));
    }
    snapshot.declarationRoots.push_back(std::move(roots));
  }
  for (const LoweredGeneratedItem &item : program.generatedItems()) {
    CppMirGeneratedThunk thunk{.identity = cppThunkIdentity(item.identity),
                               .sourceKind =
                                   cppThunkSourceKind(item.sourceKind),
                               .sourceBody = item.sourceBody,
                               .sourceDeclaration = item.sourceDeclaration,
                               .support = CppMirSurfaceSupport::Supported};
    thunk.dependencies.reserve(item.dependencies.size());
    for (const LoweredGeneratedItemIdentity &dependency : item.dependencies) {
      thunk.dependencies.push_back(cppThunkIdentity(dependency));
    }
    if (const auto *adapter =
            std::get_if<LoweredStructuralOperatorAdapterItem>(&item.payload)) {
      thunk.payload = CppMirStructuralOperatorThunk{
          .function = adapter->function, .operation = adapter->operation};
    } else if (const auto *adapter =
                   std::get_if<LoweredCallableAdapterItem>(&item.payload)) {
      thunk.payload = CppMirCallableThunk{.function = adapter->function,
                                          .capability = adapter->capability};
    } else if (const auto *cleanup =
                   std::get_if<LoweredLifecycleCleanupItem>(&item.payload)) {
      thunk.payload = CppMirLifecycleCleanupThunk{
          .ownerClass = cleanup->ownerClass,
          .classInstance = cleanup->classInstance,
          .destructorInstance = cleanup->destructorInstance,
          .form = cppLifecycleCleanupForm(cleanup->form),
          .mayRaiseDefinedFailure = cleanup->mayRaiseDefinedFailure};
    } else if (const auto *concrete =
                   std::get_if<LoweredConcreteInstanceAdapterItem>(
                       &item.payload)) {
      thunk.payload = CppMirConcreteInstanceThunk{
          .kind = cppConcreteInstanceKind(concrete->kind),
          .body = concrete->body,
          .declaration = concrete->declaration,
          .ownerClassInstance = concrete->ownerClassInstance,
          .mayRaiseDefinedFailure = concrete->mayRaiseDefinedFailure};
    } else if (const auto *callback =
                   std::get_if<LoweredNativeCallbackItem>(&item.payload)) {
      thunk.payload = CppMirNativeCallbackThunk{.adapter = callback->adapter};
    }
    snapshot.thunks.push_back(std::move(thunk));
  }

  if (build.issues.empty()) {
    const CppMirBodyEmissionMapRows emissionRows =
        buildCppMirBodyEmissionMapRows(program, standard);
    const CppMirBodyEmissionMap emissionMap(emissionRows);
    const CppMirProgramEmissionAnalysis emission =
        CppMirBodyEmitter(program.mir(), emissionMap).analyzeProgram();
    bool emissionReady = emission.ready();
    const CppMirBodyEmissionIssue *unresolvedIssue = nullptr;
    if (!emissionReady) {
      emissionReady = emission.issues.empty();
      for (const CppMirBodyEmissionAnalysis &body : emission.bodies) {
        if (body.ready()) {
          continue;
        }
        if (!specializedBodyTextIsReady(program, standard, emissionRows,
                                        body)) {
          emissionReady = false;
          if (!body.issues.empty()) {
            unresolvedIssue = &body.issues.front();
          }
          break;
        }
      }
    }
    if (!emissionReady) {
      std::string detail =
          "verified MIR program is outside the complete C++ text vocabulary";
      if (unresolvedIssue == nullptr && !emission.issues.empty()) {
        unresolvedIssue = &emission.issues.front();
      }
      if (unresolvedIssue != nullptr) {
        detail += ": " + unresolvedIssue->detail + " (body-kind " +
                  std::to_string(
                      static_cast<std::size_t>(unresolvedIssue->body.kind)) +
                  " owner " + std::to_string(unresolvedIssue->body.owner) + ')';
      }
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::UnsupportedMirEmission,
               std::move(detail));
    }
  }

  if (build.issues.empty()) {
    CppMirRepresentationSnapshotBuilderAccess::seal(snapshot);
    build.snapshot = std::move(snapshot);
  }
  return build;
}

CppMirBackendProgramRoute
selectCppMirBackendProgramRoute(const CppMirProgramPlan &plan) {
  if (!plan.complete()) {
    std::string detail;
    if (!plan.issues.empty()) {
      detail = plan.issues.front().detail;
    } else if (!plan.unsupported.empty()) {
      detail = "sealed representation plan contains an unsupported surface";
    } else {
      detail = "unknown representation-plan failure";
    }
    throw std::logic_error(
        "C++ backend requires a complete verified-MIR representation plan: " +
        detail);
  }
  return CppMirBackendProgramRoute::VerifiedMir;
}

namespace {

struct RowClassFacts {
  std::string name;
  bool passiveCAbiRecord = false;
  SpecialMemberStatus defaultConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyAssignment = SpecialMemberStatus::Deleted;
};

struct RowEnumVariantFacts {
  std::size_t index = 0;
  std::vector<SemanticType> fieldTypes;
};

struct RowEnumFacts {
  SemanticType underlyingType = SemanticType::Unknown;
  bool payload = false;
  std::vector<RowEnumVariantFacts> variants;
};

struct RowSymbolFacts {
  SymbolKind kind = SymbolKind::LocalVariable;
  std::string name;
  std::string qualifiedName;
  SemanticType type = SemanticType::Unknown;
  bool internalLinkage = false;
};

struct RowFunctionFacts {
  std::string name;
  std::vector<std::string> namespaceScope;
  std::vector<LoweredGenericParameter> genericParameters;
  std::vector<SemanticType> parameterTypes;
  std::string spelling;
  bool cLinkage = false;
  bool runtimeBinding = false;
};

class RowFactProvider final {
public:
  explicit RowFactProvider(const LoweredProgram &lowered) : lowered(lowered) {}

  [[nodiscard]] std::string typeSpelling(CppStandard standard,
                                         const SemanticType &type) const {
    return cppSemanticTypeSpelling(lowered, standard, type);
  }

  [[nodiscard]] std::optional<RowClassFacts> classFacts(ClassId id) const {
    const LoweredClassDeclaration *info = lowered.findClassDeclaration(id);
    if (info == nullptr) {
      return std::nullopt;
    }
    const std::size_t separator = info->qualifiedName.rfind("::");
    return RowClassFacts{
        .name = separator == std::string::npos
                    ? info->qualifiedName
                    : info->qualifiedName.substr(separator + 2),
        .passiveCAbiRecord = info->cAbiRecord && info->cAbiLayout &&
                             !info->cOpaqueHandle && !info->unionLayout,
        .defaultConstructor = info->defaultConstructor,
        .copyConstructor = info->copyConstructor,
        .moveAssignment = info->moveAssignment,
        .copyAssignment = info->copyAssignment};
  }

  [[nodiscard]] std::optional<RowEnumFacts> enumFacts(EnumId id) const {
    RowEnumFacts result;
    const LoweredEnumDeclaration *info = lowered.findEnumDeclaration(id);
    if (info == nullptr) {
      return std::nullopt;
    }
    result.underlyingType = info->underlyingType;
    result.payload = info->payload;
    result.variants.reserve(info->enumerators.size());
    for (const LoweredEnumerator &enumerator : info->enumerators) {
      result.variants.push_back({.index = enumerator.variantIndex,
                                 .fieldTypes = enumerator.payloadTypes});
    }
    return result;
  }

  [[nodiscard]] std::optional<RowSymbolFacts> symbolFacts(SymbolId id) const {
    const LoweredSymbol *record = lowered.findSymbol(id);
    if (record == nullptr) {
      return std::nullopt;
    }
    return RowSymbolFacts{.kind = record->kind,
                          .name = record->name,
                          .qualifiedName = record->qualifiedName,
                          .type = record->type,
                          .internalLinkage = record->internalLinkage};
  }

  [[nodiscard]] std::optional<RowFunctionFacts>
  functionFacts(FunctionId id) const {
    for (const LoweredDeclaration &declaration : lowered.declarations()) {
      const auto *info =
          std::get_if<LoweredFunctionDeclaration>(&declaration.payload);
      if (info == nullptr || info->id != id) {
        continue;
      }
      RowFunctionFacts result{
          .name = declaration.name,
          .namespaceScope = declaration.namespaceScope,
          .genericParameters = info->genericParameters,
          .spelling = cppFunctionSpelling(*info, declaration.name),
          .cLinkage = info->linkage == LanguageLinkage::C,
          .runtimeBinding =
              info->definitionKind == MirDefinitionKind::RuntimeBinding};
      result.parameterTypes.reserve(info->parameters.size());
      for (const LoweredParameter &parameter : info->parameters) {
        result.parameterTypes.push_back(parameter.type);
      }
      return result;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::string>
  lambdaCaptureName(std::size_t instanceId, SymbolId symbol,
                    std::size_t index) const {
    if (instanceId == 0 || instanceId > lowered.lambdaInstances().size()) {
      return std::nullopt;
    }
    const LoweredLambdaInstance &instance =
        lowered.lambdaInstances()[instanceId - 1];
    if (index >= instance.captures.size() ||
        instance.captures[index].symbol != symbol ||
        instance.captures[index].name.empty()) {
      return std::nullopt;
    }
    return instance.captures[index].name;
  }

private:
  const LoweredProgram &lowered;
};

// Deterministic first-seen type-row collection with the exact argument
// closure the emission analysis recurses through.
struct RowsBuilder {
  const RowFactProvider &facts;
  const MirProgram &mir;
  CppStandard standard;
  CppMirBodyEmissionMapRows rows;

  void addType(const SemanticType &type) {
    if (type == SemanticType::Unknown) {
      return;
    }
    // A C++ closure type is unnameable: any row for a Lambda-kind type
    // would carry a forged spelling. The closure-chain and template
    // vocabularies own these types row-free (a template emission injects
    // its own overlay row spelling the template parameter name).
    if (type.kind == SemanticType::Lambda) {
      return;
    }
    const std::optional<CppMirTypeRepresentationKind> kind =
        cppMirExpectedTypeRepresentation(type);
    if (kind && std::none_of(rows.types.begin(), rows.types.end(),
                             [&](const CppMirTypeRepresentation &row) {
                               return row.type == type;
                             })) {
      // A Class row carries the 0.215 boundary proof: a usable default
      // constructor and move assignment let a value of the type declare
      // in the prelude and receive its construction by assignment.
      bool boundaryConstructible = false;
      bool copyable = false;
      std::string templateNameSpelling;
      if (type.kind == SemanticType::Class) {
        const std::optional<RowClassFacts> classInfo =
            facts.classFacts(type.classId);
        boundaryConstructible =
            classInfo &&
            (classInfo->passiveCAbiRecord ||
             (classInfo->defaultConstructor != SpecialMemberStatus::Deleted &&
              classInfo->moveAssignment != SpecialMemberStatus::Deleted));
        copyable =
            classInfo &&
            (classInfo->passiveCAbiRecord ||
             (classInfo->copyConstructor != SpecialMemberStatus::Deleted &&
              classInfo->copyAssignment != SpecialMemberStatus::Deleted));
        if (!type.arguments.empty() || !type.valueArguments.empty()) {
          SemanticType primary = type;
          primary.arguments.clear();
          primary.valueArguments.clear();
          templateNameSpelling = facts.typeSpelling(standard, primary);
        }
      }
      rows.types.push_back(
          {.type = type,
           .kind = *kind,
           .spelling = facts.typeSpelling(standard, type),
           .templateNameSpelling = std::move(templateNameSpelling),
           .boundaryConstructible = boundaryConstructible,
           .copyable = copyable});
      // An enum type's declaration authority is its enum row; copy it
      // alongside the type row so a boundary that carries the enum can
      // prove its variant inventory.
      if (type.kind == SemanticType::Enum) {
        addEnum(type.enumId);
      }
    }
    for (const SemanticType &argument : type.arguments) {
      addType(argument);
    }
    for (const SemanticType &argument : type.lambdaEnclosingClassTypes) {
      addType(argument);
    }
    for (const SemanticType &argument : type.lambdaEnclosingFunctionTypes) {
      addType(argument);
    }
  }

  void addEnum(EnumId owner) {
    if (owner == 0 || std::any_of(rows.enums.begin(), rows.enums.end(),
                                  [owner](const CppMirEnumRepresentation &row) {
                                    return row.owner == owner;
                                  })) {
      return;
    }
    const std::optional<RowEnumFacts> info = facts.enumFacts(owner);
    if (!info) {
      return;
    }
    SemanticType type;
    type.kind = SemanticType::Enum;
    type.enumId = owner;
    CppMirEnumRepresentation row{.owner = owner,
                                 .spelling = facts.typeSpelling(standard, type),
                                 .underlyingType = info->underlyingType};
    // A payload enum's row copies the compatibility record layout: each
    // enumerator is a __gti_variant_<index> alternative of the __gti_value
    // variant, holding its payload fields in declaration order.
    if (info->payload) {
      for (const RowEnumVariantFacts &enumerator : info->variants) {
        row.payloadVariants.push_back(
            {.index = enumerator.index,
             .spelling = "__gti_variant_" + std::to_string(enumerator.index),
             .fieldTypes = enumerator.fieldTypes});
        for (const SemanticType &field : enumerator.fieldTypes) {
          addType(field);
        }
      }
    }
    rows.enums.push_back(std::move(row));
    addType(type);
    addType(info->underlyingType);
  }

  void addOperand(const MirOperand &operand) { addType(operand.type); }

  void addBody(const MirBody &body) {
    for (const MirPlace &place : body.places) {
      addType(place.type);
    }
    for (const MirValue &value : body.values) {
      addType(value.info.type);
    }
    addType(body.returnType);
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        addType(instruction.info.type);
        for (const SemanticType &type : instruction.parameterTypes) {
          addType(type);
        }
        for (const SemanticType &type : instruction.closureCaptureTypes) {
          addType(type);
        }
        if (instruction.receiver) {
          addOperand(*instruction.receiver);
        }
        for (const MirOperand &operand : instruction.operands) {
          addOperand(operand);
        }
        if (instruction.enumOwner) {
          addEnum(*instruction.enumOwner);
        }
      }
      if (block.terminator.value) {
        addOperand(*block.terminator.value);
      }
      for (const MirSwitchTarget &target : block.terminator.switchTargets) {
        if (!target.value) {
          continue;
        }
        addType(target.value->type);
        if (target.value->enumOwner != 0) {
          addEnum(target.value->enumOwner);
        }
      }
    }
  }

  void addContainedConstructors(const MirBody &body) {
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Construct ||
            !instruction.constructorTarget ||
            instruction.constructorKind != ConstructorKind::Ordinary) {
          continue;
        }
        const MirConstructorInstance *constructor =
            mir.findConstructorInstance(*instruction.constructorTarget);
        const MirClassInstance *owner =
            constructor == nullptr ? nullptr
                                   : mir.findClassInstance(constructor->owner);
        if (constructor == nullptr || owner == nullptr ||
            constructor->definitionKind != MirDefinitionKind::Source ||
            !constructor->mayRaiseDefinedFailure ||
            std::any_of(
                rows.containedConstructors.begin(),
                rows.containedConstructors.end(),
                [&](const CppMirContainedConstructorRepresentation &row) {
                  return row.constructor == constructor->id;
                })) {
          continue;
        }
        rows.containedConstructors.push_back(
            {.constructor = constructor->id,
             .ownerType = owner->type,
             .parameterTypes = constructor->parameterTypes,
             .tagSpelling =
                 cppMirContainedConstructorTagSpelling(constructor->id),
             .stateSpelling = "::gti_internal::backend::"
                              "mir_contained_constructor_state_v1"});
      }
    }
  }
};

} // namespace

static CppMirBodyEmissionMapRows buildCppMirBodyEmissionMapRowsFromFacts(
    const RowFactProvider &facts, const MirProgram &mir, CppStandard standard) {
  RowsBuilder builder{.facts = facts, .mir = mir, .standard = standard};

  for (const MirBodyAddress address : enumerateMirBodyAddresses(mir)) {
    if (const MirBody *body = findMirBody(mir, address)) {
      builder.addBody(*body);
      if (address.kind == MirBodyKind::FieldInitializers ||
          address.kind == MirBodyKind::StaticFieldInitializers) {
        builder.addContainedConstructors(*body);
      }
    }
  }

  // The emission analysis also requires instance-level owner metadata types
  // that need not appear inside any body structure: signatures, owner class
  // types, constructor initializer targets, and lambda captures.
  for (const MirClassInstance &instance : mir.classInstances()) {
    builder.addType(instance.type);
  }
  for (const MirFunctionInstance &function : mir.functionInstances()) {
    builder.addType(function.returnType);
    for (const SemanticType &type : function.parameterTypes) {
      builder.addType(type);
    }
    for (const MirCallableParameter &parameter : function.callableParameters) {
      builder.addType(parameter.callableType);
      for (const MirCallableSignature &signature : parameter.signatures) {
        builder.addType(signature.returnType);
        for (const SemanticType &type : signature.parameterTypes) {
          builder.addType(type);
        }
      }
    }
  }
  for (const MirConstructorInstance &constructor : mir.constructorInstances()) {
    for (const SemanticType &type : constructor.parameterTypes) {
      builder.addType(type);
    }
    for (const MirConstructorInitializer &initializer :
         constructor.initializers) {
      builder.addType(initializer.targetType);
    }
    // A constructor's emitted name is its owner type's spelling; a
    // destructor's appends the source class name. Declarations stay rowless.
    if (constructor.definitionKind == MirDefinitionKind::Source) {
      if (const MirClassInstance *owner =
              mir.findClassInstance(constructor.owner)) {
        builder.rows.bodies.push_back(
            {.address = {.kind = MirBodyKind::Constructor,
                         .owner = constructor.id},
             .spelling = facts.typeSpelling(standard, owner->type)});
      }
    }
  }
  for (const MirDestructorInstance &destructor : mir.destructorInstances()) {
    if (destructor.definitionKind != MirDefinitionKind::Source) {
      continue;
    }
    const MirClassInstance *owner = mir.findClassInstance(destructor.owner);
    const std::optional<RowClassFacts> info =
        owner == nullptr ? std::nullopt : facts.classFacts(owner->declaration);
    if (owner == nullptr || !info) {
      continue;
    }
    builder.rows.bodies.push_back(
        {.address = {.kind = MirBodyKind::Destructor, .owner = destructor.id},
         .spelling =
             facts.typeSpelling(standard, owner->type) + "::~" + info->name});
  }

  // Capture name rows: each lambda instance's captures spell exactly the
  // source capture names the compatibility literal prints, matched from
  // the semantic capture list by binding symbol.
  for (const MirLambdaInstance &lambda : mir.lambdaInstances()) {
    for (std::size_t index = 0; index < lambda.captureSymbols.size(); ++index) {
      const SymbolId symbol = lambda.captureSymbols[index];
      if (symbol == 0 || index >= lambda.captureTypes.size()) {
        continue;
      }
      const std::optional<std::string> name =
          facts.lambdaCaptureName(lambda.id, symbol, index);
      if (name) {
        builder.rows.symbols.push_back(
            {.kind = CppMirSymbolRepresentationKind::Capture,
             .owner = lambda.id,
             .symbol = symbol,
             .ordinal = index + 1,
             .type = lambda.captureTypes[index],
             .spelling = *name});
      }
    }
  }

  // A verified no-argument hosted-startup body's emitted name is the
  // program entry adapter itself.
  if ((cppMirHostedStartupNoArgumentsSchedule(mir) ||
       cppMirHostedStartupFailureFreeSchedule(mir)) &&
      mir.hostedStartupPlan()) {
    builder.rows.bodies.push_back(
        {.address = {.kind = MirBodyKind::HostedStartup,
                     .owner = mir.hostedStartupPlan()->entry},
         .spelling = "::main"});
  }

  // The executable module body carries its own name row like every other
  // executable body; it is never a call target.
  if (!mir.programInitializationPlan().steps.empty()) {
    builder.rows.bodies.push_back(
        {.address = {.kind = MirBodyKind::Module, .owner = 0},
         .spelling = "::__gti_program::__gti_module_initialization"});
  }

  // The sealed helpers the emitted artifact actually ships today. Naming
  // them here is what lets bodies that need them become analysis-Ready; the
  // text step's vocabulary still decides what actually emits.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::LifetimeStorage,
       .spelling = "::gti_internal::backend::mir_lifetime_slot"});
  // Program initialization names the shipped representation of the
  // merged module schedule: every staged step's definition carries its
  // initializer in the emitted declaration surface, so C++ static
  // initialization in plan order performs the module body's work and
  // the hosted CallProgramInitialization operation stages nothing.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::ProgramInitialization,
       .spelling = "cpp_static_initialization_v1"});
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::DefinedFailure,
       .spelling = "mir_failure_status_v1"});
  // Native interop names the artifact's shipped C-symbol surface: every
  // `gti_rt_*` call a MIR body can emit targets a prototype the artifact
  // declares (runtime/include/gti/runtime.h) and links from gti_runtime,
  // spelled identically to compatibility emission through its body-name
  // row. Intrinsic names the shipped `::gti_internal::backend` helper
  // family the compatibility path lowers intrinsics through; the text
  // vocabulary still declines any intrinsic call it cannot spell, so the
  // row moves analysis honesty, not emission.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::NativeInterop,
       .spelling = "gti_rt_c_symbols_v1"});
  // Bounds names the shipped checked fixed-array access family: the
  // mir_checked_array_read/write_v1 helpers carry the bounds check, the
  // status protocol, and the exact INDEX_OUT_OF_BOUNDS record data.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Bounds,
       .spelling = "mir_checked_array_access_v1"});
  // Borrow names the loan erasure contract (ADR 018): loans lower to
  // typed pointers with deref-at-use, constness follows access mode,
  // endpoints are compile-proven with no runtime action, and references
  // survive only at ABI boundaries.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Borrow,
       .spelling = "mir_loan_pointer_v1"});
  // RawMemory names the direct C++ raw-pointer vocabulary. Unsafe permission,
  // pointee access, and operation types are already fixed by semantics and
  // verified MIR; the backend spells only one-level address formation,
  // pointer arithmetic/difference, and raw index/dereference lvalue views.
  // There is no owning runtime primitive or inferred safety check here.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::RawMemory,
       .spelling = "cpp_raw_pointer_operations_v1"});
  // Aggregate names the value-initialization representation the emitted
  // artifact uses for empty aggregates: the row type's braced value
  // initialization (std::array<T, N>{}).
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Aggregate,
       .spelling = "cpp_value_initialization_v1"});
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Intrinsic,
       .spelling = "gti_internal_backend_helpers_v1"});
  // HostedEntry names the shipped program-entry adapter: every hosted
  // artifact carries the generated main() that marshals arguments,
  // forwards the entry call, and routes defined failure through the
  // terminal containment contract. The entry function's own body is
  // ordinary; this row is what its analysis demand resolves against.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::HostedEntry,
       .spelling = "gti_hosted_entry_adapter_v1"});
  // Expected names the artifact's shipped expected representation. The
  // row's spelling is the exact error-construction call the compatibility
  // path emits for the selected standard — std::unexpected under C++23,
  // the vendored expected-lite constructor under C++20 — so Unexpected
  // text is copied from this row and can never drift by standard.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Expected,
       .spelling = standard == CppStandard::Cpp23
                       ? "std::unexpected"
                       : "::nonstd::make_unexpected"});
  // Closure names the inline C++ lambda literal representation: a Closure
  // compute spells `[name = <place>, ...](<params>) -> <ret> { <verified
  // body> }` with capture names copied from the lambda's Capture rows,
  // exactly like the compatibility path's inline lambda emission. C++
  // closure types are unnameable, so the chain fuses into its consuming
  // invocations and nothing here is ever a spelled type or call target.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Closure,
       .spelling = "cpp_inline_lambda_v1"});
  // CallableDispatch names the deduction-based callable representation:
  // a callable value invokes as `<literal>(<args>)` and a callable-typed
  // argument passes by template-argument deduction, never through a
  // spelled closure type name. The text vocabulary still declines every
  // callable shape it cannot fuse, so the row moves analysis honesty,
  // not emission.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::CallableDispatch,
       .spelling = "cpp_deduced_callable_v1"});
  // Payload names the variant record representation the artifact ships
  // for payload enums: a struct wrapping std::variant of per-enumerator
  // __gti_variant_<index> records, constructed by wrapping the variant
  // record and read by std::get over the proven alternative. The enum
  // row's copied variant inventory carries the exact spellings.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Payload,
       .spelling = "cpp_variant_record_v1"});
  // VirtualDispatch names the C++ virtual member representation: a
  // virtual or override method's emitted in-class definition carries the
  // language's own dispatch, and a virtual call spells the ordinary
  // member call through the base-typed receiver. The text vocabulary
  // still declines any dispatch shape it cannot spell.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::VirtualDispatch,
       .spelling = "cpp_virtual_member_v1"});

  // Executable per-instance field-initializer bodies carry their own name
  // row like every other executable body; the spelling is the owner scope
  // of the definitions the transitional emitter writes for them. They are
  // never call targets.
  for (const MirClassInstance &instance : mir.classInstances()) {
    const MirBody &fieldBody = instance.fieldInitializers;
    const bool executable =
        !fieldBody.blocks.empty() &&
        std::any_of(
            fieldBody.blocks.begin(), fieldBody.blocks.end(),
            [](const MirBlock &block) { return !block.instructions.empty(); });
    if (!executable) {
      continue;
    }
    builder.rows.bodies.push_back(
        {.address = {.kind = MirBodyKind::FieldInitializers,
                     .owner = instance.id},
         .spelling = facts.typeSpelling(standard, instance.type) +
                     "::__gti_field_initializers"});
  }
  for (const MirLambdaInstance &lambda : mir.lambdaInstances()) {
    // A lambda body is never a call target: it spells only nested inside
    // its closure literal. The row exists so a Closure site can prove the
    // exact body target is representable before fusing it.
    builder.rows.bodies.push_back(
        {.address = {.kind = MirBodyKind::Lambda, .owner = lambda.id},
         .spelling = "__gti_inline_lambda_" + std::to_string(lambda.id)});
    builder.addType(lambda.type);
    builder.addType(lambda.returnType);
    for (const SemanticType &type : lambda.parameterTypes) {
      builder.addType(type);
    }
    for (const SemanticType &type : lambda.captureTypes) {
      builder.addType(type);
    }
  }

  // Namespace-global storage rows for every Symbol-rooted place MIR reads.
  // The spelling is copied from semantic symbol identity, including nested
  // source namespaces and internal-linkage holder storage.
  for (const MirBodyAddress address : enumerateMirBodyAddresses(mir)) {
    const MirBody *body = findMirBody(mir, address);
    if (body == nullptr) {
      continue;
    }
    for (const MirPlace &place : body->places) {
      if (place.root != MirPlaceRootKind::Symbol || place.capture != 0 ||
          place.symbol == 0 || !place.projections.empty()) {
        continue;
      }
      const MirProgramInitializationStep *step =
          mir.programInitializationPlan().findStepForSymbol(place.symbol);
      if (step != nullptr && step->ownerClass != 0) {
        continue;
      }
      if (std::any_of(builder.rows.symbols.begin(), builder.rows.symbols.end(),
                      [&](const CppMirSymbolRepresentation &row) {
                        return row.kind ==
                                   CppMirSymbolRepresentationKind::Storage &&
                               row.owner == 0 && row.symbol == place.symbol;
                      })) {
        continue;
      }
      const std::optional<RowSymbolFacts> record =
          facts.symbolFacts(place.symbol);
      if (!record || record->name.empty()) {
        continue;
      }
      // An enumerator read spells the enum type's qualified constant; the
      // enum spelling is the same authority the enum rows carry. Payload
      // enums stay rowless — their variants are not plain constants.
      if (record->kind == SymbolKind::Enumerator &&
          place.type.kind == SemanticType::Enum) {
        const std::optional<RowEnumFacts> enumInfo =
            facts.enumFacts(place.type.enumId);
        const std::string enumSpelling =
            facts.typeSpelling(standard, place.type);
        if (!enumInfo || enumInfo->payload || enumSpelling.empty()) {
          continue;
        }
        builder.rows.symbols.push_back(
            {.kind = CppMirSymbolRepresentationKind::Storage,
             .owner = 0,
             .symbol = place.symbol,
             .ordinal = 0,
             .type = place.type,
             .spelling = enumSpelling + "::" + record->name});
        builder.addType(place.type);
        continue;
      }
      if (record->kind != SymbolKind::GlobalVariable) {
        continue;
      }
      std::string spelling = cppProgramStorageSpelling(*record, place.symbol);
      if (spelling.empty()) {
        continue;
      }
      builder.rows.symbols.push_back(
          {.kind = CppMirSymbolRepresentationKind::Storage,
           .owner = 0,
           .symbol = place.symbol,
           .ordinal = 0,
           .type = place.type,
           .spelling = std::move(spelling)});
      builder.addType(place.type);
    }
  }

  // Program-initialization steps for plain namespace globals also carry
  // storage rows: a constexpr global's reads are frontend-substituted, so
  // no Symbol-rooted place ever demands the row above, yet the module
  // body's own Binding places still resolve their storage through it.
  for (const MirProgramInitializationStep &step :
       mir.programInitializationPlan().steps) {
    if (step.symbol == 0) {
      continue;
    }
    // A class-owned static's storage row spells the class-qualified
    // member, exactly the compatibility definition's access path.
    if (step.ownerClass != 0) {
      const MirClassInstance *ownerInstance =
          mir.findClassInstance(step.ownerClass);
      const std::optional<RowSymbolFacts> record =
          facts.symbolFacts(step.symbol);
      const MirPlace *storage = mir.module().findPlace(step.storagePlace);
      if (ownerInstance == nullptr || !record || storage == nullptr ||
          record->name.empty() ||
          std::any_of(builder.rows.symbols.begin(), builder.rows.symbols.end(),
                      [&](const CppMirSymbolRepresentation &row) {
                        return row.kind ==
                                   CppMirSymbolRepresentationKind::Storage &&
                               row.owner == step.ownerClass &&
                               row.symbol == step.symbol;
                      })) {
        continue;
      }
      builder.rows.symbols.push_back(
          {.kind = CppMirSymbolRepresentationKind::Storage,
           .owner = step.ownerClass,
           .symbol = step.symbol,
           .ordinal = 0,
           .type = storage->type,
           .spelling = facts.typeSpelling(standard, ownerInstance->type) +
                       "::" + record->name});
      builder.addType(storage->type);
      continue;
    }
    if (std::any_of(builder.rows.symbols.begin(), builder.rows.symbols.end(),
                    [&](const CppMirSymbolRepresentation &row) {
                      return row.kind ==
                                 CppMirSymbolRepresentationKind::Storage &&
                             row.owner == 0 && row.symbol == step.symbol;
                    })) {
      continue;
    }
    const std::optional<RowSymbolFacts> record = facts.symbolFacts(step.symbol);
    const MirPlace *storage = mir.module().findPlace(step.storagePlace);
    if (!record || storage == nullptr) {
      continue;
    }
    std::string spelling = cppProgramStorageSpelling(*record, step.symbol);
    if (spelling.empty()) {
      continue;
    }
    builder.rows.symbols.push_back(
        {.kind = CppMirSymbolRepresentationKind::Storage,
         .owner = 0,
         .symbol = step.symbol,
         .ordinal = 0,
         .type = storage->type,
         .spelling = std::move(spelling)});
    builder.addType(storage->type);
  }

  for (const MirClassInstance &instance : mir.classInstances()) {
    for (const MirClassFieldInfo &field : instance.declaredFields) {
      const std::optional<RowSymbolFacts> record =
          facts.symbolFacts(field.symbol);
      if (!record || record->name.empty()) {
        continue;
      }
      std::string declarationTypeSpelling;
      if (record->type != field.type && containsDependentType(record->type)) {
        declarationTypeSpelling = facts.typeSpelling(standard, record->type);
        if (declarationTypeSpelling == "void") {
          declarationTypeSpelling.clear();
        }
      }
      builder.rows.symbols.push_back(
          {.kind = CppMirSymbolRepresentationKind::Field,
           .owner = instance.id,
           .symbol = field.symbol,
           .ordinal = 0,
           .type = field.type,
           .spelling = record->name,
           .declarationTypeSpelling = std::move(declarationTypeSpelling)});
      builder.addType(field.type);
    }
  }

  for (const MirFunctionInstance &function : mir.functionInstances()) {
    // Every function instance gets its emitted name row: source definitions
    // carry their definition spelling, runtime bindings and C-linkage
    // declarations their exact external names. Only the namespace-scope
    // source-defined GTI form is also a valid call-target spelling; the
    // scalar call family's gates admit exactly that form.
    const std::optional<RowFunctionFacts> info =
        facts.functionFacts(function.declaration);
    if (!info) {
      continue;
    }
    std::string spelling;
    if (function.owner) {
      const MirClassInstance *ownerInstance =
          mir.findClassInstance(*function.owner);
      if (ownerInstance == nullptr) {
        continue;
      }
      spelling = facts.typeSpelling(standard, ownerInstance->type);
      spelling += "::";
    } else if (!info->cLinkage && !info->runtimeBinding) {
      spelling = "::__gti_program";
      for (std::size_t index = 0; index < info->namespaceScope.size();
           ++index) {
        const std::string &scope = info->namespaceScope[index];
        spelling += "::";
        spelling += index == 0 && scope == "std"
                        ? std::string(cppEmittedStandardNamespace)
                        : scope;
      }
      spelling += "::";
    } else if (!info->namespaceScope.empty()) {
      // A C-linkage or runtime-binding declaration keeps its exact
      // external symbol, but extern "C" affects linkage, not C++ name
      // lookup: when the emitted declaration lives inside a namespace,
      // the call-target spelling must name that namespace.
      spelling = "::__gti_program";
      for (std::size_t index = 0; index < info->namespaceScope.size();
           ++index) {
        const std::string &scope = info->namespaceScope[index];
        spelling += "::";
        spelling += index == 0 && scope == "std"
                        ? std::string(cppEmittedStandardNamespace)
                        : scope;
      }
      spelling += "::";
    }
    spelling += info->spelling;
    // A generic parameter that appears in no parameter type cannot be
    // deduced at a call site, so the instance's call spelling carries its
    // substituted arguments explicitly — exactly like the compatibility
    // call site. Deducible instances keep the bare name so shipped
    // spellings stay byte-stable.
    if (!function.typeArguments.empty() && !info->genericParameters.empty()) {
      const auto mentionsParameter = [&](const GenericParameterId parameter) {
        const auto mentions = [&](const auto &self,
                                  const SemanticType &type) -> bool {
          if (type.kind == SemanticType::TypeParameter &&
              type.genericParameterId == parameter) {
            return true;
          }
          for (const SemanticType &argument : type.arguments) {
            if (self(self, argument)) {
              return true;
            }
          }
          return false;
        };
        for (const SemanticType &parameterType : info->parameterTypes) {
          if (mentions(mentions, parameterType)) {
            return true;
          }
        }
        return false;
      };
      bool needsExplicit = false;
      for (const LoweredGenericParameter &parameter : info->genericParameters) {
        if (!parameter.pack && !mentionsParameter(parameter.id)) {
          needsExplicit = true;
          break;
        }
      }
      if (needsExplicit) {
        spelling += '<';
        for (std::size_t index = 0; index < function.typeArguments.size();
             ++index) {
          if (index != 0) {
            spelling += ", ";
          }
          spelling +=
              facts.typeSpelling(standard, function.typeArguments[index]);
        }
        spelling += '>';
      }
    }
    builder.rows.bodies.push_back(
        {.address = {.kind = MirBodyKind::Function, .owner = function.id},
         .spelling = std::move(spelling)});
  }

  return builder.rows;
}

CppMirBodyEmissionMapRows
buildCppMirBodyEmissionMapRows(const LoweredProgram &program,
                               CppStandard standard) {
  const RowFactProvider facts(program);
  return buildCppMirBodyEmissionMapRowsFromFacts(facts, program.mir(),
                                                 standard);
}

} // namespace lang
