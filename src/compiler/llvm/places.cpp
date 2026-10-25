#include "places.h"
#include "placement.h"

#include <algorithm>

namespace lang::native_llvm {

Places::Places(Session &session, llvm::IRBuilder<> &ir, const MirBody &body,
               std::vector<llvm::Value *> &values, Scalars::Guard guard)
    : session(session), ir(ir), body(body), values(values),
      guard(std::move(guard)), loans(body.loans.size() + 1),
      loanTypes(body.loans.size() + 1) {}

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
    for (const auto &instruction : block.instructions)
      if (!instruction.tombstone && instruction.loan &&
          (instruction.kind == MirInstructionKind::Call ||
           instruction.kind == MirInstructionKind::Borrow))
        loanTypes.at(*instruction.loan) = instruction.info.type;
}
llvm::Value *Places::load(llvm::Value *address, const SemanticType &type) {
  if (addressValue(type))
    return address;
  auto *value = ir.CreateLoad(session.layouts().get(type).storage, address);
  return type == SemanticType::Bool ? ir.CreateTrunc(value, ir.getInt1Ty())
                                    : value;
}
void Places::store(llvm::Value *value, llvm::Value *address,
                   const SemanticType &type) {
  if (addressValue(type)) {
    if (containsHierarchyStorage(session.program().mir(), type))
      throw BackendUnsupportedError(
          "LLVM hierarchy storage requires its exact fresh destination, not "
          "object-byte transport");
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
  for (const auto &place : body.places) {
    if (place.root != MirPlaceRootKind::Binding || place.binding != binding ||
        !place.projections.empty())
      continue;
    auto &target = roots.at(key(place));
    if (addressValue(place.type) && indirect)
      target.address = argument;
    else
      store(indirect ? load(argument, place.type) : argument, target.address,
            place.type);
  }
  for (const auto &entry : body.loans)
    if (entry.entry && entry.kind != MirLoanKind::Stored &&
        std::find(entry.carriers.begin(), entry.carriers.end(), binding) !=
            entry.carriers.end())
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
    return {arrayValue(value, type), type};
  }
  throw BackendUnsupportedError("LLVM place root has no admitted storage");
}
llvm::Value *Places::arrayValue(llvm::Value *value, const SemanticType &type) {
  if (addressValue(type))
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
    case MirProjectionKind::Field: {
      const auto field =
          session.layouts().fieldSubobject(type, projection.field);
      address = baseAddress(address, type, field.owner);
      address =
          ir.CreateStructGEP(session.layouts().get(field.owner.type).storage,
                             address, field.index);
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
  llvm::Value *result = nullptr;
  SemanticType source = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  if (operand.place) {
    const auto *place = body.findPlace(operand.place);
    if (!place)
      throw std::logic_error("LLVM selected address lost its sealed place");
    result = address(operand.place);
    source = place->type;
    access = place->access;
    if (source.kind == SemanticType::Reference)
      result = load(result, source);
  } else if (operand.loan) {
    const auto *record = body.findLoan(operand.loan);
    if (!record)
      throw std::logic_error("LLVM selected address lost its sealed loan");
    result = loan(operand.loan);
    source = loanTypes.at(operand.loan);
    access = record->access;
  } else if (operand.value) {
    const auto *value = body.findValue(operand.value);
    if (!value)
      throw std::logic_error("LLVM selected address lost its sealed value");
    result = values.at(operand.value);
    source = value->info.type;
    access = value->info.access;
  } else {
    throw std::logic_error("LLVM selected address has no verified source");
  }
  if (!result)
    throw std::logic_error("LLVM selected address has no dominating value");
  if (source.kind == SemanticType::Reference) {
    access = source.referenceAccess;
    auto referent = source.arguments.at(0);
    source = std::move(referent);
  }
  auto target = operand.type;
  const bool mutableTarget = operand.kind == MirOperandKind::BorrowWrite ||
                             (target.kind == SemanticType::Reference &&
                              target.referenceAccess == AccessMode::Mutable);
  if (target.kind == SemanticType::Reference) {
    auto referent = target.arguments.at(0);
    target = std::move(referent);
  }
  if (source.kind == SemanticType::Class ||
      target.kind == SemanticType::Class) {
    if (source.kind != SemanticType::Class ||
        target.kind != SemanticType::Class ||
        (mutableTarget && access != AccessMode::Mutable))
      throw BackendUnsupportedError(
          "LLVM class reference adjustment has incompatible type or access");
    result = baseAddress(result, source,
                         session.layouts().baseSubobject(source, target));
  }
  return result;
}

llvm::Value *Places::baseAddress(llvm::Value *address, SemanticType source,
                                 const BaseSubobjectLayout &path) {
  for (const auto index : path.indices) {
    address = ir.CreateStructGEP(session.layouts().get(source).storage, address,
                                 index);
    source = session.layouts().instance(source).bases.at(index).type;
  }
  if (source != path.type)
    throw std::logic_error("LLVM base projection lost its sealed target");
  return address;
}

} // namespace lang::native_llvm
