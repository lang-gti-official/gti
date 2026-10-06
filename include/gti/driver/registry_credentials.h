#pragma once

#include <memory>
#include <optional>
#include <string>

namespace lang::driver {

// Credential identity is deliberately independent of package/archive format.
// A changed endpoint or owner must never inherit an old endpoint's secret.
struct RegistryCredentialTarget {
  std::string registry;
  std::string endpoint;
  std::string realm;
  auto operator<=>(const RegistryCredentialTarget &) const = default;
};

enum class RegistryCredentialKind { AccessToken = 1 };
struct RegistryCredential {
  RegistryCredentialKind kind = RegistryCredentialKind::AccessToken;
  std::string secret;
};
struct RegistryAuthenticationInfo {
  RegistryCredentialTarget target;
  std::string tokenEnvironment;
};
enum class RegistryCredentialSource { Anonymous, Environment, Stored };
struct RegistryCredentialSelection {
  RegistryCredentialSource source = RegistryCredentialSource::Anonymous;
  std::optional<RegistryCredential> credential;
};

// Implementations must use protected OS storage, never project or cache files.
// read() on an unsupported host returns no stored credential; mutations fail.
class RegistryCredentialStore {
public:
  virtual ~RegistryCredentialStore() = default;
  [[nodiscard]] virtual bool available() const = 0;
  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual std::optional<RegistryCredential>
  read(const RegistryCredentialTarget &target) = 0;
  virtual void write(const RegistryCredentialTarget &target,
                     const RegistryCredential &credential) = 0;
  virtual void erase(const RegistryCredentialTarget &target) = 0;
};

[[nodiscard]] std::shared_ptr<RegistryCredentialStore>
systemRegistryCredentialStore();
void validateRegistryCredential(const RegistryCredential &credential);
// An explicitly empty variable disables stored credentials (anonymous mode).
[[nodiscard]] RegistryCredentialSelection
resolveRegistryCredential(const RegistryAuthenticationInfo &info,
                          RegistryCredentialStore &store);

} // namespace lang::driver
