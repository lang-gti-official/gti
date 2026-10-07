#import <Foundation/Foundation.h>

#include "bridge_config.h"
#include <cstdint>
#include <cstring>
#include <limits>

// The caller supplies writable storage valid for the duration of this call.
// Return byte count, or -1 on failure. No address is retained or returned.
// Works with Clang's default manual ownership and with -fobjc-arc.
extern "C" std::int64_t
apple_bridge_copy_label(std::uint8_t *output, std::uint64_t capacity) noexcept {
  try {
    @autoreleasepool {
      @try {
        // A deterministic Foundation operation keeps the example testable;
        // an engine can use the same boundary for platform API results.
        NSString *label =
            [@"psych" stringByAppendingPathComponent:GTI_BRIDGE_COMPONENT];
        NSData *bytes = [label dataUsingEncoding:NSUTF8StringEncoding];
        if (bytes == nil || output == nullptr || [bytes length] > capacity ||
            [bytes length] >
                std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
          return -1;
        }
        std::memcpy(output, [bytes bytes], [bytes length]);
        return static_cast<std::int64_t>([bytes length]);
      } @catch (NSException *exception) {
        (void)exception;
        return -1;
      }
    }
  } catch (...) {
    return -1;
  }
}
