#include "prm.h"
#include "yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char RUNNERS_FORMAT_HINT[] =
    "{\n"
    "  \"runners\": {\n"
    "    \"<short_name>\": {\n"
    "      \"aliases\": [\"<optional_alias>\"],\n"
    "      \"installed_prefix\": \"<optional_folder_prefix>\",\n"
    "      \"releases_url\": \"<full_releases_api_url>\"\n"
    "    }\n"
    "  }\n"
    "}";

static bool ends_with_ci(const char *str, const char *suffix) {
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    return slen >= xlen && strcasecmp(str + slen - xlen, suffix) == 0;
}

static int cmp_source(const void *a, const void *b) {
    const struct prm_source *sa = a;
    const struct prm_source *sb = b;
    return strcmp(sa->key, sb->key);
}

int load_runners_config(const char *config_dir, struct prm_source **out_sources, size_t *out_count) {
    *out_sources = NULL;
    *out_count = 0;

    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s/runners.json", config_dir);
    if (!path_exists(path)) {
        return prm_err("Missing runner configuration file at: %s\n"
                       "Create `%s` with the following JSON format:\n%s",
                       path, path, RUNNERS_FORMAT_HINT);
    }

    size_t raw_len = 0;
    char *raw = read_file_str(path, &raw_len);
    if (!raw) {
        return prm_err("Failed to read %s", path);
    }

    yyjson_read_err jerr;
    yyjson_doc *doc = yyjson_read_opts(raw, raw_len, 0, NULL, &jerr);
    free(raw);
    if (!doc) {
        return prm_err("Invalid JSON in %s: %s\nExpected format:\n%s",
                       path, jerr.msg ? jerr.msg : "parse error", RUNNERS_FORMAT_HINT);
    }

    yyjson_val *runners = yyjson_obj_get(yyjson_doc_get_root(doc), "runners");
    size_t n = yyjson_is_obj(runners) ? yyjson_obj_size(runners) : 0;
    if (n == 0) {
        yyjson_doc_free(doc);
        return prm_err("Configuration file %s has an empty `runners` map.\nExpected format:\n%s",
                       path, RUNNERS_FORMAT_HINT);
    }

    struct prm_source *items = calloc(n, sizeof(*items));
    if (!items) {
        yyjson_doc_free(doc);
        return prm_err("Out of memory");
    }

    size_t idx = 0, max = 0, count = 0;
    yyjson_val *key_val, *entry_val;
    yyjson_obj_foreach(runners, idx, max, key_val, entry_val) {
        const char *k = yyjson_get_str(key_val);
        const char *url = yyjson_get_str(yyjson_obj_get(entry_val, "releases_url"));
        if (!k || !url || (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0)) {
            free(items);
            yyjson_doc_free(doc);
            return prm_err("Invalid `releases_url` for '%s' in %s: must be a full http(s) URL",
                           k ? k : "?", path);
        }

        struct prm_source *it = &items[count++];
        snprintf(it->key, sizeof(it->key), "%s", k);
        snprintf(it->releases_url, sizeof(it->releases_url), "%s", url);

        const char *pfx = yyjson_get_str(yyjson_obj_get(entry_val, "installed_prefix"));
        if (pfx) snprintf(it->installed_prefix, sizeof(it->installed_prefix), "%s", pfx);

        yyjson_val *aliases = yyjson_obj_get(entry_val, "aliases");
        if (yyjson_is_arr(aliases)) {
            size_t a_idx = 0, a_max = 0;
            yyjson_val *av;
            yyjson_arr_foreach(aliases, a_idx, a_max, av) {
                const char *astr = yyjson_get_str(av);
                if (astr && it->alias_count < 8) {
                    snprintf(it->aliases[it->alias_count++], sizeof(it->aliases[0]), "%s", astr);
                }
            }
        }
    }

    yyjson_doc_free(doc);
    qsort(items, count, sizeof(*items), cmp_source);
    *out_sources = items;
    *out_count = count;
    return 0;
}

