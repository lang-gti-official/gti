#include "gti/driver/install.h"

#include "gti/driver/artifact.h"

#include <llvm/Support/ConvertUTF.h>

// Every driver translation unit must configure toml++ identically; see
// dependencies.cpp.
#define TOML_EXCEPTIONS 0
#include "toml.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace lang::driver {
namespace {

#if defined(_WIN32)
constexpr std::string_view unsupportedPlatform =
    "gti install and gti uninstall are not supported on Windows yet";
#endif

std::string environment(const char *name) {
  const char *value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string(value);
}

// Names come from validated target names, but the receipt is an ordinary
// user-writable file. A name is one path component so an edited receipt can
// never direct uninstall outside the bin directory.
bool validProgramName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find_first_of("/\\") == std::string_view::npos &&
         name.find('\0') == std::string_view::npos;
}

bool validUtf8(std::string_view text) {
  const auto *begin = reinterpret_cast<const llvm::UTF8 *>(text.data());
  return llvm::isLegalUTF8String(&begin, begin + text.size());
}

// TOML basic-string escaping, as in the lockfile emitter.
std::string tomlString(std::string_view value) {
  std::string result = "\"";
  for (const char character : value) {
    switch (character) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    default:
      if (static_cast<unsigned char>(character) < 0x20U ||
          character == '\x7f') {
        constexpr char hex[] = "0123456789ABCDEF";
        const auto byte = static_cast<unsigned char>(character);
        result += "\\u00";
        result += hex[byte >> 4U];
        result += hex[byte & 0x0FU];
      } else {
        result += character;
      }
      break;
    }
  }
  return result + "\"";
}

std::string renderReceipt(std::vector<InstalledProgram> programs) {
  std::sort(programs.begin(), programs.end(),
            [](const InstalledProgram &left, const InstalledProgram &right) {
              return left.name < right.name;
            });
  std::string text = "# Managed by `gti install`. Only programs recorded here "
                     "are replaced or\n# removed by gti install and gti "
                     "uninstall.\nreceipt-version = 1\n";
  for (const InstalledProgram &program : programs) {
    // The source path is informational. A path TOML cannot spell is dropped
    // rather than making the whole receipt unreadable.
    const std::string source = program.source.string();
    text += "\n[[program]]\nname = " + tomlString(program.name) +
            "\npackage = " + tomlString(program.package) +
            "\nversion = " + tomlString(program.version) +
            "\ntarget = " + tomlString(program.target) +
            "\nprofile = " + tomlString(program.profile) + "\nsource = " +
            tomlString(validUtf8(source) ? source : std::string{}) + "\n";
  }
  return text;
}

InstalledProgramsResult readReceipt(const std::filesystem::path &root) {
  const std::filesystem::path path = installReceiptPath(root);
  InstalledProgramsResult result;
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  if (status.type() == std::filesystem::file_type::not_found) {
    return result;
  }
  const auto fail = [&](std::string message) {
    result.programs.clear();
    result.error = "install record '" + path.string() + "' " + message;
    return result;
  };
  if (error) {
    return fail("cannot be inspected: " + error.message());
  }
  if (!std::filesystem::is_regular_file(status)) {
    return fail("is not a regular file");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return fail("cannot be read");
  }
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  toml::parse_result parsed = toml::parse(text, path.string());
  if (!parsed) {
    return fail("is not valid TOML: " +
                std::string(parsed.error().description()));
  }
  const toml::table &document = parsed.table();
  const std::optional<int64_t> version =
      document["receipt-version"].value<int64_t>();
  if (!version || *version != 1) {
    return fail("has an unsupported receipt-version");
  }
  const toml::node *programsNode = document.get("program");
  if (programsNode == nullptr) {
    return result;
  }
  const toml::array *programs = programsNode->as_array();
  if (programs == nullptr) {
    return fail("has a 'program' entry that is not an array of tables");
  }
  for (const toml::node &node : *programs) {
    const toml::table *entry = node.as_table();
    if (entry == nullptr) {
      return fail("has a 'program' entry that is not a table");
    }
    InstalledProgram program;
    const std::pair<const char *, std::string *> fields[] = {
        {"name", &program.name},
        {"package", &program.package},
        {"version", &program.version},
        {"target", &program.target},
        {"profile", &program.profile}};
    for (const auto &[key, field] : fields) {
      const std::optional<std::string> value =
          (*entry)[key].value<std::string>();
      if (!value) {
        return fail(std::string("has a program without a string '") + key +
                    "'");
      }
      *field = *value;
    }
    program.source = (*entry)["source"].value_or(std::string{});
    if (!validProgramName(program.name)) {
      return fail("records an invalid program name '" + program.name + "'");
    }
    if (std::any_of(result.programs.begin(), result.programs.end(),
                    [&](const InstalledProgram &existing) {
                      return existing.name == program.name;
                    })) {
      return fail("records '" + program.name + "' more than once");
    }
    result.programs.push_back(std::move(program));
  }
  std::sort(result.programs.begin(), result.programs.end(),
            [](const InstalledProgram &left, const InstalledProgram &right) {
              return left.name < right.name;
            });
  return result;
}

