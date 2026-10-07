#include "compiler_test_fixture.h"

namespace {

void testContainerScaffoldSurfaces() {
  const std::string source = R"(
#include <std/array>
#include <std/forward_list>
#include <std/iterator>
#include <std/list>
#include <std/span>
#include <std/string>
#include <std/vector>

int count_range<Iterator, Sentinel>(mut Iterator first, Sentinel last)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> {
  mut int count = 0;
  for (; first != last; ++first) {
    count++;
  }
  return count;
}

int main() {
  int initial[2] = {1, 2};
  mut std::array<int, 2> fixed{initial};
  mut std::array<int, 2> fixed_other{initial};
  fixed.fill(3);
  fixed.swap(fixed_other);
  int fixed_at = fixed.at(0);
  int fixed_ends = fixed.front() + fixed.back() + fixed_at;

  mut std::vector<int> dynamic{};
  mut std::vector<int> dynamic_other{};
  dynamic.resize(std::size_t(2));
  dynamic.resize(std::size_t(3), 7);
  dynamic.shrink_to_fit();
  dynamic.swap(dynamic_other);
  std::vector<int> dynamic_copy = dynamic.clone();
  int dynamic_ends = dynamic.front() + dynamic.back();

  mut std::string text = std::string("abc");
  mut std::string other_text = std::string("def");
  text.resize(std::size_t(4));
  text.pop_back();
  text.shrink_to_fit();
  text.swap(other_text);
  char text_at = text.at(0);
  char text_front = text.front();
  char text_back = text.back();
  bool text_state = text_at == text_front || text_at == text_back;
  std::string_view text_view = text.view();
  std::string_view text_view_copy = text_view;
  bool text_view_state = text_view_copy.size() == text.size();

  mut std::list<int> linked{};
  mut std::list<int> linked_other{};
  linked.push_back(1);
  linked.resize(std::size_t(2));
  linked.sort();
  int remove_value = 1;
  std::size_t linked_removed = linked.remove(remove_value);
  std::size_t linked_unique = linked.unique();
  linked.merge(std::move(linked_other));
  linked.reverse();
  int linked_count = count_range(linked.begin(), linked.end());

  mut std::forward_list<int> forward{};
  mut std::forward_list<int> forward_other{};
  forward.push_front(1);
  forward.resize(std::size_t(2));
  forward.sort();
  std::size_t forward_removed = forward.remove(remove_value);
  std::size_t forward_unique = forward.unique();
  forward.merge(std::move(forward_other));
  forward.reverse();
  int forward_count = count_range(forward.begin(), forward.end());

  std::span<int> view = dynamic.as_span();
  std::span<int> prefix = view.first(std::size_t(0));
  std::span<int> copied_view = view;
  bool view_state = view.empty() && prefix.empty();

  return fixed_ends + dynamic_ends + linked_count +
         forward_count + int(linked_removed + linked_unique +
                             forward_removed + forward_unique) +
         int(dynamic_copy.size()) + (text_state ? 0 : 1) +
         (text_view_state ? 0 : 1) + (view_state ? 0 : 1);
}
)";

  const lang::FrontendResult valid = lang::Frontend().analyze(
      "container-scaffolds.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected container scaffold diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "container scaffolds should expose current constrained modifiers, "
         "move-only traversal, and read-only view shapes");

  const lang::FrontendResult invalidatedSpan = lang::Frontend().analyze(
      "invalidated-span.gti",
      "#include <std/vector>\n"
      "#include <std/span>\n"
      "int main() {\n"
      "  mut std::vector<int> values = std::vector<int>({1, 2});\n"
      "  std::span<int> view = values.as_span();\n"
      "  values.push_back(3);\n"
      "  return view.empty() ? 0 : 1;\n"
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidatedSpan.canGenerateCode() &&
             hasDiagnosticCode(invalidatedSpan.diagnostics, "GTI-S2017") &&
             hasDiagnostic(invalidatedSpan.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live"),
         "mutating an owner while a copied read-only span is live should be "
         "rejected by the ordinary retained-loan rule");

  const lang::FrontendResult movedOwner = lang::Frontend().analyze(
      "moved-span-owner.gti",
      "#include <std/vector>\n"
      "#include <std/span>\n"
      "int main() {\n"
      "  mut std::vector<int> values = std::vector<int>({1, 2});\n"
      "  std::span<int> view = values.as_span();\n"
      "  std::vector<int> moved = std::move(values);\n"
      "  return view.empty() ? 0 : 1;\n"
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!movedOwner.canGenerateCode() &&
             hasDiagnosticCode(movedOwner.diagnostics, "GTI-S2017") &&
             hasDiagnostic(movedOwner.diagnostics,
                           "Cannot move storage while an overlapping borrow "
                           "may still be live"),
         "moving an owner while one of its spans is live should be rejected");

  const lang::FrontendResult escapedSpan = lang::Frontend().analyze(
      "escaped-span.gti",
      "#include <std/vector>\n"
      "#include <std/span>\n"
      "std::span<int> escape() {\n"
      "  mut std::vector<int> values = std::vector<int>({1, 2});\n"
      "  return values.as_span();\n"
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!escapedSpan.canGenerateCode() &&
             hasDiagnosticCode(escapedSpan.diagnostics, "GTI-S2045") &&
             hasDiagnostic(escapedSpan.diagnostics,
                           "borrowed return requires either one exact "
                           "global/static storage origin"),
         "a span must not escape the local owner that supplies its retained "
         "loan");

  const lang::FrontendResult invalidatedTextView = lang::Frontend().analyze(
      "invalidated-string-view.gti",
      "#include <std/string>\n"
      "T relay<T>(T value) { return value; }\n"
      "int main() {\n"
      "  mut std::string text = std::string(\"abc\");\n"
      "  std::string_view view = relay(text.view());\n"
      "  text.push_back('d');\n"
      "  return view.empty() ? 0 : 1;\n"
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidatedTextView.canGenerateCode() &&
             hasDiagnosticCode(invalidatedTextView.diagnostics, "GTI-S2017") &&
             hasDiagnostic(invalidatedTextView.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live"),
         "a concrete generic relay must preserve a dynamic string_view owner "
         "dependency and reject mutation while that result is live");

  const lang::FrontendResult movedTextOwner = lang::Frontend().analyze(
      "moved-string-view-owner.gti",
      "#include <std/string>\n"
      "int main() {\n"
      "  mut std::string text = std::string(\"abc\");\n"
      "  std::string_view view = text.view();\n"
      "  std::string moved = std::move(text);\n"
      "  return view.empty() ? 0 : 1;\n"
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!movedTextOwner.canGenerateCode() &&
             hasDiagnosticCode(movedTextOwner.diagnostics, "GTI-S2017") &&
             hasDiagnostic(movedTextOwner.diagnostics,
                           "Cannot move storage while an overlapping borrow "
                           "may still be live"),
         "moving a string while its dynamic string_view is live should be "
         "rejected");

  const std::string textOwner = R"(
#include <std/string>
class Label {
  mut std::string text = std::string("abc");
public:
  std::string_view view() { return text.view(); }
  void edit() mut { this.text.clear(); }
};
)";
  const auto ownerRelay = lang::Frontend().analyze(
      "nested-owner-view.gti", textOwner + R"(
std::string_view relay(Label& value) { return value.view(); }
int main() {
  Label value{};
  auto view = relay(value);
  return int(view.size());
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(ownerRelay.canGenerateCode() &&
             lang::verifyMirProgram(ownerRelay.mir).valid(),
         "a text view can relay a checked nested owner dependency");
  for (const std::string &body :
       {std::string("int main() { mut Label value{}; auto view = value.view(); "
                    "value.edit(); return int(view.size()); }"),
        std::string(
            "std::string_view bad() { Label value{}; return value.view(); }"),
        std::string(
            "std::string_view bad(Label value) { return value.view(); }"),
        std::string("int main() { auto view = Label().view(); return "
                    "int(view.size()); }"),
        std::string(
            "int main() { Label value{}; auto view = value.view(); "
            "auto moved = std::move(value); return int(view.size()); }")}) {
    const auto rejected = lang::Frontend().analyze(
        "invalid-nested-owner-view.gti", textOwner + body,
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    if (rejected.canGenerateCode() ||
        (!hasDiagnosticCode(rejected.diagnostics, "GTI-S2017") &&
         !hasDiagnosticCode(rejected.diagnostics, "GTI-S2045"))) {
      std::cerr << "Unexpected nested-owner result for: " << body << '\n';
      for (const auto &diagnostic : rejected.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      }
    }
    expect(!rejected.canGenerateCode() &&
               (hasDiagnosticCode(rejected.diagnostics, "GTI-S2017") ||
                hasDiagnosticCode(rejected.diagnostics, "GTI-S2045")),
           "nested text-owner relays must reject invalidation, local escape, "
           "by-value ownership escape, and temporary owners");
  }

  for (const std::string &method :
       {std::string("std::string_view view() { std::string text{\"local\"}; "
                    "return text.view(); }"),
        std::string(
            "std::string_view view(std::string text) { return text.view(); }"),
        std::string(
            "static std::string_view view() { return text.view(); }")}) {
    const auto rejected = lang::Frontend().analyze(
        "shadowed-field-view.gti",
        "#include <std/string>\nclass Owner { std::string text{\"field\"}; "
        "public: " +
            method + " };",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!rejected.canGenerateCode() &&
               (hasDiagnosticCode(rejected.diagnostics, "GTI-S2045") ||
                hasDiagnosticCode(rejected.diagnostics, "GTI-S2039")),
           "implicit-field predeclaration must not authorize local, by-value "
           "parameter or static receiver escape: " +
               method);
  }

  const auto parameterShadow = lang::Frontend().analyze(
      "parameter-shadow-field.gti", R"(
#include <std/string>
class Owner {
  mut std::string text{"field"};
public:
  std::string_view view(std::string& text) { return text.view(); }
  void edit() mut { text.clear(); }
};
int main() {
  mut Owner owner{};
  std::string text{"argument"};
  auto view = owner.view(text);
  owner.edit();
  return int(view.size());
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(parameterShadow.canGenerateCode() &&
             lang::verifyMirProgram(parameterShadow.mir).valid(),
         "a same-named read-only parameter retains its own origin, not this");

  const auto staticText = lang::Frontend().analyze("static-field-view.gti", R"(
#include <std/string>
class StaticText {
  static std::string_view text = "static";
public:
  std::string_view view() { return text; }
};
int main() {
  auto text = StaticText().view();
  return int(text.size());
}
)",
                                                   {standardLibraryPrelude()},
                                                   {}, {standardLibraryRoot()});
  expect(staticText.canGenerateCode() &&
             lang::verifyMirProgram(staticText.mir).valid(),
         "a literal-backed static field must not gain a receiver dependency");

  const lang::FrontendResult escapedTextView = lang::Frontend().analyze(
      "escaped-string-view.gti",
      "#include <std/string>\n"
      "std::string_view escape() {\n"
      "  mut std::string text = std::string(\"abc\");\n"
      "  return text.view();\n"
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!escapedTextView.canGenerateCode() &&
             hasDiagnosticCode(escapedTextView.diagnostics, "GTI-S2045") &&
             hasDiagnostic(escapedTextView.diagnostics,
                           "dynamic std::string_view cannot escape without "
                           "one exact owner dependency"),
         "a dynamic string_view must not escape the local string that owns "
         "its bytes");

  const lang::FrontendResult storedTextView = lang::Frontend().analyze(
      "stored-string-view.gti",
      "#include <std/string>\n"
      "mut std::string_view saved = \"\";\n"
      "void retain(std::string_view value) { saved = value; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!storedTextView.canGenerateCode() &&
             hasDiagnosticCode(storedTextView.diagnostics, "GTI-S2045") &&
             hasDiagnostic(storedTextView.diagnostics,
                           "cannot be assigned into an existing binding"),
         "an opaque string_view parameter must not be retained in global "
         "storage without an expressible dependency graph");

  const lang::FrontendResult nestedTextView = lang::Frontend().analyze(
      // Use an existing trusted unit that is not in string's dependency graph;
      // overriding memory.gti here would create an include cycle.
      standardLibraryRoot() / "std/cmath.gti", R"(
#include <std/string>
#include <std/memory>

int main() {
  mut std::string text = std::string("abc");
  mut gti_internal::prefix_storage<std::string_view> stored =
      gti_internal::allocate_prefix_storage<std::string_view>(uint64_t(1));
  gti_internal::prefix_storage_append(stored, text.view());
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!nestedTextView.canGenerateCode() &&
             hasDiagnosticCode(nestedTextView.diagnostics, "GTI-S2045") &&
             hasDiagnostic(nestedTextView.diagnostics,
                           "cannot be nested in dynamic storage"),
         "a dynamic string_view must not be hidden in compiler-private "
         "storage without a nested dependency representation");
  if (!hasDiagnosticCode(nestedTextView.diagnostics, "GTI-S2045"))
    for (const auto &diagnostic : nestedTextView.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';

  const lang::FrontendResult validCString = lang::Frontend().analyze(
      "string-c-string.gti",
      "#include <std/string>\n"
      "int main() {\n"
      "  mut std::string text = std::string(\"abc\");\n"
      "  c_string unchecked = text.c_string();\n"
      "  c_string checked = text.try_c_string();\n"
      "  return unchecked == nullptr || checked == nullptr ? 1 : 0;\n"
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(validCString.canGenerateCode() && validCString.mirValid &&
             validCString.mir.valid(),
         "string C-string observers should lower as verified read-only "
         "owner-dependent values");

  const lang::FrontendResult invalidatedCString = lang::Frontend().analyze(
      "invalidated-c-string.gti",
      "#include <std/string>\n"
      "int main() {\n"
      "  mut std::string text = std::string(\"abc\");\n"
      "  c_string pointer = text.c_string();\n"
      "  text.push_back('d');\n"
      "  return pointer == nullptr ? 1 : 0;\n"
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidatedCString.canGenerateCode() &&
             hasDiagnosticCode(invalidatedCString.diagnostics, "GTI-S2017") &&
             hasDiagnostic(invalidatedCString.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live"),
         "mutating a string while one of its C-string pointers remains live "
         "should be rejected");

  const lang::FrontendResult relayedCString = lang::Frontend().analyze(
      "relayed-c-string.gti",
      "#include <std/string>\n"
      "c_string relay(c_string value) { return value; }\n"
      "int main() {\n"
      "  mut std::string text = std::string(\"abc\");\n"
      "  c_string pointer = relay(text.c_string());\n"
      "  text.push_back('d');\n"
      "  return pointer == nullptr ? 1 : 0;\n"
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!relayedCString.canGenerateCode() &&
             hasDiagnosticCode(relayedCString.diagnostics, "GTI-S2017"),
         "relaying an owner-dependent C string through an exact c_string "
         "parameter and return should preserve its source loan");

  const std::string escapedCStringSource =
      "#include <std/string>\n"
      "c_string escape() {\n"
      "  mut std::string text = std::string(\"abc\");\n"
      "  return text.c_string();\n"
      "}\n";
  const lang::FrontendResult escapedCString = lang::Frontend().analyze(
      "escaped-c-string.gti", escapedCStringSource, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  const std::size_t escapedCStringSpan =
      escapedCStringSource.find("text.c_string()") +
      std::string_view("text.c_string()").size() - 1;
  expect(!escapedCString.canGenerateCode() &&
             hasDiagnosticCode(escapedCString.diagnostics, "GTI-S2045") &&
             hasDiagnosticAt(escapedCString.diagnostics, "GTI-S2045",
                             "escaped-c-string.gti", escapedCStringSpan,
                             "owner-dependent c_string cannot escape without "
                             "one exact owner dependency") &&
             hasDiagnosticHint(escapedCString.diagnostics,
                               "Return an ownerless native C string"),
         "a C-string pointer must not escape the local string that owns its "
         "bytes");

  const lang::FrontendResult nativeCString =
      lang::Frontend().analyze("native-c-string-relay.gti", R"(
extern "C" {
  c_string native_name();
}

c_string identity(c_string value) { return value; }

int main() {
  c_string value = identity(native_name());
  return value == nullptr ? 1 : 0;
}
)",
                               {standardLibraryPrelude()});
  expect(nativeCString.canGenerateCode() && nativeCString.mirValid &&
             nativeCString.mir.valid(),
         "native C-string results without a GTI owner should remain usable "
         "through an exact C-string relay");
}

void testMovedLoopLocalRedeclarations() {
  const lang::FrontendResult trivialLocals =
      lang::Frontend().analyze("moved-loop-local-trivial.gti", R"(
int32_t sink(int32_t value) { return value; }

int main() {
  mut int32_t total = 0;
  mut int32_t index = 0;
  while (index < 2) {
    int32_t element = index + 1;
    total = total + std::move(element);
    index++;
  }

  for (mut int32_t iteration = 0; iteration < 2; iteration++) {
    int32_t local = iteration + 1;
    total = total + std::move(local);
  }

  mut int32_t values[4] = {0, 0, 0, 0};
  mut int32_t position = 0;
  while (position < 4) {
    int32_t element = position * 10;
    values[position] = std::move(element);
    position++;
  }

  mut int32_t guard = 0;
  while (guard < 4) {
    int32_t element = guard + 1;
    if (guard % 2 == 0) {
      total = total + sink(std::move(element));
    }
    guard++;
  }
  return total + values[3];
}
)",
                               {standardLibraryPrelude()});
  expect(trivialLocals.canGenerateCode() && trivialLocals.mirValid &&
             trivialLocals.mir.valid(),
         "a moved trivially dropped loop-local must leave its storage "
         "re-initializable across while, for, indexed, and conditional "
         "backedge forms");

  const lang::FrontendResult genericLocals =
      lang::Frontend().analyze("moved-loop-local-generic.gti", R"(
T accumulate<std::numeric T>(T seed, T limit) {
  mut T total = seed;
  mut T step = seed;
  while (step < limit) {
    T element = step + T(1);
    total = total + std::move(element);
    step = step + T(1);
  }
  return total;
}

int main() {
  int32_t narrow = accumulate<int32_t>(0, 2);
  int64_t wide = accumulate<int64_t>(int64_t(0), int64_t(2));
  return narrow + int32_t(wide);
}
)",
                               {standardLibraryPrelude()});
  expect(genericLocals.canGenerateCode() && genericLocals.mirValid &&
             genericLocals.mir.valid(),
         "every generic instance must rearm a moved trivially dropped "
         "loop-local for the next iteration");

  const lang::FrontendResult lexicalLocals =
      lang::Frontend().analyze("moved-loop-local-lexical.gti", R"(
class Tracker {
  int32_t value;

public:
  Tracker(int32_t initial) : value(initial) {}
  Tracker(Tracker&& other) = default;
  ~Tracker() {}

  int32_t read() { return this.value; }
};

int32_t consume(Tracker taken) { return taken.read(); }

int main() {
  mut int32_t total = 0;
  mut int32_t index = 0;
  while (index < 4) {
    mut Tracker element{index + 1};
    if (index % 2 == 0) {
      total = total + consume(std::move(element));
    }
    index++;
  }
  return total;
}
)",
                               {standardLibraryPrelude()});
  expect(lexicalLocals.canGenerateCode() && lexicalLocals.mirValid &&
             lexicalLocals.mir.valid(),
         "a conditionally moved lexically dropped loop-local should verify "
         "through its scope-end drop across the loop backedge");

  const lang::HirFunctionInstance *lexicalMain = nullptr;
  for (const lang::HirFunctionInstance &function :
       lexicalLocals.hir.functionInstances()) {
    if (function.source != nullptr &&
        function.source->name().lexeme == "main") {
      lexicalMain = &function;
      break;
    }
  }
  lang::MirProgram hiddenScopeEnd = lexicalLocals.mir;
  std::size_t hiddenDrops = 0;
  if (lexicalMain != nullptr) {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        hiddenScopeEnd.functionInstances());
    for (lang::MirFunctionInstance &function : functions) {
      if (function.id != lexicalMain->id) {
        continue;
      }
      for (lang::MirBlock &block : function.body.blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Drop ||
              !instruction.destination) {
            continue;
          }
          const lang::MirPlace *place =
              function.body.findPlace(*instruction.destination);
          if (place == nullptr ||
              place->root != lang::MirPlaceRootKind::Binding ||
              !place->projections.empty() ||
              place->traits.drop != lang::DropKind::Lexical || !place->key) {
            continue;
          }
          instruction.ownership =
              lang::OwnershipEvent{.kind = lang::OwnershipEventKind::Read,
                                   .place = *place->key,
                                   .reachable = false};
          ++hiddenDrops;
        }
      }
    }
  }
  const lang::MirVerificationResult hiddenScopeEndResult =
      lang::verifyMirProgram(hiddenScopeEnd);
  expect(hiddenDrops != 0 && !hiddenScopeEndResult.valid() &&
             hasMirVerificationError(
                 hiddenScopeEndResult,
                 "initialization targets a place whose lifetime is already "
                 "active"),
         "a lexically dropped loop-local whose scope-end drop is hidden from "
         "the ownership flow model must still fail re-initialization");
}

void testInferredVoidGenericArguments() {
  const lang::FrontendResult inferredVoid =
      lang::Frontend().analyze("inferred-void-generic.gti", R"(
T* identity_pointer<T>(T* pointer) { return pointer; }

int main() {
  void* pointer = nullptr;
  void* copied = identity_pointer(pointer);
  return copied == nullptr ? 0 : 1;
}
)");
  expect(inferredVoid.canGenerateCode(),
         "implicit generic inference should accept void metadata when the "
         "actual parameter and result are pointer values");

  const lang::FrontendResult valid =
      lang::Frontend().analyze("inferred-void-pointer-generic.gti", R"(
T identity<T>(T value) { return value; }

int main() {
  void* pointer = nullptr;
  void* copied = identity(pointer);
  return copied == nullptr ? 0 : 1;
}
)");
  expect(valid.canGenerateCode(),
         "generic inference should continue accepting void* as a complete "
         "pointer type");
}

void testTrustedIntrinsicDeclarations() {
  const lang::FrontendResult trusted =
      lang::Frontend().analyze("trusted-intrinsic-declaration.gti", R"(
namespace utility = std;

int main() {
  int source = 1;
  int moved = utility::move(source);
  return moved;
}
)",
                               {standardLibraryPrelude()});
  expect(trusted.canGenerateCode(),
         "a namespace alias should retain the trusted move declaration's "
         "intrinsic identity");

  const lang::FunctionDecl *trustedMain =
      findTopLevelFunction(trusted.program, "main");
  const auto *trustedMoved =
      trustedMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                trustedMain->body()->statements().at(1).get());
  const auto *trustedCall =
      trustedMoved == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(trustedMoved->initializer().get());
  const lang::ResolvedCallInfo *trustedResolution =
      trustedCall == nullptr ? nullptr
                             : trusted.semantics.findCall(*trustedCall);
  const lang::FunctionInfo *trustedDeclaration =
      trustedResolution == nullptr || trustedResolution->declaration == nullptr
          ? nullptr
          : trusted.semantics.findFunction(*trustedResolution->declaration);
  const lang::SourceUnit *trustedUnit =
      trustedDeclaration == nullptr
          ? nullptr
          : trusted.sourceGraph.findUnit(trustedDeclaration->sourceUnit);
  expect(trustedResolution != nullptr && trustedResolution->function != 0 &&
             trustedResolution->intrinsic == lang::IntrinsicKind::Move &&
             trustedDeclaration != nullptr &&
             trustedDeclaration->intrinsic == lang::IntrinsicKind::Move &&
             trustedUnit != nullptr && trustedUnit->prelude,
         "resolved intrinsic calls should point to a trusted prelude "
         "declaration instead of deriving behavior from call-site spelling");
  expect(trustedResolution != nullptr &&
             std::none_of(trusted.hir.functionInstances().begin(),
                          trusted.hir.functionInstances().end(),
                          [&](const lang::HirFunctionInstance &instance) {
                            return instance.declaration ==
                                   trustedResolution->function;
                          }),
         "intrinsic declarations should remain semantic operations rather than "
         "bodyless HIR call targets");

  const lang::FrontendResult ordinary =
      lang::Frontend().analyze("ordinary-function-named-move.gti", R"(
namespace std {
T move<T>(T& value) { return value; }
}

int main() {
  int source = 1;
  int copied = std::move(source);
  return source + copied - 2;
}
)");
  expect(ordinary.canGenerateCode(),
         "an ordinary function named std::move should follow ordinary call "
         "semantics outside the trusted prelude");
  const lang::FunctionDecl *ordinaryMain =
      findTopLevelFunction(ordinary.program, "main");
  const auto *ordinarySource =
      ordinaryMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                ordinaryMain->body()->statements().front().get());
  const auto *ordinaryCopied =
      ordinaryMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                ordinaryMain->body()->statements().at(1).get());
  const auto *ordinaryCall = ordinaryCopied == nullptr
                                 ? nullptr
                                 : dynamic_cast<const lang::Call *>(
                                       ordinaryCopied->initializer().get());
  const lang::BindingInfo *ordinarySourceBinding =
      ordinarySource == nullptr
          ? nullptr
          : ordinary.semantics.findBinding(*ordinarySource);
  const lang::ResolvedCallInfo *ordinaryResolution =
      ordinaryCall == nullptr ? nullptr
                              : ordinary.semantics.findCall(*ordinaryCall);
  expect(ordinarySourceBinding != nullptr &&
             !ordinarySourceBinding->explicitlyMoved &&
             ordinaryResolution != nullptr &&
             ordinaryResolution->intrinsic == lang::IntrinsicKind::None,
         "untrusted declarations must not acquire ownership effects by using "
         "a standard-library spelling");

  const lang::FrontendResult ordinaryMutableReturn =
      lang::Frontend().analyze("ordinary-mutable-return.gti", R"(
mut int& expose(mut int& value) { return value; }
int main() { return 0; }
)");
  expect(ordinaryMutableReturn.canGenerateCode() &&
             lang::verifyMirProgram(ordinaryMutableReturn.mir).valid(),
         "an ordinary mutable reference relay should use the same exact "
         "argument dependency proof without acquiring an intrinsic");
}

void testThisReceiverKeyword() {
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens = lexer.scan("this self");
  expect(tokens.size() >= 3 && tokens[0].kind == lang::TokenKind::THIS &&
             tokens[1].kind == lang::TokenKind::IDENTIFIER,
         "this should be the receiver keyword and self should remain an "
         "ordinary identifier");

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("this-receiver.gti", R"(
class Counter {
  int value = 4;
public:
  int read() {
    int self = 1;
    return this.value + self - 1;
  }
};

int main() {
  Counter counter = Counter();
  return counter.read() - 4;
}
)");
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "this member access and self identifiers should compile together");

  bool foundThisValue = false;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      if (value.kind == lang::HirValueKind::This) {
        foundThisValue = true;
      }
    }
  }
  expect(foundThisValue,
         "typed HIR should preserve the current-object expression");

  const std::string generated = gti_test::emitCppText(frontend);
  expect(generated.find("const auto &__gti_mir_p_") != std::string::npos &&
             generated.find("= (*this).value;") != std::string::npos &&
             generated.find("// GTI verified-MIR body:") != std::string::npos,
         "the C++ backend should lower GTI this as a checked object receiver");

  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-this.gti", "int main() { return this.value; }");
  expect(
      !invalid.canGenerateCode() &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot use 'this' outside a class or struct method"),
      "this should be rejected outside an instance method");
}

void testClassesStructsAndAccess() {
  lang::Lexer lexer;
  const std::string validSource = R"(
class Vault {
  int secret = 7;

public:
  int reveal() { return this.secret; }
  int reveal_other(Vault other) { return other.secret; }
};

struct Reading {
  int value = 1;

private:
  int hidden = 2;

public:
  int total() { return this.value + this.hidden; }
};

int open(mut Vault vault) { return vault.reveal(); }
int read(Reading reading) { return reading.value; }
int main() {
  Vault vault{};
  Reading reading{};
  return vault.reveal() + reading.total() - 10;
}
)";
  auto validTokens = lexer.scan(validSource);
  expect(!lexer.hadError(), "class and struct access syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "class and struct access syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "public members and same-class private access should resolve");

  const lang::FrontendResult classFrontend = lang::Frontend().analyze(
      "classes-and-access.gti", validSource, {standardLibraryPrelude()});
  expect(classFrontend.canGenerateCode(),
         "access-controlled declaration defaults should retain verified MIR");
  if (!classFrontend.canGenerateCode())
    return;
  const auto *readingDeclaration =
      findTopLevelClass(classFrontend.program, "Reading");
  const auto *readingType =
      readingDeclaration == nullptr
          ? nullptr
          : classFrontend.semantics.findClassType(*readingDeclaration);
  const auto reading = std::find_if(
      classFrontend.mir.classInstances().begin(),
      classFrontend.mir.classInstances().end(), [&](const auto &owner) {
        return readingType != nullptr && owner.declaration == readingType->id;
      });
  expect(reading != classFrontend.mir.classInstances().end() &&
             reading->declaredFields.size() == 2 &&
             std::all_of(reading->declaredFields.begin(),
                         reading->declaredFields.end(),
                         [](const auto &field) {
                           return field.access == lang::AccessMode::ReadOnly;
                         }) &&
             std::any_of(classFrontend.mir.constructorInstances().begin(),
                         classFrontend.mir.constructorInstances().end(),
                         [&](const auto &constructor) {
                           return constructor.owner == reading->id &&
                                  constructor.generatedDefault &&
                                  constructor.initializers.size() == 2;
                         }),
         "immutable fields retain read-only MIR identity and execute their "
         "defaults through the selected generated constructor");
  const std::string generated = gti_test::emitCppText(classFrontend);
  expect(generated.find("class Vault;") != std::string::npos &&
             generated.find("struct Reading;") != std::string::npos &&
             generated.find("class Vault {") != std::string::npos &&
             generated.find("struct Reading {") != std::string::npos &&
             generated.find("std::int32_t __gti_fn_") != std::string::npos &&
             generated.find("_reveal() const;") != std::string::npos &&
             generated.find("private:\n    std::int32_t hidden;") !=
                 std::string::npos &&
             generated.find("= (*this).hidden;") != std::string::npos,
         "emitter should preserve declaration kinds, access labels, and "
         "frontend-owned field immutability");

  auto invalidTokens = lexer.scan(R"(
class A {
  int private_value = 1;
public:
  int visible = 2;
  int inspect(A other) { return other.private_value; }
};

struct B {
  int public_value = 1;
private:
  int hidden = 2;
};

A wrong_type(B value) { return value; }
int read_class_private(A value) { return value.private_value; }
int read_struct_private(B value) { return value.hidden; }
int read_missing(B value) { return value.missing; }

class Duplicate {
  int value = 1;
public:
  int value = 2;
};

class InvalidFields {
  mut int missing_initializer;
  int invalid_reference = missing_initializer;
  int invalid_receiver = this.private_value;
};

int invalid_global_receiver = this.private_value;
MissingType unresolved();
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid class semantics should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(
      !invalidSemantic.check(invalidProgram),
      "nominal, access, member, field, and receiver errors should be rejected");
  expect(hasDiagnostic(invalidSemantic, "Cannot return a value of type"),
         "different nominal class types should not be assignable");
  expect(hasDiagnostic(invalidSemantic, "of 'A' is private") &&
             hasDiagnostic(invalidSemantic, "of 'B' is private"),
         "class defaults and explicit private labels should be enforced");
  expect(hasDiagnostic(invalidSemantic, "Unknown member 'missing'"),
         "unknown members should be diagnosed on their nominal type");
  expect(hasDiagnostic(invalidSemantic, "Duplicate member declaration"),
         "duplicate fields and methods should be rejected");
  expect(
      hasDiagnostic(invalidSemantic, "fields must have an initializer"),
      "mutable fields should require initialization until constructors exist");
  expect(hasDiagnostic(invalidSemantic, "referenced from field initializers"),
         "field initializers should not depend on member initialization order");
  expect(hasDiagnostic(invalidSemantic, "outside a class or struct method"),
         "this should be rejected in fields and outside methods");
  expect(hasDiagnostic(invalidSemantic, "Unknown type 'MissingType'"),
         "unknown nominal types should be diagnosed");

  auto conditionalTokens = lexer.scan(R"(
class PlatformValue {
#if target.os == "public-os"
public:
#else
private:
#endif
  int value = 1;
};
int read_platform(PlatformValue value) { return value.value; }
)");
  lang::Parser conditionalParser(std::move(conditionalTokens));
  lang::Program conditionalProgram = conditionalParser.parse();
  expect(!conditionalParser.hadError(),
         "conditional access labels should parse in class bodies");

  lang::SemanticVisitor publicTarget(
      lang::TargetInfo{.os = "public-os", .vendor = "test", .arch = "test"});
  lang::SemanticVisitor privateTarget(
      lang::TargetInfo{.os = "private-os", .vendor = "test", .arch = "test"});
  expect(publicTarget.check(conditionalProgram),
         "the active public access branch should expose following members");
  expect(!privateTarget.check(conditionalProgram) &&
             hasDiagnostic(privateTarget, "is private"),
         "the active private access branch should hide following members");

  auto recoveryTokens = lexer.scan(R"(
struct Recovered {
  public int value = 1;
private:
  int hidden = 2;
};
int main() { return 0; }
)");
  lang::Parser recoveryParser(std::move(recoveryTokens));
  lang::Program recoveryProgram = recoveryParser.parse();
  expect(
      recoveryParser.errors().size() == 1 &&
          recoveryProgram.declarations().size() == 2,
      "parser recovery should resume at access labels and later declarations");

  const std::string formatted = lang::Formatter().format(
      "class Box{public:int value=1;private:int hidden=2;};"
      "struct Point{int x=0;};");
  expect(formatted == "class Box {\npublic:\n  int value = 1;\nprivate:\n"
                      "  int hidden = 2;\n};\nstruct Point {\n"
                      "  int x = 0;\n};\n",
         "formatter should outdent C++-style access labels");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted class access labels should be idempotent");
}

void testMutableFieldGroups() {
  const lang::TargetInfo target = lang::TargetInfo::host();
  const std::string source = R"(
#define FIELD_GROUP_ACTIVE

struct Counter {
public:
  mut {
    int first = 1;
#ifdef FIELD_GROUP_ACTIVE
    int second = 2;
#else
    int second = 20;
#endif
  }

  int identity = 4;

  void increment() mut {
    this.first++;
    this.second++;
  }

  int value() {
    return this.first + this.second + this.identity;
  }
};

[[c_abi]] struct GroupedNative {
  mut {
    int32_t x;
    int32_t y;
  }
};

[[c_abi]] struct ExpandedNative {
  mut int32_t x;
  mut int32_t y;
};

int main() {
  mut Counter counter = Counter();
  counter.increment();
  return counter.value() == 9 ? 0 : 1;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend({.target = target})
          .analyze("mutable-field-groups.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "mutable field groups should compile as ordinary mutable fields");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::ClassDecl *counter = nullptr;
  const lang::ClassDecl *groupedNative = nullptr;
  const lang::ClassDecl *expandedNative = nullptr;
  for (const lang::StmtPtr &declaration : frontend.program.declarations()) {
    const auto *classDecl =
        dynamic_cast<const lang::ClassDecl *>(declaration.get());
    if (classDecl == nullptr) {
      continue;
    }
    if (classDecl->name().lexeme == "Counter") {
      counter = classDecl;
    } else if (classDecl->name().lexeme == "GroupedNative") {
      groupedNative = classDecl;
    } else if (classDecl->name().lexeme == "ExpandedNative") {
      expandedNative = classDecl;
    }
  }
  expect(counter != nullptr && groupedNative != nullptr &&
             expandedNative != nullptr,
         "field-group fixtures should retain their class declarations");
  if (counter == nullptr || groupedNative == nullptr ||
      expandedNative == nullptr) {
    return;
  }

  const lang::MutableFieldGroupDecl *group = nullptr;
  for (const lang::StmtPtr &member : counter->members()) {
    if (const auto *candidate =
            dynamic_cast<const lang::MutableFieldGroupDecl *>(member.get())) {
      group = candidate;
      break;
    }
  }
  expect(group != nullptr && !group->members().empty(),
         "the AST should preserve the mutable field-group source container");
  if (group == nullptr) {
    return;
  }
  const auto *first =
      dynamic_cast<const lang::VariableDecl *>(group->members().front().get());
  expect(first != nullptr && first->isMutable(),
         "grouped field declarations should carry effective mutable "
         "semantics without an individual mut token");

  const lang::ClassTypeInfo *counterInfo =
      frontend.semantics.findClassType(*counter);
  expect(counterInfo != nullptr && counterInfo->fields.size() == 3 &&
             counterInfo->fields[0].declaration->name().lexeme == "first" &&
             counterInfo->fields[1].declaration->name().lexeme == "second" &&
             counterInfo->fields[2].declaration->name().lexeme == "identity" &&
             counterInfo->fields[0].declaration->isMutable() &&
             counterInfo->fields[1].declaration->isMutable() &&
             !counterInfo->fields[2].declaration->isMutable(),
         "semantic collection should flatten active grouped fields in source "
         "order without changing following immutable fields");

  const lang::ClassTypeInfo *groupedInfo =
      frontend.semantics.findClassType(*groupedNative);
  const lang::ClassTypeInfo *expandedInfo =
      frontend.semantics.findClassType(*expandedNative);
  bool matchingAbi = groupedInfo != nullptr && expandedInfo != nullptr &&
                     groupedInfo->cAbiLayout && expandedInfo->cAbiLayout &&
                     groupedInfo->cAbiLayout->sizeBytes ==
                         expandedInfo->cAbiLayout->sizeBytes &&
                     groupedInfo->cAbiLayout->abiAlignmentBytes ==
                         expandedInfo->cAbiLayout->abiAlignmentBytes &&
                     groupedInfo->cAbiLayout->fields.size() ==
                         expandedInfo->cAbiLayout->fields.size();
  if (matchingAbi) {
    for (std::size_t index = 0; index < groupedInfo->cAbiLayout->fields.size();
         ++index) {
      const lang::CAbiRecordFieldLayout &groupedField =
          groupedInfo->cAbiLayout->fields[index];
      const lang::CAbiRecordFieldLayout &expandedField =
          expandedInfo->cAbiLayout->fields[index];
      matchingAbi =
          matchingAbi && groupedField.type == expandedField.type &&
          groupedField.offsetBytes == expandedField.offsetBytes &&
          groupedField.sizeBytes == expandedField.sizeBytes &&
          groupedField.abiAlignmentBytes == expandedField.abiAlignmentBytes;
    }
  }
  expect(matchingAbi,
         "grouped and explicitly mutable C ABI records should have identical "
         "layout facts");

  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir, target);
  std::size_t counterFields = 0;
  bool groupLeakedIntoLowering = false;
  for (const lang::LoweredDeclaration &declaration : lowered.declarations()) {
    groupLeakedIntoLowering =
        groupLeakedIntoLowering ||
        declaration.kind == lang::LoweredDeclarationKind::Other;
    const auto *storage =
        std::get_if<lang::LoweredStorageDeclaration>(&declaration.payload);
    if (storage != nullptr && counterInfo != nullptr &&
        storage->ownerClass == counterInfo->id) {
      ++counterFields;
      if (declaration.name == "first" || declaration.name == "second") {
        expect(storage->mutability == lang::Mutability::Mutable,
               "lowered grouped fields should retain ordinary mutable field "
               "records");
      }
    }
  }
  expect(counterFields == 3 && !groupLeakedIntoLowering,
         "LoweredProgram should contain only flattened ordinary fields and no "
         "mutable-group declaration");

  const std::string generated =
      gti_test::emitCppText(frontend, lang::CppStandard::Cpp23, target);
  expect(generated.find("std::int32_t first") != std::string::npos &&
             generated.find("std::int32_t second") != std::string::npos &&
             generated.find("mutable_field_group") == std::string::npos,
         "the C++ backend should consume flattened field declarations only");

  const std::string formatted = lang::Formatter().format(
      "struct Sample{private:mut{int first=0;int second=1;}int id=2;};");
  const std::string expectedFormatting = R"(struct Sample {
private:
  mut {
    int first = 0;
    int second = 1;
  }
  int id = 2;
};
)";
  expect(formatted == expectedFormatting &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should preserve and idempotently indent mutable field "
         "groups without adding a semicolon");

  const std::array<std::pair<std::string, std::string>, 12> invalidSyntax{{
      {"interface Bad { mut { int value = 0; } };",
       "Interfaces cannot contain mutable field groups"},
      {"union Bad { mut { int value = 0; } };",
       "Unions cannot contain mutable field groups"},
      {"struct Bad { mut {} };",
       "A mutable field group must contain at least one field"},
      {"struct Bad { mut { mut int value = 0; } int recovered = 1; };",
       "already mutable"},
      {"struct Bad { mut { static int value = 0; } int recovered = 1; };",
       "only direct instance fields"},
      {"struct Bad { mut { constexpr int value = 0; } "
       "int recovered = 1; };",
       "cannot contain constexpr fields"},
      {"struct Bad { mut { void run() {} } int recovered = 1; };",
       "Methods and constructors cannot be declared"},
      {"struct Bad { mut { Bad() {} } int recovered = 1; };",
       "only direct instance field declarations"},
      {"struct Bad { mut { ~Bad() {} } int recovered = 1; };",
       "only direct instance field declarations"},
      {"struct Bad { mut { operator int() {} } int recovered = 1; };",
       "only direct instance field declarations"},
      {"struct Bad { mut { private: int value = 0; } "
       "int recovered = 1; };",
       "only direct instance field declarations"},
      {"struct Bad { mut { mut { int value = 0; } } "
       "int recovered = 1; };",
       "already mutable"},
  }};
  for (const auto &[invalidSource, expected] : invalidSyntax) {
    const lang::FrontendResult invalid = lang::Frontend().analyze(
        "invalid-mutable-field-group.gti", invalidSource);
    expect(!invalid.canGenerateCode() &&
               hasDiagnostic(invalid.diagnostics, expected),
           "invalid mutable field-group contents should produce a focused "
           "parser diagnostic");
  }

  const lang::FrontendResult storedReference =
      lang::Frontend().analyze("mutable-field-group-reference.gti", R"(
class Borrowed {
  mut {
    int& value;
  }

public:
  Borrowed(mut int& input) : value(input) {}
};
)");
  expect(storedReference.canGenerateCode() &&
             lang::verifyMirProgram(storedReference.mir).valid(),
         "a mutable field group should form the same exact mutable stored "
         "dependency as an individually mutable reference field");
}

void testConstructorsAndReceiverMutability() {
  lang::Lexer lexer;
  const std::string validSource = R"(
class Counter {
  mut int value;
  int step = 1;

public:
  Counter() : value(0) {}
  Counter(int initial) : value(initial) {}
  Counter(bool reset) : value(0) {}
  int read() { return this.value; }
  int advance(int amount) mut {
    this.value += amount;
    return this.value;
  }
};

struct Origin {
  int x = 0;
  Origin(int initial) : x(initial) {}
};

class LifecycleValue {
  int value = 0;

public:
  LifecycleValue(LifecycleValue& other) = default;
  LifecycleValue(LifecycleValue&& other) = default;
};

class MoveOnlyPolicy {
  int value = 0;

public:
  MoveOnlyPolicy(MoveOnlyPolicy& other) = delete;
  MoveOnlyPolicy(MoveOnlyPolicy&& other) = default;
};

int inspect(Counter counter) { return counter.read(); }
int main() {
  Counter zero = Counter();
  Counter fixed = Counter(1);
  mut int observed = fixed.read();
  mut Counter moving = Counter(observed);
  observed = moving.advance(2);
  Origin origin = Origin();
  Origin shifted = Origin(4);
  Counter reset = Counter(true);
  LifecycleValue source{};
  LifecycleValue copied = LifecycleValue(source);
  mut LifecycleValue movable{};
  LifecycleValue moved = LifecycleValue(std::move(movable));
  mut MoveOnlyPolicy owner{};
  MoveOnlyPolicy transferred = MoveOnlyPolicy(std::move(owner));
  return observed + origin.x;
}
)";
  auto validTokens = lexer.scan(validSource);
  expect(!lexer.hadError(), "constructor and receiver syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "constructor and receiver syntax should parse");

  const lang::FrontendResult validFrontend =
      lang::Frontend().analyze("constructors-and-receivers.gti", validSource,
                               {standardLibraryPrelude()});
  expect(validFrontend.semanticValid,
         "explicit construction and mutable receiver calls should validate");

  const lang::ClassDecl *counter =
      findTopLevelClass(validFrontend.program, "Counter");
  const lang::ClassDecl *origin =
      findTopLevelClass(validFrontend.program, "Origin");
  const lang::ClassDecl *lifecycleValue =
      findTopLevelClass(validFrontend.program, "LifecycleValue");
  const lang::ClassDecl *moveOnlyPolicy =
      findTopLevelClass(validFrontend.program, "MoveOnlyPolicy");
  const lang::FunctionDecl *main =
      findTopLevelFunction(validFrontend.program, "main");
  const auto *zero = main == nullptr
                         ? nullptr
                         : dynamic_cast<const lang::VariableDecl *>(
                               main->body()->statements().at(0).get());
  const auto *fixed = main == nullptr
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                main->body()->statements().at(1).get());
  const auto *defaultOrigin = main == nullptr
                                  ? nullptr
                                  : dynamic_cast<const lang::VariableDecl *>(
                                        main->body()->statements().at(5).get());
  const auto *zeroCall =
      zero == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(zero->initializer().get());
  const auto *fixedCall =
      fixed == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(fixed->initializer().get());
  const auto *originCall = defaultOrigin == nullptr
                               ? nullptr
                               : dynamic_cast<const lang::Call *>(
                                     defaultOrigin->initializer().get());
  const auto constructorCallAt = [&](std::size_t index) {
    const auto *declaration =
        main == nullptr ? nullptr
                        : dynamic_cast<const lang::VariableDecl *>(
                              main->body()->statements().at(index).get());
    return declaration == nullptr ? static_cast<const lang::Call *>(nullptr)
                                  : dynamic_cast<const lang::Call *>(
                                        declaration->initializer().get());
  };
  const lang::Call *copyCall = constructorCallAt(9);
  const lang::Call *moveCall = constructorCallAt(11);
  const lang::Call *moveOnlyCall = constructorCallAt(13);
  const lang::ClassLifecycleInfo *counterLifecycle =
      counter == nullptr ? nullptr
                         : validFrontend.semantics.findClassLifecycle(*counter);
  const lang::ClassLifecycleInfo *originLifecycle =
      origin == nullptr ? nullptr
                        : validFrontend.semantics.findClassLifecycle(*origin);
  const lang::ClassLifecycleInfo *valueLifecycle =
      lifecycleValue == nullptr
          ? nullptr
          : validFrontend.semantics.findClassLifecycle(*lifecycleValue);
  const lang::ClassLifecycleInfo *moveOnlyLifecycle =
      moveOnlyPolicy == nullptr
          ? nullptr
          : validFrontend.semantics.findClassLifecycle(*moveOnlyPolicy);
  const lang::ResolvedConstructionInfo *zeroConstruction =
      zeroCall == nullptr ? nullptr
                          : validFrontend.semantics.findConstruction(*zeroCall);
  const lang::ResolvedConstructionInfo *fixedConstruction =
      fixedCall == nullptr
          ? nullptr
          : validFrontend.semantics.findConstruction(*fixedCall);
  const lang::ResolvedConstructionInfo *originConstruction =
      originCall == nullptr
          ? nullptr
          : validFrontend.semantics.findConstruction(*originCall);
  const lang::ResolvedConstructionInfo *copyConstruction =
      copyCall == nullptr ? nullptr
                          : validFrontend.semantics.findConstruction(*copyCall);
  const lang::ResolvedConstructionInfo *moveConstruction =
      moveCall == nullptr ? nullptr
                          : validFrontend.semantics.findConstruction(*moveCall);
  const lang::ResolvedConstructionInfo *moveOnlyConstruction =
      moveOnlyCall == nullptr
          ? nullptr
          : validFrontend.semantics.findConstruction(*moveOnlyCall);
  expect(counterLifecycle != nullptr && originLifecycle != nullptr &&
             counterLifecycle->constructors.size() == 3 &&
             counterLifecycle->defaultConstructor ==
                 lang::SpecialMemberStatus::Declared &&
             counterLifecycle->copyConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             counterLifecycle->moveAssignment ==
                 lang::SpecialMemberStatus::Generated &&
             originLifecycle->defaultConstructor ==
                 lang::SpecialMemberStatus::Generated,
         "class lifecycle metadata should distinguish declared, generated, "
         "and available special members");
  expect(valueLifecycle != nullptr && moveOnlyLifecycle != nullptr &&
             valueLifecycle->declaredCopyConstructor &&
             valueLifecycle->declaredMoveConstructor &&
             valueLifecycle->declaredCopyConstructor->kind ==
                 lang::ConstructorKind::Copy &&
             valueLifecycle->declaredMoveConstructor->kind ==
                 lang::ConstructorKind::Move &&
             valueLifecycle->copyConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             valueLifecycle->moveConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             moveOnlyLifecycle->copyConstructor ==
                 lang::SpecialMemberStatus::Deleted &&
             moveOnlyLifecycle->moveConstructor ==
                 lang::SpecialMemberStatus::Generated,
         "declared copy and move policies should override lifecycle "
         "availability without entering ordinary overload sets");
  expect(zeroConstruction != nullptr && fixedConstruction != nullptr &&
             originConstruction != nullptr &&
             zeroConstruction->declaration != fixedConstruction->declaration &&
             !zeroConstruction->generatedDefault &&
             originConstruction->generatedDefault,
         "construction metadata should retain exact overload selection and "
         "generated default construction");
  expect(copyConstruction != nullptr && moveConstruction != nullptr &&
             moveOnlyConstruction != nullptr &&
             copyConstruction->kind == lang::ConstructorKind::Copy &&
             moveConstruction->kind == lang::ConstructorKind::Move &&
             moveOnlyConstruction->kind == lang::ConstructorKind::Move &&
             copyConstruction->declaration != nullptr &&
             moveConstruction->declaration != nullptr,
         "same-type construction should resolve explicitly to copy or move "
         "lifecycle operations");

  const lang::FrontendResult lifecycleFrontend =
      lang::Frontend().analyze("copy-move-policy.gti", R"(
class Value {
  int value = 0;
public:
  Value(Value& other) = default;
  Value(Value&& other) = default;
};
class GenericValue<T> {
  T value;
public:
  GenericValue(T initial) : value(initial) {}
  GenericValue(GenericValue<T>& other) = default;
  GenericValue(GenericValue<T>&& other) = default;
};
int main() {
  Value source{};
  Value copied = Value(source);
  mut Value movable{};
  Value moved = Value(std::move(movable));
  GenericValue<int> generic_source = GenericValue<int>(1);
  GenericValue<int> generic_copy = GenericValue<int>(generic_source);
  mut GenericValue<int> generic_movable = GenericValue<int>(2);
  GenericValue<int> generic_move =
      GenericValue<int>(std::move(generic_movable));
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(lifecycleFrontend.canGenerateCode(),
         "copy and move policy calls should pass the complete frontend");
  std::size_t hirCopies = 0;
  std::size_t hirMoves = 0;
  for (const lang::HirFunctionInstance &function :
       lifecycleFrontend.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      hirCopies += value.constructorKind == lang::ConstructorKind::Copy;
      hirMoves += value.constructorKind == lang::ConstructorKind::Move;
    }
  }
  expect(hirCopies == 2 && hirMoves == 2,
         "typed HIR should preserve special construction kinds without "
         "creating source constructor bodies");

  expect(lifecycleFrontend.mir.valid(),
         "copy and move construction should lower to valid MIR");
  std::size_t mirCopies = 0;
  std::size_t mirMoves = 0;
  for (const lang::MirFunctionInstance &function :
       lifecycleFrontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Construct) {
          continue;
        }
        mirCopies += instruction.constructorKind == lang::ConstructorKind::Copy;
        mirMoves += instruction.constructorKind == lang::ConstructorKind::Move;
      }
    }
  }
  expect(mirCopies == 2 && mirMoves == 2,
         "MIR construct instructions should retain copy and move identity");

  const lang::BackendArtifact generatedArtifact =
      gti_test::emitCpp(validFrontend, validFrontend.mir, validFrontend.mir,
                        lang::OptimizationPipeline().run(
                            validFrontend.hir, lang::OptimizationLevel::O0));
  const std::string &generated = generatedArtifact.contents;
  const std::string &lifecycleGenerated = generatedArtifact.contents;
  expect(generated.find("explicit Counter(const std::int32_t initial);") !=
                 std::string::npos &&
             generated.find("Counter::Counter(std::int32_t __gti_mir_arg_0) : "
                            "value(__gti_mir_arg_0)") != std::string::npos,
         "constructor overloads should lower explicitly with field "
         "initialization");
  expect(generated.find("std::int32_t __gti_fn_") != std::string::npos &&
             generated.find("_read() const") != std::string::npos,
         "methods should lower as read-only by default");
  expect(generated.find("_advance(const std::int32_t amount) const") ==
                 std::string::npos &&
             generated.find("_advance(const std::int32_t amount)") !=
                 std::string::npos,
         "mutable receiver methods should lower without C++ const");
  const std::string originSlotPrefix =
      "mir_lifetime_slot<::__gti_program::Origin> ";
  const std::size_t originSlotAt = generated.find(originSlotPrefix);
  const std::size_t originSlotNameAt =
      originSlotAt == std::string::npos
          ? std::string::npos
          : originSlotAt + originSlotPrefix.size();
  const std::size_t originSlotNameEnd =
      originSlotNameAt == std::string::npos
          ? std::string::npos
          : generated.find(';', originSlotNameAt);
  const std::string originSlot =
      originSlotNameEnd == std::string::npos
          ? std::string{}
          : generated.substr(originSlotNameAt,
                             originSlotNameEnd - originSlotNameAt);
  expect(!originSlot.empty() &&
             generated.find(originSlot + ".construct();", originSlotNameEnd) !=
                 std::string::npos,
         "generated default construction should remain explicit at the call "
         "site");
  expect(
      lifecycleGenerated.find("Origin();") != std::string::npos &&
          lifecycleGenerated.find("Origin::Origin()") != std::string::npos &&
          lifecycleGenerated.find("Counter(const Counter &other) = default;") !=
              std::string::npos &&
          lifecycleGenerated.find("Counter(Counter &&) = default;") !=
              std::string::npos &&
          lifecycleGenerated.find(
              "Counter &operator=(const Counter &other) = default;") !=
              std::string::npos &&
          lifecycleGenerated.find(
              "Counter &operator=(Counter &&) = default;") !=
              std::string::npos &&
          lifecycleGenerated.find("~Counter() noexcept = default;") !=
              std::string::npos &&
          lifecycleGenerated.find(
              "LifecycleValue(const LifecycleValue &other) = default;") !=
              std::string::npos &&
          lifecycleGenerated.find(
              "LifecycleValue(LifecycleValue &&) = default;") !=
              std::string::npos &&
          lifecycleGenerated.find(
              "MoveOnlyPolicy(const MoveOnlyPolicy &other) = delete;") !=
              std::string::npos,
      "the backend should explicitly emit compiler-generated special "
      "members");

  const std::string invalidSource = R"(
class MissingInitialization {
  int value;
};

class InvalidConstructor {
  int first;
  int second;

public:
  InvalidConstructor(int value)
      : second(value), first(this.second), second(value) { return; }
  InvalidConstructor(mut int value) : first(value), second(value) {}
};

class ReservedCopy {
  int value = 0;

public:
  ReservedCopy(ReservedCopy& other) : value(other.value) {}
};

class CleanupCopy {
  int value = 0;

public:
  CleanupCopy(CleanupCopy& other) = default;
  ~CleanupCopy() {}
};

class DeletedCopy {
  int value = 0;

public:
  DeletedCopy(DeletedCopy& other) = delete;
  DeletedCopy(DeletedCopy&& other) = default;
};

class DeletedMove {
  int value = 0;

public:
  DeletedMove(DeletedMove& other) = default;
  DeletedMove(DeletedMove&& other) = delete;
};

class InvalidDefaultTarget {
  int value = 0;

public:
  InvalidDefaultTarget(int value) = default;
};

class PrivatePolicy {
  PrivatePolicy(PrivatePolicy& other) = delete;
};

void invalid_rvalue_reference(DeletedCopy&& value) {}
void consume_deleted(DeletedCopy value) {}

class PrivateValue {
  int value;
  PrivateValue(int initial) : value(initial) {}
};

class MutableValue {
  mut int value = 0;

public:
  void mutate() { this.value = 1; }
  void mutate_other(MutableValue other) mut { other.value = 1; }
  void bump() mut { this.value += 1; }
};

class ImmutableField {
  int value = 0;

public:
  void replace() mut { this.value = 1; }
};

int main() {
  PrivateValue hidden = PrivateValue(1);
  MutableValue fixed = MutableValue();
  fixed.bump();
  mut MutableValue moving = MutableValue();
  moving.bump();
  mut MutableValue uninitialized;
  InvalidConstructor mismatch = InvalidConstructor(true);
  MutableValue implicit_value = 1;
  DeletedCopy original{};
  DeletedCopy copied = DeletedCopy(original);
  DeletedCopy copied_directly = original;
  consume_deleted(original);
  mut DeletedMove stationary{};
  DeletedMove moved = DeletedMove(std::move(stationary));
  return 0;
}
)";
  auto invalidTokens = lexer.scan(invalidSource);
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid constructor semantics should remain valid syntax");

  const lang::FrontendResult invalidFrontend =
      lang::Frontend().analyze("invalid-constructors-and-receivers.gti",
                               invalidSource, {standardLibraryPrelude()});
  const std::vector<lang::Diagnostic> &invalidSemantic =
      invalidFrontend.diagnostics;
  expect(!invalidFrontend.semanticValid,
         "invalid construction and receiver use should be rejected");
  expect(hasDiagnostic(invalidSemantic, "fields must have an initializer"),
         "a class without a constructor should still initialize every field");
  expect(hasDiagnostic(invalidSemantic,
                       "Duplicate constructor overload signature"),
         "constructor overload signatures should be unique by exact parameter "
         "type");
  expect(hasDiagnostic(invalidSemantic, "field declaration order") &&
             hasDiagnostic(invalidSemantic, "initialized more than once"),
         "constructor initializer order and uniqueness should be enforced");
  expect(hasDiagnostic(invalidSemantic,
                       "Cannot use 'this' in a constructor initializer"),
         "constructor initializers should not observe a partial object");
  expect(hasDiagnostic(invalidSemantic,
                       "Constructors cannot contain return statements"),
         "constructor bodies should reject return statements");
  expect(hasDiagnostic(invalidSemantic,
                       "Custom copy and move constructor bodies require"),
         "custom lifecycle bodies should remain gated on place-aware moves");
  expect(hasDiagnostic(invalidSemantic,
                       "defaulted copy constructor of 'CleanupCopy' is "
                       "unavailable"),
         "a defaulted copy policy should reject structurally noncopyable "
         "state");
  expect(hasDiagnostic(invalidSemantic,
                       "Copy construction of 'DeletedCopy' is deleted"),
         "an explicitly deleted copy constructor should reject construction");
  expect(hasDiagnostic(invalidSemantic, "Cannot initialize 'copied_directly'"),
         "deleted copy policy should govern direct initialization");
  expect(hasDiagnostic(invalidSemantic, "deleted copy constructor"),
         "deleted copy policy should govern by-value calls");
  expect(hasDiagnostic(invalidSemantic, "type 'DeletedMove' is not movable"),
         "an explicitly deleted move constructor should reject std::move");
  expect(hasDiagnostic(invalidSemantic,
                       "must use the exact form 'InvalidDefaultTarget() = "
                       "default;'"),
         "ordinary constructor defaulting should require the exact "
         "zero-argument form");
  expect(
      hasDiagnostic(invalidSemantic, "constructor policies must be public") &&
          hasDiagnostic(invalidSemantic,
                        "'&&' is currently confined to a class or struct's "
                        "exact move constructor policy"),
      "the confined policy layer should reject private policies and "
      "general rvalue references");
  expect(hasDiagnostic(invalidSemantic,
                       "Constructor of 'PrivateValue' is private"),
         "constructor access should follow class access labels");
  expect(hasDiagnostic(invalidSemantic,
                       "Mutable method requires a mutable receiver") &&
             hasDiagnostic(invalidSemantic,
                           "Cannot mutate through a read-only receiver"),
         "mutable methods and field writes should require mutable receivers");
  expect(hasDiagnostic(invalidSemantic, "Member is immutable"),
         "frontend field immutability should remain enforced independently of "
         "the C++ representation");
  expect(
      hasDiagnostic(invalidSemantic,
                    "No constructor of 'InvalidConstructor'") &&
          hasDiagnostic(invalidSemantic, "Cannot initialize 'implicit_value'"),
      "constructor calls should reject mismatched and implicit conversions");
  expect(hasDiagnostic(invalidSemantic, "require explicit construction"),
         "class variables should never invoke construction implicitly");

  const std::string formatted = lang::Formatter().format(
      "class Counter{mut int value;public:Counter(int initial):value(initial){}"
      "Counter(Counter& other)=default;Counter(Counter&& other)=delete;"
      "int read(){return this.value;}void reset()mut{this.value=0;}};");
  expect(formatted.find("Counter(int initial) : value(initial) {}") !=
                 std::string::npos &&
             formatted.find("Counter(Counter& other) = default;") !=
                 std::string::npos &&
             formatted.find("Counter(Counter&& other) = delete;") !=
                 std::string::npos &&
             formatted.find("void reset() mut {") != std::string::npos,
         "formatter should distinguish constructor policies from logical "
         "operators and switch labels");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted constructors and receiver qualifiers should be idempotent");

  lang::Parser globalQualifierParser(lexer.scan("void invalid() mut {}"));
  globalQualifierParser.parse();
  expect(globalQualifierParser.hadError() &&
             globalQualifierParser.errors().front().message.find(
                 "Only class and struct methods") != std::string::npos,
         "free functions should reject receiver mutability qualifiers");
}

void testInheritanceAndInterfaces() {
  const std::string source = R"(
interface Renderable {
  int render(int frame);
};

interface Named {
  int name_id();
};

class Entity {
public:
  Entity(int id) : id(id) {}
  virtual int tick(int frame) { return frame + this.id; }
  virtual int tick(float frame) { return 10; }

private:
  int id;
};

class Sprite : public Entity, public Renderable, public Named {
public:
  Sprite(int id) : Entity(id) {}
  int tick(int frame) override { return frame + 2; }
  int inherited_tick() { return tick(1.5); }
  int render(int frame) override { return this.tick(frame); }
  int name_id() override { return 7; }
};

interface Reader<T> {
  T read();
};

class Box<T> : public Reader<T> {
  T value;

public:
  Box(T initial) : value(initial) {}
  T read() override { return this.value; }
};

class ClassContract {
public:
  virtual int class_only() = 0;
};

class ClassImplementation : public ClassContract {
public:
  int class_only() override { return 1; }
};

int invoke(Renderable& renderable) {
  return renderable.render(3);
}

int invoke_direct(Sprite& sprite) {
  return sprite.render(3);
}

int main() {
  Sprite sprite{4};
  Box<int> box{3};
  Reader<int>& reader = box;
  int inherited_overload = sprite.tick(1.5);
  return invoke(sprite) + invoke_direct(sprite) + inherited_overload +
         sprite.inherited_tick() + reader.read() - 33;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("inheritance.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected inheritance diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "public state, interface, and generic inheritance should pass the "
         "frontend");

  const auto *renderable = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(0).get());
  const auto *entity = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(2).get());
  const auto *sprite = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(3).get());
  const auto *box = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(5).get());
  const lang::ClassTypeInfo *renderableType =
      renderable == nullptr ? nullptr
                            : frontend.semantics.findClassType(*renderable);
  const lang::ClassTypeInfo *spriteType =
      sprite == nullptr ? nullptr : frontend.semantics.findClassType(*sprite);
  const lang::ClassLifecycleInfo *entityLifecycle =
      entity == nullptr ? nullptr
                        : frontend.semantics.findClassLifecycle(*entity);
  const lang::ClassLifecycleInfo *spriteLifecycle =
      sprite == nullptr ? nullptr
                        : frontend.semantics.findClassLifecycle(*sprite);
  expect(renderableType != nullptr && renderableType->abstract &&
             renderableType->polymorphic &&
             renderableType->kind == lang::ClassKind::Interface,
         "interfaces should be abstract polymorphic semantic types");
  expect(spriteType != nullptr && spriteType->bases.size() == 3 &&
             !spriteType->abstract && spriteType->polymorphic &&
             entityLifecycle != nullptr && entityLifecycle->polymorphic &&
             spriteLifecycle != nullptr && spriteLifecycle->polymorphic,
         "concrete derived classes should retain base and lifecycle metadata");

  const auto findMethod = [](const lang::ClassDecl *owner,
                             std::string_view name) {
    if (owner == nullptr) {
      return static_cast<const lang::FunctionDecl *>(nullptr);
    }
    for (const lang::StmtPtr &member : owner->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      if (function != nullptr && function->name().lexeme == name) {
        return function;
      }
    }
    return static_cast<const lang::FunctionDecl *>(nullptr);
  };
  const lang::FunctionDecl *contract = findMethod(renderable, "render");
  const lang::FunctionDecl *implementation = findMethod(sprite, "render");
  const lang::FunctionDecl *boxRead = findMethod(box, "read");
  const lang::FunctionInfo *contractInfo =
      contract == nullptr ? nullptr
                          : frontend.semantics.findFunction(*contract);
  const lang::FunctionInfo *implementationInfo =
      implementation == nullptr
          ? nullptr
          : frontend.semantics.findFunction(*implementation);
  expect(contract != nullptr && !contract->isPure() &&
             contractInfo != nullptr && contractInfo->virtualMethod &&
             contractInfo->pureVirtual && implementationInfo != nullptr &&
             implementationInfo->virtualMethod &&
             implementationInfo->overrideMethod &&
             implementationInfo->virtualRoots.size() == 1 &&
             implementationInfo->virtualRoots.front() == contractInfo->id,
         "interface signatures should become pure virtual contracts without "
         "requiring a source-level '= 0;'");

  const lang::FunctionDecl *invoke =
      findTopLevelFunction(frontend.program, "invoke");
  const auto *invokeReturn =
      invoke == nullptr || invoke->body()->statements().empty()
          ? nullptr
          : dynamic_cast<const lang::ReturnStmt *>(
                invoke->body()->statements().front().get());
  const auto *virtualCall =
      invokeReturn == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(invokeReturn->value().get());
  const lang::ResolvedCallInfo *callInfo =
      virtualCall == nullptr ? nullptr
                             : frontend.semantics.findCall(*virtualCall);
  expect(callInfo != nullptr &&
             callInfo->dispatch == lang::CallDispatch::Virtual,
         "calls through interface references should carry virtual dispatch");

  const lang::FunctionDecl *invokeDirect =
      findTopLevelFunction(frontend.program, "invoke_direct");
  const auto *directReturn =
      invokeDirect == nullptr || invokeDirect->body()->statements().empty()
          ? nullptr
          : dynamic_cast<const lang::ReturnStmt *>(
                invokeDirect->body()->statements().front().get());
  const auto *directCall =
      directReturn == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(directReturn->value().get());
  const lang::ResolvedCallInfo *directCallInfo =
      directCall == nullptr ? nullptr
                            : frontend.semantics.findCall(*directCall);
  expect(directCallInfo != nullptr && implementationInfo != nullptr &&
             spriteType != nullptr &&
             directCallInfo->function == implementationInfo->id &&
             directCallInfo->dispatch == lang::CallDispatch::Virtual &&
             directCallInfo->dispatchOwner.kind == lang::SemanticType::Class &&
             directCallInfo->dispatchOwner.classId == spriteType->id,
         "a concrete override call should retain its exact target, virtual "
         "dispatch, and concrete dispatch owner in semantics");

  const lang::HirClassInstance *spriteHir = nullptr;
  const lang::HirFunctionInstance *invokeHir = nullptr;
  const lang::HirFunctionInstance *invokeDirectHir = nullptr;
  const lang::HirFunctionInstance *boxReadHir = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source == sprite) {
      spriteHir = &instance;
    }
  }
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == invoke) {
      invokeHir = &instance;
    }
    if (instance.source == invokeDirect) {
      invokeDirectHir = &instance;
    }
    if (instance.source == boxRead) {
      boxReadHir = &instance;
    }
  }
  const bool hirHasVirtualCall =
      invokeHir != nullptr &&
      std::any_of(invokeHir->body.values.begin(), invokeHir->body.values.end(),
                  [](const lang::HirValue &value) {
                    return value.kind == lang::HirValueKind::Call &&
                           value.dispatch == lang::CallDispatch::Virtual &&
                           value.dispatchOwner.kind ==
                               lang::SemanticType::Class;
                  });
  const lang::HirValue *directHirCall = nullptr;
  if (invokeDirectHir != nullptr) {
    const auto found = std::find_if(
        invokeDirectHir->body.values.begin(),
        invokeDirectHir->body.values.end(), [&](const lang::HirValue &value) {
          return value.kind == lang::HirValueKind::Call &&
                 value.source == directCall;
        });
    if (found != invokeDirectHir->body.values.end()) {
      directHirCall = &*found;
    }
  }
  const lang::HirFunctionInstance *directHirTarget =
      directHirCall != nullptr && directHirCall->functionTarget
          ? frontend.hir.findFunctionInstance(*directHirCall->functionTarget)
          : nullptr;
  expect(spriteHir != nullptr && spriteHir->bases.size() == 3 &&
             spriteHir->polymorphic && hirHasVirtualCall &&
             boxReadHir != nullptr && boxReadHir->owner &&
             boxReadHir->virtualMethod && boxReadHir->overrideMethod,
         "HIR should retain base instances and virtual call dispatch");
  expect(directHirCall != nullptr && directHirTarget != nullptr &&
             spriteHir != nullptr &&
             directHirCall->dispatch == lang::CallDispatch::Virtual &&
             directHirCall->dispatchOwner == spriteHir->type &&
             directHirTarget->source == implementation &&
             directHirTarget->virtualRoots.size() == 1 &&
             contractInfo != nullptr &&
             directHirTarget->virtualRoots.front() == contractInfo->id,
         "HIR should preserve the concrete override target and its exact "
         "virtual contract root");
  const lang::MirClassInstance *spriteMir =
      spriteHir == nullptr ? nullptr
                           : frontend.mir.findClassInstance(spriteHir->id);
  expect(spriteMir != nullptr && spriteMir->bases.size() == 3 &&
             spriteMir->polymorphic,
         "MIR should preserve inheritance metadata for future backends");
  const lang::MirFunctionInstance *invokeDirectMir =
      invokeDirectHir == nullptr
          ? nullptr
          : frontend.mir.findFunctionInstance(invokeDirectHir->id);
  const lang::MirInstruction *directMirCall = nullptr;
  if (invokeDirectMir != nullptr && directHirTarget != nullptr) {
    for (const lang::MirBlock &block : invokeDirectMir->body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.functionTarget == directHirTarget->id;
          });
      if (found != block.instructions.end()) {
        directMirCall = &*found;
        break;
      }
    }
  }
  const lang::MirFunctionInstance *directMirTarget =
      directMirCall != nullptr && directMirCall->functionTarget
          ? frontend.mir.findFunctionInstance(*directMirCall->functionTarget)
          : nullptr;
  expect(directMirCall != nullptr && directMirTarget != nullptr &&
             spriteMir != nullptr &&
             directMirCall->dispatch == lang::CallDispatch::Virtual &&
             directMirCall->dispatchOwner == spriteMir->type &&
             directMirCall->definedFailure.propagation ==
                 lang::FailurePropagationKind::VirtualCall &&
             directMirTarget->declaration == implementationInfo->id &&
             directMirTarget->virtualRoots.size() == 1 &&
             contractInfo != nullptr &&
             directMirTarget->virtualRoots.front() == contractInfo->id,
         "MIR should preserve the exact concrete target, virtual owner, and "
         "contract identity without devirtualizing the call");

  const lang::BackendArtifact generatedArtifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir,
                        lang::OptimizationPipeline().run(
                            frontend.hir, lang::OptimizationLevel::O0));
  const std::string &generated = generatedArtifact.contents;
  constexpr std::string_view nameIdDefinition =
      "std::int32_t Sprite::name_id() const {";
  const std::size_t firstNameIdDefinition = generated.find(nameIdDefinition);
  expect(
      generated.find(
          "class Sprite : public ::__gti_program::Entity, public "
          "::__gti_program::Renderable, public ::__gti_program::Named") !=
              std::string::npos &&
          generated.find("virtual std::int32_t render(") != std::string::npos &&
          generated.find("render(const std::int32_t frame) const override") !=
              std::string::npos &&
          generated.find("virtual std::int32_t class_only() const = 0;") !=
              std::string::npos &&
          generated.find("class_only() const override") != std::string::npos &&
          generated.find(" = 0;") != std::string::npos &&
          generated.find("virtual ~Renderable() noexcept = default;") !=
              std::string::npos &&
          generated.find("explicit Sprite(const std::int32_t id);") !=
              std::string::npos &&
          generated.find("Sprite::Sprite(std::int32_t __gti_mir_arg_0) : "
                         "::__gti_program::Entity(__gti_mir_arg_0)") !=
              std::string::npos &&
          generated.find("virtual bool read__gti_mir_failure(") !=
              std::string::npos &&
          generated.find("read__gti_mir_failure(std::int32_t "
                         "*__gti_mir_out_result") != std::string::npos &&
          generated.find("_Box::read(std::add_pointer_t<std::int32_t> "
                         "__gti_mir_out_result) const") != std::string::npos &&
          firstNameIdDefinition != std::string::npos &&
          generated.find(nameIdDefinition,
                         firstNameIdDefinition + nameIdDefinition.size()) ==
              std::string::npos &&
          generated.find("static_cast<const ::__gti_program::Entity &>") !=
              std::string::npos,
      "the C++ backend should emit inheritance, pure contracts, overrides, "
      "base construction, polymorphic destruction, and GTI-selected virtual "
      "overloads");

  const std::string formatted = lang::Formatter().format(
      "interface Renderable{int render(int frame);};"
      "class Sprite:public Entity,public Renderable{public:"
      "Sprite(int id):Entity(id){}int render(int frame)override{return frame;}"
      "};");
  if (formatted.find("interface Renderable {") == std::string::npos ||
      formatted.find("class Sprite : public Entity, public Renderable {") ==
          std::string::npos ||
      formatted.find("int render(int frame) override {") == std::string::npos ||
      lang::Formatter().format(formatted) != formatted) {
    std::cerr << "Inheritance formatted output was:\n" << formatted;
  }
  expect(
      formatted.find("interface Renderable {") != std::string::npos &&
          formatted.find("int render(int frame);") != std::string::npos &&
          formatted.find("int render(int frame) = 0;") == std::string::npos &&
          formatted.find("class Sprite : public Entity, public Renderable {") !=
              std::string::npos &&
          formatted.find("int render(int frame) override {") !=
              std::string::npos &&
          lang::Formatter().format(formatted) == formatted,
      "inheritance and override syntax should format idempotently");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-inheritance.gti", R"(
interface Broken {
public:
  Broken() {}
  int state = 0;
  int body() { return 1; }
};

interface LegacyPureSyntax {
  int old_style() = 0;
};

class Left {};
class Right {};
class TooMany : public Left, public Right {};
class HiddenBase : Left {};

interface RootContract { int root(); };
interface LeftContract : public RootContract {};
interface RightContract : public RootContract {};
interface DiamondContract : public LeftContract, public RightContract {};
interface InvalidInterfaceBase : public Left { int invalid(); };

class CycleLeft : public CycleRight {};
class CycleRight : public CycleLeft {};

class Abstract {
public:
  virtual int work(int value) = 0;
};

class Missing : public Abstract {};
class Accidental : public Abstract {
public:
  int work(int value) { return value; }
};
class ReturnMismatch : public Abstract {
public:
  bool work(int value) override { return true; }
};

class RequiresArgument {
  int stored;

public:
  RequiresArgument(int value) : stored(value) {}
};
class MissingBaseInitializer : public RequiresArgument {
public:
  MissingBaseInitializer() {}
};

class Plain {
public:
  int read() { return 1; }
};
class HidesPlain : public Plain {
public:
  int read() override { return 2; }
};

class PrivateState {
  int secret = 1;
};
class LeaksPrivateState : public PrivateState {
public:
  int leak() { return secret; }
};

class Concrete : public Abstract {
public:
  int work(int value) override { return value; }
};
int consume(Abstract& value) { return value.work(1); }
int main() {
  Missing missing{};
  Concrete concrete{};
  Abstract sliced = concrete;
  return consume(concrete);
}
)");
  expect(!invalid.canGenerateCode(),
         "invalid inheritance and interface programs should be rejected");
  expect(hasDiagnostic(invalid.diagnostics, "Interfaces are behavior-only") &&
             hasDiagnostic(invalid.diagnostics,
                           "Interfaces cannot declare constructors") &&
             hasDiagnostic(invalid.diagnostics,
                           "Interface methods are pure contracts") &&
             hasDiagnostic(invalid.diagnostics,
                           "Interface methods are implicitly pure"),
         "interfaces should permit only declaration-only public behavior "
         "contracts and reject redundant '= 0;' syntax");
  expect(hasDiagnostic(invalid.diagnostics, "only one state-bearing base") &&
             hasDiagnostic(invalid.diagnostics, "Inheritance must be public"),
         "inheritance should be public with at most one state-bearing base");
  expect(hasDiagnostic(invalid.diagnostics,
                       "An interface can inherit only from other interfaces") &&
             hasDiagnostic(invalid.diagnostics,
                           "inheritance diamonds are not supported") &&
             hasDiagnostic(invalid.diagnostics, "Inheritance cycle"),
         "interface bases, inheritance diamonds, and cycles should be "
         "diagnosed before lowering");
  expect(hasDiagnostic(invalid.diagnostics, "Overriding methods must use") &&
             hasDiagnostic(invalid.diagnostics,
                           "Override return type must exactly match") &&
             hasDiagnostic(invalid.diagnostics,
                           "same signature as a non-virtual base method"),
         "override checking should be exact, explicit, and virtual-only");
  expect(
      hasDiagnostic(invalid.diagnostics, "must explicitly initialize base") &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot construct abstract class 'Missing'"),
      "derived construction should initialize bases and reject abstract "
      "objects");
  expect(hasDiagnostic(invalid.diagnostics,
                       "Member 'secret' of 'PrivateState' is private") &&
             hasDiagnostic(invalid.diagnostics, "Cannot initialize 'sliced'"),
         "private state access and object slicing should stay rejected while "
         "public-derived lvalues bind to base reference parameters");
}

void testInheritedGenericTargetInstances() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("inherited-generic-targets.gti", R"(
class Base<T> {
public:
  T echo(T value) { return value; }
};

class Derived<First, Second> : public Base<Second> {
public:
  Second indirect(Second value) { return echo(value); }
};

class SizedBase<T, uint64_t N> {
public:
  uint64_t extent() { return N; }
};

class SizedDerived<T, uint64_t N> : public SizedBase<T, N> {};

int main() {
  Derived<int32_t, uint64_t> value{};
  SizedDerived<int32_t, 3> sized{};
  uint64_t direct = value.echo(uint64_t(7));
  uint64_t indirect = value.indirect(uint64_t(8));
  uint64_t extent = sized.extent();
  return int32_t(direct + indirect + extent) - 18;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected inherited-generic diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mir.valid(),
         "inherited methods should retain their resolved generic base "
         "instance through HIR and MIR");

  const lang::ClassDecl *base = findTopLevelClass(frontend.program, "Base");
  const lang::FunctionDecl *echo = nullptr;
  if (base != nullptr) {
    for (const lang::StmtPtr &member : base->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      if (function != nullptr && function->name().lexeme == "echo") {
        echo = function;
        break;
      }
    }
  }

  std::vector<const lang::HirClassInstance *> baseInstances;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source == base) {
      baseInstances.push_back(&instance);
    }
  }
  const lang::HirClassInstance *concreteBase =
      baseInstances.size() == 1 ? baseInstances.front() : nullptr;
  expect(concreteBase != nullptr && concreteBase->typeArguments.size() == 1 &&
             concreteBase->typeArguments.front() == lang::SemanticType::UInt64,
         "inherited calls should enqueue only the resolved Base<uint64_t> "
         "owner, not a symbolic or derived-argument-shaped base");

  const lang::ClassDecl *sizedBase =
      findTopLevelClass(frontend.program, "SizedBase");
  std::vector<const lang::HirClassInstance *> sizedBaseInstances;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source == sizedBase) {
      sizedBaseInstances.push_back(&instance);
    }
  }
  const lang::HirClassInstance *concreteSizedBase =
      sizedBaseInstances.size() == 1 ? sizedBaseInstances.front() : nullptr;
  expect(concreteSizedBase != nullptr &&
             concreteSizedBase->typeArguments.size() == 1 &&
             concreteSizedBase->typeArguments.front() ==
                 lang::SemanticType::Int32 &&
             concreteSizedBase->valueArguments.size() == 1 &&
             concreteSizedBase->valueArguments.front().kind ==
                 lang::CompileTimeValue::UInt64 &&
             concreteSizedBase->valueArguments.front().value == 3,
         "inherited calls should preserve the resolved base's type and value "
         "arguments together");

  std::vector<const lang::HirFunctionInstance *> echoInstances;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == echo) {
      echoInstances.push_back(&instance);
    }
  }
  const lang::HirFunctionInstance *concreteEcho =
      echoInstances.size() == 1 ? echoInstances.front() : nullptr;
  expect(concreteEcho != nullptr && concreteEcho->owner.has_value() &&
             concreteBase != nullptr &&
             *concreteEcho->owner == concreteBase->id &&
             concreteEcho->returnType == lang::SemanticType::UInt64 &&
             concreteEcho->parameterTypes.size() == 1 &&
             concreteEcho->parameterTypes.front() == lang::SemanticType::UInt64,
         "the inherited method target should use the concrete base owner and "
         "signature selected by semantics");

  const auto containsUnresolvedType =
      [&](const auto &self, const lang::SemanticType &type) -> bool {
    return type.kind == lang::SemanticType::TypeParameter ||
           type.kind == lang::SemanticType::TypePack ||
           std::any_of(type.arguments.begin(), type.arguments.end(),
                       [&](const lang::SemanticType &argument) {
                         return self(self, argument);
                       });
  };
  bool concreteBody = concreteEcho != nullptr;
  if (concreteEcho != nullptr) {
    concreteBody =
        std::none_of(concreteEcho->body.bindings.begin(),
                     concreteEcho->body.bindings.end(),
                     [&](const lang::HirBinding &binding) {
                       return containsUnresolvedType(containsUnresolvedType,
                                                     binding.info.type);
                     }) &&
        std::none_of(concreteEcho->body.values.begin(),
                     concreteEcho->body.values.end(),
                     [&](const lang::HirValue &value) {
                       return containsUnresolvedType(containsUnresolvedType,
                                                     value.info.type);
                     });
  }
  expect(concreteBody,
         "a concrete inherited method must not retain symbolic class types in "
         "its executable HIR body");

  bool callsUseConcreteTarget = concreteEcho != nullptr;
  if (concreteEcho != nullptr) {
    std::size_t calls = 0;
    for (const lang::HirFunctionInstance &function :
         frontend.hir.functionInstances()) {
      for (const lang::HirValue &value : function.body.values) {
        if (value.functionTarget == concreteEcho->id) {
          ++calls;
          callsUseConcreteTarget =
              callsUseConcreteTarget &&
              value.dispatchOwner.kind == lang::SemanticType::Class &&
              value.dispatchOwner.classId == concreteBase->declaration &&
              value.dispatchOwner.arguments.size() == 1 &&
              value.dispatchOwner.arguments.front() ==
                  lang::SemanticType::UInt64;
        }
      }
    }
    callsUseConcreteTarget = callsUseConcreteTarget && calls == 2;
  }
  expect(callsUseConcreteTarget,
         "direct and unqualified inherited calls should share the same "
         "resolved concrete HIR target");

  const lang::MirFunctionInstance *mirEcho =
      concreteEcho == nullptr
          ? nullptr
          : frontend.mir.findFunctionInstance(concreteEcho->id);
  bool concreteMir = mirEcho != nullptr &&
                     mirEcho->body.returnType == lang::SemanticType::UInt64;
  if (mirEcho != nullptr) {
    concreteMir =
        concreteMir &&
        std::none_of(mirEcho->body.places.begin(), mirEcho->body.places.end(),
                     [&](const lang::MirPlace &place) {
                       return containsUnresolvedType(containsUnresolvedType,
                                                     place.type);
                     }) &&
        std::none_of(mirEcho->body.values.begin(), mirEcho->body.values.end(),
                     [&](const lang::MirValue &value) {
                       return containsUnresolvedType(containsUnresolvedType,
                                                     value.info.type);
                     });
  }
  expect(concreteMir,
         "MIR for a concrete inherited method should contain only substituted "
         "types");
}

void testDirectBraceConstruction() {
  const std::string source = R"(
class Box<T> {
  T value;

public:
  Box(T initial) : value(initial) {}
  T read() { return this.value; }
};

using IntBox = Box<int>;

class Holder {
  Box<int> box{3};

public:
  int read() { return this.box.read(); }
};

class Pair {
  int left;
  int right;

public:
  Pair(int left, int right) : left(left), right(right) {}
  int sum() { return this.left + this.right; }
};

struct Point {
  int x = 4;
};

int main() {
  Box<int> boxed{1};
  mut Box<int> changing{2};
  IntBox aliased{3,};
  Holder holder{};
  Point point{};
  Pair pair{5, 6,};
  return boxed.read() + changing.read() + aliased.read() + holder.read() +
         point.x + pair.sum();
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("direct-initialization.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected direct-initialization diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "class direct initialization should pass the shared frontend");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto *boxed = main == nullptr || main->body()->statements().empty()
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                main->body()->statements().front().get());
  const auto *initializer = boxed == nullptr
                                ? nullptr
                                : dynamic_cast<const lang::DirectInitializer *>(
                                      boxed->initializer().get());
  const lang::ResolvedConstructionInfo *construction =
      initializer == nullptr
          ? nullptr
          : frontend.semantics.findConstruction(*initializer);
  expect(initializer != nullptr && initializer->arguments().size() == 1 &&
             lang::AstPrinter().print(*initializer) == "(direct-init 1)",
         "the AST should preserve direct constructor arguments");
  expect(construction != nullptr && construction->constructor != 0 &&
             construction->constructedType.kind == lang::SemanticType::Class,
         "semantics should retain the exact selected constructor identity");

  const lang::HirFunctionInstance *mainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "main") {
      mainInstance = &instance;
      break;
    }
  }
  const lang::HirStatement *boxedStatement =
      mainInstance == nullptr || mainInstance->body.roots.empty()
          ? nullptr
          : mainInstance->body.findStatement(mainInstance->body.roots.front());
  const lang::HirValue *boxedValue =
      boxedStatement == nullptr || !boxedStatement->value
          ? nullptr
          : mainInstance->body.findValue(*boxedStatement->value);
  expect(boxedValue != nullptr &&
             boxedValue->kind == lang::HirValueKind::DirectInitializer &&
             boxedValue->operands.size() == 1 &&
             boxedValue->constructorTarget.has_value(),
         "HIR should retain direct initialization and its constructor edge");

  const std::string generated = gti_test::emitCppText(frontend);
  expect(generated.find("mir_lifetime_slot<::__gti_program::__gti_type_") !=
                 std::string::npos &&
             generated.find(".construct(__gti_mir_v_") != std::string::npos &&
             generated.find("mir_lifetime_slot<::__gti_program::Holder>") !=
                 std::string::npos &&
             generated.find(".construct();") != std::string::npos &&
             generated.find("mir_lifetime_slot<::__gti_program::Pair>") !=
                 std::string::npos &&
             generated.find("Pair::Pair(std::int32_t __gti_mir_arg_0, "
                            "std::int32_t __gti_mir_arg_1)") !=
                 std::string::npos,
         "the backend should lower braces through explicit constructor calls");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-direct-initialization.gti", R"(
class Sample {
public:
  Sample(int value) {}
};

int main() {
  int primitive{1};
  int values[1]{1};
  auto inferred{1};
  Sample copy_list = {1};
  Sample& reference{1};
  Sample mismatch{true};
  Sample missing;
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "unsupported brace forms and constructor mismatches should fail");
  expect(
      countDiagnosticCode(invalid.diagnostics, "GTI-S2038") == 3 &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2092") == 1 &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2028") == 1 &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2012") == 1 &&
          hasDiagnostic(invalid.diagnostics,
                        "limited to classes and structs") &&
          hasDiagnostic(invalid.diagnostics, "initialize a fixed array") &&
          hasDiagnostic(invalid.diagnostics,
                        "Collection initialization requires") &&
          hasDiagnostic(invalid.diagnostics, "cannot initialize a reference") &&
          hasDiagnostic(invalid.diagnostics, "require explicit construction"),
      "direct initialization should diagnose each unsupported form at the "
      "GTI layer");

  lang::Lexer lexer;
  lang::Parser recoveryParser(lexer.scan(R"(
class Sample {
public:
  Sample(int value) {}
};
int main() {
  Sample broken{1;
  Sample retained{2};
  return retained.value;
}
)",
                                         "direct-recovery.gti"));
  const lang::Program recovered = recoveryParser.parse();
  const lang::FunctionDecl *recoveredMain =
      findTopLevelFunction(recovered, "main");
  expect(recoveryParser.hadError() && recoveredMain != nullptr &&
             recoveredMain->body()->statements().size() == 2,
         "a malformed direct initializer should preserve later declarations");

  lang::Parser parenthesizedParser(lexer.scan(R"(
class Sample {
public:
  Sample(int value) {}
};
int main() {
  mut Sample value(1);
  return 0;
}
)"));
  parenthesizedParser.parse();
  expect(parenthesizedParser.hadError() &&
             hasDiagnostic(parenthesizedParser.errors(),
                           "use 'Type name{arguments};'"),
         "block-scope parenthesized declarations should direct users to the "
         "unambiguous brace form");

  const std::string formatted = lang::Formatter().format(
      "class Box<T>{public:Box(T value){}};int main(){mut Box<int> "
      "value { 1, };Box<int> empty { };int values[1]={1};return 0;}");
  if (formatted.find("mut Box<int> value{1,};") == std::string::npos ||
      formatted.find("Box<int> empty{};") == std::string::npos ||
      formatted.find("int values[1] = {1};") == std::string::npos) {
    std::cerr << "Direct-initialization formatted output was:\n" << formatted;
  }
  expect(formatted.find("mut Box<int> value{1,};") != std::string::npos &&
             formatted.find("Box<int> empty{};") != std::string::npos &&
             formatted.find("int values[1] = {1};") != std::string::npos,
         "formatter should keep direct braces compact and distinct from "
         "array initialization");
  expect(lang::Formatter().format(formatted) == formatted,
         "direct brace formatting should be idempotent");
}

void testRecursiveClassLayouts() {
  const std::string directSource = R"(
struct Cell {
  int value;
  Cell inner;

public:
  Cell(int value, Cell cell) : value(value), inner(cell) {}
};
)";
  const lang::FrontendResult direct =
      lang::Frontend().analyze("recursive-class-layout.gti", directSource);
  const lang::Diagnostic *directDiagnostic =
      findDiagnosticByCode(direct.diagnostics, "GTI-S2082");
  const std::size_t fieldType = directSource.find("Cell inner");
  expect(!direct.canGenerateCode() && directDiagnostic != nullptr &&
             directDiagnostic->primary.start == fieldType &&
             directDiagnostic->primary.end == fieldType + 4 &&
             countDiagnosticCode(direct.diagnostics, "GTI-S2082") == 1 &&
             countDiagnosticCode(direct.diagnostics, "GTI-S2003") == 0 &&
             hasRelatedDiagnostic(direct.diagnostics,
                                  "recursively contained type") &&
             hasDiagnosticHint(direct.diagnostics, "std::unique_ptr<Cell>"),
         "ordinary classes should reject direct by-value recursion at the "
         "field without cascading into constructor conversion diagnostics");

  const lang::FrontendResult generic =
      lang::Frontend().analyze("recursive-generic-layout.gti", R"(
class Node<T> {
  T value;
  Node<T> next;

public:
  Node(T value, Node<T> next) : value(value), next(next) {}
};
)");
  expect(!generic.canGenerateCode() &&
             countDiagnosticCode(generic.diagnostics, "GTI-S2082") == 1 &&
             countDiagnosticCode(generic.diagnostics, "GTI-S2003") == 0 &&
             hasDiagnosticHint(generic.diagnostics, "std::unique_ptr<Node<T>>"),
         "generic self-layout diagnostics should preserve the written owner "
         "type in their indirection hint");

  const lang::FrontendResult fixedArray =
      lang::Frontend().analyze("recursive-array-layout.gti", R"(
struct Branch {
  Branch children[2];
};
)");
  expect(!fixedArray.canGenerateCode() &&
             countDiagnosticCode(fixedArray.diagnostics, "GTI-S2082") == 1 &&
             hasDiagnostic(fixedArray.diagnostics, "through field 'children'"),
         "fixed arrays should preserve their by-value recursive layout edge");
}

void testNamedGenerics() {
  lang::Lexer lexer;
  const std::string source = R"(
class Box<T> {
  mut T value;

public:
  Box(T value) : value(value) {}
  T get() { return this.value; }
  U echo<U>(U replacement) { return replacement; }
  void set(T replacement) mut { this.value = replacement; }
};

T identity<T>(T value) { return value; }
T unbox<T>(Box<T> box) { return box.get(); }
U relay<T, U>(Box<T> box, U value) { return box.echo<U>(value); }

int main() {
  mut Box<int> box = Box<int>(identity(7));
  box.set(identity<int>(9));
  int value = unbox(box);
  int relayed = relay(box, box.echo<int>(value));
  std::string_view text = identity<std::string_view>("generic");
  return relayed;
}

)";
  auto validTokens = lexer.scan(source);
  expect(!lexer.hadError(), "named generic source should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "generic classes, functions, and applications should parse");

  const lang::FrontendResult validFrontend = lang::Frontend().analyze(
      "named-generics.gti", source, {standardLibraryPrelude()});
  if (!validFrontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : validFrontend.diagnostics) {
      std::cerr << "Unexpected generic diagnostic: "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(validFrontend.canGenerateCode(),
         "generic substitution and exact inference should validate");

  const std::string generated = gti_test::emitCppText(validFrontend);
  expect(generated.find("_Box;") != std::string::npos &&
             generated.find("_Box {") != std::string::npos &&
             generated.find("template <typename T>\n  class Box") ==
                 std::string::npos,
         "a selected nameable generic class should lower with matching "
         "GTI-owned concrete forward and full declarations");
  expect(generated.find("__gti_inst_") != std::string::npos &&
             containsMangledFunction(generated, "", "identity",
                                     "(std::int32_t __gti_mir_arg_0)") &&
             containsMangledFunction(generated, "", "identity",
                                     "__gti_mir_failure(std::string_view "
                                     "__gti_mir_arg_0") &&
             containsMangledFunction(generated, "", "unbox", ""),
         "nameable generic functions should lower as GTI-selected concrete "
         "functions");
  expect(
      generated.find("mir_lifetime_slot<::__gti_program::__gti_type_") !=
              std::string::npos &&
          generated.find("_identity(__gti_mir_v_") != std::string::npos &&
          generated.find("_Box::__gti_member_") != std::string::npos &&
          generated.find("_set(") != std::string::npos &&
          containsMangledFunction(generated, "", "identity", "(__gti_mir_v_") &&
          generated.find("_set(__gti_mir_v_") != std::string::npos,
      "applied types and explicit generic calls should lower recursively");
  expect(generated.find(".template __gti_fn_") == std::string::npos &&
             generated.find("_Box::__gti_member_") != std::string::npos &&
             generated.find("_echo(__gti_mir_v_") != std::string::npos &&
             generated.find("template <> ") == std::string::npos,
         "the backend should emit the concrete selected generic method "
         "without reopening C++ dependent-template lookup");

  auto invalidTokens = lexer.scan(R"(
class Duplicate<T, T> {};
class SameName<SameName> {};
class Shadow<T> {
public:
  T replace<T>(T value) { return value; }
};
class Box<T> {
  T value;
public:
  Box(T value) : value(value) {}
  T get() { return this.value; }
};

T identity<T>(T value) { return value; }
T choose<T>(T left, T right) { return left; }
T unsupported_add<T>(T left, T right) { return left + right; }
bool unsupported_equal<T>(T left, T right) { return left == right; }
T make<T>();
int ordinary(int value) { return value; }
int main<T>() { return 0; }

int use() {
  Box missing = Box(1);
  Box<int, bool> excessive = Box<int, bool>(1);
  Box<void> impossible = Box<void>(1);
  int mismatch = identity<int>(true);
  int conflict = choose(1, true);
  int unknown = make();
  int excessive_types = identity<int, bool>(1);
  int not_generic = ordinary<int>(1);
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid generic semantics should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid generic applications should be rejected semantically");
  expect(hasDiagnostic(invalidSemantic, "Duplicate generic type parameter"),
         "generic parameter names should be unique");
  expect(hasDiagnostic(invalidSemantic, "same name as its declaration") &&
             hasDiagnostic(invalidSemantic, "cannot shadow"),
         "generic parameters should not collide with enclosing declarations");
  expect(hasDiagnostic(invalidSemantic, "requires 1 generic type argument"),
         "generic class applications should enforce arity");
  expect(
      hasDiagnostic(invalidSemantic, "No constructor of 'Box' exactly matches"),
      "void metadata must not make an incompatible constructor viable");
  expect(hasDiagnostic(invalidSemantic, "Conflicting types inferred"),
         "repeated generic parameters should infer one exact type");
  expect(
      hasDiagnostic(invalidSemantic, "numeric operands"),
      "unconstrained type parameters should not gain operators by duck typing");
  expect(hasDiagnostic(invalidSemantic, "Equality operands"),
         "generic equality should wait for an explicit contract model");
  expect(hasDiagnostic(invalidSemantic, "Cannot infer generic type parameter"),
         "return-only generic parameters should require explicit arguments");
  expect(
      hasDiagnostic(invalidSemantic, "wrong number of type arguments") &&
          hasDiagnostic(invalidSemantic, "Non-generic functions do not take"),
      "explicit function type arguments should enforce generic arity");
  expect(hasDiagnostic(invalidSemantic, "main entry point cannot be generic"),
         "the native entry point should remain non-generic");

  const std::string formatted = lang::Formatter().format(
      "class Box<T>{T value;public:Box(T value):value(value){}T get(){return "
      "this.value;}};T identity<T>(T value){return value;}int main(){Box<"
      "Box<int>> nested=Box<Box<int>>(Box<int>(1));int value=identity<int>(1);"
      "return value;}");
  expect(formatted.find("class Box<T> {") != std::string::npos &&
             formatted.find("T identity<T>(T value) {") != std::string::npos &&
             formatted.find("Box<Box<int>> nested = Box<Box<int>>(") !=
                 std::string::npos &&
             formatted.find("identity<int>(1)") != std::string::npos,
         "formatter should preserve compact generic angle brackets");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted generic syntax should be idempotent");

  const std::string comparison =
      lang::Formatter().format("bool result=a < b > c;");
  expect(comparison == "bool result = a < b > c;\n",
         "formatter should not treat relational expressions as generic types");

  lang::Parser malformedParser(lexer.scan("class Broken<> {}; int okay = 1;"));
  const lang::Program recovered = malformedParser.parse();
  expect(malformedParser.hadError() && recovered.declarations().size() == 1,
         "parser recovery should continue after malformed generic parameters");
}

void testNativeGenericCarrierClosure() {
  const std::string source = R"(
class Leaf<T> {
  T value;

public:
  Leaf(T value) : value(value) {}
  T get() { return this.value; }
};

class DormantCarrier<T> {
  Leaf<T> value;

public:
  DormantCarrier(Leaf<T> value) : value(value) {}
};

int main() {
  Leaf<int> value = Leaf<int>(7);
  return value.get();
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("native-generic-carrier-closure.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected carrier-closure diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "a dormant dependent generic carrier should validate");

  const std::string generated = gti_test::emitCppText(frontend);
  expect(
      concreteBackendSymbols(generated, "__gti_type_", "Leaf").size() == 1 &&
          generated.find("template <typename T>\n  class Leaf;") !=
              std::string::npos &&
          generated.find("template <typename T>\n  class Leaf {") !=
              std::string::npos &&
          generated.find("template <typename T>\n  class DormantCarrier {") !=
              std::string::npos,
      "a retained native carrier should close over every dependent generic "
      "family while concrete calls keep their direct representation");
}

void testUninstantiatedGenericSignatureClosure() {
  const auto frontend =
      lang::Frontend().analyze("generic-signature-closure.gti", R"(
struct Result<T> { T value; };
class Dormant<T> {
  Result<char> field = {.value = 'x'};
public:
  Dormant(Result<int8_t> input) {}
  Result<int> result();
  void accept(Result<bool> input);
  Result<T> dependent(Result<T> input) { return input; }
};
int main() { return 0; }
)");
  expect(frontend.canGenerateCode(),
         "unused generic signatures should reach verified MIR");
  if (!frontend.canGenerateCode())
    return;
  std::size_t concrete = 0;
  bool dependentOwner = false;
  for (const auto &instance : frontend.hir.classInstances()) {
    if (instance.source == nullptr)
      continue;
    if (instance.source->name().lexeme == "Result") {
      ++concrete;
      expect(instance.typeArguments.size() == 1 &&
                 instance.typeArguments.front().kind !=
                     lang::SemanticType::TypeParameter,
             "only concrete signature components should be instantiated");
    }
    dependentOwner |= instance.source->name().lexeme == "Dormant";
  }
  expect(concrete == 4 && !dependentOwner,
         "return, parameter, constructor and field types must close without a "
         "dependent owner");
  expect(
      frontend.hir.functionInstances().size() == 1,
      "signature discovery must not instantiate an unused generic method body");
  const auto generated = gti_test::emitCppText(frontend);
  expect(concreteBackendSymbols(generated, "__gti_type_", "Result").size() == 4,
         "each concrete declaration signature type must have a representation "
         "identity");
}

void testDependentCarrierRepresentationClosure() {
  const auto frontend = analyzeTrustedPreludeFixture("carrier-closure.gti", R"(
struct Leaf<T> { T value; Leaf(T value) : value(std::move(value)) {} };
struct Wrapper<T> { Leaf<T> leaf; Wrapper(T value) : leaf(Leaf<T>(std::move(value))) {} };
struct Independent<T> { T value; };
class Owner<T> {
  using value_type = T;
public:
  Owner() {}
  Wrapper<T> result(T value) { return Wrapper<T>(std::move(value)); }
  Independent<int> concrete();
};
int main() { Owner<int> owner{}; auto value = owner.result(42); return value.leaf.value - 42; }
)");
  expect(frontend.canGenerateCode(), "dependent carrier closure must verify");
  if (!frontend.canGenerateCode())
    return;
  const auto generated = gti_test::emitCppText(frontend);
  expect(
      concreteBackendSymbols(generated, "__gti_type_", "Owner").empty() &&
          concreteBackendSymbols(generated, "__gti_type_", "Wrapper").empty() &&
          concreteBackendSymbols(generated, "__gti_type_", "Leaf").empty(),
      "dependent owner result and transitive field must share native "
      "template representation with concrete MIR signatures");
  expect(
      concreteBackendSymbols(generated, "__gti_type_", "Independent").size() ==
          1,
      "fully concrete signature references must retain direct representation");
}

void testExactQualifiedClassSpecializations() {
  const std::string source = R"(
class demo::Value<int> {
public:
  int read() { return 2; }
};

namespace demo {
class Value<T> {
public:
  int read() { return 1; }
};

class Count<uint64_t N> {
public:
  int read() { return 1; }
};
}

int read_exact(demo::Value<int>& value) { return value.read(); }

class demo::Count<7> {
public:
  int read() { return 7; }
};

int read_through_generic<T>(demo::Value<T>& value) {
  return value.read();
}

int main() {
  demo::Value<int> exact{};
  demo::Value<bool> primary{};
  demo::Count<7> exact_value{};
  demo::Count<8> primary_value{};
  return read_exact(exact) + read_through_generic<int>(exact) +
         primary.read() + exact_value.read() + primary_value.read() - 13;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("exact-qualified-specialization.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected exact-specialization diagnostic: "
                << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "a qualified exact class specialization should replace its primary "
         "for the matching canonical application");

  const lang::ClassTypeInfo *specialization = nullptr;
  const lang::ClassTypeInfo *primary = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    const lang::ClassTypeInfo *info =
        frontend.semantics.findClassType(instance.declaration);
    if (info == nullptr || info->qualifiedName != "demo::Value") {
      continue;
    }
    if (info->exactSpecializationPrimary == 0) {
      primary = info;
    } else {
      specialization = info;
    }
  }
  expect(primary != nullptr && specialization != nullptr &&
             specialization->exactSpecializationPrimary == primary->id &&
             specialization->exactTypeArguments ==
                 std::vector<lang::SemanticType>{lang::SemanticType::Int32},
         "HIR discovery should retain separate primary and exact semantic "
         "class identities");

  if (frontend.canGenerateCode()) {
    const std::string generated = gti_test::emitCppText(frontend);
    const std::size_t firstValue = generated.find("_Value {");
    const std::size_t secondValue =
        firstValue == std::string::npos
            ? std::string::npos
            : generated.find("_Value {", firstValue + 1);
    const std::size_t firstCount = generated.find("_Count {");
    const std::size_t secondCount =
        firstCount == std::string::npos
            ? std::string::npos
            : generated.find("_Count {", firstCount + 1);
    expect(generated.find("_Value;") != std::string::npos &&
               firstValue != std::string::npos &&
               secondValue != std::string::npos &&
               firstCount != std::string::npos &&
               secondCount != std::string::npos &&
               generated.find("Value<std::") == std::string::npos &&
               generated.find("Count<") == std::string::npos,
           "the C++ representation should emit ordinary concrete classes for "
           "both primary applications and selected exact specializations");
  }

  const std::string formatted = lang::Formatter().format(
      "namespace demo{class Value<T>{};}class demo::Value<int>{};");
  expect(formatted.find("class demo::Value<int> {") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should preserve qualified exact-specialization "
         "syntax idempotently");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-exact-qualified-specialization.gti", R"(
namespace demo {
class Value<T> {};
class Nested<T> {};
}
using signed_int = int32_t;
class demo::Value<int> {};
class demo::Value<signed_int> {};
class demo::Nested<UnknownType> {};
class demo::Value<int>;
struct demo::Value<bool> {};
namespace misplaced {
  class demo::Nested<bool> {};
}
)");
  expect(!invalid.canGenerateCode() &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2078") &&
             hasDiagnostic(invalid.diagnostics,
                           "Duplicate exact specialization") &&
             hasRelatedDiagnostic(invalid.diagnostics,
                                  "Previous exact specialization") &&
             hasDiagnostic(invalid.diagnostics,
                           "must provide a complete definition") &&
             hasDiagnostic(invalid.diagnostics,
                           "must use the primary declaration's 'class'") &&
             hasDiagnostic(invalid.diagnostics,
                           "must be declared at package scope"),
         "exact specialization diagnostics should reject canonical "
         "duplicates, unknown arguments, forward declarations, and "
         "nested-namespace "
         "declarations");

  const lang::FrontendResult wrongArity =
      lang::Frontend().analyze("wrong-arity-exact-specialization.gti", R"(
namespace demo { class Value<T> {}; }
class demo::Value<int, bool> {};
)");
  expect(!wrongArity.canGenerateCode() &&
             hasDiagnosticCode(wrongArity.diagnostics, "GTI-S2078") &&
             hasDiagnostic(wrongArity.diagnostics,
                           "requires 1 generic type argument"),
         "a malformed exact key should use the exact-specialization "
         "diagnostic family");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "gti-exact-specialization";
  const lang::FrontendResult standardHash = lang::Frontend().analyze(
      root / "hash.gti", R"(
#include <std/functional>
class Widget {
  int value = 42;
public:
  int read() { return this.value; }
};
class std::hash<Widget> {
public:
  size_t operator()(Widget& value) { return size_t(value.read()); }
};
int main() {
  Widget value{};
  std::hash<Widget> hash{};
  return int(hash(value)) - 42;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(standardHash.canGenerateCode(),
         "an application should be able to specialize std::hash for a "
         "nominal type owned by that application");

  const lang::FrontendResult orphanHash = lang::Frontend().analyze(
      root / "orphan-hash.gti", R"(
#include <std/functional>
class std::hash<int> {
public:
  size_t operator()(int& value) { return size_t(value); }
};
int main() { return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !orphanHash.canGenerateCode() &&
          hasDiagnosticCode(orphanHash.diagnostics, "GTI-S2078") &&
          hasDiagnostic(orphanHash.diagnostics,
                        "owns neither the primary nor a top-level nominal") &&
          hasDiagnosticHint(orphanHash.diagnostics, "primary's package"),
      "the coherence rule should reject a std::hash specialization for a "
      "builtin type owned by neither side");

  const lang::FrontendResult nestedOrphanHash = lang::Frontend().analyze(
      root / "nested-orphan-hash.gti", R"(
#include <std/functional>
#include <std/vector>
class Widget {};
class std::hash<std::vector<Widget>> {
public:
  size_t operator()(std::vector<Widget>& value) { return 0; }
};
int main() { return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!nestedOrphanHash.canGenerateCode() &&
             hasDiagnosticCode(nestedOrphanHash.diagnostics, "GTI-S2078") &&
             hasDiagnostic(nestedOrphanHash.diagnostics,
                           "owns neither the primary nor a top-level nominal"),
         "a nominal nested inside a foreign generic argument should not grant "
         "specialization ownership");

  lang::Lexer lexer;
  lang::Parser malformedParser(
      lexer.scan("class demo::Value {}; int recovered() { return 0; }"));
  const lang::Program recovered = malformedParser.parse();
  expect(malformedParser.hadError() &&
             findTopLevelFunction(recovered, "recovered") != nullptr,
         "a qualified class declaration without an exact argument list "
         "should diagnose and recover to later declarations");
}

void testContextualNumericLiterals() {
  const auto valid =
      lang::Frontend().analyze("contextual-numeric-literals.gti", R"(
concept number<T> = std::numeric<T>;
bool zero<number T>(T value) { return value == (0); }
bool positive<std::numeric T>(T value) { return (+0) < value; }
T half<std::numeric T>(T value) { return T(value / 2); }
bool negative<std::signed_numeric T>(T value) { return value == (-1); }
bool floating<std::floating_point T>(T value) { return (-2) <= value; }
int main() {
  bool a = zero(uint64_t(0));
  bool b = zero(0.0);
  bool c = zero(0.0d);
  bool d = positive(uint8_t(1));
  bool e = positive(1.0d);
  bool f = negative(int8_t(-1));
  bool g = negative(-1.0);
  bool h = floating(-1.0d);
  uint8_t small = half(uint8_t(4));
  float decimal = half(4.0);
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(valid.canGenerateCode(),
         "integer literals should work with composed numeric, signed, and "
         "floating constraints, on either side and with grouping/signs");
  bool sawIntegerContext = false;
  bool sawFloatingConversionSource = false;
  for (const auto &instance : valid.hir.functionInstances()) {
    if (instance.source == nullptr || instance.source->name().lexeme != "zero")
      continue;
    for (const auto &value : instance.body.values) {
      if (!value.literal)
        continue;
      const auto *magnitude = std::get_if<std::uint64_t>(&*value.literal);
      if (magnitude == nullptr || *magnitude != 0)
        continue;
      sawIntegerContext |= value.info.type == lang::SemanticType::UInt64;
      sawFloatingConversionSource |=
          value.info.type == lang::SemanticType::Int32;
    }
  }
  expect(sawIntegerContext && sawFloatingConversionSource,
         "concrete HIR should preserve integer contextual typing and the "
         "ordinary integer source of a floating comparison");
  if (valid.canGenerateCode())
    expect(!gti_test::emitCppText(valid).empty(),
           "numeric literal instances must pass verified MIR emission");

  const std::string outOfRangeSource = R"(
bool invalid<std::numeric T>(T value) { return value == (-1); }
int main() { if (invalid(uint8_t(0))) { return 1; } return 0; }
)";
  const auto outOfRange =
      lang::Frontend().analyze("numeric-literal-range.gti", outOfRangeSource,
                               {standardLibraryPrelude()});
  expect(!outOfRange.canGenerateCode() &&
             countDiagnosticCode(outOfRange.diagnostics, "GTI-S2004") == 1,
         "numeric instances must reject negative literals for unsigned "
         "operands before backend entry");
  for (const auto &diagnostic : outOfRange.diagnostics) {
    if (diagnostic.code != "GTI-S2004")
      continue;
    expect(diagnostic.primary.start == outOfRangeSource.find("-1") + 1 &&
               diagnostic.primary.end == outOfRangeSource.find("-1") + 2 &&
               !diagnostic.related.empty() && !diagnostic.hints.empty() &&
               diagnostic.fixes.empty(),
           "range rejection should retain digit span, instance context, "
           "hint, and no unsafe automatic conversion");
  }

  const std::string rejectedSources[] = {
      "bool mixed<std::numeric T>(T value, int other) { return value == other; "
      "}",
      "bool unconstrained<T>(T value) { return value == 0; }",
      "void exact(uint8_t value) {} int main() { exact(1); return 0; }",
      "T narrow<std::numeric T>(T value) { return value / 2; } "
      "int main() { uint8_t result = narrow(uint8_t(4)); return 0; }",
      "T invalid<std::numeric T>(T value) { return T(value % 2); }",
      "bool invalid<std::numeric T>(T value) { return value == 256; } "
      "int main() { if (invalid(uint8_t(0))) { return 1; } return 0; }",
  };
  for (const auto &source : rejectedSources) {
    const auto invalid = lang::Frontend().analyze(
        "numeric-literal-boundaries.gti", source, {standardLibraryPrelude()});
    expect(!invalid.canGenerateCode() &&
               std::any_of(invalid.diagnostics.begin(),
                           invalid.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.phase ==
                                    lang::DiagnosticPhase::Semantics;
                           }),
           "literal context must independently preserve each semantic "
           "boundary: " +
               source);
  }
}

void testConstrainedGenerics() {
  const std::string source = R"(
T minimum<std::ordered T>(T left, T right) {
  if (left < right) { return left; }
  return right;
}

T multiply<std::numeric T>(T left, T right) {
  return T(left * right);
}

T remainder<std::integral T>(T left, T right) {
  return T(left % right);
}

bool is_zero<std::integral T>(T value) {
  return value == (0) and (0) == value;
}

T increment<std::integral T>(T value) {
  return T(value + 1);
}

T low_bits<std::integral T>(T value) {
  return T(value & 3);
}

T negate<std::signed_numeric T>(T value) { return T(-value); }
T signed_integer<std::signed_integral T>(T value) { return value; }
T unsigned_integer<std::unsigned_integral T>(T value) { return value; }
T floating_value<std::floating_point T>(T value) { return value; }

T square_integral<std::integral T>(T value) {
  return multiply(value, value);
}

void consume_integrals<std::integral Values...>(Values... values) {}

class NumericBox<std::numeric T> {
  T value;

public:
  NumericBox(T value) : value(value) {}
  T doubled() { return T(this.value + this.value); }
};

int main() {
  int low = minimum(2, 5);
  int product = multiply(3, 4);
  int rest = remainder(7, 4);
  bool zero = is_zero(uint64_t(0));
  uint64_t incremented = increment(uint64_t(41));
  uint8_t bits = low_bits(uint8_t(7));
  int negative = negate(2);
  int signed_value = signed_integer(2);
  uint64_t unsigned_value = unsigned_integer(uint64_t(2));
  float decimal = floating_value(2.5);
  int square = square_integral(5);
  consume_integrals(1, uint64_t(2));
  NumericBox<int> box = NumericBox<int>(6);
  if (low == 2 and product == 12 and rest == 3 and zero and
      incremented == 42 and bits == 3 and negative == -2 and
      signed_value == 2 and unsigned_value == uint64_t(2) and
      decimal == 2.5 and square == 25 and box.doubled() == 12) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult valid = lang::Frontend().analyze(
      "constrained-generics.gti", source, {standardLibraryPrelude()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected constrained generic diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "standard numeric constraints should validate generic operations, "
         "propagation, packs, and classes");

  bool sawContextualUInt64Literal = false;
  bool sawContextualUInt8Literal = false;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source == nullptr) {
      continue;
    }
    for (const lang::HirValue &value : instance.body.values) {
      if (!value.literal) {
        continue;
      }
      const auto *magnitude = std::get_if<std::uint64_t>(&*value.literal);
      if (magnitude == nullptr) {
        continue;
      }
      sawContextualUInt64Literal =
          sawContextualUInt64Literal ||
          (instance.source->name().lexeme == "is_zero" && *magnitude == 0 &&
           value.info.type == lang::SemanticType::UInt64);
      sawContextualUInt8Literal =
          sawContextualUInt8Literal ||
          (instance.source->name().lexeme == "low_bits" && *magnitude == 3 &&
           value.info.type == lang::SemanticType::UInt8);
    }
  }
  expect(sawContextualUInt64Literal && sawContextualUInt8Literal,
         "concrete generic reanalysis should retain the contextual integer "
         "literal type in HIR");

  const lang::FunctionDecl *minimum =
      findTopLevelFunction(valid.program, "minimum");
  expect(minimum != nullptr && minimum->genericParameters().size() == 1 &&
             minimum->genericParameters().front().constraint &&
             minimum->genericParameters().front().constraint->segments.size() ==
                 2 &&
             minimum->genericParameters()
                     .front()
                     .constraint->segments.back()
                     .lexeme == "ordered",
         "the AST should retain the qualified generic constraint path");

  const std::string generated = gti_test::emitCppText(valid);
  expect(
      generated.find("template <typename T>\n  T __gti_fn_") ==
              std::string::npos &&
          containsMangledFunction(generated, "", "minimum",
                                  "(std::int32_t __gti_mir_arg_0, std::int32_t "
                                  "__gti_mir_arg_1)") &&
          generated.find("_minimum<std::int32_t>(") == std::string::npos &&
          generated.find("mir_checked_multiply_v1<std::int32_t>") !=
              std::string::npos &&
          generated.find("requires __gti_std::") == std::string::npos,
      "constraints should remain frontend metadata while generic numeric "
      "conversions lower through checked backend casts");
  expect(generated.find("static_cast<std::uint64_t>(0)") != std::string::npos &&
             generated.find("static_cast<std::uint8_t>(3)") !=
                 std::string::npos,
         "the C++ backend should explicitly preserve contextual generic "
         "integer literal types");

  lang::FrontendResult capabilities =
      lang::Frontend().analyze("exact-generic-capabilities.gti", R"(
class Rank {
  int value;

public:
  Rank() : value(0) {}
  Rank(int value) : value(value) {}
  int get() { return this.value; }

  bool operator==(Rank& other) { return this.value == other.value; }
  bool operator!=(Rank& other) { return this.value != other.value; }
  bool operator<(Rank& other) { return this.value < other.value; }
  bool operator<=(Rank& other) { return this.value <= other.value; }
  bool operator>(Rank& other) { return this.value > other.value; }
  bool operator>=(Rank& other) { return this.value >= other.value; }
};

class MoveOnly {
  int value;

public:
  MoveOnly() : value(0) {}
  MoveOnly(int value) : value(value) {}
  MoveOnly(MoveOnly& other) = delete;
  MoveOnly(MoveOnly&& other) = default;
};

bool equivalent<std::equality_comparable T>(T left, T right) {
  return left == right;
}

T earlier<std::totally_ordered T>(T left, T right) {
  if (left < right) { return left; }
  return right;
}

T legacy_earlier<std::ordered T>(T left, T right) {
  return earlier(left, right);
}

T copy_value<std::copyable T>(T value) { return value; }
T move_value<std::movable T>(T value) { return std::move(value); }
T default_value<std::default_initializable T>() { return T(); }
bool ordered_equal<std::totally_ordered T>(T left, T right) {
  return equivalent(left, right);
}
T numeric_copy<std::numeric T>(T value) { return copy_value(value); }
T copy_move<std::copyable T>(T value) {
  return move_value(std::move(value));
}

int main() {
  Rank first = Rank(1);
  Rank second = Rank(2);
  bool same = equivalent(first, first);
  Rank lower = earlier(first, second);
  Rank legacy = legacy_earlier(first, second);
  Rank copied = copy_value(first);
  Rank moved_copy = copy_move(first);
  Rank zero = default_value<Rank>();
  int copied_number = numeric_copy(4);
  MoveOnly source = MoveOnly(3);
  MoveOnly moved = move_value(std::move(source));
  if (same and ordered_equal(first, first) and lower.get() == 1 and
      legacy.get() == 1 and copied.get() == 1 and moved_copy.get() == 1 and
      zero.get() == 0 and copied_number == 4) {
    return 0;
  }
  return 1;
}
)",
                               {standardLibraryPrelude()});
  if (!capabilities.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : capabilities.diagnostics) {
      std::cerr << "Unexpected exact capability diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(capabilities.canGenerateCode(),
         "exact lifecycle and comparison capabilities should accept "
         "matching primitives and nominal classes");

  lang::HirConstructorInstanceId defaultRankConstructor = 0;
  for (const lang::HirConstructorInstance &instance :
       capabilities.hir.constructorInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "Rank" &&
        instance.parameterTypes.empty()) {
      defaultRankConstructor = instance.id;
      break;
    }
  }
  bool sawExactDefaultConstructionHir = false;
  bool sawGenericDefaultIntrinsicHir = false;
  for (const lang::HirFunctionInstance &instance :
       capabilities.hir.functionInstances()) {
    for (const lang::HirValue &value : instance.body.values) {
      sawGenericDefaultIntrinsicHir =
          sawGenericDefaultIntrinsicHir ||
          value.intrinsic ==
              lang::IntrinsicKind::DefaultTypeParameterConstruction;
      sawExactDefaultConstructionHir =
          sawExactDefaultConstructionHir ||
          (defaultRankConstructor != 0 &&
           value.constructorTarget == defaultRankConstructor &&
           value.intrinsic == lang::IntrinsicKind::None);
    }
  }
  bool sawExactDefaultConstructionMir = false;
  bool sawGenericDefaultIntrinsicMir = false;
  for (const lang::MirFunctionInstance &instance :
       capabilities.mir.functionInstances()) {
    for (const lang::MirBlock &block : instance.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        sawGenericDefaultIntrinsicMir =
            sawGenericDefaultIntrinsicMir ||
            instruction.intrinsic ==
                lang::IntrinsicKind::DefaultTypeParameterConstruction;
        sawExactDefaultConstructionMir =
            sawExactDefaultConstructionMir ||
            (instruction.kind == lang::MirInstructionKind::Construct &&
             defaultRankConstructor != 0 &&
             instruction.constructorTarget == defaultRankConstructor &&
             instruction.intrinsic == lang::IntrinsicKind::None);
      }
    }
  }
  const lang::MirConstructorInstance *defaultRankMir =
      capabilities.mir.findConstructorInstance(defaultRankConstructor);
  expect(defaultRankConstructor != 0 && sawExactDefaultConstructionHir &&
             sawExactDefaultConstructionMir && !sawGenericDefaultIntrinsicHir &&
             !sawGenericDefaultIntrinsicMir && defaultRankMir != nullptr &&
             !defaultRankMir->mayRaiseDefinedFailure,
         "concrete generic default construction should replace the generic "
         "intrinsic with one exact, failure-free constructor target through "
         "HIR and MIR");

  const std::string capabilityCpp = gti_test::emitCppText(capabilities);
  expect(capabilityCpp.find("operator<(") != std::string::npos &&
             capabilityCpp.find("operator>=(") != std::string::npos &&
             capabilityCpp.find(
                 "construct_representation_at(__gti_mir_out_result);") !=
                 std::string::npos &&
             capabilityCpp.find("constructor-instance " +
                                std::to_string(defaultRankConstructor)) !=
                 std::string::npos &&
             capabilityCpp.find("concept ") == std::string::npos,
         "the C++ backend should emit selected relational methods and generic "
         "default construction without C++ concepts");

  lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-constrained-generics.gti", R"(
T unsupported_add<T>(T left, T right) { return left + right; }
T ordered_value<std::ordered T>(T value) { return value; }
T needs_numeric<std::numeric T>(T value) { return value; }
T integral_value<std::integral T>(T value) { return value; }
T signed_number<std::signed_numeric T>(T value) { return value; }
T forwards_ordered<std::ordered T>(T value) { return needs_numeric(value); }
T bad_negate<std::numeric T>(T value) { return -value; }
T bad_modulo<std::numeric T>(T left, T right) { return left % right; }
T bad_conversion<T>(T value) { return T(1); }
T unknown<std::mystery T>(T value) { return value; }
T signed_integer<std::signed_integral T>(T value) { return value; }
T unsigned_integer<std::unsigned_integral T>(T value) { return value; }
T floating_value<std::floating_point T>(T value) { return value; }

class EqualOnly {
public:
  bool operator==(EqualOnly& other) { return true; }
};

class AlmostOrdered {
public:
  bool operator==(AlmostOrdered& other) { return true; }
  bool operator!=(AlmostOrdered& other) { return false; }
  bool operator<(AlmostOrdered& other) { return false; }
  bool operator<=(AlmostOrdered& other) { return true; }
  bool operator>(AlmostOrdered& other) { return false; }
};

class DeletedCopy {
public:
  DeletedCopy() {}
  DeletedCopy(DeletedCopy& other) = delete;
  DeletedCopy(DeletedCopy&& other) = default;
};

class DeletedMove {
public:
  DeletedMove() {}
  DeletedMove(DeletedMove&& other) = delete;
};

class CopyConstructOnly {
  int& value;
public:
  CopyConstructOnly(int& source) : value(source) {}
};

class NoDefault {
  int stored;

public:
  NoDefault(int value) : stored(value) {}
};

bool needs_equality<std::equality_comparable T>(T left, T right) {
  return left == right;
}
T needs_total_order<std::totally_ordered T>(T value) { return value; }
T needs_copy_construction<std::copy_constructible T>(T value) {
  return value;
}
T needs_copy<std::copyable T>(T value) { return value; }
T needs_move<std::movable T>(T value) { return std::move(value); }
T needs_default<std::default_initializable T>() { return T(); }
T unconstrained_default<T>() { return T(); }

class NumericBox<std::numeric T> {
  T value;
public:
  NumericBox(T value) : value(value) {}
};

void use_box(NumericBox<bool> value) {}

int use() {
  bool invalid_ordered = ordered_value(true);
  bool invalid_value = needs_numeric(true);
  float invalid_integral = integral_value(2.0);
  uint64_t invalid_signed_numeric = signed_number(uint64_t(1));
  uint64_t invalid_signed_integral = signed_integer(uint64_t(1));
  int invalid_unsigned_integral = unsigned_integer(1);
  int invalid_floating_point = floating_value(1);
  bool invalid_equality = needs_equality(EqualOnly(), EqualOnly());
  AlmostOrdered invalid_total = needs_total_order(AlmostOrdered());
  DeletedCopy invalid_construction = needs_copy_construction(DeletedCopy());
  mut int copy_source = 1;
  CopyConstructOnly copy_only = CopyConstructOnly(copy_source);
  CopyConstructOnly invalid_copy = needs_copy(std::move(copy_only));
  DeletedMove invalid_move = needs_move(DeletedMove());
  NoDefault invalid_default = needs_default<NoDefault>();
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(!invalid.canGenerateCode(),
         "unsupported operations and concrete constraint violations should "
         "fail before backend entry");
  expect(hasDiagnosticCode(invalid.diagnostics, "GTI-S2029") &&
             hasDiagnostic(invalid.diagnostics,
                           "does not satisfy generic constraint") &&
             hasDiagnostic(invalid.diagnostics,
                           "Unknown generic concept 'std::mystery'") &&
             hasDiagnostic(invalid.diagnostics,
                           "requires the numeric capability") &&
             hasDiagnostic(invalid.diagnostics, "std::ordered") &&
             hasDiagnostic(invalid.diagnostics, "std::integral") &&
             hasDiagnostic(invalid.diagnostics, "std::signed_numeric") &&
             hasDiagnostic(invalid.diagnostics, "std::signed_integral") &&
             hasDiagnostic(invalid.diagnostics, "std::unsigned_integral") &&
             hasDiagnostic(invalid.diagnostics, "std::floating_point") &&
             hasDiagnostic(invalid.diagnostics, "std::equality_comparable") &&
             hasDiagnostic(invalid.diagnostics, "std::totally_ordered") &&
             hasDiagnostic(invalid.diagnostics, "std::copy_constructible") &&
             hasDiagnostic(invalid.diagnostics, "std::copyable") &&
             hasDiagnostic(invalid.diagnostics, "std::movable") &&
             hasDiagnostic(invalid.diagnostics, "std::default_initializable") &&
             hasDiagnostic(invalid.diagnostics,
                           "requires the default-initialization capability") &&
             hasDiagnostic(invalid.diagnostics, "signed numeric value") &&
             hasDiagnostic(invalid.diagnostics, "requires integer operands") &&
             hasDiagnostic(invalid.diagnostics, "numeric operands"),
         "constraint diagnostics should cover declarations, propagation, "
         "calls, conversions, and required operator capabilities");
  expect(hasDiagnosticHint(invalid.diagnostics,
                           "public, read-only bool operator==") &&
             hasDiagnosticHint(invalid.diagnostics,
                               "exact public, read-only bool overloads") &&
             hasDiagnosticHint(invalid.diagnostics,
                               "available copy constructor") &&
             hasDiagnosticHint(invalid.diagnostics,
                               "available copy construction and assignment") &&
             hasDiagnosticHint(invalid.diagnostics,
                               "available move construction and assignment") &&
             hasDiagnosticHint(invalid.diagnostics,
                               "public zero-argument constructor"),
         "exact capability failures should explain the required public "
         "contract");

  lang::FrontendResult outOfRangeContextualLiteral = lang::Frontend().analyze(
      "out-of-range-contextual-integer-literal.gti", R"(
bool out_of_range<std::integral T>(T value) {
  return value == 256;
}

bool negative_out_of_range<std::integral T>(T value) {
  return value == -1;
}

int main() {
  bool result = out_of_range(uint8_t(1));
  bool negative = negative_out_of_range(uint8_t(1));
  if (result) { return 1; }
  if (negative) { return 2; }
  return 0;
}
)",
      {standardLibraryPrelude()});
  expect(!outOfRangeContextualLiteral.canGenerateCode() &&
             countDiagnosticCode(outOfRangeContextualLiteral.diagnostics,
                                 "GTI-S2004") == 2 &&
             hasDiagnostic(outOfRangeContextualLiteral.diagnostics,
                           "does not fit the contextual operand type "
                           "'uint8_t'") &&
             hasDiagnostic(outOfRangeContextualLiteral.diagnostics,
                           "Integer literal '-1'"),
         "concrete generic reanalysis should diagnose a contextual integer "
         "literal that does not fit the instantiated operand type");

  lang::FrontendResult mixedGenericOperands =
      lang::Frontend().analyze("mixed-generic-operands.gti", R"(
bool mixed_value<std::integral T>(T value, int other) {
  return value == other;
}
)",
                               {standardLibraryPrelude()});
  expect(!mixedGenericOperands.canGenerateCode() &&
             hasDiagnostic(mixedGenericOperands.diagnostics,
                           "Equality operands have incompatible types"),
         "non-literal mixed generic operands should remain exact-match "
         "errors");

  lang::FrontendResult duplicate =
      lang::Frontend().analyze("duplicate-constrained-overload.gti", R"(
T select<std::numeric T>(T value) { return value; }
T select<std::integral T>(T value) { return value; }
)",
                               {standardLibraryPrelude()});
  expect(!duplicate.canGenerateCode() &&
             hasDiagnostic(duplicate.diagnostics,
                           "Duplicate overload signature for 'select'"),
         "constraints should not distinguish or rank overload signatures");

  const std::string formatted = lang::Formatter().format(
      "T minimum<std::totally_ordered T>(T left,T right){if(left<right){return "
      "left;}return right;}");
  expect(
      formatted.find("T minimum<std::totally_ordered T>(T left, T right) {") !=
              std::string::npos &&
          lang::Formatter().format(formatted) == formatted,
      "constrained generic declarations should format idempotently");

  lang::Lexer lexer;
  lang::Parser malformed(lexer.scan(
      "T broken<std::numeric>(T value) { return value; } int okay = 1;"));
  const lang::Program recovered = malformed.parse();
  expect(malformed.hadError() && recovered.declarations().size() == 1,
         "parser recovery should continue after a missing constrained "
         "parameter name");
}

void testMoveConstructionCapability() {
  const auto valid =
      lang::Frontend().analyze("move-construction-capability.gti", R"(
class Cursor {
  int& source;
public:
  Cursor(int& value) : source(value) {}
};
bool accepts<T>(T& value) requires std::move_constructible<T> { return true; }
bool from_movable<std::movable T>(T& value) { return accepts(value); }
bool from_relocation<std::relocatable T>(T& value) { return accepts(value); }
bool from_either<T>(T& value)
  requires std::movable<T> || std::relocatable<T> { return accepts(value); }
int main() {
  int source = 7;
  Cursor cursor{source};
  if (accepts(cursor) && from_movable(source) && from_relocation(source) &&
      from_either(source)) { return 0; }
  return 1;
}
)",
                               {standardLibraryPrelude()});
  if (!valid.canGenerateCode()) {
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid() &&
             !gti_test::emitCppText(valid).empty(),
         "construction-only user cursors and stronger symbolic contracts "
         "should satisfy move construction without requiring assignment");

  const std::string invalidSource = R"(
class Frozen {
public:
  Frozen() {}
  Frozen(Frozen&& other) = delete;
};
class Cursor {
  int& source;
public:
  Cursor(int& value) : source(value) {}
};
bool constructible<T>(T& value) requires std::move_constructible<T> { return true; }
bool assignable<T>(T& value) requires std::movable<T> { return true; }
bool cannot_strengthen<std::move_constructible T>(T& value) {
  return assignable(value);
}
int main() {
  Frozen frozen;
  int source = 7;
  Cursor cursor{source};
  [[discard]] constructible(frozen);
  [[discard]] assignable(cursor);
  return 0;
}
)";
  const auto invalid =
      lang::Frontend().analyze("move-construction-rejected.gti", invalidSource,
                               {standardLibraryPrelude()});
  expect(!invalid.canGenerateCode() &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2029") &&
             hasDiagnostic(invalid.diagnostics, "std::move_constructible") &&
             hasDiagnostic(invalid.diagnostics, "std::movable") &&
             hasDiagnosticHint(invalid.diagnostics,
                               "move assignment is not required") &&
             !hasDiagnosticCode(invalid.diagnostics, "GTI-B0001"),
         "deleted construction and unavailable assignment must remain "
         "distinct source-level constraint failures");
  const auto failure = std::find_if(
      invalid.diagnostics.begin(), invalid.diagnostics.end(),
      [](const auto &item) {
        return item.code == "GTI-S2029" &&
               item.message.find("type 'Frozen'") != std::string::npos;
      });
  const auto argument = invalidSource.find("constructible(frozen)") +
                        std::string("constructible(").size();
  expect(
      failure != invalid.diagnostics.end() &&
          failure->primary.start == argument &&
          failure->primary.end == argument + std::string("frozen").size() &&
          !failure->related.empty() && failure->fixes.empty(),
      "move-construction failure points at the argument, relates its contract, "
      "and offers no ownership-changing automatic fix");
}

void testFiniteWrapperTraits() {
  const auto frontend =
      lang::Frontend().analyze("finite-wrapper-traits.gti", R"(
struct Wrapper<T> { T value; };
struct Closed { Wrapper<int> nested; };
struct RawField { int* pointer = nullptr; };
struct ClosedRaw { Wrapper<RawField> nested; };
int main() {
  Wrapper<Closed> finite = {.value = {.nested = {.value = 7}}};
  Wrapper<ClosedRaw> raw = {.value = {.nested = {.value = {.pointer = nullptr}}}};
  return 0;
}
)",
                               {standardLibraryPrelude()});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "a closed declaration breaks generic growth dependence, not exact "
         "cycle detection");
  const auto *main = findTopLevelFunction(frontend.program, "main");
  const auto traits =
      [&](const char *name) -> const lang::SemanticTypeTraits * {
    const auto *local = findDirectLocal(main, name);
    const auto *binding =
        local ? frontend.semantics.findBinding(*local) : nullptr;
    return binding ? &binding->traits : nullptr;
  };
  const auto *finite = traits("finite");
  const auto *raw = traits("raw");
  expect(finite && finite->movable && finite->copyable &&
             finite->relocation == lang::RelocationKind::Bitwise &&
             finite->transferCapable && finite->shareCapable,
         "finite passive wrappers retain their structural capabilities");
  expect(raw && raw->movable && !raw->transferCapable && !raw->shareCapable,
         "finite wrapper traversal must reach a raw field rather than "
         "treating a repeated generic name as a harmless recursive edge");
}

void testConcurrencyTypeCapabilities() {
  const std::string source = R"(
#include <std/cstdio>
#include <std/memory>

struct Scalar {
  int value = 0;
};

class Box<T> {
  T value;
public:
  Box(T input) : value(input) {}
};

class Node {
  int value = 0;
  mut std::unique_ptr<Node> next = std::unique_ptr<Node>();
};

class RawBox {
  int* address = nullptr;
};

[[no_transfer, unsafe_share]] class PolicyValue {
  int value = 0;
};

[[unsafe_transfer]] class AuditedCleanup {
  int* address = nullptr;
public:
  ~AuditedCleanup() {}
};

[[transfer, share]] interface ConcurrentValue {
  int read();
};

class ConcurrentScalar : public ConcurrentValue {
  int value = 0;
public:
  int read() override { return this.value; }
};

T transfer_value<std::transferable T>(T value) {
  return std::move(value);
}

void share_value<std::shareable T>(T& value) {}
int inspect_transfer<std::transferable T>(T value) { return value(); }
T relay_transfer<std::transferable T>(T value) {
  return transfer_value(std::move(value));
}
void relay_share<std::shareable T>(T& value) { share_value(value); }

int main() {
  Scalar scalar = transfer_value(Scalar());
  Node node = relay_transfer(Node());
  std::unique_ptr<Scalar> owner = std::make_unique<Scalar>();
  std::unique_ptr<Scalar> moved_owner = transfer_value(std::move(owner));
  share_value(scalar);
  relay_share(node);
  share_value(moved_owner);
  Box<int> integer_box = Box<int>(1);
  Box<std::string_view> view_box = Box<std::string_view>("view");
  expected<Scalar, int> result = Scalar();
  int values[2] = {};
  int* addresses[2] = {nullptr, nullptr};
  auto callable = [scalar]() -> int { return 1; };
  int callable_result = inspect_transfer(callable);
  ConcurrentScalar concurrent = ConcurrentScalar();
  share_value(concurrent);
  AuditedCleanup cleanup = transfer_value(AuditedCleanup());
  return callable_result - 1;
}
)";

  lang::FrontendResult valid = lang::Frontend().analyze(
      "concurrency-type-capabilities.gti", source, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected concurrency capability diagnostic: "
                << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "structural transfer/share capabilities should accept scalars, "
         "recursive owners, reviewed cleanup, interfaces, and safe captures");

  const lang::ClassDecl *node = findTopLevelClass(valid.program, "Node");
  const lang::ClassDecl *rawBox = findTopLevelClass(valid.program, "RawBox");
  const lang::ClassDecl *policy =
      findTopLevelClass(valid.program, "PolicyValue");
  const lang::ClassDecl *cleanup =
      findTopLevelClass(valid.program, "AuditedCleanup");
  const lang::ClassDecl *concurrent =
      findTopLevelClass(valid.program, "ConcurrentScalar");
  const lang::ClassDecl *fileHandle =
      findClassRecursive(valid.program.declarations(), "file_handle");
  const lang::ClassTypeInfo *nodeInfo =
      node == nullptr ? nullptr : valid.semantics.findClassType(*node);
  const lang::ClassTypeInfo *rawInfo =
      rawBox == nullptr ? nullptr : valid.semantics.findClassType(*rawBox);
  const lang::ClassTypeInfo *policyInfo =
      policy == nullptr ? nullptr : valid.semantics.findClassType(*policy);
  const lang::ClassTypeInfo *cleanupInfo =
      cleanup == nullptr ? nullptr : valid.semantics.findClassType(*cleanup);
  const lang::ClassTypeInfo *concurrentInfo =
      concurrent == nullptr ? nullptr
                            : valid.semantics.findClassType(*concurrent);
  const lang::ClassTypeInfo *fileHandleInfo =
      fileHandle == nullptr ? nullptr
                            : valid.semantics.findClassType(*fileHandle);
  expect(nodeInfo != nullptr && nodeInfo->traits.transferCapable &&
             nodeInfo->traits.shareCapable,
         "recursive unique ownership should converge to positive structural "
         "capabilities");
  expect(rawInfo != nullptr && !rawInfo->traits.transferCapable &&
             !rawInfo->traits.shareCapable,
         "raw-pointer state should conservatively deny both capabilities");
  expect(policyInfo != nullptr && !policyInfo->traits.transferCapable &&
             policyInfo->traits.shareCapable &&
             policyInfo->transferPolicy ==
                 lang::ConcurrencyCapabilityPolicy::Denied &&
             policyInfo->sharePolicy ==
                 lang::ConcurrencyCapabilityPolicy::UnsafeAsserted,
         "safe opt-out and unsafe positive policy should remain independent "
         "semantic metadata");
  expect(cleanupInfo != nullptr && cleanupInfo->traits.transferCapable &&
             !cleanupInfo->traits.shareCapable,
         "an unsafe transfer assertion should override cleanup/raw-pointer "
         "denial without implicitly asserting sharing");
  expect(concurrentInfo != nullptr && concurrentInfo->traits.transferCapable &&
             concurrentInfo->traits.shareCapable,
         "an interface implementation should prove each required capability");
  expect(fileHandleInfo != nullptr && !fileHandleInfo->traits.transferCapable &&
             !fileHandleInfo->traits.shareCapable &&
             fileHandleInfo->transferPolicy ==
                 lang::ConcurrencyCapabilityPolicy::Denied &&
             fileHandleInfo->sharePolicy ==
                 lang::ConcurrencyCapabilityPolicy::Denied,
         "the standard native file handle should retain an explicit "
         "thread-affinity opt-out despite its integer representation");

  const lang::FunctionDecl *main = findTopLevelFunction(valid.program, "main");
  const auto bindingTraits =
      [&](const std::string &name) -> const lang::SemanticTypeTraits * {
    const lang::VariableDecl *variable = findDirectLocal(main, name);
    const lang::BindingInfo *binding =
        variable == nullptr ? nullptr : valid.semantics.findBinding(*variable);
    return binding == nullptr ? nullptr : &binding->traits;
  };
  const lang::SemanticTypeTraits *resultTraits = bindingTraits("result");
  const lang::SemanticTypeTraits *valuesTraits = bindingTraits("values");
  const lang::SemanticTypeTraits *addressesTraits = bindingTraits("addresses");
  const lang::SemanticTypeTraits *callableTraits = bindingTraits("callable");
  expect(resultTraits != nullptr && resultTraits->transferCapable &&
             resultTraits->shareCapable,
         "expected<T, E> should derive capabilities from both alternatives");
  expect(valuesTraits != nullptr && valuesTraits->transferCapable &&
             valuesTraits->shareCapable && addressesTraits != nullptr &&
             !addressesTraits->transferCapable &&
             !addressesTraits->shareCapable,
         "fixed arrays should derive capabilities from their element type");
  expect(callableTraits != nullptr && callableTraits->transferCapable &&
             callableTraits->shareCapable,
         "read-only callable values should derive capabilities from captures");

  bool sawIntegerBox = false;
  bool sawViewBox = false;
  bool sawNodeHir = false;
  for (const lang::HirClassInstance &instance : valid.hir.classInstances()) {
    if (instance.source == nullptr) {
      continue;
    }
    if (instance.source->name().lexeme == "Box" &&
        instance.typeArguments.size() == 1) {
      if (instance.typeArguments.front() == lang::SemanticType::Int32) {
        sawIntegerBox =
            instance.traits.transferCapable && instance.traits.shareCapable;
      } else if (instance.typeArguments.front() ==
                 lang::SemanticType::StringView) {
        sawViewBox =
            !instance.traits.transferCapable && !instance.traits.shareCapable;
      }
    }
    if (instance.source->name().lexeme == "Node") {
      sawNodeHir =
          instance.traits.transferCapable && instance.traits.shareCapable;
    }
  }
  expect(sawIntegerBox && sawViewBox && sawNodeHir,
         "HIR should preserve concrete generic and recursive capability "
         "facts without backend name recognition");

  const lang::LanguageQueries queries;
  const std::size_t policyName = source.find("PolicyValue");
  const std::optional<lang::HoverInfo> policyHover =
      queries.hover(valid, valid.sourceGraph.entryUnit(), policyName + 1);
  expect(policyHover &&
             std::find(policyHover->notes.begin(), policyHover->notes.end(),
                       "not transfer-capable (explicit opt-out)") !=
                 policyHover->notes.end() &&
             std::find(policyHover->notes.begin(), policyHover->notes.end(),
                       "share-capable (unsafe nominal assertion)") !=
                 policyHover->notes.end(),
         "class hover should present compiler-owned capability policy facts");

  const std::string generated = gti_test::emitCppText(valid);
  const std::size_t emittedProgram =
      generated.find("namespace __gti_program {");
  const std::string_view generatedProgram =
      emittedProgram == std::string::npos
          ? std::string_view(generated)
          : std::string_view(generated).substr(emittedProgram);
  const std::size_t constrainedCallableBody =
      generatedProgram.find("// GTI verified-MIR body: deduced-callable");
  const std::size_t constrainedCallable =
      constrainedCallableBody == std::string_view::npos
          ? std::string_view::npos
          : generatedProgram.rfind("inspect_transfer", constrainedCallableBody);
  expect(constrainedCallableBody != std::string_view::npos &&
             constrainedCallable != std::string_view::npos &&
             constrainedCallableBody - constrainedCallable < 512,
         "frontend-checked generic constraints should retain a verified-MIR "
         "source-template body for an unnameable callable instance");
  expect(generatedProgram.find("unsafe_transfer") == std::string_view::npos &&
             generatedProgram.find("no_transfer") == std::string_view::npos &&
             generatedProgram.find("transferable") == std::string_view::npos &&
             generatedProgram.find("shareable") == std::string_view::npos,
         "capability policy and constraints should remain frontend facts, not "
         "emitted C++ traits or spellings");

  const std::string invalidSource = R"(
#include <std/memory>

class RawBox { int* address = nullptr; };
class Cleanup {
  int value = 0;
public:
  ~Cleanup() {}
};
[[no_transfer, no_share]] class Affine { int token = 0; };

T require_transfer<std::transferable T>(T value) { return value; }
void require_share<std::shareable T>(T& value) {}
void inspect_transfer<std::transferable T>(T value) {}

int main() {
  RawBox raw = require_transfer(RawBox());
  Cleanup cleanup = require_transfer(Cleanup());
  Affine affine = require_transfer(Affine());
  std::unique_ptr<Affine> owner = nullptr;
  std::unique_ptr<Affine> moved = require_transfer(std::move(owner));
  std::string_view view = "view";
  require_share(view);
  RawBox captured = RawBox();
  auto blocked = [captured]() -> int { return 0; };
  inspect_transfer(blocked);
  expected<int, RawBox> result = unexpected(RawBox());
  expected<int, RawBox> moved_result = require_transfer(result);
  return 0;
}
)";
  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-concurrency-type-capabilities.gti", invalidSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !invalid.canGenerateCode() &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2029") >= 7 &&
          hasDiagnostic(invalid.diagnostics, "'std::transferable<") &&
          hasDiagnostic(invalid.diagnostics, "'std::shareable<") &&
          hasDiagnosticHint(invalid.diagnostics,
                            "state and cleanup that can safely move") &&
          hasDiagnosticHint(invalid.diagnostics,
                            "read-only shared access that is safe"),
      "generic constraints should reject raw pointers, cleanup, explicit "
      "affinity, owners, borrowed views, captures, and expected alternatives");

  const std::string interfaceFailureSource = R"(
[[transfer, share]] interface Concurrent {};
[[no_transfer]] class Broken : public Concurrent { int value = 0; };
)";
  const lang::FrontendResult interfaceFailure = lang::Frontend().analyze(
      "invalid-capability-interface.gti", interfaceFailureSource,
      {standardLibraryPrelude()});
  const lang::Diagnostic *interfaceDiagnostic =
      findDiagnosticByCode(interfaceFailure.diagnostics, "GTI-S2059");
  expect(!interfaceFailure.canGenerateCode() &&
             interfaceDiagnostic != nullptr &&
             interfaceDiagnostic->message.find(
                 "does not satisfy the transfer capability") !=
                 std::string::npos &&
             hasRelatedDiagnostic(interfaceFailure.diagnostics,
                                  "Interface capability required here") &&
             interfaceDiagnostic->primary.start ==
                 interfaceFailureSource.find("Broken"),
         "interface capability proof failures should use a deterministic "
         "implementation span and related requirement span");

  const std::string policyFailureSource = R"(
[[transfer]] class WrongPositive {};
[[unsafe_share]] interface WrongInterface {};
[[no_share, unsafe_share]] class Conflicting {};
[[mystery]] struct UnknownPolicy {};
)";
  const lang::FrontendResult policyFailure =
      lang::Frontend().analyze("invalid-capability-policy.gti",
                               policyFailureSource, {standardLibraryPrelude()});
  expect(!policyFailure.canGenerateCode() &&
             countDiagnosticCode(policyFailure.diagnostics, "GTI-S2059") == 4 &&
             hasDiagnostic(policyFailure.diagnostics,
                           "only valid as an interface capability") &&
             hasDiagnostic(policyFailure.diagnostics,
                           "assertion belongs on an implementing class") &&
             hasDiagnostic(policyFailure.diagnostics,
                           "declared more than once or with conflicting") &&
             hasDiagnostic(policyFailure.diagnostics,
                           "Unknown concurrency capability attribute") &&
             hasRelatedDiagnostic(policyFailure.diagnostics,
                                  "first policy is declared here"),
         "capability policy misuse should produce one focused semantic "
         "diagnostic per invalid attribute without parser cascades");

  const std::string formatted = lang::Formatter().format(
      "[[no_transfer,no_share]]class NativeHandle{int descriptor=0;};");
  expect(formatted == "[[no_transfer, no_share]] class NativeHandle {\n"
                      "  int descriptor = 0;\n"
                      "};\n" &&
             lang::Formatter().format(formatted) == formatted,
         "concurrency capability attributes should format idempotently with "
         "C++-familiar declaration placement");
}

void testConstraintFailureProvenance() {
  const std::string prefix = R"(
namespace library {
concept snapshot_as<R, T> = requires(T& value) { { value.snapshot() } -> R; };
concept readable<T> = snapshot_as<int, T>;
int read<A, B>(A& unrelated, B& value) requires readable<B> {
  return value.snapshot();
}
}
)";
  const std::vector<std::pair<std::string, std::string>> rejected{
      {"struct Item {};", "has no member 'snapshot()'"},
      {"struct Item { bool snapshot() { return true; } };",
       "returns 'bool', but the required exact result is 'int32_t'"},
      {"class Item { int snapshot() { return 1; } };",
       "not publicly accessible"},
      {"struct Item { int snapshot() mut { return 1; } };",
       "not a read-only, nongeneric instance method"},
  };
  for (const auto &[definition, reason] : rejected) {
    const std::string source = prefix + definition + R"(
int main() {
  Item unrelated{};
  Item target{};
  return library::read(unrelated, target);
}
)";
    const auto result = lang::Frontend().analyze(
        "constraint-provenance.gti", source, {standardLibraryPrelude()});
    const auto *diagnostic =
        findDiagnosticByCode(result.diagnostics, "GTI-S2029");
    const auto argument = source.find("target);", source.find("int main"));
    expect(
        !result.canGenerateCode() && diagnostic != nullptr &&
            diagnostic->message.find("Argument 2 has type 'Item'") !=
                std::string::npos &&
            diagnostic->message.find("library::readable<Item>") !=
                std::string::npos &&
            diagnostic->primary.source.ends_with("constraint-provenance.gti") &&
            diagnostic->primary.start == argument &&
            diagnostic->primary.end == argument + 6 &&
            diagnostic->fixes.empty() &&
            hasRelatedDiagnostic(result.diagnostics,
                                 "library::snapshot_as<int32_t, Item>") &&
            hasRelatedDiagnostic(result.diagnostics, reason) &&
            hasDiagnosticHint(result.diagnostics, "public, safe, read-only"),
        "a nested reordered contract must preserve its concrete public "
        "application, exact second argument, leaf reason and safe guidance");
  }
  const std::string accepted = prefix + R"(
struct Item { int snapshot() { return 7; } };
int main() { Item value{}; return library::read(value, value); }
)";
  const auto valid = lang::Frontend().analyze(
      "constraint-provenance-valid.gti", accepted, {standardLibraryPrelude()});
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "contract diagnostic provenance must not change valid member "
         "selection or introduce backend policy");

  const std::string closed = R"(
concept snapshots<T> = requires(T& value) { { value.snapshot() } -> int; };
concept impossible<T> = snapshots<int>;
int read<T>(T value) requires impossible<T> { return 0; }
int main() { return read(true); }
)";
  const auto closedFailure = lang::Frontend().analyze(
      "closed-constraint-provenance.gti", closed, {standardLibraryPrelude()});
  const auto *closedDiagnostic =
      findDiagnosticByCode(closedFailure.diagnostics, "GTI-S2029");
  expect(
      closedDiagnostic != nullptr &&
          closedDiagnostic->message.find("Argument") == std::string::npos &&
          hasRelatedDiagnostic(closedFailure.diagnostics,
                               "Type 'int32_t' has no member") &&
          hasRelatedDiagnostic(closedFailure.diagnostics, "impossible<bool>"),
      "a closed requirement must not blame an unrelated inferred argument "
      "or replace the failed leaf type with the enclosing concept argument");

  const std::string shared = R"(
concept snapshots<T> = requires(T& value) { { value.snapshot() } -> int; };
int read<T>(T& first, T& second) requires snapshots<T> { return first.snapshot(); }
struct Item {};
int main() { Item value{}; return read(value, value); }
)";
  const auto sharedFailure = lang::Frontend().analyze(
      "shared-constraint-provenance.gti", shared, {standardLibraryPrelude()});
  const auto *sharedDiagnostic =
      findDiagnosticByCode(sharedFailure.diagnostics, "GTI-S2029");
  expect(sharedDiagnostic != nullptr &&
             sharedDiagnostic->message.find("Argument") == std::string::npos &&
             hasRelatedDiagnostic(sharedFailure.diagnostics, "snapshots<Item>"),
         "multiple occurrences of one inferred parameter retain a call-site "
         "diagnostic instead of arbitrarily blaming its first argument");

  const auto nestedArgument =
      lang::Frontend().analyze("nested-argument-constraint-provenance.gti", R"(
concept snapshots<T> = requires(T& value) { { value.snapshot() } -> int; };
struct Holder<T> {};
struct Item {};
int read<T>(Holder<T>& value) requires snapshots<T> { return 0; }
int main() { Holder<Item> value{}; return read(value); }
)",
                               {standardLibraryPrelude()});
  expect(hasDiagnostic(nestedArgument.diagnostics,
                       "Argument 1 has type 'Holder<Item>'") &&
             hasDiagnostic(nestedArgument.diagnostics,
                           "its inferred type 'Item'") &&
             hasRelatedDiagnostic(nestedArgument.diagnostics,
                                  "has no member 'snapshot()'"),
         "nested generic inference must distinguish the actual argument type "
         "from the leaf type it supplies");

  const std::string alternatives = R"(
concept snapshots<T> = requires(T& value) { { value.snapshot() } -> int; };
concept serializes<T> = requires(T& value) { { value.serialize() } -> int; };
concept readable<T> = snapshots<T> || serializes<T>;
int read<T>(T& value) requires readable<T> { return 0; }
struct Item {};
int main() { Item value{}; return read(value); }
)";
  const auto disjunction =
      lang::Frontend().analyze("disjunction-constraint-provenance.gti",
                               alternatives, {standardLibraryPrelude()});
  expect(!disjunction.canGenerateCode() &&
             hasDiagnostic(disjunction.diagnostics, "any alternative") &&
             hasRelatedDiagnostic(disjunction.diagnostics,
                                  "has no member 'snapshot()'") &&
             hasRelatedDiagnostic(disjunction.diagnostics,
                                  "has no member 'serialize()'") &&
             hasRelatedDiagnostic(disjunction.diagnostics, "readable<Item>"),
         "each failed disjunction branch must retain its own exact leaf "
         "explanation without combining unrelated proof facts");

  const std::string overloads = R"(
concept snapshots<T> = requires(T& value) { { value.snapshot() } -> int; };
concept serializes<T> = requires(T& value) { { value.serialize() } -> int; };
int read<T>(T& value) requires snapshots<T> { return value.snapshot(); }
int read<T>(T& value, int mode = 0) requires serializes<T> {
  return value.serialize();
}
struct Item {};
int main() { Item value{}; return read(value); }
)";
  const auto overloadFailure =
      lang::Frontend().analyze("overload-constraint-provenance.gti", overloads,
                               {standardLibraryPrelude()});
  expect(!overloadFailure.canGenerateCode() &&
             hasDiagnosticCode(overloadFailure.diagnostics, "GTI-S2012") &&
             hasRelatedDiagnostic(overloadFailure.diagnostics,
                                  "Candidate rejected: Argument 1") &&
             hasRelatedDiagnostic(overloadFailure.diagnostics,
                                  "has no member 'snapshot()'") &&
             hasRelatedDiagnostic(overloadFailure.diagnostics,
                                  "has no member 'serialize()'"),
         "multiple rejected overloads must retain their contract explanations "
         "rather than replace all causes with a conversion suggestion");
  const auto selected = lang::Frontend().analyze(
      "overload-constraint-valid.gti",
      overloads + "\nint read(Item& value) { return 0; }\n",
      {standardLibraryPrelude()});
  expect(selected.canGenerateCode(),
         "constraint failures on rejected overloads must not leak diagnostics "
         "when another exact candidate is viable");
}

void testMemberRequirementConcepts() {
  const std::string source = R"(
concept snapshot_as<T, R> = requires(T& item) { { item.snapshot() } -> R; };
concept snapshot_value<T> = snapshot_as<T, T>;
concept snapshot_and_copy<T> = snapshot_value<T> && std::copy_constructible<T>;
struct Sample {
  int value = 7;
  Sample snapshot() { Sample result{.value = this.value + 1}; return result; }
};
T take_snapshot<T>(T& value) requires snapshot_value<T> {
  return value.snapshot();
}
T forward_snapshot<T>(T& value) requires snapshot_and_copy<T> {
  return take_snapshot(value);
}
T direct_snapshot<T>(T& value)
  requires requires(T& x) { { x.snapshot() } -> T; } {
  return value.snapshot();
}
int main() {
  Sample value{};
  auto first = forward_snapshot(value);
  auto second = direct_snapshot(first);
  static_assert(requires(Sample& x) { { x.snapshot() } -> Sample; });
  static_assert(!(requires(Sample& x) { { x.absent() } -> Sample; }));
  return second.value - 9;
}
)";
  const auto valid = lang::Frontend().analyze("member-requirements.gti", source,
                                              {standardLibraryPrelude()});
  for (const auto &diagnostic : valid.diagnostics) {
    std::cerr << "Member requirement diagnostic: " << diagnostic.message
              << '\n';
  }
  expect(valid.canGenerateCode(),
         "named, relational, composed and direct member requirements must "
         "supply symbolic call facts without executing their bodies");
  const std::string formatted = lang::Formatter().format(source);
  expect(lang::Formatter().format(formatted) == formatted,
         "member requirements must format idempotently");
  const auto reformatted =
      lang::Frontend().analyze("member-requirements-formatted.gti", formatted,
                               {standardLibraryPrelude()});
  expect(reformatted.canGenerateCode(),
         "formatting member requirements must preserve parsing and semantics");
  if (valid.canGenerateCode()) {
    expect(!gti_test::emitCppText(valid).empty(),
           "direct unnamed requirements and unevaluated parameter metadata "
           "must survive verified LoweredProgram publication");
  }

  const std::vector<std::string> rejected{
      "struct Item {};",
      "struct Item { int snapshot() { return 1; } };",
      "struct Item { Item snapshot() mut { Item result{}; return result; } };",
      "struct Item { Item snapshot() && { Item result{}; return result; } };",
      "struct Item { static Item snapshot() { Item result{}; return result; } "
      "};",
      "class Item { Item snapshot() { Item result{}; return result; } };",
      "struct Item { Item snapshot(int value) { Item result{}; return result; "
      "} };",
  };
  for (const auto &definition : rejected) {
    const auto invalid = lang::Frontend().analyze(
        "member-requirement-rejected.gti",
        "concept snapshots<T> = requires(T& x) { { x.snapshot() } -> T; };\n" +
            definition +
            "\nT read<T>(T& x) requires snapshots<T> { return x.snapshot(); }\n"
            "int main() { Item item{}; auto copy = read(item); return 0; }",
        {standardLibraryPrelude()});
    expect(!invalid.canGenerateCode() &&
               hasDiagnostic(invalid.diagnostics, "satisfy concept"),
           "missing, mismatched, mutable, consuming, static and inaccessible "
           "members must fail the declared concept");
  }
  const auto unsafeMember = lang::Frontend().analyze(
      "member-requirement-unsafe.gti",
      "struct Item { unsafe Item snapshot() { "
      "Item result{}; return result; "
      "} }; int main() { static_assert(!(requires(Item& x) "
      "{ { x.snapshot() } -> Item; })); return 0; }",
      {standardLibraryPrelude()});
  expect(unsafeMember.canGenerateCode(),
         "safe member requirements must reject unsafe method declarations");
  const auto bounded =
      lang::Frontend().analyze("member-requirement-boundaries.gti", R"(
concept snapshots<T> = requires(T& x) { { x.snapshot() } -> T; };
struct Recursive {
  Recursive snapshot() requires snapshots<Recursive> {
    Recursive result{};
    return result;
  }
};
struct Borrowed {
  int value = 0;
  int& snapshot() { return this.value; }
};
int main() {
  static_assert(!(requires(Recursive& x) { { x.snapshot() } -> Recursive; }));
  static_assert(!(requires(Borrowed& x) { { x.snapshot() } -> int&; }));
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(bounded.canGenerateCode(),
         "recursive constraints must fail closed, and exact reference results "
         "must not satisfy owned member requirements");
  const auto disjunction =
      lang::Frontend().analyze("member-requirement-disjunction.gti", R"(
concept snapshots<T> = requires(T& x) { { x.snapshot() } -> T; };
concept maybe_snapshot<T> = snapshots<T> || std::copy_constructible<T>;
T invalid<T>(T& x) requires maybe_snapshot<T> { return x.snapshot(); }
)",
                               {standardLibraryPrelude()});
  expect(
      !disjunction.canGenerateCode(),
      "a disjunction must not grant a member absent from another alternative");

  const auto wrongName =
      lang::Frontend().analyze("member-requirement-name.gti", R"(
concept reads<T> = requires(T& x) { { x.read() } -> T; };
concept writes<T> = requires(T& x) { { x.write() } -> T; };
T write<T>(T& x) requires writes<T> { return x.write(); }
T invalid<T>(T& x) requires reads<T> { return write(x); }
)",
                               {standardLibraryPrelude()});
  expect(!wrongName.canGenerateCode(),
         "structural implication must preserve the required member name");

  const auto malformed = lang::Frontend().analyze(
      "member-requirement-malformed.gti",
      "concept bad<T> = requires(T& x) { { other.clone() } -> T; };",
      {standardLibraryPrelude()});
  expect(!malformed.canGenerateCode() &&
             hasDiagnostic(malformed.diagnostics, "declared receiver"),
         "requirement syntax must reject an undeclared synthetic receiver");
  const auto consumingQuery = lang::Frontend().analyze(
      "member-requirement-consuming-query.gti",
      "concept bad<T> = requires(T&& x) { { x.clone() } -> T; };",
      {standardLibraryPrelude()});
  expect(!consumingQuery.canGenerateCode() &&
             hasDiagnostic(consumingQuery.diagnostics, "must be a reference"),
         "member queries must not silently treat consuming references as "
         "read-only references");
  const auto directFailure =
      lang::Frontend().analyze("member-requirement-direct-failure.gti", R"(
T snapshot<T>(T& x)
    requires requires(T& item) { { item.snapshot() } -> T; } {
  return x.snapshot();
}
int main() { int value = 1; return snapshot(value); }
)",
                               {standardLibraryPrelude()});
  expect(!directFailure.canGenerateCode() &&
             hasDiagnosticCode(directFailure.diagnostics, "GTI-S2029") &&
             hasDiagnostic(directFailure.diagnostics,
                           "required member 'snapshot()'"),
         "direct member failures must identify the required member, not "
         "invent a concept name");

  const auto impossibleClone = lang::Frontend().analyze(
      "noncloneable-owner.gti", R"(
#include <std/memory>
#include <std/vector>
int main() {
  std::vector<std::unique_ptr<int>> owners{};
  auto copy = owners.clone();
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!impossibleClone.canGenerateCode() &&
             hasDiagnosticCode(impossibleClone.diagnostics, "GTI-S2029"),
         "cloning must not make exclusive owners implicitly copyable");
}

void testExactMemberSignatureConcepts() {
  const auto analyze = [](const std::string &source) {
    return lang::Frontend().analyze("member-signatures.gti", source,
                                    {standardLibraryPrelude()}, {},
                                    {standardLibraryRoot()});
  };
  const std::string contract = R"(
concept accepts<C, V> = requires(mut C& receiver, V& input, mut V& output) {
  { receiver.apply(input, output) } -> int;
};
int invoke<C>(mut C& receiver, int& input, mut int& output)
    requires accepts<C, int> { return receiver.apply(input, output); }
)";
  const std::string good = R"(
struct Processor {
  int apply(int& input, mut int& output) mut { output = input; return 1; }
};
int main() { mut Processor p{}; int input = 7; mut int output = 0;
  return invoke(p, input, output) - 1; }
)";
  const auto valid = analyze(contract + good);
  expect(valid.canGenerateCode(),
         "exact mutable receiver and parameter contracts must lower");
  if (valid.canGenerateCode())
    expect(!gti_test::emitCppText(valid).empty(),
           "concrete structural calls must reach verified MIR emission");
  const std::string formatted = lang::Formatter().format(contract + good);
  expect(lang::Formatter().format(formatted) == formatted &&
             analyze(formatted).canGenerateCode(),
         "typed member requirements must format idempotently without changing "
         "meaning");
  const std::vector<std::string> rejected{
      "int apply(int& input, mut int& output) { return 1; }",
      "int apply(int& input, int& output) mut { return 1; }",
      "int apply(int input, mut int& output) mut { return 1; }",
      "bool apply(int& input, mut int& output) mut { return true; }",
      "int apply(int& input) mut { return 1; }",
      "static int apply(int& input, mut int& output) { return 1; }",
      "private: int apply(int& input, mut int& output) mut { return 1; }",
  };
  for (const auto &method : rejected) {
    const auto invalid =
        analyze(contract + "struct Processor { " + method +
                " };"
                "int main() { mut Processor p{}; int input = 7; mut int output "
                "= 0; return invoke(p, input, output); }");
    expect(!invalid.canGenerateCode() &&
               hasDiagnosticCode(invalid.diagnostics, "GTI-S2029"),
           "signature requirements must reject receiver, access, arity, "
           "result, static and private mismatches");
    expect(hasRelatedDiagnostic(invalid.diagnostics,
                                "apply(V&, mut V&) mut -> int"),
           "requirement diagnostics must show exact parameters and receiver "
           "access instead of a misleading nullary signature");
  }
  const auto illegalMutation = analyze(contract + R"(
int bad<C>(C& receiver, int& input, mut int& output)
    requires accepts<C, int> { return receiver.apply(input, output); }
)");
  expect(!illegalMutation.canGenerateCode(),
         "a mut contract must not grant mutation through a read-only receiver");
  for (const std::string body :
       {"requires(T& r, int& a) { { r.f() } -> int; }",
        "requires(T& r, int& a) { { r.f(r) } -> int; }",
        "requires(T& r, int& a, int& a) { { r.f(a, a) } -> int; }",
        "requires(T& r, mut int a) { { r.f(a) } -> int; }"}) {
    const auto malformed = analyze("concept invalid<T> = " + body + ";");
    expect(!malformed.canGenerateCode() &&
               hasDiagnosticCode(malformed.diagnostics, "GTI-P0001"),
           "signature query must not admit expressions, duplicate bindings or "
           "unused arguments");
  }
  const auto unrelated = analyze(R"(
struct Box<T> { T value; };
struct Adapter<T> { Box<T> adapt(T& value) { Box<T> b{.value = value}; return b; } };
concept adapted<T> = requires(Adapter<T>& a, T& v) { { a.adapt(v) } -> Box<T>; };
concept indirect<T> = adapted<T>;
Box<T> get<T>(T& value) requires indirect<T> { Adapter<T> a{}; return a.adapt(value); }
int main() { int x = 7; auto result = get(x); return result.value - 7; }
)");
  expect(unrelated.canGenerateCode(),
         "dependent receiver/result compounds must substitute through nested "
         "concepts");

  const auto nestedAliases = analyze(R"(
namespace app {
using count = int;
struct Result<T> { T value; };
namespace nested {
struct Reader {
  Result<count> read() mut { Result<count> r = {.value = 3}; return r; }
};
concept reader<R> = requires(mut R& stream) {
  { stream.read() } -> Result<count>;
};
Result<count> call<R>(mut R& stream) requires reader<R> { return stream.read(); }
}
}
int main() { mut app::nested::Reader r{}; auto result = app::nested::call(r); return 0; }
)");
  expect(nestedAliases.canGenerateCode(),
         "compound signature types must resolve enclosing namespace aliases");
  const auto overloaded = analyze(R"(
struct Overloaded {
  int get(int& value) { return value; }
  bool get(bool& value) { return value; }
};
concept reads_both<R> =
  requires(R& receiver, int& value) { { receiver.get(value) } -> int; } &&
  requires(R& receiver, bool& value) { { receiver.get(value) } -> bool; };
int read<R>(R& reader, int& value, bool& condition) requires reads_both<R> {
  if (reader.get(condition)) { return reader.get(value); }
  return 0;
}
int main() { Overloaded r{}; int value = 7; bool condition = true;
  return read(r, value, condition) - 7; }
)");
  expect(overloaded.canGenerateCode(),
         "symbolic member calls must select the exact required overload, not "
         "only a member name or result");

  const std::string formatPrefix =
      "#include <std/format>\n#include <std/string>\nstruct Item {};\n";
  for (const auto &call :
       {"std::format(\"{}\", Item())", "std::try_format(\"{}\", Item())",
        "std::try_format(\"{} {}\", 1, Item())",
        "std::format(\"{0}\", 1, Item())"}) {
    const auto invalid = analyze(formatPrefix + "int main() { [[discard]] " +
                                 call + "; return 0; }");
    expect(!invalid.canGenerateCode() &&
               hasDiagnosticCode(invalid.diagnostics, "GTI-S2029"),
           "unsupported format arguments, including unused later pack "
           "elements, must fail statically");
    expect(hasDiagnostic(invalid.diagnostics, "formattable"),
           "format constraints must name the public contract");
  }
}

void testSourceDefinedConcepts() {
  const std::string source = R"(
concept arithmetic_value<T> = std::numeric<T> && std::copyable<T>;
concept signed_arithmetic<T> =
    arithmetic_value<T> and std::signed_numeric<T>;

T numeric_identity<std::numeric T>(T value) { return value; }

T twice<signed_arithmetic T>(T value) {
  return T(value + value);
}

T forward<signed_arithmetic T>(T value) {
  return numeric_identity(value);
}

int main() {
  int result = forward(twice(3));
  if (result == 6) { return 0; }
  return 1;
}
)";
  lang::FrontendResult valid = lang::Frontend().analyze(
      "source-concepts.gti", source, {standardLibraryPrelude()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected source concept diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "source-defined concept aliases and conjunctions should propagate "
         "their exact capabilities");

  const lang::FunctionDecl *twice =
      findTopLevelFunction(valid.program, "twice");
  const lang::FunctionInfo *twiceInfo =
      twice == nullptr ? nullptr : valid.semantics.findFunction(*twice);
  expect(
      twiceInfo != nullptr && twiceInfo->genericParameters.size() == 1 &&
          lang::hasConstraint(twiceInfo->genericParameters.front().constraints,
                              lang::GenericConstraintKind::Numeric) &&
          lang::hasConstraint(twiceInfo->genericParameters.front().constraints,
                              lang::GenericConstraintKind::SignedNumeric) &&
          lang::hasConstraint(twiceInfo->genericParameters.front().constraints,
                              lang::GenericConstraintKind::Copyable),
      "semantic generic metadata should retain the resolved atomic "
      "capability set rather than a public concept spelling");

  const bool recordedConcept =
      std::any_of(valid.semantics.database().symbols().begin(),
                  valid.semantics.database().symbols().end(),
                  [](const lang::SymbolRecord &symbol) {
                    return symbol.kind == lang::SymbolKind::Concept &&
                           symbol.qualifiedName == "signed_arithmetic";
                  });
  expect(recordedConcept,
         "source concepts should be compiler-owned symbols for tooling");

  const lang::SourceUnitId sourceUnit = valid.sourceGraph.entryUnit();
  const std::size_t constrainedUse =
      source.find("signed_arithmetic T", source.find("twice"));
  const std::size_t conceptDeclaration = source.find("signed_arithmetic<T>");
  const lang::LanguageQueries queries;
  const std::optional<lang::HoverInfo> conceptHover =
      queries.hover(valid, sourceUnit, constrainedUse + 1);
  const std::optional<lang::DefinitionInfo> conceptDefinition =
      queries.definition(valid, sourceUnit, constrainedUse + 1);
  expect(conceptHover &&
             conceptHover->signature ==
                 "concept signed_arithmetic<T> = arithmetic_value<T> && "
                 "std::signed_numeric<T>" &&
             conceptDefinition &&
             conceptDefinition->target.start == conceptDeclaration,
         "concept uses should expose their canonical composition and semantic "
         "definition identity");

  const std::string generated = gti_test::emitCppText(valid);
  expect(generated.find("concept arithmetic_value") == std::string::npos &&
             generated.find("concept signed_arithmetic") == std::string::npos &&
             generated.find("template <typename T>") != std::string::npos,
         "concepts should remain frontend validity metadata and not delegate "
         "constraint checking to the C++ backend");

  lang::FrontendResult concreteFailure =
      lang::Frontend().analyze("source-concept-failure.gti", R"(
concept signed_value<T> = std::signed_numeric<T> && std::copyable<T>;
T require_signed<signed_value T>(T value) { return value; }
int main() {
  uint64_t value = require_signed(uint64_t(1));
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(!concreteFailure.canGenerateCode() &&
             hasDiagnostic(concreteFailure.diagnostics,
                           "does not satisfy concept "
                           "'signed_value<uint64_t>'"),
         "concrete arguments should be checked against composed concepts with "
         "the source concept name in diagnostics");

  lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-source-concepts.gti", R"(
concept broken<T> = missing<T>;
concept first<T> = second<T>;
concept second<T> = first<T>;
concept wrong<T> = std::copyable<U>;
concept duplicate<T> = std::copyable<T>;
concept duplicate<T> = std::movable<T>;
concept leaked<T> = gti_internal::numeric_capability<T>;
@compiler_constraint("numeric") concept forged<T>;
T direct<gti_internal::numeric_capability T>(T value) { return value; }
int main() { return 0; }
)",
                               {standardLibraryPrelude()});
  expect(!invalid.canGenerateCode() &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2049") &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2058") &&
             hasDiagnostic(invalid.diagnostics, "Unknown concept 'missing'") &&
             hasDiagnostic(invalid.diagnostics,
                           "Concept definition cycle involving") &&
             hasDiagnostic(invalid.diagnostics, "must name a visible type") &&
             hasDiagnostic(invalid.diagnostics,
                           "Duplicate declaration of 'duplicate'") &&
             hasDiagnostic(invalid.diagnostics,
                           "@compiler_constraint is reserved"),
         "concept declaration diagnostics should cover unknown dependencies, "
         "cycles, parameter misuse, duplicates, and forged compiler bindings");

  lang::FrontendResult noPrelude = lang::Frontend().analyze(
      "concept-without-prelude.gti",
      "T value<std::numeric T>(T input) { return input; }");
  expect(!noPrelude.canGenerateCode() &&
             hasDiagnostic(noPrelude.diagnostics,
                           "Unknown generic concept 'std::numeric'"),
         "public standard concepts should come from the prelude rather than "
         "hard-coded std spellings");

  const std::filesystem::path visibilityRoot =
      std::filesystem::temp_directory_path() / "gti-concept-visibility";
  const std::filesystem::path visibilityEntry = visibilityRoot / "main.gti";
  const std::filesystem::path conceptsUnit = visibilityRoot / "concepts.gti";
  const std::filesystem::path wrapperUnit = visibilityRoot / "wrapper.gti";
  const auto canonical = [](const std::filesystem::path &path) {
    return std::filesystem::weakly_canonical(path).string();
  };
  const std::string conceptsSource = R"(
namespace capabilities {
concept signed_value<T> = std::signed_numeric<T>;
}
)";
  lang::FrontendResult visible = lang::Frontend().analyze(
      visibilityEntry,
      "#include \"concepts.gti\"\n"
      "namespace cap = capabilities;\n"
      "T use<cap::signed_value T>(T value) { return value; }\n"
      "int main() { return use(1) - 1; }\n",
      {standardLibraryPrelude()}, {{canonical(conceptsUnit), conceptsSource}});
  expect(visible.canGenerateCode(),
         "directly included concepts should resolve through namespace aliases");

  lang::FrontendResult hidden = lang::Frontend().analyze(
      visibilityEntry,
      "#include \"wrapper.gti\"\n"
      "T use<capabilities::signed_value T>(T value) { return value; }\n"
      "int main() { return use(1) - 1; }\n",
      {standardLibraryPrelude()},
      {{canonical(wrapperUnit), "#include \"concepts.gti\"\n"},
       {canonical(conceptsUnit), conceptsSource}});
  expect(!hidden.canGenerateCode() &&
             hasDiagnosticCode(hidden.diagnostics, "GTI-S2024") &&
             hasDiagnosticHint(hidden.diagnostics, "#include \"concepts.gti\""),
         "concept declarations should obey direct-include visibility without "
         "transitive re-export");

  lang::Lexer lexer;
  lang::Parser disjunctionParser(lexer.scan(
      "concept either<T> = std::integral<T> || std::floating_point<T>; "
      "int okay = 1;"));
  const lang::Program recovered = disjunctionParser.parse();
  const lang::ConceptDecl *either =
      recovered.declarations().empty()
          ? nullptr
          : lang::stmtAs<lang::ConceptDecl>(
                recovered.declarations().front().get());
  expect(!disjunctionParser.hadError() && either != nullptr &&
             recovered.declarations().size() == 2 &&
             either->constraintExpression().root <
                 either->constraintExpression().nodes.size() &&
             either->constraintExpression()
                     .nodes[either->constraintExpression().root]
                     .kind == lang::ConstraintExpressionKind::Disjunction,
         "concept disjunction should parse as an explicit constraint tree "
         "without losing the following declaration");

  const std::string formatted = lang::Formatter().format(
      "concept sortable<T>=std::totally_ordered<T>&&std::movable<T>;");
  expect(formatted == "concept sortable<T> = std::totally_ordered<T> && "
                      "std::movable<T>;\n" &&
             lang::Formatter().format(formatted) == formatted,
         "concept declarations should format with C++-familiar generic and "
         "conjunction spacing");
}

void testRequiresDisjunction() {
  const std::string source = R"(
concept scalar<T> = std::integral<T> || std::floating_point<T>;
concept floating_or_closed<T> =
    std::integral<double> || std::floating_point<T>;

T doubled<scalar T>(T value) { return value + value; }
T floating_identity<floating_or_closed T>(T value) { return value; }
T forward_scalar<scalar T>(T value) { return doubled(value); }

T trailing_identity<T>(T value)
  requires std::integral<T> || std::floating_point<T> {
  return value;
}

T forward_trailing<T>(T value)
  requires std::integral<T> || std::floating_point<T> {
  return trailing_identity(value);
}

T tripled<T>(T value)
  requires (std::integral<T> || std::floating_point<T>) && std::copyable<T> {
  return value + value + value;
}

int main() {
  if (doubled(2) != 4) { return 1; }
  if (doubled(1.5) != 3.0) { return 2; }
  if (floating_identity(1.25) != 1.25) { return 3; }
  if (forward_scalar(4) != 8) { return 4; }
  if (forward_trailing(2.5) != 2.5) { return 5; }
  if (tripled(3) != 9) { return 6; }
  if (tripled(2.0) != 6.0) { return 7; }
  return 0;
}
)";
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "requires-disjunction.gti", source, {standardLibraryPrelude()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected requires disjunction diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "inline and trailing disjunctions should accept either complete "
         "satisfied branch");

  const lang::ConceptDecl *scalar = nullptr;
  for (const lang::StmtPtr &declaration : valid.program.declarations()) {
    const auto *candidate = lang::stmtAs<lang::ConceptDecl>(declaration.get());
    if (candidate != nullptr && candidate->name().lexeme == "scalar") {
      scalar = candidate;
      break;
    }
  }
  const lang::FunctionDecl *doubled =
      findTopLevelFunction(valid.program, "doubled");
  const lang::FunctionDecl *tripled =
      findTopLevelFunction(valid.program, "tripled");
  const lang::FunctionInfo *doubledInfo =
      doubled == nullptr ? nullptr : valid.semantics.findFunction(*doubled);
  expect(scalar != nullptr &&
             scalar->constraintExpression().root <
                 scalar->constraintExpression().nodes.size() &&
             scalar->constraintExpression()
                     .nodes[scalar->constraintExpression().root]
                     .kind == lang::ConstraintExpressionKind::Disjunction,
         "source concepts should retain disjunction in their AST expression "
         "tree");
  expect(
      tripled != nullptr && tripled->requiresClause() &&
          tripled->requiresClause()->expression.root <
              tripled->requiresClause()->expression.nodes.size() &&
          tripled->requiresClause()
                  ->expression.nodes[tripled->requiresClause()->expression.root]
                  .kind == lang::ConstraintExpressionKind::Conjunction,
      "parentheses should override the default &&-before-|| precedence");
  lang::Lexer lexer;
  lang::Parser precedenceParser(lexer.scan(
      "concept precedence<T> = std::copyable<T> || std::movable<T> && "
      "std::default_initializable<T>;"));
  const lang::Program precedenceProgram = precedenceParser.parse();
  const auto *precedence =
      precedenceProgram.declarations().empty()
          ? nullptr
          : lang::stmtAs<lang::ConceptDecl>(
                precedenceProgram.declarations().front().get());
  const lang::ConstraintExpression *precedenceExpression =
      precedence == nullptr ? nullptr : &precedence->constraintExpression();
  const lang::ConstraintExpressionNode *precedenceRoot =
      precedenceExpression == nullptr ||
              precedenceExpression->root >= precedenceExpression->nodes.size()
          ? nullptr
          : &precedenceExpression->nodes[precedenceExpression->root];
  expect(!precedenceParser.hadError() && precedenceRoot != nullptr &&
             precedenceRoot->kind ==
                 lang::ConstraintExpressionKind::Disjunction &&
             precedenceRoot->right < precedenceExpression->nodes.size() &&
             precedenceExpression->nodes[precedenceRoot->right].kind ==
                 lang::ConstraintExpressionKind::Conjunction,
         "&& should bind more tightly than || without parentheses");
  expect(doubledInfo != nullptr &&
             doubledInfo->genericParameters.front().conceptId != 0 &&
             lang::hasConstraint(
                 doubledInfo->genericParameters.front().constraints,
                 lang::GenericConstraintKind::Numeric) &&
             !lang::hasConstraint(
                 doubledInfo->genericParameters.front().constraints,
                 lang::GenericConstraintKind::Integral) &&
             !lang::hasConstraint(
                 doubledInfo->genericParameters.front().constraints,
                 lang::GenericConstraintKind::FloatingPoint),
         "a disjunctive inline constraint should retain its concept identity "
         "and expose only common branch guarantees inside the generic body");

  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      valid, valid.mir, valid.mir, lang::TargetInfo::host());
  const lang::LoweredFunctionDeclaration *loweredDoubled = nullptr;
  const lang::LoweredFunctionDeclaration *loweredTripled = nullptr;
  for (const lang::LoweredDeclaration &declaration : lowered.declarations()) {
    const auto *function =
        std::get_if<lang::LoweredFunctionDeclaration>(&declaration.payload);
    if (function == nullptr) {
      continue;
    }
    if (function->qualifiedName == "doubled") {
      loweredDoubled = function;
    } else if (function->qualifiedName == "tripled") {
      loweredTripled = function;
    }
  }
  expect(
      lang::verifyLoweredProgram(lowered).empty() &&
          loweredDoubled != nullptr &&
          loweredDoubled->genericParameters.front().constraintConcept != 0 &&
          loweredTripled != nullptr &&
          loweredTripled->requirementRoot <
              loweredTripled->requirementExpression.size() &&
          loweredTripled->requirementExpression[loweredTripled->requirementRoot]
                  .kind == lang::ConstraintExpressionKind::Conjunction,
      "LoweredProgram should preserve and verify inline concept identity "
      "and the trailing Boolean expression tree");

  const lang::SourceUnitId sourceUnit = valid.sourceGraph.entryUnit();
  const lang::LanguageQueries queries;
  const std::size_t scalarUse = source.find("scalar T");
  const std::optional<lang::HoverInfo> scalarHover =
      queries.hover(valid, sourceUnit, scalarUse + 1);
  expect(scalarHover && scalarHover->signature ==
                            "concept scalar<T> = std::integral<T> || "
                            "std::floating_point<T>",
         "concept hover should render canonical disjunction syntax");

  const lang::FrontendResult rejected =
      lang::Frontend().analyze("requires-disjunction-rejected.gti", R"(
concept scalar<T> = std::integral<T> || std::floating_point<T>;
class marker { public: marker() {} };
T require_scalar<scalar T>(T value) { return value; }
int main() {
  marker value = require_scalar(marker());
  return 0;
}
)",
                               {standardLibraryPrelude()});
  const auto failure = std::find_if(
      rejected.diagnostics.begin(), rejected.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2029" &&
               diagnostic.message.find("any alternative in concept 'scalar'") !=
                   std::string::npos;
      });
  expect(!rejected.canGenerateCode() && failure != rejected.diagnostics.end() &&
             failure->related.size() >= 3 && !failure->hints.empty(),
         "a concrete type satisfying neither branch should receive one "
         "structured diagnostic with both alternative failures");

  const lang::FrontendResult emptyIntersection = lang::Frontend().analyze(
      "requires-disjunction-empty-intersection.gti", R"(
concept floating_or_closed<T> =
    std::integral<double> || std::floating_point<T>;
T require_floating<floating_or_closed T>(T value) { return value; }
int main() { return require_floating(1); }
)",
      {standardLibraryPrelude()});
  expect(!emptyIntersection.canGenerateCode() &&
             hasDiagnostic(emptyIntersection.diagnostics,
                           "any alternative in concept "
                           "'floating_or_closed'"),
         "an inline concept with no common facts should still enforce its "
         "exact Boolean predicate at concrete instantiation");

  const lang::FrontendResult rejectedTrailing =
      lang::Frontend().analyze("requires-disjunction-trailing-rejected.gti", R"(
class marker { public: marker() {} };
T require_scalar<T>(T value)
  requires std::integral<T> || std::floating_point<T> {
  return value;
}
int main() {
  marker value = require_scalar(marker());
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(!rejectedTrailing.canGenerateCode() &&
             hasDiagnostic(rejectedTrailing.diagnostics,
                           "any alternative in the requires clause") &&
             hasRelatedDiagnostic(rejectedTrailing.diagnostics,
                                  "Every branch joined by this disjunction"),
         "a failed trailing disjunction should report the operator and both "
         "unsatisfied alternatives");

  const lang::FrontendResult unsoundBody =
      lang::Frontend().analyze("requires-disjunction-proof.gti", R"(
concept scalar<T> = std::integral<T> || std::floating_point<T>;
T invalid_remainder<scalar T>(T value) { return value % value; }
int main() { return 0; }
)",
                               {standardLibraryPrelude()});
  expect(!unsoundBody.canGenerateCode(),
         "a generic body must not use a capability supplied by only one "
         "disjunction branch");

  const lang::FrontendResult structuralCommon = lang::Frontend().analyze(
      "requires-disjunction-structural.gti", R"(
#include <std/functional>
class readable {
public:
  int32_t operator()(int32_t& value) { return value + 1; }
};
class mutable_callable {
public:
  int32_t operator()(int32_t& value) mut { return value + 2; }
};
int32_t invoke<Callable>(int32_t value, mut Callable callable)
  requires std::read_callable<Callable, int32_t, int32_t> ||
           std::mut_callable<Callable, int32_t, int32_t> {
  return callable(value);
}
int main() {
  if (invoke(1, readable()) != 2) { return 1; }
  if (invoke(1, mutable_callable()) != 3) { return 2; }
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(structuralCommon.canGenerateCode(),
         "disjunctive structural branches should expose their strongest "
         "common callable capability to the generic body");

  lang::Parser malformed(lexer.scan(R"(
T broken<T>(T value) requires (std::integral<T> || ) { return value; }
int okay() { return 0; }
)"));
  const lang::Program recovered = malformed.parse();
  expect(malformed.hadError() &&
             findTopLevelFunction(recovered, "okay") != nullptr,
         "a missing disjunction operand should diagnose and recover at the "
         "following declaration");

  const std::string formatted = lang::Formatter().format(
      "concept scalar<T>=std::integral<T>||std::floating_point<T>;\n"
      "T select<T>(T value)requires(std::integral<T>or "
      "std::floating_point<T>)and std::copyable<T>{return value;}");
  expect(formatted.find("std::integral<T> || std::floating_point<T>") !=
                 std::string::npos &&
             formatted.find("(std::integral<T> or "
                            "std::floating_point<T>) and "
                            "std::copyable<T>") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should preserve keyword aliases and normalize "
         "spacing for parenthesized disjunctions");
}

void testDomainValueOperatorCapabilities() {
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "domain-operator-capabilities.gti", R"(
#include <std/numeric>

class vec3 {
public:
  vec3(float x, float y, float z) : x(x), y(y), z(z) {}
  float x;
  float y;
  float z;
  vec3 operator+(vec3& other) {
    return vec3(this.x + other.x, this.y + other.y, this.z + other.z);
  }
  vec3 operator-(vec3& other) {
    return vec3(this.x - other.x, this.y - other.y, this.z - other.z);
  }
  vec3 operator-() { return vec3(-this.x, -this.y, -this.z); }
  vec3 operator*(float scale) {
    return vec3(this.x * scale, this.y * scale, this.z * scale);
  }
};

int main() {
  vec3 from = vec3(0.0, 0.0, 0.0);
  vec3 to = vec3(10.0, 20.0, 30.0);
  vec3 middle = std::lerp(from, to, 0.5);
  vec3 opposite = std::negated(to);
  float scalar = std::lerp(0.0, 8.0, 0.25);
  return (middle.x == 5.0 && middle.y == 10.0 && middle.z == 15.0 &&
          opposite.x == -10.0 && scalar == 2.0)
             ? 0
             : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected domain-operator diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "a user domain type should satisfy std::vector_space and drive the "
         "generic std::lerp while built-in float uses its exact overload");

  const auto rejects = [&](std::string_view name, const std::string &source,
                           std::string_view conceptName,
                           std::string_view hintFragment,
                           std::string_view message) {
    const lang::FrontendResult result = lang::Frontend().analyze(
        std::string(name), source, {standardLibraryPrelude()}, {},
        {standardLibraryRoot()});
    expect(
        !result.canGenerateCode() &&
            hasDiagnostic(result.diagnostics, std::string("satisfy concept '") +
                                                  std::string(conceptName)) &&
            hasDiagnosticHint(result.diagnostics, std::string(hintFragment)),
        std::string(message));
  };

  rejects("domain-operator-missing-add.gti", R"(
class half {
public:
  half(float v) : v(v) {}
  float v;
};
T twice<T>(T& a) requires std::addable<T> { return a + a; }
int main() { half h = half(1.0); half r = twice(h); return 0; }
)",
          "std::addable", "T operator+(T&)",
          "a domain type without operator+ should fail std::addable with the "
          "member-form hint");

  rejects("domain-operator-missing-negate.gti", R"(
class half {
public:
  half(float v) : v(v) {}
  float v;
  half operator+(half& o) { return half(this.v + o.v); }
};
T opposite<T>(T& a) requires std::negatable<T> { return -a; }
int main() { half h = half(1.0); half r = opposite(h); return 0; }
)",
          "std::negatable", "unary",
          "a domain type without unary operator- should fail std::negatable");

  rejects("domain-operator-builtin-excluded.gti", R"(
T opposite<T>(T& a) requires std::negatable<T> { return -a; }
int main() { int32_t v = 3; int32_t r = opposite(v); return 0; }
)",
          "std::negatable", "unary",
          "the domain family is member-only: a built-in numeric keeps its own "
          "checked arithmetic and does not satisfy it");

  rejects("domain-operator-mismatched-scalar.gti", R"(
class vec2 {
public:
  vec2(float x, float y) : x(x), y(y) {}
  float x;
  float y;
  vec2 operator*(float s) { return vec2(this.x * s, this.y * s); }
};
T scale<T, S>(T& v, S s) requires std::scalable_by<T, S> { return v * s; }
int main() { vec2 v = vec2(1.0, 2.0); vec2 r = scale(v, 2); return 0; }
)",
          "std::scalable_by", "operator*(Scalar)",
          "scaling by a type the domain does not accept should fail "
          "std::scalable_by rather than converting the scalar");

  rejects("domain-operator-borrowed-result.gti", R"(
class holder {
public:
  holder(float v) : v(v) {}
  mut float v;
  holder& operator+(holder& other) { return other; }
};
T twice<T>(T& a) requires std::addable<T> { return a + a; }
int main() { holder h = holder(1.0); holder r = twice(h); return 0; }
)",
          "std::addable", "owned value",
          "a borrowed operator result should fail std::addable: the capability "
          "requires an owned domain value");
}

void testStructuralRequiresAndAccumulate() {
  const std::string source = R"(
#include <std/iterator>
#include <std/numeric>
#include <std/vector>

concept compatible_sentinel<Sentinel, Iterator> =
    std::sentinel_for<Sentinel, Iterator>;

T local_accumulate<Iterator, Sentinel, std::numeric T>(
    mut Iterator first, Sentinel last, mut T init)
  requires std::input_iterator<Iterator> &&
           compatible_sentinel<Sentinel, Iterator> &&
           std::accumulates_into<Iterator, T> {
  for (; first != last; ++first) {
    init = T(std::move(init) + *first);
  }
  return init;
}

T forward_accumulate<Iterator, Sentinel, std::numeric T>(
    mut Iterator first, Sentinel last, mut T init)
  requires std::input_iterator<Iterator> &&
           compatible_sentinel<Sentinel, Iterator> &&
           std::accumulates_into<Iterator, T> {
  return std::accumulate(std::move(first), std::move(last), std::move(init));
}

class NumberSentinel {
  int limit;

public:
  NumberSentinel(int value) : limit(value) {}
  int position() { return this.limit; }
};

class NumberIterator {
  mut int current;

public:
  NumberIterator(int value) : current(value) {}
  int& operator*() { return this.current; }
  void operator++() mut { this.current++; }
  bool operator!=(NumberSentinel& sentinel) {
    return this.current != sentinel.position();
  }
};

int main() {
  int local = local_accumulate(NumberIterator(1), NumberSentinel(5), 0);
  int standard = std::accumulate(NumberIterator(1), NumberSentinel(5), 0);
  int forwarded =
      forward_accumulate(NumberIterator(1), NumberSentinel(5), 0);
  mut std::vector<int> values{};
  values.push_back(1);
  values.push_back(2);
  values.push_back(3);
  int owned = std::accumulate(values.begin(), values.end(), 0);
  values.push_back(4);
  mut std::vector<int8_t> narrowValues{};
  narrowValues.push_back(int8_t(1));
  narrowValues.push_back(int8_t(2));
  int8_t narrow = std::accumulate(
      narrowValues.begin(), narrowValues.end(), int8_t(0));
  return local + standard + forwarded + owned + narrow - 39;
}
)";

  lang::FrontendResult valid = lang::Frontend().analyze(
      "structural-requires.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected structural requires diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "trailing structural requirements should admit an exact iterator, "
         "distinct sentinel, owner-tied std iterator, and numeric "
         "accumulator");

  const lang::ConceptDecl *sentinelConcept = nullptr;
  for (const lang::StmtPtr &declaration : valid.program.declarations()) {
    const auto *conceptDeclaration =
        dynamic_cast<const lang::ConceptDecl *>(declaration.get());
    if (conceptDeclaration != nullptr &&
        conceptDeclaration->name().lexeme == "compatible_sentinel") {
      sentinelConcept = conceptDeclaration;
      break;
    }
  }
  const lang::FunctionDecl *local =
      findTopLevelFunction(valid.program, "local_accumulate");
  const lang::RequiresClause *requiresClause =
      local != nullptr && local->requiresClause() ? &*local->requiresClause()
                                                  : nullptr;
  expect(sentinelConcept != nullptr &&
             sentinelConcept->typeParameters().size() == 2 &&
             sentinelConcept->requirements().size() == 1 &&
             sentinelConcept->requirements().front().arguments.size() == 2,
         "the AST should retain multi-parameter concept declarations and "
         "their relational argument mapping");
  expect(requiresClause != nullptr &&
             requiresClause->requirements().size() == 3 &&
             requiresClause->requirements()[0].name.last().lexeme ==
                 "input_iterator" &&
             requiresClause->requirements()[0].arguments.size() == 1 &&
             requiresClause->requirements()[1].name.last().lexeme ==
                 "compatible_sentinel" &&
             requiresClause->requirements()[1].arguments.size() == 2 &&
             requiresClause->requirements()[2].name.last().lexeme ==
                 "accumulates_into" &&
             requiresClause->requirements()[2].arguments.size() == 2,
         "the AST should preserve each trailing requires application and its "
         "type-parameter arguments in source order");

  bool foundLocalInstance = false;
  bool foundStandardInstance = false;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source == nullptr ||
        (instance.source->name().lexeme != "local_accumulate" &&
         instance.source->name().lexeme != "accumulate")) {
      continue;
    }
    const auto targetsOperator = [&](const lang::HirValue &value,
                                     lang::TokenKind token,
                                     lang::OverloadedOperator operation) {
      if (value.operation != token || !value.functionTarget ||
          *value.functionTarget == 0 ||
          *value.functionTarget > valid.hir.functionInstances().size()) {
        return false;
      }
      const lang::HirFunctionInstance &target =
          valid.hir.functionInstances()[*value.functionTarget - 1];
      return target.source != nullptr && target.source->operatorName() &&
             target.source->operatorName()->kind == operation;
    };
    const bool selectedComparison = std::any_of(
        instance.body.values.begin(), instance.body.values.end(),
        [&](const lang::HirValue &value) {
          return targetsOperator(value, lang::TokenKind::BANG_EQUAL,
                                 lang::OverloadedOperator::NotEqual);
        });
    const bool selectedDereference = std::any_of(
        instance.body.values.begin(), instance.body.values.end(),
        [&](const lang::HirValue &value) {
          return targetsOperator(value, lang::TokenKind::STAR,
                                 lang::OverloadedOperator::Dereference);
        });
    const bool selectedIncrement = std::any_of(
        instance.body.values.begin(), instance.body.values.end(),
        [&](const lang::HirValue &value) {
          return targetsOperator(value, lang::TokenKind::PLUS_PLUS,
                                 lang::OverloadedOperator::PreIncrement);
        });
    if (!selectedComparison || !selectedDereference || !selectedIncrement) {
      continue;
    }
    foundLocalInstance = foundLocalInstance ||
                         instance.source->name().lexeme == "local_accumulate";
    foundStandardInstance =
        foundStandardInstance || instance.source->name().lexeme == "accumulate";
  }
  expect(foundLocalInstance && foundStandardInstance,
         "concrete local and std::accumulate HIR instances should retain "
         "selected comparison, dereference, and increment targets");

  const lang::FrontendResult constrainedGenericClass =
      lang::Frontend().analyze("constrained-generic-class-method.gti", R"(
class NumericBox<T> {
public:
  T doubled(T value) requires std::numeric<T> {
    return T(value + value);
  }
};

int main() {
  NumericBox<int> box = NumericBox<int>();
  return box.doubled(2) - 4;
}
)",
                               {standardLibraryPrelude()});
  expect(constrainedGenericClass.canGenerateCode(),
         "an ordinary non-generic method of a generic class should be able "
         "to use an atomic trailing requirement as its symbolic proof");

  const lang::FrontendResult rejectedGenericClass =
      lang::Frontend().analyze("rejected-generic-class-method.gti", R"(
class NumericBox<T> {
public:
  T doubled(T value) requires std::numeric<T> {
    return T(value + value);
  }
};

int main() {
  NumericBox<bool> box = NumericBox<bool>();
  bool value = box.doubled(true);
  if (value) {
    return 0;
  }
  return 1;
}
)",
                               {standardLibraryPrelude()});
  expect(
      !rejectedGenericClass.canGenerateCode() &&
          hasDiagnosticCode(rejectedGenericClass.diagnostics, "GTI-S2029") &&
          hasDiagnostic(rejectedGenericClass.diagnostics,
                        "does not satisfy generic constraint 'std::numeric'"),
      "a single non-generic member overload must check requirements after "
      "substituting its generic class arguments");

  const lang::FrontendResult rejectedConstrainedOperator =
      lang::Frontend().analyze("rejected-constrained-operator.gti", R"(
class ConstrainedOperator<T> {
public:
  bool operator()(T value)
    requires std::numeric<T> {
    return true;
  }

  bool operator!=(ConstrainedOperator<T>& other)
    requires std::numeric<T> {
    return false;
  }
};
)",
                               {standardLibraryPrelude()});
  expect(!rejectedConstrainedOperator.canGenerateCode() &&
             hasDiagnosticCode(rejectedConstrainedOperator.diagnostics,
                               "GTI-S2049") &&
             hasDiagnostic(rejectedConstrainedOperator.diagnostics,
                           "not supported on operator declarations"),
         "bounded trailing requirements should reject operator declarations "
         "instead of creating recursively constrained operator proof");
  const lang::FrontendResult rejectedPolymorphicRequirements =
      lang::Frontend().analyze("rejected-polymorphic-requires.gti", R"(
class AbstractValues<T> {
public:
  virtual T concrete() requires std::numeric<T> { return T(0); }
  virtual T abstract() requires std::numeric<T> = 0;
};

interface ValueContract<T> {
  T value() requires std::numeric<T>;
};

interface PlainContract<T> {
  T inherited();
};

class DerivedValues<T> : public PlainContract<T> {
public:
  T inherited() requires std::numeric<T> override { return T(0); }
};
)",
                               {standardLibraryPrelude()});
  expect(!rejectedPolymorphicRequirements.canGenerateCode() &&
             countDiagnosticCode(rejectedPolymorphicRequirements.diagnostics,
                                 "GTI-S2049") >= 4 &&
             hasDiagnostic(rejectedPolymorphicRequirements.diagnostics,
                           "not supported on polymorphic methods"),
         "bounded trailing requirements should reject virtual, pure, and "
         "interface contracts whose dispatch cannot preserve them");

  const lang::FrontendResult unprovenForward = lang::Frontend().analyze(
      "unproven-structural-forward.gti", R"(
#include <std/numeric>
T invalid_forward<Iterator, Sentinel, std::numeric T>(
    mut Iterator first, Sentinel last, mut T init) {
  return std::accumulate(std::move(first), std::move(last), std::move(init));
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !unprovenForward.canGenerateCode() &&
          hasDiagnosticCode(unprovenForward.diagnostics, "GTI-S2029") &&
          hasDiagnostic(unprovenForward.diagnostics, "'std::input_iterator'"),
      "generic forwarding should require the outer declaration to prove "
      "the callee's exact structural contract");

  const std::string generated = gti_test::emitCppText(valid);
  expect(
      generated.find("concept compatible_sentinel") == std::string::npos &&
          generated.find("requires __gti_std::") == std::string::npos &&
          generated.find("__gti_operator_dereference") != std::string::npos &&
          generated.find("__gti_operator_pre_increment") != std::string::npos &&
          generated.find("__gti_operator_not_equal") != std::string::npos &&
          generated.find("// GTI verified-MIR body:") != std::string::npos,
      "the C++ backend should emit already-validated operator calls, not "
      "GTI concepts or trailing requires clauses");

  const lang::FrontendResult exactSentinelBridges = lang::Frontend().analyze(
      "exact-sentinel-bridges.gti", R"(
#include <std/numeric>
#include <std/iterator>

class BaseEnd {
};

class DerivedEnd : public BaseEnd {
  int limit;
public:
  DerivedEnd(int value) : limit(value) {}
  int position() { return this.limit; }
};

class OtherEnd {};

class GenericComparable<T, uint64_t N> {
public:
  bool operator!=(GenericComparable<T, N>& other) { return true; }
  bool differs(GenericComparable<T, N>& other) { return this != other; }
};

interface IteratorContract {
  int& operator*();
  void operator++() mut;
  bool operator!=(DerivedEnd& end);
};

class DynamicIterator : public IteratorContract {
  mut int current;
public:
  DynamicIterator(int value) : current(value) {}
  int& operator*() override { return this.current; }
  void operator++() mut override { this.current++; }
  bool operator!=(DerivedEnd& end) override {
    return this.current != end.position();
  }
};

int dynamic_step<Iterator, Sentinel>(mut Iterator& first, Sentinel& last)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> {
  ++first;
  if (first != last) { return *first; }
  return 0;
}

class BaseIterator {
  mut int current;
public:
  BaseIterator(int value) : current(value) {}
  int& operator*() { return this.current; }
  void operator++() mut { this.current++; }
  bool operator!=(DerivedEnd& end) {
    return this.current != end.position();
  }
};

class DerivedIterator : public BaseIterator {
public:
  DerivedIterator(int value) : BaseIterator(value) {}
  bool operator!=(BaseEnd& end) { return false; }
  bool operator!=(mut OtherEnd& end) { return true; }
};

int main() {
  mut DerivedIterator iterator = DerivedIterator(1);
  DerivedEnd end = DerivedEnd(4);
  bool direct = iterator != end;
  if (!direct) { return 1; }
  GenericComparable<int, 4> generic_left = GenericComparable<int, 4>();
  GenericComparable<int, 4> generic_right = GenericComparable<int, 4>();
  if (!generic_left.differs(generic_right)) { return 3; }
  mut DynamicIterator dynamic = DynamicIterator(1);
  mut IteratorContract& view = dynamic;
  if (dynamic_step(view, end) != 2) { return 2; }
  return std::accumulate(DerivedIterator(1), DerivedEnd(4), 0) - 6;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!exactSentinelBridges.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic :
         exactSentinelBridges.diagnostics) {
      std::cerr << "Unexpected exact sentinel bridge diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(exactSentinelBridges.canGenerateCode(),
         "an inherited exact sentinel operation should remain valid beside a "
         "derived convertible overload and mutable-sentinel overload");
  const std::string exactBridgeCpp =
      gti_test::emitCppText(exactSentinelBridges);
  expect(
      exactBridgeCpp.find("::gti_internal::backend::exact_type<const "
                          "::__gti_program::DerivedEnd &>") !=
              std::string::npos &&
          exactBridgeCpp.find("::gti_internal::backend::exact_type<const "
                              "::__gti_program::BaseEnd &>") !=
              std::string::npos &&
          exactBridgeCpp.find("::gti_internal::backend::exact_type<"
                              "::__gti_program::OtherEnd &>") ==
              std::string::npos &&
          exactBridgeCpp.find(
              "friend const std::int32_t & __gti_operator_dereference("
              "const IteratorContract &__gti_receiver)") != std::string::npos &&
          exactBridgeCpp.find(
              "_dynamic_step__gti_mir_failure("
              "::__gti_program::IteratorContract & __gti_mir_arg_0, "
              "const ::__gti_program::DerivedEnd & __gti_mir_arg_1") !=
              std::string_view::npos &&
          exactBridgeCpp.find(
              "exact_type<const ::__gti_program::__gti_type_") !=
              std::string::npos &&
          exactBridgeCpp.find("_GenericComparable &>") != std::string::npos &&
          exactBridgeCpp.find("_GenericComparable & __gti_argument_0") !=
              std::string::npos &&
          exactBridgeCpp.find("GenericComparable<void, 0>") ==
              std::string::npos,
      "sentinel bridges should tag exact read-only parameter types and "
      "must not expose mutable-sentinel overloads");

  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-structural-requires.gti", R"(
#include <std/iterator>
#include <std/numeric>

class End {
  int limit = 1;
public:
  int position() { return this.limit; }
};

class MissingDereference {
  mut int current = 0;
public:
  void operator++() mut { this.current++; }
  bool operator!=(End& sentinel) {
    return this.current != sentinel.position();
  }
};

class WrongIncrement {
  mut int current = 0;
public:
  int& operator*() { return this.current; }
  int operator++() mut { return this.current; }
  bool operator!=(End& sentinel) {
    return this.current != sentinel.position();
  }
};

class WrongSentinelParameter {
  mut int current = 0;
public:
  int& operator*() { return this.current; }
  void operator++() mut { this.current++; }
  bool operator!=(End sentinel) {
    return this.current != sentinel.position();
  }
};

class WrongElement {
  bool value = false;
  mut int current = 0;
public:
  bool& operator*() { return this.value; }
  void operator++() mut { this.current++; }
  bool operator!=(End& sentinel) {
    return this.current != sentinel.position();
  }
};

int main() {
  int missing = std::accumulate(MissingDereference(), End(), 0);
  int increment = std::accumulate(WrongIncrement(), End(), 0);
  int sentinel = std::accumulate(WrongSentinelParameter(), End(), 0);
  int element = std::accumulate(WrongElement(), End(), 0);
  return missing + increment + sentinel + element;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !invalid.canGenerateCode() &&
          hasDiagnosticCode(invalid.diagnostics, "GTI-S2029") &&
          hasDiagnostic(invalid.diagnostics, "concept 'std::input_iterator'") &&
          hasDiagnostic(invalid.diagnostics, "concept 'std::sentinel_for'") &&
          hasDiagnostic(invalid.diagnostics,
                        "concept 'std::accumulates_into'") &&
          hasDiagnostic(invalid.diagnostics,
                        "Prefix operator++ must return void") &&
          hasDiagnosticHint(invalid.diagnostics,
                            "public read-only operator*() returning") &&
          hasDiagnosticHint(invalid.diagnostics,
                            "public read-only bool operator!=") &&
          hasDiagnosticHint(invalid.diagnostics,
                            "referent is exactly the required type") &&
          hasRelatedDiagnostic(
              invalid.diagnostics,
              "Required here as 'std::input_iterator<Iterator>'"),
      "structural requirements should diagnose missing or inexact "
      "iterator, sentinel, and element operations before backend entry");

  const lang::FrontendResult missingIncrement = lang::Frontend().analyze(
      "missing-increment.gti", R"(
#include <std/numeric>

class End {
  int limit = 1;
public:
  int position() { return this.limit; }
};

class MissingIncrement {
  mut int current = 0;
public:
  int& operator*() { return this.current; }
  bool operator!=(End& sentinel) {
    return this.current != sentinel.position();
  }
};

int main() {
  return std::accumulate(MissingIncrement(), End(), 0);
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!missingIncrement.canGenerateCode() &&
             hasDiagnosticCode(missingIncrement.diagnostics, "GTI-S2029") &&
             hasDiagnostic(missingIncrement.diagnostics,
                           "concept 'std::input_iterator'") &&
             hasDiagnosticHint(missingIncrement.diagnostics,
                               "public void operator++() mut"),
         "input_iterator should reject an otherwise exact iterator that is "
         "missing prefix increment");

  const lang::FrontendResult mutableOnlyDereference = lang::Frontend().analyze(
      "mutable-only-dereference.gti", R"(
#include <std/numeric>

class End {
  int limit = 1;
public:
  int position() { return this.limit; }
};

class MutableOnlyDereference {
  mut int current = 0;
public:
  mut int& operator*() mut { return this.current; }
  void operator++() mut { this.current++; }
  bool operator!=(End& sentinel) {
    return this.current != sentinel.position();
  }
};

int main() {
  return std::accumulate(MutableOnlyDereference(), End(), 0);
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !mutableOnlyDereference.canGenerateCode() &&
          hasDiagnosticCode(mutableOnlyDereference.diagnostics, "GTI-S2029") &&
          hasDiagnostic(mutableOnlyDereference.diagnostics,
                        "concept 'std::input_iterator'"),
      "input_iterator and accumulates_into should require a read-only "
      "dereference even when the caller owns a mutable iterator value");

  const lang::FrontendResult wrongArity = lang::Frontend().analyze(
      "wrong-requires-arity.gti", R"(
#include <std/iterator>
T invalid<I, T>(I iterator, T value)
  requires std::input_iterator<I, T> {
  return value;
}
void invalid_pack<Args...>(Args... args)
  requires std::input_iterator<Args> {}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!wrongArity.canGenerateCode() &&
             hasDiagnosticCode(wrongArity.diagnostics, "GTI-S2049") &&
             hasDiagnostic(wrongArity.diagnostics,
                           "Concept 'std::input_iterator' requires 1 type "
                           "argument") &&
             hasDiagnostic(wrongArity.diagnostics, "unexpanded type pack"),
         "requires-clause concept applications should enforce exact arity "
         "and reject unexpanded type packs");

  const lang::FrontendResult duplicate = lang::Frontend().analyze(
      "duplicate-structural-overload.gti", R"(
#include <std/iterator>
#include <std/numeric>
T select<I, S, T>(I iterator, S sentinel, T value)
  requires std::input_iterator<I> && std::sentinel_for<S, I> {
  return value;
}
T select<I, S, T>(I iterator, S sentinel, T value)
  requires std::input_iterator<I> && std::accumulates_into<I, T> {
  return value;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!duplicate.canGenerateCode() &&
             hasDiagnostic(duplicate.diagnostics,
                           "Duplicate overload signature for 'select'"),
         "trailing requirements should validate candidates without "
         "distinguishing or ranking otherwise identical overloads");

  lang::Lexer lexer;
  lang::Parser malformed(lexer.scan(R"(
T broken<I>(I value)
  requires std::input_iterator<I> || std::input_iterator<I> {
  return value;
}
int okay() { return 0; }
)"));
  const lang::Program recovered = malformed.parse();
  const lang::FunctionDecl *broken = findTopLevelFunction(recovered, "broken");
  expect(
      !malformed.hadError() && broken != nullptr && broken->requiresClause() &&
          broken->requiresClause()->expression.root <
              broken->requiresClause()->expression.nodes.size() &&
          broken->requiresClause()
                  ->expression.nodes[broken->requiresClause()->expression.root]
                  .kind == lang::ConstraintExpressionKind::Disjunction &&
          findTopLevelFunction(recovered, "okay") != nullptr,
      "requires-clause disjunction should parse without losing the next "
      "declaration");

  const std::string requiresSource =
      "T sum<I,std::numeric T>(I first,T init)requires "
      "std::input_iterator<I>&&std::accumulates_into<I,T>{return init;}";
  const std::string formattedRequires =
      lang::Formatter().format(requiresSource);
  expect(formattedRequires.find("requires std::input_iterator<I> &&") !=
                 std::string::npos &&
             lang::Formatter().format(formattedRequires) == formattedRequires,
         "multi-parameter trailing requires syntax should format "
         "idempotently");
  expect(formattedRequires == R"(T sum<I, std::numeric T>(I first, T init)
  requires std::input_iterator<I> && std::accumulates_into<I, T> {
  return init;
}
)",
         "a trailing requires clause should start its own line one level past "
         "the declaration, matching the shipped standard-library style");

  lang::FormatOptions singleLineRequires;
  singleLineRequires.requiresClausePosition =
      lang::RequiresClausePosition::SingleLine;
  const std::string joinedRequires =
      lang::Formatter(singleLineRequires).format(requiresSource);
  expect(joinedRequires.find(") requires std::input_iterator<I>") !=
                 std::string::npos &&
             lang::Formatter(singleLineRequires).format(joinedRequires) ==
                 joinedRequires,
         "RequiresClausePosition SingleLine should keep the clause on the "
         "declaration line");

  const std::string bodylessRequires = lang::Formatter().format(
      "T probe<I,std::numeric T>(I first,T init)requires "
      "std::input_iterator<I>;\nint32_t after() { return 0; }\n");
  expect(bodylessRequires == R"(T probe<I, std::numeric T>(I first, T init)
  requires std::input_iterator<I>;
int32_t after() {
  return 0;
}
)",
         "a bodyless declaration should release the requires indent at its "
         "semicolon");

  const std::string requiresIdentifier =
      lang::Formatter().format("int32_t main(){int32_t requires=1;return "
                               "requires-1;}");
  expect(requiresIdentifier == R"(int32_t main() {
  int32_t requires = 1;
  return requires - 1;
}
)",
         "recovery-oriented formatting should not misindent a non-clause "
         "'requires' occurrence even though the compiler reserves it as a "
         "keyword");
}

void testCallableConceptCapabilities() {
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "callable-concepts.gti", R"(
#include <std/functional>

class reader {
public:
  bool operator()(int32_t& value) { return value == 7; }
};

class counter {
  mut int32_t calls;

public:
  counter() : calls(0) {}
  bool operator()(int32_t& value) mut {
    this.calls++;
    return value == 7;
  }
};

class once_reader {
public:
  bool operator()(int32_t& value) && { return value == 7; }
};

class descending {
public:
  bool operator()(int32_t& left, int32_t& right) {
    return left > right;
  }
};

class once_action {
public:
  void operator()() && {}
};

bool read_value<Function>(int32_t value, Function function)
  requires std::read_callable<Function, int32_t, bool> {
  return function(value);
}

bool mutate_value<Function>(int32_t value, mut Function function)
  requires std::predicate<Function, int32_t> {
  return function(value);
}

bool consume_value<Function>(int32_t value, Function function)
  requires std::once_callable<Function, int32_t, bool> {
  return std::move(function)(value);
}

bool order_values<Function>(int32_t left, int32_t right,
                            mut Function function)
  requires std::comparator<Function, int32_t> {
  return function(left, right);
}

int main() {
  bool read = read_value(7, reader());
  bool mutable_result = mutate_value(7, counter());
  bool consumed = consume_value(7, once_reader());
  bool ordered = order_values(8, 7, descending());
  std::invoke_once(once_action());
  return read && mutable_result && consumed && ordered ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected callable-concept diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "public callable concepts should establish exact read, repeatable "
         "mutable, consuming, predicate, comparator, and once-action "
         "contracts before concrete generic reanalysis");

  const auto rejects = [&](std::string_view name, const std::string &source,
                           std::string_view conceptName,
                           std::string_view hintFragment,
                           std::string_view message) {
    const lang::FrontendResult result = lang::Frontend().analyze(
        std::string(name), source, {standardLibraryPrelude()}, {},
        {standardLibraryRoot()});
    if (result.canGenerateCode() ||
        !hasDiagnosticCode(result.diagnostics, "GTI-S2029")) {
      for (const lang::Diagnostic &diagnostic : result.diagnostics) {
        std::cerr << name << " [" << diagnostic.code
                  << "]: " << diagnostic.message << '\n';
      }
    }
    expect(!result.canGenerateCode() &&
               hasDiagnostic(result.diagnostics, std::string("concept '") +
                                                     std::string(conceptName) +
                                                     "'") &&
               hasDiagnosticHint(result.diagnostics, std::string(hintFragment)),
           std::string(message));
  };

  rejects("mutable-target-at-read-site.gti", R"(
#include <std/functional>
class mutable_only {
public:
  bool operator()(int32_t& value) mut { return value > 0; }
};
bool inspect<Function>(int32_t value, Function function)
  requires std::read_callable<Function, int32_t, bool> {
  return function(value);
}
int main() { return inspect(1, mutable_only()) ? 0 : 1; }
)",
          "std::read_callable", "receiver capability",
          "a mutable-only target should not satisfy a read-only callable "
          "use site");

  rejects("once-target-at-repeatable-site.gti", R"(
#include <std/functional>
class once_only {
public:
  bool operator()(int32_t& value) && { return value > 0; }
};
bool inspect<Function>(int32_t value, mut Function function)
  requires std::predicate<Function, int32_t> {
  return function(value);
}
int main() { return inspect(1, once_only()) ? 0 : 1; }
)",
          "std::predicate", "receiver capability",
          "a consuming target should not satisfy a repeatable predicate use "
          "site");

  rejects("wrong-predicate-result.gti", R"(
#include <std/functional>
class integer_result {
public:
  int32_t operator()(int32_t& value) { return value; }
};
bool inspect<Function>(int32_t value, mut Function function)
  requires std::predicate<Function, int32_t> {
  return function(value);
}
int main() { return inspect(1, integer_result()) ? 0 : 1; }
)",
          "std::predicate", "declared owned result",
          "a predicate should require an exact owned bool result");

  rejects("borrowed-operation-result.gti", R"(
#include <std/functional>
class borrowed_result {
public:
  int32_t& operator()(int32_t& value) { return value; }
};
int32_t transform<Function>(int32_t value, mut Function function)
  requires std::operation<Function, int32_t, int32_t> {
  return function(value);
}
int main() { return transform(1, borrowed_result()) - 1; }
)",
          "std::operation", "declared owned result",
          "an operation should not satisfy an owned-result relation with a "
          "borrowed result");

  rejects("ambiguous-callable-relation.gti", R"(
#include <std/functional>
class ambiguous {
public:
  bool operator()(int32_t value) { return value > 0; }
  bool operator()(int32_t& value) { return value > 0; }
};
bool inspect<Function>(int32_t value, Function function)
  requires std::read_callable<Function, int32_t, bool> {
  return function(value);
}
int main() { return inspect(1, ambiguous()) ? 0 : 1; }
)",
          "std::read_callable", "one exact public unary operator()",
          "a callable relation should reject an ambiguous exact readable "
          "signature");

  const lang::FrontendResult privateBinding = lang::Frontend().analyze(
      "private-callable-intrinsic.gti", R"(
#include <std/functional>

concept forged<F, A, R> =
    gti_internal::read_callable_capability<F, A, R>;
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!privateBinding.canGenerateCode() &&
             hasDiagnosticCode(privateBinding.diagnostics, "GTI-S2058"),
         "compiler-owned callable relations should remain private behind the "
         "public std concepts");
}

void testSymbolicAutoLocalBinding() {
  const std::string source = R"(
#include <std/iterator>
#include <std/vector>

bool first_less_than_next<Iterator, Sentinel>(mut Iterator first, Sentinel last)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> &&
           std::copy_constructible<Iterator> {
  if (first != last) {
    auto value = *first;
    ++first;
    if (first != last) {
      return value < *first;
    }
  }
  return false;
}

int main() {
  mut std::vector<int64_t> wide = std::vector<int64_t>();
  wide.push_back(int64_t(2));
  wide.push_back(int64_t(9));
  mut std::vector<int32_t> narrow = std::vector<int32_t>();
  narrow.push_back(3);
  narrow.push_back(8);
  bool wide_result = first_less_than_next(wide.begin(), wide.end());
  bool narrow_result = first_less_than_next(narrow.begin(), narrow.end());
  return wide_result && narrow_result ? 0 : 1;
}
)";
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "symbolic-auto-local.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected symbolic auto local diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "a constrained symbolic dereference should infer an exact auto-local "
         "type independently in each concrete generic instance");

  const lang::SourceUnitId unit = valid.sourceGraph.entryUnit();
  const lang::SymbolRecord *autoLocal = nullptr;
  for (const lang::SymbolRecord &symbol :
       valid.semantics.database().symbols()) {
    if (symbol.kind == lang::SymbolKind::LocalVariable &&
        symbol.name == "value" && symbol.sourceUnit == unit) {
      autoLocal = &symbol;
      break;
    }
  }
  expect(autoLocal != nullptr &&
             autoLocal->type == lang::SemanticType::Unknown &&
             autoLocal->instanceDependentType,
         "the shared generic declaration symbol should remain explicitly "
         "instance-dependent instead of taking one specialization's type");

  bool foundInt32 = false;
  bool foundInt64 = false;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "first_less_than_next") {
      continue;
    }
    for (const lang::HirBinding &binding : instance.body.bindings) {
      if (binding.variable == nullptr ||
          binding.variable->name().lexeme != "value") {
        continue;
      }
      foundInt32 = foundInt32 || binding.info.type == lang::SemanticType::Int32;
      foundInt64 = foundInt64 || binding.info.type == lang::SemanticType::Int64;
    }
  }
  expect(foundInt32 && foundInt64,
         "concrete HIR instances should own their distinct inferred local "
         "types regardless of specialization order");

  const lang::LoweredProgramBuild lowered = lang::LoweredProgramBuilder().build(
      valid.program, valid.semantics, valid.hir, valid.mir, valid.mir, {},
      lang::TargetInfo::host());
  const lang::LoweredSymbol *loweredAuto =
      !lowered.program || autoLocal == nullptr
          ? nullptr
          : lowered.program->findSymbol(autoLocal->id);
  expect(lowered.valid() && loweredAuto != nullptr &&
             loweredAuto->type == lang::SemanticType::Unknown &&
             loweredAuto->instanceDependentType,
         "LoweredProgram should retain the dependent declaration marker while "
         "concrete bodies retain exact types");

  const std::size_t autoKeyword = source.find("auto value");
  const std::size_t valueName = source.find("value", autoKeyword);
  const lang::LanguageQueries queries;
  const std::optional<lang::HoverInfo> autoHover =
      queries.hover(valid, unit, autoKeyword + 1);
  const std::optional<lang::HoverInfo> valueHover =
      queries.hover(valid, unit, valueName + 1);
  expect(autoHover && autoHover->signature == "auto" &&
             !autoHover->notes.empty() && valueHover &&
             valueHover->signature == "auto value" &&
             !valueHover->notes.empty(),
         "tooling should describe a symbolic auto local as instance-dependent, "
         "not as whichever specialization was analyzed first");

  const lang::FrontendResult uninstantiated = lang::Frontend().analyze(
      "uninstantiated-symbolic-auto-local.gti", R"(
#include <std/iterator>

bool first_less_than_next<Iterator, Sentinel>(mut Iterator first, Sentinel last)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> &&
           std::copy_constructible<Iterator> {
  if (first != last) {
    auto value = *first;
    ++first;
    if (first != last) {
      return value < *first;
    }
  }
  return false;
}

int main() { return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const lang::LoweredProgramBuild uninstantiatedLowered =
      lang::LoweredProgramBuilder().build(
          uninstantiated.program, uninstantiated.semantics, uninstantiated.hir,
          uninstantiated.mir, uninstantiated.mir, {}, lang::TargetInfo::host());
  expect(uninstantiated.canGenerateCode() && uninstantiatedLowered.valid(),
         "an uninstantiated valid generic primary should not need a concrete "
         "specialization to make its dependent local symbol valid");
}

void testPackSpecialMemberConstruction() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/pack_special_member_construction.gti";
  const auto result = lang::Frontend().analyze(fixture, std::nullopt,
                                               {standardLibraryPrelude()}, {},
                                               {standardLibraryRoot()});
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "exact generated special members must consume concrete pack elements "
         "without pretending to invoke an ordinary constructor");
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return;
  }
  expect(!gti_test::emitCppText(result).empty(),
         "special-member pack construction must emit from verified MIR");
  std::size_t copies = 0;
  std::size_t moves = 0;
  for (const auto &function : result.mir.functionInstances()) {
    const auto *hir = result.hir.findFunctionInstance(function.id);
    if (hir == nullptr || hir->source == nullptr ||
        (hir->source->name().lexeme != "construct" &&
         hir->source->name().lexeme != "copies"))
      continue;
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.operands.size() != 1 ||
            (instruction.kind != lang::MirInstructionKind::Move &&
             instruction.kind != lang::MirInstructionKind::Load))
          continue;
        const auto *place =
            function.body.findPlace(instruction.operands.front().place);
        if (place == nullptr || place->projections.size() != 1 ||
            place->projections.front().kind !=
                lang::MirProjectionKind::PackElement)
          continue;
        const bool moved = instruction.kind == lang::MirInstructionKind::Move;
        copies += !moved;
        moves += moved;
        expect(instruction.callSite == 0 && !instruction.constructorTarget &&
                   instruction.info.type == place->type &&
                   instruction.operands.front().kind ==
                       (moved ? lang::MirOperandKind::Move
                              : lang::MirOperandKind::Copy),
               "generated pack construction is an exact existing value "
               "copy/move, not a staged ordinary call");
        if (!moved)
          continue;
        expect(instruction.lifecycle.size() == 1 &&
                   instruction.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Move &&
                   instruction.lifecycle.front().source != 0 &&
                   instruction.lifecycle.front().target != 0,
               "owning special-member pack movement must transfer the exact "
               "entry element into its returned value lifetime");
        auto missingTransfer = function.body;
        missingTransfer.blocks[block.id - 1]
            .instructions[&instruction - block.instructions.data()]
            .lifecycle.clear();
        expect(!lang::verifyMirBody(missingTransfer, function.id).valid(),
               "removing the owning pack-to-value transfer must be rejected");
        auto wrongSource = function.body;
        auto &forged =
            wrongSource.blocks[block.id - 1]
                .instructions[&instruction - block.instructions.data()];
        forged.operands.front().kind = lang::MirOperandKind::Copy;
        expect(!lang::verifyMirBody(wrongSource, function.id).valid(),
               "a pack move cannot be forged into a copy of a move-only owner");
      }
    }
  }
  expect(copies == 2 && moves == 1,
         "copyable packs remain reusable while an owning pack moves once");

  const auto trivial =
      lang::Frontend().analyze("cleanup-free-pack-special.gti", R"(
class Item {
  int number;
public:
  Item(int initial) : number(initial) {}
  Item(Item& other) = delete;
  Item(Item&& other) = default;
  int value() { return this.number; }
};
T construct<T, Args...>(Args... args) { return T(args...); }
int main() { Item item = construct<Item>(Item(7)); return item.value() - 7; }
)",
                               {standardLibraryPrelude()});
  expect(trivial.canGenerateCode() && !gti_test::emitCppText(trivial).empty(),
         "cleanup-free pack movement must not require an active result drop");

  const auto reused = lang::Frontend().analyze("reused-special-pack.gti", R"(
#include <std/string>
void twice<T, Args...>(Args... args) {
  T first = T(args...);
  T second = T(args...);
}
int main() { twice<std::string>(std::string("owned")); return 0; }
)",
                                               {standardLibraryPrelude()}, {},
                                               {standardLibraryRoot()});
  expect(!reused.canGenerateCode() &&
             hasDiagnostic(reused.diagnostics,
                           "Value 'args' has already been moved") &&
             hasRelatedDiagnostic(reused.diagnostics,
                                  "Concrete generic instance requested here"),
         "special construction must not bypass whole-pack use-after-move "
         "diagnostics or their concrete-instance context");
}

void testConstructorLocalTypeGenerics() {
  const auto borrowedFixture = std::filesystem::path(__FILE__).parent_path() /
                               "fixtures/forward_list_emplacement_borrowed.gti";
  const auto borrowed = lang::Frontend().analyze(borrowedFixture, std::nullopt,
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  expect(borrowed.canGenerateCode() &&
             lang::verifyMirProgram(borrowed.mir).valid(),
         "forwarding an owned pack element to a read-only constructor "
         "parameter must retain the selected signature in MIR");
  std::size_t borrowedPackInputs = 0;
  for (const auto &constructor : borrowed.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputKind != lang::HirCallInputKind::ReadBorrow ||
            instruction.operands.size() != 1)
          continue;
        const auto &operand = instruction.operands.front();
        const auto *place = constructor.body.findPlace(operand.place);
        if (place == nullptr || place->projections.size() != 1 ||
            place->projections.front().kind !=
                lang::MirProjectionKind::PackElement)
          continue;
        ++borrowedPackInputs;
        expect(operand.kind == lang::MirOperandKind::BorrowRead &&
                   operand.type.kind == lang::SemanticType::Reference &&
                   operand.type.arguments.size() == 1 &&
                   operand.type.arguments.front() == place->type &&
                   instruction.info.type == operand.type &&
                   !instruction.preparedParameterDrop,
               "a borrowed pack element must not become an owned parameter");
        for (const auto &candidateBlock : constructor.body.blocks) {
          for (const auto &candidate : candidateBlock.instructions) {
            expect(candidate.kind != lang::MirInstructionKind::Move ||
                       std::none_of(candidate.operands.begin(),
                                    candidate.operands.end(),
                                    [&](const auto &input) {
                                      return input.place == place->id;
                                    }),
                   "read-only pack forwarding must not move its referent");
          }
        }
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto malformed = borrowed.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          if (mutation == 0) {
            for (auto &changedBlock : changed->body.blocks)
              for (auto &stage : changedBlock.instructions)
                if (stage.id == instruction.id)
                  stage.callInputKind = lang::HirCallInputKind::MoveValue;
          } else {
            for (auto &changedPlace : changed->body.places)
              if (changedPlace.id == place->id)
                changedPlace.projections.front().constantIndex = 999;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "pack-reference roles and element bounds must verify "
                 "independently of lowering");
        }
      }
    }
  }
  expect(borrowedPackInputs == 1,
         "the borrowed emplacement fixture must exercise pack projection");
  const lang::FrontendResult inPlace = lang::Frontend().analyze(
      std::filesystem::temp_directory_path() /
          "gti-constructor-pack-in-place.gti",
      R"(
#include <std/memory>
enum class InPlaceTag { value };
class Immovable {
public:
  int value;
  Immovable(int initial) : value(initial) {}
  Immovable(Immovable& other) = delete;
  Immovable(Immovable&& other) = delete;
};
struct Node<T> {
  T value;
  Node<Args...>(InPlaceTag tag, Args... args) : value(T(args...)) {}
};
int main() {
  auto node = std::make_unique<Node<Immovable>>(InPlaceTag::value, 42);
  auto number = std::make_unique<Node<int>>(InPlaceTag::value, 42);
  auto flag = std::make_unique<Node<bool>>(InPlaceTag::value, true);
  auto character = std::make_unique<Node<char>>(InPlaceTag::value, 'x');
  auto zero = std::make_unique<Node<int>>(InPlaceTag::value);
  return node->value.value - 42;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!inPlace.canGenerateCode()) {
    for (const auto &diagnostic : inPlace.diagnostics) {
      std::cerr << diagnostic.message << '\n';
    }
    std::cerr << lang::MirPrinter().print(inPlace.mir);
  }
  expect(inPlace.canGenerateCode() &&
             lang::verifyMirProgram(inPlace.mir).valid(),
         "constructor packs should construct an immovable field in place");
  std::size_t packBindings = 0;
  std::size_t emptyPacks = 0;
  for (const auto &constructor : inPlace.mir.constructorInstances()) {
    if (constructor.parameterBindings.size() != 2)
      continue;
    const auto pack = std::find_if(
        constructor.body.places.begin(), constructor.body.places.end(),
        [&](const auto &place) {
          return place.root == lang::MirPlaceRootKind::Binding &&
                 place.binding == constructor.parameterBindings.back() &&
                 place.projections.empty() &&
                 place.type.kind == lang::SemanticType::TypePack;
        });
    if (pack == constructor.body.places.end())
      continue;
    ++packBindings;
    for (int mutation = 0; mutation != 2; ++mutation) {
      auto malformed = inPlace.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      if (mutation == 0) {
        changed->parameterBindings.back() = changed->parameterBindings.front();
      } else {
        for (auto &place : changed->body.places) {
          if (place.id == pack->id)
            place.type.concretePack = false;
        }
      }
      expect(!lang::verifyMirProgram(malformed).valid(),
             "constructor packs reject duplicate bindings and symbolic packs");
    }
    if (pack->type.arguments.empty()) {
      ++emptyPacks;
      auto malformed = inPlace.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      std::swap(changed->parameterBindings.front(),
                changed->parameterBindings.back());
      expect(!lang::verifyMirProgram(malformed).valid(),
             "an empty pack cannot precede a fixed constructor binding even "
             "when flattened parameter types are unchanged");
    }
  }
  expect(packBindings == 5 && emptyPacks == 1,
         "all concrete node constructor packs need exact binding metadata");
  const std::string source = R"(
#include <std/string>
#include <std/memory>

class Capture {
public:
  Capture<T>(T value) {}
};

void discard_constructor_pack<Args...>(Args... args) {}
class PackedCapture {
public:
  PackedCapture<Args...>(int fixed, Args... args) {
    discard_constructor_pack(args...);
  }
};

class Tagged<Stored> {
  Stored stored;

public:
  Tagged<Tag>(Stored value, Tag tag) : stored(value) {}
  Stored read() { return this.stored; }
};

int main() {
  Capture integer = Capture(1);
  Capture boolean = Capture(true);
  PackedCapture empty_pack = PackedCapture(0);
  PackedCapture two_pack = PackedCapture(0, true, 2);
  auto allocated_pack = std::make_unique<PackedCapture>(0, 'a', false);
  Tagged<int> tagged = Tagged<int>(4, true);
  mut std::string source = std::string("hello");
  mut std::string copy = std::string(source.begin(), source.end());
  source.push_back('!');
  if (copy == "hello" and source == "hello!" and tagged.read() == 4) {
    return 0;
  }
  return 1;
}
)";

  const std::filesystem::path entry = std::filesystem::temp_directory_path() /
                                      "gti-constructor-local-generics/main.gti";
  const lang::FrontendResult valid = lang::Frontend().analyze(
      entry, source, {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected constructor-local generic diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && valid.mir.valid() &&
             lang::verifyMirProgram(valid.mir).valid(),
         "constructor-local type arguments should infer exactly, satisfy "
         "declared requirements, and lower through verified MIR");
  if (!valid.canGenerateCode())
    return;
  bool checkedBorrowedPreparation = false;
  for (const auto &function : valid.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &input : block.instructions) {
        if (input.kind != lang::MirInstructionKind::CallInput ||
            input.callInputKind !=
                lang::HirCallInputKind::MoveBorrowedTemporary)
          continue;
        checkedBorrowedPreparation = true;
        expect(input.preparedParameterDrop && input.lifecycle.size() == 1 &&
                   input.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Reparent,
               "borrowed iterator temporaries retain prepared ownership until "
               "construction");
        if (!input.preparedParameterDrop)
          continue;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = valid.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          for (auto &changedBlock : body->blocks) {
            for (auto &changed : changedBlock.instructions) {
              if (changed.id == input.id) {
                if (mutation == 0)
                  changed.lifecycle.clear();
                if (mutation == 1)
                  changed.preparedParameterDrop.reset();
              }
              if (mutation == 2 &&
                  changed.kind == lang::MirInstructionKind::Construct)
                std::erase_if(changed.lifecycle, [&](const auto &event) {
                  return event.kind ==
                             lang::MirLifecycleEventKind::TransferOut &&
                         event.source == *input.preparedParameterDrop;
                });
            }
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "borrowed preparation rejects lost reparent, obligation or "
                 "invocation handoff");
        }
      }
    }
  }
  expect(checkedBorrowedPreparation,
         "constructor iterator exercises prepared borrowed temporary");

  const lang::ClassDecl *capture = findTopLevelClass(valid.program, "Capture");
  const lang::ConstructorDecl *captureConstructor = nullptr;
  if (capture != nullptr) {
    for (const lang::StmtPtr &member : capture->members()) {
      if (const auto *constructor =
              dynamic_cast<const lang::ConstructorDecl *>(member.get())) {
        captureConstructor = constructor;
        break;
      }
    }
  }
  expect(captureConstructor != nullptr &&
             captureConstructor->genericParameters().size() == 1 &&
             !captureConstructor->requiresClause(),
         "the AST should scope a constructor-local type parameter to its "
         "constructor declaration");

  const lang::ConstructorInfo *captureInfo =
      captureConstructor == nullptr
          ? nullptr
          : valid.semantics.findConstructor(*captureConstructor);
  expect(captureInfo != nullptr && captureInfo->genericParameters.size() == 1 &&
             captureInfo->parameterTypes.size() == 1 &&
             captureInfo->parameterTypes.front().kind ==
                 lang::SemanticType::TypeParameter,
         "symbolic constructor metadata should retain its own generic "
         "parameter identity and parameter pattern");

  const lang::HirConstructorInstance *integerInstance = nullptr;
  const lang::HirConstructorInstance *booleanInstance = nullptr;
  const lang::HirConstructorInstance *taggedInstance = nullptr;
  const lang::HirConstructorInstance *stringRangeInstance = nullptr;
  for (const lang::HirConstructorInstance &instance :
       valid.hir.constructorInstances()) {
    if (instance.source == nullptr) {
      continue;
    }
    if (instance.source->name().lexeme == "Capture" &&
        instance.typeArguments.size() == 1 &&
        instance.parameterTypes.size() == 1) {
      if (instance.typeArguments.front() == lang::SemanticType::Int32) {
        integerInstance = &instance;
      } else if (instance.typeArguments.front() == lang::SemanticType::Bool) {
        booleanInstance = &instance;
      }
    } else if (instance.source->name().lexeme == "Tagged" &&
               instance.typeArguments.size() == 1 &&
               instance.typeArguments.front() == lang::SemanticType::Bool) {
      taggedInstance = &instance;
    } else if (instance.source->name().lexeme == "string" &&
               instance.typeArguments.size() == 2 &&
               instance.parameterTypes.size() == 2) {
      stringRangeInstance = &instance;
    }
  }
  expect(integerInstance != nullptr && booleanInstance != nullptr &&
             integerInstance->id != booleanInstance->id &&
             integerInstance->declaration == booleanInstance->declaration &&
             integerInstance->parameterTypes.front() ==
                 lang::SemanticType::Int32 &&
             booleanInstance->parameterTypes.front() ==
                 lang::SemanticType::Bool,
         "different inferred constructor-local types should produce stable, "
         "distinct concrete constructor identities");
  expect(taggedInstance != nullptr && stringRangeInstance != nullptr &&
             stringRangeInstance->typeArguments[0] !=
                 stringRangeInstance->typeArguments[1],
         "generic owners and iterator/sentinel construction should preserve "
         "independent owner and constructor-local arguments in HIR");

  bool mirroredInteger = false;
  bool mirroredBoolean = false;
  bool mirroredRange = false;
  for (const lang::MirConstructorInstance &instance :
       valid.mir.constructorInstances()) {
    if (integerInstance != nullptr && instance.id == integerInstance->id) {
      mirroredInteger =
          instance.typeArguments == integerInstance->typeArguments &&
          instance.valueArguments == integerInstance->valueArguments &&
          instance.parameterTypes == integerInstance->parameterTypes;
    }
    if (booleanInstance != nullptr && instance.id == booleanInstance->id) {
      mirroredBoolean =
          instance.typeArguments == booleanInstance->typeArguments &&
          instance.valueArguments == booleanInstance->valueArguments &&
          instance.parameterTypes == booleanInstance->parameterTypes;
    }
    if (stringRangeInstance != nullptr &&
        instance.id == stringRangeInstance->id) {
      mirroredRange =
          instance.typeArguments == stringRangeInstance->typeArguments &&
          instance.valueArguments == stringRangeInstance->valueArguments &&
          instance.parameterTypes == stringRangeInstance->parameterTypes;
    }
  }
  expect(mirroredInteger && mirroredBoolean && mirroredRange,
         "MIR constructor instances should retain the concrete generic "
         "arguments and parameter types selected by each HIR constructor "
         "identity");

  const lang::LoweredProgram loweredProgram =
      gti_test::lowerProgram(valid, valid.mir, valid.mir);
  const std::string lowered =
      lang::LoweredProgramPrinter().print(loweredProgram);
  expect(lowered.find("constructor(id=") != std::string::npos &&
             lowered.find(";generics=[generic(") != std::string::npos &&
             lowered.find(";requirements=[{concept=") != std::string::npos &&
             lowered.find(";requires=1;") != std::string::npos,
         "the lowered declaration contract should retain constructor "
         "generics and their symbolic requirements");

  const std::string mirText = lang::MirPrinter().print(valid.mir);
  expect(mirText.find("move-borrowed-temporary") != std::string::npos,
         "verified MIR should make the move of owner-tied iterator "
         "temporaries into constructor parameters explicit");

  const std::string generated = gti_test::emitCppText(valid);
  // Private backend support may use C++ representation constraints. The
  // source constructor contract itself must remain resolved by GTI.
  const std::size_t emittedProgram =
      generated.find("namespace __gti_program {");
  expect(
      generated.find("explicit Capture(std::int32_t __gti_mir_arg_0);") !=
              std::string::npos &&
          generated.find("explicit Capture(bool __gti_mir_arg_0);") !=
              std::string::npos &&
          concreteBackendSymbols(generated, "__gti_type_", "Tagged").size() ==
              1 &&
          generated.find("Capture(T value)") == std::string::npos &&
          generated.find("string(Iterator first, Sentinel last)") ==
              std::string::npos &&
          emittedProgram != std::string::npos &&
          generated.find("requires std::", emittedProgram) == std::string::npos,
      "the backend should emit each selected constructor as an ordinary "
      "concrete overload without delegating GTI inference or constraints to "
      "C++");

  const auto analyze = [&](std::string_view name, std::string text) {
    return lang::Frontend().analyze(std::filesystem::path(name),
                                    std::move(text), {standardLibraryPrelude()},
                                    {}, {standardLibraryRoot()});
  };
  const lang::FrontendResult missingInference =
      analyze("constructor-missing-inference.gti", R"(
class Wrapper<T> {};
class Missing {
public:
  Missing<T>(Wrapper<T> value) {}
};
int main() {
  Missing value = Missing(1);
  return 0;
}
)");
  expect(!missingInference.canGenerateCode() &&
             hasDiagnosticCode(missingInference.diagnostics, "GTI-S2023") &&
             hasDiagnostic(missingInference.diagnostics,
                           "Cannot infer constructor-local type parameter "
                           "'T'") &&
             hasRelatedDiagnostic(missingInference.diagnostics,
                                  "Constructor-local generic parameter "
                                  "declared here") &&
             hasDiagnosticHint(missingInference.diagnostics, "inference-only"),
         "a structurally unmatched parameter pattern should report focused "
         "constructor-local inference failure");

  const lang::FrontendResult conflictingInference =
      analyze("constructor-conflicting-inference.gti", R"(
class Conflict {
public:
  Conflict<T>(T left, T right) {}
};
int main() {
  Conflict value = Conflict(1, true);
  return 0;
}
)");
  expect(!conflictingInference.canGenerateCode() &&
             hasDiagnosticCode(conflictingInference.diagnostics, "GTI-S2023") &&
             hasDiagnostic(conflictingInference.diagnostics,
                           "Conflicting types were inferred for "
                           "constructor-local type parameter 'T'"),
         "repeated constructor parameter patterns should require one exact "
         "canonical inferred type");

  const lang::FrontendResult failedConstraint =
      analyze("constructor-failed-constraint.gti", R"(
class IntegralOnly {
public:
  IntegralOnly<std::integral T>(T value) {}
};
int main() {
  IntegralOnly value = IntegralOnly(true);
  return 0;
}
)");
  expect(!failedConstraint.canGenerateCode() &&
             hasDiagnosticCode(failedConstraint.diagnostics, "GTI-S2029") &&
             hasDiagnostic(failedConstraint.diagnostics,
                           "does not satisfy generic constraint "
                           "'std::integral'"),
         "constructor constraints should be evaluated after exact argument "
         "inference and before candidate admission");

  const lang::FrontendResult escapedParameter =
      analyze("constructor-local-parameter-scope.gti", R"(
class Scoped {
public:
  Scoped<T>(T value) {}
  T leak;
};
int main() { return 0; }
)");
  expect(!escapedParameter.canGenerateCode() &&
             hasDiagnostic(escapedParameter.diagnostics, "Unknown type 'T'"),
         "a constructor-local type parameter should not be visible to later "
         "class members");

  const lang::FrontendResult invalidDeclarations =
      analyze("constructor-local-generic-declarations.gti", R"(
class Owner<T> {
public:
  Owner<T>(int value) {}
};
class Packed {
public:
  Packed<Args...>(Args value) {}
};
class Uninferable {
public:
  Uninferable<T>(int value) {}
};
int main() { return 0; }
)");
  expect(!invalidDeclarations.canGenerateCode() &&
             hasDiagnostic(invalidDeclarations.diagnostics,
                           "Constructor-local generic parameters cannot "
                           "shadow") &&
             hasDiagnostic(invalidDeclarations.diagnostics,
                           "A generic type pack must be bound by a final "
                           "parameter pack") &&
             hasDiagnostic(invalidDeclarations.diagnostics,
                           "must occur in a constructor parameter type"),
         "constructor-local parameters should retain the bounded shadowing, "
         "pack, and inference-only declaration rules");

  const lang::FrontendResult wrongElement =
      analyze("string-range-wrong-element.gti", R"(
#include <std/string>
#include <std/vector>
int main() {
  mut std::vector<int> values = std::vector<int>();
  std::string text = std::string(values.begin(), values.end());
  return 0;
}
)");
  expect(!wrongElement.canGenerateCode() &&
             hasDiagnosticCode(wrongElement.diagnostics, "GTI-S2029") &&
             hasDiagnostic(wrongElement.diagnostics, "std::dereferences_to") &&
             hasRelatedDiagnostic(wrongElement.diagnostics,
                                  "std::dereferences_to<Iterator, char>"),
         "the standard string constructor should reject an iterator whose "
         "read-only element referent is not exactly char");

  const lang::FrontendResult closedRequirement =
      analyze("constructor-closed-requirement.gti", R"(
#include <std/iterator>
class CharacterIterator {
  char value = 'x';
public:
  char& operator*() { return this.value; }
};
class CharacterOnly {
public:
  CharacterOnly<Iterator>(Iterator value)
    requires std::dereferences_to<Iterator, char> {}
};
int main() {
  CharacterOnly value = CharacterOnly(CharacterIterator());
  return 0;
}
)");
  expect(closedRequirement.canGenerateCode(),
         "a constructor requirement should accept a closed exact type "
         "argument without making it part of inference");

  const lang::FrontendResult dependentRequirement =
      analyze("constructor-dependent-requirement.gti", R"(
class Wrapper<T> {};
concept copyable_wrapper<T> = std::copyable<T>;
class Invalid {
public:
  Invalid<T>(T value) requires copyable_wrapper<Wrapper<T>> {}
};
int main() { return 0; }
)");
  expect(dependentRequirement.canGenerateCode(),
         "dependent compound requirements should preserve their exact type "
         "identity without participating in generic deduction");

  const std::string formatted = lang::Formatter().format(
      "class Box{int value;public:Box<std::integral T>(T input)requires "
      "std::copyable<T>:value(int(input)){};};");
  expect(formatted.find("Box<std::integral T>(T input)\n"
                        "    requires std::copyable<T> : value(int(input)) "
                        "{};") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "constructor requirements should format idempotently and end before "
         "an initializer-list colon");
}

void testDependentTypeDefaultConstraints() {
  const std::string declarations = R"(
class Ordered<std::totally_ordered T> {};
class NotOrdered {};
class Holder<T, Compare = Ordered<T>> {};
class Nested<T, Compare = Holder<T>> {};
class Constrained<T, std::integral U = T> {};
)";
  const auto analyze = [&](const std::string &suffix) {
    return lang::Frontend().analyze(
        "dependent-type-defaults.gti", declarations + suffix,
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  };
  const auto valid = analyze(R"(
int main() {
  Holder<int> ordinary{};
  Holder<NotOrdered, int> customized{};
  Nested<int> nested{};
  Nested<NotOrdered, int> nested_customized{};
  Constrained<int> constrained{};
  Constrained<double, int> constrained_customized{};
  return 0;
}
)");
  expect(valid.canGenerateCode(),
         "dependent defaults should defer unresolved constraints and an "
         "explicit argument should bypass an unused default");
  for (const std::string &suffix :
       {"int main() { Holder<NotOrdered> value{}; return 0; }",
        "int main() { Nested<NotOrdered> value{}; return 0; }",
        "int main() { Constrained<double> value{}; return 0; }",
        "void consume(Holder<NotOrdered>& value) {}",
        "class Bad<T, Compare = Ordered<double*>> {};"}) {
    const auto invalid = analyze(suffix);
    expect(!invalid.canGenerateCode() &&
               hasDiagnosticCode(invalid.diagnostics, "GTI-S2029"),
           "selected dependent defaults and invalid closed defaults must "
           "retain frontend constraint diagnostics: " +
               suffix);
  }
  const auto ordinaryUse =
      analyze("class Unconstrained<T> { Ordered<T> value; };\n");
  expect(!ordinaryUse.canGenerateCode() &&
             hasDiagnosticCode(ordinaryUse.diagnostics, "GTI-S2029"),
         "default-only deferral must not weaken ordinary symbolic class "
         "argument constraints");
  const auto functionDefault = analyze(R"(
int choose<T, C = Ordered<T>>(T& value) { return 7; }
int main() { NotOrdered value{}; return choose<NotOrdered, int>(value) - 7; }
)");
  expect(functionDefault.canGenerateCode(),
         "explicit function generic arguments bypass a dependent default");
  const auto badFunctionDefault = analyze(R"(
int choose<T, C = Ordered<T>>(T& value) { return 7; }
int main() { NotOrdered value{}; return choose(value); }
)");
  expect(!badFunctionDefault.canGenerateCode() &&
             hasDiagnosticCode(badFunctionDefault.diagnostics, "GTI-S2029"),
         "a selected function default validates nested constraints before HIR");
}

void testValueGenerics() {
  const std::string source = R"(
constexpr uint64_t default_array_base = 2;
constexpr uint64_t default_array_size = default_array_base + 1;

class StaticArray<T, uint64_t N> {
  T values[N] = {};

public:
  uint64_t size() { return N; }
  T first() { return this.values[0]; }
};

class WrappedArray<T, uint64_t N> {
  StaticArray<T, N> value = StaticArray<T, N>();

public:
  uint64_t size() { return this.value.size(); }
};

class DefaultArray<T = int, uint64_t N = default_array_size> {
  T values[N] = {};

public:
  uint64_t size() { return N; }
};

int main() {
  StaticArray<int, 4> four = StaticArray<int, 4>();
  StaticArray<int, 2 + 2> arithmetic = StaticArray<int, 2 + 2>();
  StaticArray<int, 8> eight = StaticArray<int, 8>();
  WrappedArray<int, 4> wrapped = WrappedArray<int, 4>();
  DefaultArray defaulted = DefaultArray();
  if (four.size() == 4 and arithmetic.size() == 4 and eight.size() == 8 and
      wrapped.size() == 4 and four.first() == 0 and defaulted.size() == 3) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult valid =
      lang::Frontend().analyze("value-generics.gti", source);
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected value generic diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "uint64_t value parameters should support class identity and array "
         "extents");

  bool foundFour = false;
  bool foundEight = false;
  bool foundDefaultThree = false;
  for (const lang::HirClassInstance &instance : valid.hir.classInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "DefaultArray" &&
        instance.typeArguments.size() == 1 &&
        instance.typeArguments.front() == lang::SemanticType::Int32 &&
        instance.valueArguments.size() == 1 &&
        instance.valueArguments.front().kind ==
            lang::CompileTimeValue::UInt64 &&
        instance.valueArguments.front().value == 3) {
      foundDefaultThree = true;
    }
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "StaticArray" ||
        instance.valueArguments.size() != 1 || instance.fields.empty()) {
      continue;
    }
    const lang::CompileTimeValue length = instance.valueArguments.front();
    const lang::SemanticType &storage = instance.fields.front().info.type;
    if (length.kind == lang::CompileTimeValue::UInt64 &&
        storage.kind == lang::SemanticType::Array &&
        storage.arrayLengthParameterId == 0 &&
        storage.arrayLength == length.value) {
      foundFour = foundFour || length.value == 4;
      foundEight = foundEight || length.value == 8;
    }
  }
  expect(foundFour && foundEight && foundDefaultThree,
         "HIR should keep value arguments in class identity and substitute "
         "symbolic array extents, including declaration-owned defaults");

  const std::size_t namedDefault =
      source.find("N = default_array_size") + std::string("N = ").size();
  const lang::SourceUnitId valueGenericsUnit = valid.sourceGraph.entryUnit();
  const lang::SemanticOccurrence *namedDefaultOccurrence =
      valid.semantics.database().findOccurrence(valueGenericsUnit,
                                                namedDefault + 1);
  const lang::SymbolRecord *namedDefaultSymbol =
      valid.semantics.database().findSymbolAt(valueGenericsUnit,
                                              namedDefault + 1);
  expect(namedDefaultOccurrence != nullptr &&
             lang::hasRole(namedDefaultOccurrence->roles,
                           lang::OccurrenceRole::Reference) &&
             lang::hasRole(namedDefaultOccurrence->roles,
                           lang::OccurrenceRole::Read) &&
             namedDefaultSymbol != nullptr &&
             namedDefaultSymbol->kind == lang::SymbolKind::GlobalVariable &&
             namedDefaultSymbol->name == "default_array_size",
         "an early namespace constexpr generic default should retain an exact "
         "tooling reference to its declaration after registration-time "
         "normalization");

  const std::string generated = gti_test::emitCppText(valid);
  expect(
      concreteBackendSymbols(generated, "__gti_type_", "StaticArray").size() ==
              2 &&
          concreteBackendSymbols(generated, "__gti_type_", "WrappedArray")
                  .size() == 1 &&
          concreteBackendSymbols(generated, "__gti_type_", "DefaultArray")
                  .size() == 1 &&
          generated.find("std::array<std::int32_t, 4> values") !=
              std::string::npos &&
          generated.find("std::array<std::int32_t, 8> values") !=
              std::string::npos &&
          generated.find(
              "template <typename T, std::size_t N>\n  class StaticArray") ==
              std::string::npos &&
          generated.find("typename T =") == std::string::npos &&
          generated.find("template <typename T, std::size_t N =") ==
              std::string::npos,
      "the C++ backend should give each GTI-selected mixed type/value "
      "application an ordinary concrete class and should not rely on "
      "native template selection or defaults");

  lang::Lexer lexer;
  lang::Parser invalidParser(lexer.scan(R"(
class WrongType<T, int N> {};
class WrongOrder<uint64_t N, T> {};
class NeedsValue<T, uint64_t N> {};
class ImmutableValue<uint64_t N> {
public:
  void overwrite() { N = 2; }
};
uint64_t invalid_function<uint64_t N>() { return N; }
T same_default<T>(T value) { return value; }
T same_default<T = int>(T value) { return value; }
NeedsValue<int, int> wrong_value;
NeedsValue<4, 4> wrong_type;
NeedsValue<int, Missing> missing_value;
int invalid_extent[Missing] = {};
class MissingAfterDefault<T = int, U> {};
class ForwardDefault<T = U, U = int> {};
class ForwardValue<uint64_t N = M, uint64_t M = 2> {};
class BadValueDefault<uint64_t N = 1 / 0> {};
class BadValueUnderflow<uint64_t N = 1 - 2> {};
class BadValueOverflow<uint64_t N = 18446744073709551615 + 1> {};
class BadValueName<uint64_t N = Missing> {};
void default_pack<T = int, Args...>() {}
class ConstructorDefault {
public:
  ConstructorDefault<T = int>(T value) {}
};
)"));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid value generic programs should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid value generic declarations and arguments should fail");
  expect(hasDiagnostic(invalidSemantic, "currently require type uint64_t") &&
             hasDiagnostic(invalidSemantic,
                           "type parameters must appear before value") &&
             hasDiagnostic(invalidSemantic,
                           "requires a uint64_t compile-time value") &&
             hasDiagnostic(invalidSemantic, "requires a type argument") &&
             hasDiagnostic(invalidSemantic,
                           "Cannot assign to compile-time value parameter") &&
             hasDiagnostic(invalidSemantic,
                           "is not an in-scope constexpr integer or uint64_t "
                           "value generic") &&
             hasDiagnostic(invalidSemantic,
                           "without a default cannot follow a defaulted") &&
             hasDiagnostic(invalidSemantic,
                           "may reference only visible types and earlier") &&
             hasDiagnostic(invalidSemantic,
                           "may reference only earlier value parameters") &&
             hasDiagnostic(invalidSemantic,
                           "cannot divide or take modulo by zero") &&
             hasDiagnostic(invalidSemantic, "underflows uint64_t") &&
             hasDiagnostic(invalidSemantic, "overflows uint64_t") &&
             hasDiagnostic(invalidSemantic, "must use uint64_t literals") &&
             hasDiagnostic(invalidSemantic,
                           "parameter list that contains a type pack") &&
             hasDiagnostic(invalidSemantic,
                           "Constructor-local generic parameters are "
                           "inference-only") &&
             hasDiagnostic(invalidSemantic,
                           "Duplicate overload signature for 'same_default'"),
         "value generic diagnostics should identify unsupported parameter "
         "types, ordering, scope, defaults, and argument kind without "
         "requiring every value to be inferable");

  const std::string invalidDefaultsSource =
      "T constrained<std::integral T = double>() { return T(); }\n"
      "uint64_t selected<uint64_t N>() { return N; }\n"
      "uint64_t dependent_underflow<uint64_t N, uint64_t M = N - 2>() { "
      "return M; }\n"
      "int main() { return int(selected<1 / 0>() + "
      "dependent_underflow<1>()); }\n";
  const lang::FrontendResult invalidDefaults = lang::Frontend().analyze(
      "invalid-generic-defaults.gti", invalidDefaultsSource,
      {standardLibraryPrelude()});
  const auto invalidExpression = std::find_if(
      invalidDefaults.diagnostics.begin(), invalidDefaults.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2026" &&
               diagnostic.message.find(
                   "Function value generic argument arithmetic cannot") !=
                   std::string::npos;
      });
  const auto dependentUnderflow = std::find_if(
      invalidDefaults.diagnostics.begin(), invalidDefaults.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2026" &&
               diagnostic.message.find(
                   "underflows uint64_t for the selected") != std::string::npos;
      });
  expect(!invalidDefaults.canGenerateCode() &&
             hasDiagnostic(invalidDefaults.diagnostics,
                           "does not satisfy generic constraint") &&
             hasDiagnostic(invalidDefaults.diagnostics,
                           "Function value generic argument arithmetic "
                           "cannot divide or take modulo by zero") &&
             invalidExpression != invalidDefaults.diagnostics.end() &&
             invalidExpression->primary.source.ends_with(
                 "/invalid-generic-defaults.gti") &&
             invalidExpression->primary.start ==
                 invalidDefaultsSource.find('/') &&
             dependentUnderflow != invalidDefaults.diagnostics.end() &&
             dependentUnderflow->primary.start ==
                 invalidDefaultsSource.find("N - 2") + 2,
         "invalid constrained defaults and closed value expressions should "
         "remain precisely anchored GTI diagnostics rather than native "
         "substitution failures");

  const lang::FrontendResult lateDefault = lang::Frontend().analyze(
      "late-generic-default.gti", "class LateDefault<uint64_t N = later> {};\n"
                                  "constexpr uint64_t later = 3;\n");
  expect(!lateDefault.canGenerateCode() &&
             hasDiagnostic(lateDefault.diagnostics,
                           "must use uint64_t literals, visible uint64_t "
                           "constexpr bindings, or earlier value parameters"),
         "generic defaults should not see a namespace constexpr binding "
         "declared later in source order");

  const std::string formatted = lang::Formatter().format(
      "class StaticArray<T,uint64_t N>{T values[N]={};};StaticArray<int,4> "
      "values=StaticArray<int,4>();T choose<T=int,uint64_t N=2+1>(){return "
      "T();}");
  expect(formatted.find("class StaticArray<T, uint64_t N> {") !=
                 std::string::npos &&
             formatted.find("StaticArray<int, 4> values =") !=
                 std::string::npos &&
             formatted.find("T choose<T = int, uint64_t N = 2 + 1>() {") !=
                 std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "value generic defaults and dependent expressions should format "
         "idempotently");

  lang::Parser recoveryParser(
      lexer.scan("T broken<T = >(); int recovered() { return 0; }"));
  lang::Program recovered = recoveryParser.parse();
  expect(recoveryParser.hadError() &&
             findTopLevelFunction(recovered, "recovered") != nullptr,
         "a malformed generic default should recover at the declaration "
         "boundary and preserve the following function");

  lang::Parser typeArgumentParser(lexer.scan(R"(
class Widget {};
T identity<T>(T value) { return value; }
void accept_type_arguments(Widget* pointer) {
  identity<Widget*>(pointer);
}
class CallbackHolder<T> {};
void accept_callback(CallbackHolder<(int)->void> callback) {}
)"));
  (void)typeArgumentParser.parse();
  if (typeArgumentParser.hadError()) {
    for (const lang::ParseDiagnostic &diagnostic :
         typeArgumentParser.errors()) {
      std::cerr << "Unexpected generic type-argument parser diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(!typeArgumentParser.hadError(),
         "dependent value arithmetic lookahead should preserve pointer and "
         "native-function generic type arguments");
}

void testContextualArrayArguments() {
  const std::string source = R"(
int total<uint64_t N>(int values[N]) {
  mut int result = 0;
  for (mut uint64_t index = 0; index < N; index++) {
    result += values[index];
  }
  return result;
}

T first<T, uint64_t N>(T fallback, T values[N]) { return values[0]; }

constexpr uint64_t exact_extent<uint64_t N>() { return N; }

int relay_total<uint64_t N>(int values[N]) { return total<N>(values); }

int choose(int values[2]) { return 2; }
int choose(int values[3]) { return 3; }

class Total {
  int stored;

public:
  Total<uint64_t N>(int values[N]) : stored(total(values)) {}
  int read() { return this.stored; }
  int add<uint64_t N>(int values[N]) { return this.stored + total(values); }
};

int main() {
  int summed = total({1, 2, 3});
  int selected = first(0, {4, 5});
  int extent = choose({6, 7, 8});
  Total object = Total({9, 10});
  int method = object.add({2, 3});
  int named_values[2] = {11, 12};
  int named = total(named_values);
  int explicit_total = total<3>({3, 4, 5});
  int explicit_mixed = first<int, 2>(0, {6, 7});
  int relayed = relay_total<2>({8, 9});
  uint64_t explicit_extent = exact_extent<4>();
  int explicit_method = object.add<2>({1, 1});
  return summed + selected + extent + object.read() + method + named +
         explicit_total + explicit_mixed + relayed + int(explicit_extent) +
         explicit_method - 139;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("contextual-arrays.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected contextual-array diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mir.valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "brace call arguments should form exact contextual fixed-array "
         "values through semantics, HIR, and MIR");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto *summed = main == nullptr || main->body()->statements().empty()
                           ? nullptr
                           : dynamic_cast<const lang::VariableDecl *>(
                                 main->body()->statements().front().get());
  const auto *sumCall =
      summed == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(summed->initializer().get());
  const auto *sumInitializer =
      sumCall == nullptr || sumCall->arguments().empty()
          ? nullptr
          : dynamic_cast<const lang::ArrayInitializer *>(
                sumCall->arguments().front().get());
  const lang::ResolvedCallInfo *resolvedSum =
      sumCall == nullptr ? nullptr : frontend.semantics.findCall(*sumCall);
  expect(sumInitializer != nullptr && sumInitializer->elements().size() == 3 &&
             resolvedSum != nullptr &&
             resolvedSum->valueArguments.size() == 1 &&
             resolvedSum->valueArguments.front().kind ==
                 lang::CompileTimeValue::UInt64 &&
             resolvedSum->valueArguments.front().value == 3 &&
             resolvedSum->parameterTypes.size() == 1 &&
             resolvedSum->parameterTypes.front().kind ==
                 lang::SemanticType::Array &&
             resolvedSum->parameterTypes.front().arrayLength == 3,
         "the AST and semantic call record should retain the brace elements "
         "and inferred exact extent");

  const auto *object = main == nullptr || main->body()->statements().size() < 4
                           ? nullptr
                           : dynamic_cast<const lang::VariableDecl *>(
                                 main->body()->statements()[3].get());
  const auto *objectCall =
      object == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(object->initializer().get());
  const lang::ResolvedConstructionInfo *resolvedObject =
      objectCall == nullptr ? nullptr
                            : frontend.semantics.findConstruction(*objectCall);
  expect(resolvedObject != nullptr &&
             resolvedObject->valueArguments.size() == 1 &&
             resolvedObject->valueArguments.front().kind ==
                 lang::CompileTimeValue::UInt64 &&
             resolvedObject->valueArguments.front().value == 2 &&
             resolvedObject->parameterTypes.size() == 1 &&
             resolvedObject->parameterTypes.front().kind ==
                 lang::SemanticType::Array &&
             resolvedObject->parameterTypes.front().arrayLength == 2,
         "generic constructors should infer and retain a brace argument's "
         "fixed-array extent");

  const lang::FunctionDecl *total =
      findTopLevelFunction(frontend.program, "total");
  bool foundTotalThree = false;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != total || instance.valueArguments.size() != 1) {
      continue;
    }
    foundTotalThree =
        foundTotalThree ||
        (instance.valueArguments.front().kind ==
             lang::CompileTimeValue::UInt64 &&
         instance.valueArguments.front().value == 3 &&
         instance.parameterTypes.size() == 1 &&
         instance.parameterTypes.front().kind == lang::SemanticType::Array &&
         instance.parameterTypes.front().arrayLength == 3);
  }

  bool foundConstructorTwo = false;
  for (const lang::HirConstructorInstance &instance :
       frontend.hir.constructorInstances()) {
    foundConstructorTwo =
        foundConstructorTwo ||
        (instance.source != nullptr &&
         instance.source->name().lexeme == "Total" &&
         instance.valueArguments.size() == 1 &&
         instance.valueArguments.front().kind ==
             lang::CompileTimeValue::UInt64 &&
         instance.valueArguments.front().value == 2 &&
         instance.parameterTypes.size() == 1 &&
         instance.parameterTypes.front().kind == lang::SemanticType::Array &&
         instance.parameterTypes.front().arrayLength == 2);
  }
  bool foundMethodTwo = false;
  bool foundExtentFour = false;
  bool foundRelayTwo = false;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    foundMethodTwo =
        foundMethodTwo ||
        (instance.source != nullptr &&
         instance.source->name().lexeme == "add" &&
         instance.valueArguments.size() == 1 &&
         instance.valueArguments.front().kind ==
             lang::CompileTimeValue::UInt64 &&
         instance.valueArguments.front().value == 2 &&
         instance.parameterTypes.size() == 1 &&
         instance.parameterTypes.front().kind == lang::SemanticType::Array &&
         instance.parameterTypes.front().arrayLength == 2);
    foundExtentFour =
        foundExtentFour || (instance.source != nullptr &&
                            instance.source->name().lexeme == "exact_extent" &&
                            instance.valueArguments.size() == 1 &&
                            instance.valueArguments.front().kind ==
                                lang::CompileTimeValue::UInt64 &&
                            instance.valueArguments.front().value == 4 &&
                            instance.parameterTypes.empty());
    foundRelayTwo =
        foundRelayTwo ||
        (instance.source != nullptr &&
         instance.source->name().lexeme == "relay_total" &&
         instance.valueArguments.size() == 1 &&
         instance.valueArguments.front().kind ==
             lang::CompileTimeValue::UInt64 &&
         instance.valueArguments.front().value == 2 &&
         instance.parameterTypes.size() == 1 &&
         instance.parameterTypes.front().kind == lang::SemanticType::Array &&
         instance.parameterTypes.front().arrayLength == 2);
  }
  expect(foundTotalThree && foundConstructorTwo && foundMethodTwo &&
             foundExtentFour && foundRelayTwo,
         "HIR instance identity should include inferred and explicit function "
         "and constructor value arguments, including methods and symbolic "
         "forwarding");

  std::size_t mirroredValueFunctions = 0;
  bool exactMirIdentity = true;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == nullptr || instance.valueArguments.empty()) {
      continue;
    }
    const std::string_view name = instance.source->name().lexeme;
    if (name != "total" && name != "add" && name != "exact_extent" &&
        name != "relay_total") {
      continue;
    }
    const lang::MirFunctionInstance *mir =
        frontend.mir.findFunctionInstance(instance.id);
    exactMirIdentity = exactMirIdentity && mir != nullptr &&
                       mir->typeArguments == instance.typeArguments &&
                       mir->valueArguments == instance.valueArguments;
    ++mirroredValueFunctions;
  }
  expect(exactMirIdentity && mirroredValueFunctions >= 4,
         "MIR function identity should preserve every concrete HIR type and "
         "value argument rather than reconstructing it from parameter types");

  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "LoweredProgram should retain and verify the exact MIR function "
         "value-generic identity");

  lang::MirProgram forged = frontend.mir;
  auto &forgedFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      forged.functionInstances());
  const auto forgedValue =
      std::find_if(forgedFunctions.begin(), forgedFunctions.end(),
                   [](const lang::MirFunctionInstance &instance) {
                     return !instance.valueArguments.empty() &&
                            instance.valueArguments.front().kind ==
                                lang::CompileTimeValue::UInt64;
                   });
  if (forgedValue != forgedFunctions.end()) {
    ++forgedValue->valueArguments.front().value;
  }
  const lang::LoweredProgramBuild rejected =
      lang::LoweredProgramBuilder().build(frontend.program, frontend.semantics,
                                          frontend.hir, forged, forged, {},
                                          lang::TargetInfo::host());
  expect(forgedValue != forgedFunctions.end() && !rejected.valid(),
         "the HIR-to-MIR boundary should reject a forged function value "
         "argument before backend planning");

  const std::string generated = gti_test::emitCppText(frontend);
  expect(generated.find("std::initializer_list") == std::string::npos &&
             generated.find("template <std::size_t N>\n  bool __gti_fn_") ==
                 std::string::npos &&
             generated.find("std::array<std::int32_t, 3>{__gti_mir_v_") !=
                 std::string::npos &&
             generated.find("std::array<std::int32_t, 2>{__gti_mir_v_") !=
                 std::string::npos &&
             containsMangledFunction(
                 generated, "", "total",
                 "__gti_mir_failure(std::array<std::int32_t, 3>"),
         "the backend should materialize an explicit std::array value and "
         "never delegate list semantics to C++ overload resolution");

  const std::string invalidSource = R"(
int scalar(int value) { return value; }
int fixed(int values[2]) { return values[0]; }
int ambiguous(int values[2]) { return 1; }
int ambiguous(int64_t values[2]) { return 2; }
int duplicate<uint64_t N>(int values[N]) { return 1; }
int duplicate<uint64_t M>(int values[M]) { return 2; }
int uninferable<uint64_t N>() { return int(N); }
int explicit_extent<uint64_t N>(int values[N]) { return values[0]; }
T no_common_type<T, uint64_t N>(T values[N]) { return values[0]; }
T type_only<T>(T value) { return value; }
int two_extents<uint64_t N, uint64_t M>(int first[N], int second[M]) {
  return first[0] + second[0];
}

class DuplicateConstructor {
public:
  DuplicateConstructor<uint64_t N>(int values[N]) {}
  DuplicateConstructor<uint64_t M>(int values[M]) {}
};

class ShadowedConstructor<uint64_t N> {
public:
  ShadowedConstructor<uint64_t N>(int values[N]) {}
};

int main() {
  int no_context = scalar({1});
  int wrong_count = fixed({1, 2, 3});
  int wrong_element = fixed({1, true});
  int unresolved = uninferable();
  int no_common = no_common_type({1, 2});
  int explicit_value = explicit_extent<2>({1, 2});
  int wrong_kind = type_only<2>(1);
  int missing_type_before_value = no_common_type<2>({1, 2});
  int wrong_value_count = two_extents<2>({1, 2}, {3, 4});
  int value_then_type = no_common_type<2, int>({1, 2});
  int non_generic_value = scalar<2>(1);
  int ambiguous_value = ambiguous({1, 2});
  return 0;
}
)";
  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-contextual-arrays.gti", invalidSource);
  expect(
      !invalid.canGenerateCode() &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2011") == 2 &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot infer generic type parameter 'T'") &&
          hasDiagnostic(invalid.diagnostics,
                        "wrong number of type arguments") &&
          hasDiagnostic(invalid.diagnostics,
                        "wrong number of explicit value arguments") &&
          hasDiagnostic(invalid.diagnostics,
                        "A function value generic argument must be an "
                        "in-scope uint64_t compile-time value") &&
          hasDiagnostic(
              invalid.diagnostics,
              "Non-generic functions do not take generic arguments") &&
          hasDiagnostic(invalid.diagnostics,
                        "Constructor-local generic parameters cannot shadow") &&
          hasDiagnostic(invalid.diagnostics,
                        "A brace argument requires one exact fixed-array") &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot initialize array element") &&
          hasDiagnostic(invalid.diagnostics, "is ambiguous"),
      "contextual arrays and explicit value arguments should reject duplicate "
      "generic shapes, missing array context, invalid argument kinds/counts, "
      "and ambiguous overloads at the GTI layer");

  // Resolution knows that the written 'int' occupies a value slot. Retain
  // PR111's owning value-kind diagnostic rather than the old syntactic-order
  // message; accepting the program or moving its anchor to the whole call
  // would still fail this regression.
  const auto wrongValueKind = std::find_if(
      invalid.diagnostics.begin(), invalid.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2026" &&
               diagnostic.message ==
                   "A function value generic argument must be an in-scope "
                   "uint64_t compile-time value.";
      });
  const auto valueKindOffset = invalidSource.find("int>({1, 2})");
  expect(wrongValueKind != invalid.diagnostics.end() &&
             valueKindOffset != std::string::npos &&
             wrongValueKind->primary.source.ends_with(
                 "invalid-contextual-arrays.gti") &&
             wrongValueKind->primary.start == valueKindOffset &&
             wrongValueKind->primary.end == valueKindOffset + 3,
         "a type in an explicit value slot has the exact owning diagnostic "
         "and written-argument span");

  const auto wrongExtent =
      std::find_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2015" &&
                            diagnostic.message.find("provides 3 elements") !=
                                std::string::npos;
                   });
  expect(wrongExtent != invalid.diagnostics.end() &&
             wrongExtent->primary.source.ends_with(
                 "/invalid-contextual-arrays.gti") &&
             wrongExtent->primary.start == invalidSource.find("{1, 2, 3}") &&
             wrongExtent->primary.end == wrongExtent->primary.start + 1 &&
             wrongExtent->fixes.empty(),
         "a contextual-array extent diagnostic should point at the written "
         "brace and avoid an unsafe mechanical conversion");

  const std::string formatted = lang::Formatter().format(
      "int total<uint64_t N>(int values[N]){return values[0];}int "
      "main(){return total<3>({1,2,3});}");
  expect(formatted.find("total<3>({1, 2, 3})") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "contextual brace arguments and explicit value generics should "
         "format idempotently");
}

void testVariadicGenerics() {
  const std::string source = R"(
class Packed<T...> {};
void consume<Args...>(Args... values) {}

void route<Rest...>(int first, Rest... rest) { consume(rest...); }
void route<Rest...>(bool first, Rest... rest) { consume(rest...); }

void relay<Args...>(Args... values) {
  consume(values...);
  consume(0, values...);
  route(1, values...);
}

T first<T, Rest...>(T value, Rest... rest) {
  relay(rest...);
  return value;
}

class Forwarder {
public:
  void send<Args...>(Args... values) {
    relay(values...);
  }
};

int main() {
  Packed<int, bool> metadata{};
  consume();
  relay(1, true, "gti");
  int inferred = first(7, false, "tail");
  int explicit_types = first<int, std::string_view>(9, "tail");
  Forwarder forwarder = Forwarder();
  forwarder.send(uint64_t(3), "method");
  if (inferred == 7 and explicit_types == 9) { return 0; }
  return 1;
}
)";

  lang::FrontendResult valid = lang::Frontend().analyze(
      "variadic-generics.gti", source, {standardLibraryPrelude()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected variadic diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "final function type packs should infer, forward, and preserve exact "
         "concrete MIR parameter roles across equivalent pack groupings");

  const std::string generated = gti_test::emitCppText(valid);
  expect(containsMangledFunction(
             generated, "", "consume",
             "__gti_mir_failure(std::int32_t __gti_mir_arg_0, bool "
             "__gti_mir_arg_1, std::string_view __gti_mir_arg_2"),
         "variadic declarations and forwarding should lower explicitly");
  expect(containsMangledFunction(
             generated, "", "consume",
             "__gti_mir_failure(std::int32_t __gti_mir_arg_0, "
             "std::int32_t __gti_mir_arg_1, bool __gti_mir_arg_2, "
             "std::string_view __gti_mir_arg_3"),
         "a final symbolic pack should compose with concrete pack elements");
  expect(generated.find("Rest... __gti_mir_arg_1") == std::string::npos &&
             containsMangledFunction(
                 generated, "", "first",
                 "__gti_mir_failure(std::int32_t __gti_mir_arg_0, bool "
                 "__gti_mir_arg_1, std::string_view __gti_mir_arg_2"),
         "fixed generic arguments and explicit pack elements should lower in "
         "source order");

  const lang::FrontendResult movePack = lang::Frontend().analyze(
      "move-pack.gti", R"(
#include <std/memory>

class PackedValue {};

void consume_owner<Args...>(Args... values) {}

void forward_owner<Args...>(Args... values) {
  consume_owner(values...);
}

int main() {
  auto owner = std::make_unique<PackedValue>();
  forward_owner(std::move(owner));
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  bool foundConcreteOwnerPack = false;
  for (const lang::HirFunctionInstance &instance :
       movePack.hir.functionInstances()) {
    foundConcreteOwnerPack =
        foundConcreteOwnerPack ||
        (instance.source != nullptr &&
         instance.source->name().lexeme == "forward_owner" &&
         instance.typeArguments.size() == 1 &&
         instance.typeArguments.front().kind == lang::SemanticType::Class);
  }
  expect(movePack.canGenerateCode() && foundConcreteOwnerPack,
         "a concrete move-only pack should be forwarded once and retained in "
         "typed HIR");

  const lang::FrontendResult reusedMovePack = lang::Frontend().analyze(
      "reused-move-pack.gti", R"(
#include <std/memory>

class PackedValue {};

void consume_owner<Args...>(Args... values) {}

void forward_owner_twice<Args...>(Args... values) {
  consume_owner(values...);
  consume_owner(values...);
}

int main() {
  auto owner = std::make_unique<PackedValue>();
  forward_owner_twice(std::move(owner));
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!reusedMovePack.canGenerateCode() &&
             hasDiagnostic(reusedMovePack.diagnostics,
                           "Value 'values' has already been moved") &&
             hasRelatedDiagnostic(reusedMovePack.diagnostics,
                                  "Concrete generic instance requested here"),
         "a move-only parameter pack should be consumed as a whole and cannot "
         "be forwarded twice");

  lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-variadic-generics.gti", R"(
void fixed(int value) {}
void nonfinal_generic<Args..., T>(Args... values, T tail) {}
void missing_parameter<Args...>() {}
void wrong_type<Args...>(int... values) {}
void mutable_pack<Args...>(mut Args... values) {}
void reference_pack<Args...>(Args&... values) {}
void direct_use<Args...>(Args... values) { values; }
void local_pack<Args...>(Args... values) { Args local; }
void bad_forward<Args...>(Args... values) { fixed(values...); }
void not_a_pack(int value) { consume(value...); }
)");
  expect(!invalid.canGenerateCode(),
         "invalid variadic declarations and expansions should be rejected");
  expect(
      hasDiagnostic(invalid.diagnostics,
                    "generic type pack must be the final") &&
          hasDiagnostic(invalid.diagnostics,
                        "must be bound by a final parameter pack") &&
          hasDiagnostic(invalid.diagnostics, "parameter pack type must name") &&
          hasDiagnostic(invalid.diagnostics, "Parameter packs are immutable") &&
          hasDiagnostic(invalid.diagnostics, "only by-value parameter packs") &&
          hasDiagnostic(invalid.diagnostics,
                        "must be expanded as the final call argument") &&
          hasDiagnostic(
              invalid.diagnostics,
              "can only appear in its matching final parameter-pack") &&
          hasDiagnostic(invalid.diagnostics,
                        "only be forwarded to another variadic") &&
          hasDiagnostic(invalid.diagnostics, "is not a parameter pack"),
      "variadic diagnostics should explain every confined first-wave rule");

  lang::Lexer lexer;
  lang::Parser malformed(lexer.scan(R"(
void consume<Args...>(Args... values) {}
void bad<Args...>(Args... values) { consume(values..., 1); }
int okay = 1;
)",
                                    "malformed-variadic.gti"));
  const lang::Program recovered = malformed.parse();
  const bool recoveredOkay = std::any_of(
      recovered.declarations().begin(), recovered.declarations().end(),
      [](const lang::StmtPtr &declaration) {
        const auto *variable =
            dynamic_cast<const lang::VariableDecl *>(declaration.get());
        return variable != nullptr && variable->name().lexeme == "okay";
      });
  expect(malformed.hadError() && recoveredOkay,
         "parser recovery should continue after a non-final pack expansion");

  const std::string formatted = lang::Formatter().format(
      "void relay<Args...>(Args... values){consume(values...);}");
  expect(formatted == "void relay<Args...>(Args... values) {\n"
                      "  consume(values...);\n"
                      "}\n" &&
             lang::Formatter().format(formatted) == formatted,
         "formatter should preserve compact, idempotent pack syntax");

  const std::string foldSource = R"(
void touch<T>(mut int& count, T& value) { count++; }
void other<T>(mut int& count, T& value) { count += 10; }

int visit_all<Values...>(Values... values) {
  mut int count = 0;
  (touch(count, values), ...);
  return count;
}

int main() {
  mut int count = 0;
  int value = 1;
  other(count, value);
  return visit_all(1, true, uint64_t(3)) - 3;
}
)";
  const lang::FrontendResult foldFrontend =
      lang::Frontend().analyze("pack-fold.gti", foldSource);
  if (!foldFrontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : foldFrontend.diagnostics) {
      std::cerr << "Unexpected pack-fold diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(foldFrontend.canGenerateCode() &&
             lang::verifyMirProgram(foldFrontend.mir).valid(),
         "a bounded call pack fold should select and lower every concrete "
         "element in source order");

  const lang::FunctionDecl *visitAll =
      findTopLevelFunction(foldFrontend.program, "visit_all");
  const auto *foldStatement =
      visitAll == nullptr ? nullptr
                          : dynamic_cast<const lang::ExpressionStmt *>(
                                visitAll->body()->statements().at(1).get());
  const auto *fold = foldStatement == nullptr
                         ? nullptr
                         : dynamic_cast<const lang::PackFold *>(
                               foldStatement->expression().get());
  const lang::ResolvedPackFoldInfo *resolvedFold =
      fold == nullptr ? nullptr : foldFrontend.semantics.findPackFold(*fold);
  expect(resolvedFold != nullptr && resolvedFold->function != 0 &&
             resolvedFold->packSymbol != 0 &&
             resolvedFold->packParameter != 0 &&
             resolvedFold->packArgument == 1,
         "semantic pack-fold metadata should retain its one selected "
         "function, pack identity, and argument position");

  const lang::HirFunctionInstance *concreteFold = nullptr;
  for (const lang::HirFunctionInstance &instance :
       foldFrontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "visit_all" &&
        instance.parameterTypes.size() == 3) {
      concreteFold = &instance;
      break;
    }
  }
  const lang::HirValue *hirFold = nullptr;
  if (concreteFold != nullptr) {
    const auto found = std::find_if(
        concreteFold->body.values.begin(), concreteFold->body.values.end(),
        [](const lang::HirValue &value) {
          return value.kind == lang::HirValueKind::PackFold;
        });
    if (found != concreteFold->body.values.end()) {
      hirFold = &*found;
    }
  }
  expect(hirFold != nullptr && hirFold->packFoldFunction != 0 &&
             hirFold->packFoldElements.size() == 3 &&
             std::all_of(hirFold->packFoldElements.begin(),
                         hirFold->packFoldElements.end(),
                         [](const lang::HirPackFoldElement &element) {
                           return element.functionTarget != 0 &&
                                  element.parameterTypes.size() == 2 &&
                                  element.parameterTypes.back().kind ==
                                      lang::SemanticType::Reference;
                         }),
         "HIR should retain one exact selected call target for every concrete "
         "pack element");

  const lang::MirFunctionInstance *mirFunction =
      concreteFold == nullptr
          ? nullptr
          : foldFrontend.mir.findFunctionInstance(concreteFold->id);
  std::vector<const lang::MirInstruction *> mirFoldCalls;
  if (mirFunction != nullptr && hirFold != nullptr) {
    for (const lang::MirBlock &block : mirFunction->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.hirValue == hirFold->id) {
          mirFoldCalls.push_back(&instruction);
        }
      }
    }
  }
  bool orderedMirCalls =
      hirFold != nullptr && mirFunction != nullptr &&
      mirFoldCalls.size() == hirFold->packFoldElements.size();
  for (std::size_t index = 0; orderedMirCalls && index < mirFoldCalls.size();
       ++index) {
    const lang::MirInstruction &call = *mirFoldCalls[index];
    const lang::HirPackFoldElement &element = hirFold->packFoldElements[index];
    const lang::MirPlace *packElement =
        call.operands.empty()
            ? nullptr
            : mirFunction->body.findPlace(call.operands.back().place);
    orderedMirCalls =
        call.functionTarget == element.functionTarget &&
        call.parameterTypes == element.parameterTypes &&
        call.operands.size() == element.parameterTypes.size() &&
        call.operands.back().kind == lang::MirOperandKind::BorrowRead &&
        packElement != nullptr && packElement->type == element.elementType &&
        packElement->projections.size() == 1 &&
        packElement->projections.front().kind ==
            lang::MirProjectionKind::PackElement &&
        packElement->projections.front().constantIndex == index;
  }
  expect(orderedMirCalls,
         "MIR should expand a pack fold into ordered exact calls over concrete "
         "pack-element places without reopening overload resolution");

  lang::MirProgram forgedFold = foldFrontend.mir;
  bool changedFoldSignature = false;
  auto &forgedFoldFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          forgedFold.functionInstances());
  const auto forgedFunction = std::find_if(
      forgedFoldFunctions.begin(), forgedFoldFunctions.end(),
      [&](const lang::MirFunctionInstance &candidate) {
        return concreteFold != nullptr && candidate.id == concreteFold->id;
      });
  if (forgedFunction != forgedFoldFunctions.end() && hirFold != nullptr) {
    for (lang::MirBlock &block : forgedFunction->body.blocks) {
      const auto call = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.hirValue == hirFold->id;
          });
      if (call != block.instructions.end() && !call->parameterTypes.empty()) {
        call->parameterTypes.back() = lang::SemanticType::Bool;
        changedFoldSignature = true;
        break;
      }
    }
  }
  expect(changedFoldSignature && !lang::verifyMirProgram(forgedFold).valid(),
         "MIR verification should reject a lowered pack call whose exact "
         "parameter signature was forged");

  const std::string foldGenerated = gti_test::emitCppText(foldFrontend);
  expect(resolvedFold != nullptr &&
             containsMangledFunction(foldGenerated, "", "touch",
                                     "__gti_mir_failure(__gti_mir_p_") &&
             foldGenerated.find("_touch(count, values), ...") ==
                 std::string::npos,
         "the C++ backend should emit the one compiler-selected fold target "
         "instead of asking native overload resolution to choose per element");

  const std::string invalidFoldSource = R"(
void overloaded<T>(T& value) {}
void overloaded<U>(U& value) {}
int returns_value<T>(T& value) { return 0; }
void takes_value<T>(T value) {}
int make_value() { return 1; }

void invalid<Values...>(Values... values) {
  (overloaded(values), ...);
  (returns_value(values), ...);
  (takes_value(values), ...);
  (takes_value(make_value()), ...);
}

void not_a_pack(int value) { (takes_value(value), ...); }
)";
  const lang::FrontendResult invalidFold =
      lang::Frontend().analyze("invalid-pack-fold.gti", invalidFoldSource);
  const auto invalidArgument = std::find_if(
      invalidFold.diagnostics.begin(), invalidFold.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2023" &&
               diagnostic.message.find("must be named local values") !=
                   std::string::npos;
      });
  expect(
      !invalidFold.canGenerateCode() &&
          hasDiagnosticCode(invalidFold.diagnostics, "GTI-S2023") &&
          hasDiagnostic(invalidFold.diagnostics,
                        "must name exactly one function declaration") &&
          hasDiagnostic(invalidFold.diagnostics,
                        "returning void, with one inferred type parameter") &&
          hasDiagnostic(invalidFold.diagnostics,
                        "must be named local values") &&
          hasDiagnostic(invalidFold.diagnostics,
                        "must name the current function's parameter pack") &&
          invalidArgument != invalidFold.diagnostics.end() &&
          invalidArgument->primary.start ==
              invalidFoldSource.find("make_value()), ...") +
                  std::string("make_value(").size() &&
          invalidArgument->primary.end == invalidArgument->primary.start + 1 &&
          invalidArgument->fixes.empty(),
      "invalid pack folds should fail with bounded shape diagnostics before "
      "HIR or native emission and point at the non-repeatable argument");
}

void testExactFunctionOverloadsAndConversions() {
  const std::string source = R"(
namespace math {
uint64_t pow(uint64_t base, uint64_t exponent) { return base * exponent; }
float pow(float base, float exponent) { return base * exponent; }
}

struct Selector<T, U> {
  T apply(T value) { return value; }
  U apply(U value) { return value; }
};

struct Receiver {
  mut int value = 0;
  int inspect() { return this.value; }
  int inspect() mut {
    this.value += 1;
    return this.value;
  }
};

int main() {
  uint64_t base = uint64_t(2);
  uint64_t exponent = uint64_t(8);
  uint64_t whole = math::pow(base, exponent);
  float decimal = math::pow(2.0, 8.0);
  Selector<int, float> selector = Selector<int, float>();
  int selected = selector.apply(2);
  float selected_decimal = selector.apply(2.0);
  Receiver read_only = Receiver();
  mut Receiver writable = Receiver();
  int read_result = read_only.inspect();
  int write_result = writable.inspect();
  return int(whole) + selected + read_result + write_result;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("overloads.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected overload diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "exact namespace and method overloads should validate");
  expect(
      frontend.semantics.functionCount() == 7 &&
          frontend.semantics.resolvedCallCount() == 6,
      "semantic analysis should retain function identities and selected calls");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto selectedReceiver = [&](std::size_t statementIndex) {
    if (main == nullptr ||
        statementIndex >= main->body()->statements().size()) {
      return static_cast<const lang::ResolvedCallInfo *>(nullptr);
    }
    const auto *variable = dynamic_cast<const lang::VariableDecl *>(
        main->body()->statements()[statementIndex].get());
    const auto *call =
        variable == nullptr
            ? nullptr
            : dynamic_cast<const lang::Call *>(variable->initializer().get());
    return call == nullptr ? nullptr : frontend.semantics.findCall(*call);
  };
  const lang::ResolvedCallInfo *readCall = selectedReceiver(9);
  const lang::ResolvedCallInfo *writeCall = selectedReceiver(10);
  expect(readCall != nullptr && readCall->declaration != nullptr &&
             readCall->declaration->receiverMutability() ==
                 lang::ReceiverMutability::ReadOnly &&
             writeCall != nullptr && writeCall->declaration != nullptr &&
             writeCall->declaration->receiverMutability() ==
                 lang::ReceiverMutability::Mutable,
         "method overload selection should choose the receiver-qualified "
         "declaration exactly");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(
      artifact.contents.find(
          "::__gti_program::math::__gti_fn_1_pow__gti_mir_failure(") !=
              std::string::npos &&
          artifact.contents.find(
              "::__gti_program::math::__gti_fn_2_pow__gti_mir_failure(") !=
              std::string::npos &&
          concreteBackendSymbols(artifact.contents, "__gti_type_", "Selector")
                  .size() == 1 &&
          concreteBackendSymbols(artifact.contents, "__gti_member_", "apply")
                  .size() == 2,
      "the C++ backend should emit the function identity selected by GTI");
  expect(artifact.contents.find("mir_checked_convert_v1<std::uint64_t>(") !=
                 std::string::npos &&
             artifact.contents.find("mir_checked_convert_v1<std::int32_t>(") !=
                 std::string::npos &&
             artifact.contents.find(
                 "std::bit_cast<float>(std::uint32_t{0x40000000U})") !=
                 std::string::npos,
         "explicit conversions and float literals should lower to matching C++ "
         "types");

  lang::Lexer lexer;
  auto invalidTokens = lexer.scan(R"(
int same(int value) { return value; }
float same(int value) { return float(value); }

int mutate(int value) { return value; }
int mutate(mut int value) { return value; }

T echo<T>(T value) { return value; }
U echo<U>(U value) { return value; }

struct MutableOnly {
  int inspect() mut { return 1; }
};

int choose(int value) { return value; }
T choose<T>(T value) { return value; }

uint64_t width(uint64_t value) { return value; }
float width(float value) { return value; }
float only_float(float value) { return value; }
void function_value() {}

int main(int value) { return value; }
int main() {
  MutableOnly read_only = MutableOnly();
  int invalid_receiver = read_only.inspect();
  function_value;
  width;
  float mismatch = width(1);
  int ambiguous = choose(1);
  float inexact = only_float(1);
  uint8_t too_small = uint8_t(300);
  int not_numeric = int("text");
  return 0;
}
)");
  expect(!lexer.hadError(), "invalid overload cases should lex");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid overload cases should remain syntactically valid");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid overloads and conversions should fail semantics");
  expect(countDiagnosticCode(invalidSemantic, "GTI-S2011") == 4,
         "return type, by-value mutability, and generic spelling should not "
         "create distinct overload signatures");
  expect(
      hasDiagnostic(invalidSemantic, "main entry point cannot be overloaded"),
      "the native entry point should remain a unique function");
  expect(hasDiagnostic(invalidSemantic, "No overload of 'width'") &&
             hasDiagnostic(invalidSemantic, "argument types (int32_t)"),
         "overload lookup should reject calls without an exact candidate");
  expect(hasDiagnostic(invalidSemantic, "ambiguous") &&
             hasDiagnostic(invalidSemantic, "exactly match"),
         "generic and concrete exact matches should be diagnosed as ambiguous");
  expect(hasDiagnostic(invalidSemantic, "parameter requires 'float'"),
         "single functions should also require exact argument types");
  expect(hasDiagnostic(invalidSemantic,
                       "Mutable method requires a mutable receiver"),
         "receiver-qualified overloads should reject mutable-only methods on "
         "read-only objects");
  expect(
      countDiagnosticCode(invalidSemantic, "GTI-S2090") == 1 &&
          hasDiagnostic(
              invalidSemantic,
              "function item is ambiguous without an exact callable context"),
      "function overload sets should not escape unresolved into the backend");
  expect(hasDiagnostic(invalidSemantic, "outside the range of 'uint8_t'") &&
             hasDiagnostic(invalidSemantic,
                           "numeric conversions require a numeric value"),
         "explicit conversions should reject invalid literals and domains");

  const std::string formatted = lang::Formatter().format(
      "uint64_t pow(uint64_t base,uint64_t exponent){return base*exponent;}int "
      "main(){uint64_t value=uint64_t(2);return int(value);}");
  expect(formatted.find("uint64_t value = uint64_t(2);") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "explicit conversion syntax should format like a C++ functional cast");
}

void testFixedArrays() {
  const std::string source = R"(
uint32_t video[64 * 32] = {};

int extent(int values[2]) { return 2; }
int extent(int values[3]) { return 3; }
int first(int values[3]) { return values[0]; }

struct Buffers {
public:
  mut int samples[3] = {1, 2, 3};

  void bump() mut { this.samples[1] += 4; }
  uint64_t count() { return this.samples.size(); }
};

int main() {
  mut int buffer[2 + 3] = {1, 2, 3, 4, 5};
  buffer[2] = 10;
  buffer[1]++;
  int copy[5] = buffer;
  int matrix[0x2][0b11] = {{1, 2, 3}, {4, 5, 6}};
  mut Buffers buffers = Buffers();
  buffers.bump();
  uint64_t count = buffer.size();
  if (count == 5 and first(matrix[1]) == 4 and extent(matrix[0]) == 3 and
      buffers.count() == 3 and copy[4] == 5) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("arrays.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected fixed array diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "fixed arrays should support value semantics and checked indexing");

  const lang::MirFunctionInstance *bumpMir = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "bump") {
      bumpMir = frontend.mir.findFunctionInstance(instance.id);
      break;
    }
  }
  const lang::MirInstruction *boundsRead = nullptr;
  const lang::MirInstruction *checkedAdd = nullptr;
  const lang::MirInstruction *arrayCommit = nullptr;
  if (bumpMir != nullptr) {
    for (const lang::MirBlock &block : bumpMir->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Load &&
            !instruction.localFailureSites.empty()) {
          boundsRead = &instruction;
        } else if (instruction.kind == lang::MirInstructionKind::Compute &&
                   instruction.operation == lang::MirOperation::Add &&
                   !instruction.localFailureSites.empty()) {
          checkedAdd = &instruction;
        } else if (instruction.kind == lang::MirInstructionKind::Assign &&
                   instruction.operation == lang::MirOperation::Assign &&
                   instruction.localFailureSites.empty()) {
          arrayCommit = &instruction;
        }
      }
    }
  }
  const auto hasFailureOutcome = [&](const lang::MirInstruction *instruction,
                                     lang::DefinedFailureCode code) {
    return instruction != nullptr &&
           std::any_of(
               instruction->localFailureSites.begin(),
               instruction->localFailureSites.end(),
               [&](lang::FailureSiteId id) {
                 const lang::FailureSiteDescriptor *site =
                     frontend.mir.failureMetadata().findSite(id);
                 return site != nullptr &&
                        std::any_of(
                            site->outcomes.begin(), site->outcomes.end(),
                            [&](const lang::DefinedFailureOutcome &outcome) {
                              return outcome.code == code;
                            });
               });
  };
  expect(boundsRead != nullptr && boundsRead->result &&
             hasFailureOutcome(boundsRead,
                               lang::DefinedFailureCode::IndexOutOfBounds) &&
             checkedAdd != nullptr && checkedAdd->result &&
             hasFailureOutcome(checkedAdd,
                               lang::DefinedFailureCode::IntegerOverflow) &&
             checkedAdd->operands.size() == 2 &&
             checkedAdd->operands.front().value == *boundsRead->result &&
             arrayCommit != nullptr && arrayCommit->operands.size() == 1 &&
             arrayCommit->operands.front().value == *checkedAdd->result,
         "MIR should order fixed-array compound updates as checked read, "
         "checked arithmetic, and failure-free commit");

  const auto *main = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().back().get());
  const auto *buffer = main == nullptr
                           ? nullptr
                           : dynamic_cast<const lang::VariableDecl *>(
                                 main->body()->statements().front().get());
  const lang::BindingInfo *binding =
      buffer == nullptr ? nullptr : frontend.semantics.findBinding(*buffer);
  expect(binding != nullptr &&
             binding->type.kind == lang::SemanticType::Array &&
             binding->type.arrayLength == 5 &&
             binding->type.arguments.size() == 1 &&
             binding->type.arguments[0] == lang::SemanticType::Int32 &&
             binding->access == lang::AccessMode::Mutable,
         "fixed array bindings should retain length, element type, and access");

  const lang::SemanticType moveOnlyArray = lang::SemanticType::arrayOf(
      lang::SemanticType::uniqueOwnerOf(lang::SemanticType::Int32), 2);
  expect(!lang::semanticTraits(moveOnlyArray).copyable &&
             lang::semanticTraits(moveOnlyArray).movable,
         "fixed arrays should inherit element copy and move traits");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(
      artifact.contents.find("const std::array<std::uint32_t, 2048> video = "
                             "std::array<std::uint32_t, 2048>{}") !=
              std::string::npos &&
          artifact.contents.find("std::array<std::int32_t, 5> __gti_mir_p_") !=
              std::string::npos &&
          artifact.contents.find("std::array<std::array<std::int32_t, 3>, 2> "
                                 "__gti_mir_p_") != std::string::npos &&
          artifact.contents.find("mir_checked_array_read_v1") !=
              std::string::npos &&
          artifact.contents.find("mir_checked_array_write_v1") !=
              std::string::npos &&
          artifact.contents.find("mir_checked_add_v1<std::int32_t>") !=
              std::string::npos &&
          artifact.contents.find("static_cast<std::uint64_t>") !=
              std::string::npos,
      "the C++ backend should preserve array values and checked operations");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-arrays.gti", R"(
struct NoDefault {
  int stored;
  NoDefault(int value) : stored(value) {}
};

class ReadOnlyArrayReceiver {
  mut int slots[1] = {0};

public:
  void write() { slots[0] = 1; }
};

int overflow_extent[18446744073709551615 + 1] = {};
int negative_extent[1 - 2] = {};
int zero_divisor_extent[4 / 0] = {};

int choose(int values[2]) { return 2; }
int choose(int values[3]) { return 3; }

int main() {
  int wrong_count[3] = {1, 2};
  int immutable[1] = {1};
  immutable[0] = 2;
  int values[2] = {1, 2};
  int bad_index = values[1.5];
  int negative = values[-1];
  int past_end = values[2];
  mut int missing[2];
  NoDefault objects[1] = {};
  int other[4] = {1, 2, 3, 4};
  int mismatch = choose(other);
  uint64_t hidden = other.length();
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "invalid fixed array operations should fail semantic analysis");
  expect(
      hasDiagnostic(invalid.diagnostics, "requires exactly 3") &&
          hasDiagnostic(invalid.diagnostics, "immutable fixed array") &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot modify field 'slots' because method "
                        "'write' has no trailing 'mut' receiver qualifier") &&
          hasDiagnosticHint(invalid.diagnostics, "trailing 'mut'") &&
          hasRelatedDiagnostic(invalid.diagnostics,
                               "is declared mutable, but it is writable") &&
          hasDiagnostic(invalid.diagnostics, "index must have an integer") &&
          hasDiagnostic(invalid.diagnostics, "valid range [0, 2)") &&
          hasDiagnostic(invalid.diagnostics, "require an initializer") &&
          hasDiagnostic(invalid.diagnostics, "default-initializable") &&
          hasDiagnostic(invalid.diagnostics, "No overload of 'choose'") &&
          hasDiagnostic(invalid.diagnostics, "Unknown fixed array member") &&
          hasDiagnostic(invalid.diagnostics,
                        "extent arithmetic overflows uint64_t") &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot produce a negative value") &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot divide or take modulo by zero"),
      "fixed array diagnostics should explain extent, access, and bounds");

  lang::Lexer lexer;
  lang::Parser malformed(lexer.scan("int broken[] = {}; int recovered = 1;"));
  const lang::Program recovered = malformed.parse();
  expect(malformed.hadError() && recovered.declarations().size() == 1,
         "parser recovery should continue after a missing array extent");

  const std::string formatted = lang::Formatter().format(
      "mut int buffer[1+2]={1,2,3};int matrix[0x2][0b10]={{1,2},{3,4}};");
  expect(formatted == "mut int buffer[1 + 2] = {1, 2, 3};\n"
                      "int matrix[0x2][0b10] = {{1, 2}, {3, 4}};\n" &&
             lang::Formatter().format(formatted) == formatted,
         "formatter should preserve compact C++-style array declarations");
  const std::string constructorFormatted = lang::Formatter().format(
      "class Pair<T>{T values[2];public:Pair(T left,T right):"
      "values({left,right}){}};");
  expect(constructorFormatted.find("values({left, right}) {}") !=
             std::string::npos,
         "formatter should keep array constructor initializers compact");
}

// A dynamic fixed-array element write keeps its index_out_of_bounds failure
// site, and MIR must pair that site with an Invoke terminator so the checked
// write helper's status feeds the failure successor exactly like the read
// form. Losing the edge silently no-ops the out-of-bounds write.
void testFixedArrayDynamicWriteFailureEdge() {
  const std::string source = R"(
int main() {
  mut int32_t cells[4] = {};
  mut uint64_t index = uint64_t(0);
  while (index < uint64_t(9)) {
    index++;
  }
  cells[index] = 7;
  return int(cells[uint64_t(0)]);
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("dynamic-array-write.gti", source);
  expect(frontend.canGenerateCode(),
         "the dynamic array-write fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *mainMir = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "main") {
      mainMir = frontend.mir.findFunctionInstance(instance.id);
      break;
    }
  }
  const lang::MirBlock *writeBlock = nullptr;
  const lang::MirInstruction *write = nullptr;
  if (mainMir != nullptr) {
    for (const lang::MirBlock &block : mainMir->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Assign &&
            instruction.operation == lang::MirOperation::Assign &&
            instruction.destination && !instruction.localFailureSites.empty()) {
          writeBlock = &block;
          write = &instruction;
        }
      }
    }
  }
  const bool boundsOutcome =
      write != nullptr &&
      std::any_of(
          write->localFailureSites.begin(), write->localFailureSites.end(),
          [&](lang::FailureSiteId id) {
            const lang::FailureSiteDescriptor *site =
                frontend.mir.failureMetadata().findSite(id);
            return site != nullptr &&
                   std::any_of(
                       site->outcomes.begin(), site->outcomes.end(),
                       [&](const lang::DefinedFailureOutcome &outcome) {
                         return outcome.code ==
                                lang::DefinedFailureCode::IndexOutOfBounds;
                       });
          });
  expect(
      write != nullptr && boundsOutcome &&
          lang::requiresMirFailureControlFlow(
              *write, lang::MirFailureControlFlowPosition::FullExpressionRoot),
      "a dynamic fixed-array element write should stay eligible for MIR "
      "failure control flow");
  expect(writeBlock != nullptr && write != nullptr &&
             !writeBlock->instructions.empty() &&
             writeBlock->instructions.back().id == write->id &&
             writeBlock->terminator.kind == lang::MirTerminatorKind::Invoke &&
             writeBlock->terminator.invokeInstruction == write->id &&
             writeBlock->terminator.elseTarget != 0 &&
             mainMir->body.blocks[writeBlock->terminator.elseTarget - 1]
                     .failureParameter == writeBlock->terminator.failureRecord,
         "the dynamic array write must terminate its block with an Invoke "
         "whose failure successor consumes the checked write status");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::size_t status = artifact.contents.find(
      " = ::gti_internal::backend::mir_checked_array_write_v1(");
  const std::size_t variable =
      status == std::string::npos
          ? std::string::npos
          : artifact.contents.rfind("__gti_mir_failure_status_", status);
  const std::string consumer =
      variable == std::string::npos
          ? std::string()
          : "if (" + artifact.contents.substr(variable, status - variable) +
                ".code == GTI_FAILURE_CODE_NONE_V1)";
  expect(status != std::string::npos && variable != std::string::npos &&
             !consumer.empty() &&
             artifact.contents.find(consumer, status) != std::string::npos,
         "the emitted checked array write must branch on its failure status "
         "instead of discarding it");
}

// A chained fixed-array element access with a dynamic nonterminal index is one
// checked access with one bounds site. The emitter used to admit a nonterminal
// index only when it was a compile-time in-bounds constant and declined the
// body otherwise, so `matrix[row][column]` with a runtime row raised GTI-B0001
// on the read form as well as the write form. The chained text now owns a
// bound per dimension: the failure form guards each dynamic nonterminal index
// before the terminal checked helper runs.
void testFixedArrayChainedDynamicIndex() {
  const std::string source = R"(
int main() {
  mut int32_t matrix[2][3] = {};
  mut uint64_t row = uint64_t(0);
  while (row < uint64_t(9)) {
    row = row + uint64_t(1);
  }
  uint64_t column = uint64_t(1);
  matrix[row][column] = 7;
  return int(matrix[row][column]);
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("chained-array-index.gti", source);
  expect(frontend.canGenerateCode(),
         "the chained array-index fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  // The emitter reports a declined body by throwing, which is what raises
  // GTI-B0001 through the driver. Catching it keeps the regression a readable
  // failure instead of an abort.
  lang::BackendArtifact artifact;
  std::string declined;
  try {
    artifact =
        gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  } catch (const std::exception &error) {
    declined = error.what();
  }
  expect(declined.empty(),
         "a chained dynamic element access should stay inside the complete "
         "C++ text vocabulary");

  const std::size_t entry =
      artifact.contents.rfind("bool __gti_entry__gti_mir_failure(");
  const std::string body = entry == std::string::npos
                               ? std::string()
                               : artifact.contents.substr(entry);
  const auto occurrences = [&](const std::string &needle) {
    std::size_t count = 0;
    for (std::size_t at = body.find(needle); at != std::string::npos;
         at = body.find(needle, at + needle.size())) {
      ++count;
    }
    return count;
  };
  const std::string guard =
      " = ::gti_internal::backend::mir_checked_array_bound_v1(";
  const std::string read =
      " = ::gti_internal::backend::mir_checked_array_read_v1(";
  const std::string write =
      " = ::gti_internal::backend::mir_checked_array_write_v1(";
  // One guard for the dynamic row of the read and one for the write, each
  // paired with the terminal helper that carries the column.
  expect(!body.empty() && occurrences(guard) == 2 && occurrences(read) == 1 &&
             occurrences(write) == 1,
         "each chained dynamic access should guard its nonterminal dimension "
         "alongside the terminal checked helper");

  const std::size_t guardAt = body.find(guard);
  const std::size_t statusAt =
      guardAt == std::string::npos
          ? std::string::npos
          : body.rfind("__gti_mir_failure_status_", guardAt);
  const std::string status = statusAt == std::string::npos
                                 ? std::string()
                                 : body.substr(statusAt, guardAt - statusAt);
  const std::size_t writeAt = body.find(write);
  expect(!status.empty() && writeAt != std::string::npos && guardAt < writeAt &&
             body.find("if (" + status + ".code == GTI_FAILURE_CODE_NONE_V1) {",
                       guardAt) < writeAt,
         "the guard should open the success branch the terminal checked "
         "helper runs inside");
}

void testLocalTypeInference() {
  const std::string source = R"(
class Box {
  int value = 0;

public:
  Box(int value) : value(value) {}
  int read() { return this.value; }
};

T preserve<T>(T value) {
  auto inferred = value;
  return inferred;
}

int main() {
  auto integer = 1;
  mut auto running = preserve(integer);
  running += 2;
  auto truth = running == 3;
  auto text = "gti";
  auto box = Box(running);
  int values[2] = {running, 4};
  auto copied_values = values;
  auto empty = nullptr;
  for (mut auto index = 0; index < 2; index++) {
    running += copied_values[index];
  }
  if (truth and text == "gti" and empty == nullptr and box.read() == 3 and
      running == 10) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("auto-inference.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected auto diagnostic: " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "initialized local auto bindings should pass the frontend");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto *integer = main == nullptr
                            ? nullptr
                            : dynamic_cast<const lang::VariableDecl *>(
                                  main->body()->statements().front().get());
  const auto *running = main == nullptr
                            ? nullptr
                            : dynamic_cast<const lang::VariableDecl *>(
                                  main->body()->statements().at(1).get());
  const lang::BindingInfo *integerBinding =
      integer == nullptr ? nullptr : frontend.semantics.findBinding(*integer);
  const lang::BindingInfo *runningBinding =
      running == nullptr ? nullptr : frontend.semantics.findBinding(*running);
  expect(integerBinding != nullptr &&
             integerBinding->type == lang::SemanticType::Int32 &&
             integerBinding->access == lang::AccessMode::ReadOnly &&
             runningBinding != nullptr &&
             runningBinding->type == lang::SemanticType::Int32 &&
             runningBinding->access == lang::AccessMode::Mutable,
         "semantic bindings should retain concrete inferred types and access");

  const lang::HirFunctionInstance *preserveInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "preserve" &&
        instance.typeArguments ==
            std::vector<lang::SemanticType>{lang::SemanticType::Int32}) {
      preserveInstance = &instance;
      break;
    }
  }
  expect(preserveInstance != nullptr &&
             preserveInstance->body.bindings.size() == 2 &&
             preserveInstance->body.bindings.back().info.type ==
                 lang::SemanticType::Int32,
         "instantiated HIR should retain the concrete type of an inferred "
         "generic local");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("bool __gti_entry__gti_mir_failure(") !=
                 std::string::npos &&
             artifact.contents.find("std::int32_t __gti_mir_p_") !=
                 std::string::npos &&
             artifact.contents.find("std::string_view __gti_mir_p_") !=
                 std::string::npos &&
             artifact.contents.find("std::array<std::int32_t, 2> "
                                    "__gti_mir_p_") != std::string::npos,
         "the C++ backend should lower inferred locals to their concrete MIR "
         "representations");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-auto.gti", R"(
auto global = 1;

class InvalidField {
  auto value = 1;
};

auto inferred_return() { return 1; }
int inferred_parameter(auto value) { return 0; }
void perform() {}

int main() {
  mut int value = 1;
  auto fixed = value;
  fixed = 2;
  auto missing;
  auto& reference = value;
  auto array[2] = {1, 2};
  auto values = {1, 2};
  auto no_value = perform();
  return 0;
}
)");
  expect(!invalid.canGenerateCode() &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2028") &&
             hasDiagnostic(invalid.diagnostics, "limited to local bindings") &&
             hasDiagnostic(invalid.diagnostics, "initialized local binding") &&
             hasDiagnostic(invalid.diagnostics, "requires an initializer") &&
             hasDiagnostic(invalid.diagnostics,
                           "limited to range-for element bindings") &&
             hasDiagnostic(invalid.diagnostics, "array extents") &&
             hasDiagnostic(invalid.diagnostics,
                           "requires a fixed array type from its context") &&
             hasDiagnostic(invalid.diagnostics, "complete value type") &&
             hasDiagnostic(invalid.diagnostics, "immutable binding 'fixed'"),
         "auto diagnostics should explain every unsupported declaration and "
         "inference context");

  const lang::FrontendResult moveOnly = lang::Frontend().analyze(
      "move-only-auto.gti", R"(
#include <std/memory>

class Value {
public:
  int number = 1;
};

int main() {
  auto owner = std::make_unique<Value>();
  auto copied = owner;
  return copied->number;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!moveOnly.canGenerateCode() &&
             hasDiagnosticCode(moveOnly.diagnostics, "GTI-S2003") &&
             hasDiagnostic(moveOnly.diagnostics,
                           "Cannot initialize inferred binding 'copied'") &&
             hasDiagnosticHint(moveOnly.diagnostics, "std::move(owner)"),
         "auto initialization should reject move-only copies in GTI semantics");

  const lang::FrontendResult movedOwner = lang::Frontend().analyze(
      "moved-auto.gti", R"(
#include <std/memory>

class Value {
public:
  int number = 1;
};

int main() {
  auto owner = std::make_unique<Value>();
  auto moved = std::move(owner);
  return moved->number - 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const lang::FunctionDecl *movedMain =
      findTopLevelFunction(movedOwner.program, "main");
  const auto *moved = movedMain == nullptr
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                movedMain->body()->statements().at(1).get());
  const lang::BindingInfo *movedBinding =
      moved == nullptr ? nullptr : movedOwner.semantics.findBinding(*moved);
  expect(movedOwner.canGenerateCode() && movedBinding != nullptr &&
             movedBinding->traits.ownership == lang::OwnershipKind::Unique &&
             !movedBinding->traits.copyable && movedBinding->traits.movable,
         "auto should retain ownership traits after an explicit move");

  const std::string formatted = lang::Formatter().format(
      "int main(){auto fixed=1;mut auto changing=fixed;for(mut auto i=0;i<2;"
      "i++){changing+=i;}return changing;}");
  expect(formatted.find("auto fixed = 1;") != std::string::npos &&
             formatted.find("mut auto changing = fixed;") !=
                 std::string::npos &&
             formatted.find("for (mut auto i = 0; i < 2; i++) {") !=
                 std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "local auto declarations should receive stable C++-style formatting");
}

void testStructuredBindings() {
  const std::string source = R"(
struct Pair<T, U> {
  T first;
  U second;

  Pair(T first, U second) : first(first), second(second) {}
  ~Pair() {}
};

int main() {
  int values[2] = {2, 3};
  auto [left, right] = values;
  auto [number, enabled] = Pair<int, bool>(4, true);
  mut int result = 1;
  if (left == 2 && right == 3 && number == 4 && enabled) {
    result = 0;
  }
  return result;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("structured-bindings.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected structured-binding diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "fixed arrays and public direct aggregate fields should support "
         "structured bindings");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto *arrayBinding =
      main == nullptr ? nullptr
                      : dynamic_cast<const lang::StructuredBindingDecl *>(
                            main->body()->statements().at(1).get());
  const auto *pairBinding =
      main == nullptr ? nullptr
                      : dynamic_cast<const lang::StructuredBindingDecl *>(
                            main->body()->statements().at(2).get());
  expect(arrayBinding != nullptr && pairBinding != nullptr &&
             arrayBinding->bindings().size() == 2 &&
             pairBinding->bindings().size() == 2,
         "the AST should retain ordered structured-binding declarations");

  const lang::StructuredBindingInfo *arrayInfo =
      arrayBinding == nullptr
          ? nullptr
          : frontend.semantics.findStructuredBinding(*arrayBinding);
  const lang::StructuredBindingInfo *pairInfo =
      pairBinding == nullptr
          ? nullptr
          : frontend.semantics.findStructuredBinding(*pairBinding);
  expect(arrayInfo != nullptr && arrayInfo->elements.size() == 2 &&
             arrayInfo->elements[0].projection ==
                 lang::StructuredBindingProjectionKind::ArrayElement &&
             arrayInfo->elements[0].index == 0 &&
             arrayInfo->elements[1].index == 1 &&
             arrayInfo->elements[0].binding.type == lang::SemanticType::Int32 &&
             arrayInfo->elements[0].binding.access ==
                 lang::AccessMode::ReadOnly,
         "array decomposition should record exact immutable element "
         "projections");
  expect(pairInfo != nullptr &&
             pairInfo->source.type.kind == lang::SemanticType::Class &&
             pairInfo->elements.size() == 2 &&
             pairInfo->elements[0].projection ==
                 lang::StructuredBindingProjectionKind::Field &&
             pairInfo->elements[0].field != 0 &&
             pairInfo->elements[1].field != 0 &&
             pairInfo->elements[0].binding.type == lang::SemanticType::Int32 &&
             pairInfo->elements[1].binding.type == lang::SemanticType::Bool &&
             pairInfo->elements[0].binding.symbol != 0 &&
             pairInfo->elements[1].binding.symbol != 0,
         "aggregate decomposition should substitute field types and retain "
         "source-visible symbols");

  const lang::SourceUnitId unit = frontend.sourceGraph.entryUnit();
  const lang::LanguageQueries queries;
  const std::size_t leftDeclaration = source.find("left");
  const std::size_t leftUse = source.rfind("left");
  const std::optional<lang::HoverInfo> leftHover =
      queries.hover(frontend, unit, leftDeclaration + 1);
  const std::optional<lang::DefinitionInfo> leftDefinition =
      queries.definition(frontend, unit, leftUse + 1);
  expect(leftHover && leftHover->signature == "int32_t left" &&
             leftDefinition && leftDefinition->target.start == leftDeclaration,
         "shared semantic queries should expose hover and definition for "
         "each decomposed name");

  const lang::HirFunctionInstance *mainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == main) {
      mainInstance = &instance;
      break;
    }
  }
  const lang::HirStatement *arrayStatement =
      mainInstance == nullptr || mainInstance->body.roots.size() < 3
          ? nullptr
          : mainInstance->body.findStatement(mainInstance->body.roots[1]);
  const lang::HirStatement *pairStatement =
      mainInstance == nullptr || mainInstance->body.roots.size() < 3
          ? nullptr
          : mainInstance->body.findStatement(mainInstance->body.roots[2]);
  expect(arrayStatement != nullptr && pairStatement != nullptr &&
             arrayStatement->kind ==
                 lang::HirStatementKind::StructuredBinding &&
             pairStatement->kind == lang::HirStatementKind::StructuredBinding &&
             arrayStatement->binding && pairStatement->binding &&
             arrayStatement->structuredBindings.size() == 2 &&
             pairStatement->structuredBindings.size() == 2 &&
             arrayStatement->structuredBindings[0].index &&
             pairStatement->structuredBindings[0].field != 0,
         "HIR should retain one hidden owner and typed array/field "
         "projections");

  const lang::MirFunctionInstance *mirMain =
      mainInstance == nullptr
          ? nullptr
          : frontend.mir.findFunctionInstance(mainInstance->id);
  const auto findSourcePlace =
      [&](const lang::HirStatement *statement) -> const lang::MirPlace * {
    if (mirMain == nullptr || statement == nullptr || !statement->binding) {
      return nullptr;
    }
    const auto found =
        std::find_if(mirMain->body.places.begin(), mirMain->body.places.end(),
                     [&](const lang::MirPlace &place) {
                       return place.root == lang::MirPlaceRootKind::Binding &&
                              place.binding == *statement->binding &&
                              place.projections.empty();
                     });
    return found == mirMain->body.places.end() ? nullptr : &*found;
  };
  const lang::MirPlace *arraySource = findSourcePlace(arrayStatement);
  const lang::MirPlace *pairSource = findSourcePlace(pairStatement);
  std::size_t arrayInitializations = 0;
  std::size_t pairInitializations = 0;
  std::size_t pairDrops = 0;
  if (mirMain != nullptr) {
    for (const lang::MirBlock &block : mirMain->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        arrayInitializations +=
            arrayStatement != nullptr &&
                    instruction.kind == lang::MirInstructionKind::Initialize &&
                    instruction.hirStatement == arrayStatement->id
                ? 1
                : 0;
        pairInitializations +=
            pairStatement != nullptr &&
                    instruction.kind == lang::MirInstructionKind::Initialize &&
                    instruction.hirStatement == pairStatement->id
                ? 1
                : 0;
        pairDrops +=
            pairSource != nullptr &&
                    instruction.kind == lang::MirInstructionKind::Drop &&
                    instruction.destination == pairSource->id
                ? 1
                : 0;
      }
    }
  }
  const bool hasArrayProjection =
      mirMain != nullptr && arrayStatement != nullptr &&
      std::any_of(mirMain->body.places.begin(), mirMain->body.places.end(),
                  [&](const lang::MirPlace &place) {
                    return place.root == lang::MirPlaceRootKind::Binding &&
                           place.binding == *arrayStatement->binding &&
                           std::any_of(
                               place.projections.begin(),
                               place.projections.end(),
                               [](const lang::MirPlaceProjection &projection) {
                                 return projection.kind ==
                                        lang::MirProjectionKind::Index;
                               });
                  });
  const bool hasFieldProjection =
      mirMain != nullptr && pairStatement != nullptr &&
      std::any_of(mirMain->body.places.begin(), mirMain->body.places.end(),
                  [&](const lang::MirPlace &place) {
                    return place.root == lang::MirPlaceRootKind::Binding &&
                           place.binding == *pairStatement->binding &&
                           std::any_of(
                               place.projections.begin(),
                               place.projections.end(),
                               [](const lang::MirPlaceProjection &projection) {
                                 return projection.kind ==
                                        lang::MirProjectionKind::Field;
                               });
                  });
  expect(arraySource != nullptr && pairSource != nullptr &&
             arrayInitializations == 1 && pairInitializations == 1 &&
             pairDrops == 1 && hasArrayProjection && hasFieldProjection,
         "MIR should initialize and destroy each hidden owner once while "
         "visible names remain projected places");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::unordered_set<std::string> pairTypes =
      concreteBackendSymbols(artifact.contents, "__gti_type_", "Pair");
  const std::string pairLifetime =
      pairTypes.size() == 1
          ? "mir_lifetime_slot<::__gti_program::" + *pairTypes.begin() + ">"
          : std::string();
  expect(artifact.contents.find("bool __gti_entry__gti_mir_failure(") !=
                 std::string::npos &&
             artifact.contents.find("std::array<std::int32_t, 2> "
                                    "__gti_mir_p_") != std::string::npos &&
             !pairLifetime.empty() &&
             artifact.contents.find(pairLifetime) != std::string::npos &&
             artifact.contents.find(".get().first") != std::string::npos &&
             artifact.contents.find(".get().second") != std::string::npos &&
             artifact.contents.find("const auto [left, right]") ==
                 std::string::npos,
         "the C++ backend should represent validated structured bindings as "
         "MIR projections without relying on C++ decomposition declarations");

  const std::string formatted = lang::Formatter().format(
      "int main(){int values[2]={1,2};auto[left,right]=values;return left;}");
  expect(formatted.find("auto [left, right] = values;") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "structured bindings should receive stable C++-style formatting");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-structured-bindings.gti", R"(
class Hidden {
  int secret = 1;
};

struct Base {
  int base = 1;
};

struct Derived : public Base {
  int own = 2;
};

struct MoveOnly {
  int value = 1;

  MoveOnly(MoveOnly& other) = delete;
  MoveOnly(MoveOnly&& other) = default;
};

int main() {
  int values[2] = {1, 2};
  auto [too, many, names] = values;
  auto [scalar] = 1;
  Hidden hidden{};
  auto [secret] = hidden;
  Derived derived{};
  auto [base, own] = derived;
  auto [same, same] = values;
  auto [first, second] = values;
  first = 3;
  auto moved = std::move(second);
  MoveOnly owner{};
  auto [copied] = owner;
  auto [untyped_left, untyped_right] = {1, 2};
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(!invalid.canGenerateCode() &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2048") &&
             hasDiagnostic(invalid.diagnostics,
                           "declares 3 names but the source provides 2") &&
             hasDiagnostic(invalid.diagnostics,
                           "require a fixed array or a class/struct") &&
             hasDiagnostic(invalid.diagnostics,
                           "every direct instance field to be public") &&
             hasDiagnostic(invalid.diagnostics,
                           "do not decompose inherited storage") &&
             hasDiagnostic(invalid.diagnostics, "Duplicate declaration") &&
             hasDiagnostic(invalid.diagnostics,
                           "Cannot assign to immutable binding 'first'") &&
             hasDiagnostic(invalid.diagnostics,
                           "cannot partially move structured binding") &&
             hasDiagnostic(invalid.diagnostics,
                           "Cannot initialize structured binding by copying") &&
             hasDiagnostic(invalid.diagnostics,
                           "requires a fixed array type from its context"),
         "structured-binding diagnostics should reject ambiguous ownership, "
         "shape, access, mutation, and untyped initialization");

  lang::Lexer lexer;
  const auto expectSyntaxError = [&](std::string input,
                                     std::string_view message) {
    lang::Parser parser(lexer.scan(input));
    (void)parser.parse();
    expect(parser.hadError() &&
               hasDiagnostic(parser.errors(), std::string(message)),
           "structured-binding syntax should diagnose " + std::string(message));
  };
  expectSyntaxError("auto [left, right] = value;", "local declarations");
  expectSyntaxError("class Bad { auto [left, right] = value; };",
                    "cannot declare class or struct fields");
  expectSyntaxError("int main() { mut auto [left, right] = values; return 0; }",
                    "Structured bindings are immutable");
  expectSyntaxError("int main() { auto& [left, right] = values; return 0; }",
                    "Reference structured bindings are not supported");
  expectSyntaxError(
      "int main() { for (auto [left, right] = values; true; left++) {} "
      "return 0; }",
      "not supported in a for-loop initializer");
  lang::Parser autoArrayParser(
      lexer.scan("int main() { auto[2] values = {1, 2}; return 0; }"));
  (void)autoArrayParser.parse();
  expect(!autoArrayParser.hadError(),
         "structured-binding lookahead should preserve typed array syntax for "
         "semantic auto diagnostics");
  lang::Parser recoveryParser(lexer.scan(
      "int main() { int broken = 1 auto [left, right] = values; return left; "
      "}"));
  const lang::Program recovered = recoveryParser.parse();
  const lang::FunctionDecl *recoveredMain =
      findTopLevelFunction(recovered, "main");
  expect(recoveryParser.hadError() && recoveredMain != nullptr &&
             !recoveredMain->body()->statements().empty() &&
             dynamic_cast<const lang::StructuredBindingDecl *>(
                 recoveredMain->body()->statements().front().get()) != nullptr,
         "parser recovery should resume at a structured binding after a "
         "missing semicolon");
}

void testTypeAliases() {
  lang::Lexer lexer;
  const std::string source = R"(
using LaterSize = Size;
using Size = uint64_t;
using Triple = int[3];

class Box<T> {
  using stored_type = T;
  T value;
public:
  using value_type = stored_type;
  Box(T value) : value(value) {}
  value_type get() { return this.value; }
};

class Traits {
public:
  using value_type = hidden_type;
private:
  using hidden_type = int;
public:
  value_type identity(value_type value) { return value; }
};

using IntBox = Box<int>;
using TraitValue = Traits::value_type;
using BoxValue = IntBox::value_type;
using Completion = expected<void, int>;
using Result = int;

Completion complete() { return; }

Result main() {
  LaterSize count = Size(3);
  Triple values = {1, 2, 3};
  IntBox box = IntBox(values[1]);
  Traits traits{};
  BoxValue input = 4;
  TraitValue trait_value = traits.identity(input);
  Completion completion = complete();
  if (count == 3 and box.get() == 2 and trait_value == 4 and
      completion) { return 0; }
  return 1;
}
)";
  auto tokens = lexer.scan(source);
  expect(!lexer.hadError(), "type aliases should lex as declarations");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "namespace-scoped type aliases should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "type aliases should canonicalize before semantic checks");

  const std::string generated = emitAnalyzedSource(source);
  expect(generated.find("using LaterSize = std::uint64_t;") !=
                 std::string::npos &&
             generated.find("using Triple = std::array<std::int32_t, 3>;") !=
                 std::string::npos &&
             generated.find("using IntBox = "
                            "::__gti_program::Box<std::int32_t>;") !=
                 std::string::npos &&
             generated.find("using value_type = T;") != std::string::npos &&
             generated.find("using TraitValue = std::int32_t;") !=
                 std::string::npos &&
             generated.find("static_cast<std::uint64_t>(3)") !=
                 std::string::npos &&
             generated.find("#include <expected>") != std::string::npos &&
             generated.find("int main()") != std::string::npos,
         "the C++ backend should emit canonical, declaration-order-independent "
         "aliases");

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("class-type-aliases.gti", source);
  const lang::SourceUnitId unit = frontend.sourceGraph.entryUnit();
  const lang::LanguageQueries queries;
  const std::size_t aliasDeclaration = source.find("value_type = hidden_type");
  const std::size_t aliasUse = source.find("Traits::value_type");
  const std::optional<lang::HoverInfo> hover =
      queries.hover(frontend, unit, aliasUse + 9);
  const std::optional<lang::DefinitionInfo> definition =
      queries.definition(frontend, unit, aliasUse + 9);
  expect(frontend.canGenerateCode() && hover &&
             hover->signature == "using Traits::value_type = int32_t" &&
             definition && definition->target.start == aliasDeclaration,
         "class-scoped aliases should participate in shared hover and "
         "definition queries");
  const std::size_t concreteAliasUse = source.find("IntBox::value_type");
  const auto concreteAliasDefinition =
      queries.definition(frontend, unit, concreteAliasUse + 9);
  const auto concreteAliasHover =
      queries.hover(frontend, unit, concreteAliasUse + 9);
  const auto qualifierDefinition =
      queries.definition(frontend, unit, concreteAliasUse + 1);
  expect(concreteAliasDefinition && qualifierDefinition && concreteAliasHover &&
             concreteAliasHover->signature ==
                 "using Box::value_type = int32_t" &&
             concreteAliasDefinition->target.start ==
                 source.find("value_type = stored_type") &&
             qualifierDefinition->target.start == source.find("IntBox ="),
         "concrete alias qualifiers and their members should retain distinct "
         "source declaration identities for editor navigation");

  const lang::FrontendResult constrainedAlias =
      lang::Frontend().analyze("generic-class-type-alias.gti",
                               R"(
class NumericBox<std::numeric T> {
public:
  using value_type = T;
  value_type copy(value_type input) { return value_type(input); }
};
int main() {
  NumericBox<int> box{};
  return box.copy(7) - 7;
}
)",
                               {standardLibraryPrelude()});
  expect(constrainedAlias.canGenerateCode(),
         "a generic class alias should preserve the owner's capabilities and "
         "substitute its concrete target in an instantiated member body");

  const auto qualifiedAliases =
      lang::Frontend().analyze("concrete-alias-members.gti", R"(
using Value = Concrete::nested_type::value_type;
using Concrete = Outer<int>;
struct Inner<T> { using value_type = T; };
struct Outer<T> { using nested_type = Inner<T>; };
int main() { Value value = 7; return value - 7; }
)");
  expect(qualifiedAliases.canGenerateCode(),
         "concrete aliases should qualify nested type members with canonical "
         "substitution independently of declaration order");
  const auto privateQualifier =
      lang::Frontend().analyze("private-alias-qualifier.gti", R"(
struct Inner<T> { using value_type = T; };
class Outer<T> { using hidden = Inner<T>; };
using Concrete = Outer<int>;
using Leaked = Concrete::hidden::value_type;
int main() { return 0; }
)");
  expect(!privateQualifier.canGenerateCode() &&
             hasDiagnosticCode(privateQualifier.diagnostics, "GTI-S2007"),
         "a public final member must not bypass a private alias qualifier");

  const lang::FrontendResult classKinds =
      lang::Frontend().analyze("class-kind-type-aliases.gti",
                               R"(
struct RecordTraits { using value_type = int; };
interface ContractTraits { using value_type = int; };
union UnionTraits {
  using value_type = int;
  int value;
};
using RecordValue = RecordTraits::value_type;
using ContractValue = ContractTraits::value_type;
using UnionValue = UnionTraits::value_type;
int main() {
  RecordValue record = 1;
  ContractValue contract = 2;
  UnionValue variant = 3;
  return record + contract + variant - 6;
}
)");
  expect(classKinds.canGenerateCode(),
         "struct, interface, and union aliases should follow their public "
         "default access and remain usable as direct type members");

  const std::string completionSource =
      "class Traits { public: using value_type = int; }; "
      "using Value = Traits::val; int main() { return 0; }";
  const lang::CompletionResult completion = lang::LanguageQueries().complete(
      {.entryPath = "class-alias-completion.gti",
       .source = completionSource,
       .byteOffset = completionSource.rfind("val") + 3});
  const auto completionAlias =
      std::find_if(completion.candidates.begin(), completion.candidates.end(),
                   [](const lang::CompletionCandidate &candidate) {
                     return candidate.label == "value_type";
                   });
  expect(completionAlias != completion.candidates.end() &&
             completionAlias->kind ==
                 lang::CompletionCandidateKind::TypeAlias &&
             completionAlias->detail == "using Traits::value_type = int32_t",
         "qualified completion should expose accessible class-scoped type "
         "aliases with canonical GTI details");

  const auto completeAlias = [](const std::string &source) {
    return lang::LanguageQueries().complete(
        {.entryPath = "concrete-alias-completion.gti",
         .source = source,
         .byteOffset = source.rfind("val") + 3});
  };
  const std::string concreteCompletionSource = R"(
class Box<T> {
  using value_hidden = T;
public:
  using value_type = T;
};
using Concrete = Box<int>;
using Partial = Concrete::val;
int main() { return 0; }
)";
  const auto concreteCompletion = completeAlias(concreteCompletionSource);
  const auto concreteCandidate = std::find_if(
      concreteCompletion.candidates.begin(),
      concreteCompletion.candidates.end(),
      [](const auto &candidate) { return candidate.label == "value_type"; });
  expect(
      concreteCandidate != concreteCompletion.candidates.end() &&
          concreteCandidate->kind == lang::CompletionCandidateKind::TypeAlias &&
          concreteCandidate->detail == "using Concrete::value_type = int32_t" &&
          std::none_of(concreteCompletion.candidates.begin(),
                       concreteCompletion.candidates.end(),
                       [](const auto &candidate) {
                         return candidate.label == "value_hidden";
                       }),
      "concrete alias completion should substitute member types and retain "
      "private filtering");
  const auto nestedCompletion = completeAlias(R"(
struct Inner<T> { using value_type = T; };
struct Outer<T> { using nested_type = Inner<T>; };
using Concrete = Outer<uint64_t>;
using Partial = Concrete::nested_type::val;
int main() { return 0; }
)");
  expect(std::any_of(
             nestedCompletion.candidates.begin(),
             nestedCompletion.candidates.end(),
             [](const auto &candidate) {
               return candidate.label == "value_type" &&
                      candidate.detail ==
                          "using Concrete::nested_type::value_type = uint64_t";
             }),
         "chained concrete alias completion should preserve every owner "
         "substitution");
  const auto hiddenCompletion = completeAlias(R"(
struct Inner<T> { using value_type = T; };
class Outer<T> { using hidden = Inner<T>; };
using Concrete = Outer<int>;
using Partial = Concrete::hidden::val;
int main() { return 0; }
)");
  expect(std::none_of(hiddenCompletion.candidates.begin(),
                      hiddenCompletion.candidates.end(),
                      [](const auto &candidate) {
                        return candidate.label == "value_type";
                      }),
         "qualified completion must not bypass a private intermediate alias");

  auto invalidTokens = lexer.scan(R"(
using First = Second;
using Second = First;
using Borrow = int&;
using Missing = MissingType;
using Duplicate = int;
using Duplicate = uint64_t;
int main() { return 0; }
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid alias relationships should remain semantic errors");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "cycles, reference aliases, missing targets, and duplicates should "
         "be rejected");
  expect(hasDiagnostic(invalidSemantic, "Type alias cycle") &&
             hasDiagnostic(invalidSemantic, "Reference aliases") &&
             hasDiagnostic(invalidSemantic, "Unknown type 'MissingType'") &&
             hasDiagnostic(invalidSemantic, "Duplicate declaration"),
         "invalid aliases should produce focused diagnostics");

  const lang::FrontendResult invalidClassAliases =
      lang::Frontend().analyze("invalid-class-type-aliases.gti", R"(
class PrivateTraits {
  using secret_type = int;
public:
  secret_type identity(secret_type value) { return value; }
};
using Leaked = PrivateTraits::secret_type;

struct DuplicateMember {
  using value_type = int;
  int value_type = 0;
};

class CallbackOwner {
public:
  using Callback = (int) -> void;
};

class GenericOwner<T> {
public:
  using value_type = T;
};
using UnboundGenericMember = GenericOwner::value_type;

class BaseOwner {
public:
  using inherited_type = int;
};
class DerivedOwner : public BaseOwner {};
using UninheritedMember = DerivedOwner::inherited_type;

int main() { return 0; }
)");
  const auto privateDiagnostic =
      std::find_if(invalidClassAliases.diagnostics.begin(),
                   invalidClassAliases.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2007";
                   });
  expect(!invalidClassAliases.canGenerateCode() &&
             hasDiagnosticCode(invalidClassAliases.diagnostics, "GTI-S2007") &&
             hasDiagnostic(invalidClassAliases.diagnostics,
                           "secret_type' of 'PrivateTraits' is private") &&
             privateDiagnostic != invalidClassAliases.diagnostics.end() &&
             !privateDiagnostic->related.empty() &&
             hasDiagnosticCode(invalidClassAliases.diagnostics, "GTI-S2006") &&
             hasDiagnostic(invalidClassAliases.diagnostics,
                           "already denotes a class-scoped type alias") &&
             hasDiagnosticCode(invalidClassAliases.diagnostics, "GTI-S2076") &&
             hasDiagnostic(invalidClassAliases.diagnostics,
                           "Native callback aliases must remain at namespace "
                           "scope") &&
             hasDiagnosticCode(invalidClassAliases.diagnostics, "GTI-S2030") &&
             hasDiagnostic(invalidClassAliases.diagnostics,
                           "belongs to a generic class") &&
             hasDiagnostic(invalidClassAliases.diagnostics,
                           "Unknown type 'DerivedOwner::inherited_type'"),
         "class-scoped aliases should enforce access, name uniqueness, and "
         "the deliberately bounded generic, inheritance, and native callback "
         "boundaries");

  lang::Parser localAliasParser(
      lexer.scan("int main() { using Local = int; return 0; }"));
  (void)localAliasParser.parse();
  expect(
      localAliasParser.hadError() &&
          hasDiagnostic(localAliasParser.errors(), "namespace or class scope"),
      "local type aliases should be rejected by the parser");

  const std::string formatted = lang::Formatter().format(
      "class Traits{using hidden=int;public:using value_type=hidden;};");
  expect(formatted == R"(class Traits {
  using hidden = int;
public:
  using value_type = hidden;
};
)" && lang::Formatter().format(formatted) == formatted,
         "the formatter should indent class-scoped aliases as ordinary "
         "members and preserve access-label spacing");
}

void testNamespacesAndAliases() {
  lang::Lexer lexer;
  const std::string source = R"(
namespace engine {
namespace graphics {
class Renderer {};
void render() {}
void renderTwice() {
  render();
  render();
}
}
}

namespace gfx = engine::graphics;
gfx::Renderer createRenderer();

int main() {
  engine::graphics::render();
  gfx::renderTwice();
  return 0;
}
)";
  auto tokens = lexer.scan(source);
  expect(!lexer.hadError(), "namespace source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "nested namespaces and aliases should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "qualified calls and namespace aliases should resolve");

  const std::string generated = emitAnalyzedSource(source);
  expect(generated.find("namespace engine {") != std::string::npos &&
             generated.find("namespace graphics {") != std::string::npos,
         "emitter should preserve nested namespaces");
  expect(generated.find("namespace gfx = engine::graphics;") !=
             std::string::npos,
         "emitter should preserve namespace aliases");
  expect(containsMangledFunction(generated,
                                 "::__gti_program::engine::graphics::Renderer ",
                                 "createRenderer", "();") &&
             generated.find("namespace gfx = engine::graphics;") !=
                 std::string::npos,
         "qualified types should parse and emit through namespace aliases");
  expect(containsMangledFunction(generated, "engine::graphics::", "render",
                                 "();") &&
             containsMangledFunction(
                 generated, "engine::graphics::", "renderTwice", "();"),
         "emitter should preserve qualified calls");

  auto invalidTokens = lexer.scan(R"(
namespace engine {}
namespace gfx = engine::missing;
int main() {
  engine::missing::render();
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "invalid namespace source should parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "unknown namespace paths should be rejected");
  expect(invalidSemantic.errors().size() == 2,
         "alias targets and qualified calls should both be diagnosed");

  std::string shorthand = source;
  const std::string opening = "namespace engine {\nnamespace graphics {";
  shorthand.replace(shorthand.find(opening), opening.size(),
                    "namespace engine::graphics {");
  const std::string closing = "}\n}\n\nnamespace gfx";
  shorthand.replace(shorthand.find(closing), closing.size(),
                    "}\n\nnamespace gfx");
  const lang::FrontendResult compact =
      lang::Frontend().analyze("namespace-shorthand.gti", shorthand);
  expect(compact.canGenerateCode(),
         "namespace shorthand should complete the same verified MIR pipeline "
         "as nested blocks");
  const auto *outer = compact.program.declarations().empty()
                          ? nullptr
                          : lang::stmtAs<lang::NamespaceDecl>(
                                compact.program.declarations().front().get());
  const auto *inner = outer == nullptr || outer->declarations().empty()
                          ? nullptr
                          : lang::stmtAs<lang::NamespaceDecl>(
                                outer->declarations().front().get());
  expect(outer != nullptr && inner != nullptr &&
             outer->name().lexeme == "engine" &&
             inner->name().lexeme == "graphics" &&
             outer->name().position == shorthand.find("engine") &&
             inner->name().position == shorthand.find("graphics") &&
             outer->extent() && inner->extent() &&
             outer->extent()->start == shorthand.find("namespace") &&
             inner->extent()->start == shorthand.find("graphics") &&
             outer->extent()->end == inner->extent()->end,
         "expanded namespace nodes must preserve each written token and "
         "nested source extent");
  const lang::LanguageQueries queries;
  const auto definition =
      queries.definition(compact, compact.sourceGraph.entryUnit(),
                         shorthand.find("engine::graphics::render") + 9);
  expect(definition && definition->target.start == shorthand.find("graphics") &&
             definition->target.end == shorthand.find("graphics") + 8,
         "qualified namespace uses should navigate to the exact shorthand "
         "component");
  const auto symbols =
      queries.documentSymbols(compact, compact.sourceGraph.entryUnit());
  expect(!symbols.empty() && symbols.front().name == "engine" &&
             !symbols.front().children.empty() &&
             symbols.front().children.front().name == "graphics" &&
             symbols.front().children.front().selectionRange.start ==
                 shorthand.find("graphics"),
         "document symbols should retain the ordinary nested hierarchy and "
         "exact shorthand name ranges");

  const lang::FrontendResult reopened =
      lang::Frontend().analyze("namespace-shorthand-reopen.gti", R"(
namespace outer::inner { int original() { return 3; } }
namespace outer {
  namespace inner::detail { int value() { return original(); } }
}
namespace outer::inner::detail {
  int forward() { return value(); }
}
namespace shortcut = outer::inner::detail;
int main() { return shortcut::forward() - 3; }
)");
  expect(reopened.canGenerateCode(),
         "mixed shorthand and explicit blocks should reopen the same "
         "namespaces and retain parent-scope lookup and alias targets");
  const lang::FrontendResult collision = lang::Frontend().analyze(
      "namespace-shorthand-collision.gti",
      "class outer {}; namespace outer::inner {} int main() { return 0; }");
  expect(collision.syntaxValid && !collision.semanticValid,
         "namespace shorthand must not bypass existing declaration conflicts");

  const auto moduleRoot =
      std::filesystem::temp_directory_path() / "gti-namespace-shorthand";
  const auto modulePath =
      std::filesystem::weakly_canonical(moduleRoot / "api.gti").string();
  const std::unordered_map<std::string, std::string> modules{
      {modulePath,
       "export module api;\n"
       "export namespace public_api::nested { int value() { return 3; } }\n"
       "namespace private_api::nested { int secret() { return 4; } }\n"}};
  const auto exported = lang::Frontend().analyze(
      moduleRoot / "main.gti",
      "import \"api.gti\"; int main() { return public_api::nested::value(); }",
      {}, modules);
  expect(exported.canGenerateCode(),
         "an exported shorthand namespace should export its ordinary nested "
         "declarations");
  const auto privateMember =
      lang::Frontend().analyze(moduleRoot / "main.gti",
                               "import \"api.gti\"; int main() { return "
                               "private_api::nested::secret(); }",
                               {}, modules);
  expect(privateMember.syntaxValid && !privateMember.semanticValid,
         "shorthand must not export private module declarations");

  const std::string formatted =
      lang::Formatter().format("namespace outer :: inner{namespace "
                               "detail::api{int value(){return 3;}}}");
  expect(formatted == "namespace outer::inner {\n"
                      "  namespace detail::api {\n"
                      "    int value() {\n      return 3;\n    }\n  }\n}\n" &&
             lang::Formatter().format(formatted) == formatted,
         "formatting should preserve shorthand with canonical scope spacing "
         "and idempotent namespace indentation");

  const std::string missingName = "namespace outer:: {}";
  lang::Parser missingComponent(
      lexer.scan(missingName, "namespace-missing-component.gti"));
  (void)missingComponent.parse();
  expect(!missingComponent.errors().empty() &&
             missingComponent.errors().front().code == "GTI-P0001" &&
             missingComponent.errors().front().phase ==
                 lang::DiagnosticPhase::Parsing &&
             missingComponent.errors().front().message ==
                 "Expect namespace name after '::'." &&
             missingComponent.errors().front().primary.source ==
                 "namespace-missing-component.gti" &&
             missingComponent.errors().front().primary.start ==
                 missingName.find('{') &&
             missingComponent.errors().front().primary.end ==
                 missingName.find('{') + 1 &&
             missingComponent.errors().front().fixes.empty(),
         "a missing path component should diagnose its exact boundary without "
         "guessing a namespace name");

  for (const std::string_view invalid :
       {"namespace outer:: {}", "namespace outer::::inner {}",
        "namespace ::outer {}", "namespace outer::inner = target;"}) {
    lang::Parser malformed(
        lexer.scan(std::string(invalid) + " int recovered() { return 0; }"));
    const lang::Program recovered = malformed.parse();
    expect(malformed.hadError() &&
               std::any_of(
                   recovered.declarations().begin(),
                   recovered.declarations().end(),
                   [](const lang::StmtPtr &statement) {
                     const auto *function =
                         lang::stmtAs<lang::FunctionDecl>(statement.get());
                     return function && function->name().lexeme == "recovered";
                   }),
           "malformed namespace paths should diagnose and preserve later "
           "declarations");
  }
  std::string deep = "namespace n";
  for (int index = 1; index < 64; ++index) {
    deep += "::n";
  }
  lang::Parser bounded(lexer.scan(deep + " {}"));
  (void)bounded.parse();
  expect(!bounded.hadError(),
         "64 empty namespace levels should fit the existing parser budget");
  lang::Parser excessive(lexer.scan(deep + "::n {}"));
  (void)excessive.parse();
  expect(excessive.hadError() &&
             std::any_of(excessive.errors().begin(), excessive.errors().end(),
                         [](const lang::Diagnostic &diagnostic) {
                           return diagnostic.code == "GTI-P0005";
                         }),
         "shorthand components must not bypass the parser nesting limit");
}

void testExternCInterop() {
  lang::Lexer lexer;
  const std::vector<lang::Token> keywordTokens = lexer.scan("extern \"C\" {}");
  expect(!keywordTokens.empty() &&
             keywordTokens.front().kind == lang::TokenKind::EXTERN,
         "extern should have a dedicated keyword token");

  const std::string source = R"(
extern "C" {
  int32_t socket(int32_t domain, int32_t type, int32_t protocol);
  int32_t close(int32_t descriptor);
}

int main() {
  int32_t descriptor = socket(2, 1, 0);
  if (descriptor >= 0) {
    [[discard]] close(descriptor);
  }
  return 0;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("extern-c.gti", source);
  expect(frontend.canGenerateCode(),
         "fixed-width extern C declarations and calls should compile");
  expect(frontend.diagnostics.empty(),
         "valid extern C declarations should not produce diagnostics");

  const auto *block = frontend.program.declarations().empty()
                          ? nullptr
                          : dynamic_cast<const lang::ExternCDecl *>(
                                frontend.program.declarations().front().get());
  expect(block != nullptr && block->declarations().size() == 2,
         "the AST should retain an extern C linkage block and its prototypes");
  const auto *socketDeclaration =
      block == nullptr || block->declarations().empty()
          ? nullptr
          : dynamic_cast<const lang::FunctionDecl *>(
                block->declarations().front().get());
  expect(socketDeclaration != nullptr && socketDeclaration->hasCLinkage() &&
             !socketDeclaration->body(),
         "functions inside the linkage block should retain bodyless C "
         "linkage identity");

  const lang::FunctionInfo *socketInfo =
      socketDeclaration == nullptr
          ? nullptr
          : frontend.semantics.findFunction(*socketDeclaration);
  expect(socketInfo != nullptr &&
             socketInfo->linkage == lang::LanguageLinkage::C &&
             socketInfo->externalSymbol == "socket",
         "semantics should own the exact native linkage and symbol");
  const auto hirSocket =
      std::find_if(frontend.hir.functionInstances().begin(),
                   frontend.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &function) {
                     return function.externalSymbol == "socket";
                   });
  expect(hirSocket != frontend.hir.functionInstances().end() &&
             hirSocket->linkage == lang::LanguageLinkage::C,
         "HIR should retain the selected C linkage and native symbol");
  const lang::MirFunctionInstance *mirSocket =
      hirSocket == frontend.hir.functionInstances().end()
          ? nullptr
          : frontend.mir.findFunctionInstance(hirSocket->id);
  expect(mirSocket != nullptr &&
             mirSocket->linkage == lang::LanguageLinkage::C &&
             mirSocket->externalSymbol == "socket",
         "MIR should retain C linkage for future backend consumption");

  const std::string generated = gti_test::emitCppText(frontend);
  expect(generated.find("extern \"C\" {") != std::string::npos &&
             generated.find(
                 "std::int32_t socket(std::int32_t domain, std::int32_t "
                 "type, std::int32_t protocol);") != std::string::npos &&
             generated.find("std::int32_t close(std::int32_t descriptor);") !=
                 std::string::npos,
         "the C++ backend should emit canonical unmangled C prototypes");
  expect(generated.find("socket(__gti_mir_v_") != std::string::npos &&
             generated.find("close(__gti_mir_v_") != std::string::npos &&
             generated.find("__gti_fn_") == std::string::npos,
         "resolved C calls should use exact native symbols without GTI name "
         "mangling");

  const lang::FrontendResult linkedName =
      lang::Frontend().analyze("extern-c-link-name.gti", R"(
namespace cli11 {
extern "C" {
  [[link_name("gti_cli11_app_create")]]
  int32_t app_create(int32_t value);
}
}
int main() { return cli11::app_create(42) - 42; }
)");
  expect(linkedName.canGenerateCode() && linkedName.diagnostics.empty(),
         "link_name should separate GTI source lookup from a portable C "
         "symbol");
  const auto linkedHir =
      std::find_if(linkedName.hir.functionInstances().begin(),
                   linkedName.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &function) {
                     return function.externalSymbol == "gti_cli11_app_create";
                   });
  const lang::MirFunctionInstance *linkedMir =
      linkedHir == linkedName.hir.functionInstances().end()
          ? nullptr
          : linkedName.mir.findFunctionInstance(linkedHir->id);
  expect(linkedHir != linkedName.hir.functionInstances().end() &&
             linkedHir->source != nullptr &&
             linkedHir->source->name().lexeme == "app_create" &&
             linkedHir->linkage == lang::LanguageLinkage::C &&
             linkedMir != nullptr &&
             linkedMir->externalSymbol == "gti_cli11_app_create",
         "semantic, HIR, and MIR identities should retain the source and link "
         "names independently");
  const std::string linkedCpp = gti_test::emitCppText(linkedName);
  expect(linkedCpp.find("std::int32_t gti_cli11_app_create(") !=
                 std::string::npos &&
             linkedCpp.find("::__gti_program::cli11::gti_cli11_app_create(") !=
                 std::string::npos &&
             linkedCpp.find(" app_create(") == std::string::npos,
         "the backend should declare and call only the selected native link "
         "symbol");

  const lang::FrontendResult lateDeclaration =
      lang::Frontend().analyze("late-extern-c.gti", R"(
namespace platform {
int32_t invoke_close(int32_t descriptor) { return close(descriptor); }
extern "C" { int32_t close(int32_t descriptor); }
}
int main() { return platform::invoke_close(-1); }
)");
  expect(lateDeclaration.canGenerateCode(),
         "namespace-scoped C declarations should resolve before their source "
         "position");
  const std::string lateCpp = gti_test::emitCppText(lateDeclaration);
  const std::size_t latePrototype = lateCpp.find("extern \"C\" {");
  const std::size_t lateCall =
      lateCpp.find("::__gti_program::platform::close(__gti_mir_v_");
  expect(latePrototype != std::string::npos && lateCall != std::string::npos &&
             latePrototype < lateCall,
         "the backend should forward namespace-scoped C prototypes before "
         "earlier callers");

  const lang::FrontendResult ordinaryPrototype =
      lang::Frontend().analyze("ordinary-prototype.gti", R"(
int32_t native_like(int32_t value);
int main() { return native_like(7); }
)");
  expect(ordinaryPrototype.canGenerateCode(),
         "ordinary GTI bodyless functions should remain valid declarations");
  const std::string ordinaryCpp = gti_test::emitCppText(ordinaryPrototype);
  expect(
      ordinaryCpp.find("extern \"C\"") == std::string::npos &&
          containsMangledFunction(ordinaryCpp, "std::int32_t ", "native_like",
                                  "(const std::int32_t value);") &&
          containsMangledFunction(ordinaryCpp, "", "native_like",
                                  "(__gti_mir_v_") &&
          ordinaryCpp.find("native_like__gti_mir_failure") == std::string::npos,
      "ordinary prototypes should retain GTI mangling unless linkage is "
      "explicitly C");

  const lang::FrontendResult runtime =
      analyzeTrustedStandalonePreludeFixture("extern-runtime.gti", R"(
namespace gti_internal {
class text_view {};
namespace runtime {
using native_text = gti_internal::text_view;
extern "C" {
int32_t gti_rt_write_stdout(native_text value);
int64_t gti_rt_open_file_read(native_text path);
}

void write_stdout(gti_internal::text_view value) {
  [[discard]] gti_rt_write_stdout(value);
}

int64_t open_file_read(gti_internal::text_view path) {
  return gti_rt_open_file_read(path);
}
}
}

int main() {
  gti_internal::runtime::write_stdout("hello");
  [[discard]] gti_internal::runtime::open_file_read("input");
  return 0;
}
)");
  expect(runtime.canGenerateCode(),
         "counted text buffers should support runtime C ABI migration");
  const std::string runtimeCpp = gti_test::emitCppText(runtime);
  expect(runtimeCpp.find("#include <gti/c_abi.h>") != std::string::npos &&
             runtimeCpp.find("#include <gti/runtime.hpp>") ==
                 std::string::npos &&
             runtimeCpp.find(
                 "std::int32_t gti_rt_write_stdout(::gti_c_string_view "
                 "value);") != std::string::npos &&
             runtimeCpp.find("::gti_internal::backend::to_c_string_view("
                             "__gti_mir_v_") != std::string::npos,
         "text views should cross C linkage through the explicit public "
         "counted-buffer ABI");

  const lang::FrontendResult shadowedRecord =
      analyzeTrustedStandalonePreludeFixture("shadowed-c-record.gti", R"(
namespace gti_internal { class text_view {}; }
namespace native {
struct gti_c_string_view {};
extern "C" {
int32_t measure_text(gti_internal::text_view value);
}
int32_t measure() { return measure_text("x"); }
}
int main() { return native::measure() - 1; }
)");
  expect(shadowedRecord.canGenerateCode(),
         "a GTI namespace should be allowed to reuse the public record's "
         "unqualified spelling");
  const std::string shadowedRecordCpp = gti_test::emitCppText(shadowedRecord);
  expect(
      shadowedRecordCpp.find("inline ::gti_c_string_view to_c_string_view") !=
              std::string::npos &&
          shadowedRecordCpp.find(
              "std::int32_t measure_text(::gti_c_string_view value);") !=
              std::string::npos,
      "C ABI text lowering should name the global public record even when "
      "a source namespace shadows it");

  const lang::FrontendResult legacyRuntime =
      analyzeTrustedStandalonePreludeFixture("legacy-runtime.gti", R"(
namespace gti_internal {
class text_view {};
namespace runtime {
@runtime("stdout.write")
void write_stdout(gti_internal::text_view value);
}
}
int main() {
  gti_internal::runtime::write_stdout("compatibility");
  return 0;
}
)");
  expect(legacyRuntime.canGenerateCode(),
         "the closed compiler-owned runtime attribute should remain a "
         "compatible internal declaration surface");
  const std::string legacyRuntimeCpp = gti_test::emitCppText(legacyRuntime);
  expect(legacyRuntimeCpp.find("#include <gti/runtime.hpp>") !=
                 std::string::npos &&
             legacyRuntimeCpp.find("#include <gti/c_abi.h>") ==
                 std::string::npos,
         "legacy runtime bindings should stay separate from public C ABI "
         "lowering");

  const lang::FrontendResult invalidLegacyRuntime =
      analyzeTrustedStandalonePreludeFixture("invalid-legacy-runtime.gti", R"(
namespace gti_internal { class text_view {}; }
@runtime("stdout.write")
void fake_write(gti_internal::text_view value);
int main() { fake_write("hello"); return 0; }
)");
  expect(!invalidLegacyRuntime.semanticValid &&
             hasDiagnostic(invalidLegacyRuntime.diagnostics,
                           "Invalid declaration for runtime binding"),
         "the legacy runtime attribute should remain compiler-owned rather "
         "than becoming a second general FFI");

  auto wrongLanguageTokens =
      lexer.scan("extern \"Rust\" {} int main() { return 0; }");
  lang::Parser wrongLanguageParser(std::move(wrongLanguageTokens));
  wrongLanguageParser.parse();
  expect(wrongLanguageParser.hadError() &&
             hasDiagnostic(wrongLanguageParser.errors(),
                           "only the extern \"C\" linkage"),
         "the parser should reject unknown linkage languages");

  auto bodyTokens = lexer.scan(R"(
extern "C" { int32_t native() { return 1; } }
int main() { return 0; }
)");
  lang::Parser bodyParser(std::move(bodyTokens));
  lang::Program recoveredBody = bodyParser.parse();
  expect(bodyParser.hadError() &&
             hasDiagnostic(bodyParser.errors(), "must be a bodyless") &&
             findTopLevelFunction(recoveredBody, "main") != nullptr,
         "extern C definitions should be rejected without losing following "
         "declarations during recovery");

  auto variableTokens = lexer.scan(R"(
extern "C" { int32_t not_a_function; }
int main() { return 0; }
)");
  lang::Parser variableParser(std::move(variableTokens));
  variableParser.parse();
  expect(variableParser.hadError() &&
             hasDiagnostic(variableParser.errors(),
                           "may contain only function declarations"),
         "extern C blocks should reject native variables in the bounded ABI");

  auto duplicateAttributeTokens = lexer.scan(R"(
extern "C" {
  [[link_name("first")]] [[link_name("second")]] int32_t renamed();
  int32_t recovered();
}
int main() { return 0; }
)");
  lang::Parser duplicateAttributeParser(std::move(duplicateAttributeTokens));
  lang::Program duplicateAttributeProgram = duplicateAttributeParser.parse();
  expect(duplicateAttributeParser.hadError() &&
             hasDiagnostic(duplicateAttributeParser.errors(),
                           "Duplicate [[link_name") &&
             findTopLevelFunction(duplicateAttributeProgram, "main") != nullptr,
         "duplicate link_name attributes should diagnose and recover through "
         "the enclosing linkage block");

  auto nestedTokens = lexer.scan(R"(
extern "C" {
  extern "C" { int32_t nested(); }
  int32_t recovered();
}
int main() { return 0; }
)");
  lang::Parser nestedParser(std::move(nestedTokens));
  lang::Program recoveredNested = nestedParser.parse();
  const auto *recoveredBlock =
      recoveredNested.declarations().empty()
          ? nullptr
          : dynamic_cast<const lang::ExternCDecl *>(
                recoveredNested.declarations().front().get());
  expect(nestedParser.hadError() && recoveredBlock != nullptr &&
             recoveredBlock->declarations().size() == 1 &&
             findTopLevelFunction(recoveredNested, "main") != nullptr,
         "invalid nested linkage blocks should recover at the next prototype");

  auto misplacedTokens = lexer.scan(R"(
class Holder {
  extern "C" { int32_t member_native(); }
};
int main() {
  extern "C" { int32_t local_native(); }
  return 0;
}
)");
  lang::Parser misplacedParser(std::move(misplacedTokens));
  lang::Program misplacedProgram = misplacedParser.parse();
  expect(misplacedParser.hadError() &&
             countDiagnosticContaining(misplacedParser.errors(),
                                       "limited to namespace scope") == 2 &&
             findTopLevelFunction(misplacedProgram, "main") != nullptr,
         "class and block linkage declarations should fail clearly without "
         "breaking declaration recovery");

  const lang::FrontendResult externalMain =
      lang::Frontend().analyze("external-main.gti", R"(
namespace native {
extern "C" { [[link_name("main")]] int32_t native_main(); }
}
int main() { return native::native_main(); }
)");
  expect(!externalMain.semanticValid &&
             hasDiagnostic(externalMain.diagnostics,
                           "C symbol 'main' is reserved"),
         "a namespaced C declaration must not collide with the unmangled GTI "
         "entry point");

  const lang::FrontendResult externalStorage =
      lang::Frontend().analyze("external-storage-collision.gti", R"(
int32_t native_counter = 0;
namespace native {
extern "C" {
  [[link_name("native_counter")]] int32_t read_counter();
}
}
int main() { return 0; }
)");
  expect(!externalStorage.semanticValid &&
             hasDiagnostic(externalStorage.diagnostics,
                           "conflicts with a root namespace GTI variable"),
         "an exact C symbol must not alias root GTI storage through another "
         "source namespace");

  const std::string formatted = lang::Formatter().format(
      "extern \"C\"{[[c_array(count)]][[link_name(\"native_values\")]]"
      "int32_t* values(int32_t* count);int32_t close(int32_t descriptor);}");
  expect(formatted == "extern \"C\" {\n"
                      "  [[c_array(count)]][[link_name(\"native_values\")]] "
                      "int32_t* values(int32_t* count);\n"
                      "  int32_t close(int32_t descriptor);\n"
                      "}\n" &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should normalize extern C blocks idempotently");

  const lang::FrontendResult invalid =
      analyzeTrustedPreludeFixture("invalid-extern-c.gti", R"(
class Handle {};
enum class Mode { One };
[[link_name("native_plain")]] int32_t plain();
extern "C" {
  [[link_name("bad-symbol")]] int32_t bad_link_name();
  [[link_name("shared_symbol")]] int32_t first_name();
  [[link_name("shared_symbol")]] int32_t second_name();
  [[link_name("overload_i32")]] int32_t overloaded_name(int32_t value);
  [[link_name("overload_i64")]] int32_t overloaded_name(int64_t value);
  bool bad_bool(bool value);
  char bad_char(char value);
  int32_t bad_reference(int32_t& value);
  int32_t bad_array(int32_t values[4]);
  Handle bad_class(Handle value);
  Mode bad_enum(Mode value);
  expected<int32_t, int32_t> bad_expected(int32_t value);
  gti_internal::text_view bad_text_return();
  static int32_t bad_static(int32_t value);
  int32_t bad_generic<T>(T value);
  int32_t duplicate(int32_t value);
  int32_t duplicate(int64_t value);
}
namespace other {
extern "C" { int32_t duplicate(uint32_t value); }
}
int main() { return 0; }
)");
  expect(
      !invalid.semanticValid &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2054") >= 15 &&
          hasDiagnostic(invalid.diagnostics, "valid only on an extern \"C\"") &&
          hasDiagnostic(invalid.diagnostics,
                        "is not a portable C identifier") &&
          hasDiagnostic(invalid.diagnostics,
                        "C symbol 'shared_symbol' is declared more than "
                        "once") &&
          hasDiagnostic(invalid.diagnostics,
                        "extern \"C\" functions cannot be overloaded") &&
          hasDiagnostic(invalid.diagnostics,
                        "C symbol 'duplicate' is declared more than once"),
      "semantics should reject unsafe ABI types, modifiers, generics, and "
      "global C-symbol collisions before backend emission");
}

void testScopedEnums() {
  const std::string source = R"(
namespace engine {
enum class State : uint8_t {
  Idle,
  Running = 4,
  Stopped,
};
enum class Mode { editor, game };
}

using StateAlias = engine::State;

StateAlias next(StateAlias state) {
  if (state == StateAlias::Idle) {
    return engine::State::Running;
  }
  return StateAlias::Stopped;
}

int main() {
  auto state = next(StateAlias::Idle);
  if (state == engine::State::Running) {
    return 0;
  }
  return 1;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("scoped-enums.gti", source);
  expect(frontend.canGenerateCode(),
         "scoped enum declarations and qualified enumerators should compile");
  expect(frontend.diagnostics.empty(),
         "valid scoped enums should not produce diagnostics");
  expect(frontend.hir.enumDeclarations().size() == 2,
         "HIR should retain scoped enum declarations");
  const lang::HirEnum *stateEnum =
      frontend.hir.enumDeclarations().empty()
          ? nullptr
          : &frontend.hir.enumDeclarations().front();
  expect(stateEnum != nullptr &&
             stateEnum->underlyingType == lang::SemanticType::UInt8 &&
             stateEnum->enumerators.size() == 3 &&
             stateEnum->enumerators[2].value ==
                 lang::EnumConstant{.magnitude = 5},
         "HIR should retain the backing type and evaluated enumerator values");

  bool foundResolvedEnumerator = false;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      if (value.kind == lang::HirValueKind::QualifiedName && value.enumOwner &&
          value.enumValue) {
        foundResolvedEnumerator = true;
      }
    }
  }
  expect(foundResolvedEnumerator,
         "HIR enum references should carry resolved nominal value identity");

  const std::string generated = gti_test::emitCppText(frontend);
  expect(generated.find("enum class State : std::uint8_t;") !=
                 std::string::npos &&
             generated.find("enum class State : std::uint8_t {") !=
                 std::string::npos &&
             generated.find("enum class Mode : std::int32_t {") !=
                 std::string::npos &&
             generated.find("Running = 4") != std::string::npos,
         "the C++ backend should emit fixed-backing scoped enums");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-scoped-enums.gti", R"(
enum class Tiny : uint8_t {
  First,
  TooLarge = 256,
  First = 2,
};
enum class Other { First };

int main() {
  mut Tiny uninitialized;
  Tiny value = 1;
  int raw = Tiny::First;
  if (Tiny::First) {
    return 1;
  }
  bool mixed = Tiny::First == Other::First;
  auto missing = Tiny::Missing;
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "invalid scoped enum uses should block code generation");
  expect(hasDiagnostic(invalid.diagnostics,
                       "Scoped enum value does not fit its backing type") &&
             hasDiagnostic(invalid.diagnostics, "Duplicate enumerator") &&
             hasDiagnostic(invalid.diagnostics,
                           "Equality operands have incompatible types") &&
             hasDiagnostic(invalid.diagnostics,
                           "explicit enumerator initializer") &&
             hasDiagnostic(invalid.diagnostics, "Unknown enumerator"),
         "scoped enums should diagnose range, scope, and nominal type errors");

  lang::Lexer lexer;
  lang::Parser parser(
      lexer.scan("enum Legacy { Value }; int main() { return 0; }"));
  const lang::Program recovered = parser.parse();
  expect(!parser.hadError() && recovered.declarations().size() == 2,
         "optional class spelling parses as the same scoped enum construct");

  const std::string formatted = lang::Formatter().format(
      "enum class State:uint8_t{Idle,Running=4,Stopped,};");
  expect(formatted == R"(enum class State : uint8_t {
  Idle,
  Running = 4,
  Stopped,
};
)",
         "formatter should produce stable C++-style scoped enum layout");
  expect(lang::Formatter().format(formatted) == formatted,
         "scoped enum formatting should be idempotent");
}

void testDefaultParameters() {
  {
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(
        "int choose(int required, int fallback = 2) { return required; }"));
    const lang::Program parsed = parser.parse();
    const auto *function = parsed.declarations().empty()
                               ? nullptr
                               : dynamic_cast<const lang::FunctionDecl *>(
                                     parsed.declarations().front().get());
    expect(!parser.hadError() && function != nullptr &&
               function->parameters().size() == 2 &&
               !function->parameters()[0].hasDefault() &&
               function->parameters()[1].hasDefault(),
           "the parser should retain a trailing default expression on its "
           "parameter");
  }

  const std::string source = R"(
mut int side_effects = 0;

int advance() {
  side_effects += 1;
  return side_effects;
}

int ordinary(int first, int second = advance(), int third = advance()) {
  return first + second + third;
}

class Token {
public:
  Token() {}
  int method(int value = advance()) { return value; }
  static int static_method(int value = advance()) { return value; }
};

int consume(Token token = Token()) { return 11; }

class Base {
public:
  Base(int value = 8) {}
};

class Derived : public Base {
public:
  Derived() {}
};

T generic_default<T>(T value = 7) { return value; }

constexpr int constant_default(int value = 6) { return value; }
constexpr int folded = constant_default();

int main() {
  int omitted = ordinary(1);
  int explicit_values = ordinary(1, 8, 9);
  Token token = Token();
  int method_value = token.method();
  int static_value = Token::static_method();
  int owned_value = consume();
  Derived derived = Derived();
  int generic_value = generic_default<int>();
  return omitted + explicit_values + method_value + static_value +
         owned_value + generic_value + folded;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("default-parameters.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected default-parameter diagnostic ["
                << diagnostic.code << "]: " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.hir.valid() &&
             frontend.mirValid && frontend.mir.valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "free-function, method, constructor, base-constructor, owned, "
         "generic, and constexpr defaults should lower through verified MIR");

  const lang::FunctionDecl *ordinary =
      findTopLevelFunction(frontend.program, "ordinary");
  const lang::FunctionInfo *ordinaryInfo =
      ordinary == nullptr ? nullptr
                          : frontend.semantics.findFunction(*ordinary);
  if (ordinaryInfo != nullptr) {
    const lang::SignaturePrinter signatures(frontend.semantics);
    expect(
        signatures.function(*ordinaryInfo) ==
                "int32_t ordinary(int32_t first, int32_t second = <default>, "
                "int32_t third = <default>)" &&
            signatures.parameterLabels(ordinaryInfo->parameterTypes,
                                       &ordinary->parameters(), false) ==
                std::vector<std::string>{"int32_t first",
                                         "int32_t second = <default>",
                                         "int32_t third = <default>"},
        "compiler-owned signatures should expose which trailing "
        "parameters have defaults");
  } else {
    expect(false, "the default-parameter fixture should retain ordinary()");
  }

  const std::size_t omittedCall = source.rfind("ordinary(1)");
  const std::optional<lang::SignatureHelpInfo> signatureHelp =
      lang::LanguageQueries().signatureHelp(
          frontend, frontend.sourceGraph.entryUnit(),
          omittedCall + std::string("ordinary(").size());
  expect(signatureHelp &&
             signatureHelp->label ==
                 "int32_t ordinary(int32_t first, int32_t second = <default>, "
                 "int32_t third = <default>)" &&
             signatureHelp->parameterLabels.size() == 3 &&
             signatureHelp->parameterLabels[1] == "int32_t second = <default>",
         "signature help should retain omitted parameters and their default "
         "markers");

  const lang::VariableDecl *folded = nullptr;
  for (const lang::StmtPtr &declaration : frontend.program.declarations()) {
    const auto *variable =
        dynamic_cast<const lang::VariableDecl *>(declaration.get());
    if (variable != nullptr && variable->name().lexeme == "folded") {
      folded = variable;
      break;
    }
  }
  const lang::BindingInfo *foldedBinding =
      folded == nullptr ? nullptr : frontend.semantics.findBinding(*folded);
  expect(foldedBinding != nullptr && foldedBinding->constant.has_value(),
         "constexpr evaluation should evaluate an omitted default at its "
         "call site");

  std::size_t hirDefaultInputs = 0;
  bool omittedOrdinaryHasTwoDefaults = false;
  bool explicitOrdinaryHasNoDefaults = false;
  const auto inspectHirBody = [&](const lang::HirBody &body) {
    for (const lang::HirValue &value : body.values) {
      if (!value.callPlan) {
        continue;
      }
      const std::size_t defaults = std::count_if(
          value.callPlan->arguments.begin(), value.callPlan->arguments.end(),
          [](const lang::HirCallArgument &argument) {
            return argument.defaultArgument;
          });
      hirDefaultInputs += defaults;
      const lang::HirFunctionInstance *target =
          value.functionTarget
              ? frontend.hir.findFunctionInstance(*value.functionTarget)
              : nullptr;
      const auto *call = dynamic_cast<const lang::Call *>(value.source);
      if (target == nullptr || target->source == nullptr || call == nullptr ||
          target->source->name().lexeme != "ordinary") {
        continue;
      }
      if (call->arguments().size() == 1) {
        omittedOrdinaryHasTwoDefaults = defaults == 2;
      } else if (call->arguments().size() == 3) {
        explicitOrdinaryHasNoDefaults = defaults == 0;
      }
    }
  };
  inspectHirBody(frontend.hir.module());
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    inspectHirBody(function.body);
  }
  for (const lang::HirConstructorInstance &constructor :
       frontend.hir.constructorInstances()) {
    inspectHirBody(constructor.body);
  }
  if (!(hirDefaultInputs >= 6 && omittedOrdinaryHasTwoDefaults &&
        explicitOrdinaryHasNoDefaults)) {
    std::cerr << "Default-parameter HIR evidence: inputs=" << hirDefaultInputs
              << " omitted-ordinary=" << omittedOrdinaryHasTwoDefaults
              << " explicit-ordinary=" << explicitOrdinaryHasNoDefaults << '\n';
  }
  expect(hirDefaultInputs >= 6 && omittedOrdinaryHasTwoDefaults &&
             explicitOrdinaryHasNoDefaults,
         "HIR call plans should append omitted defaults in parameter order "
         "without marking explicit arguments");

  bool hirImplicitBaseDefault = false;
  for (const lang::HirConstructorInstance &constructor :
       frontend.hir.constructorInstances()) {
    hirImplicitBaseDefault =
        hirImplicitBaseDefault ||
        std::any_of(constructor.initializers.begin(),
                    constructor.initializers.end(),
                    [](const lang::HirConstructorInitializer &initializer) {
                      return initializer.kind ==
                                 lang::ConstructorInitializerTargetKind::Base &&
                             initializer.explicitArgumentCount == 0 &&
                             initializer.arguments.size() == 1;
                    });
  }
  bool mirImplicitBaseDefault = false;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    mirImplicitBaseDefault =
        mirImplicitBaseDefault ||
        std::any_of(constructor.initializers.begin(),
                    constructor.initializers.end(),
                    [](const lang::MirConstructorInitializer &initializer) {
                      return initializer.kind ==
                                 lang::ConstructorInitializerTargetKind::Base &&
                             initializer.explicitArgumentCount == 0 &&
                             initializer.arguments.size() == 1;
                    });
  }
  expect(hirImplicitBaseDefault && mirImplicitBaseDefault,
         "implicit base construction should retain its expanded default and "
         "zero explicit arguments in HIR and MIR");

  std::size_t mirDefaultInputs = 0;
  const auto inspectMirBody = [&](const lang::MirBody &body) {
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputRole == lang::MirCallInputRole::Argument &&
            instruction.defaultArgument) {
          ++mirDefaultInputs;
        }
      }
    }
  };
  inspectMirBody(frontend.mir.module());
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    inspectMirBody(function.body);
  }
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    inspectMirBody(constructor.body);
  }
  if (mirDefaultInputs < 6) {
    std::cerr << "Default-parameter MIR inputs=" << mirDefaultInputs << '\n';
  }
  expect(mirDefaultInputs >= 6,
         "verified MIR should preserve caller-expanded default-argument "
         "provenance on prepared call inputs");

  lang::MirProgram forgedDefaultSuffix = frontend.mir;
  bool forged = false;
  auto &forgedFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      forgedDefaultSuffix.functionInstances());
  for (lang::MirFunctionInstance &function : forgedFunctions) {
    std::unordered_map<lang::HirValueId, std::vector<lang::MirInstruction *>>
        callInputs;
    for (lang::MirBlock &block : function.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputRole == lang::MirCallInputRole::Argument) {
          callInputs[instruction.callSite].push_back(&instruction);
        }
      }
    }
    for (auto &[_, inputs] : callInputs) {
      std::ranges::sort(inputs, {}, [](const lang::MirInstruction *input) {
        return input->callInputIndex;
      });
      if (inputs.size() >= 2 &&
          std::none_of(inputs.begin(), inputs.end(),
                       [](const lang::MirInstruction *input) {
                         return input->defaultArgument;
                       })) {
        inputs.front()->defaultArgument = true;
        forged = true;
        break;
      }
    }
    if (forged) {
      break;
    }
  }
  const lang::MirVerificationResult forgedVerification =
      lang::verifyMirProgram(forgedDefaultSuffix);
  expect(forged && !forgedVerification.valid() &&
             std::any_of(forgedVerification.errors.begin(),
                         forgedVerification.errors.end(),
                         [](const lang::MirVerificationError &error) {
                           return error.message.find(
                                      "trailing parameter suffix") !=
                                  std::string::npos;
                         }),
         "the MIR verifier should reject forged non-trailing default-input "
         "provenance");

  const std::string nonTrailingSource =
      "int invalid(int optional = 1, int required) { return required; }";
  const lang::FrontendResult nonTrailing =
      lang::Frontend().analyze("default-non-trailing.gti", nonTrailingSource);
  const lang::Diagnostic *nonTrailingDiagnostic =
      findDiagnosticByCode(nonTrailing.diagnostics, "GTI-S2077");
  const std::size_t required = nonTrailingSource.find("required");
  const bool nonTrailingRejected =
      !nonTrailing.canGenerateCode() && nonTrailingDiagnostic != nullptr &&
      std::filesystem::path(nonTrailingDiagnostic->primary.source).filename() ==
          "default-non-trailing.gti" &&
      nonTrailingDiagnostic->primary.start == required &&
      nonTrailingDiagnostic->primary.end ==
          required + std::string("required").size() &&
      hasRelatedDiagnostic(nonTrailing.diagnostics,
                           "default-argument suffix") &&
      hasDiagnosticHint(nonTrailing.diagnostics, "Move required parameters");
  if (!nonTrailingRejected && nonTrailingDiagnostic != nullptr) {
    std::cerr << "Non-trailing diagnostic span="
              << nonTrailingDiagnostic->primary.start << ".."
              << nonTrailingDiagnostic->primary.end
              << " source=" << nonTrailingDiagnostic->primary.source << '\n';
  }
  expect(nonTrailingRejected,
         "GTI-S2077 should point at a required parameter after the default "
         "suffix and explain the repair");

  const lang::FrontendResult parameterReference = lang::Frontend().analyze(
      "default-parameter-reference.gti",
      "int dependent(int first, int second = first) { return second; }");
  expect(!parameterReference.canGenerateCode() &&
             hasDiagnosticCode(parameterReference.diagnostics, "GTI-S2077") &&
             hasDiagnostic(parameterReference.diagnostics,
                           "cannot reference a parameter") &&
             hasDiagnosticHint(parameterReference.diagnostics, "function body"),
         "defaults should resolve in declaration scope without parameter "
         "bindings");

  const lang::FrontendResult receiverReference = lang::Frontend().analyze(
      "default-receiver-reference.gti",
      "class Item { public: int read(int value = this.read()) { return "
      "value; } };");
  expect(!receiverReference.canGenerateCode() &&
             hasDiagnosticCode(receiverReference.diagnostics, "GTI-S2077") &&
             hasDiagnostic(receiverReference.diagnostics, "cannot use 'this'"),
         "method defaults should not acquire an implicit receiver");

  const lang::FrontendResult wrongType = lang::Frontend().analyze(
      "default-type.gti", "int invalid(int value = true) { return value; }");
  expect(
      !wrongType.canGenerateCode() &&
          hasDiagnosticCode(wrongType.diagnostics, "GTI-S2077") &&
          hasDiagnostic(wrongType.diagnostics, "parameter requires 'int32_t'"),
      "a default expression should be checked against its concrete "
      "parameter type");

  const lang::FrontendResult recursive = lang::Frontend().analyze(
      "default-cycle.gti",
      "int recurse(int value = recurse()) { return value; }");
  expect(!recursive.canGenerateCode() &&
             hasDiagnosticCode(recursive.diagnostics, "GTI-S2077") &&
             hasDiagnostic(recursive.diagnostics, "recursive expansion") &&
             hasDiagnosticHint(recursive.diagnostics, "Break the cycle"),
         "recursive default expansion should be rejected before HIR "
         "lowering");

  const lang::FrontendResult ambiguous = lang::Frontend().analyze(
      "default-overload.gti",
      "int choose(int value) { return value; }\n"
      "int choose(int value, int fallback = 1) { return value + fallback; }\n"
      "int main() { return choose(1); }\n");
  expect(!ambiguous.canGenerateCode() &&
             hasDiagnosticCode(ambiguous.diagnostics, "GTI-S2013") &&
             hasDiagnostic(ambiguous.diagnostics, "2 overloads exactly match"),
         "defaults should affect viability without silently changing exact "
         "overload ranking");

  const lang::FrontendResult ambiguousImplicitBase = lang::Frontend().analyze(
      "default-base-ambiguity.gti",
      "class Base { public: Base(int value = 1) {} Base(bool value = true) "
      "{} };\n"
      "class Derived : public Base { public: Derived() {} };\n"
      "int main() { return 0; }\n");
  expect(
      !ambiguousImplicitBase.canGenerateCode() &&
          hasDiagnosticCode(ambiguousImplicitBase.diagnostics, "GTI-S2013") &&
          hasDiagnostic(ambiguousImplicitBase.diagnostics,
                        "Implicit construction of base") &&
          hasDiagnosticHint(ambiguousImplicitBase.diagnostics,
                            "explicit base initializer") &&
          hasRelatedDiagnostic(ambiguousImplicitBase.diagnostics,
                               "Candidate: Base(int") &&
          hasRelatedDiagnostic(ambiguousImplicitBase.diagnostics,
                               "Candidate: Base(bool"),
      "implicit base construction should reject multiple constructors "
      "that accept zero explicit arguments");

  const lang::FrontendResult uninferred =
      lang::Frontend().analyze("default-generic-inference.gti",
                               "T make<T>(T value = 1) { return value; }\n"
                               "int main() { return make(); }\n");
  expect(!uninferred.canGenerateCode() &&
             hasDiagnosticCode(uninferred.diagnostics, "GTI-S2023") &&
             hasDiagnostic(uninferred.diagnostics,
                           "provide explicit type arguments"),
         "an omitted default should not infer a generic argument");

  struct UnsupportedDefaultCase {
    std::string name;
    std::string source;
    std::string message;
  };
  const std::array<UnsupportedDefaultCase, 7> unsupported{{
      {"default-extern-c.gti", "extern \"C\" { int native(int value = 1); }",
       "extern \"C\" declarations"},
      {"default-operator.gti",
       "class Number { public: int operator[](uint64_t value = 1) { return "
       "int(value); "
       "} };",
       "operator declarations"},
      {"default-virtual.gti",
       "class Base { public: virtual int inspect(int value = 1) { return "
       "value; } };",
       "virtual, override, or interface methods"},
      {"default-lambda.gti",
       "int main() { auto fn = [](int value = 1) -> int { return value; }; "
       "return 0; }",
       "lambda parameters"},
      {"default-pack.gti", "void relay<Args...>(Args... values = 1) {}",
       "parameter pack"},
      {"default-reference.gti", "int inspect(int& value = 1) { return value; }",
       "owned or plain value"},
      {"default-main.gti", "int main(int value = 0) { return value; }",
       "the main entry point"},
  }};
  for (const UnsupportedDefaultCase &test : unsupported) {
    const lang::FrontendResult result =
        lang::Frontend().analyze(test.name, test.source);
    const bool rejected = !result.canGenerateCode() &&
                          hasDiagnosticCode(result.diagnostics, "GTI-S2077") &&
                          hasDiagnostic(result.diagnostics, test.message);
    if (!rejected) {
      for (const lang::Diagnostic &diagnostic : result.diagnostics) {
        std::cerr << test.name << " [" << diagnostic.code
                  << "]: " << diagnostic.message << '\n';
      }
    }
    expect(rejected,
           "GTI-S2077 should reject the unsupported default context in " +
               test.name);
  }

  const std::string formatted = lang::Formatter().format(
      "int choose(int required,int fallback=2){return required+fallback;}");
  expect(formatted == R"(int choose(int required, int fallback = 2) {
  return required + fallback;
}
)" && lang::Formatter().format(formatted) == formatted,
         "the formatter should space default parameters and remain "
         "idempotent");
}

void testFunctionContracts() {
  const std::string prefix = R"(
[[noreturn]] void stop() { while (true) {} }
[[noreturn]] int stop_number() { while (true) {} }
[[noreturn]] bool stop_test() { while (true) {} }
int identity(int value) { return value; }
)";
  const std::vector<std::pair<std::string, std::string>> accepted{
      {"direct", "[[noreturn]] void run() { stop(); }"},
      {"unused-terminal-result", "[[noreturn]] void run() { stop_number(); }"},
      {"grouped", "[[noreturn]] void run() { (stop()); }"},
      {"return-expression",
       "[[noreturn]] int run() { return stop_number() + 1; }"},
      {"initializer", "[[noreturn]] void run() { int value = stop_number(); }"},
      {"argument",
       "[[noreturn]] int run() { return identity(stop_number()); }"},
      {"if-condition", "[[noreturn]] void run() { if (stop_test()) {} }"},
      {"while-condition", "[[noreturn]] void run() { while (stop_test()) {} }"},
      {"do-condition",
       "[[noreturn]] void run() { do {} while (stop_test()); }"},
      {"for-condition", "[[noreturn]] void run() { for (; stop_test();) {} }"},
      {"switch-condition", "[[noreturn]] void run() { switch (stop_number()) { "
                           "default: break; } }"},
      {"conditional", "[[noreturn]] int run(bool select) { return select ? "
                      "stop_number() : stop_number(); }"},
      {"exhaustive-arms", "[[noreturn]] void run(int value) { switch (value) { "
                          "case 1: stop(); default: stop(); } }"},
      {"generic", "[[noreturn]] void run<T>(T value) { stop(); } void "
                  "instantiate() { run(1); }"},
      {"tracked-method",
       "struct Guard { [[track_caller]] void check() { stop(); } };"},
      {"tracked-direct-item",
       "[[track_caller]] int compute(int value) { return value; } int run() { "
       "auto target = compute; return target(1); }"},
  };
  for (const auto &[name, body] : accepted) {
    const auto result =
        lang::Frontend().analyze("function-contract-" + name + ".gti",
                                 prefix + body + "\nint main() { return 0; }",
                                 {standardLibraryPrelude()});
    if (!result.canGenerateCode()) {
      for (const auto &diagnostic : result.diagnostics) {
        std::cerr << name << " [" << diagnostic.code
                  << "]: " << diagnostic.message << '\n';
      }
    }
    expect(result.canGenerateCode(),
           "general terminal flow must accept " + name);
  }

  const std::vector<std::pair<std::string, std::string>> rejected{
      {"fallthrough", "[[noreturn]] void run() {}"},
      {"normal-return", "[[noreturn]] void run() { return; }"},
      {"conditional-return",
       "[[noreturn]] void run(bool value) { if (value) return; stop(); }"},
      {"short-circuit", "[[noreturn]] void run(bool value) { bool result = "
                        "value && stop_test(); }"},
      {"unselected-branch", "[[noreturn]] int run(bool value) { return value ? "
                            "1 : stop_number(); }"},
      {"loop-break", "[[noreturn]] void run() { while (true) { break; } }"},
      {"ordinary-enum",
       "enum class State { first, second }; [[noreturn]] void run(State value) "
       "{ switch (value) { case State::first: stop(); case State::second: "
       "stop(); } }"},
      {"duplicate", "[[noreturn, noreturn]] void run() { stop(); }"},
      {"tracked-operator", "struct Index { [[track_caller]] int "
                           "operator[](std::size_t index) { return 0; } };"},
      {"terminal-operator",
       "struct Action { [[noreturn]] void operator()() { stop(); } };"},
      {"unknown", "[[noreturn, surprise]] void run() { stop(); }"},
      {"bodyless", "[[track_caller]] void run();"},
      {"virtual",
       "class Guard { public: [[track_caller]] virtual void check() {} };"},
      {"native", "extern \"C\" { [[track_caller]] void run(); }"},
  };
  for (const auto &[name, body] : rejected) {
    const auto result =
        lang::Frontend().analyze("function-contract-rejected-" + name + ".gti",
                                 prefix + body + "\nint main() { return 0; }",
                                 {standardLibraryPrelude()});
    expect(!result.canGenerateCode() &&
               hasDiagnosticCode(result.diagnostics, "GTI-S2099"),
           "function contract diagnostic must reject " + name);
  }

  const std::string returned = "[[noreturn]] void run() { return; }";
  const auto trackedEntry = lang::Frontend().analyze(
      "tracked-entry.gti", "[[track_caller]] int main() { return 0; }",
      {standardLibraryPrelude()});
  expect(!trackedEntry.canGenerateCode() &&
             hasDiagnosticCode(trackedEntry.diagnostics, "GTI-S2099") &&
             hasDiagnostic(trackedEntry.diagnostics, "hosted entry point"),
         "the hosted entry point cannot demand a nonexistent GTI caller");
  const auto invalidBody = lang::Frontend().analyze(
      "invalid-terminal-body.gti",
      "[[noreturn]] void run() { missing(); }\nint main() { return 0; }",
      {standardLibraryPrelude()});
  expect(!invalidBody.canGenerateCode() &&
             !hasDiagnosticCode(invalidBody.diagnostics, "GTI-S2099"),
         "an invalid terminal body must retain its original error without a "
         "speculative flow cascade");
  const auto invalid = lang::Frontend().analyze(
      "function-contract-span.gti", returned + "\nint main() { return 0; }",
      {standardLibraryPrelude()});
  const auto *diagnostic =
      findDiagnosticByCode(invalid.diagnostics, "GTI-S2099");
  expect(diagnostic != nullptr &&
             diagnostic->primary.start == returned.find("return;") &&
             diagnostic->primary.end == returned.find("return;") + 6 &&
             diagnostic->fixes.empty() &&
             diagnostic->group == "function-contract",
         "terminal normal-return diagnostic must point at the actual return "
         "without inventing a fix");

  const auto native =
      lang::Frontend().analyze("tracked-native-callback.gti", R"(
using Callback = () -> void;
extern "C" { void accept(Callback callback); }
[[track_caller]] void callback() {}
int main() { unsafe { accept(callback); } return 0; }
)",
                               {standardLibraryPrelude()});
  expect(!native.canGenerateCode() &&
             hasDiagnosticCode(native.diagnostics, "GTI-S2099") &&
             hasDiagnostic(native.diagnostics, "native callback"),
         "native callback conversion must not erase hidden caller-location "
         "transport");

  const auto owned = lang::Frontend().analyze("tracked-owned-callback.gti", R"(
#include <std/functional>
[[track_caller]] int callback(int value) { return value; }
int main() { std::function<int(int)> erased{callback}; return erased(1); }
)",
                                              {standardLibraryPrelude()}, {},
                                              {standardLibraryRoot()});
  expect(owned.canGenerateCode(), "source-defined callable wrappers retain "
                                  "exact tracked function identity");
  const auto customOwned =
      lang::Frontend().analyze("tracked-custom-wrapper.gti", R"(
[[track_caller]] int callback(int value) { return value; }
struct Holder<F> {
  F callback;
  Holder(F value) : callback(value) {}
  int invoke(int value) { return callback(value); }
};
Holder<F> retain<F>(F value) { return Holder<F>(value); }
int main() { auto holder = retain(callback); return holder.invoke(1); }
)",
                               {standardLibraryPrelude()});
  expect(customOwned.canGenerateCode(),
         "unrelated source-defined owning wrappers retain exact function "
         "identity too");

  const std::string unsafeSource = R"(
[[noreturn, track_caller]] unsafe void halt() { while (true) {} }
void invoke() { unsafe { halt(); } }
int main() { return 0; }
)";
  const auto unsafeContract = lang::Frontend().analyze(
      "unsafe-function-contract.gti", unsafeSource, {standardLibraryPrelude()});
  expect(unsafeContract.canGenerateCode(),
         "terminal and caller-tracked contracts can precede an unsafe "
         "namespace function attribute");
  const lang::LanguageQueries queries;
  const auto unsafeUnit = unsafeContract.sourceGraph.entryUnit();
  const std::string expectedSignature =
      "[[noreturn]] [[track_caller]] unsafe void halt()";
  for (const auto offset :
       {unsafeSource.find("halt()"), unsafeSource.rfind("halt();")}) {
    const auto hover = queries.hover(unsafeContract, unsafeUnit, offset + 1);
    expect(hover && hover->signature == expectedSignature,
           "declaration and selected-call queries must print contract "
           "contract attributes before the unsafe declaration keyword");
    if (hover) {
      const auto roundTrip = lang::Frontend().analyze(
          "unsafe-function-contract-signature.gti",
          hover->signature + " { while (true) {} }\nint main() { return 0; }\n",
          {standardLibraryPrelude()});
      expect(roundTrip.canGenerateCode(),
             "a copied contract-bearing query signature must parse and "
             "retain valid semantics");
    }
  }

  for (const std::string attribute : {"[[noreturn", "[[track_caller]"}) {
    const std::string malformed =
        attribute + " void broken() {}\nint recovered() { return 0; }\n";
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(malformed, "incomplete-contract.gti"));
    const auto recovered = parser.parse();
    const auto *error = findDiagnosticByCode(parser.errors(), "GTI-P0001");
    const auto unexpected = malformed.find("void");
    expect(parser.hadError() && error != nullptr &&
               error->phase == lang::DiagnosticPhase::Parsing &&
               error->severity == lang::DiagnosticSeverity::Error &&
               error->message.find("after declaration attributes") !=
                   std::string::npos &&
               error->primary.source.ends_with("incomplete-contract.gti") &&
               error->primary.start == unexpected &&
               error->primary.end == unexpected + 4 &&
               error->fixes.size() == 1 &&
               error->fixes.front().span.source == error->primary.source &&
               error->fixes.front().span.start == unexpected &&
               error->fixes.front().span.end == unexpected &&
               error->fixes.front().replacement == "]" &&
               findTopLevelFunction(recovered, "recovered") != nullptr,
           "incomplete function attributes retain a declaration-neutral "
           "parser error, precise delimiter insertion and recovered functions");
    if (attribute.ends_with("]") && error != nullptr &&
        error->fixes.size() == 1) {
      std::string repaired = malformed;
      repaired.insert(error->fixes.front().span.start,
                      error->fixes.front().replacement);
      const auto fixed = lang::Frontend().analyze(
          "repaired-contract.gti", repaired + "int main() { return 0; }\n",
          {standardLibraryPrelude()});
      expect(fixed.canGenerateCode(),
             "the supplied missing-bracket fix restores the complete "
             "declaration without changing its function contract");
    }
  }

  const std::string formatted = lang::Formatter().format(
      "[[noreturn,track_caller]] void stop(){while(true){}}");
  expect(formatted.find("[[noreturn, track_caller]]") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter preserves and spaces general function-contract "
         "attributes");
}

void testStructuralClassConstraints() {
  const std::string declarations = R"(
mut int closed = 0;
struct Counter {
  unsafe int read() { return 7; }
  unsafe void close() { closed += 1; }
};
concept readable<T> = requires(T& source) {
  unsafe { source.read() } -> int;
};
concept closable<T> = readable<T> && requires(T& source) {
  unsafe { source.close() } -> void;
};
int observe<T>(T& source) requires readable<T> {
  unsafe { return source.read(); }
}
class Monitor<closable T> {
  T& source;
  mut int initial = 0;
public:
  Monitor(T& source) : source(source) {
    unsafe { this.initial = this.source.read(); }
  }
  int read() { return observe(this.source); }
  ~Monitor() { unsafe { this.source.close(); } }
};
class Outer<closable T> {
  Monitor<T> inner;
public:
  Outer(Monitor<T> inner) : inner(std::move(inner)) {}
  int read() { return this.inner.read(); }
};
class Unused<closable T> { int value = 0; };
class Defaulted<closable T = Counter> { int value = 0; };
class Packed<closable T...> { int value = 0; };
)";
  const auto analyze = [&](const std::string &source) {
    return lang::Frontend().analyze("structural-class-contract.gti", source,
                                    {standardLibraryPrelude()});
  };
  const auto valid = analyze(declarations + R"(
int main() {
  Counter counter{};
  Defaulted<> defaulted{};
  Packed<Counter, Counter> packed{};
  { Monitor<Counter> inner{counter};
    Outer<Counter> monitor{std::move(inner)};
    if (monitor.read() != 7) { return 1; } }
  return closed - 1;
}
)");
  if (!valid.canGenerateCode())
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "unary class contracts reach constructors, methods, destructors, "
         "forwarding and nested class arguments");
  if (valid.canGenerateCode())
    expect(!gti_test::emitCppText(valid).empty(),
           "concrete structural class contract emits from verified MIR");
  const auto formatted = lang::Formatter().format(
      declarations +
      "int main() { Counter counter{}; Monitor<Counter> value{counter}; "
      "return value.read() - 7; }");
  expect(analyze(formatted).canGenerateCode(),
         "existing generic annotation syntax formats without losing facts");

  for (const auto &suffix :
       {"struct Missing {}; int main() { Unused<Missing> value{}; return 0; }",
        "struct Wrong { int read(){return 1;} int close(){return 0;} }; "
        "int main() { Unused<Wrong> value{}; return 0; }",
        "class Hidden { int read(){return 1;} void close(){} }; "
        "int main() { Unused<Hidden> value{}; return 0; }",
        "struct Missing {}; int main() { Packed<Counter, Missing> value{}; "
        "return 0; }",
        "struct Missing {}; class InvalidDefault<closable T=Missing> {}; "
        "int main() { InvalidDefault<> value{}; return 0; }"}) {
    const auto rejected = analyze(declarations + suffix);
    expect(!rejected.canGenerateCode() &&
               hasDiagnosticCode(rejected.diagnostics, "GTI-S2029") &&
               !hasDiagnosticCode(rejected.diagnostics, "GTI-B0001"),
           "class argument contract fails in semantics even without a "
           "selected member body");
    expect(std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                       [](const auto &diagnostic) {
                         return diagnostic.code == "GTI-S2029" &&
                                !diagnostic.related.empty();
                       }),
           "class constraint failure retains the required member declaration");
  }
  for (const auto &source :
       {declarations + "class Bad<readable T> { T& source; public: "
                       "Bad(T& value):source(value){} "
                       "~Bad(){ unsafe { this.source.close(); } } }; "
                       "int main(){return 0;}",
        declarations + "class Bad<closable T> { T& source; public: "
                       "Bad(T& value):source(value){} "
                       "~Bad(){ this.source.close(); } }; "
                       "int main(){return 0;}",
        std::string("concept pair<T,U> = std::copyable<T> && "
                    "std::copyable<U>; class Bad<pair T> { int value=0; }; "
                    "int main(){return 0;}")}) {
    const auto rejected = analyze(source);
    expect(!rejected.canGenerateCode() &&
               !hasDiagnosticCode(rejected.diagnostics, "GTI-B0001"),
           "class annotations do not invent members, waive unsafe blocks "
           "or admit non-unary concepts");
  }
}

void testDependentRequiresArgumentAdmission() {
  const std::string prefix = R"(
class Box<std::copyable T> {
  T value;
public:
  Box(T value) : value(value) {}
  T get() { return this.value; }
};
concept boxed_int<B> = requires(B& box) { { box.get() } -> int; };
)";
  const auto analyze = [&](std::string source) {
    const auto root =
        std::filesystem::path(__FILE__).parent_path().parent_path();
    return lang::Frontend().analyze("dependent-requires.gti", prefix + source,
                                    {root / "stdlib/prelude.gti"}, {},
                                    {root / "stdlib"});
  };
  for (const std::string clause : {"std::copyable<T> && boxed_int<Box<T>>",
                                   "boxed_int<Box<T>> && std::copyable<T>",
                                   "(std::copyable<T> && boxed_int<Box<T>>) || "
                                   "(boxed_int<Box<T>> && std::copyable<T>)"}) {
    const auto result = analyze(
        "int read<T>(Box<T>& box) requires " + clause +
        " { return box.get(); }\n"
        "int main() { Box<int> box{7}; return read(box) == 7 ? 0 : 1; }");
    expect(result.canGenerateCode() && result.diagnostics.empty(),
           "compound clause arguments use complete guaranteed facts "
           "independent of conjunct order");
  }
  for (const std::string clause :
       {"boxed_int<Box<T>>",
        "(std::copyable<T> && boxed_int<Box<T>>) || boxed_int<Box<T>>"}) {
    const auto result =
        analyze("int read<T>(Box<T>& box) requires " + clause +
                " { return box.get(); }\nint main() { return 0; }");
    const auto *failure = findDiagnosticByCode(result.diagnostics, "GTI-S2029");
    expect(!result.canGenerateCode() && failure != nullptr &&
               failure->message.find("Type 'T'") != std::string::npos &&
               failure->message.find("type parameter") == std::string::npos &&
               failure->fixes.empty(),
           "a branch-local or absent copyability fact cannot admit Box<T>; "
           "diagnostics name T");
  }
  const auto concrete = analyze(R"(
int read<T>(Box<T>& box) requires std::copyable<T> && boxed_int<Box<T>> { return box.get(); }
int main() { Box<bool> box{true}; return read(box); }
)");
  expect(
      !concrete.canGenerateCode() &&
          hasDiagnosticCode(concrete.diagnostics, "GTI-S2029") &&
          !hasDiagnosticCode(concrete.diagnostics, "GTI-B0001"),
      "concrete arguments recheck the exact structural member result contract");
  const auto unknown = analyze(R"(
int read<T>(T value) requires std::copyable<NoSuchType> { return 0; }
int main() { return 0; }
)");
  expect(!unknown.canGenerateCode() &&
             hasDiagnosticCode(unknown.diagnostics, "GTI-S2000"),
         "pure first-pass clause formation must not lose unknown argument "
         "diagnostics");
  const auto closed = analyze(R"(
class Immovable {
public:
  Immovable() = default;
  Immovable(Immovable& other) = delete;
  Immovable(Immovable&& other) = delete;
};
int read<T>(T value) requires std::copyable<Immovable> && boxed_int<Box<Immovable>> { return 0; }
int main() { return read(1); }
)");
  expect(!closed.canGenerateCode() &&
             hasDiagnosticCode(closed.diagnostics, "GTI-S2029"),
         "concrete viability cannot replace a closed class's actual lifecycle "
         "capabilities");
  // Primitive capabilities are complete even during declaration registration;
  // unlike the later-resolved class lifecycle above, this isolates formation
  // of a closed compound argument inside the provisional clause scope.
  const auto closedPrimitive = analyze(R"(
class IntegerBox<std::integral T> { public: int get() { return 0; } };
int read<T>(T value) requires std::integral<bool> && boxed_int<IntegerBox<bool>> { return 0; }
int main() { return 0; }
)");
  expect(!closedPrimitive.canGenerateCode() &&
             hasDiagnosticCode(closedPrimitive.diagnostics, "GTI-S2029"),
         "a false closed primitive assertion cannot admit another closed "
         "argument type");
}

constexpr NamedTest kTests[] = {
    {"testDependentRequiresArgumentAdmission",
     testDependentRequiresArgumentAdmission},
    {"testStructuralClassConstraints", testStructuralClassConstraints},
    {"testFunctionContracts", testFunctionContracts},
    {"testContainerScaffoldSurfaces", testContainerScaffoldSurfaces},
    {"testMovedLoopLocalRedeclarations", testMovedLoopLocalRedeclarations},
    {"testInferredVoidGenericArguments", testInferredVoidGenericArguments},
    {"testTrustedIntrinsicDeclarations", testTrustedIntrinsicDeclarations},
    {"testThisReceiverKeyword", testThisReceiverKeyword},
    {"testClassesStructsAndAccess", testClassesStructsAndAccess},
    {"testMutableFieldGroups", testMutableFieldGroups},
    {"testConstructorsAndReceiverMutability",
     testConstructorsAndReceiverMutability},
    {"testInheritanceAndInterfaces", testInheritanceAndInterfaces},
    {"testInheritedGenericTargetInstances",
     testInheritedGenericTargetInstances},
    {"testDirectBraceConstruction", testDirectBraceConstruction},
    {"testRecursiveClassLayouts", testRecursiveClassLayouts},
    {"testNamedGenerics", testNamedGenerics},
    {"testNativeGenericCarrierClosure", testNativeGenericCarrierClosure},
    {"testUninstantiatedGenericSignatureClosure",
     testUninstantiatedGenericSignatureClosure},
    {"testDependentCarrierRepresentationClosure",
     testDependentCarrierRepresentationClosure},
    {"testExactQualifiedClassSpecializations",
     testExactQualifiedClassSpecializations},
    {"testContextualNumericLiterals", testContextualNumericLiterals},
    {"testConstrainedGenerics", testConstrainedGenerics},
    {"testMoveConstructionCapability", testMoveConstructionCapability},
    {"testFiniteWrapperTraits", testFiniteWrapperTraits},
    {"testConcurrencyTypeCapabilities", testConcurrencyTypeCapabilities},
    {"testSourceDefinedConcepts", testSourceDefinedConcepts},
    {"testMemberRequirementConcepts", testMemberRequirementConcepts},
    {"testExactMemberSignatureConcepts", testExactMemberSignatureConcepts},
    {"testConstraintFailureProvenance", testConstraintFailureProvenance},
    {"testRequiresDisjunction", testRequiresDisjunction},
    {"testStructuralRequiresAndAccumulate",
     testStructuralRequiresAndAccumulate},
    {"testDomainValueOperatorCapabilities",
     testDomainValueOperatorCapabilities},
    {"testCallableConceptCapabilities", testCallableConceptCapabilities},
    {"testSymbolicAutoLocalBinding", testSymbolicAutoLocalBinding},
    {"testConstructorLocalTypeGenerics", testConstructorLocalTypeGenerics},
    {"testPackSpecialMemberConstruction", testPackSpecialMemberConstruction},
    {"testValueGenerics", testValueGenerics},
    {"testDependentTypeDefaultConstraints",
     testDependentTypeDefaultConstraints},
    {"testContextualArrayArguments", testContextualArrayArguments},
    {"testVariadicGenerics", testVariadicGenerics},
    {"testExactFunctionOverloadsAndConversions",
     testExactFunctionOverloadsAndConversions},
    {"testFixedArrays", testFixedArrays},
    {"testFixedArrayDynamicWriteFailureEdge",
     testFixedArrayDynamicWriteFailureEdge},
    {"testFixedArrayChainedDynamicIndex", testFixedArrayChainedDynamicIndex},
    {"testLocalTypeInference", testLocalTypeInference},
    {"testStructuredBindings", testStructuredBindings},
    {"testTypeAliases", testTypeAliases},
    {"testNamespacesAndAliases", testNamespacesAndAliases},
    {"testExternCInterop", testExternCInterop},
    {"testScopedEnums", testScopedEnums},
    {"testDefaultParameters", testDefaultParameters},
};

} // namespace

int main(int argc, char **argv) {
  lang::installCrashHandlers("gti_compiler_declarations_tests");
  return runNamedTests(argc, argv, kTests, "declarations");
}
