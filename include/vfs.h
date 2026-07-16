#ifndef VFS_H
#define VFS_H

/* ── Minimal VFS bridge ─────────────────────────────────────────────────────
   Exposes a safe, read-only view of the kernel's in-memory filesystem to
   C++ GUI code. Never exposes raw pointers into the file_system[] array.
   All data is copied into caller-provided buffers.                         */

#define VFS_MAX_NAME 24

#define VFS_TYPE_FILE 0
#define VFS_TYPE_DIR  1

typedef struct {
    char name[VFS_MAX_NAME];
    int  type;       /* VFS_TYPE_FILE or VFS_TYPE_DIR */
    int  id;         /* slot + 1, matches parent_id of children */
    int  parent_id;
    int  size;       /* byte count of content */
} VFSEntry;

#ifdef __cplusplus
extern "C" {
#endif

/* Fill out[] with entries whose parent_id == dir_id.
   Returns the number of entries written (≤ max_count).
   Pass dir_id = 0 for root.                                               */
int vfs_list_dir(int dir_id, VFSEntry* out, int max_count);

/* Copy the content of entry_id (a VFS_TYPE_FILE entry) into buf.
   Returns byte count written (without null), or -1 on error.             */
int vfs_read_file(int entry_id, char* buf, int buf_size);

/* Create a new empty file/directory under parent_id.
   Returns the new entry id (> 0) on success, -1 on failure (disk full,
   invalid name, or invalid parent).                                       */
int vfs_create_file(const char* name, int parent_id);
int vfs_create_dir (const char* name, int parent_id);

/* Delete entry_id.  Returns 0 on success, -1 if not found.               */
int vfs_delete(int entry_id);

/* Rename entry_id to new_name.  Returns 0 on success, -1 on failure.     */
int vfs_rename(int entry_id, const char* new_name);

/* Write data into entry_id (must be VFS_TYPE_FILE).
   Returns bytes written, or -1 on error.  Truncates to FILE_CONTENT_SIZE. */
int vfs_write_file(int entry_id, const char* data, int len);

/* Resolve a name to its entry id (> 0) under parent_id (0 = current dir).
   Returns -1 if not found.  Used by the SYS_OPEN syscall.                 */
int vfs_find(const char* name, int parent_id);

/* The kernel's current working directory ID (mirrors current_dir_id).     */
extern int current_dir_id;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VFS_H */
