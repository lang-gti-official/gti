// Transitional C-04 equivalence gate: runs the deterministic C corpus
// through BoundedCReader and ClangCReader and fails on any model
// divergence. This harness exists only while both readers coexist; the
// pipeline switch removes it together with BoundedCReader, freezing the
// corpus as ClangCReader goldens in bindgen_tests.cpp.
#include "gti/bindgen/header_reader.h"
#include "gti/bindgen/native_model.h"

#include <iostream>
#include <string>
#include <string_view>

using namespace lang::bindgen;

namespace {

int divergences = 0;

void compare(std::string_view name, std::string_view header,
             CTargetProfile profile = CTargetProfile{}) {
  BoundedCReader bounded(profile);
  ClangCReader clang(profile);
  const ReadResult boundedResult = bounded.read(header);
  const ReadResult clangResult = clang.read(header);
  const std::string boundedPrint = printNativeModel(boundedResult.model);
  const std::string clangPrint = printNativeModel(clangResult.model);
  if (boundedPrint == clangPrint) {
    std::cout << "== " << name << ": models MATCH\n";
  } else {
    ++divergences;
    std::cout << "== " << name << ": models DIVERGE\n"
              << "--- bounded ---\n"
              << boundedPrint << "--- clang ---\n"
              << clangPrint;
  }
  std::cout << "-- bounded diagnostics:\n";
  for (const ReaderDiagnostic &d : boundedResult.diagnostics) {
    std::cout << "   [" << d.origin.line << ':' << d.origin.column << "] "
              << d.message << '\n';
  }
  std::cout << "-- clang diagnostics:\n";
  for (const ReaderDiagnostic &d : clangResult.diagnostics) {
    std::cout << "   [" << d.origin.line << ':' << d.origin.column << "] "
              << d.message << '\n';
  }
}

void show(std::string_view name, std::string_view header,
          CTargetProfile profile = CTargetProfile{}) {
  ClangCReader clang(profile);
  const ReadResult result = clang.read(header);
  std::cout << "## " << name << "\n"
            << printNativeModel(result.model);
  for (const ReaderDiagnostic &d : result.diagnostics) {
    std::cout << "   [" << d.origin.line << ':' << d.origin.column << "] "
              << d.message << '\n';
  }
}

} // namespace

int main() {
  compare("mixed-model", R"(
typedef struct Engine Engine;
typedef struct Vec2 { float x; float y; } Vec2;
enum Mode { MODE_A, MODE_B = 5, MODE_C };
typedef unsigned long long Stamp;
typedef void (*Log)(int level, const char *message);
Engine *engine_create(int mode);
int engine_submit(Engine *engine, Vec2 position, Stamp stamp);
)");

  compare("degradation", R"(
struct Packed { int flags : 3; };
union Overlay { int whole; float parts; };
struct Holder { struct Packed *packed; union Overlay *overlay; };
struct Plain { int value; };
int use_pointer(struct Packed *packed);
int use_value(struct Packed packed);
int use_plain(struct Plain plain);
)");

  compare("type-matrix", R"(
typedef struct Item Item;
int scalars(signed char a, unsigned short b, long c, unsigned long long d);
float floats(float a, double b);
void *pointers(const char *name, void *user, const double *values);
int open_item(Item **out_item);
int find_tail(const char **out_tail);
int rejected_bool(_Bool flag);
int rejected_char(char letter);
int rejected_two_level(int **matrix);
int rejected_const_handle(const Item **out_item);
void template(int value);
int _Reserved(int value);
)");

  compare("target-lp64", R"(
typedef long NativeLong;
typedef unsigned long NativeUnsignedLong;
typedef size_t NativeSize;
long transform(long value, size_t count);
)",
          CTargetProfile(CDataModel::Lp64));

  compare("target-llp64", R"(
typedef long NativeLong;
typedef unsigned long NativeUnsignedLong;
typedef size_t NativeSize;
long transform(long value, size_t count);
)",
          CTargetProfile(CDataModel::Llp64));

  compare("enum-and-typedef", R"(
enum Small { NEGATIVE = -2, POSITIVE = 7 };
typedef enum { ANON_A, ANON_B } Named;
typedef unsigned int Flags;
typedef struct Missing Missing;
typedef Missing *Handle;
int uses_enum(enum Small value, Named named, Flags flags, Handle handle);
)");

  compare("preprocessing", R"(#ifndef GUARD_H
#define GUARD_H
#include <stdint.h>
#define MAGIC 42
#if defined(_WIN32)
int win_only(void);
#endif
#pragma once
int always(void);
#endif
)");

  compare("skip-classes", R"(
static int hidden(void);
inline int fast(void);
int variadic_call(const char *format, ...);
int global_variable;
struct Fixed { int values[4]; };
int with_array_param(int values[], int count);
int definition(void) { return 1; }
int stdint_names(int8_t a, uint16_t b, int32_t c, uint64_t d, size_t e,
                 intptr_t f, uintptr_t g, ptrdiff_t h);
)");

  // Deliberate improvements over the bounded reader: print for inspection.
  show("recovery (clang keeps bad_one with an unsupported parameter)", R"(
int good_before(void);
int bad_one(struct { int x; } anonymous);
int good_after(int value);
)");

  show("const array parameter decay keeps const", R"(
int with_const_array(const int values[], int count);
)");

  show("anonymous top-level enum stays as constants", R"(
enum { BARE_A, BARE_B = 9 };
)");

  show("multi-extent array fields nest outermost-first", R"(
struct Grid { double cells[2][3]; };
)");

  show("invalid declaration surfaces the native error", R"(
int rejected_unknown(some_unknown_t value);
int fine(int value);
)");

  compare("builtin-typedef-spellings", R"(
typedef size_t StorageSize;
size_t storage_size(void);
)");

  std::cout << (divergences == 0 ? "ALL MATCH\n"
                                 : std::to_string(divergences) +
                                       " corpus divergence(s)\n");
  return divergences == 0 ? 0 : 1;
}
