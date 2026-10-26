#include "debug_info.h"
#include "abi.h"
#include "layout.h"
#include "parameter_packs.h"

#include <llvm/ADT/APSInt.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugProgramInstruction.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Transforms/Utils/Local.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
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

  std::vector<std::uint64_t> packOffsets(const SemanticType &pack) {
    if (pack.kind != SemanticType::TypePack || !pack.concretePack)
      throw std::logic_error("LLVM debug pack has no concrete source type");
    std::vector<std::uint64_t> offsets{0};
    for (const auto &element : pack.arguments) {
      const auto bytes = session.layouts().get(element).size;
      if (bytes >
          (std::numeric_limits<std::uint64_t>::max() - offsets.back()) / 8)
        throw BackendUnsupportedError(
            "LLVM debug pack exceeds DWARF size limits");
      offsets.push_back(offsets.back() + bytes * 8);
    }
    return offsets;
  }

  std::vector<SemanticType> sourceParameters(MirBodyAddress address,
                                             llvm::Function &function) {
    const auto &program = session.program().mir();
    const std::vector<SemanticType> *types = nullptr;
    const std::vector<HirBindingId> *bindings = nullptr;
    SemanticType result = SemanticType::Void;
    bool mayFail = false, receiver = false, tracksCaller = false;
    if (address.kind == MirBodyKind::Function) {
      const auto &instance = *program.findFunctionInstance(address.owner);
      types = &instance.parameterTypes;
      bindings = &instance.parameterBindings;
      result = instance.returnType;
      mayFail = instance.mayRaiseDefinedFailure;
      receiver = instance.owner.has_value() && !instance.staticMember;
      tracksCaller = instance.tracksCaller;
    } else if (address.kind == MirBodyKind::Constructor) {
      const auto &instance = *program.findConstructorInstance(address.owner);
      types = &instance.parameterTypes;
      bindings = &instance.parameterBindings;
      mayFail = instance.mayRaiseDefinedFailure;
      receiver = true;
    } else {
      return {};
    }
    auto concrete = planConcreteParameters(*types, *bindings,
                                           *findMirBody(program, address));
    if (!concrete)
      throw std::logic_error(
          "LLVM debug parameters lost their concrete source roles");
    auto abi =
        session.abi().gti(result, *types, mayFail, receiver, tracksCaller);
    if (function.getFunctionType() != abi.type)
      throw std::logic_error(
          "LLVM debug parameters differ from their factory ABI");
    std::vector<SemanticType> source;
    for (const auto binding : *bindings) {
      const auto pack =
          std::find_if(concrete->packs.begin(), concrete->packs.end(),
                       [&](const auto &row) { return row.binding == binding; });
      if (pack != concrete->packs.end()) {
        source.push_back(pack->type);
        continue;
      }
      const auto lane = std::find_if(
          concrete->lanes.begin(), concrete->lanes.end(), [&](const auto &row) {
            return row.binding == binding && !row.packElement;
          });
      if (lane == concrete->lanes.end())
        throw std::logic_error(
            "LLVM debug source parameter has no exact physical role");
      source.push_back(lane->type);
    }
    parameters.emplace(&function,
                       ParameterPlan{std::move(*concrete), std::move(abi)});
    return source;
  }

  llvm::DIType *type(const SemanticType &value) {
    if (value == SemanticType::Void)
      return nullptr;
    for (const auto &[previous, metadata] : types)
      if (previous == value)
        return metadata.get();
    llvm::DIType *result = nullptr;
    const auto pointerBits = session.dataLayout().getPointerSizeInBits();
    if (value.kind == SemanticType::TypePack) {
      // A logical debug grouping, not a contiguous GTI object or ABI layout.
      const auto offsets = packOffsets(value);
      std::vector<llvm::Metadata *> elements;
      for (std::size_t i = 0; i < value.arguments.size(); ++i)
        elements.push_back(builder.createMemberType(
            unit, "[" + std::to_string(i) + "]", nullptr, 0,
            offsets[i + 1] - offsets[i], 8, offsets[i], llvm::DINode::FlagZero,
            type(value.arguments[i])));
      result = builder.createStructType(
          unit,
          "parameter_pack<" + std::to_string(value.arguments.size()) + ">",
          nullptr, 0, offsets.back(), 8, llvm::DINode::FlagArtificial, nullptr,
          builder.getOrCreateArray(elements));
    } else if (value.kind == SemanticType::RawPointer ||
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
  llvm::Function *hostedStartupDefinition = nullptr;
  struct Local {
    llvm::DILocalVariable *variable = nullptr;
    llvm::Value *storage = nullptr;
  };
  std::map<llvm::Function *, std::vector<llvm::DILocalScope *>> scopes;
  std::map<std::pair<llvm::Function *, SourceLocalId>, Local> locals;
  struct ParameterPlan {
    ConcreteParameterPlan concrete;
    FunctionAbi abi;
  };
  std::map<llvm::Function *, ParameterPlan> parameters;
  struct ParameterValue {
    llvm::Value *value = nullptr;
    bool address = false;
  };
  std::map<std::tuple<llvm::Function *, SourceLocalId, std::size_t>,
           ParameterValue>
      parameterValues;
  using Point =
      std::tuple<llvm::Function *, MirBlockId, MirInstructionId, bool, bool>;
  struct Events {
    std::vector<SourceLocalId> ends;
    std::vector<SourceLocalId> begins;
    std::vector<std::pair<SourceLocalId, MirValueId>> values;
    using Fragment = std::tuple<SourceLocalId, MirPlaceId, MirValueId>;
    std::vector<Fragment> fragmentBegins;
    std::vector<Fragment> fragmentEnds;
    using Parameter = std::pair<SourceLocalId, std::size_t>;
    std::vector<Parameter> parameterBegins;
    std::vector<Parameter> parameterEnds;
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

void DebugInfo::registerHostedStartupBody(MirBodyAddress address,
                                          llvm::Function &function) {
  const auto &program = impl->session.program();
  if (function.getParent() != &impl->session.module() ||
      address.kind != MirBodyKind::HostedStartup ||
      !program.mir().hostedStartupPlan() ||
      address.owner != program.mir().hostedStartupPlan()->entry ||
      !program.findBody(address) || !findMirBody(program.mir(), address))
    throw std::logic_error("LLVM debug startup is not an exact session body");
  if (impl->hostedStartupDefinition &&
      impl->hostedStartupDefinition != &function)
    throw std::logic_error("LLVM debug startup changed its factory result");
  const auto other = impl->bodies.find(&function);
  if (other != impl->bodies.end() && other->second != address)
    throw std::logic_error("LLVM debug startup is bound to another body");
  impl->hostedStartupDefinition = &function;
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
  if (address.kind == MirBodyKind::HostedStartup) {
    if (!program.mir().hostedStartupPlan() ||
        address.owner != program.mir().hostedStartupPlan()->entry ||
        impl->hostedStartupDefinition != &function)
      throw std::logic_error(
          "LLVM debug startup does not match its factory identity");
    const auto *lowered = program.findBody(address);
    if (!lowered || lowered->sourceDebug.tables ||
        program.mir().sourceDebugProfile() != SourceDebugProfile::LexicalV1 ||
        lowered->sourceDebug.collection != SourceDebugCollection::NotApplicable)
      throw std::logic_error(
          "LLVM generated startup acquired source debug facts");
    auto flags = llvm::DISubprogram::SPFlagDefinition;
    if (impl->optimized())
      flags |= llvm::DISubprogram::SPFlagOptimized;
    auto *file = impl->builder.createFile("<gti-generated>", ".");
    function.setSubprogram(impl->builder.createFunction(
        file, function.getName(), {}, file, 0,
        impl->builder.createSubroutineType(
            impl->builder.getOrCreateTypeArray({})),
        0, llvm::DINode::FlagArtificial, flags));
    return;
  }
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
    for (const auto &parameter : impl->sourceParameters(address, function))
      parameters.push_back(impl->type(parameter));
  } else if (address.kind == MirBodyKind::Constructor) {
    const auto *instance = program.findConstructorInstance(address.owner);
    if (!instance)
      throw std::logic_error("LLVM debug constructor lost its exact identity");
    source = &instance->source;
    const auto *owner = program.findClassInstance(instance->owner);
    name = program.findClassDeclaration(owner->declaration)->qualifiedName +
           "::constructor";
    for (const auto &parameter : impl->sourceParameters(address, function))
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
  std::set<SourceLocalId> valueLocals;
  std::set<SourceLocalId> packLocals;
  for (const auto &local : lowered->sourceDebug.tables->locals)
    if (local.declaration.parameter &&
        local.declaration.type.kind == SemanticType::TypePack)
      packLocals.insert(local.declaration.id);
  for (const auto &range : lowered->sourceDebug.tables->valueRanges)
    if (impl->optimized() ||
        lowered->sourceDebug.tables->locals.at(range.local - 1).policy ==
            MirSourceLocalPolicy::OptimizedOut)
      valueLocals.insert(range.local);
  if (impl->optimized())
    for (const auto &fragment : lowered->sourceDebug.tables->fragments)
      valueLocals.insert(fragment.local);
  for (const auto &local : lowered->sourceDebug.tables->locals) {
    if (local.policy != MirSourceLocalPolicy::OptimizedOut &&
        !valueLocals.contains(local.declaration.id) &&
        !packLocals.contains(local.declaration.id))
      continue;
    const auto &declaration = local.declaration;
    auto *scope = scopes.at(declaration.scope);
    auto *file = declaration.receiver ? scope->getFile()
                                      : impl->file(declaration.declaration);
    unsigned parameter = 0;
    if (declaration.parameter && parameterBindings) {
      const auto found =
          std::find(parameterBindings->begin(), parameterBindings->end(),
                    declaration.binding);
      if (found != parameterBindings->end())
        parameter =
            static_cast<unsigned>(found - parameterBindings->begin() + 1);
    }
    auto *variable =
        parameter
            ? impl->builder.createParameterVariable(
                  scope, declaration.name, parameter, file,
                  declaration.declaration.line, impl->type(declaration.type),
                  true)
            : impl->builder.createAutoVariable(
                  scope, declaration.name, file, declaration.declaration.line,
                  impl->type(declaration.type), true,
                  declaration.receiver ? llvm::DINode::FlagArtificial
                                       : llvm::DINode::FlagZero);
    impl->locals.emplace(std::pair{&function, declaration.id},
                         Impl::Local{variable, nullptr});
  }
  const auto event = [&](MirDebugPoint point) -> Impl::Events & {
    return impl->events[{&function, point.block, point.instruction, point.after,
                         point.blockEntry}];
  };
  for (const auto &range : lowered->sourceDebug.tables->ranges) {
    if (valueLocals.contains(range.local) || packLocals.contains(range.local))
      continue;
    event(range.begin).begins.push_back(range.local);
    event(range.end).ends.push_back(range.local);
  }
  for (const auto &range : lowered->sourceDebug.tables->valueRanges) {
    if (!valueLocals.contains(range.local))
      continue;
    event(range.begin).values.emplace_back(range.local, range.value);
    event(range.end).ends.push_back(range.local);
  }
  if (impl->optimized())
    for (const auto &fragment : lowered->sourceDebug.tables->fragments) {
      const Impl::Events::Fragment value{fragment.local, fragment.place,
                                         fragment.value};
      event(fragment.begin).fragmentBegins.push_back(value);
      event(fragment.end).fragmentEnds.push_back(value);
    }
  for (const auto &fragment : lowered->sourceDebug.tables->parameterFragments) {
    const Impl::Events::Parameter value{fragment.local, fragment.element};
    event(fragment.begin).parameterBegins.push_back(value);
    event(fragment.end).parameterEnds.push_back(value);
  }
}

void DebugInfo::location(llvm::IRBuilder<> &ir, MirBodyAddress address,
                         MirBlockId block, MirInstructionId instruction) {
  ir.SetCurrentDebugLocation({});
  if (!ir.GetInsertBlock())
    return;
  auto *scope = ir.GetInsertBlock()->getParent()->getSubprogram();
  const auto identity = impl->bodies.find(ir.GetInsertBlock()->getParent());
  if (scope && (identity == impl->bodies.end() || identity->second != address))
    throw std::logic_error(
        "LLVM debug location does not match its factory body");
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
      (place.root != MirPlaceRootKind::Binding &&
       place.root != MirPlaceRootKind::This) ||
      !place.projections.empty())
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
        local.declaration.symbol != place.symbol ||
        local.declaration.receiver != (place.root == MirPlaceRootKind::This))
      continue;
    auto [entry, inserted] =
        impl->locals.try_emplace({function, local.declaration.id});
    if (!inserted) {
      if (entry->second.storage && entry->second.storage != storage)
        throw std::logic_error("LLVM debug local changed its existing storage");
      entry->second.storage = storage;
      continue;
    }
    const auto &declaration = local.declaration;
    const auto &source = declaration.declaration;
    auto *scope = impl->scopes.at(function).at(declaration.scope);
    auto *file = declaration.receiver ? scope->getFile() : impl->file(source);
    entry->second.storage = storage;
    entry->second.variable =
        parameter && declaration.parameter
            ? impl->builder.createParameterVariable(
                  scope, declaration.name, parameter, file, source.line,
                  impl->type(place.type), true)
            : impl->builder.createAutoVariable(
                  scope, declaration.name, file, source.line,
                  impl->type(place.type), true,
                  declaration.receiver ? llvm::DINode::FlagArtificial
                                       : llvm::DINode::FlagZero);
  }
}

