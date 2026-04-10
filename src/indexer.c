#define _POSIX_C_SOURCE 200809L
/*
 * indexer.c
 *
 * Implements the background file system indexer for FastFind.
 * Recursively scans directories, applies exclude rules, and batches FileItem objects for the UI.
 *
 * Features:
 *   - Built-in and user-defined exclude lists
 *   - Efficient batch delivery to main thread
 *   - Heuristics for skipping build and vendor directories
 *
 * See indexer.h for API and callback details.
 */
#include "indexer.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Number of FileItems to collect before posting one idle callback */
#define BATCH_SIZE 512

/* ── Built-in path-prefix skip list (checked before descending) ── */
static const char* const SKIP_PREFIXES[] = {"/proc", "/sys",     "/dev",      "/run", "/tmp",
                                            "/snap", "/var/run", "/var/lock", NULL};

/*
 * Built-in directory-BASENAME skip list.
 * These are matched against ent->d_name anywhere in the tree.
 * Hidden dirs (dot-prefixed) are already filtered before we reach
 * this check, so only non-hidden artifact dirs belong here.
 */
static const char* const SKIP_BASENAMES[] = {
    /* Node / JS / web tooling */
    "node_modules", "bower_components",
    /* Python */
    "__pycache__", "venv", /* non-hidden virtualenv convention */
    /* Java / Kotlin build cache */
    "build", /* combined with heuristics below — see should_skip_dir */
    /* Generic large vendor trees */
    "vendor", NULL};

/* ─────────────────────────────────────────────────────────────── */

struct _Indexer {
    char* root;
    IndexerCallback cb;
    gpointer user_data;
    GThread* thread;
    volatile gint cancelled;
    volatile gint count;
    volatile gint done;

    /*
     * Skip sets — built just before the thread starts, then read-only.
     * skip_exact : full path  → NULL  (user excludes + runtime-computed)
     * skip_names : basename   → NULL  (built-in name list)
     */
    GHashTable* skip_exact;
    GHashTable* skip_names;
};

/* ── Batch delivery ─────────────────────────────────────────────── */
typedef struct {
    Indexer* idx;
    GPtrArray* items;
    guint total;
    gboolean done;
} BatchPayload;

static gboolean deliver_batch(gpointer data) {
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
    BatchPayload* p = g_new(BatchPayload, 1);
    p->idx          = idx;
    p->items        = items;
    p->total        = (guint)g_atomic_int_get(&idx->count);
    p->done         = done;
    g_idle_add(deliver_batch, p);
}

/* ── Skip logic ─────────────────────────────────────────────────── */
static gboolean skip_by_prefix(const char* path) {
    for (int i = 0; SKIP_PREFIXES[i]; i++) {
        const char* pfx = SKIP_PREFIXES[i];
        gsize plen      = strlen(pfx);
        if (strncmp(path, pfx, plen) == 0 && (path[plen] == '\0' || path[plen] == '/')) return TRUE;
    }
    return FALSE;
}

/*
 * Conservative heuristic: skip a directory named "build" or "target" ONLY
 * when its parent contains tell-tale build-system files, avoiding false
 * positives on directories like /home/user/Documents/build-notes.
 */
static gboolean is_artifact_build_dir(const char* full_path, const char* basename) {
    if (g_strcmp0(basename, "build") != 0 && g_strcmp0(basename, "target") != 0) return FALSE;

    /* Check parent directory for build-system markers */
    char* parent = g_path_get_dirname(full_path);

    /* Rust: Cargo.toml / Cargo.lock */
    char* cargo    = g_build_filename(parent, "Cargo.toml", NULL);
    gboolean found = g_file_test(cargo, G_FILE_TEST_EXISTS);
    g_free(cargo);
    if (!found) {
        cargo = g_build_filename(parent, "Cargo.lock", NULL);
        found = g_file_test(cargo, G_FILE_TEST_EXISTS);
        g_free(cargo);
    }
    /* Gradle / Maven: build.gradle, pom.xml */
    if (!found) {
        char* f = g_build_filename(parent, "build.gradle", NULL);
        found   = g_file_test(f, G_FILE_TEST_EXISTS);
        g_free(f);
    }
    if (!found) {
        char* f = g_build_filename(parent, "build.gradle.kts", NULL);
        found   = g_file_test(f, G_FILE_TEST_EXISTS);
        g_free(f);
    }
    if (!found) {
        char* f = g_build_filename(parent, "pom.xml", NULL);
        found   = g_file_test(f, G_FILE_TEST_EXISTS);
        g_free(f);
    }

    g_free(parent);
    return found;
}

static gboolean should_skip_dir(Indexer* idx, const char* full_path, const char* basename) {
    /* 1. Hard-coded system path prefixes */
    if (skip_by_prefix(full_path)) return TRUE;

    /* 2. Exact user / runtime-computed excludes */
    if (g_hash_table_contains(idx->skip_exact, full_path)) return TRUE;

    /* 3. Built-in basename set */
    if (g_hash_table_contains(idx->skip_names, basename)) return TRUE;

    /* 4. Heuristic: "build" / "target" next to a build-system file */
    if (is_artifact_build_dir(full_path, basename)) return TRUE;

    return FALSE;
}

/* ── Recursive walker ───────────────────────────────────────────── */
static void walk(Indexer* idx, const char* dirpath, GPtrArray** batch) {
    if (g_atomic_int_get(&idx->cancelled)) return;

    DIR* d = opendir(dirpath);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (g_atomic_int_get(&idx->cancelled)) break;

        const char* dname = ent->d_name;

        /* Skip "." and ".." unconditionally */
        if (dname[0] == '.' && (dname[1] == '\0' || (dname[1] == '.' && dname[2] == '\0'))) continue;

        /* Skip all other hidden entries (dot-files / dot-dirs).
         * This already covers .git, .cargo, .venv, .gradle, .npm, etc. */
        if (dname[0] == '.') continue;

        char* full = g_build_filename(dirpath, dname, NULL);

        struct stat st;
        if (lstat(full, &st) != 0) {
            g_free(full);
            continue;
        }

        gboolean is_dir = S_ISDIR(st.st_mode);
        gboolean is_reg = S_ISREG(st.st_mode);

        if (is_dir) {
            if (should_skip_dir(idx, full, dname)) {
                g_free(full);
                continue;
            }

            /* Add dir entry to results */
            FileItem* fi = file_item_new(full, 0, TRUE, (guint64)st.st_mtime);
            g_ptr_array_add(*batch, fi);
            g_atomic_int_inc(&idx->count);

            if ((*batch)->len >= BATCH_SIZE) {
                post_batch(idx, *batch, FALSE);
                *batch = g_ptr_array_new_with_free_func(g_object_unref);
            }

            walk(idx, full, batch);

        } else if (is_reg) {
            FileItem* fi = file_item_new(full, (goffset)st.st_size, FALSE, (guint64)st.st_mtime);
            g_ptr_array_add(*batch, fi);
            g_atomic_int_inc(&idx->count);

            if ((*batch)->len >= BATCH_SIZE) {
                post_batch(idx, *batch, FALSE);
                *batch = g_ptr_array_new_with_free_func(g_object_unref);
            }
        }

        g_free(full);
    }
    closedir(d);
}

static gpointer indexer_thread(gpointer data) {
    Indexer* idx     = data;
    GPtrArray* batch = g_ptr_array_new_with_free_func(g_object_unref);
    walk(idx, idx->root, &batch);
    /* Flush the last (possibly partial) batch, marking done = TRUE */
    post_batch(idx, batch, TRUE);
    g_atomic_int_set(&idx->done, 1);
    return NULL;
}

/* ── Skip-set builder ───────────────────────────────────────────── */
static void build_skip_sets(Indexer* idx, GPtrArray* user_excludes) {
    /* skip_exact owns its keys (g_free on destroy) */
    idx->skip_exact = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* skip_names keys are string-literal constants — no destructor */
    idx->skip_names = g_hash_table_new(g_str_hash, g_str_equal);

    /* Populate name set with built-in basenames */
    for (int i = 0; SKIP_BASENAMES[i]; i++) {
        g_hash_table_add(idx->skip_names, (gpointer)SKIP_BASENAMES[i]);
    }

    /* ── Runtime-computed exact paths ─────────────────────────── */
    const char* home = g_get_home_dir();

    /* Go module cache: $GOPATH/{pkg,bin} or ~/go/{pkg,bin} */
    const char* gopath_env = g_getenv("GOPATH");
    const char* gobase     = gopath_env ? gopath_env : (home ? NULL : NULL);
    if (!gobase && home) {
        /* Check if ~/go exists before adding */
        char* default_go = g_build_filename(home, "go", NULL);
        if (g_file_test(default_go, G_FILE_TEST_IS_DIR)) {
            g_hash_table_add(idx->skip_exact, g_build_filename(home, "go", "pkg", NULL));
            g_hash_table_add(idx->skip_exact, g_build_filename(home, "go", "bin", NULL));
        }
        g_free(default_go);
    } else if (gobase) {
        g_hash_table_add(idx->skip_exact, g_build_filename(gobase, "pkg", NULL));
        g_hash_table_add(idx->skip_exact, g_build_filename(gobase, "bin", NULL));
    }

    /* Rust crate download cache — ~/.cargo/registry & ~/.cargo/git
     * (keep ~/.cargo/bin so installed tools remain findable) */
    if (home) {
        g_hash_table_add(idx->skip_exact, g_build_filename(home, ".cargo", "registry", NULL));
        g_hash_table_add(idx->skip_exact, g_build_filename(home, ".cargo", "git", NULL));

        /* Trash */
        g_hash_table_add(idx->skip_exact, g_build_filename(home, ".local", "share", "Trash", NULL));

        /* Flatpak runtime blobs */
        g_hash_table_add(idx->skip_exact, g_build_filename(home, ".local", "share", "flatpak", "runtime", NULL));
    }

    /* ── User-supplied exact excludes ─────────────────────────── */
    if (user_excludes) {
        for (guint i = 0; i < user_excludes->len; i++) {
            const char* p = g_ptr_array_index(user_excludes, i);
            if (p && *p) {
                g_hash_table_add(idx->skip_exact, g_strdup(p));
            }
        }
    }
}

static void destroy_skip_sets(Indexer* idx) {
    if (idx->skip_exact) {
        g_hash_table_destroy(idx->skip_exact);
        idx->skip_exact = NULL;
    }
    if (idx->skip_names) {
        g_hash_table_destroy(idx->skip_names);
        idx->skip_names = NULL;
    }
}

/* ── Public API ─────────────────────────────────────────────────── */

Indexer* indexer_new(const char* root_path, IndexerCallback cb, gpointer user_data) {
    g_return_val_if_fail(root_path != NULL, NULL);
    g_return_val_if_fail(cb != NULL, NULL);

    Indexer* idx   = g_new0(Indexer, 1);
    idx->root      = g_strdup(root_path);
    idx->cb        = cb;
    idx->user_data = user_data;
    return idx;
}

void indexer_set_excludes(Indexer* idx, GPtrArray* paths) {
    g_return_if_fail(idx != NULL);
    g_return_if_fail(idx->thread == NULL); /* must be set before start */

    destroy_skip_sets(idx);
    build_skip_sets(idx, paths);
    if (paths) g_ptr_array_unref(paths);
}

void indexer_start(Indexer* idx) {
    g_return_if_fail(idx != NULL);
    if (!idx->skip_exact) {
        build_skip_sets(idx, NULL);
    }
    idx->thread = g_thread_new("ff-indexer", indexer_thread, idx);
}

void indexer_cancel(Indexer* idx) {
    g_return_if_fail(idx != NULL);
    g_atomic_int_set(&idx->cancelled, 1);
    if (idx->thread) {
        g_thread_join(idx->thread);
        idx->thread = NULL;
    }
    destroy_skip_sets(idx);
    g_free(idx->root);
    g_free(idx);
}

gboolean indexer_is_done(Indexer* idx) { return (gboolean)g_atomic_int_get(&idx->done); }
guint indexer_count(Indexer* idx) { return (guint)g_atomic_int_get(&idx->count); }

/* ── Config-file helpers ────────────────────────────────────────── */

char* excludes_config_path(void) { return g_build_filename(g_get_user_config_dir(), "fastfind", "excludes", NULL); }

GPtrArray* excludes_load(void) {
    GPtrArray* arr = g_ptr_array_new_with_free_func(g_free);
    char* cfg      = excludes_config_path();
    GError* err    = NULL;
    char* txt      = NULL;
    gsize len      = 0;

    if (!g_file_get_contents(cfg, &txt, &len, &err)) {
        g_clear_error(&err); /* ENOENT is fine — first run */
        g_free(cfg);
        return arr;
    }
    g_free(cfg);

    char** lines = g_strsplit(txt, "\n", -1);
    g_free(txt);

    for (int i = 0; lines[i]; i++) {
        g_strstrip(lines[i]);
        if (lines[i][0] != '\0' && lines[i][0] != '#') {
            g_ptr_array_add(arr, g_strdup(lines[i]));
        }
    }
    g_strfreev(lines);
    return arr;
}

void excludes_save(GPtrArray* paths) {
    char* cfg   = excludes_config_path();
    char* dir   = g_path_get_dirname(cfg);
    GError* err = NULL;

    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    GString* buf = g_string_new(
        "# FastFind exclude list\n"
        "# One absolute path per line.  Lines starting with '#' are ignored.\n");

    for (guint i = 0; i < paths->len; i++) {
        const char* p = g_ptr_array_index(paths, i);
        if (p && *p) g_string_append_printf(buf, "%s\n", p);
    }

    if (!g_file_set_contents(cfg, buf->str, (gssize)buf->len, &err)) {
        g_warning("excludes_save: %s", err->message);
        g_error_free(err);
    }

    g_string_free(buf, TRUE);
    g_free(cfg);
}
