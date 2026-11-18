#include "air_bitcode_writer.hpp"
#include "DXILBitcodeWriter.h"
#include "DirectXIRPasses/DXILDebugInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/ReplaceConstant.h"
#include <vector>
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/Endian.h"

namespace mullion::air {
namespace {
template <typename T> llvm::Error prepareAIRAttributes(T &value) {
  // These promises may be discarded after optimization. Keep semantic and ABI
  // attributes, including convergent and the memory effects translated by AIR.
  for (auto kind : {llvm::Attribute::NoCreateUndefOrPoison, llvm::Attribute::NoDivergenceSource})
    value.removeFnAttr(kind);
  // Value constraints only add optimization assumptions. Removing them from
  // the output does not change the defined executions of an optimized shader.
  for (auto kind : {llvm::Attribute::NoFPClass, llvm::Attribute::Range}) {
    value.removeRetAttr(kind);
    for (unsigned i = 0; i < value.arg_size(); ++i)
      value.removeParamAttr(i, kind);
  }
  auto memory = value.getAttributes().getFnAttr(llvm::Attribute::Memory);
  if (memory.isValid()) {
    auto effects = memory.getMemoryEffects();
    if (!effects.doesNotAccessMemory() && !effects.onlyReadsMemory() && !effects.onlyAccessesArgPointees())
      value.removeFnAttr(llvm::Attribute::Memory);
  }
  for (unsigned i = 0; i < value.arg_size(); ++i) {
    auto captures = value.getAttributes().getParamAttr(i, llvm::Attribute::Captures);
    if (captures.isValid() && captures.getCaptureInfo() != llvm::CaptureInfo::none())
      value.removeParamAttr(i, llvm::Attribute::Captures);

  }
  for (auto attrs : value.getAttributes()) {
    for (auto attr : attrs) {
      if (attr.isStringAttribute()) continue;
      auto kind = attr.getKindAsEnum();
      if ((attr.isEnumAttribute() || attr.isIntAttribute()) &&
          (llvm::airbitcode::getAIRAttributeEncoding(kind) ||
           kind == llvm::Attribute::Captures || kind == llvm::Attribute::Memory))
        continue;
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                    "AIR cannot encode attribute '" + attr.getAsString() + "'");
    }
  }
  return llvm::Error::success();
}
} // namespace
llvm::Error
writeTypedBitcode(const llvm::Module &module, llvm::SmallVectorImpl<char> &output) {
  // LLVM optimizes opaque pointers. AIR requires typed pointers and casts at
  // each differently typed use; keep those serialization-only casts off the
  // module used by the optimizer and the caller.
  auto m = llvm::CloneModule(module);
  // Opaque-pointer constant GEPs can use a different element type than their
  // global. Materialize them so the typed AIR casts below also reach their bases.
  std::vector<llvm::Constant *> expressions;
  for (auto &f : *m)
    for (auto &b : f)
      for (auto &i : b)
        for (auto &operand : i.operands())
          if (auto *expression = llvm::dyn_cast<llvm::ConstantExpr>(operand.get()))
            expressions.push_back(expression);
  llvm::convertUsersOfConstantsToInstructions(expressions, nullptr, true, true);

  // LLVM 23 dropped the size operand from lifetime markers. AIR still reads
  // the typed-pointer signature (i64 size, i8* address); emitting the new form
  // makes Apple's intrinsic upgrade construct a mismatched call.
  llvm::SmallVector<llvm::CallInst *> lifetimes;
  for (auto &f : *m)
    for (auto &b : f)
      for (auto &i : b)
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&i);
            call && (call->getIntrinsicID() == llvm::Intrinsic::lifetime_start ||
                     call->getIntrinsicID() == llvm::Intrinsic::lifetime_end))
          lifetimes.push_back(call);
  for (auto *call : lifetimes) {
    auto *original = call->getCalledFunction();
    auto *pointer = call->getArgOperand(0);
    llvm::IRBuilder<> builder(call);
    auto name = std::string(call->getIntrinsicID() == llvm::Intrinsic::lifetime_start
                                ? "llvm.lifetime.start.p" : "llvm.lifetime.end.p") +
                std::to_string(pointer->getType()->getPointerAddressSpace()) + "i8";
    auto *type = llvm::FunctionType::get(builder.getVoidTy(),
                                        {builder.getInt64Ty(), pointer->getType()}, false);
    auto legacy = m->getOrInsertFunction(name, type);
    builder.CreateCall(legacy, {builder.getInt64(-1), pointer});
    call->eraseFromParent();
    if (original->use_empty()) original->eraseFromParent();
  }

  // Metal's object shader lowering fails on byte-array pointer arithmetic
  // combined with payload writes. Preserve the stride using an integer element.
  llvm::SmallPtrSet<const llvm::Function *, 4> objectFunctions;
  if (auto *objects = m->getNamedMetadata("air.object"))
    for (auto *entry : objects->operands())
      objectFunctions.insert(llvm::cast<llvm::Function>(
          llvm::cast<llvm::ConstantAsMetadata>(entry->getOperand(0))->getValue()));
  for (auto &f : *m)
    for (auto &b : f)
      for (auto &i : b) {
        if (!objectFunctions.contains(&f))
          continue;
        auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&i);
        if (!gep || gep->getNumIndices() != 1 || gep->getPointerAddressSpace() != 1)
          continue;
        auto *array = llvm::dyn_cast<llvm::ArrayType>(gep->getSourceElementType());
        if (!array || !array->getElementType()->isIntegerTy(8))
          continue;
        auto bytes = array->getNumElements();
        if (bytes != 2 && bytes != 4 && bytes != 8)
          continue;
        auto *element = llvm::IntegerType::get(m->getContext(), bytes * 8);
        gep->setSourceElementType(element);
        gep->setResultElementType(element);
      }

  llvm::SmallVector<llvm::Instruction *> insts;
  // Remove unsupported optimization promises before enumerating attribute groups,
  // so a lone promise does not leave an empty group for the AIR reader.
  for (auto &f : *m) {
    if (auto error = prepareAIRAttributes(f)) return error;
    for (auto &b : f)
      for (auto &i : b)
        insts.push_back(&i);
  }
  for (auto *i : insts) {
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(i))
      if (auto error = prepareAIRAttributes(*call)) return error;
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(i);
        call && !call->getCalledFunction() &&
        llvm::isa<llvm::Function>(call->getCalledOperand()->stripPointerCasts())) {
      auto *callee = call->getCalledOperand();
      call->setCalledOperand(new llvm::BitCastInst(callee, callee->getType(), "air.callee", i->getIterator()));
    }
    for (auto &u : i->operands()) {
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(i); call && &u == &call->getCalledOperandUse())
        continue;
      auto *v = u.get();
      if (!v->getType()->isPointerTy() || llvm::isa<llvm::Function>(v))
        continue;
      llvm::Instruction *before = i;
      if (auto *phi = llvm::dyn_cast<llvm::PHINode>(i))
        before = phi->getIncomingBlock(u.getOperandNo())->getTerminator();
      auto *cast = new llvm::BitCastInst(v, v->getType(), "air.ptr", before->getIterator());
      u.set(cast);
    }
  }

  llvm::SmallVector<char, 0> bitcode;
  {
    llvm::airbitcode::BitcodeWriter writer(bitcode);
    writer.writeModule(*m, llvm::airbitcode::DXILDebugInfoMap{});
  }
  // The Metal container embeds Darwin-wrapped bitcode, not a bare BC stream.
  auto start = output.size();
  output.resize(start + 20);
  llvm::support::endian::write32le(output.data() + start, 0x0b17c0de);
  llvm::support::endian::write32le(output.data() + start + 4, 0);
  llvm::support::endian::write32le(output.data() + start + 8, 20);
  llvm::support::endian::write32le(output.data() + start + 12, bitcode.size());
  llvm::support::endian::write32le(output.data() + start + 16, 0xffffffff);
  output.append(bitcode);
  while ((output.size() - start) % 16)
    output.push_back(0);
  return llvm::Error::success();
}
} // namespace mullion::air
