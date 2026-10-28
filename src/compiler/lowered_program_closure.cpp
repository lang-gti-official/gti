#include "lowered_program_closure.h"

#include "gti/lowered_program_printer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <streambuf>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace lang::lowered_detail {

namespace {
[[nodiscard]] bool lifecycleContainsClosure(const SemanticType &type) {
  return type.kind == SemanticType::Lambda ||
         std::any_of(type.arguments.begin(), type.arguments.end(),
                     lifecycleContainsClosure);
}
} // namespace

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

[[nodiscard]] const LoweredDeclaration *
findDeclaration(const std::vector<LoweredDeclaration> &declarations,
                LoweredDeclarationId id) {
  const auto found = std::find_if(declarations.begin(), declarations.end(),
                                  [&](const LoweredDeclaration &declaration) {
                                    return declaration.id == id;
                                  });
  return found == declarations.end() ? nullptr : &*found;
}

[[nodiscard]] LoweredDeclaration *
findDeclaration(std::vector<LoweredDeclaration> &declarations,
                LoweredDeclarationId id) {
  return const_cast<LoweredDeclaration *>(
      findDeclaration(std::as_const(declarations), id));
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

namespace {

// Hashes eight bytes at a time as text arrives. The result depends only on the
// byte sequence, not on how the printer chunks its writes.
class SealHashBuffer final : public std::streambuf {
public:
  SealHashBuffer() { setp(buffer.data(), buffer.data() + buffer.size()); }

  [[nodiscard]] std::uint64_t finish() {
    consume(pbase(), pptr());
    setp(buffer.data(), buffer.data() + buffer.size());
    std::uint64_t tail = 0;
    std::memcpy(&tail, pending.data(), pendingSize);
    std::uint64_t result = state ^ mix(tail ^ (length * 0x9E3779B97F4A7C15ULL));
    result ^= result >> 33;
    result *= 0xFF51AFD7ED558CCDULL;
    result ^= result >> 33;
    result *= 0xC4CEB9FE1A85EC53ULL;
    return result ^ (result >> 33);
  }

protected:
  int_type overflow(int_type character) override {
    consume(pbase(), pptr());
    setp(buffer.data(), buffer.data() + buffer.size());
    if (!traits_type::eq_int_type(character, traits_type::eof())) {
      *pptr() = traits_type::to_char_type(character);
      pbump(1);
    }
    return traits_type::not_eof(character);
  }

  int sync() override {
    consume(pbase(), pptr());
    setp(buffer.data(), buffer.data() + buffer.size());
    return 0;
  }

private:
  [[nodiscard]] static std::uint64_t mix(std::uint64_t value) {
    value *= 0x87C37B91114253D5ULL;
    value = (value << 31) | (value >> 33);
    return value * 0x4CF5AD432745937FULL;
  }

  void word(std::uint64_t value) {
    state ^= mix(value);
    state = ((state << 27) | (state >> 37)) * 5 + 0x52DCE729;
  }

  void consume(const char *begin, const char *end) {
    length += static_cast<std::uint64_t>(end - begin);
    while (begin != end && pendingSize != 0 && pendingSize < pending.size()) {
      pending[pendingSize++] = *begin++;
    }
    if (pendingSize == pending.size()) {
      std::uint64_t value = 0;
      std::memcpy(&value, pending.data(), pending.size());
      word(value);
      pendingSize = 0;
    }
    while (end - begin >= 8) {
      std::uint64_t value = 0;
      std::memcpy(&value, begin, 8);
      word(value);
      begin += 8;
    }
    while (begin != end) {
      pending[pendingSize++] = *begin++;
    }
  }

  std::array<char, 1 << 16> buffer{};
  std::array<char, 8> pending{};
  std::size_t pendingSize = 0;
  std::uint64_t length = 0;
  std::uint64_t state = 0x6A09E667F3BCC908ULL;
};

[[nodiscard]] std::atomic<bool> &sealValidation() {
  static std::atomic<bool> enabled = [] {
    const char *value = std::getenv("GTI_COMPILER_VALIDATION");
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
  }();
  return enabled;
}

} // namespace

} // namespace lang::lowered_detail

namespace lang {

bool loweredProgramSealValidation() noexcept {
  return lowered_detail::sealValidation().load(std::memory_order_relaxed);
}

void setLoweredProgramSealValidation(bool enabled) noexcept {
  lowered_detail::sealValidation().store(enabled, std::memory_order_relaxed);
}

} // namespace lang

