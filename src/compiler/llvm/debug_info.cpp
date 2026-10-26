#include "debug_info.h"
#include "layout.h"

#include <llvm/ADT/APSInt.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Metadata.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <tuple>

namespace lang::native_llvm {

class DebugInfo::Impl {
public:
  explicit Impl(Session &session)
      : session(session), builder(session.module()) {
    const auto &program = session.program();
    const auto remember = [&](SourceUnitId unit, const SourceSpan &span) {
      const auto *source = program.mir().failureMetadata().findSource(unit);
      if (!source || span.source.empty())
        return;
      auto [entry, inserted] =
          logical.emplace(span.source, source->logicalName);
      if (!inserted && entry->second != source->logicalName)
        throw std::logic_error("LLVM debug source identity is ambiguous");
    };
    for (const auto &symbol : program.symbols()) {
      remember(symbol.sourceUnit, symbol.nameSource);
      remember(symbol.sourceUnit, symbol.declarationSource);
      if (symbol.definitionSource)
        remember(symbol.sourceUnit, *symbol.definitionSource);
    }
    for (const auto &row : program.functionInstances())
      remember(row.sourceUnit, row.source);
    for (const auto &row : program.constructorInstances())
      remember(row.sourceUnit, row.source);
    for (const auto &row : program.destructorInstances())
      remember(row.sourceUnit, row.source);
    for (const auto &row : program.classInstances())
      remember(row.sourceUnit, row.source);
    for (const auto &body : program.bodies())
      if (body.sourceDebug.tables)
        for (const auto &scope : body.sourceDebug.tables->scopes)
          remember(scope.sourceUnit, scope.source);
    auto *primary = builder.createFile("<gti>", ".");
    // GTI is not C++. Source expression evaluation needs a debugger language
    // plugin; field/local inspection and line stepping do not require one.
    unit = builder.createCompileUnit(llvm::dwarf::DW_LANG_lo_user, primary,
                                     "GTI native LLVM", optimized(), "", 0);
    session.module().addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                                   llvm::DEBUG_METADATA_VERSION);
    session.module().addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
  }

  bool sourceLifecycle(MirBodyAddress address) const {
    const auto &program = session.program();
    if (address.kind == MirBodyKind::Constructor) {
      const auto *mir = program.mir().findConstructorInstance(address.owner);
      if (!mir)
        throw std::logic_error("LLVM debug constructor is not in the snapshot");
      if (mir->definitionKind != MirDefinitionKind::Source ||
          mir->generatedDefault)
        return false;
      if (!program.findConstructorInstance(address.owner))
        throw std::logic_error(
            "LLVM debug constructor lost its source identity");
      return true;
    }
    if (address.kind == MirBodyKind::Destructor) {
      const auto *mir = program.mir().findDestructorInstance(address.owner);
      if (!mir)
        throw std::logic_error("LLVM debug destructor is not in the snapshot");
      if (mir->definitionKind != MirDefinitionKind::Source)
        return false;
      if (!program.findDestructorInstance(address.owner))
        throw std::logic_error(
            "LLVM debug destructor lost its source identity");
      return true;
    }
    throw std::logic_error(
        "LLVM debug lifecycle registration requires a lifecycle body");
  }

  bool optimized() const {
    return session.options().optimization != LlvmCodegenOptimization::None;
  }

  llvm::DIFile *file(const SourceSpan &span) {
    const auto source = logical.find(span.source);
    if (source == logical.end())
      throw BackendUnsupportedError(
          "LLVM debug source has no sealed logical identity");
    auto [entry, inserted] = files.try_emplace(source->second, nullptr);
    if (inserted)
      entry->second = builder.createFile(source->second, ".");
    return entry->second;
  }

