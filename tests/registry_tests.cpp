#include "gti/driver/artifact.h"
#include "gti/driver/manifest.h"
#include "gti/driver/project_facts.h"
#include "gti/driver/registry.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <tuple>

namespace {
using namespace lang::driver;
int failures = 0;
void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL " << message << '\n';
    ++failures;
  }
}
template <class F> void rejects(F operation, std::string_view code) {
  try {
    operation();
    expect(false, "expected registry rejection");
  } catch (const RegistryError &error) {
    expect(error.code == code, std::string("diagnostic ") + error.code +
                                   " instead of " + std::string(code));
  }
}
void write(const std::filesystem::path &path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << text;
}
struct Temporary {
  std::filesystem::path root = temporaryArtifactPath("registry-test", ".dir");
  Temporary() { std::filesystem::create_directory(root); }
  ~Temporary() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};
void env(std::string value) {
#if defined(_WIN32)
  _putenv_s("GTI_PACKAGE_CACHE", value.c_str());
#else
  setenv("GTI_PACKAGE_CACHE", value.c_str(), 1);
#endif
}
struct FakeRegistry : Registry {
  std::map<std::string, std::map<std::string, std::string>> data;
  std::map<std::string, std::string> archives;
  int lists = 0, downloads = 0;
  std::vector<std::string> listVersions(std::string_view name) override {
    ++lists;
    std::vector<std::string> result;
    for (const auto &[version, text] : data[std::string(name)]) {
      result.push_back(version);
    }
    return result;
  }
  std::optional<std::string> metadata(std::string_view name,
                                      std::string_view version) override {
    auto &versions = data[std::string(name)];
    auto it = versions.find(std::string(version));
    return it == versions.end() || it->second.empty()
               ? std::nullopt
               : std::optional<std::string>(it->second);
  }
  std::string archive(std::string_view name, std::string_view version,
                      std::string_view) override {
    ++downloads;
    return archives.at(std::string(name) + "@" + std::string(version));
  }
  void publish(std::string_view, std::string_view, std::string_view,
               std::string_view) override {
    expect(false, "resolver must not publish");
  }
  void seed(std::string name, std::string version,
            std::vector<RegistryDependency> dependencies = {}) {
    RegistryPackageMetadata meta{.key = {"official", name},
                                 .name = "gti-" + name,
                                 .version = version,
                                 .dependencies = dependencies};
    std::string manifest = "manifest-version = 1\n[package]\nname = \"gti-" +
                           name + "\"\nversion = \"" + version + "\"\n";
    if (!dependencies.empty()) {
      manifest += "[dependencies]\n";
      for (const auto &dep : dependencies) {
        manifest += dep.alias + " = {version = \"" + dep.requirement +
                    "\", package = \"" + dep.key.package + "\", registry = \"" +
                    dep.key.registry + "\"}\n";
      }
    }
    auto bytes = encodePackageArchive(
        {{"gti.toml", manifest},
         {"src/unit.gti", "namespace test { int answer() { return 42; } }\n"}});
    meta.checksum = packageChecksum(bytes);
    archives[name + "@" + version] = bytes;
    data[name][version] = renderRegistryMetadata(meta);
  }
};
struct FakeProvider : RegistryProvider {
  FakeRegistry fake;
  Registry &get(std::string_view name) override {
    expect(name == "official", "logical registry identity");
    return fake;
  }
};
RegistryDependency dep(std::string name, std::string requirement) {
  return {name, {"official", name}, requirement};
}
void testSemver() {
  for (auto value :
       {"0.1.0", "2.3.4-alpha.1+build.02", "18446744073709551615.0.0"}) {
    expect(parsePackageVersion(value).has_value(), value);
  }
  for (auto value : {"01.0.0", "1.2", "1.2.3-alpha.01", "1.2.3+", "1.2.3.4",
                     "18446744073709551616.0.0", " 1.2.3"}) {
    expect(!parsePackageVersion(value), value);
  }
  const std::vector<std::string> ordered = {
      "1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
      "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0"};
  for (std::size_t i = 1; i < ordered.size(); ++i) {
    expect(*parsePackageVersion(ordered[i - 1]) <
               *parsePackageVersion(ordered[i]),
           "SemVer prerelease ordering");
  }
  expect(*parsePackageVersion("1.0.0+one") == *parsePackageVersion("1.0.0+two"),
         "build metadata does not affect precedence");
  for (const auto &[range, yes, no] :
       std::vector<std::tuple<std::string, std::string, std::string>>{
           {"^0.2.1", "0.2.99", "0.3.0"},
           {"^0.0.3", "0.0.3", "0.0.4"},
           {"^0.0", "0.0.9", "0.1.0"},
           {"^0", "0.9.0", "1.0.0"},
           {"1.3", "1.9.0", "2.0.0"},
           {"~1.2", "1.2.99", "1.3.0"},
           {"=1.2.3", "1.2.3", "1.2.4"},
           {"*", "7.0.0", "7.0.0-beta"},
           {"^1.0.0-beta.1", "1.0.0-beta.2", "1.1.0-beta"}}) {
    auto req = PackageVersionRequirement::parse(range);
    expect(req && req->accepts(*parsePackageVersion(yes)) &&
               !req->accepts(*parsePackageVersion(no)),
           range);
  }
  for (auto value :
       {"", "^", "=1.2", "1.x", ">=1.0", "^18446744073709551615.0.0"}) {
    expect(!PackageVersionRequirement::parse(value), value);
  }
}
void testResolution(const std::filesystem::path &cache) {
  env(cache.string());
  FakeProvider provider;
  provider.fake.seed("c", "2.0.0");
  provider.fake.seed("c", "2.1.0");
  provider.fake.seed("b", "1.0.0", {dep("c", "^2.0")});
  provider.fake.seed("a", "0.1.0", {dep("b", "^1.0")});
  provider.fake.seed("a", "0.2.0", {dep("b", "^1.0")});
  provider.fake.data["a"]["0.3.0"] =
      ""; // An interrupted publication has no final metadata.
  auto graph =
      resolveRegistryPackages({dep("a", "*")}, {}, provider, false, false);
  expect(graph.size() == 3 &&
             graph.at({"official", "a"}).metadata.version == "0.2.0" &&
             graph.at({"official", "c"}).metadata.version == "2.1.0",
         "latest stable complete metadata closure");
  expect(provider.fake.downloads == 0,
         "version solving never fetches archives");
  DependencyLock lock;
  for (const auto &[key, selection] : graph) {
    auto entry = materializeRegistryPackage(
        selection, provider,
        {.allowAcquisition = true, .allowMaterialization = true});
    for (const auto &child : selection.metadata.dependencies) {
      entry.dependencies.push_back(child.key.identity() + "@" +
                                   graph.at(child.key).metadata.version);
    }
    lock.registryPackages.push_back(entry);
  }
  const auto lists = provider.fake.lists;
  provider.fake.seed("c", "2.9.0");
  auto same =
      resolveRegistryPackages({dep("a", "*")}, lock, provider, true, true);
  expect(same.at({"official", "c"}).metadata.version == "2.1.0" &&
             provider.fake.lists == lists,
         "lock closure wins offline without discovery");
  auto upgraded = resolveRegistryPackages({dep("a", "*"), dep("c", "*")}, lock,
                                          provider, false, false,
                                          RegistryPackageKey{"official", "c"});
  expect(upgraded.at({"official", "c"}).metadata.version == "2.9.0",
         "add selects the latest compatible direct version instead of an older "
         "transitive lock");
  auto forged = lock;
  forged.registryPackages.front().dependencies = {"official:unknown@9.0.0"};
  rejects(
      [&] {
        (void)resolveRegistryPackages({dep("a", "*")}, forged, provider, true,
                                      true);
      },
      "GTI-B1701");
  rejects(
      [&] {
        (void)resolveRegistryPackages({dep("a", "^0.2"), dep("c", "=3.0.0")},
                                      {}, provider, false, false);
      },
      "GTI-B1716");
  provider.fake.seed("branch", "1.0.0", {dep("c", "^2.0")});
  provider.fake.seed("branch", "1.1.0", {dep("c", "^3.0")});
  provider.fake.seed("branch", "1.2.0", {dep("missing_transitive", "^1.0")});
  auto backtrack = resolveRegistryPackages(
      {dep("branch", "^1"), dep("c", "^2")}, {}, provider, false, false);
  expect(backtrack.at({"official", "branch"}).metadata.version == "1.0.0",
         "backtrack across feature-compatible complete graphs");
  provider.fake.seed("cyclic", "1.0.0");
  provider.fake.seed("cyclic", "1.1.0", {dep("cyclic", "^1")});
  auto acyclic = resolveRegistryPackages({dep("cyclic", "^1")}, {}, provider,
                                         false, false);
  expect(acyclic.at({"official", "cyclic"}).metadata.version == "1.0.0",
         "a cyclic latest candidate does not prevent acyclic older resolution");
  rejects(
      [&] {
        (void)resolveRegistryPackages({dep("a", "*")}, {}, provider, false,
                                      false, std::nullopt, {"gti-a"});
      },
      "GTI-B1716");
  rejects(
      [&] {
        (void)resolveRegistryPackages({dep("missing", "^1")}, {}, provider,
                                      false, false);
      },
      "GTI-B1711");
  const auto rendered = renderDependencyLock(lock);
  expect(rendered.find("lock-version = 2") != rendered.npos &&
             rendered.find("git.internal") == rendered.npos,
         "logical registry lock identity");
  Temporary project;
  write(project.root / "gti.lock", rendered);
  auto loaded = loadDependencyLock(project.root);
  expect(loaded.succeeded() && renderDependencyLock(loaded.lock) == rendered,
         "registry lock deterministic round trip");
  write(project.root / "gti.toml",
        "manifest-version = 1\n[package]\nname = \"consumer\"\nversion = "
        "\"0.1.0\"\n[dependencies]\na = \"*\"\n");
  std::filesystem::create_directory(project.root / "src");
  auto facts = resolveProjectFacts(project.root);
  expect(facts.succeeded() && facts.facts->packages().size() == 4 &&
             facts.facts->findPackage("gti-a@0.2.0")
                 ->sourceIdentity->starts_with("registry+official:a@0.2.0"),
         "read-only project facts include registry packages");
  auto entry = lock.registryPackages.front();
  auto source = registryPackageSource(entry);
  std::filesystem::remove_all(source);
  const auto missingFacts = resolveProjectFacts(project.root);
  expect(!missingFacts.succeeded() && !std::filesystem::exists(source),
         "read-only facts do not unpack cached archives");
  (void)materializeRegistryPackage(graph.at({entry.registry, entry.package}),
                                   provider, {.allowMaterialization = true},
                                   &entry);
  write(source / "src/unit.gti", "changed");
  rejects(
      [&] {
        (void)materializeRegistryPackage(
            graph.at({entry.registry, entry.package}), provider, {}, &entry);
      },
      "GTI-B1717");
}
void testMetadataAndDeclarations() {
  FakeProvider provider;
  provider.fake.seed("fixture", "1.2.3", {dep("helper", "^0.2")});
  const auto text = provider.fake.data.at("fixture").at("1.2.3");
  const RegistryPackageKey key{"official", "fixture"};
  const auto parsed = parseRegistryMetadata(text, key, "1.2.3");
  expect(parsed.name == "gti-fixture" && parsed.dependencies.size() == 1 &&
             renderRegistryMetadata(parsed) == text,
         "metadata round trip preserves canonical/public identity and edges");
  for (const auto &[from, to] :
       std::vector<std::pair<std::string, std::string>>{
           {"registry-version = 1", "registry-version = 2"},
           {"registry-version = 1", "registry-version = 1.0"},
           {"source.gtipkg", "../source.gtipkg"},
           {"sha256:", "SHA256:"},
           {"registry-name = 'fixture'", "registry-name = 'different'"}}) {
    auto malformed = text;
    const auto start = malformed.find(from);
    expect(start != malformed.npos, "metadata mutation finds expected field");
    if (start != malformed.npos) {
      malformed.replace(start, from.size(), to);
      rejects([&] { (void)parseRegistryMetadata(malformed, key, "1.2.3"); },
              "GTI-B1714");
    }
  }
  rejects(
      [&] {
        (void)parseRegistryMetadata("unexpected = true\n" + text, key, "1.2.3");
      },
      "GTI-B1714");
  rejects([&] { (void)parseRegistryMetadata(text, key, "1.2.4"); },
          "GTI-B1714");
  Temporary project;
  const std::string prefix =
      "manifest-version = 1\n[package]\nname = \"consumer\"\nversion = "
      "\"0.1.0\"\n[dependencies]\n";
  for (auto declaration : {"helper = \"^0.2\"\n",
                           "Local = {version = \"~1.2\", package = \"helper\", "
                           "registry = \"alternate\"}\n"}) {
    write(project.root / "gti.toml", prefix + declaration);
    auto result = loadProjectManifest(project.root / "gti.toml");
    expect(
        result.succeeded() && result.manifest->dependencies().size() == 1 &&
            result.manifest->dependencies().front().registry.has_value(),
        "short and renamed registry declarations are ordinary manifest data");
  }
  for (auto declaration :
       {"helper = \"not-a-range\"\n",
        "helper = {version = \"^1\", path = \"src\"}\n",
        "helper = {version = \"^1\", registry = 123}\n",
        "helper = {version = true}\n",
        "helper = {version = \"^1\", package = \"../bad\"}\n"}) {
    write(project.root / "gti.toml", prefix + declaration);
    auto result = loadProjectManifest(project.root / "gti.toml");
    expect(!result.succeeded() && !result.diagnostics.empty(),
           "invalid and hybrid dependency declarations are rejected");
    expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [&](const auto &diagnostic) {
                         return diagnostic.primary.source ==
                                    std::filesystem::weakly_canonical(
                                        project.root / "gti.toml")
                                        .string() &&
                                diagnostic.primary.end >
                                    diagnostic.primary.start;
                       }),
           "manifest registry diagnostics preserve an actionable source span");
  }
}
void testArchives() {
  std::vector<PackageArchiveEntry> entries{
      {"src/a.gti", std::string("a\0b", 3), true},
      {"gti.toml", "manifest-version = 1\n"}};
  auto archive = encodePackageArchive(entries);
  std::reverse(entries.begin(), entries.end());
  expect(archive == encodePackageArchive(entries),
         "deterministic source archive independent of inventory order");
  expect(decodePackageArchive(archive).size() == 2 &&
             decodePackageArchive(archive)[1].contents.size() == 3,
         "bounded binary archive round trip");
  for (auto path : {"../secret", "/absolute", "C:/outside", "src/../../secret",
                    "src\\evil", ".git/config", ".env", "build/output",
                    "src/CON.h", "a/", "src/trailing."}) {
    rejects([&] { (void)encodePackageArchive({{"gti.toml", ""}, {path, ""}}); },
            "GTI-B1718");
  }
  rejects(
      [&] {
        (void)encodePackageArchive(
            {{"gti.toml", ""}, {"src/A", ""}, {"src/a", ""}});
      },
      "GTI-B1718");
  rejects(
      [&] {
        (void)encodePackageArchive(
            {{"gti.toml", ""}, {"src/a", ""}, {"SRC/b", ""}});
      },
      "GTI-B1718");
  rejects(
      [&] {
        (void)encodePackageArchive(
            {{"gti.toml", ""}, {"src", ""}, {"src/a", ""}});
      },
      "GTI-B1718");
  rejects([&] { (void)decodePackageArchive(archive + "trailing"); },
          "GTI-B1718");
  for (std::size_t end = 0; end < archive.size(); ++end) {
    rejects(
        [&] {
          (void)decodePackageArchive(std::string_view(archive).substr(0, end));
        },
        "GTI-B1718");
  }
}
} // namespace
int main() {
  Temporary temporary;
  const char *old = std::getenv("GTI_PACKAGE_CACHE");
  const std::string saved = old ? old : "";
  testSemver();
  testMetadataAndDeclarations();
  testArchives();
  testResolution(temporary.root / "cache");
  env(saved);
  return failures == 0 ? 0 : 1;
}
