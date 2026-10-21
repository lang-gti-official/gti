#include "mir_text_contracts.h"

#include <algorithm>
#include <functional>
#include <map>
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

  // The ownerless inputs that the constructor's text field initializers
  // require, and the formals that alone initialize one direct text field
  // each, keyed to that field.
  struct InitializerTextInputs {
    std::set<std::size_t> requirements;
    std::map<std::size_t, SymbolId> fields;
  };
  InitializerTextInputs
  initializerTextInputs(const MirConstructorInstance &constructor,
                        bool diagnose);

private:
  const MirProgram &program;
  MirVerificationResult result;
  std::vector<std::set<std::size_t>> requirements;
  std::vector<std::set<std::size_t>> constructorRequirements;
  // Per constructor: the formals that initialize exactly one direct text
  // field and reach no other text initializer, keyed to that field.
  std::vector<std::map<std::size_t, SymbolId>> constructorTextFields;
  // Formals whose text a construction published under its result's loan; the
  // constructor body must not also store them as ownerless text.
  std::set<std::pair<std::size_t, std::size_t>> publishedFormals;
  std::unordered_set<HirFunctionInstanceId> activeReturns;
  std::set<std::size_t> activeTemporaries;

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
      // Static storage of text holds only ownerless text: semantics admits
      // only ownerless initializers and stores, and every MIR assignment to it
      // is a checked text write.
      if (place->root == MirPlaceRootKind::Symbol && place->capture == 0 &&
          !place->foreignStorage && place->projections.empty())
        return {.known = true};
      if (place->root == MirPlaceRootKind::Temporary &&
          place->projections.empty())
        return temporarySources(context, *place, active);
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
    } else if (instruction &&
               instruction->kind == MirInstructionKind::Compute &&
               instruction->operation == MirOperation::PayloadExtract &&
               textType(instruction->info.type)) {
      // A payload enum's text field holds only text that its construction
      // proved ownerless, as a class's text field does.
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
    } else if (!instruction) {
      answer = joinSources(context, operand.value, active);
    }
    active.erase({true, operand.value});
    return answer;
  }

  // A conditional's result: each branch initializes the same temporary before
  // the read. It is known when every initializing operand is; any other write,
  // or a mutable borrow of the temporary, leaves it unknown.
  TextSources temporarySources(const TextBody &context, const MirPlace &place,
                               std::set<std::pair<bool, std::size_t>> &active) {
    if (!activeTemporaries.insert(place.temporary).second)
      return {};
    const auto same = [&](const MirPlace *candidate) {
      return candidate && candidate->root == MirPlaceRootKind::Temporary &&
             candidate->temporary == place.temporary;
    };
    TextSources answer{.known = true};
    bool initialized = false;
    for (const auto &block : context.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *destination =
            instruction.destination
                ? context.body.findPlace(*instruction.destination)
                : nullptr;
        if (same(destination)) {
          if (instruction.kind != MirInstructionKind::Initialize ||
              !destination->projections.empty() ||
              instruction.operands.size() != 1) {
            answer.known = false;
          } else {
            initialized = true;
            const auto source =
                sources(context, instruction.operands.front(), active);
            answer.known = answer.known && source.known;
            answer.parameters.insert(source.parameters.begin(),
                                     source.parameters.end());
          }
        }
        for (const auto &input : instruction.operands)
          if (input.kind == MirOperandKind::BorrowWrite &&
              same(context.body.findPlace(input.place)))
            answer.known = false;
      }
    activeTemporaries.erase(place.temporary);
    answer.known = answer.known && initialized;
    return answer;
  }

  // A block parameter joins its incoming edge arguments: it is known when
  // every incoming argument is, and depends on each one's parameters.
  TextSources joinSources(const TextBody &context, MirValueId id,
                          std::set<std::pair<bool, std::size_t>> &active) {
    const auto *value = context.body.findValue(id);
    const auto *block = value && value->definition == 0
                            ? context.body.findBlock(value->definitionBlock)
                            : nullptr;
    if (!block)
      return {};
    const auto parameter =
        std::find_if(block->parameters.begin(), block->parameters.end(),
                     [&](const MirBlockParameter &candidate) {
                       return candidate.value == id;
                     });
    if (parameter == block->parameters.end())
      return {};
    const auto index =
        static_cast<std::size_t>(parameter - block->parameters.begin());
    TextSources answer{.known = true};
    bool incoming = false;
    const auto edge = [&](MirBlockId target,
                          const std::vector<MirOperand> &arguments) {
      if (target != block->id)
        return;
      incoming = true;
      if (index >= arguments.size()) {
        answer.known = false;
        return;
      }
      const auto source = sources(context, arguments[index], active);
      answer.known = answer.known && source.known;
      answer.parameters.insert(source.parameters.begin(),
                               source.parameters.end());
    };
    for (const auto &predecessor : context.body.blocks) {
      edge(predecessor.terminator.target,
           predecessor.terminator.targetArguments);
      edge(predecessor.terminator.elseTarget,
           predecessor.terminator.elseArguments);
      for (const auto &target : predecessor.terminator.switchTargets)
        edge(target.target, target.arguments);
    }
    answer.known = answer.known && incoming;
    return answer;
  }

  TextSources sources(const TextBody &context, const MirOperand &operand) {
    std::set<std::pair<bool, std::size_t>> active;
    return sources(context, operand, active);
  }

  // A construction whose result carries a read-only Stored loan designating
  // one direct text field publishes the argument for that field as dependent
  // text: the loan, not an ownerless proof, keeps the text valid.
  bool publishesText(const TextBody &context, const MirInstruction &instruction,
                     std::size_t index) {
    const auto constructor = *instruction.constructorTarget;
    const auto &fields = constructorTextFields[constructor - 1];
    const auto field = fields.find(index);
    const auto *loan =
        instruction.loan ? context.body.findLoan(*instruction.loan) : nullptr;
    if (!instruction.dependentTextPublication || field == fields.end() ||
        !loan || loan->kind != MirLoanKind::Stored ||
        loan->access != AccessMode::ReadOnly ||
        loan->designatedFields != std::vector<SymbolId>{field->second} ||
        !loan->additionalDependencies.empty() ||
        !directTextField(program, instruction.info.type, field->second))
      return false;
    const BorrowedFieldOrigin expected{.resultFields = {field->second},
                                       .origin = BorrowOriginKind::Argument,
                                       .parameter = index,
                                       .access = AccessMode::ReadOnly};
    if (std::find(instruction.borrowFields.begin(),
                  instruction.borrowFields.end(),
                  expected) == instruction.borrowFields.end())
      return false;
    publishedFormals.emplace(constructor, index);
    return true;
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
            instruction.functionTarget && *instruction.functionTarget != 0 &&
            *instruction.functionTarget <= requirements.size())
          required = &requirements[*instruction.functionTarget - 1];
        if (instruction.kind == MirInstructionKind::Construct &&
            instruction.constructorTarget &&
            *instruction.constructorTarget != 0 &&
            *instruction.constructorTarget <= constructorRequirements.size())
          required =
              &constructorRequirements[*instruction.constructorTarget - 1];
        if (required)
          for (const auto index : *required) {
            if (instruction.kind == MirInstructionKind::Construct &&
                publishesText(context, instruction, index))
              continue;
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
             std::any_of(
                 destination->projections.begin(),
                 destination->projections.end(), [](const auto &projection) {
                   return projection.kind == MirProjectionKind::RawIndex ||
                          projection.kind == MirProjectionKind::RawDereference;
                 }));
        // A text field's invariant must survive every safe writer, including
        // assignments rooted in accessor loans rather than a Field projection.
        // Raw writes keep their explicit unsafe validity obligations.
        // A local text binding, or an element of a local text array, is not
        // field storage: it may hold dynamic text, and no reader treats it as
        // ownerless.
        const bool localTextStorage =
            destination && destination->root == MirPlaceRootKind::Binding &&
            std::all_of(destination->projections.begin(),
                        destination->projections.end(),
                        [](const auto &projection) {
                          return projection.kind == MirProjectionKind::Index;
                        });
        const bool checkedTextAssignment =
            destination && textType(destination->type) && !uncheckedWrite &&
            !localTextStorage && instruction.kind == MirInstructionKind::Assign;
        if (destination && (isOwnerlessTextFieldProjection(
                                program, context.body, *destination) ||
                            checkedTextAssignment)) {
          if (instruction.operands.size() == 1)
            require(instruction, instruction.operands.front());
          else if (diagnose)
            fail(context, owner, &instruction,
                 "text storage write lacks one exact source operand");
        }
        if (diagnose && instruction.operation == MirOperation::FunctionItem &&
            instruction.functionTarget && *instruction.functionTarget != 0 &&
            *instruction.functionTarget <= requirements.size() &&
            !requirements[*instruction.functionTarget - 1].empty())
          fail(context, owner, &instruction,
               "function item erases an ownerless text input contract");
      }
    return needed;
  }
};

