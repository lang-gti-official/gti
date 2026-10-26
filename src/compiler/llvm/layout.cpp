#include "layout.h"
#include "closure_environment.h"
#include "memory_types.h"
#include "placement.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace lang::native_llvm {

Layouts::Layouts(Session &session) : session(session) {
  textView_ = llvm::StructType::create(
      session.context(), {pointer(), llvm::Type::getInt64Ty(session.context())},
      "gti.c.text_view.v1");
  for (const auto &instance : session.program().mir().classInstances()) {
    records.emplace(
        instance.id,
        Record{&instance, llvm::StructType::create(
                              session.context(),
                              "gti.type.v1.c" + std::to_string(instance.id))});
    classes[instance.declaration].push_back(instance.id);
  }
  for (const auto &instance : session.program().mir().lambdaInstances())
    lambdas.emplace(
        instance.id,
        LambdaRecord{&instance,
                     llvm::StructType::create(
                         session.context(),
                         "gti.lambda.v1.l" + std::to_string(instance.id))});
}

llvm::PointerType *Layouts::pointer() const {
  return llvm::PointerType::get(session.context(), 0);
}

const SharedOwnerLayout &Layouts::sharedOwner() {
  if (!sharedOwner_) {
    auto *handle =
        llvm::StructType::create(session.context(), {pointer(), pointer()},
                                 "__gti_native_shared_handle_v1");
    auto *block = llvm::StructType::create(
        session.context(),
        {llvm::Type::getInt64Ty(session.context()), pointer()},
        "__gti_native_shared_block_v1");
    sharedOwner_ =
        SharedOwnerLayout{describe(handle, true, pointer()), describe(block)};
  }
  return *sharedOwner_;
}

PolymorphicStorageLayout Layouts::polymorphicStorage(const SemanticType &type) {
  const auto &owner = instance(type);
  if (!polymorphicStorageRepresentation(session.program().mir(), owner))
    throw BackendUnsupportedError(
        "LLVM inline polymorphic storage requires its exact sealed capability");
  auto &entry = records.at(owner.id);
  const auto capacity = type.valueArguments[0].value;
  const auto alignment = type.valueArguments[1].value;
  if (!entry.layout) {
    auto *byte = llvm::Type::getInt8Ty(session.context());
    // A zero-length alignment anchor carries no payload bytes or SIMD
    // operation. Unlike an alloca-only alignment override, its DataLayout
    // alignment survives containment in records and arrays.
    auto *anchor = llvm::FixedVectorType::get(byte, alignment);
    if (session.dataLayout().getABITypeAlign(anchor).value() != alignment)
      throw BackendUnsupportedError(
          "LLVM target cannot represent the requested inline slot alignment");
    auto *payload = llvm::StructType::get(session.context(),
                                          {llvm::ArrayType::get(byte, capacity),
                                           llvm::ArrayType::get(anchor, 0)});
    entry.storage->setBody({pointer(), pointer(), payload});
    entry.layout = describe(entry.storage, true);
  }
  return {*entry.layout, 0, 1, 2, capacity, alignment};
}

