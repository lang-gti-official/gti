#include "cpp_mir_body_emitter_test_fixture.h"

namespace {
void checkReferents() {
  const auto root = std::filesystem::path(__FILE__).parent_path();
  for (const auto &relative :
       {"fixtures/borrow_return_exact_referent.gti",
        "fixtures/fallible_borrow.gti",
        "coverage/reproducers/a25-expected-ref-relay-wrong-referent.gti",
        "coverage/reproducers/a25-ref-return-from-expected-value.gti"}) {
    const auto path = root / relative;
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    expect(input.good() || input.eof(), "referent fixture is readable");
    auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(),
           "exact borrowed referent is publishable: " + std::string(relative));
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "exact referent MIR independently verifies");
    try {
      expect(!gti_test::emitCpp(frontend).contents.empty(),
             "exact referent reaches production representation");
    } catch (const std::logic_error &error) {
      expect(false, std::string("exact referent emission: ") + error.what());
    }
    const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
    std::size_t checked = 0;
    for (const auto &function : frontend.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Borrow ||
              !instruction.loan || instruction.operands.size() != 1 ||
              instruction.operands.front().place == 0 ||
              instruction.operands.front().kind != lang::MirOperandKind::Loan)
            continue;
          const auto *published = function.body.findLoan(*instruction.loan);
          const auto *retained =
              function.body.findLoan(instruction.operands.front().loan);
          if (!published || published->kind != lang::MirLoanKind::Return ||
              !retained || retained->kind != lang::MirLoanKind::Parameter)
            continue;
          ++checked;
          for (unsigned mutation = 0; mutation < 5; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &changedBlock : changed->body.blocks)
              for (auto &candidate : changedBlock.instructions) {
                if (candidate.id != instruction.id)
                  continue;
                if (mutation == 0)
                  candidate.operands.front().place = 0;
                if (mutation == 1)
                  candidate.operands.front().place = retained->source;
                if (mutation == 2)
                  candidate.operands.front().loan = published->id;
                if (mutation == 3)
                  changed->body.loans[published->id - 1].access =
                      lang::AccessMode::Mutable;
                if (mutation == 4)
                  changed->body.loans[published->id - 1].source =
                      candidate.operands.front().place;
              }
            expect(
                !lang::verifyMirProgram(malformed).valid(),
                "borrow return rejects forged address, authority, or access");
            expect(
                !lang::CppMirBodyEmitter(malformed, rows)
                     .supportsFailureBodyText(
                         {lang::MirBodyKind::Function, function.id}),
                "representation admission rejects forged borrow publication");
          }
        }
    if (std::string(relative) != "fixtures/fallible_borrow.gti")
      expect(checked != 0,
             "fixture exercises exact local reference publication");
  }
}

void checkInvalidEscapes() {
  const std::string declarations = R"gti(
enum class errc { missing };
class Item {
  mut int32_t value_;
public:
  Item(int32_t value) : value_(value) {}
  int32_t read() { return this.value_; }
  void set(int32_t value) mut { this.value_ = value; }
};
expected<Item&, errc> relay(Item& item) {
  Item& alias = item;
  return alias;
}
)gti";
  for (const auto &body :
       {"expected<Item&, errc> bad() { Item local{1}; return local; } "
        "int main() { return 0; }",
        "int main() { mut Item owner{1}; Item& alias = relay(owner).value(); "
        "owner.set(2); return alias.read(); }",
        "int main() { Item owner{1}; Item& alias = relay(owner).value(); "
        "alias.set(2); return alias.read(); }"}) {
    const auto frontend = analyzeWithStandardLibrary(
        "invalid-borrow-publication.gti", declarations + body);
    expect(
        frontend.syntaxValid && !frontend.semanticValid &&
            !frontend.canGenerateCode(),
        "local escape and writable use of a shared reference remain invalid");
    expect(std::none_of(frontend.diagnostics.begin(),
                        frontend.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           "invalid source is rejected before MIR/backend generation");
  }
}
} // namespace

int main() {
  checkReferents();
  checkInvalidEscapes();
  return failures == 0 ? 0 : 1;
}
