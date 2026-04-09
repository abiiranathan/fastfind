/* FastFind – an "Everything"-style instant search for Linux
 * GTK4 + GListStore + GtkColumnView + background indexer
 */
#define _POSIX_C_SOURCE 200809L
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

#include "fileitem.h"
#include "indexer.h"

/* ── CSS ────────────────────────────────────────────────────────── */
static const char* APP_CSS =
    /* Window: solid opaque background, no transparency */
    "window {"
    "  background-color: #282a36;"
    "}"

    /* Toolbar: solid, fully opaque background */
    ".toolbar {"
    "  padding: 6px 10px;"
    "  min-height: 48px;"
    "  background-color: @window_bg_color;"
    "  border-bottom: 1px solid @borders;"
    "}"

    /* Force all direct toolbar children to center vertically */
    ".toolbar > * {"
    "  margin-top: 0;"
    "  margin-bottom: 0;"
    "}"

    /* App title label */
    ".app-title {"
    "  font-weight: bold;"
    "  font-size: 14px;"
    "  color: @window_fg_color;"
    "  opacity: 1.0;"
    "}"

    /* Search entry: solid background, clearly readable */
    ".search-entry {"
    "  font-size: 13px;"
    "  min-height: 32px;"
    "  border-radius: 6px;"
    "  background-color: @entry_bg_color;"
    "  color: @entry_fg_color;"
    "  caret-color: @accent_color;"
    "  border: 1px solid alpha(@borders, 0.8);"
    "  padding-left: 8px;"
    "  padding-right: 8px;"
    "}"
    ".search-entry:focus {"
    "  border-color: @accent_color;"
    "  box-shadow: 0 0 0 2px alpha(@accent_color, 0.25);"
    "}"

    /* Toolbar buttons: solid background, high contrast */
    ".toolbar-btn {"
    "  min-height: 32px;"
    "  padding: 0 12px;"
    "  border-radius: 6px;"
    "  font-size: 13px;"
    "  color: @window_fg_color;"
    "  background-color: @headerbar_bg_color;"
    "  border: 1px solid @borders;"
    "}"
    ".toolbar-btn:hover {"
    "  background-color: @accent_color;"
    "  color: @window_bg_color;"
    "}"
    ".toolbar-btn:active {"
    "  background-color: shade(@accent_color, 0.85);"
    "  color: @window_bg_color;"
    "}"

    /* Status bar: solid, clearly separated, fully opaque */
    ".status-bar {"
    "  padding: 3px 12px;"
    "  min-height: 24px;"
    "  background-color: @window_bg_color;"
    "  border-top: 1px solid @borders;"
    "}"
    ".status-label {"
    "  font-size: 12px;"
    "  color: @window_fg_color;"
    "  opacity: 0.75;"
    "}"

    /* Column view rows */
    ".file-name {"
    "  font-weight: 600;"
    "  color: @window_fg_color;"
    "}"
    ".file-dir {"
    /* Slightly dimmed but still clearly readable — not insensitive-faint */
    "  font-size: 12px;"
    "  color: mix(@window_fg_color, @window_bg_color, 0.35);"
    "}"
    ".dir-row {"
    "  color: #6b8fe8;"
    "  font-weight: 600;"
    "}";

/* ── App state ──────────────────────────────────────────────────── */
typedef struct {
    GtkApplication* app;

    /* Window */
    GtkWidget* window;
    GtkWidget* search_entry;
    GtkWidget* column_view;
    GtkWidget* status_label;
    GtkWidget* spinner;
    GtkWidget* index_btn;
    GtkWidget* path_btn;
    GtkWidget* count_label;
    GtkWidget* root_label;

    /* Models */
    GListStore* store; /* all indexed items */
    GtkFilterListModel* filter_model;
    GtkSortListModel* sort_model;
    GtkCustomFilter* filter;

    /* Indexer */
    Indexer* indexer;
    char* index_root;
    guint total_indexed;
    gboolean indexing;
} AppState;

static AppState* g_app = NULL;

/* ── Search filter ──────────────────────────────────────────────── */
static gboolean filter_func(gpointer item, gpointer user_data) {
    AppState* app     = user_data;
    const char* query = gtk_editable_get_text(GTK_EDITABLE(app->search_entry));
    if (!query || *query == '\0') return TRUE;

    FileItem* fi     = FILE_ITEM(item);
    const char* name = file_item_get_name(fi);
    const char* path = file_item_get_path(fi);

    /* Case-insensitive substring in name or path */
    char* lq    = g_utf8_casefold(query, -1);
    char* lname = g_utf8_casefold(name, -1);
    char* lpath = g_utf8_casefold(path, -1);

    gboolean match = strstr(lname, lq) != NULL || strstr(lpath, lq) != NULL;

    g_free(lq);
    g_free(lname);
    g_free(lpath);
    return match;
}

