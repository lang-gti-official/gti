#include "semantic_analyzer_impl.h"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace lang {
namespace {

SourceSpan requestLocation(const EmbeddingExportRequest &request,
                           const SourceGraph *sourceGraph) {
  if (!request.declaration.source.empty()) {
    return request.declaration;
  }
  const SourceUnit *entry =
      sourceGraph == nullptr ? nullptr
                             : sourceGraph->findUnit(sourceGraph->entryUnit());
  return {entry == nullptr ? std::string("<embedding-request>")
                           : entry->path.string(),
          0, 1, 1};
}

bool ordinaryIdentifier(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  const auto initial = [](unsigned char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') || character == '_';
  };
  const auto continuation = [&](unsigned char character) {
    return initial(character) || (character >= '0' && character <= '9');
  };
  return initial(static_cast<unsigned char>(value.front())) &&
         std::all_of(value.begin() + 1, value.end(), [&](char character) {
           return continuation(static_cast<unsigned char>(character));
         });
}

bool qualifiedFunctionName(std::string_view value) {
  if (value.empty() || value.starts_with("::") || value.ends_with("::")) {
    return false;
  }
  while (!value.empty()) {
    const std::size_t separator = value.find("::");
    if (!ordinaryIdentifier(value.substr(0, separator))) {
      return false;
    }
    if (separator == std::string_view::npos) {
      return true;
    }
    value.remove_prefix(separator + 2);
  }
  return false;
}

bool reservedCOrCppWord(std::string_view value) {
  static constexpr std::string_view words[] = {
      "alignas",       "alignof",
      "and",           "and_eq",
      "asm",           "auto",
      "bitand",        "bitor",
      "bool",          "break",
      "case",          "catch",
      "char",          "char8_t",
      "char16_t",      "char32_t",
      "class",         "compl",
      "concept",       "const",
      "consteval",     "constexpr",
      "constinit",     "const_cast",
      "continue",      "co_await",
      "co_return",     "co_yield",
      "decltype",      "default",
      "delete",        "do",
      "double",        "else",
      "enum",          "explicit",
      "export",        "extern",
      "false",         "float",
      "for",           "friend",
      "goto",          "if",
      "import",        "inline",
      "int",           "long",
      "module",        "mutable",
      "namespace",     "new",
      "noexcept",      "not",
      "not_eq",        "nullptr",
      "operator",      "or",
      "or_eq",         "private",
      "protected",     "public",
      "register",      "requires",
      "restrict",      "return",
      "short",         "signed",
      "sizeof",        "static",
      "static_assert", "struct",
      "switch",        "template",
      "this",          "thread_local",
      "throw",         "true",
      "try",           "typedef",
      "typeid",        "typename",
      "union",         "unsigned",
      "using",         "virtual",
      "void",          "volatile",
      "wchar_t",       "while",
      "xor",           "xor_eq",
      "_Alignas",      "_Alignof",
      "_Atomic",       "_Bool",
      "_Complex",      "_Generic",
      "_Imaginary",    "_Noreturn",
      "_Pragma",       "_Static_assert",
      "_Thread_local"};
  return std::find(std::begin(words), std::end(words), value) !=
         std::end(words);
}

bool reservedEmbeddingSymbol(std::string_view value) {
  static constexpr std::string_view fixed[] = {"gti_get_module_descriptor_v1",
                                               "gti_context_create_v1",
                                               "gti_context_destroy_v1"};
  return value.starts_with('_') || value.find("__") != std::string_view::npos ||
         reservedCOrCppWord(value) ||
         std::find(std::begin(fixed), std::end(fixed), value) !=
             std::end(fixed);
}

void collectFunctions(const StmtList &statements, const TargetInfo &target,
                      std::vector<const FunctionDecl *> &functions) {
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        collectFunctions(*branch, target, functions);
      }
    } else if (const auto *space = stmtAs<NamespaceDecl>(statement.get())) {
      collectFunctions(space->declarations(), target, functions);
    } else if (const auto *external = stmtAs<ExternCDecl>(statement.get())) {
      collectFunctions(external->declarations(), target, functions);
    } else if (const auto *owner = stmtAs<ClassDecl>(statement.get())) {
      collectFunctions(owner->members(), target, functions);
    } else if (const auto *function = stmtAs<FunctionDecl>(statement.get())) {
      functions.push_back(function);
    }
  }
}

} // namespace

