#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace lang::driver {
struct NativeInputs;
struct CompilationResult;
struct CheckResult;
// Shared by artifact builds and read-only project checking.
std::string readCompiledLibraryFile(const std::filesystem::path &path);
std::optional<std::string>
compiledScalarProducerInputDiagnostic(const NativeInputs &inputs);
CheckResult compiledScalarCheckResult(CompilationResult result);
} // namespace lang::driver
