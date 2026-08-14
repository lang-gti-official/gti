#pragma once

#include "gti/cpp_standard.h"
#include "gti/target.h"

#include <memory>
#include <string>

namespace lang {

class HirProgram;
class OptimizationResult;
class Program;
class SemanticModel;

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
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang
