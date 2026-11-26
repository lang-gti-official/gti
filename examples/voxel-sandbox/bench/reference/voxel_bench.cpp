// C++20 reference port of the voxel-sandbox world kernels and headless
// benchmark: src/world/{block,chunk,noise,worldgen,mesher,world}.gti and
// src/bench/headless.gti, translated algorithm-for-algorithm (same data
// layout, same loops, same containers) so it is a fair cross-language
// baseline. It must produce the same blocks, quads and checksum as GTI.
//
//   clang++ -O3 -std=c++20 -ffp-contract=off voxel_bench.cpp -o voxel_bench_cpp
//   voxel_bench_cpp [--seed N] [--radius R] [--reps K] [--threads T] [--json PATH]
//
// -ffp-contract=off keeps a*b+c as two roundings, matching GTI's per-operation
// IEEE binary32 semantics (no FMA contraction).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace voxel {

// ---- block.gti -------------------------------------------------------------
constexpr uint8_t AIR = 0, STONE = 1, DIRT = 2, GRASS = 3, SAND = 4, WATER = 5,
                  LOG = 6, LEAVES = 7, PLANKS = 8, COBBLESTONE = 9, BEDROCK = 10,
                  SNOW = 11, GRAVEL = 12, COAL_ORE = 13, IRON_ORE = 14,
                  GLASS = 15, CACTUS = 16, BRICK = 17, SANDSTONE = 18;
constexpr uint32_t T_STONE = 0, T_DIRT = 1, T_GRASS_TOP = 2, T_GRASS_SIDE = 3,
                   T_SAND = 4, T_WATER = 5, T_LOG_SIDE = 6, T_LOG_TOP = 7,
                   T_LEAVES = 8, T_PLANKS = 9, T_COBBLESTONE = 10,
                   T_BEDROCK = 11, T_SNOW = 12, T_SNOW_SIDE = 13, T_GRAVEL = 14,
                   T_COAL_ORE = 15, T_IRON_ORE = 16, T_GLASS = 17,
                   T_CACTUS_SIDE = 18, T_CACTUS_TOP = 19, T_BRICK = 20,
                   T_SANDSTONE = 21, T_SANDSTONE_TOP = 22;
constexpr int32_t FACE_POS_X = 0, FACE_NEG_X = 1, FACE_POS_Y = 2,
                  FACE_NEG_Y = 3, FACE_POS_Z = 4, FACE_NEG_Z = 5;

bool is_solid(uint8_t id) { return id != AIR && id != WATER; }
bool is_opaque(uint8_t id) {
  return id != AIR && id != WATER && id != GLASS && id != LEAVES;
}
bool is_cutout(uint8_t id) { return id == LEAVES || id == GLASS; }

uint32_t tile_for(uint8_t id, int32_t face) {
  bool top = face == FACE_POS_Y;
  bool bottom = face == FACE_NEG_Y;
  if (id == STONE) return T_STONE;
  if (id == DIRT) return T_DIRT;
  if (id == GRASS) {
    if (top) return T_GRASS_TOP;
    if (bottom) return T_DIRT;
    return T_GRASS_SIDE;
  }
  if (id == SAND) return T_SAND;
  if (id == WATER) return T_WATER;
  if (id == LOG) {
    if (top || bottom) return T_LOG_TOP;
    return T_LOG_SIDE;
  }
  if (id == LEAVES) return T_LEAVES;
  if (id == PLANKS) return T_PLANKS;
  if (id == COBBLESTONE) return T_COBBLESTONE;
  if (id == BEDROCK) return T_BEDROCK;
  if (id == SNOW) {
    if (top) return T_SNOW;
    if (bottom) return T_DIRT;
    return T_SNOW_SIDE;
  }
  if (id == GRAVEL) return T_GRAVEL;
  if (id == COAL_ORE) return T_COAL_ORE;
  if (id == IRON_ORE) return T_IRON_ORE;
  if (id == GLASS) return T_GLASS;
  if (id == CACTUS) {
    if (top || bottom) return T_CACTUS_TOP;
    return T_CACTUS_SIDE;
  }
  if (id == BRICK) return T_BRICK;
  if (id == SANDSTONE) {
    if (top || bottom) return T_SANDSTONE_TOP;
    return T_SANDSTONE;
  }
  return T_STONE;
}

// ---- chunk.gti -------------------------------------------------------------
constexpr int32_t CHUNK_WIDTH = 16, CHUNK_DEPTH = 16, CHUNK_HEIGHT = 128;
constexpr int32_t CHUNK_LAYER = CHUNK_WIDTH * CHUNK_DEPTH;
constexpr int32_t CHUNK_VOLUME = CHUNK_LAYER * CHUNK_HEIGHT;
constexpr int32_t SEA_LEVEL = 48;

int32_t chunk_index(int32_t x, int32_t y, int32_t z) {
  return (y * CHUNK_DEPTH + z) * CHUNK_WIDTH + x;
}
int32_t chunk_coord(int32_t world) {
  if (world >= 0) return world / CHUNK_WIDTH;
  return -((-world - 1) / CHUNK_WIDTH) - 1;
}
int32_t local_coord(int32_t world) {
  return world - chunk_coord(world) * CHUNK_WIDTH;
}

struct Chunk {
  int32_t cx;
  int32_t cz;
  std::vector<uint8_t> blocks;
  uint32_t revision = 0;
  Chunk(int32_t cx, int32_t cz) : cx(cx), cz(cz) {
    blocks.reserve(uint64_t(CHUNK_VOLUME));
    for (int32_t i = 0; i < CHUNK_VOLUME; i++) blocks.push_back(AIR);
  }
  Chunk(const Chunk&) = delete;
};

