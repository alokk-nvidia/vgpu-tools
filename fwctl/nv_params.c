/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Common pGPU/vGPU parameter transport and strict scheduler response decoders.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "fwctl.h"
#include "fwctl_common.h"
#include "fwctl_params.h"
#include "nvkv.h"

#define PARAMS_MAX_INPUT_WORDS 4U
#define CAPS_FIRST_KEY NVGMC_PARAMS_SCHED_CAPS_SUPPORTED_SCHEDULERS
#define CAPS_LAST_KEY NVGMC_PARAMS_SCHED_CAPS_MIN_AVG_FACTOR_FOR_ARR
#define CAPS_REQUIRED_MASK ((1U << (CAPS_LAST_KEY + 1)) - (1U << CAPS_FIRST_KEY))
#define STATE_REQUIRED_MASK ((1U << NVGMC_PARAMS_SCHED_STATE_SCHED_POLICY) | \
        (1U << NVGMC_PARAMS_SCHED_STATE_ARR_ENABLED) | \
        (1U << NVGMC_PARAMS_SCHED_STATE_TARGET_TIMESLICE_NS) | \
        (1U << NVGMC_PARAMS_SCHED_STATE_AVG_FACTOR_FOR_ARR) | \
        (1U << NVGMC_PARAMS_SCHED_STATE_ACTIVE_VGPU_COUNT))

/* Packed DBDF reserves bits 31:16; zero is never a target shorthand. */
static int params_dbdf_valid(NvU64 dbdf)
{
        return dbdf != 0 && (dbdf & UINT64_C(0xffff0000)) == 0;
}

/* The CLI selects a BDF; descriptors restrict its PF/VF scope. GSP performs
 * the authoritative ownership check against the selected FWCTL endpoint.
 */
int fwctl_params_check_target(NvU64 dbdf, NvU32 required_type,
                              const char *param_name)
{
        if (!params_dbdf_valid(dbdf)) {
                fprintf(stderr, "error: parameter '%s' requires a valid nonzero PF or VF BDF\n",
                        param_name);
                return -1;
        }
        switch (required_type) {
        case GMCAPI_PARAMS_TARGET_TYPE_PGPU:
                return check_is_pf_dbdf(dbdf);
        case GMCAPI_PARAMS_TARGET_TYPE_VGPU:
                return check_is_vf_dbdf(dbdf);
        default:
                fprintf(stderr, "error: unsupported target scope for parameter '%s'\n",
                        param_name);
                return -1;
        }
}

/* Encode the caller's actual DBDF; GSP resolves the target type.
 * Parameter/target compatibility belongs to descriptors and GSP validation.
 * Never accept an incomplete or mismatched reply.
 */
int fwctl_params_rpc(int fd, NvU32 command_id,
                     const struct fwctl_params_target *target, NvU32 param_id,
                     const NvU64 *kv, NvU32 kv_count, void *out_buf,
                     size_t out_size, size_t *out_payload_size)
{
        struct fwctl_rpc rpc = {0};
        struct fwctl_rpc_nova_core *req_hdr, *resp_hdr = out_buf;
        struct GmcapiParamsInParams *params;
        size_t in_size;
        void *in_buf = NULL;
        int err, ret = -1;

        if (!target || !params_dbdf_valid(target->dbdf) ||
            !kv || !kv_count || kv_count > PARAMS_MAX_INPUT_WORDS ||
            !out_buf || out_size < sizeof(*resp_hdr) || !out_payload_size)
                return -1;
        *out_payload_size = 0;
        in_size = sizeof(*req_hdr) +
                  offsetof(struct GmcapiParamsInParams, nvkvPayload) +
                  (size_t)kv_count * sizeof(NvU64);
        err = posix_memalign(&in_buf, 16, in_size);
        if (err) {
                fprintf(stderr, "posix_memalign(in) failed: %s\n", strerror(err));
                return -1;
        }
        memset(in_buf, 0, in_size);
        req_hdr = in_buf;
        req_hdr->command_id = command_id;
        params = (void *)((uint8_t *)in_buf + sizeof(*req_hdr));
        params->dbdf = target->dbdf;
        params->paramId = param_id;
        memcpy(params->nvkvPayload, kv, (size_t)kv_count * sizeof(NvU64));

        rpc.size = sizeof(rpc);
        rpc.scope = FWCTL_RPC_CONFIGURATION;
        rpc.in_len = in_size;
        rpc.out_len = out_size;
        rpc.in = (uint64_t)(uintptr_t)in_buf;
        rpc.out = (uint64_t)(uintptr_t)out_buf;
        if (ioctl(fd, FWCTL_RPC, &rpc)) {
                fprintf(stderr, "FWCTL command 0x%08x failed: %s\n",
                        command_id, strerror(errno));
                goto out;
        }
        if (rpc.out_len < sizeof(*resp_hdr) || rpc.out_len > out_size ||
            resp_hdr->command_id != command_id || resp_hdr->reserved != 0) {
                fprintf(stderr, "error: invalid FWCTL parameter response\n");
                goto out;
        }
        *out_payload_size = rpc.out_len - sizeof(*resp_hdr);
        ret = 0;
out:
        free(in_buf);
        return ret;
}

