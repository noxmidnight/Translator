#pragma once

#include <string>

struct AppConfig {
  std::string host = "127.0.0.1";
  int port = 8080;
  std::string model = "allam-7b-instruct";
  double temperature = 0.3;
  int max_tokens = 768;
  std::string model_path =
      "/home/nox/models/allam-7b-instruct-preview-q4_k_m.gguf";
  std::string lexicon_path = "data/muse_ar_en.txt";
  std::string theme_css = "assets/theme.css";
};

bool load_config(const std::string& path, AppConfig& out);
std::string config_search_path();
std::string project_root();
std::string resolve_path(const std::string& maybe_relative);
