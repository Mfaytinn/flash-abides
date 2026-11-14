#include "t3/json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace t3::json {
namespace {

struct Parser {
  std::string_view s;
  size_t p = 0;

  [[noreturn]] void fail(const char *what) const {
    throw std::runtime_error(std::string("json: ") + what + " at offset " +
                             std::to_string(p));
  }
  void ws() {
    while (p < s.size() &&
           (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t'))
      ++p;
  }
  bool eat(char c) {
    ws();
    if (p < s.size() && s[p] == c) {
      ++p;
      return true;
    }
    return false;
  }
  void expect(char c) {
    if (!eat(c))
      fail("unexpected character");
  }

  static void put_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  uint32_t hex4() {
    if (p + 4 > s.size())
      fail("short \\u escape");
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s[p++];
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v |= static_cast<uint32_t>(c - 'A' + 10);
      else
        fail("bad \\u escape");
    }
    return v;
  }

  std::string string_body() {
    // Opening quote already consumed.
    std::string out;
    while (true) {
      if (p >= s.size())
        fail("unterminated string");
      const char c = s[p++];
      if (c == '"')
        break;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (p >= s.size())
        fail("bad escape");
      const char e = s[p++];
      switch (e) {
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case '/':
        out.push_back('/');
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        uint32_t cp = hex4();
        if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= s.size() && s[p] == '\\' &&
            s[p + 1] == 'u') {
          p += 2;
          const uint32_t lo = hex4();
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        put_utf8(out, cp);
        break;
      }
      default:
        fail("bad escape");
      }
    }
    return out;
  }

  Value number() {
    const size_t start = p;
    bool integral = true;
    if (s[p] == '-')
      ++p;
    while (p < s.size()) {
      const char c = s[p];
      if (c >= '0' && c <= '9') {
        ++p;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        integral = false;
        ++p;
      } else {
        break;
      }
    }
    std::string lit(s.substr(start, p - start));
    Value v;
    v.kind = Value::Kind::Number;
    char *end = nullptr;
    v.num = std::strtod(lit.c_str(), &end);
    if (end != lit.c_str() + lit.size())
      fail("bad number");
    if (integral) {
      errno = 0;
      const long long iv = std::strtoll(lit.c_str(), &end, 10);
      if (errno == 0 && end == lit.c_str() + lit.size()) {
        v.is_int = true;
        v.i = iv;
      }
    }
    return v;
  }

  Value value() {
    ws();
    if (p >= s.size())
      fail("unexpected end");
    const char c = s[p];
    Value v;
    if (c == '{') {
      ++p;
      v.kind = Value::Kind::Object;
      if (eat('}'))
        return v;
      do {
        ws();
        if (p >= s.size() || s[p] != '"')
          fail("expected key");
        ++p;
        std::string key = string_body();
        expect(':');
        Value item = value();
        // Python's json keeps the LAST duplicate key.
        bool replaced = false;
        for (auto &kv : v.obj) {
          if (kv.first == key) {
            kv.second = std::move(item);
            replaced = true;
            break;
          }
        }
        if (!replaced)
          v.obj.emplace_back(std::move(key), std::move(item));
      } while (eat(','));
      expect('}');
    } else if (c == '[') {
      ++p;
      v.kind = Value::Kind::Array;
      if (eat(']'))
        return v;
      do {
        v.arr.push_back(value());
      } while (eat(','));
      expect(']');
    } else if (c == '"') {
      ++p;
      v.kind = Value::Kind::String;
      v.str = string_body();
    } else if (s.compare(p, 4, "true") == 0) {
      p += 4;
      v.kind = Value::Kind::Bool;
      v.b = true;
    } else if (s.compare(p, 5, "false") == 0) {
      p += 5;
      v.kind = Value::Kind::Bool;
    } else if (s.compare(p, 4, "null") == 0) {
      p += 4;
    } else if (s.compare(p, 8, "Infinity") == 0) {
      p += 8;
      v.kind = Value::Kind::Number;
      v.num = HUGE_VAL;
    } else if (s.compare(p, 9, "-Infinity") == 0) {
      p += 9;
      v.kind = Value::Kind::Number;
      v.num = -HUGE_VAL;
    } else if (s.compare(p, 3, "NaN") == 0) {
      p += 3;
      v.kind = Value::Kind::Number;
      v.num = std::nan("");
    } else if (c == '-' || (c >= '0' && c <= '9')) {
      v = number();
    } else {
      fail("unexpected token");
    }
    return v;
  }
};

} // namespace

Value parse(std::string_view text) {
  Parser ps{text};
  Value v = ps.value();
  ps.ws();
  if (ps.p != text.size())
    ps.fail("trailing data");
  return v;
}

} // namespace t3::json
