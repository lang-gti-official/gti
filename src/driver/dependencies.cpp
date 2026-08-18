#include "gti/driver/dependencies.h"

#include "gti/driver/artifact.h"
#include "gti/driver/process.h"
#include "sha256.h"
// Every driver translation unit must configure toml++ identically:
// manifest.cpp established the no-exceptions mode, and mixing modes across
// translation units corrupts parse results at link time.
#define TOML_EXCEPTIONS 0
#include "toml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace lang::driver {
namespace {

constexpr std::string_view lockFilename = "gti.lock";

Diagnostic lockDiagnostic(std::string code, const std::filesystem::path &path,
                          std::string message, int line = 1) {
  return makeDiagnostic(std::move(code), DiagnosticPhase::Driver,
                        SourceSpan{path.string(), 0, 1, line},
                        std::move(message));
}

[[nodiscard]] bool isHex(std::string_view value) {
  return std::all_of(value.begin(), value.end(), [](const char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

[[nodiscard]] bool validChecksum(std::string_view checksum) {
  constexpr std::string_view prefix = "sha256:";
  return checksum.size() == prefix.size() + 64 &&
         checksum.substr(0, prefix.size()) == prefix &&
         isHex(checksum.substr(prefix.size()));
}

[[nodiscard]] std::string digestHex(std::string_view text) {
  Sha256 hash;
  hash.update(text);
  return hash.finishHex();
}

// TOML basic-string escaping for the deterministic lock emitter.
[[nodiscard]] std::string tomlString(std::string_view value) {
  std::string result = "\"";
  for (const char character : value) {
    switch (character) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(character) < 0x20U) {
        constexpr char hex[] = "0123456789ABCDEF";
        const auto value8 = static_cast<unsigned char>(character);
        result += "\\u00";
        result += hex[value8 >> 4U];
        result += hex[value8 & 0x0FU];
      } else {
        result += character;
      }
      break;
    }
  }
  result += '"';
  return result;
}

struct ExtractedTreeEntry {
  std::string path;
  bool executable = false;
  std::string objectId;
};

// One tree entry from `git ls-tree -r -z`: "<mode> <type> <oid>\t<path>\0".
[[nodiscard]] std::optional<ExtractedTreeEntry>
parseTreeRecord(std::string_view record, std::string &rejection) {
  const std::size_t tab = record.find('\t');
  if (tab == std::string_view::npos) {
    rejection = "malformed tree record";
    return std::nullopt;
  }
  const std::string_view header = record.substr(0, tab);
  const std::string_view path = record.substr(tab + 1);
  const std::size_t firstSpace = header.find(' ');
  const std::size_t secondSpace = firstSpace == std::string_view::npos
                                      ? std::string_view::npos
                                      : header.find(' ', firstSpace + 1);
  if (firstSpace == std::string_view::npos ||
      secondSpace == std::string_view::npos) {
    rejection = "malformed tree record";
    return std::nullopt;
  }
  const std::string_view mode = header.substr(0, firstSpace);
  const std::string_view type =
      header.substr(firstSpace + 1, secondSpace - firstSpace - 1);
  const std::string_view objectId = header.substr(secondSpace + 1);

  if (mode == "120000") {
    rejection = "symbolic link '" + std::string(path) + "'";
    return std::nullopt;
  }
  if (mode == "160000" || type == "commit") {
    rejection = "submodule '" + std::string(path) + "'";
    return std::nullopt;
  }
  if ((mode != "100644" && mode != "100755") || type != "blob" ||
      objectId.size() < 40 || !isHex(objectId.substr(0, 40))) {
    rejection = "unsupported tree entry '" + std::string(path) +
                "' with mode " + std::string(mode);
    return std::nullopt;
  }
  return ExtractedTreeEntry{.path = std::string(path),
                            .executable = mode == "100755",
                            .objectId = std::string(objectId)};
}

// Dependency trees are untrusted input: every extracted path must stay
// strictly inside the checkout, avoid version-control metadata, and remain
// unambiguous on case-insensitive filesystems.
[[nodiscard]] bool validExtractionPath(std::string_view path,
                                       std::set<std::string> &caseFolded,
                                       std::string &rejection) {
  if (path.empty() || path.front() == '/' || path.back() == '/') {
    rejection = "unsafe path '" + std::string(path) + "'";
    return false;
  }
  std::string folded;
  folded.reserve(path.size());
  std::string_view remaining = path;
  while (!remaining.empty()) {
    const std::size_t separator = remaining.find('/');
    const std::string_view component = remaining.substr(0, separator);
    if (component.empty() || component == "." || component == "..") {
      rejection = "unsafe path '" + std::string(path) + "'";
      return false;
    }
    for (const char character : component) {
      if (character == '\\' || character == '\0' ||
          static_cast<unsigned char>(character) < 0x20U) {
        rejection = "unsafe path '" + std::string(path) + "'";
        return false;
      }
    }
    std::string foldedComponent(component);
    std::transform(foldedComponent.begin(), foldedComponent.end(),
                   foldedComponent.begin(), [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    if (foldedComponent == ".git") {
      rejection = "version-control path '" + std::string(path) + "'";
      return false;
    }
    if (!folded.empty()) {
      folded += '/';
    }
    folded += foldedComponent;
    if (separator == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(separator + 1);
  }
  if (!caseFolded.insert(folded).second) {
    rejection = "case-folded path collision at '" + std::string(path) + "'";
    return false;
  }
  return true;
}

struct GitInvocation {
  ProcessResult process;
  std::vector<std::string> command;
};

[[nodiscard]] GitInvocation runGit(const std::string &gitExecutable,
                                   std::vector<std::string> arguments,
                                   std::string_view description,
                                   bool captureSuccessfulOutput = false) {
  std::vector<std::string> command{gitExecutable};
  command.insert(command.end(), std::make_move_iterator(arguments.begin()),
                 std::make_move_iterator(arguments.end()));
  GitInvocation invocation{
      .process = invokeProcess(
          command, {.outputMode = ProcessOutputMode::Capture,
                    .captureSuccessfulOutput = captureSuccessfulOutput,
                    .description = std::string(description)}),
      .command = std::move(command)};
  return invocation;
}

[[nodiscard]] std::string outputTail(const ProcessResult &process) {
  constexpr std::size_t limit = 400;
  std::string tail = process.driverDiagnostic.value_or("");
  if (!process.output.empty()) {
    if (!tail.empty()) {
      tail += "; ";
    }
    tail += process.output.size() > limit
                ? "..." + process.output.substr(process.output.size() - limit)
                : process.output;
  }
  while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) {
    tail.pop_back();
  }
  return tail;
}

[[nodiscard]] bool writeExtractedFile(const std::filesystem::path &destination,
                                      std::string_view contents,
                                      bool executable, std::string &detail) {
  std::error_code error;
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error) {
    detail = "cannot create '" + destination.parent_path().string() +
             "': " + error.message();
    return false;
  }
  {
    std::ofstream output(destination, std::ios::binary);
    if (!output) {
      detail = "cannot open '" + destination.string() + "' for writing";
      return false;
    }
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output) {
      detail = "cannot finish writing '" + destination.string() + "'";
      return false;
    }
  }
  if (executable) {
    std::filesystem::permissions(destination,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, error);
    if (error) {
      detail = "cannot mark '" + destination.string() +
               "' executable: " + error.message();
      return false;
    }
  }
  return true;
}

} // namespace

bool isFullGitRevision(std::string_view revision) {
  return revision.size() == 40 && isHex(revision);
}

const LockedGitPackage *DependencyLock::find(const GitSourceKey &key) const {
  const auto found = std::find_if(packages.begin(), packages.end(),
                                  [&key](const LockedGitPackage &package) {
                                    return package.url == key.url &&
                                           package.revision == key.revision;
                                  });
  return found == packages.end() ? nullptr : &*found;
}

std::filesystem::path
dependencyLockPath(const std::filesystem::path &workspaceRoot) {
  return workspaceRoot / lockFilename;
}

DependencyLockLoadResult
loadDependencyLock(const std::filesystem::path &workspaceRoot) {
  DependencyLockLoadResult result;
  const std::filesystem::path path = dependencyLockPath(workspaceRoot);
  std::error_code existsError;
  if (!std::filesystem::exists(path, existsError)) {
    result.status = existsError ? DependencyLockLoadStatus::IoFailure
                                : DependencyLockLoadStatus::Missing;
    if (existsError) {
      result.diagnostics.push_back(
          lockDiagnostic("GTI-B1702", path,
                         "Cannot inspect gti.lock: " + existsError.message()));
    }
    return result;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    result.status = DependencyLockLoadStatus::IoFailure;
    result.diagnostics.push_back(
        lockDiagnostic("GTI-B1702", path, "Cannot read gti.lock."));
    return result;
  }
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};

