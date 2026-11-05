#include "prm.h"
#include "yyjson.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *const RUNNERS_FORMAT_HINT =
    "{\n"
    "  \"runners\": {\n"
    "    \"<short_name>\": {\n"
    "      \"aliases\": [\"<optional_alias>\"],\n"
    "      \"installed_prefix\": \"<optional_folder_prefix>\",\n"
    "      \"releases_url\": \"<full_releases_api_url>\"\n"
    "    }\n"
    "  }\n"
    "}";

static void str_tolower(const char *src, char *dst, size_t cap) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static bool str_icontains(const char *haystack, const char *needle) {
    char h[512], n[256];
    str_tolower(haystack, h, sizeof(h));
    str_tolower(needle, n, sizeof(n));
    return strstr(h, n) != NULL;
}

static bool ends_with_ci(const char *str, const char *suffix) {
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    if (slen < xlen) return false;
    return strcasecmp(str + slen - xlen, suffix) == 0;
}

static int cmp_source_cfg(const void *a, const void *b) {
    const RunnerSourceConfig *sa = (const RunnerSourceConfig *)a;
    const RunnerSourceConfig *sb = (const RunnerSourceConfig *)b;
    return strcmp(sa->key, sb->key);
}

int load_runners_config(const char *config_dir, RunnersConfigFile *out, char *err, size_t err_cap) {
    memset(out, 0, sizeof(*out));
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s/runners.json", config_dir);

    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(err, err_cap,
                 "Missing runner configuration file at: %s\n"
                 "Create `%s` with the following JSON format:\n%s",
                 path, path, RUNNERS_FORMAT_HINT);
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_cap, "Failed to read %s", path);
        return -1;
    }
    char *raw = malloc((size_t)st.st_size + 1);
    if (!raw) {
        fclose(f);
        snprintf(err, err_cap, "Out of memory reading %s", path);
        return -1;
    }
    size_t rd = fread(raw, 1, (size_t)st.st_size, f);
    fclose(f);
    raw[rd] = '\0';

    yyjson_read_err jerr;
    yyjson_doc *doc = yyjson_read_opts(raw, rd, 0, NULL, &jerr);
    free(raw);
    if (!doc) {
        snprintf(err, err_cap, "Invalid JSON in %s: %s\nExpected format:\n%s",
                 path, jerr.msg ? jerr.msg : "parse error", RUNNERS_FORMAT_HINT);
        return -1;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *runners = yyjson_obj_get(root, "runners");
    if (!yyjson_is_obj(runners) || yyjson_obj_size(runners) == 0) {
        yyjson_doc_free(doc);
        snprintf(err, err_cap,
                 "Configuration file %s has an empty `runners` map.\nExpected format:\n%s",
                 path, RUNNERS_FORMAT_HINT);
        return -1;
    }

    size_t n = yyjson_obj_size(runners);
    out->items = calloc(n, sizeof(RunnerSourceConfig));
    if (!out->items) {
        yyjson_doc_free(doc);
        snprintf(err, err_cap, "Out of memory");
        return -1;
    }

    size_t idx = 0, max = 0;
    yyjson_val *key_val, *entry_val;
    yyjson_obj_foreach(runners, idx, max, key_val, entry_val) {
        const char *k = yyjson_get_str(key_val);
        const char *url = yyjson_get_str(yyjson_obj_get(entry_val, "releases_url"));
        if (!k || !url || !(strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0)) {
            snprintf(err, err_cap,
                     "Invalid `releases_url` for '%s' in %s: must be a full http(s) URL",
                     k ? k : "?", path);
            free_runners_config(out);
            yyjson_doc_free(doc);
            return -1;
        }

        RunnerSourceConfig *item = &out->items[out->count++];
        snprintf(item->key, sizeof(item->key), "%s", k);
        snprintf(item->releases_url, sizeof(item->releases_url), "%s", url);

        const char *pfx = yyjson_get_str(yyjson_obj_get(entry_val, "installed_prefix"));
        if (pfx) {
            snprintf(item->installed_prefix, sizeof(item->installed_prefix), "%s", pfx);
        }

        yyjson_val *aliases = yyjson_obj_get(entry_val, "aliases");
        if (yyjson_is_arr(aliases)) {
            size_t a_idx = 0, a_max = 0;
            yyjson_val *av;
            yyjson_arr_foreach(aliases, a_idx, a_max, av) {
                const char *astr = yyjson_get_str(av);
                if (astr && item->alias_count < 8) {
                    snprintf(item->aliases[item->alias_count++], sizeof(item->aliases[0]), "%s", astr);
                }
            }
        }
    }

    yyjson_doc_free(doc);
    qsort(out->items, out->count, sizeof(RunnerSourceConfig), cmp_source_cfg);
    return 0;
}

