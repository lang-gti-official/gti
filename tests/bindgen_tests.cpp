#include "gti/bindgen/bindgen.h"
#include "gti/bindgen/gti_writer.h"
#include "gti/frontend.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::bindgen::ReadResult
readC(std::string_view header,
      lang::bindgen::CTargetProfile profile = lang::bindgen::CTargetProfile{}) {
  lang::bindgen::ClangCReader reader(profile);
  return reader.read(header);
}

[[nodiscard]] const lang::bindgen::BindingItem *
findItem(const lang::bindgen::BindingPlan &plan, std::string_view name) {
  for (const lang::bindgen::BindingItem &item : plan.items) {
    if (item.name == name) {
      return &item;
    }
  }
  return nullptr;
}

[[nodiscard]] bool contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

// -- Reader -----------------------------------------------------------------

void testReaderModel() {
  const lang::bindgen::ReadResult read = readC(R"(
typedef struct Engine Engine;
typedef struct Vec2 { float x; float y; } Vec2;
enum Mode { MODE_A, MODE_B = 5, MODE_C };
typedef unsigned long long Stamp;
typedef void (*Log)(int level, const char *message);
Engine *engine_create(int mode);
int engine_submit(Engine *engine, Vec2 position, Stamp stamp);
)");
  expect(read.usable(), "the mixed header should read");
  const std::string printed = lang::bindgen::printNativeModel(read.model);
  expect(contains(printed, "record Engine (incomplete)"),
         "the typedef forward reference should model an incomplete record");
  expect(contains(printed, "record Vec2\n  field x: f32\n  field y: f32"),
         "the typedef struct should model its fields");
  expect(contains(printed, "enum Mode\n  MODE_A = 0\n  MODE_B = 5\n"
                           "  MODE_C = 6"),
         "enumerators should carry explicit and successor values");
  expect(contains(printed, "typedef Stamp = c_ulonglong[u64]"),
         "the multi-word builtin typedef should model as u64");
  expect(contains(printed,
                  "typedef Log = fn(c_int[i32], ptr(const char)) -> void"),
         "the callback typedef should model its shape");
  expect(contains(printed, "function engine_create(mode: c_int[i32]) -> "
                           "ptr(record:Engine)"),
         "the pointer-returning prototype should model");
  expect(contains(printed,
                  "function engine_submit(engine: ptr(record:Engine), "
                  "position: record:Vec2, stamp: alias:Stamp) -> c_int[i32]"),
         "parameters should model by value, pointer, and alias");
}

