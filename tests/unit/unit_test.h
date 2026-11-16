// gti_unit_tests: C++ unit tests of library code (runtime, driver, LLVM
// passes, formatter) that does not need a GTI program. Every `.cpp` in
// tests/unit is linked into one executable; a file declares its cases with
// GTI_UNIT_TEST(name) { ... return 0; } and needs no CMake edit.
//
// A case returns 0 to pass, 77 to skip, anything else to fail; it may also
// exit, abort or crash, because each case runs in its own child process. A
// case receives the executable as argv[0] plus any arguments given after
// `--test NAME --`. When a case re-executes argv[0], the child runs the same
// case again with the new arguments.
//
// Outside the gti_unit_tests target (GTI_UNIT_TESTS_RUNNER undefined) a case
// is the file's `main`, so a one-case file can still be compiled on its own,
// as the installed-library smoke and the native cthread harnesses do.
#pragma once

#if !defined(GTI_UNIT_TESTS_RUNNER)

#define GTI_UNIT_TEST(name)                                                    \
  int main([[maybe_unused]] int argc, [[maybe_unused]] char **argv)

#else

namespace gti_unit {

using CaseFunction = int (*)(int argc, char **argv);

struct Registration {
  Registration(const char *name, const char *file, CaseFunction function);
};

} // namespace gti_unit

#define GTI_UNIT_TEST(name)                                                    \
  static int gti_unit_case_##name(int argc, char **argv);                      \
  static const ::gti_unit::Registration gti_unit_registration_##name(          \
      #name, __FILE__, gti_unit_case_##name);                                  \
  static int gti_unit_case_##name([[maybe_unused]] int argc,                   \
                                  [[maybe_unused]] char **argv)

#endif
