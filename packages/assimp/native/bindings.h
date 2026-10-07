#ifndef GTI_PACKAGE_ASSIMP_BINDINGS_H
#define GTI_PACKAGE_ASSIMP_BINDINGS_H
#include <assimp/postprocess.h>
#include <stdint.h>

struct aiScene;
struct aiNode;
struct aiMesh;

enum GtiAssimpPostProcess {
  GTI_ASSIMP_CALC_TANGENT_SPACE = aiProcess_CalcTangentSpace,
  GTI_ASSIMP_JOIN_IDENTICAL_VERTICES = aiProcess_JoinIdenticalVertices,
  GTI_ASSIMP_TRIANGULATE = aiProcess_Triangulate,
  GTI_ASSIMP_GEN_NORMALS = aiProcess_GenNormals,
  GTI_ASSIMP_GEN_SMOOTH_NORMALS = aiProcess_GenSmoothNormals,
  GTI_ASSIMP_PRE_TRANSFORM_VERTICES = aiProcess_PreTransformVertices,
  GTI_ASSIMP_VALIDATE_DATA_STRUCTURE = aiProcess_ValidateDataStructure,
  GTI_ASSIMP_IMPROVE_CACHE_LOCALITY = aiProcess_ImproveCacheLocality,
  GTI_ASSIMP_SORT_BY_PRIMITIVE_TYPE = aiProcess_SortByPType,
  GTI_ASSIMP_FLIP_UVS = aiProcess_FlipUVs,
  GTI_ASSIMP_FLIP_WINDING_ORDER = aiProcess_FlipWindingOrder
};

typedef struct GtiAssimpVector3 {
  float x;
  float y;
  float z;
} GtiAssimpVector3;

typedef struct GtiAssimpMatrix4 {
  float values[16]; /* row-major, matching Assimp's documented field order */
} GtiAssimpMatrix4;

// Native factories initialize passive C ABI records for GTI output parameters.
GtiAssimpVector3 gti_assimp_zero_vector(void);
GtiAssimpMatrix4 gti_assimp_zero_matrix(void);

// A successful import owns a scene; release it exactly once.
const struct aiScene *gti_assimp_import_file(const char *path, uint32_t flags);
const struct aiScene *gti_assimp_import_memory(const char *bytes,
                                               uint32_t length, uint32_t flags,
                                               const char *hint);
void gti_assimp_release(const struct aiScene *scene);
const char *gti_assimp_error(void);

// All node/mesh/name pointers are borrowed until gti_assimp_release(scene).
uint32_t gti_assimp_mesh_count(const struct aiScene *scene);
const struct aiMesh *gti_assimp_mesh(const struct aiScene *scene,
                                     uint32_t index);
const struct aiNode *gti_assimp_root(const struct aiScene *scene);
const char *gti_assimp_node_name(const struct aiNode *node);
uint32_t gti_assimp_child_count(const struct aiNode *node);
const struct aiNode *gti_assimp_child(const struct aiNode *node,
                                      uint32_t index);
uint32_t gti_assimp_node_mesh_count(const struct aiNode *node);
int32_t gti_assimp_node_mesh_index(const struct aiNode *node, uint32_t index,
                                   uint32_t *output);
int32_t gti_assimp_node_transform(const struct aiNode *node,
                                  GtiAssimpMatrix4 *output);
const char *gti_assimp_mesh_name(const struct aiMesh *mesh);
uint32_t gti_assimp_vertex_count(const struct aiMesh *mesh);
uint32_t gti_assimp_face_count(const struct aiMesh *mesh);
uint32_t gti_assimp_mesh_material(const struct aiMesh *mesh);
int32_t gti_assimp_position(const struct aiMesh *mesh, uint32_t vertex,
                            GtiAssimpVector3 *output);
int32_t gti_assimp_normal(const struct aiMesh *mesh, uint32_t vertex,
                          GtiAssimpVector3 *output);
int32_t gti_assimp_texcoord(const struct aiMesh *mesh, uint32_t channel,
                            uint32_t vertex, GtiAssimpVector3 *output);
uint32_t gti_assimp_face_size(const struct aiMesh *mesh, uint32_t face);
int32_t gti_assimp_face_index(const struct aiMesh *mesh, uint32_t face,
                              uint32_t corner, uint32_t *output);
#endif
