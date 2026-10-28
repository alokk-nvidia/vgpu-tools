/* SPDX-License-Identifier: MIT */
/* Shared userspace transport and scheduler response decoding for GET/SET. */
#ifndef FWCTL_PARAMS_H
#define FWCTL_PARAMS_H

#include <stddef.h>
#include <stdint.h>
#include "gmcapi_params.h"

#define FWCTL_SCHED_POLICY_COUNT 3U
#define FWCTL_SCHED_CAPS_NVKV_WORDS 10U
#define FWCTL_SCHED_STATE_NVKV_WORDS 5U

/* Every request carries the actual nonzero target PF or VF DBDF. */
struct fwctl_params_target {
        NvU64 dbdf;
};

struct fwctl_sched_caps {
        NvU32 policies[FWCTL_SCHED_POLICY_COUNT];
        /* Scalar capability values are indexed by NVGMC_PARAMS_SCHED_CAPS_*. */
        NvU32 values[NVGMC_PARAMS_SCHED_CAPS_MIN_AVG_FACTOR_FOR_ARR + 1];
};

struct fwctl_sched_state {
        NvU32 policy;
        NvU32 arr_enabled;
        NvU32 timeslice_ns;
        NvU32 avg_factor;
        NvU32 active_vgpu_count;
};

struct fwctl_sched_log_entry {
        NvU64 timestamp_ns;
        int64_t time_run_total_ns;
        NvU64 cumulative_preemption_ns;
        NvU64 cumulative_idle_ns;
        NvU32 time_run_ns;
        NvU32 swrl_id;
        NvU32 target_timeslice_ns;
        NvU32 weight;
        NvU64 counters[NVGMC_PARAMS_SCHED_LOG_COUNTER_COUNT];
};

struct fwctl_sched_log {
        NvU32 engine_id, count, policy, arr_enabled, avg_factor, timeslice_ns;
        struct fwctl_sched_log_entry entries[NVGMC_PARAMS_SCHED_LOG_MAX_ENTRIES];
};

/* Validate the selected BDF against the parameter descriptor's scope. */
int fwctl_params_check_target(NvU64 dbdf, NvU32 required_type,
                              const char *param_name);

int fwctl_params_rpc(int fd, NvU32 command_id,
                     const struct fwctl_params_target *target, NvU32 param_id,
                     const NvU64 *kv, NvU32 kv_count, void *out_buf,
                     size_t out_size, size_t *out_payload_size);
int fwctl_query_sched_caps(int fd, const struct fwctl_params_target *target,
                           struct fwctl_sched_caps *caps);
int fwctl_query_sched_state(int fd, const struct fwctl_params_target *target,
                            struct fwctl_sched_state *state);
/* Consumes up to 100 unread records, using the same buffer as nvidia-smi -sl. */
int fwctl_query_sched_log(int fd, const struct fwctl_params_target *target,
                          struct fwctl_sched_log *log);
const char *fwctl_sched_policy_name(NvU32 rm_policy);

#endif /* FWCTL_PARAMS_H */
