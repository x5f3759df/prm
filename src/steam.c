#include "prm.h"

#include <ctype.h>
#include <dirent.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_err_buf[1024];

int prm_err(const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    memcpy(g_err_buf, tmp, sizeof(g_err_buf));
    return -1;
}

const char *prm_err_str(void) {
    return g_err_buf;
}

bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

char *read_file_str(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

static void resolve_config_dir(const char *home, char *out, size_t cap) {
    const char *env_dir = getenv("PRM_CONFIG_DIR");
    if (env_dir && *env_dir) {
        snprintf(out, cap, "%s", env_dir);
        return;
    }
    if (path_is_dir("config")) {
        snprintf(out, cap, "config");
        return;
    }

    char exe[PATH_MAX_LEN];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            char cand[PATH_MAX_LEN];
            snprintf(cand, sizeof(cand), "%s/config", exe);
            if (path_is_dir(cand)) {
                snprintf(out, cap, "%s", cand);
                return;
            }
            for (int up = 0; up < 2; up++) {
                char *p = strrchr(exe, '/');
                if (p) *p = '\0';
            }
            snprintf(cand, sizeof(cand), "%s/config", exe);
            if (path_is_dir(cand)) {
                snprintf(out, cap, "%s", cand);
                return;
            }
        }
    }

    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(out, cap, "%s/prm", xdg);
    } else {
        snprintf(out, cap, "%s/.config/prm", home);
    }
}

int prm_paths_resolve(struct prm_paths *out) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        return prm_err("HOME environment variable not set");
    }

    snprintf(out->steam_dir, sizeof(out->steam_dir), "%s/.local/share/Steam", home);
    if (!path_exists(out->steam_dir)) {
        snprintf(out->steam_dir, sizeof(out->steam_dir), "%s/.steam/root", home);
    }
    snprintf(out->compat_dir, sizeof(out->compat_dir), "%s/compatibilitytools.d", out->steam_dir);

    const char *xdg_cache = getenv("XDG_CACHE_HOME");
    if (xdg_cache && *xdg_cache) {
        snprintf(out->cache_dir, sizeof(out->cache_dir), "%s/prm", xdg_cache);
    } else {
        snprintf(out->cache_dir, sizeof(out->cache_dir), "%s/.cache/prm", home);
    }

    resolve_config_dir(home, out->config_dir, sizeof(out->config_dir));
    return 0;
}

static uint64_t g_accum_bytes;

static int sum_size_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)fpath;
    (void)ftwbuf;
    if (typeflag == FTW_F && sb) {
        g_accum_bytes += (uint64_t)sb->st_size;
    }
    return 0;
}

uint64_t dir_size(const char *path) {
    g_accum_bytes = 0;
    nftw(path, sum_size_cb, 32, FTW_PHYS);
    return g_accum_bytes;
}

static void read_dll_version(const char *runner_path, const char *comp, char *out, size_t out_cap) {
    static const char *const prefixes[] = {
        "files/lib/wine",
        "files/lib64/wine",
        "dist/lib/wine",
        "dist/lib64/wine",
    };
    out[0] = '\0';

    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        char cand[PATH_MAX_LEN];
        snprintf(cand, sizeof(cand), "%s/%s/%s/version", runner_path, prefixes[i], comp);
        char *raw = read_file_str(cand, NULL);
        if (!raw) continue;

        char *nl = strpbrk(raw, "\r\n");
        if (nl) *nl = '\0';

        char *line = raw;
        while (*line && isspace((unsigned char)*line)) line++;
        size_t len = strlen(line);
        while (len > 0 && isspace((unsigned char)line[len - 1])) line[--len] = '\0';
        if (len == 0) {
            free(raw);
            continue;
        }

        char *lp = strchr(line, '(');
        char *rp = strrchr(line, ')');
        if (lp && rp && lp < rp) {
            *rp = '\0';
            lp++;
            while (*lp && isspace((unsigned char)*lp)) lp++;
            size_t plen = strlen(lp);
            while (plen > 0 && isspace((unsigned char)lp[plen - 1])) lp[--plen] = '\0';
            snprintf(out, out_cap, "%s", lp);
            free(raw);
            return;
        }

        bool all_hex = (len == 40);
        for (size_t k = 0; all_hex && k < len; k++) {
            if (!isxdigit((unsigned char)line[k])) all_hex = false;
        }
        if (all_hex) line[10] = '\0';
        snprintf(out, out_cap, "%s", line);
        free(raw);
        return;
    }
}

