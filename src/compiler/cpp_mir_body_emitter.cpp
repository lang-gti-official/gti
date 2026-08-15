#include "cpp_mir_body_emitter.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace lang {
namespace {

template <typename Enum>
[[nodiscard]] constexpr std::size_t ordinal(Enum value) {
  return static_cast<std::size_t>(value);
}

[[nodiscard]] CppMirBodyEmissionReadiness
mergeReadiness(CppMirBodyEmissionReadiness left,
               CppMirBodyEmissionReadiness right) {
  return ordinal(left) >= ordinal(right) ? left : right;
}

[[nodiscard]] CppMirBodyEmissionReadiness
readinessForIssue(CppMirBodyEmissionIssueKind kind) {
  switch (kind) {
  case CppMirBodyEmissionIssueKind::MissingTypeRepresentation:
  case CppMirBodyEmissionIssueKind::MissingBodyRepresentation:
  case CppMirBodyEmissionIssueKind::MissingSymbolRepresentation:
  case CppMirBodyEmissionIssueKind::MissingEnumRepresentation:
  case CppMirBodyEmissionIssueKind::MissingCapabilityRepresentation:
    return CppMirBodyEmissionReadiness::MissingRepresentation;
  case CppMirBodyEmissionIssueKind::MissingPackExpansionMir:
  case CppMirBodyEmissionIssueKind::MissingOrderedCompoundMir:
  case CppMirBodyEmissionIssueKind::MissingCheckedFailureControlFlow:
  case CppMirBodyEmissionIssueKind::MissingAggregateRollbackMir:
  case CppMirBodyEmissionIssueKind::MissingCallInputScheduleMir:
  case CppMirBodyEmissionIssueKind::MissingConstructionScheduleMir:
  case CppMirBodyEmissionIssueKind::MissingPartialConstructionRollbackMir:
  case CppMirBodyEmissionIssueKind::MissingFailureCleanupMir:
  case CppMirBodyEmissionIssueKind::MissingProgramInitializationMir:
  case CppMirBodyEmissionIssueKind::MissingHostedStartupMir:
    return CppMirBodyEmissionReadiness::MissingMirAuthority;
  case CppMirBodyEmissionIssueKind::InvalidMirProgram:
  case CppMirBodyEmissionIssueKind::InvalidBodyAddress:
  case CppMirBodyEmissionIssueKind::InvalidRepresentationEnum:
  case CppMirBodyEmissionIssueKind::InvalidRepresentationRow:
  case CppMirBodyEmissionIssueKind::DuplicateTypeRepresentation:
  case CppMirBodyEmissionIssueKind::DuplicateBodyRepresentation:
  case CppMirBodyEmissionIssueKind::DuplicateSymbolRepresentation:
  case CppMirBodyEmissionIssueKind::DuplicateEnumRepresentation:
  case CppMirBodyEmissionIssueKind::DuplicateCapabilityRepresentation:
  case CppMirBodyEmissionIssueKind::InvalidBodyKind:
  case CppMirBodyEmissionIssueKind::InvalidInstructionKind:
  case CppMirBodyEmissionIssueKind::InvalidOperation:
  case CppMirBodyEmissionIssueKind::InvalidOperandKind:
  case CppMirBodyEmissionIssueKind::InvalidPlaceRootKind:
  case CppMirBodyEmissionIssueKind::InvalidProjectionKind:
  case CppMirBodyEmissionIssueKind::InvalidTerminatorKind:
  case CppMirBodyEmissionIssueKind::Count:
    return CppMirBodyEmissionReadiness::Incoherent;
  }
  return CppMirBodyEmissionReadiness::Incoherent;
}

[[nodiscard]] std::optional<CppMirTypeRepresentationKind>
expectedTypeRepresentation(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Void:
    return CppMirTypeRepresentationKind::Void;
  case SemanticType::Int8:
  case SemanticType::Int16:
  case SemanticType::Int32:
  case SemanticType::Int64:
  case SemanticType::UInt8:
  case SemanticType::UInt16:
  case SemanticType::UInt32:
  case SemanticType::UInt64:
  case SemanticType::Float:
  case SemanticType::Double:
  case SemanticType::Bool:
  case SemanticType::Char:
    return CppMirTypeRepresentationKind::Scalar;
  case SemanticType::StringView:
    return CppMirTypeRepresentationKind::StringView;
  case SemanticType::NullPtr:
    return CppMirTypeRepresentationKind::NullPointer;
  case SemanticType::RawPointer:
    return CppMirTypeRepresentationKind::RawPointer;
  case SemanticType::Array:
    return CppMirTypeRepresentationKind::FixedArray;
  case SemanticType::Class:
    return CppMirTypeRepresentationKind::Class;
  case SemanticType::Enum:
    return CppMirTypeRepresentationKind::Enum;
  case SemanticType::Reference:
    return CppMirTypeRepresentationKind::Reference;
  case SemanticType::UniqueOwner:
    return CppMirTypeRepresentationKind::UniqueOwner;
  case SemanticType::SharedPointer:
    return CppMirTypeRepresentationKind::SharedPointer;
  case SemanticType::Storage:
    return CppMirTypeRepresentationKind::Storage;
  case SemanticType::TypeParameter:
  case SemanticType::TypePack:
  case SemanticType::TypeName:
    return CppMirTypeRepresentationKind::Meta;
  case SemanticType::Function:
    return CppMirTypeRepresentationKind::Function;
  case SemanticType::Lambda:
    return CppMirTypeRepresentationKind::Lambda;
  case SemanticType::Expected:
    return CppMirTypeRepresentationKind::Expected;
  case SemanticType::Unexpected:
    return CppMirTypeRepresentationKind::Unexpected;
  case SemanticType::Unknown:
    return std::nullopt;
  }
  return std::nullopt;
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

[[nodiscard]] const MirInstruction *findInstruction(const MirBody &body,
                                                    MirInstructionId id) {
  if (id == 0) {
    return nullptr;
  }
  for (const MirBlock &block : body.blocks) {
    const auto found =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [id](const MirInstruction &instruction) {
                       return instruction.id == id;
                     });
    if (found != block.instructions.end()) {
      return &*found;
    }
  }
  return nullptr;
}

[[nodiscard]] const MirInstruction *definitionFor(const MirBody &body,
                                                  const MirOperand &operand) {
  if (operand.kind != MirOperandKind::Value || operand.value == 0) {
    return nullptr;
  }
  const MirValue *value = body.findValue(operand.value);
  return value == nullptr ? nullptr : findInstruction(body, value->definition);
}

[[nodiscard]] bool hasExactCallInput(const MirBody &body,
                                     const MirOperand &operand,
                                     HirValueId callSite, MirCallInputRole role,
                                     std::size_t index) {
  const MirInstruction *input = definitionFor(body, operand);
  return input != nullptr && input->kind == MirInstructionKind::CallInput &&
         input->callSite == callSite && input->callInputRole == role &&
         input->callInputIndex == index;
}

