#ifndef FILE_H
#define FILE_H
#include "base/base.h"

typedef struct File {
  ConstView view;
  String8   path;
} File;

File file_load(Arena* arena, String8 path);
b32  file_store(File* file, String8 path);
File file_map(String8 path);
void file_unmap(File* file);

// Lists regular files directly inside `dir_path` (NOT recursive -- a
// subdirectory is a separate package, never silently pulled in) whose name
// ends in `ext` (e.g. ".3b"). Returned paths are `dir_path` joined with the
// filename, arena-allocated, sorted lexicographically so callers get a
// deterministic compile order regardless of the OS's own readdir order.
String8* dir_list_files_with_ext(Arena* arena, String8 dir_path, const char* ext, u64* out_count);

#define file_load_str8(a, p) view_into_str8(file_load((a), (p)).view)

static inline FILE*
file_open(const char* path, const char* mode) {
  FILE* result = fopen(path, mode);
  if (!result) {
    fprintf(stderr, "file_scope: failed to open file '%s' with mode '%s'\n", path, mode);
    return NULL;
  }
  return result;
}

#define file_scope(it, path, mode)                                             \
  for (FILE* it = file_open(path, mode); it; it = (fclose(it), NULL))

#endif
