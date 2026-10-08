#include "compiler_test_fixture.h"

namespace {

// An unrelated user-defined text carrier: the generalized relay rule is
// structural, never recognition of std::string_view.
const std::string declarations = R"gti(
#include <std/memory>
#include <std/string>
#include <std/vector>
class Token {
  std::string_view text;
  int kind;
public:
  Token(std::string_view text, int kind) : text(text), kind(kind) {}
  std::string_view view() { return this.text; }
  int category() { return this.kind; }
};
class Line {
  Token token;
public:
  Line(Token token) : token(token) {}
  std::string_view text() { return this.token.view(); }
};
std::string_view tail(std::string_view text) {
  return text.substr(std::size_t(1));
}
std::string_view first_word(std::string& owner) {
  return owner.view().substr(std::size_t(0), owner.view().find(' '));
}
)gti";

lang::FrontendResult analyze(const std::string &name, const std::string &body) {
  return lang::Frontend().analyze(
      name, declarations + "\nint main() {\n" + body + "\n}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
}

void reportDiagnostics(const lang::FrontendResult &result) {
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}

void checkAcceptedLifetimes() {
  const std::vector<std::string> programs{
      // Literal views are ownerless; slices of them stay ownerless.
      "std::string_view text = \"hello world\";\n"
      "std::string_view word = text.substr(std::size_t(6));\n"
      "return word == \"world\" ? 0 : 1;",
      // Subviews and relays keep the original owner, not the view shell.
      "std::string owned = std::string(\"owned text\");\n"
      "std::string_view part = tail(owned.view());\n"
      "std::string_view word = first_word(owned);\n"
      "return part.size() == std::size_t(9) && word == \"owned\" ? 0 : 1;",
      // A temporary view shell can end while its derived view lives.
      "std::string owned = std::string(\"abcdef\");\n"
      "std::string_view derived = owned.view().substr(std::size_t(2));\n"
      "return derived == \"cdef\" ? 0 : 1;",
      // A user carrier relays its constructor's owner; the owner is reusable
      // after every dependent value has ended.
      "mut std::string owned = std::string(\"token\");\n"
      "{\n"
      "  Token token = Token(owned.view(), 3);\n"
      "  Line line = Line(token);\n"
      "  if (line.text() != \"token\" || token.category() != 3) { return 1; }\n"
      "}\n"
      "owned.push_back('!');\n"
      "return owned.size() == std::size_t(6) ? 0 : 1;",
      // Mutable views narrow their own descriptor without touching bytes.
      "std::string owned = std::string(\"  padded  \");\n"
      "mut std::string_view view = owned.view();\n"
      "view.remove_prefix(std::size_t(2));\n"
      "view.remove_suffix(std::size_t(2));\n"
      "return view == \"padded\" ? 0 : 1;",
      // Owner-backed iteration relays the storage owner through the iterator.
      "std::string owned = std::string(\"a-l-l\");\n"
      "mut int count = 0;\n"
      "for (char c : owned.view()) { if (c == 'l') { count++; } }\n"
      "return count == 2 ? 0 : 1;",
      // An empty subview keeps a well-defined, dependency-carrying value.
      "std::string owned = std::string(\"abc\");\n"
      "std::string_view empty = owned.view().substr(std::size_t(3));\n"
      "return empty.empty() ? 0 : 1;",
      // A method on a temporary view takes the view by value; a result that
      // holds no text leaves the owner free afterwards.
      "mut std::string owned = std::string(\"hello world\");\n"
      "bool starts = owned.view().substr(std::size_t(0), std::size_t(5)) == "
      "\"hello\";\n"
      "owned.push_back('!');\n"
      "return starts ? 0 : 1;",
  };
  for (const auto &program : programs) {
    const auto result = analyze("string-view-accepted.gti", program);
    expect(result.canGenerateCode() &&
               lang::verifyMirProgram(result.mir).valid(),
           "source-defined string_view lifetimes are accepted: " + program);
    if (!result.canGenerateCode())
      reportDiagnostics(result);
  }
}

void checkRejectedLifetimes() {
  struct Case {
    const char *source;
    const char *code;
    const char *message;
  };
  const std::vector<Case> cases{
      {"std::string owned = std::string(\"abc\");\n"
       "mut std::string_view kept = \"x\";\n"
       "kept = owned.view().substr(std::size_t(1));\n"
       "return int(kept.size());",
       "GTI-S2045", "cannot be assigned into an existing binding"},
      {"mut std::string owned = std::string(\"abc\");\n"
       "std::string_view part = owned.view().substr(std::size_t(1));\n"
       "owned.push_back('x');\n"
       "return int(part.size());",
       "GTI-S2017", ""},
      {"mut std::string owned = std::string(\"abc\");\n"
       "Token token = Token(owned.view(), 1);\n"
       "std::string moved = std::move(owned);\n"
       "return int(token.view().size());",
       "GTI-S2017", ""},
      // A temporary carrier passed by value as a receiver still relays its
      // owner to the text it returns.
      {"mut std::string owned = std::string(\"key=value\");\n"
       "std::string_view key = Token(owned.view(), 1).view();\n"
       "owned.clear();\n"
       "return int(key.size());",
       "GTI-S2017", ""},
      {"std::string owned = std::string(\"abc\");\n"
       "mut std::vector<std::string_view> views = "
       "std::vector<std::string_view>();\n"
       "views.push_back(owned.view());\n"
       "return 0;",
       "GTI-S2045", ""},
      // A heap allocation is dynamic storage: it cannot retain a view that
      // depends on an owner (make_unique forwards a parameter).
      {"std::string owned = std::string(\"abc\");\n"
       "auto boxed = std::make_unique<std::string_view>(owned.view());\n"
       "return 0;",
       "GTI-S2045", "nested in dynamic storage"},
      {"return int(\"abc\".text_capability().size());", "", ""},
  };
  for (const Case &item : cases) {
    const auto result = analyze("string-view-rejected.gti", item.source);
    const bool codeMatches =
        std::string_view(item.code).empty()
            ? !result.diagnostics.empty()
            : hasDiagnosticCode(result.diagnostics, item.code);
    const bool messageMatches = std::string_view(item.message).empty() ||
                                hasDiagnostic(result.diagnostics, item.message);
    expect(!result.canGenerateCode() && codeMatches && messageMatches,
           std::string("string_view lifetime violation is rejected: ") +
               item.source);
  }

  // Escapes and stored retention are checked at declaration boundaries.
  const std::vector<std::pair<std::string, std::string>> declarationCases{
      {"std::string_view escape() {\n"
       "  std::string owned = std::string(\"abc\");\n"
       "  return owned.view().substr(std::size_t(1));\n"
       "}\nint main() { return int(escape().size()); }\n",
       "cannot escape without one exact owner dependency"},
      {"class Holder {\n"
       "  std::string owned;\n"
       "  std::string_view view;\n"
       "public:\n"
       "  Holder(std::string_view view)\n"
       "      : owned(std::string(\"x\")), view(view) {}\n"
       "};\nint main() { return 0; }\n",
       "cannot be stored in a field"},
      {"class Two {\n"
       "  std::string_view first;\n"
       "  std::string_view second;\n"
       "public:\n"
       "  Two(std::string_view first, std::string_view second)\n"
       "      : first(first), second(second) {}\n"
       "};\n"
       "int main() {\n"
       "  std::string a = std::string(\"a\");\n"
       "  Two pair = Two(a.view(), a.view());\n"
       "  return 0;\n"
       "}\n",
       "exactly one text-carrying constructor parameter"},
      {"int main() { gti_internal::text_view raw = \"x\"; return 0; }\n",
       "unavailable to application source"},
  };
  for (const auto &[source, message] : declarationCases) {
    const auto result = lang::Frontend().analyze(
        "string-view-declaration.gti", "#include <std/string>\n" + source,
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!result.canGenerateCode() &&
               hasDiagnostic(result.diagnostics, message),
           "string_view declaration violation is rejected: " + message);
    if (result.canGenerateCode() || !hasDiagnostic(result.diagnostics, message))
      reportDiagnostics(result);
  }
}

// MIR independently verifies the slice relay: a forged program that loses
// the dependency or its origin, upgrades its access, forges a receiver
// reseat, or releases the owner early fails verification even though
// semantics accepted the source.
void checkMirRejections() {
  const auto result = analyze("string-view-mir.gti",
                              "std::string owned = std::string(\"abcdef\");\n"
                              "mut std::string_view view = owned.view();\n"
                              "view.remove_prefix(std::size_t(1));\n"
                              "return tail(view) == \"cdef\" ? 0 : 1;");
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "the string_view MIR mutation baseline verifies");
  if (!result.canGenerateCode()) {
    reportDiagnostics(result);
    return;
  }
  bool checkedSlice = false;
  bool checkedReseat = false;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::TextViewSlice ||
            !instruction.loan)
          continue;
        const lang::MirLoanId loan = *instruction.loan;
        {
          auto lost = result.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              lost.findFunctionInstance(function.id));
          for (auto &candidate : target->body.blocks)
            for (auto &mutated : candidate.instructions)
              if (mutated.id == instruction.id)
                mutated.loan.reset();
          expect(!lang::verifyMirProgram(lost).valid(),
                 "MIR rejects a slice that loses its owner dependency");
        }
        {
          auto unrooted = result.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              unrooted.findFunctionInstance(function.id));
          target->body.loans[loan - 1].source = 0;
          const auto rejected = lang::verifyMirProgram(unrooted);
          expect(std::any_of(rejected.errors.begin(), rejected.errors.end(),
                             [](const auto &error) {
                               return error.message.find("source place") !=
                                      std::string::npos;
                             }),
                 "MIR rejects a slice loan without its origin place");
        }
        {
          auto upgraded = result.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              upgraded.findFunctionInstance(function.id));
          target->body.loans[loan - 1].access = lang::AccessMode::Mutable;
          expect(!lang::verifyMirProgram(upgraded).valid(),
                 "MIR rejects a slice that upgrades its read-only access");
        }
        checkedSlice = true;
      }
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            !instruction.destination)
          continue;
        const auto *destination =
            function.body.findPlace(*instruction.destination);
        if (destination == nullptr ||
            destination->root != lang::MirPlaceRootKind::This ||
            destination->type.kind != lang::SemanticType::StringView)
          continue;
        // Forge the reseat: assign text that is not derived from the field.
        auto forged = result.mir;
        auto *target = const_cast<lang::MirFunctionInstance *>(
            forged.findFunctionInstance(function.id));
        for (auto &candidate : target->body.blocks)
          for (auto &mutated : candidate.instructions)
            if (mutated.id == instruction.id && mutated.operands.size() == 1) {
              const auto *value =
                  target->body.findValue(mutated.operands.front().value);
              for (auto &producerBlock : target->body.blocks)
                for (auto &producer : producerBlock.instructions)
                  if (value != nullptr && producer.id == value->definition)
                    producer.borrowOrigin = lang::BorrowOriginKind::None;
            }
        expect(!lang::verifyMirProgram(forged).valid(),
               "MIR rejects a receiver text reseat without its exact relay");
        checkedReseat = true;
      }
    }
  }
  // A loan derived from the view (a reborrow of its owner dependency) must
  // end before its parent. Swapping the two releases ends the parent first,
  // a premature release that only MIR loan liveness can reject.
  bool checkedRelease = false;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      const auto &instructions = block.instructions;
      for (std::size_t child = 0;
           child < instructions.size() && !checkedRelease; ++child) {
        if (instructions[child].kind != lang::MirInstructionKind::EndBorrow ||
            !instructions[child].loan)
          continue;
        const lang::MirLoanId parent =
            function.body.loans[*instructions[child].loan - 1].parent;
        if (parent == 0)
          continue;
        const auto ending = std::find_if(
            instructions.begin() + static_cast<std::ptrdiff_t>(child),
            instructions.end(), [&](const auto &item) {
              return item.kind == lang::MirInstructionKind::EndBorrow &&
                     item.loan == parent;
            });
        if (ending == instructions.end())
          continue;
        auto released = result.mir;
        auto *target = const_cast<lang::MirFunctionInstance *>(
            released.findFunctionInstance(function.id));
        auto &mutated = target->body.blocks[block.id - 1].instructions;
        std::swap(
            mutated[child].loan,
            mutated[static_cast<std::size_t>(ending - instructions.begin())]
                .loan);
        const auto rejected = lang::verifyMirProgram(released);
        expect(std::any_of(rejected.errors.begin(), rejected.errors.end(),
                           [](const auto &error) {
                             return error.message.find("is ended before its") !=
                                    std::string::npos;
                           }),
               "MIR rejects a view loan released before its derived loan");
        checkedRelease = true;
      }
    }
  }
  expect(checkedRelease, "the program exercises a derived view loan release");
  expect(checkedSlice, "the program exercises a verified text slice");
  expect(checkedReseat, "the program exercises a verified receiver reseat");
}

