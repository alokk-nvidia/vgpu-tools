/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Read pGPU scheduler parameters over GMCAPI GET_PARAMS. Like production
 * NVML's pGPU scheduler queries, all parameter IDs select the GR0 engine.
 */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>

#include "fwctl_common.h"
#include "fwctl_params.h"

struct get_params_options {
        struct fwctl_params_target target;
        NvU32 loop_seconds;
        int once;
};

/* Emit output only after validating the entire firmware reply. */
static int get_sched_caps(int fd, const char *bdf,
                           const struct get_params_options *opts)
{
        struct fwctl_sched_caps caps;
        unsigned int i;

        if (fwctl_query_sched_caps(fd, &opts->target, &caps))
                return -1;
        printf("GPU %s\n", bdf);
        printf("vGPU scheduler capabilities\n");
        for (i = 0; i < FWCTL_SCHED_POLICY_COUNT; i++)
                printf("    %-32s: %s\n", i ? "" : "Supported Policies",
                       fwctl_sched_policy_name(caps.policies[i]));
        printf("    %-32s: %s\n", "ARR Mode",
               caps.values[NVGMC_PARAMS_SCHED_CAPS_IS_ARR_MODE_SUPPORTED] ?
               "Supported" : "Not Supported");
        printf("    Supported Timeslice Range\n");
        printf("        %-28s: %u ns\n", "Maximum Timeslice",
               caps.values[NVGMC_PARAMS_SCHED_CAPS_MAX_TIMESLICE_NS]);
        printf("        %-28s: %u ns\n", "Minimum Timeslice",
               caps.values[NVGMC_PARAMS_SCHED_CAPS_MIN_TIMESLICE_NS]);
        printf("    Supported Scheduling Frequency\n");
        printf("        %-28s: %u Hz\n", "Maximum Frequency",
               caps.values[NVGMC_PARAMS_SCHED_CAPS_MAX_FREQUENCY_FOR_ARR]);
        printf("        %-28s: %u Hz\n", "Minimum Frequency",
               caps.values[NVGMC_PARAMS_SCHED_CAPS_MIN_FREQUENCY_FOR_ARR]);
        printf("    Supported ARR Averaging Factor\n");
        printf("        %-28s: %u\n", "Maximum Avg Factor",
               caps.values[NVGMC_PARAMS_SCHED_CAPS_MAX_AVG_FACTOR_FOR_ARR]);
        printf("        %-28s: %u\n", "Minimum Avg Factor",
               caps.values[NVGMC_PARAMS_SCHED_CAPS_MIN_AVG_FACTOR_FOR_ARR]);
        return 0;
}

/* Match nvidia-smi's v2 display: no runtime fields for inactive/Best Effort. */
static int get_sched_state(int fd, const char *bdf,
                            const struct get_params_options *opts)
{
        struct fwctl_sched_state state;

        if (fwctl_query_sched_state(fd, &opts->target, &state))
                return -1;
        printf("GPU %s\n", bdf);
        printf("    %-32s: %u\n", "Active vGPUs", state.active_vgpu_count);
        printf("    %-32s: %s\n", "Scheduler Policy",
               fwctl_sched_policy_name(state.policy));
        if (!state.active_vgpu_count ||
            state.policy == NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_UNKNOWN ||
            state.policy == NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_BEST_EFFORT)
                return 0;
        printf("    %-32s: %u\n", "Average Factor", state.avg_factor);
        printf("    %-32s: %u\n", "Time Slice(ns)", state.timeslice_ns);
        return 0;
}

/* Check policy and active count separately, as the consuming log reply has no
 * ACTIVE_VGPU_COUNT. Still read the log when inactive, like nvidia-smi -sl;
 * retained records are consumed even though the inactive display omits rows.
 */
