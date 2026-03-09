/**
 * @file main.c
 * @brief Platform entry point for POSIX systems (Linux, macOS, Windows)
 *
 * This is the platform-specific entry point that handles:
 * - Command line argument parsing
 * - Platform-specific initialization (Windows console encoding)
 * - Default config path resolution
 * - Calling the platform-agnostic application layer
 */

#include "app.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS] [config_file]\n", prog);
    printf("Options:\n");
    printf("  -c, --config <path>    Specify config file path\n");
    printf("  -l, --logs [level]     Enable logging (level: error, warn, info, debug)\n");
    printf("  -h, --help             Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s                      Use default config at ~/.mimiclaw/config.json\n", prog);
    printf("  %s ./myconfig.json      Use specified config file\n", prog);
    printf("  %s -l debug             Enable debug level logging\n", prog);
    printf("  %s --logs=debug         Enable debug level logging (alternative)\n", prog);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    bool enable_logs = false;
    const char *config_path = NULL;
    const char *log_level = NULL;

    static struct option long_options[] = {
        {"logs", optional_argument, 0, 'l'},
        {"config", required_argument, 0, 'c'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int optidx = 0;

    while ((opt = getopt_long(argc, argv, "c:l::h", long_options, &optidx)) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
                break;
            case 'l':
                enable_logs = true;
                log_level = optarg;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    /* Handle remaining non-option arguments as config file */
    if (optind < argc && config_path == NULL) {
        /* Skip any arguments that look like option values that got misplaced */
        while (optind < argc) {
            if (argv[optind][0] != '-' && 
                strcmp(argv[optind], "debug") != 0 &&
                strcmp(argv[optind], "info") != 0 &&
                strcmp(argv[optind], "warn") != 0 &&
                strcmp(argv[optind], "error") != 0) {
                config_path = argv[optind];
                break;
            }
            optind++;
        }
    }

    if (!config_path) {
        char config_buf[512] = {0};
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || home[0] == '\0') {
            home = getenv("USERPROFILE");
        }
#endif
        if (home && home[0] != '\0') {
            snprintf(config_buf, sizeof(config_buf), "%s/.mimiclaw/config.json", home);
            config_path = config_buf;
        } else {
            config_path = "./config.json";
        }
    }

    MIMI_LOGI("main", "Starting MimiClaw...");
    MIMI_LOGI("main", "Config path: %s", config_path);

    mimi_err_t err = app_init(config_path, enable_logs, log_level);
    if (err != MIMI_OK) {
        MIMI_LOGE("main", "app_init failed: %s", mimi_err_to_name(err));
        return 1;
    }

    err = app_start();
    if (err != MIMI_OK) {
        MIMI_LOGE("main", "app_start failed: %s", mimi_err_to_name(err));
        app_destroy();
        return 1;
    }

    err = app_run();

    app_stop();
    app_destroy();

    MIMI_LOGI("main", "MimiClaw exited with code: %s", mimi_err_to_name(err));
    return (err == MIMI_OK) ? 0 : 1;
}
