#include "places.h"
#include "placement.h"

#include <algorithm>

namespace lang::native_llvm {

Places::Places(Session &session, llvm::IRBuilder<> &ir, const MirBody &body,
               std::vector<llvm::Value *> &values, Scalars::Guard guard)
    : session(session), ir(ir), body(body), values(values),
      guard(std::move(guard)), loans(body.loans.size() + 1),
      loanTypes(body.loans.size() + 1), selectedValues(body.values.size() + 1) {
}

Places::Key Places::key(const MirPlace &place) {
  return {place.root,
          place.root == MirPlaceRootKind::Binding     ? place.binding
          : place.root == MirPlaceRootKind::Temporary ? place.temporary
          : place.root == MirPlaceRootKind::Symbol    ? place.symbol
          : place.root == MirPlaceRootKind::Loan      ? place.loan
                                                      : place.value,
          place.root == MirPlaceRootKind::Binding ? place.symbol : 0};
}
llvm::Value *Places::slot(llvm::Type *type) {
  llvm::IRBuilder<> entry(
      &ir.GetInsertBlock()->getParent()->getEntryBlock(),
      ir.GetInsertBlock()->getParent()->getEntryBlock().begin());
  return entry.CreateAlloca(type);
}
void Places::allocate(
    const std::map<SymbolId, llvm::GlobalVariable *> &globals) {
  for (const auto &place : body.places) {
    if (roots.contains(key(place)))
      continue;
    if (!place.projections.empty())
      continue;
    if (place.root != MirPlaceRootKind::Binding &&
        place.root != MirPlaceRootKind::Temporary &&
        place.root != MirPlaceRootKind::Symbol)
      continue;
    if (place.root == MirPlaceRootKind::Symbol &&
        !globals.contains(place.symbol))
      throw BackendUnsupportedError(
          "LLVM symbol place requires declared global storage");
    if (place.type.kind == SemanticType::TypePack && place.type.concretePack &&
        place.root == MirPlaceRootKind::Binding) {
      roots.try_emplace(key(place), Root{nullptr, place.type});
      packParameters.try_emplace(key(place), place.type.arguments.size());
      continue;
    }
    auto *storage = globals.contains(place.symbol) &&
                            (place.root == MirPlaceRootKind::Symbol ||
                             body.kind == MirBodyKind::Module)
                        ? globals.at(place.symbol)
                        : slot(session.layouts().get(place.type).storage);
    roots.try_emplace(key(place), Root{storage, place.type});
  }
  for (const auto &loan : body.loans) {
    loans.at(loan.id) = slot(session.layouts().pointer());
    if (const auto *source = body.findPlace(loan.source)) {
      loanTypes.at(loan.id) = source->type.kind == SemanticType::Reference
                                  ? source->type.arguments.at(0)
                                  : source->type;
    }
  }
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.tombstone)
        continue;
      if (instruction.result &&
          instruction.operation == MirOperation::PayloadExtract &&
          instruction.payloadExtractionKind &&
          isReferencePayloadBinding(*instruction.payloadExtractionKind))
        selectedValues.at(*instruction.result) = true;
      if (instruction.loan && (instruction.kind == MirInstructionKind::Call ||
                               instruction.kind == MirInstructionKind::Borrow))
        loanTypes.at(*instruction.loan) = instruction.info.type;
    }
}
llvm::Value *Places::load(llvm::Value *address, const SemanticType &type) {
  if (addressValue(session.program().mir(), type))
    return address;
  auto *value = ir.CreateLoad(session.layouts().get(type).storage, address);
  return type == SemanticType::Bool ? ir.CreateTrunc(value, ir.getInt1Ty())
                                    : value;
}
void Places::store(llvm::Value *value, llvm::Value *address,
                   const SemanticType &type) {
  if (addressValue(session.program().mir(), type)) {
    if (value == address)
      return;
    const auto layout = session.layouts().get(type);
    ir.CreateMemCpy(address, llvm::MaybeAlign(layout.alignment), value,
                    llvm::MaybeAlign(layout.alignment), layout.size);
    return;
  }
  if (type == SemanticType::Bool)
    value = ir.CreateZExt(value, ir.getInt8Ty());
  ir.CreateStore(value, address);
}
llvm::Value *Places::temporary(const SemanticType &type) {
  return slot(session.layouts().get(type).storage);
}
void Places::receiver(llvm::Value *address, const SemanticType &type) {
  receiverAddress = address;
  receiverType = type;
  if (body.kind != MirBodyKind::FieldInitializers)
    return;
  const auto &owner = session.layouts().instance(type);
  for (const auto &place : body.places)
    if (place.root == MirPlaceRootKind::Binding && place.projections.empty())
      for (const auto &field : owner.declaredFields)
        if (field.symbol == place.symbol)
          roots.insert_or_assign(
              key(place),
              Root{ir.CreateStructGEP(
                       session.layouts().get(type).storage, address,
                       session.layouts().fieldIndex(type, field.symbol)),
                   field.type});
}
llvm::Value *Places::loan(MirLoanId id) {
  return ir.CreateLoad(session.layouts().pointer(), loans.at(id));
}
void Places::publish(MirLoanId id, llvm::Value *address) {
  ir.CreateStore(address, loans.at(id));
}
void Places::parameter(HirBindingId binding, llvm::Value *argument,
                       bool indirect) {
  std::optional<std::size_t> packElement;
  for (const auto &place : body.places) {
    if (place.root != MirPlaceRootKind::Binding || place.binding != binding ||
        !place.projections.empty())
      continue;
    auto &target = roots.at(key(place));
    if (target.type.kind == SemanticType::TypePack) {
      const auto index = packNext[key(place)]++;
      const auto &element = target.type.arguments.at(index);
      auto *address = addressValue(session.program().mir(), element) && indirect
                          ? argument
                          : slot(session.layouts().get(element).storage);
      if (address != argument)
        store(indirect ? load(argument, element) : argument, address, element);
      packParameters.at(key(place)).at(index) = {address, element};
      packElement = index;
      continue;
    }
    if (addressValue(session.program().mir(), place.type) && indirect)
      target.address = argument;
    else
      store(indirect ? load(argument, place.type) : argument, target.address,
            place.type);
  }
  for (const auto &entry : body.loans)
    if (entry.entry && entry.kind != MirLoanKind::Stored &&
        std::find(entry.carriers.begin(), entry.carriers.end(), binding) !=
            entry.carriers.end())
      if (!packElement || [&] {
            const auto *source = body.findPlace(entry.source);
            return source && !source->projections.empty() &&
                   source->projections.front().kind ==
                       MirProjectionKind::PackElement &&
                   source->projections.front().constantIndex == packElement;
          }())
        publish(entry.id, argument);
}
Places::Root Places::root(const MirPlace &place) {
  if (auto found = roots.find(key(place)); found != roots.end())
    return found->second;
  if (place.root == MirPlaceRootKind::Loan)
    return {loan(place.loan), loanTypes.at(place.loan)};
  if (place.root == MirPlaceRootKind::This && receiverAddress)
    return {receiverAddress, receiverType};
  if (place.root == MirPlaceRootKind::Value) {
    const auto &type = body.findValue(place.value)->info.type;
    auto *value = values.at(place.value);
    if (!value)
      throw std::logic_error("LLVM place root has no dominating value");
    // A place-category Load is still a loaded value (notably a raw pointer).
    // Reference-binding extractions carry the selected referent address.
    if (selectedValues.at(place.value))
      return {value, type};
    return {arrayValue(value, type), type};
  }
  throw BackendUnsupportedError("LLVM place root has no admitted storage");
}
llvm::Value *Places::arrayValue(llvm::Value *value, const SemanticType &type) {
  if (addressValue(session.program().mir(), type))
    return value;
  auto *address = slot(value->getType());
  ir.CreateStore(value, address);
  return address;
}
llvm::Value *Places::advance(llvm::Value *address, const SemanticType &pointee,
                             llvm::Value *offset, bool signedOffset,
                             bool subtract) {
  offset = ir.CreateIntCast(offset, ir.getInt64Ty(), signedOffset);
  if (subtract)
    offset = ir.CreateNeg(offset);
  return ir.CreateGEP(session.layouts().get(pointee).storage, address, offset);
}
llvm::Value *Places::difference(llvm::Value *left, llvm::Value *right,
                                const SemanticType &pointee) {
  if (!session.layouts().get(pointee).size)
    throw BackendUnsupportedError(
        "LLVM pointer difference requires nonzero element storage");
  auto *bytes = ir.CreateSub(ir.CreatePtrToInt(left, ir.getInt64Ty()),
                             ir.CreatePtrToInt(right, ir.getInt64Ty()));
  return ir.CreateSDiv(bytes, ir.getInt64(session.layouts().get(pointee).size));
}
llvm::Value *Places::index(llvm::Value *address, const SemanticType &type,
                           llvm::Value *offset, bool checked) {
  if (type.kind == SemanticType::Array) {
    if (checked)
      guard(ir.CreateICmpUGE(offset, llvm::ConstantInt::get(offset->getType(),
                                                            type.arrayLength)),
            {DefinedFailureCode::IndexOutOfBounds,
             DefinedFailureDetail::FixedArray});
    return ir.CreateGEP(session.layouts().get(type).storage, address,
                        {ir.getInt64(0), offset});
  }
  if (type.kind == SemanticType::StringView) {
    auto *view = load(address, type);
    if (checked)
      guard(ir.CreateICmpUGE(offset, ir.CreateExtractValue(view, 1)),
            {DefinedFailureCode::IndexOutOfBounds,
             DefinedFailureDetail::StringView});
    return ir.CreateGEP(ir.getInt8Ty(), ir.CreateExtractValue(view, 0), offset);
  }
  throw BackendUnsupportedError(
      "LLVM checked indexing requires an array or text view");
}
llvm::Value *Places::address(MirPlaceId id) {
  const auto &place = *body.findPlace(id);
  auto [address, type] = root(place);
  for (const auto &projection : place.projections) {
    switch (projection.kind) {
    case MirProjectionKind::PackElement: {
      if (type.kind != SemanticType::TypePack || !type.concretePack ||
          !projection.constantIndex)
        throw std::logic_error(
            "LLVM pack projection lost its concrete parameter");
      const auto &selected =
          packParameters.at(key(place)).at(*projection.constantIndex);
      if (!selected.address)
        throw std::logic_error(
            "LLVM pack element has no incoming ABI argument");
      address = selected.address;
      type = selected.type;
      break;
    }
    case MirProjectionKind::Field: {
      const auto &owner = session.layouts().instance(type);
      const auto index = session.layouts().fieldIndex(type, projection.field);
      const auto field = owner.declaredFields.at(index - owner.bases.size());
      if (!owner.unionLayout)
        address = ir.CreateStructGEP(session.layouts().get(type).storage,
                                     address, index);
      type = field.type;
      break;
    }
    case MirProjectionKind::Base: {
      const auto &owner = session.layouts().instance(type);
      const auto index =
          session.layouts().baseIndex(type, projection.selection);
      address = ir.CreateStructGEP(session.layouts().get(type).storage, address,
                                   index);
      type = owner.bases.at(index).type;
      break;
    }
    case MirProjectionKind::Dereference:
    case MirProjectionKind::RawDereference:
      address = load(address, type);
      {
        auto element = type.arguments.at(0);
        type = std::move(element);
      }
      break;
    case MirProjectionKind::Index:
    case MirProjectionKind::RawIndex: {
      auto *offset = projection.index ? values.at(projection.index)
                     : projection.constantIndex
                         ? ir.getInt64(*projection.constantIndex)
                         : nullptr;
      if (!offset)
        throw std::logic_error("LLVM projection lost its evaluated MIR index");
      const auto domain = projection.index
                              ? constantIntegerDomain(
                                    body.findValue(projection.index)->info.type)
                              : std::nullopt;
      offset = ir.CreateIntCast(offset, ir.getInt64Ty(),
                                domain && domain->signedValue);
      if (projection.kind == MirProjectionKind::RawIndex) {
        address =
            ir.CreateGEP(session.layouts().get(type.arguments.at(0)).storage,
                         load(address, type), offset);
      } else {
        address = index(address, type, offset, true);
      }
      auto element = type.kind == SemanticType::StringView
                         ? SemanticType::Char
                         : type.arguments.at(0);
      type = std::move(element);
      break;
    }
    default:
      throw BackendUnsupportedError(
          "LLVM class/pack/base projection is not implemented");
    }
  }
  return address;
}
llvm::Value *Places::selected(const MirOperand &operand) {
  if (operand.place) {
    auto *result = address(operand.place);
    const auto &type = body.findPlace(operand.place)->type;
    return type.kind == SemanticType::Reference ? load(result, type) : result;
  }
  if (operand.loan)
    return loan(operand.loan);
  if (operand.value)
    return values.at(operand.value);
  throw std::logic_error("LLVM selected address has no verified source");
}

} // namespace lang::native_llvm