void testReaderPreprocessing() {
  const lang::bindgen::ReadResult read = readC(R"(#ifndef GUARD_H
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
  expect(read.usable(), "the guarded header should read");
  const std::string printed = lang::bindgen::printNativeModel(read.model);
  expect(contains(printed, "function always() -> c_int[i32]"),
         "the guarded declaration should survive preprocessing");
  expect(!contains(printed, "win_only"),
         "the false conditional should exclude its declarations");
  expect(contains(printed, "constant MAGIC = 42"),
         "the object-like integer macro should import as a constant");
  expect(!contains(printed, "GUARD_H"),
         "the include guard's empty macro should stay out of the model");
  bool sawInclude = false;
  for (const lang::bindgen::ReaderDiagnostic &diagnostic : read.diagnostics) {
    sawInclude = sawInclude || contains(diagnostic.message, "#include");
    // Conditionals evaluate natively now; the bounded reader's
    // conditional-block policy note is gone.
    expect(!contains(diagnostic.message, "conditional block"),
           "conditionals should evaluate without a policy note");
  }
  expect(sawInclude, "the unresolved include policy should report");
}

// Native C errors surface as reader diagnostics, and the declaration that
// does not parse is skipped rather than imported through Clang's recovery
// type — recovery would bind the wrong ABI.
void testReaderInvalidDeclarations() {
  const lang::bindgen::ReadResult read = readC(R"(
int rejected_unknown(some_unknown_t value);
int fine(int value);
)");
  expect(read.usable(), "per-declaration native errors keep the model usable");
  const std::string printed = lang::bindgen::printNativeModel(read.model);
  expect(!contains(printed, "rejected_unknown"),
         "a declaration with native C errors should not import");
  expect(contains(printed, "function fine(value: c_int[i32]) -> c_int[i32]"),
         "declarations after a native error should still model");
  bool sawNativeError = false;
  bool sawSkip = false;
  for (const lang::bindgen::ReaderDiagnostic &diagnostic : read.diagnostics) {
    sawNativeError =
        sawNativeError ||
        contains(diagnostic.message,
                 "native C error: unknown type name 'some_unknown_t'");
    sawSkip = sawSkip ||
              contains(diagnostic.message,
                       "declaration 'rejected_unknown' has native C errors");
  }
  expect(sawNativeError && sawSkip,
         "the native error and the skip should both report");
}

void testReaderRecovery() {
  const lang::bindgen::ReadResult read = readC(R"(
int good_before(void);
int bad_one(struct { int x; } anonymous);
int good_after(int value);
)");
  expect(read.usable(), "per-declaration recovery should keep reading");
  const std::string printed = lang::bindgen::printNativeModel(read.model);
  expect(contains(printed, "function good_before") &&
             contains(printed, "function good_after"),
         "declarations after a recovery should still model");
  expect(contains(printed, "function bad_one(anonymous: unsupported("),
         "the anonymous-record parameter should model as unsupported with "
         "its reason rather than dropping the function");
}

// Corpus goldens frozen at the C-04 reader switch: the bounded reader
// produced byte-identical models for these headers through the transitional
// equivalence gate, and the libclang reader now owns them.
void testReaderCorpusGoldens() {
  const lang::bindgen::ReadResult mixed = readC(R"(
typedef struct Engine Engine;
typedef struct Vec2 { float x; float y; } Vec2;
enum Mode { MODE_A, MODE_B = 5, MODE_C };
typedef unsigned long long Stamp;
typedef void (*Log)(int level, const char *message);
Engine *engine_create(int mode);
int engine_submit(Engine *engine, Vec2 position, Stamp stamp);
)");
  expect(lang::bindgen::printNativeModel(mixed.model) ==
             "record Engine (incomplete)\n"
             "record Vec2\n"
             "  field x: f32\n"
             "  field y: f32\n"
             "enum Mode\n"
             "  MODE_A = 0\n"
             "  MODE_B = 5\n"
             "  MODE_C = 6\n"
             "typedef Stamp = c_ulonglong[u64]\n"
             "typedef Log = fn(c_int[i32], ptr(const char)) -> void\n"
             "function engine_create(mode: c_int[i32]) -> ptr(record:Engine)\n"
             "function engine_submit(engine: ptr(record:Engine), "
             "position: record:Vec2, stamp: alias:Stamp) -> c_int[i32]\n",
         "the mixed-declaration corpus golden should hold");

  const lang::bindgen::ReadResult skips = readC(R"(
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
  expect(
      lang::bindgen::printNativeModel(skips.model) ==
          "function variadic_call(format: ptr(const char), ...) -> c_int[i32]\n"
          "record Fixed\n"
          "  field values: array[c_int[i32] x 4]\n"
          "function with_array_param(values: ptr(c_int[i32]), count: "
          "c_int[i32]) -> "
          "c_int[i32]\n"
          "function stdint_names(a: c_schar[i8], b: c_ushort[u16], c: "
          "c_int[i32], d: c_ulong[u64], e: c_ulong[u64], "
          "f: c_long[i64], g: c_ulong[u64], h: c_long[i64]) -> c_int[i32]\n",
      "the skip-class corpus golden should hold");
  bool sawStatic = false;
  bool sawInline = false;
  bool sawVariable = false;
  bool sawDefinition = false;
  for (const lang::bindgen::ReaderDiagnostic &diagnostic : skips.diagnostics) {
    sawStatic = sawStatic || contains(diagnostic.message, "'static'");
    sawInline = sawInline || contains(diagnostic.message, "'inline'");
    sawVariable = sawVariable || contains(diagnostic.message,
                                          "native variable 'global_variable'");
    sawDefinition =
        sawDefinition ||
        contains(diagnostic.message, "function definition 'definition'");
  }
  expect(sawStatic && sawInline && sawVariable && sawDefinition,
         "every skip class should report its note");
}

// Surface the libclang reader models beyond the bounded reader: nested
// multi-extent array fields (outermost extent first), retained anonymous
// enums, and const-preserving parameter array decay.
void testReaderExpandedSurface() {
  const lang::bindgen::ReadResult read = readC(R"(
struct Grid { double cells[2][3]; };
enum { BARE_A, BARE_B = 9 };
int with_const_array(const int values[], int count);
)");
  expect(read.usable(), "the expanded-surface header should read");
  const std::string printed = lang::bindgen::printNativeModel(read.model);
  expect(contains(printed, "field cells: array[array[f64 x 3] x 2]"),
         "multi-extent array fields should nest outermost-first");
  expect(contains(printed, "enum \n  BARE_A = 0\n  BARE_B = 9"),
         "a bare anonymous enum should keep its enumerators under an empty "
         "name");
  expect(contains(printed,
                  "function with_const_array(values: ptr(const c_int[i32]), "
                  "count: c_int[i32]) -> c_int[i32]"),
         "parameter array decay should preserve the element's const");
}

// ABI-affecting native facts are either preserved or refused, never silently
// misread: packed layouts degrade, portable assembler labels become link-name
// metadata, and typedef'd array parameters decay exactly as C does.
void testReaderAbiSafetyGates() {
  const lang::bindgen::ReadResult read = readC(R"(
struct Squeezed { int a; int b; } __attribute__((packed));
int renamed_symbol(int value) __asm__("actual_symbol");
typedef int Row[4];
int sum_row(Row row);
)");
  expect(read.usable(), "the ABI-safety header should read");
  const std::string printed = lang::bindgen::printNativeModel(read.model);
  // C-06: the assembler label is a model fact; link_name preserves it instead
  // of silently calling the header spelling.
  expect(contains(printed,
                  "function renamed_symbol(value: c_int[i32]) -> c_int[i32] "
                  "[asm \"actual_symbol\"]"),
         "an assembler label should model as a function fact");
  expect(
      contains(printed, "function sum_row(row: ptr(c_int[i32])) -> c_int[i32]"),
      "a typedef'd array parameter should decay to its element pointer");
  const lang::bindgen::BindingPlan plan =
      lang::bindgen::planBindings(read.model, {});
  const lang::bindgen::BindingItem *renamed = findItem(plan, "renamed_symbol");
  expect(renamed != nullptr &&
             renamed->kind == lang::bindgen::BindingItemKind::Function &&
             renamed->linkName == "actual_symbol",
         "the planner should retain a portable assembler label as the exact "
         "link symbol");
  // C-06: the layout attribute is a model fact, so the degradation reason
  // names which attribute rather than saying "an attribute".
  expect(contains(printed, "record Squeezed [packed]"),
         "a packed record should carry its layout fact");
  const lang::bindgen::BindingItem *squeezed = findItem(plan, "Squeezed");
  expect(squeezed != nullptr &&
             squeezed->kind == lang::bindgen::BindingItemKind::Opaque &&
             contains(squeezed->reason, "the record is packed"),
         "a packed record should degrade to opaque naming the attribute");
}

// A non-C calling convention is likewise a model fact the planner rejects
// explicitly; the canonical LLP64 parse target understands the Windows
// convention spellings.
void testCallingConventionFacts() {
  const lang::bindgen::ReadResult read =
      readC(R"(
int __vectorcall win_callback(int value);
int plain(int value);
)",
            lang::bindgen::CTargetProfile(lang::bindgen::CDataModel::Llp64));
  expect(read.usable(), "the calling-convention header should read");
  const std::string printed = lang::bindgen::printNativeModel(read.model);
  expect(contains(printed,
                  "function win_callback(value: c_int[i32]) -> c_int[i32] "
                  "[callconv vectorcall]"),
         "a non-C calling convention should model as a function fact");
  expect(contains(printed, "function plain(value: c_int[i32]) -> c_int[i32]\n"),
         "the C calling convention should carry no fact");
  const lang::bindgen::BindingPlan plan =
      lang::bindgen::planBindings(read.model, {});
  const lang::bindgen::BindingItem *item = findItem(plan, "win_callback");
  expect(item != nullptr &&
             item->kind == lang::bindgen::BindingItemKind::Skipped &&
             contains(item->reason, "'vectorcall' calling convention"),
         "the planner should skip the non-C convention with its reason");
  const lang::bindgen::BindingItem *plain = findItem(plan, "plain");
  expect(plain != nullptr &&
             plain->kind == lang::bindgen::BindingItemKind::Function,
         "the C-convention function should import");
}

void testReaderFieldAndQualifierForms() {
  const lang::bindgen::ReadResult read = readC(R"(
typedef struct Board { int cells[2][3]; int *rows[4]; } Board;
enum { FLAG_A = 1, FLAG_B = 2 };
int copy_board(const Board *restrict destination,
               const Board *__restrict source);
int fill(int *__restrict__ buffer, int count);
)");
  expect(read.usable(), "field and qualifier forms should read");
  const std::string printed = lang::bindgen::printNativeModel(read.model);
  expect(contains(printed, "field cells: array[array[c_int[i32] x 3] x 2]"),
         "a multi-extent array field should model as nested arrays");
  expect(contains(printed, "field rows: array[ptr(c_int[i32]) x 4]"),
         "a pointer-element array field should model faithfully");
  expect(contains(printed, "enum \n  FLAG_A = 1\n  FLAG_B = 2"),
         "a bare anonymous enum should model its constants");
  expect(contains(printed, "function copy_board(destination: "
                           "ptr(const record:Board), source: "
                           "ptr(const record:Board)) -> c_int[i32]"),
         "'restrict' qualifiers should not disturb pointer parameters");
  expect(contains(printed,
                  "function fill(buffer: ptr(c_int[i32]), count: c_int[i32]) "
                  "-> c_int[i32]"),
         "'__restrict__' should read as an ignorable qualifier");
}

// -- Planner ----------------------------------------------------------------

void testRecordDegradation() {
  const lang::bindgen::ReadResult read = readC(R"(
struct Packed { int flags : 3; };
union Overlay { int whole; float parts; };
struct Holder { struct Packed *packed; union Overlay *overlay; };
struct Plain { int value; };
int use_pointer(struct Packed *packed);
int use_value(struct Packed packed);
int use_plain(struct Plain plain);
)");
  const lang::bindgen::BindingPlan plan =
      lang::bindgen::planBindings(read.model, {});
  const lang::bindgen::BindingItem *packed = findItem(plan, "Packed");
  expect(packed != nullptr &&
             packed->kind == lang::bindgen::BindingItemKind::Opaque &&
             contains(packed->reason, "bit-field"),
         "a bit-field record should degrade to opaque with its reason");
  const lang::bindgen::BindingItem *overlay = findItem(plan, "Overlay");
  expect(overlay != nullptr &&
             overlay->kind == lang::bindgen::BindingItemKind::Opaque &&
             contains(overlay->reason, "union"),
         "a union should degrade to opaque with its reason");
  const lang::bindgen::BindingItem *holder = findItem(plan, "Holder");
  expect(holder != nullptr &&
             holder->kind == lang::bindgen::BindingItemKind::Record,
         "pointers to degraded records should stay expressible fields");
  const lang::bindgen::BindingItem *pointerUse = findItem(plan, "use_pointer");
  expect(pointerUse != nullptr &&
             pointerUse->kind == lang::bindgen::BindingItemKind::Function,
         "a pointer use of a degraded record should import");
  const lang::bindgen::BindingItem *valueUse = findItem(plan, "use_value");
  expect(valueUse != nullptr &&
             valueUse->kind == lang::bindgen::BindingItemKind::Skipped &&
             contains(valueUse->reason, "opaque handle"),
         "a by-value use of a degraded record should skip with its reason");
  const lang::bindgen::BindingItem *plainUse = findItem(plan, "use_plain");
  expect(plainUse != nullptr &&
             plainUse->kind == lang::bindgen::BindingItemKind::Function,
         "a by-value use of a bindable record should import");
}

void testFieldPositionDegradation() {
  // Native char pointer fields use c_char rather than the call-only c_string
  // conversion family. Unsupported nested fields still degrade honestly.
  const lang::bindgen::ReadResult read = readC(R"(
struct Labeled { const char *text; int code; };
struct Tails { const char **tails; };
)");
  const lang::bindgen::BindingPlan plan =
      lang::bindgen::planBindings(read.model, {});
  const lang::bindgen::BindingItem *labeled = findItem(plan, "Labeled");
  expect(labeled != nullptr &&
             labeled->kind == lang::bindgen::BindingItemKind::Record &&
             labeled->fields.front().spelling == "const c_char* text",
         "a 'const char*' field must retain native plain char identity");
  const lang::bindgen::BindingItem *tails = findItem(plan, "Tails");
  expect(tails != nullptr &&
             tails->kind == lang::bindgen::BindingItemKind::Opaque &&
             contains(tails->reason, "nested pointer"),
         "a nested-pointer field should degrade the record with its reason");
}

void testRecordFieldExpansion() {
  const lang::bindgen::ReadResult read = readC(R"(
typedef struct Engine Engine;
typedef struct Point { float x; float y; } Point;
typedef void (*Notify)(int code);
enum Mode { MODE_OFF, MODE_ON };
typedef struct Panel {
  int grid[2][3];
  int *cursors[4];
  const float *gains[2];
  Engine *engines[2];
  Point corners[4];
  void *scratch[2];
  Notify on_change;
  Notify handlers[3];
  enum Mode mode;
} Panel;
)");
  const lang::bindgen::BindingPlan plan =
      lang::bindgen::planBindings(read.model, {});
  const lang::bindgen::BindingItem *panel = findItem(plan, "Panel");
  expect(panel != nullptr &&
             panel->kind == lang::bindgen::BindingItemKind::Record,
         "the widened field set should keep Panel a [[c_abi]] record");
  if (panel == nullptr ||
      panel->kind != lang::bindgen::BindingItemKind::Record) {
    return;
  }
  const auto fieldSpelling = [&](std::string_view name) -> std::string {
    for (const lang::bindgen::BindingField &field : panel->fields) {
      if (field.name == name) {
        return field.spelling;
      }
    }
    return {};
  };
  expect(fieldSpelling("grid") == "c_int grid[2][3]",
         "a multi-extent array field should spell every extent in order");
  expect(fieldSpelling("cursors") == "c_int* cursors[4]",
         "a scalar-pointer array field should spell its element pointer");
  expect(fieldSpelling("gains") == "const c_float* gains[2]",
         "a const-pointee pointer array field should keep its qualifier");
  expect(fieldSpelling("engines") == "Engine* engines[2]",
         "an opaque-handle pointer array field should import");
  expect(fieldSpelling("corners") == "Point corners[4]",
         "a by-value record array field should import");
  expect(fieldSpelling("scratch") == "void* scratch[2]",
         "a void-pointer array field should import");
  expect(fieldSpelling("on_change") == "Notify on_change",
         "a callback-alias field should import");
  expect(fieldSpelling("handlers") == "Notify handlers[3]",
         "a callback-alias array field should import");
  expect(fieldSpelling("mode") == "Mode mode",
         "an enum-typed field should spell through its int32_t alias");
}

void testAnonymousEnumPlanning() {
  const lang::bindgen::ReadResult read = readC(R"(
enum { CAP_AUDIO = 1, CAP_VIDEO = 2 };
)");
  const lang::bindgen::BindingPlan plan =
      lang::bindgen::planBindings(read.model, {});
  const lang::bindgen::BindingItem *anonymous = findItem(plan, "");
  expect(anonymous != nullptr &&
             anonymous->kind == lang::bindgen::BindingItemKind::Enum &&
             anonymous->constants.size() == 2 &&
             anonymous->constants[0].name == "CAP_AUDIO" &&
             anonymous->constants[0].value == 1 &&
             anonymous->constants[1].value == 2,
         "a bare anonymous enum should plan as constants without an alias");
}

void testTypeMappingMatrix() {
  const lang::bindgen::ReadResult read = readC(R"(
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
  const lang::bindgen::BindingPlan plan =
      lang::bindgen::planBindings(read.model, {});
  const lang::bindgen::BindingItem *scalars = findItem(plan, "scalars");
  expect(scalars != nullptr &&
             scalars->kind == lang::bindgen::BindingItemKind::Function &&
             scalars->parameters.size() == 4 &&
             scalars->parameters[0].spelling == "c_schar" &&
             scalars->parameters[1].spelling == "c_ushort" &&
             scalars->parameters[2].spelling == "c_long" &&
             scalars->parameters[3].spelling == "c_ulonglong",
         "platform integer spellings must retain their native declaration "
         "families");
  const lang::bindgen::BindingItem *pointers = findItem(plan, "pointers");
  expect(pointers != nullptr &&
             pointers->kind == lang::bindgen::BindingItemKind::Function &&
             pointers->resultSpelling == "void*" &&
             pointers->parameters[0].spelling == "c_string" &&
             pointers->parameters[1].spelling == "void*" &&
             pointers->parameters[2].spelling == "const c_double*",
         "pointer spellings should map c_string, void*, and const pointees");
  const lang::bindgen::BindingItem *openItem = findItem(plan, "open_item");
  const lang::bindgen::BindingItem *findTail = findItem(plan, "find_tail");
  expect(openItem != nullptr && findTail != nullptr &&
             openItem->kind == lang::bindgen::BindingItemKind::Function &&
             findTail->kind == lang::bindgen::BindingItemKind::Function &&
             openItem->parameters[0].spelling == "Item**" &&
             findTail->parameters[0].spelling == "c_string*",
         "the importer should retain exact opaque-handle and C-string out "
         "parameters without enabling general nested pointers");
  const auto expectSkip = [&](std::string_view name,
                              std::string_view fragment) {
    const lang::bindgen::BindingItem *item = findItem(plan, name);
    expect(item != nullptr &&
               item->kind == lang::bindgen::BindingItemKind::Skipped &&
               contains(item->reason, fragment),
           std::string(name) + " should skip mentioning '" +
               std::string(fragment) + "'");
  };
  for (const auto &[name, spelling] : {std::pair{"rejected_bool", "c_bool"},
                                       std::pair{"rejected_char", "c_char"}}) {
    const auto *item = findItem(plan, name);
    expect(item && item->kind == lang::bindgen::BindingItemKind::Function &&
               item->parameters.front().spelling == spelling,
           std::string(name) +
               " must import with its exact native scalar identity");
  }
  expectSkip("rejected_two_level", "two-level pointers are limited");
  expectSkip("rejected_const_handle", "two-level pointers are limited");
  expectSkip("template", "reserved GTI spelling");
  expectSkip("_Reserved", "implementation-reserved");
}

void testTargetDataModels() {
  constexpr std::string_view header = R"(
typedef long NativeLong;
typedef unsigned long NativeUnsignedLong;
typedef size_t NativeSize;
long transform(long value, size_t count);
)";
  const lang::bindgen::ReadResult lp64 = readC(
      header, lang::bindgen::CTargetProfile(lang::bindgen::CDataModel::Lp64));
  const lang::bindgen::ReadResult llp64 = readC(
      header, lang::bindgen::CTargetProfile(lang::bindgen::CDataModel::Llp64));
  const std::string lp64Model = lang::bindgen::printNativeModel(lp64.model);
  const std::string llp64Model = lang::bindgen::printNativeModel(llp64.model);
  expect(contains(lp64Model, "typedef NativeLong = c_long[i64]") &&
             contains(lp64Model, "typedef NativeUnsignedLong = c_ulong[u64]") &&
             contains(
                 lp64Model,
                 "function transform(value: c_long[i64], count: c_ulong[u64]) "
                 "-> c_long[i64]"),
         "LP64 should model long as 64 bits and size_t as 64 bits");
  expect(
      contains(llp64Model, "typedef NativeLong = c_long[i32]") &&
          contains(llp64Model, "typedef NativeUnsignedLong = c_ulong[u32]") &&
          contains(
              llp64Model,
              "function transform(value: c_long[i32], count: c_ulonglong[u64]) "
              "-> c_long[i32]"),
      "LLP64 should model long as 32 bits and size_t as 64 bits");
}

void testEnumAndTypedefPlanning() {
  const lang::bindgen::ReadResult read = readC(R"(
enum Small { NEGATIVE = -2, POSITIVE = 7 };
typedef enum { ANON_A, ANON_B } Named;
typedef unsigned int Flags;
typedef struct Missing Missing;
typedef Missing *Handle;
int uses_enum(enum Small value, Named named, Flags flags, Handle handle);
)");
  const lang::bindgen::BindingPlan plan =
      lang::bindgen::planBindings(read.model, {});
  const lang::bindgen::BindingItem *small = findItem(plan, "Small");
  expect(small != nullptr &&
             small->kind == lang::bindgen::BindingItemKind::Enum &&
             small->constants.size() == 2 && small->constants[0].value == -2 &&
             small->constants[1].value == 7,
         "enum constants should carry their exact values");
  const lang::bindgen::BindingItem *named = findItem(plan, "Named");
  expect(named != nullptr &&
             named->kind == lang::bindgen::BindingItemKind::Enum,
         "a typedef'd anonymous enum should bind under its typedef name");
  const lang::bindgen::BindingItem *flags = findItem(plan, "Flags");
  expect(flags != nullptr &&
             flags->kind == lang::bindgen::BindingItemKind::Alias &&
             flags->aliasSpelling == "c_uint",
         "an object typedef should alias its mapped scalar");
  const lang::bindgen::BindingItem *handle = findItem(plan, "Handle");
  expect(handle != nullptr &&
             handle->kind == lang::bindgen::BindingItemKind::Alias &&
             handle->aliasSpelling == "Missing*",
         "a pointer typedef to an opaque record should alias its handle");
  const lang::bindgen::BindingItem *uses = findItem(plan, "uses_enum");
  expect(uses != nullptr &&
             uses->kind == lang::bindgen::BindingItemKind::Function &&
             uses->parameters[0].spelling == "Small" &&
             uses->parameters[1].spelling == "Named" &&
             uses->parameters[2].spelling == "Flags" &&
             uses->parameters[3].spelling == "Handle",
         "enum, alias, and handle parameters should spell through their "
         "names");
}

// -- Writer -----------------------------------------------------------------

void testWriterShapeAndDeterminism() {
  const std::string header = R"(
typedef struct Engine Engine;
typedef struct Vec2 { float x; float y; } Vec2;
enum { ENGINE_CAP_BASIC = 1 };
Engine *engine_create(int mode);
int engine_variadic(const char *format, ...);
)";
  lang::bindgen::BindingOptions options;
  options.namespaceName = "engine";
  const lang::bindgen::BindgenResult first =
      lang::bindgen::runCBindgen(header, "engine.h", options);
  const lang::bindgen::BindgenResult second =
      lang::bindgen::runCBindgen(header, "engine.h", options);
  expect(first.gtiSource == second.gtiSource,
         "identical inputs should produce byte-identical output");
  expect(contains(first.gtiSource, "namespace engine {"),
         "the namespace option should wrap the generated source names");
  expect(contains(first.gtiSource, "[[c_opaque]] struct Engine;"),
         "the opaque handle should emit");
  expect(contains(first.gtiSource, "mut c_float x;"),
         "record fields should emit as mutable");
  expect(contains(first.gtiSource, "constexpr int32_t ENGINE_CAP_BASIC = 1;") &&
             !contains(first.gtiSource, "using  ="),
         "anonymous enum constants should emit without an alias line");
  expect(contains(first.gtiSource, "extern \"C\" {"),
         "functions should emit inside one extern block");
  expect(contains(first.gtiSource, "// Not imported:") &&
             contains(first.gtiSource, "engine_variadic"),
         "the skip report should name what was not imported");
}

