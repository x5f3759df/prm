#include "prm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int cmd_list(const SteamPaths *paths, const char *remote, size_t limit, char *err, size_t err_cap) {
    if (remote != NULL) {
        while (*remote && isspace((unsigned char)*remote)) remote++;
        if (*remote == '\0') {
            RunnersConfigFile cfg;
            if (load_runners_config(paths->config_dir, &cfg, err, err_cap) != 0) {
                return -1;
            }
            const char *headers[] = {"SOURCE", "ALIASES", "RELEASES URL"};
            const char **rows = calloc(cfg.count * 3, sizeof(char *));
            char (*alias_bufs)[256] = calloc(cfg.count, sizeof(*alias_bufs));
            if (!rows || !alias_bufs) {
                free(rows);
                free(alias_bufs);
                free_runners_config(&cfg);
                snprintf(err, err_cap, "Out of memory");
                return -1;
            }
            for (size_t i = 0; i < cfg.count; i++) {
                if (cfg.items[i].alias_count == 0) {
                    snprintf(alias_bufs[i], 256, "-");
                } else {
                    size_t pos = 0;
                    for (size_t a = 0; a < cfg.items[i].alias_count; a++) {
                        int w = snprintf(alias_bufs[i] + pos, 256 - pos, "%s%s",
                                         a > 0 ? ", " : "", cfg.items[i].aliases[a]);
                        if (w > 0 && pos + (size_t)w < 256) pos += (size_t)w;
                    }
                }
                rows[i * 3 + 0] = cfg.items[i].key;
                rows[i * 3 + 1] = alias_bufs[i];
                rows[i * 3 + 2] = cfg.items[i].releases_url;
            }
            print_table(headers, 3, rows, cfg.count);
            free(rows);
            free(alias_bufs);
            free_runners_config(&cfg);
            return 0;
        }

        EndpointConfig ep;
        if (resolve_runner_endpoint(paths->config_dir, remote, &ep, err, err_cap) != 0) {
            return -1;
        }
        ReleaseInfo *releases = NULL;
        size_t count = 0;
        if (fetch_releases(&ep, limit, &releases, &count, err, err_cap) != 0) {
            return -1;
        }

        const char *headers[] = {"TAG", "PUBLISHED", "SIZE", "ASSET"};
        const char **rows = calloc(count * 4, sizeof(char *));
        char (*size_bufs)[64] = calloc(count, sizeof(*size_bufs));
        char (*asset_bufs)[256] = calloc(count, sizeof(*asset_bufs));
        if (!rows || !size_bufs || !asset_bufs) {
            free(rows);
            free(size_bufs);
            free(asset_bufs);
            free_releases(releases, count);
            snprintf(err, err_cap, "Out of memory");
            return -1;
        }

        for (size_t i = 0; i < count; i++) {
            SelectedReleaseAssets sel;
            char tmp_err[128];
            if (select_release_assets(&releases[i], &sel, tmp_err, sizeof(tmp_err)) == 0) {
                format_bytes(sel.archive.size, size_bufs[i], 64);
                snprintf(asset_bufs[i], 256, "%s", sel.archive.name);
            } else {
                snprintf(size_bufs[i], 64, "-");
                snprintf(asset_bufs[i], 256, "(no x86_64 archive)");
            }
            rows[i * 4 + 0] = releases[i].tag_name;
            rows[i * 4 + 1] = releases[i].published_at;
            rows[i * 4 + 2] = size_bufs[i];
            rows[i * 4 + 3] = asset_bufs[i];
        }
        print_table(headers, 4, rows, count);
        free(rows);
        free(size_bufs);
        free(asset_bufs);
        free_releases(releases, count);
        return 0;
    }

    InstalledRunner *runners = NULL;
    size_t count = scan_installed_runners(paths->compat_dir, &runners);
    if (count == 0) {
        printf("No custom Proton runners installed in %s\n", paths->compat_dir);
        free(runners);
        return 0;
    }

    uint64_t total_bytes = 0;
    const char *headers[] = {"RUNNER", "SIZE", "DXVK", "VKD3D-PROTON", "NVAPI", "D7VK"};
    const char **rows = calloc(count * 6, sizeof(char *));
    char (*size_bufs)[64] = calloc(count, sizeof(*size_bufs));
    if (!rows || !size_bufs) {
        free(rows);
        free(size_bufs);
        free(runners);
        snprintf(err, err_cap, "Out of memory");
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        total_bytes += runners[i].size_bytes;
        format_bytes(runners[i].size_bytes, size_bufs[i], 64);
        rows[i * 6 + 0] = runners[i].dir_name;
        rows[i * 6 + 1] = size_bufs[i];
        rows[i * 6 + 2] = runners[i].dxvk_version[0] ? runners[i].dxvk_version : "-";
        rows[i * 6 + 3] = runners[i].vkd3d_version[0] ? runners[i].vkd3d_version : "-";
        rows[i * 6 + 4] = runners[i].nvapi_version[0] ? runners[i].nvapi_version : "-";
        rows[i * 6 + 5] = runners[i].d7vk_version[0] ? runners[i].d7vk_version : "-";
    }
    print_table(headers, 6, rows, count);

    char total_str[64];
    format_bytes(total_bytes, total_str, sizeof(total_str));
    printf("\nTotal: %zu runner(s), %s\n", count, total_str);

    free(rows);
    free(size_bufs);
    free(runners);
    return 0;
}