int resolve_runner_endpoint(const char *config_dir, const char *query, struct prm_source *out) {
    struct prm_source *items = NULL;
    size_t count = 0;
    if (load_runners_config(config_dir, &items, &count) != 0) {
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        bool match = (strcasecmp(items[i].key, query) == 0);
        for (size_t a = 0; !match && a < items[i].alias_count; a++) {
            if (strcasecmp(items[i].aliases[a], query) == 0) match = true;
        }
        if (match) {
            *out = items[i];
            free(items);
            return 0;
        }
    }

    char avail[512] = {0};
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        int w = snprintf(avail + pos, sizeof(avail) - pos, "%s%s", i > 0 ? ", " : "", items[i].key);
        if (w > 0 && pos + (size_t)w < sizeof(avail)) pos += (size_t)w;
    }
    free(items);
    return prm_err("Runner source '%s' not found in %s/runners.json. Configured sources: %s",
                   query, config_dir, avail);
}

static void parse_gh_release(yyjson_val *rval, struct prm_release *out) {
    memset(out, 0, sizeof(*out));
    const char *tag = yyjson_get_str(yyjson_obj_get(rval, "tag_name"));
    if (tag) snprintf(out->tag_name, sizeof(out->tag_name), "%s", tag);

    const char *pub = yyjson_get_str(yyjson_obj_get(rval, "published_at"));
    if (pub) snprintf(out->published_at, sizeof(out->published_at), "%.10s", pub);

    yyjson_val *assets = yyjson_obj_get(rval, "assets");
    size_t n = yyjson_is_arr(assets) ? yyjson_arr_size(assets) : 0;
    if (n > 0 && (out->assets = calloc(n, sizeof(*out->assets))) != NULL) {
        size_t idx = 0, max = 0;
        yyjson_val *av;
        yyjson_arr_foreach(assets, idx, max, av) {
            const char *aname = yyjson_get_str(yyjson_obj_get(av, "name"));
            const char *aurl = yyjson_get_str(yyjson_obj_get(av, "browser_download_url"));
            if (aname && aurl) {
                struct prm_asset *ra = &out->assets[out->asset_count++];
                snprintf(ra->name, sizeof(ra->name), "%s", aname);
                snprintf(ra->download_url, sizeof(ra->download_url), "%s", aurl);
                ra->size = yyjson_get_uint(yyjson_obj_get(av, "size"));
            }
        }
    }
}

int fetch_releases(const struct prm_source *src, size_t limit, struct prm_release **out_rels, size_t *out_count) {
    *out_rels = NULL;
    *out_count = 0;

    size_t per_page = limit < 1 ? 1 : (limit > 50 ? 50 : limit);
    char url[600];
    snprintf(url, sizeof(url), "%s%sper_page=%zu",
             src->releases_url,
             strchr(src->releases_url, '?') ? "&" : "?",
             per_page);

    char *body = NULL;
    size_t body_len = 0;
    if (https_get(url, "application/vnd.github+json", getenv("GITHUB_TOKEN"), &body, &body_len) != 0) {
        return prm_err("API request failed for '%s' (%s): %s", src->key, url, prm_err_str());
    }

    yyjson_doc *doc = yyjson_read(body, body_len, 0);
    free(body);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_arr(root)) {
        yyjson_doc_free(doc);
        return prm_err("Failed to parse releases JSON array for '%s'", src->key);
    }

    size_t arr_len = yyjson_arr_size(root);
    size_t take = arr_len < limit ? arr_len : limit;
    if (take == 0) {
        yyjson_doc_free(doc);
        return 0;
    }

    struct prm_release *list = calloc(take, sizeof(*list));
    if (!list) {
        yyjson_doc_free(doc);
        return prm_err("Out of memory");
    }

    size_t idx = 0, max = 0, count = 0;
    yyjson_val *rval;
    yyjson_arr_foreach(root, idx, max, rval) {
        if (count >= take) break;
        parse_gh_release(rval, &list[count++]);
    }
    yyjson_doc_free(doc);

    *out_rels = list;
    *out_count = count;
    return 0;
}

