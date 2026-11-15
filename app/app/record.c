// See record.h.
#include "record.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define sig_t gen_sig_t   // the generated table's name, not <signal.h>'s
#include "signals_gen.h"

const char *record_dir = "/data/rec";

static FILE *fp, *fc;            // <name>.panda, <name>.cmdr (NULL until the first push)
static char name[32];
static int64_t t0, flushed, capped;
static bool failed;               // stopped on an error: until the switch goes off
static bool wrote;                // anything in <name>.panda: an empty session is deleted as it closes
static char buf[64 * 1024];       // ~a second of CAN, so stdio writes about as often as the flush

static void close_all(void) {
  if (fp) {
    fclose(fp);
    char p[256];
    snprintf(p, sizeof p, "%s/%s.panda", record_dir, name);
    if (!wrote && !fc) unlink(p);   // no link came up: no file (a restart loop would leave thousands)
  }
  if (fc) fclose(fc);
  fp = fc = NULL;
}

static void fail(const char *what, const char *why) {
  log_msg("record %s: %s; recording stopped", what, why);
  close_all();
  failed = true;
}

static FILE *open_in(const char *base, const char *ext, const char *mode) {
  char p[256];
  snprintf(p, sizeof p, "%s/%s.%s", record_dir, base, ext);
  FILE *f = fopen(p, mode);
  if (f) fcntl(fileno(f), F_SETFD, FD_CLOEXEC);   // a daemon a script starts must not keep a pruned recording's space
  return f;
}

static int64_t ms(int64_t at) { return at < t0 ? 0 : at - t0 > UINT32_MAX ? UINT32_MAX : at - t0; }

// Oldest sessions (by mtime) go until the folder fits max_mb (0: no cap). The current one alone too big: stop.
// ponytail: a rescan per deletion, fine for the hundred-odd sessions a card holds.
static void prune(int max_mb) {
  while (max_mb > 0) {
    DIR *dir = opendir(record_dir);
    if (!dir) { fail(record_dir, strerror(errno)); return; }
    long long total = 0;
    time_t old_t = 0;
    char old[64] = "", p[512];
    for (struct dirent *e; (e = readdir(dir));) {
      char *dot = strrchr(e->d_name, '.');
      struct stat st;
      if (!dot || (strcmp(dot, ".panda") && strcmp(dot, ".cmdr")) || dot - e->d_name >= (int)sizeof old) continue;
      snprintf(p, sizeof p, "%s/%s", record_dir, e->d_name);
      if (stat(p, &st)) continue;
      total += st.st_size;
      *dot = 0;
      if (strcmp(e->d_name, name) && (!old[0] || st.st_mtime < old_t)) snprintf(old, sizeof old, "%.63s", e->d_name), old_t = st.st_mtime;
    }
    closedir(dir);
    if (total <= (long long)max_mb << 20) return;
    if (!old[0]) { fail(name, "over record_max_mb"); return; }
    snprintf(p, sizeof p, "%s/%s.panda", record_dir, old);
    bool ok = !unlink(p) || errno == ENOENT;
    snprintf(p, sizeof p, "%s/%s.cmdr", record_dir, old);
    ok = (!unlink(p) || errno == ENOENT) && ok;
    if (!ok) { fail(old, strerror(errno)); return; }   // never the same oldest forever: the loop would starve the watchdog
    log_msg("record: deleted %s (record_max_mb %d)", old, max_mb);
  }
}

static void start(const conf_t *cf, int64_t now) {
  if (mkdir(record_dir, 0755) && errno != EEXIST) { fail(record_dir, strerror(errno)); return; }
  struct stat sr, sd;   // /data not mounted (no p3, or fsck refused it): /data is the RAM root, 256 MB in all
  if (!strncmp(record_dir, "/data/", 6) && !stat("/", &sr) && !stat(record_dir, &sd) && sr.st_dev == sd.st_dev) {
    fail(record_dir, "not on the card's data partition");
    return;
  }
  wrote = false;
  time_t t = time(NULL);
  struct tm tm;
  if (localtime_r(&t, &tm) && tm.tm_year + 1900 > 2025) {
    strftime(name, sizeof name, "%Y%m%d-%H%M%S", &tm);
    fp = open_in(name, "panda", "wx");
  }
  if (!fp) {   // no real clock yet (or that second taken): the next rec-NNNN
    int top = 0, k;
    DIR *dir = opendir(record_dir);
    for (struct dirent *e; dir && (e = readdir(dir));)
      if (sscanf(e->d_name, "rec-%d", &k) == 1 && k > top) top = k;
    if (dir) closedir(dir);
    snprintf(name, sizeof name, "rec-%04d", top + 1);
    fp = open_in(name, "panda", "wx");
  }
  if (!fp) { fail(name, strerror(errno)); return; }
  setvbuf(fp, buf, _IOFBF, sizeof buf);
  t0 = flushed = capped = now;
  log_msg("record: %s/%s", record_dir, name);
  prune(cf->record_max_mb);
}

void record_sync(bool on, const conf_t *cf, int64_t now) {
  if (!on) {
    if (fp) log_msg("record: %s stopped", name), close_all();
    failed = false;
    return;
  }
  if (failed) return;
  if (!fp) start(cf, now);
  if (fp && now - flushed >= 1000) {   // no fsync: the page cache writes it out (sysctl: within ~3 s), a power cut loses that
    flushed = now;
    if (fflush(fp) || (fc && fflush(fc))) fail(name, strerror(errno));
  }
  if (fp && now - capped >= 10000) capped = now, prune(cf->record_max_mb);
}

void record_panda(const uint8_t *d, size_t n, int64_t at) {
  static uint8_t b[6 + 4096];
  if (!fp || !n || n % 16 || n > 4096) return;
  size_t k = 6;
  for (size_t o = 0; o < n; o += 16) {   // only the ids the app subscribes to (the ACK and strays left out)
    unsigned id = (unsigned)(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (unsigned)d[o + 3] << 24) >> 21;
    if ((d[o + 4] & 15) && sub_index(id) >= 0) memcpy(b + k, d + o, 16), k += 16;
  }
  if (k == 6) return;
  uint32_t t = (uint32_t)ms(at);
  uint16_t len = (uint16_t)(k - 6);
  b[0] = t, b[1] = t >> 8, b[2] = t >> 16, b[3] = t >> 24, b[4] = len, b[5] = len >> 8;
  if (fwrite(b, 1, k, fp) != k) fail(name, strerror(errno));
  else wrote = true;
}

void record_cmdr(const bt_map_t *m, int64_t at) {
  if (!fp || (!m && !fc)) return;   // a link drop before any push (a step-aside, a dial) is not a recording
  if (!fc && !(fc = open_in(name, "cmdr", "w"))) { fail(name, strerror(errno)); return; }
  fprintf(fc, "%lld", (long long)ms(at));
  if (!m) fputs(" -", fc);
  for (int f = 0; m && f < BT_FIELDS; f++)
    if (m->has[f] && m->is_int[f]) fprintf(fc, " %d=%lld", f, (long long)m->v[f]);
    else if (m->has[f]) fprintf(fc, " %d=x%llx", f, (unsigned long long)m->v[f]);
  if (fputc('\n', fc) == EOF) fail(name, strerror(errno));
}
