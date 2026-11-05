#include "../src/prm.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_missing_config(void) {
    RunnersConfigFile cfg;
    char err[1024] = {0};
    int rc = load_runners_config("/tmp/prm-definitely-missing-config-dir-99999", &cfg, err, sizeof(err));
    assert(rc != 0);
    assert(strstr(err, "Missing runner configuration file") != NULL);
    assert(strstr(err, "\"releases_url\"") != NULL);
}

static void test_repo_config_valid(void) {
    RunnersConfigFile cfg;
    char err[1024] = {0};
    int rc = load_runners_config("config", &cfg, err, sizeof(err));
    assert(rc == 0);
    assert(cfg.count >= 5);

    EndpointConfig ep;
    assert(resolve_runner_endpoint("config", "ge", &ep, err, sizeof(err)) == 0);
    assert(strcmp(ep.key, "ge") == 0);
    assert(resolve_runner_endpoint("config", "proton-cachyos", &ep, err, sizeof(err)) == 0);
    assert(strcmp(ep.key, "cachyos") == 0);
    free_runners_config(&cfg);
}

static void test_select_release_assets(void) {
    ReleaseAsset assets[4] = {
        {"proton-cachyos-10.0-arm64.tar.xz", "http://local/arm64.tar.xz", 100},
        {"proton-cachyos-10.0-x86_64_v3.tar.xz", "http://local/v3.tar.xz", 100},
        {"proton-cachyos-10.0-x86_64.tar.xz", "http://local/v1.tar.xz", 100},
        {"proton-cachyos-10.0-x86_64.tar.xz.sha512sum", "http://local/v1.sha512sum", 128},
    };
    ReleaseInfo rel;
    memset(&rel, 0, sizeof(rel));
    snprintf(rel.tag_name, sizeof(rel.tag_name), "cachyos-10.0");
    snprintf(rel.published_at, sizeof(rel.published_at), "2026-10-01");
    rel.assets = assets;
    rel.asset_count = 4;

    SelectedReleaseAssets sel;
    char err[256] = {0};
    assert(select_release_assets(&rel, &sel, err, sizeof(err)) == 0);
    assert(strcmp(sel.archive.name, "proton-cachyos-10.0-x86_64.tar.xz") == 0);
    assert(sel.has_checksum);
}

static void test_vdf_parsers(void) {
    const char *compat_vdf =
        "\"compatibilitytools\"\n"
        "{\n"
        "  \"compat_tools\"\n"
        "  {\n"
        "    \"GE-Proton11-12\" // Internal name\n"
        "    {\n"
        "      \"install_path\" \".\"\n"
        "      \"display_name\" \"GE-Proton11-12\"\n"
        "    }\n"
        "  }\n"
        "}\n";
    char internal[256] = {0};
    parse_compatibilitytool_vdf(compat_vdf, "fallback", internal, sizeof(internal));
    assert(strcmp(internal, "GE-Proton11-12") == 0);

    const char *cfg_vdf =
        "\"InstallConfigStore\"\n"
        "{\n"
        "\t\"Software\"\n"
        "\t{\n"
        "\t\t\"Valve\"\n"
        "\t\t{\n"
        "\t\t\t\"Steam\"\n"
        "\t\t\t{\n"
        "\t\t\t\t\"CompatToolMapping\"\n"
        "\t\t\t\t{\n"
        "\t\t\t\t\t\"0\"\n"
        "\t\t\t\t\t{\n"
        "\t\t\t\t\t\t\"name\"\t\t\"GE-Proton11-12\"\n"
        "\t\t\t\t\t\t\"config\"\t\t\"\"\n"
        "\t\t\t\t\t\t\"priority\"\t\t\"75\"\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\t\"2231380\"\n"
        "\t\t\t\t\t{\n"
        "\t\t\t\t\t\t\"name\"\t\t\"GE-Proton9-27\"\n"
        "\t\t\t\t\t\t\"config\"\t\t\"\"\n"
        "\t\t\t\t\t\t\"priority\"\t\t\"250\"\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n"
        "}";
    char (*used)[256] = NULL;
    size_t count = parse_used_runners_str(cfg_vdf, &used);
    assert(count == 2);
    assert(strcmp(used[0], "GE-Proton11-12") == 0);
    assert(strcmp(used[1], "GE-Proton9-27") == 0);
}

static void test_sha256_and_sha512(void) {
    char hex256[160] = {0}, hex512[160] = {0}, err[256] = {0};
    assert(compute_file_checksum("config/runners.json", true, hex256, sizeof(hex256), err, sizeof(err)) == 0);
    assert(strlen(hex256) == 64);
    assert(compute_file_checksum("config/runners.json", false, hex512, sizeof(hex512), err, sizeof(err)) == 0);
    assert(strlen(hex512) == 128);
}

static void test_tar_extract_gz_xz_zst(void) {
    char err[512] = {0};
    const char *base = "/tmp/prm-c-archive-test";
    remove_dir_force(base, err, sizeof(err));
    assert(mkdir_p("/tmp/prm-c-archive-test/src/FakeProton/files/lib64/wine/dxvk", err, sizeof(err)) == 0);

    FILE *f = fopen("/tmp/prm-c-archive-test/src/FakeProton/proton", "w");
    assert(f != NULL);
    fputs("#!/bin/sh\n", f);
    fclose(f);
    chmod("/tmp/prm-c-archive-test/src/FakeProton/proton", 0555);

    f = fopen("/tmp/prm-c-archive-test/src/FakeProton/files/lib64/wine/dxvk/version", "w");
    assert(f != NULL);
    fputs("dxvk (v2.5.1)\n", f);
    fclose(f);

    assert(system("tar -C /tmp/prm-c-archive-test/src -czf /tmp/prm-c-archive-test/FakeProton.tar.gz FakeProton") == 0);
    assert(system("tar -C /tmp/prm-c-archive-test/src -cJf /tmp/prm-c-archive-test/FakeProton.tar.xz FakeProton") == 0);
    assert(system("tar -C /tmp/prm-c-archive-test/src --zstd -cf /tmp/prm-c-archive-test/FakeProton.tar.zst FakeProton") == 0);

    const char *exts[] = {"tar.gz", "tar.xz", "tar.zst"};
    for (size_t i = 0; i < 3; i++) {
        char arch[256], dest[256], extracted[PATH_MAX_LEN];
        snprintf(arch, sizeof(arch), "/tmp/prm-c-archive-test/FakeProton.%s", exts[i]);
        snprintf(dest, sizeof(dest), "/tmp/prm-c-archive-test/out_%zu", i);
        assert(extract_tar_archive(arch, dest, extracted, sizeof(extracted), err, sizeof(err)) == 0);

        InstalledRunner *runners = NULL;
        size_t n = scan_installed_runners(dest, &runners);
        assert(n == 1);
        assert(strcmp(runners[0].dir_name, "FakeProton") == 0);
        assert(strcmp(runners[0].dxvk_version, "v2.5.1") == 0);
        free(runners);
    }

    assert(remove_dir_force(base, err, sizeof(err)) == 0);
}

int main(void) {
    test_missing_config();
    test_repo_config_valid();
    test_select_release_assets();
    test_vdf_parsers();
    test_sha256_and_sha512();
    test_tar_extract_gz_xz_zst();
    printf("All C unit tests passed.\n");
    return 0;
}
