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
  // A lowered capability class declaration lets any emitted declaration,
  // including an uninstantiated generic definition, name the class template
  // that represents it. These forward-declare those templates and add no
  // native support; `atomics` and `mutexes` select the definitions.
  bool sharedOwnerDeclared = false;
  bool atomicStorageDeclared = false;
  bool mutexStorageDeclared = false;
  bool mutexLockTokenDeclared = false;
  // The <std/polymorphic> slot templates are pure templates; a declared
  // capability class selects them.
  bool polymorphicStorage = false;
  bool atomics = false;
  bool mutexes = false;
  bool simd = false;
  bool definedFailure = true;
  bool runtimeBinding = false;
  bool cAbiTextView = false;
  bool payloadReferences = false;
};

void writeCppSupportPreamble(std::ostream &output,
                             CppSupportPreambleFeatures features);
void writeCppCAbiTextViewAdapter(std::ostream &output,
                                 CppSupportPreambleFeatures features);

} // namespace lang