TypeLayout Layouts::describe(llvm::Type *storage, bool addressOnly,
                             llvm::Type *value) const {
  const auto &data = session.dataLayout();
  if (storage->isVoidTy())
    return {storage, storage, 0, 1, 0, false, {}};
  if (!storage->isSized())
    throw BackendUnsupportedError(
        "LLVM storage requires a complete sized type");
  const auto size = data.getTypeAllocSize(storage).getFixedValue();
  if (size >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    throw BackendUnsupportedError(
        "LLVM object size exceeds the target addressable extent");
  TypeLayout result{value ? value : storage,
                    storage,
                    size,
                    data.getABITypeAlign(storage).value(),
                    size,
                    addressOnly,
                    {}};
  if (auto *record = llvm::dyn_cast<llvm::StructType>(storage)) {
    const auto *layout = data.getStructLayout(record);
    for (unsigned index = 0; index < record->getNumElements(); ++index)
      result.fieldOffsets.push_back(layout->getElementOffset(index));
  }
  return result;
}

TypeLayout Layouts::get(const SemanticType &type) {
  auto &context = session.context();
  switch (type.kind) {
  case SemanticType::Void:
    return describe(llvm::Type::getVoidTy(context));
  case SemanticType::Bool:
    return describe(llvm::Type::getInt8Ty(context), false,
                    llvm::Type::getInt1Ty(context));
  case SemanticType::Char:
  case SemanticType::Int8:
  case SemanticType::UInt8:
    return describe(llvm::Type::getInt8Ty(context));
  case SemanticType::Int16:
  case SemanticType::UInt16:
    return describe(llvm::Type::getInt16Ty(context));
  case SemanticType::Int32:
  case SemanticType::UInt32:
    return describe(llvm::Type::getInt32Ty(context));
  case SemanticType::Int64:
  case SemanticType::UInt64:
    return describe(llvm::Type::getInt64Ty(context));
  case SemanticType::Float:
    return describe(llvm::Type::getFloatTy(context));
  case SemanticType::Double:
    return describe(llvm::Type::getDoubleTy(context));
  case SemanticType::Reference:
  case SemanticType::RawPointer:
  case SemanticType::NullPtr:
  case SemanticType::CString:
  case SemanticType::NativeFunction:
  case SemanticType::UniqueOwner:
    return describe(pointer());
  case SemanticType::FunctionItem:
    // Its exact declaration/instance identity remains in MIR. The physical
    // value is only a state-free materialization for ordinary value flow.
    return describe(llvm::Type::getInt8Ty(context));
  case SemanticType::Lambda:
    return closure(type);
  case SemanticType::StringView:
    return describe(textView_, true);
  case SemanticType::Array: {
    if (type.arguments.size() != 1 || type.arrayLengthParameterId)
      throw BackendUnsupportedError(
          "LLVM array layout requires a concrete element and extent");
    auto element = get(type.arguments.front());
    if (element.storage->isVoidTy() ||
        (element.size &&
         type.arrayLength > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max()) /
                                element.size))
      throw BackendUnsupportedError(
          "LLVM array extent exceeds the target object-size limit");
    auto result = describe(
        llvm::ArrayType::get(element.storage, type.arrayLength), true,
        addressValue(session.program().mir(), type) ? pointer() : nullptr);
    result.stride = element.size;
    return result;
  }
  case SemanticType::Enum: {
    const auto *declaration = session.program().findEnumDeclaration(type);
    if (!declaration)
      throw std::logic_error("LLVM enum lost its sealed declaration");
    if (declaration->payload)
      return payload(type).type;
    return get(declaration->underlyingType);
  }
  case SemanticType::Class:
    return record(type);
  default:
    throw BackendUnsupportedError(
        "LLVM layout for this type family is not implemented yet");
  }
}

