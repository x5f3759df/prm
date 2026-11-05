#include "prm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const HELP =
    "prm — Proton Runner Manager\n\n"
    "Usage: prm <COMMAND> [OPTIONS]\n\n"
    "Commands:\n"
    "  list [-r|--remote [SOURCE]] [-l|--limit <N>]   List installed runners or remote releases\n"
    "  install [-f|--force] <SPEC>                    Install runner (e.g. ge, cachyos, ge@GE-Proton10-1)\n"
    "  update [SOURCE]                                Update installed runner families\n"
    "  remove <RUNNER>...                             Remove installed runner(s)\n"
    "  purge                                          Remove all unused installed runners\n\n"
    "Options:\n"
    "  -h, --help      Print help\n"
    "  -V, --version   Print version";

static bool is_help_flag(const char *s) {
    return strcmp(s, "-h") == 0 || strcmp(s, "--help") == 0;
}

static int run_cli(int argc, char **argv, char *err, size_t err_cap) {
    if (argc < 2) {
        puts(HELP);
        return 0;
    }

    const char *cmd = argv[1];
    if (is_help_flag(cmd) || strcmp(cmd, "help") == 0) {
        puts(HELP);
        return 0;
    }
    if (strcmp(cmd, "-V") == 0 || strcmp(cmd, "--version") == 0) {
        printf("prm %s\n", PRM_VERSION);
        return 0;
    }

    for (int i = 2; i < argc; i++) {
        if (is_help_flag(argv[i])) {
            puts(HELP);
            return 0;
        }
    }

    SteamPaths paths;
    if (steam_paths_resolve(&paths, err, err_cap) != 0) {
        return -1;
    }

    if (strcmp(cmd, "list") == 0) {
        const char *remote = NULL;
        size_t limit = 10;
        int i = 2;
        while (i < argc) {
            const char *arg = argv[i];
            if (strcmp(arg, "-r") == 0 || strcmp(arg, "--remote") == 0) {
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    remote = argv[i + 1];
                    i += 2;
                } else {
                    remote = "";
                    i += 1;
                }
            } else if (strncmp(arg, "--remote=", 9) == 0) {
                remote = arg + 9;
                i += 1;
            } else if (strcmp(arg, "-l") == 0 || strcmp(arg, "--limit") == 0) {
                if (i + 1 >= argc) {
                    snprintf(err, err_cap, "Missing value for --limit");
                    return -1;
                }
                char *end = NULL;
                unsigned long v = strtoul(argv[i + 1], &end, 10);
                if (!argv[i + 1][0] || (end && *end != '\0')) {
                    snprintf(err, err_cap, "Invalid limit: '%s'", argv[i + 1]);
                    return -1;
                }
                limit = (size_t)v;
                i += 2;
            } else if (strncmp(arg, "--limit=", 8) == 0) {
                const char *val = arg + 8;
                char *end = NULL;
                unsigned long v = strtoul(val, &end, 10);
                if (!val[0] || (end && *end != '\0')) {
                    snprintf(err, err_cap, "Invalid limit: '%s'", val);
                    return -1;
                }
                limit = (size_t)v;
                i += 1;
            } else {
                snprintf(err, err_cap, "Unexpected argument for list: '%s'", arg);
                return -1;
            }
        }
        return cmd_list(&paths, remote, limit, err, err_cap);
    }

    if (strcmp(cmd, "install") == 0) {
        bool force = false;
        const char *spec = NULL;
        for (int i = 2; i < argc; i++) {
            const char *arg = argv[i];
            if (strcmp(arg, "-f") == 0 || strcmp(arg, "--force") == 0) {
                force = true;
            } else if (arg[0] == '-') {
                snprintf(err, err_cap, "Unknown option for install: '%s'", arg);
                return -1;
            } else if (!spec) {
                spec = arg;
            } else {
                snprintf(err, err_cap, "Unexpected argument for install: '%s'", arg);
                return -1;
            }
        }
        if (!spec) {
            snprintf(err, err_cap, "Usage: prm install [-f|--force] <SPEC>");
            return -1;
        }
        return cmd_install(&paths, spec, force, NULL, 0, err, err_cap);
    }

    if (strcmp(cmd, "update") == 0) {
        if (argc > 3) {
            snprintf(err, err_cap, "Usage: prm update [SOURCE]");
            return -1;
        }
        if (argc == 3 && argv[2][0] == '-') {
            snprintf(err, err_cap, "Unknown option for update: '%s'", argv[2]);
            return -1;
        }
        return cmd_update(&paths, argc == 3 ? argv[2] : NULL, err, err_cap);
    }

    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) {
            snprintf(err, err_cap, "Usage: prm remove <RUNNER>...");
            return -1;
        }
        for (int i = 2; i < argc; i++) {
            if (argv[i][0] == '-') {
                snprintf(err, err_cap, "Unknown option for remove: '%s'", argv[i]);
                return -1;
            }
        }
        return cmd_remove(&paths, (const char *const *)(argv + 2), (size_t)(argc - 2), err, err_cap);
    }

    if (strcmp(cmd, "purge") == 0) {
        if (argc > 2) {
            snprintf(err, err_cap, "Unexpected argument for purge: '%s'", argv[2]);
            return -1;
        }
        return cmd_purge(&paths, err, err_cap);
    }

    snprintf(err, err_cap, "Unknown command '%s'. Run 'prm --help' for usage.", cmd);
    return -1;
}

#ifndef PRM_TESTING
int main(int argc, char **argv) {
    char err[1024] = {0};
    if (run_cli(argc, argv, err, sizeof(err)) != 0) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }
    return 0;
}
#endif
