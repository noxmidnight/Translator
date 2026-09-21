#pragma once

#include <string>

enum class Direction { ArToEn, EnToAr, Auto };
enum class Mode {
  Translate,
  ExplainWord,
  TranslateAndExplain,
  Transliterate,
  WordByWord
};

struct PromptPair {
  std::string system;
  std::string user;
};

struct GenParams {
  double temperature = 0.3;
  int max_tokens = 512;
};

PromptPair build_prompt(Mode mode, Direction direction, const std::string& text,
                        const std::string& rag_context = {});
GenParams generation_params_for(Mode mode);
bool contains_arabic(const std::string& text);
