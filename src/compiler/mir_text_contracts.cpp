#include "mir_text_contracts.h"

#include "mir_verification_queries.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <tuple>
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

// A text field, or a text payload reached through a payload-enum field.
using TextFieldPath = std::vector<SymbolId>;

// Dynamic text that a value or local binding holds (L-OWN-02): the owner text
// of `loan`, a read-only Stored loan that a publication designates, at `path`
// within the value (empty for a view, and for a reference's referent). A
// relay call's own Stored loan on the same owner may retain it instead.
struct HeldText {
  MirLoanId loan = 0;
  TextFieldPath path;
  std::set<MirLoanId> substitutes;

  friend auto operator<=>(const HeldText &, const HeldText &) = default;
};
using HeldTexts = std::set<HeldText>;

struct TextBody {
  const MirBody &body;
  std::span<const SemanticType> types;
  std::span<const HirBindingId> bindings;
  // A constructor's text field paths that a construction may publish, each
  // mapped to the formal that initializes it (L-OWN-02).
  const std::map<TextFieldPath, std::size_t> *publishedFields = nullptr;
  // A function whose formals and receiver carry field-level contracts.
  const MirFunctionInstance *fieldContracts = nullptr;
  // The function whose body this is, when its returns may relay formals.
  const MirFunctionInstance *function = nullptr;
  // The constructor whose body this is.
  const MirConstructorInstance *constructor = nullptr;
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

// A publishable class with a text path under it: a text field, a text
// payload of a payload-enum field, or such a class in a field or payload,
// which semantics' nestedTextClass names the same way (L-OWN-02 phase 3).
bool nestedTextClass(const MirProgram &program, const SemanticType &type,
                     std::size_t depth = 0) {
  if (depth > 8 || !textPublishableClass(program, type))
    return false;
  const auto instance = std::find_if(
      program.classInstances().begin(), program.classInstances().end(),
      [&](const auto &candidate) { return candidate.type == type; });
  const auto text = [&](const SemanticType &candidate) {
    return textType(candidate) ||
           (candidate.kind == SemanticType::Class &&
            nestedTextClass(program, candidate, depth + 1));
  };
  for (const auto &field : instance->declaredFields) {
    if (text(field.type))
      return true;
    if (const MirPayloadEnum *enumeration = program.findPayloadEnum(field.type);
        enumeration != nullptr && !enumeration->traits.containsBorrowedState)
      for (const auto &variant : enumeration->variants)
        for (const auto &payload : variant.payloadTypes)
          if (text(payload))
            return true;
  }
  return false;
}

// Whether a function's formal or receiver is read-only: a read-only
// receiver, a read-only reference, or a by-value formal whose binding place
// is read-only. Only such a formal takes a relayed value.
bool readOnlyContractFormal(const MirFunctionInstance &function,
                            const OwnerlessTextField &formal) {
  // A consuming receiver owns the caller's value: a write into its text field
  // stores only ownerless text, as for any text field.
  if (formal.receiver)
    return function.receiverMutability == ReceiverMutability::ReadOnly ||
           function.receiverMutability == ReceiverMutability::Consuming;
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
  // require, and the formals that alone initialize one direct text field or
  // one text payload of a payload-enum field each, keyed to that path.
  struct InitializerTextInputs {
    std::set<std::size_t> requirements;
    std::map<std::size_t, TextFieldPath> fields;
    // A class formal's text field that alone fills one text field path, or,
    // keyed by field 0, a class formal stored whole at one path.
    std::map<std::pair<std::size_t, SymbolId>, TextFieldPath> fieldRelays;
    // The instructions passing a formal stored whole to its initializer:
    // that use is the relay itself, not a use letting its text escape.
    std::set<MirInstructionId> wholeRelayProducers;
  };
  InitializerTextInputs initializerTextInputs(
      const MirConstructorInstance &constructor, bool diagnose,
      const std::map<TextFieldPath, std::size_t> *published = nullptr);

private:
  const MirProgram &program;
  MirVerificationResult result;
  std::vector<std::set<std::size_t>> requirements;
  // Derived per function: the formal and receiver text fields it lets
  // outlive the call (L-OWN-02).
  std::vector<std::set<OwnerlessTextField>> fieldRequirements;
  std::vector<std::set<std::size_t>> constructorRequirements;
  // Per constructor: the formals that initialize exactly one text field path
  // and reach no other text initializer, keyed to that path.
  std::vector<std::map<std::size_t, TextFieldPath>> constructorTextFields;
  // Per constructor: each publishable text field path and the formal that
  // initializes it, before excluding fields read back through `this`.
  std::vector<std::map<TextFieldPath, std::size_t>> constructorPublishedFields;
  // Formals whose text a construction published under its result's loan; the
  // constructor body must not also store them as ownerless text.
  std::set<std::pair<std::size_t, std::size_t>> publishedFormals;
  // Per constructor: a class formal's text field that alone fills a text
  // field path, which a construction re-roots its argument's owner at.
  std::vector<std::map<std::pair<std::size_t, SymbolId>, TextFieldPath>>
      constructorFieldRelays;
  std::vector<std::set<MirInstructionId>> constructorRelayProducers;
  // (constructor, formal, field) re-rooted under some construction's loan;
  // the body must not also store the field as ownerless text.
  std::set<std::tuple<std::size_t, std::size_t, SymbolId>> publishedFieldRelays;
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
    const auto index = exactFormal(context, operand);
    return index && textType(context.types[*index]) ? index : std::nullopt;
  }

  // The class formal that `operand` passes on whole: an exact load or move
  // of its binding, possibly staged as a call input.
  std::optional<std::size_t> exactClassFormal(const TextBody &context,
                                              const MirOperand &operand) const {
    const auto index = exactFormal(context, operand);
    return index && context.types[*index].kind == SemanticType::Class
               ? index
               : std::nullopt;
  }

  std::optional<std::size_t> exactFormal(const TextBody &context,
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
    if (found == context.bindings.end() || index >= context.types.size())
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

  // A call that publishes a dependent class argument, which its callee
  // returns whole in a field of its fresh result, nests the argument's path
  // `held` under that field in its own read-only Stored loan (L-OWN-02
  // phase 3).
  bool publishesWholeCallText(const TextBody &context,
                              const MirInstruction &call, std::size_t parameter,
                              std::span<const SymbolId> held) const {
    const auto *target =
        call.kind == MirInstructionKind::Call && call.functionTarget
            ? program.findFunctionInstance(*call.functionTarget)
            : nullptr;
    const auto *loan = call.loan ? context.body.findLoan(*call.loan) : nullptr;
    if (target == nullptr || !call.dependentTextPublication || !loan ||
        held.empty() || parameter >= target->parameterTypes.size() ||
        target->parameterTypes[parameter].kind != SemanticType::Class ||
        loan->kind != MirLoanKind::Stored ||
        loan->access != AccessMode::ReadOnly ||
        !loan->additionalDependencies.empty() ||
        loan->designatedFields.size() != held.size() + 1 ||
        !std::equal(held.begin(), held.end(),
                    loan->designatedFields.begin() + 1) ||
        !std::binary_search(
            target->publishedTextFields.begin(),
            target->publishedTextFields.end(),
            PublishedTextField{.parameter = parameter,
                               .field = loan->designatedFields.front()}))
      return false;
    const BorrowedFieldOrigin expected{
        .resultFields = loan->designatedFields,
        .origin = BorrowOriginKind::Argument,
        .parameter = parameter,
        .sourceFields = TextFieldPath(held.begin(), held.end()),
        .access = AccessMode::ReadOnly};
    return std::find(call.borrowFields.begin(), call.borrowFields.end(),
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
  std::optional<OwnerlessTextField>
  contractPlace(const TextBody &context, const MirPlace &place,
                bool payloadField = false) const {
    // A constructor's by-value class formal holds the caller's value: its
    // text fields hold what the argument held (L-OWN-02), and a write into
    // one stores only ownerless text.
    const bool constructorFormal =
        !context.fieldContracts && context.constructor != nullptr &&
        place.root == MirPlaceRootKind::Binding && place.capture == 0;
    if (!context.fieldContracts && !constructorFormal)
      return std::nullopt;
    OwnerlessTextField formal;
    SemanticType type = SemanticType::Unknown;
    std::size_t offset = 0;
    if (constructorFormal) {
      const auto found = std::find(context.bindings.begin(),
                                   context.bindings.end(), place.binding);
      formal.parameter =
          static_cast<std::size_t>(found - context.bindings.begin());
      if (found == context.bindings.end() ||
          formal.parameter >= context.types.size() ||
          context.types[formal.parameter].kind != SemanticType::Class)
        return std::nullopt;
      type = context.types[formal.parameter];
    } else if (place.root == MirPlaceRootKind::This) {
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
        (!constructorFormal &&
         !readOnlyContractFormal(*context.fieldContracts, formal)))
      return std::nullopt;
    if (place.projections.size() == offset)
      return formal;
    if (place.projections.size() == offset + 1 &&
        place.projections[offset].kind == MirProjectionKind::Field &&
        (payloadField ? payloadEnumField(type, place.projections[offset].field)
                      : directTextField(program, type,
                                        place.projections[offset].field))) {
      formal.field = place.projections[offset].field;
      return formal;
    }
    return std::nullopt;
  }

  // Whether `field` is a payload-enum field of the class `type`.
  bool payloadEnumField(const SemanticType &type, SymbolId field) const {
    const auto instance = std::find_if(
        program.classInstances().begin(), program.classInstances().end(),
        [&](const auto &candidate) { return candidate.type == type; });
    return instance != program.classInstances().end() &&
           std::any_of(instance->declaredFields.begin(),
                       instance->declaredFields.end(), [&](const auto &entry) {
                         return entry.symbol == field &&
                                program.findPayloadEnum(entry.type) != nullptr;
                       });
  }

  // The payload requirement a payload extraction from a contract formal's
  // or the receiver's payload-enum field places on its text payload: binding
  // the payload requires it ownerless at each call.
  std::optional<OwnerlessTextField>
  contractPayloadExtraction(const TextBody &context,
                            const MirInstruction &instruction) const {
    if (instruction.kind != MirInstructionKind::Compute ||
        instruction.operation != MirOperation::PayloadExtract ||
        instruction.operands.size() != 1 || !instruction.enumVariant ||
        !instruction.payloadIndex)
      return std::nullopt;
    const auto &subject = instruction.operands.front();
    const auto *loan = subject.kind == MirOperandKind::Loan
                           ? context.body.findLoan(subject.loan)
                           : nullptr;
    const auto *place =
        subject.kind == MirOperandKind::Value
            ? nullptr
            : context.body.findPlace(loan ? loan->source : subject.place);
    auto formal = place ? contractPlace(context, *place, true) : std::nullopt;
    // The field that a formal's accessor returns a reference to holds what
    // `formal.field` holds.
    if (!formal && place != nullptr) {
      std::vector<const MirFunctionInstance *> visiting;
      const MirPlace *receiver = nullptr;
      const auto field =
          accessorResultField(context.body, *place, visiting, &receiver);
      auto owner =
          field && receiver ? contractPlace(context, *receiver) : std::nullopt;
      if (owner && owner->field == 0) {
        owner->field = *field;
        formal = owner;
      }
    }
    const auto *enumeration = program.findPayloadEnum(subject.type);
    if (!formal || formal->field == 0 || enumeration == nullptr ||
        *instruction.enumVariant >= enumeration->variants.size())
      return std::nullopt;
    const auto &variant = enumeration->variants[*instruction.enumVariant];
    if (*instruction.payloadIndex >= variant.payloadSymbols.size() ||
        !directTextPayload(program, subject.type,
                           variant.payloadSymbols[*instruction.payloadIndex]))
      return std::nullopt;
    formal->payload = variant.payloadSymbols[*instruction.payloadIndex];
    return formal;
  }

  // The direct text fields of a formal's or the receiver's class, and the
  // text payloads of its payload-enum fields.
  std::vector<OwnerlessTextField>
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
    std::vector<OwnerlessTextField> fields;
    const auto instance = std::find_if(
        program.classInstances().begin(), program.classInstances().end(),
        [&](const auto &candidate) { return candidate.type == type; });
    if (instance == program.classInstances().end())
      return fields;
    for (const auto &field : instance->declaredFields) {
      if (textType(field.type))
        fields.push_back({.receiver = formal.receiver,
                          .parameter = formal.parameter,
                          .field = field.symbol});
      if (instance->traits.containsBorrowedState)
        continue;
      if (const auto *enumeration = program.findPayloadEnum(field.type))
        for (const auto &variant : enumeration->variants)
          for (const SymbolId payload : variant.payloadSymbols)
            if (directTextPayload(program, field.type, payload))
              fields.push_back({.receiver = formal.receiver,
                                .parameter = formal.parameter,
                                .field = field.symbol,
                                .payload = payload});
    }
    return fields;
  }

  // The text path that a dependent carrier operand's Stored loan designates
  // within the operand: from a binding the loan carries, or a field of it
  // that the path goes through, or from the construction result it produced.
  std::optional<TextFieldPath>
  dependentDesignationPath(const MirBody &body,
                           const MirOperand &operand) const {
    // A moved or loaded carrier designates what its place does.
    if (operand.kind == MirOperandKind::Value)
      if (const auto *producer = definition(body, operand.value);
          producer != nullptr && producer->operands.size() == 1 &&
          (producer->kind == MirInstructionKind::Move ||
           producer->kind == MirInstructionKind::Load) &&
          producer->operands.front().kind != MirOperandKind::Value)
        if (auto path =
                dependentDesignationPath(body, producer->operands.front()))
          return path;
    const auto *loan = operand.kind == MirOperandKind::Loan
                           ? body.findLoan(operand.loan)
                           : nullptr;
    // A reference to the text an accessor returned designates that view,
    // not a field path of the accessor's owner, which is the loan's source.
    // A view has no fields to relay; its owner is its loan's stored
    // dependency.
    const SemanticType &referent =
        operand.type.kind == SemanticType::Reference &&
                operand.type.arguments.size() == 1
            ? operand.type.arguments.front()
            : operand.type;
    if (loan != nullptr && loan->kind == MirLoanKind::CallResult &&
        (referent.kind == SemanticType::StringView ||
         referent.kind == SemanticType::CString))
      return std::nullopt;
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
    const bool fieldPrefix =
        place && place->root == MirPlaceRootKind::Binding &&
        std::all_of(place->projections.begin(), place->projections.end(),
                    [](const auto &projection) {
                      return projection.kind == MirProjectionKind::Field;
                    });
    for (const auto &candidate : body.loans) {
      if (!type || candidate.kind != MirLoanKind::Stored)
        continue;
      std::size_t prefix = 0;
      if (fieldPrefix &&
          std::find(candidate.carriers.begin(), candidate.carriers.end(),
                    place->binding) != candidate.carriers.end() &&
          place->projections.size() < candidate.designatedFields.size() &&
          std::equal(place->projections.begin(), place->projections.end(),
                     candidate.designatedFields.begin(),
                     [](const auto &projection, SymbolId field) {
                       return projection.field == field;
                     }))
        prefix = place->projections.size();
      else if (!(value && value->sourceValue == candidate.producedBy) &&
               !(fieldPrefix && place->projections.empty() &&
                 std::find(candidate.carriers.begin(), candidate.carriers.end(),
                           place->binding) != candidate.carriers.end()))
        continue;
      const TextFieldPath tail(candidate.designatedFields.begin() +
                                   static_cast<std::ptrdiff_t>(prefix),
                               candidate.designatedFields.end());
      if ((tail.size() == 1 &&
           (directTextField(program, *type, tail.front()) ||
            directTextPayload(program, *type, tail.front()))) ||
          (tail.size() == 2 && directTextPath(program, *type, tail)))
        return tail;
    }
    return std::nullopt;
  }

  // The direct text field or payload that it designates.
  std::optional<SymbolId>
  dependentDesignation(const MirBody &body, const MirOperand &operand) const {
    const auto path = dependentDesignationPath(body, operand);
    return path && path->size() == 1 ? std::optional{path->front()}
                                     : std::nullopt;
  }

  // What a call's argument or receiver holds in one of its direct text
  // fields: that field of a formal or the receiver here, unknown text where a
  // dependent carrier designates the field, and ownerless text otherwise.
  TextSources heldFieldSources(const TextBody &context,
                               const MirOperand &operand, SymbolId field,
                               SymbolId payload = 0) const {
    // Follow a staged input, and a load or move of a place, to that place.
    const MirOperand *held = &operand;
    for (std::size_t depth = 0;
         depth < 3 && held->kind == MirOperandKind::Value; ++depth) {
      const auto *input = definition(context.body, held->value);
      if (input == nullptr || input->operands.size() != 1 ||
          (input->kind != MirInstructionKind::CallInput &&
           input->kind != MirInstructionKind::Load &&
           input->kind != MirInstructionKind::Move))
        break;
      held = &input->operands.front();
    }
    const TextFieldPath path =
        payload == 0 ? TextFieldPath{field} : TextFieldPath{field, payload};
    // A carrier designating a path through `path` holds a relayed owner's
    // text under it, as one designating `path` itself does.
    if (const auto designated = dependentDesignationPath(context.body, *held))
      return designated->size() >= path.size() &&
                     std::equal(path.begin(), path.end(), designated->begin())
                 ? TextSources{}
                 : TextSources{.known = true};
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
                          .field = field,
                          .payload = payload}}};
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
      // A reference that a call returned designates what the callee's
      // returned reference does, whatever place its loan is rooted at; so
      // does a returned reborrow of it.
      if (loan != nullptr) {
        if (const auto *call = textReferenceCall(context.body, loan->id))
          return callResultSources(context, *call, active);
        if (const auto *forwarded = forwardedLoan(context.body, loan->id))
          if (const auto *call = textReferenceCall(context.body, *forwarded))
            return callResultSources(context, *call, active);
      }
      const auto *place =
          context.body.findPlace(loan ? loan->source : operand.place);
      if (!place || !textType(place->type))
        return {};
      // Text read under a nested text class that a call returned by
      // reference is what the call's origin holds under the relayed payload.
      if (place->root == MirPlaceRootKind::Loan && !place->projections.empty())
        if (auto relayed = classReferenceSources(context, *place))
          return *relayed;
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
        const auto published = context.publishedFields->find(
            TextFieldPath{place->projections.front().field});
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
      // A contract formal's or the receiver's payload holds what the caller
      // passed there, under the function's field contract.
      if (!dependent)
        if (const auto payload =
                contractPayloadExtraction(context, *instruction))
          answer.fields.insert(*payload);
    } else if (instruction && instruction->operands.size() == 1 &&
               (instruction->kind == MirInstructionKind::Load ||
                instruction->kind == MirInstructionKind::CallInput ||
                instruction->intrinsic == IntrinsicKind::Move)) {
      answer = sources(context, instruction->operands.front(), active);
    } else if (instruction && instruction->kind == MirInstructionKind::Call &&
               instruction->functionTarget) {
      const auto *target =
          program.findFunctionInstance(*instruction->functionTarget);
      // An untracked C-string result at a native C boundary has external
      // validity obligations, not a GTI owner to reconstruct from a body.
      // Keep annotated results on the ordinary dependency path.
      if (target && target->returnType.kind == SemanticType::CString &&
          target->linkage == LanguageLinkage::C &&
          target->definitionKind != MirDefinitionKind::Source &&
          target->returnBorrowOrigin == BorrowOriginKind::None &&
          target->returnBorrowFields.empty() && !target->completeReturnBorrow &&
          !instruction->loan &&
          instruction->borrowOrigin == BorrowOriginKind::None &&
          instruction->borrowFields.empty()) {
        answer.known = true;
      } else if (target && textType(target->returnType)) {
        answer = callResultSources(context, *instruction, active);
      }
    } else if (!instruction) {
      answer = joinSources(context, operand.value, active);
    }
    active.erase({true, operand.value});
    return answer;
  }

  // The instruction that produces `loan`: a Borrow or a Call.
  const MirInstruction *loanProducer(const MirBody &body,
                                     MirLoanId loan) const {
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.loan == loan &&
            (instruction.kind == MirInstructionKind::Borrow ||
             instruction.kind == MirInstructionKind::Call))
          return &instruction;
    return nullptr;
  }

  // Whether `operand`, a call's receiver, is the enclosing method's own
  // receiver.
  bool ownReceiver(const MirBody &body, const MirOperand &operand) const {
    const MirOperand *current = &operand;
    for (std::size_t depth = 0;
         depth < 4 && current->kind == MirOperandKind::Value; ++depth) {
      const auto *producer = definition(body, current->value);
      if (producer == nullptr || producer->operands.size() != 1)
        return false;
      current = &producer->operands.front();
    }
    const auto *loan = current->kind == MirOperandKind::Loan
                           ? body.findLoan(current->loan)
                           : nullptr;
    const auto *place = body.findPlace(loan ? loan->source : current->place);
    return place != nullptr && place->root == MirPlaceRootKind::This &&
           place->projections.empty();
  }

  // The receiver payload that every reference return of `target` to a
  // nested text class designates: a payload reference binding of one of
  // its payload-enum fields, or another such accessor of its own receiver
  // (L-OWN-02 phase 3).
  std::optional<OwnerlessTextField>
  classReferenceRelay(const MirFunctionInstance &target,
                      std::size_t depth = 0) {
    const auto &type = target.returnType;
    if (depth > 4 || type.kind != SemanticType::Reference ||
        type.arguments.size() != 1 ||
        !nestedTextClass(program, type.arguments.front()) ||
        !fieldContractFunction(program, target))
      return std::nullopt;
    const TextBody callee{.body = target.body,
                          .types = target.parameterTypes,
                          .bindings = target.parameterBindings,
                          .fieldContracts = &target};
    std::optional<OwnerlessTextField> result;
    bool returned = false;
    for (const auto &block : target.body.blocks) {
      if (block.terminator.kind != MirTerminatorKind::Return)
        continue;
      returned = true;
      std::optional<OwnerlessTextField> entry;
      std::optional<MirLoanId> loan = block.terminator.returnLoan;
      for (std::size_t step = 0; loan && step < 4 && !entry; ++step) {
        const auto *producer = loanProducer(target.body, *loan);
        if (producer == nullptr)
          break;
        if (producer->kind == MirInstructionKind::Call) {
          const auto *inner =
              producer->functionTarget
                  ? program.findFunctionInstance(*producer->functionTarget)
                  : nullptr;
          if (inner != nullptr && producer->receiver &&
              ownReceiver(target.body, *producer->receiver))
            entry = classReferenceRelay(*inner, depth + 1);
          break;
        }
        const auto &operand = producer->operands.front();
        if (operand.kind == MirOperandKind::Loan) {
          loan = operand.loan;
          continue;
        }
        const auto *place = target.body.findPlace(operand.place);
        const auto *extract = place && place->root == MirPlaceRootKind::Value &&
                                      place->projections.empty()
                                  ? definition(target.body, place->value)
                                  : nullptr;
        if (const auto field = extract
                                   ? contractPayloadExtraction(callee, *extract)
                                   : std::nullopt;
            field && field->receiver)
          entry = field;
        else if (extract != nullptr &&
                 extract->kind == MirInstructionKind::Compute &&
                 extract->operation == MirOperation::PayloadExtract &&
                 extract->operands.size() == 1 && extract->enumVariant &&
                 extract->payloadIndex)
          entry = classPayloadExtraction(callee, *extract);
        break;
      }
      if (!entry || (result && *result != *entry))
        return std::nullopt;
      result = entry;
    }
    return returned ? result : std::nullopt;
  }

  // The contract payload of a nested text class that `instruction`, a
  // payload extraction from a formal's or the receiver's payload-enum field,
  // extracts.
  std::optional<OwnerlessTextField>
  classPayloadExtraction(const TextBody &context,
                         const MirInstruction &instruction) const {
    const auto &subject = instruction.operands.front();
    const auto *loan = subject.kind == MirOperandKind::Loan
                           ? context.body.findLoan(subject.loan)
                           : nullptr;
    const auto *place =
        subject.kind == MirOperandKind::Value
            ? nullptr
            : context.body.findPlace(loan ? loan->source : subject.place);
    auto formal = place ? contractPlace(context, *place, true) : std::nullopt;
    const auto *enumeration = program.findPayloadEnum(subject.type);
    if (!formal || formal->field == 0 || enumeration == nullptr ||
        *instruction.enumVariant >= enumeration->variants.size())
      return std::nullopt;
    const auto &variant = enumeration->variants[*instruction.enumVariant];
    if (*instruction.payloadIndex >= variant.payloadSymbols.size() ||
        *instruction.payloadIndex >= variant.payloadTypes.size() ||
        !nestedTextClass(program,
                         variant.payloadTypes[*instruction.payloadIndex]))
      return std::nullopt;
    formal->payload = variant.payloadSymbols[*instruction.payloadIndex];
    return formal;
  }

  // The contract prefix that `place`, rooted at a call's returned reference
  // to a nested text class, reads under: the call's receiver or argument at
  // the callee's relayed payload.
  std::optional<TextSources> classReferenceSources(const TextBody &context,
                                                   const MirPlace &place) {
    if (place.root != MirPlaceRootKind::Loan)
      return std::nullopt;
    const MirInstruction *call = nullptr;
    std::optional<MirLoanId> loan = place.loan;
    for (std::size_t step = 0; loan && step < 4 && call == nullptr; ++step) {
      const auto *producer = loanProducer(context.body, *loan);
      if (producer == nullptr)
        return std::nullopt;
      if (producer->kind == MirInstructionKind::Call)
        call = producer;
      else if (producer->operands.size() == 1 &&
               producer->operands.front().kind == MirOperandKind::Loan)
        loan = producer->operands.front().loan;
      else
        return std::nullopt;
    }
    const auto *target =
        call != nullptr && call->functionTarget
            ? program.findFunctionInstance(*call->functionTarget)
            : nullptr;
    const auto relay =
        target != nullptr ? classReferenceRelay(*target) : std::nullopt;
    if (!relay)
      return std::nullopt;
    const MirOperand *origin =
        relay->receiver ? (call->receiver ? &*call->receiver : nullptr)
        : relay->parameter < call->operands.size()
            ? &call->operands[relay->parameter]
            : nullptr;
    if (origin == nullptr)
      return TextSources{};
    return heldFieldSources(context, *origin, relay->field, relay->payload);
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
      answer.join(
          heldFieldSources(context, *origin, field.field, field.payload));
    }
    return answer;
  }

  // The loan that a Borrow producing `loan` reborrows, if any.
  const MirLoanId *forwardedLoan(const MirBody &body, MirLoanId loan) const {
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == MirInstructionKind::Borrow &&
            instruction.loan == loan && instruction.operands.size() == 1 &&
            instruction.operands.front().kind == MirOperandKind::Loan)
          return &instruction.operands.front().loan;
    return nullptr;
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

  // The statically dispatched call to a non-virtual source accessor whose
  // returned reference `loan` holds, and that call's receiver operand
  // (through its CallInput staging).
  std::pair<const MirInstruction *, const MirOperand *>
  accessorCall(const MirBody &body, MirLoanId loan) const {
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Call ||
            instruction.loan != loan)
          continue;
        const auto *target =
            instruction.functionTarget
                ? program.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        if (target == nullptr || !instruction.receiver ||
            instruction.dispatch != CallDispatch::Static ||
            target->virtualMethod ||
            target->definitionKind != MirDefinitionKind::Source)
          return {};
        const MirOperand *receiver = &*instruction.receiver;
        if (receiver->kind == MirOperandKind::Value)
          if (const auto *input = definition(body, receiver->value);
              input != nullptr &&
              input->kind == MirInstructionKind::CallInput &&
              input->operands.size() == 1)
            receiver = &input->operands.front();
        return {&instruction, receiver};
      }
    return {};
  }

  // The place `operand` addresses, through the loan holding it.
  const MirPlace *addressedPlace(const MirBody &body,
                                 const MirOperand &operand) const {
    if (operand.kind == MirOperandKind::Value)
      return nullptr;
    const auto *loan = operand.kind == MirOperandKind::Loan
                           ? body.findLoan(operand.loan)
                           : nullptr;
    return body.findPlace(loan ? loan->source : operand.place);
  }

  // The loans an accessor body returns when every return borrows a field of
  // its own receiver, directly or through forwarding: a read-only,
  // non-virtual method returning a reference, as semantics'
  // `accessorReceiverFields` admits. Callers keep the result dependent on
  // every returned field.
  std::unordered_set<MirLoanId> accessorReturnLoans(const TextBody &context) {
    std::unordered_set<MirLoanId> loans;
    const auto *function = context.fieldContracts;
    if (function == nullptr || !function->owner || function->staticMember ||
        function->virtualMethod || function->overrideMethod ||
        function->receiverMutability != ReceiverMutability::ReadOnly ||
        function->returnType.kind != SemanticType::Reference)
      return loans;
    bool designated = false;
    for (const auto &block : context.body.blocks) {
      const auto &terminator = block.terminator;
      if (terminator.kind != MirTerminatorKind::Return)
        continue;
      if (!terminator.value || terminator.value->kind != MirOperandKind::Loan)
        return {};
      std::vector<MirLoanId> chain{terminator.value->loan};
      for (std::size_t depth = 0; depth < context.body.loans.size(); ++depth) {
        const auto *forwarded = forwardedLoan(context.body, chain.back());
        if (forwarded == nullptr)
          break;
        chain.push_back(*forwarded);
      }
      const auto *record = context.body.findLoan(chain.back());
      const auto *source =
          record ? context.body.findPlace(record->source) : nullptr;
      if (source == nullptr || source->root != MirPlaceRootKind::This ||
          source->projections.size() != 1 ||
          source->projections.front().kind != MirProjectionKind::Field)
        return {};
      designated = true;
      loans.insert(chain.begin(), chain.end());
    }
    return designated ? loans : std::unordered_set<MirLoanId>{};
  }

  // The class type of a formal or the receiver.
  SemanticType fieldOwnerType(const TextBody &context,
                              const OwnerlessTextField &formal) const {
    if (formal.receiver) {
      const auto owner = context.fieldContracts
                             ? context.fieldContracts->owner
                             : std::optional<HirClassInstanceId>{};
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
  }

  // The one field of its own receiver that `place`, rooted at an accessor
  // call's returned reference in `body`, designates.
  std::optional<SymbolId>
  accessorResultField(const MirBody &body, const MirPlace &place,
                      std::vector<const MirFunctionInstance *> &visiting,
                      const MirPlace **receiver = nullptr) const {
    const auto fields = accessorResultFields(body, place, visiting, receiver);
    return fields && fields->size() == 1 ? std::optional{fields->front()}
                                         : std::nullopt;
  }

  // The fields of its own receiver that `place`, rooted at an accessor call's
  // returned reference in `body`, designates, sorted: each return of the
  // accessor borrows `this.field`, or the result of another such accessor
  // called on its own receiver.
  std::optional<std::vector<SymbolId>>
  accessorResultFields(const MirBody &body, const MirPlace &place,
                       std::vector<const MirFunctionInstance *> &visiting,
                       const MirPlace **receiver = nullptr) const {
    if (place.root != MirPlaceRootKind::Loan || !place.projections.empty())
      return std::nullopt;
    const auto [call, held] = accessorCall(body, place.loan);
    const auto *self = held ? addressedPlace(body, *held) : nullptr;
    const auto *target =
        call ? program.findFunctionInstance(*call->functionTarget) : nullptr;
    if (self == nullptr || target == nullptr ||
        target->returnType.kind != SemanticType::Reference ||
        std::find(visiting.begin(), visiting.end(), target) != visiting.end())
      return std::nullopt;
    if (receiver != nullptr)
      *receiver = self;
    visiting.push_back(target);
    std::vector<SymbolId> result;
    bool valid = true;
    bool returned = false;
    for (const auto &block : target->body.blocks) {
      const auto &terminator = block.terminator;
      if (terminator.kind != MirTerminatorKind::Return)
        continue;
      returned = true;
      std::optional<std::vector<SymbolId>> fields;
      MirLoanId loan =
          terminator.value && terminator.value->kind == MirOperandKind::Loan
              ? terminator.value->loan
              : 0;
      for (std::size_t depth = 0;
           loan != 0 && depth < target->body.loans.size(); ++depth) {
        const auto *forwarded = forwardedLoan(target->body, loan);
        if (forwarded == nullptr)
          break;
        loan = *forwarded;
      }
      const auto *record = target->body.findLoan(loan);
      const auto *source =
          record ? target->body.findPlace(record->source) : nullptr;
      if (source != nullptr && source->root == MirPlaceRootKind::This &&
          source->projections.size() == 1 &&
          source->projections.front().kind == MirProjectionKind::Field) {
        fields = std::vector{source->projections.front().field};
      } else if (source != nullptr) {
        const MirPlace *inner = nullptr;
        fields = accessorResultFields(target->body, *source, visiting, &inner);
        if (inner == nullptr || inner->root != MirPlaceRootKind::This ||
            !inner->projections.empty())
          fields.reset();
      }
      if (!fields) {
        valid = false;
        break;
      }
      result.insert(result.end(), fields->begin(), fields->end());
    }
    visiting.pop_back();
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return valid && returned ? std::optional{std::move(result)} : std::nullopt;
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

  // A construction whose read-only Stored loan designates the path at which
  // its constructor stores the part of formal `parameter` that the
  // argument's designated path `held` goes through, one of its fields or the
  // whole value, re-roots the argument's dependency there (L-OWN-02).
  bool publishesFieldRelay(const TextBody &context,
                           const MirInstruction &instruction,
                           std::size_t parameter,
                           std::span<const SymbolId> held) {
    if (!instruction.constructorTarget || *instruction.constructorTarget == 0 ||
        *instruction.constructorTarget > constructorFieldRelays.size() ||
        !instruction.dependentTextPublication || !instruction.loan ||
        held.empty())
      return false;
    const auto constructor = *instruction.constructorTarget;
    const auto *loan = context.body.findLoan(*instruction.loan);
    if (!loan || loan->kind != MirLoanKind::Stored ||
        loan->access != AccessMode::ReadOnly ||
        !loan->additionalDependencies.empty())
      return false;
    const auto path = rerootedTextPath(constructor, parameter, held);
    if (!path || loan->designatedFields != path->first ||
        !directTextPath(program, instruction.info.type, path->first))
      return false;
    const BorrowedFieldOrigin expected{
        .resultFields = path->first,
        .origin = BorrowOriginKind::Argument,
        .parameter = parameter,
        .sourceFields = TextFieldPath(held.begin(), held.end()),
        .access = AccessMode::ReadOnly};
    if (std::find(instruction.borrowFields.begin(),
                  instruction.borrowFields.end(),
                  expected) == instruction.borrowFields.end())
      return false;
    publishedFieldRelays.emplace(constructor, parameter, path->second);
    return true;
  }

  // The path at which constructor `constructor` stores the text that formal
  // `parameter` holds at `held`, and the relay key it uses: the field of the
  // formal that `held` starts with, or 0 for the whole formal.
  std::optional<std::pair<TextFieldPath, SymbolId>>
  rerootedTextPath(std::size_t constructor, std::size_t parameter,
                   std::span<const SymbolId> held) const {
    const auto &relays = constructorFieldRelays[constructor - 1];
    for (const SymbolId key : {held.front(), SymbolId{0}}) {
      const auto relay = relays.find({parameter, key});
      if (relay == relays.end())
        continue;
      TextFieldPath path = relay->second;
      path.insert(path.end(), held.begin() + (key == 0 ? 0 : 1), held.end());
      return std::pair{std::move(path), key};
    }
    return std::nullopt;
  }

  // The read-only Stored loan that `instruction` publishes at a direct text
  // field or payload path of its result: a construction, payload
  // construction or call over a local owner's dynamic text (L-OWN-02).
  const MirLoan *textPublicationLoan(const MirBody &body,
                                     const MirInstruction &instruction) const {
    const auto *loan = instruction.loan && instruction.result
                           ? body.findLoan(*instruction.loan)
                           : nullptr;
    if (!loan || loan->kind != MirLoanKind::Stored ||
        loan->access != AccessMode::ReadOnly || loan->designatedFields.empty())
      return nullptr;
    const auto &type = instruction.info.type;
    const auto &path = loan->designatedFields;
    return (type.kind == SemanticType::Enum && path.size() == 1 &&
            directTextPayload(program, type, path.front())) ||
                   (type.kind == SemanticType::Class &&
                    directTextPath(program, type, path))
               ? loan
               : nullptr;
  }

  // Which local bindings hold a published owner's dynamic text (L-OWN-02),
  // derived from each publication through copies, moves, field reads,
  // payload extraction, calls returning a formal's text or text field,
  // borrows and loads through returned references; never from the loans'
  // carrier lists, which semantics chose. Each such binding must carry a
  // loan that keeps the owner borrowed: the publication's loan, a loan whose
  // stored dependency chain reaches it, or the relaying call's own Stored
  // loan on that owner.
  void verifyDependentTextCarriers(const TextBody &context, std::size_t owner) {
    const MirBody &body = context.body;
    std::set<MirLoanId> published;
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions)
        if (const auto *loan = textPublicationLoan(body, instruction))
          published.insert(loan->id);
    if (published.empty())
      return;
    std::map<MirValueId, HeldTexts> values;
    std::map<HirBindingId, HeldTexts> bindings;
    std::map<MirTemporaryId, HeldTexts> temporaries;
    // What the referent of a loan from a Borrow or a reference-returning
    // call holds.
    std::map<MirLoanId, HeldTexts> referents;
    std::map<HirBindingId, const MirInstruction *> writers;
    bool changed = true;
    // A view, or a reference to one, holds owner text only as a whole; a
    // class or payload-enum value only at a designated path. Other types,
    // such as an int read through a carrier's reference, hold none.
    const auto holds = [&](const SemanticType &type, const HeldText &text) {
      const auto &held =
          type.kind == SemanticType::Reference && type.arguments.size() == 1
              ? type.arguments.front()
              : type;
      return text.path.empty() ? textType(held)
                               : held.kind == SemanticType::Class ||
                                     held.kind == SemanticType::Enum;
    };
    const auto add = [&](HeldTexts &target, const HeldTexts &held,
                         const SemanticType &type) {
      for (const auto &text : held)
        if (holds(type, text))
          changed = target.insert(text).second || changed;
    };
    // A loan's referent also holds the owner text of each publication its
    // stored dependency chain reaches: a reference to a dependent view or
    // to a carrier's designated field.
    const auto referentTexts = [&](MirLoanId id) {
      HeldTexts held;
      if (const auto found = referents.find(id); found != referents.end())
        held = found->second;
      const auto *loan = body.findLoan(id);
      for (std::size_t depth = 0;
           loan && loan->storedDependency != 0 && depth <= body.loans.size();
           ++depth) {
        if (published.contains(loan->storedDependency))
          held.insert({.loan = loan->storedDependency});
        loan = body.findLoan(loan->storedDependency);
      }
      return held;
    };
    // A reference binding's texts are its referent's, so a dereference
    // keeps them; a field keeps those designated within it.
    const auto placeTexts = [&](const MirPlace &place) {
      HeldTexts held;
      const auto copy = [&](const auto &map, const auto &key) {
        if (const auto found = map.find(key); found != map.end())
          held = found->second;
      };
      if (place.root == MirPlaceRootKind::Binding)
        copy(bindings, place.binding);
      else if (place.root == MirPlaceRootKind::Temporary)
        copy(temporaries, place.temporary);
      else if (place.root == MirPlaceRootKind::Value)
        copy(values, place.value);
      else if (place.root == MirPlaceRootKind::Loan)
        held = referentTexts(place.loan);
      for (const auto &projection : place.projections) {
        if (projection.kind == MirProjectionKind::Dereference)
          continue;
        HeldTexts inner;
        if (projection.kind == MirProjectionKind::Field)
          for (const auto &text : held)
            if (!text.path.empty() && text.path.front() == projection.field)
              inner.insert({.loan = text.loan,
                            .path = TextFieldPath(text.path.begin() + 1,
                                                  text.path.end()),
                            .substitutes = text.substitutes});
        held = std::move(inner);
      }
      std::erase_if(
          held, [&](const HeldText &text) { return !holds(place.type, text); });
      return held;
    };
    const auto operandTexts = [&](const MirOperand &operand) {
      HeldTexts held;
      const auto *place = body.findPlace(operand.place);
      switch (operand.kind) {
      case MirOperandKind::Value:
        if (const auto found = values.find(operand.value);
            found != values.end())
          held = found->second;
        break;
      case MirOperandKind::Loan:
        held = referentTexts(operand.loan);
        if (place)
          for (const auto &text : placeTexts(*place))
            held.insert(text);
        break;
      case MirOperandKind::Copy:
      case MirOperandKind::Move:
      case MirOperandKind::BorrowRead:
      case MirOperandKind::BorrowWrite:
        if (place)
          held = placeTexts(*place);
        break;
      case MirOperandKind::Address:
      case MirOperandKind::Constant:
        break;
      }
      return held;
    };
    // The publication and a relay's own Stored loan name the same owner.
    const auto sameOwner = [&](const MirLoan &relay, const MirLoan &loan) {
      const auto *first = body.findPlace(relay.source);
      const auto *second = body.findPlace(loan.source);
      return first && second && relay.access == loan.access &&
             relay.additionalDependencies.empty() &&
             loan.additionalDependencies.empty() &&
             first->root == second->root && first->binding == second->binding &&
             first->symbol == second->symbol &&
             first->capture == second->capture &&
             first->temporary == second->temporary &&
             first->value == second->value && first->loan == second->loan &&
             first->projections == second->projections;
    };
    // What a source call's text result, or the referent of its returned
    // reference to text, holds: the callee's body-derived returns mapped
    // through the call's arguments and receiver.
    const auto callTexts = [&](const MirInstruction &call) {
      HeldTexts held;
      const auto *target =
          call.functionTarget
              ? program.findFunctionInstance(*call.functionTarget)
              : nullptr;
      if (!target || target->definitionKind != MirDefinitionKind::Source)
        return held;
      const auto &type = target->returnType;
      const bool reference = type.kind == SemanticType::Reference &&
                             type.arguments.size() == 1 &&
                             textType(type.arguments.front());
      if (!textType(type) && !reference)
        return held;
      const auto returned = returnSources(*target);
      for (const auto parameter : returned.parameters)
        if (parameter < call.operands.size())
          for (const auto &text : operandTexts(call.operands[parameter]))
            if (text.path.empty())
              held.insert(text);
      for (const auto &field : returned.fields) {
        const MirOperand *origin =
            field.receiver ? (call.receiver ? &*call.receiver : nullptr)
            : field.parameter < call.operands.size()
                ? &call.operands[field.parameter]
                : nullptr;
        const TextFieldPath path =
            field.payload == 0 ? TextFieldPath{field.field}
                               : TextFieldPath{field.field, field.payload};
        if (origin)
          for (const auto &text : operandTexts(*origin))
            if (text.path == path)
              held.insert({.loan = text.loan, .substitutes = text.substitutes});
      }
      if (reference) {
        if (call.loan)
          add(referents[*call.loan], held, type);
        return held;
      }
      const auto *own = call.loan ? body.findLoan(*call.loan) : nullptr;
      if (!own || own->kind != MirLoanKind::Stored ||
          !own->designatedFields.empty())
        return held;
      HeldTexts relayed;
      for (auto text : held) {
        if (const auto *loan = body.findLoan(text.loan);
            loan && sameOwner(*own, *loan))
          text.substitutes.insert(own->id);
        relayed.insert(std::move(text));
      }
      return relayed;
    };
    while (changed) {
      changed = false;
      for (const auto &block : body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.tombstone)
            continue;
          HeldTexts produced;
          if (const auto *loan = textPublicationLoan(body, instruction))
            produced.insert({.loan = loan->id, .path = loan->designatedFields});
          const bool single = instruction.operands.size() == 1;
          const auto forward = [&] {
            if (single)
              for (const auto &text : operandTexts(instruction.operands[0]))
                produced.insert(text);
          };
          switch (instruction.kind) {
          case MirInstructionKind::Load:
          case MirInstructionKind::Move:
          case MirInstructionKind::CallInput:
          case MirInstructionKind::Initialize:
          case MirInstructionKind::Assign:
            forward();
            break;
          case MirInstructionKind::Borrow:
            if (single && instruction.loan)
              add(referents[*instruction.loan],
                  operandTexts(instruction.operands[0]),
                  instruction.operands[0].type);
            break;
          case MirInstructionKind::Compute:
            if (instruction.operation == MirOperation::Identity) {
              forward();
            } else if (instruction.operation == MirOperation::PayloadExtract &&
                       single && instruction.enumVariant &&
                       instruction.payloadIndex) {
              const auto &subject = instruction.operands[0];
              const auto *enumeration = program.findPayloadEnum(
                  subject.type.kind == SemanticType::Reference &&
                          subject.type.arguments.size() == 1
                      ? subject.type.arguments.front()
                      : subject.type);
              const auto *variant =
                  enumeration && *instruction.enumVariant <
                                     enumeration->variants.size()
                      ? &enumeration->variants[*instruction.enumVariant]
                      : nullptr;
              if (variant &&
                  *instruction.payloadIndex < variant->payloadSymbols.size()) {
                const TextFieldPath payload{
                    variant->payloadSymbols[*instruction.payloadIndex]};
                for (const auto &text : operandTexts(subject))
                  if (text.path == payload)
                    produced.insert(
                        {.loan = text.loan, .substitutes = text.substitutes});
              }
            }
            break;
          case MirInstructionKind::Call:
            if (instruction.intrinsic == IntrinsicKind::Move)
              forward();
            else
              for (const auto &text : callTexts(instruction))
                produced.insert(text);
            break;
          default:
            break;
          }
          if (produced.empty())
            continue;
          if (instruction.result)
            add(values[*instruction.result], produced, instruction.info.type);
          const auto *destination =
              instruction.destination ? body.findPlace(*instruction.destination)
                                      : nullptr;
          if (!destination || !destination->projections.empty())
            continue;
          if (destination->root == MirPlaceRootKind::Binding) {
            add(bindings[destination->binding], produced, destination->type);
            writers.emplace(destination->binding, &instruction);
          } else if (destination->root == MirPlaceRootKind::Temporary) {
            add(temporaries[destination->temporary], produced,
                destination->type);
          }
        }
        // A block parameter holds what each incoming argument holds.
        const auto edge = [&](MirBlockId target,
                              const std::vector<MirOperand> &arguments) {
          const auto *successor = body.findBlock(target);
          for (std::size_t index = 0; successor && index < arguments.size() &&
                                      index < successor->parameters.size();
               ++index)
            add(values[successor->parameters[index].value],
                operandTexts(arguments[index]),
                successor->parameters[index].type);
        };
        edge(block.terminator.target, block.terminator.targetArguments);
        edge(block.terminator.elseTarget, block.terminator.elseArguments);
        for (const auto &target : block.terminator.switchTargets)
          edge(target.target, target.arguments);
      }
    }
    for (const auto &[binding, held] : bindings)
      for (const auto &text : held) {
        const auto retains = [&](const MirLoan &candidate) {
          if (std::find(candidate.carriers.begin(), candidate.carriers.end(),
                        binding) == candidate.carriers.end())
            return false;
          const MirLoan *loan = &candidate;
          for (std::size_t depth = 0; loan && depth <= body.loans.size();
               ++depth) {
            if (loan->id == text.loan || text.substitutes.contains(loan->id))
              return true;
            loan = loan->storedDependency != 0
                       ? body.findLoan(loan->storedDependency)
                       : nullptr;
          }
          return false;
        };
        if (std::none_of(body.loans.begin(), body.loans.end(), retains)) {
          fail(context, owner, writers[binding],
               "local binding holding dependent text does not carry a loan "
               "that retains its owner");
          break;
        }
      }
  }

  // The payload construction that is a constructor field initializer's
  // whole argument, whose stored text that initializer accounts for.
  bool fieldInitializerValue(const TextBody &context,
                             const MirInstruction &instruction) const {
    const auto *value = context.constructor && instruction.result
                            ? context.body.findValue(*instruction.result)
                            : nullptr;
    return value != nullptr &&
           std::any_of(context.constructor->initializers.begin(),
                       context.constructor->initializers.end(),
                       [&](const auto &initializer) {
                         return initializer.kind ==
                                    ConstructorInitializerTargetKind::Field &&
                                initializer.arguments.size() == 1 &&
                                initializer.arguments.front() ==
                                    value->sourceValue &&
                                initializer.targetType == value->info.type;
                       });
  }

  // A payload-enum field initialized by a payload construction stores each
  // of its text payloads. A formal that alone fills one, in a field the
  // constructor never reads back through `this`, may be published at the
  // path to that payload.
  using FieldUses = std::map<std::pair<std::size_t, SymbolId>, std::size_t>;
  // A formal's text field that alone fills `path`, as its source names it.
  static void noteFieldRelay(const TextSources &source, bool exclusive,
                             TextFieldPath path, InitializerTextInputs &inputs,
                             FieldUses &fieldUses) {
    for (const auto &field : source.fields)
      if (!field.receiver && field.payload == 0)
        ++fieldUses[{field.parameter, field.field}];
    if (exclusive && source.known && source.parameters.empty() &&
        source.fields.size() == 1 && !source.fields.begin()->receiver &&
        source.fields.begin()->payload == 0)
      inputs.fieldRelays.emplace(std::pair{source.fields.begin()->parameter,
                                           source.fields.begin()->field},
                                 std::move(path));
  }

  // The receiver field that every return of `function` moves or loads
  // whole, as an accessor of a nested value does.
  std::optional<SymbolId>
  returnedReceiverField(const MirFunctionInstance &function) const {
    std::optional<SymbolId> field;
    for (const auto &block : function.body.blocks) {
      if (block.terminator.kind != MirTerminatorKind::Return)
        continue;
      const auto &value = block.terminator.value;
      const auto *producer = value && value->kind == MirOperandKind::Value
                                 ? definition(function.body, value->value)
                                 : nullptr;
      if (producer == nullptr || producer->operands.size() != 1 ||
          (producer->kind != MirInstructionKind::Move &&
           producer->kind != MirInstructionKind::Load))
        return std::nullopt;
      const auto &read = producer->operands.front();
      const auto *place =
          read.kind == MirOperandKind::Copy || read.kind == MirOperandKind::Move
              ? function.body.findPlace(read.place)
              : nullptr;
      if (place == nullptr || place->root != MirPlaceRootKind::This ||
          place->projections.size() != 1 ||
          place->projections.front().kind != MirProjectionKind::Field ||
          (field && *field != place->projections.front().field))
        return std::nullopt;
      field = place->projections.front().field;
    }
    return field;
  }

  // The class formal and its field that `operand` passes on whole: a read
  // of `formal.field`, or an accessor of the exact formal returning it.
  std::optional<std::pair<std::size_t, SymbolId>>
  classFieldSource(const TextBody &context, const MirOperand &operand) const {
    const MirOperand *current = &operand;
    for (std::size_t depth = 0;
         depth < 4 && current->kind == MirOperandKind::Value; ++depth) {
      const auto *producer = definition(context.body, current->value);
      if (producer == nullptr)
        return std::nullopt;
      if (producer->kind == MirInstructionKind::Call) {
        const auto *target =
            producer->functionTarget
                ? program.findFunctionInstance(*producer->functionTarget)
                : nullptr;
        const auto field =
            target != nullptr ? returnedReceiverField(*target) : std::nullopt;
        const auto formal = field && producer->receiver
                                ? exactClassFormal(context, *producer->receiver)
                                : std::nullopt;
        return formal ? std::optional{std::pair{*formal, *field}}
                      : std::nullopt;
      }
      if (producer->operands.size() != 1 ||
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
    if (place == nullptr || place->root != MirPlaceRootKind::Binding ||
        place->capture != 0 || place->projections.size() != 1 ||
        place->projections.front().kind != MirProjectionKind::Field)
      return std::nullopt;
    const auto found = std::find(context.bindings.begin(),
                                 context.bindings.end(), place->binding);
    const auto index =
        static_cast<std::size_t>(found - context.bindings.begin());
    if (found == context.bindings.end() || index >= context.types.size() ||
        context.types[index].kind != SemanticType::Class)
      return std::nullopt;
    return std::pair{index, place->projections.front().field};
  }

  // A class formal that `operand` passes on whole, of the stored `type`,
  // alone fills `path`: its argument's text paths nest there.
  void noteWholeRelay(const TextBody &context, const MirOperand &operand,
                      const SemanticType &type, bool exclusive,
                      TextFieldPath path, InitializerTextInputs &inputs,
                      FieldUses &fieldUses) const {
    if (!nestedTextClass(program, type))
      return;
    const auto formal = exactClassFormal(context, operand);
    if (!formal || context.types[*formal] != type) {
      // A class field of a formal, read directly or through an accessor,
      // stored whole re-roots the paths through that field.
      if (const auto source = classFieldSource(context, operand)) {
        ++fieldUses[{source->first, source->second}];
        if (exclusive)
          inputs.fieldRelays.emplace(*source, std::move(path));
      }
      return;
    }
    ++fieldUses[{*formal, 0}];
    if (!exclusive)
      return;
    inputs.fieldRelays.emplace(std::pair{*formal, SymbolId{0}},
                               std::move(path));
    for (const MirOperand *current = &operand;
         current->kind == MirOperandKind::Value;) {
      const auto *producer = definition(context.body, current->value);
      if (producer == nullptr || producer->operands.size() != 1)
        break;
      inputs.wholeRelayProducers.insert(producer->id);
      current = &producer->operands.front();
    }
  }

  void payloadInitializerTextInputs(
      const TextBody &context, const MirConstructorInitializer &initializer,
      bool diagnose, InitializerTextInputs &inputs,
      std::map<std::size_t, std::size_t> &uses, FieldUses &fieldUses) {
    const auto found = std::find_if(
        context.body.values.begin(), context.body.values.end(),
        [&](const auto &value) {
          return value.sourceValue == initializer.arguments.front() &&
                 value.info.type == initializer.targetType;
        });
    const auto *producer = found == context.body.values.end()
                               ? nullptr
                               : definition(context.body, found->id);
    const auto *enumeration = program.findPayloadEnum(initializer.targetType);
    if (producer == nullptr || enumeration == nullptr ||
        producer->kind != MirInstructionKind::Compute ||
        producer->operation != MirOperation::PayloadConstruct ||
        !producer->enumVariant ||
        *producer->enumVariant >= enumeration->variants.size())
      return;
    const auto &variant = enumeration->variants[*producer->enumVariant];
    const bool readBack = std::any_of(
        context.body.places.begin(), context.body.places.end(),
        [&](const MirPlace &place) {
          return place.root == MirPlaceRootKind::This &&
                 (place.projections.empty() ||
                  (place.projections.front().kind == MirProjectionKind::Field &&
                   place.projections.front().field == initializer.field)) &&
                 readsPlace(context.body, place.id);
        });
    for (std::size_t index = 0; index < variant.payloadTypes.size() &&
                                index < producer->operands.size();
         ++index) {
      // A class formal stored whole as the payload nests its text paths.
      if (variant.payloadTypes[index].kind == SemanticType::Class) {
        if (initializer.field != 0 && index < variant.payloadSymbols.size())
          noteWholeRelay(
              context, producer->operands[index], variant.payloadTypes[index],
              !readBack,
              TextFieldPath{initializer.field, variant.payloadSymbols[index]},
              inputs, fieldUses);
        continue;
      }
      if (!textType(variant.payloadTypes[index]))
        continue;
      const auto source = sources(context, producer->operands[index]);
      if (!source.known && diagnose)
        fail(context, context.constructor->id, producer,
             "text payload initializer has no independent ownerless input "
             "proof");
      inputs.requirements.insert(source.parameters.begin(),
                                 source.parameters.end());
      for (const auto parameter : source.parameters)
        ++uses[parameter];
      if (source.known && source.parameters.size() == 1 && !readBack &&
          initializer.field != 0 && index < variant.payloadSymbols.size())
        inputs.fields.emplace(
            *source.parameters.begin(),
            TextFieldPath{initializer.field, variant.payloadSymbols[index]});
      if (initializer.field != 0 && index < variant.payloadSymbols.size())
        noteFieldRelay(
            source, !readBack,
            TextFieldPath{initializer.field, variant.payloadSymbols[index]},
            inputs, fieldUses);
    }
  }

  // Whether any instruction or loan reads `place`, other than writing it.
  static bool readsPlace(const MirBody &body, MirPlaceId place) {
    const auto reads = [&](const MirOperand &operand) {
      return (operand.kind != MirOperandKind::Value &&
              operand.kind != MirOperandKind::Constant &&
              operand.kind != MirOperandKind::Loan && operand.place == place);
    };
    for (const auto &loan : body.loans)
      if (loan.source == place)
        return true;
    for (const auto &block : body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (std::any_of(instruction.operands.begin(),
                        instruction.operands.end(), reads) ||
            (instruction.receiver && reads(*instruction.receiver)))
          return true;
      }
      if (block.terminator.value && reads(*block.terminator.value))
        return true;
    }
    return false;
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
        loan->designatedFields != field->second ||
        !loan->additionalDependencies.empty() ||
        !directTextPath(program, instruction.info.type, field->second))
      return false;
    const BorrowedFieldOrigin expected{.resultFields = field->second,
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
    // A reference to text that a call returned is not one: its readers
    // resolve the text it designates through the callee's returns.
    // The formal or receiver, and the fields of it, that `operand`
    // designates when it is the reference a formal's accessor returned, or a
    // place rooted at that reference: the fields the accessor's returns name
    // (`accessorResultFields`), as in semantics' `contractEnumFields`.
    const auto accessorOperandFields = [&](const MirOperand &operand)
        -> std::optional<std::pair<OwnerlessTextField, std::vector<SymbolId>>> {
      if (operand.kind == MirOperandKind::Value ||
          operand.kind == MirOperandKind::Constant)
        return std::nullopt;
      MirPlace result;
      result.root = MirPlaceRootKind::Loan;
      result.loan = operand.loan;
      const auto *loan = operand.kind == MirOperandKind::Loan
                             ? context.body.findLoan(operand.loan)
                             : nullptr;
      for (const MirPlace *place : std::array<const MirPlace *, 2>{
               operand.kind == MirOperandKind::Loan ? &result : nullptr,
               context.body.findPlace(loan ? loan->source : operand.place)}) {
        std::vector<const MirFunctionInstance *> visiting;
        const MirPlace *receiver = nullptr;
        auto fields = place ? accessorResultFields(context.body, *place,
                                                   visiting, &receiver)
                            : std::nullopt;
        const auto formal = fields && receiver
                                ? contractPlace(context, *receiver)
                                : std::nullopt;
        if (formal && formal->field == 0)
          return std::pair{*formal, std::move(*fields)};
      }
      return std::nullopt;
    };
    const auto wholeUse =
        [&](const MirOperand &operand) -> std::optional<OwnerlessTextField> {
      if (operand.kind == MirOperandKind::Loan &&
          textReferenceCall(context.body, operand.loan))
        return std::nullopt;
      // Uses of an accessor's reference resolve the fields it designates.
      if (accessorOperandFields(operand))
        return std::nullopt;
      const auto *loan = operand.kind == MirOperandKind::Loan
                             ? context.body.findLoan(operand.loan)
                             : nullptr;
      const auto *place =
          context.body.findPlace(loan ? loan->source : operand.place);
      const auto formal = place ? contractPlace(context, *place) : std::nullopt;
      return formal && formal->field == 0 ? formal : std::nullopt;
    };
    const auto requireAllFields = [&](const OwnerlessTextField &formal) {
      for (const auto &field : formalTextFields(context, formal))
        neededFields.insert(field);
    };
    // The call that consumes a CallInput's result, and the callee formal or
    // receiver it binds. A later checked argument places the call in another
    // block than the input.
    std::function<std::pair<const MirInstruction *, OwnerlessTextField>(
        const MirInstruction &)>
        consumer;
    consumer = [&](const MirInstruction &input)
        -> std::pair<const MirInstruction *, OwnerlessTextField> {
      // A move staged into a call input forwards to that input's call.
      if (input.kind == MirInstructionKind::Move && input.result)
        for (const auto &block : context.body.blocks)
          for (const auto &candidate : block.instructions)
            if (candidate.kind == MirInstructionKind::CallInput &&
                candidate.operands.size() == 1 &&
                candidate.operands.front().kind == MirOperandKind::Value &&
                candidate.operands.front().value == *input.result)
              return consumer(candidate);
      // A borrow that a call returning a reference to text takes as its
      // receiver forwards like an input: readers resolve the returned text
      // through the callee's returns.
      if (input.kind == MirInstructionKind::Borrow && input.loan)
        for (const auto &block : context.body.blocks)
          for (const auto &candidate : block.instructions)
            if (candidate.kind == MirInstructionKind::Call &&
                candidate.receiver &&
                candidate.receiver->kind == MirOperandKind::Loan &&
                candidate.receiver->loan == *input.loan && candidate.loan &&
                textReferenceCall(context.body, *candidate.loan) == &candidate)
              return {&candidate, {.receiver = true}};
      if (input.kind != MirInstructionKind::CallInput || !input.result)
        return {nullptr, {}};
      for (const auto &block : context.body.blocks)
        for (const auto &candidate : block.instructions) {
          if (candidate.kind != MirInstructionKind::Call &&
              candidate.kind != MirInstructionKind::Construct)
            continue;
          if (candidate.receiver &&
              candidate.receiver->kind == MirOperandKind::Value &&
              candidate.receiver->value == *input.result)
            return {&candidate, {.receiver = true}};
          for (std::size_t index = 0; index < candidate.operands.size();
               ++index)
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
        // Whole-value uses of a formal or the receiver, and forwarding. A
        // constructor's whole class formal counts too: a field it re-roots
        // must reach nothing else.
        if (context.fieldContracts || context.constructor) {
          const auto visit = [&](const MirOperand &operand, bool receiver) {
            const auto formal = wholeUse(operand);
            if (!formal)
              return;
            // Passing a formal stored whole to its initializer is the relay.
            if (context.constructor != nullptr &&
                context.constructor->id <= constructorRelayProducers.size() &&
                constructorRelayProducers[context.constructor->id - 1].contains(
                    instruction.id))
              return;
            // A call returning a reference to text takes its receiver as an
            // input does: readers resolve the returned text.
            auto [call, calleeFormal] = consumer(instruction);
            if (receiver && instruction.kind == MirInstructionKind::Call &&
                instruction.loan &&
                textReferenceCall(context.body, *instruction.loan) ==
                    &instruction) {
              call = &instruction;
              calleeFormal = {.receiver = true};
            }
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
                                     .field = field.field,
                                     .payload = field.payload});
          };
          for (const auto &operand : instruction.operands)
            visit(operand, false);
          if (instruction.receiver)
            visit(*instruction.receiver, true);
        }
        // A payload-enum field of a formal or the receiver that anything but
        // a payload extraction uses, a copy, move or borrow of it, may let
        // any of its text payloads outlive the call (L-OWN-02). A switch
        // observes its subject in place, in its terminator.
        // An accessor's returned borrow of its field is its designation:
        // callers resolve it as the field (`accessorResultField`).
        // A switch on an accessor designating several fields reads them all,
        // as in semantics' `notePayloadFieldRead`: a payload binding cannot
        // name the field.
        const bool extract =
            instruction.kind == MirInstructionKind::Compute &&
            instruction.operation == MirOperation::PayloadExtract;
        if (context.fieldContracts &&
            !(instruction.kind == MirInstructionKind::Borrow &&
              instruction.loan &&
              accessorReturnLoans(context).contains(*instruction.loan))) {
          const auto visitField = [&](const MirOperand &operand) {
            if (operand.kind == MirOperandKind::Value ||
                operand.kind == MirOperandKind::Constant)
              return;
            std::optional<OwnerlessTextField> formal;
            std::vector<SymbolId> designated;
            // The reference an accessor of a formal or the receiver returns
            // holds what that formal's fields hold.
            if (auto accessed = accessorOperandFields(operand)) {
              if (extract && accessed->second.size() < 2)
                return;
              formal = accessed->first;
              for (const SymbolId field : accessed->second)
                if (payloadEnumField(fieldOwnerType(context, *formal), field))
                  designated.push_back(field);
            } else if (!extract) {
              const auto *loan = operand.kind == MirOperandKind::Loan
                                     ? context.body.findLoan(operand.loan)
                                     : nullptr;
              const auto *place =
                  context.body.findPlace(loan ? loan->source : operand.place);
              formal =
                  place ? contractPlace(context, *place, true) : std::nullopt;
              if (formal)
                designated.push_back(formal->field);
            }
            if (!formal)
              return;
            for (const auto &field :
                 formalTextFields(context, {.receiver = formal->receiver,
                                            .parameter = formal->parameter}))
              if (field.payload != 0 &&
                  std::find(designated.begin(), designated.end(),
                            field.field) != designated.end())
                neededFields.insert(field);
          };
          for (const auto &operand : instruction.operands)
            visitField(operand);
          if (instruction.receiver)
            visitField(*instruction.receiver);
        }
        // A whole use of a nested text class that a call returned by
        // reference, a copy of it or a call on it, may let any text under the
        // relayed payload escape; a field read through it is resolved where
        // its text is stored (L-OWN-02 phase 3).
        {
          const auto visitRelay = [&](const MirOperand &operand) {
            if (operand.kind == MirOperandKind::Value ||
                operand.kind == MirOperandKind::Constant)
              return;
            const auto *loan = operand.kind == MirOperandKind::Loan
                                   ? context.body.findLoan(operand.loan)
                                   : nullptr;
            const auto *place =
                context.body.findPlace(loan ? loan->source : operand.place);
            if (place == nullptr || place->root != MirPlaceRootKind::Loan ||
                !place->projections.empty() ||
                !nestedTextClass(program, place->type))
              return;
            if (const auto relayed = classReferenceSources(context, *place)) {
              if (!relayed->known && diagnose)
                fail(context, owner, &instruction,
                     "nested text value relayed by reference may escape");
              neededFields.insert(relayed->fields.begin(),
                                  relayed->fields.end());
            }
          };
          for (const auto &operand : instruction.operands)
            visitRelay(operand);
          if (instruction.receiver)
            visitRelay(*instruction.receiver);
        }
        // A dependent value relayed as a formal or receiver: its callee
        // must carry a contract that lets none of its fields outlive the
        // call.
        if (diagnose && instruction.kind == MirInstructionKind::CallInput &&
            instruction.operands.size() == 1)
          if (const auto designated = dependentDesignationPath(
                  context.body, instruction.operands.front())) {
            const auto [call, calleeFormal] = consumer(instruction);
            const auto *target =
                call && call->kind == MirInstructionKind::Call &&
                        call->functionTarget
                    ? program.findFunctionInstance(*call->functionTarget)
                    : nullptr;
            const OwnerlessTextField field{
                .receiver = calleeFormal.receiver,
                .parameter = calleeFormal.parameter,
                .field = designated->front(),
                .payload = designated->size() >= 2 ? (*designated)[1] : 0};
            // A construction re-roots the value's dependency at the path its
            // constructor stores that field, or the whole value, in, under its
            // own loan.
            const bool reRooted =
                call && !calleeFormal.receiver &&
                ((call->kind == MirInstructionKind::Construct &&
                  publishesFieldRelay(context, *call, calleeFormal.parameter,
                                      *designated)) ||
                 publishesWholeCallText(context, *call, calleeFormal.parameter,
                                        *designated));
            // A contract names a text field or a payload-enum field's text
            // payload; a path through a nested class field has no contract.
            SemanticType argument = instruction.info.type;
            if (argument.kind == SemanticType::Reference &&
                argument.arguments.size() == 1)
              argument = SemanticType(argument.arguments.front());
            const bool contracted =
                designated->size() == 1 ||
                (designated->size() >= 2 &&
                 payloadEnumField(argument, designated->front()));
            if (reRooted) {
            } else if (!contracted || !target ||
                       !fieldContractFunction(program, *target) ||
                       !readOnlyContractFormal(*target, calleeFormal) ||
                       std::binary_search(target->ownerlessTextFields.begin(),
                                          target->ownerlessTextFields.end(),
                                          field))
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
              if (field != fields.end() && field->second.size() == 1 &&
                  formal &&
                  directTextField(program, instruction.info.type,
                                  field->second.front())) {
                relayedFormals.insert(
                    {.parameter = *formal, .field = field->second.front()});
                continue;
              }
            }
            if (index < instruction.operands.size())
              require(instruction, instruction.operands[index]);
            else if (diagnose)
              fail(context, owner, &instruction,
                   "ownerless text argument is missing");
          }
        // A returned fresh value relays a class formal that its constructor
        // stores whole in one field (L-OWN-02 phase 3).
        if (instruction.kind == MirInstructionKind::Construct &&
            instruction.constructorTarget &&
            *instruction.constructorTarget != 0 &&
            *instruction.constructorTarget <= constructorFieldRelays.size() &&
            returnedFreshValue(context, instruction))
          for (const auto &[key, path] :
               constructorFieldRelays[*instruction.constructorTarget - 1])
            if (key.second == 0 && path.size() == 1 &&
                key.first < instruction.operands.size())
              if (const auto formal = exactClassFormal(
                      context, instruction.operands[key.first]))
                relayedFormals.insert(
                    {.parameter = *formal, .field = path.front()});
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
            // A constructor's field initializer accounts for the text it
            // stores in its payload (`initializerTextInputs`).
            if (!fieldInitializerValue(context, instruction))
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
      // A returned reference to text that a call returned designates what
      // the callee's returns do; callers resolve it through returnSources.
      const bool returnedTextReference =
          terminator.kind == MirTerminatorKind::Return && terminator.value &&
          terminator.value->kind == MirOperandKind::Loan &&
          (textReferenceCall(context.body, terminator.value->loan) || [&] {
            const auto *forwarded =
                forwardedLoan(context.body, terminator.value->loan);
            return forwarded && textReferenceCall(context.body, *forwarded);
          }());
      if (terminator.value && !returnedTextReference)
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
    const std::map<TextFieldPath, std::size_t> *published)
    -> InitializerTextInputs {
  const TextBody context{.body = constructor.body,
                         .types = constructor.parameterTypes,
                         .bindings = constructor.parameterBindings,
                         .publishedFields = published,
                         .constructor = &constructor};
  InitializerTextInputs inputs;
  std::map<std::size_t, std::size_t> uses;
  FieldUses fieldUses;
  for (const auto &initializer : constructor.initializers) {
    if (initializer.kind == ConstructorInitializerTargetKind::Field &&
        initializer.targetType.kind == SemanticType::Enum &&
        initializer.arguments.size() == 1) {
      payloadInitializerTextInputs(context, initializer, diagnose, inputs, uses,
                                   fieldUses);
      continue;
    }
    // A class formal stored whole in a field of its type nests its text
    // paths there (L-OWN-02).
    if (initializer.kind == ConstructorInitializerTargetKind::Field &&
        initializer.targetType.kind == SemanticType::Class &&
        initializer.arguments.size() == 1 && initializer.field != 0) {
      const auto found = std::find_if(
          constructor.body.values.begin(), constructor.body.values.end(),
          [&](const auto &value) {
            return value.sourceValue == initializer.arguments.front() &&
                   value.info.type == initializer.targetType;
          });
      if (found != constructor.body.values.end())
        noteWholeRelay(context,
                       {.kind = MirOperandKind::Value,
                        .value = found->id,
                        .type = found->info.type},
                       initializer.targetType, true,
                       TextFieldPath{initializer.field}, inputs, fieldUses);
      continue;
    }
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
      inputs.fields.emplace(*source.parameters.begin(),
                            TextFieldPath{initializer.field});
    if (initializer.field != 0)
      noteFieldRelay(source, true, TextFieldPath{initializer.field}, inputs,
                     fieldUses);
  }
  std::erase_if(inputs.fields,
                [&](const auto &entry) { return uses[entry.first] != 1; });
  std::erase_if(inputs.fieldRelays,
                [&](const auto &entry) { return fieldUses[entry.first] != 1; });
  // A formal stored whole reaches no other initializer.
  std::erase_if(inputs.fieldRelays, [&](const auto &entry) {
    if (entry.first.second != 0)
      return false;
    const auto parameter = entry.first.first;
    return uses[parameter] != 0 ||
           std::any_of(
               fieldUses.begin(), fieldUses.end(), [&](const auto &use) {
                 return use.first.first == parameter && use.first.second != 0;
               });
  });
  return inputs;
}