/* ── Status bar update ──────────────────────────────────────────── */
static void update_status(AppState* app) {
    guint total       = app->total_indexed;
    guint filtered    = g_list_model_get_n_items(G_LIST_MODEL(app->sort_model));
    const char* query = gtk_editable_get_text(GTK_EDITABLE(app->search_entry));

    char buf[256];
    if (app->indexing) {
        g_snprintf(buf, sizeof(buf), "Indexing %s …  %'u files scanned", app->index_root, total);
    } else if (!query || *query == '\0') {
        g_snprintf(buf, sizeof(buf), "%'u files indexed in %s", total, app->index_root);
    } else {
        g_snprintf(buf, sizeof(buf), "%'u of %'u results for \"%s\"", filtered, total, query);
    }
    gtk_label_set_text(GTK_LABEL(app->status_label), buf);
}

/* ── Indexer callback (main thread) ────────────────────────────── */
static void on_batch_ready(GPtrArray* items, guint total, gboolean done, gpointer user_data) {
    AppState* app      = user_data;
    app->total_indexed = total;

    /* Append all items to the store in one batch */
    for (guint i = 0; i < items->len; i++) {
        g_list_store_append(app->store, g_ptr_array_index(items, i));
    }
    g_ptr_array_unref(items);

    if (done) {
        app->indexing = FALSE;
        gtk_spinner_stop(GTK_SPINNER(app->spinner));
        gtk_widget_set_visible(app->spinner, FALSE);
        gtk_button_set_label(GTK_BUTTON(app->index_btn), "Re-index");
        gtk_widget_set_sensitive(app->index_btn, TRUE);
    }

    /* Invalidate filter so visible count updates */
    gtk_filter_changed(GTK_FILTER(app->filter), GTK_FILTER_CHANGE_DIFFERENT);
    update_status(app);
}

/* ── Start / restart indexing ───────────────────────────────────── */
static void start_indexing(AppState* app, const char* root) {
    if (app->indexer) {
        indexer_cancel(app->indexer);
        app->indexer = NULL;
    }
    g_list_store_remove_all(app->store);
    g_free(app->index_root);
    app->index_root    = g_strdup(root);
    app->total_indexed = 0;
    app->indexing      = TRUE;

    char short_root[64];
    g_strlcpy(short_root, root, sizeof(short_root));
    gtk_label_set_text(GTK_LABEL(app->root_label), short_root);

    gtk_widget_set_visible(app->spinner, TRUE);
    gtk_spinner_start(GTK_SPINNER(app->spinner));
    gtk_button_set_label(GTK_BUTTON(app->index_btn), "Indexing…");
    gtk_widget_set_sensitive(app->index_btn, FALSE);

    app->indexer = indexer_new(root, on_batch_ready, app);
    indexer_start(app->indexer);
    update_status(app);
}

/* ── Search changed ─────────────────────────────────────────────── */
static void on_search_changed(GtkSearchEntry* entry, gpointer user_data) {
    (void)entry;
    AppState* app = user_data;
    gtk_filter_changed(GTK_FILTER(app->filter), GTK_FILTER_CHANGE_DIFFERENT);
    update_status(app);
}

/* ── Choose root dir ────────────────────────────────────────────── */
static void on_folder_chosen(GObject* source, GAsyncResult* res, gpointer user_data) {
    AppState* app = user_data;
    GFile* file   = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), res, NULL);
    if (!file) return;
    char* path = g_file_get_path(file);
    g_object_unref(file);
    if (path) {
        start_indexing(app, path);
        g_free(path);
    }
}

static void on_pick_folder(GtkButton* btn, gpointer user_data) {
    AppState* app = user_data;
    (void)btn;
    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Choose directory to index");
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(app->window), NULL, on_folder_chosen, app);
    g_object_unref(dlg);
}

static void on_reindex(GtkButton* btn, gpointer user_data) {
    (void)btn;
    AppState* app = user_data;
    start_indexing(app, app->index_root);
}