void free_runners_config(RunnersConfigFile *cfg) {
    free(cfg->items);
    cfg->items = NULL;
    cfg->count = 0;
}

int resolve_runner_endpoint(const char *config_dir, const char *source_query, EndpointConfig *out, char *err, size_t err_cap) {
    RunnersConfigFile cfg;
    if (load_runners_config(config_dir, &cfg, err, err_cap) != 0) {
        return -1;
    }

    for (size_t i = 0; i < cfg.count; i++) {
        const RunnerSourceConfig *it = &cfg.items[i];
        bool match = (strcasecmp(it->key, source_query) == 0);
        for (size_t a = 0; !match && a < it->alias_count; a++) {
            if (strcasecmp(it->aliases[a], source_query) == 0) match = true;
        }
        if (match) {
            snprintf(out->key, sizeof(out->key), "%s", it->key);
            snprintf(out->releases_url, sizeof(out->releases_url), "%s", it->releases_url);
            free_runners_config(&cfg);
            return 0;
        }
    }

    char avail[512] = {0};
    size_t pos = 0;
    for (size_t i = 0; i < cfg.count; i++) {
        int w = snprintf(avail + pos, sizeof(avail) - pos, "%s%s", i > 0 ? ", " : "", cfg.items[i].key);
        if (w > 0 && pos + (size_t)w < sizeof(avail)) pos += (size_t)w;
    }
    free_runners_config(&cfg);

    snprintf(err, err_cap, "Runner source '%s' not found in %s/runners.json. Configured sources: %s",
             source_query, config_dir, avail);
    return -1;
}

void parse_source_spec(const char *spec, char *source_out, size_t source_cap, char *ver_out, size_t ver_cap, bool *has_ver) {
    const char *at = strchr(spec, '@');
    if (at) {
        size_t slen = (size_t)(at - spec);
        if (slen >= source_cap) slen = source_cap - 1;
        memcpy(source_out, spec, slen);
        source_out[slen] = '\0';
        snprintf(ver_out, ver_cap, "%s", at + 1);
        *has_ver = true;
    } else {
        snprintf(source_out, source_cap, "%s", spec);
        ver_out[0] = '\0';
        *has_ver = false;
    }
}

static void parse_gh_release_val(yyjson_val *rval, ReleaseInfo *out) {
    memset(out, 0, sizeof(*out));
    const char *tag = yyjson_get_str(yyjson_obj_get(rval, "tag_name"));
    if (tag) snprintf(out->tag_name, sizeof(out->tag_name), "%s", tag);

    const char *pub = yyjson_get_str(yyjson_obj_get(rval, "published_at"));
    if (pub) {
        snprintf(out->published_at, sizeof(out->published_at), "%.10s", pub);
    }

    yyjson_val *assets = yyjson_obj_get(rval, "assets");
    if (yyjson_is_arr(assets) && yyjson_arr_size(assets) > 0) {
        size_t n = yyjson_arr_size(assets);
        out->assets = calloc(n, sizeof(ReleaseAsset));
        if (out->assets) {
            size_t idx = 0, max = 0;
            yyjson_val *av;
            yyjson_arr_foreach(assets, idx, max, av) {
                const char *aname = yyjson_get_str(yyjson_obj_get(av, "name"));
                const char *aurl = yyjson_get_str(yyjson_obj_get(av, "browser_download_url"));
                uint64_t asize = yyjson_get_uint(yyjson_obj_get(av, "size"));
                if (aname && aurl) {
                    ReleaseAsset *ra = &out->assets[out->asset_count++];
                    snprintf(ra->name, sizeof(ra->name), "%s", aname);
                    snprintf(ra->download_url, sizeof(ra->download_url), "%s", aurl);
                    ra->size = asize;
                }
            }
        }
    }
}

