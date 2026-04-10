#pragma once
/*
 * fileitem.h
 *
 * Declares the FileItem type and its API for representing files and directories in the FastFind index.
 *
 * FileItem is a GObject-based structure with optimized memory usage:
 *   - Stores full path, basename offset, and interned parent directory
 *   - Provides accessors for name, path, dir, size, type, and mtime
 *   - Used by the indexer and UI for displaying and filtering files
 *
 * See fileitem.c for implementation details.
 */
#include <glib-object.h>

G_BEGIN_DECLS

#define FILE_TYPE_ITEM (file_item_get_type())
G_DECLARE_FINAL_TYPE(FileItem, file_item, FILE, ITEM, GObject)

/*
 * Memory layout (per item):
 *   - ONE heap allocation: path  (g_strdup)
 *   - name  → pointer into path  (no extra alloc)
 *   - dir   → g_intern_string    (deduplicated globally, never freed per-item)
 *
 * This cuts per-item allocations from 3 → 1 compared with the old
 * (name, path, dir) triple.  Directories with many files share a single
 * interned dir string in the GLib atom table.
 */
FileItem* file_item_new(const char* path, goffset size, gboolean is_dir, guint64 mtime);

const char* file_item_get_name(FileItem* self); /* basename; points into path */
const char* file_item_get_path(FileItem* self);
const char* file_item_get_dir(FileItem* self); /* interned parent dir        */
goffset file_item_get_size(FileItem* self);
gboolean file_item_get_is_dir(FileItem* self);
guint64 file_item_get_mtime(FileItem* self);
const char* file_item_get_size_str(FileItem* self); /* human-readable size        */
const char* file_item_get_type_str(FileItem* self); /* "Directory" | extension    */

G_END_DECLS