// Object-like integer macros import as constants: literal bases and
// suffixes, operator expressions, references to earlier macro constants,
// and C redefinition (latest value at the first position). Function-like
// macros and include guards are code and structure, never constants; an
// object-like macro that does not evaluate is noted, not approximated.
void testMacroConstantImport() {
  const lang::bindgen::BindgenResult result =
      lang::bindgen::runCBindgen(R"(
#define BASE 3
#define HEXFLAG 0x40
#define SHIFTED (1 << 5)
#define COMBINED (BASE | HEXFLAG)
#define NEGATIVE (-BASE)
#define SUFFIXED 7ull
#define WIDE 5000000000
#define REDEFINED 1
#undef REDEFINED
#define REDEFINED 2
#define GUARD_ONLY
#define CALL(x) ((x) + 1)
#define STRINGY "text"
#define FLOATY 1.5
#define TOO_BIG 0xffffffffffffffff
int use_base(int value);
)",
                                 "constants.h");
  expect(result.usable(), "the macro header should read");
  const std::string &source = result.gtiSource;
  expect(contains(source, "constexpr int32_t BASE = 3;"),
         "a plain integer macro should import");
  expect(contains(source, "constexpr int32_t HEXFLAG = 64;"),
         "a hex literal should evaluate");
  expect(contains(source, "constexpr int32_t SHIFTED = 32;"),
         "a shift expression should evaluate");
  expect(contains(source, "constexpr int32_t COMBINED = 67;"),
         "a reference to earlier macro constants should resolve");
  expect(contains(source, "constexpr int32_t NEGATIVE = int32_t(-3);"),
         "a negative value should emit through a conversion");
  expect(contains(source, "constexpr int32_t SUFFIXED = 7;"),
         "integer suffixes should strip");
  expect(contains(source, "constexpr int64_t WIDE = 5000000000;"),
         "a value beyond int32 should widen to int64_t");
  expect(contains(source, "constexpr int32_t REDEFINED = 2;"),
         "redefinition should keep the latest value");
  expect(!contains(source, "GUARD_ONLY") && !contains(source, "CALL"),
         "guards and function-like macros should stay silent");
  // C-06 constants cover every bounded form a C macro body can hold.
  expect(contains(source, "constexpr std::string_view STRINGY = \"text\";"),
         "a string macro should import as a string-view constant");
  expect(contains(source, "constexpr double FLOATY = 1.5;"),
         "a floating macro should import as a double constant");
  expect(!contains(source, "TOO_BIG"),
         "an out-of-range macro should not import");
  bool notedUnevaluable = false;
  for (const lang::bindgen::ReaderDiagnostic &diagnostic : result.diagnostics) {
    notedUnevaluable =
        notedUnevaluable ||
        contains(diagnostic.message, "'TOO_BIG' is not a bounded constant");
  }
  expect(notedUnevaluable,
         "a non-evaluable object-like macro should be noted, not dropped");
}

