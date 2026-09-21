#!/usr/bin/env python3
"""Exhaustive translation / lexicon probe for debug session b496cb."""

from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

LOG_PATH = ROOT / ".cursor" / "debug-b496cb.log"
SESSION = "b496cb"

from translator.config import load_settings
from translator.lexicon import Lexicon, default_lexicon_path, normalize_arabic, tokenize
from translator.llama_client import LlamaClient
from translator.prompts import Direction, Mode, build_prompt, generation_params_for, target_language


def log(hypothesis_id: str, location: str, message: str, data: dict, run_id: str = "pre"):
    payload = {
        "sessionId": SESSION,
        "runId": run_id,
        "hypothesisId": hypothesis_id,
        "location": location,
        "message": message,
        "data": data,
        "timestamp": int(time.time() * 1000),
    }
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with LOG_PATH.open("a", encoding="utf-8") as f:
        f.write(json.dumps(payload, ensure_ascii=False) + "\n")


def parse_wbw(content: str):
    rows = []
    full = ""
    for raw in content.splitlines():
        line = raw.strip()
        if not line:
            continue
        lower = line.lower()
        if lower.startswith("full translation:"):
            full = line.split(":", 1)[1].strip()
            continue
        if "|" in line:
            parts = [p.strip() for p in line.split("|")]
            if len(parts) >= 2:
                rows.append(parts[:3] if len(parts) >= 3 else [parts[0], "", parts[1]])
    return {"ok": bool(rows), "n_rows": len(rows), "full": full, "sample": rows[:3]}


def check_format_both(content: str) -> dict:
    has_t = bool(re.search(r"(?im)^translation\s*:", content))
    has_e = bool(re.search(r"(?im)^explanation\s*:", content))
    return {"has_translation": has_t, "has_explanation": has_e, "ok": has_t and has_e}


def lexicon_unit_tests(lex: Lexicon) -> list[dict]:
    cases = []
    # H-A: normalization / prefix stripping
    probes = [
        ("كتاب", "book-like"),
        ("الكتاب", "with al-"),
        ("والكتاب", "waw+al"),
        ("بالكتاب", "ba+al"),
        ("مَرْحَبًا", "tashkeel"),
        ("مدرسة", "taa marbuta key"),
        ("hello", "english miss"),
        ("مرحبا", "greeting"),
    ]
    for tok, note in probes:
        glosses = lex.lookup(tok)
        cases.append(
            {
                "token": tok,
                "note": note,
                "norm": normalize_arabic(tok),
                "glosses": glosses[:3],
                "hit": bool(glosses),
            }
        )
        log(
            "A",
            "debug_exhaustive_test.py:lexicon",
            "lexicon lookup",
            {"token": tok, "note": note, "hit": bool(glosses), "glosses": glosses[:5]},
        )

    # H-B: tokenize mixed punctuation / numbers
    samples = [
        "مرحبا، بالعالم!",
        "Hello, world!",
        "السعر 100 دولار",
        "U.S.A. في القاهرة",
    ]
    for s in samples:
        toks = tokenize(s)
        rag = lex.build_rag_context(s)
        cases.append({"text": s, "tokens": toks, "rag_lines": rag.count("\n") if rag else 0})
        log(
            "B",
            "debug_exhaustive_test.py:tokenize",
            "tokenize+rag",
            {"text": s, "tokens": toks, "rag_preview": (rag or "")[:300], "rag_empty": not rag},
        )
    return cases