  toml::parse_result parsed = toml::parse(text, path.string());
  if (!parsed) {
    const toml::parse_error &parseError = parsed.error();
    result.status = DependencyLockLoadStatus::ParseFailure;
    result.diagnostics.push_back(lockDiagnostic(
        "GTI-B1702", path,
        "gti.lock is not valid TOML: " + std::string(parseError.description()) +
            ".",
        static_cast<int>(parseError.source().begin.line)));
    return result;
  }
  const toml::table &document = parsed.table();

  const auto reject = [&](std::string message) {
    result.status = DependencyLockLoadStatus::ParseFailure;
    result.diagnostics.push_back(
        lockDiagnostic("GTI-B1702", path, std::move(message)));
  };

  const toml::node *versionNode = document.get("lock-version");
  const std::optional<std::int64_t> version =
      versionNode == nullptr ? std::nullopt
                             : versionNode->value<std::int64_t>();
  if (!version || *version != currentDependencyLockVersion) {
    reject("gti.lock must declare lock-version = 1.");
    return result;
  }
  for (const auto &[key, node] : document) {
    if (key.str() != "lock-version" && key.str() != "package") {
      reject("gti.lock contains an unknown field '" + std::string(key.str()) +
             "'.");
      return result;
    }
  }

  const toml::node *packagesNode = document.get("package");
  if (packagesNode == nullptr) {
    result.status = DependencyLockLoadStatus::Success;
    return result;
  }
  const toml::array *packages = packagesNode->as_array();
  if (packages == nullptr) {
    reject("gti.lock field 'package' must be an array of tables.");
    return result;
  }

