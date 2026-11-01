#pragma once
#include "gti/compiled_scalar_signature.h"
#include "gti/lowered_program.h"
#include <span>
namespace lang {
struct PersistentDeclarationOwner;
// Source IDs belong to this compilation only and never enter wire metadata.
struct CompiledScalarImport {
  CompiledScalarSignature signature;
  SourceUnitId sourceUnit = 0;
};
// Imported calls conservatively retain the normal GTI failure channel.
class CompiledScalarUnitBuilder final {
public:
  [[nodiscard]] std::vector<CompiledScalarSignature>
  selectExports(const LoweredProgram &, SourceUnitId,
                const PersistentDeclarationOwner &,
                std::span<const std::string> functions,
                std::span<const CompiledScalarImport> imports = {}) const;
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

private:
  [[nodiscard]] LoweredProgram
  bindScoped(LoweredProgram, std::span<const CompiledScalarImport> exports,
             std::span<const CompiledScalarImport> imports) const;
};
[[nodiscard]] std::string compiledScalarSymbol(const CompiledScalarSignature &);
[[nodiscard]] std::string compiledScalarSymbol(std::string_view key);
[[nodiscard]] std::vector<std::string>
verifyCompiledScalarUnit(const LoweredProgram &);
} // namespace lang
