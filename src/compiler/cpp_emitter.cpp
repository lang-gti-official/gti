#include "gti/cpp_emitter.h"

#include "cpp_program_emission_plan.h"
#include "cpp_program_text_writer.h"
#include "gti/backend.h"
#include "gti/lowered_program.h"
#include "gti/support.h"

#include <memory>
#include <utility>

namespace lang {

class CppEmitter::Impl final {
public:
  explicit Impl(const VerifiedLoweredProgram &program, CppStandard standard,
                bool debugInfo)
      : plan([&] {
          if (program.program().mir().requiresNativeObjectLayout())
            throw BackendUnsupportedError(
                "general sizeof/alignof queries require the LLVM backend's "
                "native GTI object representation");
          const PhaseTimeScope timeScope("backend-plan");
          return buildCppProgramEmissionPlan(program, standard, debugInfo);
        }()) {}

  [[nodiscard]] std::string emit() const {
    const PhaseTimeScope timeScope("backend-write");
    return writeCppProgramEmissionPlan(plan);
  }

private:
  CppProgramEmissionPlan plan;
};

CppEmitter::CppEmitter(const VerifiedLoweredProgram &program,
                       CppStandard standard, bool debugInfo)
    : impl(std::make_unique<Impl>(program, standard, debugInfo)) {}

CppEmitter::~CppEmitter() = default;
CppEmitter::CppEmitter(CppEmitter &&) noexcept = default;
CppEmitter &CppEmitter::operator=(CppEmitter &&) noexcept = default;

std::string CppEmitter::emit() { return impl->emit(); }

} // namespace lang
