#pragma once

#include "gti/cpp_standard.h"

#include <iosfwd>

namespace lang {

// Explicit selection for generated support owned by the backend. Storage and
// defined-failure support remain universal in the current artifact contract;
// the other fragments are emitted only when the lowered program requires them.
struct CppSupportPreambleFeatures {
  CppStandard standard = CppStandard::Cpp23;
  bool expected = false;
  bool storage = true;
  bool managedThreads = false;
  bool cThreads = false;
  bool atomics = false;
  bool mutexes = false;
  bool simd = false;
  bool definedFailure = true;
  bool runtimeBinding = false;
  bool cAbiTextView = false;
};

void writeCppSupportPreamble(std::ostream &output,
                             CppSupportPreambleFeatures features);
void writeCppCAbiTextViewAdapter(std::ostream &output,
                                 CppSupportPreambleFeatures features);

} // namespace lang
