// cimgui's C++ declarations require Dear ImGui's public and internal types.
// clang-format off
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_opengl3.h>
#include <cimgui.h>
// clang-format on
extern "C" {
#include "bindings.h"
}

extern "C" {
const char *gti_imgui_version(void) { return igGetVersion(); }
ImGuiContext *gti_imgui_create_context(void) {
  return igCreateContext(nullptr);
}
void gti_imgui_destroy_context(ImGuiContext *context) {
  igDestroyContext(context);
}
void gti_imgui_set_current_context(ImGuiContext *context) {
  igSetCurrentContext(context);
}
void gti_imgui_configure_frame(float width, float height, float delta_seconds) {
  ImGuiIO *io = igGetIO_Nil();
  io->DisplaySize = ImVec2(width, height);
  io->DeltaTime = delta_seconds;
  io->IniFilename = nullptr; // No implicit imgui.ini writes from the binding.
  unsigned char *pixels = nullptr;
  int atlas_width = 0;
  int atlas_height = 0;
  io->Fonts->GetTexDataAsRGBA32(&pixels, &atlas_width, &atlas_height);
}
void gti_imgui_new_frame(void) { igNewFrame(); }
void gti_imgui_render(void) { igRender(); }
ImDrawData *gti_imgui_draw_data(void) { return igGetDrawData(); }
int32_t gti_imgui_draw_data_valid(void) {
  const ImDrawData *data = igGetDrawData();
  return data != nullptr && data->Valid;
}
int32_t gti_imgui_opengl3_init(const char *glsl_version) {
  return ImGui_ImplOpenGL3_Init(glsl_version) ? 1 : 0;
}
void gti_imgui_opengl3_new_frame(void) { ImGui_ImplOpenGL3_NewFrame(); }
void gti_imgui_opengl3_render_draw_data(void) {
  ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
}
void gti_imgui_opengl3_shutdown(void) { ImGui_ImplOpenGL3_Shutdown(); }
int32_t gti_imgui_begin(const char *title, int32_t *open, int32_t flags) {
  bool native_open = open == nullptr || *open != 0;
  const bool visible =
      igBegin(title, open == nullptr ? nullptr : &native_open, flags);
  if (open != nullptr) {
    *open = native_open ? 1 : 0;
  }
  return visible ? 1 : 0;
}
void gti_imgui_end(void) { igEnd(); }
void gti_imgui_text(const char *text) { igTextUnformatted(text, nullptr); }
int32_t gti_imgui_button(const char *label, float width, float height) {
  return igButton(label, ImVec2_c{width, height}) ? 1 : 0;
}
int32_t gti_imgui_checkbox(const char *label, int32_t *checked) {
  bool value = *checked != 0;
  const bool changed = igCheckbox(label, &value);
  *checked = value ? 1 : 0;
  return changed ? 1 : 0;
}
int32_t gti_imgui_slider_float(const char *label, float *value, float minimum,
                               float maximum) {
  return igSliderFloat(label, value, minimum, maximum, "%.3f", 0) ? 1 : 0;
}
void gti_imgui_same_line(float offset, float spacing) {
  igSameLine(offset, spacing);
}
void gti_imgui_separator(void) { igSeparator(); }
void gti_imgui_mouse_position(float x, float y) {
  ImGuiIO_AddMousePosEvent(igGetIO_Nil(), x, y);
}
void gti_imgui_mouse_button(int32_t button, int32_t down) {
  ImGuiIO_AddMouseButtonEvent(igGetIO_Nil(), button, down != 0);
}
void gti_imgui_mouse_wheel(float horizontal, float vertical) {
  ImGuiIO_AddMouseWheelEvent(igGetIO_Nil(), horizontal, vertical);
}
void gti_imgui_input_text(const char *text) {
  ImGuiIO_AddInputCharactersUTF8(igGetIO_Nil(), text);
}
}
