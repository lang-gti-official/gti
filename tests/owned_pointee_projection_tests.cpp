#include "cpp_mir_body_emitter_test_fixture.h"

namespace {
bool hasProjectionIssue(const lang::MirVerificationResult &result) {
  return std::any_of(
      result.errors.begin(), result.errors.end(), [](const auto &error) {
        return error.message.find("owned-pointee projection summary") !=
               std::string::npos;
      });
}

void checkProjectionMutations(const lang::FrontendResult &frontend,
                              std::size_t expected) {
  expect(frontend.canGenerateCode(), "accessor source must pass the frontend");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  std::size_t count = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (!function.ownedPointeeProjection)
      continue;
    ++count;
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    expect(hir != nullptr &&
               hir->ownedPointeeProjection == function.ownedPointeeProjection,
           "concrete HIR and MIR retain exactly the same accessor proof");
    expect(function.ownedPointeeProjection->fields.size() == 1,
           "first projection slice proves one owned field only");
    for (int mutation = 0; mutation != 17; ++mutation) {
      if (mutation == 16 && function.ownedPointeeProjection->borrowIntrinsic !=
                                lang::IntrinsicKind::UniqueOwnerBorrowMut)
        continue;
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      auto *entry = changed->body.findBlock(changed->body.entry);
      auto *success = changed->body.findBlock(entry->terminator.target);
      switch (mutation) {
      case 0:
        changed->ownedPointeeProjection->fields.clear();
        break;
      case 1:
        changed->ownedPointeeProjection->fields.front() = 0;
        break;
      case 2:
        changed->ownedPointeeProjection->borrowIntrinsic =
            lang::IntrinsicKind::None;
        break;
      case 3:
        changed->staticMember = true;
        break;
      case 4:
        changed->virtualMethod = true;
        break;
      case 5:
        changed->parameterTypes.push_back(lang::SemanticType::Int32);
        break;
      case 6:
        changed->returnType.referenceAccess =
            changed->returnType.referenceAccess == lang::AccessMode::Mutable
                ? lang::AccessMode::ReadOnly
                : lang::AccessMode::Mutable;
        break;
      case 7:
        changed->returnBorrowOrigin = lang::BorrowOriginKind::Argument;
        break;
      case 8: {
        auto *place = const_cast<lang::MirPlace *>(changed->body.findPlace(
            entry->instructions.front().operands.front().place));
        place->root = lang::MirPlaceRootKind::Value;
        break;
      }
      case 9: {
        auto *place = const_cast<lang::MirPlace *>(changed->body.findPlace(
            entry->instructions.front().operands.front().place));
        place->projections.front().kind = lang::MirProjectionKind::Dereference;
        break;
      }
      case 10: {
        auto *owner = const_cast<lang::MirClassInstance *>(
            malformed.findClassInstance(*changed->owner));
        for (auto &field : owner->declaredFields)
          if (field.symbol == changed->ownedPointeeProjection->fields.front())
            field.type = lang::SemanticType::referenceTo(
                field.type.arguments.front(), lang::AccessMode::ReadOnly);
        break;
      }
      case 11:
        entry->instructions.push_back(entry->instructions.back());
        break;
      case 12:
        entry->terminator.kind = lang::MirTerminatorKind::Branch;
        break;
      case 13:
        success->terminator.returnLoan = entry->instructions.back().loan;
        break;
      case 14:
        changed->body.fullExpressions.front().roots.clear();
        break;
      case 15:
        changed->definitionKind = lang::MirDefinitionKind::RuntimeBinding;
        break;
      case 16: {
        auto *owner = const_cast<lang::MirClassInstance *>(
            malformed.findClassInstance(*changed->owner));
        for (auto &field : owner->declaredFields)
          if (field.symbol == changed->ownedPointeeProjection->fields.front())
            field.access = lang::AccessMode::ReadOnly;
        break;
      }
      }
      expect(hasProjectionIssue(lang::verifyMirProgram(malformed)),
             "each forged summary/body must fail its independent projection "
             "proof");
    }
  }
  expect(count == expected,
         "fixture must retain the expected exact accessor count");
}

lang::FrontendResult trusted(std::string name, const std::string &source) {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto path = std::filesystem::temp_directory_path() / name;
  const auto key = std::filesystem::weakly_canonical(path).string();
  return lang::Frontend().analyze(path, source, {root / "prelude.gti", path},
                                  {{key, source}}, {root});
}

void checkSourceDefinedIdentity() {
  const auto frontend = trusted("gti-owned-pointee-renamed.gti", R"gti(
#include <std/memory>
class Item { public: int read() { return 4; }  public: Item() = default; };
class UnrelatedName<T> {
  mut gti_internal::unique_owner<T> resource = nullptr;
public:
  T& inspect() { return gti_internal::unique_owner_borrow(this.resource); }
  mut T& alter() mut { return gti_internal::unique_owner_borrow_mut(this.resource); }
  T& extra_statement() {
    int unrelated = 1;
    return gti_internal::unique_owner_borrow(this.resource);
  }
  T& with_parameter(int unrelated) {
    return gti_internal::unique_owner_borrow(this.resource);
  }
public:
  UnrelatedName() = default;
};
class Borrowed {
  mut Item value{};
public:
  mut Item& alter() mut { return this.value; }
public:
  Borrowed() = default;
};
int read(UnrelatedName<Item>& owner) { return owner.inspect().read(); }
int write(mut UnrelatedName<Item>& owner) { return owner.alter().read(); }
int extra(UnrelatedName<Item>& owner) { return owner.extra_statement().read(); }
int parameterized(UnrelatedName<Item>& owner) { return owner.with_parameter(1).read(); }
int borrowed(mut Borrowed& owner) { return owner.alter().read(); }
int main() { return 0; }
)gti");
  checkProjectionMutations(frontend, 2);
  if (!frontend.canGenerateCode())
    return;
  std::size_t unprovenAccessors = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (!function.owner ||
        function.returnType.kind != lang::SemanticType::Reference ||
        function.ownedPointeeProjection)
      continue;
    ++unprovenAccessors;
    auto malformed = frontend.mir;
    auto *changed = const_cast<lang::MirFunctionInstance *>(
        malformed.findFunctionInstance(function.id));
    const auto *owner = malformed.findClassInstance(*function.owner);
    if (owner == nullptr || owner->declaredFields.empty())
      continue;
    changed->ownedPointeeProjection = lang::OwnedPointeeProjection{
        .fields = {owner->declaredFields.front().symbol},
        .borrowIntrinsic = lang::IntrinsicKind::UniqueOwnerBorrowMut};
    expect(hasProjectionIssue(lang::verifyMirProgram(malformed)),
           "an unproven reference accessor cannot forge unique ownership "
           "provenance");
  }
  expect(
      unprovenAccessors == 3,
      "ordinary, extra-statement, and parameterized accessors remain unproven");
}
} // namespace

int main() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/owned_pointee_accessor.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  checkProjectionMutations(analyzeWithStandardLibrary(path.string(), source),
                           2);
  checkSourceDefinedIdentity();
  if (failures != 0)
    return 1;
  std::cout << "owned-pointee accessor projection proof tests passed\n";
  return 0;
}
