/**
 * @file main.c
 * @brief Platform entry point for POSIX systems (Linux, macOS, Windows)
 *
 * This is the platform-specific entry point that handles:
 * - Command line argument parsing
 * - Platform-specific initialization (Windows console encoding)
 * - Default config path resolution
 * - Starting the OS scheduler with application logic
 */

#include "app.h"
#include "log.h"
#include "os/os.h"

#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Options:\n");
    printf("  -c, --config <path>    Specify config file path\n");
    printf("  -l, --logs [level]     Enable logging (level: error, warn, info, debug)\n");
    printf("  -f, --log-file <path>  Write logs to file (relative path uses workspace)\n");
    printf("  -g, --gateway          Gateway mode (no STDIO, daemon-like)\n");
    printf("  -h, --help             Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s                      Use default config at ~/.mimiclaw/config.json\n", prog);
    printf("  %s -c ./myconfig.json   Use specified config file\n", prog);
    printf("  %s -l debug             Enable debug level logging\n", prog);
    printf("  %s --logs=debug         Enable debug level logging (alternative)\n", prog);
    printf("  %s -f logs/app.log      Write logs to workspace/logs/app.log\n", prog);
    printf("  %s -g                   Run in gateway mode (no STDIO)\n", prog);
}

typedef struct {
    int argc;
    char **argv;
    bool enable_logs;
    bool gateway_mode;
    const char *config_path;
    const char *log_level;
    const char *log_file_path;
} main_args_t;

/* Application main function - runs in task context for FreeRTOS */
static void app_main_task(void *arg)
{
    main_args_t *args = (main_args_t *)arg;
    
    MIMI_LOGI("main", "Starting MimiClaw (%s)...", mimi_os_get_version());
    MIMI_LOGI("main", "Config path: %s", args->config_path);
    MIMI_LOGI("main", "Gateway mode: %s", args->gateway_mode ? "enabled" : "disabled");

    mimi_err_t err = mimi_os_init();
    if (err != MIMI_OK) {
        MIMI_LOGE("main", "mimi_os_init failed: %s", mimi_err_to_name(err));
        return;
    }

    err = app_init(args->config_path, args->enable_logs, args->log_level, args->gateway_mode, args->log_file_path);
    if (err != MIMI_OK) {
        MIMI_LOGE("main", "app_init failed: %s", mimi_err_to_name(err));
        return;
    }

    err = app_start();
    if (err != MIMI_OK) {
        MIMI_LOGE("main", "app_start failed: %s", mimi_err_to_name(err));
        app_destroy();
        return;
    }

    err = app_run();

    app_stop();
    app_destroy();

    MIMI_LOGI("main", "MimiClaw exited with code: %s", mimi_err_to_name(err));
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    main_args_t args = {
        .argc = argc,
        .argv = argv,
        .enable_logs = false,
        .gateway_mode = false,
        .config_path = NULL,
        .log_level = NULL,
        .log_file_path = NULL
    };

    static struct option long_options[] = {
        {"logs", optional_argument, 0, 'l'},
        {"config", required_argument, 0, 'c'},
        {"log-file", required_argument, 0, 'f'},
        {"gateway", no_argument, 0, 'g'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int optidx = 0;

    optind = 1;
    opterr = 0;

    while ((opt = getopt_long(argc, argv, "c:l:f:gh", long_options, &optidx)) != -1) {
        switch (opt) {
            case 'c':
                args.config_path = optarg;
                break;
            case 'l':
                args.enable_logs = true;
                args.log_level = optarg;
                break;
            case 'f':
                args.enable_logs = true;
                args.log_file_path = optarg;
                break;
            case 'g':
                args.gateway_mode = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (!args.config_path) {
        char config_buf[512] = {0};
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || home[0] == '\0') {
            home = getenv("USERPROFILE");
        }
#endif
        if (home && home[0] != '\0') {
            snprintf(config_buf, sizeof(config_buf), "%s/.mimiclaw/config.json", home);
            args.config_path = config_buf;
        } else {
            args.config_path = "./config.json";
        }
    }

    /* Start OS scheduler and run application in task context */
    mimi_err_t err = mimi_os_start_scheduler(app_main_task, &args);
    
    return (err == MIMI_OK) ? 0 : 1;
}
