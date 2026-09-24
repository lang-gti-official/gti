#include "bindings.h"
#include <assimp/cimport.h>
#include <assimp/scene.h>

GtiAssimpVector3 gti_assimp_zero_vector(void) {
  const GtiAssimpVector3 result = {0};
  return result;
}
GtiAssimpMatrix4 gti_assimp_zero_matrix(void) {
  const GtiAssimpMatrix4 result = {{0}};
  return result;
}

const struct aiScene *gti_assimp_import_file(const char *path, uint32_t flags) {
  return aiImportFile(path, flags);
}
const struct aiScene *gti_assimp_import_memory(const char *bytes,
                                               uint32_t length, uint32_t flags,
                                               const char *hint) {
  return aiImportFileFromMemory(bytes, length, flags, hint);
}
void gti_assimp_release(const struct aiScene *scene) { aiReleaseImport(scene); }
const char *gti_assimp_error(void) { return aiGetErrorString(); }

uint32_t gti_assimp_mesh_count(const struct aiScene *scene) {
  return scene == NULL ? 0 : scene->mNumMeshes;
}
const struct aiMesh *gti_assimp_mesh(const struct aiScene *scene,
                                     uint32_t index) {
  return scene == NULL || index >= scene->mNumMeshes ? NULL
                                                     : scene->mMeshes[index];
}
const struct aiNode *gti_assimp_root(const struct aiScene *scene) {
  return scene == NULL ? NULL : scene->mRootNode;
}
const char *gti_assimp_node_name(const struct aiNode *node) {
  return node == NULL ? NULL : node->mName.data;
}
uint32_t gti_assimp_child_count(const struct aiNode *node) {
  return node == NULL ? 0 : node->mNumChildren;
}
const struct aiNode *gti_assimp_child(const struct aiNode *node,
                                      uint32_t index) {
  return node == NULL || index >= node->mNumChildren ? NULL
                                                     : node->mChildren[index];
}
uint32_t gti_assimp_node_mesh_count(const struct aiNode *node) {
  return node == NULL ? 0 : node->mNumMeshes;
}
int32_t gti_assimp_node_mesh_index(const struct aiNode *node, uint32_t index,
                                   uint32_t *output) {
  if (node == NULL || output == NULL || index >= node->mNumMeshes) {
    return 0;
  }
  *output = node->mMeshes[index];
  return 1;
}
int32_t gti_assimp_node_transform(const struct aiNode *node,
                                  GtiAssimpMatrix4 *output) {
  if (node == NULL || output == NULL) {
    return 0;
  }
  const struct aiMatrix4x4 *m = &node->mTransformation;
  const GtiAssimpMatrix4 value = {{m->a1, m->a2, m->a3, m->a4, m->b1, m->b2,
                                   m->b3, m->b4, m->c1, m->c2, m->c3, m->c4,
                                   m->d1, m->d2, m->d3, m->d4}};
  *output = value;
  return 1;
}
const char *gti_assimp_mesh_name(const struct aiMesh *mesh) {
  return mesh == NULL ? NULL : mesh->mName.data;
}
uint32_t gti_assimp_vertex_count(const struct aiMesh *mesh) {
  return mesh == NULL ? 0 : mesh->mNumVertices;
}
uint32_t gti_assimp_face_count(const struct aiMesh *mesh) {
  return mesh == NULL ? 0 : mesh->mNumFaces;
}
uint32_t gti_assimp_mesh_material(const struct aiMesh *mesh) {
  return mesh == NULL ? UINT32_MAX : mesh->mMaterialIndex;
}

static int32_t vector_at(const struct aiVector3D *values, uint32_t count,
                         uint32_t index, GtiAssimpVector3 *output) {
  if (values == NULL || output == NULL || index >= count) {
    return 0;
  }
  const GtiAssimpVector3 value = {values[index].x, values[index].y,
                                  values[index].z};
  *output = value;
  return 1;
}
int32_t gti_assimp_position(const struct aiMesh *mesh, uint32_t vertex,
                            GtiAssimpVector3 *output) {
  return mesh == NULL
             ? 0
             : vector_at(mesh->mVertices, mesh->mNumVertices, vertex, output);
}
int32_t gti_assimp_normal(const struct aiMesh *mesh, uint32_t vertex,
                          GtiAssimpVector3 *output) {
  return mesh == NULL
             ? 0
             : vector_at(mesh->mNormals, mesh->mNumVertices, vertex, output);
}
int32_t gti_assimp_texcoord(const struct aiMesh *mesh, uint32_t channel,
                            uint32_t vertex, GtiAssimpVector3 *output) {
  return mesh == NULL || channel >= AI_MAX_NUMBER_OF_TEXTURECOORDS
             ? 0
             : vector_at(mesh->mTextureCoords[channel], mesh->mNumVertices,
                         vertex, output);
}
uint32_t gti_assimp_face_size(const struct aiMesh *mesh, uint32_t face) {
  return mesh == NULL || face >= mesh->mNumFaces
             ? 0
             : mesh->mFaces[face].mNumIndices;
}
int32_t gti_assimp_face_index(const struct aiMesh *mesh, uint32_t face,
                              uint32_t corner, uint32_t *output) {
  if (mesh == NULL || output == NULL || face >= mesh->mNumFaces ||
      corner >= mesh->mFaces[face].mNumIndices) {
    return 0;
  }
  const uint32_t index = mesh->mFaces[face].mIndices[corner];
  if (index >= mesh->mNumVertices) {
    return 0;
  }
  *output = index;
  return 1;
}