  llvm::DIType *type(const SemanticType &value) {
    if (value == SemanticType::Void)
      return nullptr;
    for (const auto &[previous, metadata] : types)
      if (previous == value)
        return metadata.get();
    llvm::DIType *result = nullptr;
    const auto pointerBits = session.dataLayout().getPointerSizeInBits();
    if (value.kind == SemanticType::RawPointer ||
        value.kind == SemanticType::Reference) {
      llvm::DIType *pointee =
          value.arguments.empty() ? nullptr : type(value.arguments[0]);
      result =
          value.kind == SemanticType::Reference
              ? builder.createReferenceType(llvm::dwarf::DW_TAG_reference_type,
                                            pointee, pointerBits)
              : builder.createPointerType(pointee, pointerBits);
    } else if (value.kind == SemanticType::Array) {
      const auto layout = session.layouts().get(value);
      llvm::Metadata *range = builder.getOrCreateSubrange(0, value.arrayLength);
      result = builder.createArrayType(layout.size * 8, layout.alignment * 8,
                                       type(value.arguments.at(0)),
                                       builder.getOrCreateArray({range}));
    } else if (value.kind == SemanticType::Class) {
      const auto &owner = session.layouts().instance(value);
      const auto *declaration =
          session.program().findClassDeclaration(owner.declaration);
      const auto *instance = session.program().findClassInstance(owner.id);
      if (!declaration || !instance)
        throw std::logic_error(
            "LLVM debug class lost its concrete declaration");
      if (owner.cOpaqueHandle) {
        result = builder.createUnspecifiedType(declaration->qualifiedName);
      } else {
        const auto layout = session.layouts().get(value);
        auto *source = file(instance->source);
        const auto tag = owner.unionLayout ? llvm::dwarf::DW_TAG_union_type
                                           : llvm::dwarf::DW_TAG_structure_type;
        auto *temporary = builder.createReplaceableCompositeType(
            tag, declaration->qualifiedName, unit, source,
            instance->source.line, 0, layout.size * 8, layout.alignment * 8);
        const auto index = types.size();
        types.emplace_back(value,
                           llvm::TypedTrackingMDRef<llvm::DIType>(temporary));
        std::vector<llvm::Metadata *> members;
        for (const auto &base : owner.bases) {
          const auto offset = layout.fieldOffsets.at(
              session.layouts().baseIndex(value, base.instance));
          members.push_back(
              builder.createInheritance(temporary, type(base.type), offset * 8,
                                        0, llvm::DINode::FlagPublic));
        }
        for (const auto &field : owner.declaredFields) {
          const auto *symbol = session.program().findSymbol(field.symbol);
          if (!symbol)
            throw std::logic_error("LLVM debug field lost its symbol");
          const auto fieldLayout = session.layouts().get(field.type);
          const auto offset = layout.fieldOffsets.at(
              session.layouts().fieldIndex(value, field.symbol));
          members.push_back(builder.createMemberType(
              temporary, symbol->name, file(symbol->declarationSource),
              symbol->declarationSource.line, fieldLayout.size * 8,
              fieldLayout.alignment * 8, offset * 8, llvm::DINode::FlagZero,
              type(field.type)));
        }
        auto *complete = owner.unionLayout
                             ? builder.createUnionType(
                                   unit, declaration->qualifiedName, source,
                                   instance->source.line, layout.size * 8,
                                   layout.alignment * 8, llvm::DINode::FlagZero,
                                   builder.getOrCreateArray(members))
                             : builder.createStructType(
                                   unit, declaration->qualifiedName, source,
                                   instance->source.line, layout.size * 8,
                                   layout.alignment * 8, llvm::DINode::FlagZero,
                                   nullptr, builder.getOrCreateArray(members));
        builder.replaceTemporary(llvm::TempDICompositeType(temporary),
                                 complete);
        return types[index].second.get();
      }
    } else if (value.kind == SemanticType::Enum) {
      for (const auto &enumeration : session.program().mir().integralEnums()) {
        if (enumeration.type != value)
          continue;
        const auto *row = session.program().findEnumDeclarationRow(value);
        const auto *declaration = session.program().findEnumDeclaration(value);
        const auto domain = constantIntegerDomain(enumeration.underlyingType);
        if (!declaration || !row || !domain)
          throw std::logic_error("LLVM debug enum lost its declaration");
        std::vector<llvm::Metadata *> enumerators;
        for (const auto &enumerator : declaration->enumerators) {
          llvm::APInt bits(domain->width, enumerator.value.magnitude);
          if (enumerator.value.negative)
            bits = -bits;
          enumerators.push_back(builder.createEnumerator(
              enumerator.name, llvm::APSInt(bits, !domain->signedValue)));
        }
        const auto layout = session.layouts().get(value);
        result = builder.createEnumerationType(
            unit, declaration->qualifiedName, file(row->source),
            row->source.line, layout.size * 8, layout.alignment * 8,
            builder.getOrCreateArray(enumerators),
            type(enumeration.underlyingType));
        break;
      }
    } else if (value == SemanticType::Float || value == SemanticType::Double) {
      result = builder.createBasicType(
          value == SemanticType::Float ? "float" : "double",
          session.layouts().get(value).size * 8, llvm::dwarf::DW_ATE_float);
    } else if (value == SemanticType::Bool || value == SemanticType::Char) {
      result = builder.createBasicType(
          value == SemanticType::Bool ? "bool" : "char",
          session.layouts().get(value).size * 8,
          value == SemanticType::Bool ? llvm::dwarf::DW_ATE_boolean
                                      : llvm::dwarf::DW_ATE_UTF);
    } else if (auto integer = constantIntegerDomain(value)) {
      result = builder.createBasicType(
          (integer->signedValue ? "int" : "uint") +
              std::to_string(integer->width) + "_t",
          session.layouts().get(value).size * 8,
          integer->signedValue ? llvm::dwarf::DW_ATE_signed
                               : llvm::dwarf::DW_ATE_unsigned);
    } else if (value.kind == SemanticType::StringView) {
      const auto layout = session.layouts().get(value);
      auto *storage = llvm::cast<llvm::StructType>(layout.storage);
      const auto *offsets = session.dataLayout().getStructLayout(storage);
      auto *data = builder.createMemberType(
          unit, "data", nullptr, 0, pointerBits, 0,
          offsets->getElementOffsetInBits(0), llvm::DINode::FlagZero,
          builder.createPointerType(type(SemanticType::UInt8), pointerBits));
      auto *size = builder.createMemberType(
          unit, "size", nullptr, 0, 64, 0, offsets->getElementOffsetInBits(1),
          llvm::DINode::FlagZero, type(SemanticType::UInt64));
      result = builder.createStructType(unit, "string_view", nullptr, 0,
                                        layout.size * 8, layout.alignment * 8,
                                        llvm::DINode::FlagZero, nullptr,
                                        builder.getOrCreateArray({data, size}));
    } else if (value.kind == SemanticType::CString ||
               value.kind == SemanticType::NativeFunction) {
      result = builder.createPointerType(nullptr, pointerBits);
    }
    if (!result)
      throw BackendUnsupportedError(
          "LLVM debug type requires its representation milestone");
    types.emplace_back(value, llvm::TypedTrackingMDRef<llvm::DIType>(result));
    return result;
  }