const PayloadLayout &Layouts::payload(const SemanticType &type) {
  const auto *selected = session.program().mir().findPayloadEnum(type);
  if (!selected || selected->variants.empty() ||
      selected->variants.size() >= PayloadLayout::inactive)
    throw BackendUnsupportedError(
        "LLVM payload requires exact bounded alternatives");
  auto &entry = payloads[selected];
  if (entry.layout)
    return *entry.layout;
  if (entry.building)
    throw std::logic_error("recursive inline LLVM payload layout");
  entry.building = true;
  try {
    PayloadLayout result;
    auto *anchor =
        static_cast<llvm::Type *>(llvm::Type::getInt8Ty(session.context()));
    std::uint64_t extent = 1;
    for (const auto &variant : selected->variants) {
      if (variant.variantIndex != result.variants.size())
        throw std::logic_error(
            "LLVM payload lost its sealed alternative index");
      std::vector<llvm::Type *> fields;
      for (const auto &field : variant.payloadTypes)
        fields.push_back(get(field).storage);
      auto *storage = llvm::StructType::get(session.context(), fields);
      const auto layout = describe(storage);
      extent = std::max(extent, layout.size);
      if (layout.alignment >
          session.dataLayout().getABITypeAlign(anchor).value())
        anchor = storage;
      result.variants.push_back(storage);
    }
    const auto anchorSize =
        session.dataLayout().getTypeAllocSize(anchor).getFixedValue();
    auto *overlap = llvm::StructType::get(
        session.context(),
        {anchor, llvm::ArrayType::get(llvm::Type::getInt8Ty(session.context()),
                                      extent - anchorSize)});
    result.storage = llvm::StructType::get(
        session.context(),
        {llvm::Type::getInt32Ty(session.context()), overlap});
    result.type = describe(result.storage, true, pointer());
    entry.layout = std::move(result);
    entry.building = false;
    return *entry.layout;
  } catch (...) {
    entry.building = false;
    throw;
  }
}

TypeLayout Layouts::record(const SemanticType &type) {
  const auto candidates = classes.find(type.classId);
  Record *selected = nullptr;
  if (candidates != classes.end()) {
    for (const auto id : candidates->second) {
      auto &candidate = records.at(id);
      if (candidate.instance->type == type) {
        if (selected)
          throw std::logic_error("ambiguous concrete LLVM record identity");
        selected = &candidate;
      }
    }
  }
  if (!selected)
    throw BackendUnsupportedError(
        "LLVM record layout requires an exact concrete class instance");
  auto &entry = *selected;
  if (entry.layout)
    return *entry.layout;
  if (entry.building)
    throw std::logic_error("recursive by-value LLVM record layout");
  const auto &instance = *entry.instance;
  if (instance.compilerCapability ==
      CompilerCapabilityTypeKind::FunctionPointer) {
    entry.building = true;
    try {
      entry.storage->setBody({pointer()});
      auto result = describe(entry.storage, true, pointer());
      entry.layout = result;
      entry.building = false;
      return result;
    } catch (...) {
      entry.building = false;
      throw;
    }
  }
  if (sharedOwnerRepresentation(session.program().mir(), instance)) {
    entry.layout = sharedOwner().handle;
    return *entry.layout;
  }
  if (polymorphicStorageRepresentation(session.program().mir(), instance))
    return polymorphicStorage(type).type;
  const bool vacant =
      uninitializedStorageRepresentation(session.program().mir(), instance);
  if (instance.cOpaqueHandle ||
      (instance.compilerCapability != CompilerCapabilityTypeKind::None &&
       !vacant))
    throw BackendUnsupportedError(
        "LLVM record representation requires its later "
        "lifecycle/capability/dispatch milestone");
  entry.building = true;
  try {
    std::vector<llvm::Type *> fields;
    if (vacant)
      fields.push_back(get(instance.type.arguments[0]).storage);
    for (const auto &base : instance.bases)
      fields.push_back(get(base.type).storage);
    for (const auto &field : instance.declaredFields)
      fields.push_back(get(field.type).storage);
    if (instance.requiresActiveDropState)
      fields.push_back(llvm::Type::getInt8Ty(session.context()));
    if (dispatchIndex(type))
      fields.push_back(pointer());
    if (instance.unionLayout) {
      const auto &expected = *instance.unionLayout;
      llvm::Type *anchor = llvm::Type::getInt8Ty(session.context());
      for (auto *field : fields)
        if (session.dataLayout().getABITypeAlign(field) >
            session.dataLayout().getABITypeAlign(anchor))
          anchor = field;
      const auto anchorSize =
          session.dataLayout().getTypeAllocSize(anchor).getFixedValue();
      if (anchorSize > expected.sizeBytes ||
          fields.size() != expected.fields.size())
        throw std::logic_error(
            "LLVM union layout disagrees with sealed fields");
      for (std::size_t index = 0; index < fields.size(); ++index) {
        if (session.dataLayout().getTypeAllocSize(fields[index]) !=
                expected.fields[index].sizeBytes ||
            session.dataLayout().getABITypeAlign(fields[index]).value() !=
                expected.fields[index].abiAlignmentBytes)
          throw std::logic_error(
              "LLVM union field layout disagrees with sealed facts");
      }
      fields = {anchor,
                llvm::ArrayType::get(llvm::Type::getInt8Ty(session.context()),
                                     expected.sizeBytes - anchorSize)};
    } else if (fields.empty()) {
      fields.push_back(llvm::Type::getInt8Ty(session.context()));
    }
    std::uint64_t extent = 0;
    constexpr auto limit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    for (auto *field : fields) {
      const auto alignment =
          session.dataLayout().getABITypeAlign(field).value();
      const auto size =
          session.dataLayout().getTypeAllocSize(field).getFixedValue();
      const auto padding = (alignment - extent % alignment) % alignment;
      if (padding > limit - extent || size > limit - extent - padding)
        throw BackendUnsupportedError(
            "LLVM record extent exceeds the target object-size limit");
      extent += padding + size;
    }
    // Validate a temporary literal shape before completing the named type, so
    // a rejected layout never leaves a poisoned reusable cache entry.
    auto result =
        describe(llvm::StructType::get(session.context(), fields), true);
    if (instance.cAbiLayout && !instance.unionLayout) {
      const auto &expected = *instance.cAbiLayout;
      if (result.size != expected.sizeBytes ||
          result.alignment != expected.abiAlignmentBytes ||
          result.fieldOffsets.size() != expected.fields.size())
        throw std::logic_error(
            "LLVM C record layout disagrees with sealed facts");
      for (std::size_t index = 0; index < expected.fields.size(); ++index) {
        const auto field = get(expected.fields[index].type);
        if (result.fieldOffsets[index] != expected.fields[index].offsetBytes ||
            field.size != expected.fields[index].sizeBytes ||
            field.alignment != expected.fields[index].abiAlignmentBytes)
          throw std::logic_error(
              "LLVM C record field layout disagrees with sealed facts");
      }
    }
    if (instance.unionLayout) {
      const auto &expected = *instance.unionLayout;
      if (result.size != expected.sizeBytes ||
          result.alignment != expected.abiAlignmentBytes)
        throw std::logic_error("LLVM union layout disagrees with sealed facts");
      result.fieldOffsets.assign(instance.declaredFields.size(), 0);
    }
    entry.storage->setBody(fields);
    result.storage = entry.storage;
    result.value = pointer();
    entry.layout = result;
    entry.building = false;
    return result;
  } catch (...) {
    // No partially defined record is reusable after a failed planning attempt.
    entry.building = false;
    throw;
  }
}