[[nodiscard]] bool hasCompleteCallInputSchedule(const MirBody &body,
                                                const MirInstruction &call) {
  if (call.callSite == 0) {
    // HostedStartup is compiler-generated and has no source HIR call site.
    // Its nonzero operation tag is closed over the exact call/input schedule
    // by verifyMirProgram before this private classifier runs.
    if (body.kind == MirBodyKind::HostedStartup &&
        call.hostedStartupOperation != 0) {
      return true;
    }
    return !call.receiver && call.operands.empty();
  }
  if (call.receiver && !hasExactCallInput(body, *call.receiver, call.callSite,
                                          MirCallInputRole::Receiver, 0)) {
    return false;
  }
  for (std::size_t index = 0; index < call.operands.size(); ++index) {
    if (!hasExactCallInput(body, call.operands[index], call.callSite,
                           MirCallInputRole::Argument, index)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
isHostedStartupArgumentIndexAdvance(const MirBody &body,
                                    const MirInstruction &instruction) {
  // The hosted-startup verifier binds every nonzero operation tag to one
  // exact plan row. It is therefore the sole authority for the generated
  // Modify/PreIncrement schedule; source Modify remains unsupported here.
  return body.kind == MirBodyKind::HostedStartup &&
         instruction.hostedStartupOperation != 0 &&
         instruction.kind == MirInstructionKind::Modify &&
         instruction.operation == MirOperation::PreIncrement;
}

[[nodiscard]] bool instructionHasInvoke(const MirBlock &block,
                                        const MirInstruction &instruction) {
  return block.terminator.kind == MirTerminatorKind::Invoke &&
         block.terminator.invokeInstruction == instruction.id;
}

[[nodiscard]] bool classHasStateBearingBase(const MirClassInstance &instance) {
  return std::any_of(
      instance.bases.begin(), instance.bases.end(),
      [](const HirBaseInstance &base) { return !base.interface; });
}

class BodyAnalysisBuilder {
public:
  BodyAnalysisBuilder(const MirProgram &program,
                      const CppMirBodyEmissionMap &representations,
                      MirBodyAddress address)
      : program(program), representations(representations) {
    result.body = address;
    result.readiness = CppMirBodyEmissionReadiness::Ready;
  }

  [[nodiscard]] CppMirBodyEmissionAnalysis run(bool validateProgramAndMap) {
    if (validateProgramAndMap) {
      validateProgram();
      validateRepresentations();
    }

    const MirBody *body = findMirBody(program, result.body);
    if (body == nullptr || body->kind != result.body.kind) {
      add(CppMirBodyEmissionIssueKind::InvalidBodyAddress, 0, 0,
          "MIR body address does not resolve to its exact core owner");
      return std::move(result);
    }
    if (classifyCppMirBodyKind(body->kind) == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidBodyKind, 0, 0,
          "MIR body kind is outside the exhaustive emitter vocabulary");
      return std::move(result);
    }

    scanOwnerMetadata(*body);
    scanBody(*body);
    return std::move(result);
  }

  void validateProgram() {
    const MirVerificationResult verification = verifyMirProgram(program);
    if (!program.valid() || !verification.valid()) {
      if (verification.errors.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidMirProgram, 0, 0,
            "MIR program is not marked valid");
        return;
      }
      for (const MirVerificationError &error : verification.errors) {
        add(CppMirBodyEmissionIssueKind::InvalidMirProgram, error.block,
            error.instruction, error.message);
      }
    }
  }

  void validateRepresentations() {
    for (std::size_t index = 0; index < representations.types().size();
         ++index) {
      const CppMirTypeRepresentation &row = representations.types()[index];
      const std::optional<CppMirTypeRepresentationKind> expected =
          expectedTypeRepresentation(row.type);
      if (!expected ||
          ordinal(row.kind) >= ordinal(CppMirTypeRepresentationKind::Count)) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, 0, 0,
            "type row has an invalid semantic or representation kind");
      } else if (*expected != row.kind || row.spelling.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "type row disagrees with the exact semantic type");
      }
      if (std::find_if(representations.types().begin(),
                       representations.types().begin() + index,
                       [&](const CppMirTypeRepresentation &prior) {
                         return prior.type == row.type;
                       }) != representations.types().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateTypeRepresentation, 0, 0,
            "copied map contains duplicate exact type rows");
      }
    }

    for (std::size_t index = 0; index < representations.bodies().size();
         ++index) {
      const CppMirBodyNameRepresentation &row = representations.bodies()[index];
      if (findMirBody(program, row.address) == nullptr ||
          row.spelling.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "body-name row is stale or empty");
      }
      if (std::find_if(representations.bodies().begin(),
                       representations.bodies().begin() + index,
                       [&](const CppMirBodyNameRepresentation &prior) {
                         return prior.address == row.address;
                       }) != representations.bodies().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateBodyRepresentation, 0, 0,
            "copied map contains duplicate body-name rows");
      }
    }

    for (std::size_t index = 0; index < representations.symbols().size();
         ++index) {
      const CppMirSymbolRepresentation &row = representations.symbols()[index];
      const bool enumValid =
          ordinal(row.kind) < ordinal(CppMirSymbolRepresentationKind::Count);
      const bool ownerValid =
          row.kind == CppMirSymbolRepresentationKind::Storage || row.owner != 0;
      const bool ordinalValid =
          row.kind == CppMirSymbolRepresentationKind::Capture
              ? row.ordinal != 0
              : row.ordinal == 0;
      if (!enumValid) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, 0, 0,
            "symbol row has an invalid representation kind");
      } else if (!ownerValid || !ordinalValid || row.symbol == 0 ||
                 row.type == SemanticType::Unknown || row.spelling.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "symbol row has an invalid owner, identity, type, or spelling");
      }
      if (std::find_if(representations.symbols().begin(),
                       representations.symbols().begin() + index,
                       [&](const CppMirSymbolRepresentation &prior) {
                         return prior.kind == row.kind &&
                                prior.owner == row.owner &&
                                prior.symbol == row.symbol &&
                                prior.ordinal == row.ordinal;
                       }) != representations.symbols().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateSymbolRepresentation, 0, 0,
            "copied map contains duplicate symbol rows");
      }
    }

    for (std::size_t index = 0; index < representations.enums().size();
         ++index) {
      const CppMirEnumRepresentation &row = representations.enums()[index];
      if (row.owner == 0 || row.spelling.empty() ||
          row.underlyingType == SemanticType::Unknown) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "enum row has an invalid owner, underlying type, or spelling");
      }
      if (std::find_if(representations.enums().begin(),
                       representations.enums().begin() + index,
                       [&](const CppMirEnumRepresentation &prior) {
                         return prior.owner == row.owner;
                       }) != representations.enums().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateEnumRepresentation, 0, 0,
            "copied map contains duplicate enum rows");
      }
      for (std::size_t variant = 0; variant < row.payloadVariants.size();
           ++variant) {
        const CppMirPayloadVariantRepresentation &current =
            row.payloadVariants[variant];
        if (current.spelling.empty() ||
            std::any_of(current.fieldTypes.begin(), current.fieldTypes.end(),
                        [](const SemanticType &type) {
                          return type == SemanticType::Unknown;
                        })) {
          add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
              "payload-variant row has an empty spelling or unknown field");
        }
        if (std::find_if(row.payloadVariants.begin(),
                         row.payloadVariants.begin() + variant,
                         [&](const CppMirPayloadVariantRepresentation &prior) {
                           return prior.index == current.index;
                         }) != row.payloadVariants.begin() + variant) {
          add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
              "enum row contains a duplicate payload variant index");
        }
      }
    }

    for (std::size_t index = 0; index < representations.capabilities().size();
         ++index) {
      const CppMirEmissionCapabilityRepresentation &row =
          representations.capabilities()[index];
      if (ordinal(row.kind) >= ordinal(CppMirEmissionCapabilityKind::Count)) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, 0, 0,
            "capability row has an invalid representation kind");
      } else if (row.spelling.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "capability row has an empty helper spelling");
      }
      if (std::find_if(
              representations.capabilities().begin(),
              representations.capabilities().begin() + index,
              [&](const CppMirEmissionCapabilityRepresentation &prior) {
                return prior.kind == row.kind;
              }) != representations.capabilities().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateCapabilityRepresentation, 0,
            0, "copied map contains duplicate capability rows");
      }
    }
  }

  [[nodiscard]] CppMirBodyEmissionAnalysis finishValidation() {
    return std::move(result);
  }

