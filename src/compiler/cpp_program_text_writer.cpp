#include "cpp_program_text_writer.h"

#include <sstream>
#include <stdexcept>

namespace lang {
namespace {

class SupportTextWriter {
public:
  static void write(std::ostream &output,
                    const CppProgramTextFragment &fragment) {
    output << fragment.text;
  }
};

class DeclarationTextWriter {
public:
  static void write(std::ostream &output,
                    const CppProgramTextFragment &fragment) {
    output << fragment.text;
  }
};

class AdapterTextWriter {
public:
  static void write(std::ostream &output,
                    const CppProgramTextFragment &fragment) {
    output << fragment.text;
  }
};

} // namespace

std::string writeCppProgramEmissionPlan(const CppProgramEmissionPlan &plan) {
  if (plan.empty()) {
    throw std::logic_error("C++ program emission received an empty plan");
  }
  std::ostringstream output;
  for (const CppProgramTextFragment &fragment : plan.fragments()) {
    switch (fragment.role) {
    case CppProgramTextRole::SupportPreamble:
    case CppProgramTextRole::FailureAndLifecycleSupport:
      SupportTextWriter::write(output, fragment);
      break;
    case CppProgramTextRole::DeclarationTypeMemberLifecycle:
      DeclarationTextWriter::write(output, fragment);
      break;
    case CppProgramTextRole::NativeAndThreadAdapter:
    case CppProgramTextRole::InitializerAndModuleAdapter:
    case CppProgramTextRole::HostedEntryAdapter:
      AdapterTextWriter::write(output, fragment);
      break;
    }
  }
  return output.str();
}

} // namespace lang
