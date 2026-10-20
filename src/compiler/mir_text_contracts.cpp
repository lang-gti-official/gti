#include "mir_text_contracts.h"

#include <algorithm>
#include <functional>
#include <set>
#include <span>
#include <unordered_set>

namespace lang::mir_detail {
namespace {
bool textType(const SemanticType &type) {
  return type.kind == SemanticType::StringView ||
         type.kind == SemanticType::CString;
}

struct TextSources {
  bool known = false;
  std::set<std::size_t> parameters;
};

struct TextBody {
  const MirBody &body;
  std::span<const SemanticType> types;
  std::span<const HirBindingId> bindings;
};

class TextContracts {
public:
  explicit TextContracts(const MirProgram &program) : program(program) {}

  MirVerificationResult verify();

private:
  const MirProgram &program;
  MirVerificationResult result;
  std::vector<std::set<std::size_t>> requirements;
  std::vector<std::set<std::size_t>> constructorRequirements;
  std::unordered_set<HirFunctionInstanceId> activeReturns;

  const MirInstruction *definition(const MirBody &body, MirValueId id) const {
    const auto *value = body.findValue(id);
    const auto *block =
        value ? body.findBlock(value->definitionBlock) : nullptr;
    if (!block)
      return nullptr;
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const auto &i) { return i.id == value->definition; });
    return found == block->instructions.end() ? nullptr : &*found;
  }

  void fail(const TextBody &context, std::size_t owner,
            const MirInstruction *instruction, std::string message) {
    result.errors.push_back({.bodyKind = context.body.kind,
                             .owner = owner,
                             .instruction = instruction ? instruction->id : 0,
                             .message = std::move(message)});
  }

  TextSources sources(const TextBody &context, const MirOperand &operand,
                      std::set<std::pair<bool, std::size_t>> &active) {
    if (operand.kind == MirOperandKind::Constant && operand.literal &&
        (std::holds_alternative<std::string>(*operand.literal) ||
         std::holds_alternative<std::nullptr_t>(*operand.literal)))
      return {.known = true};
    if (operand.kind != MirOperandKind::Value) {
      const auto *loan = operand.kind == MirOperandKind::Loan
                             ? context.body.findLoan(operand.loan)
                             : nullptr;
      const auto *place =
          context.body.findPlace(loan ? loan->source : operand.place);
      if (!place || !textType(place->type))
        return {};
      if (isOwnerlessTextFieldProjection(program, context.body, *place))
        return {.known = true};
      if (place->root != MirPlaceRootKind::Binding ||
          !place->projections.empty() ||
          !active.emplace(false, place->binding).second)
        return {};
      TextSources answer;
      const auto formal = std::find(context.bindings.begin(),
                                    context.bindings.end(), place->binding);
      if (formal != context.bindings.end()) {
        const auto index =
            static_cast<std::size_t>(formal - context.bindings.begin());
        if (index < context.types.size() && context.types[index] == place->type)
          answer = {.known = true, .parameters = {index}};
      } else {
        const MirInstruction *initializer = nullptr;
        bool writable = false;
        for (const auto &block : context.body.blocks)
          for (const auto &instruction : block.instructions) {
            const auto *destination =
                instruction.destination
                    ? context.body.findPlace(*instruction.destination)
                    : nullptr;
            if (destination && destination->root == MirPlaceRootKind::Binding &&
                destination->binding == place->binding) {
              if (instruction.kind != MirInstructionKind::Initialize ||
                  initializer)
                writable = true;
              initializer = &instruction;
            }
            for (const auto &input : instruction.operands) {
              const auto *written = context.body.findPlace(input.place);
              if (input.kind == MirOperandKind::BorrowWrite && written &&
                  written->root == MirPlaceRootKind::Binding &&
                  written->binding == place->binding)
                writable = true;
            }
          }
        if (!writable && initializer && initializer->operands.size() == 1)
          answer = sources(context, initializer->operands.front(), active);
      }
      active.erase({false, place->binding});
      return answer;
    }
    if (!active.emplace(true, operand.value).second)
      return {};
    const auto *instruction = definition(context.body, operand.value);
    TextSources answer;
    if (instruction && instruction->operation == MirOperation::Literal &&
        instruction->literal &&
        (std::holds_alternative<std::string>(*instruction->literal) ||
         std::holds_alternative<std::nullptr_t>(*instruction->literal))) {
      answer.known = true;
    } else if (instruction && instruction->operands.size() == 1 &&
               (instruction->kind == MirInstructionKind::Load ||
                instruction->kind == MirInstructionKind::CallInput ||
                instruction->intrinsic == IntrinsicKind::Move)) {
      answer = sources(context, instruction->operands.front(), active);
    } else if (instruction && instruction->kind == MirInstructionKind::Call &&
               instruction->functionTarget) {
      const auto *target =
          program.findFunctionInstance(*instruction->functionTarget);
      if (target && textType(target->returnType) &&
          target->definitionKind == MirDefinitionKind::Source &&
          activeReturns.insert(target->id).second) {
        const TextBody callee{target->body, target->parameterTypes,
                              target->parameterBindings};
        bool sawReturn = false;
        answer.known = true;
        for (const auto &block : target->body.blocks) {
          if (block.terminator.kind != MirTerminatorKind::Return ||
              !block.terminator.value)
            continue;
          sawReturn = true;
          std::set<std::pair<bool, std::size_t>> nested;
          const auto returned =
              sources(callee, *block.terminator.value, nested);
          answer.known = answer.known && returned.known;
          for (const auto parameter : returned.parameters) {
            if (parameter >= instruction->operands.size()) {
              answer.known = false;
              continue;
            }
            const auto mapped =
                sources(context, instruction->operands[parameter], active);
            answer.known = answer.known && mapped.known;
            answer.parameters.insert(mapped.parameters.begin(),
                                     mapped.parameters.end());
          }
        }
        answer.known = answer.known && sawReturn;
        activeReturns.erase(target->id);
      }
    }
    active.erase({true, operand.value});
    return answer;
  }

