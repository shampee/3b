// File loading and directory listing. Two ways to read a file: file_load
// copies into an arena, file_map maps it copy-on-write for zero-copy reads.
#include "file.h"
#include "3b.h" // for cstr_from_str8_temp
#if !defined(_WIN32)
# include <sys/stat.h>
# include <fcntl.h>
# include <dirent.h>
#endif
// windows.h arrives transitively through file.h -> base/base.h.

// ANSI C stdio rather than a raw POSIX fd, so this needs no platform branch at
// all. `ftell` returns long, which caps one load at ~2GB under Windows' LLP64
// model -- fine for source files.
File
file_load(Arena* arena, String8 path) {
  File result = {0};

  const char* cpath = cstr_from_str8_temp(path);
  FILE* f           = fopen(cpath, "rb");
  if (!f) {
    fprintf(stderr, "file_load(): failed to open\n");
    return result;
  }

  if (fseek(f, 0, SEEK_END) != 0) {
    fprintf(stderr, "file_load(): failed to seek\n");
    fclose(f);
    return result;
  }
  long size = ftell(f);
  if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
    fprintf(stderr, "file_load(): empty or invalid file\n");
    fclose(f);
    return result;
  }

  u8* dst = push_array(arena, u8, size);
  if (!dst) {
    fprintf(stderr, "file_load(): failed to allocate memory\n");
    fclose(f);
    return result;
  }

  u64 total_read = fread(dst, 1, (u64)size, f);
  fclose(f);
  if (total_read != (u64)size) {
    fprintf(stderr, "file_load(): failed to read\n");
    return result;
  }

  result.view = const_view(dst, size);
  result.path = path;
  return result;
}

b32
file_store(File* file, String8 path) {
  const char* cpath = cstr_from_str8_temp(path);
  FILE* f           = fopen(cpath, "wb");
  if (!f) {
    fprintf(stderr, "file_store(): failed to open\n");
    return false;
  }
  b32 ok = fwrite(file->view.data, 1, file->view.size, f) == file->view.size;
  fclose(f);
  return ok;
}

// Zero-copy read through mmap, mapped writable copy-on-write rather than
// read-only so a caller can patch a few known offsets in place -- bcio.c's
// loader resolves a handful of pointers this way -- without copying the whole
// file first. Copy-on-write means the OS lazily gives a private copy of just
// the pages actually written; the file on disk is never modified, and every
// untouched page stays a shared mapping backed by the page cache.
#if !defined(_WIN32)

File
file_map(String8 path) {
  File result = {0};

  i32 fd = open((char*)path.str, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "file_map(): failed to open\n");
    return result;
  }

  struct stat st;
  i64 size = (fstat(fd, &st) == 0) ? st.st_size : -1;
  if (size <= 0) {
    fprintf(stderr, "file_map(): empty or invalid file\n");
    close(fd);
    return result;
  }

  void* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  close(fd);

  if (data == MAP_FAILED) {
    fprintf(stderr, "file_map(): failed to map file\n");
    return result;
  }

  result.view = const_view(data, size);
  result.path = path;
  return result;
}

void
file_unmap(File* file) {
  if (file && file->view.data) {
    munmap((void*)file->view.data, file->view.size);
    *file = (File){0};
  }
}

#else // _WIN32

File
file_map(String8 path) {
  File result = {0};

  const char* cpath = cstr_from_str8_temp(path);
  HANDLE hfile = CreateFileA(cpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hfile == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "file_map(): failed to open\n");
    return result;
  }

  LARGE_INTEGER size;
  if (!GetFileSizeEx(hfile, &size) || size.QuadPart <= 0) {
    fprintf(stderr, "file_map(): empty or invalid file\n");
    CloseHandle(hfile);
    return result;
  }

  // The Windows analogue of PROT_WRITE|MAP_PRIVATE: writable, copy-on-write,
  // never touching the file on disk. The mapping handle can be closed as soon
  // as MapViewOfFile succeeds -- the view stays valid on its own, as with the
  // POSIX fd above.
  HANDLE hmap = CreateFileMappingA(hfile, NULL, PAGE_WRITECOPY, 0, 0, NULL);
  CloseHandle(hfile);
  if (!hmap) {
    fprintf(stderr, "file_map(): failed to create file mapping\n");
    return result;
  }

  void* data = MapViewOfFile(hmap, FILE_MAP_COPY, 0, 0, 0);
  CloseHandle(hmap);
  if (!data) {
    fprintf(stderr, "file_map(): failed to map file\n");
    return result;
  }

  result.view = const_view(data, (u64)size.QuadPart);
  result.path = path;
  return result;
}

void
file_unmap(File* file) {
  if (file && file->view.data) {
    UnmapViewOfFile((void*)file->view.data);
    *file = (File){0};
  }
}

