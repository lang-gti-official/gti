#pragma once

#include "gti/frontend.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lang {

class SignaturePrinter {
public:
  explicit SignaturePrinter(const SemanticModel &semantics);

  [[nodiscard]] std::string
  function(const FunctionInfo &info,
           const ResolvedCallInfo *selected = nullptr) const;
  [[nodiscard]] std::string
  conceptSignature(const SymbolRecord &symbol,
                   const ConceptDecl *declaration) const;
  [[nodiscard]] std::string
  constructor(const ClassTypeInfo &owner,
              const ResolvedConstructionInfo &selected) const;
  [[nodiscard]] std::string constructor(const ClassTypeInfo &owner,
                                        const ConstructorInfo &info) const;
  [[nodiscard]] std::string classType(const ClassTypeInfo &info) const;
  [[nodiscard]] std::string enumType(const EnumTypeInfo &info) const;
  [[nodiscard]] std::string enumerator(const EnumTypeInfo &owner,
                                       std::string_view name) const;
  [[nodiscard]] std::string typeAlias(const TypeAliasInfo &info) const;
  [[nodiscard]] std::string binding(const SemanticOccurrence &occurrence) const;
  [[nodiscard]] std::string destructor(const DestructorInfo &info) const;

private:
  [[nodiscard]] static std::string path(const NamePath &name);
  [[nodiscard]] std::string functionName(const FunctionInfo &info) const;
  void appendTypes(std::string &result,
                   const std::vector<SemanticType> &arguments) const;
  void appendSelectedGenericArguments(
      std::string &result, const std::vector<GenericParameterInfo> &parameters,
      const std::vector<SemanticType> &typeArguments,
      const std::vector<CompileTimeValue> &valueArguments) const;
  void appendGenericParameters(
      std::string &result,
      const std::vector<GenericParameterInfo> &parameters) const;
  void appendParameters(std::string &result,
                        const std::vector<SemanticType> &parameterTypes,
                        const std::vector<Parameter> *parameters,
                        bool preservePackSyntax) const;
  static void appendConceptApplication(std::string &result,
                                       const ConceptApplication &application);
  void appendRequirements(std::string &result, const FunctionInfo &info,
                          const ResolvedCallInfo *selected) const;

  const SemanticModel &semantics;
  SemanticTypePrinter types;
};

struct HoverInfo {
  SourceSpan range;
  std::string signature;
  std::optional<std::string> documentationMarkdown;
  std::vector<std::string> notes;
};

struct DefinitionInfo {
  SymbolId symbol = 0;
  SourceSpan origin;
  SourceSpan target;
};

enum class CompletionCandidateKind {
  Namespace,
  TypeAlias,
  Class,
  Struct,
  Enum,
  Enumerator,
  Function,
  Method,
  Field,
  Variable,
  Parameter,
  TypeParameter,
};

struct CompletionCandidate {
  CompletionCandidateKind kind = CompletionCandidateKind::Variable;
  std::string label;
  std::string detail;
  std::string insertion;
  std::optional<std::string> snippet;
  std::string sortText;
  SourceSpan replacementRange;
};

struct CompletionResult {
  std::vector<CompletionCandidate> candidates;
  bool isIncomplete = false;
};

struct CompletionInput {
  std::filesystem::path entryPath;
  std::string source;
  std::size_t byteOffset = 0;
  std::vector<std::filesystem::path> preludePaths;
  std::unordered_map<std::string, std::string> sourceOverrides;
  std::vector<std::filesystem::path> standardLibraryRoots;
  std::vector<PackageSourceRoot> packageSourceRoots;
};

class LanguageQueries {
public:
  [[nodiscard]] std::optional<HoverInfo> hover(const FrontendResult &snapshot,
                                               SourceUnitId sourceUnit,
                                               std::size_t byteOffset) const;
  [[nodiscard]] std::optional<DefinitionInfo>
  definition(const FrontendResult &snapshot, SourceUnitId sourceUnit,
             std::size_t byteOffset) const;
  [[nodiscard]] CompletionResult complete(const CompletionInput &input) const;
};

} // namespace lang
