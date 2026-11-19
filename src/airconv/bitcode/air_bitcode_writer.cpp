#include "air_bitcode_writer.hpp"
#include "DXILBitcodeWriter.h"
#include "DirectXIRPasses/DXILDebugInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Support/KnownBits.h"
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
// LLVM validates IR types; mesh capacities and topology are AIR metadata contracts.
llvm::Error verifyMeshOutputs(const llvm::Module &module) {
  auto *meshes = module.getNamedMetadata("air.mesh");
  if (!meshes)
    return llvm::Error::success();
  for (auto *entry : meshes->operands()) {
    if (!entry->getNumOperands())
      return llvm::createStringError("AIR mesh entry has no function");
    auto *function = llvm::mdconst::dyn_extract_or_null<llvm::Function>(entry->getOperand(0));
    if (!function)
      return llvm::createStringError("AIR mesh entry has no function");
    llvm::SmallPtrSet<const llvm::MDNode *, 16> visited;
    llvm::SmallVector<const llvm::MDNode *, 16> pending{entry};
    const llvm::MDNode *info = nullptr;
    while (!pending.empty()) {
      auto *node = pending.pop_back_val();
      if (!visited.insert(node).second)
        continue;
      auto *tag = node->getNumOperands() ? llvm::dyn_cast_or_null<llvm::MDString>(node->getOperand(0)) : nullptr;
      if (tag && tag->getString() == "air.mesh_type_info") {
        if (info)
          return llvm::createStringError("AIR mesh has multiple output declarations");
        info = node;
      }
      for (auto &operand : node->operands())
        if (auto *child = llvm::dyn_cast_or_null<llvm::MDNode>(operand))
          pending.push_back(child);
    }
    if (!info || info->getNumOperands() != 6)
      return llvm::createStringError("AIR mesh requires an output capacity and topology declaration");
    auto *vertices = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(info->getOperand(3));
    auto *primitives = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(info->getOperand(4));
    auto *topology = llvm::dyn_cast_or_null<llvm::MDString>(info->getOperand(5));
    if (!vertices || !primitives || !vertices->getType()->isIntegerTy(32) ||
        !primitives->getType()->isIntegerTy(32) || vertices->getZExtValue() > 256 || primitives->getZExtValue() > 512)
      return llvm::createStringError("AIR mesh output capacity exceeds 256 vertices or 512 primitives");
    unsigned corners = topology && topology->getString() == "air.point" ? 1
                       : topology && topology->getString() == "air.line" ? 2
                       : topology && topology->getString() == "air.triangle" ? 3 : 0;
    if (!corners)
      return llvm::createStringError("AIR mesh has an invalid output topology");
    for (auto &block : *function)
      for (auto &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        auto *callee = call ? call->getCalledFunction() : nullptr;
        if (!callee)
          continue;
        auto exceeds = [&](unsigned argument, uint64_t limit) {
          auto *value = argument < call->arg_size() ? llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(argument)) : nullptr;
          return value && value->getValue().uge(limit);
        };
        if (callee->getName() == "air.set_index_mesh" &&
            (exceeds(1, corners * primitives->getZExtValue()) || exceeds(2, vertices->getZExtValue())))
          return llvm::createStringError("AIR mesh constant index exceeds its declared output capacity");
        if (callee->getName() == "air.set_primitive_count_mesh" && exceeds(1, primitives->getZExtValue() + 1))
          return llvm::createStringError("AIR mesh primitive count exceeds its declared output capacity");
      }
  }
  return llvm::Error::success();
}

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
  std::string verification;
  llvm::raw_string_ostream diagnostics(verification);
  if (llvm::verifyModule(module, &diagnostics))
    return llvm::createStringError(llvm::inconvertibleErrorCode(), "Invalid input AIR IR: " + verification);
  if (auto error = verifyMeshOutputs(module))
    return error;
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
  // A dead user of one expression may itself occur later in this list. LLVM's
  // cleanup would delete it before visiting that entry. Leave constant lifetime
  // to the LLVM context; only materialize the instruction operands here.
  llvm::convertUsersOfConstantsToInstructions(expressions, nullptr, false, true);

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
        if (gep->getSourceElementType()->isIntegerTy(8)) {
          auto *offset = gep->getOperand(1);
          auto zeros = llvm::computeKnownBits(offset, m->getDataLayout()).countMinTrailingZeros();
          if (zeros >= 1) {
            unsigned shift = std::min(zeros, 2u);
            auto *element = llvm::IntegerType::get(m->getContext(), 8u << shift);
            llvm::IRBuilder<> builder(gep);
            gep->setOperand(1, builder.CreateAShr(offset, shift, "air.element.offset", true));
            gep->setSourceElementType(element);
            gep->setResultElementType(element);
          }
        }
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

  verification.clear();
  if (llvm::verifyModule(*m, &diagnostics))
    return llvm::createStringError(llvm::inconvertibleErrorCode(), "Invalid prepared AIR IR: " + verification);

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