static int check_already_installed_and_verified(
    const SteamPaths *paths,
    const SelectedReleaseAssets *selected,
    char *out_name,
    size_t out_cap,
    bool *out_matched,
    char *err,
    size_t err_cap)
{
    *out_matched = false;
    InstalledRunner *runners = NULL;
    size_t count = scan_installed_runners(paths->compat_dir, &runners);

    char stem[256];
    archive_stem(selected->archive.name, stem, sizeof(stem));

    const InstalledRunner *installed = NULL;
    for (size_t i = 0; i < count; i++) {
        const InstalledRunner *r = &runners[i];
        if (strcmp(r->dir_name, selected->tag_name) == 0 ||
            strcmp(r->internal_name, selected->tag_name) == 0 ||
            strcmp(r->dir_name, stem) == 0 ||
            strcmp(r->internal_name, stem) == 0 ||
            (r->version_tag[0] && strcmp(r->version_tag, selected->tag_name) == 0)) {
            installed = r;
            break;
        }
    }

    if (!installed) {
        free(runners);
        return 0;
    }

    char proton_bin[PATH_MAX_LEN], vdf_file[PATH_MAX_LEN];
    snprintf(proton_bin, sizeof(proton_bin), "%s/proton", installed->path);
    snprintf(vdf_file, sizeof(vdf_file), "%s/compatibilitytool.vdf", installed->path);
    if (!file_exists(proton_bin) && !file_exists(vdf_file)) {
        free(runners);
        return 0;
    }

    if (!selected->has_checksum) {
        snprintf(out_name, out_cap, "%s", installed->internal_name);
        *out_matched = true;
        free(runners);
        return 0;
    }

    char expected_hex[160];
    if (fetch_expected_checksum(&selected->checksum, expected_hex, sizeof(expected_hex), err, err_cap) != 0) {
        free(runners);
        return -1;
    }

    char marker_path[PATH_MAX_LEN];
    snprintf(marker_path, sizeof(marker_path), "%s/.prm-checksum", installed->path);
    FILE *mf = fopen(marker_path, "r");
    if (mf) {
        char saved_hex[160] = {0};
        if (fscanf(mf, "%159s", saved_hex) == 1 && strcasecmp(saved_hex, expected_hex) == 0) {
            fclose(mf);
            snprintf(out_name, out_cap, "%s", installed->internal_name);
            *out_matched = true;
            free(runners);
            return 0;
        }
        fclose(mf);
    }

    char cached_archive[PATH_MAX_LEN];
    snprintf(cached_archive, sizeof(cached_archive), "%s/downloads/%s",
             paths->cache_dir, selected->archive.name);
    if (file_exists(cached_archive)) {
        size_t clen = strlen(selected->checksum.name);
        bool use_sha256 = (clen >= 10 && strcasecmp(selected->checksum.name + clen - 10, ".sha256sum") == 0) ||
                          strlen(expected_hex) == 64;
        char actual_hex[160];
        char tmp_err[256];
        if (compute_file_checksum(cached_archive, use_sha256, actual_hex, sizeof(actual_hex), tmp_err, sizeof(tmp_err)) == 0 &&
            strcasecmp(actual_hex, expected_hex) == 0) {
            FILE *wf = fopen(marker_path, "w");
            if (wf) {
                fprintf(wf, "%s  %s\n", expected_hex, selected->archive.name);
                fclose(wf);
            }
            snprintf(out_name, out_cap, "%s", installed->internal_name);
            *out_matched = true;
            free(runners);
            return 0;
        }
    }

    free(runners);
    return 0;
}

