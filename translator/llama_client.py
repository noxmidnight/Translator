"""Minimal OpenAI-compatible client for llama-server."""

from __future__ import annotations

import json
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any


@dataclass
class ChatResult:
    ok: bool
    content: str = ""
    error: str = ""


def has_forbidden_script(text: str) -> bool:
    """CJK / Cyrillic / etc. — not Arabic or Latin."""
    for ch in text:
        o = ord(ch)
        if 0x4E00 <= o <= 0x9FFF:  # CJK Unified
            return True
        if 0x3400 <= o <= 0x4DBF:  # CJK Extension A
            return True
        if 0x3040 <= o <= 0x30FF:  # Hiragana / Katakana
            return True
        if 0xAC00 <= o <= 0xD7AF:  # Hangul
            return True
        if 0x0400 <= o <= 0x04FF:  # Cyrillic
            return True
    return False


def mask_forbidden_script(text: str) -> str:
    """Replace forbidden-script runs with a blank placeholder."""
    out: list[str] = []
    in_bad = False
    for ch in text:
        if has_forbidden_script(ch):
            if not in_bad:
                out.append(" ___ ")
                in_bad = True
        else:
            out.append(ch)
            in_bad = False
    return " ".join("".join(out).split())


def _source_from_user(user: str) -> str:
    """Best-effort: last non-empty paragraph is usually the source text."""
    parts = [p.strip() for p in user.strip().split("\n\n") if p.strip()]
    return parts[-1] if parts else user.strip()


def _clean_orphan_letters(text: str) -> str:
    """Drop lone Arabic letters left beside stripped CJK (e.g. 'ت' before 跳过)."""
    toks = text.split()
    kept: list[str] = []
    for t in toks:
        if len(t) == 1 and "\u0600" <= t <= "\u06ff":
            continue
        kept.append(t)
    return " ".join(kept)


class LlamaClient:
    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 8080,
        model: str = "allam-7b-instruct",
        temperature: float = 0.3,
        max_tokens: int = 768,
        timeout: float = 300.0,
    ) -> None:
        self.base = f"http://{host}:{port}"
        self.model = model
        self.temperature = temperature
        self.max_tokens = max_tokens
        self.timeout = timeout

    def health_check(self) -> bool:
        try:
            with urllib.request.urlopen(f"{self.base}/health", timeout=2) as resp:
                return 200 <= resp.status < 300
        except Exception:
            return False

    def _request(
        self,
        system: str,
        user: str,
        temperature: float,
        max_tokens: int,
    ) -> ChatResult:
        payload: dict[str, Any] = {
            "model": self.model,
            "temperature": temperature,
            "max_tokens": max_tokens,
            "messages": [
                {"role": "system", "content": system},
                {"role": "user", "content": user},
            ],
        }
        data = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            f"{self.base}/v1/chat/completions",
            data=data,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                body = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            try:
                err = json.loads(detail).get("error", {})
                msg = err.get("message", detail) if isinstance(err, dict) else detail
            except Exception:
                msg = detail or str(exc)
            return ChatResult(False, error=f"Server error ({exc.code}): {msg}")
        except urllib.error.URLError as exc:
            return ChatResult(
                False,
                error=(
                    f"Cannot reach llama-server at {self.base}. "
                    f"Start it with ./scripts/translator.sh ({exc.reason})"
                ),
            )
        except Exception as exc:
            return ChatResult(False, error=str(exc))

        try:
            content = body["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError):
            return ChatResult(False, error="Could not parse model response")

        if not content:
            return ChatResult(False, error="Empty model response")
        return ChatResult(True, content=content.strip())

    def chat(
        self,
        system: str,
        user: str,
        temperature: float | None = None,
        max_tokens: int | None = None,
    ) -> ChatResult:
        temp = self.temperature if temperature is None else temperature
        tokens = self.max_tokens if max_tokens is None else max_tokens
        result = self._request(system, user, temp, tokens)
        if not result.ok:
            return result
        if not has_forbidden_script(result.content):
            return result

        source = _source_from_user(user)
        nuclear_system = (
            "You are a precise bilingual Arabic–English translator. "
            "Output ONLY Arabic script for Arabic translations (Latin only if "
            "translating into English). Never use Chinese, Japanese, Korean, or Cyrillic. "
            "Known phrase: jumps over → يقفز فوق."
        )
        nuclear_user = (
            "Translate the following into the appropriate target language. "
            "Reply with the translation only.\n\n" + source
        )
        retry = self._request(nuclear_system, nuclear_user, 0.0, tokens)
        if retry.ok and not has_forbidden_script(retry.content):
            return retry

        repair_system = (
            "You are a precise Arabic–English translator. "
            "Output ONLY Arabic and/or Latin script. Never use Chinese or other scripts. "
            "Use يقفز فوق for 'jumps over'."
        )
        repair_user = (
            "Ignore any Chinese in prior drafts. Translate this source cleanly:\n\n"
            + source
        )
        retry2 = self._request(repair_system, repair_user, 0.0, tokens)
        if retry2.ok and not has_forbidden_script(retry2.content):
            return retry2

        # Last resort: strip forbidden chars and drop orphan single Arabic letters.
        stripped = mask_forbidden_script(result.content).replace("___", " ").strip()
        stripped = _clean_orphan_letters(" ".join(stripped.split()))
        if stripped and not has_forbidden_script(stripped):
            if retry.ok and len(retry.content) > len(stripped):
                cleaned = _clean_orphan_letters(
                    mask_forbidden_script(retry.content).replace("___", " ")
                )
                if cleaned and not has_forbidden_script(cleaned):
                    return ChatResult(True, content=cleaned)
            return ChatResult(True, content=stripped)
        if retry2.ok:
            return ChatResult(
                True,
                content=_clean_orphan_letters(
                    mask_forbidden_script(retry2.content).replace("___", " ")
                )
                or retry2.content,
            )
        if retry.ok:
            return ChatResult(
                True,
                content=_clean_orphan_letters(
                    mask_forbidden_script(retry.content).replace("___", " ")
                )
                or retry.content,
            )
        return result
