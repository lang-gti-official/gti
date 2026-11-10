#include "llvm_backend_test_support.h"
#include "mir_test_fixture.h"

using namespace gti_test::mir_fixture;

int main() {
  const auto fixtures = std::filesystem::path(__FILE__).parent_path();
  for (const auto &path :
       {fixtures /
            "coverage/reproducers/a55-ctor-pack-move-only-field-init.gti",
        fixtures / "fixtures/constructor_pack_field_forwarding.gti"}) {
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(input.good() || input.eof(), "constructor pack fixture must exist");
    expect(frontend.canGenerateCode(), "owning constructor packs must verify");
    if (!frontend.canGenerateCode())
      continue;
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "constructor pack fields must produce verified MIR");
    std::string error;
    expect(gti_test::llvmEmits(frontend, lang::OptimizationLevel::O0,
                               lang::TargetInfo::host(), &error),
           "constructor pack fields must have complete native emission: " +
               error);
    std::size_t ownedPacks = 0;
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      std::vector<lang::MirDropObligationId> elements;
      for (const auto &drop : constructor.body.dropObligations) {
        const auto *place = constructor.body.findPlace(drop.place);
        if (drop.kind == lang::MirDropObligationKind::Binding && place &&
            place->projections.size() == 1 &&
            place->projections.front().kind ==
                lang::MirProjectionKind::PackElement)
          elements.push_back(drop.id);
      }
      if (elements.empty())
        continue;
      ++ownedPacks;
      expect(constructor.mayRaiseDefinedFailure,
             "concrete constructor pack cleanup must carry the MIR "
             "defined-failure dimension");
      for (const auto id : elements) {
        for (int mutation = 0; mutation < 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          auto &drop = changed->body.dropObligations[id - 1];
          if (mutation == 0)
            drop.initiallyActive = false;
          else if (mutation == 1)
            drop.binding = 0;
          else
            changed->body.places[drop.place - 1]
                .projections.front()
                .constantIndex = UINT64_MAX;
          expect(
              !lang::verifyMirProgram(malformed).valid(),
              "constructor pack entry, binding and ordinal forgery must fail");
        }
      }
    }
    expect(ownedPacks != 0,
           "fixture must retain owned constructor pack entries");
  }
  const auto repeated =
      analyzeWithStandardLibrary("pack-double-consume.gti", R"(
import std.string;
class Item {
  std::string value;
public:
  Item(std::string value) : value(std::move(value)) {}
};
struct Pair<T> {
  T first;
  T second;
  Pair<Args...>(Args... args) : first(T(args...)), second(T(args...)) {}
};
int main() { Pair<Item> pair = Pair<Item>(std::string("owned")); return 0; }
)");
  expect(!repeated.canGenerateCode(),
         "a move-only pack element cannot initialize two different fields");
  return failures == 0 ? 0 : 1;
}