/* Match the RM-to-NVML names, including the explicitly defined Unknown policy. */
const char *fwctl_sched_policy_name(NvU32 rm_policy)
{
        switch (rm_policy) {
        case NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_UNKNOWN: return "Unknown";
        case NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_BEST_EFFORT: return "Best Effort";
        case NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_EQUAL_SHARE: return "Equal Share";
        case NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_FIXED_SHARE: return "Fixed Share";
        default: return NULL;
        }
}

struct caps_decode {
        struct fwctl_sched_caps *caps;
        NvU32 present;
        int invalid;
};

/* Validate the complete CAPS schema before printing or using its limits. */
static NvBool sched_caps_key(NvU64 index, NvU64 key,
                             const NVKVValue *value, void *data)
{
        struct caps_decode *decode = data;
        struct fwctl_sched_caps *caps = decode->caps;

        if (index != 0 || key < CAPS_FIRST_KEY || key > CAPS_LAST_KEY ||
            (decode->present & (1U << key)) ||
            value->valueType != NVKV_VALUE_TYPE_U32)
                goto invalid;
        if (key == NVGMC_PARAMS_SCHED_CAPS_SUPPORTED_SCHEDULERS) {
                if (value->valueCount != FWCTL_SCHED_POLICY_COUNT)
                        goto invalid;
                memcpy(caps->policies, value->valueData.pU32,
                       sizeof(caps->policies));
        } else {
                if (value->valueCount != 1)
                        goto invalid;
                caps->values[key] = value->valueData.pU32[0];
        }
        decode->present |= 1U << key;
        return NV_TRUE;
invalid:
        decode->invalid = 1;
        return NV_FALSE;
}

/* Scheduler queries use GR0 and no scheduler-setting input fields. */
static int query_sched(int fd, const struct fwctl_params_target *target,
                        NvU32 param_id, void *response,
                        size_t response_size, size_t *payload_size)
{
        NvU64 query;
        NVKVContext ctx = NVKV_BEGIN(&query, 0, 1);

        nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_STATE_ENGINE_ID,
                     NV2080_ENGINE_TYPE_GRAPHICS);
        (void)NVKV_END(&ctx);
        return fwctl_params_rpc(fd, FWCTL_CMD_NOVA_CORE_GMCAPI_GET_PARAMS,
                                target, param_id, &query, 1, response,
                                response_size, payload_size);
}

int fwctl_query_sched_caps(int fd, const struct fwctl_params_target *target,
                           struct fwctl_sched_caps *caps)
{
        _Alignas(16) struct {
                struct fwctl_rpc_nova_core header;
                NvU64 kv[FWCTL_SCHED_CAPS_NVKV_WORDS];
        } response = {0};
        struct caps_decode decode = { .caps = caps };
        size_t payload_size;
        unsigned int i;

        memset(caps, 0, sizeof(*caps));
        if (query_sched(fd, target, GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_CAPS,
                        &response, sizeof(response), &payload_size))
                return -1;
        if (payload_size % sizeof(NvU64) ||
            nvkvDecode(sched_caps_key, response.kv,
                       payload_size / sizeof(NvU64), &decode) != NV_OK ||
            decode.invalid || decode.present != CAPS_REQUIRED_MASK ||
            caps->values[NVGMC_PARAMS_SCHED_CAPS_IS_ARR_MODE_SUPPORTED] > 1 ||
            caps->values[NVGMC_PARAMS_SCHED_CAPS_MIN_TIMESLICE_NS] >
                caps->values[NVGMC_PARAMS_SCHED_CAPS_MAX_TIMESLICE_NS] ||
            caps->values[NVGMC_PARAMS_SCHED_CAPS_MIN_FREQUENCY_FOR_ARR] >
                caps->values[NVGMC_PARAMS_SCHED_CAPS_MAX_FREQUENCY_FOR_ARR] ||
            caps->values[NVGMC_PARAMS_SCHED_CAPS_MIN_AVG_FACTOR_FOR_ARR] >
                caps->values[NVGMC_PARAMS_SCHED_CAPS_MAX_AVG_FACTOR_FOR_ARR])
                goto malformed;
        for (i = 0; i < FWCTL_SCHED_POLICY_COUNT; i++)
                if (!fwctl_sched_policy_name(caps->policies[i]))
                        goto malformed;
        return 0;
malformed:
        fprintf(stderr, "error: malformed SCHED_CAPS response\n");
        return -1;
}

struct state_decode {
        NvU32 values[NVGMC_PARAMS_SCHED_STATE_ACTIVE_VGPU_COUNT + 1];
        NvU32 present;
        int invalid;
};

/* Five response-only scalar U32 keys, with no duplicates or nonzero indexes. */
static NvBool sched_state_key(NvU64 index, NvU64 key,
                              const NVKVValue *value, void *data)
{
        struct state_decode *decode = data;

        if (index != 0 || key > NVGMC_PARAMS_SCHED_STATE_ACTIVE_VGPU_COUNT ||
            !(STATE_REQUIRED_MASK & (1U << key)) ||
            (decode->present & (1U << key)) ||
            value->valueType != NVKV_VALUE_TYPE_U32 || value->valueCount != 1) {
                decode->invalid = 1;
                return NV_FALSE;
        }
        decode->values[key] = value->valueData.pU32[0];
        decode->present |= 1U << key;
        return NV_TRUE;
}

int fwctl_query_sched_state(int fd, const struct fwctl_params_target *target,
                            struct fwctl_sched_state *state)
{
        _Alignas(16) struct {
                struct fwctl_rpc_nova_core header;
                NvU64 kv[FWCTL_SCHED_STATE_NVKV_WORDS];
        } response = {0};
        struct state_decode decode = {0};
        size_t payload_size;

        memset(state, 0, sizeof(*state));
        if (query_sched(fd, target, GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_STATE,
                        &response, sizeof(response), &payload_size))
                return -1;
        if (payload_size % sizeof(NvU64) ||
            nvkvDecode(sched_state_key, response.kv,
                       payload_size / sizeof(NvU64), &decode) != NV_OK ||
            decode.invalid || decode.present != STATE_REQUIRED_MASK ||
            !fwctl_sched_policy_name(decode.values[NVGMC_PARAMS_SCHED_STATE_SCHED_POLICY]) ||
            decode.values[NVGMC_PARAMS_SCHED_STATE_ARR_ENABLED] >
                NV2080_CTRL_CMD_VGPU_SCHEDULER_ARR_ENABLE) {
                fprintf(stderr, "error: malformed SCHED_STATE response\n");
                return -1;
        }
        state->policy = decode.values[NVGMC_PARAMS_SCHED_STATE_SCHED_POLICY];
        state->arr_enabled = decode.values[NVGMC_PARAMS_SCHED_STATE_ARR_ENABLED];
        state->timeslice_ns = decode.values[NVGMC_PARAMS_SCHED_STATE_TARGET_TIMESLICE_NS];
        state->avg_factor = decode.values[NVGMC_PARAMS_SCHED_STATE_AVG_FACTOR_FOR_ARR];
        state->active_vgpu_count = decode.values[NVGMC_PARAMS_SCHED_STATE_ACTIVE_VGPU_COUNT];
        return 0;
}

#define LOG_META_MASK ((1U << NVGMC_PARAMS_SCHED_LOG_HEADER_KV_COUNT) - 1U)
#define LOG_ENTRY_MASK ((1U << 9) - 1U)

struct log_decode {
        struct fwctl_sched_log *log;
        NvU32 metadata[NVGMC_PARAMS_SCHED_LOG_HEADER_KV_COUNT];
        NvU32 metadata_present;
        NvU32 entry_present[NVGMC_PARAMS_SCHED_LOG_MAX_ENTRIES];
        int invalid;
};

/* Counts and entries may arrive in any key order. Bound every index before
 * access, then verify complete, consecutive entries against COUNT at the end.
 */