  std::set<std::string> names;
  std::set<std::string> sources;
  for (const toml::node &packageNode : *packages) {
    const toml::table *package = packageNode.as_table();
    if (package == nullptr) {
      reject("gti.lock entries must be [[package]] tables.");
      return result;
    }
    LockedGitPackage locked;
    const auto requireString = [&](std::string_view field,
                                   std::string &destination) {
      const toml::node *node = package->get(field);
      const std::optional<std::string> value =
          node == nullptr ? std::nullopt : node->value<std::string>();
      if (!value || value->empty()) {
        reject("gti.lock package entries require a non-empty '" +
               std::string(field) + "' string.");
        return false;
      }
      destination = *value;
      return true;
    };
    if (!requireString("name", locked.name) ||
        !requireString("version", locked.version) ||
        !requireString("source", locked.url) ||
        !requireString("rev", locked.revision) ||
        !requireString("checksum", locked.checksum)) {
      return result;
    }
    for (const auto &[key, node] : *package) {
      if (key.str() != "name" && key.str() != "version" &&
          key.str() != "source" && key.str() != "rev" &&
          key.str() != "checksum" && key.str() != "dependencies") {
        reject("gti.lock package '" + locked.name +
               "' contains an unknown field '" + std::string(key.str()) + "'.");
        return result;
      }
    }
    constexpr std::string_view sourcePrefix = "git+";
    if (locked.url.size() <= sourcePrefix.size() ||
        locked.url.substr(0, sourcePrefix.size()) != sourcePrefix) {
      reject("gti.lock package '" + locked.name +
             "' has a source that is not a git+ URL.");
      return result;
    }
    locked.url = locked.url.substr(sourcePrefix.size());
    if (!isFullGitRevision(locked.revision)) {
      reject("gti.lock package '" + locked.name +
             "' must pin a full lowercase 40-hex revision.");
      return result;
    }
    if (!validChecksum(locked.checksum)) {
      reject("gti.lock package '" + locked.name +
             "' must record a sha256:<64 hex> checksum.");
      return result;
    }
    if (const toml::node *dependenciesNode = package->get("dependencies")) {
      const toml::array *dependencies = dependenciesNode->as_array();
      if (dependencies == nullptr) {
        reject("gti.lock package '" + locked.name +
               "' field 'dependencies' must be an array of strings.");
        return result;
      }
      for (const toml::node &dependency : *dependencies) {
        const std::optional<std::string> name = dependency.value<std::string>();
        if (!name || name->empty()) {
          reject("gti.lock package '" + locked.name +
                 "' field 'dependencies' must be an array of strings.");
          return result;
        }
        locked.dependencies.push_back(*name);
      }
      std::sort(locked.dependencies.begin(), locked.dependencies.end());
    }
    if (!names.insert(locked.name).second) {
      reject("gti.lock records package '" + locked.name + "' more than once.");
      return result;
    }
    if (!sources.insert(locked.url + "#" + locked.revision).second) {
      reject("gti.lock records source '" + locked.url + "#" + locked.revision +
             "' more than once.");
      return result;
    }
    result.lock.packages.push_back(std::move(locked));
  }

