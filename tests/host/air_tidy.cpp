// Minimal declarations keep this rule test independent of the LLVM and Metal SDKs.
namespace llvm {
struct Error {
  ~Error();
  explicit operator bool() const;
};
}
namespace mullion::metallib {
struct MetallibWriter { llvm::Error Write(); };
}
void consume(llvm::Error);
void ignored(mullion::metallib::MetallibWriter &writer) {
  writer.Write(); // EXPECT_WARNING
}
void handled(mullion::metallib::MetallibWriter &writer) {
  if (auto error = writer.Write()) consume(static_cast<llvm::Error &&>(error));
  consume(writer.Write());
}
llvm::Error returned(mullion::metallib::MetallibWriter &writer) {
  return writer.Write();
}
struct OtherWriter { llvm::Error Write(); };
void unrelated(OtherWriter &writer) { writer.Write(); }