int cmd_install(const SteamPaths *paths, const char *spec, bool force, char *out_installed_name, size_t out_cap, char *err, size_t err_cap) {
    char source[64], version[128];
    bool has_ver = false;
    parse_source_spec(spec, source, sizeof(source), version, sizeof(version), &has_ver);

    EndpointConfig endpoint;
    if (resolve_runner_endpoint(paths->config_dir, source, &endpoint, err, err_cap) != 0) {
        return -1;
    }

    ReleaseInfo release;
    if (resolve_release(&endpoint, has_ver ? version : NULL, &release, err, err_cap) != 0) {
        return -1;
    }

    SelectedReleaseAssets selected;
    if (select_release_assets(&release, &selected, err, err_cap) != 0) {
        free_release_info(&release);
        return -1;
    }
    free_release_info(&release);

    if (!force) {
        char existing[256];
        bool matched = false;
        if (check_already_installed_and_verified(paths, &selected, existing, sizeof(existing), &matched, err, err_cap) != 0) {
            return -1;
        }
        if (matched) {
            printf("'%s' (%s) is already installed, up to date, and checksum-verified. Nothing to do.\n",
                   existing, selected.tag_name);
            if (out_installed_name && out_cap > 0) {
                snprintf(out_installed_name, out_cap, "%s", existing);
            }
            return 0;
        }
    }

    fprintf(stderr, "Resolved %s -> %s (asset: %s)\n", spec, selected.tag_name, selected.archive.name);

    char archive_path[PATH_MAX_LEN];
    snprintf(archive_path, sizeof(archive_path), "%s/downloads/%s", paths->cache_dir, selected.archive.name);
    if (download_file(selected.archive.download_url, archive_path, selected.archive.size, err, err_cap) != 0) {
        return -1;
    }

    char verified_hex[160] = {0};
    if (selected.has_checksum) {
        if (verify_checksum(archive_path, &selected.checksum, verified_hex, sizeof(verified_hex), err, err_cap) != 0) {
            return -1;
        }
    }

    char extracted_dir[PATH_MAX_LEN];
    if (extract_tar_archive(archive_path, paths->compat_dir, extracted_dir, sizeof(extracted_dir), err, err_cap) != 0) {
        return -1;
    }

    if (verified_hex[0]) {
        char marker[PATH_MAX_LEN];
        snprintf(marker, sizeof(marker), "%s/.prm-checksum", extracted_dir);
        FILE *mf = fopen(marker, "w");
        if (mf) {
            fprintf(mf, "%s  %s\n", verified_hex, selected.archive.name);
            fclose(mf);
        }
    }

    const char *slash = strrchr(extracted_dir, '/');
    const char *dir_name = slash ? slash + 1 : extracted_dir;

    char vdf_path[PATH_MAX_LEN];
    snprintf(vdf_path, sizeof(vdf_path), "%s/compatibilitytool.vdf", extracted_dir);
    char internal_name[256];
    snprintf(internal_name, sizeof(internal_name), "%s", dir_name);

    FILE *vf = fopen(vdf_path, "rb");
    if (vf) {
        fseek(vf, 0, SEEK_END);
        long sz = ftell(vf);
        rewind(vf);
        if (sz > 0) {
            char *vdf = malloc((size_t)sz + 1);
            if (vdf) {
                size_t rd = fread(vdf, 1, (size_t)sz, vf);
                vdf[rd] = '\0';
                parse_compatibilitytool_vdf(vdf, dir_name, internal_name, sizeof(internal_name));
                free(vdf);
            }
        }
        fclose(vf);
    }

    printf("Installed '%s' to %s\n", internal_name, extracted_dir);
    if (out_installed_name && out_cap > 0) {
        snprintf(out_installed_name, out_cap, "%s", internal_name);
    }
    return 0;
}

