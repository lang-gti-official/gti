#ifndef GTI_PACKAGE_CLI11_BINDINGS_H
#define GTI_PACKAGE_CLI11_BINDINGS_H

#include <gti/c_abi.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GtiCli11App GtiCli11App;
typedef struct GtiCli11Command GtiCli11Command;
typedef struct GtiCli11Option GtiCli11Option;

// App owns every command and option handle returned from it. Those borrowed
// handles become invalid when the app is destroyed.
GtiCli11App *gti_cli11_app_create(gti_c_string_view description);
void gti_cli11_app_destroy(GtiCli11App *app);
GtiCli11Command *gti_cli11_app_root(GtiCli11App *app);

GtiCli11Command *
gti_cli11_command_add_subcommand(GtiCli11Command *command,
                                 gti_c_string_view name,
                                 gti_c_string_view description);
GtiCli11Option *gti_cli11_command_add_flag(GtiCli11Command *command,
                                           gti_c_string_view names,
                                           gti_c_string_view description);
GtiCli11Option *gti_cli11_command_add_option(GtiCli11Command *command,
                                             gti_c_string_view names,
                                             gti_c_string_view description);
GtiCli11Option *gti_cli11_command_add_i64_option(GtiCli11Command *command,
                                                 gti_c_string_view names,
                                                 gti_c_string_view description,
                                                 int64_t default_value);
GtiCli11Option *gti_cli11_command_add_u64_option(GtiCli11Command *command,
                                                 gti_c_string_view names,
                                                 gti_c_string_view description,
                                                 uint64_t default_value);
GtiCli11Option *gti_cli11_command_add_double_option(
    GtiCli11Command *command, gti_c_string_view names,
    gti_c_string_view description, double default_value);
GtiCli11Option *gti_cli11_command_set_help_flag(GtiCli11Command *command,
                                                gti_c_string_view names,
                                                gti_c_string_view description);
GtiCli11Option *gti_cli11_command_set_version_flag(
    GtiCli11Command *command, gti_c_string_view names,
    gti_c_string_view version, gti_c_string_view description);

int32_t gti_cli11_command_allow_extras(GtiCli11Command *command, int32_t allow);
int32_t gti_cli11_command_require_subcommand(GtiCli11Command *command,
                                             int32_t minimum, int32_t maximum);
int32_t gti_cli11_command_parsed(const GtiCli11Command *command);

int32_t gti_cli11_option_required(GtiCli11Option *option, int32_t required);
int32_t gti_cli11_option_expected(GtiCli11Option *option, int32_t minimum,
                                  int32_t maximum);
int32_t gti_cli11_option_allow_extra_args(GtiCli11Option *option,
                                          int32_t allow);
uint64_t gti_cli11_option_count(const GtiCli11Option *option);
uint64_t gti_cli11_option_result_count(const GtiCli11Option *option);
uint64_t gti_cli11_option_result_size(const GtiCli11Option *option,
                                      uint64_t index);
uint8_t gti_cli11_option_result_byte(const GtiCli11Option *option,
                                     uint64_t index, uint64_t offset);
int32_t gti_cli11_option_i64_value(const GtiCli11Option *option,
                                   int64_t *output);
int32_t gti_cli11_option_u64_value(const GtiCli11Option *option,
                                   uint64_t *output);
int32_t gti_cli11_option_double_value(const GtiCli11Option *option,
                                      double *output);

// Arguments are copied immediately, so dynamic GTI string views are safe.
int32_t gti_cli11_app_add_argument(GtiCli11App *app,
                                   gti_c_string_view argument);
void gti_cli11_app_clear_arguments(GtiCli11App *app);
int32_t gti_cli11_app_parse(GtiCli11App *app);
int32_t gti_cli11_app_should_exit(const GtiCli11App *app);
int32_t gti_cli11_app_exit_code(const GtiCli11App *app);

uint64_t gti_cli11_app_output_size(const GtiCli11App *app);
uint8_t gti_cli11_app_output_byte(const GtiCli11App *app, uint64_t offset);
uint64_t gti_cli11_app_error_size(const GtiCli11App *app);
uint8_t gti_cli11_app_error_byte(const GtiCli11App *app, uint64_t offset);
uint64_t gti_cli11_app_remaining_count(const GtiCli11App *app);
uint64_t gti_cli11_app_remaining_size(const GtiCli11App *app, uint64_t index);
uint8_t gti_cli11_app_remaining_byte(const GtiCli11App *app, uint64_t index,
                                     uint64_t offset);

const char *gti_cli11_last_error(void);
uint64_t gti_cli11_last_error_size(void);
uint8_t gti_cli11_last_error_byte(uint64_t offset);
int32_t gti_cli11_version_major(void);
int32_t gti_cli11_version_minor(void);
int32_t gti_cli11_version_patch(void);

#ifdef __cplusplus
}
#endif

#endif