// ---- noise.gti -------------------------------------------------------------
uint32_t to_bits(int32_t value) { return uint32_t(value); }

uint32_t mix32(uint32_t h) {
  h = h ^ (h >> 16);
  h = h * 2146121005u;
  h = h ^ (h >> 15);
  h = h * 2221713035u;
  h = h ^ (h >> 16);
  return h;
}
uint32_t hash2(uint32_t seed, int32_t x, int32_t z) {
  uint32_t h = mix32(seed ^ (to_bits(x) * 2654435761u));
  h = mix32(h ^ (to_bits(z) * 2246822519u));
  return h;
}
uint32_t hash3(uint32_t seed, int32_t x, int32_t y, int32_t z) {
  uint32_t h = mix32(seed ^ (to_bits(x) * 2654435761u));
  h = mix32(h ^ (to_bits(y) * 3266489917u));
  h = mix32(h ^ (to_bits(z) * 668265263u));
  return h;
}
uint32_t seed_offset(uint32_t seed, uint32_t salt) {
  return mix32(seed + salt * 2654435761u);
}
int32_t fast_floor(float value) {
  int32_t truncated = int32_t(value);
  if (value < float(truncated)) return truncated - 1;
  return truncated;
}
float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
float grad2(uint32_t h, float dx, float dz) {
  uint32_t k = h & 7u;
  if (k == 0) return dx;
  if (k == 1) return -dx;
  if (k == 2) return dz;
  if (k == 3) return -dz;
  if (k == 4) return dx + dz;
  if (k == 5) return -dx + dz;
  if (k == 6) return dx - dz;
  return -dx - dz;
}
float grad3(uint32_t h, float dx, float dy, float dz) {
  uint32_t k = h & 15u;
  float u = k < 8u ? dx : dy;
  float v = k < 4u ? dy : ((k == 12u || k == 14u) ? dx : dz);
  float a = (k & 1u) == 0 ? u : -u;
  float b = (k & 2u) == 0 ? v : -v;
  return a + b;
}
float perlin2(uint32_t seed, float x, float z) {
  int32_t x0 = fast_floor(x);
  int32_t z0 = fast_floor(z);
  float fx = x - float(x0);
  float fz = z - float(z0);
  float u = fade(fx);
  float v = fade(fz);
  float n00 = grad2(hash2(seed, x0, z0), fx, fz);
  float n10 = grad2(hash2(seed, x0 + 1, z0), fx - 1.0f, fz);
  float n01 = grad2(hash2(seed, x0, z0 + 1), fx, fz - 1.0f);
  float n11 = grad2(hash2(seed, x0 + 1, z0 + 1), fx - 1.0f, fz - 1.0f);
  return lerp(lerp(n00, n10, u), lerp(n01, n11, u), v);
}
float perlin3(uint32_t seed, float x, float y, float z) {
  int32_t x0 = fast_floor(x);
  int32_t y0 = fast_floor(y);
  int32_t z0 = fast_floor(z);
  float fx = x - float(x0);
  float fy = y - float(y0);
  float fz = z - float(z0);
  float u = fade(fx);
  float v = fade(fy);
  float w = fade(fz);
  float n000 = grad3(hash3(seed, x0, y0, z0), fx, fy, fz);
  float n100 = grad3(hash3(seed, x0 + 1, y0, z0), fx - 1.0f, fy, fz);
  float n010 = grad3(hash3(seed, x0, y0 + 1, z0), fx, fy - 1.0f, fz);
  float n110 = grad3(hash3(seed, x0 + 1, y0 + 1, z0), fx - 1.0f, fy - 1.0f, fz);
  float n001 = grad3(hash3(seed, x0, y0, z0 + 1), fx, fy, fz - 1.0f);
  float n101 = grad3(hash3(seed, x0 + 1, y0, z0 + 1), fx - 1.0f, fy, fz - 1.0f);
  float n011 = grad3(hash3(seed, x0, y0 + 1, z0 + 1), fx, fy - 1.0f, fz - 1.0f);
  float n111 = grad3(hash3(seed, x0 + 1, y0 + 1, z0 + 1), fx - 1.0f, fy - 1.0f, fz - 1.0f);
  float x00 = lerp(n000, n100, u);
  float x10 = lerp(n010, n110, u);
  float x01 = lerp(n001, n101, u);
  float x11 = lerp(n011, n111, u);
  return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}
float fbm2(uint32_t seed, float x, float z, int32_t octaves) {
  float sum = 0.0f, amplitude = 1.0f, total = 0.0f, frequency = 1.0f;
  for (int32_t octave = 0; octave < octaves; octave++) {
    uint32_t octave_seed = seed + uint32_t(octave) * 1013u;
    sum += amplitude * perlin2(octave_seed, x * frequency, z * frequency);
    total += amplitude;
    amplitude = amplitude * 0.5f;
    frequency = frequency * 2.0f;
  }
  return sum * (1.0f / total);
}

// ---- worldgen.gti ----------------------------------------------------------
constexpr int32_t BIOME_PLAINS = 0, BIOME_FOREST = 1, BIOME_DESERT = 2,
                  BIOME_SNOWY = 3;
constexpr int32_t TREE_MARGIN = 2;
constexpr int32_t COLUMN_SPAN = 20;

struct ColumnInfo {
  int32_t height = 0;
  int32_t biome = 0;
  bool gravel = false;
};