auto SemanticVisitor::Impl::applyProgramRequest(const Program &program,
                                                const ProgramRequest &request)
    -> bool {
  semanticModel.setProgramKind(request.kind);
  semanticModel.setEmbeddingPlan(std::nullopt);
  if (request.kind == ProgramKind::Executable) {
    if (!request.exports.empty()) {
      diagnostics.push_back(makeDiagnostic(
          "GTI-S2104", DiagnosticPhase::Semantics,
          requestLocation(request.exports.front(), sourceGraph),
          "Executable programs cannot declare host embedding exports."));
    }
    return !hadError();
  }

  const SourceSpan fallback =
      request.exports.empty()
          ? requestLocation({}, sourceGraph)
          : requestLocation(request.exports.front(), sourceGraph);
  if (request.exports.empty()) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2104", DiagnosticPhase::Semantics, fallback,
        "A library program must select at least one host export.");
    diagnostic.hints.emplace_back(
        "Add a [[targets.<name>.exports]] entry naming one exact qualified "
        "GTI function and C symbol.");
    diagnostics.push_back(std::move(diagnostic));
    return false;
  }

  std::vector<const FunctionDecl *> syntaxFunctions;
  collectFunctions(program.declarations(), target, syntaxFunctions);
  std::vector<const FunctionInfo *> functions;
  functions.reserve(syntaxFunctions.size());
  for (const FunctionDecl *syntax : syntaxFunctions) {
    if (const FunctionInfo *info = semanticModel.findFunction(*syntax)) {
      functions.push_back(info);
    }
  }

  const auto main = std::find_if(
      functions.begin(), functions.end(),
      [](const FunctionInfo *function) { return function->entryPoint; });
  if (main != functions.end()) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2104", DiagnosticPhase::Semantics,
        tokenSpan((*main)->declaration->name()),
        "A library program cannot define the hosted main entry point.");
    diagnostic.hints.emplace_back(
        "Remove main and select namespace-scope functions through the "
        "library target's exports list.");
    diagnostics.push_back(std::move(diagnostic));
  }

  // Current native callbacks have process-lifetime addresses. An unloadable
  // module cannot safely contain one until registration gains revocation.
  if (semanticModel.hasNativeFunctionConversions()) {
    diagnostics.push_back(makeDiagnostic(
        "GTI-S2104", DiagnosticPhase::Semantics, fallback,
        "Library programs cannot create GTI-to-C callback adapters in "
        "embedding ABI version 1."));
  }

  std::unordered_map<std::string, SourceSpan> symbols;
  std::unordered_map<std::string, SourceSpan> selectedFunctions;
  SemanticEmbeddingPlan plan;
  for (const EmbeddingExportRequest &exportRequest : request.exports) {
    const SourceSpan location = requestLocation(exportRequest, sourceGraph);
    const SourceSpan symbolLocation =
        exportRequest.symbolDeclaration.source.empty()
            ? location
            : exportRequest.symbolDeclaration;
    const SourceSpan functionLocation =
        exportRequest.functionDeclaration.source.empty()
            ? location
            : exportRequest.functionDeclaration;

    bool requestValid = true;
    if (!ordinaryIdentifier(exportRequest.symbol) ||
        reservedEmbeddingSymbol(exportRequest.symbol)) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2104", DiagnosticPhase::Semantics, symbolLocation,
          "Embedding symbol '" + exportRequest.symbol +
              "' is not a non-reserved C17 and C++ identifier.");
      diagnostic.hints.emplace_back(
          "Use an ASCII identifier that does not begin with '_' and is not "
          "a language keyword or fixed GTI embedding symbol.");
      diagnostics.push_back(std::move(diagnostic));
      requestValid = false;
    }
    if (!qualifiedFunctionName(exportRequest.function)) {
      diagnostics.push_back(makeDiagnostic(
          "GTI-S2104", DiagnosticPhase::Semantics, functionLocation,
          "Embedding function '" + exportRequest.function +
              "' is not an exact qualified GTI name."));
      requestValid = false;
    }
    const auto [priorSymbol, uniqueSymbol] =
        symbols.emplace(exportRequest.symbol, symbolLocation);
    if (!uniqueSymbol) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2104", DiagnosticPhase::Semantics, symbolLocation,
          "Embedding symbol '" + exportRequest.symbol +
              "' is selected more than once.");
      diagnostic.related.push_back(
          {priorSymbol->second, "The previous symbol selection is here."});
      diagnostics.push_back(std::move(diagnostic));
      requestValid = false;
    }
    const auto [priorFunction, uniqueFunction] =
        selectedFunctions.emplace(exportRequest.function, functionLocation);
    if (!uniqueFunction) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2104", DiagnosticPhase::Semantics, functionLocation,
          "GTI function '" + exportRequest.function +
              "' is selected for export more than once.");
      diagnostic.related.push_back(
          {priorFunction->second, "The previous function selection is here."});
      diagnostics.push_back(std::move(diagnostic));
      requestValid = false;
    }
    if (!requestValid) {
      continue;
    }

    std::vector<const FunctionInfo *> matches;
    for (const FunctionInfo *function : functions) {
      if (function->qualifiedName == exportRequest.function) {
        matches.push_back(function);
      }
    }
    if (matches.empty()) {
      diagnostics.push_back(makeDiagnostic(
          "GTI-S2104", DiagnosticPhase::Semantics, functionLocation,
          "No GTI function named '" + exportRequest.function +
              "' exists in this library source graph."));
      continue;
    }
    if (matches.size() != 1) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2104", DiagnosticPhase::Semantics, functionLocation,
          "Embedding export '" + exportRequest.function +
              "' names an overloaded function set; version 1 requires one "
              "unambiguous declaration.");
      for (const FunctionInfo *match : matches) {
        diagnostic.related.push_back(
            {tokenSpan(match->declaration->name()), "Candidate is here."});
      }
      diagnostics.push_back(std::move(diagnostic));
      continue;
    }

    const FunctionInfo &function = *matches.front();
    const FunctionDecl &declaration = *function.declaration;
    const bool ordinarySourceFunction =
        function.ownerClass == 0 && !function.entryPoint &&
        function.genericParameters.empty() && !function.parameterPack &&
        function.linkage == LanguageLinkage::Gti &&
        function.intrinsic == IntrinsicKind::None &&
        !function.compilerPrivate && declaration.body() != nullptr &&
        !declaration.runtimeBinding() && !declaration.operatorName();
    if (!ordinarySourceFunction) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2104", DiagnosticPhase::Semantics, functionLocation,
          "Embedding export '" + exportRequest.function +
              "' must name one non-generic, non-overloaded, namespace-scope "
              "GTI source function with a body.");
      diagnostic.related.push_back(
          {tokenSpan(declaration.name()), "The selected declaration is here."});
      diagnostics.push_back(std::move(diagnostic));
      continue;
    }

    const auto boundaryTypeAllowed = [&](const SemanticType &type,
                                         bool result) {
      if (result && type == SemanticType::Void) {
        return true;
      }
      // The frozen embedding ABI is narrower than ordinary extern-C support.
      // Later native scalar support must not silently expand this ABI family.
      if ((isCAbiScalar(type) && type.kind != SemanticType::Bool &&
           type.nativeCScalar == NativeCScalarKind::None) ||
          isCAbiRecordType(type)) {
        return true;
      }
      return type.kind == SemanticType::RawPointer &&
             type.arguments.size() == 1 &&
             (isCAbiRecordType(type.arguments.front()) ||
              isCOpaqueHandleType(type.arguments.front()));
    };

    bool signatureValid =
        declaration.returnMutability() == Mutability::Immutable &&
        !declaration.returnType().reference &&
        boundaryTypeAllowed(function.returnType, true);
    for (std::size_t index = 0; index < declaration.parameters().size();
         ++index) {
      const Parameter &parameter = declaration.parameters()[index];
      if (index >= function.parameterTypes.size() ||
          parameter.mutability != Mutability::Immutable ||
          parameter.type.reference || !parameter.type.arrayExtents.empty() ||
          parameter.pack ||
          !boundaryTypeAllowed(function.parameterTypes[index], false)) {
        signatureValid = false;
      }
    }
    if (function.parameterTypes.size() != declaration.parameters().size()) {
      signatureValid = false;
    }
    if (!signatureValid) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2104", DiagnosticPhase::Semantics, functionLocation,
          "Embedding export '" + exportRequest.function +
              "' has a type outside the version 1 host boundary.");
      diagnostic.related.push_back(
          {tokenSpan(declaration.name()), "The selected declaration is here."});
      diagnostic.hints.emplace_back(
          "Use immutable by-value fixed-width integers, float, double, "
          "passive [[c_abi]] records, or one raw pointer to a passive record "
          "or [[c_opaque]] handle; only the result may be void.");
      diagnostics.push_back(std::move(diagnostic));
      continue;
    }

    plan.exports.push_back({.function = function.id,
                            .qualifiedName = function.qualifiedName,
                            .symbol = exportRequest.symbol,
                            .returnType = function.returnType,
                            .parameterTypes = function.parameterTypes,
                            .requestSource = location});
  }

  std::sort(plan.exports.begin(), plan.exports.end(),
            [](const SemanticEmbeddingExport &left,
               const SemanticEmbeddingExport &right) {
              if (left.symbol != right.symbol) {
                return left.symbol < right.symbol;
              }
              return left.qualifiedName < right.qualifiedName;
            });
  if (!hadError() && plan.exports.size() == request.exports.size()) {
    semanticModel.setEmbeddingPlan(std::move(plan));
  }
  return !hadError();
}

} // namespace lang
