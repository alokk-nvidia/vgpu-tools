/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * pGPU SET_PARAMS dispatch. SCHED_STATE follows production NVML's non-MIG
 * interface: GR0, active-vGPU preflight, policy translation, capability-based
 * ARR validation, and status-only SET completion.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/ioctl.h>

#include "fwctl.h"
#include "fwctl_common.h"
#include "fwctl_params.h"
#include "gmcapi_params.h"
#include "nvkv.h"

#define SET_PARAMS_MAX_NVKV_WORDS 4U

enum set_params_sched_policy {
        SET_PARAMS_SCHED_POLICY_BEST_EFFORT = 1,
        SET_PARAMS_SCHED_POLICY_EQUAL_SHARE = 2,
        SET_PARAMS_SCHED_POLICY_FIXED_SHARE = 3,
};

struct set_params_cli_options {
        struct fwctl_params_target target;
        NvU32 sched_policy;
        NvU32 frequency_for_arr;
        NvU32 avg_factor_for_arr;
        int got_sched_policy;
        int got_frequency_for_arr;
        int got_avg_factor_for_arr;
};

typedef int (*set_params_encode_fn)(const struct set_params_cli_options *opts,
                                   NvU64 *kv, NvU32 kv_limit, NvU32 *kv_count);
typedef int (*set_params_validate_fn)(int fd,
                                     const struct set_params_cli_options *opts);

struct fwctl_set_params_descriptor {
        const char *name;
        NvU32 target_type;
        NvU32 param_id;
        set_params_encode_fn encode;
        set_params_validate_fn validate;
};

/* CLI/NVML policies and SDK wire policies are different enumerations. */
static int sched_policy_to_rm(NvU32 policy, NvU32 *rm_policy)
{
        switch (policy) {
        case SET_PARAMS_SCHED_POLICY_BEST_EFFORT:
                *rm_policy = NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_BEST_EFFORT;
                return 0;
        case SET_PARAMS_SCHED_POLICY_EQUAL_SHARE:
                *rm_policy = NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_EQUAL_SHARE;
                return 0;
        case SET_PARAMS_SCHED_POLICY_FIXED_SHARE:
                *rm_policy = NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_FIXED_SHARE;
                return 0;
        default:
                fprintf(stderr,
                        "error: --policy must be 1 (Best Effort), "
                        "2 (Equal Share), or 3 (Fixed Share); got %u\n", policy);
                return -1;
        }
}

/* Check the required policy and encode locally; ARR compatibility is checked
 * after the active-vGPU preflight.
 */
static int encode_sched_state(const struct set_params_cli_options *opts,
                              NvU64 *kv, NvU32 kv_limit, NvU32 *kv_count)
{
        NVKVContext ctx;
        NvU64 count;
        NvU32 rm_policy;

        if (!opts->got_sched_policy) {
                fprintf(stderr, "error: sched-state requires --policy\n");
                return -1;
        }
        if (sched_policy_to_rm(opts->sched_policy, &rm_policy))
                return -1;
        ctx = NVKV_BEGIN(kv, 0, kv_limit);
        nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_STATE_ENGINE_ID,
                     NV2080_ENGINE_TYPE_GRAPHICS);
        nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_STATE_SCHED_POLICY, rm_policy);
        if (opts->got_frequency_for_arr)
                nvkvSetImm32(&ctx, 0,
                             NVGMC_PARAMS_SCHED_STATE_FREQUENCY_FOR_ARR,
                             opts->frequency_for_arr);
        if (opts->got_avg_factor_for_arr)
                nvkvSetImm32(&ctx, 0,
                             NVGMC_PARAMS_SCHED_STATE_AVG_FACTOR_FOR_ARR,
                             opts->avg_factor_for_arr);
        count = NVKV_END(&ctx);
        if (count > kv_limit) {
                fprintf(stderr, "error: sched-state NVKV payload overflow\n");
                return -1;
        }
        *kv_count = (NvU32)count;
        return 0;
}

