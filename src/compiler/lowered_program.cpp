#include "gti/lowered_program.h"

#include "gti/ast.h"
#include "gti/failure_metadata.h"
#include "gti/hir.h"
#include "gti/lowered_program_printer.h"
#include "gti/semantic_analyzer.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace lang {
namespace {

template <typename Enum>
[[nodiscard]] constexpr std::size_t ordinal(Enum value) {
  return static_cast<std::size_t>(value);
}

[[nodiscard]] bool generatedLess(const LoweredGeneratedItemIdentity &left,
                                 const LoweredGeneratedItemIdentity &right) {
  return std::tuple{ordinal(left.kind), left.owner, left.ordinal} <
         std::tuple{ordinal(right.kind), right.owner, right.ordinal};
}

[[nodiscard]] bool
containsGenerated(const std::vector<LoweredGeneratedItemIdentity> &items,
                  const LoweredGeneratedItemIdentity &identity) {
  return std::find(items.begin(), items.end(), identity) != items.end();
}

[[nodiscard]] const LoweredGeneratedItem *
findGenerated(const std::vector<LoweredGeneratedItem> &items,
              const LoweredGeneratedItemIdentity &identity) {
  const auto found =
      std::find_if(items.begin(), items.end(),
                   [&](const auto &item) { return item.identity == identity; });
  return found == items.end() ? nullptr : &*found;
}

[[nodiscard]] const LoweredBody *
findBody(const std::vector<LoweredBody> &bodies, MirBodyAddress address) {
  const auto found =
      std::find_if(bodies.begin(), bodies.end(), [&](const LoweredBody &body) {
        return body.identity.address == address;
      });
  return found == bodies.end() ? nullptr : &*found;
}

[[nodiscard]] LoweredBody *findBody(std::vector<LoweredBody> &bodies,
                                    MirBodyAddress address) {
  return const_cast<LoweredBody *>(findBody(std::as_const(bodies), address));
}

[[nodiscard]] LoweredBodyDefinitionKind
bodyDefinition(MirDefinitionKind definition) {
  switch (definition) {
  case MirDefinitionKind::Source:
    return LoweredBodyDefinitionKind::Source;
  case MirDefinitionKind::RuntimeBinding:
    return LoweredBodyDefinitionKind::RuntimeBinding;
  case MirDefinitionKind::Declaration:
    return LoweredBodyDefinitionKind::Declaration;
  }
  return LoweredBodyDefinitionKind::Count;
}

[[nodiscard]] bool isInitializerBody(MirBodyKind kind) {
  return kind == MirBodyKind::Module ||
         kind == MirBodyKind::FieldInitializers ||
         kind == MirBodyKind::StaticFieldInitializers;
}

[[nodiscard]] bool isCanonicalNoExecutionInitializer(const MirBody &body) {
  if (!isInitializerBody(body.kind) || body.returnType != SemanticType::Void ||
      body.entry != 1 || body.blocks.size() != 1 || !body.places.empty() ||
      !body.loans.empty() || !body.fullExpressions.empty() ||
      !body.cleanupBoundaries.empty() || !body.dropObligations.empty() ||
      !body.failureRecords.empty() || !body.values.empty() ||
      !body.valueUses.empty()) {
    return false;
  }
  MirBlock expected;
  expected.id = 1;
  expected.terminator.kind = MirTerminatorKind::Exit;
  expected.reachable = true;
  return body.blocks.front() == expected;
}

[[nodiscard]] bool
hasExecutableProgramInitialization(const MirProgram &program) {
  return std::any_of(
      program.programInitializationPlan().steps.begin(),
      program.programInitializationPlan().steps.end(), [](const auto &step) {
        return step.role == ProgramInitializationStepRole::Initializer;
      });
}

[[nodiscard]] std::size_t
programInitializationBodyCallCount(const MirProgram &program) {
  const MirBody *body = program.hostedStartup();
  if (body == nullptr) {
    return 0;
  }
  std::size_t result = 0;
  for (const MirBlock &block : body->blocks) {
    result += static_cast<std::size_t>(std::count_if(
        block.instructions.begin(), block.instructions.end(),
        [](const MirInstruction &instruction) {
          return instruction.kind == MirInstructionKind::CallBody &&
                 instruction.bodyTarget ==
                     MirBodyAddress{.kind = MirBodyKind::Module, .owner = 0};
        }));
  }
  return result;
}

[[nodiscard]] std::uint64_t fingerprint(std::string_view text) {
  constexpr std::uint64_t offset = 14695981039346656037ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  std::uint64_t result = offset;
  for (const unsigned char byte : text) {
    result ^= byte;
    result *= prime;
  }
  return result;
}

void addIssue(
    std::vector<LoweredProgramIssue> &issues, LoweredProgramIssueKind kind,
    std::string detail, std::optional<MirBodyAddress> body = std::nullopt,
    std::optional<LoweredDeclarationId> declaration = std::nullopt,
    std::optional<LoweredGeneratedItemIdentity> generated = std::nullopt) {
  issues.push_back({.kind = kind,
                    .detail = std::move(detail),
                    .body = body,
                    .declaration = declaration,
                    .generatedItem = generated});
}

[[nodiscard]] const HirClassInstance *findHirClass(const HirProgram &hir,
                                                   std::size_t id) {
  return id == 0 || id > hir.classInstances().size()
             ? nullptr
             : &hir.classInstances()[id - 1];
}

[[nodiscard]] SourceSpan bodySource(const HirProgram &hir,
                                    MirBodyAddress address) {
  switch (address.kind) {
  case MirBodyKind::Module:
    return {};
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
    if (const HirClassInstance *instance = findHirClass(hir, address.owner);
        instance != nullptr && instance->source != nullptr) {
      return tokenSpan(instance->source->name());
    }
    return {};
  case MirBodyKind::Function:
    if (const HirFunctionInstance *instance =
            hir.findFunctionInstance(address.owner);
        instance != nullptr && instance->source != nullptr) {
      return tokenSpan(instance->source->name());
    }
    return {};
  case MirBodyKind::Constructor:
    if (const HirConstructorInstance *instance =
            hir.findConstructorInstance(address.owner);
        instance != nullptr && instance->source != nullptr) {
      return tokenSpan(instance->source->name());
    }
    return {};
  case MirBodyKind::Destructor:
    if (const HirDestructorInstance *instance =
            hir.findDestructorInstance(address.owner);
        instance != nullptr && instance->source != nullptr) {
      return tokenSpan(instance->source->tilde());
    }
    return {};
  case MirBodyKind::Lambda:
    if (const HirLambda *instance = hir.findLambda(address.owner);
        instance != nullptr && instance->source != nullptr) {
      return tokenSpan(instance->source->bracket());
    }
    return {};
  case MirBodyKind::HostedStartup:
    if (const HirFunctionInstance *instance =
            hir.findFunctionInstance(address.owner);
        instance != nullptr && instance->source != nullptr) {
      return tokenSpan(instance->source->name());
    }
    return {};
  }
  return {};
}

[[nodiscard]] std::optional<LoweredBodyIdentity>
captureBodyIdentity(const MirProgram &mir, const HirProgram &hir,
                    MirBodyAddress address) {
  const MirBody *body = findMirBody(mir, address);
  if (body == nullptr || body->kind != address.kind) {
    return std::nullopt;
  }
  LoweredBodyIdentity result{.address = address,
                             .placeDomain = body->placeDomain,
                             .source = bodySource(hir, address)};
  switch (address.kind) {
  case MirBodyKind::Module:
    return address.owner == 0 ? std::optional{std::move(result)} : std::nullopt;
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers: {
    const MirClassInstance *instance = mir.findClassInstance(address.owner);
    if (instance == nullptr || instance->id != address.owner) {
      return std::nullopt;
    }
    result.declaration = instance->declaration;
    result.concreteOwner = instance->id;
    return result;
  }
  case MirBodyKind::Function: {
    const MirFunctionInstance *instance =
        mir.findFunctionInstance(address.owner);
    if (instance == nullptr || instance->id != address.owner) {
      return std::nullopt;
    }
    result.definition = bodyDefinition(instance->definitionKind);
    result.declaration = instance->declaration;
    result.concreteOwner = instance->owner.value_or(0);
    return result.definition == LoweredBodyDefinitionKind::Count
               ? std::nullopt
               : std::optional{std::move(result)};
  }
  case MirBodyKind::Constructor: {
    const MirConstructorInstance *instance =
        mir.findConstructorInstance(address.owner);
    const HirConstructorInstance *source =
        hir.findConstructorInstance(address.owner);
    if (instance == nullptr || source == nullptr ||
        instance->id != address.owner) {
      return std::nullopt;
    }
    result.definition = bodyDefinition(instance->definitionKind);
    result.declaration = source->declaration;
    result.concreteOwner = instance->owner;
    return result.definition == LoweredBodyDefinitionKind::Count
               ? std::nullopt
               : std::optional{std::move(result)};
  }
  case MirBodyKind::Destructor: {
    const MirDestructorInstance *instance =
        mir.findDestructorInstance(address.owner);
    if (instance == nullptr || instance->id != address.owner) {
      return std::nullopt;
    }
    result.definition = bodyDefinition(instance->definitionKind);
    result.concreteOwner = instance->owner;
    return result.definition == LoweredBodyDefinitionKind::Count
               ? std::nullopt
               : std::optional{std::move(result)};
  }
  case MirBodyKind::Lambda: {
    const MirLambdaInstance *instance = mir.findLambda(address.owner);
    if (instance == nullptr || instance->id != address.owner) {
      return std::nullopt;
    }
    result.definition = LoweredBodyDefinitionKind::Source;
    result.declaration = instance->declaration;
    return result;
  }
  case MirBodyKind::HostedStartup: {
    const std::optional<MirHostedStartupPlan> &startup =
        mir.hostedStartupPlan();
    if (!startup || address.owner != startup->entry ||
        mir.hostedStartup() != body) {
      return std::nullopt;
    }
    const MirFunctionInstance *entry = mir.findFunctionInstance(startup->entry);
    if (entry == nullptr || entry->entryKind == ProgramEntryKind::None) {
      return std::nullopt;
    }
    result.definition = LoweredBodyDefinitionKind::CompilerGenerated;
    result.declaration = entry->declaration;
    result.concreteOwner = entry->id;
    return result;
  }
  }
  return std::nullopt;
}

[[nodiscard]] LoweredBodyRole bodyRole(const MirProgram &program,
                                       const LoweredBodyIdentity &identity) {
  switch (identity.definition) {
  case LoweredBodyDefinitionKind::ImplicitSource: {
    if (identity.address.kind == MirBodyKind::Module) {
      return hasExecutableProgramInitialization(program)
                 ? LoweredBodyRole::SourceExecutable
                 : LoweredBodyRole::DataOnly;
    }
    const MirBody *body = findMirBody(program, identity.address);
    return body != nullptr && isCanonicalNoExecutionInitializer(*body)
               ? LoweredBodyRole::DataOnly
               : LoweredBodyRole::SourceExecutable;
  }
  case LoweredBodyDefinitionKind::Source:
  case LoweredBodyDefinitionKind::CompilerGenerated:
    return LoweredBodyRole::SourceExecutable;
  case LoweredBodyDefinitionKind::RuntimeBinding:
  case LoweredBodyDefinitionKind::Declaration:
    return LoweredBodyRole::AbiDeclaration;
  case LoweredBodyDefinitionKind::Count:
    return LoweredBodyRole::Count;
  }
  return LoweredBodyRole::Count;
}

class DeclarationCollector final {
public:
  DeclarationCollector(const SemanticModel &semantics, const TargetInfo &target)
      : semantics_(semantics), target_(target) {}

