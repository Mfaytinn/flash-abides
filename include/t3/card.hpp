// The one value the engine reads from a unit card: [scoring.params]
// requires_message_ledger.
//
// The official runner mounts a unit's whole input folder, so card.toml sits
// next to scenario.json (organizers, track3-simulation-public issues 31 and 35,
// 2026-10-05 and 2026-10-09). Their rule for a single-scenario unit: writing
// message_trace.parquet is always safe; leaving it out is safe only when that
// unit's card sets requires_message_ledger = false. A required ledger that is
// missing scores the unit 0, so this is not a TOML parser: it recognises the
// one spelling the card generators write and answers Unknown for every text it
// does not fully account for. On Unknown the engine writes the ledger.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace t3 {

enum class LedgerFlag { Unknown, Required, NotRequired };

// A generated card is 2 KB. A file larger than this is not read.
constexpr size_t kCardMaxBytes = size_t{64} << 10;

namespace card_detail {

constexpr std::string_view kKey = "requires_message_ledger";
constexpr std::string_view kTable = "scoring.params";

inline std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.remove_suffix(1);
  return s;
}

// `rest` with one trailing "# comment" removed; empty optional-like flag when
// anything else follows.
inline bool only_comment_follows(std::string_view rest) {
  rest = trim(rest);
  return rest.empty() || rest.front() == '#';
}

} // namespace card_detail

// The flag in a card's text. NotRequired or Required only when the text holds
// exactly one mention of the key, as a `requires_message_ledger = false|true`
// line (bare lowercase boolean, optional spaces and trailing comment) under a
// `[scoring.params]` header. Unknown for a missing key, for any other mention
// of it outside a comment line (another table, a dotted or quoted key, an
// inline table, a repeat), and for a text this line-by-line reader cannot vouch
// for:
//  - a multi-line string, inside which a line can look like a header or a key
//  without being one;
//  - a brace outside a comment line: an inline table, which newer TOML lets
//  span lines;
//  - a byte order mark, a control byte other than tab and line feed, or a
//  carriage return not
//    followed by a line feed: the scorer's own TOML reader refuses these, so it
//    could not have read false from such a card.
inline LedgerFlag card_ledger_flag(std::string_view toml) {
  using namespace card_detail;
  if (toml.substr(0, 3) == "\xEF\xBB\xBF")
    return LedgerFlag::Unknown;
  if (toml.find("\"\"\"") != std::string_view::npos ||
      toml.find("'''") != std::string_view::npos)
    return LedgerFlag::Unknown;
  for (size_t i = 0; i < toml.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(toml[i]);
    if (c >= 0x20 || c == '\t' || c == '\n')
      continue;
    if (c == '\r' && i + 1 < toml.size() && toml[i + 1] == '\n')
      continue;
    return LedgerFlag::Unknown;
  }
  LedgerFlag found = LedgerFlag::Unknown;
  bool in_params = false, seen = false;
  while (!toml.empty()) {
    const size_t nl = toml.find('\n');
    const std::string_view line = trim(toml.substr(0, nl));
    toml.remove_prefix(nl == std::string_view::npos ? toml.size() : nl + 1);
    if (line.empty() || line.front() == '#')
      continue;
    if (line.find_first_of("{}") != std::string_view::npos)
      return LedgerFlag::Unknown;
    if (line.front() == '[') {
      // A header: `[scoring.params]`, spaces inside the brackets and a trailing
      // comment allowed.
      const size_t close = line.find(']');
      in_params = close != std::string_view::npos &&
                  trim(line.substr(1, close - 1)) == kTable &&
                  only_comment_follows(line.substr(close + 1));
      if (line.find(kKey) != std::string_view::npos)
        return LedgerFlag::Unknown;
      continue;
    }
    if (line.find(kKey) == std::string_view::npos)
      continue;
    // The key is mentioned: it must be the whole key of this line, in the right
    // table, once.
    if (seen || !in_params || line.substr(0, kKey.size()) != kKey)
      return LedgerFlag::Unknown;
    std::string_view rest = trim(line.substr(kKey.size()));
    if (rest.empty() || rest.front() != '=')
      return LedgerFlag::Unknown;
    rest = trim(rest.substr(1));
    LedgerFlag value;
    if (rest.substr(0, 5) == "false") {
      value = LedgerFlag::NotRequired;
      rest.remove_prefix(5);
    } else if (rest.substr(0, 4) == "true") {
      value = LedgerFlag::Required;
      rest.remove_prefix(4);
    } else {
      return LedgerFlag::Unknown;
    }
    // "falsey" or "false true": the character after the boolean must be a space
    // or a comment.
    if (!rest.empty() && rest.front() != ' ' && rest.front() != '\t' &&
        rest.front() != '#')
      return LedgerFlag::Unknown;
    if (!only_comment_follows(rest))
      return LedgerFlag::Unknown;
    seen = true;
    found = value;
  }
  return found;
}

// The flag of the card.toml in the directory of `scenario_path`. Unknown when
// there is no such file, it is not a regular file (opened without blocking, so
// a pipe cannot stall the run), it cannot be read whole, or it is larger than
// kCardMaxBytes. A symbolic link is followed.
inline LedgerFlag
read_card_ledger_flag(const std::string &scenario_path) noexcept {
  try {
    const size_t slash = scenario_path.rfind('/');
    const std::string path =
        (slash == std::string::npos ? std::string()
                                    : scenario_path.substr(0, slash + 1)) +
        "card.toml";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
      return LedgerFlag::Unknown;
    struct stat st{};
    char buf[kCardMaxBytes + 1];
    size_t n = 0;
    bool whole = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                 static_cast<size_t>(st.st_size) <= kCardMaxBytes;
    while (whole && n < sizeof buf) {
      const ssize_t r = ::read(fd, buf + n, sizeof buf - n);
      if (r == 0)
        break;
      if (r < 0)
        whole = false;
      else
        n += static_cast<size_t>(r);
    }
    ::close(fd);
    if (!whole || n > kCardMaxBytes)
      return LedgerFlag::Unknown;
    return card_ledger_flag(std::string_view(buf, n));
  } catch (...) {
    return LedgerFlag::Unknown;
  }
}

} // namespace t3