struct vdf_lexer {
    const char *src;
    size_t pos;
};

static bool vdf_next(struct vdf_lexer *lx, char *tok, size_t cap) {
    const char *s = lx->src;
    size_t i = lx->pos;
    while (s[i]) {
        unsigned char b = (unsigned char)s[i];
        if (isspace(b)) {
            i++;
            continue;
        }
        if (b == '/' && s[i + 1] == '/') {
            while (s[i] && s[i] != '\n') i++;
            continue;
        }
        if (b == '{' || b == '}') {
            tok[0] = (char)b;
            tok[1] = '\0';
            lx->pos = i + 1;
            return true;
        }
        if (b == '"') {
            i++;
            size_t out_i = 0;
            while (s[i]) {
                char c = s[i++];
                if (c == '\\' && s[i]) {
                    c = s[i++];
                } else if (c == '"') {
                    break;
                }
                if (out_i + 1 < cap) tok[out_i++] = c;
            }
            tok[out_i] = '\0';
            lx->pos = i;
            return true;
        }
        size_t start = i;
        while (s[i] && !isspace((unsigned char)s[i]) &&
               s[i] != '{' && s[i] != '}' && s[i] != '"') {
            i++;
        }
        size_t n = i - start;
        if (n >= cap) n = cap - 1;
        memcpy(tok, s + start, n);
        tok[n] = '\0';
        lx->pos = i;
        return true;
    }
    lx->pos = i;
    return false;
}

void parse_compatibilitytool_vdf(const char *content, const char *fallback, char *out, size_t out_cap) {
    struct vdf_lexer lx = {content, 0};
    char t0[256], t1[256], t2[256];
    if (!vdf_next(&lx, t0, sizeof(t0)) || !vdf_next(&lx, t1, sizeof(t1))) {
        snprintf(out, out_cap, "%s", fallback);
        return;
    }
    while (vdf_next(&lx, t2, sizeof(t2))) {
        if (strcmp(t0, "compat_tools") == 0 && strcmp(t1, "{") == 0) {
            snprintf(out, out_cap, "%s", t2);
            return;
        }
        memcpy(t0, t1, sizeof(t0));
        memcpy(t1, t2, sizeof(t1));
    }
    snprintf(out, out_cap, "%s", fallback);
}

static int cmp_runner(const void *a, const void *b) {
    const struct prm_runner *ra = a;
    const struct prm_runner *rb = b;
    return strcmp(ra->dir_name, rb->dir_name);
}

size_t scan_installed_runners(const char *compat_dir, struct prm_runner **out_runners) {
    *out_runners = NULL;
    DIR *d = opendir(compat_dir);
    if (!d) return 0;

    size_t cap = 16, count = 0;
    struct prm_runner *list = calloc(cap, sizeof(*list));
    if (!list) {
        closedir(d);
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[PATH_MAX_LEN];
        snprintf(full, sizeof(full), "%s/%s", compat_dir, ent->d_name);
        if (!path_is_dir(full)) continue;

        if (count == cap) {
            cap *= 2;
            struct prm_runner *next = realloc(list, cap * sizeof(*list));
            if (!next) break;
            list = next;
        }

        struct prm_runner *r = &list[count++];
        memset(r, 0, sizeof(*r));
        snprintf(r->dir_name, sizeof(r->dir_name), "%s", ent->d_name);
        snprintf(r->path, sizeof(r->path), "%s", full);

        char vdf_path[PATH_MAX_LEN];
        snprintf(vdf_path, sizeof(vdf_path), "%s/compatibilitytool.vdf", full);
        char *vdf = read_file_str(vdf_path, NULL);
        if (vdf) {
            parse_compatibilitytool_vdf(vdf, r->dir_name, r->internal_name, sizeof(r->internal_name));
            free(vdf);
        } else {
            snprintf(r->internal_name, sizeof(r->internal_name), "%s", r->dir_name);
        }

        char ver_path[PATH_MAX_LEN];
        snprintf(ver_path, sizeof(ver_path), "%s/version", full);
        char *ver_raw = read_file_str(ver_path, NULL);
        if (ver_raw) {
            char *p = ver_raw;
            while (*p && isspace((unsigned char)*p)) p++;
            while (*p && !isspace((unsigned char)*p)) p++;
            while (*p && isspace((unsigned char)*p)) p++;
            char *end = p;
            while (*end && !isspace((unsigned char)*end)) end++;
            *end = '\0';
            if (*p) snprintf(r->version_tag, sizeof(r->version_tag), "%s", p);
            free(ver_raw);
        }

        r->size_bytes = dir_size(full);
        read_dll_version(full, "dxvk", r->dxvk, sizeof(r->dxvk));
        read_dll_version(full, "vkd3d-proton", r->vkd3d, sizeof(r->vkd3d));
        read_dll_version(full, "nvapi", r->nvapi, sizeof(r->nvapi));
        read_dll_version(full, "d7vk", r->d7vk, sizeof(r->d7vk));
    }
    closedir(d);

    if (count > 1) {
        qsort(list, count, sizeof(*list), cmp_runner);
    }
    *out_runners = list;
    return count;
}