void checkConstantParity() {
  const auto result = lang::Frontend().analyze(
      "string-view-constexpr.gti",
      "#include <std/string>\n"
      "constexpr std::string_view path = \"mesh/player\";\n"
      "constexpr std::size_t slash = path.find('/');\n"
      "static_assert(slash == std::size_t(4));\n"
      "static_assert(path.substr(slash + std::size_t(1)) == \"player\");\n"
      "static_assert(path.starts_with(\"mesh\") && !path.ends_with(\"x\"));\n"
      "static_assert(path.compare(\"mesh/playes\") < 0);\n"
      "static_assert(path.find_last_of(\"/\") == slash);\n"
      "int main() {\n"
      "  std::string_view runtime = path;\n"
      "  return runtime.find('/') == slash ? 0 : 1;\n"
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(result.canGenerateCode(),
         "source-defined string_view members evaluate at compile time");
  if (!result.canGenerateCode())
    reportDiagnostics(result);
  const auto dynamic = lang::Frontend().analyze(
      "string-view-dynamic-constexpr.gti",
      "#include <std/string>\n"
      "int main() {\n"
      "  std::string owned = std::string(\"abc\");\n"
      "  constexpr std::size_t size = owned.view().size();\n"
      "  return int(size);\n"
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!dynamic.canGenerateCode() &&
             hasDiagnosticCode(dynamic.diagnostics, "GTI-S2057"),
         "constant evaluation of dynamically owned text stays rejected");
}

const NamedTest tests[] = {
    {"accepted-lifetimes", checkAcceptedLifetimes},
    {"rejected-lifetimes", checkRejectedLifetimes},
    {"mir-rejections", checkMirRejections},
    {"constant-parity", checkConstantParity},
};

} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, tests, "string_view");
}
