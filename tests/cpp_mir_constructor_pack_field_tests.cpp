#include "cpp_mir_body_emitter_test_fixture.h"

using namespace gti_test::cpp_mir_body_emitter_fixture;

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
    const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.analyzeProgram().ready(),
           "constructor pack fields must have complete MIR emission");
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
      expect(emitter.supportsFailureBodyText(
                 {lang::MirBodyKind::Constructor, constructor.id}),
             "concrete constructor pack cleanup must use the status ABI");
    }
    expect(ownedPacks != 0,
           "fixture must retain owned constructor pack entries");
  }
  return failures == 0 ? 0 : 1;
}
