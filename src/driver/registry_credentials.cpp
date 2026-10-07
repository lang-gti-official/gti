#include "gti/driver/registry.h"
#include "registry_credentials_internal.h"

#include <algorithm>
#include <cstdlib>
#include <exception>

namespace lang::driver {
namespace {
constexpr std::string_view recordPrefix = "GTI-CREDENTIAL1\naccess-token\n";
[[noreturn]] void invalid() {
  throw RegistryError("GTI-B1725", "Invalid registry access token; supply one "
                                   "nonempty token without spaces or controls "
                                   "(at most 4096 bytes).");
}
} // namespace
void validateRegistryCredential(const RegistryCredential &credential) {
  if (credential.kind != RegistryCredentialKind::AccessToken ||
      credential.secret.empty() || credential.secret.size() > 4096 ||
      !std::all_of(credential.secret.begin(), credential.secret.end(),
                   [](unsigned char c) { return c > 0x20 && c < 0x7f; })) {
    invalid();
  }
}
void validateRegistryPasswordLogin(const RegistryPasswordLogin &login) {
  const auto clean = [](std::string_view value) {
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return c >= 0x20 && c != 0x7f; });
  };
  if (login.username.empty() || login.username.size() > 256 ||
      !clean(login.username) || login.username.find(':') != std::string::npos ||
      login.password.empty() || login.password.size() > 4096 ||
      !clean(login.password) || login.oneTimeCode.size() > 128 ||
      !clean(login.oneTimeCode) ||
      (login.permission != RegistryLoginPermission::Read &&
       login.permission != RegistryLoginPermission::Publish)) {
    throw RegistryError("GTI-B1725",
                        "Invalid registry sign-in input; provide a "
                        "username, nonempty password and optional "
                        "authenticator code without control characters.");
  }
}
std::string registryCredentialAccount(const RegistryCredentialTarget &target) {
  if (target.registry.empty() || target.endpoint.empty() ||
      target.realm.empty()) {
    throw RegistryError("GTI-B1720",
                        "Registry credential scope is incomplete.");
  }
  std::string framed = "GTI-REGISTRY-CREDENTIAL-SCOPE1\n";
  for (const auto &field : {target.registry, target.endpoint, target.realm}) {
    framed += std::to_string(field.size()) + ":" + field;
  }
  return packageChecksum(framed);
}
std::string encodeRegistryCredential(const RegistryCredential &credential) {
  validateRegistryCredential(credential);
  return std::string(recordPrefix) + credential.secret;
}
RegistryCredential decodeRegistryCredential(std::string_view bytes) {
  if (!bytes.starts_with(recordPrefix)) {
    throw RegistryError("GTI-B1724",
                        "Stored registry credential has an "
                        "unsupported format; log out and log in again.");
  }
  RegistryCredential result{.secret =
                                std::string(bytes.substr(recordPrefix.size()))};
  validateRegistryCredential(result);
  return result;
}
RegistryCredentialSelection
resolveRegistryCredential(const RegistryAuthenticationInfo &info,
                          RegistryCredentialStore &store) {
  if (const char *value = std::getenv(info.tokenEnvironment.c_str())) {
    RegistryCredentialSelection result{
        .source = RegistryCredentialSource::Environment};
    if (*value) {
      result.credential = RegistryCredential{.secret = value};
      validateRegistryCredential(*result.credential);
    }
    return result;
  }
  auto credential = store.read(info.target);
  if (credential) {
    validateRegistryCredential(*credential);
    return {.source = RegistryCredentialSource::Stored,
            .credential = std::move(credential)};
  }
  return {};
}
void Registry::validateCredential(const RegistryCredential &) {
  throw RegistryError("GTI-B1725",
                      "This registry backend does not support login.");
}
RegistryIssuedCredential
Registry::issueCredential(const RegistryPasswordLogin &) {
  throw RegistryError("GTI-B1725",
                      "This registry backend does not support "
                      "username/password login; use --token instead.");
}
void Registry::discardIssuedCredential(const RegistryPasswordLogin &,
                                       const RegistryIssuedCredential &) {
  throw RegistryError("GTI-B1727", "This registry backend cannot revoke the "
                                   "newly issued login credential.");
}
void loginRegistry(Registry &registry, RegistryCredentialStore &store,
                   const RegistryCredential &credential) {
  const auto info = registry.authentication();
  if (!info) {
    throw RegistryError("GTI-B1725",
                        "This registry backend does not support login.");
  }
  if (!store.available()) {
    throw RegistryError("GTI-B1724",
                        "Secure registry credential storage is not "
                        "available on this host; use the configured "
                        "token-env variable instead.");
  }
  validateRegistryCredential(credential);
  registry.validateCredential(credential);
  store.write(info->target, credential);
}
void logoutRegistry(Registry &registry, RegistryCredentialStore &store) {
  const auto info = registry.authentication();
  if (!info) {
    throw RegistryError("GTI-B1725",
                        "This registry backend does not support logout.");
  }
  store.erase(info->target);
}
void loginRegistryWithPassword(Registry &registry,
                               RegistryCredentialStore &store,
                               const RegistryPasswordLogin &login) {
  const auto info = registry.authentication();
  if (!info || !info->passwordLogin) {
    throw RegistryError("GTI-B1725",
                        "This registry backend does not support "
                        "username/password login; use --token instead.");
  }
  if (!store.available()) {
    throw RegistryError("GTI-B1724",
                        "Secure registry credential storage is not "
                        "available; use an existing token in token-env.");
  }
  validateRegistryPasswordLogin(login);
  const auto issued = registry.issueCredential(login);
  try {
    loginRegistry(registry, store, issued.credential);
  } catch (...) {
    const auto failure = std::current_exception();
    try {
      registry.discardIssuedCredential(login, issued);
    } catch (...) {
      std::string cause;
      try {
        std::rethrow_exception(failure);
      } catch (const RegistryError &original) {
        cause = " Original failure [" + original.code + "]: " + original.what();
      } catch (...) {
        // Unknown exception details need not be credential-safe.
      }
      throw RegistryError(
          "GTI-B1727", "Login failed and the newly created remote "
                       "token could not be revoked. Remove GTI's "
                       "new token in the server's account settings "
                       "before retrying; previous saved login is unchanged." +
                           cause);
    }
    std::rethrow_exception(failure);
  }
}

#if !defined(__APPLE__)
namespace {
class UnavailableCredentialStore final : public RegistryCredentialStore {
public:
  bool available() const override { return false; }
  std::string name() const override { return "unavailable"; }
  std::optional<RegistryCredential>
  read(const RegistryCredentialTarget &) override {
    return std::nullopt;
  }
  void write(const RegistryCredentialTarget &,
             const RegistryCredential &) override {
    unavailable();
  }
  void erase(const RegistryCredentialTarget &) override { unavailable(); }

private:
  [[noreturn]] static void unavailable() {
    throw RegistryError("GTI-B1724",
                        "Secure registry credential storage is not "
                        "available on this host; use token-env instead.");
  }
};
} // namespace
std::shared_ptr<RegistryCredentialStore> systemRegistryCredentialStore() {
  return std::make_shared<UnavailableCredentialStore>();
}
#endif
} // namespace lang::driver
