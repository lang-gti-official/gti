#include "cpp_mir_program_plan.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <string_view>
#include <tuple>
#include <utility>

namespace lang {
namespace {

template <typename Enum>
[[nodiscard]] constexpr std::size_t ordinal(Enum value) {
  return static_cast<std::size_t>(value);
}

[[nodiscard]] bool addressLess(MirBodyAddress left, MirBodyAddress right) {
  return std::pair{ordinal(left.kind), left.owner} <
         std::pair{ordinal(right.kind), right.owner};
}

[[nodiscard]] bool thunkLess(const CppMirThunkIdentity &left,
                             const CppMirThunkIdentity &right) {
  return std::tuple{ordinal(left.kind), left.owner, left.ordinal} <
         std::tuple{ordinal(right.kind), right.owner, right.ordinal};
}

[[nodiscard]] bool dataLess(const CppMirDataIdentity &left,
                            const CppMirDataIdentity &right) {
  return std::tuple{ordinal(left.kind), left.owner, left.declaration,
                    left.ordinal} < std::tuple{ordinal(right.kind), right.owner,
                                               right.declaration,
                                               right.ordinal};
}

[[nodiscard]] bool validThunkIdentity(const CppMirThunkIdentity &identity) {
  if (ordinal(identity.kind) >= ordinal(CppMirThunkKind::Count)) {
    return false;
  }
  if (identity.kind == CppMirThunkKind::HostedEntry) {
    return identity.owner != 0 && identity.ordinal == 0;
  }
  if (identity.kind == CppMirThunkKind::ProgramInitialization) {
    return identity.owner == 0 && identity.ordinal == 0;
  }
  return identity.owner != 0;
}

[[nodiscard]] bool validDataIdentity(const CppMirDataIdentity &identity) {
  return ordinal(identity.kind) < ordinal(CppMirDataKind::Count) &&
         identity.declaration != 0;
}

[[nodiscard]] CppMirBodyDefinitionKind
bodyDefinition(MirDefinitionKind definition) {
  switch (definition) {
  case MirDefinitionKind::Source:
    return CppMirBodyDefinitionKind::Source;
  case MirDefinitionKind::RuntimeBinding:
    return CppMirBodyDefinitionKind::RuntimeBinding;
  case MirDefinitionKind::Declaration:
    return CppMirBodyDefinitionKind::Declaration;
  }
  return CppMirBodyDefinitionKind::Count;
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

[[nodiscard]] bool isContractedThunkKind(CppMirThunkKind kind) {
  return kind == CppMirThunkKind::HostedEntry ||
         kind == CppMirThunkKind::ProgramInitialization;
}

[[nodiscard]] bool familySupportsBody(CppMirExecutionFamily family) {
  switch (family) {
  case CppMirExecutionFamily::None:
  case CppMirExecutionFamily::Unsupported:
  case CppMirExecutionFamily::GeneralV1:
    return true;
  case CppMirExecutionFamily::Count:
    return false;
  }
  return false;
}

[[nodiscard]] const CppMirGeneratedThunk *
findThunk(const std::vector<CppMirGeneratedThunk> &thunks,
          const CppMirThunkIdentity &identity) {
  const auto found = std::find_if(thunks.begin(), thunks.end(),
                                  [&](const CppMirGeneratedThunk &thunk) {
                                    return thunk.identity == identity;
                                  });
  return found == thunks.end() ? nullptr : &*found;
}

[[nodiscard]] std::size_t
bodyOrder(const std::vector<MirBodyAddress> &addresses,
          MirBodyAddress address) {
  const auto found = std::find(addresses.begin(), addresses.end(), address);
  return found == addresses.end() ? std::numeric_limits<std::size_t>::max()
                                  : static_cast<std::size_t>(std::distance(
                                        addresses.begin(), found));
}

void addIssue(CppMirProgramPlan &plan, CppMirPlanIssueKind kind,
              std::string detail,
              std::optional<MirBodyAddress> body = std::nullopt,
              std::optional<CppMirDataIdentity> data = std::nullopt,
              std::optional<CppMirThunkIdentity> thunk = std::nullopt) {
  plan.issues.push_back({.kind = kind,
                         .body = body,
                         .data = data,
                         .thunk = thunk,
                         .detail = std::move(detail)});
}

void addUnsupported(CppMirProgramPlan &plan, CppMirUnsupportedSurfaceKind kind,
                    std::optional<MirBodyAddress> body = std::nullopt,
                    std::optional<CppMirDataIdentity> data = std::nullopt,
                    std::optional<CppMirThunkIdentity> thunk = std::nullopt) {
  plan.unsupported.push_back(
      {.kind = kind, .body = body, .data = data, .thunk = thunk});
}

[[nodiscard]] bool
containsThunk(const std::vector<CppMirThunkIdentity> &identities,
              const CppMirThunkIdentity &identity) {
  return std::find(identities.begin(), identities.end(), identity) !=
         identities.end();
}

} // namespace

std::optional<CppMirBodyIdentity>
captureCppMirBodyIdentity(const MirProgram &program, MirBodyAddress address) {
  const MirBody *body = findMirBody(program, address);
  if (body == nullptr || body->kind != address.kind) {
    return std::nullopt;
  }

  CppMirBodyIdentity result{.address = address,
                            .placeDomain = body->placeDomain};
  switch (address.kind) {
  case MirBodyKind::Module:
    if (address.owner != 0) {
      return std::nullopt;
    }
    return result;
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers: {
    const MirClassInstance *instance = program.findClassInstance(address.owner);
    if (instance == nullptr || instance->id != address.owner) {
      return std::nullopt;
    }
    result.declaration = instance->declaration;
    result.concreteOwner = instance->id;
    return result;
  }
  case MirBodyKind::Function: {
    const MirFunctionInstance *instance =
        program.findFunctionInstance(address.owner);
    if (instance == nullptr || instance->id != address.owner) {
      return std::nullopt;
    }
    result.definition = bodyDefinition(instance->definitionKind);
    result.declaration = instance->declaration;
    result.concreteOwner = instance->owner.value_or(0);
    return result.definition == CppMirBodyDefinitionKind::Count
               ? std::nullopt
               : std::optional{std::move(result)};
  }
  case MirBodyKind::Constructor: {
    const MirConstructorInstance *instance =
        program.findConstructorInstance(address.owner);
    if (instance == nullptr || instance->id != address.owner) {
      return std::nullopt;
    }
    result.definition = bodyDefinition(instance->definitionKind);
    result.concreteOwner = instance->owner;
    return result.definition == CppMirBodyDefinitionKind::Count
               ? std::nullopt
               : std::optional{std::move(result)};
  }
  case MirBodyKind::Destructor: {
    const MirDestructorInstance *instance =
        program.findDestructorInstance(address.owner);
    if (instance == nullptr || instance->id != address.owner) {
      return std::nullopt;
    }
    result.definition = bodyDefinition(instance->definitionKind);
    result.concreteOwner = instance->owner;
    return result.definition == CppMirBodyDefinitionKind::Count
               ? std::nullopt
               : std::optional{std::move(result)};
  }
  case MirBodyKind::Lambda: {
    const MirLambdaInstance *instance = program.findLambda(address.owner);
    if (instance == nullptr || instance->id != address.owner) {
      return std::nullopt;
    }
    result.definition = CppMirBodyDefinitionKind::Source;
    result.declaration = instance->declaration;
    return result;
  }
  case MirBodyKind::HostedStartup: {
    const std::optional<MirHostedStartupPlan> &startup =
        program.hostedStartupPlan();
    if (!startup || startup->entry == 0 || address.owner != startup->entry ||
        program.hostedStartup() != body) {
      return std::nullopt;
    }
    const MirFunctionInstance *entry =
        program.findFunctionInstance(startup->entry);
    if (entry == nullptr || entry->entryKind == ProgramEntryKind::None) {
      return std::nullopt;
    }
    result.definition = CppMirBodyDefinitionKind::CompilerGenerated;
    result.declaration = entry->declaration;
    result.concreteOwner = entry->id;
    return result;
  }
  }
  return std::nullopt;
}

CppMirProgramPlan planCppMirProgram(const MirProgram &program,
                                    CppMirRepresentationSnapshot snapshot) {
  CppMirProgramPlan plan;
  const bool inventorySealed =
      snapshot.inventorySeal_.has_value() &&
      snapshot.inventorySeal_->mir == snapshot.mir &&
      snapshot.inventorySeal_->mir.has_value() &&
      *snapshot.inventorySeal_->mir == program &&
      snapshot.inventorySeal_->bodies == snapshot.bodies &&
      snapshot.inventorySeal_->data == snapshot.data &&
      snapshot.inventorySeal_->thunks == snapshot.thunks;
  if (!inventorySealed) {
    addIssue(plan, CppMirPlanIssueKind::InvalidInventorySeal,
             "representation inventory differs from the exact private "
             "builder seal");
  }
  plan.bodies = std::move(snapshot.bodies);
  plan.data = std::move(snapshot.data);
  plan.thunks = std::move(snapshot.thunks);

  const MirVerificationResult verification = verifyMirProgram(program);
  if (!program.valid() || !verification.valid()) {
    addIssue(plan, CppMirPlanIssueKind::InvalidMirProgram,
             verification.errors.empty() ? "MIR program is not marked valid"
                                         : verification.errors.front().message);
  }

  if (!snapshot.mir || *snapshot.mir != program) {
    addIssue(plan, CppMirPlanIssueKind::StaleMirIdentity,
             "representation facts do not describe the exact verified MIR "
             "snapshot");
  }

  const std::vector<MirBodyAddress> addresses =
      enumerateMirBodyAddresses(program);
  std::stable_sort(plan.bodies.begin(), plan.bodies.end(),
                   [&](const CppMirBodyRepresentation &left,
                       const CppMirBodyRepresentation &right) {
                     const std::size_t leftOrder =
                         bodyOrder(addresses, left.identity.address);
                     const std::size_t rightOrder =
                         bodyOrder(addresses, right.identity.address);
                     if (leftOrder != rightOrder) {
                       return leftOrder < rightOrder;
                     }
                     return addressLess(left.identity.address,
                                        right.identity.address);
                   });

  for (std::size_t index = 0; index < addresses.size(); ++index) {
    if (std::find(addresses.begin(), addresses.begin() + index,
                  addresses[index]) != addresses.begin() + index) {
      addIssue(plan, CppMirPlanIssueKind::InvalidMirProgram,
               "core MIR body inventory contains a duplicate address",
               addresses[index]);
    }
  }

  for (const MirBodyAddress address : addresses) {
    const auto first = std::find_if(plan.bodies.begin(), plan.bodies.end(),
                                    [&](const CppMirBodyRepresentation &body) {
                                      return body.identity.address == address;
                                    });
    const std::size_t count = static_cast<std::size_t>(
        std::count_if(plan.bodies.begin(), plan.bodies.end(),
                      [&](const CppMirBodyRepresentation &body) {
                        return body.identity.address == address;
                      }));
    if (count == 0) {
      addIssue(plan, CppMirPlanIssueKind::MissingBodyRow,
               "representation snapshot omitted a core MIR body", address);
      continue;
    }
    if (count != 1) {
      addIssue(plan, CppMirPlanIssueKind::DuplicateBodyRow,
               "representation snapshot contains duplicate body rows", address);
    }

    const std::optional<CppMirBodyIdentity> expected =
        captureCppMirBodyIdentity(program, address);
    if (!expected) {
      addIssue(plan, CppMirPlanIssueKind::InvalidMirProgram,
               "core MIR body address does not resolve to its exact owner",
               address);
      continue;
    }

    for (auto current = first;
         current != plan.bodies.end() && current->identity.address == address;
         ++current) {
      if (current->identity != *expected) {
        addIssue(plan, CppMirPlanIssueKind::StaleBodyIdentity,
                 "body row identity is missing or stale", address);
      }

      const MirBody *mirBody = findMirBody(program, address);
      const CppMirBodyDefinitionKind definition = expected->definition;
      bool roleValid = false;
      switch (definition) {
      case CppMirBodyDefinitionKind::ImplicitSource:
        if (address.kind == MirBodyKind::Module) {
          roleValid =
              current->role == (hasExecutableProgramInitialization(program)
                                    ? CppMirBodyRole::SourceExecutable
                                    : CppMirBodyRole::DataOnly);
        } else {
          roleValid = isInitializerBody(address.kind) &&
                      (current->role == CppMirBodyRole::SourceExecutable ||
                       (current->role == CppMirBodyRole::DataOnly &&
                        mirBody != nullptr &&
                        isCanonicalNoExecutionInitializer(*mirBody)));
        }
        break;
      case CppMirBodyDefinitionKind::Source:
        roleValid = current->role == CppMirBodyRole::SourceExecutable;
        break;
      case CppMirBodyDefinitionKind::CompilerGenerated:
        roleValid = address.kind == MirBodyKind::HostedStartup &&
                    current->role == CppMirBodyRole::SourceExecutable;
        break;
      case CppMirBodyDefinitionKind::RuntimeBinding:
      case CppMirBodyDefinitionKind::Declaration:
        // Exact bodylessness is a core MIR verifier invariant. The planner
        // only accepts the corresponding ABI role after that verification.
        roleValid = current->role == CppMirBodyRole::AbiDeclaration;
        break;
      case CppMirBodyDefinitionKind::Count:
        break;
      }
      if (!roleValid ||
          ordinal(current->role) >= ordinal(CppMirBodyRole::Count)) {
        addIssue(plan, CppMirPlanIssueKind::InvalidBodyRole,
                 "body role disagrees with MIR definition provenance", address);
      }

      const bool executable = current->role == CppMirBodyRole::SourceExecutable;
      const bool validFamilyValue =
          ordinal(current->family) < ordinal(CppMirExecutionFamily::Count);
      const bool familyValid =
          validFamilyValue && familySupportsBody(current->family) &&
          (executable ? current->family != CppMirExecutionFamily::None
                      : current->family == CppMirExecutionFamily::None);
      if (!familyValid) {
        addIssue(plan, CppMirPlanIssueKind::InvalidExecutionFamily,
                 "execution-family claim is invalid for the body role or "
                 "kind",
                 address);
      } else if (executable &&
                 current->family != CppMirExecutionFamily::GeneralV1) {
        // Only the sealed builder can derive GeneralV1 after complete text
        // preflight. Historical or unsupported labels remain inventory-only.
        addUnsupported(plan, CppMirUnsupportedSurfaceKind::Body, address);
      }

      std::sort(current->requiredThunks.begin(), current->requiredThunks.end(),
                thunkLess);
      for (std::size_t dependency = 0;
           dependency < current->requiredThunks.size(); ++dependency) {
        const CppMirThunkIdentity identity =
            current->requiredThunks[dependency];
        if (!validThunkIdentity(identity)) {
          addIssue(plan, CppMirPlanIssueKind::InvalidThunkIdentity,
                   "body names an invalid generated-thunk identity", address,
                   std::nullopt, identity);
        }
        if (dependency != 0 &&
            current->requiredThunks[dependency - 1] == identity) {
          addIssue(plan, CppMirPlanIssueKind::DuplicateBodyThunkDependency,
                   "body names one generated thunk more than once", address,
                   std::nullopt, identity);
        }
      }
      if (!executable && !current->requiredThunks.empty()) {
        addIssue(plan, CppMirPlanIssueKind::InvalidBodyRole,
                 "non-executable body cannot root generated execution "
                 "thunks",
                 address);
      }
    }
  }

  for (const CppMirBodyRepresentation &body : plan.bodies) {
    if (std::find(addresses.begin(), addresses.end(), body.identity.address) ==
        addresses.end()) {
      addIssue(plan, CppMirPlanIssueKind::UnexpectedBodyRow,
               "representation snapshot contains a non-core body address",
               body.identity.address);
    }
  }

  std::sort(plan.data.begin(), plan.data.end(),
            [](const CppMirDataRepresentation &left,
               const CppMirDataRepresentation &right) {
              return dataLess(left.identity, right.identity);
            });
  for (std::size_t index = 0; index < plan.data.size(); ++index) {
    const CppMirDataRepresentation &data = plan.data[index];
    if (!validDataIdentity(data.identity)) {
      addIssue(plan, CppMirPlanIssueKind::InvalidDataIdentity,
               "data-only representation identity is invalid", std::nullopt,
               data.identity);
    }
    if (index != 0 && plan.data[index - 1].identity == data.identity) {
      addIssue(plan, CppMirPlanIssueKind::DuplicateDataIdentity,
               "data-only representation identity is duplicated", std::nullopt,
               data.identity);
    }
    if (ordinal(data.support) >= ordinal(CppMirSurfaceSupport::Count)) {
      addIssue(plan, CppMirPlanIssueKind::InvalidDataSupport,
               "data-only representation has an invalid support state",
               std::nullopt, data.identity);
    } else if (data.support == CppMirSurfaceSupport::Unsupported) {
      addUnsupported(plan, CppMirUnsupportedSurfaceKind::Data, std::nullopt,
                     data.identity);
    }
  }

  std::sort(
      plan.thunks.begin(), plan.thunks.end(),
      [](const CppMirGeneratedThunk &left, const CppMirGeneratedThunk &right) {
        return thunkLess(left.identity, right.identity);
      });
  bool uniqueThunks = true;
  for (std::size_t index = 0; index < plan.thunks.size(); ++index) {
    CppMirGeneratedThunk &thunk = plan.thunks[index];
    const bool validIdentity = validThunkIdentity(thunk.identity);
    if (!validIdentity) {
      addIssue(plan, CppMirPlanIssueKind::InvalidThunkIdentity,
               "generated thunk identity is invalid", std::nullopt,
               std::nullopt, thunk.identity);
    }
    if (index != 0 && plan.thunks[index - 1].identity == thunk.identity) {
      uniqueThunks = false;
      addIssue(plan, CppMirPlanIssueKind::DuplicateThunkIdentity,
               "generated thunk identity is duplicated", std::nullopt,
               std::nullopt, thunk.identity);
    }
    if (ordinal(thunk.support) >= ordinal(CppMirSurfaceSupport::Count)) {
      addIssue(plan, CppMirPlanIssueKind::InvalidThunkSupport,
               "generated thunk has an invalid support state", std::nullopt,
               std::nullopt, thunk.identity);
    } else if (thunk.support == CppMirSurfaceSupport::Unsupported ||
               (validIdentity && !isContractedThunkKind(thunk.identity.kind))) {
      // All other kinds are inventory-only until their snapshot-builder
      // provenance and generic-emitter contracts are sealed.
      addUnsupported(plan, CppMirUnsupportedSurfaceKind::Thunk, std::nullopt,
                     std::nullopt, thunk.identity);
    }

    const auto source =
        std::find_if(plan.bodies.begin(), plan.bodies.end(),
                     [&](const CppMirBodyRepresentation &body) {
                       return body.identity.address == thunk.sourceBody;
                     });
    const std::size_t sourceCount = static_cast<std::size_t>(
        std::count_if(plan.bodies.begin(), plan.bodies.end(),
                      [&](const CppMirBodyRepresentation &body) {
                        return body.identity.address == thunk.sourceBody;
                      }));
    const bool sourceExists = sourceCount == 1 && source != plan.bodies.end() &&
                              findMirBody(program, thunk.sourceBody) != nullptr;
    bool validSource = sourceExists;
    if (validSource && thunk.identity.kind == CppMirThunkKind::HostedEntry) {
      validSource = source->role == CppMirBodyRole::SourceExecutable;
      const std::optional<MirHostedStartupPlan> &startup =
          program.hostedStartupPlan();
      const MirFunctionInstance *entry =
          program.findFunctionInstance(thunk.identity.owner);
      validSource = validSource &&
                    thunk.sourceBody.kind == MirBodyKind::HostedStartup &&
                    thunk.sourceBody.owner == thunk.identity.owner && startup &&
                    startup->entry == thunk.identity.owner &&
                    program.hostedStartup() != nullptr && entry != nullptr &&
                    entry->id == thunk.identity.owner &&
                    entry->definitionKind == MirDefinitionKind::Source &&
                    entry->entryKind != ProgramEntryKind::None;
    } else if (validSource &&
               thunk.identity.kind == CppMirThunkKind::ProgramInitialization) {
      // The verified merged Module/0 Initializer plan is the sole authority
      // for this program-wide thunk. Legacy generic static-initializer bodies
      // remain unsupported inventory and cannot infer this contract.
      validSource =
          thunk.sourceBody ==
              MirBodyAddress{.kind = MirBodyKind::Module, .owner = 0} &&
          source->role == CppMirBodyRole::SourceExecutable &&
          hasExecutableProgramInitialization(program);
    } else if (validSource) {
      validSource = source->role == CppMirBodyRole::SourceExecutable;
    }
    if (!validSource) {
      addIssue(plan, CppMirPlanIssueKind::InvalidThunkSource,
               "generated thunk does not name its exact contracted executable "
               "core body",
               thunk.sourceBody, std::nullopt, thunk.identity);
    }

    std::sort(thunk.dependencies.begin(), thunk.dependencies.end(), thunkLess);
    for (std::size_t dependency = 0; dependency < thunk.dependencies.size();
         ++dependency) {
      const CppMirThunkIdentity identity = thunk.dependencies[dependency];
      if (!validThunkIdentity(identity)) {
        addIssue(plan, CppMirPlanIssueKind::InvalidThunkIdentity,
                 "generated thunk dependency identity is invalid", std::nullopt,
                 std::nullopt, identity);
      }
      if (dependency != 0 && thunk.dependencies[dependency - 1] == identity) {
        addIssue(plan, CppMirPlanIssueKind::DuplicateThunkDependency,
                 "generated thunk dependency is duplicated", std::nullopt,
                 std::nullopt, thunk.identity);
      }
    }
  }

  // Contracted thunks are not an open-ended representation claim. Derive
  // their complete graph independently from verified MIR plus the validated
  // body roles, so a copied snapshot cannot make a coordinated omission or
  // injection look coherent.
  const CppMirThunkIdentity initializationIdentity{
      .kind = CppMirThunkKind::ProgramInitialization, .owner = 0, .ordinal = 0};
  const bool initializationExpected =
      hasExecutableProgramInitialization(program);
  for (const CppMirBodyRepresentation &body : plan.bodies) {
    const bool expectedRoot =
        initializationExpected &&
        body.identity.address ==
            MirBodyAddress{.kind = MirBodyKind::Module, .owner = 0};

    const std::size_t initializationRequirements = static_cast<std::size_t>(
        std::count_if(body.requiredThunks.begin(), body.requiredThunks.end(),
                      [](const CppMirThunkIdentity &identity) {
                        return identity.kind ==
                               CppMirThunkKind::ProgramInitialization;
                      }));
    const std::size_t exactInitializationRequirements =
        static_cast<std::size_t>(std::count(body.requiredThunks.begin(),
                                            body.requiredThunks.end(),
                                            initializationIdentity));
    if (initializationRequirements != (expectedRoot ? 1U : 0U) ||
        exactInitializationRequirements != (expectedRoot ? 1U : 0U)) {
      addIssue(plan, CppMirPlanIssueKind::InvalidContractedThunkGraph,
               expectedRoot
                   ? "executable merged Module initialization must directly "
                     "root the exact program-initialization thunk"
                   : "only exact Module/0 may root "
                     "the program-initialization thunk",
               body.identity.address, std::nullopt, initializationIdentity);
    }
  }

  const std::size_t initializationThunkCount = static_cast<std::size_t>(
      std::count_if(plan.thunks.begin(), plan.thunks.end(),
                    [](const CppMirGeneratedThunk &thunk) {
                      return thunk.identity.kind ==
                             CppMirThunkKind::ProgramInitialization;
                    }));
  const std::size_t exactInitializationThunkCount = static_cast<std::size_t>(
      std::count_if(plan.thunks.begin(), plan.thunks.end(),
                    [&](const CppMirGeneratedThunk &thunk) {
                      return thunk.identity == initializationIdentity;
                    }));
  if (initializationExpected && exactInitializationThunkCount == 0) {
    addIssue(plan, CppMirPlanIssueKind::MissingContractedThunk,
             "executable program-wide initializers require the exact "
             "program-initialization thunk",
             std::nullopt, std::nullopt, initializationIdentity);
  }
  if ((!initializationExpected && initializationThunkCount != 0) ||
      (initializationExpected &&
       initializationThunkCount != exactInitializationThunkCount)) {
    addIssue(plan, CppMirPlanIssueKind::UnexpectedContractedThunk,
             "program-initialization thunk exists without the exact derived "
             "initializer surface",
             std::nullopt, std::nullopt, initializationIdentity);
  }
  for (const CppMirGeneratedThunk &thunk : plan.thunks) {
    if (thunk.identity != initializationIdentity) {
      continue;
    }
    if (!initializationExpected ||
        thunk.sourceBody !=
            MirBodyAddress{.kind = MirBodyKind::Module, .owner = 0} ||
        !thunk.dependencies.empty()) {
      addIssue(plan, CppMirPlanIssueKind::InvalidContractedThunkGraph,
               "program-initialization must have exact Module/0 ownership "
               "and no thunk dependencies",
               thunk.sourceBody, std::nullopt, thunk.identity);
    }
  }

  std::vector<CppMirThunkIdentity> expectedHostedEntries;
  if (const std::optional<MirHostedStartupPlan> &startup =
          program.hostedStartupPlan();
      startup && program.hostedStartup() != nullptr) {
    expectedHostedEntries.push_back({.kind = CppMirThunkKind::HostedEntry,
                                     .owner = startup->entry,
                                     .ordinal = 0});
  }
  std::sort(expectedHostedEntries.begin(), expectedHostedEntries.end(),
            thunkLess);

  const std::size_t initializationBodyCalls =
      programInitializationBodyCallCount(program);
  if (initializationBodyCalls != (initializationExpected ? 1U : 0U)) {
    addIssue(
        plan, CppMirPlanIssueKind::InvalidContractedThunkGraph,
        "hosted startup must call exact Module/0 once if and only if "
        "the verified program-initialization plan is executable",
        program.hostedStartupPlan()
            ? std::optional<MirBodyAddress>{{.kind = MirBodyKind::HostedStartup,
                                             .owner =
                                                 program.hostedStartupPlan()
                                                     ->entry}}
            : std::nullopt,
        std::nullopt, initializationIdentity);
  }

  for (const CppMirThunkIdentity &expected : expectedHostedEntries) {
    const std::size_t count = static_cast<std::size_t>(
        std::count_if(plan.thunks.begin(), plan.thunks.end(),
                      [&](const CppMirGeneratedThunk &thunk) {
                        return thunk.identity == expected;
                      }));
    if (count == 0) {
      addIssue(plan, CppMirPlanIssueKind::MissingContractedThunk,
               "MIR hosted entry requires its exact generated thunk",
               MirBodyAddress{.kind = MirBodyKind::HostedStartup,
                              .owner = expected.owner},
               std::nullopt, expected);
    }
  }
  for (const CppMirGeneratedThunk &thunk : plan.thunks) {
    if (thunk.identity.kind != CppMirThunkKind::HostedEntry) {
      continue;
    }
    if (!containsThunk(expectedHostedEntries, thunk.identity)) {
      addIssue(plan, CppMirPlanIssueKind::UnexpectedContractedThunk,
               "hosted-entry thunk has no exact MIR entry owner",
               thunk.sourceBody, std::nullopt, thunk.identity);
      continue;
    }
    const std::vector<CppMirThunkIdentity> expectedDependencies =
        initializationBodyCalls == 1
            ? std::vector<CppMirThunkIdentity>{initializationIdentity}
            : std::vector<CppMirThunkIdentity>{};
    if (thunk.sourceBody != MirBodyAddress{.kind = MirBodyKind::HostedStartup,
                                           .owner = thunk.identity.owner} ||
        thunk.dependencies != expectedDependencies) {
      addIssue(plan, CppMirPlanIssueKind::InvalidContractedThunkGraph,
               "hosted-entry must have its exact generated startup source "
               "and mirror that body's Module/0 call dependency",
               thunk.sourceBody, std::nullopt, thunk.identity);
    }
  }

  for (const CppMirBodyRepresentation &body : plan.bodies) {
    std::optional<CppMirThunkIdentity> expected;
    if (body.identity.address.kind == MirBodyKind::HostedStartup) {
      const CppMirThunkIdentity candidate{.kind = CppMirThunkKind::HostedEntry,
                                          .owner = body.identity.address.owner,
                                          .ordinal = 0};
      if (containsThunk(expectedHostedEntries, candidate)) {
        expected = candidate;
      }
    }
    const std::size_t hostedRequirements = static_cast<std::size_t>(
        std::count_if(body.requiredThunks.begin(), body.requiredThunks.end(),
                      [](const CppMirThunkIdentity &identity) {
                        return identity.kind == CppMirThunkKind::HostedEntry;
                      }));
    const std::size_t exactHostedRequirements =
        expected ? static_cast<std::size_t>(
                       std::count(body.requiredThunks.begin(),
                                  body.requiredThunks.end(), *expected))
                 : 0;
    if (hostedRequirements != (expected ? 1U : 0U) ||
        exactHostedRequirements != (expected ? 1U : 0U)) {
      addIssue(plan, CppMirPlanIssueKind::InvalidContractedThunkGraph,
               expected ? "generated startup body must directly root its exact "
                          "hosted-entry thunk"
                        : "only the exact generated startup body may root a "
                          "hosted-entry thunk",
               body.identity.address, std::nullopt, expected);
    }
  }

  for (const CppMirBodyRepresentation &body : plan.bodies) {
    for (const CppMirThunkIdentity &required : body.requiredThunks) {
      const CppMirGeneratedThunk *thunk = findThunk(plan.thunks, required);
      if (thunk == nullptr) {
        addIssue(plan, CppMirPlanIssueKind::MissingThunkDependency,
                 "body-required generated thunk is absent",
                 body.identity.address, std::nullopt, required);
      } else if (required.kind == CppMirThunkKind::HostedEntry &&
                 body.identity.address != thunk->sourceBody) {
        addIssue(plan, CppMirPlanIssueKind::InvalidThunkSource,
                 "hosted-entry thunk must be rooted by its exact generated "
                 "startup body",
                 body.identity.address, std::nullopt, required);
      } else if (required.kind == CppMirThunkKind::ProgramInitialization &&
                 (body.role != CppMirBodyRole::SourceExecutable ||
                  body.identity.address !=
                      MirBodyAddress{.kind = MirBodyKind::Module, .owner = 0} ||
                  !initializationExpected)) {
        addIssue(plan, CppMirPlanIssueKind::InvalidThunkSource,
                 "program-initialization thunk must be rooted by exact "
                 "executable Module/0 plan authority",
                 body.identity.address, std::nullopt, required);
      }
    }
  }
  for (const CppMirGeneratedThunk &thunk : plan.thunks) {
    for (const CppMirThunkIdentity &dependency : thunk.dependencies) {
      if (findThunk(plan.thunks, dependency) == nullptr) {
        addIssue(plan, CppMirPlanIssueKind::MissingThunkDependency,
                 "generated thunk dependency is absent", std::nullopt,
                 std::nullopt, thunk.identity);
      }
    }
  }

  std::vector<CppMirThunkIdentity> roots;
  for (const CppMirBodyRepresentation &body : plan.bodies) {
    roots.insert(roots.end(), body.requiredThunks.begin(),
                 body.requiredThunks.end());
  }
  std::sort(roots.begin(), roots.end(), thunkLess);
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());

