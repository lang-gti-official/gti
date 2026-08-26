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
  expect(contains(printed, "typedef Stamp = u64"),
         "the multi-word builtin typedef should model as u64");
  expect(contains(printed, "typedef Log = fn(i32, ptr(const char)) -> void"),
         "the callback typedef should model its shape");
  expect(contains(printed, "function engine_create(mode: i32) -> "
                           "ptr(record:Engine)"),
         "the pointer-returning prototype should model");
  expect(contains(printed, "function engine_submit(engine: ptr(record:Engine), "
                           "position: record:Vec2, stamp: alias:Stamp) -> i32"),
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
  expect(contains(printed, "function always() -> i32"),
         "the guarded declaration should survive preprocessing");
  expect(!contains(printed, "win_only"),
         "the false conditional should exclude its declarations");
  bool sawInclude = false;
  bool sawMacro = false;
  for (const lang::bindgen::ReaderDiagnostic &diagnostic : read.diagnostics) {
    sawInclude = sawInclude || contains(diagnostic.message, "#include");
    sawMacro = sawMacro || contains(diagnostic.message, "macro definitions");
    // Conditionals evaluate natively now; the bounded reader's
    // conditional-block policy note is gone.
    expect(!contains(diagnostic.message, "conditional block"),
           "conditionals should evaluate without a policy note");
  }
  expect(sawInclude && sawMacro,
         "include and macro policies should each report");
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
  expect(contains(printed, "function fine(value: i32) -> i32"),
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
             "typedef Stamp = u64\n"
             "typedef Log = fn(i32, ptr(const char)) -> void\n"
             "function engine_create(mode: i32) -> ptr(record:Engine)\n"
             "function engine_submit(engine: ptr(record:Engine), "
             "position: record:Vec2, stamp: alias:Stamp) -> i32\n",
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
  expect(lang::bindgen::printNativeModel(skips.model) ==
             "function variadic_call(format: ptr(const char), ...) -> i32\n"
             "record Fixed\n"
             "  field values: array[i32 x 4]\n"
             "function with_array_param(values: ptr(i32), count: i32) -> "
             "i32\n"
             "function stdint_names(a: i8, b: u16, c: i32, d: u64, e: u64, "
             "f: i64, g: u64, h: i64) -> i32\n",
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
  expect(contains(printed, "function with_const_array(values: ptr(const i32), "
                           "count: i32) -> i32"),
         "parameter array decay should preserve the element's const");
}

// ABI-affecting native facts the bounded surface cannot express are refused,
// never silently misread: packed layouts degrade, assembler labels skip, and
// typedef'd array parameters decay exactly as C does.
void testReaderAbiSafetyGates() {
  const lang::bindgen::ReadResult read = readC(R"(
struct Squeezed { int a; int b; } __attribute__((packed));
int renamed_symbol(int value) __asm__("_actual_symbol");
typedef int Row[4];
int sum_row(Row row);
)");
  expect(read.usable(), "the ABI-safety header should read");
  const std::string printed = lang::bindgen::printNativeModel(read.model);
  expect(!contains(printed, "renamed_symbol"),
         "an assembler-labeled function should not import under its C name");
  bool sawAsmLabel = false;
  for (const lang::bindgen::ReaderDiagnostic &diagnostic : read.diagnostics) {
    sawAsmLabel =
        sawAsmLabel || contains(diagnostic.message, "assembler label");
  }
  expect(sawAsmLabel, "the assembler-label skip should report its reason");
  expect(contains(printed, "function sum_row(row: ptr(i32)) -> i32"),
         "a typedef'd array parameter should decay to its element pointer");
  const lang::bindgen::BindingPlan plan =
      lang::bindgen::planBindings(read.model, {});
  const lang::bindgen::BindingItem *squeezed = findItem(plan, "Squeezed");
  expect(squeezed != nullptr &&
             squeezed->kind == lang::bindgen::BindingItemKind::Opaque &&
             contains(squeezed->reason, "attribute"),
         "a packed record should degrade to opaque with its reason");
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
  expect(contains(printed, "field cells: array[array[i32 x 3] x 2]"),
         "a multi-extent array field should model as nested arrays");
  expect(contains(printed, "field rows: array[ptr(i32) x 4]"),
         "a pointer-element array field should model faithfully");
  expect(contains(printed, "enum \n  FLAG_A = 1\n  FLAG_B = 2"),
         "a bare anonymous enum should model its constants");
  expect(contains(printed, "function copy_board(destination: "
                           "ptr(const record:Board), source: "
                           "ptr(const record:Board)) -> i32"),
         "'restrict' qualifiers should not disturb pointer parameters");
  expect(contains(printed, "function fill(buffer: ptr(i32), count: i32) "
                           "-> i32"),
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
  // GTI's [[c_abi]] field set has no c_string or nested-pointer position;
  // records using them must degrade honestly rather than generating source
  // the frontend rejects.
  const lang::bindgen::ReadResult read = readC(R"(
struct Labeled { const char *text; int code; };
struct Tails { const char **tails; };
)");
  const lang::bindgen::BindingPlan plan =
      lang::bindgen::planBindings(read.model, {});
  const lang::bindgen::BindingItem *labeled = findItem(plan, "Labeled");
  expect(labeled != nullptr &&
             labeled->kind == lang::bindgen::BindingItemKind::Opaque &&
             contains(labeled->reason, "c_string crosses only function"),
         "a 'const char*' field should degrade the record with its reason");
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
  expect(fieldSpelling("grid") == "int32_t grid[2][3]",
         "a multi-extent array field should spell every extent in order");
  expect(fieldSpelling("cursors") == "int32_t* cursors[4]",
         "a scalar-pointer array field should spell its element pointer");
  expect(fieldSpelling("gains") == "const float* gains[2]",
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
             scalars->parameters[0].spelling == "int8_t" &&
             scalars->parameters[1].spelling == "uint16_t" &&
             scalars->parameters[2].spelling == "int64_t" &&
             scalars->parameters[3].spelling == "uint64_t",
         "platform integer spellings should map through the LP64 profile");
  const lang::bindgen::BindingItem *pointers = findItem(plan, "pointers");
  expect(pointers != nullptr &&
             pointers->kind == lang::bindgen::BindingItemKind::Function &&
             pointers->resultSpelling == "void*" &&
             pointers->parameters[0].spelling == "c_string" &&
             pointers->parameters[1].spelling == "void*" &&
             pointers->parameters[2].spelling == "const double*",
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
  expectSkip("rejected_bool", "not a GTI C ABI scalar");
  expectSkip("rejected_char", "plain 'char'");
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
  expect(contains(lp64Model, "typedef NativeLong = i64") &&
             contains(lp64Model, "typedef NativeUnsignedLong = u64") &&
             contains(lp64Model, "function transform(value: i64, count: u64) "
                                 "-> i64"),
         "LP64 should model long as 64 bits and size_t as 64 bits");
  expect(contains(llp64Model, "typedef NativeLong = i32") &&
             contains(llp64Model, "typedef NativeUnsignedLong = u32") &&
             contains(llp64Model, "function transform(value: i32, count: u64) "
                                  "-> i32"),
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
             flags->aliasSpelling == "uint32_t",
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
  expect(contains(first.gtiSource, "mut float x;"),
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

// -- Frontend acceptance ----------------------------------------------------

// The closing gate: generated bindings must be accepted by the real
// frontend, whose native-interop validation is the ABI authority.
void testGeneratedBindingsCompile() {
  const lang::bindgen::BindgenResult result =
      lang::bindgen::runCBindgen(R"(
#ifndef DEVICE_H
#define DEVICE_H
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
  testReaderFieldAndQualifierForms();
  testRecordDegradation();
  testFieldPositionDegradation();
  testRecordFieldExpansion();
  testAnonymousEnumPlanning();
  testTypeMappingMatrix();
  testTargetDataModels();
  testEnumAndTypedefPlanning();
  testWriterShapeAndDeterminism();
  testGeneratedBindingsCompile();
  if (failures != 0) {
    std::cerr << failures << " bindgen test failure(s)\n";
    return 1;
  }
  std::cout << "bindgen tests passed\n";
  return 0;
}
