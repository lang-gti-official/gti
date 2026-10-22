#include "mir_text_contracts.h"

#include "mir_verification_queries.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <utility>

namespace lang::mir_detail {
namespace {
bool textType(const SemanticType &type) {
  return type.kind == SemanticType::StringView ||
         type.kind == SemanticType::CString;
}

struct TextSources {
  bool known = false;
  std::set<std::size_t> parameters;
  // Text fields of formals or of the receiver that this text is read from:
  // ownerless only under the function's field contract (L-OWN-02).
  std::set<OwnerlessTextField> fields;

  // Text that may hold either: known only when both are.
  void join(const TextSources &other) {
    known = known && other.known;
    parameters.insert(other.parameters.begin(), other.parameters.end());
    fields.insert(other.fields.begin(), other.fields.end());
  }
  friend bool operator==(const TextSources &, const TextSources &) = default;
};

struct TextBody {
  const MirBody &body;
  std::span<const SemanticType> types;
  std::span<const HirBindingId> bindings;
  // A constructor's text fields that a construction may publish, each mapped
  // to the formal that initializes it (L-OWN-02).
  const std::map<SymbolId, std::size_t> *publishedFields = nullptr;
  // A function whose formals and receiver carry field-level contracts.
  const MirFunctionInstance *fieldContracts = nullptr;
  // The function whose body this is, when its returns may relay formals.
  const MirFunctionInstance *function = nullptr;
};

// A class whose direct text fields may hold a relayed owner's text: no
// borrowed state, bases, virtual methods or declared destructor. This
// implies the semantic publication rule, so MIR never derives a field
// requirement that semantics could not have declared.
bool textPublishableClass(const MirProgram &program, const SemanticType &type) {
  if (type.kind != SemanticType::Class)
    return false;
  const auto instance = std::find_if(
      program.classInstances().begin(), program.classInstances().end(),
      [&](const auto &candidate) { return candidate.type == type; });
  return instance != program.classInstances().end() &&
         !instance->traits.containsBorrowedState && instance->bases.empty() &&
         !instance->polymorphic &&
         instance->destructorStatus == SpecialMemberStatus::Generated;
}

// Whether a function's formal or receiver is read-only: a read-only
// receiver, a read-only reference, or a by-value formal whose binding place
// is read-only. Only such a formal takes a relayed value.
bool readOnlyContractFormal(const MirFunctionInstance &function,
                            const OwnerlessTextField &formal) {
  if (formal.receiver)
    return function.receiverMutability == ReceiverMutability::ReadOnly;
  if (formal.parameter >= function.parameterTypes.size() ||
      formal.parameter >= function.parameterBindings.size())
    return false;
  const auto &type = function.parameterTypes[formal.parameter];
  if (type.kind == SemanticType::Reference)
    return type.referenceAccess == AccessMode::ReadOnly;
  const auto binding = function.parameterBindings[formal.parameter];
  return std::any_of(function.body.places.begin(), function.body.places.end(),
                     [&](const MirPlace &place) {
                       return place.root == MirPlaceRootKind::Binding &&
                              place.binding == binding &&
                              place.projections.empty() &&
                              place.access == AccessMode::ReadOnly;
                     });
}

// A non-generic, non-virtual GTI source function or method, including an
// instance's method of a generic class: the functions that carry field-level
// contracts.
bool fieldContractFunction(const MirProgram &program,
                           const MirFunctionInstance &function) {
  if (function.definitionKind != MirDefinitionKind::Source ||
      function.virtualMethod || function.overrideMethod ||
      function.linkage != LanguageLinkage::Gti ||
      !function.typeArguments.empty() || !function.valueArguments.empty())
    return false;
  return !function.owner || program.findClassInstance(*function.owner);
}

class TextContracts {
public:
  explicit TextContracts(const MirProgram &program)
      : program(program), returnSummaries(program.functionInstances().size()) {}

  MirVerificationResult verify();