// Staged beside the receipt and renamed into place, so a reader never sees
// a partially written record.
std::string writeReceipt(const std::filesystem::path &root,
                         const std::vector<InstalledProgram> &programs) {
  const std::filesystem::path path = installReceiptPath(root);
  const std::filesystem::path staged = stagedArtifactPath(path);
  TemporaryArtifact cleanup(staged, true);
  if (writeArtifact(staged, renderReceipt(programs)) !=
      ArtifactWriteStatus::Success) {
    return "cannot write install record '" + staged.string() + "'";
  }
  if (const ArtifactPublishResult published = publishArtifact(staged, path);
      !published.succeeded()) {
    return "cannot update install record '" + path.string() +
           "': " + published.error.message();
  }
  cleanup.keep();
  return {};
}

#if !defined(_WIN32)
// Serializes receipt read-modify-write cycles between concurrent gti
// processes sharing one root. The lock file itself is never removed.
class ReceiptLock final {
public:
  explicit ReceiptLock(const std::filesystem::path &root) {
    const std::filesystem::path directory =
        installReceiptPath(root).parent_path();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
      error_ = "cannot create '" + directory.string() + "': " + error.message();
      return;
    }
    const std::filesystem::path path = directory / "installs.lock";
    descriptor_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (descriptor_ < 0 || ::flock(descriptor_, LOCK_EX) != 0) {
      error_ = "cannot lock '" + path.string() + "'";
    }
  }
  ReceiptLock(const ReceiptLock &) = delete;
  ReceiptLock &operator=(const ReceiptLock &) = delete;
  ~ReceiptLock() {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
  }
  [[nodiscard]] const std::string &error() const { return error_; }

private:
  int descriptor_ = -1;
  std::string error_;
};
#endif

std::optional<InstallResult>
destinationConflict(const InstallRequest &request,
                    const std::vector<InstalledProgram> &programs) {
  const std::filesystem::path destination =
      installBinDirectory(request.root) / request.program.name;
  const auto refuse = [&](InstallStatus status, std::string message) {
    return InstallResult{.status = status,
                         .destination = destination,
                         .message = std::move(message)};
  };
  if (!validProgramName(request.program.name)) {
    return refuse(InstallStatus::Failed,
                  "'" + request.program.name + "' is not a valid program name");
  }
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(destination, error);
  const bool exists = status.type() != std::filesystem::file_type::not_found;
  if (error && exists) {
    return refuse(InstallStatus::Failed, "cannot inspect '" +
                                             destination.string() +
                                             "': " + error.message());
  }
  if (exists && std::filesystem::is_directory(status)) {
    return refuse(InstallStatus::Failed,
                  "'" + destination.string() + "' is a directory");
  }
  if (request.force || !exists) {
    return std::nullopt;
  }
  const auto recorded = std::find_if(
      programs.begin(), programs.end(), [&](const InstalledProgram &program) {
        return program.name == request.program.name;
      });
  if (recorded == programs.end()) {
    return refuse(InstallStatus::Conflict,
                  "'" + destination.string() +
                      "' already exists and was not installed by gti install");
  }
  if (recorded->package != request.program.package) {
    return refuse(InstallStatus::Conflict,
                  "'" + request.program.name +
                      "' is already installed from package " +
                      recorded->package + "@" + recorded->version);
  }
  return std::nullopt;
}

} // namespace