  [[nodiscard]] std::vector<LoweredDeclaration>
  collect(const StmtList &declarations) {
    collectList(declarations, 0, 0, {});
    return std::move(result_);
  }

private:
  void collectList(const StmtList &statements, LoweredDeclarationId parent,
                   ClassId ownerClass,
                   const std::vector<std::string> &namespaceScope) {
    for (const StmtPtr &statement : statements) {
      if (statement != nullptr) {
        collectStatement(*statement, parent, ownerClass, namespaceScope);
      }
    }
  }

  void collectStatement(const Stmt &statement, LoweredDeclarationId parent,
                        ClassId ownerClass,
                        const std::vector<std::string> &namespaceScope) {
    if (const auto *conditional =
            dynamic_cast<const ConditionalStmt *>(&statement)) {
      if (const StmtList *active = conditional->activeBranch(target_)) {
        collectList(*active, parent, ownerClass, namespaceScope);
      }
      return;
    }

    LoweredDeclaration row;
    row.id = result_.size() + 1;
    row.parent = parent;
    row.ownerClass = ownerClass;
    row.ordinal = ++nextOrdinal_[parent];
    row.namespaceScope = namespaceScope;

    const StmtList *children = nullptr;
    std::vector<std::string> childNamespace = namespaceScope;
    ClassId childOwner = ownerClass;
    if (const auto *declaration =
            dynamic_cast<const NamespaceDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Namespace;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      children = &declaration->declarations();
      childNamespace.push_back(row.name);
    } else if (const auto *declaration =
                   dynamic_cast<const NamespaceAliasDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::NamespaceAlias;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
    } else if (const auto *declaration =
                   dynamic_cast<const TypeAliasDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::TypeAlias;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
    } else if (const auto *declaration =
                   dynamic_cast<const ClassDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Class;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      row.generic = !declaration->genericParameters().empty();
      if (const ClassTypeInfo *info = semantics_.findClassType(*declaration)) {
        row.semanticIdentity = info->id;
        childOwner = info->id;
      }
      children = &declaration->members();
    } else if (const auto *declaration =
                   dynamic_cast<const EnumDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Enum;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      if (const EnumTypeInfo *info = semantics_.findEnumType(*declaration)) {
        row.semanticIdentity = info->id;
      }
    } else if (const auto *declaration =
                   dynamic_cast<const FunctionDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Function;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      row.generic = !declaration->genericParameters().empty();
      if (const FunctionInfo *info = semantics_.findFunction(*declaration)) {
        row.semanticIdentity = info->id;
      }
    } else if (const auto *declaration =
                   dynamic_cast<const ConstructorDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Constructor;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      row.generic = !declaration->genericParameters().empty();
      if (const ConstructorInfo *info =
              semantics_.findConstructor(*declaration)) {
        row.semanticIdentity = info->id;
      }
    } else if (const auto *declaration =
                   dynamic_cast<const DestructorDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Destructor;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->tilde());
      if (const DestructorInfo *info =
              semantics_.findDestructor(*declaration)) {
        row.semanticIdentity = info->owner;
      }
    } else if (const auto *declaration =
                   dynamic_cast<const VariableDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Storage;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      if (const BindingInfo *info = semantics_.findBinding(*declaration)) {
        row.semanticIdentity = info->symbol;
      }
    } else if (const auto *declaration =
                   dynamic_cast<const AccessSpecifierDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Access;
      row.name = declaration->keyword().lexeme;
      row.source = tokenSpan(declaration->keyword());
    } else if (const auto *declaration =
                   dynamic_cast<const ExternCDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::LanguageLinkage;
      row.name = declaration->language().lexeme;
      row.source = tokenSpan(declaration->keyword());
      children = &declaration->declarations();
    } else if (const auto *declaration =
                   dynamic_cast<const ConceptDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Concept;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      row.generic = !declaration->typeParameters().empty();
    } else if (const auto *declaration =
                   dynamic_cast<const EmptyStmt *>(&statement)) {
      row.kind = LoweredDeclarationKind::Empty;
      row.source = tokenSpan(declaration->semicolon());
    } else {
      row.kind = LoweredDeclarationKind::Other;
    }

    result_.push_back(std::move(row));
    const LoweredDeclarationId id = result_.back().id;
    if (children != nullptr) {
      collectList(*children, id, childOwner, childNamespace);
    }
  }

  const SemanticModel &semantics_;
  const TargetInfo &target_;
  std::vector<LoweredDeclaration> result_;
  std::unordered_map<LoweredDeclarationId, std::size_t> nextOrdinal_;
};

struct DerivedGeneratedGraph {
  std::vector<LoweredGeneratedItem> items;
  std::vector<
      std::pair<MirBodyAddress, std::vector<LoweredGeneratedItemIdentity>>>
      roots;
};

[[nodiscard]] DerivedGeneratedGraph
deriveGeneratedGraph(const MirProgram &program) {
  DerivedGeneratedGraph graph;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    graph.roots.emplace_back(address,
                             std::vector<LoweredGeneratedItemIdentity>{});
  }
  const auto root = [&](MirBodyAddress address,
                        LoweredGeneratedItemIdentity identity) {
    const auto found =
        std::find_if(graph.roots.begin(), graph.roots.end(),
                     [&](const auto &entry) { return entry.first == address; });
    if (found != graph.roots.end()) {
      found->second.push_back(identity);
    }
  };

  std::optional<LoweredGeneratedItemIdentity> initialization;
  if (hasExecutableProgramInitialization(program)) {
    initialization = LoweredGeneratedItemIdentity{
        .kind = LoweredGeneratedItemKind::ProgramInitialization};
    graph.items.push_back(
        {.identity = *initialization,
         .sourceBody = {.kind = MirBodyKind::Module, .owner = 0}});
    root({.kind = MirBodyKind::Module, .owner = 0}, *initialization);
  }

  if (const std::optional<MirHostedStartupPlan> &startup =
          program.hostedStartupPlan();
      startup && program.hostedStartup() != nullptr) {
    const LoweredGeneratedItemIdentity identity{
        .kind = LoweredGeneratedItemKind::HostedEntry, .owner = startup->entry};
    LoweredGeneratedItem item{.identity = identity,
                              .sourceBody = {.kind = MirBodyKind::HostedStartup,
                                             .owner = startup->entry}};
    if (initialization && programInitializationBodyCallCount(program) != 0) {
      item.dependencies.push_back(*initialization);
    }
    graph.items.push_back(std::move(item));
    root({.kind = MirBodyKind::HostedStartup, .owner = startup->entry},
         identity);
  }

  for (const MirNativeCallbackAdapter &adapter :
       program.nativeCallbackAdapters()) {
    const LoweredGeneratedItemIdentity identity{
        .kind = LoweredGeneratedItemKind::NativeInteropAdapter,
        .owner = adapter.id};
    graph.items.push_back(
        {.identity = identity,
         .sourceBody = {.kind = MirBodyKind::Function, .owner = adapter.target},
         .payload = LoweredNativeCallbackItem{.adapter = adapter}});
  }

  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    const MirBody *body = findMirBody(program, address);
    if (body == nullptr) {
      continue;
    }
    for (const MirBlock &block : body->blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.operation != MirOperation::NativeCallback ||
            !instruction.nativeCallbackAdapter) {
          continue;
        }
        root(address, {.kind = LoweredGeneratedItemKind::NativeInteropAdapter,
                       .owner = *instruction.nativeCallbackAdapter});
      }
    }
  }

  for (auto &[address, roots] : graph.roots) {
    static_cast<void>(address);
    std::sort(roots.begin(), roots.end(), generatedLess);
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  }
  for (LoweredGeneratedItem &item : graph.items) {
    std::sort(item.dependencies.begin(), item.dependencies.end(),
              generatedLess);
    item.dependencies.erase(
        std::unique(item.dependencies.begin(), item.dependencies.end()),
        item.dependencies.end());
  }
  std::sort(
      graph.items.begin(), graph.items.end(),
      [](const LoweredGeneratedItem &left, const LoweredGeneratedItem &right) {
        return generatedLess(left.identity, right.identity);
      });
  return graph;
}