// An inline function-pointer parameter or field binds through a synthesized
// named callback alias — the language's own admitted form — deduplicated by
// shape and emitted before its first use. A shape the bounded ABI cannot
// express still skips with its reason, and a skipped declaration leaves no
// orphaned alias behind.
void testSynthesizedCallbackAliases() {
  const lang::bindgen::BindgenResult result =
      lang::bindgen::runCBindgen(R"(
typedef struct Engine Engine;
void engine_on_event(Engine *engine, void (*handler)(int code));
void engine_on_error(Engine *engine, void (*handler)(int code));
typedef struct Hooks { void (*on_tick)(void *user); } Hooks;
void engine_bad(void (*handler)(int code), long double *broken);
int engine_no_proto(void (*handler)());
)",
                                 "hooks.h");
  expect(result.usable(), "the callback header should read");
  const std::string &source = result.gtiSource;
  expect(
      contains(source, "using engine_on_event_handler_fn = (c_int) -> void;"),
      "the inline function pointer should synthesize a named alias");
  expect(contains(source, "engine_on_event(Engine* engine, "
                          "engine_on_event_handler_fn handler)"),
         "the parameter should spell the synthesized alias");
  expect(contains(source, "engine_on_error(Engine* engine, "
                          "engine_on_event_handler_fn handler)"),
         "an identical shape should reuse the first alias");
  expect(!contains(source, "engine_on_error_handler_fn"),
         "no second alias should exist for the same shape");
  expect(contains(source, "using Hooks_on_tick_fn = (void*) -> void;") &&
             contains(source, "mut Hooks_on_tick_fn on_tick;"),
         "a function-pointer field should bind through a synthesized alias");
  expect(contains(source, "[[c_abi]]\nstruct Hooks {"),
         "the callback field should keep the record on the [[c_abi]] path");
  const lang::bindgen::BindingItem *bad = findItem(result.plan, "engine_bad");
  expect(bad != nullptr && bad->kind == lang::bindgen::BindingItemKind::Skipped,
         "the declaration with an unbindable second parameter should skip");
  expect(!contains(source, "engine_bad_handler_fn"),
         "a skipped declaration should leave no orphaned alias");
  const lang::bindgen::BindingItem *noProto =
      findItem(result.plan, "engine_no_proto");
  expect(noProto != nullptr &&
             noProto->kind == lang::bindgen::BindingItemKind::Skipped &&
             contains(noProto->reason, "without a prototype"),
         "a prototype-less function pointer should skip with its reason");
  const std::size_t aliasAt = source.find("using engine_on_event_handler_fn");
  const std::size_t useAt = source.find("engine_on_event(Engine*");
  expect(aliasAt != std::string::npos && useAt != std::string::npos &&
             aliasAt < useAt,
         "the synthesized alias should emit before its first use");
}