  TextSources sources(const TextBody &context, const MirOperand &operand) {
    std::set<std::pair<bool, std::size_t>> active;
    return sources(context, operand, active);
  }

  std::set<std::size_t> scan(const TextBody &context, std::size_t owner,
                             bool diagnose) {
    std::set<std::size_t> needed;
    const auto require = [&](const MirInstruction &instruction,
                             const MirOperand &operand) {
      const auto source = sources(context, operand);
      if (!source.known && diagnose)
        fail(context, owner, &instruction,
             "ownerless text storage or call lacks exact producer evidence");
      needed.insert(source.parameters.begin(), source.parameters.end());
    };
    for (const auto &block : context.body.blocks)
      for (const auto &instruction : block.instructions) {
        const std::set<std::size_t> *required = nullptr;
        if (instruction.kind == MirInstructionKind::Call &&
            instruction.functionTarget &&
            *instruction.functionTarget <= requirements.size())
          required = &requirements[*instruction.functionTarget - 1];
        if (instruction.kind == MirInstructionKind::Construct &&
            instruction.constructorTarget &&
            *instruction.constructorTarget <= constructorRequirements.size())
          required =
              &constructorRequirements[*instruction.constructorTarget - 1];
        if (required)
          for (const auto index : *required) {
            if (index < instruction.operands.size())
              require(instruction, instruction.operands[index]);
            else if (diagnose)
              fail(context, owner, &instruction,
                   "ownerless text argument is missing");
          }
        if (instruction.kind == MirInstructionKind::Compute &&
            instruction.operation == MirOperation::PayloadConstruct &&
            instruction.enumVariant) {
          const auto *enumeration =
              program.findPayloadEnum(instruction.info.type);
          if (enumeration &&
              *instruction.enumVariant < enumeration->variants.size()) {
            const auto &fields =
                enumeration->variants[*instruction.enumVariant].payloadTypes;
            for (std::size_t index = 0;
                 index < fields.size() && index < instruction.operands.size();
                 ++index)
              if (textType(fields[index]))
                require(instruction, instruction.operands[index]);
          }
        }
        const auto *destination =
            instruction.destination
                ? context.body.findPlace(*instruction.destination)
                : nullptr;
        const bool uncheckedWrite =
            destination &&
            (destination->foreignStorage ||
             std::any_of(destination->projections.begin(),
                         destination->projections.end(),
                         [](const auto &projection) {
                           return projection.kind == MirProjectionKind::RawIndex ||
                                  projection.kind == MirProjectionKind::RawDereference;
                         }));
        // A text field's invariant must survive every safe writer, including
        // assignments rooted in accessor loans rather than a Field projection.
        // Raw writes keep their explicit unsafe validity obligations.
        const bool checkedTextAssignment =
            destination && textType(destination->type) && !uncheckedWrite &&
            instruction.kind == MirInstructionKind::Assign;
        if (destination &&
            (isOwnerlessTextFieldProjection(program, context.body, *destination) ||
             checkedTextAssignment)) {
          if (instruction.operands.size() == 1)
            require(instruction, instruction.operands.front());
          else if (diagnose)
            fail(context, owner, &instruction,
                 "text storage write lacks one exact source operand");
        }
        if (diagnose && instruction.operation == MirOperation::FunctionItem &&
            instruction.functionTarget &&
            *instruction.functionTarget <= requirements.size() &&
            !requirements[*instruction.functionTarget - 1].empty())
          fail(context, owner, &instruction,
               "function item erases an ownerless text input contract");
      }
    return needed;
  }
};