  Session &session;
  llvm::DIBuilder builder;
  llvm::DICompileUnit *unit;
  std::map<std::string, std::string> logical;
  std::map<std::string, llvm::DIFile *> files;
  std::vector<std::pair<SemanticType, llvm::TypedTrackingMDRef<llvm::DIType>>>
      types;
  std::map<llvm::Function *, MirBodyAddress> bodies;
  std::map<std::pair<MirBodyKind, std::uint32_t>, llvm::Function *>
      lifecycleDefinitions;
  struct Local {
    llvm::DILocalVariable *variable = nullptr;
    llvm::Value *storage = nullptr;
  };
  std::map<llvm::Function *, std::vector<llvm::DILocalScope *>> scopes;
  std::map<std::pair<llvm::Function *, SourceLocalId>, Local> locals;
  using Point =
      std::tuple<llvm::Function *, MirBlockId, MirInstructionId, bool, bool>;
  struct Events {
    std::vector<SourceLocalId> ends;
    std::vector<SourceLocalId> begins;
  };
  std::map<Point, Events> events;
  bool finalized = false;
};

DebugInfo::DebugInfo(Session &session)
    : impl(std::make_unique<Impl>(session)) {}
DebugInfo::~DebugInfo() = default;

void DebugInfo::registerLifecycleBody(MirBodyAddress address,
                                      llvm::Function &function) {
  if (function.getParent() != &impl->session.module())
    throw std::logic_error("LLVM debug lifecycle belongs to another session");
  if (!impl->sourceLifecycle(address))
    return;
  const auto key = std::pair(address.kind, address.owner);
  const auto previous = impl->lifecycleDefinitions.find(key);
  if (previous != impl->lifecycleDefinitions.end() &&
      previous->second != &function)
    throw std::logic_error(
        "LLVM debug lifecycle identity changed its factory result");
  const auto other = impl->bodies.find(&function);
  if (other != impl->bodies.end() && other->second != address)
    throw std::logic_error("LLVM debug function is bound to another body");
  impl->lifecycleDefinitions.emplace(key, &function);
  impl->bodies.emplace(&function, address);
}

