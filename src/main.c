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
    "}"
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
    ".file-name {"
    "  font-weight: 600;"
    "  color: @window_fg_color;"
    "}"
    ".file-dir {"
    "  font-size: 12px;"
    "  color: mix(@window_fg_color, @window_bg_color, 0.35);"
    "}"
    ".dir-row {"
    "  color: #6b8fe8;"
    "  font-weight: 600;"
    "}"
    /* Exclude-list window */
    ".excl-row {"
    "  padding: 4px 8px;"
    "}"
    ".excl-path {"
    "  font-size: 13px;"
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
    GtkWidget* root_label;
    GtkWidget* context_menu; /* GtkPopoverMenu for right-click */

    /* Models */
    GListStore* store;
    GtkFilterListModel* filter_model;
    GtkSortListModel* sort_model;
    GtkCustomFilter* filter;

    /* Indexer */
    Indexer* indexer;
    char* index_root;
    guint total_indexed;
    gboolean indexing;

    /* Persistent exclude list */
    GPtrArray* excludes; /* GPtrArray<gchar*>, owned */
} AppState;

static AppState* g_app = NULL;

/* ── Forward declarations ───────────────────────────────────────── */
static void start_indexing(AppState* app, const char* root);
static void update_status(AppState* app);

/* ── Helpers ────────────────────────────────────────────────────── */

/* Return the currently selected FileItem, or NULL.
 * Caller must g_object_unref() the returned item. */
static FileItem* get_selected_item(AppState* app) {
    GtkSelectionModel* sel = gtk_column_view_get_model(GTK_COLUMN_VIEW(app->column_view));
    GtkBitset* bs          = gtk_selection_model_get_selection(sel);
    if (gtk_bitset_get_size(bs) == 0) return NULL;
    guint64 idx = gtk_bitset_get_nth(bs, 0);
    return FILE_ITEM(g_list_model_get_item(G_LIST_MODEL(app->sort_model), (guint)idx));
}

/* ── Search filter ──────────────────────────────────────────────── */
static gboolean filter_func(gpointer item, gpointer user_data) {
    AppState* app     = user_data;
    const char* query = gtk_editable_get_text(GTK_EDITABLE(app->search_entry));
    if (!query || *query == '\0') return TRUE;

    FileItem* fi     = FILE_ITEM(item);
    const char* name = file_item_get_name(fi);
    const char* path = file_item_get_path(fi);

    char* lq    = g_utf8_casefold(query, -1);
    char* lname = g_utf8_casefold(name, -1);
    char* lpath = g_utf8_casefold(path, -1);

    gboolean match = (strstr(lname, lq) != NULL) || (strstr(lpath, lq) != NULL);

    g_free(lq);
    g_free(lname);
    g_free(lpath);
    return match;
}

/* ── Status bar ─────────────────────────────────────────────────── */
static void update_status(AppState* app) {
    guint total       = app->total_indexed;
    guint filtered    = g_list_model_get_n_items(G_LIST_MODEL(app->sort_model));
    const char* query = gtk_editable_get_text(GTK_EDITABLE(app->search_entry));

    char buf[320];
    if (app->indexing) {
        g_snprintf(buf, sizeof(buf), "Indexing %s…   %'u files scanned", app->index_root, total);
    } else if (!query || *query == '\0') {
        g_snprintf(buf, sizeof(buf), "%'u files indexed in %s", total, app->index_root);
    } else if (filtered == 0) {
        g_snprintf(buf, sizeof(buf), "No results for \"%s\"   (%'u total)", query, total);
    } else {
        g_snprintf(buf, sizeof(buf), "%'u of %'u results for \"%s\"", filtered, total, query);
    }
    gtk_label_set_text(GTK_LABEL(app->status_label), buf);
}

/* ── Indexer callback (main thread) ────────────────────────────── */
static void on_batch_ready(GPtrArray* items, guint total, gboolean done, gpointer user_data) {
    AppState* app      = user_data;
    app->total_indexed = total;

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
        /* Update window title */
        char title[128];
        g_snprintf(title, sizeof(title), "FastFind — %'u files", total);
        gtk_window_set_title(GTK_WINDOW(app->window), title);
    }

    gtk_filter_changed(GTK_FILTER(app->filter), GTK_FILTER_CHANGE_DIFFERENT);
    update_status(app);
}