static int print_sched_log_read(int fd, const struct fwctl_params_target *target,
                                 const char *bdf, struct fwctl_sched_log *log)
{
        struct fwctl_sched_state state;
        unsigned int i;

        if (fwctl_query_sched_state(fd, target, &state))
                return -1;
        if (state.policy == NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_BEST_EFFORT ||
            state.policy == NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_UNKNOWN) {
                printf("GPU %s is operating with policy %s\n", bdf,
                       fwctl_sched_policy_name(state.policy));
                printf("Scheduler logging is not supported for this policy.\n");
                return 0;
        }
        if (fwctl_query_sched_log(fd, target, log))
                return -1;
        printf("GPU %s\n", bdf);
        printf("Engine Id          %u\n", log->engine_id);
        printf("Scheduler Policy   %s\n", fwctl_sched_policy_name(log->policy));
        if (!state.active_vgpu_count) {
                printf("GPU %s has no active VM runlist.\n", bdf);
                return 0;
        }
        printf("Avg Factor         %u\n", log->avg_factor);
        printf("Time Slice(ns)     %u\n", log->timeslice_ns);
        printf("%-16s %13s %10s %20s %23s %26s %21s %27s\n",
               "GPU", "SW Runlist Id", "Weight(0x)", "Timestamp(ns)",
               "Cumulative Run Time(ns)", "Prev Timeslice Runtime(ns)",
               "Target Time Slice(ns)", "Cumulative Preempt Time(ns)");
        for (i = 0; i < log->count; i++) {
                const struct fwctl_sched_log_entry *entry = &log->entries[i];

                printf("%-16s %13u %10.8x %20" PRIu64 " %23" PRId64
                       " %26u %21u %27" PRIu64 "\n", bdf, entry->swrl_id,
                       entry->weight, (uint64_t)entry->timestamp_ns,
                       (int64_t)entry->time_run_total_ns, entry->time_run_ns,
                       entry->target_timeslice_ns,
                       (uint64_t)entry->cumulative_preemption_ns);
        }
        return 0;
}

static volatile sig_atomic_t sched_log_stop;

static void stop_sched_log(int signo)
{
        (void)signo;
        sched_log_stop = 1;
}

/* Match -sl's 1,000-entry cycle with ten 100-entry GMCAPI reads and one-second polling.
 * Restore signal handlers so this command does not change its caller's state.
 */
static int get_sched_log(int fd, const char *bdf,
                          const struct get_params_options *opts)
{
        struct fwctl_sched_log *log;
        struct sigaction action = { .sa_handler = stop_sched_log };
        struct sigaction old_int, old_term;
        int ret = 0;

        log = calloc(1, sizeof(*log));
        if (!log) {
                perror("allocate scheduler log");
                return -1;
        }
        sched_log_stop = 0;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, &old_int) < 0) {
                perror("install scheduler log SIGINT handler");
                free(log);
                return -1;
        }
        if (sigaction(SIGTERM, &action, &old_term) < 0) {
                perror("install scheduler log SIGTERM handler");
                sigaction(SIGINT, &old_int, NULL);
                free(log);
                return -1;
        }
        do {
                struct timespec delay = { .tv_sec = opts->loop_seconds };
                unsigned int i;

                for (i = 0; i < 10 && !sched_log_stop; i++) {
                        if (print_sched_log_read(fd, &opts->target, bdf, log)) {
                                ret = sched_log_stop ? 0 : -1;
                                goto out;
                        }
                }
                fflush(stdout);
                if (opts->once || sched_log_stop)
                        break;
                while (!sched_log_stop && nanosleep(&delay, &delay) < 0) {
                        if (errno != EINTR) {
                                perror("scheduler log polling delay");
                                ret = -1;
                                goto out;
                        }
                }
        } while (!sched_log_stop);
out:
        sigaction(SIGTERM, &old_term, NULL);
        sigaction(SIGINT, &old_int, NULL);
        free(log);
        return ret;
}

struct fwctl_get_params_descriptor {
        const char *name;
        NvU32 target_type;
        int (*query_and_print)(int fd, const char *bdf,
                              const struct get_params_options *opts);
        int polling;
};

/* Each query helper selects its published shared pGPU parameter ID. */
static const struct fwctl_get_params_descriptor get_params_descriptors[] = {
        { "sched-caps", GMCAPI_PARAMS_TARGET_TYPE_PGPU, get_sched_caps, 0 },
        { "sched-state", GMCAPI_PARAMS_TARGET_TYPE_PGPU, get_sched_state, 0 },
        { "sched-log", GMCAPI_PARAMS_TARGET_TYPE_PGPU, get_sched_log, 1 },
};