namespace lang::lowered_detail {

std::uint64_t constructionSeal(const LoweredProgram &program) {
  SealHashBuffer hash;
  std::ostream output(&hash);
  LoweredProgramPrinter().print(program, output);
  output.flush();
  return hash.finish();
}

void addIssue(std::vector<LoweredProgramIssue> &issues,
              LoweredProgramIssueKind kind, std::string detail,
              std::optional<MirBodyAddress> body,
              std::optional<LoweredDeclarationId> declaration,
              std::optional<LoweredGeneratedItemIdentity> generated) {
  issues.push_back({.kind = kind,
                    .detail = std::move(detail),
                    .body = body,
                    .declaration = declaration,
                    .generatedItem = generated});
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

[[nodiscard]] bool
structuralOperatorAdapter(const LoweredFunctionDeclaration &function,
                          AccessModifier access) {
  if (access != AccessModifier::Public || !function.overloadedOperator ||
      function.compilerPrivate || function.hasRequiresClause ||
      !function.requirements.empty()) {
    return false;
  }
  switch (*function.overloadedOperator) {
  case OverloadedOperator::Dereference:
    return function.parameters.empty() &&
           function.receiverMutability == ReceiverMutability::ReadOnly &&
           function.returnType.kind == SemanticType::Reference &&
           function.returnType.arguments.size() == 1;
  case OverloadedOperator::PreIncrement:
    return function.parameters.empty() &&
           function.receiverMutability == ReceiverMutability::Mutable &&
           function.returnType == SemanticType::Void;
  case OverloadedOperator::NotEqual:
    return function.receiverMutability == ReceiverMutability::ReadOnly &&
           function.returnType == SemanticType::Bool &&
           function.parameters.size() == 1 &&
           function.parameters.front().type.kind == SemanticType::Reference &&
           function.parameters.front().type.arguments.size() == 1 &&
           function.parameters.front().type.referenceAccess ==
               AccessMode::ReadOnly;
  default:
    return false;
  }
}

[[nodiscard]] bool callableAdapter(const LoweredFunctionDeclaration &function) {
  return function.ownerClass != 0 && function.overloadedOperator &&
         *function.overloadedOperator == OverloadedOperator::Call &&
         function.definitionKind == MirDefinitionKind::Source &&
         !function.compilerPrivate && !function.hasRequiresClause &&
         function.requirements.empty();
}

[[nodiscard]] bool concreteLifecycleType(const SemanticType &type) {
  if (type == SemanticType::Unknown ||
      type.kind == SemanticType::TypeParameter ||
      type.kind == SemanticType::TypePack) {
    return false;
  }
  return std::all_of(type.arguments.begin(), type.arguments.end(),
                     concreteLifecycleType);
}

[[nodiscard]] DerivedGeneratedGraph deriveGeneratedGraph(
    const MirProgram &program,
    const std::vector<LoweredDeclaration> &declarations,
    const std::vector<LoweredConstructorInstance> &constructorInstances) {
  DerivedGeneratedGraph graph;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    graph.bodyRoots.emplace_back(address,
                                 std::vector<LoweredGeneratedItemIdentity>{});
  }
  for (const LoweredDeclaration &declaration : declarations) {
    graph.declarationRoots.emplace_back(
        declaration.id, std::vector<LoweredGeneratedItemIdentity>{});
  }
  const auto rootBody = [&](MirBodyAddress address,
                            LoweredGeneratedItemIdentity identity) {
    const auto found =
        std::find_if(graph.bodyRoots.begin(), graph.bodyRoots.end(),
                     [&](const auto &entry) { return entry.first == address; });
    if (found != graph.bodyRoots.end()) {
      found->second.push_back(identity);
    }
  };
  const auto rootDeclaration = [&](LoweredDeclarationId declaration,
                                   LoweredGeneratedItemIdentity identity) {
    const auto found = std::find_if(
        graph.declarationRoots.begin(), graph.declarationRoots.end(),
        [&](const auto &entry) { return entry.first == declaration; });
    if (found != graph.declarationRoots.end()) {
      found->second.push_back(identity);
    }
  };

  for (const auto &storage : program.threadLocalStorage()) {
    const LoweredGeneratedItemIdentity identity{
        .kind = LoweredGeneratedItemKind::ThreadLocalStorage,
        .owner = storage.symbol};
    const MirBodyAddress body{.kind = MirBodyKind::ThreadLocalInitializer,
                              .owner = storage.symbol};
    graph.items.push_back({.identity = identity, .sourceBody = body});
    rootBody(body, identity);
  }
  for (const auto address : enumerateMirBodyAddresses(program)) {
    const auto *user = findMirBody(program, address);
    if (user == nullptr)
      continue;
    for (const auto &block : user->blocks)
      for (const auto &instruction : block.instructions)
        if (!instruction.tombstone &&
            instruction.kind == MirInstructionKind::EnsureThreadLocal)
          rootBody(address,
                   {.kind = LoweredGeneratedItemKind::ThreadLocalStorage,
                    .owner = instruction.threadLocalStorage});
  }

  std::optional<LoweredGeneratedItemIdentity> initialization;
  if (hasExecutableProgramInitialization(program)) {
    initialization = LoweredGeneratedItemIdentity{
        .kind = LoweredGeneratedItemKind::ProgramInitialization};
    graph.items.push_back(
        {.identity = *initialization,
         .sourceBody = {.kind = MirBodyKind::Module, .owner = 0}});
    rootBody({.kind = MirBodyKind::Module, .owner = 0}, *initialization);
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
    rootBody({.kind = MirBodyKind::HostedStartup, .owner = startup->entry},
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

  for (const MirThreadTaskAdapter &adapter : program.threadTaskAdapters()) {
    const LoweredGeneratedItemIdentity identity{
        .kind = LoweredGeneratedItemKind::ThreadTaskAdapter,
        .owner = adapter.id};
    graph.items.push_back(
        {.identity = identity,
         .sourceBody = adapter.sourceBody,
         .payload = LoweredThreadTaskAdapterItem{.adapter = adapter}});
  }

  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    const MirBody *body = findMirBody(program, address);
    if (body == nullptr) {
      continue;
    }
    for (const MirBlock &block : body->blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.intrinsic == IntrinsicKind::ThreadSpawn &&
            instruction.threadTaskAdapter) {
          rootBody(address,
                   {.kind = LoweredGeneratedItemKind::ThreadTaskAdapter,
                    .owner = *instruction.threadTaskAdapter});
        }
        if (instruction.intrinsic == IntrinsicKind::ThreadScope)
          for (const auto &task : instruction.scopedThreadTasks)
            rootBody(address,
                     {.kind = LoweredGeneratedItemKind::ThreadTaskAdapter,
                      .owner = task.adapter});
        if (instruction.operation != MirOperation::NativeCallback ||
            !instruction.nativeCallbackAdapter) {
          continue;
        }
        rootBody(address,
                 {.kind = LoweredGeneratedItemKind::NativeInteropAdapter,
                  .owner = *instruction.nativeCallbackAdapter});
      }
    }
  }

