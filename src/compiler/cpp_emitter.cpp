#include "gti/cpp_emitter.h"

#include "cpp_program_emission_plan.h"
#include "cpp_program_text_writer.h"
#include "gti/lowered_program.h"

#include <memory>
#include <utility>

namespace lang {

class CppEmitter::Impl final {
public:
  explicit Impl(const LoweredProgram &program, CppStandard standard)
      : plan(buildCppProgramEmissionPlan(program, standard)) {}

  [[nodiscard]] std::string emit() const {
    return writeCppProgramEmissionPlan(plan);
  }

private:
  CppProgramEmissionPlan plan;
};

CppEmitter::CppEmitter(const LoweredProgram &program, CppStandard standard)
    : impl(std::make_unique<Impl>(program, standard)) {}

CppEmitter::~CppEmitter() = default;
CppEmitter::CppEmitter(CppEmitter &&) noexcept = default;
CppEmitter &CppEmitter::operator=(CppEmitter &&) noexcept = default;

std::string CppEmitter::emit() { return impl->emit(); }

} // namespace lang
