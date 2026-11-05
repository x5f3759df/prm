#include "prm.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void resolve_config_dir(const char *home_path, char *out, size_t out_cap) {
    const char *env_dir = getenv("PRM_CONFIG_DIR");
    if (env_dir && *env_dir) {
        snprintf(out, out_cap, "%s", env_dir);
        return;
    }

    if (is_dir("config")) {
        snprintf(out, out_cap, "config");
        return;
    }

    char exe_buf[PATH_MAX_LEN];
    ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n > 0) {
        exe_buf[n] = '\0';
        char *slash = strrchr(exe_buf, '/');
        if (slash) {
            *slash = '\0';
            char cand[PATH_MAX_LEN];
            snprintf(cand, sizeof(cand), "%s/config", exe_buf);
            if (is_dir(cand)) {
                snprintf(out, out_cap, "%s", cand);
                return;
            }
            /* Check two levels up (e.g., build/release/prm) */
            for (int up = 0; up < 2; up++) {
                char *p = strrchr(exe_buf, '/');
                if (p) *p = '\0';
            }
            snprintf(cand, sizeof(cand), "%s/config", exe_buf);
            if (is_dir(cand)) {
                snprintf(out, out_cap, "%s", cand);
                return;
            }
        }
    }

    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(out, out_cap, "%s/prm", xdg);
    } else {
        snprintf(out, out_cap, "%s/.config/prm", home_path);
    }
}

int steam_paths_resolve(SteamPaths *out, char *err, size_t err_cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        snprintf(err, err_cap, "HOME environment variable not set");
        return -1;
    }

    char local_steam[PATH_MAX_LEN];
    snprintf(local_steam, sizeof(local_steam), "%s/.local/share/Steam", home);
    if (file_exists(local_steam)) {
        snprintf(out->steam_dir, sizeof(out->steam_dir), "%s", local_steam);
    } else {
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

void steam_config_vdf_path(const SteamPaths *paths, char *out, size_t out_cap) {
    snprintf(out, out_cap, "%s/config/config.vdf", paths->steam_dir);
}

uint64_t dir_size(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode)) {
        return 0;
    }
    if (S_ISREG(st.st_mode)) {
        return (uint64_t)st.st_size;
    }
    if (!S_ISDIR(st.st_mode)) {
        return 0;
    }

    DIR *d = opendir(path);
    if (!d) return 0;

    uint64_t total = 0;
    struct dirent *ent;
    char child[PATH_MAX_LEN];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        total += dir_size(child);
    }
    closedir(d);
    return total;
}

static char *read_entire_file(const char *path, size_t *out_len) {
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
        char *raw = read_entire_file(cand, NULL);
        if (!raw) continue;

        char *nl = strpbrk(raw, "\r\n");
        if (nl) *nl = '\0';

        char *line = raw;
        while (*line && isspace((unsigned char)*line)) line++;
        size_t len = strlen(line);
        while (len > 0 && isspace((unsigned char)line[len - 1])) {
            line[--len] = '\0';
        }
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
        if (all_hex) {
            line[10] = '\0';
        }
        snprintf(out, out_cap, "%s", line);
        free(raw);
        return;
    }
}

/* Streaming VDF tokenizer */
typedef struct {
    const char *src;
    size_t len;
    size_t pos;
} VdfScanner;

static bool vdf_next_token(VdfScanner *s, char *tok, size_t tok_cap) {
    while (s->pos < s->len) {
        unsigned char b = (unsigned char)s->src[s->pos];
        if (isspace(b)) {
            s->pos++;
            continue;
        }
        if (b == '/' && s->pos + 1 < s->len && s->src[s->pos + 1] == '/') {
            while (s->pos < s->len && s->src[s->pos] != '\n') s->pos++;
            continue;
        }
        if (b == '{' || b == '}') {
            tok[0] = (char)b;
            tok[1] = '\0';
            s->pos++;
            return true;
        }
        if (b == '"') {
            s->pos++;
            size_t out_i = 0;
            while (s->pos < s->len) {
                char c = s->src[s->pos++];
                if (c == '\\' && s->pos < s->len) {
                    c = s->src[s->pos++];
                } else if (c == '"') {
                    break;
                }
                if (out_i + 1 < tok_cap) tok[out_i++] = c;
            }
            tok[out_i] = '\0';
            return true;
        }
        size_t start = s->pos;
        while (s->pos < s->len && !isspace((unsigned char)s->src[s->pos]) &&
               s->src[s->pos] != '{' && s->src[s->pos] != '}' && s->src[s->pos] != '"') {
            s->pos++;
        }
        size_t n = s->pos - start;
        if (n >= tok_cap) n = tok_cap - 1;
        memcpy(tok, s->src + start, n);
        tok[n] = '\0';
        return true;
    }
    return false;
}

