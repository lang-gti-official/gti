#include "embedding_internal.h"

extern "C" gti_embed_status_v1
gti_context_create_v1(uint32_t abi, gti_failure_observer_v1 observer,
                      void *observerContext, gti_context_v1 **outContext,
                      gti_failure_record_v1 *outFailure) {
  const auto status = gti_rt_embed_create_begin_v1(
      abi, observer, observerContext, outContext, outFailure);
  if (status != 0)
    return status;
  gti_rt_embed_create_end_v1(outContext);
  return 0;
}
extern "C" gti_embed_status_v1
gti_context_destroy_v1(gti_context_v1 *context,
                       gti_failure_record_v1 *failure) {
  return gti_rt_embed_destroy_v1(context, failure);
}
extern "C" gti_embed_status_v1 embed_mix(gti_context_v1 *context,
                                         uint64_t input, uint64_t *output,
                                         gti_failure_record_v1 *failure) {
  if (!output)
    return 2;
  const auto status = gti_rt_embed_enter_v1(context, failure);
  if (status != 0)
    return status;
  *output = input ^ UINT64_C(91);
  gti_rt_embed_leave_v1(context);
  return 0;
}