float clamp01(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

void column_info(uint32_t seed, int32_t wx, int32_t wz, ColumnInfo& out) {
  float fx = float(wx);
  float fz = float(wz);
  float continent = fbm2(seed_offset(seed, 1), fx * 0.00390625f, fz * 0.00390625f, 4);
  float detail = fbm2(seed_offset(seed, 2), fx * 0.015625f, fz * 0.015625f, 3);
  float ridge = fbm2(seed_offset(seed, 3), fx * 0.001953125f, fz * 0.001953125f, 3);
  float temperature = fbm2(seed_offset(seed, 4), fx * 0.001953125f, fz * 0.001953125f, 2);
  float humidity = fbm2(seed_offset(seed, 5), fx * 0.00390625f, fz * 0.00390625f, 2);
  float mountain = clamp01((ridge - 0.05f) * 4.0f);
  float height_f = 57.0f + continent * 28.0f + detail * 6.0f + mountain * mountain * 52.0f;
  int32_t height = fast_floor(height_f);
  if (height < 4) height = 4;
  if (height > 124) height = 124;
  out.height = height;
  if ((mountain > 0.4f && height > 76) || temperature < -0.4f) {
    out.biome = BIOME_SNOWY;
  } else if (temperature > 0.1f && humidity < 0.12f) {
    out.biome = BIOME_DESERT;
  } else if (humidity > 0.05f) {
    out.biome = BIOME_FOREST;
  } else {
    out.biome = BIOME_PLAINS;
  }
  out.gravel = perlin2(seed_offset(seed, 6), fx * 0.0625f, fz * 0.0625f) > 0.3f;
}

uint8_t surface_block(const ColumnInfo& column, int32_t y) {
  int32_t depth = column.height - y;
  if (column.biome == BIOME_DESERT) {
    if (depth < 4) return SAND;
    if (depth < 7) return SANDSTONE;
    return STONE;
  }
  if (column.height <= SEA_LEVEL + 1) {
    if (depth < 3) return column.gravel ? GRAVEL : SAND;
    return STONE;
  }
  if (column.biome == BIOME_SNOWY) {
    if (depth == 0) return SNOW;
    if (depth < 3 && column.height < 72) return DIRT;
    return STONE;
  }
  if (depth == 0) return GRASS;
  if (depth < 4) return DIRT;
  return STONE;
}

void put_if_air(Chunk& chunk, int32_t lx, int32_t y, int32_t lz, uint8_t id) {
  if (lx < 0 || lx >= CHUNK_WIDTH || lz < 0 || lz >= CHUNK_DEPTH) return;
  if (y < 0 || y >= CHUNK_HEIGHT) return;
  uint64_t index = uint64_t(chunk_index(lx, y, lz));
  if (chunk.blocks[index] == AIR) chunk.blocks[index] = id;
}
void put_block(Chunk& chunk, int32_t lx, int32_t y, int32_t lz, uint8_t id) {
  if (lx < 0 || lx >= CHUNK_WIDTH || lz < 0 || lz >= CHUNK_DEPTH) return;
  if (y < 0 || y >= CHUNK_HEIGHT) return;
  chunk.blocks[uint64_t(chunk_index(lx, y, lz))] = id;
}

void place_tree(Chunk& chunk, int32_t lx, int32_t lz, int32_t ground, uint32_t h) {
  int32_t trunk = 4 + int32_t((h >> 8) % 3u);
  int32_t top = ground + trunk;
  bool round_corners = ((h >> 12) & 1u) == 1u;
  for (int32_t y = top - 2; y <= top + 1; y++) {
    int32_t radius = y >= top ? 1 : 2;
    for (int32_t dz = -radius; dz <= radius; dz++) {
      for (int32_t dx = -radius; dx <= radius; dx++) {
        bool corner = (dx == radius || dx == -radius) && (dz == radius || dz == -radius);
        if (corner && (radius == 1 || round_corners)) continue;
        put_if_air(chunk, lx + dx, y, lz + dz, LEAVES);
      }
    }
  }
  for (int32_t y = ground + 1; y <= top; y++) put_block(chunk, lx, y, lz, LOG);
  put_block(chunk, lx, ground, lz, DIRT);
}

void generate_chunk(Chunk& chunk, uint32_t seed) {
  int32_t base_x = chunk.cx * CHUNK_WIDTH;
  int32_t base_z = chunk.cz * CHUNK_DEPTH;
  uint32_t cave_a = seed_offset(seed, 7);
  uint32_t cave_b = seed_offset(seed, 8);
  uint32_t ore_seed = seed_offset(seed, 9);
  uint32_t bedrock_seed = seed_offset(seed, 10);
  uint32_t plant_seed = seed_offset(seed, 11);

  std::vector<ColumnInfo> columns;
  columns.reserve(uint64_t(COLUMN_SPAN * COLUMN_SPAN));
  for (int32_t cz = 0; cz < COLUMN_SPAN; cz++) {
    for (int32_t cx = 0; cx < COLUMN_SPAN; cx++) {
      ColumnInfo info{};
      column_info(seed, base_x + cx - TREE_MARGIN, base_z + cz - TREE_MARGIN, info);
      columns.push_back(info);
    }
  }

  for (int32_t lz = 0; lz < CHUNK_DEPTH; lz++) {
    for (int32_t lx = 0; lx < CHUNK_WIDTH; lx++) {
      const ColumnInfo& column = columns[uint64_t((lz + TREE_MARGIN) * COLUMN_SPAN + lx + TREE_MARGIN)];
      int32_t wx = base_x + lx;
      int32_t wz = base_z + lz;
      int32_t height = column.height;
      for (int32_t y = 0; y < CHUNK_HEIGHT; y++) {
        uint8_t id = AIR;
        if (y <= height) {
          id = surface_block(column, y);
        } else if (y <= SEA_LEVEL) {
          id = WATER;
        }
        if (y == 0) {
          id = BEDROCK;
        } else if (y <= 2 && hash3(bedrock_seed, wx, y, wz) % 3u == 0u) {
          id = BEDROCK;
        }
        if (id == STONE) {
          if (y >= 4 && y <= height - 4) {
            float fx = float(wx) * 0.03125f;
            float fy = float(y) * 0.0625f;
            float fz = float(wz) * 0.03125f;
            float a = perlin3(cave_a, fx, fy, fz);
            float b = perlin3(cave_b, fx, fy, fz);
            if (a * a + b * b < 0.006f) id = AIR;
          }
        }
        if (id == STONE) {
          uint32_t r = hash3(ore_seed, wx, y, wz) % 1000u;
          if (y < 64 && r < 10u) {
            id = COAL_ORE;
          } else if (y < 40 && r >= 10u && r < 16u) {
            id = IRON_ORE;
          }
        }
        chunk.blocks[uint64_t(chunk_index(lx, y, lz))] = id;
      }
    }
  }

  for (int32_t lz = 0; lz < CHUNK_DEPTH; lz++) {
    for (int32_t lx = 0; lx < CHUNK_WIDTH; lx++) {
      const ColumnInfo& column = columns[uint64_t((lz + TREE_MARGIN) * COLUMN_SPAN + lx + TREE_MARGIN)];
      if (column.biome != BIOME_DESERT || column.height <= SEA_LEVEL + 1) continue;
      uint32_t h = hash2(plant_seed, base_x + lx, base_z + lz);
      if (h % 1000u >= 6u) continue;
      int32_t tall = 1 + int32_t((h >> 8) % 3u);
      for (int32_t y = column.height + 1; y <= column.height + tall; y++) {
        put_if_air(chunk, lx, y, lz, CACTUS);
      }
    }
  }

  for (int32_t cz = 0; cz < COLUMN_SPAN; cz++) {
    for (int32_t cx = 0; cx < COLUMN_SPAN; cx++) {
      const ColumnInfo& column = columns[uint64_t(cz * COLUMN_SPAN + cx)];
      if (column.biome != BIOME_FOREST && column.biome != BIOME_PLAINS) continue;
      if (column.height <= SEA_LEVEL + 1 || column.height > 116) continue;
      int32_t lx = cx - TREE_MARGIN;
      int32_t lz = cz - TREE_MARGIN;
      uint32_t h = hash2(plant_seed, base_x + lx, base_z + lz);
      uint32_t chance = column.biome == BIOME_FOREST ? 30u : 4u;
      if (h % 1000u >= chance) continue;
      place_tree(chunk, lx, lz, column.height, h);
    }
  }
}

// ---- mesher.gti ------------------------------------------------------------
constexpr int32_t PAD_W = 18, PAD_H = 130;
constexpr int32_t PAD_LAYER = PAD_W * PAD_W;
constexpr int32_t PAD_VOLUME = PAD_LAYER * PAD_H;

int32_t pad_index(int32_t x, int32_t y, int32_t z) {
  return ((y + 1) * PAD_W + (z + 1)) * PAD_W + (x + 1);
}
uint32_t pack_word0(int32_t x, int32_t y, int32_t z, int32_t face, int32_t ao) {
  return uint32_t(x) | (uint32_t(y) << 5) | (uint32_t(z) << 13) |
         (uint32_t(face) << 18) | (uint32_t(ao) << 21);
}
uint32_t pack_word1(uint32_t tile, bool water_surface) {
  return tile | (water_surface ? 256u : 0u);
}

struct ChunkMesh {
  std::vector<uint32_t> opaque;
  std::vector<uint32_t> translucent;
  void clear() {
    opaque.clear();
    translucent.clear();
  }
  int64_t opaque_quads() const { return int64_t(opaque.size() / 8); }
  int64_t translucent_quads() const { return int64_t(translucent.size() / 8); }
};

int32_t face_axis(int32_t face) { return face / 2; }
int32_t face_sign(int32_t face) { return (face % 2) == 0 ? 1 : -1; }

int32_t corner_offset(int32_t face, int32_t c, int32_t axis) {
  if (face == FACE_POS_X) {
    if (axis == 0) return 1;
    if (axis == 1) return c >= 2 ? 1 : 0;
    return (c == 0 || c == 3) ? 1 : 0;
  }
  if (face == FACE_NEG_X) {
    if (axis == 0) return 0;
    if (axis == 1) return c >= 2 ? 1 : 0;
    return (c == 1 || c == 2) ? 1 : 0;
  }
  if (face == FACE_POS_Y) {
    if (axis == 1) return 1;
    if (axis == 0) return (c == 1 || c == 2) ? 1 : 0;
    return c <= 1 ? 1 : 0;
  }
  if (face == FACE_NEG_Y) {
    if (axis == 1) return 0;
    if (axis == 0) return (c == 1 || c == 2) ? 1 : 0;
    return c >= 2 ? 1 : 0;
  }
  if (face == FACE_POS_Z) {
    if (axis == 2) return 1;
    if (axis == 0) return (c == 1 || c == 2) ? 1 : 0;
    return c >= 2 ? 1 : 0;
  }
  if (axis == 2) return 0;
  if (axis == 0) return (c == 0 || c == 3) ? 1 : 0;
  return c >= 2 ? 1 : 0;
}

bool occludes(uint8_t id) { return is_opaque(id); }
bool face_visible(uint8_t id, uint8_t neighbor) {
  if (id == WATER) return neighbor != WATER && !is_opaque(neighbor);
  if (is_opaque(neighbor)) return false;
  if (is_cutout(id) && neighbor == id) return false;
  return true;
}
int32_t ao_value(bool side1, bool side2, bool corner) {
  if (side1 && side2) return 0;
  int32_t n = 0;
  if (side1) n++;
  if (side2) n++;
  if (corner) n++;
  return 3 - n;
}

class Mesher {
  std::vector<uint32_t> mask;

public:
  int32_t corner_ao(const std::vector<uint8_t>& padded, int32_t front, int32_t face,
                    int32_t c) const {
    int32_t axis = face_axis(face);
    int32_t ua = axis == 0 ? 1 : 0;
    int32_t va = axis == 2 ? 1 : 2;
    int32_t ustep = ua == 0 ? 1 : PAD_LAYER;
    int32_t vstep = va == 1 ? PAD_LAYER : PAD_W;
    int32_t du = corner_offset(face, c, ua) == 1 ? ustep : -ustep;
    int32_t dv = corner_offset(face, c, va) == 1 ? vstep : -vstep;
    bool s1 = occludes(padded[uint64_t(front + du)]);
    bool s2 = occludes(padded[uint64_t(front + dv)]);
    bool cc = occludes(padded[uint64_t(front + du + dv)]);
    return ao_value(s1, s2, cc);
  }

  void emit_quad(std::vector<uint32_t>& out, int32_t x, int32_t y, int32_t z,
                 int32_t face, int32_t w, int32_t h, int32_t ao0, int32_t ao1,
                 int32_t ao2, int32_t ao3, uint32_t tile, bool water_surface) const {
    int32_t axis = face_axis(face);
    int32_t ua = axis == 0 ? 1 : 0;
    int32_t va = axis == 2 ? 1 : 2;
    uint32_t word1 = pack_word1(tile, water_surface);
    int32_t start = (ao0 + ao2 < ao1 + ao3) ? 1 : 0;
    for (int32_t k = 0; k < 4; k++) {
      int32_t c = (k + start) % 4;
      int32_t vx = x + corner_offset(face, c, 0);
      int32_t vy = y + corner_offset(face, c, 1);
      int32_t vz = z + corner_offset(face, c, 2);
      if (corner_offset(face, c, ua) == 1) {
        if (ua == 0) vx += w - 1; else vy += w - 1;
      }
      if (corner_offset(face, c, va) == 1) {
        if (va == 1) vy += h - 1; else vz += h - 1;
      }
      int32_t ao = c == 0 ? ao0 : (c == 1 ? ao1 : (c == 2 ? ao2 : ao3));
      out.push_back(pack_word0(vx, vy, vz, face, ao));
      out.push_back(word1);
    }
  }

  void mesh_plain(const std::vector<uint8_t>& padded, ChunkMesh& out) const {
    out.clear();
    for (int32_t y = 0; y < CHUNK_HEIGHT; y++) {
      for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
        for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
          int32_t index = pad_index(x, y, z);
          uint8_t id = padded[uint64_t(index)];
          if (id == AIR) continue;
          for (int32_t face = 0; face < 6; face++) {
            int32_t axis = face_axis(face);
            int32_t sign = face_sign(face);
            int32_t step = axis == 0 ? 1 : (axis == 1 ? PAD_LAYER : PAD_W);
            uint8_t neighbor = padded[uint64_t(index + sign * step)];
            if (!face_visible(id, neighbor)) continue;
            int32_t ao0 = corner_ao(padded, index + sign * step, face, 0);
            int32_t ao1 = corner_ao(padded, index + sign * step, face, 1);
            int32_t ao2 = corner_ao(padded, index + sign * step, face, 2);
            int32_t ao3 = corner_ao(padded, index + sign * step, face, 3);
            uint32_t tile = tile_for(id, face);
            if (id == WATER) {
              bool surface = face == FACE_POS_Y && neighbor != WATER;
              emit_quad(out.translucent, x, y, z, face, 1, 1, ao0, ao1, ao2, ao3, tile, surface);
            } else {
              emit_quad(out.opaque, x, y, z, face, 1, 1, ao0, ao1, ao2, ao3, tile, false);
            }
          }
        }
      }
    }
  }

  uint32_t face_key(const std::vector<uint8_t>& padded, int32_t x, int32_t y, int32_t z,
                    int32_t face) const {
    int32_t index = pad_index(x, y, z);
    uint8_t id = padded[uint64_t(index)];
    if (id == AIR) return 0u;
    int32_t axis = face_axis(face);
    int32_t sign = face_sign(face);
    int32_t step = axis == 0 ? 1 : (axis == 1 ? PAD_LAYER : PAD_W);
    uint8_t neighbor = padded[uint64_t(index + sign * step)];
    if (!face_visible(id, neighbor)) return 0u;
    uint32_t ao0 = uint32_t(corner_ao(padded, index + sign * step, face, 0));
    uint32_t ao1 = uint32_t(corner_ao(padded, index + sign * step, face, 1));
    uint32_t ao2 = uint32_t(corner_ao(padded, index + sign * step, face, 2));
    uint32_t ao3 = uint32_t(corner_ao(padded, index + sign * step, face, 3));
    bool water = id == WATER;
    bool surface = water && face == FACE_POS_Y && neighbor != WATER;
    return 1u | (tile_for(id, face) << 1) | (ao0 << 9) | (ao1 << 11) | (ao2 << 13) |
           (ao3 << 15) | (surface ? 1u << 17 : 0u) | (water ? 1u << 18 : 0u);
  }

  void mesh_greedy(const std::vector<uint8_t>& padded, ChunkMesh& out) {
    out.clear();
    for (int32_t face = 0; face < 6; face++) {
      int32_t axis = face_axis(face);
      int32_t ua = axis == 0 ? 1 : 0;
      int32_t va = axis == 2 ? 1 : 2;
      int32_t depth_n = axis == 1 ? CHUNK_HEIGHT : CHUNK_WIDTH;
      int32_t u_n = ua == 1 ? CHUNK_HEIGHT : CHUNK_WIDTH;
      int32_t v_n = va == 1 ? CHUNK_HEIGHT : CHUNK_DEPTH;
      uint64_t mask_size = uint64_t(u_n * v_n);
      if (mask.size() < mask_size) mask.resize(mask_size, 0u);
      for (int32_t d = 0; d < depth_n; d++) {
        for (int32_t v = 0; v < v_n; v++) {
          for (int32_t u = 0; u < u_n; u++) {
            int32_t x = 0, y = 0, z = 0;
            if (axis == 0) { x = d; y = u; z = v; }
            else if (axis == 1) { y = d; x = u; z = v; }
            else { z = d; x = u; y = v; }
            mask[uint64_t(v * u_n + u)] = face_key(padded, x, y, z, face);
          }
        }
        for (int32_t v = 0; v < v_n; v++) {
          int32_t u = 0;
          while (u < u_n) {
            uint32_t key = mask[uint64_t(v * u_n + u)];
            if (key == 0u) { u++; continue; }
            int32_t w = 1;
            while (u + w < u_n && mask[uint64_t(v * u_n + u + w)] == key) w++;
            int32_t h = 1;
            bool grow = true;
            while (grow && v + h < v_n) {
              for (int32_t k = 0; k < w; k++) {
                if (mask[uint64_t((v + h) * u_n + u + k)] != key) { grow = false; break; }
              }
              if (grow) h++;
            }
            for (int32_t hv = 0; hv < h; hv++) {
              for (int32_t k = 0; k < w; k++) mask[uint64_t((v + hv) * u_n + u + k)] = 0u;
            }
            int32_t x = 0, y = 0, z = 0;
            if (axis == 0) { x = d; y = u; z = v; }
            else if (axis == 1) { y = d; x = u; z = v; }
            else { z = d; x = u; y = v; }
            uint32_t tile = (key >> 1) & 255u;
            int32_t ao0 = int32_t((key >> 9) & 3u);
            int32_t ao1 = int32_t((key >> 11) & 3u);
            int32_t ao2 = int32_t((key >> 13) & 3u);
            int32_t ao3 = int32_t((key >> 15) & 3u);
            bool surface = ((key >> 17) & 1u) == 1u;
            bool water = ((key >> 18) & 1u) == 1u;
            if (water) {
              emit_quad(out.translucent, x, y, z, face, w, h, ao0, ao1, ao2, ao3, tile, surface);
            } else {
              emit_quad(out.opaque, x, y, z, face, w, h, ao0, ao1, ao2, ao3, tile, false);
            }
            u += w;
          }
        }
      }
    }
  }
};

