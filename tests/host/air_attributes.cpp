#include "../../src/airconv/bitcode/air_bitcode_writer.hpp"
#include "../../src/airconv/metallib_writer.hpp"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <tuple>

static void require(bool ok, const char *message) {
  if (!ok) { llvm::errs() << message << "\n"; std::exit(1); }
}

static std::unique_ptr<llvm::Module> convert(llvm::StringRef ir, llvm::LLVMContext &context) {
  llvm::SMDiagnostic diagnostic;
  auto input = llvm::parseAssemblyString(ir, diagnostic, context);
  if (!input) diagnostic.print("air_attributes", llvm::errs());
  require(input && !llvm::verifyModule(*input, &llvm::errs()), "invalid test IR");
  std::string original;
  llvm::raw_string_ostream originalStream(original);
  input->print(originalStream, nullptr);
  llvm::SmallVector<char, 0> bytes;
  if (auto error = mullion::air::writeTypedBitcode(*input, bytes)) {
    llvm::errs() << llvm::toString(std::move(error)) << "\n";
    std::exit(1);
  }
  std::string after;
  llvm::raw_string_ostream afterStream(after);
  input->print(afterStream, nullptr);
  require(original == after, "serialization changed the optimizer's module");
  auto output = llvm::parseBitcodeFile(llvm::MemoryBufferRef(llvm::StringRef(bytes.data(), bytes.size()), "AIR"), context);
  if (!output) { llvm::errs() << llvm::toString(output.takeError()) << "\n"; std::exit(1); }
  return std::move(*output);
}

