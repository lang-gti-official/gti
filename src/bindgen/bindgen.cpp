#include "gti/bindgen/bindgen.h"

#include "gti/bindgen/gti_writer.h"

namespace lang::bindgen {

BindgenResult runCBindgen(std::string_view headerText,
                          std::string_view headerName, BindingOptions options,
                          CTargetProfile profile) {
  BindgenResult result;
  BoundedCReader reader(profile);
  ReadResult read = reader.read(headerText);
  result.diagnostics = std::move(read.diagnostics);
  if (!result.usable()) {
    return result;
  }
  result.plan = planBindings(read.model, std::move(options));
  result.gtiSource = writeGtiBindings(result.plan, headerName);
  return result;
}

} // namespace lang::bindgen
