"""Prompt builders with optional lexical RAG context."""

from __future__ import annotations

from enum import Enum


class Direction(Enum):
    AR_TO_EN = "ar_en"
    EN_TO_AR = "en_ar"
    AUTO = "auto"


class Mode(Enum):
    TRANSLATE = "translate"
    EXPLAIN = "explain"
    BOTH = "both"
    TRANSLITERATE = "transliterate"
    WORD_BY_WORD = "word-by-word"


_SCRIPT_RULE = (
    "Output ONLY Arabic script and/or Latin script. "
    "Never use Chinese, Japanese, Korean, Cyrillic, or any other writing system. "
    "Translate every content word into the target language — do not leave source-language "
    "words untranslated (except unavoidable proper names)."
)

_SYSTEM_TRANSLATE = (
    "You are a precise bilingual Arabic–English machine translator. "
    "Produce a faithful translation only: preserve meaning, tone, named entities, "
    "numbers, and punctuation. Do not add commentary, notes, or alternatives. "
    + _SCRIPT_RULE
)

_SYSTEM_EXPLAIN = (
    "You are an Arabic language expert. Explain Arabic words and phrases clearly "
    "for learners: lemma, sense(s), register (MSA/dialect if relevant), "
    "and one short example sentence in Arabic with an English gloss. "
    "When dictionary hints are provided, prefer senses that match those glosses. "
    + _SCRIPT_RULE
)

_SYSTEM_BOTH = (
    "You are a bilingual Arabic–English translator and language tutor. "
    "Give an accurate translation, then briefly explain notable words, "
    "idioms, or cultural nuance. Keep notes concise. "
    "For idioms, translate the intended meaning (not a literal calque). "
    "Use dictionary hints when provided. "
    + _SCRIPT_RULE
)

_SYSTEM_TRANSLITERATE = (
    "You are an expert in Arabic romanization and phonetic transcription for learners. "
    "Use clear scholarly-style Latin transliteration (ALA-LC / DIN-like): "
    "mark long vowels (ā ī ū), use ʾ for hamza and ʿ for ʿayn when useful. "
    "Do not translate meaning unless asked. "
    + _SCRIPT_RULE
)

_SYSTEM_WORD_BY_WORD = (
    "You are a bilingual Arabic–English tutor producing a word-by-word gloss. "
    "Output exactly one line per input token in the same order. "
    "Do not skip, merge, invent, or append extra tokens. "
    "Column 2 is ALWAYS Latin-script pronunciation/transliteration (never a gloss list). "
    "Column 3 is the meaning gloss; when dictionary hints exist, pick one fitting gloss there. "
    "Always end with a natural Full translation line. "
    + _SCRIPT_RULE
)


def _contains_arabic(text: str) -> bool:
    return any("\u0600" <= ch <= "\u06ff" for ch in text)


def _contains_latin_letter(text: str) -> bool:
    return any(("A" <= ch <= "Z") or ("a" <= ch <= "z") for ch in text)


def target_language(direction: Direction, text: str) -> str:
    if direction is Direction.AR_TO_EN:
        return "English"
    if direction is Direction.EN_TO_AR:
        return "Arabic"
    # Mixed: prefer translating Arabic → English when both scripts appear.
    if _contains_arabic(text) and _contains_latin_letter(text):
        return "English"
    return "English" if _contains_arabic(text) else "Arabic"


def generation_params_for(mode: Mode) -> tuple[float, int]:
    if mode in (Mode.TRANSLATE, Mode.TRANSLITERATE):
        return 0.1, 512
    if mode is Mode.WORD_BY_WORD:
        return 0.15, 1024
    return 0.3, 768


def _with_rag(body: str, rag: str) -> str:
    if not rag:
        return body
    return rag.rstrip() + "\n\n" + body


