// The optimizer rejects malformed input and folds known values with LLVM's standard passes.
#include "../../src/airconv/airconv_context.hpp"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>

static void require(bool ok, const char *why) {
  if (!ok) { llvm::errs() << why << "\n"; std::exit(1); }
}

int main() {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"(
    define i32 @known(i32 %x) {
      %bits = and i32 %x, 0
      %result = add i32 %bits, 42
      ret i32 %result
    }
  )", diagnostic, context);
  require(bool(module), "fixture did not parse");
  mullion::initializeModule(*module);
  std::string error;
  llvm::raw_string_ostream stream(error);
  require(mullion::runOptimizationPasses(*module, stream), "valid input rejected");
  auto *function = module->getFunction("known");
  auto *ret = llvm::dyn_cast<llvm::ReturnInst>(function->getEntryBlock().getTerminator());
  auto *constant = ret ? llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue()) : nullptr;
  require(constant && constant->getZExtValue() == 42, "known bits were not folded");
  // Inject a return type mismatch: verification must reject it before optimization consumes it.
  ret->setOperand(0, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 42));
  require(!mullion::runOptimizationPasses(*module, stream), "invalid return accepted");
  require(!error.empty(), "missing verifier diagnostic");
  llvm::outs() << "PASS: known-bit folding and invalid-IR rejection\n";
}
