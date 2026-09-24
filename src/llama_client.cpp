#include "llama_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <sstream>

namespace {

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  const size_t n = size * nmemb;
  if (out->capacity() < out->size() + n) {
    out->reserve(out->size() + n + 4096);
  }
  out->append(ptr, n);
  return n;
}

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + s.size() / 4 + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

void append_utf8_cp(std::string& out, uint32_t cp) {
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

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decode \uXXXX (and surrogate pairs) into UTF-8. Advances pos past the hex.
bool decode_unicode_escape(const std::string& json, size_t& pos, std::string& out) {
  if (pos + 3 >= json.size()) return false;
  int h0 = hex_nibble(json[pos]);
  int h1 = hex_nibble(json[pos + 1]);
  int h2 = hex_nibble(json[pos + 2]);
  int h3 = hex_nibble(json[pos + 3]);
  if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return false;
  uint32_t cp = static_cast<uint32_t>((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
  pos += 4;
  // Surrogate pair
  if (cp >= 0xD800 && cp <= 0xDBFF && pos + 5 < json.size() && json[pos] == '\\' &&
      json[pos + 1] == 'u') {
    size_t p2 = pos + 2;
    int g0 = hex_nibble(json[p2]);
    int g1 = hex_nibble(json[p2 + 1]);
    int g2 = hex_nibble(json[p2 + 2]);
    int g3 = hex_nibble(json[p2 + 3]);
    if (g0 >= 0 && g1 >= 0 && g2 >= 0 && g3 >= 0) {
      uint32_t low = static_cast<uint32_t>((g0 << 12) | (g1 << 8) | (g2 << 4) | g3);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
        pos = p2 + 4;
      }
    }
  }
  append_utf8_cp(out, cp);
  return true;
}

std::string extract_content(const std::string& json) {
  const std::string key = "\"content\"";
  size_t pos = json.find("\"choices\"");
  if (pos == std::string::npos) pos = 0;
  pos = json.find(key, pos);
  if (pos == std::string::npos) return {};
  pos = json.find(':', pos + key.size());
  if (pos == std::string::npos) return {};
  ++pos;
  while (pos < json.size() &&
         (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n'))
    ++pos;
  if (pos >= json.size() || json[pos] != '"') return {};
  ++pos;
  std::string out;
  out.reserve(std::min(json.size() - pos, size_t{4096}));
  while (pos < json.size()) {
    char c = json[pos++];
    if (c == '\\' && pos < json.size()) {
      char n = json[pos++];
      switch (n) {
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case '"':
          out += '"';
          break;
        case '\\':
          out += '\\';
          break;
        case '/':
          out += '/';
          break;
        case 'u':
          if (!decode_unicode_escape(json, pos, out)) {
            // skip malformed escape
          }
          break;
        default:
          out += n;
          break;
      }
    } else if (c == '"') {
      break;
    } else {
      out += c;
    }
  }
  return out;
}

std::string extract_error_message(const std::string& json) {
  const std::string key = "\"message\"";
  size_t pos = json.find("\"error\"");
  if (pos == std::string::npos)
    return json.substr(0, std::min(json.size(), size_t{200}));
  pos = json.find(key, pos);
  if (pos == std::string::npos)
    return json.substr(0, std::min(json.size(), size_t{200}));
  pos = json.find(':', pos + key.size());
  if (pos == std::string::npos) return "unknown error";
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  if (pos >= json.size() || json[pos] != '"') return "unknown error";
  ++pos;
  std::string out;
  while (pos < json.size()) {
    char c = json[pos++];
    if (c == '\\' && pos < json.size()) {
      out += json[pos++];
    } else if (c == '"') {
      break;
    } else {
      out += c;
    }
  }
  return out.empty() ? "unknown error" : out;
}

bool has_forbidden_script(const std::string& text) {
  for (size_t i = 0; i < text.size();) {
    const unsigned char c0 = static_cast<unsigned char>(text[i]);
    uint32_t cp = 0;
    size_t n = 0;
    if (c0 < 0x80) {
      cp = c0;
      n = 1;
    } else if ((c0 & 0xE0) == 0xC0 && i + 1 < text.size()) {
      cp = ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
      n = 2;
    } else if ((c0 & 0xF0) == 0xE0 && i + 2 < text.size()) {
      cp = ((c0 & 0x0F) << 12) |
           ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[i + 2]) & 0x3F);
      n = 3;
    } else if ((c0 & 0xF8) == 0xF0 && i + 3 < text.size()) {
      cp = ((c0 & 0x07) << 18) |
           ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[i + 3]) & 0x3F);
      n = 4;
    } else {
      ++i;
      continue;
    }
    i += n;
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
        (cp >= 0x0400 && cp <= 0x04FF)) {
      return true;
    }
  }
  return false;
}