def build_prompt(
    mode: Mode,
    direction: Direction,
    text: str,
    rag_context: str = "",
) -> tuple[str, str]:
    text = text.strip()
    target = target_language(direction, text)
    arabic_source = (
        direction is Direction.AR_TO_EN
        or (direction is Direction.AUTO and _contains_arabic(text))
        or (direction is Direction.EN_TO_AR and _contains_arabic(text))
    )
    mixed = _contains_arabic(text) and _contains_latin_letter(text)

    if mode is Mode.TRANSLATE:
        mixed_note = ""
        if mixed and target == "English":
            mixed_note = (
                "The input mixes English and Arabic: keep the English words, "
                "and translate every Arabic word/phrase into natural English.\n"
            )
        elif mixed and target == "Arabic":
            mixed_note = (
                "The input mixes English and Arabic: keep Arabic words, "
                "and translate every English word/phrase into natural Arabic.\n"
            )
        user = _with_rag(
            f"{mixed_note}"
            f"Translate the following text into {target}.\n"
            f"Reply with the translation only — no preface, no quotes, no ellipsis placeholders.\n"
            f"Every word must be in {target} (except proper names).\n\n{text}",
            rag_context,
        )
        return _SYSTEM_TRANSLATE, user

    if mode is Mode.EXPLAIN:
        user = _with_rag(
            "Explain this Arabic word or short phrase for an English-speaking learner. "
            "If the input is English, give the correct Arabic equivalent(s) with accurate "
            "voweling when helpful, and explain usage. Do not invent wrong Arabic spellings.\n\n"
            f"{text}",
            rag_context,
        )
        return _SYSTEM_EXPLAIN, user

    if mode is Mode.BOTH:
        user = _with_rag(
            f"Translate into {target}, then briefly explain key words or nuance.\n\n"
            "Use exactly this layout (fill in real content; never write literal dots):\n"
            "Translation: <your translation here>\n"
            "Explanation: <your explanation here>\n\n"
            f"{text}",
            rag_context,
        )
        return _SYSTEM_BOTH, user

    if mode is Mode.TRANSLITERATE:
        if arabic_source or _contains_arabic(text):
            user = (
                "Transliterate the following Arabic into Latin script only. "
                f"Output one line of romanization (no translation).\n\n{text}"
            )
        else:
            user = (
                "Provide a conventional Arabic-script phonetic spelling of this "
                "English name or phrase (تعريب صوتي), then on a second line a simple "
                "Latin pronunciation guide. Prefer well-known Arabic spellings for "
                f"place/person names when they exist (e.g. Washington → واشنطن).\n\n{text}"
            )
        return _SYSTEM_TRANSLITERATE, user

    if _contains_arabic(text) or direction is Direction.AR_TO_EN:
        user = _with_rag(
            "Give a word-by-word gloss of the Arabic text into English.\n"
            "Rules:\n"
            "- Exactly one output line per input token, same order (no extra lines).\n"
            "- Format: Arabic | Latin-transliteration | English gloss\n"
            "- Transliteration must be Latin letters only (e.g. fī, al-bayt) — "
            "never paste dictionary hint lists into that column.\n"
            "- Gloss: one short English meaning (pick from dictionary hints when fitting).\n"
            "- Final line MUST be: Full translation: <natural English sentence>\n\n"
            f"{text}",
            rag_context,
        )
    else:
        user = _with_rag(
            "Give a word-by-word gloss of the English text into Arabic.\n"
            "Rules:\n"
            "- Exactly one output line per input word, same order (no extra lines).\n"
            "- Format: English | Arabic | Latin-transliteration\n"
            "- Arabic column: ONE Arabic word/phrase only (not a slash-separated list).\n"
            "- Transliteration: Latin pronunciation of that Arabic only.\n"
            "- Do not skip or merge words.\n"
            "- Final line MUST be: Full translation: <natural Arabic sentence>\n\n"
            f"{text}",
            rag_context,
        )
    return _SYSTEM_WORD_BY_WORD, user