private:
  void add(CppMirBodyEmissionIssueKind kind, MirBlockId block,
           MirInstructionId instruction, std::string detail) {
    const auto duplicate =
        std::find_if(result.issues.begin(), result.issues.end(),
                     [&](const CppMirBodyEmissionIssue &issue) {
                       return issue.kind == kind && issue.block == block &&
                              issue.instruction == instruction &&
                              issue.detail == detail;
                     });
    if (duplicate != result.issues.end()) {
      return;
    }
    result.readiness =
        mergeReadiness(result.readiness, readinessForIssue(kind));
    result.issues.push_back({.kind = kind,
                             .body = result.body,
                             .block = block,
                             .instruction = instruction,
                             .detail = std::move(detail)});
  }

  [[nodiscard]] const CppMirTypeRepresentation *
  findType(const SemanticType &type) const {
    const auto found = std::find_if(
        representations.types().begin(), representations.types().end(),
        [&](const CppMirTypeRepresentation &row) { return row.type == type; });
    return found == representations.types().end() ? nullptr : &*found;
  }

  void requireType(const SemanticType &type, MirBlockId block = 0,
                   MirInstructionId instruction = 0) {
    if (type == SemanticType::Unknown) {
      add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block,
          instruction, "executable MIR references an unknown semantic type");
      return;
    }
    const CppMirTypeRepresentation *row = findType(type);
    if (row == nullptr) {
      add(CppMirBodyEmissionIssueKind::MissingTypeRepresentation, block,
          instruction, "copied map has no row for an exact MIR type");
    } else if (expectedTypeRepresentation(type) != row->kind ||
               row->spelling.empty()) {
      add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block,
          instruction, "copied type row is stale or structurally mismatched");
    }
    for (const SemanticType &argument : type.arguments) {
      requireType(argument, block, instruction);
    }
    for (const SemanticType &argument : type.lambdaEnclosingClassTypes) {
      requireType(argument, block, instruction);
    }
    for (const SemanticType &argument : type.lambdaEnclosingFunctionTypes) {
      requireType(argument, block, instruction);
    }
  }

  void requireBody(MirBodyAddress address, MirBlockId block = 0,
                   MirInstructionId instruction = 0) {
    const auto found = std::find_if(
        representations.bodies().begin(), representations.bodies().end(),
        [&](const CppMirBodyNameRepresentation &row) {
          return row.address == address;
        });
    if (found == representations.bodies().end()) {
      add(CppMirBodyEmissionIssueKind::MissingBodyRepresentation, block,
          instruction,
          "copied map has no emitted name for an exact MIR body target");
    } else if (found->spelling.empty()) {
      add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block,
          instruction, "body-name row has an empty spelling");
    }
  }

  [[nodiscard]] const CppMirSymbolRepresentation *
  findSymbol(CppMirSymbolRepresentationKind kind, std::size_t owner,
             SymbolId symbol, std::size_t ordinalValue = 0,
             bool *ambiguousStorage = nullptr) const {
    if (ambiguousStorage != nullptr) {
      *ambiguousStorage = false;
    }
    if (kind == CppMirSymbolRepresentationKind::Storage && owner == 0) {
      const CppMirSymbolRepresentation *only = nullptr;
      std::size_t matches = 0;
      for (const CppMirSymbolRepresentation &row : representations.symbols()) {
        if (row.kind != kind || row.symbol != symbol ||
            row.ordinal != ordinalValue) {
          continue;
        }
        only = &row;
        ++matches;
      }
      if (ambiguousStorage != nullptr) {
        *ambiguousStorage = matches > 1;
      }
      return matches == 1 ? only : nullptr;
    }

    const auto exact = std::find_if(
        representations.symbols().begin(), representations.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == kind && row.owner == owner &&
                 row.symbol == symbol && row.ordinal == ordinalValue;
        });
    if (exact != representations.symbols().end()) {
      return &*exact;
    }
    if (kind == CppMirSymbolRepresentationKind::Storage) {
      const auto namespaceStorage = std::find_if(
          representations.symbols().begin(), representations.symbols().end(),
          [&](const CppMirSymbolRepresentation &row) {
            return row.kind == kind && row.owner == 0 && row.symbol == symbol &&
                   row.ordinal == ordinalValue;
          });
      return namespaceStorage == representations.symbols().end()
                 ? nullptr
                 : &*namespaceStorage;
    }
    return nullptr;
  }

  void requireSymbol(CppMirSymbolRepresentationKind kind, std::size_t owner,
                     SymbolId symbol, const SemanticType *type,
                     std::size_t ordinalValue, MirBlockId block = 0,
                     MirInstructionId instruction = 0) {
    bool ambiguousStorage = false;
    const CppMirSymbolRepresentation *row =
        findSymbol(kind, owner, symbol, ordinalValue, &ambiguousStorage);
    if (row == nullptr) {
      if (ambiguousStorage) {
        add(CppMirBodyEmissionIssueKind::MissingProgramInitializationMir, block,
            instruction,
            "MIR does not identify which concrete static-storage owner a "
            "same-symbol representation row denotes");
        return;
      }
      add(CppMirBodyEmissionIssueKind::MissingSymbolRepresentation, block,
          instruction,
          "copied map has no exact storage, field, or capture name row");
      return;
    }
    if ((type != nullptr && row->type != *type) || row->spelling.empty()) {
      add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block,
          instruction, "symbol row type or spelling disagrees with MIR");
    }
  }

  [[nodiscard]] const CppMirEnumRepresentation *findEnum(EnumId owner) const {
    const auto found = std::find_if(
        representations.enums().begin(), representations.enums().end(),
        [owner](const CppMirEnumRepresentation &row) {
          return row.owner == owner;
        });
    return found == representations.enums().end() ? nullptr : &*found;
  }

  const CppMirEnumRepresentation *
  requireEnum(EnumId owner, MirBlockId block = 0,
              MirInstructionId instruction = 0) {
    const CppMirEnumRepresentation *row = findEnum(owner);
    if (row == nullptr) {
      add(CppMirBodyEmissionIssueKind::MissingEnumRepresentation, block,
          instruction,
          "copied map has no declaration row for an exact MIR enum");
    } else {
      requireType(row->underlyingType, block, instruction);
    }
    return row;
  }

  void requireCapability(CppMirEmissionCapabilityKind kind,
                         MirBlockId block = 0,
                         MirInstructionId instruction = 0) {
    const auto found =
        std::find_if(representations.capabilities().begin(),
                     representations.capabilities().end(),
                     [kind](const CppMirEmissionCapabilityRepresentation &row) {
                       return row.kind == kind;
                     });
    if (found == representations.capabilities().end()) {
      add(CppMirBodyEmissionIssueKind::MissingCapabilityRepresentation, block,
          instruction,
          "copied map lacks a required sealed representation helper");
    }
  }

  [[nodiscard]] std::optional<HirClassInstanceId>
  classInstanceForType(const SemanticType &type) const {
    std::optional<HirClassInstanceId> resultId;
    for (const MirClassInstance &instance : program.classInstances()) {
      if (instance.type != type) {
        continue;
      }
      if (resultId) {
        return std::nullopt;
      }
      resultId = instance.id;
    }
    return resultId;
  }

  [[nodiscard]] std::optional<SemanticType> thisType() const {
    switch (result.body.kind) {
    case MirBodyKind::FieldInitializers:
    case MirBodyKind::StaticFieldInitializers:
      if (const MirClassInstance *instance =
              program.findClassInstance(result.body.owner)) {
        return instance->type;
      }
      return std::nullopt;
    case MirBodyKind::Function:
      if (const MirFunctionInstance *function =
              program.findFunctionInstance(result.body.owner);
          function != nullptr && function->owner) {
        if (const MirClassInstance *instance =
                program.findClassInstance(*function->owner)) {
          return instance->type;
        }
      }
      return std::nullopt;
    case MirBodyKind::Constructor:
      if (const MirConstructorInstance *constructor =
              program.findConstructorInstance(result.body.owner)) {
        if (const MirClassInstance *instance =
                program.findClassInstance(constructor->owner)) {
          return instance->type;
        }
      }
      return std::nullopt;
    case MirBodyKind::Destructor:
      if (const MirDestructorInstance *destructor =
              program.findDestructorInstance(result.body.owner)) {
        if (const MirClassInstance *instance =
                program.findClassInstance(destructor->owner)) {
          return instance->type;
        }
      }
      return std::nullopt;
    case MirBodyKind::Module:
    case MirBodyKind::Lambda:
    case MirBodyKind::HostedStartup:
      return std::nullopt;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<HirClassInstanceId> concreteClassOwner() const {
    switch (result.body.kind) {
    case MirBodyKind::FieldInitializers:
    case MirBodyKind::StaticFieldInitializers:
      return result.body.owner;
    case MirBodyKind::Function:
      if (const MirFunctionInstance *function =
              program.findFunctionInstance(result.body.owner)) {
        return function->owner;
      }
      return std::nullopt;
    case MirBodyKind::Constructor:
      if (const MirConstructorInstance *constructor =
              program.findConstructorInstance(result.body.owner)) {
        return constructor->owner;
      }
      return std::nullopt;
    case MirBodyKind::Destructor:
      if (const MirDestructorInstance *destructor =
              program.findDestructorInstance(result.body.owner)) {
        return destructor->owner;
      }
      return std::nullopt;
    case MirBodyKind::Module:
    case MirBodyKind::Lambda:
    case MirBodyKind::HostedStartup:
      return std::nullopt;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::size_t storageOwner(SymbolId symbol) const {
    if (const MirProgramInitializationStep *step =
            program.programInitializationPlan().findStepForSymbol(symbol)) {
      return step->ownerClass;
    }
    return concreteClassOwner().value_or(0);
  }

  [[nodiscard]] static bool sameRoot(const MirPlace &left,
                                     const MirPlace &right) {
    if (left.root != right.root) {
      return false;
    }
    switch (left.root) {
    case MirPlaceRootKind::Binding:
      return left.binding == right.binding;
    case MirPlaceRootKind::Symbol:
      return left.symbol == right.symbol && left.capture == right.capture;
    case MirPlaceRootKind::This:
      return true;
    case MirPlaceRootKind::Temporary:
      return left.temporary == right.temporary;
    case MirPlaceRootKind::Value:
      return left.value == right.value;
    case MirPlaceRootKind::Loan:
      return left.loan == right.loan;
    }
    return false;
  }

  [[nodiscard]] std::optional<SemanticType>
  rootType(const MirBody &body, const MirPlace &place) const {
    if (place.projections.empty()) {
      return place.type;
    }
    if (place.root == MirPlaceRootKind::This) {
      return thisType();
    }
    if (place.root == MirPlaceRootKind::Value) {
      const MirValue *value = body.findValue(place.value);
      return value == nullptr ? std::nullopt
                              : std::optional<SemanticType>{value->info.type};
    }
    if (place.root == MirPlaceRootKind::Loan) {
      const MirLoan *loan = body.findLoan(place.loan);
      const MirPlace *source =
          loan == nullptr ? nullptr : body.findPlace(loan->source);
      return source == nullptr ? std::nullopt
                               : std::optional<SemanticType>{source->type};
    }
    const auto root = std::find_if(
        body.places.begin(), body.places.end(), [&](const MirPlace &candidate) {
          return candidate.projections.empty() && sameRoot(candidate, place);
        });
    if (root != body.places.end()) {
      return root->type;
    }
    if (place.root == MirPlaceRootKind::Symbol) {
      const CppMirSymbolRepresentationKind kind =
          place.capture == 0 ? CppMirSymbolRepresentationKind::Storage
                             : CppMirSymbolRepresentationKind::Capture;
      const std::size_t owner =
          place.capture == 0 ? storageOwner(place.symbol) : result.body.owner;
      if (const CppMirSymbolRepresentation *row =
              findSymbol(kind, owner, place.symbol, place.capture)) {
        return row->type;
      }
    }
    return std::nullopt;
  }

  void scanOwnerMetadata(const MirBody &body) {
    const bool executableBody =
        result.body.kind == MirBodyKind::Module
            ? hasExecutableProgramInitialization(program)
            : !isCanonicalNoExecutionInitializer(body);
    if (executableBody) {
      requireBody(result.body);
    }
    requireType(body.returnType);

    switch (result.body.kind) {
    case MirBodyKind::Module:
      if (hasExecutableProgramInitialization(program)) {
        requireCapability(CppMirEmissionCapabilityKind::ProgramInitialization);
      }
      return;
    case MirBodyKind::FieldInitializers:
      if (!isCanonicalNoExecutionInitializer(body)) {
        add(CppMirBodyEmissionIssueKind::MissingConstructionScheduleMir, 0, 0,
            "declaration field initializers lack a complete constructor "
            "destination and partial-construction schedule");
      }
      if (const MirClassInstance *owner =
              program.findClassInstance(result.body.owner)) {
        requireType(owner->type);
      }
      return;
    case MirBodyKind::StaticFieldInitializers:
      if (!isCanonicalNoExecutionInitializer(body)) {
        requireCapability(CppMirEmissionCapabilityKind::ProgramInitialization);
        add(CppMirBodyEmissionIssueKind::MissingProgramInitializationMir, 0, 0,
            "static-field initialization is not yet merged into the verified "
            "program initialization walk");
      }
      if (const MirClassInstance *owner =
              program.findClassInstance(result.body.owner)) {
        requireType(owner->type);
      }
      return;
    case MirBodyKind::Function: {
      const MirFunctionInstance *function =
          program.findFunctionInstance(result.body.owner);
      if (function == nullptr) {
        return;
      }
      requireType(function->returnType);
      for (const SemanticType &type : function->parameterTypes) {
        requireType(type);
      }
      if (function->owner) {
        const MirClassInstance *owner =
            program.findClassInstance(*function->owner);
        if (owner != nullptr) {
          requireType(owner->type);
        }
      }
      if (function->entryKind != ProgramEntryKind::None) {
        requireCapability(CppMirEmissionCapabilityKind::HostedEntry);
      }
      if (function->linkage == LanguageLinkage::C ||
          function->definitionKind == MirDefinitionKind::RuntimeBinding) {
        requireCapability(CppMirEmissionCapabilityKind::NativeInterop);
      }
      if (function->virtualMethod || function->pureVirtual ||
          function->overrideMethod) {
        requireCapability(CppMirEmissionCapabilityKind::VirtualDispatch);
      }
      if (!function->callableParameters.empty()) {
        requireCapability(CppMirEmissionCapabilityKind::CallableDispatch);
      }
      for (const MirCallableParameter &parameter :
           function->callableParameters) {
        requireType(parameter.callableType);
        for (const MirCallableSignature &signature : parameter.signatures) {
          requireType(signature.returnType);
          for (const SemanticType &type : signature.parameterTypes) {
            requireType(type);
          }
          if (signature.functionTarget) {
            requireBody({.kind = MirBodyKind::Function,
                         .owner = *signature.functionTarget});
          }
          if (signature.lambdaTarget) {
            requireBody({.kind = MirBodyKind::Lambda,
                         .owner = *signature.lambdaTarget});
          }
        }
      }
      return;
    }
    case MirBodyKind::Constructor: {
      const MirConstructorInstance *constructor =
          program.findConstructorInstance(result.body.owner);
      if (constructor == nullptr) {
        return;
      }
      const MirClassInstance *owner =
          program.findClassInstance(constructor->owner);
      if (owner != nullptr) {
        requireType(owner->type);
      }
      for (const SemanticType &type : constructor->parameterTypes) {
        requireType(type);
      }
      for (const MirConstructorInitializer &initializer :
           constructor->initializers) {
        requireType(initializer.targetType);
        if (initializer.constructorTarget) {
          requireBody({.kind = MirBodyKind::Constructor,
                       .owner = *initializer.constructorTarget});
        }
      }
      if (constructor->definitionKind == MirDefinitionKind::Source &&
          (constructor->mayRaiseDefinedFailure ||
           (owner != nullptr && (owner->requiresActiveCleanup ||
                                 classHasStateBearingBase(*owner))))) {
        add(CppMirBodyEmissionIssueKind::MissingPartialConstructionRollbackMir,
            0, 0,
            "constructor lacks general initialized-subobject rollback and "
            "failure cleanup authority");
      }
      return;
    }
    case MirBodyKind::Destructor: {
      const MirDestructorInstance *destructor =
          program.findDestructorInstance(result.body.owner);
      if (destructor == nullptr) {
        return;
      }
      const MirClassInstance *owner =
          program.findClassInstance(destructor->owner);
      if (owner != nullptr) {
        requireType(owner->type);
      }
      if (destructor->definitionKind == MirDefinitionKind::Source &&
          destructor->mayRaiseDefinedFailure) {
        add(CppMirBodyEmissionIssueKind::MissingFailureCleanupMir, 0, 0,
            "failure-capable cleanup requires the emergency double-failure "
            "control path");
      }
      if (owner != nullptr &&
          (classHasStateBearingBase(*owner) ||
           std::any_of(owner->fields.begin(), owner->fields.end(),
                       [](const MirClassFieldLifecycle &field) {
                         return field.requiresActiveCleanup;
                       }))) {
        add(CppMirBodyEmissionIssueKind::MissingConstructionScheduleMir, 0, 0,
            "general field/base destruction composition is not yet a complete "
            "MIR body schedule");
      }
      return;
    }
    case MirBodyKind::Lambda: {
      const MirLambdaInstance *lambda = program.findLambda(result.body.owner);
      if (lambda == nullptr) {
        return;
      }
      requireCapability(CppMirEmissionCapabilityKind::Closure);
      requireType(lambda->type);
      requireType(lambda->returnType);
      for (const SemanticType &type : lambda->parameterTypes) {
        requireType(type);
      }
      for (std::size_t index = 0; index < lambda->captureTypes.size();
           ++index) {
        requireType(lambda->captureTypes[index]);
        if (index < lambda->captureSymbols.size() &&
            lambda->captureSymbols[index] != 0) {
          requireSymbol(CppMirSymbolRepresentationKind::Capture, lambda->id,
                        lambda->captureSymbols[index],
                        &lambda->captureTypes[index], index + 1);
        }
      }
      return;
    }
    case MirBodyKind::HostedStartup:
      requireCapability(CppMirEmissionCapabilityKind::HostedEntry);
      add(CppMirBodyEmissionIssueKind::MissingFailureCleanupMir, 0, 0,
          "compiler-generated hosted startup lacks the Stage-E terminal "
          "failure-containment path");
      if (std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                      [](const MirDropObligation &obligation) {
                        return obligation.dropType.requiresActiveCleanup;
                      })) {
        add(CppMirBodyEmissionIssueKind::MissingPartialConstructionRollbackMir,
            0, 0,
            "owned hosted arguments lack the Stage-E partial-construction "
            "rollback and transfer envelope");
      }
      return;
    }
  }

  void scanBody(const MirBody &body) {
    for (const MirPlace &place : body.places) {
      scanPlace(body, place);
    }
    bool ownsFailureCleanup = false;
    for (const MirDropObligation &obligation : body.dropObligations) {
      requireType(obligation.dropType.type);
      if (obligation.dropType.destructor) {
        requireBody({.kind = MirBodyKind::Destructor,
                     .owner = *obligation.dropType.destructor});
      }
      if (obligation.dropType.requiresActiveCleanup) {
        ownsFailureCleanup = true;
        requireCapability(CppMirEmissionCapabilityKind::LifetimeStorage);
      }
    }
    if (!body.failureRecords.empty() && ownsFailureCleanup) {
      add(CppMirBodyEmissionIssueKind::MissingFailureCleanupMir, 0, 0,
          "a general failure-capable body with live cleanup owners lacks a "
          "sealed whole-component cleanup and containment proof");
    }
    for (const MirValue &value : body.values) {
      requireType(value.info.type);
    }

    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        scanInstruction(body, block, instruction);
      }
      scanTerminator(block);
    }
  }

  void scanPlace(const MirBody &body, const MirPlace &place) {
    requireType(place.type);
    const CppMirEmissionEncoding root = classifyCppMirPlaceRootKind(place.root);
    if (root == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidPlaceRootKind, 0, 0,
          "place root is outside the exhaustive emitter vocabulary");
    }
    std::optional<SemanticType> currentType = rootType(body, place);
    if (place.root == MirPlaceRootKind::Binding &&
        result.body.kind == MirBodyKind::Module) {
      const MirProgramInitializationStep *step =
          program.programInitializationPlan().findStepForSymbol(place.symbol);
      if (step == nullptr || step->binding != place.binding ||
          step->storagePlace != place.id) {
        add(CppMirBodyEmissionIssueKind::InvalidMirProgram, 0, 0,
            "Module binding place has no exact program-initialization row");
      } else {
        requireSymbol(CppMirSymbolRepresentationKind::Storage, step->ownerClass,
                      place.symbol, currentType ? &*currentType : nullptr, 0);
      }
    } else if (place.root == MirPlaceRootKind::Symbol) {
      if (place.capture != 0 && result.body.kind == MirBodyKind::Lambda) {
        requireSymbol(CppMirSymbolRepresentationKind::Capture,
                      result.body.owner, place.symbol,
                      currentType ? &*currentType : nullptr, place.capture);
      } else {
        requireSymbol(CppMirSymbolRepresentationKind::Storage,
                      storageOwner(place.symbol), place.symbol,
                      currentType ? &*currentType : nullptr, 0);
      }
    } else if (place.root == MirPlaceRootKind::This) {
      if (result.body.kind == MirBodyKind::Function) {
        const MirFunctionInstance *function =
            program.findFunctionInstance(result.body.owner);
        if (function != nullptr && function->owner) {
          const MirClassInstance *owner =
              program.findClassInstance(*function->owner);
          if (owner != nullptr) {
            requireType(owner->type);
          }
        }
      }
    } else if (place.root == MirPlaceRootKind::Loan) {
      requireCapability(CppMirEmissionCapabilityKind::Borrow);
    }

    if (!currentType) {
      add(CppMirBodyEmissionIssueKind::MissingTypeRepresentation, 0, 0,
          "projected place needs an explicit copied root-type row; MIR does "
          "not identify a unique concrete root type");
    }

    for (const MirPlaceProjection &projection : place.projections) {
      const CppMirEmissionEncoding encoding =
          classifyCppMirProjectionKind(projection.kind);
      if (encoding == CppMirEmissionEncoding::Invalid) {
        add(CppMirBodyEmissionIssueKind::InvalidProjectionKind, 0, 0,
            "place projection is outside the exhaustive emitter vocabulary");
        continue;
      }
      switch (projection.kind) {
      case MirProjectionKind::Field: {
        const std::optional<HirClassInstanceId> owner =
            currentType ? classInstanceForType(*currentType) : std::nullopt;
        const MirClassInstance *instance =
            owner ? program.findClassInstance(*owner) : nullptr;
        const auto field =
            instance == nullptr
                ? std::vector<MirClassFieldInfo>::const_iterator{}
                : std::find_if(instance->declaredFields.begin(),
                               instance->declaredFields.end(),
                               [&](const MirClassFieldInfo &candidate) {
                                 return candidate.symbol == projection.field;
                               });
        if (instance == nullptr || field == instance->declaredFields.end()) {
          add(CppMirBodyEmissionIssueKind::MissingSymbolRepresentation, 0, 0,
              "field projection cannot be keyed to one exact concrete class "
              "instance from its evolving place type");
          currentType.reset();
        } else {
          requireSymbol(CppMirSymbolRepresentationKind::Field, instance->id,
                        projection.field, &field->type, 0);
          currentType = field->type;
        }
        break;
      }
      case MirProjectionKind::Index:
        requireCapability(CppMirEmissionCapabilityKind::Bounds);
        if (!projection.constantIndex) {
          add(CppMirBodyEmissionIssueKind::MissingCheckedFailureControlFlow, 0,
              0,
              "dynamic safe-index projection has no exact checked "
              "instruction, failure record, or Invoke successor");
        }
        if (currentType && currentType->kind == SemanticType::Array &&
            currentType->arguments.size() == 1) {
          // Copy before assignment: the element lives inside the vector the
          // assignment replaces, so a direct self-assign reads freed storage.
          SemanticType element = currentType->arguments.front();
          currentType = std::move(element);
        } else {
          currentType.reset();
        }
        break;
      case MirProjectionKind::Dereference:
        requireCapability(CppMirEmissionCapabilityKind::Borrow);
        if (currentType && currentType->arguments.size() == 1 &&
            (currentType->kind == SemanticType::Reference ||
             currentType->kind == SemanticType::UniqueOwner ||
             currentType->kind == SemanticType::SharedPointer)) {
          SemanticType pointee = currentType->arguments.front();
          currentType = std::move(pointee);
        } else {
          currentType.reset();
        }
        break;
      case MirProjectionKind::RawIndex:
      case MirProjectionKind::RawDereference:
        requireCapability(CppMirEmissionCapabilityKind::RawMemory);
        if (currentType && currentType->kind == SemanticType::RawPointer &&
            currentType->arguments.size() == 1) {
          SemanticType pointee = currentType->arguments.front();
          currentType = std::move(pointee);
        } else {
          currentType.reset();
        }
        break;
      }
    }
    if (currentType && *currentType != place.type) {
      add(CppMirBodyEmissionIssueKind::InvalidMirProgram, 0, 0,
          "place projection result type disagrees with its concrete root "
          "and projection chain");
    }
  }

  void scanOperand(const MirOperand &operand, MirBlockId block,
                   MirInstructionId instruction) {
    const CppMirEmissionEncoding encoding =
        classifyCppMirOperandKind(operand.kind);
    if (encoding == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidOperandKind, block, instruction,
          "operand kind is outside the exhaustive emitter vocabulary");
      return;
    }
    requireType(operand.type, block, instruction);
    switch (operand.kind) {
    case MirOperandKind::Address:
      requireCapability(CppMirEmissionCapabilityKind::RawMemory, block,
                        instruction);
      break;
    case MirOperandKind::BorrowRead:
    case MirOperandKind::BorrowWrite:
    case MirOperandKind::Loan:
      requireCapability(CppMirEmissionCapabilityKind::Borrow, block,
                        instruction);
      break;
    case MirOperandKind::Value:
    case MirOperandKind::Constant:
    case MirOperandKind::Copy:
    case MirOperandKind::Move:
      break;
    }
  }

  void scanOperation(const MirBody &body, const MirBlock &block,
                     const MirInstruction &instruction) {
    const CppMirEmissionEncoding encoding =
        classifyCppMirOperation(instruction.operation);
    if (encoding == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidOperation, block.id,
          instruction.id,
          "MIR operation is outside the exhaustive emitter vocabulary");
      return;
    }
    if (encoding == CppMirEmissionEncoding::MissingMirAuthority &&
        !isHostedStartupArgumentIndexAdvance(body, instruction)) {
      if (instruction.operation == MirOperation::PackExpansion) {
        add(CppMirBodyEmissionIssueKind::MissingPackExpansionMir, block.id,
            instruction.id,
            "PackExpansion has no concrete ordered element operands or "
            "targets in MIR");
      } else {
        add(CppMirBodyEmissionIssueKind::MissingOrderedCompoundMir, block.id,
            instruction.id,
            "compound operation lacks the complete verified target/operand/"
            "commit schedule required for generic emission");
      }
    }

    switch (instruction.operation) {
    case MirOperation::EnumConstant:
      if (instruction.enumOwner) {
        requireEnum(*instruction.enumOwner, block.id, instruction.id);
      }
      break;
    case MirOperation::Aggregate:
      requireCapability(CppMirEmissionCapabilityKind::Aggregate, block.id,
                        instruction.id);
      if (instruction.info.traits.drop != DropKind::Trivial) {
        add(CppMirBodyEmissionIssueKind::MissingAggregateRollbackMir, block.id,
            instruction.id,
            "cleanup-owning aggregate construction lacks per-element partial "
            "initialization and rollback state");
      }
      break;
    case MirOperation::Index:
      requireCapability(CppMirEmissionCapabilityKind::Bounds, block.id,
                        instruction.id);
      break;
    case MirOperation::AddressOf:
    case MirOperation::PointerAdd:
    case MirOperation::PointerSubtract:
    case MirOperation::PointerDifference:
      requireCapability(CppMirEmissionCapabilityKind::RawMemory, block.id,
                        instruction.id);
      break;
    case MirOperation::ExpectedHasValue:
    case MirOperation::Unexpected:
      requireCapability(CppMirEmissionCapabilityKind::Expected, block.id,
                        instruction.id);
      break;
    case MirOperation::Closure:
      requireCapability(CppMirEmissionCapabilityKind::Closure, block.id,
                        instruction.id);
      if (instruction.lambdaTarget) {
        requireBody(
            {.kind = MirBodyKind::Lambda, .owner = *instruction.lambdaTarget},
            block.id, instruction.id);
      }
      break;
    case MirOperation::PackFold:
      requireCapability(CppMirEmissionCapabilityKind::PackFold, block.id,
                        instruction.id);
      for (const MirPackFoldElement &element : instruction.packFoldElements) {
        requireType(element.elementType, block.id, instruction.id);
        for (const SemanticType &type : element.parameterTypes) {
          requireType(type, block.id, instruction.id);
        }
        requireBody(
            {.kind = MirBodyKind::Function, .owner = element.functionTarget},
            block.id, instruction.id);
      }
      break;
    case MirOperation::PayloadConstruct:
    case MirOperation::PayloadExtract: {
      requireCapability(CppMirEmissionCapabilityKind::Payload, block.id,
                        instruction.id);
      const CppMirEnumRepresentation *enumeration =
          instruction.enumOwner
              ? requireEnum(*instruction.enumOwner, block.id, instruction.id)
              : nullptr;
      if (enumeration != nullptr && instruction.enumVariant) {
        const auto variant =
            std::find_if(enumeration->payloadVariants.begin(),
                         enumeration->payloadVariants.end(),
                         [&](const CppMirPayloadVariantRepresentation &row) {
                           return row.index == *instruction.enumVariant;
                         });
        if (variant == enumeration->payloadVariants.end()) {
          add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block.id,
              instruction.id,
              "payload operation names a variant absent from the copied map");
        } else if (instruction.operation == MirOperation::PayloadConstruct) {
          std::vector<SemanticType> operands;
          operands.reserve(instruction.operands.size());
          for (const MirOperand &operand : instruction.operands) {
            operands.push_back(operand.type);
          }
          if (variant->fieldTypes != operands) {
            add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block.id,
                instruction.id,
                "payload constructor fields disagree with the copied enum "
                "variant");
          }
        } else if (!instruction.payloadIndex ||
                   *instruction.payloadIndex >= variant->fieldTypes.size() ||
                   variant->fieldTypes[*instruction.payloadIndex] !=
                       instruction.info.type) {
          add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block.id,
              instruction.id,
              "payload extraction index or type disagrees with the copied "
              "enum variant");
        }
      }
      break;
    }
    case MirOperation::PackExpansion:
    case MirOperation::Comma:
    case MirOperation::AddAssign:
    case MirOperation::SubtractAssign:
    case MirOperation::MultiplyAssign:
    case MirOperation::DivideAssign:
    case MirOperation::RemainderAssign:
    case MirOperation::BitwiseAndAssign:
    case MirOperation::BitwiseOrAssign:
    case MirOperation::BitwiseXorAssign:
    case MirOperation::ShiftLeftAssign:
    case MirOperation::ShiftRightAssign:
    case MirOperation::PreIncrement:
    case MirOperation::PreDecrement:
    case MirOperation::PostIncrement:
    case MirOperation::PostDecrement:
      break;
    case MirOperation::None:
    case MirOperation::Literal:
    case MirOperation::Identity:
    case MirOperation::Convert:
    case MirOperation::Add:
    case MirOperation::Subtract:
    case MirOperation::Multiply:
    case MirOperation::Divide:
    case MirOperation::Remainder:
    case MirOperation::BitwiseAnd:
    case MirOperation::BitwiseOr:
    case MirOperation::BitwiseXor:
    case MirOperation::ShiftLeft:
    case MirOperation::ShiftRight:
    case MirOperation::Equal:
    case MirOperation::NotEqual:
    case MirOperation::Less:
    case MirOperation::LessEqual:
    case MirOperation::Greater:
    case MirOperation::GreaterEqual:
    case MirOperation::Positive:
    case MirOperation::Negate:
    case MirOperation::LogicalNot:
    case MirOperation::BitwiseNot:
    case MirOperation::Assign:
      break;
    case MirOperation::Count:
      add(CppMirBodyEmissionIssueKind::InvalidOperation, block.id,
          instruction.id, "MirOperation::Count is not executable");
      break;
    }

    (void)body;
  }

  void scanInstruction(const MirBody &body, const MirBlock &block,
                       const MirInstruction &instruction) {
    const CppMirEmissionEncoding kind =
        classifyCppMirInstructionKind(instruction.kind);
    if (kind == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidInstructionKind, block.id,
          instruction.id,
          "instruction kind is outside the exhaustive emitter vocabulary");
      return;
    }
    if (kind == CppMirEmissionEncoding::MissingMirAuthority &&
        !isHostedStartupArgumentIndexAdvance(body, instruction)) {
      add(CppMirBodyEmissionIssueKind::MissingOrderedCompoundMir, block.id,
          instruction.id,
          "Modify lacks the verified read/check/convert/commit schedule");
    }

    switch (instruction.kind) {
    case MirInstructionKind::Drop:
    case MirInstructionKind::EndBorrow:
    case MirInstructionKind::Lifecycle:
      if (instruction.info.type != SemanticType::Unknown) {
        requireType(instruction.info.type, block.id, instruction.id);
      }
      break;
    case MirInstructionKind::Compute:
    case MirInstructionKind::Load:
    case MirInstructionKind::Initialize:
    case MirInstructionKind::Assign:
    case MirInstructionKind::Modify:
    case MirInstructionKind::Move:
    case MirInstructionKind::Borrow:
    case MirInstructionKind::CallInput:
    case MirInstructionKind::Call:
    case MirInstructionKind::Construct:
      requireType(instruction.info.type, block.id, instruction.id);
      break;
    case MirInstructionKind::CallBody:
      requireType(instruction.info.type, block.id, instruction.id);
      break;
    case MirInstructionKind::Count:
      break;
    }
    for (const SemanticType &type : instruction.parameterTypes) {
      requireType(type, block.id, instruction.id);
    }
    for (const SemanticType &type : instruction.closureCaptureTypes) {
      requireType(type, block.id, instruction.id);
    }
    if (instruction.receiver) {
      scanOperand(*instruction.receiver, block.id, instruction.id);
    }
    for (const MirOperand &operand : instruction.operands) {
      scanOperand(operand, block.id, instruction.id);
    }
    scanOperation(body, block, instruction);

    if (instruction.functionTarget) {
      requireBody(
          {.kind = MirBodyKind::Function, .owner = *instruction.functionTarget},
          block.id, instruction.id);
      const MirFunctionInstance *target =
          program.findFunctionInstance(*instruction.functionTarget);
      if (target != nullptr && target->linkage == LanguageLinkage::C) {
        requireCapability(CppMirEmissionCapabilityKind::NativeInterop, block.id,
                          instruction.id);
      }
    }
    if (instruction.constructorTarget) {
      requireBody({.kind = MirBodyKind::Constructor,
                   .owner = *instruction.constructorTarget},
                  block.id, instruction.id);
    }
    if (instruction.lambdaTarget) {
      requireBody(
          {.kind = MirBodyKind::Lambda, .owner = *instruction.lambdaTarget},
          block.id, instruction.id);
    }
    if (instruction.bodyTarget) {
      requireBody(*instruction.bodyTarget, block.id, instruction.id);
    }
    if (instruction.dispatch == CallDispatch::Virtual) {
      requireCapability(CppMirEmissionCapabilityKind::VirtualDispatch, block.id,
                        instruction.id);
    }
    if (instruction.callableInvocation || instruction.callableBoundary ||
        !instruction.callableArguments.empty()) {
      requireCapability(CppMirEmissionCapabilityKind::CallableDispatch,
                        block.id, instruction.id);
    }
    if (instruction.intrinsic != IntrinsicKind::None) {
      if (ordinal(instruction.intrinsic) >= ordinal(IntrinsicKind::Count)) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, block.id,
            instruction.id, "call has an invalid intrinsic identity");
      }
      requireCapability(CppMirEmissionCapabilityKind::Intrinsic, block.id,
                        instruction.id);
    }
    if (instruction.synchronization.kind !=
        SynchronizationOperationKind::None) {
      if (ordinal(instruction.synchronization.kind) >=
          ordinal(SynchronizationOperationKind::Count)) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, block.id,
            instruction.id, "call has an invalid synchronization identity");
      }
      requireCapability(CppMirEmissionCapabilityKind::Synchronization, block.id,
                        instruction.id);
    }
    if (instruction.unsafeOperation != UnsafeOperationKind::None ||
        instruction.rawMemoryAccess) {
      requireCapability(CppMirEmissionCapabilityKind::RawMemory, block.id,
                        instruction.id);
    }
    if (!instruction.definedFailure.empty()) {
      requireCapability(CppMirEmissionCapabilityKind::DefinedFailure, block.id,
                        instruction.id);
      if (result.body.kind == MirBodyKind::HostedStartup) {
        add(CppMirBodyEmissionIssueKind::MissingFailureCleanupMir, block.id,
            instruction.id,
            instruction.definedFailure.propagation ==
                    FailurePropagationKind::BodyCall
                ? "compiler-generated body-call propagation lacks the "
                  "Stage-E hosted cleanup and terminal containment path"
                : "compiler-generated hosted failure propagation lacks the "
                  "Stage-E cleanup and terminal containment path");
      } else if (!instructionHasInvoke(block, instruction)) {
        add(CppMirBodyEmissionIssueKind::MissingCheckedFailureControlFlow,
            block.id, instruction.id,
            "checked operation has no exact Invoke/record/cleanup successor");
      }
      if (result.body.kind == MirBodyKind::Constructor) {
        add(CppMirBodyEmissionIssueKind::MissingPartialConstructionRollbackMir,
            block.id, instruction.id,
            "failure-capable construction has no general subobject rollback");
      }
      if (result.body.kind == MirBodyKind::Destructor) {
        add(CppMirBodyEmissionIssueKind::MissingFailureCleanupMir, block.id,
            instruction.id,
            "failure-capable cleanup has no double-failure envelope path");
      }
    }

    if ((instruction.kind == MirInstructionKind::Call ||
         instruction.kind == MirInstructionKind::Construct) &&
        instruction.intrinsic == IntrinsicKind::None &&
        !hasCompleteCallInputSchedule(body, instruction)) {
      add(CppMirBodyEmissionIssueKind::MissingCallInputScheduleMir, block.id,
          instruction.id,
          "call or construction operands do not all come from exact ordered "
          "CallInput stages");
    }
    if (instruction.kind == MirInstructionKind::Construct &&
        instruction.constructorKind != ConstructorKind::Ordinary) {
      add(CppMirBodyEmissionIssueKind::MissingConstructionScheduleMir, block.id,
          instruction.id,
          "generated copy/move construction lacks the complete generic "
          "destination and cleanup schedule");
    }
    if (instruction.kind == MirInstructionKind::Borrow ||
        instruction.kind == MirInstructionKind::EndBorrow) {
      requireCapability(CppMirEmissionCapabilityKind::Borrow, block.id,
                        instruction.id);
    }
    if (instruction.kind == MirInstructionKind::Drop ||
        !instruction.lifecycle.empty()) {
      requireCapability(CppMirEmissionCapabilityKind::LifetimeStorage, block.id,
                        instruction.id);
    }
  }

  void scanTerminator(const MirBlock &block) {
    const MirTerminator &terminator = block.terminator;
    const CppMirEmissionEncoding encoding =
        classifyCppMirTerminatorKind(terminator.kind);
    if (encoding == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidTerminatorKind, block.id, 0,
          "terminator kind is not executable");
      return;
    }
    if (terminator.value) {
      scanOperand(*terminator.value, block.id, 0);
    }
    for (const MirSwitchTarget &target : terminator.switchTargets) {
      if (!target.value) {
        continue;
      }
      requireType(target.value->type, block.id, 0);
      if (target.value->enumOwner != 0) {
        requireEnum(target.value->enumOwner, block.id, 0);
      }
    }
    if (terminator.kind == MirTerminatorKind::Invoke ||
        terminator.kind == MirTerminatorKind::PropagateFailure ||
        terminator.kind == MirTerminatorKind::ContainFailure ||
        terminator.kind == MirTerminatorKind::TerminateCleanupFailure) {
      requireCapability(CppMirEmissionCapabilityKind::DefinedFailure, block.id,
                        0);
    }
    if (terminator.kind == MirTerminatorKind::Exit &&
        !isInitializerBody(result.body.kind)) {
      add(CppMirBodyEmissionIssueKind::InvalidTerminatorKind, block.id, 0,
          "Exit is reserved for module/field initializer bodies");
    }
  }

  const MirProgram &program;
  const CppMirBodyEmissionMap &representations;
  CppMirBodyEmissionAnalysis result;
};

} // namespace