void free_releases(struct prm_release *rels, size_t count) {
    if (!rels) return;
    for (size_t i = 0; i < count; i++) {
        free(rels[i].assets);
    }
    free(rels);
}

static bool try_fetch_single_release(const char *url, struct prm_release *out_rel) {
    char *body = NULL;
    size_t body_len = 0;
    if (https_get(url, "application/vnd.github+json", getenv("GITHUB_TOKEN"), &body, &body_len) != 0) {
        return false;
    }
    yyjson_doc *doc = yyjson_read(body, body_len, 0);
    free(body);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_obj(root) || !yyjson_get_str(yyjson_obj_get(root, "tag_name"))) {
        yyjson_doc_free(doc);
        return false;
    }
    parse_gh_release(root, out_rel);
    yyjson_doc_free(doc);
    return true;
}

int resolve_release(const struct prm_source *src, const char *version, struct prm_release *out_rel) {
    char base[512];
    snprintf(base, sizeof(base), "%s", src->releases_url);
    size_t blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '/') base[--blen] = '\0';

    if (!version || !*version || strcmp(version, "latest") == 0) {
        char latest_url[600];
        snprintf(latest_url, sizeof(latest_url), "%s/latest", base);
        if (try_fetch_single_release(latest_url, out_rel)) return 0;

        struct prm_release *list = NULL;
        size_t count = 0;
        if (fetch_releases(src, 10, &list, &count) != 0) return -1;
        if (count == 0) {
            free_releases(list, count);
            return prm_err("No releases found for '%s'", src->key);
        }
        *out_rel = list[0];
        list[0].assets = NULL;
        free_releases(list, count);
        return 0;
    }

    char tag_url[600];
    snprintf(tag_url, sizeof(tag_url), "%s/tags/%s", base, version);
    if (try_fetch_single_release(tag_url, out_rel)) return 0;

    struct prm_release *list = NULL;
    size_t count = 0;
    if (fetch_releases(src, 30, &list, &count) != 0) return -1;

    const char *q_trim = (*version == 'v' || *version == 'V') ? version + 1 : version;
    for (size_t i = 0; i < count; i++) {
        const char *t = list[i].tag_name;
        const char *t_trim = (*t == 'v' || *t == 'V') ? t + 1 : t;
        if (strcasecmp(t, version) == 0 || strcasecmp(t_trim, q_trim) == 0 || strcasestr(t, version) != NULL) {
            *out_rel = list[i];
            list[i].assets = NULL;
            free_releases(list, count);
            return 0;
        }
    }

    free_releases(list, count);
    return prm_err("Release '%s' not found for '%s'", version, src->key);
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
    return !strcasestr(name, "arm64") &&
           !strcasestr(name, "aarch64") &&
           !strcasestr(name, "x86_64_v") &&
           !strcasestr(name, "x86-64-v");
}

int select_release_assets(const struct prm_release *rel, struct prm_selected_assets *out) {
    memset(out, 0, sizeof(*out));
    const struct prm_asset *chosen = NULL;
    for (size_t i = 0; i < rel->asset_count; i++) {
        if (is_supported_archive(rel->assets[i].name)) {
            chosen = &rel->assets[i];
            break;
        }
    }
    if (!chosen) {
        return prm_err("Release '%s' has no supported x86_64 archive (.tar.gz, .tar.xz, .tar.zst)",
                       rel->tag_name);
    }

    snprintf(out->tag_name, sizeof(out->tag_name), "%s", rel->tag_name);
    out->archive = *chosen;

    char stem[256];
    archive_stem(chosen->name, stem, sizeof(stem));
    size_t stem_len = strlen(stem);
    size_t arch_len = strlen(chosen->name);

    for (size_t i = 0; i < rel->asset_count; i++) {
        const char *n = rel->assets[i].name;
        if ((ends_with_ci(n, ".sha512sum") || ends_with_ci(n, ".sha256sum")) &&
            (strncmp(n, stem, stem_len) == 0 || strncmp(n, chosen->name, arch_len) == 0)) {
            out->checksum = rel->assets[i];
            out->has_checksum = true;
            break;
        }
    }
    return 0;
}