TypeLayout Layouts::closure(const SemanticType &type) {
  const auto environment =
      closureEnvironmentLayout(session.program().mir(), type);
  if (!environment)
    throw BackendUnsupportedError(
        "LLVM closure layout requires one compatible concrete environment");
  LambdaRecord *selected = nullptr;
  for (auto &[id, candidate] : lambdas) {
    (void)id;
    if (candidate.instance->type == type && !selected)
      selected = &candidate;
  }
  if (!selected)
    throw BackendUnsupportedError(
        "LLVM closure layout requires an exact concrete lambda instance");
  if (selected->layout)
    return *selected->layout;
  if (selected->building)
    throw std::logic_error("recursive by-value LLVM closure layout");
  selected->building = true;
  try {
    std::vector<llvm::Type *> fields;
    fields.reserve(environment->captureTypes.size());
    for (const auto &capture : environment->captureTypes)
      fields.push_back(get(capture).storage);
    if (fields.empty())
      fields.push_back(llvm::Type::getInt8Ty(session.context()));
    selected->storage->setBody(fields);
    auto result = describe(selected->storage, true, pointer());
    selected->layout = result;
    selected->building = false;
    return result;
  } catch (...) {
    selected->building = false;
    throw;
  }
}

unsigned Layouts::captureIndex(const SemanticType &type,
                               std::size_t capture) const {
  const auto environment =
      closureEnvironmentLayout(session.program().mir(), type);
  if (!environment)
    throw BackendUnsupportedError(
        "LLVM closure projection requires one compatible environment");
  if (capture == 0 || capture > environment->captureTypes.size())
    throw std::logic_error("LLVM closure capture index is out of range");
  return static_cast<unsigned>(capture - 1);
}