CppMirBodyEmissionMap::CppMirBodyEmissionMap(CppMirBodyEmissionMapRows rows)
    : types_(std::move(rows.types)), bodies_(std::move(rows.bodies)),
      symbols_(std::move(rows.symbols)), enums_(std::move(rows.enums)),
      capabilities_(std::move(rows.capabilities)) {}

CppMirEmissionEncoding classifyCppMirInstructionKind(MirInstructionKind kind) {
  switch (kind) {
  case MirInstructionKind::Compute:
  case MirInstructionKind::Load:
  case MirInstructionKind::Initialize:
  case MirInstructionKind::Assign:
  case MirInstructionKind::Move:
  case MirInstructionKind::Lifecycle:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirInstructionKind::Borrow:
  case MirInstructionKind::CallInput:
  case MirInstructionKind::Call:
  case MirInstructionKind::Construct:
  case MirInstructionKind::Drop:
  case MirInstructionKind::EndBorrow:
  case MirInstructionKind::CallBody:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  case MirInstructionKind::Modify:
    return CppMirEmissionEncoding::MissingMirAuthority;
  case MirInstructionKind::Count:
    return CppMirEmissionEncoding::Invalid;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirOperation(MirOperation operation) {
  switch (operation) {
  case MirOperation::None:
  case MirOperation::Literal:
  case MirOperation::Identity:
  case MirOperation::Convert:
  case MirOperation::Add:
  case MirOperation::Subtract:
  case MirOperation::Multiply:
  case MirOperation::Divide:
  case MirOperation::Remainder:
  case MirOperation::BitwiseAnd:
  case MirOperation::BitwiseOr:
  case MirOperation::BitwiseXor:
  case MirOperation::ShiftLeft:
  case MirOperation::ShiftRight:
  case MirOperation::Equal:
  case MirOperation::NotEqual:
  case MirOperation::Less:
  case MirOperation::LessEqual:
  case MirOperation::Greater:
  case MirOperation::GreaterEqual:
  case MirOperation::Positive:
  case MirOperation::Negate:
  case MirOperation::LogicalNot:
  case MirOperation::BitwiseNot:
  case MirOperation::Assign:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirOperation::EnumConstant:
  case MirOperation::Aggregate:
  case MirOperation::Index:
  case MirOperation::ExpectedHasValue:
  case MirOperation::Closure:
  case MirOperation::PackFold:
  case MirOperation::PayloadConstruct:
  case MirOperation::PayloadExtract:
  case MirOperation::Unexpected:
  case MirOperation::AddressOf:
  case MirOperation::PointerAdd:
  case MirOperation::PointerSubtract:
  case MirOperation::PointerDifference:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  case MirOperation::PackExpansion:
  case MirOperation::Comma:
  case MirOperation::AddAssign:
  case MirOperation::SubtractAssign:
  case MirOperation::MultiplyAssign:
  case MirOperation::DivideAssign:
  case MirOperation::RemainderAssign:
  case MirOperation::BitwiseAndAssign:
  case MirOperation::BitwiseOrAssign:
  case MirOperation::BitwiseXorAssign:
  case MirOperation::ShiftLeftAssign:
  case MirOperation::ShiftRightAssign:
  case MirOperation::PreIncrement:
  case MirOperation::PreDecrement:
  case MirOperation::PostIncrement:
  case MirOperation::PostDecrement:
    return CppMirEmissionEncoding::MissingMirAuthority;
  case MirOperation::Count:
    return CppMirEmissionEncoding::Invalid;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirOperandKind(MirOperandKind kind) {
  switch (kind) {
  case MirOperandKind::Value:
  case MirOperandKind::Constant:
  case MirOperandKind::Copy:
  case MirOperandKind::Move:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirOperandKind::Address:
  case MirOperandKind::BorrowRead:
  case MirOperandKind::BorrowWrite:
  case MirOperandKind::Loan:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirPlaceRootKind(MirPlaceRootKind kind) {
  switch (kind) {
  case MirPlaceRootKind::Binding:
  case MirPlaceRootKind::Temporary:
  case MirPlaceRootKind::Value:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirPlaceRootKind::Symbol:
  case MirPlaceRootKind::This:
  case MirPlaceRootKind::Loan:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirProjectionKind(MirProjectionKind kind) {
  switch (kind) {
  case MirProjectionKind::Field:
  case MirProjectionKind::Index:
  case MirProjectionKind::Dereference:
  case MirProjectionKind::RawIndex:
  case MirProjectionKind::RawDereference:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirTerminatorKind(MirTerminatorKind kind) {
  switch (kind) {
  case MirTerminatorKind::Goto:
  case MirTerminatorKind::Branch:
  case MirTerminatorKind::Switch:
  case MirTerminatorKind::Return:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirTerminatorKind::Invoke:
  case MirTerminatorKind::PropagateFailure:
  case MirTerminatorKind::ContainFailure:
  case MirTerminatorKind::TerminateCleanupFailure:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  case MirTerminatorKind::None:
    return CppMirEmissionEncoding::Invalid;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirBodyKind(MirBodyKind kind) {
  switch (kind) {
  case MirBodyKind::Module:
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
  case MirBodyKind::Function:
  case MirBodyKind::Constructor:
  case MirBodyKind::Destructor:
  case MirBodyKind::Lambda:
  case MirBodyKind::HostedStartup:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  }
  return CppMirEmissionEncoding::Invalid;
}

namespace {

// General per-instance text step (ADR 016 phase 5). Ported verbatim from the
// transitional emitter's scalar-cfg/scalar-direct-call body emission so
// production text is byte-identical across the delegation; every naming or
// type consultation is replaced by a copied representation row. Constructs
// outside this vocabulary after a Ready analysis are emission drift and
// throw, exactly as the transitional emitter throws.
class ScalarBodyTextEmitter {
public:
  ScalarBodyTextEmitter(const MirProgram &program,
                        const CppMirBodyEmissionMap &representations,
                        std::size_t indentation)
      : program(program), representations(representations),
        indentation(indentation) {}

  [[nodiscard]] std::string emit(const MirFunctionInstance &function,
                                 std::string_view familyLabel,
                                 CppMirBodyTextForm form) {
    output.str("");
    output << "{\n";
    ++indentation;
    writeIndent();
    output << "// GTI verified-MIR body: " << familyLabel
           << " function-instance " << function.id << "\n";
    if (form == CppMirBodyTextForm::ScalarStraightLine) {
      emitStraightLine(function);
      --indentation;
      writeIndent();
      output << "}\n";
      return output.str();
    }
    for (const MirPlace &place : function.body.places) {
      // Receiver-place handling is derived from MIR, not selected by the
      // caller: a This-rooted place is the projection carrier (skipped) or
      // one projected read-only field (bound by reference to the live
      // member). No admitted body declares a receiver as an ordinary local.
      if (place.root == MirPlaceRootKind::This) {
        // The bare receiver place is only the projection carrier and is never
        // referenced. A field place binds by reference so every load reads
        // the live member; receivers here are read-only, so no write occurs.
        if (place.projections.empty()) {
          continue;
        }
        writeIndent();
        output << "const auto &__gti_mir_p_" << place.id << " = (*this)."
               << fieldSpelling(function, place.projections.front().field)
               << ";\n";
        continue;
      }
      writeIndent();
      output << typeSpelling(place.type) << " __gti_mir_p_" << place.id;
      if (const std::optional<std::size_t> parameter =
              parameterIndex(place, function)) {
        output << " = __gti_mir_arg_" << *parameter;
      } else {
        output << "{}";
      }
      output << ";\n";
    }
    for (const MirValue &value : function.body.values) {
      writeIndent();
      output << typeSpelling(value.info.type) << " __gti_mir_v_" << value.id
             << "{};\n";
    }
    writeIndent();
    output << "std::size_t __gti_mir_bb = " << function.body.entry << ";\n";
    writeIndent();
    output << "for (;;) {\n";
    ++indentation;
    writeIndent();
    output << "switch (__gti_mir_bb) {\n";
    ++indentation;
    for (const MirBlock &block : function.body.blocks) {
      writeIndent();
      output << "case " << block.id << ": {\n";
      ++indentation;
      for (const MirInstruction &instruction : block.instructions) {
        emitInstruction(instruction);
      }
      emitTerminator(block.terminator);
      --indentation;
      writeIndent();
      output << "}\n";
    }
    writeIndent();
    output << "default:\n";
    ++indentation;
    writeIndent();
    output << "std::abort();\n";
    --indentation;
    --indentation;
    writeIndent();
    output << "}\n";
    --indentation;
    writeIndent();
    output << "}\n";
    --indentation;
    writeIndent();
    output << "}\n";
    return output.str();
  }

private:
  // Ported verbatim from the transitional emitter's scalar-leaf body
  // emission: single-block SSA text with const values, direct parameter
  // reads, and one trailing return.
  void emitStraightLine(const MirFunctionInstance &function) {
    for (const MirInstruction &instruction :
         function.body.blocks.front().instructions) {
      writeIndent();
      if (instruction.kind == MirInstructionKind::Lifecycle) {
        output << "// GTI MIR full-expression boundary "
               << instruction.fullExpressionEnd << "\n";
        continue;
      }
      output << "const " << typeSpelling(instruction.info.type)
             << " __gti_mir_v_" << *instruction.result << " = ";
      if (instruction.kind == MirInstructionKind::Load) {
        const MirPlace *place =
            function.body.findPlace(instruction.operands.front().place);
        if (place == nullptr) {
          throw std::logic_error(
              "verified MIR scalar-leaf load lost its parameter place");
        }
        const auto parameter =
            std::find(function.parameterBindings.begin(),
                      function.parameterBindings.end(), place->binding);
        output << "__gti_mir_arg_"
               << std::distance(function.parameterBindings.begin(), parameter);
      } else if (instruction.operation == MirOperation::Literal) {
        emitStraightLineLiteral(*instruction.literal, instruction.info.type);
      } else {
        output << "__gti_mir_v_" << instruction.operands.front().value;
      }
      output << ";\n";
    }
    writeIndent();
    output << "return";
    if (function.body.blocks.front().terminator.value) {
      output << " __gti_mir_v_"
             << function.body.blocks.front().terminator.value->value;
    }
    output << ";\n";
  }

  void emitStraightLineLiteral(const Literal &literal,
                               const SemanticType &type) {
    if (const auto *integer = std::get_if<std::uint64_t>(&literal)) {
      output << "static_cast<" << typeSpelling(type) << ">(";
      emitIntegerLiteral(*integer);
      output << ')';
      return;
    }
    throw std::logic_error(
        "verified MIR scalar-leaf literal has an unsupported representation");
  }

  void writeIndent() {
    for (std::size_t index = 0; index < indentation; ++index) {
      output << "  ";
    }
  }

  [[nodiscard]] const std::string &typeSpelling(const SemanticType &type) {
    const auto found = std::find_if(
        representations.types().begin(), representations.types().end(),
        [&](const CppMirTypeRepresentation &row) { return row.type == type; });
    if (found == representations.types().end() || found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost a copied type row");
    }
    return found->spelling;
  }

  [[nodiscard]] const std::string &
  fieldSpelling(const MirFunctionInstance &function, SymbolId field) {
    if (!function.owner) {
      throw std::logic_error(
          "general MIR body emission lost the receiver class instance");
    }
    const auto found = std::find_if(
        representations.symbols().begin(), representations.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == CppMirSymbolRepresentationKind::Field &&
                 row.owner == *function.owner && row.symbol == field &&
                 row.ordinal == 0;
        });
    if (found == representations.symbols().end() || found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost an exact field symbol row");
    }
    return found->spelling;
  }

  [[nodiscard]] const std::string &bodySpelling(HirFunctionInstanceId target) {
    const MirBodyAddress address{.kind = MirBodyKind::Function,
                                 .owner = target};
    const auto found = std::find_if(
        representations.bodies().begin(), representations.bodies().end(),
        [&](const CppMirBodyNameRepresentation &row) {
          return row.address == address;
        });
    if (found == representations.bodies().end() || found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost an exact call-target name row");
    }
    return found->spelling;
  }

  [[nodiscard]] static std::optional<std::size_t>
  parameterIndex(const MirPlace &place, const MirFunctionInstance &function) {
    const auto parameter =
        std::find(function.parameterBindings.begin(),
                  function.parameterBindings.end(), place.binding);
    if (parameter == function.parameterBindings.end()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(
        std::distance(function.parameterBindings.begin(), parameter));
  }

  [[nodiscard]] static bool
  isSyntheticLogicalConstant(const MirOperand &operand) {
    return operand.kind == MirOperandKind::Constant && operand.value == 0 &&
           operand.place == 0 && operand.loan == 0 && operand.literal &&
           operand.type == SemanticType::Bool &&
           std::holds_alternative<bool>(*operand.literal);
  }

  void emitIntegerLiteral(std::uint64_t value) {
    output << value;
    if (value >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      output << "ULL";
    }
  }

  void emitLiteral(const Literal &literal, const SemanticType &type) {
    if (const auto *integer = std::get_if<std::uint64_t>(&literal)) {
      output << "static_cast<" << typeSpelling(type) << ">(";
      emitIntegerLiteral(*integer);
      output << ')';
      return;
    }
    if (const auto *character = std::get_if<CharacterLiteral>(&literal)) {
      output << "std::uint8_t{" << static_cast<unsigned int>(character->value)
             << '}';
      return;
    }
    if (const auto *boolean = std::get_if<bool>(&literal)) {
      output << (*boolean ? "true" : "false");
      return;
    }
    throw std::logic_error(
        "verified MIR scalar-CFG literal has an unsupported representation");
  }

  void emitOperand(const MirOperand &operand,
                   bool allowSyntheticLogicalConstant = false) {
    if (operand.kind == MirOperandKind::Value) {
      output << "__gti_mir_v_" << operand.value;
      return;
    }
    if (allowSyntheticLogicalConstant && isSyntheticLogicalConstant(operand)) {
      emitLiteral(*operand.literal, operand.type);
      return;
    }
    throw std::logic_error(
        "verified MIR scalar-CFG operand is not a proven value");
  }

  void emitCompute(const MirInstruction &instruction) {
    output << "__gti_mir_v_" << *instruction.result << " = ";
    if (instruction.operation == MirOperation::Literal) {
      emitLiteral(*instruction.literal, instruction.info.type);
      output << ";\n";
      return;
    }
    if (instruction.operation == MirOperation::Identity) {
      emitOperand(instruction.operands.front());
      output << ";\n";
      return;
    }
    if (instruction.operation == MirOperation::LogicalNot) {
      output << '!';
      emitOperand(instruction.operands.front());
      output << ";\n";
      return;
    }
    if (instruction.operation == MirOperation::Positive ||
        instruction.operation == MirOperation::BitwiseNot) {
      output << "static_cast<" << typeSpelling(instruction.info.type) << ">("
             << (instruction.operation == MirOperation::Positive ? '+' : '~');
      emitOperand(instruction.operands.front());
      output << ");\n";
      return;
    }
    const auto spelling = [&]() -> std::string_view {
      switch (instruction.operation) {
      case MirOperation::BitwiseAnd:
        return "&";
      case MirOperation::BitwiseOr:
        return "|";
      case MirOperation::BitwiseXor:
        return "^";
      case MirOperation::Equal:
        return "==";
      case MirOperation::NotEqual:
        return "!=";
      case MirOperation::Less:
        return "<";
      case MirOperation::LessEqual:
        return "<=";
      case MirOperation::Greater:
        return ">";
      case MirOperation::GreaterEqual:
        return ">=";
      default:
        throw std::logic_error(
            "verified MIR scalar-CFG compute operation is unsupported");
      }
    }();
    const bool castResult = instruction.operation == MirOperation::BitwiseAnd ||
                            instruction.operation == MirOperation::BitwiseOr ||
                            instruction.operation == MirOperation::BitwiseXor;
    if (castResult) {
      output << "static_cast<" << typeSpelling(instruction.info.type) << ">(";
    }
    emitOperand(instruction.operands[0]);
    output << ' ' << spelling << ' ';
    emitOperand(instruction.operands[1]);
    if (castResult) {
      output << ')';
    }
    output << ";\n";
  }

  void emitPlainInstruction(const MirInstruction &instruction) {
    writeIndent();
    if (instruction.kind == MirInstructionKind::Lifecycle) {
      output << "// GTI MIR full-expression boundary "
             << instruction.fullExpressionEnd << "\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Compute) {
      emitCompute(instruction);
      return;
    }
    if (instruction.kind == MirInstructionKind::Load) {
      output << "__gti_mir_v_" << *instruction.result << " = __gti_mir_p_"
             << instruction.operands.front().place << ";\n";
      return;
    }
    output << "__gti_mir_p_" << *instruction.destination << " = ";
    emitOperand(instruction.operands.front(),
                instruction.kind == MirInstructionKind::Initialize);
    output << ";\n";
    if (instruction.kind == MirInstructionKind::Assign) {
      writeIndent();
      output << "__gti_mir_v_" << *instruction.result << " = __gti_mir_p_"
             << *instruction.destination << ";\n";
    }
  }

  void emitInstruction(const MirInstruction &instruction) {
    if (instruction.kind != MirInstructionKind::CallInput &&
        instruction.kind != MirInstructionKind::Call) {
      emitPlainInstruction(instruction);
      return;
    }
    writeIndent();
    if (instruction.kind == MirInstructionKind::CallInput) {
      output << "__gti_mir_v_" << *instruction.result << " = ";
      emitOperand(instruction.operands.front());
      output << ";\n";
      return;
    }
    if (!instruction.functionTarget) {
      throw std::logic_error(
          "verified MIR direct call lost its exact target declaration");
    }
    if (instruction.result) {
      output << "__gti_mir_v_" << *instruction.result << " = ";
    }
    output << bodySpelling(*instruction.functionTarget);
    output << '(';
    for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      emitOperand(instruction.operands[index]);
    }
    output << ");\n";
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

  void emitTerminator(const MirTerminator &terminator) {
    switch (terminator.kind) {
    case MirTerminatorKind::Goto:
      writeIndent();
      output << "__gti_mir_bb = " << terminator.target << ";\n";
      writeIndent();
      output << "continue;\n";
      return;
    case MirTerminatorKind::Branch:
      writeIndent();
      output << "__gti_mir_bb = ";
      emitOperand(*terminator.value);
      output << " ? " << terminator.target << " : " << terminator.elseTarget
             << ";\n";
      writeIndent();
      output << "continue;\n";
      return;
    case MirTerminatorKind::Switch:
      writeIndent();
      output << "switch (";
      emitOperand(*terminator.value);
      output << ") {\n";
      ++indentation;
      for (const MirSwitchTarget &target : terminator.switchTargets) {
        writeIndent();
        output << "case static_cast<" << typeSpelling(target.value->type)
               << ">(";
        emitSwitchInteger(target.value->value, target.value->type);
        output << "):\n";
        ++indentation;
        writeIndent();
        output << "__gti_mir_bb = " << target.target << ";\n";
        writeIndent();
        output << "break;\n";
        --indentation;
      }
      writeIndent();
      output << "default:\n";
      ++indentation;
      writeIndent();
      output << "__gti_mir_bb = " << terminator.target << ";\n";
      writeIndent();
      output << "break;\n";
      --indentation;
      --indentation;
      writeIndent();
      output << "}\n";
      writeIndent();
      output << "continue;\n";
      return;
    case MirTerminatorKind::Return:
      writeIndent();
      output << "return";
      if (terminator.value) {
        output << ' ';
        emitOperand(*terminator.value);
      }
      output << ";\n";
      return;
    case MirTerminatorKind::Unreachable:
      writeIndent();
      output << "std::abort();\n";
      return;
    default:
      break;
    }
    throw std::logic_error("verified MIR scalar-CFG terminator is unsupported");
  }

  const MirProgram &program;
  const CppMirBodyEmissionMap &representations;
  std::size_t indentation;
  std::ostringstream output;
};

} // namespace

std::optional<CppMirTypeRepresentationKind>
cppMirExpectedTypeRepresentation(const SemanticType &type) {
  return expectedTypeRepresentation(type);
}

bool CppMirBodyEmitter::supportsBodyText(MirBodyAddress address) const {
  if (address.kind != MirBodyKind::Function) {
    return false;
  }
  const MirFunctionInstance *function =
      program_.findFunctionInstance(address.owner);
  if (function == nullptr) {
    return false;
  }
  const MirBody &body = function->body;
  if (body.blocks.empty() || body.entry == 0 ||
      body.entry > body.blocks.size()) {
    return false;
  }

  const auto typeRow = [&](const SemanticType &type) {
    const auto found = std::find_if(
        representations_.types().begin(), representations_.types().end(),
        [&](const CppMirTypeRepresentation &row) { return row.type == type; });
    return found != representations_.types().end() && !found->spelling.empty();
  };
  const auto fieldRow = [&](SymbolId field) {
    if (!function->owner) {
      return false;
    }
    const auto found = std::find_if(
        representations_.symbols().begin(), representations_.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == CppMirSymbolRepresentationKind::Field &&
                 row.owner == *function->owner && row.symbol == field &&
                 row.ordinal == 0;
        });
    return found != representations_.symbols().end() &&
           !found->spelling.empty();
  };
  const auto bodyRow = [&](HirFunctionInstanceId target) {
    const MirBodyAddress callee{.kind = MirBodyKind::Function, .owner = target};
    const auto found = std::find_if(
        representations_.bodies().begin(), representations_.bodies().end(),
        [&](const CppMirBodyNameRepresentation &row) {
          return row.address == callee;
        });
    return found != representations_.bodies().end() && !found->spelling.empty();
  };
  const auto valueOperand = [](const MirOperand &operand) {
    return operand.kind == MirOperandKind::Value;
  };
  const auto syntheticBool = [](const MirOperand &operand) {
    return operand.kind == MirOperandKind::Constant && operand.value == 0 &&
           operand.place == 0 && operand.loan == 0 && operand.literal &&
           operand.type == SemanticType::Bool &&
           std::holds_alternative<bool>(*operand.literal);
  };
  const auto literalSupported = [&](const std::optional<Literal> &literal,
                                    const SemanticType &type) {
    if (!literal) {
      return false;
    }
    if (std::holds_alternative<std::uint64_t>(*literal)) {
      return typeRow(type);
    }
    return std::holds_alternative<CharacterLiteral>(*literal) ||
           std::holds_alternative<bool>(*literal);
  };

  for (const MirPlace &place : body.places) {
    if (place.root == MirPlaceRootKind::This) {
      if (place.projections.empty()) {
        continue;
      }
      if (place.projections.size() != 1 ||
          place.projections.front().kind != MirProjectionKind::Field ||
          !fieldRow(place.projections.front().field)) {
        return false;
      }
      continue;
    }
    if (!place.projections.empty() || !typeRow(place.type)) {
      return false;
    }
  }
  for (const MirValue &value : body.values) {
    if (!typeRow(value.info.type)) {
      return false;
    }
  }

  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      switch (instruction.kind) {
      case MirInstructionKind::Lifecycle:
        continue;
      case MirInstructionKind::Compute: {
        if (!instruction.result) {
          return false;
        }
        switch (instruction.operation) {
        case MirOperation::Literal:
          if (!literalSupported(instruction.literal, instruction.info.type)) {
            return false;
          }
          continue;
        case MirOperation::Identity:
        case MirOperation::LogicalNot:
          if (instruction.operands.size() != 1 ||
              !valueOperand(instruction.operands.front())) {
            return false;
          }
          continue;
        case MirOperation::Positive:
        case MirOperation::BitwiseNot:
          if (instruction.operands.size() != 1 ||
              !valueOperand(instruction.operands.front()) ||
              !typeRow(instruction.info.type)) {
            return false;
          }
          continue;
        case MirOperation::BitwiseAnd:
        case MirOperation::BitwiseOr:
        case MirOperation::BitwiseXor:
          if (instruction.operands.size() != 2 ||
              !valueOperand(instruction.operands[0]) ||
              !valueOperand(instruction.operands[1]) ||
              !typeRow(instruction.info.type)) {
            return false;
          }
          continue;
        case MirOperation::Equal:
        case MirOperation::NotEqual:
        case MirOperation::Less:
        case MirOperation::LessEqual:
        case MirOperation::Greater:
        case MirOperation::GreaterEqual:
          if (instruction.operands.size() != 2 ||
              !valueOperand(instruction.operands[0]) ||
              !valueOperand(instruction.operands[1])) {
            return false;
          }
          continue;
        default:
          return false;
        }
      }
      case MirInstructionKind::Load:
        if (!instruction.result || instruction.operands.size() != 1 ||
            instruction.operands.front().place == 0) {
          return false;
        }
        continue;
      case MirInstructionKind::Initialize:
        if (!instruction.destination || instruction.operands.size() != 1 ||
            (!valueOperand(instruction.operands.front()) &&
             !syntheticBool(instruction.operands.front()))) {
          return false;
        }
        continue;
      case MirInstructionKind::Assign:
        if (!instruction.destination || !instruction.result ||
            instruction.operands.size() != 1 ||
            !valueOperand(instruction.operands.front())) {
          return false;
        }
        continue;
      case MirInstructionKind::CallInput:
        if (!instruction.result || instruction.operands.size() != 1 ||
            !valueOperand(instruction.operands.front())) {
          return false;
        }
        continue;
      case MirInstructionKind::Call:
        if (!instruction.functionTarget ||
            !bodyRow(*instruction.functionTarget) ||
            !std::all_of(instruction.operands.begin(),
                         instruction.operands.end(), valueOperand)) {
          return false;
        }
        continue;
      default:
        return false;
      }
    }
    switch (block.terminator.kind) {
    case MirTerminatorKind::Goto:
    case MirTerminatorKind::Unreachable:
      continue;
    case MirTerminatorKind::Branch:
      if (!block.terminator.value || !valueOperand(*block.terminator.value)) {
        return false;
      }
      continue;
    case MirTerminatorKind::Switch: {
      if (!block.terminator.value || !valueOperand(*block.terminator.value)) {
        return false;
      }
      bool targets = true;
      for (const MirSwitchTarget &target : block.terminator.switchTargets) {
        targets = targets && target.value && typeRow(target.value->type);
      }
      if (!targets) {
        return false;
      }
      continue;
    }
    case MirTerminatorKind::Return:
      if (block.terminator.value && !valueOperand(*block.terminator.value)) {
        return false;
      }
      continue;
    default:
      return false;
    }
  }
  return true;
}

CppMirBodyEmissionText CppMirBodyEmitter::emitBodyText(
    MirBodyAddress address, std::string_view familyLabel,
    CppMirBodyTextForm form, std::size_t indentation) const {
  CppMirBodyEmissionText result;
  result.analysis = analyze(address);
  if (!result.analysis.ready()) {
    return result;
  }
  if (address.kind != MirBodyKind::Function) {
    throw std::logic_error(
        "general MIR body text emission supports function bodies only");
  }
  const MirFunctionInstance *function =
      program_.findFunctionInstance(address.owner);
  if (function == nullptr) {
    throw std::logic_error(
        "general MIR body text emission lost its exact function instance");
  }
  result.text = ScalarBodyTextEmitter(program_, representations_, indentation)
                    .emit(*function, familyLabel, form);
  return result;
}

CppMirBodyEmissionAnalysis
CppMirBodyEmitter::analyze(MirBodyAddress address) const {
  return BodyAnalysisBuilder(program_, representations_, address).run(true);
}

CppMirProgramEmissionAnalysis CppMirBodyEmitter::analyzeProgram() const {
  CppMirProgramEmissionAnalysis analysis;
  analysis.readiness = CppMirBodyEmissionReadiness::Ready;

  const std::vector<MirBodyAddress> addresses =
      enumerateMirBodyAddresses(program_);
  if (addresses.empty()) {
    analysis.readiness = CppMirBodyEmissionReadiness::Incoherent;
    analysis.issues.push_back(
        {.kind = CppMirBodyEmissionIssueKind::InvalidMirProgram,
         .detail = "core MIR body inventory is empty"});
    return analysis;
  }

  BodyAnalysisBuilder validation(program_, representations_, addresses.front());
  validation.validateProgram();
  validation.validateRepresentations();
  CppMirBodyEmissionAnalysis validationResult = validation.finishValidation();
  analysis.readiness =
      mergeReadiness(analysis.readiness, validationResult.readiness);
  analysis.issues = validationResult.issues;

  analysis.bodies.reserve(addresses.size());
  for (const MirBodyAddress address : addresses) {
    CppMirBodyEmissionAnalysis body =
        BodyAnalysisBuilder(program_, representations_, address).run(false);
    analysis.readiness = mergeReadiness(analysis.readiness, body.readiness);
    analysis.bodies.push_back(std::move(body));
  }
  return analysis;
}

} // namespace lang
