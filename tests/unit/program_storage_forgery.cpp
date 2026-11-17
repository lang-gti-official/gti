// Forged MIR cannot erase a source global's required lifetime cleanup.
#include "gti/frontend.h"
#include "gti/mir_verifier.h"
#include "unit_test.h"

#include <iostream>

GTI_UNIT_TEST(program_storage_cleanup_forgery) {
  const auto frontend =
      lang::Frontend().analyze("program-storage-forgery.gti", R"(
class Owner {
public:
  Owner() {}
  ~Owner() {}
};
Owner root{};
int main() { return 0; }
)");
  if (!frontend.canGenerateCode() || !frontend.mirValid ||
      !lang::verifyMirProgram(frontend.mir).valid()) {
    std::cerr << "valid owning-global fixture failed
                 ";
        return 1;
  }
  auto forged = frontend.mir;
  auto &plan = const_cast<lang::MirProgramInitializationPlan &>(
      forged.programInitializationPlan());
  if (plan.steps.size() != 1 || !plan.steps.front().sourceCleanup)
    return 2;
  plan.steps.front().sourceCleanup.reset();
  plan.steps.front().requiresActiveCleanup = false;
  const auto result = lang::verifyMirProgram(forged);
  for (const auto &error : result.errors)
    if (error.message == "program-initialization step omits required cleanup")
      return 0;
  std::cerr << "erased cleanup obligation was not rejected by its storage proof
               ";
      return 3;
}