/* ── Start / restart indexing ───────────────────────────────────── */
static void start_indexing(AppState* app, const char* root) {
    /* Duplicate immediately — caller may pass app->index_root itself */
    char* new_root = g_strdup(root);

    if (app->indexer) {
        indexer_cancel(app->indexer);
        app->indexer = NULL;
    }
    g_list_store_remove_all(app->store);
    g_free(app->index_root);
    app->index_root    = new_root; /* safe now */
    app->total_indexed = 0;
    app->indexing      = TRUE;

    char* dr = g_filename_display_name(app->index_root);
    gtk_label_set_text(GTK_LABEL(app->root_label), dr);
    gtk_window_set_title(GTK_WINDOW(app->window), "FastFind — Indexing…");
    g_free(dr);

    gtk_widget_set_visible(app->spinner, TRUE);
    gtk_spinner_start(GTK_SPINNER(app->spinner));
    gtk_button_set_label(GTK_BUTTON(app->index_btn), "Indexing…");
    gtk_widget_set_sensitive(app->index_btn, FALSE);

    app->indexer = indexer_new(app->index_root, on_batch_ready, app);

    /* Pass a ref-copy of the current exclude list to the indexer */
    if (app->excludes && app->excludes->len > 0) {
        GPtrArray* copy = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < app->excludes->len; i++) {
            g_ptr_array_add(copy, g_strdup(g_ptr_array_index(app->excludes, i)));
        }
        indexer_set_excludes(app->indexer, copy);
    }

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
    (void)btn;
    AppState* app      = user_data;
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

/* ── Open item / reveal in file manager ─────────────────────────── */
static void open_path_in_manager(const char* path) {
    GFile* file = g_file_new_for_path(path);
    g_app_info_launch_default_for_uri(g_file_get_uri(file), NULL, NULL);
    g_object_unref(file);
}

static void open_item(FileItem* fi) { open_path_in_manager(file_item_get_path(fi)); }

/* Activate (double-click / Enter on row) */
static void on_activate_item(GtkColumnView* cv, guint pos, gpointer user_data) {
    (void)cv;
    (void)user_data;
    AppState* app = g_app;
    FileItem* fi  = FILE_ITEM(g_list_model_get_item(G_LIST_MODEL(app->sort_model), pos));
    if (!fi) return;
    open_item(fi);
    g_object_unref(fi);
}

/* ── Actions ────────────────────────────────────────────────────── */
static void on_action_open_item(GSimpleAction* a, GVariant* p, gpointer u) {
    (void)a;
    (void)p;
    AppState* app = u;
    FileItem* fi  = get_selected_item(app);
    if (!fi) return;
    open_item(fi);
    g_object_unref(fi);
}

static void on_action_reveal_item(GSimpleAction* a, GVariant* p, gpointer u) {
    (void)a;
    (void)p;
    AppState* app = u;
    FileItem* fi  = get_selected_item(app);
    if (!fi) return;
    /* Always reveal the containing folder */
    open_path_in_manager(file_item_get_dir(fi));
    g_object_unref(fi);
}

static void on_action_copy_path(GSimpleAction* a, GVariant* p, gpointer u) {
    (void)a;
    (void)p;
    AppState* app = u;
    FileItem* fi  = get_selected_item(app);
    if (!fi) return;
    GdkClipboard* clip = gtk_widget_get_clipboard(app->window);
    gdk_clipboard_set_text(clip, file_item_get_path(fi));
    g_object_unref(fi);
}

/* ── Right-click context menu ───────────────────────────────────── */
static void on_right_click(GtkGestureClick* gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
    (void)gesture;
    (void)n_press;
    AppState* app = user_data;
    if (!get_selected_item(app)) return; /* nothing selected — don't show */
    FileItem* fi = get_selected_item(app);
    if (fi) g_object_unref(fi); /* we only checked; don't leak */

    GdkRectangle rect = {(gint)x, (gint)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(app->context_menu), &rect);
    gtk_popover_popup(GTK_POPOVER(app->context_menu));
}

/* ── Keyboard handling ──────────────────────────────────────────── */

/* Key controller on the search entry */
static gboolean on_entry_key(GtkEventControllerKey* ctrl, guint keyval, guint keycode, GdkModifierType state,
                             gpointer user_data) {
    (void)ctrl;
    (void)keycode;
    (void)state;
    AppState* app = user_data;

    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_Page_Down) {
        /* Move focus into the list */
        gtk_widget_grab_focus(app->column_view);
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        /* Activate first result, if any */
        if (g_list_model_get_n_items(G_LIST_MODEL(app->sort_model)) > 0) {
            GtkSelectionModel* sel = gtk_column_view_get_model(GTK_COLUMN_VIEW(app->column_view));
            gtk_selection_model_select_item(sel, 0, TRUE);
            FileItem* fi = FILE_ITEM(g_list_model_get_item(G_LIST_MODEL(app->sort_model), 0));
            if (fi) {
                open_item(fi);
                g_object_unref(fi);
            }
        }
        return TRUE;
    }
    return FALSE;
}

/* Global window key controller */
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

/* ── Open With ──────────────────────────────────────────────────── */
/* Row activated in the custom "Open With" listbox */
static void on_app_list_row_activated(GtkListBox* box, GtkListBoxRow* row, gpointer user_data) {
    (void)box;
    GtkWindow* dlg   = GTK_WINDOW(user_data);
    GAppInfo* info   = g_object_get_data(G_OBJECT(row), "app-info");
    const char* path = g_object_get_data(G_OBJECT(dlg), "item-path");
    if (info && path) {
        GFile* file  = g_file_new_for_path(path);
        GList* files = g_list_append(NULL, file);
        g_app_info_launch(info, files, NULL, NULL);
        g_list_free(files);
        g_object_unref(file);
    }
    gtk_window_close(dlg);
}

static void on_action_open_with(GSimpleAction* a, GVariant* p, gpointer u) {
    (void)a;
    (void)p;
    AppState* app = u;
    FileItem* fi  = get_selected_item(app);
    if (!fi) return;

    const char* path = file_item_get_path(fi);

    /* Resolve content-type to enumerate capable apps */
    GFile* gfile = g_file_new_for_path(path);
    GFileInfo* finfo =
        g_file_query_info(gfile, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
    g_object_unref(gfile);

    const char* ctype = (finfo && g_file_info_get_content_type(finfo)) ? g_file_info_get_content_type(finfo)
                                                                       : "application/octet-stream";
    GList* apps       = g_app_info_get_all_for_type(ctype);

    /* ── Dialog ── */
    GtkWidget* win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Open With");
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 360, 420);
    g_object_set_data_full(G_OBJECT(win), "item-path", g_strdup(path), g_free);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    GtkWidget* heading = gtk_label_new("Choose an application");
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_box_append(GTK_BOX(vbox), heading);

    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(vbox), scroll);

    GtkWidget* listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), listbox);

    for (GList* l = apps; l; l = l->next) {
        GAppInfo* info     = l->data;
        GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(row_box, 8);
        gtk_widget_set_margin_end(row_box, 8);
        gtk_widget_set_margin_top(row_box, 4);
        gtk_widget_set_margin_bottom(row_box, 4);

        GIcon* icon    = g_app_info_get_icon(info); /* may be NULL; GTK handles it */
        GtkWidget* img = gtk_image_new_from_gicon(icon);
        gtk_image_set_pixel_size(GTK_IMAGE(img), 24);

        GtkWidget* name_lbl = gtk_label_new(g_app_info_get_display_name(info));
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0);
        gtk_widget_set_hexpand(name_lbl, TRUE);

        gtk_box_append(GTK_BOX(row_box), img);
        gtk_box_append(GTK_BOX(row_box), name_lbl);

        GtkWidget* row = gtk_list_box_row_new();
        g_object_set_data_full(G_OBJECT(row), "app-info", g_object_ref(info), g_object_unref);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
        gtk_list_box_append(GTK_LIST_BOX(listbox), row);
    }

    g_signal_connect(listbox, "row-activated", G_CALLBACK(on_app_list_row_activated), win);

    GtkWidget* cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_widget_set_halign(cancel_btn, GTK_ALIGN_END);
    g_signal_connect_swapped(cancel_btn, "clicked", G_CALLBACK(gtk_window_close), win);
    gtk_box_append(GTK_BOX(vbox), cancel_btn);

    g_list_free_full(apps, g_object_unref);
    if (finfo) g_object_unref(finfo);
    g_object_unref(fi);

    gtk_window_present(GTK_WINDOW(win));
}

