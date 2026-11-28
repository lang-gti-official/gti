#include "transport.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string_view>
#include <thread>

namespace {
// Fixture descriptors are statically pinned. These are NOT compiler-generated
// content hashes and do not test an artifact loader's unload/pinning protocol.
const std::uint8_t canonical[] = {1};
const gti_failure_outcome_descriptor_v1 outcome{
    GTI_FAILURE_CODE_PANIC_V2, GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2, 0};
const gti_failure_site_descriptor_v1 sites[] = {
    {{"origin.gti", 10}, 41, 100, 110, &outcome, 1, 0},
    {{"caller.gti", 10}, 73, 200, 210, &outcome, 1, 0},
    {{"cleanup.gti", 11}, 89, 300, 310, &outcome, 1, 0}};
const gti_failure_artifact_descriptor_v1 artifacts[] = {
    {1, 0, {1}, &sites[0], 1, 0, canonical, 1},
    {1, 0, {2}, &sites[1], 1, 0, canonical, 1},
    {1, 0, {3}, &sites[2], 1, 0, canonical, 1}};

void require(bool condition) {
  if (!condition) {
    std::fputs("transport invariant failed\n", stderr);
    std::abort();
  }
}

void summary(const Context &c, int result) {
  std::printf("trace=%s drops=%llu published=%d result=%d\n", c.trace,
              static_cast<unsigned long long>(c.drops), c.published, result);
  std::fflush(stdout);
}

bool invoke(Context *c, Target target, int value, bool fail, int *result) {
#if TRANSPORT_STATUS
  return frame(result, &c->contained, c, value, fail, 2, target);
#else
  try {
    *result = frame(c, value, fail, 2, target);
    return true;
  } catch (const Channel &failure) {
    c->contained = failure;
    return false;
  }
#endif
}

extern "C" int callback(void *opaque) noexcept {
  auto *c = static_cast<Context *>(opaque);
  int result = -999;
  // Native carrier is caught before returning to the actual C host frame.
  return invoke(c, leaf, 7, true, &result) ? 0 : -1;
}
} // namespace

void event(Context *c, char tag) noexcept {
  if (c->tracing) {
    require(c->used + 1 < sizeof(c->trace));
    c->trace[c->used++] = tag;
    c->trace[c->used] = 0;
  }
}

void drop(Context *c, char tag) noexcept {
  ++c->drops;
  event(c, tag);
}

Channel originate(Context *c, bool secondary) noexcept {
  Channel failure{};
  failure.artifact = &artifacts[secondary ? 2 : c->caller ? 1 : 0];
  auto &record = failure.record;
  record.abi_version = 2;
  record.code = GTI_FAILURE_CODE_PANIC_V2;
  record.detail = GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2;
  record.site_index = 1;
  std::memcpy(record.artifact_identity, failure.artifact->artifact_identity,
              32);
  char message[64];
  std::strcpy(message, secondary ? "secondary-owned" : "owned-before-cleanup");
  require(gti_rt_failure_capture_panic_bytes_v2(
              &record, reinterpret_cast<const std::uint8_t *>(message),
              std::strlen(message)) == GTI_FAILURE_REPORT_OK_V1);
  // Volatile writes ensure source storage really changes before propagation.
  volatile char *source = message;
  for (unsigned i = 0; i < sizeof(message); ++i)
    source[i] = 'X';
  if (!secondary)
    c->original = failure;
  return failure;
}

void audit(const Channel &failure, const Context &c) noexcept {
  require(gti_rt_failure_validate_v2(&failure.record, failure.artifact) ==
          GTI_FAILURE_REPORT_OK_V1);
  auto invalid = failure.record;
  invalid.artifact_identity[0] ^= 127;
  require(gti_rt_failure_validate_v2(&invalid, failure.artifact) ==
          GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1);
  invalid = failure.record;
  invalid.site_index = 2;
  require(gti_rt_failure_validate_v2(&invalid, failure.artifact) ==
          GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1);
  require(failure.artifact == c.original.artifact);
  require(std::memcmp(&failure.record, &c.original.record,
                      sizeof(failure.record)) == 0);
  require(failure.record.message_size == 20);
  require(std::memcmp(failure.record.message, "owned-before-cleanup", 20) == 0);
}