  std::sort(result.lock.packages.begin(), result.lock.packages.end(),
            [](const LockedGitPackage &left, const LockedGitPackage &right) {
              return left.name < right.name;
            });
  result.status = DependencyLockLoadStatus::Success;
  return result;
}

std::string renderDependencyLock(const DependencyLock &lock) {
  std::vector<const LockedGitPackage *> ordered;
  ordered.reserve(lock.packages.size());
  for (const LockedGitPackage &package : lock.packages) {
    ordered.push_back(&package);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const LockedGitPackage *left, const LockedGitPackage *right) {
              return left->name < right->name;
            });

  std::string text = "# Generated by `gti fetch`; records the exact pinned "
                     "git dependency closure.\nlock-version = 1\n";
  for (const LockedGitPackage *package : ordered) {
    text += "\n[[package]]\n";
    text += "name = " + tomlString(package->name) + "\n";
    text += "version = " + tomlString(package->version) + "\n";
    text += "source = " + tomlString("git+" + package->url) + "\n";
    text += "rev = " + tomlString(package->revision) + "\n";
    text += "checksum = " + tomlString(package->checksum) + "\n";
    if (!package->dependencies.empty()) {
      std::vector<std::string> dependencies = package->dependencies;
      std::sort(dependencies.begin(), dependencies.end());
      text += "dependencies = [";
      for (std::size_t index = 0; index < dependencies.size(); ++index) {
        if (index != 0) {
          text += ", ";
        }
        text += tomlString(dependencies[index]);
      }
      text += "]\n";
    }
  }
  return text;
}

bool writeDependencyLock(const std::filesystem::path &workspaceRoot,
                         const DependencyLock &lock,
                         std::string &errorMessage) {
  const std::filesystem::path destination = dependencyLockPath(workspaceRoot);
  const std::filesystem::path staged = stagedArtifactPath(destination);
  TemporaryArtifact stagedCleanup(staged, true);
  {
    std::ofstream output(staged, std::ios::binary);
    if (!output) {
      errorMessage = "cannot open '" + staged.string() + "' for writing";
      return false;
    }
    const std::string text = renderDependencyLock(lock);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output) {
      errorMessage = "cannot finish writing '" + staged.string() + "'";
      return false;
    }
  }
  const ArtifactPublishResult published = publishArtifact(staged, destination);
  if (!published.succeeded()) {
    errorMessage = "cannot publish '" + destination.string() +
                   "': " + published.error.message();
    return false;
  }
  return true;
}

std::filesystem::path
gitDependencyStoreRoot(const std::filesystem::path &workspaceRoot) {
  return workspaceRoot / "build" / "gti" / "deps" / "git";
}

std::filesystem::path
gitCheckoutPath(const std::filesystem::path &workspaceRoot,
                const GitSourceKey &key) {
  return gitDependencyStoreRoot(workspaceRoot) / "checkouts" /
         digestHex(key.url).substr(0, 32) / key.revision;
}

