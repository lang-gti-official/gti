#pragma once

#include "gti/cpp_standard.h"
#include "gti/target.h"

#include <memory>
#include <string>

namespace lang {

class CppMirBodyEmissionMap;
class HirProgram;
class MirProgram;
class OptimizationResult;
class Program;
class SemanticModel;
class CppBackend;

class CppEmitter final {
public:
  explicit CppEmitter(const SemanticModel &semantics, const HirProgram &hir,
                      CppStandard standard = CppStandard::Cpp23,
                      TargetInfo target = TargetInfo::host(),
                      const OptimizationResult *optimizations = nullptr);
  ~CppEmitter();

  CppEmitter(const CppEmitter &) = delete;
  CppEmitter &operator=(const CppEmitter &) = delete;
  CppEmitter(CppEmitter &&) noexcept;
  CppEmitter &operator=(CppEmitter &&) noexcept;

  CppEmitter(const SemanticModel &&, const HirProgram &,
             CppStandard = CppStandard::Cpp23, TargetInfo = TargetInfo::host(),
             const OptimizationResult * = nullptr) = delete;
  CppEmitter(const SemanticModel &, const HirProgram &&,
             CppStandard = CppStandard::Cpp23, TargetInfo = TargetInfo::host(),
             const OptimizationResult * = nullptr) = delete;
  CppEmitter(const SemanticModel &&, const HirProgram &&,
             CppStandard = CppStandard::Cpp23, TargetInfo = TargetInfo::host(),
             const OptimizationResult * = nullptr) = delete;

  [[nodiscard]] std::string emit(const Program &program);

private:
  friend class CppBackend;

  // The copied representation rows are built at the backend boundary
  // (ADR 016): CppBackend constructs and validates them beside the program
  // plan and passes ownership in, so the emitter never derives a spelling
  // authority of its own.
  CppEmitter(const SemanticModel &semantics, const HirProgram &hir,
             const MirProgram &verifiedMir, CppMirBodyEmissionMap generalRows,
             CppStandard standard, TargetInfo target,
             const OptimizationResult *optimizations);

  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang
