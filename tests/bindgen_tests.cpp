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
  lang::bindgen::BoundedCReader reader(profile);
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
         "the conditional block should be skipped as a unit");
  bool sawInclude = false;
  bool sawMacro = false;
  bool sawConditional = false;
  for (const lang::bindgen::ReaderDiagnostic &diagnostic : read.diagnostics) {
    sawInclude = sawInclude || contains(diagnostic.message, "#include");
    sawMacro = sawMacro || contains(diagnostic.message, "macro definitions");
    sawConditional =
        sawConditional || contains(diagnostic.message, "conditional block");
  }
  expect(sawInclude && sawMacro && sawConditional,
         "include, macro, and conditional policies should each report");
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

void testTypeMappingMatrix() {
  const lang::bindgen::ReadResult read = readC(R"(
typedef struct Item Item;
int scalars(signed char a, unsigned short b, long c, unsigned long long d);
float floats(float a, double b);
void *pointers(const char *name, void *user, const double *values);
int rejected_bool(_Bool flag);
int rejected_char(char letter);
int rejected_two_level(int **matrix);
int rejected_unknown(some_unknown_t value);
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
  expectSkip("rejected_two_level", "multi-level");
  expectSkip("rejected_unknown", "outside the bounded surface");
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
typedef void (*DeviceLostCallback)(int code, const char *reason);
Device *device_open(const char *path, enum DeviceState initial);
void device_close(Device *device);
int device_report(const Device *device, Report report, Extent *out_extent);
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
  testReaderRecovery();
  testRecordDegradation();
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