  std::unordered_map<LoweredDeclarationId, AccessModifier> classAccess;
  for (const LoweredDeclaration &declaration : declarations) {
    if (const auto *classDeclaration =
            std::get_if<LoweredClassDeclaration>(&declaration.payload)) {
      classAccess[declaration.id] = classDeclaration->kind == ClassKind::Class
                                        ? AccessModifier::Private
                                        : AccessModifier::Public;
      continue;
    }
    if (const auto *access =
            std::get_if<LoweredAccessDeclaration>(&declaration.payload)) {
      if (classAccess.contains(declaration.parent)) {
        classAccess[declaration.parent] = access->access;
      }
      continue;
    }
    const auto *function =
        std::get_if<LoweredFunctionDeclaration>(&declaration.payload);
    if (function == nullptr || function->id == 0 ||
        !classAccess.contains(declaration.parent)) {
      continue;
    }
    if (structuralOperatorAdapter(*function,
                                  classAccess.at(declaration.parent))) {
      const LoweredGeneratedItemIdentity identity{
          .kind = LoweredGeneratedItemKind::StructuralOperatorAdapter,
          .owner = function->id};
      graph.items.push_back(
          {.identity = identity,
           .sourceKind = LoweredGeneratedItemSourceKind::Declaration,
           .sourceDeclaration = declaration.id,
           .payload = LoweredStructuralOperatorAdapterItem{
               .function = function->id,
               .operation = *function->overloadedOperator}});
      rootDeclaration(declaration.id, identity);
    }
    if (callableAdapter(*function)) {
      const LoweredGeneratedItemIdentity identity{
          .kind = LoweredGeneratedItemKind::CallableAdapter,
          .owner = function->id};
      graph.items.push_back(
          {.identity = identity,
           .sourceKind = LoweredGeneratedItemSourceKind::Declaration,
           .sourceDeclaration = declaration.id,
           .payload = LoweredCallableAdapterItem{
               .function = function->id,
               .capability = callableInvocationCapability(
                   function->receiverMutability)}});
      rootDeclaration(declaration.id, identity);
    }
  }

