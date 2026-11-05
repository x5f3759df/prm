#include "prm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int cmd_list(const struct prm_paths *paths, const char *remote, size_t limit) {
    if (remote != NULL) {
        while (*remote && isspace((unsigned char)*remote)) remote++;
        if (*remote == '\0') {
            struct prm_source *sources = NULL;
            size_t count = 0;
            if (load_runners_config(paths->config_dir, &sources, &count) != 0) return -1;

            const char *headers[] = {"SOURCE", "ALIASES", "RELEASES URL"};
            const char **cells = calloc(count * 3, sizeof(*cells));
            char (*alias_bufs)[256] = calloc(count, sizeof(*alias_bufs));
            if (!cells || !alias_bufs) {
                free(cells);
                free(alias_bufs);
                free(sources);
                return prm_err("Out of memory");
            }
            for (size_t i = 0; i < count; i++) {
                if (sources[i].alias_count == 0) {
                    snprintf(alias_bufs[i], 256, "-");
                } else {
                    size_t pos = 0;
                    for (size_t a = 0; a < sources[i].alias_count; a++) {
                        int w = snprintf(alias_bufs[i] + pos, 256 - pos, "%s%s",
                                         a > 0 ? ", " : "", sources[i].aliases[a]);
                        if (w > 0 && pos + (size_t)w < 256) pos += (size_t)w;
                    }
                }
                cells[i * 3 + 0] = sources[i].key;
                cells[i * 3 + 1] = alias_bufs[i];
                cells[i * 3 + 2] = sources[i].releases_url;
            }
            print_table(headers, 3, cells, count);
            free(cells);
            free(alias_bufs);
            free(sources);
            return 0;
        }

        struct prm_source ep;
        if (resolve_runner_endpoint(paths->config_dir, remote, &ep) != 0) return -1;

        struct prm_release *rels = NULL;
        size_t count = 0;
        if (fetch_releases(&ep, limit, &rels, &count) != 0) return -1;

        const char *headers[] = {"TAG", "PUBLISHED", "SIZE", "ASSET"};
        const char **cells = calloc(count * 4, sizeof(*cells));
        char (*size_bufs)[64] = calloc(count, sizeof(*size_bufs));
        char (*asset_bufs)[256] = calloc(count, sizeof(*asset_bufs));
        if (!cells || !size_bufs || !asset_bufs) {
            free(cells);
            free(size_bufs);
            free(asset_bufs);
            free_releases(rels, count);
            return prm_err("Out of memory");
        }

        for (size_t i = 0; i < count; i++) {
            struct prm_selected_assets sel;
            if (select_release_assets(&rels[i], &sel) == 0) {
                format_bytes(sel.archive.size, size_bufs[i], 64);
                snprintf(asset_bufs[i], 256, "%s", sel.archive.name);
            } else {
                snprintf(size_bufs[i], 64, "-");
                snprintf(asset_bufs[i], 256, "(no x86_64 archive)");
            }
            cells[i * 4 + 0] = rels[i].tag_name;
            cells[i * 4 + 1] = rels[i].published_at;
            cells[i * 4 + 2] = size_bufs[i];
            cells[i * 4 + 3] = asset_bufs[i];
        }
        print_table(headers, 4, cells, count);
        free(cells);
        free(size_bufs);
        free(asset_bufs);
        free_releases(rels, count);
        return 0;
    }

    struct prm_runner *runners = NULL;
    size_t count = scan_installed_runners(paths->compat_dir, &runners);
    if (count == 0) {
        printf("No custom Proton runners installed in %s\n", paths->compat_dir);
        free(runners);
        return 0;
    }

    uint64_t total_bytes = 0;
    const char *headers[] = {"RUNNER", "SIZE", "DXVK", "VKD3D-PROTON", "NVAPI", "D7VK"};
    const char **cells = calloc(count * 6, sizeof(*cells));
    char (*size_bufs)[64] = calloc(count, sizeof(*size_bufs));
    if (!cells || !size_bufs) {
        free(cells);
        free(size_bufs);
        free(runners);
        return prm_err("Out of memory");
    }

    for (size_t i = 0; i < count; i++) {
        total_bytes += runners[i].size_bytes;
        format_bytes(runners[i].size_bytes, size_bufs[i], 64);
        cells[i * 6 + 0] = runners[i].dir_name;
        cells[i * 6 + 1] = size_bufs[i];
        cells[i * 6 + 2] = runners[i].dxvk[0] ? runners[i].dxvk : "-";
        cells[i * 6 + 3] = runners[i].vkd3d[0] ? runners[i].vkd3d : "-";
        cells[i * 6 + 4] = runners[i].nvapi[0] ? runners[i].nvapi : "-";
        cells[i * 6 + 5] = runners[i].d7vk[0] ? runners[i].d7vk : "-";
    }
    print_table(headers, 6, cells, count);

    char total_str[64];
    format_bytes(total_bytes, total_str, sizeof(total_str));
    printf("\nTotal: %zu runner(s), %s\n", count, total_str);

    free(cells);
    free(size_bufs);
    free(runners);
    return 0;
}