// Read-only string arrays (`const char* const*`) share the language's
// c_string* spelling with `const char**` out parameters; a writable char**
// stays outside the bounded surface.
void testStringArrayParameters() {
  const lang::bindgen::BindgenResult result =
      lang::bindgen::runCBindgen(R"(
int run_tool(const char *const *arguments, int count);
int take_out(const char **out_message);
int take_writable(char **buffers);
)",
                                 "argv.h");
  expect(result.usable(), "the string-array header should read");
  const std::string &source = result.gtiSource;
  expect(contains(source, "run_tool(c_string* arguments, c_int count)"),
         "a const char* const* parameter should bind as c_string*");
  expect(contains(source, "take_out(c_string* out_message)"),
         "the const char** out parameter should stay bound");
  const lang::bindgen::BindingItem *writable =
      findItem(result.plan, "take_writable");
  expect(writable != nullptr &&
             writable->kind == lang::bindgen::BindingItemKind::Skipped,
         "a writable char** should stay outside the bounded surface");
}

// Shapes real system headers exposed: a use must never spell a typedef the
// plan skipped (it spells the canonical type instead), C prototype-scoped
// forward references of one tag collapse to one handle, and a record whose
// name GTI cannot spell (an interior '__') poisons every use instead of
// emitting an invalid declaration.
void testRealHeaderShapes() {
  const lang::bindgen::BindgenResult result =
      lang::bindgen::runCBindgen(R"(
typedef struct Info Info;
typedef Info **InfoHandle;
void info_init(InfoHandle handle);
int first_use(struct SharedTag *tag);
int second_use(struct SharedTag *tag);
struct util__queue { int head; };
int queue_size(struct util__queue *queue);
)",
                                 "shapes.h");
  expect(result.usable(), "the shapes header should read");
  const std::string &source = result.gtiSource;
  expect(contains(source, "info_init(Info** handle)"),
         "a use of a skipped Handle** typedef should spell the canonical "
         "out-parameter form");
  expect(!contains(source, "InfoHandle handle"),
         "the skipped typedef name should not appear at a use site");
  std::size_t handles = 0;
  for (std::size_t at = source.find("[[c_opaque]] struct SharedTag;");
       at != std::string::npos;
       at = source.find("[[c_opaque]] struct SharedTag;", at + 1)) {
    ++handles;
  }
  expect(handles == 1,
         "prototype-scoped forward references should collapse to one handle");
  expect(!contains(source, "struct util__queue {"),
         "a record with a reserved '__' spelling should not emit");
  const lang::bindgen::BindingItem *use = findItem(result.plan, "queue_size");
  expect(use != nullptr &&
             use->kind == lang::bindgen::BindingItemKind::Skipped &&
             contains(use->reason, "__"),
         "a use of the reserved-spelling record should skip with the reason");
}

// C-05: line markers in preprocessed input carry the include graph. System
// declarations are modeled — a system typedef resolves canonically and a
// system macro's value feeds user constants — but only root and user-header
// declarations emit; a system record referenced through pointers is pulled
// in as one opaque handle, and every origin reports its original file.
void testSourceProvenanceAndImportPolicy() {
  const lang::bindgen::BindgenResult result = lang::bindgen::runCBindgen(
      "# 1 \"/work/app.h\"\n"
      "# 1 \"<built-in>\" 1\n"
      "# 1 \"/work/app.h\" 2\n"
      "# 1 \"/usr/include/systime.h\" 1 3\n"
      "typedef long time_t;\n"
      "struct timespec { time_t seconds; long nanoseconds; };\n"
      "#define CLOCK_MONO 1\n"
      "int system_only(int value);\n"
      "# 2 \"/work/app.h\" 2\n"
      "# 1 \"/work/util.h\" 1\n"
      "int util_helper(int value);\n"
      "# 3 \"/work/app.h\" 2\n"
      "#define APP_FLAG (CLOCK_MONO | 4)\n"
      "time_t app_now(void);\n"
      "struct timespec *app_timespec(void);\n"
      "struct timespec app_by_value(void);\n",
      "app.h");
  expect(result.usable(), "the provenance input should read");
  expect(result.plan.sourceFiles.size() == 2,
         "the consumed-file table should list both included files");
  if (result.plan.sourceFiles.size() == 2) {
    expect(result.plan.sourceFiles[0].path == "/usr/include/systime.h" &&
               result.plan.sourceFiles[0].system,
           "the system header should classify as system provenance");
    expect(result.plan.sourceFiles[1].path == "/work/util.h" &&
               !result.plan.sourceFiles[1].system,
           "the user include should classify as user provenance");
  }
  const std::string &source = result.gtiSource;
  expect(contains(source, "c_long app_now()"),
         "a system typedef should spell canonically at its use");
  expect(!contains(source, "using time_t"),
         "the system typedef itself should not emit");
  expect(contains(source, "[[c_opaque]] struct timespec;") &&
             contains(source, "declared in /usr/include/systime.h"),
         "a referenced system record should pull in as one opaque handle");
  expect(contains(source, "timespec* app_timespec()"),
         "pointer uses of the pulled system record should import");
  expect(contains(source, "constexpr int32_t APP_FLAG = 5;"),
         "a user macro referencing a system macro should evaluate");
  expect(!contains(source, "CLOCK_MONO ="),
         "the system macro itself should not emit");
  expect(contains(source, "c_int util_helper(c_int value)"),
         "user-include declarations should import");
  expect(!contains(source, "system_only"),
         "system functions should stay out of the import and its skip "
         "report");
  const lang::bindgen::BindingItem *byValue =
      findItem(result.plan, "app_by_value");
  expect(byValue != nullptr &&
             byValue->kind == lang::bindgen::BindingItemKind::Skipped &&
             contains(byValue->reason, "opaque handle"),
         "a by-value use of a system record should skip with its reason");
  const std::string printed = lang::bindgen::printNativeModel(
      lang::bindgen::ClangCReader(lang::bindgen::CTargetProfile{})
          .read("# 1 \"/work/app.h\"\n"
                "# 1 \"/usr/include/systime.h\" 1 3\n"
                "typedef long time_t;\n"
                "# 2 \"/work/app.h\" 2\n"
                "time_t app_now(void);\n")
          .model);
  expect(contains(printed, "typedef time_t = c_long[i64] (external)"),
         "system declarations should print with external provenance");

  // The closing gate: the policy-filtered source is real GTI.
  const std::string program = result.gtiSource + "\nint main() {\n"
                                                 "  return 0;\n"
                                                 "}\n";
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "bindgen-provenance.gti", program, {standardLibrary / "prelude.gti"}, {},
      {standardLibrary});
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "the frontend should accept the policy-filtered bindings");
}

// C-07: opt-in policy. Allow/deny narrow the import by name while excluded
// declarations still resolve uses exactly like out-of-policy ones; a rename
// gives a declaration a usable GTI source name while functions retain their C
// symbol through `[[link_name]]`; and a declared pointer-plus-count contract
// emits `[[c_array(count)]]`, including the
// two-level result form the bounded surface otherwise refuses.
void testOptInImportPolicy() {
  const std::string header = R"(
typedef struct Point { int x; int y; } Point;
typedef struct Handle Handle;
struct lib__internal { int state; };
const Point *lib_points(unsigned int *count);
Handle **lib_handles(int *count);
int lib_take_internal(struct lib__internal *state);
int lib_internal_helper(int value);
int lib_public(int value);
)";

  {
    lang::bindgen::BindingOptions options;
    options.deny.push_back({"lib_internal_*"});
    const lang::bindgen::BindgenResult result =
        lang::bindgen::runCBindgen(header, "lib.h", options);
    expect(result.usable(), "the deny-configured header should read");
    expect(!contains(result.gtiSource, "lib_internal_helper"),
           "a denied declaration should not emit");
    expect(contains(result.gtiSource, "lib_public"),
           "declarations outside the deny list should emit");
  }
  {
    lang::bindgen::BindingOptions options;
    options.allow.push_back({"lib_public"});
    const lang::bindgen::BindgenResult result =
        lang::bindgen::runCBindgen(header, "lib.h", options);
    expect(contains(result.gtiSource, "lib_public") &&
               !contains(result.gtiSource, "lib_points"),
           "a non-empty allow list should admit only matching declarations");
  }
  {
    // The rename gives the reserved-spelling record a usable name, so the
    // function taking it through a pointer becomes importable.
    lang::bindgen::BindingOptions options;
    options.renames.emplace("lib__internal", "LibInternal");
    const lang::bindgen::BindgenResult result =
        lang::bindgen::runCBindgen(header, "lib.h", options);
    expect(contains(result.gtiSource, "[[c_abi]]\nstruct LibInternal {"),
           "the renamed record should emit under its GTI name");
    expect(contains(result.gtiSource, "lib_take_internal(LibInternal* state)"),
           "uses of the renamed record should spell the new name");
  }
  {
    // A function rename changes only its GTI source spelling; link_name keeps
    // the original C symbol exact.
    lang::bindgen::BindingOptions options;
    options.renames.emplace("lib_public", "lib_renamed");
    const lang::bindgen::BindgenResult result =
        lang::bindgen::runCBindgen(header, "lib.h", options);
    expect(contains(result.gtiSource, "[[link_name(\"lib_public\")]] c_int "
                                      "lib_renamed(c_int value)"),
           "a renamed function should expose its GTI name and retain its C "
           "link symbol");
    expect(result.plan.policyProblems.empty(),
           "a matched function rename should be accepted policy");
  }
  {
    lang::bindgen::BindingOptions options;
    options.arrayReturns.push_back({"lib_points", "count"});
    options.arrayReturns.push_back({"lib_handles", "count"});
    const lang::bindgen::BindgenResult result =
        lang::bindgen::runCBindgen(header, "lib.h", options);
    expect(contains(result.gtiSource,
                    "[[c_array(count)]] const Point* lib_points("
                    "c_uint* count)"),
           "a one-level array result should carry the attribute");
    expect(contains(result.gtiSource,
                    "[[c_array(count)]] Handle** lib_handles(c_int* count)"),
           "the two-level array result form should import with the "
           "attribute");
    const lang::bindgen::BindgenResult unannotated =
        lang::bindgen::runCBindgen(header, "lib.h", {});
    const lang::bindgen::BindingItem *handles =
        findItem(unannotated.plan, "lib_handles");
    expect(handles != nullptr &&
               handles->kind == lang::bindgen::BindingItemKind::Skipped,
           "without the contract the two-level result stays refused");
  }
  {
    // An unusable count parameter must refuse the function rather than emit
    // an attribute the frontend rejects.
    lang::bindgen::BindingOptions options;
    options.arrayReturns.push_back({"lib_points", "missing"});
    const lang::bindgen::BindgenResult result =
        lang::bindgen::runCBindgen(header, "lib.h", options);
    const lang::bindgen::BindingItem *points =
        findItem(result.plan, "lib_points");
    expect(points != nullptr &&
               points->kind == lang::bindgen::BindingItemKind::Skipped &&
               contains(points->reason, "does not name a parameter"),
           "an array count naming no parameter should refuse the function");
  }
  {
    // A configured entry that matches nothing is reported, never silent.
    lang::bindgen::BindingOptions options;
    options.deny.push_back({"lib_absent_*"});
    options.renames.emplace("AbsentType", "Present");
    options.arrayReturns.push_back({"lib_absent", "count"});
    const lang::bindgen::BindgenResult result =
        lang::bindgen::runCBindgen(header, "lib.h", options);
    expect(result.plan.policyProblems.size() == 3,
           "every unmatched configuration entry should be reported");
  }

  // The closing gate: a fully configured import is real GTI.
  lang::bindgen::BindingOptions options;
  options.deny.push_back({"lib_internal_*"});
  options.renames.emplace("lib__internal", "LibInternal");
  options.renames.emplace("lib_points", "points");
  options.arrayReturns.push_back({"lib_points", "count"});
  options.arrayReturns.push_back({"lib_handles", "count"});
  const lang::bindgen::BindgenResult configured =
      lang::bindgen::runCBindgen(header, "lib.h", std::move(options));
  const std::string program = configured.gtiSource + "\nint main() {\n"
                                                     "  return 0;\n"
                                                     "}\n";
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "bindgen-policy.gti", program, {standardLibrary / "prelude.gti"}, {},
      {standardLibrary});
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "the frontend should accept the configured bindings");
}

