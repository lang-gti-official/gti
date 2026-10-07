#pragma once

#include "gti/failure.h"
#include <iosfwd>
#include <string_view>

namespace lang {

class FailureMetadata;

[[nodiscard]] std::string_view
cppFailureCodeAbiConstant(DefinedFailureCode code);
[[nodiscard]] std::string_view
cppFailureDetailAbiConstant(DefinedFailureDetail detail);

void writeCppFailureArtifactDescriptor(std::ostream &output,
                                       const FailureMetadata &metadata);
void writeCppMirFailureHelpers(std::ostream &output);

} // namespace lang