static void usage_get_params(const char *prog)
{
        fprintf(stderr,
                "Usage: %s -b <pf-bdf> -P <sched-caps|sched-state|sched-log> [options]\n"
                "\n"
                "  -b, --bdf <bdf>     PF BDF selecting the pGPU/fwctl device\n"
                "  -P, --param <name>  sched-caps, sched-state, or sched-log\n"
                "  -l, --loop <sec>    sched-log polling interval, 1..10 (default 1)\n"
                "      --once          sched-log: one cycle of ten consuming reads\n"
                "  -h, --help          show this help\n"
                "\n"
                "Engine is always GR0; no scheduler-setting inputs are needed.\n"
                "Scheduler parameters require a PF BDF; the target is inferred from -b.\n"
                "Timeslices are nanoseconds; ARR frequency limits are Hz.\n",
                prog);
}

int cmd_get_params(const char *prog, int argc, char **argv)
{
        static const struct option long_options[] = {
                { "bdf", required_argument, NULL, 'b' },
                { "param", required_argument, NULL, 'P' },
                { "loop", required_argument, NULL, 'l' },
                { "once", no_argument, NULL, 256 },
                { "help", no_argument, NULL, 'h' },
                { NULL, 0, NULL, 0 },
        };
        const struct fwctl_get_params_descriptor *desc = NULL;
        const char *param_name = NULL;
        struct get_params_options opts = {
                .loop_seconds = 1,
        };
        NvU64 dbdf = 0;
        char bdf[32];
        size_t i;
        int opt, got_dbdf = 0, got_loop = 0, fd, ret;

        while ((opt = getopt_long(argc, argv, "b:P:l:h", long_options, NULL)) != -1) {
                switch (opt) {
                case 'b':
                        if (got_dbdf++)
                                goto duplicate;
                        if (parse_dbdf(optarg, &dbdf)) {
                                fprintf(stderr, "error: invalid -b value '%s'\n", optarg);
                                return EXIT_FAILURE;
                        }
                        break;
                case 'P':
                        if (param_name)
                                goto duplicate;
                        param_name = optarg;
                        break;
                case 'l':
                        if (got_loop++)
                                goto duplicate;
                        if (parse_u32("--loop", optarg, &opts.loop_seconds))
                                return EXIT_FAILURE;
                        if (opts.loop_seconds < 1 || opts.loop_seconds > 10) {
                                fprintf(stderr, "error: --loop must be between 1 and 10 seconds\n");
                                return EXIT_FAILURE;
                        }
                        break;
                case 256:
                        if (opts.once++)
                                goto duplicate;
                        break;
                case 'h':
                        usage_get_params(prog);
                        return EXIT_SUCCESS;
                default:
                        usage_get_params(prog);
                        return EXIT_FAILURE;
                }
        }
        if (reject_extra_args(prog, argc, argv))
                return EXIT_FAILURE;
        if (!got_dbdf || !param_name) {
                fprintf(stderr, "error: -b <pf-bdf> and -P <param> are required\n\n");
                usage_get_params(prog);
                return EXIT_FAILURE;
        }
        for (i = 0; i < sizeof(get_params_descriptors) /
                        sizeof(get_params_descriptors[0]); i++)
                if (!strcmp(param_name, get_params_descriptors[i].name)) {
                        desc = &get_params_descriptors[i];
                        break;
                }
        if (!desc) {
                fprintf(stderr, "error: unsupported GET_PARAMS parameter '%s'\n", param_name);
                return EXIT_FAILURE;
        }
        opts.target.dbdf = dbdf;
        if (!desc->polling && (got_loop || opts.once)) {
                fprintf(stderr, "error: --loop and --once require -P sched-log\n");
                return EXIT_FAILURE;
        }
        if (fwctl_params_check_target(dbdf, desc->target_type, desc->name) < 0 ||
            check_nvidia_vendor_dbdf(dbdf) < 0)
                return EXIT_FAILURE;
        fd = open_nova_core_fwctl_for_dbdf(dbdf);
        if (fd < 0) {
                fprintf(stderr, "no nova-core fwctl device found\n");
                return EXIT_FAILURE;
        }
        format_dbdf(dbdf, bdf, sizeof(bdf));
        for (char *p = bdf; *p; p++)
                *p = (char)toupper((unsigned char)*p);
        ret = desc->query_and_print(fd, bdf, &opts);
        close(fd);
        return ret ? EXIT_FAILURE : EXIT_SUCCESS;
duplicate:
        fprintf(stderr, "error: duplicate GET_PARAMS option '%s'\n", argv[optind - 1]);
        return EXIT_FAILURE;
}
