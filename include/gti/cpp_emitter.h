#pragma once

#include "gti/cpp_standard.h"
#include "gti/target.h"

#include <memory>
#include <string>

namespace lang {

class CppMirBodyEmissionMap;
struct CppMirProgramPlan;
class HirProgram;
class LoweredProgram;
class MirProgram;
class OptimizationResult;
class Program;
class SemanticModel;
class CppBackend;

class CppEmitter final {
public:
  ~CppEmitter();

  CppEmitter(const CppEmitter &) = delete;
  CppEmitter &operator=(const CppEmitter &) = delete;
  CppEmitter(CppEmitter &&) noexcept;
  CppEmitter &operator=(CppEmitter &&) noexcept;

  [[nodiscard]] std::string emit(const Program &program);

private:
  friend class CppBackend;

  // CppBackend is the only construction boundary. Verified MIR and copied
  // representation rows are mandatory, so executable emission cannot fall
  // back to the former AST/HIR-only route.
  CppEmitter(const SemanticModel &semantics, const HirProgram &hir,
             const MirProgram &verifiedMir, CppMirProgramPlan programPlan,
             CppMirBodyEmissionMap generalRows, CppStandard standard,
             TargetInfo target, const OptimizationResult *optimizations,
             const LoweredProgram *loweredProgram);

  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang
