#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class Lexicon {
 public:
  bool load(const std::string& path);
  bool empty() const { return map_.empty(); }
  size_t size() const { return map_.size(); }

  // Up to `limit` English glosses for an Arabic token.
  std::vector<std::string> lookup(const std::string& token, size_t limit = 5) const;
  // Arabic forms for a single-token English gloss (reverse MUSE index).
  std::vector<std::string> lookup_en(const std::string& token, size_t limit = 5) const;

  // Build a RAG hint block for prompt injection (empty if no hits).
  std::string build_rag_context(const std::string& text, size_t max_entries = 40) const;

  static std::string normalize_arabic(const std::string& text);
  static std::vector<std::string> tokenize(const std::string& text);

 private:
  std::unordered_map<std::string, std::vector<std::string>> map_;
  std::unordered_map<std::string, std::vector<std::string>> en_map_;
  std::vector<std::string> lookup_raw(
      const std::unordered_map<std::string, std::vector<std::string>>& m,
      const std::string& key, size_t limit) const;
};
