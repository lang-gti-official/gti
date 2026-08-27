#include "gti/bindgen/bindgen.h"

#include "gti/bindgen/gti_writer.h"

namespace lang::bindgen {

BindgenResult runCBindgen(std::string_view headerText,
                          std::string_view headerName, BindingOptions options,
                          CTargetProfile profile) {
  BindgenResult result;
  ClangCReader reader(profile);
  ReadResult read = reader.read(headerText);
  result.diagnostics = std::move(read.diagnostics);
  if (!result.usable()) {
    return result;
  }
  result.plan = planBindings(read.model, std::move(options));
  // Configuration that matched nothing reports as a warning: the import
  // still succeeds, but the caller sees that an opt-in entry did not apply.
  for (const std::string &problem : result.plan.policyProblems) {
    result.diagnostics.push_back({ReaderSeverity::Warning, {}, problem});
  }
  result.gtiSource = writeGtiBindings(result.plan, headerName);
  return result;
}

} // namespace lang::bindgen