auto TextContracts::initializerTextInputs(
    const MirConstructorInstance &constructor, bool diagnose)
    -> InitializerTextInputs {
  const TextBody context{constructor.body, constructor.parameterTypes,
                         constructor.parameterBindings};
  InitializerTextInputs inputs;
  std::map<std::size_t, std::size_t> uses;
  for (const auto &initializer : constructor.initializers) {
    if (initializer.kind != ConstructorInitializerTargetKind::Field ||
        !textType(initializer.targetType) || initializer.arguments.size() != 1)
      continue;
    // A `nullptr` initializer of a c_string field keeps its own literal
    // type; the literal is ownerless text by construction.
    const auto found = std::find_if(
        constructor.body.values.begin(), constructor.body.values.end(),
        [&](const auto &value) {
          return value.sourceValue == initializer.arguments.front() &&
                 (value.info.type == initializer.targetType ||
                  (initializer.targetType.kind == SemanticType::CString &&
                   value.info.type.kind == SemanticType::NullPtr));
        });
    const auto source = found == constructor.body.values.end()
                            ? TextSources{}
                            : sources(context, {.kind = MirOperandKind::Value,
                                                .value = found->id,
                                                .type = found->info.type});
    if (!source.known && diagnose)
      fail(context, constructor.id, nullptr,
           "text field initializer has no independent ownerless input proof");
    inputs.requirements.insert(source.parameters.begin(),
                               source.parameters.end());
    for (const auto parameter : source.parameters)
      ++uses[parameter];
    if (source.known && source.parameters.size() == 1 && initializer.field != 0)
      inputs.fields.emplace(*source.parameters.begin(), initializer.field);
  }
  std::erase_if(inputs.fields,
                [&](const auto &entry) { return uses[entry.first] != 1; });
  return inputs;
}