int cmd_update(const SteamPaths *paths, const char *source_filter, char *err, size_t err_cap) {
    RunnersConfigFile cfg;
    if (load_runners_config(paths->config_dir, &cfg, err, err_cap) != 0) {
        return -1;
    }

    InstalledRunner *runners = NULL;
    size_t runner_count = scan_installed_runners(paths->compat_dir, &runners);

    char families[32][64];
    size_t family_count = 0;

    if (source_filter && *source_filter) {
        EndpointConfig ep;
        if (resolve_runner_endpoint(paths->config_dir, source_filter, &ep, err, err_cap) != 0) {
            free(runners);
            free_runners_config(&cfg);
            return -1;
        }
        snprintf(families[family_count++], 64, "%s", ep.key);
    } else {
        for (size_t i = 0; i < cfg.count && family_count < 32; i++) {
            const char *prefix = cfg.items[i].installed_prefix[0]
                                     ? cfg.items[i].installed_prefix
                                     : cfg.items[i].key;
            size_t plen = strlen(prefix);
            for (size_t r = 0; r < runner_count; r++) {
                if (strncasecmp(runners[r].dir_name, prefix, plen) == 0) {
                    snprintf(families[family_count++], 64, "%s", cfg.items[i].key);
                    break;
                }
            }
        }
    }

    free(runners);
    free_runners_config(&cfg);

    if (family_count == 0) {
        printf("No matching installed runner families detected from %s/runners.json.\n", paths->config_dir);
        return 0;
    }

    for (size_t i = 0; i < family_count; i++) {
        EndpointConfig ep;
        if (resolve_runner_endpoint(paths->config_dir, families[i], &ep, err, err_cap) != 0) {
            return -1;
        }
        ReleaseInfo latest;
        if (resolve_release(&ep, NULL, &latest, err, err_cap) != 0) {
            return -1;
        }
        SelectedReleaseAssets selected;
        if (select_release_assets(&latest, &selected, err, err_cap) != 0) {
            free_release_info(&latest);
            return -1;
        }

        char existing_name[256];
        bool matched = false;
        if (check_already_installed_and_verified(paths, &selected, existing_name, sizeof(existing_name), &matched, err, err_cap) != 0) {
            free_release_info(&latest);
            return -1;
        }

        if (matched) {
            printf("%s: '%s' (%s) is already up to date and checksum-verified. Nothing to do.\n",
                   families[i], existing_name, selected.tag_name);
        } else {
            printf("%s: new or unverified release %s -> installing...\n", families[i], latest.tag_name);
            char spec[256];
            snprintf(spec, sizeof(spec), "%s@%s", families[i], latest.tag_name);
            if (cmd_install(paths, spec, false, NULL, 0, err, err_cap) != 0) {
                free_release_info(&latest);
                return -1;
            }
        }
        free_release_info(&latest);
    }

    return 0;
}