#endif

static int
str8_qsort_cmp(const void* a, const void* b) {
  const String8* sa = (const String8*)a;
  const String8* sb = (const String8*)b;
  u64 n = sa->size < sb->size ? sa->size : sb->size;
  int c = n > 0 ? memcmp(sa->str, sb->str, n) : 0;
  if (c != 0) return c;
  if (sa->size < sb->size) return -1;
  if (sa->size > sb->size) return 1;
  return 0;
}

#if defined(_WIN32)

String8*
dir_list_files_with_ext(Arena* arena, String8 dir_path, const char* ext, u64* out_count) {
  if (out_count) *out_count = 0;

  String8 trimmed = dir_path;
  while (trimmed.size > 0 && char_is_slash(trimmed.str[trimmed.size - 1])) trimmed.size -= 1;

  u64 ext_len = strlen(ext);
  ArenaTemp temp = arena_temp_begin(ctx_scratch());
  String8*  names = NULL; // just the filenames, for now -- full paths built after sorting

  String8 pattern      = str8_cat(temp.arena, trimmed, str8_lit("/*"));
  char*   pattern_cstr = cstr_from_str8_temp(pattern);

  WIN32_FIND_DATAA find_data;
  HANDLE h = FindFirstFileA(pattern_cstr, &find_data);
  if (h == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "dir_list_files_with_ext(): failed to open directory '%.*s'\n", str8_varg(trimmed));
    arena_temp_end(&temp);
    return NULL;
  }

  do {
    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue; // subdirectories are always separate packages
    String8 name = str8_cstring(find_data.cFileName);
    if (name.size < ext_len) continue;
    String8 suffix = str8_substr(name, rng_1u64(name.size - ext_len, name.size));
    if (!str8_match(suffix, str8_cstring((char*)ext), 0)) continue;

    dyn_push(temp.arena, names, str8_copy(temp.arena, name));
  } while (FindNextFileA(h, &find_data));
  FindClose(h);

  u64 count = dyn_count(names);
  if (count == 0) {
    arena_temp_end(&temp);
    return NULL;
  }

  qsort(names, count, sizeof(String8), str8_qsort_cmp);

  String8* result = push_array(arena, String8, count);
  foreach_index(i, count) {
    String8 full = str8_cat(arena, trimmed, str8_lit("/"));
    result[i]    = str8_cat(arena, full, names[i]);
  }
  arena_temp_end(&temp);

  if (out_count) *out_count = count;
  return result;
}

#else // POSIX (Linux/Mac)

String8*
dir_list_files_with_ext(Arena* arena, String8 dir_path, const char* ext, u64* out_count) {
  if (out_count) *out_count = 0;

  String8 trimmed = dir_path;
  while (trimmed.size > 0 && char_is_slash(trimmed.str[trimmed.size - 1])) trimmed.size -= 1;

  const char* dir_cstr = cstr_from_str8_temp(trimmed);
  DIR* dir = opendir(dir_cstr);
  if (!dir) {
    fprintf(stderr, "dir_list_files_with_ext(): failed to open directory '%s'\n", dir_cstr);
    return NULL;
  }

  u64 ext_len = strlen(ext);
  ArenaTemp temp = arena_temp_begin(ctx_scratch());
  String8*  names = NULL; // just the filenames, for now -- full paths built after sorting

  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    String8 name = str8_cstring(entry->d_name);
    if (name.size < ext_len) continue;
    String8 suffix = str8_substr(name, rng_1u64(name.size - ext_len, name.size));
    if (!str8_match(suffix, str8_cstring((char*)ext), 0)) continue;

    // Skip directories that merely end in the extension: a subdirectory is
    // always a separate package, never a source file.
    String8 full = str8_cat(temp.arena, trimmed, str8_lit("/"));
    full         = str8_cat(temp.arena, full, name);
    struct stat st;
    if (stat((char*)cstr_from_str8_temp(full), &st) != 0 || !S_ISREG(st.st_mode)) continue;

    // readdir reuses d_name's buffer, so the name must be copied out now.
    dyn_push(temp.arena, names, str8_copy(temp.arena, name));
  }
  closedir(dir);

  u64 count = dyn_count(names);
  if (count == 0) {
    arena_temp_end(&temp);
    return NULL;
  }

  qsort(names, count, sizeof(String8), str8_qsort_cmp);

  String8* result = push_array(arena, String8, count);
  foreach_index(i, count) {
    String8 full = str8_cat(arena, trimmed, str8_lit("/"));
    result[i]    = str8_cat(arena, full, names[i]);
  }
  arena_temp_end(&temp);

  if (out_count) *out_count = count;
  return result;
}

#endif
