#include "bindings.h"

#include <CLI/CLI.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

struct GtiCli11App;

struct GtiCli11Command {
  GtiCli11App *owner = nullptr;
  CLI::App *command = nullptr;
};

enum class GtiCli11OptionKind {
  Untyped,
  Signed,
  Unsigned,
  Floating,
};

struct GtiCli11Option {
  GtiCli11App *owner = nullptr;
  CLI::Option *option = nullptr;
  GtiCli11OptionKind kind = GtiCli11OptionKind::Untyped;
  int64_t signed_value = 0;
  uint64_t unsigned_value = 0;
  double floating_value = 0.0;
};

struct GtiCli11App {
  std::unique_ptr<CLI::App> app;
  std::vector<std::unique_ptr<GtiCli11Command>> commands;
  std::vector<std::unique_ptr<GtiCli11Option>> options;
  std::vector<std::string> arguments;
  std::vector<std::string> remaining;
  std::string output;
  std::string error;
  bool should_exit = false;
  int32_t exit_code = 0;
};

namespace {
thread_local std::string last_error;

bool copy_text(gti_c_string_view input, std::string &output,
               std::string &error) {
  if (input.length > std::numeric_limits<std::size_t>::max()) {
    error = "text length exceeds the native size range";
    return false;
  }
  if (input.data == nullptr) {
    if (input.length != 0) {
      error = "text has a null address with a nonzero length";
      return false;
    }
    output.clear();
    return true;
  }
  const std::size_t size = static_cast<std::size_t>(input.length);
  if (std::memchr(input.data, '\0', size) != nullptr) {
    error = "CLI11 text cannot contain an embedded NUL byte";
    return false;
  }
  output.assign(input.data, size);
  return true;
}

void clear_result(GtiCli11App &app) {
  app.output.clear();
  app.error.clear();
  app.remaining.clear();
  app.should_exit = false;
  app.exit_code = 0;
}

void set_error(GtiCli11App *app, const char *message) {
  if (app != nullptr) {
    app->error = message == nullptr ? "CLI11 operation failed" : message;
  } else {
    last_error = message == nullptr ? "CLI11 operation failed" : message;
  }
}

template <typename Operation>
int32_t mutate(GtiCli11App *app, Operation operation) {
  if (app == nullptr) {
    last_error = "null CLI11 app";
    return 0;
  }
  app->error.clear();
  try {
    operation();
    return 1;
  } catch (const std::exception &error) {
    app->error = error.what();
  } catch (...) {
    app->error = "unknown native exception";
  }
  return 0;
}

template <typename Add>
GtiCli11Option *add_option(GtiCli11Command *command, gti_c_string_view names,
                           gti_c_string_view description,
                           GtiCli11OptionKind kind, Add add) {
  if (command == nullptr || command->owner == nullptr ||
      command->command == nullptr) {
    last_error = "null CLI11 command";
    return nullptr;
  }
  GtiCli11App &owner = *command->owner;
  owner.error.clear();
  std::string native_names;
  std::string native_description;
  if (!copy_text(names, native_names, owner.error) ||
      !copy_text(description, native_description, owner.error)) {
    return nullptr;
  }
  try {
    auto handle = std::make_unique<GtiCli11Option>();
    handle->owner = &owner;
    handle->kind = kind;
    GtiCli11Option *result = handle.get();
    owner.options.push_back(std::move(handle));
    try {
      result->option = add(*result, native_names, native_description);
    } catch (...) {
      owner.options.pop_back();
      throw;
    }
    if (result->option == nullptr) {
      owner.options.pop_back();
      owner.error = "CLI11 did not create an option";
      return nullptr;
    }
    return result;
  } catch (const std::exception &error) {
    owner.error = error.what();
  } catch (...) {
    owner.error = "unknown native exception";
  }
  return nullptr;
}

const std::string *result_at(const GtiCli11Option *option, uint64_t index) {
  if (option == nullptr || option->option == nullptr ||
      index > std::numeric_limits<std::size_t>::max()) {
    return nullptr;
  }
  const auto &results = option->option->results();
  const std::size_t position = static_cast<std::size_t>(index);
  return position < results.size() ? &results[position] : nullptr;
}

uint8_t byte_at(const std::string *text, uint64_t offset) {
  if (text == nullptr || offset > std::numeric_limits<std::size_t>::max()) {
    return 0;
  }
  const std::size_t position = static_cast<std::size_t>(offset);
  return position < text->size()
             ? static_cast<uint8_t>(
                   static_cast<unsigned char>((*text)[position]))
             : 0;
}
} // namespace

