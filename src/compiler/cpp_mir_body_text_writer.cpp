#include "cpp_mir_body_text_writer.h"

#include <string>

namespace lang {
namespace {

[[nodiscard]] std::string renderTemplate(std::string_view text,
                                         std::string_view familyLabel,
                                         std::size_t indentation) {
  std::string rendered(text);
  const auto replaceAll = [&](std::string_view marker,
                              std::string_view replacement) {
    std::size_t at = rendered.find(marker);
    if (at == std::string::npos) {
      return false;
    }
    while (at != std::string::npos) {
      rendered.replace(at, marker.size(), replacement);
      at = rendered.find(marker, at + replacement.size());
    }
    return true;
  };
  if (!replaceAll(cppMirBodyEmissionFamilyPlaceholder, familyLabel)) {
    return {};
  }
  const std::string prefix(indentation * 2, ' ');
  replaceAll(cppMirBodyEmissionIndentationPlaceholder, prefix);
  return rendered;
}

} // namespace

CppMirBodyEmissionText CppMirBodyTextWriter::write(
    const CppMirBodyEmissionPlan &plan, CppMirBodyTextForm form,
    std::string_view familyLabel, std::size_t indentation) {
  CppMirBodyEmissionText result{.analysis = plan.analysis()};
  const bool deducedCallable = familyLabel.starts_with("deduced-callable");
  const std::string &deducedText =
      form == CppMirBodyTextForm::Failure
          ? plan.failureDeducedCallableTextTemplate_
          : plan.plainDeducedCallableTextTemplate_;
  const std::string &text =
      form == CppMirBodyTextForm::BaseGate ? plan.baseGateTextTemplate_
      : form == CppMirBodyTextForm::BasePreparation
          ? plan.basePreparationTextTemplate_
      : deducedCallable && !deducedText.empty()
          ? deducedText
          : (form == CppMirBodyTextForm::Failure ? plan.failureTextTemplate_
                                                 : plan.plainTextTemplate_);
  const bool supported =
      form == CppMirBodyTextForm::BaseGate ? plan.supportsBaseGateText()
      : form == CppMirBodyTextForm::BasePreparation
          ? plan.supportsBasePreparationText()
      : form == CppMirBodyTextForm::Failure ? plan.supportsFailureText()
                                            : plan.supportsPlainText();
  if (supported) {
    result.text = renderTemplate(text, familyLabel, indentation);
  }
  return result;
}

} // namespace lang