MirVerificationResult TextContracts::verify() {
  requirements.resize(program.functionInstances().size());
  constructorRequirements.resize(program.constructorInstances().size());
  constructorTextFields.resize(program.constructorInstances().size());
  for (const auto &constructor : program.constructorInstances()) {
    auto inputs = initializerTextInputs(constructor, true);
    constructorRequirements[constructor.id - 1] =
        std::move(inputs.requirements);
    constructorTextFields[constructor.id - 1] = std::move(inputs.fields);
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
  std::vector<std::set<std::size_t>> constructorBodyNeeds;
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
    constructorBodyNeeds.push_back(needed);
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
  for (const auto &[constructor, index] : publishedFormals)
    if (constructorBodyNeeds[constructor - 1].contains(index)) {
      const auto &instance = program.constructorInstances()[constructor - 1];
      fail({instance.body, instance.parameterTypes, instance.parameterBindings},
           constructor, nullptr,
           "dependent text argument also reaches ownerless text storage");
    }
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

namespace {
// Whether the object that a direct text-field place reads carries a Stored
// loan designating that field: one of the loan's carrier bindings, or the
// value that its publishing construction produced (L-OWN-02). Such a field
// holds its owner's dynamic text.
bool dependentTextCarrierField(const MirBody &body, const MirPlace &place) {
  if (place.projections.size() != 1 ||
      place.projections.front().kind != MirProjectionKind::Field)
    return false;
  const std::vector<SymbolId> field{place.projections.front().field};
  const MirPlace *root = &place;
  for (std::size_t depth = 0;
       root->root == MirPlaceRootKind::Loan && depth <= body.loans.size();
       ++depth) {
    const auto *loan = body.findLoan(root->loan);
    const auto *source = loan ? body.findPlace(loan->source) : nullptr;
    if (!source || !source->projections.empty())
      return false;
    root = source;
  }
  const auto *value = root->root == MirPlaceRootKind::Value
                          ? body.findValue(root->value)
                          : nullptr;
  return std::any_of(
      body.loans.begin(), body.loans.end(), [&](const MirLoan &loan) {
        return loan.kind == MirLoanKind::Stored &&
               loan.designatedFields == field &&
               ((root->root == MirPlaceRootKind::Binding &&
                 std::find(loan.carriers.begin(), loan.carriers.end(),
                           root->binding) != loan.carriers.end()) ||
                (value != nullptr && value->sourceValue == loan.producedBy));
      });
}
} // namespace

bool isOwnerlessTextFieldProjection(const MirProgram &program,
                                    const MirBody &body,
                                    const MirPlace &place) {
  // A text field, or an element of a text array reached through a field,
  // holds only text its checked writers proved ownerless. A local array does
  // not: it never crosses a field projection.
  if (!textType(place.type) || place.foreignStorage ||
      place.projections.empty() ||
      (place.projections.back().kind != MirProjectionKind::Field &&
       place.projections.back().kind != MirProjectionKind::Index) ||
      std::none_of(place.projections.begin(), place.projections.end(),
                   [](const auto &projection) {
                     return projection.kind == MirProjectionKind::Field;
                   }) ||
      dependentTextCarrierField(body, place))
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
          for (std::size_t index = 0; index < variant.payloadSymbols.size();
               ++index)
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

bool directTextField(const MirProgram &program, const SemanticType &type,
                     SymbolId field) {
  if (type.kind != SemanticType::Class || field == 0)
    return false;
  const auto instance = std::find_if(
      program.classInstances().begin(), program.classInstances().end(),
      [&](const auto &candidate) { return candidate.type == type; });
  return instance != program.classInstances().end() &&
         !instance->traits.containsBorrowedState &&
         std::any_of(instance->declaredFields.begin(),
                     instance->declaredFields.end(), [&](const auto &entry) {
                       return entry.symbol == field && textType(entry.type);
                     });
}

std::optional<std::size_t>
dependentTextFieldFormal(const MirProgram &program,
                         const MirConstructorInstance &constructor,
                         SymbolId field) {
  TextContracts contracts(program);
  for (const auto &[formal, target] :
       contracts.initializerTextInputs(constructor, false).fields)
    if (target == field)
      return formal;
  return std::nullopt;
}

MirVerificationResult verifyOwnerlessTextContracts(const MirProgram &program) {
  return TextContracts(program).verify();
}
} // namespace lang::mir_detail
