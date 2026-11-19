//===- Target/DirectX/PointerTypeAnalisis.cpp - PointerType analysis ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Analysis pass to assign types to opaque pointers.
//
//===----------------------------------------------------------------------===//

#include "PointerTypeAnalysis.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

using namespace llvm;
using namespace llvm::airbitcode;

static Type *resourceType(LLVMContext &C, StringRef Kind, unsigned AS);

namespace {

Type *classifyFunctionType(const Function &F, PointerTypeMap &Map);

// Resource handles must keep their opaque struct type through helper calls and
// loads. Metal's texture validation cannot interpret them as byte pointers.
static Type *
resourceUseType(const Value *V, const PointerTypeMap &Map, SmallPtrSetImpl<const Value *> &Seen) {
  if (!Seen.insert(V).second)
    return nullptr;
  // LLVM 23 represents numeric splats as ConstantData, which deliberately has
  // no use-list. Pointer classification only needs real instruction users.
  if (!V->hasUseList())
    return nullptr;
  for (auto *U : V->users()) {
    if (auto *Call = dyn_cast<CallBase>(U)) {
      auto *F = Call->getCalledFunction();
      if (!F)
        continue;
      for (unsigned i = 0; i < Call->arg_size() && i < F->arg_size(); ++i) {
        if (Call->getArgOperand(i) != V)
          continue;
        auto It = Map.find(F->getArg(i));
        if (It == Map.end())
          continue;
        auto *P = dyn_cast<TypedPointerType>(It->second);
        auto *S = P ? dyn_cast<StructType>(P->getElementType()) : nullptr;
        if (S && S->isOpaque())
          return P;
      }
    } else if (isa<BitCastInst>(U) || isa<PHINode>(U) || isa<SelectInst>(U)) {
      if (auto *T = resourceUseType(U, Map, Seen))
        return T;
    }
  }
  return nullptr;
}

// Classifies the type of the value passed in by walking the value's users to
// find a typed instruction to materialize a type from.
Type *
classifyPointerType(const Value *V, PointerTypeMap &Map) {
  assert(V->getType()->isPointerTy() && "classifyPointerType called with non-pointer");

  // A CallInst will trigger this case, and we want to classify its Function
  // operand as a Function rather than a generic Value.
  if (const Function *F = dyn_cast<Function>(V))
    return classifyFunctionType(*F, Map);

  // There can potentially be dead constants hanging off of the globals we do
  // not want to deal with. So we remove them here.
  if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(V))
    GV->removeDeadConstantUsers();

  auto It = Map.find(V);
  if (It != Map.end())
    return It->second;

  Type *PointeeTy = nullptr;
  if (auto *GEP = dyn_cast<GEPOperator>(V)) {
    if (!GEP->getResultElementType()->isPointerTy())
      PointeeTy = GEP->getResultElementType();
  } else if (auto *Inst = dyn_cast<AllocaInst>(V)) {
    PointeeTy = Inst->getAllocatedType();
  } else if (auto *GV = dyn_cast<GlobalVariable>(V)) {
    PointeeTy = GV->getValueType();
  }