// ---- world.gti -------------------------------------------------------------
int64_t chunk_key(int32_t cx, int32_t cz) {
  return int64_t(uint64_t(int64_t(cx)) << 32) ^ (int64_t(cz) & 4294967295LL);
}

class World {
  std::unordered_map<int64_t, std::unique_ptr<Chunk>> chunks;
  std::vector<uint8_t> padded;
  Mesher mesher;

public:
  uint32_t seed;
  bool greedy = true;

  explicit World(uint32_t seed) : seed(seed) {}

  bool has_chunk(int32_t cx, int32_t cz) const { return chunks.contains(chunk_key(cx, cz)); }

  void generate_chunk(int32_t cx, int32_t cz) {
    if (has_chunk(cx, cz)) return;
    auto chunk = std::make_unique<Chunk>(cx, cz);
    voxel::generate_chunk(*chunk, seed);
    chunks.insert({chunk_key(cx, cz), std::move(chunk)});
  }

  bool insert_chunk(std::unique_ptr<Chunk> chunk) {
    int64_t key = chunk_key(chunk->cx, chunk->cz);
    if (chunks.contains(key)) return false;
    chunks.insert({key, std::move(chunk)});
    return true;
  }

  bool neighbors_loaded(int32_t cx, int32_t cz) const {
    for (int32_t dz = -1; dz <= 1; dz++) {
      for (int32_t dx = -1; dx <= 1; dx++) {
        if (dx == 0 && dz == 0) continue;
        if (!has_chunk(cx + dx, cz + dz)) return false;
      }
    }
    return true;
  }