  std::vector<CppMirThunkIdentity> reachable;
  std::function<void(const CppMirThunkIdentity &)> markReachable =
      [&](const CppMirThunkIdentity &identity) {
        if (containsThunk(reachable, identity)) {
          return;
        }
        reachable.push_back(identity);
        if (const CppMirGeneratedThunk *thunk =
                findThunk(plan.thunks, identity)) {
          for (const CppMirThunkIdentity &dependency : thunk->dependencies) {
            markReachable(dependency);
          }
        }
      };
  for (const CppMirThunkIdentity &root : roots) {
    markReachable(root);
  }
  for (const CppMirGeneratedThunk &thunk : plan.thunks) {
    if (!containsThunk(reachable, thunk.identity)) {
      addIssue(plan, CppMirPlanIssueKind::OrphanThunk,
               "generated thunk is not rooted by any body", std::nullopt,
               std::nullopt, thunk.identity);
    }
  }

  std::vector<CppMirThunkIdentity> visiting;
  std::vector<CppMirThunkIdentity> visited;
  std::vector<CppMirGeneratedThunk> orderedThunks;
  bool acyclic = true;
  std::function<void(const CppMirGeneratedThunk &)> visit =
      [&](const CppMirGeneratedThunk &thunk) {
        if (containsThunk(visited, thunk.identity)) {
          return;
        }
        if (containsThunk(visiting, thunk.identity)) {
          acyclic = false;
          addIssue(plan, CppMirPlanIssueKind::CyclicThunkDependency,
                   "generated thunk dependency graph contains a cycle",
                   std::nullopt, std::nullopt, thunk.identity);
          return;
        }
        visiting.push_back(thunk.identity);
        for (const CppMirThunkIdentity &dependency : thunk.dependencies) {
          if (const CppMirGeneratedThunk *target =
                  findThunk(plan.thunks, dependency)) {
            visit(*target);
          }
        }
        visiting.erase(
            std::remove(visiting.begin(), visiting.end(), thunk.identity),
            visiting.end());
        if (!containsThunk(visited, thunk.identity)) {
          visited.push_back(thunk.identity);
          orderedThunks.push_back(thunk);
        }
      };
  if (uniqueThunks) {
    for (const CppMirGeneratedThunk &thunk : plan.thunks) {
      visit(thunk);
    }
  }
  if (uniqueThunks && acyclic && orderedThunks.size() == plan.thunks.size()) {
    plan.thunks = std::move(orderedThunks);
  }

  plan.status = !plan.issues.empty() ? CppMirProgramPlanStatus::Incoherent
                : !plan.unsupported.empty()
                    ? CppMirProgramPlanStatus::UnsupportedSurface
                    : CppMirProgramPlanStatus::Complete;
  return plan;
}

} // namespace lang