InstallRootResult
resolveInstallRoot(const std::optional<std::filesystem::path> &requested) {
  std::filesystem::path root;
  std::string_view origin;
  if (requested) {
    if (requested->empty()) {
      return {.error = "--root requires a directory"};
    }
    std::error_code error;
    root = std::filesystem::absolute(*requested, error);
    if (error) {
      return {.error = "cannot resolve --root '" + requested->string() +
                       "': " + error.message()};
    }
    origin = "--root";
  } else if (const std::string configured = environment("GTI_INSTALL_ROOT");
             !configured.empty()) {
    root = configured;
    origin = "GTI_INSTALL_ROOT";
  } else if (const std::string home = environment("HOME"); !home.empty()) {
    root = std::filesystem::path(home) / ".local";
    origin = "HOME";
  } else {
    return {.error = "set HOME or GTI_INSTALL_ROOT, or pass --root, to choose "
                     "an install location"};
  }
  if (!root.is_absolute()) {
    return {.error = std::string(origin) + " must name an absolute directory"};
  }
  root = root.lexically_normal();
  if (root == root.root_path()) {
    return {.error = "refusing to install into the filesystem root"};
  }
  return {.root = root};
}

std::filesystem::path installBinDirectory(const std::filesystem::path &root) {
  return root / "bin";
}

std::filesystem::path installReceiptPath(const std::filesystem::path &root) {
  return root / "share" / "gti" / "installs.toml";
}

InstalledProgramsResult
listInstalledPrograms(const std::filesystem::path &root) {
#if defined(_WIN32)
  return {.error = std::string(unsupportedPlatform)};
#else
  return readReceipt(root);
#endif
}

std::optional<InstallResult>
checkInstallDestination(const InstallRequest &request) {
#if defined(_WIN32)
  return InstallResult{.message = std::string(unsupportedPlatform)};
#else
  const InstalledProgramsResult receipt = readReceipt(request.root);
  if (!receipt.succeeded()) {
    return InstallResult{.message = receipt.error};
  }
  return destinationConflict(request, receipt.programs);
#endif
}

