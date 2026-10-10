#include "fixtures/embedding/gti_embedding_v1.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

static_assert(GTI_EMBED_ABI_VERSION_V1 == 1);
static_assert(GTI_EMBED_STATUS_OK_V1 == 0);
static_assert(GTI_EMBED_STATUS_DEFINED_FAILURE_V1 == 1);
static_assert(GTI_EMBED_STATUS_CONTEXT_INACTIVE_V1 == 7);
static_assert(sizeof(gti_embed_status_v1) == 4);
static_assert(std::is_standard_layout_v<gti_module_descriptor_v1>);
static_assert(std::is_trivially_copyable_v<game_frame_v1>);
static_assert(sizeof(gti_export_descriptor_v1::signature_identity) == 32);
static_assert(sizeof(gti_export_descriptor_v1) == 48);
static_assert(offsetof(gti_export_descriptor_v1, symbol) == 0);
static_assert(offsetof(gti_export_descriptor_v1, signature_identity) == 16);
static_assert(sizeof(gti_module_descriptor_v1) == 64);
static_assert(offsetof(gti_module_descriptor_v1, abi_version) == 0);
static_assert(offsetof(gti_module_descriptor_v1, struct_size) == 4);
static_assert(offsetof(gti_module_descriptor_v1, module_identity) == 8);
static_assert(offsetof(gti_module_descriptor_v1, failure_artifact) == 40);
static_assert(offsetof(gti_module_descriptor_v1, exports) == 48);
static_assert(offsetof(gti_module_descriptor_v1, export_count) == 56);
static_assert(offsetof(gti_module_descriptor_v1, reserved) == 60);
static_assert(sizeof(game_frame_v1) == 16);
static_assert(offsetof(game_frame_v1, frame) == 0);
static_assert(offsetof(game_frame_v1, delta_seconds) == 8);

using Descriptor = const gti_module_descriptor_v1 *(*)();
using Create = gti_embed_status_v1 (*)(std::uint32_t, gti_failure_observer_v1,
                                       void *, gti_context_v1 **,
                                       gti_failure_record_v1 *);
using Destroy = gti_embed_status_v1 (*)(gti_context_v1 *,
                                        gti_failure_record_v1 *);
using Update = gti_embed_status_v1 (*)(gti_context_v1 *, game_engine_v1 *,
                                       game_frame_v1, std::int32_t *,
                                       gti_failure_record_v1 *);

static_assert(
    std::is_same_v<decltype(&gti_get_module_descriptor_v1), Descriptor>);
static_assert(std::is_same_v<decltype(&gti_context_create_v1), Create>);
static_assert(std::is_same_v<decltype(&gti_context_destroy_v1), Destroy>);
static_assert(std::is_same_v<decltype(&game_update_v1), Update>);

int main() { return 0; }
