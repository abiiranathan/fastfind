#pragma once
#include <glib-object.h>

G_BEGIN_DECLS

#define FILE_TYPE_ITEM (file_item_get_type())
G_DECLARE_FINAL_TYPE(FileItem, file_item, FILE, ITEM, GObject)

FileItem* file_item_new(const char* name, const char* path, goffset size, gboolean is_dir, guint64 mtime);

const char* file_item_get_name(FileItem* self);
const char* file_item_get_path(FileItem* self);
const char* file_item_get_dir(FileItem* self); /* parent dir */
goffset file_item_get_size(FileItem* self);
gboolean file_item_get_is_dir(FileItem* self);
guint64 file_item_get_mtime(FileItem* self);
const char* file_item_get_size_str(FileItem* self); /* human-readable */
const char* file_item_get_type_str(FileItem* self);

G_END_DECLS
