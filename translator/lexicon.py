"""Arabic–English lexical RAG from MUSE TSV (ar\\ten)."""

from __future__ import annotations

import os
import re
from pathlib import Path

_TASHKEEL = re.compile(r"[\u064B-\u065F\u0670\u06D6-\u06ED\u0640]")
# Letters only (exclude Arabic punctuation U+060C/061B/061F etc. inside 0600–06FF).
_TOKEN = re.compile(
    r"[\u0621-\u063A\u0641-\u064A\u066E-\u066F\u0671-\u06D3\u06D5"
    r"\u06EE-\u06EF\u06FA-\u06FF\u0750-\u077F\u08A0-\u08FF"
    r"\uFB50-\uFDFF\uFE70-\uFEFF]+"
    r"|[\u0660-\u0669\u06F0-\u06F9]+"  # Arabic-Indic / Extended digits
    r"|[A-Za-z0-9]+(?:['-][A-Za-z0-9]+)*"
)

_PREFIXES = ("ال", "و", "ب", "ف", "ك", "ل")
_EN_STOP = frozenset(
    {
        "the",
        "a",
        "an",
        "of",
        "to",
        "in",
        "on",
        "for",
        "and",
        "or",
        "is",
        "are",
        "was",
        "were",
        "be",
        "by",
        "at",
        "as",
        "it",
        "i",
    }
)

# Multi-word EN→AR hints for phrases the model often contaminates or calques.
_PHRASE_HINTS: list[tuple[str, str]] = [
    ("jumps over", "يقفز فوق"),
    ("jump over", "يقفز فوق"),
    ("good morning", "صباح الخير"),
    ("break a leg", "بالتوفيق"),
    ("how are you", "كيف حالك"),
]


def normalize_arabic(text: str) -> str:
    text = _TASHKEEL.sub("", text)
    out = []
    for ch in text:
        if ch in "أإآٱ":
            out.append("ا")
        elif ch == "ى":
            out.append("ي")
        elif ch == "ة":
            out.append("ه")
        else:
            out.append(ch)
    return "".join(out)


def tokenize(text: str) -> list[str]:
    return _TOKEN.findall(text)


class Lexicon:
    def __init__(self) -> None:
        self._map: dict[str, list[str]] = {}
        self._en_map: dict[str, list[str]] = {}

    def load(self, path: str | Path) -> bool:
        self._map.clear()
        self._en_map.clear()
        p = Path(path)
        if not p.is_file():
            return False
        with p.open(encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "\t" not in line:
                    continue
                ar, en = line.split("\t", 1)
                key = normalize_arabic(ar)
                en = en.strip()
                if not key or not en:
                    continue
                glosses = self._map.setdefault(key, [])
                if en not in glosses and len(glosses) < 8:
                    glosses.append(en)
                # Reverse index for single-token English glosses (EN→AR RAG).
                en_key = en.lower()
                if " " not in en_key and en_key.isascii() and en_key.isalnum():
                    rev = self._en_map.setdefault(en_key, [])
                    if ar not in rev and len(rev) < 8:
                        rev.append(ar)
        return bool(self._map)

    @property
    def size(self) -> int:
        return len(self._map)

    def lookup(self, token: str, limit: int = 5) -> list[str]:
        if not token:
            return []
        key = normalize_arabic(token)
        hit = self._map.get(key, [])
        if hit:
            return hit[:limit]
        hit = self._map.get(token.lower(), [])
        if hit:
            return hit[:limit]
        for pref in _PREFIXES:
            if key.startswith(pref) and len(key) > len(pref):
                hit = self._map.get(key[len(pref) :], [])
                if hit:
                    return hit[:limit]
        for pref in ("و", "ب", "ف", "ك", "ل"):
            if key.startswith(pref):
                rest = key[len(pref) :]
                if rest.startswith("ال") and len(rest) > 2:
                    hit = self._map.get(rest[2:], [])
                    if hit:
                        return hit[:limit]
        return []

    def lookup_en(self, token: str, limit: int = 5) -> list[str]:
        if not token:
            return []
        key = token.lower()
        # Exact key only — stemming (jumps→jump) often yields the wrong POS.
        hit = list(self._en_map.get(key, []))
        if not hit:
            return []

        def score(ar: str) -> tuple[int, int]:
            # Prefer definite-article lemmas (البني, الثعلب, الكلب) over
            # phonetic loans (براون, فوكس).
            has_al = 1 if ar.startswith("ال") else 0
            return (has_al, len(ar))

        hit.sort(key=score, reverse=True)
        return hit[:limit]

    def build_rag_context(self, text: str, max_entries: int = 40) -> str:
        if not self._map and not self._en_map:
            return ""
        lines: list[str] = []
        seen: set[str] = set()
        lower = text.lower()
        for phrase, ar in _PHRASE_HINTS:
            if len(lines) >= max_entries:
                break
            if phrase in lower and phrase not in seen:
                seen.add(phrase)
                lines.append(f"{phrase} → {ar}")
        for tok in tokenize(text):
            if len(lines) >= max_entries:
                break
            glosses = self.lookup(tok, 2)
            direction = "ar→en"
            if not glosses:
                if tok.lower() in _EN_STOP:
                    continue
                glosses = self.lookup_en(tok, 2)
                direction = "en→ar"
            if not glosses:
                continue
            norm = normalize_arabic(tok) if direction == "ar→en" else tok.lower()
            if norm in seen:
                continue
            seen.add(norm)
            # Slash-separated to avoid `;` looking like a WBW column value.
            lines.append(f"{tok} → {' / '.join(glosses)}")
        if not lines:
            return ""
        return (
            "Dictionary hints (prefer these when they fit context; "
            "in word-by-word mode use them ONLY for the meaning/gloss column, "
            "never as transliteration):\n"
            + "\n".join(lines)
            + "\n"
        )


def default_lexicon_path(configured: str | None = None) -> Path:
    env = os.environ.get("TRANSLATOR_LEXICON")
    if env:
        return Path(env)
    if configured:
        p = Path(configured)
        if p.is_absolute():
            return p
        return Path(__file__).resolve().parent.parent / p
    root = Path(__file__).resolve().parent.parent
    return root / "data" / "muse_ar_en.txt"
