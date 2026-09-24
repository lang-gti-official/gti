#include "bindings.h"
#include <glad/gl.h>
#include <stddef.h>

static int32_t gti_glad_loaded = 0;
int32_t gti_glad_load(void) {
  const int version = gladLoaderLoadGL();
  gti_glad_loaded = version != 0 && GLAD_GL_VERSION_3_3;
  return gti_glad_loaded ? version : 0;
}
int32_t gti_glad_is_loaded(void) { return gti_glad_loaded; }
void gti_glad_unload(void) {
  gladLoaderUnloadGL();
  gti_glad_loaded = 0;
}
void gti_glad_clear_color(float r, float g, float b, float a) {
  glClearColor(r, g, b, a);
}
void gti_glad_clear(uint32_t mask) { glClear(mask); }
void gti_glad_viewport(int32_t x, int32_t y, int32_t w, int32_t h) {
  glViewport(x, y, w, h);
}
void gti_glad_enable(uint32_t capability) { glEnable(capability); }
void gti_glad_disable(uint32_t capability) { glDisable(capability); }
uint32_t gti_glad_get_error(void) { return glGetError(); }
uint32_t gti_glad_create_shader(uint32_t type) { return glCreateShader(type); }
void gti_glad_shader_source(uint32_t shader, const char *source) {
  glShaderSource(shader, 1, &source, NULL);
}
void gti_glad_compile_shader(uint32_t shader) { glCompileShader(shader); }
void gti_glad_get_shader_iv(uint32_t shader, uint32_t parameter,
                            int32_t *value) {
  glGetShaderiv(shader, parameter, value);
}
void gti_glad_delete_shader(uint32_t shader) { glDeleteShader(shader); }
uint32_t gti_glad_create_program(void) { return glCreateProgram(); }
void gti_glad_attach_shader(uint32_t program, uint32_t shader) {
  glAttachShader(program, shader);
}
void gti_glad_link_program(uint32_t program) { glLinkProgram(program); }
void gti_glad_get_program_iv(uint32_t program, uint32_t parameter,
                             int32_t *value) {
  glGetProgramiv(program, parameter, value);
}
void gti_glad_use_program(uint32_t program) { glUseProgram(program); }
void gti_glad_delete_program(uint32_t program) { glDeleteProgram(program); }
void gti_glad_gen_vertex_arrays(int32_t count, uint32_t *arrays) {
  glGenVertexArrays(count, arrays);
}
void gti_glad_bind_vertex_array(uint32_t array) { glBindVertexArray(array); }
void gti_glad_delete_vertex_arrays(int32_t count, const uint32_t *arrays) {
  glDeleteVertexArrays(count, arrays);
}
void gti_glad_gen_buffers(int32_t count, uint32_t *buffers) {
  glGenBuffers(count, buffers);
}
void gti_glad_bind_buffer(uint32_t target, uint32_t buffer) {
  glBindBuffer(target, buffer);
}
void gti_glad_buffer_data(uint32_t target, int64_t bytes, const void *data,
                          uint32_t usage) {
  glBufferData(target, (GLsizeiptr)bytes, data, usage);
}
void gti_glad_delete_buffers(int32_t count, const uint32_t *buffers) {
  glDeleteBuffers(count, buffers);
}
void gti_glad_vertex_attrib_pointer(uint32_t index, int32_t size, uint32_t type,
                                    int32_t normalized, int32_t stride,
                                    uint64_t offset) {
  glVertexAttribPointer(index, size, type, normalized != 0, stride,
                        (const void *)(uintptr_t)offset);
}
void gti_glad_enable_vertex_attrib_array(uint32_t index) {
  glEnableVertexAttribArray(index);
}
void gti_glad_draw_arrays(uint32_t mode, int32_t first, int32_t count) {
  glDrawArrays(mode, first, count);
}
void gti_glad_draw_elements(uint32_t mode, int32_t count, uint32_t type,
                            uint64_t offset) {
  glDrawElements(mode, count, type, (const void *)(uintptr_t)offset);
}