int cmd_remove(const SteamPaths *paths, const char *const *targets, size_t target_count, char *err, size_t err_cap) {
    InstalledRunner *runners = NULL;
    size_t count = scan_installed_runners(paths->compat_dir, &runners);

    const InstalledRunner **to_remove = calloc(target_count, sizeof(*to_remove));
    if (!to_remove) {
        free(runners);
        snprintf(err, err_cap, "Out of memory");
        return -1;
    }
    size_t rem_count = 0;

    for (size_t t = 0; t < target_count; t++) {
        const InstalledRunner *matched = NULL;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(runners[i].dir_name, targets[t]) == 0 ||
                strcmp(runners[i].internal_name, targets[t]) == 0) {
                matched = &runners[i];
                break;
            }
        }
        if (!matched) {
            snprintf(err, err_cap, "Installed runner '%s' not found.", targets[t]);
            free(to_remove);
            free(runners);
            return -1;
        }
        bool dup = false;
        for (size_t k = 0; k < rem_count; k++) {
            if (strcmp(to_remove[k]->dir_name, matched->dir_name) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) to_remove[rem_count++] = matched;
    }

    for (size_t i = 0; i < rem_count; i++) {
        if (remove_dir_force(to_remove[i]->path, err, err_cap) != 0) {
            free(to_remove);
            free(runners);
            return -1;
        }
        char sz[64];
        format_bytes(to_remove[i]->size_bytes, sz, sizeof(sz));
        printf("Removed %s (freed %s)\n", to_remove[i]->dir_name, sz);
    }

    free(to_remove);
    free(runners);
    return 0;
}

int cmd_purge(const SteamPaths *paths, char *err, size_t err_cap) {
    InstalledRunner *runners = NULL;
    size_t count = scan_installed_runners(paths->compat_dir, &runners);
    if (count == 0) {
        printf("No installed runners to purge.\n");
        free(runners);
        return 0;
    }

    char vdf_path[PATH_MAX_LEN];
    steam_config_vdf_path(paths, vdf_path, sizeof(vdf_path));
    char (*used)[256] = NULL;
    size_t used_count = read_used_runners(vdf_path, &used);

    const InstalledRunner **unused = calloc(count, sizeof(*unused));
    size_t unused_count = 0;
    for (size_t i = 0; i < count; i++) {
        bool is_used = false;
        for (size_t u = 0; u < used_count; u++) {
            if (strcmp(used[u], runners[i].internal_name) == 0 ||
                strcmp(used[u], runners[i].dir_name) == 0) {
                is_used = true;
                break;
            }
        }
        if (!is_used) unused[unused_count++] = &runners[i];
    }
    free(used);

    if (unused_count == 0) {
        printf("No unused runners found.\n");
        free(unused);
        free(runners);
        return 0;
    }

    uint64_t total_bytes = 0;
    const char *headers[] = {"UNUSED RUNNER", "SIZE"};
    const char **rows = calloc(unused_count * 2, sizeof(char *));
    char (*size_bufs)[64] = calloc(unused_count, sizeof(*size_bufs));
    for (size_t i = 0; i < unused_count; i++) {
        total_bytes += unused[i]->size_bytes;
        format_bytes(unused[i]->size_bytes, size_bufs[i], 64);
        rows[i * 2 + 0] = unused[i]->dir_name;
        rows[i * 2 + 1] = size_bufs[i];
    }
    print_table(headers, 2, rows, unused_count);
    free(rows);
    free(size_bufs);

    char total_str[64];
    format_bytes(total_bytes, total_str, sizeof(total_str));
    printf("\nRemove %zu unused runner(s) (%s)? [y/N]: ", unused_count, total_str);
    fflush(stdout);

    char input[64] = {0};
    if (!fgets(input, sizeof(input), stdin)) {
        free(unused);
        free(runners);
        snprintf(err, err_cap, "Failed to read confirmation");
        return -1;
    }

    char *ans = input;
    while (*ans && isspace((unsigned char)*ans)) ans++;
    size_t alen = strlen(ans);
    while (alen > 0 && isspace((unsigned char)ans[alen - 1])) ans[--alen] = '\0';

    if (strcasecmp(ans, "y") != 0 && strcasecmp(ans, "yes") != 0) {
        printf("Aborted.\n");
        free(unused);
        free(runners);
        return 0;
    }

    for (size_t i = 0; i < unused_count; i++) {
        if (remove_dir_force(unused[i]->path, err, err_cap) != 0) {
            free(unused);
            free(runners);
            return -1;
        }
        char sz[64];
        format_bytes(unused[i]->size_bytes, sz, sizeof(sz));
        printf("Removed %s (freed %s)\n", unused[i]->dir_name, sz);
    }

    free(unused);
    free(runners);
    return 0;
}
