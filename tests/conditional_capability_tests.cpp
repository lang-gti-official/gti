#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << message << '\n';
    ++failures;
  }
}
lang::FrontendResult analyze(std::string source) {
  const auto stdlib =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("conditional-capability.gti",
                                  std::move(source), {stdlib / "prelude.gti"},
                                  {}, {stdlib});
}
constexpr std::string_view declarations = R"(
[[unsafe_transfer(T), unsafe_share(T)]] class Reviewed<T> {
  T* pointer = nullptr;
public:
  Reviewed() {}
  ~Reviewed() {}
};
[[no_transfer]] struct Affine { int id = 0;  public: Affine() = default; };
[[no_share]] struct Exclusive { int id = 0;  public: Exclusive() = default; };
enum Packet<T> { item(T value) };
enum Metadata<T> { item(int value) };
void consume<std::transferable T>(T value) {}
void observe<std::shareable T>(T& value) {}
void relay<std::transferable T>(T value) { consume(Reviewed<T>()); }
)";
} // namespace

int main() {
  const std::string source = std::string(declarations) + R"(
int main() {
  consume(Reviewed<int>());
  consume(Reviewed<Reviewed<int>>());
  consume(Reviewed<Exclusive>());
  consume(Reviewed<expected<Exclusive, int>>());
  consume(Reviewed<Packet<int>>());
  consume(Reviewed<Packet<Exclusive>>());
  consume(Reviewed<Metadata<Affine>>());
  Reviewed<Affine> shared{};
  observe(shared);
  Reviewed<Packet<Affine>> shared_packet{};
  observe(shared_packet);
  relay(1);
  return 0;
})";
  const auto good = analyze(source);
  if (!good.canGenerateCode())
    for (const auto &diagnostic : good.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(good.canGenerateCode(),
         "conditional capabilities instantiate and compose");
  const auto serialized = lang::MirPrinter().print(good.mir);
  expect(serialized.find('\0') == std::string::npos &&
             serialized.find("transfer-policy=0") != std::string::npos &&
             serialized.find("share-policy=0") != std::string::npos,
         "conditional policy enum identities serialize as text, not bytes");
  bool concreteInt = false;
  for (const auto &instance : good.hir.classInstances()) {
    if (instance.source && instance.source->name().lexeme == "Reviewed" &&
        instance.typeArguments ==
            std::vector<lang::SemanticType>{lang::SemanticType::Int32}) {
      concreteInt = instance.traits.transferCapable &&
                    instance.traits.shareCapable &&
                    instance.transferCondition == lang::SemanticType::Int32 &&
                    instance.shareCondition == lang::SemanticType::Int32;
    }
  }
  expect(concreteInt, "HIR preserves concrete conditional capability facts");
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto forged = good.mir;
    bool found = false;
    for (auto &instance : const_cast<std::vector<lang::MirClassInstance> &>(
             forged.classInstances())) {
      if (instance.transferCondition != lang::SemanticType::Int32)
        continue;
      found = true;
      if (mutation == 0)
        instance.traits.transferCapable = false;
      if (mutation == 1)
        instance.transferCondition.reset();
      if (mutation == 2)
        instance.transferPolicy =
            lang::ConcurrencyCapabilityPolicy::UnsafeAsserted;
      break;
    }
    expect(found && !lang::verifyMirProgram(forged).valid(),
           "MIR rejects a forged conditional capability or erased condition");
  }
  for (const char *statement :
       {"consume(Reviewed<Affine>());",
        "consume(Reviewed<Reviewed<Affine>>());", "consume(Reviewed<int*>());",
        "consume(Reviewed<expected<Affine, int>>());",
        "consume(Reviewed<Packet<Affine>>());",
        "Reviewed<Packet<Exclusive>> value{}; observe(value);",
        "Reviewed<Exclusive> value{}; observe(value);",
        "Reviewed<Reviewed<Exclusive>> value{}; observe(value);"}) {
    const auto bad = analyze(std::string(declarations) + "int main() { " +
                             statement + " return 0; }");
    expect(!bad.canGenerateCode() &&
               std::any_of(bad.diagnostics.begin(), bad.diagnostics.end(),
                           [](const auto &d) { return d.code == "GTI-S2029"; }),
           "conditional assertion must not erase a payload capability denial");
  }
  // The public source owner must retain the old payload-dependent capability
  // contract without a compiler-recognized storage type. These are separate
  // clients of the general Reviewed<T> assertions above, not replacements.
  const std::string storageDeclarations =
      "import std.memory;\nimport std.resource;\n" + std::string(declarations);
  const auto storage = analyze(storageDeclarations + R"(
int main() {
  std::allocator allocator{};
  auto values = allocator.try_storage<int>(std::size_t(1)).expect("allocate");
  observe(values);
  consume(std::move(values));
  auto shared = allocator.try_storage<Affine>(std::size_t(1)).expect("allocate");
  observe(shared);
  auto transferred = allocator.try_storage<Exclusive>(std::size_t(1)).expect("allocate");
  consume(std::move(transferred));
  std::process_resource provider{};
  unsafe {
    auto binding = std::bind_resource(provider);
    std::resource_storage<int, std::process_resource> local{binding};
    if (local.capacity() != std::size_t(0)) { return 1; }
  }
  return 0;
})");
  if (!storage.canGenerateCode())
    for (const auto &diagnostic : storage.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(storage.canGenerateCode(),
         "source storage preserves independent payload transfer/share "
         "capabilities and resource storage remains usable locally");
  for (const char *statement : {
           "std::allocator allocator{}; auto slots = "
           "allocator.try_storage<Affine>(std::size_t(1)).expect(\"allocate\");"
           " "
           "consume(std::move(slots));",
           "std::allocator allocator{}; auto slots = "
           "allocator.try_storage<Exclusive>(std::size_t(1)).expect("
           "\"allocate\"); "
           "observe(slots);",
           "std::process_resource provider{}; unsafe { "
           "auto binding = std::bind_resource(provider); "
           "std::resource_storage<int, std::process_resource> slots{binding}; "
           "consume(std::move(slots)); }",
           "std::process_resource provider{}; unsafe { "
           "auto binding = std::bind_resource(provider); "
           "std::resource_storage<int, std::process_resource> slots{binding}; "
           "observe(slots); }",
       }) {
    const auto bad = analyze(storageDeclarations + "int main() { " + statement +
                             " return 0; }");
    expect(!bad.canGenerateCode() &&
               std::any_of(bad.diagnostics.begin(), bad.diagnostics.end(),
                           [](const auto &d) { return d.code == "GTI-S2029"; }),
           "source storage must not erase payload denial or a resource "
           "owner's checked external borrow");
  }
  for (const char *declaration :
       {"[[unsafe_transfer(U)]] class Wrong<T> { public: Wrong() = default; };",
        "[[unsafe_transfer(N)]] class Wrong<uint64_t N> { public: Wrong() = "
        "default; };",
        "[[unsafe_share(T)]] class Wrong<T...> { public: Wrong() = default; };",
        "[[unsafe_transfer(T), no_transfer]] class Wrong<T> { public: Wrong() "
        "= default; };",
        "[[unsafe_share, unsafe_share(T)]] class Wrong<T> { public: Wrong() = "
        "default; };",
        "[[unsafe_share(T)]] interface Wrong<T> {};",
        "[[unsafe_transfer(int)]] class Wrong {};"}) {
    const auto bad =
        analyze(std::string(declaration) + " int main() { return 0; }");
    expect(!bad.canGenerateCode() &&
               std::any_of(bad.diagnostics.begin(), bad.diagnostics.end(),
                           [](const auto &d) {
                             return d.code == "GTI-S2059" ||
                                    d.code == "GTI-P0001";
                           }),
           "malformed or conflicting conditional policies fail closed");
  }
  const auto formatted = lang::Formatter().format(source);
  expect(lang::Formatter().format(formatted) == formatted &&
             analyze(formatted).canGenerateCode(),
         "formatting conditional attributes preserves their meaning and is "
         "idempotent");
  const auto hover = lang::LanguageQueries().hover(
      good, good.sourceGraph.entryUnit(), source.find("Reviewed<T>") + 1);
  expect(hover &&
             std::any_of(hover->notes.begin(), hover->notes.end(),
                         [](const auto &note) {
                           return note.find(
                                      "conditional unsafe nominal assertion") !=
                                  std::string::npos;
                         }),
         "hover must distinguish conditional from unconditional assertions");
  const auto definition = lang::LanguageQueries().definition(
      good, good.sourceGraph.entryUnit(),
      source.find("unsafe_transfer(T)") + 16);
  expect(
      definition && definition->target.start == source.find("Reviewed<T>") + 9,
      "attribute parameter navigation resolves the generic declaration binder");
  const auto references = lang::LanguageQueries().references(
      good, good.sourceGraph.entryUnit(),
      source.find("unsafe_transfer(T)") + 16, true);
  expect(
      references &&
          std::any_of(references->sites.begin(), references->sites.end(),
                      [&](const auto &site) {
                        return site.span.start ==
                               source.find("unsafe_share(T)") + 13;
                      }),
      "both attribute conditions participate in generic-parameter references");
  return failures == 0 ? 0 : 1;
}