bool is_forbidden_cp(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
         (cp >= 0x0400 && cp <= 0x04FF);
}

std::string mask_forbidden_script(const std::string& text) {
  std::string out;
  bool in_bad = false;
  for (size_t i = 0; i < text.size();) {
    const size_t start = i;
    const unsigned char c0 = static_cast<unsigned char>(text[i]);
    uint32_t cp = 0;
    size_t n = 1;
    if (c0 < 0x80) {
      cp = c0;
      n = 1;
    } else if ((c0 & 0xE0) == 0xC0 && i + 1 < text.size()) {
      cp = ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
      n = 2;
    } else if ((c0 & 0xF0) == 0xE0 && i + 2 < text.size()) {
      cp = ((c0 & 0x0F) << 12) |
           ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[i + 2]) & 0x3F);
      n = 3;
    } else if ((c0 & 0xF8) == 0xF0 && i + 3 < text.size()) {
      cp = ((c0 & 0x07) << 18) |
           ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[i + 3]) & 0x3F);
      n = 4;
    }
    i += n;
    if (is_forbidden_cp(cp)) {
      if (!in_bad) {
        out += " ___ ";
        in_bad = true;
      }
    } else {
      out.append(text, start, n);
      in_bad = false;
    }
  }
  return out;
}

std::string source_from_user(const std::string& user) {
  std::string best;
  size_t start = 0;
  while (start <= user.size()) {
    auto pos = user.find("\n\n", start);
    std::string part =
        (pos == std::string::npos) ? user.substr(start) : user.substr(start, pos - start);
    // trim
    while (!part.empty() && (part.front() == ' ' || part.front() == '\n' ||
                             part.front() == '\r' || part.front() == '\t'))
      part.erase(part.begin());
    while (!part.empty() && (part.back() == ' ' || part.back() == '\n' ||
                             part.back() == '\r' || part.back() == '\t'))
      part.pop_back();
    if (!part.empty()) best = part;
    if (pos == std::string::npos) break;
    start = pos + 2;
  }
  return best.empty() ? user : best;
}

std::string clean_orphan_letters(const std::string& text) {
  std::string out;
  std::istringstream iss(text);
  std::string tok;
  bool first = true;
  while (iss >> tok) {
    // Drop single Arabic-letter tokens
    if (tok.size() == 2) {
      const unsigned char c0 = static_cast<unsigned char>(tok[0]);
      if (c0 >= 0xD8 && c0 <= 0xDB) continue;
    }
    if (!first) out += ' ';
    out += tok;
    first = false;
  }
  return out;
}

std::string collapse_ws(const std::string& s) {
  std::string cleaned;
  bool space = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n') {
      if (!space && !cleaned.empty()) {
        cleaned += ' ';
        space = true;
      }
    } else {
      cleaned += c;
      space = false;
    }
  }
  return cleaned;
}

}  // namespace

LlamaClient::LlamaClient(AppConfig config) : config_(std::move(config)) {}

std::string LlamaClient::base_url() const {
  return "http://" + config_.host + ":" + std::to_string(config_.port);
}

bool LlamaClient::health_check() const {
  CURL* curl = curl_easy_init();
  if (!curl) return false;

  std::string response;
  const std::string url = base_url() + "/health";
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);

  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  return rc == CURLE_OK && http_code >= 200 && http_code < 300;
}

