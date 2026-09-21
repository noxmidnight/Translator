#include "config.h"
#include "lexicon.h"
#include "llama_client.h"
#include "prompts.h"

#include <curl/curl.h>
#include <gtk/gtk.h>

#include <atomic>
#include <array>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct AppWidgets {
  GtkWidget* window = nullptr;
  GtkWidget* direction = nullptr;
  GtkWidget* mode = nullptr;
  GtkTextBuffer* input_buf = nullptr;
  GtkTextBuffer* output_buf = nullptr;
  GtkWidget* input_view = nullptr;
  GtkWidget* output_view = nullptr;
  GtkWidget* output_text_scroll = nullptr;
  GtkWidget* output_tree_scroll = nullptr;
  GtkWidget* output_text_frame = nullptr;
  GtkWidget* output_tree_frame = nullptr;
  GtkWidget* tree_view = nullptr;
  GtkListStore* tree_store = nullptr;
  GtkWidget* full_translation = nullptr;
  GtkWidget* run_btn = nullptr;
  GtkWidget* status = nullptr;
};

struct AppState {
  AppConfig config;
  std::unique_ptr<LlamaClient> client;
  Lexicon lexicon;
  AppWidgets ui;
  std::atomic<bool> busy{false};
};

AppState* g_app = nullptr;

enum {
  COL_SOURCE = 0,
  COL_TRANSLIT = 1,
  COL_GLOSS = 2,
  N_COLS
};

std::string buffer_text(GtkTextBuffer* buf) {
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buf, &start, &end);
  gchar* raw = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
  std::string text = raw ? raw : "";
  g_free(raw);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

void set_status(const std::string& msg) {
  if (g_app && g_app->ui.status) {
    gtk_label_set_text(GTK_LABEL(g_app->ui.status), msg.c_str());
  }
}

Direction current_direction() {
  const gint idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_app->ui.direction));
  if (idx == 0) return Direction::ArToEn;
  if (idx == 1) return Direction::EnToAr;
  return Direction::Auto;
}

Mode current_mode() {
  const gint idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_app->ui.mode));
  switch (idx) {
    case 1:
      return Mode::ExplainWord;
    case 2:
      return Mode::TranslateAndExplain;
    case 3:
      return Mode::Transliterate;
    case 4:
      return Mode::WordByWord;
    default:
      return Mode::Translate;
  }
}

void apply_rtl(GtkWidget* view, bool rtl) {
  gtk_widget_set_direction(view, rtl ? GTK_TEXT_DIR_RTL : GTK_TEXT_DIR_LTR);
}

void sync_input_direction() {
  const Direction d = current_direction();
  const std::string text = buffer_text(g_app->ui.input_buf);
  const bool rtl =
      d == Direction::ArToEn || (d == Direction::Auto && contains_arabic(text));
  apply_rtl(g_app->ui.input_view, rtl);
}

void show_word_by_word_view(bool show) {
  if (show) {
    gtk_widget_hide(g_app->ui.output_text_frame);
    gtk_widget_show(g_app->ui.output_tree_frame);
    gtk_widget_show(g_app->ui.full_translation);
  } else {
    gtk_widget_show(g_app->ui.output_text_frame);
    gtk_widget_hide(g_app->ui.output_tree_frame);
    gtk_widget_hide(g_app->ui.full_translation);
  }
}

void on_mode_changed(GtkComboBox*, gpointer) {
  show_word_by_word_view(current_mode() == Mode::WordByWord);
}

gboolean refresh_status_idle(gpointer) {
  if (!g_app || g_app->busy.load()) return G_SOURCE_REMOVE;
  const bool ok = g_app->client->health_check();
  std::string msg;
  if (ok) {
    msg = "Connected · " + g_app->config.host + ":" +
          std::to_string(g_app->config.port);
    if (!g_app->lexicon.empty()) {
      msg += " · lexicon " + std::to_string(g_app->lexicon.size()) + " entries";
    }
  } else {
    msg = "llama-server offline — run ./scripts/translator.sh";
  }
  set_status(msg);
  return G_SOURCE_REMOVE;
}

gboolean poll_status(gpointer) {
  g_idle_add(refresh_status_idle, nullptr);
  return G_SOURCE_CONTINUE;
}