size_t parse_used_runners_str(const char *content, char (**out_names)[256]) {
    *out_names = NULL;
    if (!content) return 0;

    struct vdf_lexer lx = {content, 0};
    char tok[256];
    size_t cap = 16, count = 0;
    char (*names)[256] = calloc(cap, sizeof(*names));
    if (!names) return 0;

    while (vdf_next(&lx, tok, sizeof(tok))) {
        if (strcmp(tok, "CompatToolMapping") == 0) {
            if (!vdf_next(&lx, tok, sizeof(tok)) || strcmp(tok, "{") != 0) break;
            size_t depth = 1;
            while (depth > 0 && vdf_next(&lx, tok, sizeof(tok))) {
                if (strcmp(tok, "{") == 0) {
                    depth++;
                } else if (strcmp(tok, "}") == 0) {
                    depth--;
                } else if (depth == 2 && strcmp(tok, "name") == 0) {
                    if (!vdf_next(&lx, tok, sizeof(tok))) break;
                    if (!tok[0]) continue;
                    bool dup = false;
                    for (size_t i = 0; i < count; i++) {
                        if (strcmp(names[i], tok) == 0) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) {
                        if (count == cap) {
                            cap *= 2;
                            char (*next)[256] = realloc(names, cap * sizeof(*names));
                            if (!next) break;
                            names = next;
                        }
                        snprintf(names[count++], 256, "%s", tok);
                    }
                }
            }
            break;
        }
    }
    *out_names = names;
    return count;
}

size_t read_used_runners(const char *config_vdf, char (**out_names)[256]) {
    char *raw = read_file_str(config_vdf, NULL);
    if (!raw) {
        *out_names = NULL;
        return 0;
    }
    size_t n = parse_used_runners_str(raw, out_names);
    free(raw);
    return n;
}

void format_bytes(uint64_t bytes, char *out, size_t out_cap) {
    const double KB = 1024.0;
    const double MB = KB * 1024.0;
    const double GB = MB * 1024.0;
    double b = (double)bytes;
    if (b >= GB) {
        snprintf(out, out_cap, "%.2f GiB", b / GB);
    } else if (b >= MB) {
        snprintf(out, out_cap, "%.1f MiB", b / MB);
    } else if (b >= KB) {
        snprintf(out, out_cap, "%.1f KiB", b / KB);
    } else {
        snprintf(out, out_cap, "%llu B", (unsigned long long)bytes);
    }
}

void print_table(const char *const *headers, size_t cols, const char *const *cells, size_t row_count) {
    size_t widths[8] = {0};
    for (size_t c = 0; c < cols; c++) {
        widths[c] = strlen(headers[c]);
    }
    for (size_t r = 0; r < row_count; r++) {
        for (size_t c = 0; c < cols; c++) {
            size_t w = strlen(cells[r * cols + c]);
            if (w > widths[c]) widths[c] = w;
        }
    }

    for (size_t c = 0; c < cols; c++) {
        if (c + 1 < cols) {
            printf("%-*s  ", (int)widths[c], headers[c]);
        } else {
            puts(headers[c]);
        }
    }
    for (size_t c = 0; c < cols; c++) {
        for (size_t k = 0; k < widths[c]; k++) putchar('-');
        if (c + 1 < cols) printf("  ");
    }
    putchar('\n');

    for (size_t r = 0; r < row_count; r++) {
        for (size_t c = 0; c < cols; c++) {
            if (c + 1 < cols) {
                printf("%-*s  ", (int)widths[c], cells[r * cols + c]);
            } else {
                puts(cells[r * cols + c]);
            }
        }
    }
}