  if (auto *Cast = dyn_cast<BitCastInst>(V)) {
    if (Cast->hasOneUse()) {
      auto *User = *Cast->user_begin();
      if (auto *L = dyn_cast<LoadInst>(User)) {
        PointeeTy = L->getType();
        if (PointeeTy->isPointerTy())
          PointeeTy = classifyPointerType(L, Map);
      } else if (auto *S = dyn_cast<StoreInst>(User)) {
        if (S->getPointerOperand() == Cast) {
          PointeeTy = S->getValueOperand()->getType();
          if (PointeeTy->isPointerTy())
            PointeeTy = TypedPointerType::get(Type::getInt8Ty(V->getContext()), PointeeTy->getPointerAddressSpace());
        }
      } else if (auto *Atomic = dyn_cast<AtomicRMWInst>(User)) {
        if (Atomic->getPointerOperand() == Cast)
          PointeeTy = Atomic->getValOperand()->getType();
      } else if (auto *Atomic = dyn_cast<AtomicCmpXchgInst>(User)) {
        if (Atomic->getPointerOperand() == Cast)
          PointeeTy = Atomic->getCompareOperand()->getType();
      } else if (auto *Call = dyn_cast<CallBase>(User)) {
        if (Call->getCalledOperand() == Cast) {
          SmallVector<Type *> Parameters;
          auto *FT = Call->getFunctionType();
          for (unsigned i = 0; i < FT->getNumParams(); ++i) {
            auto *T = FT->getParamType(i);
            Parameters.push_back(T->isPointerTy() ? classifyPointerType(Call->getArgOperand(i), Map) : T);
          }
          auto *Return = FT->getReturnType();
          if (Return->isPointerTy())
            Return = TypedPointerType::get(Type::getInt8Ty(V->getContext()), Return->getPointerAddressSpace());
          PointeeTy = FunctionType::get(Return, Parameters, FT->isVarArg());
        } else if (auto *F = Call->getCalledFunction()) {
          auto *FT = cast<FunctionType>(cast<TypedPointerType>(classifyFunctionType(*F, Map))->getElementType());
          for (unsigned i = 0; i < Call->arg_size(); ++i)
            if (Call->getArgOperand(i) == Cast) {
              auto *PT = cast<TypedPointerType>(FT->getParamType(i));
              PointeeTy = PT->getElementType();
            }
        }
      } else if (auto *G = dyn_cast<GetElementPtrInst>(User)) {
        PointeeTy = G->getSourceElementType();
      }
    }
  }
  if (!PointeeTy) {
    SmallPtrSet<const Value *, 16> Seen;
    if (auto *Resource = resourceUseType(V, Map, Seen)) {
      Map[V] = Resource;
      return Resource;
    }
  }
  // Recover device buffer elements from their typed accesses instead of
  // introducing byte pointers into Metal's object payload lowering.
  bool objectShader = false;
  if (auto *Inst = dyn_cast<Instruction>(V))
    if (auto *Objects = Inst->getModule()->getNamedMetadata("air.object"))
      for (auto *Entry : Objects->operands())
        objectShader |= cast<ConstantAsMetadata>(Entry->getOperand(0))->getValue() == Inst->getFunction();
  if (!PointeeTy && objectShader && V->getType()->getPointerAddressSpace() == 1) {
    for (auto *U : V->users()) {
      auto *Use = U;
      if (auto *Cast = dyn_cast<BitCastInst>(Use); Cast && Cast->hasOneUse())
        Use = *Cast->user_begin();
      if (auto *GEP = dyn_cast<GetElementPtrInst>(Use))
        PointeeTy = GEP->getSourceElementType();
      else if (auto *Load = dyn_cast<LoadInst>(Use))
        PointeeTy = Load->getType();
      else if (auto *Store = dyn_cast<StoreInst>(Use))
        PointeeTy = Store->getValueOperand()->getType();
      if (PointeeTy && (PointeeTy->isIntegerTy() || PointeeTy->isFloatingPointTy()))
        break;
      PointeeTy = nullptr;
    }
  }
  // If we were unable to determine the pointee type, set to i8
  // If we were able to determine the pointee type as ptr, set to i8*
  if (!PointeeTy)
    PointeeTy = Type::getInt8Ty(V->getContext());
  if (PointeeTy->isPointerTy())
    PointeeTy = TypedPointerType::get(Type::getInt8Ty(V->getContext()), PointeeTy->getPointerAddressSpace());
  auto *TypedPtrTy = TypedPointerType::get(PointeeTy, V->getType()->getPointerAddressSpace());

  Map[V] = TypedPtrTy;
  return TypedPtrTy;
}

