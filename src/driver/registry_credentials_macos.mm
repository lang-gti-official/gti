#include "gti/driver/registry.h"
#include "registry_credentials_internal.h"

#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>
#include <Security/Security.h>

namespace lang::driver {
namespace {
constexpr auto service = "dev.gti.registry.credentials.v1";
[[noreturn]] void failed(OSStatus status) {
  // Do not render OS descriptions, item data or user-controlled server replies.
  throw RegistryError("GTI-B1724",
                      "macOS Keychain access failed (status " +
                          std::to_string(status) +
                          "); unlock your login keychain and allow GTI "
                          "access, or use the configured token-env variable.");
}
class KeychainStore final : public RegistryCredentialStore {
public:
  bool available() const override { return true; }
  std::string name() const override { return "macOS Keychain"; }
  std::optional<RegistryCredential>
  read(const RegistryCredentialTarget &target) override {
    @autoreleasepool {
      NSMutableDictionary *query = key(target);
      query[(__bridge id)kSecReturnData] = @YES;
      query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
      CFTypeRef data = nullptr;
      const auto status =
          SecItemCopyMatching((__bridge CFDictionaryRef)query, &data);
      if (status == errSecItemNotFound) {
        return std::nullopt;
      }
      if (status != errSecSuccess) {
        failed(status);
      }
      if (!data || CFGetTypeID(data) != CFDataGetTypeID()) {
        if (data) {
          CFRelease(data);
        }
        failed(errSecDecode);
      }
      const auto bytes = static_cast<CFDataRef>(data);
      const auto size = CFDataGetLength(bytes);
      if (size < 0 || size > 8192) {
        CFRelease(data);
        failed(errSecDecode);
      }
      std::string record(
          reinterpret_cast<const char *>(CFDataGetBytePtr(bytes)),
          static_cast<std::size_t>(size));
      CFRelease(data);
      return decodeRegistryCredential(record);
    }
  }
  void write(const RegistryCredentialTarget &target,
             const RegistryCredential &credential) override {
    @autoreleasepool {
      const auto record = encodeRegistryCredential(credential);
      NSMutableDictionary *query = key(target);
      NSData *data = [NSData dataWithBytes:record.data() length:record.size()];
      NSDictionary *changes = @{(__bridge id)kSecValueData : data};
      auto status = SecItemUpdate((__bridge CFDictionaryRef)query,
                                  (__bridge CFDictionaryRef)changes);
      if (status == errSecItemNotFound) {
        query[(__bridge id)kSecValueData] = data;
        query[(__bridge id)kSecAttrAccessible] =
            (__bridge id)kSecAttrAccessibleWhenUnlockedThisDeviceOnly;
        status = SecItemAdd((__bridge CFDictionaryRef)query, nullptr);
        if (status == errSecDuplicateItem) {
          [query removeObjectForKey:(__bridge id)kSecValueData];
          [query removeObjectForKey:(__bridge id)kSecAttrAccessible];
          status = SecItemUpdate((__bridge CFDictionaryRef)query,
                                 (__bridge CFDictionaryRef)changes);
        }
      }
      if (status != errSecSuccess) {
        failed(status);
      }
    }
  }
  void erase(const RegistryCredentialTarget &target) override {
    @autoreleasepool {
      const auto status = SecItemDelete((__bridge CFDictionaryRef)key(target));
      if (status != errSecSuccess && status != errSecItemNotFound) {
        failed(status);
      }
    }
  }

private:
  static NSMutableDictionary *key(const RegistryCredentialTarget &target) {
    const auto account = registryCredentialAccount(target);
    LAContext *context = [[LAContext alloc] init];
    context.interactionNotAllowed = YES;
    return [@{
      (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
      (__bridge id)kSecAttrService : @(service),
      (__bridge id)kSecAttrSynchronizable : @NO,
      (__bridge id)
      kSecAttrAccount : [NSString stringWithUTF8String:account.c_str()],
      (__bridge id)kSecUseAuthenticationContext : context
    } mutableCopy];
  }
};
} // namespace
std::shared_ptr<RegistryCredentialStore> systemRegistryCredentialStore() {
  return std::make_shared<KeychainStore>();
}
} // namespace lang::driver