CASES = [
    # (id, direction, mode, text, hypotheses)
    ("T1", "ar-en", "translate", "مرحبا بالعالم", ["C", "D"]),
    ("T2", "en-ar", "translate", "Hello world", ["C", "D"]),
    ("T3", "auto", "translate", "السلام عليكم", ["C", "E"]),
    ("T4", "auto", "translate", "Good morning", ["C", "E"]),
    ("T5", "ar-en", "translate", "أَهْلًا وَسَهْلًا", ["A", "C"]),
    ("T6", "ar-en", "translate", "السعر ١٠٠ دولار فقط!", ["B", "C"]),
    ("T7", "en-ar", "translate", "The quick brown fox jumps over the lazy dog.", ["C", "F"]),
    ("T8", "ar-en", "translate", "ذهب أحمد إلى القاهرة يوم الجمعة.", ["C", "D"]),
    ("T9", "en-ar", "translate", "Please submit the report by Monday.", ["C"]),
    ("T10", "ar-en", "translate", "كيف حالك؟", ["C"]),
    ("T11", "ar-en", "explain", "كتاب", ["C", "D"]),
    ("T12", "en-ar", "explain", "democracy", ["C"]),
    ("T13", "ar-en", "both", "إن شاء الله", ["C", "G"]),
    ("T14", "en-ar", "both", "Break a leg!", ["C", "G"]),
    ("T15", "ar-en", "transliterate", "السلام عليكم ورحمة الله", ["C"]),
    ("T16", "en-ar", "transliterate", "Washington", ["C"]),
    ("T17", "ar-en", "word-by-word", "أحب اللغة العربية", ["C", "H"]),
    ("T18", "en-ar", "word-by-word", "I love Arabic", ["C", "H"]),
    ("T19", "ar-en", "word-by-word", "في البيت الكبير", ["A", "H"]),
    ("T20", "auto", "translate", "Mix English and عربي in one sentence", ["E", "C"]),
    ("T21", "ar-en", "translate", "لا", ["C"]),
    ("T22", "en-ar", "translate", "OK", ["C"]),
    ("T23", "ar-en", "translate", "محمد بن سلمان", ["C"]),
    ("T24", "ar-en", "both", "رمضان كريم", ["G"]),
]


