#include "gti/optimization/rewrite.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace lang {
namespace {

[[nodiscard]] std::size_t bodyGroup(MirBodyKind kind) {
  switch (kind) {
  case MirBodyKind::Module:
    return 0;
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
    return 1;
  case MirBodyKind::Function:
    return 2;
  case MirBodyKind::Constructor:
    return 3;
  case MirBodyKind::Destructor:
    return 4;
  case MirBodyKind::Lambda:
    return 5;
  }
  return 6;
}

[[nodiscard]] auto bodyKey(MirBodyAddress address) {
  return std::tuple{bodyGroup(address.kind), address.owner,
                    static_cast<std::size_t>(address.kind)};
}

[[nodiscard]] bool addressLess(const MirInstructionAddress &left,
                               const MirInstructionAddress &right) {
  if (bodyKey(left.body) != bodyKey(right.body)) {
    return bodyKey(left.body) < bodyKey(right.body);
  }
  return std::tie(left.block, left.index) < std::tie(right.block, right.index);
}

void addError(MirEditResult &result, MirInstructionAddress address,
              MirInstructionId instruction, std::string message) {
  result.verification.errors.push_back({.bodyKind = address.body.kind,
                                        .owner = address.body.owner,
                                        .block = address.block,
                                        .instruction = instruction,
                                        .message = std::move(message)});
}

} // namespace

std::vector<MirBodyAddress> MirProgramEditor::bodies() const {
  std::vector<MirBodyAddress> result;
  result.push_back({.kind = MirBodyKind::Module});
  for (const MirClassInstance &instance : program.classInstances()) {
    result.push_back(
        {.kind = MirBodyKind::FieldInitializers, .owner = instance.id});
    result.push_back(
        {.kind = MirBodyKind::StaticFieldInitializers, .owner = instance.id});
  }
  for (const MirFunctionInstance &instance : program.functionInstances()) {
    result.push_back({.kind = MirBodyKind::Function, .owner = instance.id});
  }
  for (const MirConstructorInstance &instance :
       program.constructorInstances()) {
    result.push_back({.kind = MirBodyKind::Constructor, .owner = instance.id});
  }
  for (const MirDestructorInstance &instance : program.destructorInstances()) {
    result.push_back({.kind = MirBodyKind::Destructor, .owner = instance.id});
  }
  for (const MirLambdaInstance &instance : program.lambdaInstances()) {
    result.push_back({.kind = MirBodyKind::Lambda, .owner = instance.id});
  }
  return result;
}

const MirBody *MirProgramEditor::body(MirBodyAddress address) const {
  switch (address.kind) {
  case MirBodyKind::Module:
    return address.owner == 0 ? &program.module() : nullptr;
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
    if (const MirClassInstance *instance =
            program.findClassInstance(address.owner)) {
      return address.kind == MirBodyKind::FieldInitializers
                 ? &instance->fieldInitializers
                 : &instance->staticFieldInitializers;
    }
    return nullptr;
  case MirBodyKind::Function:
    if (const MirFunctionInstance *instance =
            program.findFunctionInstance(address.owner)) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::Constructor:
    if (const MirConstructorInstance *instance =
            program.findConstructorInstance(address.owner)) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::Destructor:
    if (const MirDestructorInstance *instance =
            program.findDestructorInstance(address.owner)) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::Lambda:
    if (const MirLambdaInstance *instance = program.findLambda(address.owner)) {
      return &instance->body;
    }
    return nullptr;
  }
  return nullptr;
}

void MirProgramEditor::queueLiteralReplacement(
    MirInstructionAddress address, MirInstructionId expectedInstruction,
    MirOperation expectedOperation, Literal literal) {
  patches.push_back({.address = address,
                     .expectedInstruction = expectedInstruction,
                     .expectedOperation = expectedOperation,
                     .literal = std::move(literal)});
}

