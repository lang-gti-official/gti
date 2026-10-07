#include "fixtures/embedding/gti_embedding_v1.h"

#include <stddef.h>

_Static_assert(GTI_EMBED_ABI_VERSION_V1 == 1, "embedding ABI version");
_Static_assert(GTI_EMBED_STATUS_OK_V1 == 0, "success status ordinal");
_Static_assert(GTI_EMBED_STATUS_DEFINED_FAILURE_V1 == 1,
               "defined-failure status ordinal");
_Static_assert(GTI_EMBED_STATUS_CONTEXT_INACTIVE_V1 == 7,
               "status ordinal extent");
_Static_assert(sizeof(gti_embed_status_v1) == 4, "status storage width");
_Static_assert(sizeof(((gti_module_descriptor_v1 *)0)->module_identity) == 32,
               "module identity width");
_Static_assert(sizeof(gti_export_descriptor_v1) == 48,
               "export descriptor size");
_Static_assert(offsetof(gti_export_descriptor_v1, symbol) == 0,
               "export symbol offset");
_Static_assert(offsetof(gti_export_descriptor_v1, signature_identity) == 16,
               "signature identity offset");
_Static_assert(sizeof(gti_module_descriptor_v1) == 64,
               "module descriptor size");
_Static_assert(offsetof(gti_module_descriptor_v1, abi_version) == 0,
               "ABI version offset");
_Static_assert(offsetof(gti_module_descriptor_v1, struct_size) == 4,
               "structure size offset");
_Static_assert(offsetof(gti_module_descriptor_v1, module_identity) == 8,
               "module identity offset");
_Static_assert(offsetof(gti_module_descriptor_v1, failure_artifact) == 40,
               "failure descriptor offset");
_Static_assert(offsetof(gti_module_descriptor_v1, exports) == 48,
               "export table offset");
_Static_assert(offsetof(gti_module_descriptor_v1, export_count) == 56,
               "export count offset");
_Static_assert(offsetof(gti_module_descriptor_v1, reserved) == 60,
               "reserved offset");
_Static_assert(sizeof(game_frame_v1) == 16, "representative record size");
_Static_assert(offsetof(game_frame_v1, frame) == 0,
               "representative scalar offset");
_Static_assert(offsetof(game_frame_v1, delta_seconds) == 8,
               "representative double offset");

typedef const gti_module_descriptor_v1 *(*descriptor_function_v1)(void);
typedef gti_embed_status_v1 (*create_function_v1)(uint32_t,
                                                  gti_failure_observer_v1,
                                                  void *, gti_context_v1 **,
                                                  gti_failure_record_v1 *);
typedef gti_embed_status_v1 (*destroy_function_v1)(gti_context_v1 *,
                                                   gti_failure_record_v1 *);
typedef gti_embed_status_v1 (*update_function_v1)(gti_context_v1 *,
                                                  game_engine_v1 *,
                                                  game_frame_v1, int32_t *,
                                                  gti_failure_record_v1 *);

_Static_assert(_Generic(&gti_get_module_descriptor_v1,
                   descriptor_function_v1: 1,
                   default: 0),
               "descriptor prototype");
_Static_assert(_Generic(&gti_context_create_v1,
                   create_function_v1: 1,
                   default: 0),
               "context creation prototype");
_Static_assert(_Generic(&gti_context_destroy_v1,
                   destroy_function_v1: 1,
                   default: 0),
               "context destruction prototype");
_Static_assert(_Generic(&game_update_v1, update_function_v1: 1, default: 0),
               "wrapper prototype");

int main(void) { return 0; }