/* ── Open item in file manager ──────────────────────────────────── */
static void open_item(FileItem* fi) {
    const char* path = file_item_get_is_dir(fi) ? file_item_get_path(fi) : file_item_get_dir(fi);
    GFile* file      = g_file_new_for_path(path);
    g_app_info_launch_default_for_uri(g_file_get_uri(file), NULL, NULL);
    g_object_unref(file);
}

/* ── Copy path on Ctrl+C ────────────────────────────────────────── */
static void on_activate_item(GtkListView* lv, guint pos, gpointer user_data) {
    (void)lv;
    (void)user_data;
    AppState* app     = g_app;
    GListModel* model = G_LIST_MODEL(app->sort_model);
    FileItem* fi      = FILE_ITEM(g_list_model_get_item(model, pos));
    if (!fi) return;
    open_item(fi);
    g_object_unref(fi);
}

/* ── Column factory helpers ─────────────────────────────────────── */

/* ------ Name column ------ */
static void setup_name(GtkSignalListItemFactory* f, GtkListItem* item, gpointer d) {
    (void)f;
    (void)d;
    GtkWidget* box   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* icon  = gtk_image_new();
    GtkWidget* label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
    gtk_widget_add_css_class(label, "file-name");
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(item, box);
}
static void bind_name(GtkSignalListItemFactory* f, GtkListItem* item, gpointer d) {
    (void)f;
    (void)d;
    FileItem* fi     = FILE_ITEM(gtk_list_item_get_item(item));
    GtkWidget* box   = gtk_list_item_get_child(item);
    GtkWidget* icon  = gtk_widget_get_first_child(box);
    GtkWidget* label = gtk_widget_get_last_child(box);
    gtk_label_set_text(GTK_LABEL(label), file_item_get_name(fi));
    if (file_item_get_is_dir(fi)) {
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), "folder-symbolic");
        gtk_widget_add_css_class(label, "dir-row");
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), "text-x-generic-symbolic");
        gtk_widget_remove_css_class(label, "dir-row");
    }
}

/* ------ Path column ------ */
static void setup_path(GtkSignalListItemFactory* f, GtkListItem* item, gpointer d) {
    (void)f;
    (void)d;
    GtkWidget* label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_START);
    gtk_widget_add_css_class(label, "file-dir");
    gtk_list_item_set_child(item, label);
}
static void bind_path(GtkSignalListItemFactory* f, GtkListItem* item, gpointer d) {
    (void)f;
    (void)d;
    FileItem* fi     = FILE_ITEM(gtk_list_item_get_item(item));
    GtkWidget* label = gtk_list_item_get_child(item);
    gtk_label_set_text(GTK_LABEL(label), file_item_get_dir(fi));
}

/* ------ Size column ------ */
static void setup_size(GtkSignalListItemFactory* f, GtkListItem* item, gpointer d) {
    (void)f;
    (void)d;
    GtkWidget* label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 1);
    gtk_widget_set_halign(label, GTK_ALIGN_END);
    gtk_list_item_set_child(item, label);
}
static void bind_size(GtkSignalListItemFactory* f, GtkListItem* item, gpointer d) {
    (void)f;
    (void)d;
    FileItem* fi     = FILE_ITEM(gtk_list_item_get_item(item));
    GtkWidget* label = gtk_list_item_get_child(item);
    gtk_label_set_text(GTK_LABEL(label), file_item_get_size_str(fi));
}

/* ------ Type column ------ */
static void setup_type(GtkSignalListItemFactory* f, GtkListItem* item, gpointer d) {
    (void)f;
    (void)d;
    GtkWidget* label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_list_item_set_child(item, label);
}
static void bind_type(GtkSignalListItemFactory* f, GtkListItem* item, gpointer d) {
    (void)f;
    (void)d;
    FileItem* fi     = FILE_ITEM(gtk_list_item_get_item(item));
    GtkWidget* label = gtk_list_item_get_child(item);
    gtk_label_set_text(GTK_LABEL(label), file_item_get_type_str(fi));
}

/* ── Column helpers ─────────────────────────────────────────────── */
static GtkColumnViewColumn* make_column(const char* title, GCallback setup_cb, GCallback bind_cb, int fixed_width,
                                        gboolean expand, gpointer data) {
    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", setup_cb, data);
    g_signal_connect(factory, "bind", bind_cb, data);

    GtkColumnViewColumn* col = gtk_column_view_column_new(title, GTK_LIST_ITEM_FACTORY(factory));
    gtk_column_view_column_set_resizable(col, TRUE);
    gtk_column_view_column_set_expand(col, expand);
    if (fixed_width > 0) gtk_column_view_column_set_fixed_width(col, fixed_width);
    return col;
}