void DebugInfo::parameter(llvm::IRBuilder<> &ir, llvm::Argument &argument) {
  if (!ir.GetInsertBlock())
    return;
  auto *function = ir.GetInsertBlock()->getParent();
  if (argument.getParent() != function)
    throw std::logic_error("LLVM debug parameter belongs to another function");
  const auto plan = impl->parameters.find(function);
  if (plan == impl->parameters.end())
    return;
  const auto &physical = plan->second.abi.parameters.at(argument.getArgNo());
  if (physical.channel != AbiChannel::Argument)
    throw std::logic_error("LLVM debug parameter is a hidden ABI channel");
  const auto &lane = plan->second.concrete.lanes.at(physical.sourceIndex);
  if (!lane.packElement)
    return;
  const auto &body =
      *impl->session.program().findBody(impl->bodies.at(function));
  for (const auto &local : body.sourceDebug.tables->locals) {
    const auto &declaration = local.declaration;
    if (!declaration.parameter || declaration.binding != lane.binding ||
        declaration.type.kind != SemanticType::TypePack)
      continue;
    if (*lane.packElement >= declaration.type.arguments.size() ||
        declaration.type.arguments[*lane.packElement] != lane.type ||
        argument.getType() !=
            plan->second.abi.type->getParamType(argument.getArgNo()))
      throw std::logic_error(
          "LLVM debug pack parameter lost its exact ABI type");
    impl->parameterValues.emplace(
        std::tuple{function, declaration.id, *lane.packElement},
        Impl::ParameterValue{&argument, physical.indirect});
  }
}