/* Match nvidia-smi's active-vGPU preflight, then NVML's CAPS validation. */
static int validate_sched_state(int fd, const struct set_params_cli_options *opts)
{
        struct fwctl_sched_state state;
        struct fwctl_sched_caps caps;
        NvU32 rm_policy, min_freq, max_freq, min_avg, max_avg;
        unsigned int i;

        if (fwctl_query_sched_state(fd, &opts->target, &state))
                return -1;
        if (state.active_vgpu_count != 0) {
                fprintf(stderr,
                        "error: vGPU scheduler state cannot be configured while "
                        "vGPU instance%s %s active on this GPU [vGPU count = %u]; "
                        "stop all vGPU instances and retry\n",
                        state.active_vgpu_count == 1 ? "" : "s",
                        state.active_vgpu_count == 1 ? "is" : "are",
                        state.active_vgpu_count);
                return -1;
        }
        if ((opts->got_frequency_for_arr || opts->got_avg_factor_for_arr) &&
            opts->sched_policy == SET_PARAMS_SCHED_POLICY_BEST_EFFORT) {
                fprintf(stderr,
                        "error: -asf/--arr-frequency and -aavg/--arr-avg-factor "
                        "require Equal Share (-p 2) or Fixed Share (-p 3); "
                        "Best Effort (-p 1) does not accept ARR options\n");
                return -1;
        }
        /* GSP-RM still checks permissions in case a vGPU starts after GET. */
        if (fwctl_query_sched_caps(fd, &opts->target, &caps))
                return -1;
        if (sched_policy_to_rm(opts->sched_policy, &rm_policy))
                return -1;
        for (i = 0; i < FWCTL_SCHED_POLICY_COUNT; i++)
                if (caps.policies[i] == rm_policy)
                        break;
        if (i == FWCTL_SCHED_POLICY_COUNT) {
                fprintf(stderr, "error: policy %u is not supported by this GPU\n",
                        opts->sched_policy);
                return -1;
        }
        if ((opts->got_frequency_for_arr || opts->got_avg_factor_for_arr) &&
            !caps.values[NVGMC_PARAMS_SCHED_CAPS_IS_ARR_MODE_SUPPORTED]) {
                fprintf(stderr, "error: this GPU does not support ARR options\n");
                return -1;
        }
        min_freq = caps.values[NVGMC_PARAMS_SCHED_CAPS_MIN_FREQUENCY_FOR_ARR];
        max_freq = caps.values[NVGMC_PARAMS_SCHED_CAPS_MAX_FREQUENCY_FOR_ARR];
        min_avg = caps.values[NVGMC_PARAMS_SCHED_CAPS_MIN_AVG_FACTOR_FOR_ARR];
        max_avg = caps.values[NVGMC_PARAMS_SCHED_CAPS_MAX_AVG_FACTOR_FOR_ARR];
        if (opts->frequency_for_arr != 0 &&
            (opts->frequency_for_arr < min_freq ||
             opts->frequency_for_arr > max_freq)) {
                fprintf(stderr,
                        "error: ARR frequency must be 0 (default) or "
                        "%u..%u Hz; got %u\n", min_freq, max_freq,
                        opts->frequency_for_arr);
                return -1;
        }
        if (opts->avg_factor_for_arr != 0 &&
            (opts->avg_factor_for_arr < min_avg ||
             opts->avg_factor_for_arr > max_avg)) {
                fprintf(stderr,
                        "error: ARR averaging factor must be 0 (default) or "
                        "%u..%u; got %u\n", min_avg, max_avg,
                        opts->avg_factor_for_arr);
                return -1;
        }
        return 0;
}

/* Register future pGPU encoders and optional parameter preflights here. */
static const struct fwctl_set_params_descriptor g_fwctlSetParamsDescriptors[] = {
        {
                "sched-state",
                GMCAPI_PARAMS_TARGET_TYPE_PGPU,
                GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_STATE,
                encode_sched_state,
                validate_sched_state,
        },
};

static const struct fwctl_set_params_descriptor *
find_set_params_descriptor(const char *name)
{
        size_t i;

        for (i = 0; i < sizeof(g_fwctlSetParamsDescriptors) /
                        sizeof(g_fwctlSetParamsDescriptors[0]); i++)
                if (!strcmp(name, g_fwctlSetParamsDescriptors[i].name))
                        return &g_fwctlSetParamsDescriptors[i];
        return NULL;
}

static void usage_set_params(const char *prog)
{
        fprintf(stderr,
                "Usage: %s -b <pf-bdf> -P sched-state -p <1|2|3> [options]\n"
                "\n"
                "  -b, --bdf <bdf>             PF BDF selecting the pGPU/fwctl device\n"
                "  -P, --param <name>          parameter to set (sched-state)\n"
                "  -p, --policy <1|2|3>        1=Best Effort, 2=Equal Share, 3=Fixed Share\n"
                "  -asf, --arr-frequency <Hz>  ARR frequency; 0/omitted selects defaults\n"
                "  -aavg, --arr-avg-factor <v> ARR average; 0/omitted selects defaults\n"
                "                              ARR options require -p 2 or -p 3;\n"
                "                              nonzero limits come from GPU SCHED_CAPS\n"
                "  -h, --help                  show this help\n"
                "\n"
                "Engine is always GR0. SET returns status only, not effective settings.\n"
                "Scheduler parameters require a PF BDF; the target is inferred from -b.\n",
                prog);
}