[[nodiscard]] bool frontendInstancesMatch(const SemanticModel &semantics,
                                          const HirProgram &hir,
                                          const MirProgram &mir,
                                          std::string &detail) {
  const auto reject = [&](std::string message) {
    detail = std::move(message);
    return false;
  };
  if (semantics.analysisSeal() != hir.analysisSeal()) {
    return reject("semantic and HIR analyzed-program seals differ");
  }
  if (semantics.executionProfile() != hir.executionProfile() ||
      hir.executionProfile() != mir.executionProfile() ||
      semantics.placeSnapshot() != hir.module().placeDomain.snapshot ||
      hir.module().placeDomain != mir.module().placeDomain ||
      hir.classInstances().size() != mir.classInstances().size() ||
      hir.functionInstances().size() != mir.functionInstances().size() ||
      hir.nativeCallbackAdapters().size() !=
          mir.nativeCallbackAdapters().size() ||
      hir.constructorInstances().size() != mir.constructorInstances().size() ||
      hir.destructorInstances().size() != mir.destructorInstances().size() ||
      hir.lambdaInstances().size() != mir.lambdaInstances().size()) {
    return reject("frontend profile, place domain, or instance counts differ");
  }
  for (std::size_t index = 0; index < hir.classInstances().size(); ++index) {
    const HirClassInstance &source = hir.classInstances()[index];
    const MirClassInstance &lowered = mir.classInstances()[index];
    const ClassTypeInfo *info = source.source == nullptr
                                    ? nullptr
                                    : semantics.findClassType(*source.source);
    if (source.id != lowered.id || source.declaration != lowered.declaration ||
        source.type != lowered.type || source.kind != lowered.kind ||
        source.fieldInitializers.placeDomain !=
            lowered.fieldInitializers.placeDomain ||
        source.staticFieldInitializers.placeDomain !=
            lowered.staticFieldInitializers.placeDomain ||
        info == nullptr || info->id != source.declaration ||
        info->declaration != source.source ||
        info->sourceUnit != source.sourceUnit) {
      return reject("class instance " + std::to_string(index + 1) + " differs");
    }
  }
  for (std::size_t index = 0; index < hir.functionInstances().size(); ++index) {
    const HirFunctionInstance &source = hir.functionInstances()[index];
    const MirFunctionInstance &lowered = mir.functionInstances()[index];
    const FunctionInfo *info = source.source == nullptr
                                   ? nullptr
                                   : semantics.findFunction(*source.source);
    const MirDefinitionKind expectedDefinition =
        source.source == nullptr           ? MirDefinitionKind::Declaration
        : source.source->runtimeBinding()  ? MirDefinitionKind::RuntimeBinding
        : source.source->body() != nullptr ? MirDefinitionKind::Source
                                           : MirDefinitionKind::Declaration;
    if (source.id != lowered.id || source.declaration != lowered.declaration ||
        source.owner != lowered.owner ||
        source.returnType != lowered.returnType ||
        source.parameterTypes != lowered.parameterTypes ||
        source.parameterBindings != lowered.parameterBindings ||
        source.entryKind != lowered.entryKind ||
        source.entryArgumentAppendTarget != lowered.entryArgumentAppendTarget ||
        source.staticMember != lowered.staticMember ||
        source.constexprFunction != lowered.constexprFunction ||
        source.linkage != lowered.linkage ||
        source.externalSymbol != lowered.externalSymbol ||
        source.body.placeDomain != lowered.body.placeDomain ||
        lowered.definitionKind != expectedDefinition || info == nullptr ||
        info->id != source.declaration || info->declaration != source.source ||
        info->sourceUnit != source.sourceUnit) {
      return reject("function instance " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.constructorInstances().size();
       ++index) {
    const HirConstructorInstance &source = hir.constructorInstances()[index];
    const MirConstructorInstance &lowered = mir.constructorInstances()[index];
    if (source.id != lowered.id || source.owner != lowered.owner ||
        source.parameterTypes != lowered.parameterTypes ||
        source.parameterBindings != lowered.parameterBindings ||
        source.body.placeDomain != lowered.body.placeDomain) {
      return reject("constructor instance " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.destructorInstances().size();
       ++index) {
    const HirDestructorInstance &source = hir.destructorInstances()[index];
    const MirDestructorInstance &lowered = mir.destructorInstances()[index];
    if (source.id != lowered.id || source.owner != lowered.owner ||
        source.body.placeDomain != lowered.body.placeDomain) {
      return reject("destructor instance " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.lambdaInstances().size(); ++index) {
    const HirLambda &source = hir.lambdaInstances()[index];
    const MirLambdaInstance &lowered = mir.lambdaInstances()[index];
    if (source.id != lowered.id || source.declaration != lowered.declaration ||
        source.type != lowered.type ||
        source.returnType != lowered.returnType ||
        source.parameterTypes != lowered.parameterTypes ||
        source.parameterBindings != lowered.parameterBindings ||
        source.body.placeDomain != lowered.body.placeDomain) {
      return reject("lambda instance " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.nativeCallbackAdapters().size();
       ++index) {
    const HirNativeCallbackAdapter &source =
        hir.nativeCallbackAdapters()[index];
    const MirNativeCallbackAdapter &lowered =
        mir.nativeCallbackAdapters()[index];
    if (source.id != lowered.id || source.target != lowered.target ||
        source.type != lowered.type) {
      return reject("native callback adapter " + std::to_string(index + 1) +
                    " differs");
    }
  }
  return true;
}

[[nodiscard]] bool
validGeneratedIdentity(const LoweredGeneratedItemIdentity &identity) {
  if (ordinal(identity.kind) >= ordinal(LoweredGeneratedItemKind::Count)) {
    return false;
  }
  if (identity.kind == LoweredGeneratedItemKind::ProgramInitialization) {
    return identity.owner == 0 && identity.ordinal == 0;
  }
  if (identity.kind == LoweredGeneratedItemKind::HostedEntry ||
      identity.kind == LoweredGeneratedItemKind::NativeInteropAdapter) {
    return identity.owner != 0 && identity.ordinal == 0;
  }
  return identity.owner != 0;
}

} // namespace

const LoweredBody *LoweredProgram::findBody(MirBodyAddress address) const {
  return lang::findBody(bodies_, address);
}

const LoweredDeclaration *
LoweredProgram::findDeclaration(LoweredDeclarationId id) const {
  return id == 0 || id > declarations_.size() ? nullptr
                                              : &declarations_[id - 1];
}

const LoweredGeneratedItem *LoweredProgram::findGeneratedItem(
    const LoweredGeneratedItemIdentity &identity) const {
  return findGenerated(generatedItems_, identity);
}

LoweredProgramBuild LoweredProgramBuilder::build(
    const Program &program, const SemanticModel &semantics,
    const HirProgram &hir, const MirProgram &sourceMir,
    const MirProgram &optimizedMir, const TargetInfo &target) const {
  LoweredProgramBuild build;
  if (!target.supported()) {
    addIssue(build.issues, LoweredProgramIssueKind::UnsupportedTarget,
             "lowered program requires a supported target data layout");
  }
  if (!semantics.analysisSeal().matchesTarget(target) ||
      !semantics.analysisSeal().matchesProgram(program, target)) {
    addIssue(build.issues, LoweredProgramIssueKind::FrontendMismatch,
             "supplied Program or target differs from the semantic analysis "
             "seal");
  }
  const HirProgramPlanVerificationResult hirPlans =
      verifyHirProgramPlans(semantics, hir);
  if (!hirPlans.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::FrontendMismatch,
             hirPlans.errors.empty() ? "semantic and HIR program plans differ"
                                     : hirPlans.errors.front());
  }
  const MirVerificationResult sourceVerification = verifyMirProgram(sourceMir);
  if (!sourceMir.valid() || !sourceVerification.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::InvalidSourceMir,
             sourceVerification.errors.empty()
                 ? "source MIR is not marked valid"
                 : sourceVerification.errors.front().message);
  }
  const MirVerificationResult optimizedVerification =
      verifyMirProgram(optimizedMir);
  if (!optimizedMir.valid() || !optimizedVerification.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::InvalidOptimizedMir,
             optimizedVerification.errors.empty()
                 ? "optimized MIR is not marked valid"
                 : optimizedVerification.errors.front().message);
  }
  const FailureMetadataVerificationResult failureVerification =
      verifyFailureMetadata(optimizedMir.failureMetadata());
  if (!failureVerification.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::InvalidFailureMetadata,
             failureVerification.errors.empty()
                 ? "optimized MIR failure metadata is invalid"
                 : failureVerification.errors.front());
  }
  const MirVerificationResult optimization =
      verifyMirOptimizationCoherence(sourceMir, optimizedMir);
  if (!optimization.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::InvalidOptimization,
             optimization.errors.empty()
                 ? "optimized MIR differs from the source MIR contract"
                 : optimization.errors.front().message);
  }
  std::string frontendMismatch;
  if (!frontendInstancesMatch(semantics, hir, sourceMir, frontendMismatch)) {
    addIssue(build.issues, LoweredProgramIssueKind::FrontendMismatch,
             std::move(frontendMismatch));
  }
  if (!build.issues.empty()) {
    return build;
  }

  LoweredProgram lowered;
  lowered.target_ = target;
  lowered.mir_ = optimizedMir;
  lowered.declarations_ =
      DeclarationCollector(semantics, target).collect(program.declarations());

  for (const MirBodyAddress address : enumerateMirBodyAddresses(optimizedMir)) {
    const std::optional<LoweredBodyIdentity> identity =
        captureBodyIdentity(optimizedMir, hir, address);
    if (!identity) {
      addIssue(build.issues, LoweredProgramIssueKind::InvalidBodyInventory,
               "optimized MIR body lacks a stable lowered identity", address);
      continue;
    }
    lowered.bodies_.push_back(
        {.identity = *identity, .role = bodyRole(optimizedMir, *identity)});
  }

  DerivedGeneratedGraph graph = deriveGeneratedGraph(optimizedMir);
  lowered.generatedItems_ = std::move(graph.items);
  for (auto &[address, roots] : graph.roots) {
    if (LoweredBody *body = findBody(lowered.bodies_, address)) {
      body->requiredGeneratedItems = std::move(roots);
    }
  }
  if (!build.issues.empty()) {
    return build;
  }

  lowered.constructionSeal_ =
      fingerprint(LoweredProgramPrinter().print(lowered));
  build.issues = verifyLoweredProgram(lowered);
  if (build.issues.empty()) {
    build.program.emplace(std::move(lowered));
  }
  return build;
}

std::vector<LoweredProgramIssue>
verifyLoweredProgram(const LoweredProgram &program) {
  std::vector<LoweredProgramIssue> issues;
  if (!program.target_.supported()) {
    addIssue(issues, LoweredProgramIssueKind::UnsupportedTarget,
             "lowered program has an unsupported target data layout");
  }
  if (program.target_.executionProfile != program.mir_.executionProfile()) {
    addIssue(issues, LoweredProgramIssueKind::FrontendMismatch,
             "lowered target and MIR execution profiles differ");
  }
  const MirVerificationResult mirVerification = verifyMirProgram(program.mir_);
  if (!program.mir_.valid() || !mirVerification.valid()) {
    addIssue(issues, LoweredProgramIssueKind::InvalidOptimizedMir,
             mirVerification.errors.empty()
                 ? "lowered MIR is not marked valid"
                 : mirVerification.errors.front().message);
  }
  const FailureMetadataVerificationResult failureVerification =
      verifyFailureMetadata(program.mir_.failureMetadata());
  if (!failureVerification.valid()) {
    addIssue(issues, LoweredProgramIssueKind::InvalidFailureMetadata,
             failureVerification.errors.empty()
                 ? "lowered MIR failure metadata is invalid"
                 : failureVerification.errors.front());
  }

  const std::vector<MirBodyAddress> addresses =
      enumerateMirBodyAddresses(program.mir_);
  if (program.bodies_.size() != addresses.size()) {
    addIssue(issues, LoweredProgramIssueKind::InvalidBodyInventory,
             "lowered body census does not match the MIR body census");
  }
  for (std::size_t index = 0;
       index < std::min(program.bodies_.size(), addresses.size()); ++index) {
    const LoweredBody &body = program.bodies_[index];
    const MirBody *mirBody = findMirBody(program.mir_, addresses[index]);
    if (body.identity.address != addresses[index] || mirBody == nullptr ||
        body.identity.placeDomain != mirBody->placeDomain ||
        body.role != bodyRole(program.mir_, body.identity) ||
        ordinal(body.identity.definition) >=
            ordinal(LoweredBodyDefinitionKind::Count) ||
        ordinal(body.role) >= ordinal(LoweredBodyRole::Count)) {
      addIssue(issues, LoweredProgramIssueKind::InvalidBodyInventory,
               "lowered body identity or role differs from verified MIR",
               body.identity.address);
    }
    if (!std::is_sorted(body.requiredGeneratedItems.begin(),
                        body.requiredGeneratedItems.end(), generatedLess) ||
        std::adjacent_find(body.requiredGeneratedItems.begin(),
                           body.requiredGeneratedItems.end()) !=
            body.requiredGeneratedItems.end() ||
        (body.role != LoweredBodyRole::SourceExecutable &&
         !body.requiredGeneratedItems.empty())) {
      addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
               "body generated-item roots are unordered, duplicated, or "
               "attached to a non-executable body",
               body.identity.address);
    }
  }

  std::unordered_map<LoweredDeclarationId, std::size_t> nextOrdinal;
  for (std::size_t index = 0; index < program.declarations_.size(); ++index) {
    const LoweredDeclaration &declaration = program.declarations_[index];
    const std::size_t expectedOrdinal = ++nextOrdinal[declaration.parent];
    if (declaration.id != index + 1 ||
        (declaration.parent != 0 && declaration.parent >= declaration.id) ||
        declaration.ordinal != expectedOrdinal ||
        ordinal(declaration.kind) >= ordinal(LoweredDeclarationKind::Count)) {
      addIssue(issues, LoweredProgramIssueKind::InvalidDeclarationInventory,
               "lowered declaration identities are not dense, ordered, and "
               "parent-before-child",
               std::nullopt, declaration.id);
    }
  }

  const DerivedGeneratedGraph expected = deriveGeneratedGraph(program.mir_);
  if (program.generatedItems_ != expected.items) {
    addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
             "generated-item inventory differs from exact MIR-derived "
             "contracts");
  }
  for (const auto &[address, roots] : expected.roots) {
    const LoweredBody *body = findBody(program.bodies_, address);
    if (body == nullptr || body->requiredGeneratedItems != roots) {
      addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
               "body generated-item roots differ from exact MIR operations",
               address);
    }
  }

  bool uniqueItems = true;
  for (std::size_t index = 0; index < program.generatedItems_.size(); ++index) {
    const LoweredGeneratedItem &item = program.generatedItems_[index];
    if (!validGeneratedIdentity(item.identity)) {
      addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
               "generated item has an invalid stable identity", std::nullopt,
               std::nullopt, item.identity);
    }
    if (std::find_if(program.generatedItems_.begin(),
                     program.generatedItems_.begin() +
                         static_cast<std::ptrdiff_t>(index),
                     [&](const LoweredGeneratedItem &prior) {
                       return prior.identity == item.identity;
                     }) !=
        program.generatedItems_.begin() + static_cast<std::ptrdiff_t>(index)) {
      uniqueItems = false;
      addIssue(issues, LoweredProgramIssueKind::DuplicateGeneratedItem,
               "generated item identity is duplicated", std::nullopt,
               std::nullopt, item.identity);
    }
    if (findMirBody(program.mir_, item.sourceBody) == nullptr ||
        !std::is_sorted(item.dependencies.begin(), item.dependencies.end(),
                        generatedLess) ||
        std::adjacent_find(item.dependencies.begin(),
                           item.dependencies.end()) !=
            item.dependencies.end()) {
      addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
               "generated item source or dependency order is invalid",
               item.sourceBody, std::nullopt, item.identity);
    }
    for (const LoweredGeneratedItemIdentity &dependency : item.dependencies) {
      if (findGenerated(program.generatedItems_, dependency) == nullptr) {
        addIssue(issues, LoweredProgramIssueKind::MissingGeneratedItem,
                 "generated-item dependency is absent", std::nullopt,
                 std::nullopt, item.identity);
      }
    }
  }

  std::vector<LoweredGeneratedItemIdentity> roots;
  for (const LoweredBody &body : program.bodies_) {
    roots.insert(roots.end(), body.requiredGeneratedItems.begin(),
                 body.requiredGeneratedItems.end());
  }
  std::sort(roots.begin(), roots.end(), generatedLess);
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  std::vector<LoweredGeneratedItemIdentity> reachable;
  std::function<void(const LoweredGeneratedItemIdentity &)> markReachable =
      [&](const LoweredGeneratedItemIdentity &identity) {
        if (containsGenerated(reachable, identity)) {
          return;
        }
        reachable.push_back(identity);
        if (const LoweredGeneratedItem *item =
                findGenerated(program.generatedItems_, identity)) {
          for (const LoweredGeneratedItemIdentity &dependency :
               item->dependencies) {
            markReachable(dependency);
          }
        }
      };
  for (const LoweredGeneratedItemIdentity &root : roots) {
    markReachable(root);
  }
  for (const LoweredGeneratedItem &item : program.generatedItems_) {
    if (!containsGenerated(reachable, item.identity)) {
      addIssue(issues, LoweredProgramIssueKind::OrphanGeneratedItem,
               "generated item is not rooted by an executable body",
               std::nullopt, std::nullopt, item.identity);
    }
  }

  std::vector<LoweredGeneratedItemIdentity> visiting;
  std::vector<LoweredGeneratedItemIdentity> visited;
  std::function<void(const LoweredGeneratedItem &)> visit =
      [&](const LoweredGeneratedItem &item) {
        if (containsGenerated(visited, item.identity)) {
          return;
        }
        if (containsGenerated(visiting, item.identity)) {
          addIssue(issues,
                   LoweredProgramIssueKind::CyclicGeneratedItemDependency,
                   "generated-item dependency graph contains a cycle",
                   std::nullopt, std::nullopt, item.identity);
          return;
        }
        visiting.push_back(item.identity);
        for (const LoweredGeneratedItemIdentity &dependency :
             item.dependencies) {
          if (const LoweredGeneratedItem *target =
                  findGenerated(program.generatedItems_, dependency)) {
            visit(*target);
          }
        }
        visiting.erase(
            std::remove(visiting.begin(), visiting.end(), item.identity),
            visiting.end());
        visited.push_back(item.identity);
      };
  if (uniqueItems) {
    for (const LoweredGeneratedItem &item : program.generatedItems_) {
      visit(item);
    }
  }

  const std::uint64_t currentSeal =
      fingerprint(LoweredProgramPrinter().print(program));
  if (!program.constructionSeal_ || *program.constructionSeal_ != currentSeal) {
    addIssue(issues, LoweredProgramIssueKind::InvalidConstructionSeal,
             "lowered program differs from its immutable construction seal");
  }
  return issues;
}

} // namespace lang