int main() {
  llvm::LLVMContext context;
  // Cleaning one constant's dead users must not invalidate another expression
  // still queued for expansion/cleanup (the ptrtoint uses the same GEP).
  auto nested = convert(R"(
    @words = internal addrspace(3) global [2 x i32] zeroinitializer
    define i64 @nested_constants() {
      %value = load volatile i32, ptr addrspace(3) getelementptr ([2 x i32], ptr addrspace(3) @words, i64 0, i64 1)
      ret i64 ptrtoint (ptr addrspace(3) getelementptr ([2 x i32], ptr addrspace(3) @words, i64 0, i64 1) to i64)
    }
  )", context);
  require(!llvm::verifyModule(*nested, &llvm::errs()), "invalid nested constant round trip");
  auto values = convert(R"(
    define noundef range(i32 0, 128) i32 @f(i32 noundef range(i32 0, 128) %x) norecurse {
      ret i32 %x
    }
    define nofpclass(nan) float @fp(float nofpclass(nan) %x) { ret float %x }
  )", context);
  require(values->getFunction("f")->hasFnAttribute(llvm::Attribute::NoRecurse) &&
          values->getFunction("f")->hasRetAttribute(llvm::Attribute::NoUndef) &&
          !values->getFunction("f")->hasRetAttribute(llvm::Attribute::Range), "integer promise conversion failed");
  require(values->getFunction("fp")->getAttributes().isEmpty(), "float promises survived");
  auto captures = convert(R"(
    define void @f(ptr captures(address) %p) { ret void }
    define void @none(ptr captures(none) %p) { ret void }
  )", context);
  require(captures->getFunction("f")->getAttributes().isEmpty(), "unrepresentable capture promise survived");
  require(captures->getFunction("none")->getAttributes().getParamAttr(0, llvm::Attribute::Captures).getCaptureInfo() ==
              llvm::CaptureInfo::none(), "nocapture translation lost");
  auto memory = convert(R"(
    define void @f() memory(write) { ret void }
    define void @none() memory(none) { ret void }
    define void @read() memory(read) { ret void }
    define void @arg(ptr %p) memory(argmem: readwrite) { ret void }
    define void @restricted() convergent noduplicate { ret void }
  )", context);
  require(memory->getFunction("f")->getAttributes().isEmpty(), "empty memory group survived");
  require(memory->getFunction("none")->getMemoryEffects().doesNotAccessMemory(), "readnone translation lost");
  require(memory->getFunction("read")->getMemoryEffects().onlyReadsMemory(), "readonly translation lost");
  require(memory->getFunction("arg")->getMemoryEffects().onlyAccessesArgPointees(), "argmem translation lost");
  require(memory->getFunction("restricted")->hasFnAttribute(llvm::Attribute::Convergent) &&
          memory->getFunction("restricted")->hasFnAttribute(llvm::Attribute::NoDuplicate), "execution restrictions lost");

  for (auto [vertices, primitives, topology, index, vertex, count, valid] : {
           std::tuple{256, 512, "air.triangle", 1535, 255, 512, true},
           std::tuple{4, 2, "air.triangle", 5, 3, 2, true},
           std::tuple{257, 2, "air.triangle", 0, 0, 0, false},
           std::tuple{4, 513, "air.triangle", 0, 0, 0, false},
           std::tuple{4, 2, "unknown", 0, 0, 0, false},
           std::tuple{4, 2, "air.triangle", 6, 0, 0, false},
           std::tuple{4, 2, "air.triangle", 0, 4, 0, false},
           std::tuple{4, 2, "air.triangle", 0, 0, 3, false},
           std::tuple{4, 2, "air.line", 4, 0, 0, false},
           std::tuple{4, 2, "air.point", 2, 0, 0, false}}) {
    std::string ir =
        "declare void @air.set_index_mesh(ptr addrspace(7), i32, i8)\n"
        "declare void @air.set_primitive_count_mesh(ptr addrspace(7), i32)\n"
        "define void @mesh(ptr addrspace(7) %mesh) {\n"
        "call void @air.set_index_mesh(ptr addrspace(7) %mesh, i32 " + std::to_string(index) + ", i8 " + std::to_string(vertex) + ")\n"
        "call void @air.set_primitive_count_mesh(ptr addrspace(7) %mesh, i32 " + std::to_string(count) + ")\nret void\n}\n"
        "!air.mesh = !{!0}\n!0 = !{ptr @mesh, !2, !3}\n"
        "!1 = !{!\"air.mesh_type_info\", !2, !2, i32 " + std::to_string(vertices) + ", i32 " +
        std::to_string(primitives) + ", !\"" + topology + "\"}\n!2 = !{}\n!3 = !{!4}\n!4 = !{i32 0, !\"air.mesh\", !1, !\"air.arg_type_name\", !\"mesh<>\"}\n";
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(ir, diagnostic, context);
    require(module && !llvm::verifyModule(*module, &llvm::errs()), "invalid mesh verifier fixture");
    llvm::SmallVector<char, 0> bytes;
    auto error = mullion::air::writeTypedBitcode(*module, bytes);
    require(bool(error) != valid, "mesh verifier accepted an invalid output or rejected a valid one");
    if (error) {
      require(llvm::toString(std::move(error)).find("AIR mesh") != std::string::npos, "missing mesh diagnostic");
      require(bytes.empty(), "invalid mesh wrote partial bitcode");
    }
  }

  for (auto tuple : {"!{}", "!{i32 2}", "!{i32 2, !\"nine\", i32 0}", "!{i32 2, null, i32 0}",
                     "!{i32 2, i32 65536, i32 0}", "!{i32 3, i32 9, i32 0}"}) {
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(std::string("!air.version = !{!0}\n!0 = ") + tuple, diagnostic, context);
    require(module && !llvm::verifyModule(*module, &llvm::errs()), "expected LLVM-valid AIR metadata fixture");
    llvm::SmallVector<char, 0> bytes;
    llvm::raw_svector_ostream output(bytes);
    auto error = mullion::metallib::write(*module, output);
    require(bool(error), "invalid AIR version accepted");
    require(llvm::toString(std::move(error)).find("air.version") != std::string::npos, "missing version diagnostic");
    require(bytes.empty(), "invalid version wrote a partial metallib");
  }
  for (auto ir : {"define void @f() null_pointer_is_valid { ret void }",
                  "define void @f(ptr byval(i32) %p) { ret void }",
                  "define void @f(ptr initializes((0, 4)) %p) { store i32 0, ptr %p ret void }"}) {
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(ir, diagnostic, context);
    if (!module) diagnostic.print("air_attributes", llvm::errs());
    require(module && !llvm::verifyModule(*module, &llvm::errs()), "invalid rejection fixture");
    llvm::SmallVector<char, 0> bytes;
    llvm::raw_svector_ostream output(bytes);
    auto error = mullion::metallib::write(*module, output);
    require(bool(error), "unsupported semantic attribute was silently accepted");
    auto message = llvm::toString(std::move(error));
    require(message.find("AIR cannot encode attribute") != std::string::npos, "missing attribute diagnostic");
    require(bytes.empty(), "failure wrote a partial metallib");
  }
  {
    llvm::SMDiagnostic diagnostic;
    auto invalid = llvm::parseAssemblyString("define i32 @broken() { ret i32 1 }", diagnostic, context);
    require(bool(invalid), "invalid verifier fixture");
    invalid->getFunction("broken")->getEntryBlock().getTerminator()->setOperand(
        0, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1));
    llvm::SmallVector<char, 0> bytes;
    auto error = mullion::air::writeTypedBitcode(*invalid, bytes);
    require(bool(error), "invalid return type reached AIR serialization");
    auto message = llvm::toString(std::move(error));
    require(message.find("Invalid input AIR IR") != std::string::npos, "missing verifier diagnostic");
    require(bytes.empty(), "invalid IR wrote partial bitcode");
  }
  llvm::outs() << "PASS: AIR attribute round trips and unsupported-attribute errors\n";
}
