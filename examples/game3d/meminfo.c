// Current process RSS (resident set size -- actual physical memory in
// use right now, not a high-water mark) in bytes. Linux-only: reads
// /proc/self/statm's 2nd field (RSS, in pages) and multiplies by the
// page size. Deliberately NOT going through 3b's own `os/read-file`
// (runtime/bbb_file.c) -- that does fseek/ftell to size the file up
// front, which reports 0 for a procfs pseudo-file (no real byte length
// to seek to), so it would fail to read this every time. Plain
// fopen+fscanf sidesteps that entirely: no seeking, just read the two
// leading whitespace-separated integers off the front of the file.
//
// getrusage()'s ru_maxrss was the other option -- simpler (no file I/O),
// but reports the process's PEAK RSS ever, not its CURRENT usage, which
// is what a live "how much memory is this using RIGHT NOW" HUD stat
// actually wants.
#include <stdio.h>
#include <unistd.h>

unsigned long long meminfo_process_rss_bytes(void) {
  FILE* f = fopen("/proc/self/statm", "r");
  if (!f) return 0;
  long total_pages = 0, rss_pages = 0;
  int n = fscanf(f, "%ld %ld", &total_pages, &rss_pages);
  fclose(f);
  if (n != 2) return 0;
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return 0;
  return (unsigned long long)rss_pages * (unsigned long long)page_size;
}