  void fill_padded(int32_t cx, int32_t cz) {
    if (padded.size() != uint64_t(PAD_VOLUME)) {
      padded.clear();
      padded.resize(uint64_t(PAD_VOLUME), AIR);
    }
    for (int32_t z = -1; z <= CHUNK_DEPTH; z++) {
      for (int32_t x = -1; x <= CHUNK_WIDTH; x++) {
        padded[uint64_t(pad_index(x, -1, z))] = BEDROCK;
        padded[uint64_t(pad_index(x, CHUNK_HEIGHT, z))] = AIR;
      }
    }
    for (int32_t dz = -1; dz <= 1; dz++) {
      for (int32_t dx = -1; dx <= 1; dx++) {
        const Chunk& source = *chunks.at(chunk_key(cx + dx, cz + dz));
        int32_t x0 = dx < 0 ? -1 : (dx == 0 ? 0 : CHUNK_WIDTH);
        int32_t x1 = dx < 0 ? -1 : (dx == 0 ? CHUNK_WIDTH - 1 : CHUNK_WIDTH);
        int32_t z0 = dz < 0 ? -1 : (dz == 0 ? 0 : CHUNK_DEPTH);
        int32_t z1 = dz < 0 ? -1 : (dz == 0 ? CHUNK_DEPTH - 1 : CHUNK_DEPTH);
        int32_t ox = dx * CHUNK_WIDTH;
        int32_t oz = dz * CHUNK_DEPTH;
        for (int32_t y = 0; y < CHUNK_HEIGHT; y++) {
          for (int32_t z = z0; z <= z1; z++) {
            for (int32_t x = x0; x <= x1; x++) {
              padded[uint64_t(pad_index(x, y, z))] =
                  source.blocks[uint64_t(chunk_index(x - ox, y, z - oz))];
            }
          }
        }
      }
    }
  }

