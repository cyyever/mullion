// Minimal declarations keep this rule test independent of the LLVM and Metal SDKs.
namespace llvm {
struct Error {
  ~Error();
  explicit operator bool() const;
};
}
namespace mullion::metallib {
llvm::Error write();
}
void consume(llvm::Error);
void ignored() {
  mullion::metallib::write(); // EXPECT: custom-air-write-error
  (void)mullion::metallib::write(); // EXPECT: custom-air-write-error
  static_cast<void>(mullion::metallib::write()); // EXPECT: custom-air-write-error
}
void handled() {
  if (auto error = mullion::metallib::write()) consume(static_cast<llvm::Error &&>(error));
  consume(mullion::metallib::write());
}
llvm::Error returned() {
  return mullion::metallib::write();
}
namespace other { llvm::Error write(); }
void unrelated() { other::write(); }

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
