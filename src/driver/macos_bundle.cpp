#include "gti/driver/macos_bundle.h"

#include "gti/driver/artifact.h"
#include "gti/driver/build.h"
#include "gti/driver/process.h"
#include "gti/driver/project.h"

#include <fstream>
#ifdef __APPLE__
#include <stdio.h>
#endif

namespace lang::driver {
namespace fs = std::filesystem;
#ifdef __APPLE__
namespace {
struct StagedBundle {
  fs::path path;
  ~StagedBundle() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};

std::string plistString(std::string_view value) {
  std::string escaped;
  for (char c : value) {
    switch (c) {
    case '&':
      escaped += "&amp;";
      break;
    case '<':
      escaped += "&lt;";
      break;
    case '>':
      escaped += "&gt;";
      break;
    default:
      escaped += c;
    }
  }
  return "<string>" + escaped + "</string>\n";
}

std::string infoPlist(const ProjectBuildPlan &plan) {
  const auto &bundle = *plan.macosBundle();
  std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<plist version=\"1.0\"><dict>\n";
  auto field = [&](std::string key, const std::string &value) {
    xml += "<key>" + key + "</key>" + plistString(value);
  };
  field("CFBundlePackageType", "APPL");
  field("CFBundleInfoDictionaryVersion", "6.0");
  field("CFBundleExecutable", plan.targetName());
  field("CFBundleIdentifier", bundle.identifier);
  field("CFBundleName", bundle.name);
  field("CFBundleDisplayName", bundle.name);
  // Apple version keys use the numeric components of the package version.
  const auto version = plan.packageVersion().substr(
      0, plan.packageVersion().find_first_of("-+"));
  field("CFBundleShortVersionString", version);
  field("CFBundleVersion", version);
  if (!bundle.icon.empty())
    field("CFBundleIconFile", fs::path(bundle.icon).filename().string());
  return xml + "</dict></plist>\n";
}
} // namespace
#endif

std::optional<std::string> publishMacosBundle(const ProjectBuildPlan &plan,
                                              fs::path &destination) {
#ifdef __APPLE__
  if (auto issue = managedExecutableDiagnostic(
          {plan.workspaceRoot(), plan.workspaceRoot() / "build/gti"},
          plan.output()))
    return issue;
  const auto &bundle = *plan.macosBundle();
  destination = plan.output().parent_path() / (bundle.name + ".app");
  const auto stagePath = stagedArtifactPath(destination);
  try {
    if (!fs::create_directory(stagePath))
      return "bundle staging directory already exists";
    StagedBundle stage{stagePath};
    const auto contents = stage.path / "Contents";
    const auto resources = contents / "Resources";
    fs::create_directories(contents / "MacOS");
    fs::create_directory(resources);
    fs::copy_file(plan.output(), contents / "MacOS" / plan.targetName());
    auto copyResource = [&](const fs::path &relative, bool icon) {
      if (relative.empty() || relative.is_absolute() ||
          relative.lexically_normal() != relative ||
          *relative.begin() == ".." ||
          relative.string().find('\0') != std::string::npos)
        throw std::runtime_error("invalid bundle resource path: " +
                                 relative.string());
      const auto source = fs::canonical(plan.packageRoot() / relative);
      const auto within =
          source.lexically_relative(fs::canonical(plan.packageRoot()));
      if (within.empty() || *within.begin() == ".." ||
          !fs::is_regular_file(source))
        throw std::runtime_error(
            "bundle resource must be a file inside the package: " +
            relative.string());
      const auto output = resources / (icon ? relative.filename() : relative);
      fs::create_directories(output.parent_path());
      fs::copy_file(source, output);
    };
    if (!bundle.icon.empty())
      copyResource(bundle.icon, true);
    for (const auto &file : bundle.resources)
      copyResource(file, false);
    if (writeArtifact(contents / "Info.plist", infoPlist(plan)) !=
        ArtifactWriteStatus::Success)
      return "could not write bundle Info.plist";
    const auto marker = fs::path("Contents/Resources/.gti-bundle-owner");
    const auto owner = plan.packageName() + "/" + plan.targetName();
    if (fs::exists(stage.path / marker) ||
        writeArtifact(stage.path / marker, owner) !=
            ArtifactWriteStatus::Success)
      return "could not write bundle ownership record";
    const auto sign = invokeProcess(
        {"/usr/bin/codesign", "--force", "--sign", "-", stage.path.string()});
    if (!sign.succeeded())
      return "ad-hoc signing failed: " +
             sign.driverDiagnostic.value_or(sign.output);
    if (renamex_np(stage.path.c_str(), destination.c_str(), RENAME_EXCL) == 0)
      return std::nullopt;
    if (errno != EEXIST)
      return "could not publish bundle: " +
             std::error_code(errno, std::generic_category()).message();
    if (!fs::is_directory(fs::symlink_status(destination)) ||
        !fs::is_regular_file(fs::symlink_status(destination / marker)) ||
        fs::canonical(destination / marker) !=
            fs::canonical(destination) / marker)
      return "refusing to replace an unmanaged bundle: " + destination.string();
    std::ifstream record(destination / marker);
    std::string previousOwner;
    std::getline(record, previousOwner);
    if (previousOwner != owner)
      return "bundle destination belongs to another target";
    if (renamex_np(stage.path.c_str(), destination.c_str(), RENAME_SWAP) != 0)
      return "could not replace bundle: " +
             std::error_code(errno, std::generic_category()).message();
    return std::nullopt;
  } catch (const std::exception &e) {
    return e.what();
  }
#else
  (void)plan;
  (void)destination;
  return "macOS bundles require a macOS host";
#endif
}
} // namespace lang::driver
