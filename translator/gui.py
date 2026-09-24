"""Tkinter GUI fallback — matte black + gold, lexical RAG."""

from __future__ import annotations

import threading
import tkinter as tk
from tkinter import messagebox, ttk

from translator.config import load_settings
from translator.lexicon import Lexicon, default_lexicon_path
from translator.llama_client import LlamaClient
from translator.prompts import Direction, Mode, build_prompt, generation_params_for

BG = "#0d0d0d"
SURFACE = "#161616"
GOLD = "#b5a88c"
GOLD_DIM = "#6e6452"
TEXT = "#e6e2d8"
MUTED = "#9a917c"


class TranslatorApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Arabic ↔ English Translator")
        self.geometry("860x700")
        self.minsize(680, 520)
        self.configure(bg=BG)

        self.settings = load_settings()
        self.client = LlamaClient(
            host=self.settings.host,
            port=self.settings.port,
            model=self.settings.model_name,
            temperature=self.settings.temperature,
            max_tokens=self.settings.max_tokens,
        )
        self.lexicon = Lexicon()
        self.lexicon.load(default_lexicon_path(self.settings.lexicon_path))
        self._busy = False

        self.direction = tk.StringVar(value="Arabic → English")
        self.mode = tk.StringVar(value="Translate")
        self.status = tk.StringVar(value="Checking llama-server…")

        self._style()
        self._build_ui()
        self.after(200, self._refresh_status)
        self.after(5000, self._poll_status)

    def _style(self) -> None:
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure(".", background=BG, foreground=TEXT, fieldbackground=SURFACE)
        style.configure("TFrame", background=BG)
        style.configure("TLabel", background=BG, foreground=TEXT)
        style.configure("Title.TLabel", background=BG, foreground=GOLD, font=("Sans", 18, "bold"))
        style.configure("Gold.TLabel", background=BG, foreground=GOLD, font=("Sans", 10, "bold"))
        style.configure("Muted.TLabel", background=BG, foreground=MUTED, font=("Sans", 9))
        style.configure(
            "TCombobox",
            fieldbackground=SURFACE,
            background=SURFACE,
            foreground=TEXT,
            arrowcolor=GOLD,
        )
        style.configure(
            "TButton",
            background=SURFACE,
            foreground=TEXT,
            bordercolor=GOLD,
            padding=(12, 8),
        )
        style.configure(
            "Run.TButton",
            background="#1f1a0a",
            foreground=GOLD,
            padding=(16, 8),
            font=("Sans", 10, "bold"),
        )
        style.map(
            "Run.TButton",
            background=[("active", "#2a2410")],
            foreground=[("active", "#d0c6b0")],
        )

    def _framed_text(self, parent: ttk.Frame, height: int) -> tk.Text:
        wrap = tk.Frame(parent, bg=GOLD_DIM, bd=0, highlightthickness=0)
        wrap.pack(fill=tk.BOTH, expand=True, padx=2, pady=4)
        inner = tk.Frame(wrap, bg=SURFACE, bd=0)
        inner.pack(fill=tk.BOTH, expand=True, padx=1, pady=1)
        text = tk.Text(
            inner,
            height=height,
            wrap=tk.WORD,
            font=("Sans", 12),
            bg=SURFACE,
            fg=TEXT,
            insertbackground=GOLD,
            relief=tk.FLAT,
            padx=12,
            pady=10,
            highlightthickness=0,
            bd=0,
        )
        text.pack(fill=tk.BOTH, expand=True)
        return text

    def _build_ui(self) -> None:
        root = ttk.Frame(self, padding=20)
        root.pack(fill=tk.BOTH, expand=True)

        ttk.Label(root, text="Translator", style="Title.TLabel").pack(anchor=tk.W, pady=(0, 8))

        controls = ttk.Frame(root)
        controls.pack(fill=tk.X, pady=8)
        ttk.Label(controls, text="Direction", style="Gold.TLabel").grid(row=0, column=0, sticky=tk.W)
        dir_box = ttk.Combobox(
            controls,
            textvariable=self.direction,
            state="readonly",
            width=22,
            values=["Arabic → English", "English → Arabic", "Auto"],
        )
        dir_box.grid(row=0, column=1, sticky=tk.W, padx=(8, 20))
        dir_box.bind("<<ComboboxSelected>>", lambda _e: self._sync_input_dir())

        ttk.Label(controls, text="Mode", style="Gold.TLabel").grid(row=0, column=2, sticky=tk.W)
        mode_box = ttk.Combobox(
            controls,
            textvariable=self.mode,
            state="readonly",
            width=22,
            values=[
                "Translate",
                "Explain word",
                "Translate + explain",
                "Transliteration",
                "Word-by-word",
            ],
        )
        mode_box.grid(row=0, column=3, sticky=tk.W, padx=(8, 0))

        ttk.Label(root, text="Input", style="Gold.TLabel").pack(anchor=tk.W, pady=(8, 0))
        self.input = self._framed_text(root, 8)
        self.input.tag_configure("rtl", justify=tk.RIGHT)
        self.input.bind("<KeyRelease>", lambda _e: self._sync_input_dir())

        buttons = ttk.Frame(root)
        buttons.pack(fill=tk.X, pady=10)
        self.run_btn = ttk.Button(buttons, text="Run", style="Run.TButton", command=self._on_run)
        self.run_btn.pack(side=tk.LEFT)
        ttk.Button(buttons, text="Copy output", command=self._copy_output).pack(side=tk.LEFT, padx=8)
        ttk.Button(buttons, text="Clear", command=self._clear).pack(side=tk.LEFT)

        ttk.Label(root, text="Output", style="Gold.TLabel").pack(anchor=tk.W)
        self.output = self._framed_text(root, 10)
        self.output.tag_configure("rtl", justify=tk.RIGHT)

        ttk.Label(root, textvariable=self.status, style="Muted.TLabel").pack(
            anchor=tk.W, pady=(8, 0)
        )
        self._sync_input_dir()

    def _direction_enum(self) -> Direction:
        label = self.direction.get()
        if label.startswith("Arabic"):
            return Direction.AR_TO_EN
        if label.startswith("English"):
            return Direction.EN_TO_AR
        return Direction.AUTO

    def _mode_enum(self) -> Mode:
        label = self.mode.get()
        if label.startswith("Explain"):
            return Mode.EXPLAIN
        if label.startswith("Transliteration"):
            return Mode.TRANSLITERATE
        if label.startswith("Word-by-word"):
            return Mode.WORD_BY_WORD
        if "explain" in label.lower():
            return Mode.BOTH
        return Mode.TRANSLATE

    def _sync_input_dir(self) -> None:
        text = self.input.get("1.0", tk.END)
        d = self._direction_enum()
        arabic_source = d is Direction.AR_TO_EN or (
            d is Direction.AUTO and any("\u0600" <= c <= "\u06ff" for c in text)
        )
        if arabic_source:
            self.input.tag_add("rtl", "1.0", tk.END)
        else:
            self.input.tag_remove("rtl", "1.0", tk.END)

    def _refresh_status(self) -> None:
        ok = self.client.health_check()
        if ok:
            msg = f"Connected · {self.settings.host}:{self.settings.port}"
            if self.lexicon.size:
                msg += f" · lexicon {self.lexicon.size} entries"
            self.status.set(msg)
        else:
            self.status.set(
                f"llama-server offline — run ./scripts/translator.sh "
                f"(expecting {self.settings.host}:{self.settings.port})"
            )

    def _poll_status(self) -> None:
        if not self._busy:
            self._refresh_status()
        self.after(5000, self._poll_status)

    def _on_run(self) -> None:
        if self._busy:
            return
        text = self.input.get("1.0", tk.END).strip()
        if not text:
            messagebox.showinfo("Empty", "Enter text to translate or explain.")
            return

        mode = self._mode_enum()
        rag = ""
        if mode in (Mode.WORD_BY_WORD, Mode.EXPLAIN, Mode.TRANSLATE, Mode.BOTH):
            rag = self.lexicon.build_rag_context(text)
        system, user = build_prompt(mode, self._direction_enum(), text, rag)
        temp, max_tok = generation_params_for(mode)

        self._busy = True
        self.run_btn.configure(state=tk.DISABLED)
        self.status.set("Generating…")
        self.output.delete("1.0", tk.END)

        def worker() -> None:
            result = self.client.chat(system, user, temperature=temp, max_tokens=max_tok)
            self.after(0, lambda: self._on_result(result))

        threading.Thread(target=worker, daemon=True).start()

    def _on_result(self, result) -> None:
        self._busy = False
        self.run_btn.configure(state=tk.NORMAL)
        if not result.ok:
            self.status.set("Error")
            messagebox.showerror("Request failed", result.error)
            self._refresh_status()
            return

        self.output.insert(tk.END, result.content)
        if any("\u0600" <= c <= "\u06ff" for c in result.content):
            self.output.tag_add("rtl", "1.0", tk.END)
        else:
            self.output.tag_remove("rtl", "1.0", tk.END)
        self.status.set("Done")
        self.after(1500, self._refresh_status)

    def _copy_output(self) -> None:
        text = self.output.get("1.0", tk.END).strip()
        if not text:
            return
        self.clipboard_clear()
        self.clipboard_append(text)
        self.status.set("Copied to clipboard")

    def _clear(self) -> None:
        self.input.delete("1.0", tk.END)
        self.output.delete("1.0", tk.END)
        self._sync_input_dir()


def main() -> None:
    app = TranslatorApp()
    app.mainloop()


if __name__ == "__main__":
    main()
