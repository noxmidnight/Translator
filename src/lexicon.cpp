#include "lexicon.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace {

// Decode one UTF-8 codepoint; advances i. Returns 0 on error.
uint32_t next_cp(const std::string& s, size_t& i) {
  if (i >= s.size()) return 0;
  const unsigned char c0 = static_cast<unsigned char>(s[i]);
  if (c0 < 0x80) {
    ++i;
    return c0;
  }
  if ((c0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
    const uint32_t cp = ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    i += 2;
    return cp;
  }
  if ((c0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
    const uint32_t cp = ((c0 & 0x0F) << 12) |
                        ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                        (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    i += 3;
    return cp;
  }
  if ((c0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
    const uint32_t cp = ((c0 & 0x07) << 18) |
                        ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                        ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                        (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    i += 4;
    return cp;
  }
  ++i;
  return 0;
}

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool is_tashkeel(uint32_t cp) {
  return (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 ||
         (cp >= 0x06D6 && cp <= 0x06ED);
}

bool is_arabic_punct(uint32_t cp) {
  // Arabic comma, semicolon, question mark, full stop, percent marks, etc.
  return cp == 0x060C || cp == 0x061B || cp == 0x061F || cp == 0x06D4 ||
         cp == 0x066A || cp == 0x066B || cp == 0x066C || cp == 0x066D ||
         cp == 0x06DD || cp == 0x06DE || cp == 0x06E9;
}

bool is_arabic_digit(uint32_t cp) {
  return (cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9);
}

// Arabic *letters* only — exclude punctuation / digits / tashkeel inside 0600–06FF.
bool is_arabic_letter(uint32_t cp) {
  if (is_tashkeel(cp) || is_arabic_punct(cp) || is_arabic_digit(cp)) return false;
  if (cp == 0x0640) return false;  // tatweel
  return (cp >= 0x0621 && cp <= 0x063A) || (cp >= 0x0641 && cp <= 0x064A) ||
         (cp >= 0x066E && cp <= 0x066F) || (cp >= 0x0671 && cp <= 0x06D3) ||
         cp == 0x06D5 || (cp >= 0x06EE && cp <= 0x06EF) ||
         (cp >= 0x06FA && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
         (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
         (cp >= 0xFE70 && cp <= 0xFEFF);
}

bool is_word_char(uint32_t cp) {
  if (is_arabic_letter(cp) || is_arabic_digit(cp)) return true;
  if (cp < 128 && (std::isalnum(static_cast<unsigned char>(cp)) || cp == '\'' ||
                   cp == '-'))
    return true;
  return false;
}

std::string to_lower_ascii(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool is_ascii_alnum_token(const std::string& s) {
  if (s.empty()) return false;
  for (unsigned char c : s) {
    if (!std::isalnum(c)) return false;
  }
  return true;
}

// UTF-8 string for Arabic definite article and common clitics.
const char* kAl = "\xd8\xa7\xd9\x84";  // ال
const char* kWaw = "\xd9\x88";         // و
const char* kBa = "\xd8\xa8";          // ب
const char* kFa = "\xd9\x81";          // ف
const char* kKaf = "\xd9\x83";         // ك
const char* kLam = "\xd9\x84";         // ل

bool starts_with_utf8(const std::string& s, const char* prefix) {
  const size_t n = std::char_traits<char>::length(prefix);
  return s.size() >= n && s.compare(0, n, prefix, n) == 0;
}

bool is_en_stop(const std::string& tok) {
  static const std::unordered_set<std::string> stops = {
      "the", "a",   "an",  "of",  "to", "in", "on", "for", "and", "or",
      "is",  "are", "was", "were", "be", "by", "at", "as",  "it",  "i"};
  return stops.count(to_lower_ascii(tok)) > 0;
}

struct PhraseHint {
  const char* en;
  const char* ar;
};

const PhraseHint kPhraseHints[] = {
    {"jumps over", "\xd9\x8a\xd9\x82\xd9\x81\xd8\xb2 \xd9\x81\xd9\x88\xd9\x82"},  // يقفز فوق
    {"jump over", "\xd9\x8a\xd9\x82\xd9\x81\xd8\xb2 \xd9\x81\xd9\x88\xd9\x82"},
    {"good morning",
     "\xd8\xb5\xd8\xa8\xd8\xa7\xd8\xad \xd8\xa7\xd9\x84\xd8\xae\xd9\x8a\xd8\xb1"},  // صباح الخير
    {"break a leg",
     "\xd8\xa8\xd8\xa7\xd9\x84\xd8\xaa\xd9\x88\xd9\x81\xd9\x8a\xd9\x82"},  // بالتوفيق
    {"how are you",
     "\xd9\x83\xd9\x8a\xd9\x81 \xd8\xad\xd8\xa7\xd9\x84\xd9\x83"},  // كيف حالك
};

}  // namespace

std::string Lexicon::normalize_arabic(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const uint32_t cp = next_cp(text, i);
    if (cp == 0) continue;
    if (is_tashkeel(cp) || cp == 0x0640) continue;  // tatweel
    uint32_t n = cp;
    // Alef variants → ا
    if (n == 0x0622 || n == 0x0623 || n == 0x0625 || n == 0x0671) n = 0x0627;
    // ى → ي
    if (n == 0x0649) n = 0x064A;
    // ة → ه (lookup fallback key)
    if (n == 0x0629) n = 0x0647;
    append_utf8(out, n);
  }
  return out;
}

std::vector<std::string> Lexicon::tokenize(const std::string& text) {
  std::vector<std::string> tokens;
  std::string cur;
  size_t i = 0;
  while (i < text.size()) {
    const size_t start = i;
    const uint32_t cp = next_cp(text, i);
    if (cp == 0) continue;
    if (is_word_char(cp)) {
      cur.append(text, start, i - start);
    } else {
      if (!cur.empty()) {
        tokens.push_back(cur);
        cur.clear();
      }
    }
  }
  if (!cur.empty()) tokens.push_back(cur);
  return tokens;
}

bool Lexicon::load(const std::string& path) {
  map_.clear();
  en_map_.clear();
  std::ifstream in(path);
  if (!in) return false;

  // MUSE AR–EN is ~31k lines; reserve to cut rehashing.
  map_.reserve(28000);
  en_map_.reserve(12000);

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;
    std::string ar = line.substr(0, tab);
    std::string en = line.substr(tab + 1);
    while (!en.empty() && (en.back() == '\r' || en.back() == ' ')) en.pop_back();
    while (!en.empty() && en.front() == ' ') en.erase(en.begin());
    if (ar.empty() || en.empty()) continue;

    const std::string key = normalize_arabic(ar);
    if (key.empty()) continue;
    auto& glosses = map_[key];
    bool dup = false;
    for (const auto& g : glosses) {
      if (g == en) {
        dup = true;
        break;
      }
    }
    if (!dup && glosses.size() < 8) glosses.push_back(en);

    const std::string en_key = to_lower_ascii(en);
    if (en_key.find(' ') == std::string::npos && is_ascii_alnum_token(en_key)) {
      auto& rev = en_map_[en_key];
      bool rdup = false;
      for (const auto& a : rev) {
        if (a == ar) {
          rdup = true;
          break;
        }
      }
      if (!rdup && rev.size() < 8) rev.push_back(ar);
    }
  }
  return !map_.empty();
}

std::vector<std::string> Lexicon::lookup_raw(
    const std::unordered_map<std::string, std::vector<std::string>>& m,
    const std::string& key, size_t limit) const {
  const auto it = m.find(key);
  if (it == m.end()) return {};
  std::vector<std::string> out;
  for (size_t i = 0; i < it->second.size() && i < limit; ++i) {
    out.push_back(it->second[i]);
  }
  return out;
}

std::vector<std::string> Lexicon::lookup(const std::string& token,
                                        size_t limit) const {
  if (token.empty()) return {};

  std::string key = normalize_arabic(token);
  auto hit = lookup_raw(map_, key, limit);
  if (!hit.empty()) return hit;

  hit = lookup_raw(map_, to_lower_ascii(token), limit);
  if (!hit.empty()) return hit;

  static const char* prefixes[] = {kAl, kWaw, kBa, kFa, kKaf, kLam};
  for (const char* p : prefixes) {
    if (starts_with_utf8(key, p)) {
      const size_t n = std::char_traits<char>::length(p);
      if (key.size() > n) {
        hit = lookup_raw(map_, key.substr(n), limit);
        if (!hit.empty()) return hit;
      }
    }
  }
  for (const char* p : {kWaw, kBa, kFa, kKaf, kLam}) {
    if (starts_with_utf8(key, p)) {
      const size_t n = std::char_traits<char>::length(p);
      std::string rest = key.substr(n);
      if (starts_with_utf8(rest, kAl) && rest.size() > 4) {
        hit = lookup_raw(map_, rest.substr(4), limit);
        if (!hit.empty()) return hit;
      }
    }
  }
  return {};
}

std::vector<std::string> Lexicon::lookup_en(const std::string& token,
                                           size_t limit) const {
  if (token.empty()) return {};
  const std::string key = to_lower_ascii(token);
  // Exact key only — stemming often yields the wrong part of speech.
  auto hit = lookup_raw(en_map_, key, 8);
  if (hit.empty()) return {};
  const std::string al = "\xd8\xa7\xd9\x84";  // ال
  std::stable_sort(hit.begin(), hit.end(),
                   [&al](const std::string& a, const std::string& b) {
                     const bool a_al = a.size() >= 4 && a.compare(0, 4, al) == 0;
                     const bool b_al = b.size() >= 4 && b.compare(0, 4, al) == 0;
                     if (a_al != b_al) return a_al;
                     return a.size() > b.size();
                   });
  if (hit.size() > limit) hit.resize(limit);
  return hit;
}

std::string Lexicon::build_rag_context(const std::string& text,
                                       size_t max_entries) const {
  if (map_.empty() && en_map_.empty()) return {};
  const auto tokens = tokenize(text);
  std::ostringstream oss;
  std::unordered_set<std::string> seen;
  size_t count = 0;
  const std::string lower = to_lower_ascii(text);
  for (const auto& ph : kPhraseHints) {
    if (count >= max_entries) break;
    if (lower.find(ph.en) != std::string::npos && seen.insert(ph.en).second) {
      oss << ph.en << " → " << ph.ar << '\n';
      ++count;
    }
  }
  for (const auto& tok : tokens) {
    if (count >= max_entries) break;
    auto glosses = lookup(tok, 2);
    bool from_en = false;
    if (glosses.empty()) {
      if (is_en_stop(tok)) continue;
      glosses = lookup_en(tok, 2);
      from_en = true;
    }
    if (glosses.empty()) continue;
    const std::string norm = from_en ? to_lower_ascii(tok) : normalize_arabic(tok);
    if (!seen.insert(norm).second) continue;
    oss << tok << " → ";
    for (size_t i = 0; i < glosses.size(); ++i) {
      if (i) oss << " / ";
      oss << glosses[i];
    }
    oss << '\n';
    ++count;
  }
  if (count == 0) return {};
  return "Dictionary hints (prefer these when they fit context; "
         "in word-by-word mode use them ONLY for the meaning/gloss column, "
         "never as transliteration):\n" +
         oss.str();
}
