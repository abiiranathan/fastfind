#include "fileitem.h"
/*
 * fileitem.c
 *
 * Implements the FileItem object, which represents a file or directory in the FastFind index.
 * Handles memory management, path/basename/dir logic, and provides accessors for file metadata.
 *
 * Key features:
 *   - Efficient memory layout for per-file data
 *   - Interned directory strings for deduplication
 *   - Pre-formatted size strings for display
 *
 * See fileitem.h for API details.
 */
#include <glib.h>
#include <string.h>

struct _FileItem {
    GObject parent_instance;
    char* path;       /* owned; full path                        */
    const char* dir;  /* interned via g_intern_string; NOT freed  */
    guint32 name_off; /* byte offset of basename within path      */
    gboolean is_dir;
    goffset size;
    guint64 mtime;
    char size_buf[20]; /* pre-formatted human-readable size      */
};

G_DEFINE_TYPE(FileItem, file_item, G_TYPE_OBJECT)

static void file_item_finalize(GObject* obj) {
    FileItem* self = FILE_ITEM(obj);
    g_free(self->path);
    /* dir is interned via g_intern_string — must NOT be freed */
    G_OBJECT_CLASS(file_item_parent_class)->finalize(obj);
}

static void file_item_class_init(FileItemClass* klass) { G_OBJECT_CLASS(klass)->finalize = file_item_finalize; }
static void file_item_init(FileItem* self) { (void)self; }

FileItem* file_item_new(const char* path, goffset size, gboolean is_dir, guint64 mtime) {
    g_return_val_if_fail(path != NULL, NULL);

    FileItem* self = g_object_new(FILE_TYPE_ITEM, NULL);
    self->path     = g_strdup(path);
    self->is_dir   = is_dir;
    self->size     = size;
    self->mtime    = mtime;

    /* ── Compute basename offset ──────────────────────────────── */
    const char* last_slash = strrchr(path, '/');
    if (last_slash && last_slash[1] != '\0') {
        self->name_off = (guint32)(last_slash + 1 - path);
    } else {
        self->name_off = 0;
    }

    /* ── Intern the parent-directory string ───────────────────── */
    if (last_slash && last_slash != path) {
        /* Temporary stack buffer via g_strndup; freed after interning */
        gsize dir_len = (gsize)(last_slash - path);
        char* tmp     = g_strndup(path, dir_len);
        self->dir     = g_intern_string(tmp);
        g_free(tmp);
    } else {
        self->dir = g_intern_string("/");
    }

    /* ── Pre-format size string ───────────────────────────────── */
    if (is_dir) {
        /* U+2014 EM DASH in UTF-8 */
        g_strlcpy(self->size_buf, "\xe2\x80\x94", sizeof(self->size_buf));
    } else if (size < 1024) {
        g_snprintf(self->size_buf, sizeof(self->size_buf), "%" G_GOFFSET_FORMAT " B", size);
    } else if (size < 1024 * 1024) {
        g_snprintf(self->size_buf, sizeof(self->size_buf), "%.1f KB", size / 1024.0);
    } else if (size < 1024LL * 1024 * 1024) {
        g_snprintf(self->size_buf, sizeof(self->size_buf), "%.1f MB", size / (1024.0 * 1024));
    } else {
        g_snprintf(self->size_buf, sizeof(self->size_buf), "%.2f GB", size / (1024.0 * 1024 * 1024));
    }

    return self;
}

const char* file_item_get_name(FileItem* s) { return s->path + s->name_off; }
const char* file_item_get_path(FileItem* s) { return s->path; }
const char* file_item_get_dir(FileItem* s) { return s->dir; }
goffset file_item_get_size(FileItem* s) { return s->size; }
gboolean file_item_get_is_dir(FileItem* s) { return s->is_dir; }
guint64 file_item_get_mtime(FileItem* s) { return s->mtime; }
const char* file_item_get_size_str(FileItem* s) { return s->size_buf; }

const char* file_item_get_type_str(FileItem* s) {
    if (s->is_dir) return "Directory";
    const char* name = s->path + s->name_off;
    const char* dot  = strrchr(name, '.');
    if (!dot || dot == name) return "File";
    return dot + 1; /* extension without leading dot */
}
