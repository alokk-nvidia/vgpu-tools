/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Userspace subset of GSP-RM's common GET_PARAMS/SET_PARAMS wire contract.
 */

#ifndef GMCAPI_PARAMS_H
#define GMCAPI_PARAMS_H

#include <stddef.h>
#include "nvrm.h"

/* Internal descriptor scope; GSP derives it from the target DBDF. */
enum gmcapi_params_target_type {
        GMCAPI_PARAMS_TARGET_TYPE_INVALID = 0,
        GMCAPI_PARAMS_TARGET_TYPE_PGPU = 1,
        GMCAPI_PARAMS_TARGET_TYPE_VGPU = 2,
};

/* IDs identify a parameter within its target, independent of GET or SET.
 * Each command's descriptor table determines which operations are supported.
 */
enum gmcapi_pgpu_params_param_id {
        GMCAPI_PGPU_PARAMS_PARAM_ID_INVALID = 0,
        GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_CAPS = 1,
        GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_STATE = 2,
        GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_LOG = 3,
};

/* No vGPU parameters are implemented yet. */
enum gmcapi_vgpu_params_param_id {
        GMCAPI_VGPU_PARAMS_PARAM_ID_INVALID = 0,
};

/* Shared SET/GET keys; values are U32 scalars at NVKV index zero. */
#define NVGMC_PARAMS_SCHED_STATE_ENGINE_ID            0x0001
#define NVGMC_PARAMS_SCHED_STATE_SCHED_POLICY         0x0002
#define NVGMC_PARAMS_SCHED_STATE_FREQUENCY_FOR_ARR    0x0003
#define NVGMC_PARAMS_SCHED_STATE_AVG_FACTOR_FOR_ARR   0x0004
#define NVGMC_PARAMS_SCHED_STATE_ARR_ENABLED          0x0005
#define NVGMC_PARAMS_SCHED_STATE_TARGET_TIMESLICE_NS  0x0006
#define NVGMC_PARAMS_SCHED_STATE_ACTIVE_VGPU_COUNT    0x0007

/* GET SCHED_CAPS is used to validate SET input, like production NVML. */
#define NVGMC_PARAMS_SCHED_CAPS_ENGINE_ID              NVGMC_PARAMS_SCHED_STATE_ENGINE_ID
#define NVGMC_PARAMS_SCHED_CAPS_SUPPORTED_SCHEDULERS   0x0002
#define NVGMC_PARAMS_SCHED_CAPS_IS_ARR_MODE_SUPPORTED  0x0003
#define NVGMC_PARAMS_SCHED_CAPS_MAX_TIMESLICE_NS       0x0004
#define NVGMC_PARAMS_SCHED_CAPS_MIN_TIMESLICE_NS       0x0005
#define NVGMC_PARAMS_SCHED_CAPS_MAX_FREQUENCY_FOR_ARR  0x0006
#define NVGMC_PARAMS_SCHED_CAPS_MIN_FREQUENCY_FOR_ARR  0x0007
#define NVGMC_PARAMS_SCHED_CAPS_MAX_AVG_FACTOR_FOR_ARR 0x0008
#define NVGMC_PARAMS_SCHED_CAPS_MIN_AVG_FACTOR_FOR_ARR 0x0009

/* Consuming SCHED_LOG reads: six U32 metadata keys at index zero, followed by
 * indexed entries (four U64 scalars, four U32 scalars, eight U64 counters).
 * TIME_RUN_TOTAL_NS carries the SDK signed NvS64 two's-complement bits.
 * Always reserve MAX_RESPONSE_SIZE payload bytes, including for empty reads.
 */