  bool build_mesh(int32_t cx, int32_t cz, ChunkMesh& out) {
    if (!has_chunk(cx, cz) || !neighbors_loaded(cx, cz)) return false;
    fill_padded(cx, cz);
    if (greedy) mesher.mesh_greedy(padded, out);
    else mesher.mesh_plain(padded, out);
    return true;
  }

  const Chunk& chunk_at(int32_t cx, int32_t cz) const { return *chunks.at(chunk_key(cx, cz)); }
};

// ---- headless.gti ----------------------------------------------------------
int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct PhaseStats {
  int64_t count = 0, total_ns = 0, min_ns = 0, median_ns = 0, p95_ns = 0, max_ns = 0;
};

void summarize(std::vector<int64_t>& samples, std::vector<int64_t>& totals, int64_t per_rep,
               PhaseStats& out) {
  std::sort(samples.begin(), samples.end());
  std::sort(totals.begin(), totals.end());
  uint64_t n = samples.size();
  out.count = per_rep;
  if (n == 0) return;
  out.min_ns = samples[0];
  out.median_ns = samples[n / 2];
  uint64_t p95 = (n * 95) / 100;
  if (p95 >= n) p95 = n - 1;
  out.p95_ns = samples[p95];
  out.max_ns = samples[n - 1];
  out.total_ns = totals[totals.size() / 2];
}