int fetch_releases(const EndpointConfig *endpoint, size_t limit, ReleaseInfo **out_releases, size_t *out_count, char *err, size_t err_cap) {
    *out_releases = NULL;
    *out_count = 0;

    size_t per_page = limit < 1 ? 1 : (limit > 50 ? 50 : limit);
    char url[600];
    snprintf(url, sizeof(url), "%s%sper_page=%zu",
             endpoint->releases_url,
             strchr(endpoint->releases_url, '?') ? "&" : "?",
             per_page);

    const char *token = getenv("GITHUB_TOKEN");
    char *body = NULL;
    size_t body_len = 0;
    if (https_get_Buffer(url, "application/vnd.github+json", token, &body, &body_len, err, err_cap) != 0) {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s", err);
        snprintf(err, err_cap, "API request failed for '%s' (%s): %s", endpoint->key, url, tmp);
        return -1;
    }

    yyjson_doc *doc = yyjson_read(body, body_len, 0);
    free(body);
    if (!doc) {
        snprintf(err, err_cap, "Failed to parse releases JSON for '%s'", endpoint->key);
        return -1;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root)) {
        yyjson_doc_free(doc);
        snprintf(err, err_cap, "Expected JSON array of releases for '%s'", endpoint->key);
        return -1;
    }

    size_t arr_len = yyjson_arr_size(root);
    size_t take = arr_len < limit ? arr_len : limit;
    if (take == 0) {
        yyjson_doc_free(doc);
        return 0;
    }

    ReleaseInfo *list = calloc(take, sizeof(ReleaseInfo));
    if (!list) {
        yyjson_doc_free(doc);
        snprintf(err, err_cap, "Out of memory");
        return -1;
    }

    size_t idx = 0, max = 0, count = 0;
    yyjson_val *rval;
    yyjson_arr_foreach(root, idx, max, rval) {
        if (count >= take) break;
        parse_gh_release_val(rval, &list[count++]);
    }
    yyjson_doc_free(doc);

    *out_releases = list;
    *out_count = count;
    return 0;
}

void free_release_info(ReleaseInfo *rel) {
    if (!rel) return;
    free(rel->assets);
    rel->assets = NULL;
    rel->asset_count = 0;
}

void free_releases(ReleaseInfo *releases, size_t count) {
    if (!releases) return;
    for (size_t i = 0; i < count; i++) {
        free_release_info(&releases[i]);
    }
    free(releases);
}

static bool try_fetch_single_release(const char *url, ReleaseInfo *out_rel) {
    const char *token = getenv("GITHUB_TOKEN");
    char *body = NULL;
    size_t body_len = 0;
    char err[256];
    if (https_get_Buffer(url, "application/vnd.github+json", token, &body, &body_len, err, sizeof(err)) != 0) {
        return false;
    }
    yyjson_doc *doc = yyjson_read(body, body_len, 0);
    free(body);
    if (!doc) return false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root) || !yyjson_get_str(yyjson_obj_get(root, "tag_name"))) {
        yyjson_doc_free(doc);
        return false;
    }
    parse_gh_release_val(root, out_rel);
    yyjson_doc_free(doc);
    return true;
}