  // The ownerless inputs that the constructor's text field initializers
  // require, and the formals that alone initialize one direct text field
  // each, keyed to that field.
  struct InitializerTextInputs {
    std::set<std::size_t> requirements;
    std::map<std::size_t, SymbolId> fields;
  };
  InitializerTextInputs initializerTextInputs(
      const MirConstructorInstance &constructor, bool diagnose,
      const std::map<SymbolId, std::size_t> *published = nullptr);

private:
  const MirProgram &program;
  MirVerificationResult result;
  std::vector<std::set<std::size_t>> requirements;
  // Derived per function: the formal and receiver text fields it lets
  // outlive the call (L-OWN-02).
  std::vector<std::set<OwnerlessTextField>> fieldRequirements;
  std::vector<std::set<std::size_t>> constructorRequirements;
  // Per constructor: the formals that initialize exactly one direct text
  // field and reach no other text initializer, keyed to that field.
  std::vector<std::map<std::size_t, SymbolId>> constructorTextFields;
  // Per constructor: each publishable text field and the formal that
  // initializes it, before excluding fields read back through `this`.
  std::vector<std::map<SymbolId, std::size_t>> constructorPublishedFields;
  // Formals whose text a construction published under its result's loan; the
  // constructor body must not also store them as ownerless text.
  std::set<std::pair<std::size_t, std::size_t>> publishedFormals;
  // Derived per function: the formals a returned fresh value holds in a
  // direct text field, which each call then publishes (L-OWN-02).
  std::vector<std::set<PublishedTextField>> relays;
  // Per function instance, by position: what its text result may hold
  // (`returnSources`).
  struct ReturnSummary {
    // Empty: the result of no completed return. A summary only grows.
    TextSources sources{.known = true};
    // The 1-based position on the evaluation stack while its returns are
    // being evaluated, and zero otherwise.
    std::size_t depth = 0;
    // A summary whose evaluation read one lower on the stack is provisional
    // until an evaluation that read nothing lower completes; it keeps the
    // lowest position it depends on.
    std::size_t lowestRead = 0;
    bool provisional = false;
    bool complete = false;
  };
  std::vector<ReturnSummary> returnSummaries;
  // Provisional summaries, in the order they were computed.
  std::vector<std::size_t> provisionalSummaries;
  std::size_t returnDepth = 0;
  // The lowest stack position the current evaluation has read.
  std::size_t readDepth = std::numeric_limits<std::size_t>::max();
  // Whether a provisional summary grew in the current evaluation.
  bool provisionalGrew = false;
  std::set<std::size_t> activeTemporaries;

  // Whether a Return of this body yields exactly `value`.
  static bool returnsValue(const MirBody &body, MirValueId value) {
    return std::any_of(
        body.blocks.begin(), body.blocks.end(), [&](const MirBlock &block) {
          return block.terminator.kind == MirTerminatorKind::Return &&
                 block.terminator.value &&
                 block.terminator.value->kind == MirOperandKind::Value &&
                 block.terminator.value->value == value;
        });
  }

  // Whether `instruction` produces this function's returned fresh value of
  // its own return type, whose text fields may then relay formals.
  bool returnedFreshValue(const TextBody &context,
                          const MirInstruction &instruction) const {
    return context.function && instruction.result &&
           instruction.info.type == context.function->returnType &&
           textPublishableClass(program, instruction.info.type) &&
           returnsValue(context.body, *instruction.result);
  }

  // The by-value text formal an operand reads directly, through its call
  // stage and an exact load or move of the formal's binding.
  std::optional<std::size_t> exactTextFormal(const TextBody &context,
                                             const MirOperand &operand) const {
    const MirOperand *current = &operand;
    for (std::size_t depth = 0;
         depth < 3 && current->kind == MirOperandKind::Value; ++depth) {
      const auto *producer = definition(context.body, current->value);
      if (!producer || producer->operands.size() != 1 ||
          (producer->kind != MirInstructionKind::CallInput &&
           producer->kind != MirInstructionKind::Load &&
           producer->kind != MirInstructionKind::Move))
        return std::nullopt;
      current = &producer->operands.front();
    }
    const auto *place = current->kind == MirOperandKind::Copy ||
                                current->kind == MirOperandKind::Move
                            ? context.body.findPlace(current->place)
                            : nullptr;
    if (!place || place->root != MirPlaceRootKind::Binding ||
        !place->projections.empty() || place->capture != 0)
      return std::nullopt;
    const auto found = std::find(context.bindings.begin(),
                                 context.bindings.end(), place->binding);
    const auto index =
        static_cast<std::size_t>(found - context.bindings.begin());
    if (found == context.bindings.end() || index >= context.types.size() ||
        !textType(context.types[index]))
      return std::nullopt;
    return index;
  }

  // A call that publishes a dynamic argument at a callee's published text
  // field carries its own read-only Stored loan designating that field.
  static bool publishesCallText(const TextBody &context,
                                const MirInstruction &call,
                                const PublishedTextField &entry) {
    const auto *loan = call.loan ? context.body.findLoan(*call.loan) : nullptr;
    const BorrowedFieldOrigin expected{.resultFields = {entry.field},
                                       .origin = BorrowOriginKind::Argument,
                                       .parameter = entry.parameter,
                                       .access = AccessMode::ReadOnly};
    return call.dependentTextPublication && loan &&
           loan->kind == MirLoanKind::Stored &&
           loan->access == AccessMode::ReadOnly &&
           loan->designatedFields == std::vector<SymbolId>{entry.field} &&
           loan->additionalDependencies.empty() &&
           std::find(call.borrowFields.begin(), call.borrowFields.end(),
                     expected) != call.borrowFields.end();
  }

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