void parse_compatibilitytool_vdf(const char *content, const char *fallback, char *out, size_t out_cap) {
    VdfScanner s = {content, strlen(content), 0};
    char t0[256], t1[256], t2[256];
    if (!vdf_next_token(&s, t0, sizeof(t0)) || !vdf_next_token(&s, t1, sizeof(t1))) {
        snprintf(out, out_cap, "%s", fallback);
        return;
    }
    while (vdf_next_token(&s, t2, sizeof(t2))) {
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
    const InstalledRunner *ra = (const InstalledRunner *)a;
    const InstalledRunner *rb = (const InstalledRunner *)b;
    return strcmp(ra->dir_name, rb->dir_name);
}

size_t scan_installed_runners(const char *compat_dir, InstalledRunner **out_runners) {
    *out_runners = NULL;
    DIR *d = opendir(compat_dir);
    if (!d) return 0;

    size_t cap = 16, count = 0;
    InstalledRunner *list = calloc(cap, sizeof(InstalledRunner));
    if (!list) {
        closedir(d);
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[PATH_MAX_LEN];
        snprintf(full, sizeof(full), "%s/%s", compat_dir, ent->d_name);
        if (!is_dir(full)) continue;

        if (count == cap) {
            cap *= 2;
            InstalledRunner *next = realloc(list, cap * sizeof(InstalledRunner));
            if (!next) break;
            list = next;
        }

        InstalledRunner *r = &list[count++];
        memset(r, 0, sizeof(*r));
        snprintf(r->dir_name, sizeof(r->dir_name), "%s", ent->d_name);
        snprintf(r->path, sizeof(r->path), "%s", full);

        char vdf_path[PATH_MAX_LEN];
        snprintf(vdf_path, sizeof(vdf_path), "%s/compatibilitytool.vdf", full);
        char *vdf = read_entire_file(vdf_path, NULL);
        if (vdf) {
            parse_compatibilitytool_vdf(vdf, r->dir_name, r->internal_name, sizeof(r->internal_name));
            free(vdf);
        } else {
            snprintf(r->internal_name, sizeof(r->internal_name), "%s", r->dir_name);
        }

        char ver_path[PATH_MAX_LEN];
        snprintf(ver_path, sizeof(ver_path), "%s/version", full);
        char *ver_raw = read_entire_file(ver_path, NULL);
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
        read_dll_version(full, "dxvk", r->dxvk_version, sizeof(r->dxvk_version));
        read_dll_version(full, "vkd3d-proton", r->vkd3d_version, sizeof(r->vkd3d_version));
        read_dll_version(full, "nvapi", r->nvapi_version, sizeof(r->nvapi_version));
        read_dll_version(full, "d7vk", r->d7vk_version, sizeof(r->d7vk_version));
    }
    closedir(d);

    if (count > 1) {
        qsort(list, count, sizeof(InstalledRunner), cmp_runner);
    }
    *out_runners = list;
    return count;
}

size_t parse_used_runners_str(const char *content, char (**out_names)[256]) {
    *out_names = NULL;
    if (!content) return 0;

    VdfScanner s = {content, strlen(content), 0};
    char tok[256];
    size_t cap = 16, count = 0;
    char (*names)[256] = calloc(cap, sizeof(*names));
    if (!names) return 0;

    while (vdf_next_token(&s, tok, sizeof(tok))) {
        if (strcmp(tok, "CompatToolMapping") == 0) {
            if (!vdf_next_token(&s, tok, sizeof(tok)) || strcmp(tok, "{") != 0) break;
            size_t depth = 1;
            while (depth > 0 && vdf_next_token(&s, tok, sizeof(tok))) {
                if (strcmp(tok, "{") == 0) {
                    depth++;
                } else if (strcmp(tok, "}") == 0) {
                    depth--;
                } else if (depth == 2 && strcmp(tok, "name") == 0) {
                    if (!vdf_next_token(&s, tok, sizeof(tok))) break;
                    if (tok[0] != '\0') {
                        bool exists = false;
                        for (size_t i = 0; i < count; i++) {
                            if (strcmp(names[i], tok) == 0) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
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
            }
            break;
        }
    }
    *out_names = names;
    return count;
}

size_t read_used_runners(const char *config_vdf, char (**out_names)[256]) {
    char *raw = read_entire_file(config_vdf, NULL);
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

void print_table(const char **headers, size_t cols, const char *const *rows, size_t row_count) {
    size_t widths[16] = {0};
    for (size_t c = 0; c < cols; c++) {
        widths[c] = strlen(headers[c]);
    }
    for (size_t r = 0; r < row_count; r++) {
        for (size_t c = 0; c < cols; c++) {
            const char *cell = rows[r * cols + c];
            size_t w = strlen(cell);
            if (w > widths[c]) widths[c] = w;
        }
    }

    for (size_t c = 0; c < cols; c++) {
        if (c + 1 < cols) {
            printf("%-*s  ", (int)widths[c], headers[c]);
        } else {
            printf("%s\n", headers[c]);
        }
    }
    for (size_t c = 0; c < cols; c++) {
        for (size_t k = 0; k < widths[c]; k++) putchar('-');
        if (c + 1 < cols) printf("  ");
    }
    putchar('\n');

    for (size_t r = 0; r < row_count; r++) {
        for (size_t c = 0; c < cols; c++) {
            const char *cell = rows[r * cols + c];
            if (c + 1 < cols) {
                printf("%-*s  ", (int)widths[c], cell);
            } else {
                printf("%s\n", cell);
            }
        }
    }
}