InstallResult installProgram(const InstallRequest &request) {
#if defined(_WIN32)
  return {.message = std::string(unsupportedPlatform)};
#else
  const std::filesystem::path bin = installBinDirectory(request.root);
  const std::filesystem::path destination = bin / request.program.name;
  const auto failed = [&](std::string message) {
    return InstallResult{.status = InstallStatus::Failed,
                         .destination = destination,
                         .message = std::move(message)};
  };
  const ReceiptLock lock(request.root);
  if (!lock.error().empty()) {
    return failed(lock.error());
  }
  InstalledProgramsResult receipt = readReceipt(request.root);
  if (!receipt.succeeded()) {
    return failed(receipt.error);
  }
  if (std::optional<InstallResult> conflict =
          destinationConflict(request, receipt.programs)) {
    return *conflict;
  }

  std::error_code error;
  std::filesystem::create_directories(bin, error);
  if (error) {
    return failed("cannot create '" + bin.string() + "': " + error.message());
  }
  const bool replacing =
      std::filesystem::symlink_status(destination, error).type() !=
      std::filesystem::file_type::not_found;
  error.clear();

  // Copy to a fresh sibling and rename over the destination: a running copy
  // of the old program keeps its inode, and no reader sees a partial file.
  const std::filesystem::path staged = stagedArtifactPath(destination);
  TemporaryArtifact stagedCleanup(staged, true);
  std::filesystem::copy_file(request.executable, staged, error);
  if (error) {
    return failed("cannot copy '" + request.executable.string() + "' to '" +
                  bin.string() + "': " + error.message());
  }
  using std::filesystem::perms;
  std::filesystem::permissions(staged,
                               perms::owner_all | perms::group_read |
                                   perms::group_exec | perms::others_read |
                                   perms::others_exec,
                               std::filesystem::perm_options::replace, error);
  if (error) {
    return failed("cannot make '" + staged.string() +
                  "' executable: " + error.message());
  }

  // Record before publishing. A record without its file is harmless and
  // uninstall clears it; an unrecorded file would block the next install.
  const std::vector<InstalledProgram> previous = receipt.programs;
  std::erase_if(receipt.programs, [&](const InstalledProgram &program) {
    return program.name == request.program.name;
  });
  receipt.programs.push_back(request.program);
  if (std::string recordError = writeReceipt(request.root, receipt.programs);
      !recordError.empty()) {
    return failed(std::move(recordError));
  }
  if (const ArtifactPublishResult published =
          publishArtifact(staged, destination);
      !published.succeeded()) {
    // Best effort: the original failure is what the caller reports.
    (void)writeReceipt(request.root, previous);
    return failed("cannot install '" + destination.string() +
                  "': " + published.error.message());
  }
  stagedCleanup.keep();
  return {.status =
              replacing ? InstallStatus::Replaced : InstallStatus::Installed,
          .destination = destination};
#endif
}

UninstallResult uninstallProgram(const std::filesystem::path &root,
                                 std::string_view name) {
#if defined(_WIN32)
  return {.message = std::string(unsupportedPlatform)};
#else
  const std::filesystem::path destination =
      installBinDirectory(root) /
      (validProgramName(name) ? std::string(name) : std::string{});
  const auto failed = [&](UninstallStatus status, std::string message) {
    return UninstallResult{.status = status,
                           .destination = destination,
                           .message = std::move(message)};
  };
  if (!validProgramName(name)) {
    return failed(UninstallStatus::NotInstalled,
                  "'" + std::string(name) + "' is not a program name");
  }
  const ReceiptLock lock(root);
  if (!lock.error().empty()) {
    return failed(UninstallStatus::Failed, lock.error());
  }
  InstalledProgramsResult receipt = readReceipt(root);
  if (!receipt.succeeded()) {
    return failed(UninstallStatus::Failed, receipt.error);
  }
  if (std::erase_if(receipt.programs, [&](const InstalledProgram &program) {
        return program.name == name;
      }) == 0) {
    return failed(UninstallStatus::NotInstalled,
                  "'" + std::string(name) + "' was not installed by gti " +
                      "install in '" + root.string() + "'");
  }

  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(destination, error);
  const bool missing = status.type() == std::filesystem::file_type::not_found;
  if (!missing) {
    if (error) {
      return failed(UninstallStatus::Failed, "cannot inspect '" +
                                                 destination.string() +
                                                 "': " + error.message());
    }
    // Install only ever places a regular file. Anything else was put there
    // by someone else after installation and is left alone.
    if (!std::filesystem::is_regular_file(status)) {
      return failed(UninstallStatus::Failed,
                    "'" + destination.string() +
                        "' is no longer the file gti install placed there; "
                        "remove it yourself, then uninstall again");
    }
    if (!std::filesystem::remove(destination, error) || error) {
      return failed(UninstallStatus::Failed, "cannot remove '" +
                                                 destination.string() +
                                                 "': " + error.message());
    }
  }
  if (std::string recordError = writeReceipt(root, receipt.programs);
      !recordError.empty()) {
    return failed(UninstallStatus::Failed, std::move(recordError));
  }
  return {.status = UninstallStatus::Removed,
          .destination = destination,
          .fileMissing = missing};
#endif
}

} // namespace lang::driver
