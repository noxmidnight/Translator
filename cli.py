#!/usr/bin/env python3
"""CLI frontend with lexical RAG."""

from __future__ import annotations

import argparse
import sys

from translator import __version__
from translator.config import load_settings
from translator.lexicon import Lexicon, default_lexicon_path
from translator.llama_client import LlamaClient
from translator.prompts import Direction, Mode, build_prompt, generation_params_for


def _parse_direction(value: str) -> Direction:
    return {
        "ar-en": Direction.AR_TO_EN,
        "en-ar": Direction.EN_TO_AR,
        "auto": Direction.AUTO,
    }[value]


def _parse_mode(value: str) -> Mode:
    return {
        "translate": Mode.TRANSLATE,
        "explain": Mode.EXPLAIN,
        "both": Mode.BOTH,
        "transliterate": Mode.TRANSLITERATE,
        "word-by-word": Mode.WORD_BY_WORD,
    }[value]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Arabic ↔ English translator CLI")
    parser.add_argument("text", nargs="?", help="Text to process (or stdin)")
    parser.add_argument(
        "-d", "--direction", choices=["ar-en", "en-ar", "auto"], default="ar-en"
    )
    parser.add_argument(
        "-m",
        "--mode",
        choices=["translate", "explain", "both", "transliterate", "word-by-word"],
        default="translate",
    )
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    args = parser.parse_args(argv)

    text = args.text
    if not text:
        text = sys.stdin.read()
    text = (text or "").strip()
    if not text:
        parser.error("No input text")

    settings = load_settings()
    client = LlamaClient(
        host=settings.host,
        port=settings.port,
        model=settings.model_name,
        temperature=settings.temperature,
        max_tokens=settings.max_tokens,
    )
    if not client.health_check():
        print(
            f"llama-server offline at {settings.host}:{settings.port}. "
            "Run ./scripts/translator.sh",
            file=sys.stderr,
        )
        return 1

    mode = _parse_mode(args.mode)
    lexicon = Lexicon()
    lexicon.load(default_lexicon_path(settings.lexicon_path))
    rag = ""
    if mode in (Mode.WORD_BY_WORD, Mode.EXPLAIN, Mode.TRANSLATE, Mode.BOTH):
        rag = lexicon.build_rag_context(text)

    system, user = build_prompt(mode, _parse_direction(args.direction), text, rag)
    temp, max_tok = generation_params_for(mode)
    result = client.chat(system, user, temperature=temp, max_tokens=max_tok)
    if not result.ok:
        print(result.error, file=sys.stderr)
        return 1
    print(result.content)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
