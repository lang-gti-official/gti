#pragma once

#include <iosfwd>

namespace lang {

class FailureMetadata;

void writeCppFailureArtifactDescriptor(std::ostream &output,
                                       const FailureMetadata &metadata);
void writeCppMirFailureHelpers(std::ostream &output);

} // namespace lang
