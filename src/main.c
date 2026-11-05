#include "prm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char HELP[] =
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

static int dispatch(int argc, char **argv) {
    if (argc < 2 || is_help_flag(argv[1]) || strcmp(argv[1], "help") == 0) {
        puts(HELP);
        return 0;
    }
    if (strcmp(argv[1], "-V") == 0 || strcmp(argv[1], "--version") == 0) {
        puts("prm " PRM_VERSION);
        return 0;
    }
    for (int i = 2; i < argc; i++) {
        if (is_help_flag(argv[i])) {
            puts(HELP);
            return 0;
        }
    }

    struct prm_paths paths;
    if (prm_paths_resolve(&paths) != 0) return -1;

    const char *cmd = argv[1];
    if (strcmp(cmd, "list") == 0) {
        const char *remote = NULL;
        size_t limit = 10;
        for (int i = 2; i < argc; ) {
            const char *arg = argv[i];
            if (strcmp(arg, "-r") == 0 || strcmp(arg, "--remote") == 0) {
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    remote = argv[i + 1];
                    i += 2;
                } else {
                    remote = "";
                    i++;
                }
            } else if (strncmp(arg, "--remote=", 9) == 0) {
                remote = arg + 9;
                i++;
            } else if (strcmp(arg, "-l") == 0 || strcmp(arg, "--limit") == 0) {
                if (i + 1 >= argc) return prm_err("Missing value for --limit");
                char *end = NULL;
                unsigned long v = strtoul(argv[i + 1], &end, 10);
                if (!argv[i + 1][0] || (end && *end)) return prm_err("Invalid limit: '%s'", argv[i + 1]);
                limit = (size_t)v;
                i += 2;
            } else if (strncmp(arg, "--limit=", 8) == 0) {
                const char *val = arg + 8;
                char *end = NULL;
                unsigned long v = strtoul(val, &end, 10);
                if (!val[0] || (end && *end)) return prm_err("Invalid limit: '%s'", val);
                limit = (size_t)v;
                i++;
            } else {
                return prm_err("Unexpected argument for list: '%s'", arg);
            }
        }
        return cmd_list(&paths, remote, limit);
    }

    if (strcmp(cmd, "install") == 0) {
        bool force = false;
        const char *spec = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0) {
                force = true;
            } else if (argv[i][0] == '-') {
                return prm_err("Unknown option for install: '%s'", argv[i]);
            } else if (!spec) {
                spec = argv[i];
            } else {
                return prm_err("Unexpected argument for install: '%s'", argv[i]);
            }
        }
        if (!spec) return prm_err("Usage: prm install [-f|--force] <SPEC>");
        return cmd_install(&paths, spec, force);
    }

    if (strcmp(cmd, "update") == 0) {
        if (argc > 3) return prm_err("Usage: prm update [SOURCE]");
        if (argc == 3 && argv[2][0] == '-') return prm_err("Unknown option for update: '%s'", argv[2]);
        return cmd_update(&paths, argc == 3 ? argv[2] : NULL);
    }

    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) return prm_err("Usage: prm remove <RUNNER>...");
        for (int i = 2; i < argc; i++) {
            if (argv[i][0] == '-') return prm_err("Unknown option for remove: '%s'", argv[i]);
        }
        return cmd_remove(&paths, argv + 2, (size_t)(argc - 2));
    }

    if (strcmp(cmd, "purge") == 0) {
        if (argc > 2) return prm_err("Unexpected argument for purge: '%s'", argv[2]);
        return cmd_purge(&paths);
    }

    return prm_err("Unknown command '%s'. Run 'prm --help' for usage.", cmd);
}

int main(int argc, char **argv) {
    if (dispatch(argc, argv) != 0) {
        fprintf(stderr, "Error: %s\n", prm_err_str());
        return 1;
    }
    return 0;
}