int resolve_release(const EndpointConfig *endpoint, const char *version, ReleaseInfo *out_rel, char *err, size_t err_cap) {
    char base[512];
    snprintf(base, sizeof(base), "%s", endpoint->releases_url);
    size_t blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '/') base[--blen] = '\0';

    if (!version || !*version || strcmp(version, "latest") == 0) {
        char latest_url[600];
        snprintf(latest_url, sizeof(latest_url), "%s/latest", base);
        if (try_fetch_single_release(latest_url, out_rel)) {
            return 0;
        }
        ReleaseInfo *list = NULL;
        size_t count = 0;
        if (fetch_releases(endpoint, 10, &list, &count, err, err_cap) != 0) {
            return -1;
        }
        if (count == 0) {
            free_releases(list, count);
            snprintf(err, err_cap, "No releases found for '%s'", endpoint->key);
            return -1;
        }
        *out_rel = list[0];
        list[0].assets = NULL;
        free_releases(list, count);
        return 0;
    }

    char tag_url[600];
    snprintf(tag_url, sizeof(tag_url), "%s/tags/%s", base, version);
    if (try_fetch_single_release(tag_url, out_rel)) {
        return 0;
    }

    ReleaseInfo *list = NULL;
    size_t count = 0;
    if (fetch_releases(endpoint, 30, &list, &count, err, err_cap) != 0) {
        return -1;
    }

    const char *q_trim = (*version == 'v' || *version == 'V') ? version + 1 : version;
    for (size_t i = 0; i < count; i++) {
        const char *t = list[i].tag_name;
        const char *t_trim = (*t == 'v' || *t == 'V') ? t + 1 : t;
        if (strcasecmp(t, version) == 0 || strcasecmp(t_trim, q_trim) == 0 || str_icontains(t, version)) {
            *out_rel = list[i];
            list[i].assets = NULL;
            free_releases(list, count);
            return 0;
        }
    }

    free_releases(list, count);
    snprintf(err, err_cap, "Release '%s' not found for '%s'", version, endpoint->key);
    return -1;
}

void archive_stem(const char *name, char *out, size_t out_cap) {
    static const char *const exts[] = {".tar.gz", ".tgz", ".tar.xz", ".tar.zst"};
    snprintf(out, out_cap, "%s", name);
    size_t len = strlen(out);
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t elen = strlen(exts[i]);
        if (len >= elen && strcasecmp(out + len - elen, exts[i]) == 0) {
            out[len - elen] = '\0';
            return;
        }
    }
}

bool is_supported_archive(const char *name) {
    bool ext_ok = ends_with_ci(name, ".tar.gz") ||
                  ends_with_ci(name, ".tgz") ||
                  ends_with_ci(name, ".tar.xz") ||
                  ends_with_ci(name, ".tar.zst");
    if (!ext_ok) return false;
    if (str_icontains(name, "arm64") ||
        str_icontains(name, "aarch64") ||
        str_icontains(name, "x86_64_v") ||
        str_icontains(name, "x86-64-v")) {
        return false;
    }
    return true;
}

int select_release_assets(const ReleaseInfo *release, SelectedReleaseAssets *out, char *err, size_t err_cap) {
    memset(out, 0, sizeof(*out));
    const ReleaseAsset *chosen = NULL;
    for (size_t i = 0; i < release->asset_count; i++) {
        if (is_supported_archive(release->assets[i].name)) {
            chosen = &release->assets[i];
            break;
        }
    }
    if (!chosen) {
        snprintf(err, err_cap,
                 "Release '%s' has no supported x86_64 archive (.tar.gz, .tar.xz, .tar.zst)",
                 release->tag_name);
        return -1;
    }

    snprintf(out->tag_name, sizeof(out->tag_name), "%s", release->tag_name);
    out->archive = *chosen;

    char stem[256];
    archive_stem(chosen->name, stem, sizeof(stem));
    size_t stem_len = strlen(stem);
    size_t arch_len = strlen(chosen->name);

    for (size_t i = 0; i < release->asset_count; i++) {
        const char *n = release->assets[i].name;
        if ((ends_with_ci(n, ".sha512sum") || ends_with_ci(n, ".sha256sum")) &&
            (strncmp(n, stem, stem_len) == 0 || strncmp(n, chosen->name, arch_len) == 0)) {
            out->checksum = release->assets[i];
            out->has_checksum = true;
            break;
        }
    }
    return 0;
}