  for (const MirDestructorInstance &destructor :
       program.destructorInstances()) {
    const MirClassInstance *owner = program.findClassInstance(destructor.owner);
    if (owner == nullptr || owner->destructor != destructor.id ||
        owner->destructorStatus != SpecialMemberStatus::Declared ||
        !owner->requiresActiveDropState || !owner->requiresActiveCleanup ||
        owner->cAbiRecord || owner->cAbiLayout || owner->unionLayout ||
        owner->kind == ClassKind::Union ||
        destructor.definitionKind != MirDefinitionKind::Source ||
        !concreteLifecycleType(owner->type)) {
      continue;
    }

    const LoweredDeclaration *classRow = nullptr;
    const LoweredDeclaration *destructorRow = nullptr;
    for (const LoweredDeclaration &declaration : declarations) {
      if (const auto *classDeclaration =
              std::get_if<LoweredClassDeclaration>(&declaration.payload);
          classDeclaration != nullptr &&
          classDeclaration->id == owner->declaration) {
        classRow = &declaration;
      }
      if (const auto *destructorDeclaration =
              std::get_if<LoweredDestructorDeclaration>(&declaration.payload);
          destructorDeclaration != nullptr &&
          destructorDeclaration->owner == owner->declaration) {
        destructorRow = &declaration;
      }
    }
    const auto *classDeclaration =
        classRow == nullptr
            ? nullptr
            : std::get_if<LoweredClassDeclaration>(&classRow->payload);
    if (classDeclaration == nullptr || destructorRow == nullptr ||
        !classDeclaration->requiresActiveDropState ||
        classDeclaration->destructor != SpecialMemberStatus::Declared) {
      continue;
    }

    const bool generic = !classDeclaration->genericParameters.empty();
    const bool instantiated =
        !owner->type.arguments.empty() || !owner->type.valueArguments.empty();
    if (generic && !instantiated) {
      continue;
    }
    const LoweredLifecycleCleanupForm form =
        generic && lifecycleContainsClosure(owner->type)
            ? LoweredLifecycleCleanupForm::RetainedPrimary
        : generic ? LoweredLifecycleCleanupForm::ConcreteSpecialization
                  : LoweredLifecycleCleanupForm::OrdinaryClass;
    const LoweredGeneratedItemIdentity identity{
        .kind = LoweredGeneratedItemKind::LifecycleCleanup,
        .owner = destructor.id};
    graph.items.push_back(
        {.identity = identity,
         .sourceKind = LoweredGeneratedItemSourceKind::Declaration,
         .sourceDeclaration = destructorRow->id,
         .payload = LoweredLifecycleCleanupItem{
             .ownerClass = owner->declaration,
             .classInstance = owner->id,
             .destructorInstance = destructor.id,
             .form = form,
             .mayRaiseDefinedFailure = destructor.mayRaiseDefinedFailure}});
    rootDeclaration(destructorRow->id, identity);
  }

