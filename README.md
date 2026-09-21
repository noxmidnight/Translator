# Arabic ↔ English Translator (Jetson + llama.cpp)

Lightweight **C++ / GTK3** translator for Jetson Orin Nano 8GB — matte black + gold UI.
Uses **ALLaM-7B-Instruct** (Q4_K_M) via `llama-server`, with **lexical dictionary RAG** (MUSE AR–EN) for better word-by-word and sense selection. No second embedding model (RAM-safe on 8GB).

## Quick start

```bash
./scripts/download-model.sh      # ALLaM GGUF once
./scripts/download-lexicon.sh    # MUSE dict (~0.6 MB) if missing
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)

./scripts/translator.sh          # server + GUI; closing GUI stops server
```

### Desktop app (menu / dock)

```bash
./scripts/install-desktop.sh           # app menu entry + icons
./scripts/install-desktop.sh --desktop # also place shortcut on ~/Desktop
```

Click **Translator** to start `llama-server` + the GTK GUI. Closing the window stops the model server so Jetson RAM is freed. Icon: matte black + gold `A ↔ ع` (`assets/translator.png`).

Run from the project root (or set `TRANSLATOR_ROOT`) so `data/` and `assets/` resolve.

## Accuracy features

| Piece | Role |
|-------|------|
| MUSE `data/muse_ar_en.txt` | ~31k AR→EN glosses injected as prompt hints |
| Arabic normalize | Strip tashkeel, unify alef / ة / ى; strip clitics on miss |
| Mode temperatures | Translate/transliterate 0.1; word-by-word 0.15; explain 0.3 |
| Word-by-word UI | 3-column table (Source \| Translit \| Gloss) + full translation |

Embedding RAG is intentionally avoided so ALLaM can keep ~6 GB unified memory.

## Modes

Translate · Explain word · Translate + explain · Transliteration · Word-by-word

CLI: `python3 cli.py -m word-by-word 'مرحبا بالعالم'`

## Theme

`assets/theme.css` — background `#0d0d0d`, surfaces `#161616`, antique gold `#b5a88c` (outlines `#6e6452`).

## Config

`config.ini`: `[server]`, `[model]`, `[generation]`, `[paths]` (`lexicon`, `theme_css`).

## Architecture

```
translator.sh → llama-server (ALLaM)
                     ↑
GTK GUI ← prompts + MUSE lexicon hints
```
