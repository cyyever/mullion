#include "guard_out_of_bounds.hpp"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

namespace llvm::air {

// A null address suppresses an access. ICB compute pipelines also guard zero-row loads:
// Metal shader validation reports invalid reads of the shader-global fallback with
// supportIndirectCommandBuffers enabled, including for Apple-compiled MSL.
static bool
Fallback(Value *Pointer, bool Load) {
  if (isa<ConstantPointerNull>(Pointer))
    return true;
  auto Global = dyn_cast<GlobalVariable>(Pointer->stripPointerCasts());
  return Load && Global && Global->getName() == "mullion.zero_row" && Global->isConstant() &&
         Global->hasInitializer() && Global->getInitializer()->isNullValue();
}

static SelectInst *
Bounded(Value *Pointer, bool Load = false) {
  // Scalar components can address a field of the selected row.
  while (auto GEP = dyn_cast_or_null<GetElementPtrInst>(Pointer))
    Pointer = GEP->getPointerOperand();
  auto Select = dyn_cast_or_null<SelectInst>(Pointer);
  return Select && (Fallback(Select->getFalseValue(), Load) || Fallback(Select->getTrueValue(), Load)) ? Select
                                                                                                       : nullptr;
}

PreservedAnalyses
GuardOutOfBoundsPass::run(Function &F, FunctionAnalysisManager &AM) {
  const bool GuardConstants = F.hasFnAttribute("mullion.guard-cbuffer-loads");
  F.removeFnAttr("mullion.guard-cbuffer-loads");
  SmallVector<std::pair<Instruction *, SelectInst *>> Accesses;
  SmallSetVector<SelectInst *, 8> Selects;
  for (auto &I : instructions(F)) {
    SelectInst *Select = nullptr;
    if (auto Pointer = getLoadStorePointerOperand(&I))
      Select = Bounded(Pointer, GuardConstants && isa<LoadInst>(I));
    else if (auto RMW = dyn_cast<AtomicRMWInst>(&I))
      Select = Bounded(RMW->getPointerOperand());
    else if (auto Exchange = dyn_cast<AtomicCmpXchgInst>(&I))
      Select = Bounded(Exchange->getPointerOperand());
    else if (auto Call = dyn_cast<CallInst>(&I))
      for (auto &Arg : Call->args())
        if ((Select = Bounded(Arg)))
          break;
    if (Select)
      Accesses.push_back({&I, Select}), Selects.insert(Select);
  }
  for (auto [I, Select] : Accesses) {
    auto Head = I->getParent();
    bool Inverted = Fallback(Select->getTrueValue(), GuardConstants && isa<LoadInst>(I));
    auto Inside = Inverted ? BinaryOperator::CreateNot(Select->getCondition(), "", I) : Select->getCondition();
    auto Then = SplitBlockAndInsertIfThen(Inside, I, false);
    auto Tail = I->getParent();
    I->moveBefore(Then);
    I->replaceUsesOfWith(Select, Inverted ? Select->getFalseValue() : Select->getTrueValue());
    if (!I->getType()->isVoidTy()) {
      auto Read = PHINode::Create(I->getType(), 2, "", &Tail->front());
      I->replaceAllUsesWith(Read);
      Read->addIncoming(I, Then->getParent());
      Read->addIncoming(Constant::getNullValue(I->getType()), Head);
    }
  }
  for (auto Select : Selects)
    if (Select->use_empty())
      Select->eraseFromParent();
  return Accesses.empty() && !GuardConstants ? PreservedAnalyses::all() : PreservedAnalyses::none();
}

} // namespace llvm::air
