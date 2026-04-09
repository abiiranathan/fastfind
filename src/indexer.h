#pragma once
#include <gio/gio.h>
#include <glib.h>
#include "fileitem.h"

/* Called on the main thread when a batch of new items is ready.
 * @items  : GPtrArray of FileItem* (caller takes ownership)
 * @total  : running total of indexed files so far
 * @done   : TRUE when indexing is complete
 */
typedef void (*IndexerCallback)(GPtrArray* items, guint total, gboolean done, gpointer user_data);

typedef struct _Indexer Indexer;

Indexer* indexer_new(const char* root_path, IndexerCallback cb, gpointer user_data);

void indexer_start(Indexer* idx);
void indexer_cancel(Indexer* idx); /* stop and free */
gboolean indexer_is_done(Indexer* idx);
guint indexer_count(Indexer* idx);
