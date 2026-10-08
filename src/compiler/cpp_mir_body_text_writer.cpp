#include "cpp_mir_body_text_writer.h"

#include <string>

namespace lang {
namespace {

[[nodiscard]] std::string renderTemplate(std::string_view text,
                                         std::string_view familyLabel,
                                         std::size_t indentation) {
  if (text.find(cppMirBodyEmissionFamilyPlaceholder) ==
      std::string_view::npos) {
    return {};
  }
  // Append unchanged slices rather than repeatedly shifting the remaining
  // body. Large verified cleanup templates can contain tens of thousands of
  // indentation markers. Preserve the original two-pass substitution order,
  // including indentation markers introduced by the family label.
  const auto replaceAll = [](std::string_view source, std::string_view marker,
                             std::string_view replacement) {
    std::string result;
    result.reserve(source.size());
    std::size_t cursor = 0;
    for (auto at = source.find(marker, cursor); at != std::string_view::npos;
         at = source.find(marker, cursor)) {
      result.append(source.substr(cursor, at - cursor));
      result.append(replacement);
      cursor = at + marker.size();
    }
    result.append(source.substr(cursor));
    return result;
  };
  const std::string rendered =
      replaceAll(text, cppMirBodyEmissionFamilyPlaceholder, familyLabel);
  const std::string prefix(indentation * 2, ' ');
  return replaceAll(rendered, cppMirBodyEmissionIndentationPlaceholder, prefix);
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
