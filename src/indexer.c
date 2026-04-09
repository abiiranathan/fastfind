#define _POSIX_C_SOURCE 200809L
#include "indexer.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Batch size: how many entries to collect before posting to main thread */
#define BATCH_SIZE 512

/* Directories to skip entirely */
static const char* SKIP_DIRS[] = {"/proc", "/sys", "/dev", "/run", "/tmp", "/snap", "/var/run", "/var/lock", NULL};

struct _Indexer {
    char* root;
    IndexerCallback cb;
    gpointer user_data;
    GThread* thread;
    volatile gint cancelled;
    volatile gint count;
    volatile gint done;
};

/* ── main-thread delivery ──────────────────────────────────────── */
typedef struct {
    Indexer* idx;
    GPtrArray* items;
    guint total;
    gboolean done;
} BatchPayload;

static gboolean deliver_batch_idle(gpointer data) {
    BatchPayload* p = data;
    if (!g_atomic_int_get(&p->idx->cancelled)) {
        p->idx->cb(p->items, p->total, p->done, p->idx->user_data);
    } else {
        g_ptr_array_unref(p->items);
    }
    g_free(p);
    return G_SOURCE_REMOVE;
}

static void post_batch(Indexer* idx, GPtrArray* items, gboolean done) {
    BatchPayload* p = g_new0(BatchPayload, 1);
    p->idx          = idx;
    p->items        = items;
    p->total        = (guint)g_atomic_int_get(&idx->count);
    p->done         = done;
    g_idle_add(deliver_batch_idle, p);
}

/* ── recursive walker ──────────────────────────────────────────── */
static gboolean should_skip(const char* path) {
    for (int i = 0; SKIP_DIRS[i]; i++) {
        if (g_str_has_prefix(path, SKIP_DIRS[i])) {
            size_t slen = strlen(SKIP_DIRS[i]);
            if (path[slen] == '\0' || path[slen] == '/') return TRUE;
        }
    }
    return FALSE;
}

static void walk(Indexer* idx, const char* dirpath, GPtrArray** batch) {
    if (g_atomic_int_get(&idx->cancelled)) return;
    if (should_skip(dirpath)) return;

    DIR* d = opendir(dirpath);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (g_atomic_int_get(&idx->cancelled)) break;
        if (ent->d_name[0] == '.') continue; /* skip hidden */

        char* full = g_build_filename(dirpath, ent->d_name, NULL);

        struct stat st;
        if (lstat(full, &st) != 0) {
            g_free(full);
            continue;
        }

        gboolean is_dir = S_ISDIR(st.st_mode);
        gboolean is_reg = S_ISREG(st.st_mode);

        if (is_dir || is_reg) {
            FileItem* fi =
                file_item_new(ent->d_name, full, is_reg ? (goffset)st.st_size : 0, is_dir, (guint64)st.st_mtime);
            g_ptr_array_add(*batch, fi);
            g_atomic_int_inc(&idx->count);

            if ((*batch)->len >= BATCH_SIZE) {
                post_batch(idx, *batch, FALSE);
                *batch = g_ptr_array_new_with_free_func(g_object_unref);
            }
        }

        if (is_dir) {
            walk(idx, full, batch);
        }
        g_free(full);
    }
    closedir(d);
}

static gpointer indexer_thread(gpointer data) {
    Indexer* idx     = data;
    GPtrArray* batch = g_ptr_array_new_with_free_func(g_object_unref);

    walk(idx, idx->root, &batch);

    /* flush remaining */
    post_batch(idx, batch, TRUE);
    g_atomic_int_set(&idx->done, 1);
    return NULL;
}

/* ── public API ────────────────────────────────────────────────── */
Indexer* indexer_new(const char* root_path, IndexerCallback cb, gpointer user_data) {
    Indexer* idx   = g_new0(Indexer, 1);
    idx->root      = g_strdup(root_path);
    idx->cb        = cb;
    idx->user_data = user_data;
    return idx;
}

void indexer_start(Indexer* idx) { idx->thread = g_thread_new("fastfind-indexer", indexer_thread, idx); }

void indexer_cancel(Indexer* idx) {
    g_atomic_int_set(&idx->cancelled, 1);
    if (idx->thread) {
        g_thread_join(idx->thread);
        idx->thread = NULL;
    }
    g_free(idx->root);
    g_free(idx);
}

gboolean indexer_is_done(Indexer* idx) { return g_atomic_int_get(&idx->done); }
guint indexer_count(Indexer* idx) { return (guint)g_atomic_int_get(&idx->count); }
