#pragma once

#include "gti/cpp_standard.h"

#include <memory>
#include <string>

namespace lang {

class VerifiedLoweredProgram;
class CppBackend;

class CppEmitter final {
public:
  ~CppEmitter();

  CppEmitter(const CppEmitter &) = delete;
  CppEmitter &operator=(const CppEmitter &) = delete;
  CppEmitter(CppEmitter &&) noexcept;
  CppEmitter &operator=(CppEmitter &&) noexcept;

  [[nodiscard]] std::string emit();

private:
  friend class CppBackend;

  // CppBackend is the only construction boundary. The emitter derives all
  // C++-private plans and spelling rows from this verified lowered program.
  CppEmitter(const VerifiedLoweredProgram &program, CppStandard standard,
             bool debugInfo);

  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang
