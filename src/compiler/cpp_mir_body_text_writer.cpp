#include "cpp_mir_body_text_writer.h"

#include <string>

namespace lang {
namespace {

[[nodiscard]] std::string renderTemplate(std::string_view text,
                                         std::string_view familyLabel,
                                         std::size_t indentation) {
  std::string rendered(text);
  std::size_t marker = rendered.find(cppMirBodyEmissionFamilyPlaceholder);
  if (marker == std::string::npos) {
    return {};
  }
  while (marker != std::string::npos) {
    rendered.replace(marker, cppMirBodyEmissionFamilyPlaceholder.size(),
                     familyLabel);
    marker = rendered.find(cppMirBodyEmissionFamilyPlaceholder,
                           marker + familyLabel.size());
  }
  if (indentation == 0) {
    return rendered;
  }

  const std::string prefix(indentation * 2, ' ');
  std::string indented;
  indented.reserve(rendered.size() + prefix.size() * 8);
  for (std::size_t index = 0; index < rendered.size(); ++index) {
    indented.push_back(rendered[index]);
    if (rendered[index] == '\n' && index + 1 < rendered.size() &&
        rendered[index + 1] != '\n') {
      indented += prefix;
    }
  }
  return indented;
}

} // namespace

CppMirBodyEmissionText CppMirBodyTextWriter::write(
    const CppMirBodyEmissionPlan &plan, CppMirBodyTextForm form,
    std::string_view familyLabel, std::size_t indentation) {
  CppMirBodyEmissionText result{.analysis = plan.analysis()};
  const std::string &text = form == CppMirBodyTextForm::Failure
                                ? plan.failureTextTemplate_
                                : plan.plainTextTemplate_;
  const bool supported = form == CppMirBodyTextForm::Failure
                             ? plan.supportsFailureText()
                             : plan.supportsPlainText();
  if (supported) {
    result.text = renderTemplate(text, familyLabel, indentation);
  }
  return result;
}

} // namespace lang