static std::string trim_ws(const std::string& s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  const auto b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

struct ParsedWbw {
  std::vector<std::array<std::string, 3>> rows;
  std::string full;
  bool ok = false;
};

std::string strip_wbw_prefix(std::string line) {
  // Drop common markdown / numbering prefixes: "1.", "1)", "-", "*", "•"
  line = trim_ws(line);
  while (!line.empty()) {
    if (line[0] == '-' || line[0] == '*' || line[0] == '\xE2') {
      // bullet or possible UTF-8 bullet — trim one char / skip utf8 bullet
      if (static_cast<unsigned char>(line[0]) == 0xE2 && line.size() >= 3) {
        line = trim_ws(line.substr(3));
      } else {
        line = trim_ws(line.substr(1));
      }
      continue;
    }
    size_t i = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
    if (i > 0 && i < line.size() && (line[i] == '.' || line[i] == ')')) {
      line = trim_ws(line.substr(i + 1));
      continue;
    }
    break;
  }
  return line;
}

ParsedWbw parse_word_by_word(const std::string& content) {
  ParsedWbw out;
  std::istringstream iss(content);
  std::string line;
  while (std::getline(iss, line)) {
    line = strip_wbw_prefix(line);
    if (line.empty()) continue;
    std::string lower_copy = line;
    for (char& c : lower_copy) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    // Accept "Full translation:" / "full translation :" variants
    const auto ft = lower_copy.find("full translation");
    if (ft != std::string::npos) {
      auto pos = line.find(':');
      if (pos != std::string::npos) out.full = trim_ws(line.substr(pos + 1));
      continue;
    }
    if (line.find('|') == std::string::npos) continue;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= line.size()) {
      auto bar = line.find('|', start);
      if (bar == std::string::npos) {
        parts.push_back(trim_ws(line.substr(start)));
        break;
      }
      parts.push_back(trim_ws(line.substr(start, bar - start)));
      start = bar + 1;
    }
    if (parts.size() >= 3) {
      if (parts[0].empty()) continue;  // skip blank/junk rows
      const std::string& mid = parts[1];
      const std::string& gloss = parts[2];
      // Recover when the model dumps a gloss-list into the transliteration column.
      if (mid.find(';') != std::string::npos || mid.find(" / ") != std::string::npos) {
        size_t cut = mid.find(';');
        if (cut == std::string::npos) cut = mid.find(" / ");
        std::string first =
            trim_ws(cut == std::string::npos ? mid : mid.substr(0, cut));
        // For EN→AR layout col2 is Arabic — keep first alternative only.
        out.rows.push_back({parts[0], first.empty() ? gloss : first,
                            gloss.find(" / ") != std::string::npos
                                ? trim_ws(gloss.substr(0, gloss.find(" / ")))
                                : gloss});
      } else {
        out.rows.push_back({parts[0], parts[1], parts[2]});
      }
    } else if (parts.size() == 2) {
      if (parts[0].empty()) continue;
      out.rows.push_back({parts[0], "", parts[1]});
    }
  }
  out.ok = !out.rows.empty();
  return out;
}

void clear_tree() {
  gtk_list_store_clear(g_app->ui.tree_store);
  gtk_label_set_text(GTK_LABEL(g_app->ui.full_translation), "");
}

void fill_tree(const ParsedWbw& parsed) {
  clear_tree();
  for (const auto& row : parsed.rows) {
    GtkTreeIter iter;
    gtk_list_store_append(g_app->ui.tree_store, &iter);
    gtk_list_store_set(g_app->ui.tree_store, &iter, COL_SOURCE, row[0].c_str(),
                       COL_TRANSLIT, row[1].c_str(), COL_GLOSS, row[2].c_str(),
                       -1);
  }
  if (!parsed.full.empty()) {
    gtk_label_set_text(GTK_LABEL(g_app->ui.full_translation),
                       ("Full translation: " + parsed.full).c_str());
  }
}

struct WorkerResult {
  ChatResult result;
  Mode mode = Mode::Translate;
};