/* ── Column factory helpers ─────────────────────────────────────── */
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

static GtkColumnViewColumn* make_column(const char* title, GCallback setup_cb, GCallback bind_cb, int fixed_width,
                                        gboolean expand) {
    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", setup_cb, NULL);
    g_signal_connect(factory, "bind", bind_cb, NULL);
    GtkColumnViewColumn* col = gtk_column_view_column_new(title, GTK_LIST_ITEM_FACTORY(factory));
    gtk_column_view_column_set_resizable(col, TRUE);
    gtk_column_view_column_set_expand(col, expand);
    if (fixed_width > 0) gtk_column_view_column_set_fixed_width(col, fixed_width);
    return col;
}

/* ── Excludes window ────────────────────────────────────────────── */

typedef struct {
    AppState* app;
    GPtrArray* working; /* in-progress copy */
    GtkWidget* listbox;
    GtkWidget* path_entry;
    GtkWidget* win;
} ExcludesCtx;

static void excludes_ctx_free(gpointer data) {
    ExcludesCtx* ctx = data;
    g_ptr_array_unref(ctx->working);
    g_free(ctx);
}

/* Add one row to the excludes listbox */
static void excludes_append_row(ExcludesCtx* ctx, const char* path);

static void on_excl_remove(GtkButton* btn, gpointer user_data) {
    ExcludesCtx* ctx = user_data;
    GtkWidget* row   = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_LIST_BOX_ROW);
    if (!row) return;
    const char* path = g_object_get_data(G_OBJECT(row), "excl-path");

    /* Remove from working array */
    for (guint i = 0; i < ctx->working->len; i++) {
        if (g_strcmp0(g_ptr_array_index(ctx->working, i), path) == 0) {
            g_ptr_array_remove_index(ctx->working, i);
            break;
        }
    }
    gtk_list_box_remove(GTK_LIST_BOX(ctx->listbox), row);
}

static void excludes_append_row(ExcludesCtx* ctx, const char* path) {
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(hbox, "excl-row");

    GtkWidget* lbl = gtk_label_new(path);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_START);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_add_css_class(lbl, "excl-path");

    GtkWidget* rm = gtk_button_new_from_icon_name("list-remove-symbolic");
    gtk_widget_set_tooltip_text(rm, "Remove");
    g_signal_connect(rm, "clicked", G_CALLBACK(on_excl_remove), ctx);

    gtk_box_append(GTK_BOX(hbox), lbl);
    gtk_box_append(GTK_BOX(hbox), rm);

    GtkWidget* row = gtk_list_box_row_new();
    g_object_set_data_full(G_OBJECT(row), "excl-path", g_strdup(path), g_free);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
    gtk_list_box_append(GTK_LIST_BOX(ctx->listbox), row);
}

static void on_excl_add(GtkButton* btn, gpointer user_data) {
    (void)btn;
    ExcludesCtx* ctx = user_data;
    const char* text = gtk_editable_get_text(GTK_EDITABLE(ctx->path_entry));
    if (!text || *text == '\0') return;

    /* Avoid duplicates */
    for (guint i = 0; i < ctx->working->len; i++) {
        if (g_strcmp0(g_ptr_array_index(ctx->working, i), text) == 0) {
            gtk_editable_set_text(GTK_EDITABLE(ctx->path_entry), "");
            return;
        }
    }

    g_ptr_array_add(ctx->working, g_strdup(text));
    excludes_append_row(ctx, text);
    gtk_editable_set_text(GTK_EDITABLE(ctx->path_entry), "");
}

static void on_excl_browse_done(GObject* source, GAsyncResult* res, gpointer user_data) {
    ExcludesCtx* ctx = user_data;
    GFile* f         = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), res, NULL);
    if (!f) return;
    char* path = g_file_get_path(f);
    g_object_unref(f);
    if (path) {
        gtk_editable_set_text(GTK_EDITABLE(ctx->path_entry), path);
        g_free(path);
    }
}

static void on_excl_browse(GtkButton* btn, gpointer user_data) {
    (void)btn;
    ExcludesCtx* ctx   = user_data;
    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Choose directory to exclude");
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(ctx->win), NULL, on_excl_browse_done, ctx);
    g_object_unref(dlg);
}