MirEditResult MirProgramEditor::apply() {
  MirEditResult result;
  std::vector<LiteralReplacement> queued = std::move(patches);
  patches.clear();
  if (queued.empty()) {
    return result;
  }

  std::sort(
      queued.begin(), queued.end(),
      [](const LiteralReplacement &left, const LiteralReplacement &right) {
        return addressLess(left.address, right.address);
      });

  for (std::size_t index = 0; index < queued.size(); ++index) {
    const LiteralReplacement &patch = queued[index];
    if (index != 0 && patch.address == queued[index - 1].address) {
      addError(result, patch.address, patch.expectedInstruction,
               "duplicate MIR instruction replacement");
      continue;
    }
    const MirBody *target = body(patch.address.body);
    if (target == nullptr) {
      addError(result, patch.address, patch.expectedInstruction,
               "MIR replacement body does not exist");
      continue;
    }
    const MirBlock *block = target->findBlock(patch.address.block);
    if (block == nullptr || patch.address.index >= block->instructions.size()) {
      addError(result, patch.address, patch.expectedInstruction,
               "MIR replacement address is outside the body");
      continue;
    }
    const MirInstruction &instruction =
        block->instructions[patch.address.index];
    if (patch.expectedInstruction == 0 ||
        instruction.id != patch.expectedInstruction) {
      addError(result, patch.address, patch.expectedInstruction,
               "MIR replacement target is stale");
      continue;
    }
    if (instruction.kind != MirInstructionKind::Compute ||
        instruction.operation != patch.expectedOperation) {
      addError(result, patch.address, patch.expectedInstruction,
               "MIR replacement target is not the expected computation");
      continue;
    }
    if (const auto *integer = std::get_if<std::uint64_t>(&patch.literal)) {
      const std::optional<CheckedIntegerDomain> domain =
          constantIntegerDomain(instruction.info.type);
      if (!domain ||
          !evaluateConstantLiteral(Literal{*integer}, *domain).value) {
        addError(result, patch.address, patch.expectedInstruction,
                 "MIR integer literal replacement is outside its result "
                 "domain");
      }
    }
  }
  if (!result.verification.valid()) {
    return result;
  }

  MirProgram candidate = program;
  const auto mutableBody = [&candidate](MirBodyAddress address) -> MirBody * {
    switch (address.kind) {
    case MirBodyKind::Module:
      return address.owner == 0 ? &candidate.moduleBody : nullptr;
    case MirBodyKind::FieldInitializers:
    case MirBodyKind::StaticFieldInitializers:
      for (MirClassInstance &instance : candidate.classes) {
        if (instance.id == address.owner) {
          return address.kind == MirBodyKind::FieldInitializers
                     ? &instance.fieldInitializers
                     : &instance.staticFieldInitializers;
        }
      }
      return nullptr;
    case MirBodyKind::Function:
      for (MirFunctionInstance &instance : candidate.functions) {
        if (instance.id == address.owner) {
          return &instance.body;
        }
      }
      return nullptr;
    case MirBodyKind::Constructor:
      for (MirConstructorInstance &instance : candidate.constructors) {
        if (instance.id == address.owner) {
          return &instance.body;
        }
      }
      return nullptr;
    case MirBodyKind::Destructor:
      for (MirDestructorInstance &instance : candidate.destructors) {
        if (instance.id == address.owner) {
          return &instance.body;
        }
      }
      return nullptr;
    case MirBodyKind::Lambda:
      for (MirLambdaInstance &instance : candidate.lambdas) {
        if (instance.id == address.owner) {
          return &instance.body;
        }
      }
      return nullptr;
    }
    return nullptr;
  };

  std::vector<MirBodyAddress> touched;
  for (const LiteralReplacement &patch : queued) {
    MirBody *target = mutableBody(patch.address.body);
    MirInstruction &instruction = target->blocks[patch.address.block - 1]
                                      .instructions[patch.address.index];
    MirInstruction replacement;
    replacement.id = instruction.id;
    replacement.kind = MirInstructionKind::Compute;
    replacement.hirValue = instruction.hirValue;
    replacement.hirStatement = instruction.hirStatement;
    replacement.unsafeOperation = instruction.unsafeOperation;
    replacement.rawMemoryAccess = instruction.rawMemoryAccess;
    replacement.result = instruction.result;
    replacement.operation = MirOperation::Literal;
    replacement.literal = patch.literal;
    replacement.info = instruction.info;
    instruction = std::move(replacement);
    if (std::find(touched.begin(), touched.end(), patch.address.body) ==
        touched.end()) {
      touched.push_back(patch.address.body);
    }
  }

  for (MirBodyAddress address : touched) {
    MirBody *target = mutableBody(address);
    if (target == nullptr || !rebuildMirValueUses(*target)) {
      addError(result, {.body = address}, 0,
               "MIR replacement produced invalid value uses");
    }
  }
  if (!result.verification.valid()) {
    return result;
  }

  result.verification = verifyMirProgram(candidate);
  if (!result.verification.valid()) {
    return result;
  }

  program = std::move(candidate);
  result.changed = true;
  result.appliedPatches = queued.size();
  result.valueUsesRebuilt = true;
  result.invalidation.instructionFacts = true;
  result.invalidation.valueUses = true;
  return result;
}

} // namespace lang