def run_llm_cases(client: LlamaClient, lex: Lexicon):
    dir_map = {
        "ar-en": Direction.AR_TO_EN,
        "en-ar": Direction.EN_TO_AR,
        "auto": Direction.AUTO,
    }
    mode_map = {
        "translate": Mode.TRANSLATE,
        "explain": Mode.EXPLAIN,
        "both": Mode.BOTH,
        "transliterate": Mode.TRANSLITERATE,
        "word-by-word": Mode.WORD_BY_WORD,
    }
    results = []
    for case_id, d_s, m_s, text, hyps in CASES:
        direction = dir_map[d_s]
        mode = mode_map[m_s]
        rag = ""
        if mode in (Mode.WORD_BY_WORD, Mode.EXPLAIN, Mode.TRANSLATE, Mode.BOTH):
            rag = lex.build_rag_context(text)
        system, user = build_prompt(mode, direction, text, rag)
        temp, max_tok = generation_params_for(mode)
        target = target_language(direction, text)
        t0 = time.time()
        result = client.chat(system, user, temperature=temp, max_tokens=max_tok)
        elapsed = round(time.time() - t0, 2)
        content = result.content if result.ok else ""
        issues = []
        if not result.ok:
            issues.append("request_failed")
        if result.ok:
            # CJK / other-script contamination
            if any("\u4e00" <= c <= "\u9fff" for c in content):
                issues.append("cjk_contamination")
            if "..." in content.split("\n")[0] or content.strip().startswith("..."):
                issues.append("ellipsis_placeholder")
        if result.ok and mode is Mode.TRANSLATE:
            # preface leakage
            low = content.lower()
            if any(
                low.startswith(p)
                for p in ("here is", "translation:", "sure,", "of course", "the translation")
            ):
                issues.append("preface_leak")
            if direction is Direction.AR_TO_EN or (
                direction is Direction.AUTO and any("\u0600" <= c <= "\u06ff" for c in text)
            ):
                # expected mostly Latin
                ar_chars = sum(1 for c in content if "\u0600" <= c <= "\u06ff")
                if ar_chars > max(3, len(content) // 4):
                    issues.append("wrong_script_ar_in_en_output")
            if direction is Direction.EN_TO_AR or (
                direction is Direction.AUTO
                and not any("\u0600" <= c <= "\u06ff" for c in text)
            ):
                ar_chars = sum(1 for c in content if "\u0600" <= c <= "\u06ff")
                if ar_chars < 1 and len(content) > 2:
                    issues.append("missing_arabic_in_en_ar")
                # leftover Latin content words (heuristic)
                latin_words = re.findall(r"[A-Za-z]{3,}", content)
                if latin_words and ar_chars > 0:
                    issues.append("leftover_english")
        fmt = {}
        if result.ok and mode is Mode.BOTH:
            fmt = check_format_both(content)
            if not fmt["ok"]:
                issues.append("both_format_broken")
            if re.search(r"(?im)^translation\s*:\s*\.\.\.\s*$", content) or re.search(
                r"(?im)^explanation\s*:\s*\.\.\.\s*$", content
            ):
                issues.append("both_template_echo")
            if content.lower().count("translation:") > 1:
                issues.append("both_duplicate_headers")
        wbw = {}
        if result.ok and mode is Mode.WORD_BY_WORD:
            wbw = parse_wbw(content)
            if not wbw["ok"]:
                issues.append("wbw_parse_fail")
            toks = tokenize(text)
            if wbw["ok"] and abs(wbw["n_rows"] - len(toks)) > 1:
                issues.append("wbw_row_count_mismatch")
            if wbw["ok"] and not wbw["full"]:
                issues.append("wbw_missing_full")
        if result.ok and mode is Mode.TRANSLITERATE:
            if direction is Direction.AR_TO_EN or any("\u0600" <= c <= "\u06ff" for c in text):
                # should be latin-ish
                ar = sum(1 for c in content if "\u0600" <= c <= "\u06ff")
                if ar > len(content) // 2:
                    issues.append("translit_still_arabic")
        entry = {
            "id": case_id,
            "direction": d_s,
            "mode": m_s,
            "text": text,
            "target": target,
            "ok": result.ok,
            "error": result.error,
            "elapsed_s": elapsed,
            "rag_hits": rag.count("→") if rag else 0,
            "content_preview": content[:400],
            "content_len": len(content),
            "issues": issues,
            "fmt": fmt,
            "wbw": wbw,
        }
        results.append(entry)
        for h in hyps:
            log(
                h,
                f"debug_exhaustive_test.py:{case_id}",
                "case result",
                entry,
            )
        print(
            f"[{case_id}] {d_s}/{m_s} ok={result.ok} {elapsed}s issues={issues} "
            f"preview={content[:80]!r}"
        )
        sys.stdout.flush()
    return results


def main() -> int:
    settings = load_settings()
    client = LlamaClient(
        host=settings.host,
        port=settings.port,
        model=settings.model_name,
        temperature=settings.temperature,
        max_tokens=settings.max_tokens,
    )
    healthy = client.health_check()
    log("Z", "debug_exhaustive_test.py:main", "health", {"ok": healthy, "base": client.base})
    if not healthy:
        print("server offline", file=sys.stderr)
        return 1

    lex = Lexicon()
    loaded = lex.load(default_lexicon_path())
    log(
        "A",
        "debug_exhaustive_test.py:main",
        "lexicon loaded",
        {"loaded": loaded, "size": lex.size, "path": str(default_lexicon_path())},
    )
    print(f"lexicon size={lex.size} loaded={loaded}")
    lexicon_unit_tests(lex)
    results = run_llm_cases(client, lex)
    n_fail = sum(1 for r in results if not r["ok"])
    n_issues = sum(1 for r in results if r["issues"])
    summary = {
        "n_cases": len(results),
        "n_fail": n_fail,
        "n_with_issues": n_issues,
        "issue_counts": {},
    }
    for r in results:
        for i in r["issues"]:
            summary["issue_counts"][i] = summary["issue_counts"].get(i, 0) + 1
    log("Z", "debug_exhaustive_test.py:main", "summary", summary)
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