gboolean on_worker_done(gpointer data) {
  auto* wr = static_cast<WorkerResult*>(data);
  g_app->busy = false;
  gtk_widget_set_sensitive(g_app->ui.run_btn, TRUE);

  if (!wr->result.ok) {
    set_status("Error");
    GtkWidget* dialog = gtk_message_dialog_new(
        GTK_WINDOW(g_app->ui.window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK, "%s", wr->result.error.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_idle_add(refresh_status_idle, nullptr);
  } else if (wr->mode == Mode::WordByWord) {
    auto parsed = parse_word_by_word(wr->result.content);
    if (parsed.ok) {
      show_word_by_word_view(true);
      fill_tree(parsed);
      // Keep raw text as backup in buffer for Copy
      gtk_text_buffer_set_text(g_app->ui.output_buf, wr->result.content.c_str(),
                               -1);
    } else {
      show_word_by_word_view(false);
      gtk_text_buffer_set_text(g_app->ui.output_buf, wr->result.content.c_str(),
                               -1);
    }
    set_status("Done");
  } else {
    show_word_by_word_view(false);
    gtk_text_buffer_set_text(g_app->ui.output_buf, wr->result.content.c_str(),
                             -1);
    apply_rtl(g_app->ui.output_view, contains_arabic(wr->result.content));
    set_status("Done");
  }

  g_timeout_add_seconds(2, [](gpointer) -> gboolean {
    g_idle_add(refresh_status_idle, nullptr);
    return G_SOURCE_REMOVE;
  }, nullptr);

  delete wr;
  return G_SOURCE_REMOVE;
}

void on_run_clicked(GtkButton*, gpointer) {
  if (g_app->busy.load()) return;
  const std::string text = buffer_text(g_app->ui.input_buf);
  if (text.empty()) {
    GtkWidget* dialog = gtk_message_dialog_new(
        GTK_WINDOW(g_app->ui.window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK, "Enter text to translate or explain.");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return;
  }

  const Mode mode = current_mode();
  std::string rag;
  if (mode == Mode::WordByWord || mode == Mode::ExplainWord ||
      mode == Mode::Translate || mode == Mode::TranslateAndExplain) {
    rag = g_app->lexicon.build_rag_context(text);
  }
  // Translate: only inject if we have hits (build_rag_context already empty if none)
  const auto prompt = build_prompt(mode, current_direction(), text, rag);
  const auto gen = generation_params_for(mode);

  g_app->busy = true;
  gtk_widget_set_sensitive(g_app->ui.run_btn, FALSE);
  set_status("Generating…");
  gtk_text_buffer_set_text(g_app->ui.output_buf, "", -1);
  clear_tree();

  std::thread([system = prompt.system, user = prompt.user, mode, gen]() {
    auto* wr = new WorkerResult;
    wr->mode = mode;
    wr->result = g_app->client->chat(system, user, gen.temperature, gen.max_tokens);
    g_idle_add(on_worker_done, wr);
  }).detach();
}

void on_copy_clicked(GtkButton*, gpointer) {
  std::string text;
  if (current_mode() == Mode::WordByWord &&
      gtk_widget_get_visible(g_app->ui.output_tree_frame)) {
    text = buffer_text(g_app->ui.output_buf);
    const gchar* full = gtk_label_get_text(GTK_LABEL(g_app->ui.full_translation));
    if (full && *full && text.find("Full translation:") == std::string::npos) {
      if (!text.empty() && text.back() != '\n') text += '\n';
      text += full;
    }
  } else {
    text = buffer_text(g_app->ui.output_buf);
  }
  if (text.empty()) return;
  GtkClipboard* clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text(clip, text.c_str(), -1);
  set_status("Copied to clipboard");
}

void on_clear_clicked(GtkButton*, gpointer) {
  gtk_text_buffer_set_text(g_app->ui.input_buf, "", -1);
  gtk_text_buffer_set_text(g_app->ui.output_buf, "", -1);
  clear_tree();
  sync_input_direction();
}

void on_direction_changed(GtkComboBox*, gpointer) { sync_input_direction(); }

void on_input_changed(GtkTextBuffer*, gpointer) { sync_input_direction(); }

GtkWidget* wrap_gold_frame(GtkWidget* child) {
  GtkWidget* frame = gtk_frame_new(nullptr);
  gtk_style_context_add_class(gtk_widget_get_style_context(frame), "gold-frame");
  gtk_container_add(GTK_CONTAINER(frame), child);
  gtk_container_set_border_width(GTK_CONTAINER(frame), 8);
  return frame;
}

GtkWidget* make_scrolled_text(GtkTextBuffer** out_buf, GtkWidget** out_view,
                              int height) {
  GtkWidget* view = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 12);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 12);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 10);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 10);
  gtk_widget_set_size_request(view, -1, height);
  GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_NONE);
  gtk_widget_set_margin_start(scroll, 4);
  gtk_widget_set_margin_end(scroll, 4);
  gtk_widget_set_margin_top(scroll, 4);
  gtk_widget_set_margin_bottom(scroll, 4);
  gtk_container_add(GTK_CONTAINER(scroll), view);
  *out_buf = buf;
  *out_view = view;
  return scroll;
}