std::optional<std::string>
checkoutChecksum(const std::filesystem::path &checkout,
                 std::string &errorMessage) {
  struct Entry {
    std::string path;
    bool executable = false;
    std::filesystem::path file;
  };
  std::vector<Entry> entries;
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(checkout, error);
  const std::filesystem::recursive_directory_iterator end;
  if (error) {
    errorMessage =
        "cannot enumerate '" + checkout.string() + "': " + error.message();
    return std::nullopt;
  }
  while (iterator != end) {
    const std::filesystem::directory_entry entry = *iterator;
    error.clear();
    const std::filesystem::file_status status = entry.symlink_status(error);
    if (error) {
      errorMessage =
          "cannot inspect '" + entry.path().string() + "': " + error.message();
      return std::nullopt;
    }
    if (std::filesystem::is_regular_file(status)) {
      entries.push_back(
          {.path = entry.path().lexically_relative(checkout).generic_string(),
           .executable =
               (status.permissions() & std::filesystem::perms::owner_exec) !=
               std::filesystem::perms::none,
           .file = entry.path()});
    } else if (!std::filesystem::is_directory(status)) {
      errorMessage = "checkout contains a non-regular entry '" +
                     entry.path().string() + "'";
      return std::nullopt;
    }
    iterator.increment(error);
    if (error) {
      errorMessage =
          "cannot enumerate '" + checkout.string() + "': " + error.message();
      return std::nullopt;
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const Entry &left, const Entry &right) {
              return left.path < right.path;
            });

  Sha256 hash;
  const auto framed = [&hash](std::string_view value) {
    std::array<char, 8> size{};
    const std::uint64_t length = value.size();
    for (std::size_t index = 0; index < size.size(); ++index) {
      size[size.size() - index - 1] =
          static_cast<char>((length >> (index * 8U)) & 0xffU);
    }
    hash.update(std::string_view(size.data(), size.size()));
    hash.update(value);
  };
  framed("gti-dependency-tree-v1");
  for (const Entry &entry : entries) {
    framed(entry.path);
    framed(entry.executable ? "x" : "-");
    std::ifstream input(entry.file, std::ios::binary);
    if (!input) {
      errorMessage = "cannot read '" + entry.file.string() + "'";
      return std::nullopt;
    }
    const std::string contents{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    if (input.bad()) {
      errorMessage = "cannot finish reading '" + entry.file.string() + "'";
      return std::nullopt;
    }
    framed(contents);
  }
  return "sha256:" + hash.finishHex();
}

std::string discoverGitExecutable() {
  if (const char *configured = std::getenv("GTI_GIT");
      configured != nullptr && *configured != '\0') {
    return configured;
  }
  return "git";
}

GitFetchResult fetchGitSource(const std::filesystem::path &workspaceRoot,
                              const GitSourceKey &key,
                              const std::string &gitExecutable, bool offline) {
  GitFetchResult result;
  if (!isFullGitRevision(key.revision) || key.url.empty()) {
    result.status = GitFetchStatus::FetchFailure;
    result.detail = "git sources require a URL and a full 40-hex revision";
    return result;
  }
  result.checkout = gitCheckoutPath(workspaceRoot, key);

  const GitInvocation probe =
      runGit(gitExecutable, {"--version"}, "git discovery probe");
  if (!probe.process.succeeded()) {
    result.status = GitFetchStatus::GitUnavailable;
    result.detail =
        "cannot run '" + gitExecutable + "': " + outputTail(probe.process);
    return result;
  }

  const std::filesystem::path database =
      gitDependencyStoreRoot(workspaceRoot) / "db" /
      (digestHex(key.url).substr(0, 32) + ".git");
  std::error_code storeError;
  std::filesystem::create_directories(database.parent_path(), storeError);
  if (storeError) {
    result.status = GitFetchStatus::StoreFailure;
    result.detail = "cannot create dependency store: " + storeError.message();
    return result;
  }
  if (!std::filesystem::is_directory(database, storeError) || storeError) {
    const GitInvocation initialize =
        runGit(gitExecutable,
               {"init", "--quiet", "--bare", "--template=", database.string()},
               "git database initialization");
    if (!initialize.process.succeeded()) {
      result.status = GitFetchStatus::StoreFailure;
      result.detail = outputTail(initialize.process);
      return result;
    }
  }

  const auto revisionPresent = [&]() {
    return runGit(gitExecutable,
                  {"-C", database.string(), "cat-file", "-e",
                   key.revision + "^{commit}"},
                  "git revision probe")
        .process.succeeded();
  };
  if (!revisionPresent()) {
    if (offline) {
      result.status = GitFetchStatus::RevisionUnavailable;
      result.detail = "revision " + key.revision +
                      " is not in the local dependency store, and this "
                      "command is offline";
      return result;
    }
    const GitInvocation direct =
        runGit(gitExecutable,
               {"-C", database.string(), "fetch", "--quiet", "--no-tags",
                key.url, key.revision},
               "git revision fetch");
    if (!direct.process.succeeded()) {
      // Servers may refuse unadvertised revisions; mirror every ref and
      // check reachability locally instead.
      const GitInvocation mirror =
          runGit(gitExecutable,
                 {"-C", database.string(), "fetch", "--quiet", key.url,
                  "+refs/*:refs/gti/*"},
                 "git reference fetch");
      if (!mirror.process.succeeded()) {
        result.status = GitFetchStatus::FetchFailure;
        result.detail = outputTail(mirror.process);
        return result;
      }
    }
    if (!revisionPresent()) {
      result.status = GitFetchStatus::RevisionUnavailable;
      result.detail = "revision " + key.revision + " is not reachable from '" +
                      key.url + "'";
      return result;
    }
  }

  const GitInvocation listing =
      runGit(gitExecutable,
             {"-C", database.string(), "ls-tree", "-r", "-z", "--full-tree",
              key.revision},
             "git tree listing", true);
  if (!listing.process.succeeded()) {
    result.status = GitFetchStatus::FetchFailure;
    result.detail = outputTail(listing.process);
    return result;
  }

  std::vector<ExtractedTreeEntry> entries;
  std::set<std::string> caseFolded;
  std::string_view remaining = listing.process.output;
  while (!remaining.empty()) {
    const std::size_t terminator = remaining.find('\0');
    const std::string_view record = remaining.substr(0, terminator);
    if (!record.empty()) {
      std::string rejection;
      std::optional<ExtractedTreeEntry> entry =
          parseTreeRecord(record, rejection);
      if (entry && !validExtractionPath(entry->path, caseFolded, rejection)) {
        entry.reset();
      }
      if (!entry) {
        result.status = GitFetchStatus::TreeRejected;
        result.detail = "dependency tree rejected: " + rejection;
        return result;
      }
      entries.push_back(std::move(*entry));
    }
    if (terminator == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(terminator + 1);
  }

  const std::filesystem::path staging =
      result.checkout.parent_path() /
      stagedArtifactPath(result.checkout).filename();
  std::error_code stagingError;
  std::filesystem::create_directories(staging, stagingError);
  if (stagingError) {
    result.status = GitFetchStatus::StoreFailure;
    result.detail =
        "cannot create staging directory: " + stagingError.message();
    return result;
  }
  bool keepStaging = false;
  struct StagingCleanup {
    const std::filesystem::path &path;
    const bool &keep;
    ~StagingCleanup() {
      if (!keep) {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    }
  } stagingCleanup{staging, keepStaging};

  for (const ExtractedTreeEntry &entry : entries) {
    const GitInvocation blob =
        runGit(gitExecutable,
               {"-C", database.string(), "cat-file", "blob", entry.objectId},
               "git blob extraction", true);
    if (!blob.process.succeeded()) {
      result.status = GitFetchStatus::FetchFailure;
      result.detail = outputTail(blob.process);
      return result;
    }
    std::string writeDetail;
    if (!writeExtractedFile(staging / entry.path, blob.process.output,
                            entry.executable, writeDetail)) {
      result.status = GitFetchStatus::StoreFailure;
      result.detail = std::move(writeDetail);
      return result;
    }
  }

  std::string checksumError;
  const std::optional<std::string> checksum =
      checkoutChecksum(staging, checksumError);
  if (!checksum) {
    result.status = GitFetchStatus::StoreFailure;
    result.detail = std::move(checksumError);
    return result;
  }

  // The staged tree extracted from the object database is the authority. An
  // existing checkout that already matches is kept; a divergent one (torn,
  // tampered, or from a concurrent writer) is replaced.
  std::error_code checkoutState;
  if (std::filesystem::is_directory(result.checkout, checkoutState) &&
      !checkoutState) {
    std::string verifyError;
    const std::optional<std::string> existing =
        checkoutChecksum(result.checkout, verifyError);
    if (existing && *existing == *checksum) {
      result.status = GitFetchStatus::Success;
      result.checksum = *checksum;
      return result;
    }
    std::error_code removeError;
    std::filesystem::remove_all(result.checkout, removeError);
    if (removeError) {
      result.status = GitFetchStatus::StoreFailure;
      result.detail = "cannot replace divergent checkout '" +
                      result.checkout.string() + "': " + removeError.message();
      return result;
    }
  }
  std::error_code renameError;
  std::filesystem::rename(staging, result.checkout, renameError);
  if (renameError) {
    // A concurrent fetch may have published first; verify what exists.
    std::string verifyError;
    const std::optional<std::string> published =
        checkoutChecksum(result.checkout, verifyError);
    if (!published || *published != *checksum) {
      result.status = GitFetchStatus::StoreFailure;
      result.detail = "cannot publish checkout '" + result.checkout.string() +
                      "': " + renameError.message();
      return result;
    }
  } else {
    keepStaging = true;
  }

  result.status = GitFetchStatus::Success;
  result.checksum = *checksum;
  return result;
}

} // namespace lang::driver