// -- Frontend acceptance ----------------------------------------------------

// The closing gate: generated bindings must be accepted by the real
// frontend, whose native-interop validation is the ABI authority.
void testGeneratedBindingsCompile() {
  const lang::bindgen::BindgenResult result =
      lang::bindgen::runCBindgen(R"(
#ifndef DEVICE_H
#define DEVICE_H
#define DEVICE_MAGIC 0x40
#define DEVICE_MASK (DEVICE_MAGIC | 0x3)
typedef struct Device Device;
typedef struct Extent { unsigned int width; unsigned int height; } Extent;
typedef struct Report { long long stamp; Extent extent; void *user; } Report;
enum DeviceState { DEVICE_OFF, DEVICE_ON = 4 };
enum { DEVICE_CAP_BASIC = 1, DEVICE_CAP_BURST = 2 };
typedef void (*DeviceLostCallback)(int code, const char *reason);
typedef struct Channels {
  int levels[2][3];
  float *taps[4];
  Device *devices[2];
  Extent extents[2];
  void *scratch[2];
  DeviceLostCallback on_lost;
  DeviceLostCallback handlers[2];
  enum DeviceState state;
} Channels;
Device *device_open(const char *path, enum DeviceState initial);
void device_close(Device *device);
int device_report(const Device *device, Report report, Extent *out_extent);
int device_channels(const Device *restrict device,
                    Channels *restrict out_channels);
const char *device_vendor(const Device *device);
void device_on_lost(Device *device, DeviceLostCallback callback);
void device_watch(Device *device, void (*observer)(int event, void *user));
int device_run(const char *const *arguments, int count);
#endif
)",
                                 "device.h");
  expect(result.usable(), "the acceptance header should read");
  expect(result.plan.skippedCount() == 0,
         "every declaration in the acceptance header should import");
  const std::string program = result.gtiSource + "\nint main() {\n"
                                                 "  return 0;\n"
                                                 "}\n";
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "bindgen-acceptance.gti", program, {standardLibrary / "prelude.gti"}, {},
      {standardLibrary});
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "the frontend should accept the generated bindings");
}

} // namespace

int main() {
  testReaderModel();
  testReaderPreprocessing();
  testReaderInvalidDeclarations();
  testReaderRecovery();
  testReaderCorpusGoldens();
  testReaderExpandedSurface();
  testReaderAbiSafetyGates();
  testCallingConventionFacts();
  testSourceProvenanceAndImportPolicy();
  testOptInImportPolicy();
  testReaderFieldAndQualifierForms();
  testRecordDegradation();
  testFieldPositionDegradation();
  testRecordFieldExpansion();
  testAnonymousEnumPlanning();
  testTypeMappingMatrix();
  testTargetDataModels();
  testEnumAndTypedefPlanning();
  testWriterShapeAndDeterminism();
  testMacroConstantImport();
  testSynthesizedCallbackAliases();
  testStringArrayParameters();
  testRealHeaderShapes();
  testGeneratedBindingsCompile();
  if (failures != 0) {
    std::cerr << failures << " bindgen test failure(s)\n";
    return 1;
  }
  std::cout << "bindgen tests passed\n";
  return 0;
}
