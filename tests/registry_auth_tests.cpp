#include "gti/driver/registry.h"
#include "registry_credentials_internal.h"

#include <cstdlib>
#include <iostream>
#include <map>

namespace {
using namespace lang::driver;
int failures = 0;
void expect(bool value, std::string_view label) {
  if (!value) {
    std::cerr << "FAIL " << label << '\n';
    ++failures;
  }
}
template <class F> void rejects(F action, std::string_view code) {
  try {
    action();
    expect(false, "expected credential rejection");
  } catch (const RegistryError &error) {
    expect(error.code == code, "credential diagnostic code");
    expect(std::string_view(error.what()).find("fixture-secret") ==
               std::string_view::npos,
           "diagnostic never includes secret");
  }
}
void environment(const char *name, const char *value) {
#if defined(_WIN32)
  _putenv_s(name, value ? value : "");
#else
  if (value) {
    setenv(name, value, 1);
  } else {
    unsetenv(name);
  }
#endif
}
struct MemoryStore : RegistryCredentialStore {
  std::map<RegistryCredentialTarget, RegistryCredential> data;
  int reads = 0, writes = 0, erases = 0;
  bool usable = true, denied = false;
  bool available() const override { return usable; }
  std::string name() const override { return "test memory store"; }
  std::optional<RegistryCredential>
  read(const RegistryCredentialTarget &target) override {
    ++reads;
    if (denied) {
      throw RegistryError("GTI-B1724", "Fixture store denied access.");
    }
    auto it = data.find(target);
    return it == data.end() ? std::nullopt : std::optional(it->second);
  }
  void write(const RegistryCredentialTarget &target,
             const RegistryCredential &credential) override {
    ++writes;
    data.insert_or_assign(target, credential);
  }
  void erase(const RegistryCredentialTarget &target) override {
    ++erases;
    data.erase(target);
  }
};
// An unrelated backend proves storage/login policy does not depend on Gitea.
struct OtherRegistry : Registry {
  RegistryAuthenticationInfo info{
      {"custom", "https://registry.example", "team"}, "GTI_AUTH_MODEL_TOKEN"};
  int validations = 0;
  std::optional<RegistryAuthenticationInfo> authentication() const override {
    return info;
  }
  void validateCredential(const RegistryCredential &credential) override {
    ++validations;
    if (credential.secret != "fixture-secret") {
      throw RegistryError("GTI-B1713", "Fixture authentication rejected.");
    }
  }
  std::vector<std::string> listVersions(std::string_view) override {
    return {};
  }
  std::optional<std::string> metadata(std::string_view,
                                      std::string_view) override {
    return {};
  }
  std::string archive(std::string_view, std::string_view,
                      std::string_view) override {
    return {};
  }
  void publish(std::string_view, std::string_view, std::string_view,
               std::string_view) override {}
};
void model() {
  OtherRegistry backend;
  MemoryStore store;
  environment("GTI_AUTH_MODEL_TOKEN", nullptr);
  RegistryCredential token{.secret = "fixture-secret"};
  expect(resolveRegistryCredential(backend.info, store).source ==
             RegistryCredentialSource::Anonymous,
         "missing stored credentials allow anonymous access");
  loginRegistry(backend, store, token);
  expect(backend.validations == 1 && store.writes == 1,
         "validate before saving");
  expect(resolveRegistryCredential(backend.info, store).source ==
             RegistryCredentialSource::Stored,
         "stored lookup");
  rejects([&] { loginRegistry(backend, store, {.secret = "wrong"}); },
          "GTI-B1713");
  expect(store.writes == 1 &&
             store.data.at(backend.info.target).secret == token.secret,
         "failed login retains previous credential");
  store.usable = false;
  rejects([&] { loginRegistry(backend, store, token); }, "GTI-B1724");
  expect(backend.validations == 2,
         "unavailable secure store avoids network validation");
  store.usable = true;
  for (auto changed :
       {RegistryCredentialTarget{"other", "https://registry.example", "team"},
        RegistryCredentialTarget{"custom", "https://elsewhere.example", "team"},
        RegistryCredentialTarget{"custom", "https://registry.example",
                                 "other"}}) {
    expect(registryCredentialAccount(changed) !=
               registryCredentialAccount(backend.info.target),
           "scope includes logical name, endpoint and owner");
    expect(!store.read(changed), "no secret inherited by a changed scope");
  }
  expect(registryCredentialAccount({"a", "bc", "d"}) !=
             registryCredentialAccount({"ab", "c", "d"}),
         "scope hashing is length framed");
  store.denied = true;
  const auto previousReads = store.reads;
  environment("GTI_AUTH_MODEL_TOKEN", "override");
  auto selected = resolveRegistryCredential(backend.info, store);
  expect(selected.source == RegistryCredentialSource::Environment &&
             selected.credential->secret == "override" &&
             store.reads == previousReads,
         "environment takes precedence without accessing locked store");
#if !defined(_WIN32)
  environment("GTI_AUTH_MODEL_TOKEN", "");
  selected = resolveRegistryCredential(backend.info, store);
  expect(!selected.credential &&
             selected.source == RegistryCredentialSource::Environment &&
             store.reads == previousReads,
         "explicit empty environment opts out of stored login");
#endif
  environment("GTI_AUTH_MODEL_TOKEN", nullptr);
  rejects([&] { (void)resolveRegistryCredential(backend.info, store); },
          "GTI-B1724");
  store.denied = false;
  for (auto secret :
       {std::string{}, std::string("fixture-secret\nInjected: yes"),
        std::string("a\0b", 3), std::string("two tokens"),
        std::string(4097, 'a')}) {
    rejects([&] { validateRegistryCredential({.secret = secret}); },
            "GTI-B1725");
  }
  rejects(
      [&] {
        validateRegistryCredential(
            {static_cast<RegistryCredentialKind>(99), "token"});
      },
      "GTI-B1725");
  expect(decodeRegistryCredential(encodeRegistryCredential(token)).secret ==
             token.secret,
         "versioned credential round trip");
  rejects(
      [&] {
        (void)decodeRegistryCredential(
            "GTI-CREDENTIAL2\naccess-token\nfixture-secret");
      },
      "GTI-B1724");
  rejects(
      [&] {
        (void)decodeRegistryCredential(
            "GTI-CREDENTIAL1\npassword\nfixture-secret");
      },
      "GTI-B1724");
  logoutRegistry(backend, store);
  logoutRegistry(backend, store);
  expect(store.data.empty(), "logout is exact and idempotent");
}
void adapter() {
  auto store = std::make_shared<MemoryStore>();
  auto registries = configuredRegistries(store);
  auto &backend = registries->get("official");
  const auto info = *backend.authentication();
  expect(info.target.realm == "test",
         "configured Gitea owner is credential realm");
  expect(!info.target.endpoint.ends_with('/'),
         "endpoint scope normalizes trailing slash");
  expect(store->reads == 0,
         "configuring adapter does not access protected storage");
  // CLI environment overrides must not contaminate validation of a new login.
  environment(info.tokenEnvironment.c_str(), "incorrect-environment");
  loginRegistry(backend, *store, {.secret = "fixture-secret"});
  rejects([&] { loginRegistry(backend, *store, {.secret = "wrong"}); },
          "GTI-B1713");
  expect(store->writes == 1,
         "Gitea rejects invalid credentials before replacement");
  environment(info.tokenEnvironment.c_str(), nullptr);
  const auto versions = backend.listVersions("sample");
  expect(versions.size() == 1 && versions.front() == "1.0.0",
         "stored token enables Gitea discovery");
  expect(backend.metadata("sample", "1.0.0") == "metadata",
         "stored token enables metadata");
  expect(backend.archive("sample", "1.0.0", "source.gtipkg") == "archive",
         "stored token enables archive");
  backend.publish("fresh", "1.0.0", "archive", "metadata");
  expect(store->reads == 1, "stored lookup cached per command provider");
  environment(info.tokenEnvironment.c_str(), "wrong");
  rejects([&] { (void)backend.listVersions("sample"); }, "GTI-B1713");
#if !defined(_WIN32)
  environment(info.tokenEnvironment.c_str(), "");
  rejects([&] { (void)backend.listVersions("sample"); }, "GTI-B1713");
#endif
  environment(info.tokenEnvironment.c_str(), nullptr);
  logoutRegistry(backend, *store);
  auto fresh = configuredRegistries(store);
  rejects([&] { (void)fresh->get("official").listVersions("sample"); },
          "GTI-B1713");
}
} // namespace
int main(int argc, char *[]) {
  if (argc == 1) {
    model();
  } else {
    adapter();
  }
  return failures ? 1 : 0;
}