/* ── Keyboard shortcuts ─────────────────────────────────────────── */
static gboolean on_key_pressed(GtkEventControllerKey* ctrl, guint keyval, guint keycode, GdkModifierType state,
                               gpointer user_data) {
    (void)ctrl;
    (void)keycode;
    AppState* app = user_data;
    if (keyval == GDK_KEY_Escape) {
        gtk_editable_set_text(GTK_EDITABLE(app->search_entry), "");
        gtk_widget_grab_focus(app->search_entry);
        return TRUE;
    }
    /* Ctrl+L → focus search */
    if (keyval == GDK_KEY_l && (state & GDK_CONTROL_MASK)) {
        gtk_widget_grab_focus(app->search_entry);
        return TRUE;
    }
    return FALSE;
}

/* ── Copy path shortcut ─────────────────────────────────────────── */
static void on_copy_path(GSimpleAction* action, GVariant* param, gpointer user_data) {
    (void)action;
    (void)param;
    AppState* app          = user_data;
    GtkSelectionModel* sel = gtk_column_view_get_model(GTK_COLUMN_VIEW(app->column_view));
    GtkBitset* bs          = gtk_selection_model_get_selection(sel);
    if (gtk_bitset_get_size(bs) == 0) return;
    guint64 idx  = gtk_bitset_get_nth(bs, 0);
    FileItem* fi = FILE_ITEM(g_list_model_get_item(G_LIST_MODEL(app->sort_model), (guint)idx));
    if (!fi) return;
    GdkClipboard* clip = gtk_widget_get_clipboard(app->window);
    gdk_clipboard_set_text(clip, file_item_get_path(fi));
    g_object_unref(fi);
}

