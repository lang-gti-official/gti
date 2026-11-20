#pragma once
#include "gti/compiled_mir.h"
#include "gti/compiled_scalar_signature.h"
#include "gti/lowered_program.h"
#include <span>
namespace lang {
struct PersistentDeclarationOwner;
// A recipe retains the exact source declaration and its verified body. Its
// byte offset distinguishes overloads within that immutable module snapshot.
struct CompiledModuleEntry {
  std::string name;
  std::size_t offset = 0;
  bool mayFail = false;
  std::string symbol;
};
struct CompiledModuleScope {
  std::string identity;
  std::vector<std::string> dataSymbols;
  std::map<std::string,std::string> dependencies;
};
struct CompiledModuleImport {
  SourceUnitId sourceUnit = 0;
  CompiledModuleEntry entry;
  std::shared_ptr<const CompiledModuleScope> scope;
};
std::string compiledModuleSymbol(std::string_view identity, std::string_view name,
                                 std::size_t offset, bool mayFail);
// Source IDs belong to this compilation only and never enter wire metadata.
struct CompiledScalarImport {
  CompiledScalarSignature signature;
  SourceUnitId sourceUnit = 0;
  std::shared_ptr<const CompiledMirBody> optimizationBody;
};
// Imported calls conservatively retain the normal GTI failure channel.
class CompiledScalarUnitBuilder final {
public:
  [[nodiscard]] std::vector<CompiledModuleEntry> selectModuleEntries(
      const LoweredProgram &, SourceUnitId, std::span<const std::string>,
      std::string_view identity) const;
  [[nodiscard]] LoweredProgram bindModuleEntries(
      LoweredProgram, std::span<const CompiledModuleImport>, bool producer, std::string_view stdlibRoot) const;
  [[nodiscard]] std::vector<CompiledScalarSignature> selectExports(
      const LoweredProgram &, SourceUnitId, const PersistentDeclarationOwner &,
      std::span<const std::string> functions,
      std::span<const CompiledScalarImport> imports = {},
      std::span<const SourceSpan> publicDeclarations = {},
      std::span<const CompiledInterfaceUnit> producerUnits = {}) const;
  [[nodiscard]] LoweredProgram
  bindExports(LoweredProgram, SourceUnitId root,
              std::span<const CompiledScalarSignature>) const;
  [[nodiscard]] LoweredProgram
      bindImports(LoweredProgram, std::span<const CompiledScalarImport>) const;
  [[nodiscard]] LoweredProgram
  bindCombined(LoweredProgram, SourceUnitId root,
               std::span<const CompiledScalarSignature> exports,
               std::span<const CompiledScalarImport> imports) const;
  // Legacy development API: int32 results, unique source-unit inference.
  [[nodiscard]] LoweredProgram bind(LoweredProgram program,
                                    std::span<const std::string> keys,
                                    bool producer) const;

  [[nodiscard]] LoweredProgram
  bindScoped(LoweredProgram, std::span<const CompiledScalarImport> exports,
             std::span<const CompiledScalarImport> imports) const;
};
[[nodiscard]] std::string compiledScalarSymbol(const CompiledScalarSignature &);
[[nodiscard]] std::string compiledScalarSymbol(std::string_view key);
[[nodiscard]] std::vector<std::string>
verifyCompiledScalarUnit(const LoweredProgram &);
} // namespace lang
