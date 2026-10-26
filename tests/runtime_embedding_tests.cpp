#include "embedding_internal.h"
#include <cassert>
#include <cstring>
#include <thread>

int main() {
  gti_failure_record_v1 failure;
  std::memset(&failure, 0x57, sizeof failure);
  const auto original = failure;
  gti_context_v1 *context = nullptr;
  assert(gti_rt_embed_create_begin_v1(2, nullptr, nullptr, &context,
                                      &failure) == 3);
  assert(!context);
  assert(gti_rt_embed_create_begin_v1(1, nullptr, nullptr, nullptr, &failure) ==
         2);
  assert(gti_rt_embed_create_begin_v1(1, nullptr, nullptr, &context, nullptr) ==
         2);
  assert(gti_rt_embed_create_begin_v1(1, nullptr, nullptr, &context,
                                      &failure) == 0);
  assert(gti_rt_embed_create_begin_v1(1, nullptr, nullptr, &context,
                                      &failure) == 4);
  gti_rt_embed_create_end_v1(&context);
  assert(context);
  assert(gti_rt_embed_enter_v1(context, nullptr) == 2);
  assert(gti_rt_embed_enter_v1(context, &failure) == 0);
  assert(gti_rt_embed_enter_v1(context, &failure) == 4);
  assert(gti_rt_embed_destroy_v1(context, &failure) == 4);
  std::thread foreign([&] {
    assert(gti_rt_embed_enter_v1(context, &failure) == 5);
    assert(gti_rt_embed_destroy_v1(context, &failure) == 5);
  });
  foreign.join();
  gti_rt_embed_leave_v1(context);
  assert(gti_rt_embed_enter_v1(context, &failure) == 0);
  gti_rt_embed_leave_v1(context);
  assert(gti_rt_embed_destroy_v1(context, &failure) == 0);
  assert(gti_rt_embed_enter_v1(context, &failure) == 7);
  assert(gti_rt_embed_destroy_v1(context, &failure) == 7);
  assert(std::memcmp(&failure, &original, sizeof failure) == 0);
}
