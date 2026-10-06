#include "gti/driver/manifest.h"
#include "gti/driver/registry.h"
#include "sha256.h"
#define TOML_EXCEPTIONS 0
#include "toml.hpp"

#include <algorithm>
#include <sstream>

namespace lang::driver {
RegistryError::RegistryError(std::string id, std::string message)
    : std::runtime_error(std::move(message)), code(std::move(id)) {}
std::string RegistryPackageKey::identity() const {
  return registry + ":" + package;
}
bool validRegistryName(std::string_view name) {
  return name.size() <= 64 && isPortableProjectName(name) &&
         std::all_of(name.begin(), name.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
         });
}
std::string packageChecksum(std::string_view bytes) {
  Sha256 hash;
  hash.update(bytes);
  return "sha256:" + hash.finishHex();
}
bool validPackageChecksum(std::string_view checksum) {
  return checksum.size() == 71 && checksum.starts_with("sha256:") &&
         std::all_of(checksum.begin() + 7, checksum.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

RegistryPackageMetadata parseRegistryMetadata(std::string_view text,
                                              const RegistryPackageKey &key,
                                              std::string_view version) {
  const auto fail = []() -> void {
    throw RegistryError("GTI-B1714",
                        "Malformed or unsupported GTI registry metadata.");
  };
  if (text.size() > 1024U * 1024U) {
    fail();
  }
  auto parsed = toml::parse(text);
  if (!parsed) {
    fail();
  }
  const auto &doc = parsed.table();
  for (const auto &[field, value] : doc) {
    if (field.str() != "registry-version" && field.str() != "package" &&
        field.str() != "archive" && field.str() != "dependencies") {
      fail();
    }
  }
  if (!doc["registry-version"].is_integer() ||
      doc["registry-version"].value<std::int64_t>() != 1) {
    fail();
  }
  const auto *package = doc["package"].as_table(),
             *archive = doc["archive"].as_table();
  if (!package || !archive) {
    fail();
  }
  RegistryPackageMetadata result{.key = key};
  const auto string = [&](const toml::table &table, std::string_view field) {
    auto value = table[field].value<std::string>();
    if (!value || value->empty()) {
      fail();
    }
    return *value;
  };
  for (const auto &[field, value] : *package) {
    if (field.str() != "name" && field.str() != "version" &&
        field.str() != "registry-name") {
      fail();
    }
  }
  for (const auto &[field, value] : *archive) {
    if (field.str() != "file" && field.str() != "checksum") {
      fail();
    }
  }
  result.name = string(*package, "name");
  result.version = string(*package, "version");
  result.archive = string(*archive, "file");
  result.checksum = string(*archive, "checksum");
  if (string(*package, "registry-name") != key.package ||
      result.version != version || !isPortableProjectName(result.name) ||
      !parsePackageVersion(result.version) ||
      result.archive != "source.gtipkg" ||
      !validPackageChecksum(result.checksum)) {
    fail();
  }
  if (const auto *node = doc.get("dependencies")) {
    const auto *deps = node->as_table();
    if (!deps || deps->size() > 256) {
      fail();
    }
    for (const auto &[alias, value] : *deps) {
      RegistryDependency dep{
          .alias = std::string(alias.str()),
          .key = {.registry = "official", .package = std::string(alias.str())}};
      const auto validAlias = [](std::string_view name) {
        if (name.empty() || name == "std" ||
            (name.front() >= '0' && name.front() <= '9')) {
          return false;
        }
        return std::all_of(name.begin(), name.end(), [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_';
        });
      };
      if (!validAlias(dep.alias)) {
        fail();
      }
      if (auto range = value.value<std::string>()) {
        dep.requirement = *range;
      } else if (const auto *table = value.as_table()) {
        for (const auto &[field, item] : *table) {
          if (field.str() != "version" && field.str() != "registry" &&
              field.str() != "package") {
            fail();
          }
        }
        dep.requirement = string(*table, "version");
        if (table->get("registry")) {
          dep.key.registry = string(*table, "registry");
        }
        if (table->get("package")) {
          dep.key.package = string(*table, "package");
        }
      } else {
        fail();
      }
      if (!validRegistryName(dep.key.registry) ||
          !validRegistryName(dep.key.package) ||
          !PackageVersionRequirement::parse(dep.requirement)) {
        fail();
      }
      result.dependencies.push_back(std::move(dep));
    }
  }
  return result;
}

std::string renderRegistryMetadata(const RegistryPackageMetadata &metadata) {
  toml::table doc;
  doc.insert("registry-version", 1);
  doc.insert("package", toml::table{{"name", metadata.name},
                                    {"registry-name", metadata.key.package},
                                    {"version", metadata.version}});
  doc.insert("archive", toml::table{{"file", metadata.archive},
                                    {"checksum", metadata.checksum}});
  toml::table deps;
  for (const auto &dep : metadata.dependencies) {
    deps.insert(dep.alias, toml::table{{"version", dep.requirement},
                                       {"registry", dep.key.registry},
                                       {"package", dep.key.package}});
  }
  doc.insert("dependencies", std::move(deps));
  std::ostringstream output;
  output << doc << '\n';
  return output.str();
}
} // namespace lang::driver