  for (const MirFunctionInstance &instance : program.functionInstances()) {
    if (instance.definitionKind != MirDefinitionKind::Source) {
      continue;
    }
    const LoweredDeclaration *sourceRow = nullptr;
    const LoweredFunctionDeclaration *source = nullptr;
    for (const LoweredDeclaration &declaration : declarations) {
      const auto *function =
          std::get_if<LoweredFunctionDeclaration>(&declaration.payload);
      if (function != nullptr && function->id == instance.declaration) {
        sourceRow = &declaration;
        source = function;
        break;
      }
    }
    const LoweredClassDeclaration *owner =
        source == nullptr || source->ownerClass == 0
            ? nullptr
            : [&]() -> const LoweredClassDeclaration * {
      for (const LoweredDeclaration &declaration : declarations) {
        const auto *candidate =
            std::get_if<LoweredClassDeclaration>(&declaration.payload);
        if (candidate != nullptr && candidate->id == source->ownerClass) {
          return candidate;
        }
      }
      return nullptr;
    }();
    const bool generic =
        source != nullptr &&
        (!source->genericParameters.empty() ||
         (owner != nullptr && !owner->genericParameters.empty()));
    if (sourceRow == nullptr || !generic) {
      continue;
    }
    const LoweredGeneratedItemIdentity identity{
        .kind = LoweredGeneratedItemKind::ConcreteInstanceAdapter,
        .owner = instance.id,
        .ordinal = ordinal(LoweredConcreteInstanceAdapterKind::Function) + 1};
    graph.items.push_back(
        {.identity = identity,
         .sourceKind = LoweredGeneratedItemSourceKind::Declaration,
         .sourceDeclaration = sourceRow->id,
         .payload = LoweredConcreteInstanceAdapterItem{
             .kind = LoweredConcreteInstanceAdapterKind::Function,
             .body = {.kind = MirBodyKind::Function, .owner = instance.id},
             .declaration = instance.declaration,
             .ownerClassInstance = instance.owner.value_or(0),
             .mayRaiseDefinedFailure = instance.mayRaiseDefinedFailure}});
    rootDeclaration(sourceRow->id, identity);
  }

  for (const MirConstructorInstance &instance :
       program.constructorInstances()) {
    const LoweredConstructorInstance *concrete =
        instance.id == 0 || instance.id > constructorInstances.size()
            ? nullptr
            : &constructorInstances[instance.id - 1];
    if (concrete == nullptr || concrete->id != instance.id ||
        concrete->declaration == 0 ||
        instance.definitionKind != MirDefinitionKind::Source) {
      continue;
    }
    const LoweredDeclaration *sourceRow = nullptr;
    const LoweredConstructorDeclaration *source = nullptr;
    for (const LoweredDeclaration &declaration : declarations) {
      const auto *constructor =
          std::get_if<LoweredConstructorDeclaration>(&declaration.payload);
      if (constructor != nullptr && constructor->id == concrete->declaration) {
        sourceRow = &declaration;
        source = constructor;
        break;
      }
    }
    const MirClassInstance *ownerInstance =
        program.findClassInstance(instance.owner);
    const LoweredClassDeclaration *owner = nullptr;
    if (ownerInstance != nullptr) {
      for (const LoweredDeclaration &declaration : declarations) {
        const auto *candidate =
            std::get_if<LoweredClassDeclaration>(&declaration.payload);
        if (candidate != nullptr &&
            candidate->id == ownerInstance->declaration) {
          owner = candidate;
          break;
        }
      }
    }
    const bool generic =
        source != nullptr &&
        (!source->genericParameters.empty() ||
         (owner != nullptr && !owner->genericParameters.empty()));
    if (sourceRow == nullptr || !generic) {
      continue;
    }
    const LoweredGeneratedItemIdentity identity{
        .kind = LoweredGeneratedItemKind::ConcreteInstanceAdapter,
        .owner = instance.id,
        .ordinal =
            ordinal(LoweredConcreteInstanceAdapterKind::Constructor) + 1};
    graph.items.push_back(
        {.identity = identity,
         .sourceKind = LoweredGeneratedItemSourceKind::Declaration,
         .sourceDeclaration = sourceRow->id,
         .payload = LoweredConcreteInstanceAdapterItem{
             .kind = LoweredConcreteInstanceAdapterKind::Constructor,
             .body = {.kind = MirBodyKind::Constructor, .owner = instance.id},
             .declaration = concrete->declaration,
             .ownerClassInstance = instance.owner,
             .mayRaiseDefinedFailure = instance.mayRaiseDefinedFailure}});
    rootDeclaration(sourceRow->id, identity);
  }

  for (auto &[address, roots] : graph.bodyRoots) {
    static_cast<void>(address);
    std::sort(roots.begin(), roots.end(), generatedLess);
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  }
  for (auto &[declaration, roots] : graph.declarationRoots) {
    static_cast<void>(declaration);
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

} // namespace lang::lowered_detail
