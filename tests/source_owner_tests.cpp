#include "gti/frontend.h"
#include "gti/source_loader.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace {
void expect(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  const auto root = std::filesystem::current_path() / "owner-fixture";
  const auto app = root / "app";
  const auto library = root / "library";
  const auto entry = app / "main.gti";
  const auto api = library / "api.gti";
  const auto helper = library / "private.gti";
  const auto implementation = library / "body.gti";
  lang::SourceOwnerDescriptor application{.identity = "application",
                                          .name = "application",
                                          .packageRoot = app,
                                          .sourceRoot = app,
                                          .dependencies = {{"dep", "library"}}};
  lang::SourceOwnerDescriptor dependency{
      .identity = "library",
      .name = "library",
      .packageRoot = library,
      .sourceRoot = library,
      .modules = {{"net.http", api}},
      .moduleImplementations = {{"net.http", {implementation}}}};
  std::unordered_map<std::string, std::string> sources{
      {api.string(),
       "export module net /* trivia */ . http;\nimport \"private.gti\";\n"},
      {helper.string(), "export module helper;\n"},
      {implementation.string(), "module net.http;\n"}};
  auto cache = std::make_shared<lang::FrontendCache>();
  for (int pass = 0; pass != 2; ++pass) {
    lang::SourceLoader loader(nullptr, cache);
    const auto graph = loader.load(entry, "import dep.net.http;", {}, sources,
                                   {}, std::nullopt, {application, dependency});
    for (const auto &diagnostic : loader.errors())
      std::cerr << diagnostic.message << '\n';
    expect(!loader.hadError(),
           "registered dotted modules and private owner-local imports load");
    const auto interface = graph.sourceUnitForPath(api.string());
    const auto body = graph.sourceUnitForPath(implementation.string());
    const auto privateUnit = graph.sourceUnitForPath(helper.string());
    expect(graph.sameModule(interface, body) &&
               !graph.sameModule(interface, privateUnit),
           "only registered implementations share module identity");
    expect(graph.findUnit(interface)->moduleDeclarationSpan->end -
                   graph.findUnit(interface)->moduleDeclarationSpan->start ==
               23,
           "module name spans retain comments and whitespace on fresh and "
           "cached loads");
    expect(graph.findUnit(interface)->role == lang::SourceUnitRole::Application,
           "source ownership does not grant standard-library trust");
    expect(graph.imports().size() == 2 && graph.sourceOwners(),
           "the graph retains import records and immutable owners");
  }
  const auto fails = [&](std::string source,
                         std::vector<lang::PackageSourceRoot> owners) {
    lang::SourceLoader loader;
    loader.load(entry, source, {}, sources, {}, std::nullopt, owners);
    return loader.hadError();
  };
  expect(fails("import \"../library/api.gti\";", {application, dependency}),
         "quoted imports cannot bypass a public owner boundary");
  expect(fails("import dep.helper;", {application, dependency}),
         "unregistered private modules are not public aliases");
  auto ambiguous = application;
  ambiguous.modules = {{"dep.net.http", app / "local.gti"}};
  sources[(app / "local.gti").string()] = "export module dep.net.http;";
  expect(fails("import dep.net.http;", {ambiguous, dependency}),
         "exact local names cannot silently shadow dependency prefixes");
  auto unambiguous = application;
  unambiguous.modules = {{"dep.local", app / "local.gti"}};
  sources[(app / "local.gti").string()] = "export module dep.local;";
  expect(!fails("import dep.local;", {unambiguous, dependency}),
         "an alias prefix alone does not shadow a complete local registration");
  sources[implementation.string()] = "module other;";
  expect(fails("import dep.net.http;", {application, dependency}),
         "implementation headers must match the registered module");
  sources[implementation.string()] = "module net.http;";
  lang::SourceLoader implementationLoader;
  const auto bodyGraph = implementationLoader.load(
      implementation, sources.at(implementation.string()), {}, sources, {},
      std::nullopt, {application, dependency});
  expect(!implementationLoader.hadError() &&
             bodyGraph.moduleForUnit(bodyGraph.entryUnit()) != 0,
         "an implementation entry activates its registered interface");
  {
    auto cyclicOwner = dependency;
    const auto otherInterface = library / "other.gti";
    const auto otherBody = library / "other_body.gti";
    cyclicOwner.modules.push_back({"other", otherInterface});
    cyclicOwner.moduleImplementations.push_back({"other", {otherBody}});
    auto cycleSources = sources;
    cycleSources[implementation.string()] = "module net.http; import other;";
    cycleSources[otherInterface.string()] = "export module other;";
    cycleSources[otherBody.string()] = "module other; import net.http;";
    lang::SourceLoader cyclic;
    cyclic.load(entry, "import dep.net.http;", {}, cycleSources, {},
                std::nullopt, {application, cyclicOwner});
    expect(cyclic.hadError() && cyclic.errors().size() == 1 &&
               cyclic.errors().front().code == "GTI-I0001",
           "cycles through registered bodies are rejected at source loading");
    cycleSources[otherBody.string()] = "module other;";
    lang::SourceLoader acyclic;
    acyclic.load(entry, "import dep.net.http;", {}, cycleSources, {},
                 std::nullopt, {application, cyclicOwner});
    expect(!acyclic.hadError(),
           "a body may import another module without creating a false cycle");
  }
  lang::SourceLoader direct;
  direct.load(entry, "module orphan;", {}, sources);
  expect(direct.hadError(),
         "an unregistered header cannot claim module-private access");

  auto configured = dependency;
  configured.kind = lang::SourceOwnerKind::Toolchain;
  configured.implicitAlias = "sdk";
  configured.defaultModule = "net.http";
  lang::SourceLoader configuredLoader;
  const auto configuredGraph = configuredLoader.load(
      entry, "import sdk;", {}, sources, {}, std::nullopt, {configured});
  expect(!configuredLoader.hadError() &&
             configuredGraph.sourceOwners()->isDirectMode(),
         "nonstandard configured owners share the same map rules in "
         "manifest-independent direct mode");
  auto duplicate = dependency;
  duplicate.packageRoot = root / "another";
  expect(fails("", {dependency, duplicate}),
         "duplicate owner identities cannot silently grant same-module access");
  auto escaping = dependency;
  escaping.modules.front().second = app / "local.gti";
  expect(fails("", {application, escaping}),
         "invalid unused public maps fail closed");

  const auto mapRoot =
      std::filesystem::temp_directory_path() /
      ("gti-module-map-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(mapRoot / "std");
  std::ofstream(mapRoot / "module-map.json")
      << R"({"default":"sample","modules":{"sample":"std/sample.gti"}})";
  std::ofstream(mapRoot / "std/sample.gti")
      << "export module sample; import \"helper.gti\";";
  std::ofstream(mapRoot / "std/helper.gti") << "export module helper;";
  lang::SourceLoader mapped;
  mapped.load(entry, "import std;", {}, {}, {mapRoot});
  expect(!mapped.hadError(),
         "installed public maps permit owner-local private standard imports");
  lang::SourceLoader privateStandard;
  privateStandard.load(entry, "import std.helper;", {}, {}, {mapRoot});
  expect(privateStandard.hadError(),
         "installed helper files are not implicitly public modules");
  std::ofstream(mapRoot / "module-map.json")
      << R"({"modules":{"escape":"../escape.gti"}})";
  lang::SourceLoader badMap;
  badMap.load(entry, "", {}, {}, {mapRoot});
  expect(badMap.hadError(),
         "installed public maps reject containment escapes even when unused");
  std::filesystem::remove_all(mapRoot);

  auto changed = dependency;
  changed.modules.front().first = "net.https";
  expect(lang::SourceOwnerRegistry({dependency}).resolutionIdentity() !=
             lang::SourceOwnerRegistry({changed}).resolutionIdentity(),
         "public map changes invalidate resolution identity");
  const auto relocated = root / "relocated";
  auto moved = dependency;
  moved.packageRoot = relocated;
  moved.sourceRoot = relocated;
  moved.modules.front().second = relocated / "api.gti";
  moved.moduleImplementations.front().second.front() = relocated / "body.gti";
  expect(lang::SourceOwnerRegistry({dependency}).resolutionIdentity() ==
             lang::SourceOwnerRegistry({moved}).resolutionIdentity(),
         "logical resolution identity survives package relocation");
  // The public logical maps can stay identical while containment assigns the
  // same loaded source to a different owner. Shared frontend caches must use
  // the resolved membership, not only source bytes and relocatable maps.
  lang::SourceOwnerDescriptor firstOwner{.identity = "first",
                                         .name = "first",
                                         .packageRoot = app,
                                         .sourceRoot = app};
  lang::SourceOwnerDescriptor secondOwner{.identity = "second",
                                          .name = "second",
                                          .packageRoot = library,
                                          .sourceRoot = library};
  auto swappedFirst = firstOwner;
  auto swappedSecond = secondOwner;
  std::swap(swappedFirst.packageRoot, swappedSecond.packageRoot);
  std::swap(swappedFirst.sourceRoot, swappedSecond.sourceRoot);
  expect(lang::SourceOwnerRegistry({firstOwner, secondOwner})
                 .resolutionIdentity() ==
             lang::SourceOwnerRegistry({swappedFirst, swappedSecond})
                 .resolutionIdentity(),
         "the cache topology fixture keeps logical public metadata unchanged");
  auto sharedFrontendCache = std::make_shared<lang::FrontendCache>();
  auto sharedPreambles = std::make_shared<lang::SemanticPreambleCache>();
  const auto prelude = app / "seed.gti";
  const auto local = app / "local.gti";
  for (int round = 0; round != 3; ++round) {
    lang::FrontendWorkMetrics work;
    lang::FrontendOptions options;
    options.cache = sharedFrontendCache;
    options.preambleCache = sharedPreambles;
    options.workMetrics = &work;
    options.stopAfter = lang::FrontendPhase::Semantics;
    options.suggestIncludes = false;
    const auto result = lang::Frontend(options).analyze(
        entry, "import \"local.gti\"; int main() { return value() + seed(); }",
        {prelude},
        {{local.string(),
          "export module local; export int value() { return 7; }"},
         {prelude.string(),
          "export module seed; export int seed() { return 1; }"}},
        {},
        round == 2
            ? std::vector<lang::PackageSourceRoot>{swappedFirst, swappedSecond}
            : std::vector<lang::PackageSourceRoot>{firstOwner, secondOwner});
    expect(result.sourceValid && result.syntaxValid && result.semanticValid,
           "shared-cache topology analyses preserve valid module programs");
    const auto *module =
        result.sourceGraph.findModule(result.sourceGraph.moduleForUnit(
            result.sourceGraph.sourceUnitForPath(local.string())));
    expect(module && module->ownerIdentity == (round == 2 ? "second" : "first"),
           "module ownership follows current canonical containment");
    expect(work.semanticPreambleAdoptions == 1 &&
               work.semanticPreambleBuilds == (round == 1 ? 0U : 1U),
           "unchanged membership adopts its semantic preamble while changed "
           "containment rebuilds declaration ownership");
  }
  std::cout << "source owner registration checks passed\n";
}
