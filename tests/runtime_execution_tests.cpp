#include "gti/runtime_execution.h"
#include "gti/runtime_thread.hpp"

#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>

static_assert(
    std::is_same_v<decltype(&gti_rt_thread_locals_begin_v1), void (*)(void)>);
static_assert(
    std::is_same_v<decltype(&gti_rt_thread_locals_drain_v2),
                   void (*)(const gti_failure_record_v2 *,
                            const gti_failure_artifact_descriptor_v1 *)>);
static_assert(
    std::is_same_v<decltype(gti_thread_local_cleanup_entry_v1::cleanup),
                   bool (*)(void *, gti_failure_record_v2 *,
                            const gti_failure_record_v2 *,
                            const gti_failure_artifact_descriptor_v1 *)>);

namespace {
std::vector<int> order;
bool cleanup(void *storage, gti_failure_record_v2 *,
             const gti_failure_record_v2 *,
             const gti_failure_artifact_descriptor_v1 *) {
  order.push_back(*static_cast<int *>(storage));
  return true;
}

struct PrimaryFacts {
  gti_failure_record_v2 expected{};
  const gti_failure_artifact_descriptor_v1 *artifact = nullptr;
  bool valid = true;
  int order = 0;
};
struct PrimaryNode {
  PrimaryFacts *facts;
  int id;
};

bool explicitPrimary(void *storage, gti_failure_record_v2 *failure,
                     const gti_failure_record_v2 *primary,
                     const gti_failure_artifact_descriptor_v1 *artifact) {
  auto &node = *static_cast<PrimaryNode *>(storage);
  auto &facts = *node.facts;
  facts.valid &= primary && failure != primary && artifact == facts.artifact &&
                 std::memcmp(primary, &facts.expected, sizeof(*primary)) == 0;
  facts.valid &=
      gti_rt_thread_locals_attached_v1() && gti_rt_thread_locals_closing_v1();
  facts.order = facts.order * 10 + node.id;
  // A successful callback's scratch output is not a new pending primary.
  *failure = {};
  failure->site_index = 999;
  return true;
}

bool explicitPrimaryAcrossThreads() {
  bool valid[2] = {false, false};
  const auto run = [&](unsigned index) {
    // Opaque descriptor sentinels test forwarding, not report validation.
    gti_failure_artifact_descriptor_v1 primaryArtifact{},
        registrationArtifact{};
    PrimaryFacts facts;
    facts.artifact = &primaryArtifact;
    facts.expected.abi_version = GTI_FAILURE_ABI_VERSION_V2;
    facts.expected.code = GTI_FAILURE_CODE_PANIC_V2;
    facts.expected.detail = GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2;
    facts.expected.site_index = 37 + index;
    facts.expected.artifact_identity[index] = static_cast<uint8_t>(71 + index);
    facts.expected.message_size = 3;
    facts.expected.message[0] = 'A';
    facts.expected.message[1] = 0;
    facts.expected.message[2] = static_cast<uint8_t>('0' + index);
    const auto original = facts.expected;
    PrimaryNode first{&facts, 1}, second{&facts, 2};
    gti_thread_local_cleanup_entry_v1 a{.storage = &first,
                                        .cleanup = explicitPrimary,
                                        .artifact = &registrationArtifact};
    gti_thread_local_cleanup_entry_v1 b{.storage = &second,
                                        .cleanup = explicitPrimary,
                                        .artifact = &registrationArtifact};
    facts.valid &= !gti_rt_thread_locals_attached_v1() &&
                   !gti_rt_thread_locals_closing_v1();
    gti_rt_thread_locals_begin_v1();
    gti_rt_thread_locals_register_v1(&a);
    gti_rt_thread_locals_register_v1(&b);
    gti_rt_thread_locals_drain_v2(&original, &primaryArtifact);
    gti_rt_thread_locals_drain_v2(&original, &primaryArtifact);
    valid[index] =
        facts.valid && facts.order == 21 && !a.registered && !b.registered &&
        !gti_rt_thread_locals_attached_v1() &&
        gti_rt_thread_locals_closing_v1() &&
        std::memcmp(&original, &facts.expected, sizeof(original)) == 0;
  };
  std::thread first(run, 0), second(run, 1);
  first.join();
  second.join();
  return valid[0] && valid[1];
}
} // namespace
int main() {
  if (gti_rt_thread_locals_attached_v1() || gti_rt_thread_locals_closing_v1())
    return 1;
  gti_rt_thread_locals_begin_v1();
  if (!gti_rt_thread_locals_attached_v1() || gti_rt_thread_locals_closing_v1())
    return 2;
  int first = 1, second = 2;
  gti_runtime_detail::thread_local_cleanup_entry a{.storage = &first,
                                                   .cleanup = cleanup};
  gti_runtime_detail::thread_local_cleanup_entry b{.storage = &second,
                                                   .cleanup = cleanup};
  gti_rt_thread_locals_register_v1(&a);
  gti_rt_thread_locals_register_v1(&b);
  gti_rt_thread_locals_drain_v2(nullptr, nullptr);
  gti_rt_thread_locals_drain_v2(nullptr, nullptr);
  return explicitPrimaryAcrossThreads() && order == std::vector<int>({2, 1}) &&
                 !a.registered && !b.registered &&
                 !gti_rt_thread_locals_attached_v1() &&
                 gti_rt_thread_locals_closing_v1()
             ? 0
             : 3;
}
