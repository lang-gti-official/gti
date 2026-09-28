#pragma once
#include <stdint.h>

typedef struct ImGuiContext ImGuiContext;
typedef struct ImDrawData ImDrawData;

const char *gti_imgui_version(void);
ImGuiContext *gti_imgui_create_context(void);
void gti_imgui_destroy_context(ImGuiContext *context);
void gti_imgui_set_current_context(ImGuiContext *context);
// Uses the default font. Does not install an input or rendering backend.
void gti_imgui_configure_frame(float width, float height, float delta_seconds);
void gti_imgui_new_frame(void);
void gti_imgui_render(void);
ImDrawData *gti_imgui_draw_data(void);
int32_t gti_imgui_draw_data_valid(void);
// Renderer backend for an already-current desktop OpenGL context.
int32_t gti_imgui_opengl3_init(const char *glsl_version);
void gti_imgui_opengl3_new_frame(void);
void gti_imgui_opengl3_render_draw_data(void);
void gti_imgui_opengl3_shutdown(void);
int32_t gti_imgui_begin(const char *title, int32_t *open, int32_t flags);
void gti_imgui_end(void);
void gti_imgui_text(const char *text);
int32_t gti_imgui_button(const char *label, float width, float height);
int32_t gti_imgui_checkbox(const char *label, int32_t *checked);
int32_t gti_imgui_slider_float(const char *label, float *value, float minimum,
                               float maximum);
void gti_imgui_same_line(float offset, float spacing);
void gti_imgui_separator(void);
void gti_imgui_mouse_position(float x, float y);
void gti_imgui_mouse_button(int32_t button, int32_t down);
void gti_imgui_mouse_wheel(float horizontal, float vertical);
void gti_imgui_input_text(const char *text);