uint64_t fnv_byte(uint64_t h, uint64_t byte) { return (h ^ byte) * 1099511628211ull; }
uint64_t fnv_u64(uint64_t h, uint64_t value) {
  for (uint64_t i = 0; i < 8; i++) h = fnv_byte(h, (value >> (i * 8)) & 255);
  return h;
}

std::string phase_json(const PhaseStats& s) {
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "{\"count\": %lld, \"total_ns\": %lld, \"min_ns\": %lld, \"median_ns\": %lld, "
                "\"p95_ns\": %lld, \"max_ns\": %lld}",
                (long long)s.count, (long long)s.total_ns, (long long)s.min_ns,
                (long long)s.median_ns, (long long)s.p95_ns, (long long)s.max_ns);
  return buf;
}

std::string us(int64_t ns) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%lld.%03lld", (long long)(ns / 1000), (long long)(ns % 1000));
  return buf;
}

void print_phase(const char* name, const PhaseStats& s) {
  int64_t per_chunk = s.count > 0 ? s.total_ns / s.count : 0;
  int64_t rate = s.total_ns > 0 ? (s.count * 1000000000) / s.total_ns : 0;
  std::printf("%s: chunks=%lld total_ms=%s us/chunk=%s min_us=%s median_us=%s p95_us=%s "
              "max_us=%s chunks/s=%lld\n",
              name, (long long)s.count, us(s.total_ns / 1000).c_str(), us(per_chunk).c_str(),
              us(s.min_ns).c_str(), us(s.median_ns).c_str(), us(s.p95_ns).c_str(),
              us(s.max_ns).c_str(), (long long)rate);
}

bool parse_int(const std::string& text, int64_t& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  long long v = std::strtoll(text.c_str(), &end, 10);
  if (*end != '\0') return false;
  out = v;
  return true;
}

void gen_task(uint32_t seed, int32_t radius, int32_t offset, int32_t stride,
              std::vector<std::unique_ptr<Chunk>>& out) {
  int32_t side = 2 * radius + 1;
  for (int32_t i = offset; i < side * side; i += stride) {
    int32_t cx = i % side - radius;
    int32_t cz = i / side - radius;
    auto chunk = std::make_unique<Chunk>(cx, cz);
    generate_chunk(*chunk, seed);
    out.push_back(std::move(chunk));
  }
}

bool generate_parallel(World& world, uint32_t seed, int32_t r, int32_t t) {
  std::vector<std::vector<std::unique_ptr<Chunk>>> outputs(8);
  {
    std::vector<std::jthread> workers;
    for (int32_t k = 0; k < t; k++) {
      workers.emplace_back([&, k] { gen_task(seed, r, k, t, outputs[uint64_t(k)]); });
    }
  }
  for (auto& chunks : outputs) {
    for (auto& chunk : chunks) world.insert_chunk(std::move(chunk));
    chunks.clear();
  }
  return true;
}

