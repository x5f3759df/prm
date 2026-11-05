#ifndef PRM_H
#define PRM_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRM_VERSION "0.4.0"
#define PATH_MAX_LEN 4096

#if defined(__GNUC__) || defined(__clang__)
#define PRM_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define PRM_PRINTF(fmt_idx, arg_idx)
#endif

struct prm_paths {
    char steam_dir[PATH_MAX_LEN];
    char compat_dir[PATH_MAX_LEN];
    char cache_dir[PATH_MAX_LEN];
    char config_dir[PATH_MAX_LEN];
};

struct prm_runner {
    char dir_name[256];
    char internal_name[256];
    char version_tag[256];
    char path[PATH_MAX_LEN];
    uint64_t size_bytes;
    char dxvk[64];
    char vkd3d[64];
    char nvapi[64];
    char d7vk[64];
};

struct prm_source {
    char key[64];
    char aliases[8][64];
    size_t alias_count;
    char installed_prefix[64];
    char releases_url[512];
};

struct prm_asset {
    char name[256];
    char download_url[1024];
    uint64_t size;
};

struct prm_release {
    char tag_name[128];
    char published_at[32];
    struct prm_asset *assets;
    size_t asset_count;
};

struct prm_selected_assets {
    char tag_name[128];
    struct prm_asset archive;
    struct prm_asset checksum;
    bool has_checksum;
};

/* Error handling & common fs utilities (steam.c) */
int prm_err(const char *fmt, ...) PRM_PRINTF(1, 2);
const char *prm_err_str(void);
bool path_exists(const char *path);
bool path_is_dir(const char *path);
char *read_file_str(const char *path, size_t *out_len);

/* steam.c */
int prm_paths_resolve(struct prm_paths *out);
uint64_t dir_size(const char *path);
void parse_compatibilitytool_vdf(const char *content, const char *fallback, char *out, size_t out_cap);
size_t scan_installed_runners(const char *compat_dir, struct prm_runner **out_runners);
size_t parse_used_runners_str(const char *content, char (**out_names)[256]);
size_t read_used_runners(const char *config_vdf, char (**out_names)[256]);
void format_bytes(uint64_t bytes, char *out, size_t out_cap);
void print_table(const char *const *headers, size_t cols, const char *const *cells, size_t row_count);

/* sources.c */
int load_runners_config(const char *config_dir, struct prm_source **out_sources, size_t *out_count);
int resolve_runner_endpoint(const char *config_dir, const char *query, struct prm_source *out);
int fetch_releases(const struct prm_source *src, size_t limit, struct prm_release **out_rels, size_t *out_count);
void free_releases(struct prm_release *rels, size_t count);
int resolve_release(const struct prm_source *src, const char *version, struct prm_release *out_rel);
void archive_stem(const char *name, char *out, size_t out_cap);
bool is_supported_archive(const char *name);
int select_release_assets(const struct prm_release *rel, struct prm_selected_assets *out);

/* archive.c */
int https_get(const char *url, const char *accept_hdr, const char *bearer_token, char **out_buf, size_t *out_len);
int download_file(const char *url, const char *dest, uint64_t expected_size);
int fetch_expected_checksum(const struct prm_asset *cs_asset, char *out_hex, size_t out_cap);
int compute_file_checksum(const char *archive_path, bool use_sha256, char *out_hex, size_t out_cap);
int verify_checksum(const char *archive_path, const struct prm_asset *cs_asset, char *out_hex, size_t out_cap);
int extract_tar_archive(const char *archive_path, const char *target_parent_dir, char *out_extracted_dir, size_t out_cap);
int remove_dir_force(const char *path);
int mkdir_p(const char *path);

/* runner.c */
int cmd_list(const struct prm_paths *paths, const char *remote, size_t limit);
int cmd_install(const struct prm_paths *paths, const char *spec, bool force);
int cmd_update(const struct prm_paths *paths, const char *source_filter);
int cmd_remove(const struct prm_paths *paths, char *const *targets, size_t target_count);
int cmd_purge(const struct prm_paths *paths);

#endif /* PRM_H */
