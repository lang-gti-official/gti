#include "gti/compiled_scalar_unit.h"
#include "gti/compiled_interface_names.h"
#include "gti/compiled_interface_types.h"
#include "gti/persistent_declaration.h"
#include "lowered_program_closure.h"
#include <algorithm>
#include <array>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>
#include <set>
#include <stdexcept>
namespace lang {
namespace {
constexpr std::array scalarTypes{std::pair{SemanticType::Void, "void"},
                                 std::pair{SemanticType::Int8, "int8_t"},
                                 std::pair{SemanticType::Int16, "int16_t"},
                                 std::pair{SemanticType::Int32, "int32_t"},
                                 std::pair{SemanticType::Int64, "int64_t"},
                                 std::pair{SemanticType::UInt8, "uint8_t"},
                                 std::pair{SemanticType::UInt16, "uint16_t"},
                                 std::pair{SemanticType::UInt32, "uint32_t"},
                                 std::pair{SemanticType::UInt64, "uint64_t"},
                                 std::pair{SemanticType::Float, "float"},
                                 std::pair{SemanticType::Double, "double"},
                                 std::pair{SemanticType::Bool, "bool"},
                                 std::pair{SemanticType::Char, "char"}};
}
bool isCompiledScalarResultType(const SemanticType &type) {
  if (type.kind == SemanticType::Class)
    return type.classId && type == SemanticType::classType(type.classId);
  if (type.kind == SemanticType::Enum)
    return type.enumId && type == SemanticType::enumType(type.enumId);
  if (type.kind == SemanticType::Reference) {
    if (type.arguments.size() != 1 ||
        (type.referenceAccess != AccessMode::ReadOnly &&
         type.referenceAccess != AccessMode::Mutable))
      return false;
    const auto &referent = type.arguments.front();
    return referent.kind != SemanticType::Reference &&
           isCompiledScalarValueType(referent) &&
           type == SemanticType::referenceTo(referent, type.referenceAccess);
  }
  return std::any_of(
      scalarTypes.begin(), scalarTypes.end(),
      [&](const auto &entry) { return type == SemanticType(entry.first); });
}
bool isCompiledScalarValueType(const SemanticType &type) {
  return type != SemanticType::Void && isCompiledScalarResultType(type);
}
std::string_view compiledScalarTypeSpelling(const SemanticType &type) {
  for (const auto &[kind, spelling] : scalarTypes)
    if (type == SemanticType(kind))
      return spelling;
  throw std::invalid_argument("unsupported compiled scalar type");
}
namespace {
std::string resultRecipe(const CompiledScalarSignature &signature) {
  return signature.resultRecipe.empty()
             ? writePersistentType(
                   signature.result,
                   compiledInterfaceSyntheticRegistry(signature.typeMetadata))
             : signature.resultRecipe;
}
void appendContract(std::string &out, std::string_view value) {
  out += std::to_string(value.size()) + ":" + std::string(value);
}
void retainInterfaceUnit(std::vector<CompiledInterfaceUnit> &units,
                         SourceUnitId sourceUnit,
                         const PersistentDeclarationOwner &owner) {
  for (const auto &unit : units)
    if (unit.sourceUnit == sourceUnit && unit.owner == owner)
      return;
  units.push_back({sourceUnit, owner});
  validateCompiledInterfaceUnits(units);
}
std::vector<CompiledInterfaceUnit>
interfaceUnits(std::span<const CompiledScalarImport> inputs) {
  std::vector<CompiledInterfaceUnit> units;
  for (const auto &input : inputs) {
    const auto registry =
        compiledInterfaceSyntheticRegistry(input.signature.typeMetadata);
    retainInterfaceUnit(
        units, input.sourceUnit,
        readPersistentFunctionKey(input.signature.key, registry).owner);
  }
  return units;
}
} // namespace
void validateCompiledBorrowContract(const SemanticType &result,
                                    std::span<const SemanticType> parameters,
                                    const CompiledBorrowContract &borrow) {
  if (result.kind != SemanticType::Reference) {
    if (borrow != CompiledBorrowContract{})
      throw std::invalid_argument("non-reference result has a borrow contract");
    return;
  }
  if (borrow.origin != BorrowOriginKind::Argument ||
      borrow.parameter >= parameters.size() || result.arguments.size() != 1 ||
      (result.referenceAccess != AccessMode::ReadOnly &&
       result.referenceAccess != AccessMode::Mutable) ||
      borrow.access != result.referenceAccess)
    throw std::invalid_argument(
        "compiled reference result requires one argument origin");
  const auto &source = parameters[borrow.parameter];
  if (source.kind != SemanticType::Reference || source.arguments.size() != 1 ||
      (source.referenceAccess != AccessMode::ReadOnly &&
       source.referenceAccess != AccessMode::Mutable) ||
      source.arguments.front() != result.arguments.front() ||
      (borrow.access == AccessMode::Mutable &&
       source.referenceAccess != AccessMode::Mutable))
    throw std::invalid_argument("compiled reference result must borrow its "
                                "whole compatible reference argument");
}
namespace {
std::string callableSource(const CompiledScalarSignature &signature,
                           std::string_view aliasPrefix) {
  const auto registry =
      compiledInterfaceSyntheticRegistry(signature.typeMetadata);
  const auto key = readPersistentFunctionKey(signature.key, registry);
  const auto result = readPersistentType(resultRecipe(signature), registry);
  if (key.sourceLocal || !isCompiledInterfaceName(key.qualifiedName) ||
      !isCompiledScalarResultType(result) ||
      !std::all_of(key.parameters.begin(), key.parameters.end(),
                   isCompiledScalarValueType))
    throw std::invalid_argument("unsupported compiled callable interface");
  validateCompiledBorrowContract(result, key.parameters, signature.borrow);
  std::string out = "export ";
  if (signature.borrow.origin != BorrowOriginKind::None)
    out += "[[borrowed_from(arg" + std::to_string(signature.borrow.parameter) +
           ")]] ";
  out += compiledInterfaceTypeSpelling(result, registry, aliasPrefix) + " " +
         std::string(compiledInterfaceBaseName(key.qualifiedName)) + "(";
  for (std::size_t i = 0; i < key.parameters.size(); ++i) {
    if (i)
      out += ", ";
    out += compiledInterfaceTypeSpelling(key.parameters[i], registry,
                                         aliasPrefix) +
           " arg" + std::to_string(i);
  }
  return compiledInterfaceNamespaceSource(key.qualifiedName, out + ");\n");
}
} // namespace
std::string
compiledInterfaceCallableSource(const CompiledScalarSignature &signature) {
  return callableSource(signature, {});
}
std::string
compiledInterfaceSource(std::span<const CompiledScalarSignature> signatures) {
  std::set<std::string> identifiers, metadata;
  const auto retain = [&](std::string_view name) {
    if (!isCompiledInterfaceName(name))
      throw std::invalid_argument("invalid qualified interface declaration");
    while (true) {
      const auto separator = name.find("::");
      identifiers.emplace(name.substr(0, separator));
      if (separator == name.npos)
        break;
      name.remove_prefix(separator + 2);
    }
  };
  for (const auto &signature : signatures) {
    const auto registry =
        compiledInterfaceSyntheticRegistry(signature.typeMetadata);
    retain(readPersistentFunctionKey(signature.key, registry).qualifiedName);
    if (metadata.insert(signature.typeMetadata).second)
      for (const auto &name :
           compiledInterfaceDeclarationNames(signature.typeMetadata))
        retain(name);
  }
  // Private aliases are ordinary frontend declarations. Select their prefix
  // against the complete facade inventory, including namespace components,
  // so no relative lookup or user-chosen namespace can capture them.
  std::string prefix;
  for (std::size_t candidate = 0;; ++candidate) {
    prefix = "gtilib_type_" + std::to_string(candidate) + "_";
    const auto next = identifiers.lower_bound(prefix);
    if (next == identifiers.end() || !next->starts_with(prefix))
      break;
  }
  std::string source;
  for (const auto &bytes : metadata)
    source += compiledInterfaceTypeSource(bytes, prefix);
  for (const auto &signature : signatures)
    source += callableSource(signature, prefix);
  return source;
}
std::string compiledScalarSymbol(const CompiledScalarSignature &signature) {
  (void)compiledInterfaceCallableSource(signature);
  std::string contract = "gti.callable.3:";
  appendContract(contract, signature.key);
  appendContract(contract, resultRecipe(signature));
  appendContract(contract, signature.typeMetadata);
  appendContract(
      contract, std::to_string(static_cast<unsigned>(signature.borrow.origin)));
  appendContract(contract, std::to_string(signature.borrow.parameter));
  appendContract(
      contract, std::to_string(static_cast<unsigned>(signature.borrow.access)));
  auto digest = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(contract.data()), contract.size()));
  constexpr char hex[] = "0123456789abcdef";
  std::string result = "__gtilib_callable_v3_";
  for (auto b : digest) {
    result += hex[b >> 4];
    result += hex[b & 15];
  }
  return result;
}
std::string compiledScalarSymbol(std::string_view key) {
  return compiledScalarSymbol(
      CompiledScalarSignature{std::string(key), SemanticType::Int32});
}
namespace {
CompiledBorrowContract borrowContract(const MirFunctionInstance &function) {
  if (!function.ownerlessTextParameters.empty() ||
      !function.ownerlessTextFields.empty() ||
      !function.publishedTextFields.empty() || function.dependentTextReturn ||
      function.returnBorrowPlace || function.restoresReferenceResult ||
      !function.restoredBorrowFields.empty() ||
      function.ownedPointeeProjection ||
      !function.returnBorrowParameters.empty())
    throw std::invalid_argument(
        "compiled callable has unsupported ownership dependencies");
  CompiledBorrowContract result;
  if (function.returnType.kind == SemanticType::Reference)
    result = {function.returnBorrowOrigin, function.returnBorrowParameter,
              function.returnBorrowAccess};
  else if (function.returnBorrowOrigin != BorrowOriginKind::None)
    throw std::invalid_argument(
        "compiled value result has unsupported borrowed state");
  validateCompiledBorrowContract(function.returnType, function.parameterTypes,
                                 result);
  const auto simpleMapping = [&](const BorrowedFieldOrigin &mapping) {
    return result.origin == BorrowOriginKind::Argument &&
           mapping.origin == result.origin &&
           mapping.parameter == result.parameter &&
           mapping.access == result.access && !mapping.globalRoot &&
           mapping.resultFields.empty() && mapping.sourceFields.empty() &&
           mapping.resultConditions.empty() && mapping.sourceConditions.empty();
  };
  if (!std::all_of(function.returnBorrowFields.begin(),
                   function.returnBorrowFields.end(), simpleMapping))
    throw std::invalid_argument(
        "compiled borrow field projections require complete metadata");
  if (function.completeReturnBorrow &&
      (!std::all_of(function.completeReturnBorrow->dependencies.begin(),
                    function.completeReturnBorrow->dependencies.end(),
                    simpleMapping) ||
       std::any_of(function.completeReturnBorrow->possibleAlternatives.begin(),
                   function.completeReturnBorrow->possibleAlternatives.end(),
                   [](const auto &path) { return !path.conditions.empty(); })))
    throw std::invalid_argument(
        "compiled conditional borrow dependencies require complete metadata");
  return result;
}
bool scalarFunction(const LoweredProgram &program,
                    const MirFunctionInstance &f) {
  const auto *d = program.findFunctionDeclaration(f.declaration);
  return d && d->explicitlyExported && !f.owner && !d->ownerClass &&
         d->genericParameters.empty() && d->requirements.empty() &&
         !d->hasRequiresClause && !d->parameterPack && !d->internalLinkage &&
         !d->constexprFunction && !d->requiresUnsafe && !d->neverReturns &&
         !d->tracksCaller && !d->compilerPrivate &&
         d->callableParameters.empty() && d->intrinsic == IntrinsicKind::None &&
         d->entryKind == ProgramEntryKind::None &&
         f.linkage == LanguageLinkage::Gti && f.externalSymbol.empty() &&
         isCompiledScalarResultType(f.returnType) &&
         d->requiredParameterCount == d->parameters.size() &&
         std::all_of(f.parameterTypes.begin(), f.parameterTypes.end(),
                     isCompiledScalarValueType);
}
} // namespace
std::vector<CompiledScalarSignature> CompiledScalarUnitBuilder::selectExports(
    const LoweredProgram &program, SourceUnitId entry,
    const PersistentDeclarationOwner &owner,
    std::span<const std::string> functions,
    std::span<const CompiledScalarImport> imports) const {
  auto units = interfaceUnits(imports);
  retainInterfaceUnit(units, entry, owner);
  if (functions.empty() || functions.size() > 256)
    throw std::invalid_argument(
        "compiled library requires 1 to 256 selected functions");
  std::vector<const MirFunctionInstance *> selected;
  std::vector<SemanticType> roots;
  std::set<std::string> names;
  for (const auto &name : functions) {
    if (!names.insert(name).second)
      throw std::invalid_argument(
          "duplicate compiled library function selection");
    bool found = false;
    for (const auto &function : program.mir().functionInstances()) {
      const auto *declaration =
          program.findFunctionDeclaration(function.declaration);
      if (!declaration || declaration->sourceUnit != entry ||
          declaration->qualifiedName != name ||
          !declaration->explicitlyExported)
        continue;
      if (!scalarFunction(program, function))
        throw std::invalid_argument("unsupported compiled function signature");
      found = true;
      selected.push_back(&function);
      roots.push_back(function.returnType);
      roots.insert(roots.end(), function.parameterTypes.begin(),
                   function.parameterTypes.end());
      if (selected.size() > 256)
        throw std::invalid_argument(
            "compiled library exceeds 256 exported overloads");
    }
    if (!found)
      throw std::invalid_argument("compiled library function must be "
                                  "explicitly exported by the root module: " +
                                  name);
  }
  const auto metadata =
      projectCompiledInterfaceTypes(program, entry, owner, roots, units);
  const auto registry =
      compiledInterfaceTypeRegistry(program, entry, metadata, units);
  std::vector<CompiledScalarSignature> signatures;
  for (const auto *function : selected) {
    const auto *declaration =
        program.findFunctionDeclaration(function->declaration);
    PersistentFunctionKey key{.owner = owner,
                              .qualifiedName = declaration->qualifiedName,
                              .parameters = function->parameterTypes};
    signatures.push_back({writePersistentFunctionKey(key, registry),
                          function->returnType, metadata,
                          writePersistentType(function->returnType, registry),
                          borrowContract(*function)});
  }
  std::sort(signatures.begin(), signatures.end(),
            [](const auto &a, const auto &b) { return a.key < b.key; });
  return signatures;
}
std::vector<std::string>
verifyCompiledScalarUnit(const LoweredProgram &program) {
  std::vector<std::string> errors;
  try {
    validateCompiledInterfaceUnits(program.compiledInterfaceUnits());
  } catch (const std::exception &) {
    errors.push_back("invalid compiled interface unit ownership");
  }
  if (program.compiledScalarBindings().empty() &&
      program.compiledInterfaceUnits().empty()) {
    if (program.compiledScalarProducer())
      errors.push_back("compiled library has no exports");
    return errors;
  }
  if (program.compiledScalarBindings().size() > 256 || program.embeddingPlan())
    errors.push_back("invalid compiled scalar unit profile");
  if (program.compiledScalarProducer() &&
      (program.mir().hostedStartupPlan() ||
       !program.mir().programInitializationPlan().steps.empty() ||
       !program.mir().threadLocalStorage().empty()))
    errors.push_back(
        "compiled scalar library requires no entry point or program storage");
  if (program.compiledScalarProducer() &&
      std::none_of(program.compiledScalarBindings().begin(),
                   program.compiledScalarBindings().end(),
                   [](const auto &binding) { return !binding.imported; }))
    errors.push_back("compiled library has no exports");
  std::set<std::string> keys, symbols;
  std::set<std::size_t> ids;
  for (const auto &binding : program.compiledScalarBindings()) {
    const auto *f = program.mir().findFunctionInstance(binding.function);
    if (!f || !scalarFunction(program, *f)) {
      errors.push_back("unsupported compiled function signature");
      continue;
    }
    if ((!binding.imported && !program.compiledScalarProducer()) ||
        f->definitionKind != (binding.imported ? MirDefinitionKind::Declaration
                                               : MirDefinitionKind::Source) ||
        (binding.imported && !f->mayRaiseDefinedFailure))
      errors.push_back("compiled scalar binding direction mismatch or import "
                       "lost failure channel");
    try {
      const auto registry = compiledInterfaceTypeRegistry(
          program, binding.sourceUnit, binding.typeMetadata,
          program.compiledInterfaceUnits(), binding.imported);
      const auto decoded = readPersistentFunctionKey(binding.key, registry);
      if (std::none_of(program.compiledInterfaceUnits().begin(),
                       program.compiledInterfaceUnits().end(),
                       [&](const auto &unit) {
                         return unit.sourceUnit == binding.sourceUnit &&
                                unit.owner == decoded.owner;
                       }))
        errors.push_back("compiled callable has no matching library owner");
      const auto decodedResult =
          readPersistentType(binding.resultRecipe, registry);
      const auto *d = program.findFunctionDeclaration(f->declaration);
      if (decoded.sourceLocal || decoded.qualifiedName != d->qualifiedName ||
          decoded.parameters != f->parameterTypes ||
          binding.result != f->returnType || decodedResult != f->returnType ||
          binding.borrow != borrowContract(*f) || !binding.sourceUnit ||
          binding.sourceUnit != d->sourceUnit ||
          binding.symbol !=
              compiledScalarSymbol({binding.key, binding.result,
                                    binding.typeMetadata, binding.resultRecipe,
                                    binding.borrow}))
        errors.push_back("compiled scalar binding identity mismatch");
    } catch (const std::exception &) {
      errors.push_back("invalid compiled scalar declaration key");
    }
    if (!keys.insert(binding.key).second ||
        !symbols.insert(binding.symbol).second ||
        !ids.insert(binding.function).second)
      errors.push_back("duplicate compiled scalar binding");
  }
  // Open fragment analysis is permitted only for the explicitly bound imports.
  for (const auto &f : program.mir().functionInstances())
    if (f.linkage == LanguageLinkage::Gti &&
        f.definitionKind == MirDefinitionKind::Declaration &&
        !ids.contains(f.id))
      errors.push_back("unbound GTI declaration in compiled scalar unit");
  return errors;
}
LoweredProgram CompiledScalarUnitBuilder::bindScoped(
    LoweredProgram program, std::span<const CompiledScalarImport> exports,
    std::span<const CompiledScalarImport> imports) const {
  if (!verifyLoweredProgram(program).empty())
    throw std::invalid_argument(
        "compiled unit requires verified lowered program");
  if (!program.compiledScalarBindings_.empty() ||
      !program.compiledInterfaceUnits_.empty() ||
      exports.size() + imports.size() > 256)
    throw std::invalid_argument("invalid compiled unit binding request");
  program.compiledScalarProducer_ = !exports.empty();
  program.compiledInterfaceUnits_ = interfaceUnits(imports);
  for (const auto &unit : interfaceUnits(exports))
    retainInterfaceUnit(program.compiledInterfaceUnits_, unit.sourceUnit,
                        unit.owner);
  std::set<std::string> keys;
  const auto bindOne = [&](const CompiledScalarImport &input, bool imported) {
    auto signature = input.signature;
    if (!input.sourceUnit || !keys.insert(signature.key).second)
      throw std::invalid_argument("invalid compiled scalar signature request");
    // Validate the complete wire contract even when reachability eliminated
    // its concrete types and callable. Only fresh-session ID lookup may skip.
    (void)compiledScalarSymbol(signature);
    const auto registry = compiledInterfaceTypeRegistry(
        program, input.sourceUnit, signature.typeMetadata,
        program.compiledInterfaceUnits_, imported);
    if (signature.resultRecipe.empty())
      signature.resultRecipe = writePersistentType(signature.result, registry);
    PersistentFunctionKey record;
    SemanticType result;
    try {
      record = readPersistentFunctionKey(signature.key, registry);
      result = readPersistentType(signature.resultRecipe, registry);
    } catch (const std::invalid_argument &) {
      if (imported)
        return;
      throw;
    }
    validateCompiledBorrowContract(result, record.parameters, signature.borrow);
    const MirFunctionInstance *selected = nullptr;
    for (const auto &f : program.mir().functionInstances()) {
      const auto *d = program.findFunctionDeclaration(f.declaration);
      if (d && d->sourceUnit == input.sourceUnit &&
          d->qualifiedName == record.qualifiedName &&
          f.parameterTypes == record.parameters) {
        if (selected)
          throw std::invalid_argument("ambiguous compiled scalar declaration");
        selected = &f;
      }
    }
    if (!selected && imported)
      return;
    if (!selected)
      throw std::invalid_argument("compiled scalar declaration is absent");
    program.compiledScalarBindings_.push_back(
        {selected->id, signature.key, compiledScalarSymbol(signature), imported,
         result, input.sourceUnit, signature.typeMetadata,
         signature.resultRecipe, signature.borrow});
  };
  for (const auto &input : exports)
    bindOne(input, false);
  for (const auto &input : imports)
    bindOne(input, true);
  for (const auto &function : program.mir().functionInstances())
    if (function.linkage == LanguageLinkage::Gti &&
        function.definitionKind == MirDefinitionKind::Declaration &&
        !program.compiledScalarBinding(function.id))
      throw std::invalid_argument(
          "unbound GTI declaration in compiled scalar unit");
  auto errors = verifyCompiledScalarUnit(program);
  if (!errors.empty())
    throw std::invalid_argument(errors.front());
  if (loweredProgramSealValidation())
    program.constructionSeal_ = lowered_detail::constructionSeal(program);
  return program;
}
LoweredProgram CompiledScalarUnitBuilder::bindExports(
    LoweredProgram program, SourceUnitId root,
    std::span<const CompiledScalarSignature> signatures) const {
  if (signatures.empty())
    throw std::invalid_argument("compiled library has no exports");
  std::vector<CompiledScalarImport> scoped;
  for (const auto &signature : signatures)
    scoped.push_back({signature, root});
  return bindScoped(std::move(program), scoped, {});
}
LoweredProgram CompiledScalarUnitBuilder::bindImports(
    LoweredProgram program,
    std::span<const CompiledScalarImport> imports) const {
  return bindScoped(std::move(program), {}, imports);
}
LoweredProgram CompiledScalarUnitBuilder::bindCombined(
    LoweredProgram program, SourceUnitId root,
    std::span<const CompiledScalarSignature> exports,
    std::span<const CompiledScalarImport> imports) const {
  if (exports.empty())
    throw std::invalid_argument("compiled library has no exports");
  std::vector<CompiledScalarImport> scoped;
  for (const auto &signature : exports)
    scoped.push_back({signature, root});
  return bindScoped(std::move(program), scoped, imports);
}
LoweredProgram
CompiledScalarUnitBuilder::bind(LoweredProgram program,
                                std::span<const std::string> keys,
                                bool producer) const {
  std::vector<CompiledScalarImport> scoped;
  for (const auto &key : keys) {
    const auto record = readPersistentFunctionKey(key);
    SourceUnitId source = 0;
    for (const auto &declaration : program.declarations()) {
      const auto *d =
          std::get_if<LoweredFunctionDeclaration>(&declaration.payload);
      if (!d || d->qualifiedName != record.qualifiedName ||
          d->parameters.size() != record.parameters.size())
        continue;
      bool matches = true;
      for (std::size_t i = 0; i < d->parameters.size(); ++i)
        matches &= d->parameters[i].type == record.parameters[i];
      if (!matches)
        continue;
      if (source && source != d->sourceUnit)
        throw std::invalid_argument("ambiguous compiled scalar source unit");
      source = d->sourceUnit;
    }
    if (!source)
      throw std::invalid_argument("compiled scalar declaration is absent");
    scoped.push_back({{key, SemanticType::Int32}, source});
  }
  return producer ? bindScoped(std::move(program), scoped,
                               std::span<const CompiledScalarImport>{})
                  : bindScoped(std::move(program),
                               std::span<const CompiledScalarImport>{}, scoped);
}
} // namespace lang