  // The formal or receiver of a publishable class that `place` names in a
  // field-contract function: its whole value (field 0), or one of its direct
  // text fields. Other paths name neither.
  std::optional<OwnerlessTextField> contractPlace(const TextBody &context,
                                                  const MirPlace &place) const {
    if (!context.fieldContracts)
      return std::nullopt;
    OwnerlessTextField formal;
    SemanticType type = SemanticType::Unknown;
    std::size_t offset = 0;
    if (place.root == MirPlaceRootKind::This) {
      const auto &owner = context.fieldContracts->owner;
      const auto *instance =
          owner ? program.findClassInstance(*owner) : nullptr;
      if (!instance || context.fieldContracts->staticMember)
        return std::nullopt;
      formal.receiver = true;
      type = instance->type;
    } else if (place.root == MirPlaceRootKind::Binding && place.capture == 0) {
      const auto found = std::find(context.bindings.begin(),
                                   context.bindings.end(), place.binding);
      formal.parameter =
          static_cast<std::size_t>(found - context.bindings.begin());
      if (found == context.bindings.end() ||
          formal.parameter >= context.types.size())
        return std::nullopt;
      type = context.types[formal.parameter];
      if (type.kind == SemanticType::Reference && type.arguments.size() == 1) {
        type = SemanticType(type.arguments.front());
        if (!place.projections.empty() &&
            place.projections.front().kind == MirProjectionKind::Dereference)
          offset = 1;
      }
    } else {
      return std::nullopt;
    }
    if (!textPublishableClass(program, type) ||
        !readOnlyContractFormal(*context.fieldContracts, formal))
      return std::nullopt;
    if (place.projections.size() == offset)
      return formal;
    if (place.projections.size() == offset + 1 &&
        place.projections[offset].kind == MirProjectionKind::Field &&
        directTextField(program, type, place.projections[offset].field)) {
      formal.field = place.projections[offset].field;
      return formal;
    }
    return std::nullopt;
  }

  // The direct text fields of a formal's or the receiver's class.
  std::vector<SymbolId>
  formalTextFields(const TextBody &context,
                   const OwnerlessTextField &formal) const {
    SemanticType type = SemanticType::Unknown;
    if (formal.receiver) {
      const auto &owner = context.fieldContracts->owner;
      const auto *instance =
          owner ? program.findClassInstance(*owner) : nullptr;
      if (instance)
        type = instance->type;
    } else if (formal.parameter < context.types.size()) {
      type = context.types[formal.parameter];
      if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
        type = SemanticType(type.arguments.front());
    }
    std::vector<SymbolId> fields;
    const auto instance = std::find_if(
        program.classInstances().begin(), program.classInstances().end(),
        [&](const auto &candidate) { return candidate.type == type; });
    if (instance != program.classInstances().end())
      for (const auto &field : instance->declaredFields)
        if (textType(field.type))
          fields.push_back(field.symbol);
    return fields;
  }

  // The text field that a dependent carrier operand's Stored loan designates:
  // a binding the loan carries, or the construction result it produced.
  std::optional<SymbolId>
  dependentDesignation(const MirBody &body, const MirOperand &operand) const {
    const auto *loan = operand.kind == MirOperandKind::Loan
                           ? body.findLoan(operand.loan)
                           : nullptr;
    const auto *place = body.findPlace(loan ? loan->source : operand.place);
    const auto *value = operand.kind == MirOperandKind::Value
                            ? body.findValue(operand.value)
                        : place && place->root == MirPlaceRootKind::Value
                            ? body.findValue(place->value)
                            : nullptr;
    // Only a text dependency counts: a reference carrier's Stored loan also
    // designates a field, but its class has type-level borrowed state.
    const SemanticType *type = value   ? &value->info.type
                               : place ? &place->type
                                       : nullptr;
    for (const auto &candidate : body.loans)
      if (type && candidate.kind == MirLoanKind::Stored &&
          candidate.designatedFields.size() == 1 &&
          (directTextField(program, *type,
                           candidate.designatedFields.front()) ||
           directTextPayload(program, *type,
                             candidate.designatedFields.front())) &&
          ((place && place->root == MirPlaceRootKind::Binding &&
            place->projections.empty() &&
            std::find(candidate.carriers.begin(), candidate.carriers.end(),
                      place->binding) != candidate.carriers.end()) ||
           (value && value->sourceValue == candidate.producedBy)))
        return candidate.designatedFields.front();
    return std::nullopt;
  }