int run_bench(int argc, char** argv) {
  int64_t seed = 1337, radius = 8, reps = 1, threads = 1;
  std::string json_path;
  for (int i = 1; i < argc; i += 2) {
    std::string flag = argv[i];
    if (i + 1 >= argc) {
      std::printf("missing value for %s\n", flag.c_str());
      return 2;
    }
    std::string value = argv[i + 1];
    bool ok = true;
    if (flag == "--seed") ok = parse_int(value, seed);
    else if (flag == "--radius") ok = parse_int(value, radius);
    else if (flag == "--reps") ok = parse_int(value, reps);
    else if (flag == "--threads") ok = parse_int(value, threads);
    else if (flag == "--json") json_path = value;
    else {
      std::printf("unknown flag %s\n", flag.c_str());
      return 2;
    }
    if (!ok || seed < 0 || seed > 4294967295LL || radius < 1 || radius > 64 || reps < 1 ||
        !(threads == 1 || threads == 2 || threads == 4 || threads == 8)) {
      std::printf("bad value for %s\n", flag.c_str());
      return 2;
    }
  }
  uint32_t world_seed = uint32_t(seed);
  int32_t r = int32_t(radius);
  int32_t side = 2 * r + 1;
  int64_t gen_chunks = int64_t(side) * side;
  int64_t mesh_chunks = int64_t(side - 2) * (side - 2);

  std::vector<int64_t> gen_samples, gen_totals, plain_samples, plain_totals, greedy_samples,
      greedy_totals;
  uint64_t checksum = 14695981039346656037ull;
  int64_t plain_opaque = 0, plain_translucent = 0, greedy_opaque = 0, greedy_translucent = 0;

  for (int64_t rep = 0; rep < reps; rep++) {
    World world(world_seed);
    int64_t gen_start = now_ns();
    if (threads == 1) {
      for (int32_t cz = -r; cz <= r; cz++) {
        for (int32_t cx = -r; cx <= r; cx++) {
          int64_t t0 = now_ns();
          world.generate_chunk(cx, cz);
          gen_samples.push_back(now_ns() - t0);
        }
      }
    } else {
      if (!generate_parallel(world, world_seed, r, int32_t(threads))) return 3;
    }
    int64_t gen_total = now_ns() - gen_start;
    gen_totals.push_back(gen_total);
    if (threads != 1) gen_samples.push_back(gen_total / gen_chunks);

    if (rep == 0) {
      for (int32_t cz = -r; cz <= r; cz++) {
        for (int32_t cx = -r; cx <= r; cx++) {
          const Chunk& chunk = world.chunk_at(cx, cz);
          for (uint64_t b = 0; b < chunk.blocks.size(); b++) {
            checksum = fnv_byte(checksum, uint64_t(chunk.blocks[b]));
          }
        }
      }
    }

    ChunkMesh mesh;
    for (int32_t mode = 0; mode < 2; mode++) {
      world.greedy = mode == 1;
      int64_t opaque = 0, translucent = 0;
      int64_t phase_start = now_ns();
      for (int32_t cz = -r + 1; cz <= r - 1; cz++) {
        for (int32_t cx = -r + 1; cx <= r - 1; cx++) {
          int64_t t0 = now_ns();
          world.build_mesh(cx, cz, mesh);
          int64_t dt = now_ns() - t0;
          if (mode == 0) plain_samples.push_back(dt); else greedy_samples.push_back(dt);
          opaque += mesh.opaque_quads();
          translucent += mesh.translucent_quads();
        }
      }
      int64_t phase_total = now_ns() - phase_start;
      if (mode == 0) {
        plain_totals.push_back(phase_total);
        plain_opaque = opaque;
        plain_translucent = translucent;
      } else {
        greedy_totals.push_back(phase_total);
        greedy_opaque = opaque;
        greedy_translucent = translucent;
      }
    }
  }
  checksum = fnv_u64(checksum, uint64_t(plain_opaque));
  checksum = fnv_u64(checksum, uint64_t(plain_translucent));
  checksum = fnv_u64(checksum, uint64_t(greedy_opaque));
  checksum = fnv_u64(checksum, uint64_t(greedy_translucent));

  PhaseStats gen, plain, greedy;
  summarize(gen_samples, gen_totals, gen_chunks, gen);
  summarize(plain_samples, plain_totals, mesh_chunks, plain);
  summarize(greedy_samples, greedy_totals, mesh_chunks, greedy);

  char checksum_text[32];
  std::snprintf(checksum_text, sizeof checksum_text, "%016llx", (unsigned long long)checksum);
  std::printf("voxel-bench lang=cpp seed=%lld radius=%lld reps=%lld threads=%lld\n",
              (long long)seed, (long long)radius, (long long)reps, (long long)threads);
  print_phase("generate", gen);
  print_phase("mesh_plain", plain);
  print_phase("mesh_greedy", greedy);
  std::printf("quads plain opaque=%lld translucent=%lld greedy opaque=%lld translucent=%lld\n",
              (long long)plain_opaque, (long long)plain_translucent, (long long)greedy_opaque,
              (long long)greedy_translucent);
  std::printf("checksum %s\n", checksum_text);

  if (!json_path.empty()) {
    std::string json = "{\n  \"lang\": \"cpp\",\n";
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "  \"seed\": %lld,\n  \"radius\": %lld,\n  \"reps\": %lld,\n  \"threads\": %lld,\n",
                  (long long)seed, (long long)radius, (long long)reps, (long long)threads);
    json += buf;
    json += "  \"generate\": " + phase_json(gen) + ",\n";
    json += "  \"mesh_plain\": " + phase_json(plain) + ",\n";
    json += "  \"mesh_greedy\": " + phase_json(greedy) + ",\n";
    std::snprintf(buf, sizeof buf,
                  "  \"quads\": {\"plain_opaque\": %lld, \"plain_translucent\": %lld, "
                  "\"greedy_opaque\": %lld, \"greedy_translucent\": %lld},\n",
                  (long long)plain_opaque, (long long)plain_translucent,
                  (long long)greedy_opaque, (long long)greedy_translucent);
    json += buf;
    json += std::string("  \"checksum\": \"") + checksum_text + "\"\n}\n";
    FILE* f = std::fopen(json_path.c_str(), "w");
    if (!f) {
      std::printf("cannot open %s\n", json_path.c_str());
      return 4;
    }
    bool ok = std::fwrite(json.data(), 1, json.size(), f) == json.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
      std::printf("cannot write %s\n", json_path.c_str());
      return 4;
    }
  }
  return 0;
}

}  // namespace voxel

int main(int argc, char** argv) { return voxel::run_bench(argc, argv); }
