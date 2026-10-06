#include "gti/driver/manifest.h"
#include "gti/driver/registry.h"
#include "gti/http.h"

#include <llvm/Support/JSON.h>
#define TOML_EXCEPTIONS 0
#include "toml.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>

namespace lang::driver {
namespace {
std::string environment(const char *name) {
  const char *value = std::getenv(name);
  return value ? value : "";
}
struct Configuration {
  std::string backend = "gitea-generic";
  std::string url = "https://git.internal.howetotech.dev";
  std::string owner = "gti-lang";
  std::string tokenEnvironment = "GTI_REGISTRY_TOKEN";
};
[[noreturn]] void configurationError() {
  throw RegistryError("GTI-B1720",
                      "Invalid registry configuration; use a supported "
                      "backend, HTTPS URL, owner and token-env name.");
}
std::string escaped(std::string_view text) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string output;
  for (unsigned char c : text) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
      output += static_cast<char>(c);
    } else {
      output += '%';
      output += hex[c >> 4U];
      output += hex[c & 15U];
    }
  }
  return output;
}
struct Reply {
  std::uint32_t status;
  std::string body;
};
class GiteaGenericRegistry final : public Registry {
public:
  explicit GiteaGenericRegistry(std::string identity, Configuration value,
                                std::shared_ptr<RegistryCredentialStore> store)
      : config(std::move(value)), credentials(std::move(store)),
        registryIdentity(std::move(identity)) {
    while (config.url.ends_with('/')) {
      config.url.pop_back();
    }
    const bool loopback = config.url.starts_with("http://127.0.0.1:") ||
                          config.url.starts_with("http://localhost:");
    if (config.backend != "gitea-generic") {
      throw RegistryError("GTI-B1720", "Unsupported registry backend; this "
                                       "bootstrap supports gitea-generic.");
    }
    if ((!config.url.starts_with("https://") && !loopback) ||
        config.url.find_first_of("@?#\\") != config.url.npos ||
        !std::all_of(config.url.begin(), config.url.end(),
                     [](unsigned char c) { return c > 0x20 && c < 0x7f; }) ||
        !isPortableProjectName(config.owner)) {
      configurationError();
    }
    if (config.tokenEnvironment.empty() ||
        !std::all_of(config.tokenEnvironment.begin(),
                     config.tokenEnvironment.end(), [](char c) {
                       return (c >= 'a' && c <= 'z') ||
                              (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '_';
                     })) {
      configurationError();
    }
  }
  std::optional<RegistryAuthenticationInfo> authentication() const override {
    return RegistryAuthenticationInfo{
        .target = {registryIdentity, config.url, config.owner},
        .tokenEnvironment = config.tokenEnvironment};
  }
  void validateCredential(const RegistryCredential &credential) override {
    validateRegistryCredential(credential);
    // This authenticated API requires package-read scope, not read:user.
    // Use the supplied token directly, ignoring environment/stored credentials.
    auto reply = request(GTI_HTTP_GET_V1,
                         "/api/v1/packages/" + escaped(config.owner) +
                             "?type=generic&page=1&limit=1",
                         {}, 1024U * 1024U, &credential);
    require(reply, 200);
    auto json = llvm::json::parse(reply.body);
    if (!json || !json->getAsArray()) {
      if (!json) {
        llvm::consumeError(json.takeError());
      }
      malformed();
    }
  }
  std::vector<std::string> listVersions(std::string_view package) override {
    std::vector<std::string> versions;
    for (unsigned page = 1; page <= 100; ++page) {
      auto reply = request(GTI_HTTP_GET_V1,
                           apiPackage(package) +
                               "?page=" + std::to_string(page) + "&limit=100");
      if (reply.status == 404) {
        return versions;
      }
      require(reply, 200);
      auto json = llvm::json::parse(reply.body);
      if (!json || !json->getAsArray()) {
        if (!json) {
          llvm::consumeError(json.takeError());
        }
        malformed();
      }
      auto &array = *json->getAsArray();
      for (const auto &item : array) {
        const auto *object = item.getAsObject();
        const auto value = object ? object->getString("version") : std::nullopt;
        if (!value) {
          malformed();
        }
        if (parsePackageVersion(value->str())) {
          versions.push_back(value->str());
        }
      }
      if (array.size() < 100) {
        return versions;
      }
    }
    throw RegistryError(
        "GTI-B1721", "Registry version listing exceeds the bootstrap limit.");
  }
  std::optional<std::string> metadata(std::string_view package,
                                      std::string_view version) override {
    auto reply =
        request(GTI_HTTP_GET_V1, file(package, version, "package.toml"));
    if (reply.status == 404) {
      return std::nullopt;
    }
    require(reply, 200);
    return std::move(reply.body);
  }
  std::string archive(std::string_view package, std::string_view version,
                      std::string_view name) override {
    auto reply = request(GTI_HTTP_GET_V1, file(package, version, name), {},
                         256U * 1024U * 1024U);
    if (reply.status == 404) {
      throw RegistryError("GTI-B1715", "Published package archive is missing.");
    }
    require(reply, 200);
    return std::move(reply.body);
  }
  void publish(std::string_view package, std::string_view version,
               std::string_view archiveBytes,
               std::string_view metadataBytes) override {
    if (!credential()) {
      throw RegistryError("GTI-B1713",
                          "Publishing needs a package-write token; log in "
                          "or set the configured token-env variable.");
    }
    auto preflight = request(GTI_HTTP_GET_V1, apiPackage(package) + "/" +
                                                  escaped(version) + "/files");
    if (preflight.status == 200) {
      throw RegistryError("GTI-B1719",
                          "This package version already exists (including "
                          "partial publications); publish a new version.");
    }
    require(preflight, 404);
    // The immutable archive upload claims the version. Concurrent publishers
    // cannot both claim it: a 409 stops before metadata can mark it complete.
    auto source = request(
        GTI_HTTP_PUT_V1, file(package, version, "source.gtipkg"), archiveBytes);
    if (source.status == 409) {
      throw RegistryError(
          "GTI-B1719",
          "This package version was already claimed by another publication.");
    }
    require(source, 201);
    auto completion = request(
        GTI_HTTP_PUT_V1, file(package, version, "package.toml"), metadataBytes);
    require(completion, 201);
  }

private:
  Configuration config;
  std::shared_ptr<RegistryCredentialStore> credentials;
  std::string registryIdentity;
  mutable bool storedLoaded = false;
  mutable std::optional<RegistryCredential> storedCredential;
  std::optional<RegistryCredential> credential() const {
    // Never read the OS store when an environment override is present, even
    // explicitly empty. Otherwise read once per provider/command lifetime.
    if (std::getenv(config.tokenEnvironment.c_str())) {
      return resolveRegistryCredential(*authentication(), *credentials)
          .credential;
    }
    if (!storedLoaded) {
      storedCredential =
          resolveRegistryCredential(*authentication(), *credentials).credential;
      storedLoaded = true;
    }
    return storedCredential;
  }
  [[noreturn]] static void malformed() {
    throw RegistryError("GTI-B1714",
                        "Registry returned malformed version-list metadata.");
  }
  std::string apiPackage(std::string_view package) const {
    return "/api/v1/packages/" + escaped(config.owner) + "/generic/" +
           escaped(package);
  }
  std::string file(std::string_view package, std::string_view version,
                   std::string_view name) const {
    return "/api/packages/" + escaped(config.owner) + "/generic/" +
           escaped(package) + "/" + escaped(version) + "/" + escaped(name);
  }
  static void require(const Reply &reply, std::uint32_t wanted) {
    if (reply.status == wanted) {
      return;
    }
    if (reply.status == 401 || reply.status == 403) {
      throw RegistryError(
          "GTI-B1713",
          "Registry authentication or package permission was rejected (HTTP " +
              std::to_string(reply.status) + ").");
    }
    throw RegistryError("GTI-B1712", "Registry request failed (HTTP " +
                                         std::to_string(reply.status) +
                                         "); redirects are not followed.");
  }
  Reply request(gti_http_method_v1 method, std::string_view path,
                std::string_view body = {}, std::uint64_t limit = 1024U * 1024U,
                const RegistryCredential *explicitCredential = nullptr) const {
    const auto selected =
        explicitCredential
            ? std::optional<RegistryCredential>(*explicitCredential)
            : credential();
    std::int64_t client = 0, response = 0;
    if (gti_rt_http_client_create_v1(&client) != GTI_HTTP_OK_V1) {
      throw RegistryError("GTI-B1712",
                          "Registry HTTP transport is unavailable; install a "
                          "supported libcurl runtime.");
    }
    std::string headers;
    auto header = [&](std::string_view name, std::string_view value) {
      headers += name;
      headers += '\0';
      headers += value;
      headers += '\0';
    };
    header("Content-Type", "application/octet-stream");
    if (selected) {
      header("Authorization", "token " + selected->secret);
    }
    const auto url = config.url + std::string(path);
    std::int32_t native = 0;
    const auto view = [](std::string_view value) {
      return gti_c_string_view{value.data(), value.size()};
    };
    const auto status = gti_rt_http_client_send_v1(
        client, method, view(url), view(headers), selected ? 2 : 1, view(body),
        10000, 90000, limit, view({}), &response, &native);
    gti_rt_http_client_destroy_v1(client);
    if (status != GTI_HTTP_OK_V1) {
      throw RegistryError(
          "GTI-B1712", "Cannot reach the registry securely (transport error " +
                           std::to_string(status) + ").");
    }
    Reply result{.status = gti_rt_http_response_status_v1(response)};
    result.body.resize(gti_rt_http_response_body_size_v1(response));
    const auto read = gti_rt_http_response_body_read_v1(
        response, 0, reinterpret_cast<std::uint8_t *>(result.body.data()),
        result.body.size());
    gti_rt_http_response_destroy_v1(response);
    if (read < 0 || static_cast<std::uint64_t>(read) != result.body.size()) {
      throw RegistryError("GTI-B1712", "Cannot read registry response bytes.");
    }
    return result;
  }
};
class ConfiguredRegistries final : public RegistryProvider {
public:
  explicit ConfiguredRegistries(std::shared_ptr<RegistryCredentialStore> store)
      : credentials(std::move(store)) {}
  Registry &get(std::string_view identity) override {
    const std::string name(identity);
    if (!validRegistryName(name)) {
      configurationError();
    }
    if (auto found = registries.find(name); found != registries.end()) {
      return *found->second;
    }
    if (!loaded) {
      load();
    }
    auto found = configurations.find(name);
    if (found == configurations.end()) {
      throw RegistryError("GTI-B1720",
                          "Unknown logical registry '" + name + "'.");
    }
    auto backend = std::make_unique<GiteaGenericRegistry>(name, found->second,
                                                          credentials);
    auto &result = *backend;
    registries.emplace(name, std::move(backend));
    return result;
  }

private:
  std::shared_ptr<RegistryCredentialStore> credentials;
  bool loaded = false;
  std::map<std::string, Configuration> configurations;
  std::map<std::string, std::unique_ptr<Registry>> registries;
  void load() {
    loaded = true;
    configurations.emplace("official", Configuration{});
    std::filesystem::path path = environment("GTI_REGISTRY_CONFIG");
    const bool explicitPath = !path.empty();
    if (path.empty()) {
      auto base = environment("XDG_CONFIG_HOME");
      if (base.empty()) {
        base = environment("HOME");
        if (!base.empty()) {
          base += "/.config";
        }
      }
      if (base.empty()) {
        return;
      }
      path = std::filesystem::path(base) / "gti/registries.toml";
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      if (explicitPath) {
        configurationError();
      }
      return;
    }
    std::string text{std::istreambuf_iterator<char>(input), {}};
    if (text.size() > 1024U * 1024U) {
      configurationError();
    }
    auto parsed = toml::parse(text);
    if (!parsed || parsed.table().size() != 1) {
      configurationError();
    }
    const auto *table = parsed.table()["registries"].as_table();
    if (!table) {
      configurationError();
    }
    for (const auto &[key, node] : *table) {
      if (!validRegistryName(key.str())) {
        configurationError();
      }
      const auto *settings = node.as_table();
      if (!settings) {
        configurationError();
      }
      Configuration config;
      if (key.str() != "official") {
        if (!settings->get("backend") || !settings->get("url") ||
            !settings->get("owner")) {
          configurationError();
        }
        std::string scoped(key.str());
        for (auto &character : scoped) {
          if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
          }
          if (character == '-') {
            character = '_';
          }
        }
        config.tokenEnvironment = "GTI_REGISTRY_" + scoped + "_TOKEN";
      }
      for (const auto &[field, value] : *settings) {
        auto textValue = value.value<std::string>();
        if (!textValue || textValue->empty()) {
          configurationError();
        }
        if (field.str() == "backend") {
          config.backend = *textValue;
        } else if (field.str() == "url") {
          config.url = *textValue;
        } else if (field.str() == "owner") {
          config.owner = *textValue;
        } else if (field.str() == "token-env") {
          config.tokenEnvironment = *textValue;
        } else {
          configurationError();
        }
      }
      configurations.insert_or_assign(std::string(key.str()),
                                      std::move(config));
    }
  }
};
} // namespace
std::unique_ptr<RegistryProvider>
configuredRegistries(std::shared_ptr<RegistryCredentialStore> credentials) {
  return std::make_unique<ConfiguredRegistries>(
      credentials ? std::move(credentials) : systemRegistryCredentialStore());
}
} // namespace lang::driver
