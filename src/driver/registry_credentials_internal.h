#pragma once

#include "gti/driver/registry_credentials.h"

#include <string_view>

namespace lang::driver {
[[nodiscard]] std::string
registryCredentialAccount(const RegistryCredentialTarget &target);
[[nodiscard]] std::string
encodeRegistryCredential(const RegistryCredential &credential);
[[nodiscard]] RegistryCredential
decodeRegistryCredential(std::string_view bytes);
} // namespace lang::driver