const MirClassInstance &Layouts::instance(const SemanticType &type) const {
  const MirClassInstance *result = nullptr;
  if (auto found = classes.find(type.classId); found != classes.end())
    for (auto id : found->second)
      if (records.at(id).instance->type == type) {
        if (result)
          throw std::logic_error("ambiguous concrete LLVM class identity");
        result = records.at(id).instance;
      }
  if (!result)
    throw BackendUnsupportedError(
        "LLVM class requires an exact concrete instance");
  return *result;
}
unsigned Layouts::fieldIndex(const SemanticType &type, SymbolId symbol) const {
  const auto &owner = instance(type);
  for (unsigned index = 0; index < owner.declaredFields.size(); ++index)
    if (owner.declaredFields[index].symbol == symbol)
      return static_cast<unsigned>(owner.bases.size()) + index;
  throw std::logic_error("LLVM projection lost its sealed field identity");
}
unsigned Layouts::baseIndex(const SemanticType &type,
                            HirClassInstanceId base) const {
  const auto &owner = instance(type);
  for (unsigned index = 0; index < owner.bases.size(); ++index)
    if (owner.bases[index].instance == base)
      return index;
  throw std::logic_error("LLVM projection lost its sealed base identity");
}
std::optional<unsigned> Layouts::activeIndex(const SemanticType &type) const {
  const auto &owner = instance(type);
  if (!owner.requiresActiveDropState)
    return {};
  return static_cast<unsigned>(owner.bases.size() +
                               owner.declaredFields.size());
}

std::optional<unsigned> Layouts::dispatchIndex(const SemanticType &type) const {
  const auto &owner = instance(type);
  if (!owner.polymorphic)
    return {};
  if (const auto *base = primaryBase(type);
      base && instance(base->type).polymorphic)
    return {};
  return static_cast<unsigned>(owner.bases.size() +
                               owner.declaredFields.size() +
                               (owner.requiresActiveDropState ? 1 : 0));
}

const HirBaseInstance *Layouts::primaryBase(const SemanticType &type) const {
  const HirBaseInstance *selected = nullptr;
  for (const auto &base : instance(type).bases) {
    const auto &concrete = instance(base.type);
    if (concrete.id != base.instance ||
        base.interface != (concrete.kind == ClassKind::Interface))
      throw std::logic_error(
          "LLVM primary chain lost its sealed base identity");
    if (!base.interface) {
      if (selected)
        throw std::logic_error("LLVM primary chain has multiple state bases");
      selected = &base;
    }
  }
  return selected;
}

std::optional<DispatchLocation>
Layouts::dispatchLocation(const SemanticType &type) {
  if (!instance(type).polymorphic)
    return {};
  auto selected = type;
  std::vector<HirClassInstanceId> path;
  std::unordered_set<HirClassInstanceId> seen;
  while (seen.insert(instance(selected).id).second) {
    if (const auto index = dispatchIndex(selected))
      return DispatchLocation{basePath(type, path), *index};
    const auto *base = primaryBase(selected);
    if (!base || !instance(base->type).polymorphic)
      throw std::logic_error(
          "LLVM dispatch pointer has no sealed primary owner");
    path.push_back(base->instance);
    selected = base->type;
  }
  throw std::logic_error("LLVM dispatch primary chain is cyclic");
}