void DebugInfo::begin(MirBodyAddress address, llvm::Function &function) {
  if (function.getParent() != &impl->session.module())
    throw std::logic_error(
        "LLVM debug function belongs to another emission session");
  const auto &program = impl->session.program();
  if (address.kind == MirBodyKind::Constructor ||
      address.kind == MirBodyKind::Destructor) {
    if (!impl->sourceLifecycle(address))
      return;
    const auto found =
        impl->lifecycleDefinitions.find(std::pair(address.kind, address.owner));
    if (found == impl->lifecycleDefinitions.end() || found->second != &function)
      throw std::logic_error(
          "LLVM debug lifecycle does not match its factory identity");
  }
  const auto *body = findMirBody(program.mir(), address);
  if (!body)
    throw std::logic_error("LLVM debug body does not belong to the snapshot");
  // Generated glue has no source scope, even when its result type has no
  // source debug representation. Exclude it before planning source types.
  if (address.kind != MirBodyKind::Function &&
      address.kind != MirBodyKind::Constructor &&
      address.kind != MirBodyKind::Destructor)
    return;
  const SourceSpan *source = nullptr;
  std::string name;
  std::vector<llvm::Metadata *> parameters{impl->type(body->returnType)};
  if (address.kind == MirBodyKind::Function) {
    const auto *instance = program.findFunctionInstance(address.owner);
    const auto *mir = program.mir().findFunctionInstance(address.owner);
    if (!instance || !mir)
      throw std::logic_error("LLVM debug function lost its exact identity");
    if (mir->definitionKind != MirDefinitionKind::Source)
      return;
    if (!impl->session.hasSourceDefinition(address.owner, function))
      throw std::logic_error(
          "LLVM debug function does not match its source ABI identity");
    source = &instance->source;
    name =
        program.findFunctionDeclaration(instance->declaration)->qualifiedName;
    for (const auto &parameter : mir->parameterTypes)
      parameters.push_back(impl->type(parameter));
  } else if (address.kind == MirBodyKind::Constructor) {
    const auto *instance = program.findConstructorInstance(address.owner);
    if (!instance)
      throw std::logic_error("LLVM debug constructor lost its exact identity");
    source = &instance->source;
    const auto *owner = program.findClassInstance(instance->owner);
    name = program.findClassDeclaration(owner->declaration)->qualifiedName +
           "::constructor";
    for (const auto &parameter : instance->parameterTypes)
      parameters.push_back(impl->type(parameter));
  } else if (address.kind == MirBodyKind::Destructor) {
    const auto *instance = program.findDestructorInstance(address.owner);
    if (!instance)
      throw std::logic_error("LLVM debug destructor lost its exact identity");
    source = &instance->source;
    const auto *owner = program.findClassInstance(instance->owner);
    name = program.findClassDeclaration(owner->declaration)->qualifiedName +
           "::destructor";
  } else {
    return; // Generated glue has no manufactured source scope or location.
  }
  auto flags = llvm::DISubprogram::SPFlagDefinition;
  if (impl->optimized())
    flags |= llvm::DISubprogram::SPFlagOptimized;
  auto *file = impl->file(*source);
  // GTI's private execution symbols are not source-language manglings. A
  // linkage name makes debuggers prefer those opaque symbols to this name.
  function.setSubprogram(impl->builder.createFunction(
      file, name, {}, file, source->line,
      impl->builder.createSubroutineType(
          impl->builder.getOrCreateTypeArray(parameters)),
      source->line, llvm::DINode::FlagPrototyped, flags));
  impl->bodies.emplace(&function, address);
  const auto *lowered = program.findBody(address);
  if (!lowered || !lowered->sourceDebug.tables)
    throw std::logic_error(
        "LLVM source debug requires collected lowered facts");
  auto &scopes = impl->scopes[&function];
  scopes.push_back(function.getSubprogram());
  for (const auto &scope : lowered->sourceDebug.tables->scopes) {
    if (scope.id != scopes.size() || scope.parent >= scopes.size())
      throw std::logic_error(
          "LLVM lexical scope lost its sealed parent identity");
    scopes.push_back(
        scope.id == 1 && !scope.parent
            ? static_cast<llvm::DILocalScope *>(function.getSubprogram())
            : impl->builder.createLexicalBlock(scopes[scope.parent],
                                               impl->file(scope.source),
                                               scope.source.line, 0));
  }
  const std::vector<HirBindingId> *parameterBindings = nullptr;
  if (address.kind == MirBodyKind::Function)
    parameterBindings =
        &program.mir().findFunctionInstance(address.owner)->parameterBindings;
  else if (address.kind == MirBodyKind::Constructor)
    parameterBindings = &program.mir()
                             .findConstructorInstance(address.owner)
                             ->parameterBindings;
  for (const auto &local : lowered->sourceDebug.tables->locals) {
    if (local.policy != MirSourceLocalPolicy::OptimizedOut)
      continue;
    const auto &declaration = local.declaration;
    auto *scope = scopes.at(declaration.scope);
    auto *file = impl->file(declaration.declaration);
    unsigned parameter = 0;
    if (declaration.parameter && parameterBindings) {
      const auto found =
          std::find(parameterBindings->begin(), parameterBindings->end(),
                    declaration.binding);
      if (found != parameterBindings->end())
        parameter =
            static_cast<unsigned>(found - parameterBindings->begin() + 1);
    }
    auto *variable = parameter ? impl->builder.createParameterVariable(
                                     scope, declaration.name, parameter, file,
                                     declaration.declaration.line,
                                     impl->type(declaration.type), true)
                               : impl->builder.createAutoVariable(
                                     scope, declaration.name, file,
                                     declaration.declaration.line,
                                     impl->type(declaration.type), true);
    impl->locals.emplace(std::pair{&function, declaration.id},
                         Impl::Local{variable, nullptr});
  }
  const auto event = [&](MirDebugPoint point) -> Impl::Events & {
    return impl->events[{&function, point.block, point.instruction, point.after,
                         point.blockEntry}];
  };
  for (const auto &range : lowered->sourceDebug.tables->ranges) {
    event(range.begin).begins.push_back(range.local);
    event(range.end).ends.push_back(range.local);
  }
}