#define NVGMC_PARAMS_SCHED_LOG_ENGINE_ID                      NVGMC_PARAMS_SCHED_STATE_ENGINE_ID
#define NVGMC_PARAMS_SCHED_LOG_COUNT                          0x0002
#define NVGMC_PARAMS_SCHED_LOG_SCHED_POLICY                   0x0003
#define NVGMC_PARAMS_SCHED_LOG_ARR_ENABLED                    0x0004
#define NVGMC_PARAMS_SCHED_LOG_AVG_FACTOR_FOR_ARR             0x0005
#define NVGMC_PARAMS_SCHED_LOG_TARGET_TIMESLICE_NS            0x0006
#define NVGMC_PARAMS_SCHED_LOG_TIMESTAMP_NS                   0x0010
#define NVGMC_PARAMS_SCHED_LOG_TIME_RUN_TOTAL_NS              0x0011
#define NVGMC_PARAMS_SCHED_LOG_CUMULATIVE_PREEMPTION_TIME_NS  0x0012
#define NVGMC_PARAMS_SCHED_LOG_CUMULATIVE_IDLE_TIME_NS        0x0013
#define NVGMC_PARAMS_SCHED_LOG_TIME_RUN_NS                    0x0014
#define NVGMC_PARAMS_SCHED_LOG_SWRL_ID                        0x0015
#define NVGMC_PARAMS_SCHED_LOG_TARGET_TIME_SLICE_NS           0x0016
#define NVGMC_PARAMS_SCHED_LOG_WEIGHT                         0x0017
#define NVGMC_PARAMS_SCHED_LOG_COUNTERS                       0x0018
#define NVGMC_PARAMS_SCHED_LOG_MAX_ENTRIES                    100U
#define NVGMC_PARAMS_SCHED_LOG_COUNTER_COUNT                  8U
#define NVGMC_PARAMS_SCHED_LOG_HEADER_KV_COUNT                6U
#define NVGMC_PARAMS_SCHED_LOG_ENTRY_KV_COUNT \
        (5U + 3U + 1U + NVGMC_PARAMS_SCHED_LOG_COUNTER_COUNT)
#define NVGMC_PARAMS_SCHED_LOG_MAX_RESPONSE_SIZE \
        ((NVGMC_PARAMS_SCHED_LOG_HEADER_KV_COUNT + \
          NVGMC_PARAMS_SCHED_LOG_MAX_ENTRIES * \
          NVGMC_PARAMS_SCHED_LOG_ENTRY_KV_COUNT) * sizeof(NvU64))

/* NV2080 SDK wire values, not the NVML/CLI policy numbers 1, 2 and 3. */
#define NV2080_ENGINE_TYPE_GRAPHICS                         0x00000001U
#define NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_UNKNOWN        0U
#define NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_OTHER          1U
#define NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_BEST_EFFORT    2U
#define NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_EQUAL_SHARE    3U
#define NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_FIXED_SHARE    4U

#define NV2080_CTRL_CMD_VGPU_SCHEDULER_ARR_DEFAULT           0U
#define NV2080_CTRL_CMD_VGPU_SCHEDULER_ARR_DISABLE           1U
#define NV2080_CTRL_CMD_VGPU_SCHEDULER_ARR_ENABLE            2U

/*
 * Common SET/GET input. dbdf is the actual nonzero PF or VF DBDF.
 * The PF must match the FWCTL endpoint, or the VF must belong to that PF.
 * GSP derives the target scope. The aligned word array keeps NVKV at byte
 * offset 16 without an explicit reserved header field.
 */
struct GmcapiParamsInParams {
        NvU64 dbdf;
        NvU32 paramId; /* GMCAPI_{PGPU,VGPU}_PARAMS_PARAM_ID */
        _Alignas(8) NvU64 nvkvPayload[];
};

_Static_assert(offsetof(struct GmcapiParamsInParams, paramId) == 8,
               "GMCAPI paramId must immediately follow dbdf");
_Static_assert(offsetof(struct GmcapiParamsInParams, nvkvPayload) == 16,
               "GMCAPI parameter envelope must be 16 bytes");

#endif /* GMCAPI_PARAMS_H */