int cmd_set_params(const char *prog, int argc, char **argv)
{
        static const struct option long_options[] = {
                { "bdf", required_argument, NULL, 'b' },
                { "param", required_argument, NULL, 'P' },
                { "policy", required_argument, NULL, 'p' },
                { "arr-frequency", required_argument, NULL, 'f' },
                { "asf", required_argument, NULL, 'f' },
                { "arr-avg-factor", required_argument, NULL, 'a' },
                { "aavg", required_argument, NULL, 'a' },
                { "help", no_argument, NULL, 'h' },
                { NULL, 0, NULL, 0 },
        };
        struct set_params_cli_options opts = {0};
        const struct fwctl_set_params_descriptor *desc;
        _Alignas(16) struct fwctl_rpc_nova_core response = {0};
        NvU64 kv[SET_PARAMS_MAX_NVKV_WORDS] = {0};
        NvU64 dbdf = 0;
        NvU32 kv_count = 0;
        size_t payload_size;
        const char *param_name = NULL;
        int got_dbdf = 0, fd = -1, ret = EXIT_FAILURE, opt;

        while ((opt = getopt_long_only(argc, argv, "b:P:p:h",
                                       long_options, NULL)) != -1) {
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
                case 'p':
                        if (opts.got_sched_policy++)
                                goto duplicate;
                        if (parse_u32("--policy", optarg, &opts.sched_policy))
                                return EXIT_FAILURE;
                        break;
                case 'f':
                        if (opts.got_frequency_for_arr++)
                                goto duplicate;
                        if (parse_u32("--arr-frequency", optarg,
                                      &opts.frequency_for_arr))
                                return EXIT_FAILURE;
                        break;
                case 'a':
                        if (opts.got_avg_factor_for_arr++)
                                goto duplicate;
                        if (parse_u32("--arr-avg-factor", optarg,
                                      &opts.avg_factor_for_arr))
                                return EXIT_FAILURE;
                        break;
                case 'h':
                        usage_set_params(prog);
                        return EXIT_SUCCESS;
                default:
                        usage_set_params(prog);
                        return EXIT_FAILURE;
                }
        }
        if (reject_extra_args(prog, argc, argv))
                return EXIT_FAILURE;
        if (!got_dbdf || !param_name) {
                fprintf(stderr, "error: -b <pf-bdf> and -P <param> are required\n\n");
                usage_set_params(prog);
                return EXIT_FAILURE;
        }
        desc = find_set_params_descriptor(param_name);
        if (!desc) {
                fprintf(stderr, "error: unsupported SET_PARAMS parameter '%s'\n",
                        param_name);
                return EXIT_FAILURE;
        }
        opts.target.dbdf = dbdf;
        if (desc->encode(&opts, kv, SET_PARAMS_MAX_NVKV_WORDS, &kv_count))
                return EXIT_FAILURE;
        if (fwctl_params_check_target(dbdf, desc->target_type, desc->name) < 0 ||
            check_nvidia_vendor_dbdf(dbdf) < 0)
                return EXIT_FAILURE;
        fd = open_nova_core_fwctl_for_dbdf(dbdf);
        if (fd < 0) {
                fprintf(stderr, "no nova-core fwctl device found\n");
                return EXIT_FAILURE;
        }
        if (desc->validate && desc->validate(fd, &opts))
                goto out;
        if (fwctl_params_rpc(fd, FWCTL_CMD_NOVA_CORE_GMCAPI_SET_PARAMS,
                       &opts.target, desc->param_id, kv, kv_count, &response,
                       sizeof(response), &payload_size))
                goto out;
        if (payload_size != 0) {
                fprintf(stderr, "error: unexpected SET_PARAMS response payload\n");
                goto out;
        }
        fprintf(stdout, "SET_PARAMS %s: OK\n", desc->name);
        ret = EXIT_SUCCESS;
out:
        if (fd >= 0)
                close(fd);
        return ret;
duplicate:
        fprintf(stderr, "error: duplicate SET_PARAMS option '%s'\n", argv[optind - 1]);
        return EXIT_FAILURE;
}
