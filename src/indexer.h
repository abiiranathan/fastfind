#pragma once
#include <gio/gio.h>
#include <glib.h>
#include "fileitem.h"

/*
 * Called on the MAIN thread when a batch of newly indexed items is ready.
 *
 * @items : GPtrArray<FileItem*> — caller takes ownership; must g_ptr_array_unref()
 * @total : running count of items indexed so far
 * @done  : TRUE once the background walk has finished completely
 */
typedef void (*IndexerCallback)(GPtrArray* items, guint total, gboolean done, gpointer user_data);

typedef struct _Indexer Indexer;

/* Create a new indexer.  Does NOT start it yet. */
Indexer* indexer_new(const char* root_path, IndexerCallback cb, gpointer user_data);

/*
 * Supply user-defined extra exclude paths.
 * Takes ownership of @paths (will g_ptr_array_unref it when done).
 * Must be called BEFORE indexer_start().
 */
void indexer_set_excludes(Indexer* idx, GPtrArray* paths);

/* Start the background indexer thread. */
void indexer_start(Indexer* idx);

/* Cancel, join the thread, and free everything. */
void indexer_cancel(Indexer* idx);

gboolean indexer_is_done(Indexer* idx);
guint indexer_count(Indexer* idx);

/* ── Persistent exclude-list helpers ──────────────────────────────
 *
 * Config file location: $XDG_CONFIG_HOME/fastfind/excludes
 * (usually ~/.config/fastfind/excludes)
 * Format: one absolute path per line; lines starting with '#' are comments.
 */

/* Returns the config file path.  Caller must g_free(). */
char* excludes_config_path(void);

/* Load exclude list from disk.
 * Returns GPtrArray<gchar*> with free_func = g_free.
 * Returns an empty array (not NULL) if the file doesn't exist yet. */
GPtrArray* excludes_load(void);

/* Write exclude list to disk (creates parent dirs as needed). */
void excludes_save(GPtrArray* paths);
