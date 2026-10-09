#include "cpp_mir_body_text_writer.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace lang {
namespace {

// Replaces every non-overlapping occurrence of `marker`, left to right, while
// copying `text` once. Replacing in place would move the remaining text at
// every occurrence, which is quadratic in a long body. Returns nothing when
// `marker` does not occur.
[[nodiscard]] std::optional<std::string>
replaceAll(std::string_view text, std::string_view marker,
           std::string_view replacement) {
  std::size_t at = text.find(marker);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  std::string rendered;
  rendered.reserve(text.size());
  std::size_t copied = 0;
  while (at != std::string_view::npos) {
    rendered.append(text.substr(copied, at - copied));
    rendered.append(replacement);
    copied = at + marker.size();
    at = text.find(marker, copied);
  }
  rendered.append(text.substr(copied));
  return rendered;
}

[[nodiscard]] std::string renderTemplate(std::string_view text,
                                         std::string_view familyLabel,
                                         std::size_t indentation) {
  std::optional<std::string> rendered =
      replaceAll(text, cppMirBodyEmissionFamilyPlaceholder, familyLabel);
  if (!rendered) {
    return {};
  }
  // Append unchanged slices rather than repeatedly shifting the remaining
  // body. Large verified cleanup templates can contain tens of thousands of
  // indentation markers. Preserve the original two-pass substitution order,
  // including indentation markers introduced by the family label.
  const std::string prefix(indentation * 2, ' ');
  if (std::optional<std::string> indented = replaceAll(
          *rendered, cppMirBodyEmissionIndentationPlaceholder, prefix)) {
    return std::move(*indented);
  }
  return std::move(*rendered);
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
