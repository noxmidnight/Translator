#include "prompts.h"

#include <cstdint>

namespace {

const char* kScriptRule =
    "Output ONLY Arabic script and/or Latin script. "
    "Never use Chinese, Japanese, Korean, Cyrillic, or any other writing system. "
    "Translate every content word into the target language — do not leave source-language "
    "words untranslated (except unavoidable proper names).";

std::string system_translate() {
  return std::string(
             "You are a precise bilingual Arabic–English machine translator. "
             "Produce a faithful translation only: preserve meaning, tone, named entities, "
             "numbers, and punctuation. Do not add commentary, notes, or alternatives. ") +
         kScriptRule;
}

std::string system_explain() {
  return std::string(
             "You are an Arabic language expert. Explain Arabic words and phrases clearly "
             "for learners: lemma, sense(s), register (MSA/dialect if relevant), "
             "and one short example sentence in Arabic with an English gloss. "
             "When dictionary hints are provided, prefer senses that match those glosses. ") +
         kScriptRule;
}

std::string system_both() {
  return std::string(
             "You are a bilingual Arabic–English translator and language tutor. "
             "Give an accurate translation, then briefly explain notable words, "
             "idioms, or cultural nuance. Keep notes concise. "
             "For idioms, translate the intended meaning (not a literal calque). "
             "Use dictionary hints when provided. ") +
         kScriptRule;
}

std::string system_transliterate() {
  return std::string(
             "You are an expert in Arabic romanization and phonetic transcription for learners. "
             "Use clear scholarly-style Latin transliteration (ALA-LC / DIN-like): "
             "mark long vowels (ā ī ū), use ʾ for hamza and ʿ for ʿayn when useful. "
             "Do not translate meaning unless asked. ") +
         kScriptRule;
}

std::string system_word_by_word() {
  return std::string(
             "You are a bilingual Arabic–English tutor producing a word-by-word gloss. "
             "Output exactly one line per input token in the same order. "
             "Do not skip, merge, invent, or append extra tokens. "
             "Column 2 is ALWAYS Latin-script pronunciation/transliteration (never a gloss list). "
             "Column 3 is the meaning gloss; when dictionary hints exist, pick one fitting gloss there. "
             "Always end with a natural Full translation line. ") +
         kScriptRule;
}

bool contains_latin_letter(const std::string& text) {
  for (unsigned char c : text) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
  }
  return false;
}

std::string target_language(Direction d, const std::string& text) {
  if (d == Direction::ArToEn) return "English";
  if (d == Direction::EnToAr) return "Arabic";
  if (contains_arabic(text) && contains_latin_letter(text)) return "English";
  return contains_arabic(text) ? "English" : "Arabic";
}

std::string with_rag(const std::string& body, const std::string& rag) {
  if (rag.empty()) return body;
  return rag + "\n" + body;
}

}  // namespace

bool contains_arabic(const std::string& text) {
  // Proper UTF-8 Arabic ranges (not just lead-byte heuristic).
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
    if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
        (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
        (cp >= 0xFE70 && cp <= 0xFEFF)) {
      return true;
    }
  }
  return false;
}

GenParams generation_params_for(Mode mode) {
  switch (mode) {
    case Mode::Translate:
    case Mode::Transliterate:
      return {0.1, 512};
    case Mode::WordByWord:
      return {0.15, 1024};
    case Mode::ExplainWord:
    case Mode::TranslateAndExplain:
      return {0.3, 768};
  }
  return {0.3, 512};
}

PromptPair build_prompt(Mode mode, Direction direction, const std::string& text,
                        const std::string& rag_context) {
  PromptPair p;
  const std::string target = target_language(direction, text);
  const bool arabic_source =
      direction == Direction::ArToEn ||
      (direction == Direction::Auto && contains_arabic(text)) ||
      (direction == Direction::EnToAr && contains_arabic(text));
  const bool mixed = contains_arabic(text) && contains_latin_letter(text);

  switch (mode) {
    case Mode::Translate: {
      p.system = system_translate();
      std::string mixed_note;
      if (mixed && target == "English") {
        mixed_note =
            "The input mixes English and Arabic: keep the English words, "
            "and translate every Arabic word/phrase into natural English.\n";
      } else if (mixed && target == "Arabic") {
        mixed_note =
            "The input mixes English and Arabic: keep Arabic words, "
            "and translate every English word/phrase into natural Arabic.\n";
      }
      p.user = with_rag(
          mixed_note + "Translate the following text into " + target +
              ".\nReply with the translation only — no preface, no quotes, no "
              "ellipsis placeholders.\nEvery word must be in " +
              target + " (except proper names).\n\n" + text,
          rag_context);
      break;
    }
    case Mode::ExplainWord:
      p.system = system_explain();
      p.user = with_rag(
          "Explain this Arabic word or short phrase for an English-speaking learner. "
          "If the input is English, give the correct Arabic equivalent(s) with accurate "
          "voweling when helpful, and explain usage. Do not invent wrong Arabic spellings.\n\n" +
              text,
          rag_context);
      break;
    case Mode::TranslateAndExplain:
      p.system = system_both();
      p.user = with_rag(
          "Translate into " + target +
              ", then briefly explain key words or nuance.\n\n"
              "Use exactly this layout (fill in real content; never write literal dots):\n"
              "Translation: <your translation here>\n"
              "Explanation: <your explanation here>\n\n" +
              text,
          rag_context);
      break;
    case Mode::Transliterate:
      p.system = system_transliterate();
      if (arabic_source || contains_arabic(text)) {
        p.user =
            "Transliterate the following Arabic into Latin script only. "
            "Output one line of romanization (no translation).\n\n" +
            text;
      } else {
        p.user =
            "Provide a conventional Arabic-script phonetic spelling of this "
            "English name or phrase (تعريب صوتي), then on a second line a simple "
            "Latin pronunciation guide. Prefer well-known Arabic spellings for "
            "place/person names when they exist (e.g. Washington → واشنطن).\n\n" +
            text;
      }
      break;
    case Mode::WordByWord:
      p.system = system_word_by_word();
      if (contains_arabic(text) || direction == Direction::ArToEn) {
        p.user = with_rag(
            "Give a word-by-word gloss of the Arabic text into English.\n"
            "Rules:\n"
            "- Exactly one output line per input token, same order (no extra lines).\n"
            "- Format: Arabic | Latin-transliteration | English gloss\n"
            "- Transliteration must be Latin letters only (e.g. fī, al-bayt) — "
            "never paste dictionary hint lists into that column.\n"
            "- Gloss: one short English meaning (pick from dictionary hints when fitting).\n"
            "- Final line MUST be: Full translation: <natural English sentence>\n\n" +
                text,
            rag_context);
      } else {
        p.user = with_rag(
            "Give a word-by-word gloss of the English text into Arabic.\n"
            "Rules:\n"
            "- Exactly one output line per input word, same order (no extra lines).\n"
            "- Format: English | Arabic | Latin-transliteration\n"
            "- Arabic column: ONE Arabic word/phrase only (not a slash-separated list).\n"
            "- Transliteration: Latin pronunciation of that Arabic only.\n"
            "- Do not skip or merge words.\n"
            "- Final line MUST be: Full translation: <natural Arabic sentence>\n\n" +
                text,
            rag_context);
      }
      break;
  }
  return p;
}