/* ── Build the UI ───────────────────────────────────────────────── */
static void build_ui(AppState* app) {
    /* ── Apply CSS ── */
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, APP_CSS);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* ── Window ── */
    app->window = gtk_application_window_new(app->app);
    gtk_window_set_title(GTK_WINDOW(app->window), "FastFind");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1100, 680);

    /* ── Root container ── */
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(app->window), vbox);

    /* ── Toolbar ── */
    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(toolbar, "toolbar");
    gtk_widget_set_valign(toolbar, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(vbox), toolbar);

    /* App icon */
    GtkWidget* app_icon = gtk_image_new_from_icon_name("system-search-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(app_icon), 20);
    gtk_widget_set_valign(app_icon, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(toolbar), app_icon);

    /* App title */
    GtkWidget* title_lbl = gtk_label_new("FastFind");
    gtk_widget_add_css_class(title_lbl, "app-title");
    gtk_widget_set_valign(title_lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(title_lbl, 4);
    gtk_box_append(GTK_BOX(toolbar), title_lbl);

    GtkWidget* sep1 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_valign(sep1, GTK_ALIGN_FILL);
    gtk_widget_set_margin_top(sep1, 8);
    gtk_widget_set_margin_bottom(sep1, 8);
    gtk_widget_set_margin_start(sep1, 2);
    gtk_widget_set_margin_end(sep1, 2);
    gtk_box_append(GTK_BOX(toolbar), sep1);

    /* Search entry */
    app->search_entry = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(app->search_entry), "Search files…  (Ctrl+L)");
    gtk_widget_add_css_class(app->search_entry, "search-entry");
    gtk_widget_set_hexpand(app->search_entry, TRUE);
    gtk_widget_set_valign(app->search_entry, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(toolbar), app->search_entry);

    /* Spinner (shown while indexing) */
    app->spinner = gtk_spinner_new();
    gtk_widget_set_valign(app->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(app->spinner, 20, 20);
    gtk_widget_set_visible(app->spinner, FALSE);
    gtk_box_append(GTK_BOX(toolbar), app->spinner);

    /* Pick folder button */
    app->path_btn = gtk_button_new_with_label("Choose Dir");
    gtk_widget_add_css_class(app->path_btn, "toolbar-btn");
    gtk_widget_set_valign(app->path_btn, GTK_ALIGN_CENTER);
    g_signal_connect(app->path_btn, "clicked", G_CALLBACK(on_pick_folder), app);
    gtk_box_append(GTK_BOX(toolbar), app->path_btn);

    /* Re-index button */
    app->index_btn = gtk_button_new_with_label("Re-index");
    gtk_widget_add_css_class(app->index_btn, "toolbar-btn");
    gtk_widget_set_valign(app->index_btn, GTK_ALIGN_CENTER);
    g_signal_connect(app->index_btn, "clicked", G_CALLBACK(on_reindex), app);
    gtk_box_append(GTK_BOX(toolbar), app->index_btn);

    /* ── Column view inside a scrolled window ── */
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), scroll);

    /* Models */
    app->store        = g_list_store_new(FILE_TYPE_ITEM);
    app->filter       = gtk_custom_filter_new(filter_func, app, NULL);
    app->filter_model = gtk_filter_list_model_new(G_LIST_MODEL(app->store), GTK_FILTER(app->filter));
    gtk_filter_list_model_set_incremental(app->filter_model, TRUE);

    app->sort_model = gtk_sort_list_model_new(G_LIST_MODEL(app->filter_model), NULL);

    GtkSelectionModel* sel = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(app->sort_model)));

    app->column_view = gtk_column_view_new(sel);
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(app->column_view), TRUE);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(app->column_view), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app->column_view);

    /* Columns */
    GtkColumnViewColumn* col_name = make_column("Name", G_CALLBACK(setup_name), G_CALLBACK(bind_name), 280, TRUE, NULL);
    GtkColumnViewColumn* col_path =
        make_column("Directory", G_CALLBACK(setup_path), G_CALLBACK(bind_path), 0, TRUE, NULL);
    GtkColumnViewColumn* col_size = make_column("Size", G_CALLBACK(setup_size), G_CALLBACK(bind_size), 90, FALSE, NULL);
    GtkColumnViewColumn* col_type =
        make_column("Type", G_CALLBACK(setup_type), G_CALLBACK(bind_type), 100, FALSE, NULL);

    gtk_column_view_append_column(GTK_COLUMN_VIEW(app->column_view), col_name);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(app->column_view), col_path);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(app->column_view), col_size);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(app->column_view), col_type);

    g_object_unref(col_name);
    g_object_unref(col_path);
    g_object_unref(col_size);
    g_object_unref(col_type);

    /* ── Status bar ── */
    GtkWidget* status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(status_box, "status-bar");
    gtk_box_append(GTK_BOX(vbox), status_box);

    app->status_label = gtk_label_new("Ready — choose a directory to index");
    gtk_label_set_xalign(GTK_LABEL(app->status_label), 0);
    gtk_widget_set_hexpand(app->status_label, TRUE);
    gtk_widget_set_valign(app->status_label, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(app->status_label, "status-label");
    gtk_box_append(GTK_BOX(status_box), app->status_label);

    app->root_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->root_label), 1);
    gtk_widget_set_valign(app->root_label, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(app->root_label, "status-label");
    gtk_box_append(GTK_BOX(status_box), app->root_label);

    /* ── Signals ── */
    g_signal_connect(app->search_entry, "search-changed", G_CALLBACK(on_search_changed), app);

    /* Double-click or Enter to open */
    GtkListView* lv = GTK_LIST_VIEW(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroll)));
    (void)lv; /* column_view handles activate differently */
    g_signal_connect(app->column_view, "activate", G_CALLBACK(on_activate_item), app);

    /* Keyboard controller */
    GtkEventController* key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), app);
    gtk_widget_add_controller(app->window, key_ctrl);

    /* Copy path action */
    GSimpleAction* copy_act = g_simple_action_new("copy-path", NULL);
    g_signal_connect(copy_act, "activate", G_CALLBACK(on_copy_path), app);
    g_action_map_add_action(G_ACTION_MAP(app->window), G_ACTION(copy_act));

    GtkShortcut* copy_sc =
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_c, GDK_CONTROL_MASK), gtk_named_action_new("win.copy-path"));
    GtkEventController* sc_ctrl = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc_ctrl), copy_sc);
    gtk_widget_add_controller(app->window, sc_ctrl);

    gtk_window_present(GTK_WINDOW(app->window));

    /* Auto-index home dir on startup */
    const char* home = g_get_home_dir();
    start_indexing(app, home ? home : "/");
}

/* ── GtkApplication activate ────────────────────────────────────── */
static void on_activate(GtkApplication* app_obj, gpointer user_data) {
    (void)user_data;
    AppState* app   = g_new0(AppState, 1);
    g_app           = app;
    app->app        = app_obj;
    app->index_root = g_strdup(g_get_home_dir());
    build_ui(app);
}

/* ── main ───────────────────────────────────────────────────────── */
int main(int argc, char* argv[]) {
    /* Use locale-aware number formatting */
    setlocale(LC_ALL, "");

    GtkApplication* app = gtk_application_new("io.github.fastfind", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