MirVerificationResult TextContracts::verify() {
  requirements.resize(program.functionInstances().size());
  fieldRequirements.resize(program.functionInstances().size());
  constructorRequirements.resize(program.constructorInstances().size());
  constructorTextFields.resize(program.constructorInstances().size());
  constructorFieldRelays.resize(program.constructorInstances().size());
  constructorRelayProducers.resize(program.constructorInstances().size());
  constructorPublishedFields.resize(program.constructorInstances().size());
  for (const auto &constructor : program.constructorInstances()) {
    auto inputs = initializerTextInputs(constructor, true);
    auto &published = constructorPublishedFields[constructor.id - 1];
    for (const auto &[formal, path] : inputs.fields)
      published.emplace(path, formal);
    // Another initializer reading a published field through `this` stores
    // that formal's text too, so the field is no longer exclusive.
    if (!published.empty())
      inputs = initializerTextInputs(constructor, false, &published);
    constructorRequirements[constructor.id - 1] =
        std::move(inputs.requirements);
    constructorTextFields[constructor.id - 1] = std::move(inputs.fields);
    constructorFieldRelays[constructor.id - 1] = std::move(inputs.fieldRelays);
    constructorRelayProducers[constructor.id - 1] =
        std::move(inputs.wholeRelayProducers);
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
    verifyDependentTextCarriers(context, function.id);
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
  verifyDependentTextCarriers({program.module(), {}, {}}, 0);
  for (const auto &instance : program.classInstances()) {
    (void)scan({instance.fieldInitializers, {}, {}}, instance.id, true);
    (void)scan({instance.staticFieldInitializers, {}, {}}, instance.id, true);
    verifyDependentTextCarriers({instance.fieldInitializers, {}, {}},
                                instance.id);
    verifyDependentTextCarriers({instance.staticFieldInitializers, {}, {}},
                                instance.id);
  }
  std::vector<std::set<std::size_t>> constructorBodyNeeds;
  std::vector<std::set<OwnerlessTextField>> constructorBodyFieldNeeds;
  for (const auto &constructor : program.constructorInstances()) {
    const TextBody context{.body = constructor.body,
                           .types = constructor.parameterTypes,
                           .bindings = constructor.parameterBindings,
                           .publishedFields =
                               &constructorPublishedFields[constructor.id - 1],
                           .constructor = &constructor};
    std::set<OwnerlessTextField> fieldNeeds;
    const auto needed = scan(context, constructor.id, true, &fieldNeeds);
    constructorBodyFieldNeeds.push_back(std::move(fieldNeeds));
    verifyDependentTextCarriers(context, constructor.id);
    if (!std::includes(constructorRequirements[constructor.id - 1].begin(),
                       constructorRequirements[constructor.id - 1].end(),
                       needed.begin(), needed.end()))
      fail(context, constructor.id, nullptr,
           "constructor call requires an unrepresented ownerless text input "
           "contract");
    constructorBodyNeeds.push_back(needed);
  }
  for (const auto &destructor : program.destructorInstances()) {
    (void)scan({destructor.body, {}, {}}, destructor.owner, true);
    verifyDependentTextCarriers({destructor.body, {}, {}}, destructor.owner);
  }
  for (const auto &lambda : program.lambdaInstances()) {
    verifyDependentTextCarriers(
        {lambda.body, lambda.parameterTypes, lambda.parameterBindings},
        lambda.id);
    if (!scan({lambda.body, lambda.parameterTypes, lambda.parameterBindings},
              lambda.id, true)
             .empty())
      fail({lambda.body, lambda.parameterTypes, lambda.parameterBindings},
           lambda.id, nullptr,
           "lambda requires an unrepresented ownerless text input contract");
  }
  for (const auto &[constructor, index, field] : publishedFieldRelays)
    if (field == 0
            ? std::any_of(constructorBodyFieldNeeds[constructor - 1].begin(),
                          constructorBodyFieldNeeds[constructor - 1].end(),
                          [&](const OwnerlessTextField &need) {
                            return !need.receiver && need.parameter == index;
                          })
            : constructorBodyFieldNeeds[constructor - 1].contains(
                  {.parameter = index, .field = field})) {
      const auto &instance = program.constructorInstances()[constructor - 1];
      fail({instance.body, instance.parameterTypes, instance.parameterBindings},
           constructor, nullptr,
           "re-rooted text field also reaches ownerless text storage");
    }
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

bool directTextPath(const MirProgram &program, const SemanticType &type,
                    std::span<const SymbolId> path) {
  if (path.empty())
    return false;
  // Each step is a field of a class or a payload of a payload enum without
  // borrowed state; the path ends at text.
  std::optional<SemanticType> next;
  if (type.kind == SemanticType::Class) {
    const auto instance = std::find_if(
        program.classInstances().begin(), program.classInstances().end(),
        [&](const auto &candidate) { return candidate.type == type; });
    if (instance == program.classInstances().end() ||
        instance->traits.containsBorrowedState)
      return false;
    for (const auto &entry : instance->declaredFields)
      if (entry.symbol == path.front())
        next = entry.type;
  } else if (const MirPayloadEnum *enumeration =
                 type.kind == SemanticType::Enum ? program.findPayloadEnum(type)
                                                 : nullptr;
             enumeration != nullptr &&
             !enumeration->traits.containsBorrowedState) {
    for (const auto &variant : enumeration->variants)
      for (std::size_t index = 0; index < variant.payloadSymbols.size() &&
                                  index < variant.payloadTypes.size();
           ++index)
        if (variant.payloadSymbols[index] == path.front())
          next = variant.payloadTypes[index];
  }
  if (!next)
    return false;
  return path.size() == 1 ? textType(*next)
                          : directTextPath(program, *next, path.subspan(1));
}

std::optional<TextFieldPath>
dependentTextRelayPath(const MirProgram &program,
                       const MirConstructorInstance &constructor,
                       std::size_t parameter, std::span<const SymbolId> held) {
  if (held.empty())
    return std::nullopt;
  TextContracts contracts(program);
  const auto relays =
      contracts.initializerTextInputs(constructor, false).fieldRelays;
  for (const SymbolId key : {held.front(), SymbolId{0}}) {
    const auto found = relays.find({parameter, key});
    if (found == relays.end())
      continue;
    TextFieldPath path = found->second;
    path.insert(path.end(), held.begin() + (key == 0 ? 0 : 1), held.end());
    return path;
  }
  return std::nullopt;
}

std::optional<std::size_t>
dependentTextFieldFormal(const MirProgram &program,
                         const MirConstructorInstance &constructor,
                         std::span<const SymbolId> path) {
  TextContracts contracts(program);
  for (const auto &[formal, target] :
       contracts.initializerTextInputs(constructor, false).fields)
    if (std::equal(target.begin(), target.end(), path.begin(), path.end()))
      return formal;
  return std::nullopt;
}

MirVerificationResult verifyOwnerlessTextContracts(const MirProgram &program) {
  return TextContracts(program).verify();
}
} // namespace lang::mir_detail