MirVerificationResult TextContracts::verify() {
  requirements.resize(program.functionInstances().size());
  constructorRequirements.resize(program.constructorInstances().size());
  for (const auto &constructor : program.constructorInstances()) {
    const TextBody context{constructor.body, constructor.parameterTypes,
                           constructor.parameterBindings};
    for (const auto &initializer : constructor.initializers) {
      if (initializer.kind != ConstructorInitializerTargetKind::Field ||
          !textType(initializer.targetType) ||
          initializer.arguments.size() != 1)
        continue;
      const auto found = std::find_if(
          constructor.body.values.begin(), constructor.body.values.end(),
          [&](const auto &value) {
            return value.sourceValue == initializer.arguments.front() &&
                   value.info.type == initializer.targetType;
          });
      const auto source = found == constructor.body.values.end()
                              ? TextSources{}
                              : sources(context, {.kind = MirOperandKind::Value,
                                                  .value = found->id,
                                                  .type = found->info.type});
      if (!source.known)
        fail(context, constructor.id, nullptr,
             "text field initializer has no independent ownerless input proof");
      constructorRequirements[constructor.id - 1].insert(
          source.parameters.begin(), source.parameters.end());
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &function : program.functionInstances()) {
      const auto required = scan(
          {function.body, function.parameterTypes, function.parameterBindings},
          function.id, false);
      auto &known = requirements[function.id - 1];
      const auto before = known.size();
      known.insert(required.begin(), required.end());
      changed = changed || before != known.size();
    }
  }
  for (const auto &function : program.functionInstances()) {
    const TextBody context{function.body, function.parameterTypes,
                           function.parameterBindings};
    (void)scan(context, function.id, true);
    std::set<std::size_t> declared;
    for (const auto index : function.ownerlessTextParameters) {
      if (index >= function.parameterTypes.size()) {
        fail(context, function.id, nullptr,
             "ownerless text contract has an invalid formal index");
        continue;
      }
      if (textType(function.parameterTypes[index]))
        declared.insert(index);
    }
    if (declared != requirements[function.id - 1])
      fail(context, function.id, nullptr,
           "ownerless text input contract differs from independently derived "
           "storage and forwarding");
    if (!declared.empty() &&
        (function.virtualMethod || function.overrideMethod ||
         function.linkage != LanguageLinkage::Gti))
      fail(context, function.id, nullptr,
           "unsupported dispatch erases an ownerless text contract");
    if (function.definitionKind == MirDefinitionKind::Source &&
        textType(function.returnType) &&
        function.returnBorrowOrigin == BorrowOriginKind::None)
      for (const auto &block : function.body.blocks)
        if (block.terminator.kind == MirTerminatorKind::Return &&
            block.terminator.value) {
          const auto returned = sources(context, *block.terminator.value);
          if (!returned.known || !returned.parameters.empty())
            fail(context, function.id, nullptr,
                 "ownerless text return has no independent result proof");
        }
  }
  (void)scan({program.module(), {}, {}}, 0, true);
  for (const auto &instance : program.classInstances()) {
    (void)scan({instance.fieldInitializers, {}, {}}, instance.id, true);
    (void)scan({instance.staticFieldInitializers, {}, {}}, instance.id, true);
  }
  for (const auto &constructor : program.constructorInstances()) {
    const TextBody context{constructor.body, constructor.parameterTypes,
                           constructor.parameterBindings};
    const auto needed = scan(context, constructor.id, true);
    if (!std::includes(constructorRequirements[constructor.id - 1].begin(),
                       constructorRequirements[constructor.id - 1].end(),
                       needed.begin(), needed.end()))
      fail(context, constructor.id, nullptr,
           "constructor call requires an unrepresented ownerless text input "
           "contract");
  }
  for (const auto &destructor : program.destructorInstances())
    (void)scan({destructor.body, {}, {}}, destructor.owner, true);
  for (const auto &lambda : program.lambdaInstances())
    if (!scan({lambda.body, lambda.parameterTypes, lambda.parameterBindings},
              lambda.id, true)
             .empty())
      fail({lambda.body, lambda.parameterTypes, lambda.parameterBindings},
           lambda.id, nullptr,
           "lambda requires an unrepresented ownerless text input contract");
  if (program.embeddingPlan())
    for (const auto &entry : program.embeddingPlan()->exports)
      if (entry.function && entry.function <= requirements.size() &&
          !requirements[entry.function - 1].empty())
        result.errors.push_back(
            {.bodyKind = MirBodyKind::Module,
             .message =
                 "native export erases an ownerless text input contract"});
  return std::move(result);
}
} // namespace