void DebugInfo::point(llvm::IRBuilder<> &ir, MirDebugPoint point,
                      std::span<llvm::Value *const> values) {
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
    if (std::none_of(event->second.values.begin(), event->second.values.end(),
                     [&](const auto &value) { return value.first == id; }) &&
        std::find(event->second.begins.begin(), event->second.begins.end(),
                  id) == event->second.begins.end())
      emit(id, false);
  for (const auto id : event->second.begins)
    emit(id, true);
  const auto emitParameter = [&](const Impl::Events::Parameter &parameter,
                                 bool begin) {
    const auto [id, element] = parameter;
    const auto &local = impl->locals.at({function, id});
    const auto &tables = *impl->session.program()
                              .findBody(impl->bodies.at(function))
                              ->sourceDebug.tables;
    const auto offsets =
        impl->packOffsets(tables.locals.at(id - 1).declaration.type);
    const auto value = impl->parameterValues.find({function, id, element});
    if (value == impl->parameterValues.end())
      throw std::logic_error(
          "LLVM debug pack has no registered physical parameter");
    auto *expression = impl->builder.createExpression(
        {llvm::dwarf::DW_OP_LLVM_fragment, offsets.at(element),
         offsets.at(element + 1) - offsets.at(element)});
    auto *location = llvm::DILocation::get(
        impl->session.context(), begin ? local.variable->getLine() : 0, 0,
        local.variable->getScope());
    if (begin && value->second.address)
      impl->builder.insertDeclare(value->second.value, local.variable,
                                  expression, location, ir.GetInsertBlock());
    else
      impl->builder.insertDbgValueIntrinsic(
          begin ? value->second.value
                : llvm::PoisonValue::get(value->second.value->getType()),
          local.variable, expression, location, ir.GetInsertBlock());
  };
  for (const auto &parameter : event->second.parameterEnds)
    if (std::find(event->second.parameterBegins.begin(),
                  event->second.parameterBegins.end(),
                  parameter) == event->second.parameterBegins.end())
      emitParameter(parameter, false);
  for (const auto &parameter : event->second.parameterBegins)
    emitParameter(parameter, true);
  const auto fragmentField = [&](MirPlaceId id) {
    const auto address = impl->bodies.at(function);
    const auto *body = findMirBody(impl->session.program().mir(), address);
    const auto *place = body->findPlace(id);
    if (!place || place->root != MirPlaceRootKind::This ||
        place->projections.size() != 1 ||
        place->projections.front().kind != MirProjectionKind::Field)
      throw std::logic_error("LLVM debug fragment lost its exact field place");
    return place->projections.front().field;
  };
  const auto emitFragment = [&](const Impl::Events::Fragment &fragment,
                                bool begin) {
    const auto [id, placeId, valueId] = fragment;
    const auto address = impl->bodies.at(function);
    const auto &program = impl->session.program();
    const auto *body = findMirBody(program.mir(), address);
    const auto &place = *body->findPlace(placeId);
    const auto &declaration = program.findBody(address)
                                  ->sourceDebug.tables->locals.at(id - 1)
                                  .declaration;
    const auto field = fragmentField(placeId);
    const auto layout = impl->session.layouts().get(declaration.type);
    const auto leaf = impl->session.layouts().get(place.type);
    const auto offset = layout.fieldOffsets.at(
        impl->session.layouts().fieldIndex(declaration.type, field));
    if (!leaf.size || offset > layout.size || leaf.size > layout.size - offset)
      throw std::logic_error(
          "LLVM debug fragment exceeds its exact object layout");
    const auto &local = impl->locals.at({function, id});
    auto *value = begin && valueId < values.size() ? values[valueId] : nullptr;
    if (begin &&
        (!value || value->getType() != leaf.value ||
         (llvm::isa<llvm::Instruction>(value) &&
          llvm::cast<llvm::Instruction>(value)->getFunction() != function) ||
         (llvm::isa<llvm::Argument>(value) &&
          llvm::cast<llvm::Argument>(value)->getParent() != function)))
      throw std::logic_error(
          "LLVM debug fragment has no exact existing SSA value");
    impl->builder.insertDbgValueIntrinsic(
        begin ? value : llvm::PoisonValue::get(leaf.value), local.variable,
        impl->builder.createExpression(
            {llvm::dwarf::DW_OP_LLVM_fragment, offset * 8, leaf.size * 8}),
        llvm::DILocation::get(impl->session.context(), 0, 0,
                              local.variable->getScope()),
        ir.GetInsertBlock());
  };
  for (const auto &fragment : event->second.fragmentEnds)
    if (std::none_of(event->second.fragmentBegins.begin(),
                     event->second.fragmentBegins.end(), [&](const auto &next) {
                       return std::get<0>(fragment) == std::get<0>(next) &&
                              fragmentField(std::get<1>(fragment)) ==
                                  fragmentField(std::get<1>(next));
                     }))
      emitFragment(fragment, false);
  for (const auto &fragment : event->second.fragmentBegins)
    emitFragment(fragment, true);
  for (const auto &[id, valueId] : event->second.values) {
    const auto &local = impl->locals.at({function, id});
    const auto address = impl->bodies.at(function);
    const auto &declaration = impl->session.program()
                                  .findBody(address)
                                  ->sourceDebug.tables->locals.at(id - 1)
                                  .declaration;
    auto *value = valueId < values.size() ? values[valueId] : nullptr;
    if (!value ||
        value->getType() !=
            impl->session.layouts().get(declaration.type).value ||
        (llvm::isa<llvm::Instruction>(value) &&
         llvm::cast<llvm::Instruction>(value)->getFunction() != function) ||
        (llvm::isa<llvm::Argument>(value) &&
         llvm::cast<llvm::Argument>(value)->getParent() != function))
      throw std::logic_error(
          "LLVM debug value is missing, mistyped or belongs to another body");
    auto inserted = impl->builder.insertDbgValueIntrinsic(
        value, local.variable, impl->builder.createExpression(),
        llvm::DILocation::get(impl->session.context(), 0, 0,
                              local.variable->getScope()),
        ir.GetInsertBlock());
    // Preserve a pure scalar computation as a debugger expression before
    // machine lowering can discard its transient result. LLVM owns the bit
    // semantics; no load, call, new SSA value or execution use is introduced.
    if (auto *operation = llvm::dyn_cast<llvm::BinaryOperator>(value);
        operation && operation->getType()->isIntegerTy() &&
        operation->getType()->getIntegerBitWidth() <= 64) {
      llvm::SmallVector<uint64_t, 8> operations;
      llvm::SmallVector<llvm::Value *, 2> additional;
      if (llvm::salvageDebugInfoImpl(*operation, 1, operations, additional))
        if (auto *record =
                llvm::dyn_cast_if_present<llvm::DbgRecord *>(inserted))
          llvm::salvageDebugInfoForDbgValues(
              *operation,
#if LLVM_VERSION_MAJOR < 22
              {},
#endif
              {llvm::cast<llvm::DbgVariableRecord>(record)});
    }
  }
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
