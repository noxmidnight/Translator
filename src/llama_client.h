#pragma once

#include "config.h"

#include <optional>
#include <string>

struct ChatResult {
  bool ok = false;
  std::string content;
  std::string error;
};

class LlamaClient {
 public:
  explicit LlamaClient(AppConfig config);

  bool health_check() const;
  ChatResult chat(const std::string& system, const std::string& user,
                  std::optional<double> temperature = std::nullopt,
                  std::optional<int> max_tokens = std::nullopt) const;

 private:
  AppConfig config_;
  std::string base_url() const;
  ChatResult request(const std::string& system, const std::string& user,
                     double temperature, int max_tokens) const;
};
