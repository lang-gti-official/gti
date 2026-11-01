#pragma once
#include "gti/persistent_declaration.h"
#include <span>
namespace lang {
class LoweredProgram;
struct CompiledScalarSignature;
// Canonical nominal declarations, independent of compiler-session IDs. The
// initial executable profile retains ordinary generated value lifecycles.
std::string projectCompiledInterfaceTypes(const LoweredProgram &, SourceUnitId,
                                          const PersistentDeclarationOwner &,
                                          std::span<const SemanticType> roots);
PersistentTypeRegistry compiledInterfaceSyntheticRegistry(std::string_view);
PersistentTypeRegistry compiledInterfaceTypeRegistry(const LoweredProgram &,
                                                     SourceUnitId,
                                                     std::string_view);
std::vector<std::string> compiledInterfaceDeclarationNames(std::string_view);
std::string compiledInterfaceTypeSource(std::string_view,
                                        std::string_view aliasPrefix = {});
// Complete facade projection chooses collision-free private aliases so source
// namespace lookup cannot capture an already resolved nominal type.
std::string compiledInterfaceSource(std::span<const CompiledScalarSignature>);
std::string compiledInterfaceNamespaceSource(std::string_view qualifiedName,
                                             std::string declaration);
std::string compiledInterfaceTypeSpelling(const SemanticType &,
                                          const PersistentTypeRegistry &,
                                          std::string_view aliasPrefix = {});
} // namespace lang
