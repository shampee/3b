////////////////////////////////
//~ OS-facing primitives

bbb_String8
bbb_os_getenv(bbb_Arena arena, bbb_String8 name) {
  char*       cname = bbb_cstring_str8(bbb_ctx_scratch(), name);
  const char* value = getenv(cname);
  if (!value) return (bbb_String8){0};
  return bbb_str8_copy(&arena, bbb_str8_cstring((char*)value));
}

// popen/_popen -- the same technique bcosprims.c's bytecode-VM-side
// `os/exec-capture` uses, so the two agree on the one observable detail a
// caller can see: which bytes come back.
bbb_String8
bbb_os_exec_capture(bbb_Arena arena, bbb_String8 cmd) {
  char* ccmd = bbb_cstring_str8(bbb_ctx_scratch(), cmd);
#if defined(_WIN32)
  FILE* p = _popen(ccmd, "r");
#else
  FILE* p = popen(ccmd, "r");
#endif
  if (!p) return (bbb_String8){0};

  // Grown directly in `arena`, one doubling at a time: the alternative --
  // staging in scratch and copying across -- breaks when `arena` IS the
  // scratch arena, which is the common case for a caller inside a
  // `(scratch [temp] ...)` block.
  u8* out = NULL; // bbb dyn array
  u8  chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), p)) > 0) {
    for (size_t i = 0; i < n; i += 1) bbb_dyn_push(&arena, out, chunk[i]);
  }
#if defined(_WIN32)
  _pclose(p);
#else
  pclose(p);
#endif

  u64 len = bbb_dyn_count(out);
  if (len > 0 && out[len - 1] == '\n') len -= 1;
  if (len > 0 && out[len - 1] == '\r') len -= 1;
  return (bbb_String8){ .str = out, .size = len };
}

i64
bbb_os_time_unix(void) {
  return (i64)time(NULL);
}

void
bbb_os_sleep(u32 seconds) {
#if defined(_WIN32)
  Sleep(seconds * 1000);
#else
  sleep(seconds);
#endif
}