static int check_installed_verified(const struct prm_paths *paths,
                                    const struct prm_selected_assets *selected,
                                    char *out_name, size_t out_cap, bool *out_matched) {
    *out_matched = false;
    struct prm_runner *runners = NULL;
    size_t count = scan_installed_runners(paths->compat_dir, &runners);

    char stem[256];
    archive_stem(selected->archive.name, stem, sizeof(stem));

    const struct prm_runner *installed = NULL;
    for (size_t i = 0; i < count; i++) {
        const struct prm_runner *r = &runners[i];
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
    if (!path_exists(proton_bin) && !path_exists(vdf_file)) {
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
    if (fetch_expected_checksum(&selected->checksum, expected_hex, sizeof(expected_hex)) != 0) {
        free(runners);
        return -1;
    }

    char marker_path[PATH_MAX_LEN];
    snprintf(marker_path, sizeof(marker_path), "%s/.prm-checksum", installed->path);
    char *saved = read_file_str(marker_path, NULL);
    if (saved) {
        char saved_hex[160] = {0};
        if (sscanf(saved, "%159s", saved_hex) == 1 && strcasecmp(saved_hex, expected_hex) == 0) {
            free(saved);
            snprintf(out_name, out_cap, "%s", installed->internal_name);
            *out_matched = true;
            free(runners);
            return 0;
        }
        free(saved);
    }

    char cached_archive[PATH_MAX_LEN];
    snprintf(cached_archive, sizeof(cached_archive), "%s/downloads/%s",
             paths->cache_dir, selected->archive.name);
    if (path_exists(cached_archive)) {
        size_t clen = strlen(selected->checksum.name);
        bool use_sha256 = (clen >= 10 && strcasecmp(selected->checksum.name + clen - 10, ".sha256sum") == 0) ||
                          strlen(expected_hex) == 64;
        char actual_hex[160];
        if (compute_file_checksum(cached_archive, use_sha256, actual_hex, sizeof(actual_hex)) == 0 &&
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

int cmd_install(const struct prm_paths *paths, const char *spec, bool force) {
    char source[64];
    const char *at = strchr(spec, '@');
    const char *version = NULL;
    if (at) {
        size_t slen = (size_t)(at - spec);
        if (slen >= sizeof(source)) slen = sizeof(source) - 1;
        memcpy(source, spec, slen);
        source[slen] = '\0';
        version = at + 1;
    } else {
        snprintf(source, sizeof(source), "%s", spec);
    }

    struct prm_source ep;
    if (resolve_runner_endpoint(paths->config_dir, source, &ep) != 0) return -1;

    struct prm_release rel;
    if (resolve_release(&ep, version, &rel) != 0) return -1;

    struct prm_selected_assets selected;
    int sel_rc = select_release_assets(&rel, &selected);
    free(rel.assets);
    if (sel_rc != 0) return -1;

    if (!force) {
        char existing[256];
        bool matched = false;
        if (check_installed_verified(paths, &selected, existing, sizeof(existing), &matched) != 0) {
            return -1;
        }
        if (matched) {
            printf("'%s' (%s) is already installed, up to date, and checksum-verified. Nothing to do.\n",
                   existing, selected.tag_name);
            return 0;
        }
    }

    fprintf(stderr, "Resolved %s -> %s (asset: %s)\n", spec, selected.tag_name, selected.archive.name);

    char archive_path[PATH_MAX_LEN];
    snprintf(archive_path, sizeof(archive_path), "%s/downloads/%s", paths->cache_dir, selected.archive.name);
    if (download_file(selected.archive.download_url, archive_path, selected.archive.size) != 0) return -1;

    char verified_hex[160] = {0};
    if (selected.has_checksum &&
        verify_checksum(archive_path, &selected.checksum, verified_hex, sizeof(verified_hex)) != 0) {
        return -1;
    }

    char extracted_dir[PATH_MAX_LEN];
    if (extract_tar_archive(archive_path, paths->compat_dir, extracted_dir, sizeof(extracted_dir)) != 0) {
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

    char vdf_path[PATH_MAX_LEN], internal_name[256];
    snprintf(vdf_path, sizeof(vdf_path), "%s/compatibilitytool.vdf", extracted_dir);
    char *vdf = read_file_str(vdf_path, NULL);
    if (vdf) {
        parse_compatibilitytool_vdf(vdf, dir_name, internal_name, sizeof(internal_name));
        free(vdf);
    } else {
        snprintf(internal_name, sizeof(internal_name), "%s", dir_name);
    }

    printf("Installed '%s' to %s\n", internal_name, extracted_dir);
    return 0;
}

int cmd_update(const struct prm_paths *paths, const char *source_filter) {
    struct prm_source *sources = NULL;
    size_t src_count = 0;
    if (load_runners_config(paths->config_dir, &sources, &src_count) != 0) return -1;

    struct prm_runner *runners = NULL;
    size_t runner_count = scan_installed_runners(paths->compat_dir, &runners);

    char families[32][64];
    size_t family_count = 0;

    if (source_filter && *source_filter) {
        struct prm_source ep;
        if (resolve_runner_endpoint(paths->config_dir, source_filter, &ep) != 0) {
            free(runners);
            free(sources);
            return -1;
        }
        snprintf(families[family_count++], 64, "%s", ep.key);
    } else {
        for (size_t i = 0; i < src_count && family_count < 32; i++) {
            const char *prefix = sources[i].installed_prefix[0]
                                     ? sources[i].installed_prefix
                                     : sources[i].key;
            size_t plen = strlen(prefix);
            for (size_t r = 0; r < runner_count; r++) {
                if (strncasecmp(runners[r].dir_name, prefix, plen) == 0) {
                    snprintf(families[family_count++], 64, "%s", sources[i].key);
                    break;
                }
            }
        }
    }

    free(runners);
    free(sources);

    if (family_count == 0) {
        printf("No matching installed runner families detected from %s/runners.json.\n", paths->config_dir);
        return 0;
    }

    for (size_t i = 0; i < family_count; i++) {
        struct prm_source ep;
        if (resolve_runner_endpoint(paths->config_dir, families[i], &ep) != 0) return -1;

        struct prm_release latest;
        if (resolve_release(&ep, NULL, &latest) != 0) return -1;

        struct prm_selected_assets selected;
        int sel_rc = select_release_assets(&latest, &selected);
        free(latest.assets);
        if (sel_rc != 0) return -1;

        char existing_name[256];
        bool matched = false;
        if (check_installed_verified(paths, &selected, existing_name, sizeof(existing_name), &matched) != 0) {
            return -1;
        }

        if (matched) {
            printf("%s: '%s' (%s) is already up to date and checksum-verified. Nothing to do.\n",
                   families[i], existing_name, selected.tag_name);
        } else {
            printf("%s: new or unverified release %s -> installing...\n", families[i], selected.tag_name);
            char spec[256];
            snprintf(spec, sizeof(spec), "%s@%s", families[i], selected.tag_name);
            if (cmd_install(paths, spec, false) != 0) return -1;
        }
    }
    return 0;
}

int cmd_remove(const struct prm_paths *paths, char *const *targets, size_t target_count) {
    struct prm_runner *runners = NULL;
    size_t count = scan_installed_runners(paths->compat_dir, &runners);

    const struct prm_runner **to_remove = calloc(target_count, sizeof(*to_remove));
    if (!to_remove) {
        free(runners);
        return prm_err("Out of memory");
    }
    size_t rem_count = 0;

    for (size_t t = 0; t < target_count; t++) {
        const struct prm_runner *matched = NULL;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(runners[i].dir_name, targets[t]) == 0 ||
                strcmp(runners[i].internal_name, targets[t]) == 0) {
                matched = &runners[i];
                break;
            }
        }
        if (!matched) {
            free(to_remove);
            free(runners);
            return prm_err("Installed runner '%s' not found.", targets[t]);
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
        if (remove_dir_force(to_remove[i]->path) != 0) {
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

int cmd_purge(const struct prm_paths *paths) {
    struct prm_runner *runners = NULL;
    size_t count = scan_installed_runners(paths->compat_dir, &runners);
    if (count == 0) {
        printf("No installed runners to purge.\n");
        free(runners);
        return 0;
    }

    char vdf_path[PATH_MAX_LEN];
    snprintf(vdf_path, sizeof(vdf_path), "%s/config/config.vdf", paths->steam_dir);
    char (*used)[256] = NULL;
    size_t used_count = read_used_runners(vdf_path, &used);

    const struct prm_runner **unused = calloc(count, sizeof(*unused));
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
    const char **cells = calloc(unused_count * 2, sizeof(*cells));
    char (*size_bufs)[64] = calloc(unused_count, sizeof(*size_bufs));
    for (size_t i = 0; i < unused_count; i++) {
        total_bytes += unused[i]->size_bytes;
        format_bytes(unused[i]->size_bytes, size_bufs[i], 64);
        cells[i * 2 + 0] = unused[i]->dir_name;
        cells[i * 2 + 1] = size_bufs[i];
    }
    print_table(headers, 2, cells, unused_count);
    free(cells);
    free(size_bufs);

    char total_str[64];
    format_bytes(total_bytes, total_str, sizeof(total_str));
    printf("\nRemove %zu unused runner(s) (%s)? [y/N]: ", unused_count, total_str);
    fflush(stdout);

    char input[64] = {0};
    if (!fgets(input, sizeof(input), stdin)) {
        free(unused);
        free(runners);
        return prm_err("Failed to read confirmation");
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
        if (remove_dir_force(unused[i]->path) != 0) {
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