static NvBool sched_log_key(NvU64 index, NvU64 key,
                            const NVKVValue *value, void *data)
{
        struct log_decode *decode = data;
        struct fwctl_sched_log_entry *entry;
        NvU32 bit;

        if (key >= NVGMC_PARAMS_SCHED_LOG_ENGINE_ID &&
            key <= NVGMC_PARAMS_SCHED_LOG_TARGET_TIMESLICE_NS) {
                bit = 1U << (key - NVGMC_PARAMS_SCHED_LOG_ENGINE_ID);
                if (index != 0 || (decode->metadata_present & bit) ||
                    value->valueType != NVKV_VALUE_TYPE_U32 || value->valueCount != 1)
                        goto invalid;
                decode->metadata[key - NVGMC_PARAMS_SCHED_LOG_ENGINE_ID] =
                        value->valueData.pU32[0];
                decode->metadata_present |= bit;
                return NV_TRUE;
        }
        if (index >= NVGMC_PARAMS_SCHED_LOG_MAX_ENTRIES ||
            key < NVGMC_PARAMS_SCHED_LOG_TIMESTAMP_NS ||
            key > NVGMC_PARAMS_SCHED_LOG_COUNTERS)
                goto invalid;
        bit = 1U << (key - NVGMC_PARAMS_SCHED_LOG_TIMESTAMP_NS);
        if (decode->entry_present[index] & bit)
                goto invalid;
        entry = &decode->log->entries[index];
        if (key == NVGMC_PARAMS_SCHED_LOG_COUNTERS) {
                if (value->valueType != NVKV_VALUE_TYPE_U64 ||
                    value->valueCount != NVGMC_PARAMS_SCHED_LOG_COUNTER_COUNT)
                        goto invalid;
                memcpy(entry->counters, value->valueData.pU64, sizeof(entry->counters));
        } else if (key <= NVGMC_PARAMS_SCHED_LOG_CUMULATIVE_IDLE_TIME_NS) {
                if (value->valueType != NVKV_VALUE_TYPE_U64 || value->valueCount != 1)
                        goto invalid;
                switch (key) {
                case NVGMC_PARAMS_SCHED_LOG_TIMESTAMP_NS:
                        entry->timestamp_ns = value->valueData.pU64[0]; break;
                case NVGMC_PARAMS_SCHED_LOG_TIME_RUN_TOTAL_NS:
                        memcpy(&entry->time_run_total_ns, value->valueData.pU64,
                               sizeof(entry->time_run_total_ns)); break;
                case NVGMC_PARAMS_SCHED_LOG_CUMULATIVE_PREEMPTION_TIME_NS:
                        entry->cumulative_preemption_ns = value->valueData.pU64[0]; break;
                case NVGMC_PARAMS_SCHED_LOG_CUMULATIVE_IDLE_TIME_NS:
                        entry->cumulative_idle_ns = value->valueData.pU64[0]; break;
                }
        } else {
                if (value->valueType != NVKV_VALUE_TYPE_U32 || value->valueCount != 1)
                        goto invalid;
                switch (key) {
                case NVGMC_PARAMS_SCHED_LOG_TIME_RUN_NS:
                        entry->time_run_ns = value->valueData.pU32[0]; break;
                case NVGMC_PARAMS_SCHED_LOG_SWRL_ID:
                        entry->swrl_id = value->valueData.pU32[0]; break;
                case NVGMC_PARAMS_SCHED_LOG_TARGET_TIME_SLICE_NS:
                        entry->target_timeslice_ns = value->valueData.pU32[0]; break;
                case NVGMC_PARAMS_SCHED_LOG_WEIGHT:
                        entry->weight = value->valueData.pU32[0]; break;
                }
        }
        decode->entry_present[index] |= bit;
        return NV_TRUE;
invalid:
        decode->invalid = 1;
        return NV_FALSE;
}

int fwctl_query_sched_log(int fd, const struct fwctl_params_target *target,
                          struct fwctl_sched_log *log)
{
        const size_t response_size = sizeof(struct fwctl_rpc_nova_core) +
                                     NVGMC_PARAMS_SCHED_LOG_MAX_RESPONSE_SIZE;
        struct log_decode decode = { .log = log };
        void *response = NULL;
        size_t payload_size;
        unsigned int i;
        int err, ret = -1;

        memset(log, 0, sizeof(*log));
        err = posix_memalign(&response, 16, response_size);
        if (err) {
                fprintf(stderr, "posix_memalign(log response) failed: %s\n", strerror(err));
                return -1;
        }
        memset(response, 0, response_size);
        if (query_sched(fd, target, GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_LOG,
                        response, response_size, &payload_size))
                goto out;
        if (payload_size % sizeof(NvU64) ||
            nvkvDecode(sched_log_key,
                       (const void *)((struct fwctl_rpc_nova_core *)response + 1),
                       payload_size / sizeof(NvU64), &decode) != NV_OK ||
            decode.invalid || decode.metadata_present != LOG_META_MASK)
                goto malformed;
        log->engine_id = decode.metadata[0];
        log->count = decode.metadata[1];
        log->policy = decode.metadata[2];
        log->arr_enabled = decode.metadata[3];
        log->avg_factor = decode.metadata[4];
        log->timeslice_ns = decode.metadata[5];
        if (log->engine_id != NV2080_ENGINE_TYPE_GRAPHICS ||
            log->count > NVGMC_PARAMS_SCHED_LOG_MAX_ENTRIES ||
            (log->policy != NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_EQUAL_SHARE &&
             log->policy != NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_FIXED_SHARE) ||
            log->arr_enabled > NV2080_CTRL_CMD_VGPU_SCHEDULER_ARR_ENABLE)
                goto malformed;
        for (i = 0; i < NVGMC_PARAMS_SCHED_LOG_MAX_ENTRIES; i++)
                if (decode.entry_present[i] != (i < log->count ? LOG_ENTRY_MASK : 0U))
                        goto malformed;
        ret = 0;
        goto out;
malformed:
        fprintf(stderr, "error: malformed SCHED_LOG response\n");
out:
        free(response);
        return ret;
}