  // What a call's argument or receiver holds in one of its direct text
  // fields: that field of a formal or the receiver here, unknown text where a
  // dependent carrier designates the field, and ownerless text otherwise.
  TextSources heldFieldSources(const TextBody &context,
                               const MirOperand &operand,
                               SymbolId field) const {
    const MirOperand *held = &operand;
    if (held->kind == MirOperandKind::Value)
      if (const auto *input = definition(context.body, held->value);
          input != nullptr && input->kind == MirInstructionKind::CallInput &&
          input->operands.size() == 1)
        held = &input->operands.front();
    if (const auto designated = dependentDesignation(context.body, *held))
      return *designated == field ? TextSources{} : TextSources{.known = true};
    const auto *loan = held->kind == MirOperandKind::Loan
                           ? context.body.findLoan(held->loan)
                           : nullptr;
    const auto *place =
        held->kind == MirOperandKind::Value
            ? nullptr
            : context.body.findPlace(loan ? loan->source : held->place);
    if (const auto formal =
            place ? contractPlace(context, *place) : std::nullopt;
        formal && formal->field == 0)
      return {.known = true,
              .fields = {{.receiver = formal->receiver,
                          .parameter = formal->parameter,
                          .field = field}}};
    return {.known = true};
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
      // Text read through a reference that a call returned is the text that
      // the callee's returned reference designates.
      if (place->root == MirPlaceRootKind::Loan && place->projections.empty())
        if (const auto *call = textReferenceCall(context.body, place->loan))
          return callResultSources(context, *call, active);
      // A reference bound to a payload views the extracted payload value.
      if (place->root == MirPlaceRootKind::Value && place->projections.empty())
        return sources(context,
                       {.kind = MirOperandKind::Value,
                        .value = place->value,
                        .type = place->type},
                       active);
      // A published field read back through `this` inside its constructor
      // holds that formal's text, not ownerless text.
      if (context.publishedFields && place->root == MirPlaceRootKind::This &&
          place->projections.size() == 1 &&
          place->projections.front().kind == MirProjectionKind::Field) {
        const auto published =
            context.publishedFields->find(place->projections.front().field);
        if (published != context.publishedFields->end())
          return {.known = true, .parameters = {published->second}};
      }
      if (const auto formal = contractPlace(context, *place);
          formal && formal->field != 0)
        return {.known = true, .fields = {*formal}};
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
      // A payload enum's text payload holds only text that its construction
      // proved ownerless, as a class's text field does, unless the subject
      // carries a dependency designating this payload (L-OWN-02).
      const auto designated =
          instruction->operands.size() == 1
              ? dependentDesignation(context.body,
                                     instruction->operands.front())
              : std::nullopt;
      const MirPayloadEnum *enumeration =
          designated
              ? program.findPayloadEnum(instruction->operands.front().type)
              : nullptr;
      const MirPayloadEnumVariant *variant =
          enumeration && instruction->enumVariant &&
                  *instruction->enumVariant < enumeration->variants.size()
              ? &enumeration->variants[*instruction->enumVariant]
              : nullptr;
      const bool dependent =
          designated &&
          (variant == nullptr || !instruction->payloadIndex ||
           *instruction->payloadIndex >= variant->payloadSymbols.size() ||
           variant->payloadSymbols[*instruction->payloadIndex] == *designated);
      answer.known = !dependent;
    } else if (instruction && instruction->operands.size() == 1 &&
               (instruction->kind == MirInstructionKind::Load ||
                instruction->kind == MirInstructionKind::CallInput ||
                instruction->intrinsic == IntrinsicKind::Move)) {
      answer = sources(context, instruction->operands.front(), active);
    } else if (instruction && instruction->kind == MirInstructionKind::Call &&
               instruction->functionTarget) {
      const auto *target =
          program.findFunctionInstance(*instruction->functionTarget);
      if (target && textType(target->returnType))
        answer = callResultSources(context, *instruction, active);
    } else if (!instruction) {
      answer = joinSources(context, operand.value, active);
    }
    active.erase({true, operand.value});
    return answer;
  }

  // The text a source call returns, or that its returned reference
  // designates, in terms of this call's arguments and receiver. Under its
  // field contracts a callee may return a formal's or the receiver's text
  // field (L-OWN-02): the result holds what that argument or receiver holds
  // there.
  TextSources
  callResultSources(const TextBody &context, const MirInstruction &instruction,
                    std::set<std::pair<bool, std::size_t>> &active) {
    const auto *target =
        instruction.functionTarget
            ? program.findFunctionInstance(*instruction.functionTarget)
            : nullptr;
    if (!target || target->definitionKind != MirDefinitionKind::Source)
      return {};
    const auto returned = returnSources(*target);
    TextSources answer{.known = returned.known};
    for (const auto parameter : returned.parameters) {
      if (parameter >= instruction.operands.size()) {
        answer.known = false;
        continue;
      }
      answer.join(sources(context, instruction.operands[parameter], active));
    }
    for (const auto &field : returned.fields) {
      const MirOperand *origin =
          field.receiver
              ? (instruction.receiver ? &*instruction.receiver : nullptr)
          : field.parameter < instruction.operands.size()
              ? &instruction.operands[field.parameter]
              : nullptr;
      if (origin == nullptr) {
        answer.known = false;
        continue;
      }
      answer.join(heldFieldSources(context, *origin, field.field));
    }
    return answer;
  }

  // The source call whose returned reference to text `loan` holds.
  const MirInstruction *textReferenceCall(const MirBody &body,
                                          MirLoanId loan) const {
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Call ||
            instruction.loan != loan || !instruction.functionTarget)
          continue;
        const auto *target =
            program.findFunctionInstance(*instruction.functionTarget);
        if (target == nullptr)
          return nullptr;
        const auto &type = target->returnType;
        return type.kind == SemanticType::Reference &&
                       type.arguments.size() == 1 &&
                       textType(type.arguments.front())
                   ? &instruction
                   : nullptr;
      }
    return nullptr;
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
            answer.fields.insert(source.fields.begin(), source.fields.end());
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
      answer.fields.insert(source.fields.begin(), source.fields.end());
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

