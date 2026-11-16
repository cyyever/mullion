#pragma once
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"
namespace llvm { class Module; }
namespace mullion::air {
llvm::Error writeTypedBitcode(const llvm::Module &module, llvm::SmallVectorImpl<char> &output);
}
