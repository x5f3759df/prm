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

#define PRM_VERSION "0.3.0"
#define PATH_MAX_LEN 4096

typedef struct {
    char steam_dir[PATH_MAX_LEN];
    char compat_dir[PATH_MAX_LEN];
    char cache_dir[PATH_MAX_LEN];
    char config_dir[PATH_MAX_LEN];
} SteamPaths;

typedef struct {
    char dir_name[256];
    char internal_name[256];
    char version_tag[256];
    char path[PATH_MAX_LEN];
    uint64_t size_bytes;
    char dxvk_version[64];
    char vkd3d_version[64];
    char nvapi_version[64];
    char d7vk_version[64];
} InstalledRunner;

typedef struct {
    char key[64];
    char aliases[8][64];
    size_t alias_count;
    char installed_prefix[64];
    char releases_url[512];
} RunnerSourceConfig;

typedef struct {
    RunnerSourceConfig *items;
    size_t count;
} RunnersConfigFile;

typedef struct {
    char key[64];
    char releases_url[512];
} EndpointConfig;

typedef struct {
    char name[256];
    char download_url[1024];
    uint64_t size;
} ReleaseAsset;

typedef struct {
    char tag_name[128];
    char published_at[32];
    ReleaseAsset *assets;
    size_t asset_count;
} ReleaseInfo;

typedef struct {
    char tag_name[128];
    ReleaseAsset archive;
    ReleaseAsset checksum;
    bool has_checksum;
} SelectedReleaseAssets;

/* steam.c */
int steam_paths_resolve(SteamPaths *out, char *err, size_t err_cap);
void steam_config_vdf_path(const SteamPaths *paths, char *out, size_t out_cap);
uint64_t dir_size(const char *path);
void parse_compatibilitytool_vdf(const char *content, const char *fallback, char *out, size_t out_cap);
size_t scan_installed_runners(const char *compat_dir, InstalledRunner **out_runners);
size_t parse_used_runners_str(const char *content, char (**out_names)[256]);
size_t read_used_runners(const char *config_vdf, char (**out_names)[256]);
void format_bytes(uint64_t bytes, char *out, size_t out_cap);
void print_table(const char **headers, size_t cols, const char *const *rows, size_t row_count);

/* sources.c */
int load_runners_config(const char *config_dir, RunnersConfigFile *out, char *err, size_t err_cap);
void free_runners_config(RunnersConfigFile *cfg);
int resolve_runner_endpoint(const char *config_dir, const char *source_query, EndpointConfig *out, char *err, size_t err_cap);
void parse_source_spec(const char *spec, char *source_out, size_t source_cap, char *ver_out, size_t ver_cap, bool *has_ver);
int fetch_releases(const EndpointConfig *endpoint, size_t limit, ReleaseInfo **out_releases, size_t *out_count, char *err, size_t err_cap);
void free_releases(ReleaseInfo *releases, size_t count);
int resolve_release(const EndpointConfig *endpoint, const char *version, ReleaseInfo *out_rel, char *err, size_t err_cap);
void free_release_info(ReleaseInfo *rel);
void archive_stem(const char *name, char *out, size_t out_cap);
bool is_supported_archive(const char *name);
int select_release_assets(const ReleaseInfo *release, SelectedReleaseAssets *out, char *err, size_t err_cap);

/* archive.c */
int https_get_Buffer(const char *url, const char *accept_hdr, const char *bearer_token, char **out_buf, size_t *out_len, char *err, size_t err_cap);
int download_file(const char *url, const char *dest, uint64_t expected_size, char *err, size_t err_cap);
int fetch_expected_checksum(const ReleaseAsset *checksum_asset, char *out_hex, size_t out_cap, char *err, size_t err_cap);
int compute_file_checksum(const char *archive_path, bool use_sha256, char *out_hex, size_t out_cap, char *err, size_t err_cap);
int verify_checksum(const char *archive_path, const ReleaseAsset *checksum_asset, char *out_hex, size_t out_cap, char *err, size_t err_cap);
int extract_tar_archive(const char *archive_path, const char *target_parent_dir, char *out_extracted_dir, size_t out_cap, char *err, size_t err_cap);
int remove_dir_force(const char *path, char *err, size_t err_cap);
int mkdir_p(const char *path, char *err, size_t err_cap);

/* runner.c */
int cmd_list(const SteamPaths *paths, const char *remote, size_t limit, char *err, size_t err_cap);
int cmd_install(const SteamPaths *paths, const char *spec, bool force, char *out_installed_name, size_t out_cap, char *err, size_t err_cap);
int cmd_update(const SteamPaths *paths, const char *source_filter, char *err, size_t err_cap);
int cmd_remove(const SteamPaths *paths, const char *const *targets, size_t target_count, char *err, size_t err_cap);
int cmd_purge(const SteamPaths *paths, char *err, size_t err_cap);

#endif /* PRM_H */