  // What a source function's text result may hold, in terms of its own
  // formals and their text fields: the least fixpoint over the returns of
  // each set of mutually recursive functions. A call to a function whose
  // returns are being evaluated reads its current summary through the
  // call's arguments, so a recursive call that permutes formals reaches every
  // formal that some depth of the recursion returns. The first function of a
  // recursive set to be evaluated evaluates its returns again, together with
  // one evaluation of each other member, until no summary of the set grows,
  // and then completes them all.
  TextSources returnSources(const MirFunctionInstance &target) {
    const auto index =
        static_cast<std::size_t>(&target - program.functionInstances().data());
    auto &summary = returnSummaries[index];
    if (summary.complete)
      return summary.sources;
    if (summary.depth != 0 || summary.provisional) {
      readDepth = std::min(readDepth, summary.depth != 0 ? summary.depth
                                                         : summary.lowestRead);
      return summary.sources;
    }
    // The summary depends on the target alone, not on the body that first
    // needed it.
    auto temporaries = std::exchange(activeTemporaries, {});
    const auto outerReadDepth = readDepth;
    const bool outerGrew = provisionalGrew;
    summary.depth = ++returnDepth;
    const TextBody callee{.body = target.body,
                          .types = target.parameterTypes,
                          .bindings = target.parameterBindings,
                          .fieldContracts =
                              fieldContractFunction(program, target) ? &target
                                                                     : nullptr};
    for (;;) {
      const auto mark = provisionalSummaries.size();
      readDepth = std::numeric_limits<std::size_t>::max();
      provisionalGrew = false;
      auto next = summary.sources;
      bool sawReturn = false;
      for (const auto &block : target.body.blocks)
        if (block.terminator.kind == MirTerminatorKind::Return &&
            block.terminator.value) {
          sawReturn = true;
          next.join(sources(callee, *block.terminator.value));
        }
      next.known = next.known && sawReturn;
      const bool grew = next != summary.sources;
      summary.sources = std::move(next);
      if (readDepth < summary.depth) {
        // A lower evaluation completes this summary, and those computed
        // here, which now depend on what this one depends on.
        for (auto entry = mark; entry < provisionalSummaries.size(); ++entry)
          returnSummaries[provisionalSummaries[entry]].lowestRead = readDepth;
        summary.provisional = true;
        summary.lowestRead = readDepth;
        provisionalSummaries.push_back(index);
        provisionalGrew = outerGrew || provisionalGrew || grew;
        break;
      }
      // Each provisional summary computed in this pass depends on this one:
      // a pass in which none grew completes them all.
      const bool again = grew || provisionalGrew;
      for (auto entry = mark; entry < provisionalSummaries.size(); ++entry) {
        auto &member = returnSummaries[provisionalSummaries[entry]];
        member.provisional = false;
        member.complete = !again;
      }
      provisionalSummaries.resize(mark);
      if (!again) {
        summary.complete = true;
        provisionalGrew = outerGrew;
        break;
      }
    }
    --returnDepth;
    summary.depth = 0;
    readDepth = std::min(outerReadDepth, readDepth);
    activeTemporaries = std::move(temporaries);
    return summary.sources;
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
                             bool diagnose,
                             std::set<OwnerlessTextField> *fieldNeeds = nullptr,
                             std::set<PublishedTextField> *relayed = nullptr) {
    std::set<std::size_t> needed;
    std::set<OwnerlessTextField> ignoredFields;
    std::set<PublishedTextField> ignoredRelays;
    auto &neededFields = fieldNeeds ? *fieldNeeds : ignoredFields;
    auto &relayedFormals = relayed ? *relayed : ignoredRelays;
    const auto require = [&](const MirInstruction &instruction,
                             const MirOperand &operand) {
      const auto source = sources(context, operand);
      if (!source.known && diagnose)
        fail(context, owner, &instruction,
             "ownerless text storage or call lacks exact producer evidence");
      needed.insert(source.parameters.begin(), source.parameters.end());
      neededFields.insert(source.fields.begin(), source.fields.end());
    };
    // A whole formal or receiver value: every text field may outlive the call.
    const auto wholeUse = [&](const MirOperand &operand) {
      const auto *loan = operand.kind == MirOperandKind::Loan
                             ? context.body.findLoan(operand.loan)
                             : nullptr;
      const auto *place =
          context.body.findPlace(loan ? loan->source : operand.place);
      const auto formal = place ? contractPlace(context, *place) : std::nullopt;
      return formal && formal->field == 0 ? formal : std::nullopt;
    };
    const auto requireAllFields = [&](const OwnerlessTextField &formal) {
      for (const SymbolId field : formalTextFields(context, formal))
        neededFields.insert({.receiver = formal.receiver,
                             .parameter = formal.parameter,
                             .field = field});
    };
    // The call that consumes a CallInput's result, and the callee formal or
    // receiver it binds.
    const auto consumer = [&](const MirBlock &block,
                              const MirInstruction &input)
        -> std::pair<const MirInstruction *, OwnerlessTextField> {
      if (input.kind != MirInstructionKind::CallInput || !input.result)
        return {nullptr, {}};
      for (const auto &candidate : block.instructions) {
        if (candidate.kind != MirInstructionKind::Call &&
            candidate.kind != MirInstructionKind::Construct)
          continue;
        if (candidate.receiver &&
            candidate.receiver->kind == MirOperandKind::Value &&
            candidate.receiver->value == *input.result)
          return {&candidate, {.receiver = true}};
        for (std::size_t index = 0; index < candidate.operands.size(); ++index)
          if (candidate.operands[index].kind == MirOperandKind::Value &&
              candidate.operands[index].value == *input.result)
            return {&candidate, {.parameter = index}};
      }
      return {nullptr, {}};
    };
    // The callee a formal is forwarded into, when it carries field contracts
    // and takes the same class there.
    const auto forwardTarget =
        [&](const MirInstruction *call, const OwnerlessTextField &calleeFormal,
            const SemanticType &type) -> const MirFunctionInstance * {
      const auto *target =
          call && call->kind == MirInstructionKind::Call && call->functionTarget
              ? program.findFunctionInstance(*call->functionTarget)
              : nullptr;
      if (!target || !fieldContractFunction(program, *target) ||
          !readOnlyContractFormal(*target, calleeFormal))
        return nullptr;
      SemanticType formal = SemanticType::Unknown;
      if (calleeFormal.receiver) {
        const auto *instance =
            target->owner ? program.findClassInstance(*target->owner) : nullptr;
        if (instance && !target->staticMember)
          formal = instance->type;
      } else if (calleeFormal.parameter < target->parameterTypes.size()) {
        formal = target->parameterTypes[calleeFormal.parameter];
        if (formal.kind == SemanticType::Reference &&
            formal.arguments.size() == 1)
          formal = SemanticType(formal.arguments.front());
      }
      return formal == type ? target : nullptr;
    };
    const auto formalType = [&](const OwnerlessTextField &formal) {
      if (formal.receiver) {
        const auto &owner = context.fieldContracts->owner;
        const auto *instance =
            owner ? program.findClassInstance(*owner) : nullptr;
        return instance ? instance->type : SemanticType(SemanticType::Unknown);
      }
      SemanticType type = formal.parameter < context.types.size()
                              ? context.types[formal.parameter]
                              : SemanticType(SemanticType::Unknown);
      if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
        type = SemanticType(type.arguments.front());
      return type;
    };
    for (const auto &block : context.body.blocks) {
      for (const auto &instruction : block.instructions) {
        // Whole-value uses of a formal or the receiver, and forwarding.
        if (context.fieldContracts) {
          const auto visit = [&](const MirOperand &operand) {
            const auto formal = wholeUse(operand);
            if (!formal)
              return;
            const auto [call, calleeFormal] = consumer(block, instruction);
            const auto *target =
                forwardTarget(call, calleeFormal, formalType(*formal));
            if (!target) {
              requireAllFields(*formal);
              return;
            }
            for (const auto &field : fieldRequirements[target->id - 1])
              if (field.receiver == calleeFormal.receiver &&
                  (field.receiver || field.parameter == calleeFormal.parameter))
                neededFields.insert({.receiver = formal->receiver,
                                     .parameter = formal->parameter,
                                     .field = field.field});
          };
          for (const auto &operand : instruction.operands)
            visit(operand);
          if (instruction.receiver)
            visit(*instruction.receiver);
        }
        // A dependent value relayed as a formal or receiver: its callee
        // must carry a contract that lets none of its fields outlive the
        // call.
        if (diagnose && instruction.kind == MirInstructionKind::CallInput &&
            instruction.operands.size() == 1)
          if (const auto designated = dependentDesignation(
                  context.body, instruction.operands.front())) {
            const auto [call, calleeFormal] = consumer(block, instruction);
            const auto *target =
                call && call->kind == MirInstructionKind::Call &&
                        call->functionTarget
                    ? program.findFunctionInstance(*call->functionTarget)
                    : nullptr;
            const OwnerlessTextField field{.receiver = calleeFormal.receiver,
                                           .parameter = calleeFormal.parameter,
                                           .field = *designated};
            if (!target || !fieldContractFunction(program, *target) ||
                !readOnlyContractFormal(*target, calleeFormal) ||
                std::binary_search(target->ownerlessTextFields.begin(),
                                   target->ownerlessTextFields.end(), field))
              fail(context, owner, &instruction,
                   "dependent text value is relayed where its text field may "
                   "outlive the call");
          }
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
            // This function's returned fresh value relays the formal its
            // published text field stores (L-OWN-02): the caller's argument
            // is published there at each call instead.
            if (instruction.kind == MirInstructionKind::Construct &&
                index < instruction.operands.size() &&
                returnedFreshValue(context, instruction)) {
              const auto &fields =
                  constructorTextFields[*instruction.constructorTarget - 1];
              const auto field = fields.find(index);
              const auto formal =
                  exactTextFormal(context, instruction.operands[index]);
              if (field != fields.end() && formal &&
                  directTextField(program, instruction.info.type,
                                  field->second)) {
                relayedFormals.insert(
                    {.parameter = *formal, .field = field->second});
                continue;
              }
            }
            if (index < instruction.operands.size())
              require(instruction, instruction.operands[index]);
            else if (diagnose)
              fail(context, owner, &instruction,
                   "ownerless text argument is missing");
          }
        // A callee that returns an argument in a text field of its fresh
        // result (L-OWN-02): a dynamic argument is published under the call's
        // own loan, a returned call relays this function's formal, and any
        // other formal or unknown text there must be ownerless.
        // Semantics saw this callee's relays before they were final
        // (recursion): every text argument must be ownerless instead.
        if (instruction.kind == MirInstructionKind::Call &&
            instruction.provisionalTextRelays) {
          for (std::size_t index = 0; index < instruction.operands.size() &&
                                      index < instruction.parameterTypes.size();
               ++index)
            if (textType(instruction.parameterTypes[index]))
              require(instruction, instruction.operands[index]);
        } else if (instruction.kind == MirInstructionKind::Call &&
                   instruction.functionTarget &&
                   *instruction.functionTarget != 0 &&
                   *instruction.functionTarget <= relays.size())
          for (const auto &entry : relays[*instruction.functionTarget - 1]) {
            if (entry.parameter >= instruction.operands.size() ||
                !directTextField(program, instruction.info.type, entry.field) ||
                publishesCallText(context, instruction, entry))
              continue;
            const auto &operand = instruction.operands[entry.parameter];
            if (returnedFreshValue(context, instruction))
              if (const auto formal = exactTextFormal(context, operand)) {
                relayedFormals.insert(
                    {.parameter = *formal, .field = entry.field});
                continue;
              }
            require(instruction, operand);
          }
        if (instruction.kind == MirInstructionKind::Compute &&
            instruction.operation == MirOperation::PayloadConstruct &&
            instruction.enumVariant) {
          const auto *enumeration =
              program.findPayloadEnum(instruction.info.type);
          if (enumeration &&
              *instruction.enumVariant < enumeration->variants.size()) {
            const auto &variant =
                enumeration->variants[*instruction.enumVariant];
            const auto &fields = variant.payloadTypes;
            // A payload this construction publishes under its own read-only
            // Stored loan holds that dependent text (L-OWN-02); the loan,
            // not an ownerless proof, keeps it valid.
            const auto *loan =
                instruction.dependentTextPublication && instruction.loan
                    ? context.body.findLoan(*instruction.loan)
                    : nullptr;
            const auto published = [&](std::size_t index) {
              return loan != nullptr && loan->kind == MirLoanKind::Stored &&
                     loan->access == AccessMode::ReadOnly &&
                     loan->producedBy == instruction.hirValue &&
                     index < variant.payloadSymbols.size() &&
                     loan->designatedFields ==
                         std::vector<SymbolId>{variant.payloadSymbols[index]};
            };
            for (std::size_t index = 0;
                 index < fields.size() && index < instruction.operands.size();
                 ++index)
              if (textType(fields[index]) && !published(index))
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
      if (!context.fieldContracts)
        continue;
      // A text return without a borrow summary is ownerless storage for the
      // caller; any other whole formal on an edge escapes the call too.
      const auto &terminator = block.terminator;
      const bool ownerlessReturn =
          terminator.kind == MirTerminatorKind::Return && terminator.value &&
          textType(context.fieldContracts->returnType) &&
          context.fieldContracts->returnBorrowOrigin == BorrowOriginKind::None;
      if (ownerlessReturn) {
        const auto source = sources(context, *terminator.value);
        neededFields.insert(source.fields.begin(), source.fields.end());
      }
      const auto edge = [&](const MirOperand &operand) {
        if (const auto formal = wholeUse(operand))
          requireAllFields(*formal);
      };
      if (terminator.value)
        edge(*terminator.value);
      for (const auto &operand : terminator.targetArguments)
        edge(operand);
      for (const auto &operand : terminator.elseArguments)
        edge(operand);
      for (const auto &target : terminator.switchTargets)
        for (const auto &operand : target.arguments)
          edge(operand);
    }
    return needed;
  }
};

auto TextContracts::initializerTextInputs(
    const MirConstructorInstance &constructor, bool diagnose,
    const std::map<SymbolId, std::size_t> *published) -> InitializerTextInputs {
  const TextBody context{constructor.body, constructor.parameterTypes,
                         constructor.parameterBindings, published};
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
  fieldRequirements.resize(program.functionInstances().size());
  constructorRequirements.resize(program.constructorInstances().size());
  constructorTextFields.resize(program.constructorInstances().size());
  constructorPublishedFields.resize(program.constructorInstances().size());
  for (const auto &constructor : program.constructorInstances()) {
    auto inputs = initializerTextInputs(constructor, true);
    auto &published = constructorPublishedFields[constructor.id - 1];
    for (const auto &[formal, field] : inputs.fields)
      published.emplace(field, formal);
    // Another initializer reading a published field through `this` stores
    // that formal's text too, so the field is no longer exclusive.
    if (!published.empty())
      inputs = initializerTextInputs(constructor, false, &published);
    constructorRequirements[constructor.id - 1] =
        std::move(inputs.requirements);
    constructorTextFields[constructor.id - 1] = std::move(inputs.fields);
  }
  relays.resize(program.functionInstances().size());
  const auto functionContext = [&](const MirFunctionInstance &function) {
    return TextBody{
        .body = function.body,
        .types = function.parameterTypes,
        .bindings = function.parameterBindings,
        .fieldContracts =
            fieldContractFunction(program, function) ? &function : nullptr,
        .function = function.definitionKind == MirDefinitionKind::Source &&
                            function.linkage == LanguageLinkage::Gti
                        ? &function
                        : nullptr};
  };
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &function : program.functionInstances()) {
      std::set<OwnerlessTextField> fields;
      std::set<PublishedTextField> relayed;
      const auto required = scan(functionContext(function), function.id, false,
                                 &fields, &relayed);
      auto &known = requirements[function.id - 1];
      auto &knownFields = fieldRequirements[function.id - 1];
      auto &knownRelays = relays[function.id - 1];
      const auto before = known.size();
      const auto beforeFields = knownFields.size();
      const auto beforeRelays = knownRelays.size();
      known.insert(required.begin(), required.end());
      knownFields.insert(fields.begin(), fields.end());
      knownRelays.insert(relayed.begin(), relayed.end());
      changed = changed || before != known.size() ||
                beforeFields != knownFields.size() ||
                beforeRelays != knownRelays.size();
    }
  }
  for (const auto &function : program.functionInstances()) {
    const TextBody context = functionContext(function);
    (void)scan(context, function.id, true);
    // A function's relays are what its returns derive (L-OWN-02); a caller
    // publishes nothing a callee does not derive, and a derived relay must be
    // declared or the caller would treat that text as ownerless.
    if (!std::equal(relays[function.id - 1].begin(),
                    relays[function.id - 1].end(),
                    function.publishedTextFields.begin(),
                    function.publishedTextFields.end()))
      fail(context, function.id, nullptr,
           "published text result contract differs from independently "
           "derived relays");
    // Every independently derived field requirement must be declared; a
    // declared one MIR does not derive only rejects more relays.
    const auto &declaredFields = function.ownerlessTextFields;
    if (!context.fieldContracts && !declaredFields.empty())
      fail(context, function.id, nullptr,
           "a field-level ownerless text contract is declared on a function "
           "that cannot carry one");
    for (const auto &field : fieldRequirements[function.id - 1])
      if (!std::binary_search(declaredFields.begin(), declaredFields.end(),
                              field)) {
        fail(context, function.id, nullptr,
             "ownerless text field contract omits an independently derived "
             "requirement");
        break;
      }
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
    // A text field result (L-OWN-02) relays one formal's or the receiver's
    // field: each return reads exactly that field, or ownerless text.
    if (function.definitionKind == MirDefinitionKind::Source &&
        textFieldResult(function.returnType, function.returnBorrowOrigin,
                        function.returnBorrowParameter,
                        function.returnBorrowFields)) {
      const bool receiver =
          function.returnBorrowOrigin == BorrowOriginKind::Receiver;
      const OwnerlessTextField declared{
          .receiver = receiver,
          .parameter = receiver ? 0 : function.returnBorrowParameter,
          .field = function.returnBorrowFields.front().sourceFields.front()};
      if (!context.fieldContracts)
        fail(context, function.id, nullptr,
             "a text field result is declared on a function that cannot "
             "carry field contracts");
      else
        for (const auto &block : function.body.blocks)
          if (block.terminator.kind == MirTerminatorKind::Return &&
              block.terminator.value) {
            const auto returned = sources(context, *block.terminator.value);
            if (!returned.known || !returned.parameters.empty() ||
                std::any_of(
                    returned.fields.begin(), returned.fields.end(),
                    [&](const auto &field) { return field != declared; }))
              fail(context, function.id, nullptr,
                   "text field result returns other text than its declared "
                   "field");
          }
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
                           constructor.parameterBindings,
                           &constructorPublishedFields[constructor.id - 1]};
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
  // A temporary's field view may be its value's only place; the value then
  // names the record type.
  if (!current && place.root == MirPlaceRootKind::Value)
    if (const auto *value = body.findValue(place.value))
      current = value->info.type;
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

bool directTextPayload(const MirProgram &program, const SemanticType &type,
                       SymbolId payload) {
  const MirPayloadEnum *enumeration =
      type.kind == SemanticType::Enum && payload != 0
          ? program.findPayloadEnum(type)
          : nullptr;
  if (enumeration == nullptr || enumeration->traits.containsBorrowedState)
    return false;
  for (const auto &variant : enumeration->variants)
    for (std::size_t index = 0; index < variant.payloadSymbols.size() &&
                                index < variant.payloadTypes.size();
         ++index)
      if (variant.payloadSymbols[index] == payload)
        return textType(variant.payloadTypes[index]);
  return false;
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