// This function constructs a function type accepting typed pointers. It only
// handles function arguments and return types, and assigns the function type to
// the function's value in the type map.
Type *
classifyFunctionType(const Function &F, PointerTypeMap &Map) {
  auto It = Map.find(&F);
  if (It != Map.end())
    return It->second;

  SmallVector<Type *, 8> NewArgs;
  Type *RetTy = F.getReturnType();
  LLVMContext &Ctx = F.getContext();
  if (F.getName() == "air.get_read_sampler")
    RetTy = resourceType(Ctx, "sampler", 2);
  if (RetTy->isPointerTy()) {
    RetTy = nullptr;
    for (const auto &B : F) {
      const auto *RetInst = dyn_cast_or_null<ReturnInst>(B.getTerminator());
      if (!RetInst)
        continue;

      Type *NewRetTy = classifyPointerType(RetInst->getReturnValue(), Map);
      if (!RetTy)
        RetTy = NewRetTy;
      else if (RetTy != NewRetTy)
        RetTy = TypedPointerType::get(Type::getInt8Ty(Ctx), F.getReturnType()->getPointerAddressSpace());
    }
    // For function decl.
    if (!RetTy)
      RetTy = TypedPointerType::get(Type::getInt8Ty(Ctx), F.getReturnType()->getPointerAddressSpace());
  }
  for (auto &A : F.args()) {
    Type *ArgTy = A.getType();
    if (ArgTy->isPointerTy())
      ArgTy = classifyPointerType(&A, Map);
    NewArgs.push_back(ArgTy);
  }
  auto *TypedPtrTy = TypedPointerType::get(FunctionType::get(RetTy, NewArgs, false), 0);
  Map[&F] = TypedPtrTy;
  return TypedPtrTy;
}
} // anonymous namespace

static Type *
classifyConstantWithOpaquePtr(const Constant *C, PointerTypeMap &Map) {
  // FIXME: support ConstantPointerNull and UndefValue which could map to more
  // than one TypedPointerType. See
  // https://github.com/llvm/llvm-project/issues/57942.
  if (isa<ConstantPointerNull>(C) || (isa<UndefValue>(C) && C->getType()->isPointerTy()))
    return TypedPointerType::get(Type::getInt8Ty(C->getContext()), C->getType()->getPointerAddressSpace());

  // Skip ConstantData which cannot have opaque ptr.
  if (isa<ConstantData>(C))
    return C->getType();

  auto It = Map.find(C);
  if (It != Map.end())
    return It->second;

  if (const auto *F = dyn_cast<Function>(C))
    return classifyFunctionType(*F, Map);

  Type *Ty = C->getType();
  Type *TargetTy = nullptr;
  if (auto *CS = dyn_cast<ConstantStruct>(C)) {
    SmallVector<Type *> EltTys;
    for (unsigned int I = 0; I < CS->getNumOperands(); ++I) {
      const Constant *Elt = C->getAggregateElement(I);
      Type *EltTy = classifyConstantWithOpaquePtr(Elt, Map);
      EltTys.emplace_back(EltTy);
    }
    TargetTy = StructType::get(C->getContext(), EltTys);
  } else if (auto *CA = dyn_cast<ConstantAggregate>(C)) {

    Type *TargetEltTy = nullptr;
    for (auto &Elt : CA->operands()) {
      Type *EltTy = classifyConstantWithOpaquePtr(cast<Constant>(&Elt), Map);
      assert(TargetEltTy == EltTy || TargetEltTy == nullptr);
      TargetEltTy = EltTy;
    }

    if (auto *AT = dyn_cast<ArrayType>(Ty)) {
      TargetTy = ArrayType::get(TargetEltTy, AT->getNumElements());
    } else {
      // Not struct, not array, must be vector here.
      auto *VT = cast<VectorType>(Ty);
      TargetTy = VectorType::get(TargetEltTy, VT);
    }
  }
  // Must have a target ty when map.
  assert(TargetTy && "PointerTypeAnalyisis failed to identify target type");

  // Same type, no need to map.
  if (TargetTy == Ty)
    return Ty;

  Map[C] = TargetTy;
  return TargetTy;
}

