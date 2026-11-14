// Minimal JSON DOM for scenario files: objects keep insertion order, numbers
// keep both their double value and whether the literal was an integer (Python's
// json module makes the same distinction).
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace t3::json {

struct Value {
  // Define special members after Value is complete: the recursive pair in obj
  // needs this with Clang-based compilers.
  Value();
  ~Value() noexcept;
  Value(const Value &);
  Value(Value &&) noexcept;
  Value &operator=(const Value &);
  Value &operator=(Value &&) noexcept;

  enum class Kind { Null, Bool, Number, String, Array, Object };
  Kind kind = Kind::Null;
  bool b = false;
  double num = 0.0;
  bool is_int = false; // literal had no fraction/exponent
  int64_t i = 0;       // exact value when is_int (and it fits)
  std::string str;
  std::vector<Value> arr;
  std::vector<std::pair<std::string, Value>> obj;

  const Value *get(std::string_view key) const;
  bool is_number() const { return kind == Kind::Number; }
  // Python truthiness.
  bool truthy() const;
};

inline Value::Value() = default;
inline Value::~Value() noexcept = default;
inline Value::Value(const Value &) = default;
inline Value::Value(Value &&) noexcept = default;
inline Value &Value::operator=(const Value &) = default;
inline Value &Value::operator=(Value &&) noexcept = default;

inline const Value *Value::get(std::string_view key) const {
  if (kind != Kind::Object)
    return nullptr;
  for (const auto &[k, v] : obj)
    if (k == key)
      return &v;
  return nullptr;
}

inline bool Value::truthy() const {
  switch (kind) {
  case Kind::Null:
    return false;
  case Kind::Bool:
    return b;
  case Kind::Number:
    return num != 0.0;
  case Kind::String:
    return !str.empty();
  case Kind::Array:
    return !arr.empty();
  case Kind::Object:
    return !obj.empty();
  }
  return false;
}

Value parse(std::string_view text);

} // namespace t3::json