extern "C" {
GtiCli11App *gti_cli11_app_create(gti_c_string_view description) {
  last_error.clear();
  std::string native_description;
  if (!copy_text(description, native_description, last_error)) {
    return nullptr;
  }
  try {
    auto result = std::make_unique<GtiCli11App>();
    result->app = std::make_unique<CLI::App>(native_description);
    auto root = std::make_unique<GtiCli11Command>();
    root->owner = result.get();
    root->command = result->app.get();
    result->commands.push_back(std::move(root));
    return result.release();
  } catch (const std::exception &error) {
    last_error = error.what();
  } catch (...) {
    last_error = "unknown native exception";
  }
  return nullptr;
}

void gti_cli11_app_destroy(GtiCli11App *app) { delete app; }

GtiCli11Command *gti_cli11_app_root(GtiCli11App *app) {
  return app == nullptr || app->commands.empty() ? nullptr
                                                 : app->commands.front().get();
}

GtiCli11Command *
gti_cli11_command_add_subcommand(GtiCli11Command *command,
                                 gti_c_string_view name,
                                 gti_c_string_view description) {
  if (command == nullptr || command->owner == nullptr ||
      command->command == nullptr) {
    last_error = "null CLI11 command";
    return nullptr;
  }
  GtiCli11App &owner = *command->owner;
  owner.error.clear();
  std::string native_name;
  std::string native_description;
  if (!copy_text(name, native_name, owner.error) ||
      !copy_text(description, native_description, owner.error)) {
    return nullptr;
  }
  try {
    auto handle = std::make_unique<GtiCli11Command>();
    handle->owner = &owner;
    GtiCli11Command *result = handle.get();
    owner.commands.push_back(std::move(handle));
    try {
      result->command =
          command->command->add_subcommand(native_name, native_description);
    } catch (...) {
      owner.commands.pop_back();
      throw;
    }
    return result;
  } catch (const std::exception &error) {
    owner.error = error.what();
  } catch (...) {
    owner.error = "unknown native exception";
  }
  return nullptr;
}

GtiCli11Option *gti_cli11_command_add_flag(GtiCli11Command *command,
                                           gti_c_string_view names,
                                           gti_c_string_view description) {
  return add_option(command, names, description, GtiCli11OptionKind::Untyped,
                    [command](GtiCli11Option &, const std::string &native_names,
                              const std::string &native_description) {
                      return command->command->add_flag(native_names,
                                                        native_description);
                    });
}

GtiCli11Option *gti_cli11_command_add_option(GtiCli11Command *command,
                                             gti_c_string_view names,
                                             gti_c_string_view description) {
  return add_option(command, names, description, GtiCli11OptionKind::Untyped,
                    [command](GtiCli11Option &, const std::string &native_names,
                              const std::string &native_description) {
                      return command->command->add_option(native_names,
                                                          native_description);
                    });
}

GtiCli11Option *gti_cli11_command_add_i64_option(GtiCli11Command *command,
                                                 gti_c_string_view names,
                                                 gti_c_string_view description,
                                                 int64_t default_value) {
  return add_option(command, names, description, GtiCli11OptionKind::Signed,
                    [command, default_value](
                        GtiCli11Option &handle, const std::string &native_names,
                        const std::string &native_description) {
                      handle.signed_value = default_value;
                      return command->command->add_option(native_names,
                                                          handle.signed_value,
                                                          native_description);
                    });
}

GtiCli11Option *gti_cli11_command_add_u64_option(GtiCli11Command *command,
                                                 gti_c_string_view names,
                                                 gti_c_string_view description,
                                                 uint64_t default_value) {
  return add_option(command, names, description, GtiCli11OptionKind::Unsigned,
                    [command, default_value](
                        GtiCli11Option &handle, const std::string &native_names,
                        const std::string &native_description) {
                      handle.unsigned_value = default_value;
                      return command->command->add_option(native_names,
                                                          handle.unsigned_value,
                                                          native_description);
                    });
}

GtiCli11Option *gti_cli11_command_add_double_option(
    GtiCli11Command *command, gti_c_string_view names,
    gti_c_string_view description, double default_value) {
  return add_option(command, names, description, GtiCli11OptionKind::Floating,
                    [command, default_value](
                        GtiCli11Option &handle, const std::string &native_names,
                        const std::string &native_description) {
                      handle.floating_value = default_value;
                      return command->command->add_option(native_names,
                                                          handle.floating_value,
                                                          native_description);
                    });
}

GtiCli11Option *gti_cli11_command_set_help_flag(GtiCli11Command *command,
                                                gti_c_string_view names,
                                                gti_c_string_view description) {
  return add_option(command, names, description, GtiCli11OptionKind::Untyped,
                    [command](GtiCli11Option &, const std::string &native_names,
                              const std::string &native_description) {
                      return command->command->set_help_flag(
                          native_names, native_description);
                    });
}

GtiCli11Option *gti_cli11_command_set_version_flag(
    GtiCli11Command *command, gti_c_string_view names,
    gti_c_string_view version, gti_c_string_view description) {
  if (command == nullptr || command->owner == nullptr) {
    last_error = "null CLI11 command";
    return nullptr;
  }
  std::string native_version;
  if (!copy_text(version, native_version, command->owner->error)) {
    return nullptr;
  }
  return add_option(command, names, description, GtiCli11OptionKind::Untyped,
                    [command, native_version](
                        GtiCli11Option &, const std::string &native_names,
                        const std::string &native_description) {
                      return command->command->set_version_flag(
                          native_names, native_version, native_description);
                    });
}

int32_t gti_cli11_command_allow_extras(GtiCli11Command *command,
                                       int32_t allow) {
  if (command == nullptr || command->owner == nullptr ||
      command->command == nullptr) {
    last_error = "null CLI11 command";
    return 0;
  }
  return mutate(command->owner, [command, allow] {
    command->command->allow_extras(allow != 0);
  });
}

int32_t gti_cli11_command_require_subcommand(GtiCli11Command *command,
                                             int32_t minimum, int32_t maximum) {
  if (command == nullptr || command->owner == nullptr ||
      command->command == nullptr) {
    last_error = "null CLI11 command";
    return 0;
  }
  if (minimum < 0 || maximum < 0 || (maximum != 0 && maximum < minimum)) {
    command->owner->error = "invalid required subcommand range";
    return 0;
  }
  return mutate(command->owner, [command, minimum, maximum] {
    command->command->require_subcommand(minimum, maximum);
  });
}

int32_t gti_cli11_command_parsed(const GtiCli11Command *command) {
  return command != nullptr && command->command != nullptr &&
                 command->command->parsed()
             ? 1
             : 0;
}

int32_t gti_cli11_option_required(GtiCli11Option *option, int32_t required) {
  if (option == nullptr || option->owner == nullptr ||
      option->option == nullptr) {
    last_error = "null CLI11 option";
    return 0;
  }
  return mutate(option->owner, [option, required] {
    option->option->required(required != 0);
  });
}

int32_t gti_cli11_option_expected(GtiCli11Option *option, int32_t minimum,
                                  int32_t maximum) {
  if (option == nullptr || option->owner == nullptr ||
      option->option == nullptr) {
    last_error = "null CLI11 option";
    return 0;
  }
  if (minimum < 0 || maximum < minimum) {
    option->owner->error = "invalid expected-value range";
    return 0;
  }
  return mutate(option->owner, [option, minimum, maximum] {
    option->option->expected(minimum, maximum);
  });
}

int32_t gti_cli11_option_allow_extra_args(GtiCli11Option *option,
                                          int32_t allow) {
  if (option == nullptr || option->owner == nullptr ||
      option->option == nullptr) {
    last_error = "null CLI11 option";
    return 0;
  }
  return mutate(option->owner, [option, allow] {
    option->option->allow_extra_args(allow != 0);
  });
}

uint64_t gti_cli11_option_count(const GtiCli11Option *option) {
  return option == nullptr || option->option == nullptr
             ? 0
             : static_cast<uint64_t>(option->option->count());
}

uint64_t gti_cli11_option_result_count(const GtiCli11Option *option) {
  return option == nullptr || option->option == nullptr
             ? 0
             : static_cast<uint64_t>(option->option->results().size());
}

uint64_t gti_cli11_option_result_size(const GtiCli11Option *option,
                                      uint64_t index) {
  const std::string *result = result_at(option, index);
  return result == nullptr ? 0 : static_cast<uint64_t>(result->size());
}

uint8_t gti_cli11_option_result_byte(const GtiCli11Option *option,
                                     uint64_t index, uint64_t offset) {
  return byte_at(result_at(option, index), offset);
}

int32_t gti_cli11_option_i64_value(const GtiCli11Option *option,
                                   int64_t *output) {
  if (option == nullptr || output == nullptr ||
      option->kind != GtiCli11OptionKind::Signed) {
    set_error(option == nullptr ? nullptr : option->owner,
              "option is not a signed integer option");
    return 0;
  }
  *output = option->signed_value;
  return 1;
}

int32_t gti_cli11_option_u64_value(const GtiCli11Option *option,
                                   uint64_t *output) {
  if (option == nullptr || output == nullptr ||
      option->kind != GtiCli11OptionKind::Unsigned) {
    set_error(option == nullptr ? nullptr : option->owner,
              "option is not an unsigned integer option");
    return 0;
  }
  *output = option->unsigned_value;
  return 1;
}

int32_t gti_cli11_option_double_value(const GtiCli11Option *option,
                                      double *output) {
  if (option == nullptr || output == nullptr ||
      option->kind != GtiCli11OptionKind::Floating) {
    set_error(option == nullptr ? nullptr : option->owner,
              "option is not a floating-point option");
    return 0;
  }
  *output = option->floating_value;
  return 1;
}

int32_t gti_cli11_app_add_argument(GtiCli11App *app,
                                   gti_c_string_view argument) {
  if (app == nullptr) {
    last_error = "null CLI11 app";
    return 0;
  }
  std::string native_argument;
  if (!copy_text(argument, native_argument, app->error)) {
    return 0;
  }
  try {
    app->arguments.push_back(std::move(native_argument));
    return 1;
  } catch (const std::exception &error) {
    app->error = error.what();
  } catch (...) {
    app->error = "unknown native exception";
  }
  return 0;
}

void gti_cli11_app_clear_arguments(GtiCli11App *app) {
  if (app != nullptr) {
    app->arguments.clear();
    clear_result(*app);
  }
}

int32_t gti_cli11_app_parse(GtiCli11App *app) {
  if (app == nullptr) {
    last_error = "null CLI11 app";
    return 1;
  }
  clear_result(*app);
  if (app->arguments.empty()) {
    app->error = "CLI11 parse requires argv[0]";
    app->should_exit = true;
    app->exit_code = 1;
    return app->exit_code;
  }
  if (app->arguments.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    app->error = "CLI11 argument count exceeds INT_MAX";
    app->should_exit = true;
    app->exit_code = 1;
    return app->exit_code;
  }
  try {
    app->app->clear();
    std::vector<const char *> argv;
    argv.reserve(app->arguments.size());
    for (const std::string &argument : app->arguments) {
      argv.push_back(argument.c_str());
    }
    app->app->parse(static_cast<int>(argv.size()), argv.data());
    app->remaining = app->app->remaining(true);
    return 0;
  } catch (const CLI::ParseError &error) {
    app->should_exit = true;
    std::ostringstream output;
    std::ostringstream errors;
    app->exit_code = app->app->exit(error, output, errors);
    app->output = output.str();
    app->error = errors.str();
    return app->exit_code;
  } catch (const std::exception &error) {
    app->should_exit = true;
    app->exit_code = 1;
    app->error = error.what();
  } catch (...) {
    app->should_exit = true;
    app->exit_code = 1;
    app->error = "unknown native exception";
  }
  return app->exit_code;
}

int32_t gti_cli11_app_should_exit(const GtiCli11App *app) {
  return app != nullptr && app->should_exit ? 1 : 0;
}

int32_t gti_cli11_app_exit_code(const GtiCli11App *app) {
  return app == nullptr ? 1 : app->exit_code;
}

uint64_t gti_cli11_app_output_size(const GtiCli11App *app) {
  return app == nullptr ? 0 : static_cast<uint64_t>(app->output.size());
}

uint8_t gti_cli11_app_output_byte(const GtiCli11App *app, uint64_t offset) {
  return byte_at(app == nullptr ? nullptr : &app->output, offset);
}

uint64_t gti_cli11_app_error_size(const GtiCli11App *app) {
  return app == nullptr ? 0 : static_cast<uint64_t>(app->error.size());
}

uint8_t gti_cli11_app_error_byte(const GtiCli11App *app, uint64_t offset) {
  return byte_at(app == nullptr ? nullptr : &app->error, offset);
}

uint64_t gti_cli11_app_remaining_count(const GtiCli11App *app) {
  return app == nullptr ? 0 : static_cast<uint64_t>(app->remaining.size());
}

uint64_t gti_cli11_app_remaining_size(const GtiCli11App *app, uint64_t index) {
  if (app == nullptr || index > std::numeric_limits<std::size_t>::max()) {
    return 0;
  }
  const std::size_t position = static_cast<std::size_t>(index);
  return position < app->remaining.size()
             ? static_cast<uint64_t>(app->remaining[position].size())
             : 0;
}

uint8_t gti_cli11_app_remaining_byte(const GtiCli11App *app, uint64_t index,
                                     uint64_t offset) {
  if (app == nullptr || index > std::numeric_limits<std::size_t>::max()) {
    return 0;
  }
  const std::size_t position = static_cast<std::size_t>(index);
  return byte_at(position < app->remaining.size() ? &app->remaining[position]
                                                  : nullptr,
                 offset);
}

const char *gti_cli11_last_error(void) { return last_error.c_str(); }

uint64_t gti_cli11_last_error_size(void) {
  return static_cast<uint64_t>(last_error.size());
}

uint8_t gti_cli11_last_error_byte(uint64_t offset) {
  return byte_at(&last_error, offset);
}

int32_t gti_cli11_version_major(void) { return CLI11_VERSION_MAJOR; }
int32_t gti_cli11_version_minor(void) { return CLI11_VERSION_MINOR; }
int32_t gti_cli11_version_patch(void) { return CLI11_VERSION_PATCH; }
}
