#pragma once
#include <stdint.h>

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_INT 0x1405
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_DEPTH_TEST 0x0B71

// Native loader: call only after making an OpenGL context current.
int32_t gti_glad_load(void);
int32_t gti_glad_is_loaded(void);
void gti_glad_unload(void);
void gti_glad_clear_color(float red, float green, float blue, float alpha);
void gti_glad_clear(uint32_t mask);
void gti_glad_viewport(int32_t x, int32_t y, int32_t width, int32_t height);
void gti_glad_enable(uint32_t capability);
void gti_glad_disable(uint32_t capability);
uint32_t gti_glad_get_error(void);
uint32_t gti_glad_create_shader(uint32_t type);
void gti_glad_shader_source(uint32_t shader, const char *source);
void gti_glad_compile_shader(uint32_t shader);
void gti_glad_get_shader_iv(uint32_t shader, uint32_t parameter,
                            int32_t *value);
void gti_glad_delete_shader(uint32_t shader);
uint32_t gti_glad_create_program(void);
void gti_glad_attach_shader(uint32_t program, uint32_t shader);
void gti_glad_link_program(uint32_t program);
void gti_glad_get_program_iv(uint32_t program, uint32_t parameter,
                             int32_t *value);
void gti_glad_use_program(uint32_t program);
void gti_glad_delete_program(uint32_t program);
void gti_glad_gen_vertex_arrays(int32_t count, uint32_t *arrays);
void gti_glad_bind_vertex_array(uint32_t array);
void gti_glad_delete_vertex_arrays(int32_t count, const uint32_t *arrays);
void gti_glad_gen_buffers(int32_t count, uint32_t *buffers);
void gti_glad_bind_buffer(uint32_t target, uint32_t buffer);
void gti_glad_buffer_data(uint32_t target, int64_t bytes, const void *data,
                          uint32_t usage);
void gti_glad_delete_buffers(int32_t count, const uint32_t *buffers);
void gti_glad_vertex_attrib_pointer(uint32_t index, int32_t size, uint32_t type,
                                    int32_t normalized, int32_t stride,
                                    uint64_t offset);
void gti_glad_enable_vertex_attrib_array(uint32_t index);
void gti_glad_draw_arrays(uint32_t mode, int32_t first, int32_t count);
void gti_glad_draw_elements(uint32_t mode, int32_t count, uint32_t type,
                            uint64_t offset);
