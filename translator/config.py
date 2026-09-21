"""Load config.ini for the translator GUI and server scripts."""

from __future__ import annotations

import configparser
import os
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Settings:
    host: str = "127.0.0.1"
    port: int = 8080
    model_name: str = "allam-7b-instruct"
    model_path: str = "/home/nox/models/allam-7b-instruct-preview-q4_k_m.gguf"
    temperature: float = 0.3
    max_tokens: int = 768
    ctx_size: int = 2048
    gpu_layers: int = 99
    lexicon_path: str = "data/muse_ar_en.txt"


def project_root() -> Path:
    return Path(__file__).resolve().parent.parent


def config_path() -> Path:
    env = os.environ.get("TRANSLATOR_CONFIG")
    if env:
        return Path(env)
    return project_root() / "config.ini"


def load_settings(path: Path | None = None) -> Settings:
    cfg_path = path or config_path()
    example = project_root() / "config.ini.example"
    if not cfg_path.exists() and example.exists():
        cfg_path.write_text(example.read_text(encoding="utf-8"), encoding="utf-8")

    s = Settings()
    if not cfg_path.exists():
        return s

    parser = configparser.ConfigParser()
    parser.read(cfg_path, encoding="utf-8")

    if parser.has_section("server"):
        s.host = parser.get("server", "host", fallback=s.host)
        s.port = parser.getint("server", "port", fallback=s.port)
    if parser.has_section("model"):
        s.model_path = parser.get("model", "path", fallback=s.model_path)
        s.model_name = parser.get("model", "name", fallback=s.model_name)
        s.ctx_size = parser.getint("model", "ctx_size", fallback=s.ctx_size)
        s.gpu_layers = parser.getint("model", "gpu_layers", fallback=s.gpu_layers)
    if parser.has_section("generation"):
        s.temperature = parser.getfloat("generation", "temperature", fallback=s.temperature)
        s.max_tokens = parser.getint("generation", "max_tokens", fallback=s.max_tokens)
    if parser.has_section("paths"):
        s.lexicon_path = parser.get("paths", "lexicon", fallback=s.lexicon_path)
    return s