[[noreturn]] void emergency(Context *c, const Channel &primary,
                           const Channel &secondary) noexcept {
  audit(primary, *c);
  require(gti_rt_failure_validate_v2(&secondary.record, secondary.artifact) ==
          GTI_FAILURE_REPORT_OK_V1);
  require(secondary.artifact == &artifacts[2]);
  require(secondary.record.message_size == 15);
  require(std::memcmp(secondary.record.message, "secondary-owned", 15) == 0);
  summary(*c, -999);
  const gti_failure_emergency_v2 envelope{2, 0, primary.record,
                                          secondary.record};
  gti_rt_failure_terminate_cleanup_v2(&envelope, primary.artifact,
                                      secondary.artifact);
}

#ifdef TRANSPORT_ALLOCATION_AUDIT
extern "C" std::uint64_t transport_exception_requests();
#endif

int main(int argc, char **argv) {
  require(argc >= 2);
  const std::string_view mode = argv[1];
  const bool benchmark = argc == 4 && std::string_view(argv[2]) == "--bench";
  const bool dynamic = mode.starts_with("dynamic");
  const bool fail = mode != "direct-success" && mode != "dynamic-success";
  // Opaque across translation units, with two exact-signature targets.
  Target target = dynamic
                      ? (std::getenv("TRANSPORT_ALTERNATE") ? alternate : leaf)
                      : nullptr;
  Context c;
  c.secondary = mode == "secondary";
  c.return_failure = mode == "return-cleanup";
  c.caller = mode == "caller";
  if (benchmark) {
    require(mode == "direct-success" || mode == "dynamic-success" ||
            mode == "direct-failure" || mode == "dynamic-failure");
    const auto iterations = std::strtoull(argv[3], nullptr, 10);
    require(iterations > 0 && iterations <= 100000000);
    c.tracing = false;
    std::uint64_t checksum = 0;
    const auto cpuStarted = std::clock();
    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
      int result = -999;
      const bool ok =
          invoke(&c, target, static_cast<int>(i & 65535), fail, &result);
      require(ok == !fail);
      checksum += ok ? result : c.contained.record.message[0];
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    const auto cpu =
        (std::clock() - cpuStarted) * (1000000000.0 / CLOCKS_PER_SEC);
    std::printf("checksum=%llu drops=%llu", (unsigned long long)checksum,
                (unsigned long long)c.drops);
    std::printf(" elapsed_ns=%llu", (unsigned long long)elapsed);
    std::printf(" cpu_ns=%llu", (unsigned long long)cpu);
#ifdef TRANSPORT_ALLOCATION_AUDIT
    std::printf(" exception_storage_requests=%llu",
                (unsigned long long)transport_exception_requests());
#endif
    std::puts("");
    return 0;
  }

  int result = -999;
  bool ok;
  if (mode == "ffi") {
    int returned = 0;
    require(transport_c_host(callback, &c, &returned) == -1 && returned == 1);
    event(&c, 'F');
    ok = false;
  } else if (mode == "worker") {
    std::thread worker([&] {
      require(!invoke(&c, target, 7, true, &result));
      event(&c, 'W');
    });
    worker.join();
    audit(c.contained, c);
    event(&c, 'J');
    // Re-raise the original record only after join, never across thread entry.
#if TRANSPORT_STATUS
    ok = false;
#else
    try {
      throw c.contained;
    } catch (const Channel &failure) {
      c.contained = failure;
      ok = false;
    }
#endif
  } else if (mode == "before-transfer") {
    event(&c, 'P');
    c.contained = originate(&c);
    event(&c, 'F');
    drop(&c, 'p'); // Later argument fails: caller still owns prepared input.
    ok = false;
  } else if (mode == "after-transfer") {
    event(&c, 'P');
    event(&c, 'T');
    ok = invoke(&c, target, 7, true, &result);
    drop(&c, 'p'); // Callee/boundary owns input after explicit handoff.
  } else {
    ok = invoke(&c, target, 7, c.return_failure ? false : fail, &result);
  }
  c.published = ok;
  require(result == (ok ? (7 ^ 90) : -999));
  summary(c, result);
  if (!ok) {
    audit(c.contained, c);
    gti_rt_failure_terminate_v2(&c.contained.record, c.contained.artifact,
                                nullptr, nullptr);
  }
  return 0;
}