void DebugInfo::location(llvm::IRBuilder<> &ir, MirBodyAddress address,
                         MirBlockId block, MirInstructionId instruction) {
  ir.SetCurrentDebugLocation({});
  if (!ir.GetInsertBlock())
    return;
  auto *scope = ir.GetInsertBlock()->getParent()->getSubprogram();
  const auto *body = impl->session.program().findBody(address);
  if (!scope || !body)
    return;
  // Line zero denotes compiler-generated routing, not a fabricated user line.
  ir.SetCurrentDebugLocation(
      llvm::DILocation::get(impl->session.context(), 0, 0, scope));
  for (const auto &row : body->sourceLocations()) {
    if (row.block != block || row.instruction != instruction)
      continue;
    auto *source = impl->file(row.source);
    const auto scopes = impl->scopes.find(ir.GetInsertBlock()->getParent());
    if (scopes == impl->scopes.end() || row.scope >= scopes->second.size())
      throw std::logic_error("LLVM location lost its sealed lexical scope");
    llvm::DILocalScope *selected = scopes->second[row.scope];
    if (source != selected->getFile())
      selected = impl->builder.createLexicalBlockFile(selected, source, 0);
    ir.SetCurrentDebugLocation(llvm::DILocation::get(
        impl->session.context(), row.source.line, 0, selected));
    return;
  }
}

void DebugInfo::location(llvm::IRBuilder<> &ir, MirBlockId block,
                         MirInstructionId instruction) {
  if (!ir.GetInsertBlock())
    return;
  const auto found = impl->bodies.find(ir.GetInsertBlock()->getParent());
  if (found == impl->bodies.end()) {
    ir.SetCurrentDebugLocation({});
    return;
  }
  location(ir, found->second, block, instruction);
}

