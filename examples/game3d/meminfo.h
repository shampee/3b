// Hand-written, not translator output -- there's no library here to bind,
// just one small platform query 3b's own `os` module doesn't expose (see
// meminfo.c's own comment for why). `-include`d before compiling game3d's
// own generated .c (build.cfg.3b's `include-first` entry for `game3d`
// itself, not just the usual third-party binding packages) -- without a
// real prototype in scope, this falls back to an implicit declaration,
// which for a `u64`-returning function assumes a 32-bit `int` return
// instead, silently truncating -- the exact class of bug documented in
// build.cfg.3b's own big comment, hit once already with stbtt's `float`
// argument.
//
// Named `meminfo_*`, NOT `game3d_*` -- meminfo.3b's own 3b-facing wrapper
// (`process-rss-bytes`) mangles to the C name `game3d_process_rss_bytes`
// (package `game3d` + the wrapper's own stripped name), so naming the
// REAL function that exact string would collide head-on with its own
// wrapper the same way stbi/stbimg's rename fixed for stb_image (see
// stbimg.cfg.3b's own comment) -- hit here too, immediately after fixing
// it there, before catching it via a `3b build` link error.
unsigned long long meminfo_process_rss_bytes(void);
