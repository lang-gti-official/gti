#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang::driver {

// `gti install` copies a built project executable to <root>/bin and records
// it in <root>/share/gti/installs.toml. The record is the ownership
// boundary: install replaces, and uninstall removes, only programs it
// recorded, so the root may be shared with other tools. POSIX hosts only;
// every operation reports a failure on Windows.
struct InstalledProgram {
  std::string name;
  std::string package;
  std::string version;
  std::string target;
  std::string profile;
  // The package root the program was built from, for display.
  std::filesystem::path source;
};

struct InstallRootResult {
  std::optional<std::filesystem::path> root;
  std::string error;
};

// An explicit --root, then GTI_INSTALL_ROOT, then $HOME/.local. The selected
// root must be absolute once a relative --root is resolved against the
// current directory.
[[nodiscard]] InstallRootResult
resolveInstallRoot(const std::optional<std::filesystem::path> &requested);

[[nodiscard]] std::filesystem::path
installBinDirectory(const std::filesystem::path &root);
[[nodiscard]] std::filesystem::path
installReceiptPath(const std::filesystem::path &root);

struct InstalledProgramsResult {
  // Ordered by name.
  std::vector<InstalledProgram> programs;
  std::string error;

  [[nodiscard]] bool succeeded() const { return error.empty(); }
};

// A missing receipt is an empty installation. A malformed one is an error:
// callers must not treat it as empty and lose track of installed programs.
[[nodiscard]] InstalledProgramsResult
listInstalledPrograms(const std::filesystem::path &root);

enum class InstallStatus {
  Installed,
  Replaced,
  // The destination belongs to another package or to nothing GTI recorded.
  Conflict,
  Failed,
};

struct InstallRequest {
  std::filesystem::path root;
  std::filesystem::path executable;
  InstalledProgram program;
  // Replace a destination that install would otherwise refuse.
  bool force = false;
};

struct InstallResult {
  InstallStatus status = InstallStatus::Failed;
  std::filesystem::path destination;
  std::string message;
};

// Reports a Conflict or Failed result that installProgram would produce,
// without changing anything. Lets a caller refuse before a long build;
// installProgram repeats the check under the receipt lock.
[[nodiscard]] std::optional<InstallResult>
checkInstallDestination(const InstallRequest &request);

[[nodiscard]] InstallResult installProgram(const InstallRequest &request);

enum class UninstallStatus { Removed, NotInstalled, Failed };

struct UninstallResult {
  UninstallStatus status = UninstallStatus::Failed;
  std::filesystem::path destination;
  // The program was recorded but its file was already gone; only the record
  // was removed.
  bool fileMissing = false;
  std::string message;
};

[[nodiscard]] UninstallResult
uninstallProgram(const std::filesystem::path &root, std::string_view name);

} // namespace lang::driver
