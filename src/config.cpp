#include "config.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
#include <limits.h>

static std::string trim(const std::string& s) {
  const auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return {};
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

bool load_config(const std::string& path, AppConfig& out) {
  std::ifstream in(path);
  if (!in) return false;

  std::string line;
  std::string section;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    const std::string val = trim(line.substr(eq + 1));

    if (section == "server") {
      if (key == "host") out.host = val;
      else if (key == "port") out.port = std::stoi(val);
    } else if (section == "model") {
      if (key == "path") out.model_path = val;
      else if (key == "name") out.model = val;
    } else if (section == "generation") {
      if (key == "temperature") out.temperature = std::stod(val);
      else if (key == "max_tokens") out.max_tokens = std::stoi(val);
    } else if (section == "paths") {
      if (key == "lexicon") out.lexicon_path = val;
      else if (key == "theme_css") out.theme_css = val;
    } else {
      if (key == "host") out.host = val;
      else if (key == "port") out.port = std::stoi(val);
      else if (key == "model") out.model = val;
      else if (key == "temperature") out.temperature = std::stod(val);
      else if (key == "max_tokens") out.max_tokens = std::stoi(val);
      else if (key == "model_path") out.model_path = val;
      else if (key == "lexicon_path") out.lexicon_path = val;
    }
  }
  return true;
}

std::string config_search_path() {
  if (const char* env = std::getenv("TRANSLATOR_CONFIG")) {
    return env;
  }
  return "config.ini";
}

std::string project_root() {
  if (const char* env = std::getenv("TRANSLATOR_ROOT")) {
    return env;
  }
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd))) return cwd;
  return ".";
}

std::string resolve_path(const std::string& maybe_relative) {
  if (maybe_relative.empty()) return maybe_relative;
  if (maybe_relative[0] == '/') return maybe_relative;
  return project_root() + "/" + maybe_relative;
}
