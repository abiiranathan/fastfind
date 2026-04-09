#include "fileitem.h"
#include <glib.h>
#include <string.h>

struct _FileItem {
    GObject parent_instance;
    char* name;
    char* path;
    char* dir;
    goffset size;
    gboolean is_dir;
    guint64 mtime;
    char size_buf[32];
};

G_DEFINE_TYPE(FileItem, file_item, G_TYPE_OBJECT)

static void file_item_finalize(GObject* obj) {
    FileItem* self = FILE_ITEM(obj);
    g_free(self->name);
    g_free(self->path);
    g_free(self->dir);
    G_OBJECT_CLASS(file_item_parent_class)->finalize(obj);
}

static void file_item_class_init(FileItemClass* klass) {
    GObjectClass* obj_class = G_OBJECT_CLASS(klass);
    obj_class->finalize     = file_item_finalize;
}

static void file_item_init(FileItem* self) { (void)self; }

FileItem* file_item_new(const char* name, const char* path, goffset size, gboolean is_dir, guint64 mtime) {
    FileItem* self = g_object_new(FILE_TYPE_ITEM, NULL);
    self->name     = g_strdup(name);
    self->path     = g_strdup(path);
    self->is_dir   = is_dir;
    self->size     = size;
    self->mtime    = mtime;

    /* compute parent dir */
    char* last_slash = strrchr(path, '/');
    if (last_slash && last_slash != path) {
        self->dir = g_strndup(path, (gsize)(last_slash - path));
    } else {
        self->dir = g_strdup("/");
    }

    /* pre-format size string */
    if (is_dir) {
        g_strlcpy(self->size_buf, "—", sizeof(self->size_buf));
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

const char* file_item_get_name(FileItem* s) { return s->name; }
const char* file_item_get_path(FileItem* s) { return s->path; }
const char* file_item_get_dir(FileItem* s) { return s->dir; }
goffset file_item_get_size(FileItem* s) { return s->size; }
gboolean file_item_get_is_dir(FileItem* s) { return s->is_dir; }
guint64 file_item_get_mtime(FileItem* s) { return s->mtime; }
const char* file_item_get_size_str(FileItem* s) { return s->size_buf; }

const char* file_item_get_type_str(FileItem* s) {
    if (s->is_dir) return "Directory";
    const char* dot = strrchr(s->name, '.');
    if (!dot || dot == s->name) return "File";
    return dot + 1;
}