void load_theme_css(const std::string& path) {
  GtkCssProvider* provider = gtk_css_provider_new();
  GError* err = nullptr;
  if (!gtk_css_provider_load_from_path(provider, path.c_str(), &err)) {
    if (err) {
      g_warning("CSS load failed: %s", err->message);
      g_error_free(err);
    }
    g_object_unref(provider);
    return;
  }
  gtk_style_context_add_provider_for_screen(
      gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

void build_ui(GtkApplication* app) {
  load_theme_css(resolve_path(g_app->config.theme_css));

  g_app->ui.window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(g_app->ui.window),
                       "Arabic ↔ English Translator");
  gtk_window_set_default_size(GTK_WINDOW(g_app->ui.window), 860, 700);
  gtk_container_set_border_width(GTK_CONTAINER(g_app->ui.window), 20);
  gtk_style_context_add_class(gtk_widget_get_style_context(g_app->ui.window),
                              "translator-root");
  {
    const std::string icon_path = resolve_path("assets/translator.png");
    if (g_file_test(icon_path.c_str(), G_FILE_TEST_IS_REGULAR)) {
      GError* icon_err = nullptr;
      gtk_window_set_icon_from_file(GTK_WINDOW(g_app->ui.window), icon_path.c_str(),
                                    &icon_err);
      if (icon_err) g_error_free(icon_err);
    }
  }

  GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_add(GTK_CONTAINER(g_app->ui.window), vbox);

  GtkWidget* title = gtk_label_new("Translator");
  gtk_style_context_add_class(gtk_widget_get_style_context(title), "label-title");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(title, 4);
  gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 0);

  GtkWidget* controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_bottom(controls, 4);
  gtk_box_pack_start(GTK_BOX(vbox), controls, FALSE, FALSE, 0);

  GtkWidget* dir_lbl = gtk_label_new("Direction");
  gtk_style_context_add_class(gtk_widget_get_style_context(dir_lbl),
                              "label-section");
  gtk_box_pack_start(GTK_BOX(controls), dir_lbl, FALSE, FALSE, 0);
  g_app->ui.direction = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app->ui.direction),
                                 "Arabic → English");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app->ui.direction),
                                 "English → Arabic");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app->ui.direction), "Auto");
  gtk_combo_box_set_active(GTK_COMBO_BOX(g_app->ui.direction), 0);
  g_signal_connect(g_app->ui.direction, "changed",
                   G_CALLBACK(on_direction_changed), nullptr);
  gtk_box_pack_start(GTK_BOX(controls), g_app->ui.direction, FALSE, FALSE, 0);

  GtkWidget* mode_lbl = gtk_label_new("Mode");
  gtk_style_context_add_class(gtk_widget_get_style_context(mode_lbl),
                              "label-section");
  gtk_box_pack_start(GTK_BOX(controls), mode_lbl, FALSE, FALSE, 0);
  g_app->ui.mode = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app->ui.mode), "Translate");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app->ui.mode),
                                 "Explain word");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app->ui.mode),
                                 "Translate + explain");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app->ui.mode),
                                 "Transliteration");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app->ui.mode),
                                 "Word-by-word");
  gtk_combo_box_set_active(GTK_COMBO_BOX(g_app->ui.mode), 0);
  g_signal_connect(g_app->ui.mode, "changed", G_CALLBACK(on_mode_changed),
                   nullptr);
  gtk_box_pack_start(GTK_BOX(controls), g_app->ui.mode, FALSE, FALSE, 0);

  GtkWidget* in_lbl = gtk_label_new("Input");
  gtk_style_context_add_class(gtk_widget_get_style_context(in_lbl),
                              "label-section");
  gtk_widget_set_halign(in_lbl, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(vbox), in_lbl, FALSE, FALSE, 0);

  GtkWidget* input_scroll =
      make_scrolled_text(&g_app->ui.input_buf, &g_app->ui.input_view, 150);
  g_signal_connect(g_app->ui.input_buf, "changed", G_CALLBACK(on_input_changed),
                   nullptr);
  gtk_box_pack_start(GTK_BOX(vbox), wrap_gold_frame(input_scroll), TRUE, TRUE, 0);

  GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_margin_top(buttons, 4);
  gtk_widget_set_margin_bottom(buttons, 4);
  gtk_box_pack_start(GTK_BOX(vbox), buttons, FALSE, FALSE, 0);

  g_app->ui.run_btn = gtk_button_new_with_label("Run");
  gtk_style_context_add_class(gtk_widget_get_style_context(g_app->ui.run_btn),
                              "primary-run");
  g_signal_connect(g_app->ui.run_btn, "clicked", G_CALLBACK(on_run_clicked),
                   nullptr);
  gtk_box_pack_start(GTK_BOX(buttons), g_app->ui.run_btn, FALSE, FALSE, 0);

  GtkWidget* copy_btn = gtk_button_new_with_label("Copy output");
  g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_clicked), nullptr);
  gtk_box_pack_start(GTK_BOX(buttons), copy_btn, FALSE, FALSE, 0);

  GtkWidget* clear_btn = gtk_button_new_with_label("Clear");
  g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_clicked), nullptr);
  gtk_box_pack_start(GTK_BOX(buttons), clear_btn, FALSE, FALSE, 0);

  GtkWidget* out_lbl = gtk_label_new("Output");
  gtk_style_context_add_class(gtk_widget_get_style_context(out_lbl),
                              "label-section");
  gtk_widget_set_halign(out_lbl, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(vbox), out_lbl, FALSE, FALSE, 0);

  // Text output
  g_app->ui.output_text_scroll =
      make_scrolled_text(&g_app->ui.output_buf, &g_app->ui.output_view, 200);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(g_app->ui.output_view), FALSE);
  g_app->ui.output_text_frame = wrap_gold_frame(g_app->ui.output_text_scroll);
  gtk_box_pack_start(GTK_BOX(vbox), g_app->ui.output_text_frame, TRUE, TRUE, 0);

  // Word-by-word tree
  g_app->ui.tree_store =
      gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  g_app->ui.tree_view =
      gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_app->ui.tree_store));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(g_app->ui.tree_view), TRUE);
  gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(g_app->ui.tree_view),
                               GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);

  const char* titles[] = {"Source", "Transliteration", "Gloss"};
  for (int i = 0; i < N_COLS; ++i) {
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "ypad", 8, "xpad", 12, nullptr);
    GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes(
        titles[i], renderer, "text", i, nullptr);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_app->ui.tree_view), col);
  }

  g_app->ui.output_tree_scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_app->ui.output_tree_scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(g_app->ui.output_tree_scroll, -1, 200);
  gtk_widget_set_margin_start(g_app->ui.output_tree_scroll, 4);
  gtk_widget_set_margin_end(g_app->ui.output_tree_scroll, 4);
  gtk_widget_set_margin_top(g_app->ui.output_tree_scroll, 4);
  gtk_widget_set_margin_bottom(g_app->ui.output_tree_scroll, 4);
  gtk_container_add(GTK_CONTAINER(g_app->ui.output_tree_scroll),
                    g_app->ui.tree_view);

  g_app->ui.output_tree_frame = wrap_gold_frame(g_app->ui.output_tree_scroll);
  gtk_box_pack_start(GTK_BOX(vbox), g_app->ui.output_tree_frame, TRUE, TRUE, 0);

  g_app->ui.full_translation = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(g_app->ui.full_translation), TRUE);
  gtk_label_set_xalign(GTK_LABEL(g_app->ui.full_translation), 0.0f);
  gtk_style_context_add_class(
      gtk_widget_get_style_context(g_app->ui.full_translation),
      "label-full-translation");
  gtk_widget_set_margin_start(g_app->ui.full_translation, 8);
  gtk_widget_set_margin_end(g_app->ui.full_translation, 8);
  gtk_box_pack_start(GTK_BOX(vbox), g_app->ui.full_translation, FALSE, FALSE, 0);

  g_app->ui.status = gtk_label_new("Checking llama-server…");
  gtk_style_context_add_class(gtk_widget_get_style_context(g_app->ui.status),
                              "label-muted");
  gtk_widget_set_halign(g_app->ui.status, GTK_ALIGN_START);
  gtk_widget_set_margin_top(g_app->ui.status, 6);
  gtk_box_pack_start(GTK_BOX(vbox), g_app->ui.status, FALSE, FALSE, 0);

  sync_input_direction();
  gtk_widget_show_all(g_app->ui.window);
  show_word_by_word_view(false);
  g_idle_add(refresh_status_idle, nullptr);
  g_timeout_add_seconds(5, poll_status, nullptr);
}

void on_activate(GtkApplication* app, gpointer) { build_ui(app); }

}  // namespace

int main(int argc, char** argv) {
  curl_global_init(CURL_GLOBAL_DEFAULT);

  AppState state;
  g_app = &state;
  load_config(config_search_path(), state.config);
  state.client = std::make_unique<LlamaClient>(state.config);

  const std::string lex_path = resolve_path(state.config.lexicon_path);
  if (!state.lexicon.load(lex_path)) {
    g_warning("Lexicon not loaded from %s (word-by-word RAG disabled)",
              lex_path.c_str());
  }

  GtkApplication* app =
      gtk_application_new("com.nox.translator", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
  const int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);

  curl_global_cleanup();
  g_app = nullptr;
  return status;
}