bool isOwnerlessTextFieldProjection(const MirProgram &program,
                                     const MirBody &body,
                                     const MirPlace &place) {
  if (!textType(place.type) || place.foreignStorage ||
      place.projections.empty() ||
      place.projections.back().kind != MirProjectionKind::Field)
    return false;
  std::optional<SemanticType> current;
  for (const auto &root : body.places) {
    if (!root.projections.empty() || root.root != place.root ||
        root.binding != place.binding || root.symbol != place.symbol ||
        root.capture != place.capture || root.temporary != place.temporary ||
        root.value != place.value || root.loan != place.loan)
      continue;
    if (current && *current != root.type)
      return false;
    current = root.type;
  }
  if (!current)
    return false;
  for (const auto &projection : place.projections) {
    if (projection.kind == MirProjectionKind::Field) {
      std::optional<SemanticType> fieldType;
      const auto accept = [&](const SemanticType &type) {
        if (fieldType && *fieldType != type)
          return false;
        fieldType = type;
        return true;
      };
      for (const auto &owner : program.classInstances())
        if (owner.type == *current)
          for (const auto &field : owner.declaredFields)
            if (field.symbol == projection.field && !accept(field.type))
              return false;
      if (const auto *enumeration = program.findPayloadEnum(*current))
        for (const auto &variant : enumeration->variants)
          for (std::size_t index = 0; index < variant.payloadSymbols.size(); ++index)
            if (variant.payloadSymbols[index] == projection.field &&
                (index >= variant.payloadTypes.size() ||
                 !accept(variant.payloadTypes[index])))
              return false;
      if (!fieldType)
        return false;
      current = std::move(fieldType);
    } else if (projection.kind == MirProjectionKind::Dereference &&
               (current->kind == SemanticType::Reference ||
                current->kind == SemanticType::UniqueOwner) &&
               current->arguments.size() == 1) {
      SemanticType pointee = current->arguments.front();
      current = std::move(pointee);
    } else if (projection.kind == MirProjectionKind::Index &&
               current->kind == SemanticType::Array &&
               current->arguments.size() == 1) {
      SemanticType element = current->arguments.front();
      current = std::move(element);
    } else {
      return false;
    }
  }
  return *current == place.type;
}

MirVerificationResult verifyOwnerlessTextContracts(const MirProgram &program) {
  return TextContracts(program).verify();
}
} // namespace lang::mir_detail