void DebugInfo::local(llvm::IRBuilder<> &ir, const MirPlace &place,
                      llvm::Value *storage, unsigned parameter) {
  if (!ir.GetInsertBlock() || !storage || !storage->getType()->isPointerTy() ||
      place.root != MirPlaceRootKind::Binding || !place.projections.empty())
    return;
  auto *function = ir.GetInsertBlock()->getParent();
  const auto address = impl->bodies.find(function);
  if (!function->getSubprogram() || address == impl->bodies.end())
    return;
  const auto *body = impl->session.program().findBody(address->second);
  if (!body || !body->sourceDebug.tables)
    return;
  for (const auto &local : body->sourceDebug.tables->locals) {
    if (local.place != place.id ||
        local.policy != MirSourceLocalPolicy::ExistingPlace ||
        local.declaration.type != place.type ||
        local.declaration.symbol != place.symbol)
      continue;
    auto [entry, inserted] =
        impl->locals.try_emplace({function, local.declaration.id});
    if (!inserted) {
      if (entry->second.storage != storage)
        throw std::logic_error("LLVM debug local changed its existing storage");
      continue;
    }
    const auto &declaration = local.declaration;
    const auto &source = declaration.declaration;
    auto *scope = impl->scopes.at(function).at(declaration.scope);
    auto *file = impl->file(source);
    entry->second.storage = storage;
    entry->second.variable =
        parameter && declaration.parameter
            ? impl->builder.createParameterVariable(
                  scope, declaration.name, parameter, file, source.line,
                  impl->type(place.type), true)
            : impl->builder.createAutoVariable(scope, declaration.name, file,
                                               source.line,
                                               impl->type(place.type), true);
  }
}

void DebugInfo::point(llvm::IRBuilder<> &ir, MirDebugPoint point) {
  if (!ir.GetInsertBlock())
    return;
  auto *function = ir.GetInsertBlock()->getParent();
  const auto event =
      impl->events.find({function, point.block, point.instruction, point.after,
                         point.blockEntry});
  if (event == impl->events.end())
    return;
  const auto emit = [&](SourceLocalId id, bool begin) {
    const auto found = impl->locals.find({function, id});
    if (found == impl->locals.end())
      throw std::logic_error(
          "LLVM debug range has no registered existing storage");
    const auto &local = found->second;
    const auto *location = llvm::DILocation::get(
        impl->session.context(), begin ? local.variable->getLine() : 0, 0,
        local.variable->getScope());
    if (begin)
      impl->builder.insertDeclare(local.storage, local.variable,
                                  impl->builder.createExpression(), location,
                                  ir.GetInsertBlock());
    else
      impl->builder.insertDbgValueIntrinsic(
          llvm::PoisonValue::get(ir.getInt8Ty()), local.variable,
          impl->builder.createExpression(), location, ir.GetInsertBlock());
  };
  for (const auto id : event->second.ends)
    emit(id, false);
  for (const auto id : event->second.begins)
    emit(id, true);
}

void DebugInfo::endLocal(llvm::IRBuilder<> &ir, const MirPlace &place) {
  if (!ir.GetInsertBlock())
    return;
  auto *function = ir.GetInsertBlock()->getParent();
  const auto address = impl->bodies.find(function);
  if (address == impl->bodies.end())
    return;
  const auto *body = impl->session.program().findBody(address->second);
  if (!body || !body->sourceDebug.tables)
    return;
  for (const auto &local : body->sourceDebug.tables->locals) {
    if (local.place != place.id)
      continue;
    const auto found = impl->locals.find({function, local.declaration.id});
    if (found != impl->locals.end())
      impl->builder.insertDbgValueIntrinsic(
          llvm::PoisonValue::get(ir.getInt8Ty()), found->second.variable,
          impl->builder.createExpression(),
          llvm::DILocation::get(impl->session.context(), 0, 0,
                                found->second.variable->getScope()),
          ir.GetInsertBlock());
  }
}

void DebugInfo::finalize() {
  if (!impl->finalized) {
    impl->builder.finalize();
    impl->finalized = true;
  }
}

} // namespace lang::native_llvm