BaseSubobjectLayout
Layouts::basePath(const SemanticType &type,
                  std::span<const HirClassInstanceId> path) {
  BaseSubobjectLayout result{.type = type};
  std::unordered_set<HirClassInstanceId> seen{instance(type).id};
  for (const auto id : path) {
    const auto &owner = instance(result.type);
    const HirBaseInstance *selected = nullptr;
    unsigned index = 0;
    for (unsigned candidate = 0; candidate < owner.bases.size(); ++candidate)
      if (owner.bases[candidate].instance == id) {
        if (selected)
          throw std::logic_error(
              "LLVM base path has ambiguous direct identity");
        selected = &owner.bases[candidate];
        index = candidate;
      }
    if (!selected || !seen.insert(id).second ||
        instance(selected->type).id != id)
      throw std::logic_error("LLVM base path lost its sealed direct identity");
    const auto parent = get(result.type);
    const auto offset = parent.fieldOffsets.at(index);
    constexpr auto limit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (offset > limit || result.offset > limit - offset)
      throw BackendUnsupportedError(
          "LLVM base adjustment exceeds target range");
    result.offset += offset;
    result.indices.push_back(index);
    result.type = selected->type;
  }
  return result;
}

BaseSubobjectLayout Layouts::findSubobject(const SemanticType &source,
                                           HirClassInstanceId target,
                                           SymbolId field) {
  const auto matches = [&](const MirClassInstance &owner) {
    return field != 0           ? std::count_if(owner.declaredFields.begin(),
                                                owner.declaredFields.end(),
                                                [&](const auto &candidate) {
                                        return candidate.symbol == field;
                                                })
           : owner.id == target ? 1
                                : 0;
  };
  const auto &root = instance(source);
  // The ordinary non-inherited record path needs no traversal containers.
  if (root.bases.empty()) {
    if (matches(root) != 1)
      throw std::logic_error(
          "LLVM physical subobject identity is missing or ambiguous");
    return {.type = source};
  }
  std::unordered_set<HirClassInstanceId> seen;
  std::vector<HirClassInstanceId> path;
  std::optional<std::vector<HirClassInstanceId>> selected;
  const auto visit = [&](const auto &self, const SemanticType &type) -> void {
    const auto &owner = instance(type);
    if (!seen.insert(owner.id).second)
      throw std::logic_error("LLVM physical subobject identity is repeated");
    const auto count = matches(owner);
    if (count > 1 || (count == 1 && selected))
      throw std::logic_error("LLVM physical subobject identity is ambiguous");
    if (count == 1)
      selected = path;
    for (const auto &base : owner.bases) {
      if (instance(base.type).id != base.instance)
        throw std::logic_error("LLVM physical base identity is not sealed");
      path.push_back(base.instance);
      self(self, base.type);
      path.pop_back();
    }
  };
  visit(visit, source);
  if (!selected)
    throw std::logic_error("LLVM physical subobject identity is missing");
  return basePath(source, *selected);
}

BaseSubobjectLayout Layouts::baseSubobject(const SemanticType &source,
                                           const SemanticType &target) {
  return findSubobject(source, instance(target).id, 0);
}

FieldSubobjectLayout Layouts::fieldSubobject(const SemanticType &source,
                                             SymbolId field) {
  if (field == 0)
    throw std::logic_error("LLVM field projection has no sealed identity");
  auto owner = findSubobject(source, 0, field);
  const auto &instance = this->instance(owner.type);
  const auto index = fieldIndex(owner.type, field);
  const auto offset = get(owner.type).fieldOffsets.at(index);
  constexpr auto limit =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (offset > limit || owner.offset > limit - offset)
    throw BackendUnsupportedError("LLVM field adjustment exceeds target range");
  FieldSubobjectLayout result{
      .owner = owner,
      .index = index,
      .type = instance.declaredFields.at(index - instance.bases.size()).type,
      .offset = owner.offset + offset};
  return result;
}

} // namespace lang::native_llvm