static void
classifyGlobalCtorPointerType(const GlobalVariable &GV, PointerTypeMap &Map) {
  const auto *CA = dyn_cast<ConstantArray>(GV.getInitializer());
  if (!CA) {
    // An empty global_ctors will be a zeroinitializer, so just skip it.
    assert(
        isa<ConstantAggregateZero>(GV.getInitializer()) &&
        "global_ctors should be a ConstantArray or ConstantAggregateZero"
    );
    return;
  }
  // Type for global ctor should be array of { i32, void ()*, i8* }.
  Type *CtorArrayTy = classifyConstantWithOpaquePtr(CA, Map);

  // Map the global type.
  Map[&GV] = TypedPointerType::get(CtorArrayTy, GV.getType()->getPointerAddressSpace());
}

static Metadata *
field(const MDNode *N, StringRef Key) {
  for (unsigned i = 0; i + 1 < N->getNumOperands(); ++i)
    if (auto *S = dyn_cast<MDString>(N->getOperand(i)); S && S->getString() == Key)
      return N->getOperand(i + 1).get();
  return nullptr;
}
static Type *
resourceType(LLVMContext &C, StringRef Kind, unsigned AS) {
  std::string Name = ("struct._" + Kind + "_t").str();
  auto *T = StructType::getTypeByName(C, Name);
  if (!T)
    T = StructType::create(C, Name);
  return TypedPointerType::get(T, AS);
}
static Type *
airArgumentType(const MDNode *N, LLVMContext &C) {
  auto *Name = dyn_cast_or_null<MDString>(field(N, "air.arg_type_name"));
  Type *T = Type::getInt8Ty(C);
  if (Name) {
    auto S = Name->getString();
    if (S == "mesh_grid_properties")
      return resourceType(C, "mesh_grid_properties", 3);
    if (S.starts_with("mesh<"))
      return resourceType(C, "mesh", 7);
    if (S == "sampler")
      return resourceType(C, "sampler", 2);
    if (S.starts_with("texture") || S.starts_with("depth")) {
      auto Kind = S.take_until([](char c) { return c == '<'; }).str();
      auto Pos = Kind.starts_with("texture") ? 7 : 5;
      Kind.insert(Pos, "_");
      return resourceType(C, Kind, 1);
    }
    if (S == "uint")
      T = Type::getInt32Ty(C);
    else if (S == "uint4")
      T = FixedVectorType::get(Type::getInt32Ty(C), 4);
    else if (S == "float")
      T = Type::getFloatTy(C);
    else if (S == "ulong")
      T = Type::getInt64Ty(C);
  }
  if (auto *Fields = dyn_cast_or_null<MDNode>(field(N, "air.struct_type_info"))) {
    SmallVector<Type *> Types;
    for (unsigned i = 0; i + 1 < Fields->getNumOperands(); ++i)
      if (auto *S = dyn_cast<MDString>(Fields->getOperand(i)); S && S->getString() == "air.indirect_argument")
        Types.push_back(airArgumentType(cast<MDNode>(Fields->getOperand(i + 1)), C));
    T = StructType::get(C, Types);
  }
  if (auto *AS = dyn_cast_or_null<ConstantAsMetadata>(field(N, "air.address_space")))
    T = TypedPointerType::get(T, cast<ConstantInt>(AS->getValue())->getZExtValue());
  return T;
}

