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
  writer.Write(); // EXPECT: custom-air-write-error
  (void)writer.Write(); // EXPECT: custom-air-write-error
  static_cast<void>(writer.Write()); // EXPECT: custom-air-write-error
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

struct Record {
  Record(const Record &);
  Record(Record &&);
  void read() const;
};
void inspect(const Record (&items)[4]) {
  for (const auto item : items) // EXPECT: performance-for-range-copy
    item.read();
  for (const auto &item : items)
    item.read();
}
#include <utility>
void take(Record);
void moved(Record item) {
  take(std::move(item));
  item.read(); // EXPECT: bugprone-use-after-move
}
void redundant() {
  return; // EXPECT: readability-redundant-control-flow
}

#include <algorithm>
#include <vector>
void sort_values(std::vector<int> &values) {
  std::sort(values.begin(), values.end()); // EXPECT: modernize-use-ranges
  std::ranges::sort(values);
}
std::vector<int> append_without_reserve(const std::vector<int> &input) {
  std::vector<int> output;
  for (auto value : input)
    output.push_back(value); // EXPECT: performance-inefficient-vector-operation
  return output;
}
std::vector<int> append_with_reserve(const std::vector<int> &input) {
  std::vector<int> output;
  output.reserve(input.size());
  for (auto value : input)
    output.push_back(value);
  return output;
}