static void on_excl_save(GtkButton* btn, gpointer user_data) {
    (void)btn;
    ExcludesCtx* ctx = user_data;
    AppState* app    = ctx->app;

    /* Replace app excludes with the working copy */
    g_ptr_array_unref(app->excludes);
    app->excludes = g_ptr_array_ref(ctx->working);

    excludes_save(app->excludes);

    /* Re-index to apply new excludes */
    start_indexing(app, app->index_root);

    gtk_window_close(GTK_WINDOW(ctx->win));
}

static void on_open_excludes(GtkButton* btn, gpointer user_data) {
    (void)btn;
    AppState* app = user_data;

    /* Build the context for this dialog instance */
    ExcludesCtx* ctx = g_new0(ExcludesCtx, 1);
    ctx->app         = app;
    /* Deep-copy the current exclude list into a working copy */
    ctx->working = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < app->excludes->len; i++) {
        g_ptr_array_add(ctx->working, g_strdup(g_ptr_array_index(app->excludes, i)));
    }

    /* ── Window ── */
    GtkWidget* win = gtk_window_new();
    ctx->win       = win;
    gtk_window_set_title(GTK_WINDOW(win), "Excluded Directories");
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 540, 420);
    /* Free context when window is destroyed */
    g_object_set_data_full(G_OBJECT(win), "excl-ctx", ctx, excludes_ctx_free);

    /* ── Layout ── */
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 14);
    gtk_widget_set_margin_end(vbox, 14);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    /* Header label */
    GtkWidget* hdr = gtk_label_new(
        "Directories to skip during indexing.\n"
        "Changes take effect on the next re-index.");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0);
    gtk_label_set_wrap(GTK_LABEL(hdr), TRUE);
    gtk_box_append(GTK_BOX(vbox), hdr);

    /* Scrolled listbox */
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 220);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(vbox), scroll);

    ctx->listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ctx->listbox), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ctx->listbox);

    /* Populate with current working list */
    for (guint i = 0; i < ctx->working->len; i++) {
        excludes_append_row(ctx, g_ptr_array_index(ctx->working, i));
    }

    /* Separator */
    gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* Add-path row */
    GtkWidget* add_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ctx->path_entry    = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->path_entry), "/path/to/exclude");
    gtk_widget_set_hexpand(ctx->path_entry, TRUE);
    /* Pressing Enter in the entry also adds the path */
    g_signal_connect_swapped(ctx->path_entry, "activate", G_CALLBACK(on_excl_add), ctx);

    GtkWidget* browse_btn = gtk_button_new_with_label("Browse…");
    g_signal_connect(browse_btn, "clicked", G_CALLBACK(on_excl_browse), ctx);

    GtkWidget* add_btn = gtk_button_new_with_label("Add");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_excl_add), ctx);

    gtk_box_append(GTK_BOX(add_box), ctx->path_entry);
    gtk_box_append(GTK_BOX(add_box), browse_btn);
    gtk_box_append(GTK_BOX(add_box), add_btn);
    gtk_box_append(GTK_BOX(vbox), add_box);

    /* Save / Cancel buttons */
    GtkWidget* btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_END);

    GtkWidget* cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect_swapped(cancel_btn, "clicked", G_CALLBACK(gtk_window_close), win);

    GtkWidget* save_btn = gtk_button_new_with_label("Save & Re-index");
    gtk_widget_add_css_class(save_btn, "suggested-action");
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_excl_save), ctx);

    gtk_box_append(GTK_BOX(btn_row), cancel_btn);
    gtk_box_append(GTK_BOX(btn_row), save_btn);
    gtk_box_append(GTK_BOX(vbox), btn_row);

    gtk_window_present(GTK_WINDOW(win));
}