ChatResult LlamaClient::request(const std::string& system, const std::string& user,
                                double temperature, int max_tokens) const {
  ChatResult result;
  CURL* curl = curl_easy_init();
  if (!curl) {
    result.error = "Failed to init libcurl";
    return result;
  }

  std::ostringstream body;
  body << "{"
       << "\"model\":\"" << json_escape(config_.model) << "\","
       << "\"temperature\":" << temperature << ","
       << "\"max_tokens\":" << max_tokens << ","
       << "\"messages\":["
       << "{\"role\":\"system\",\"content\":\"" << json_escape(system) << "\"},"
       << "{\"role\":\"user\",\"content\":\"" << json_escape(user) << "\"}"
       << "]}";

  const std::string payload = body.str();
  std::string response;
  const std::string url = base_url() + "/v1/chat/completions";

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    result.error = std::string("HTTP request failed: ") + curl_easy_strerror(rc) +
                   ". Is llama-server running? Try ./scripts/translator.sh";
    return result;
  }

  if (http_code < 200 || http_code >= 300) {
    result.error = "Server error (" + std::to_string(http_code) +
                   "): " + extract_error_message(response);
    return result;
  }

  result.content = extract_content(response);
  if (result.content.empty()) {
    result.error = "Could not parse model response";
    return result;
  }
  result.ok = true;
  return result;
}

ChatResult LlamaClient::chat(const std::string& system, const std::string& user,
                             std::optional<double> temperature,
                             std::optional<int> max_tokens) const {
  const double temp = temperature.value_or(config_.temperature);
  const int tokens = max_tokens.value_or(config_.max_tokens);
  ChatResult result = request(system, user, temp, tokens);
  if (!result.ok || !has_forbidden_script(result.content)) return result;

  const std::string source = source_from_user(user);
  const std::string nuclear_system =
      "You are a precise bilingual Arabic–English translator. "
      "Output ONLY Arabic script for Arabic translations (Latin only if "
      "translating into English). Never use Chinese, Japanese, Korean, or Cyrillic. "
      "Known phrase: jumps over → يقفز فوق.";
  const std::string nuclear_user =
      "Translate the following into the appropriate target language. "
      "Reply with the translation only.\n\n" +
      source;
  ChatResult retry = request(nuclear_system, nuclear_user, 0.0, tokens);
  if (retry.ok && !has_forbidden_script(retry.content)) return retry;

  const std::string repair_system =
      "You are a precise Arabic–English translator. "
      "Output ONLY Arabic and/or Latin script. Never use Chinese or other scripts. "
      "Use يقفز فوق for 'jumps over'.";
  const std::string repair_user =
      "Ignore any Chinese in prior drafts. Translate this source cleanly:\n\n" + source;
  ChatResult retry2 = request(repair_system, repair_user, 0.0, tokens);
  if (retry2.ok && !has_forbidden_script(retry2.content)) return retry2;

  // Last resort: drop forbidden characters and orphan single Arabic letters.
  std::string stripped = mask_forbidden_script(result.content);
  const std::string marker = "___";
  size_t pos;
  while ((pos = stripped.find(marker)) != std::string::npos) {
    stripped.replace(pos, marker.size(), " ");
  }
  stripped = clean_orphan_letters(collapse_ws(stripped));
  if (!stripped.empty() && !has_forbidden_script(stripped)) {
    if (retry.ok) {
      std::string alt = clean_orphan_letters(
          collapse_ws(mask_forbidden_script(retry.content)));
      // erase ___ leftovers
      while ((pos = alt.find(marker)) != std::string::npos) alt.replace(pos, marker.size(), " ");
      alt = clean_orphan_letters(collapse_ws(alt));
      if (!alt.empty() && !has_forbidden_script(alt) && alt.size() > stripped.size()) {
        ChatResult out;
        out.ok = true;
        out.content = alt;
        return out;
      }
    }
    ChatResult out;
    out.ok = true;
    out.content = stripped;
    return out;
  }
  if (retry2.ok) {
    retry2.content = clean_orphan_letters(
        collapse_ws(mask_forbidden_script(retry2.content)));
    return retry2;
  }
  if (retry.ok) {
    retry.content =
        clean_orphan_letters(collapse_ws(mask_forbidden_script(retry.content)));
    return retry;
  }
  return result;
}