PointerTypeMap
PointerTypeAnalysis::run(const Module &M) {
  PointerTypeMap Map;
  for (auto Kind : {"air.vertex", "air.fragment", "air.kernel", "air.object", "air.mesh"}) {
    auto *Named = M.getNamedMetadata(Kind);
    if (!Named)
      continue;
    for (auto *Entry : Named->operands()) {
      auto *F = dyn_cast<Function>(cast<ConstantAsMetadata>(Entry->getOperand(0))->getValue());
      if (!F)
        continue;
      auto *Args = cast<MDNode>(Entry->getOperand(2));
      for (auto &Arg : Args->operands()) {
        auto *N = cast<MDNode>(Arg);
        auto Idx = cast<ConstantInt>(cast<ConstantAsMetadata>(N->getOperand(0))->getValue())->getZExtValue();
        if (F->getArg(Idx)->getType()->isPointerTy()) {
          auto *T = airArgumentType(N, M.getContext());
          // Payload metadata names its element type without an address-space
          // field. The function signature still carries the pointer's space.
          if (!isa<TypedPointerType>(T))
            T = TypedPointerType::get(T, F->getArg(Idx)->getType()->getPointerAddressSpace());
          Map[F->getArg(Idx)] = T;
        }
      }
    }
  }
  for (auto &G : M.globals()) {
    if (G.getType()->isPointerTy())
      classifyPointerType(&G, Map);
    if (G.getName() == "llvm.global_ctors")
      classifyGlobalCtorPointerType(G, Map);
  }

  for (auto &F : M) {
    if (!F.isDeclaration() || !F.getName().starts_with("air."))
      continue;
    auto N = F.getName();
    // Mesh handles are opaque resources, like textures, not byte buffers.
    // Match Apple's AIR ABI and propagate the types through helper calls.
    if (N == "air.set_threadgroups_per_grid_mesh_properties") {
      Map[F.getArg(0)] = resourceType(M.getContext(), "mesh_grid_properties", 3);
      continue;
    }
    if (N.starts_with("air.set_") && N.contains("_mesh")) {
      if (!F.arg_empty() && F.getArg(0)->getType()->isPointerTy() &&
          F.getArg(0)->getType()->getPointerAddressSpace() == 7)
        Map[F.getArg(0)] = resourceType(M.getContext(), "mesh", 7);
      continue;
    }

    // AIR atomic buffer intrinsics encode the pointee in their scalar suffix.
    // Declarations have no loads/stores from which opaque-pointer inference can recover it.
    if (N.starts_with("air.atomic.global.") || N.starts_with("air.atomic.local.")) {
      const auto Bits = N.ends_with(".i32") ? 32 : N.ends_with(".i64") ? 64 : 0;
      if (Bits)
        for (auto &A : F.args())
          if (A.getType()->isPointerTy())
            Map[&A] = TypedPointerType::get(Type::getIntNTy(M.getContext(), Bits), A.getType()->getPointerAddressSpace());
      continue;
    }
    auto Start = N.find("texture_");
    if (Start == StringRef::npos)
      Start = N.find("depth_");
    if (Start == StringRef::npos)
      continue;
    auto Kind = N.substr(Start).split('.').first;
    for (auto &A : F.args()) {
      if (!A.getType()->isPointerTy())
        continue;
      if (A.getArgNo() == 0)
        Map[&A] = resourceType(M.getContext(), Kind, A.getType()->getPointerAddressSpace());
      else if (N.starts_with("air.atomic_compare_exchange_weak_explicit_"))
        // The expected-value pointer uses the same texel type as the desired value.
        // The declaration has no memory accesses from which to infer that type.
        Map[&A] = TypedPointerType::get(F.getArg(A.getArgNo() + 1)->getType(), A.getType()->getPointerAddressSpace());
      else if (A.getArgNo() == 1 && (N.contains("sample_") || N.contains("gather_") || N.contains("read_")))
        Map[&A] = resourceType(M.getContext(), "sampler", A.getType()->getPointerAddressSpace());
    }
  }
  for (auto &F : M) {
    classifyFunctionType(F, Map);

    for (const auto &B : F) {
      for (const auto &I : B) {
        if (I.getType()->isPointerTy())
          classifyPointerType(&I, Map);
        for (const auto &O : I.operands())
          if (O.get()->getType()->isPointerTy())
            classifyPointerType(O.get(), Map);
      }
    }
  }
  return Map;
}