/* ── Build the main UI ──────────────────────────────────────────── */
static void build_ui(AppState* app) {
    /* ── CSS ── */
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

    /* Pick-folder button */
    app->path_btn = gtk_button_new_with_label("Choose Dir");
    gtk_widget_add_css_class(app->path_btn, "toolbar-btn");
    gtk_widget_set_valign(app->path_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(app->path_btn, "Choose directory to index");
    g_signal_connect(app->path_btn, "clicked", G_CALLBACK(on_pick_folder), app);
    gtk_box_append(GTK_BOX(toolbar), app->path_btn);

    /* Re-index button */
    app->index_btn = gtk_button_new_with_label("Re-index");
    gtk_widget_add_css_class(app->index_btn, "toolbar-btn");
    gtk_widget_set_valign(app->index_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(app->index_btn, "Scan again from scratch");
    g_signal_connect(app->index_btn, "clicked", G_CALLBACK(on_reindex), app);
    gtk_box_append(GTK_BOX(toolbar), app->index_btn);

    /* Excludes button */
    GtkWidget* excl_btn = gtk_button_new_with_label("Excludes…");
    gtk_widget_add_css_class(excl_btn, "toolbar-btn");
    gtk_widget_set_valign(excl_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(excl_btn, "Manage directories to skip during indexing");
    g_signal_connect(excl_btn, "clicked", G_CALLBACK(on_open_excludes), app);
    gtk_box_append(GTK_BOX(toolbar), excl_btn);

    /* ── Column view ── */
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
    GtkColumnViewColumn* col_name = make_column("Name", G_CALLBACK(setup_name), G_CALLBACK(bind_name), 280, TRUE);
    GtkColumnViewColumn* col_path = make_column("Directory", G_CALLBACK(setup_path), G_CALLBACK(bind_path), 0, TRUE);
    GtkColumnViewColumn* col_size = make_column("Size", G_CALLBACK(setup_size), G_CALLBACK(bind_size), 90, FALSE);
    GtkColumnViewColumn* col_type = make_column("Type", G_CALLBACK(setup_type), G_CALLBACK(bind_type), 100, FALSE);

    gtk_column_view_append_column(GTK_COLUMN_VIEW(app->column_view), col_name);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(app->column_view), col_path);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(app->column_view), col_size);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(app->column_view), col_type);
    g_object_unref(col_name);
    g_object_unref(col_path);
    g_object_unref(col_size);
    g_object_unref(col_type);

    /* ── Context menu ── */
    {
        GMenu* menu = g_menu_new();
        g_menu_append(menu, "Open", "win.open-item");
        g_menu_append(menu, "Open With…", "win.open-with");
        g_menu_append(menu, "Open Containing Folder", "win.reveal-item");
        g_menu_append(menu, "Copy Path  (Ctrl+C)", "win.copy-path");
        app->context_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
        gtk_popover_set_has_arrow(GTK_POPOVER(app->context_menu), FALSE);
        gtk_widget_set_parent(app->context_menu, app->column_view);
        g_object_unref(menu);
    }

    GtkGesture* rclick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
    g_signal_connect(rclick, "pressed", G_CALLBACK(on_right_click), app);
    gtk_widget_add_controller(app->column_view, GTK_EVENT_CONTROLLER(rclick));

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
    g_signal_connect(app->column_view, "activate", G_CALLBACK(on_activate_item), app);

    /* Key controller on search entry (Down, Enter) */
    GtkEventController* entry_key = gtk_event_controller_key_new();
    g_signal_connect(entry_key, "key-pressed", G_CALLBACK(on_entry_key), app);
    gtk_widget_add_controller(app->search_entry, entry_key);

    /* Global window key controller (Escape, Ctrl+L) */
    GtkEventController* win_key = gtk_event_controller_key_new();
    g_signal_connect(win_key, "key-pressed", G_CALLBACK(on_key_pressed), app);
    gtk_widget_add_controller(app->window, win_key);

    /* ── Actions ── */
    static const struct {
        const char* name;
        GCallback cb;
    } actions[] = {
        {"open-item", G_CALLBACK(on_action_open_item)},
        {"open-with", G_CALLBACK(on_action_open_with)},
        {"reveal-item", G_CALLBACK(on_action_reveal_item)},
        {"copy-path", G_CALLBACK(on_action_copy_path)},
    };

    for (gsize i = 0; i < G_N_ELEMENTS(actions); i++) {
        GSimpleAction* a = g_simple_action_new(actions[i].name, NULL);
        g_signal_connect(a, "activate", actions[i].cb, app);
        g_action_map_add_action(G_ACTION_MAP(app->window), G_ACTION(a));
    }

    /* Ctrl+C → copy-path shortcut */
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

    /* Load persisted exclude list */
    app->excludes = excludes_load();

    build_ui(app);
}

/* ── main ───────────────────────────────────────────────────────── */
int main(int argc, char* argv[]) {
    setlocale(LC_ALL, "");

    GtkApplication* app = gtk_application_new("io.github.fastfind", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
