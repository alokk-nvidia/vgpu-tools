/* SPDX-License-Identifier: MIT */
/* Inspect real RPC envelopes at a mocked ioctl boundary, without a GPU.
 * Mock success verifies transport encoding, not firmware parameter support.
 */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fwctl.h"
#include "fwctl_common.h"
#include "fwctl_params.h"
#include "nvkv.h"

#define TEST_FD 12345

static struct {
        struct fwctl_params_target target;
        NvU32 command_id, param_id;
        NvU64 kv;
        unsigned int calls;
        int fail;
} mock;
static unsigned int tests;

int __wrap_ioctl(int fd, unsigned long request, ...)
{
        const struct fwctl_rpc_nova_core *in;
        struct fwctl_rpc_nova_core *out;
        const struct GmcapiParamsInParams *params;
        struct fwctl_rpc *rpc;
        va_list ap;

        assert(fd == TEST_FD && request == FWCTL_RPC);
        va_start(ap, request);
        rpc = va_arg(ap, struct fwctl_rpc *);
        va_end(ap);
        mock.calls++;
        assert(rpc->size == sizeof(*rpc) && rpc->scope == FWCTL_RPC_CONFIGURATION);
        assert(rpc->in % 16 == 0 && rpc->out % 16 == 0);
        assert(rpc->in_len == sizeof(*in) + 16 + sizeof(NvU64));
        in = (const void *)(uintptr_t)rpc->in;
        out = (void *)(uintptr_t)rpc->out;
        params = (const void *)(in + 1);
        assert(in->command_id == mock.command_id && in->reserved == 0);
        assert(params->dbdf == mock.target.dbdf);
        assert(params->paramId == mock.param_id);
        assert(memcmp(params->nvkvPayload, &mock.kv, sizeof(mock.kv)) == 0);
        if (mock.fail) {
                errno = EIO;
                return -1;
        }
        out->command_id = in->command_id;
        out->reserved = 0;
        rpc->out_len = sizeof(*out);
        return 0;
}

static void reset(const struct fwctl_params_target *target,
                   NvU32 command_id, NvU32 param_id)
{
        memset(&mock, 0, sizeof(mock));
        mock.target = *target;
        mock.command_id = command_id;
        mock.param_id = param_id;
        mock.kv = NVKV_KEY_IMM32(0, NVGMC_PARAMS_SCHED_STATE_ENGINE_ID,
                               NV2080_ENGINE_TYPE_GRAPHICS);
}

static void send_target(const struct fwctl_params_target *target, int valid)
{
        _Alignas(16) struct fwctl_rpc_nova_core response = {0};
        size_t payload_size;
        unsigned int calls = mock.calls;

        assert((fwctl_params_rpc(TEST_FD, mock.command_id, target,
                                 mock.param_id, &mock.kv, 1, &response,
                                 sizeof(response), &payload_size) == 0) == valid);
        assert(mock.calls == calls + !!valid);
        if (valid)
                assert(payload_size == 0);
        tests++;
}

int main(void)
{
        _Static_assert(GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_CAPS == 1, "caps ID");
        _Static_assert(GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_STATE == 2, "shared state ID");
        _Static_assert(GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_LOG == 3, "log ID");
        _Static_assert(GMCAPI_VGPU_PARAMS_PARAM_ID_INVALID == 0, "vGPU namespace");
        struct fwctl_params_target targets[] = {
                { .dbdf = 0x9b00 },
                { .dbdf = 0 },
                { .dbdf = 0 },
        };
        const NvU32 commands[] = {
                FWCTL_CMD_NOVA_CORE_GMCAPI_GET_PARAMS,
                FWCTL_CMD_NOVA_CORE_GMCAPI_SET_PARAMS,
        };
        struct fwctl_params_target invalid;
        struct fwctl_sched_caps caps;
        struct fwctl_sched_state state;
        struct fwctl_sched_log log;
        unsigned int i, j;

        assert(parse_dbdf("0000:9b:00.4", &targets[1].dbdf) == 0);
        assert(parse_dbdf("0001:9b:00.4", &targets[2].dbdf) == 0);
        assert(targets[2].dbdf > UINT32_MAX);
        /* Both commands preserve the actual PF/VF DBDF, including its PCI domain. */
        for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
                for (j = 0; j < sizeof(targets) / sizeof(targets[0]); j++) {
                        reset(&targets[j], commands[i],
                              commands[i] == FWCTL_CMD_NOVA_CORE_GMCAPI_GET_PARAMS ?
                                  GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_CAPS :
                                  GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_STATE);
                        send_target(&targets[j], 1);
                }
                reset(&targets[0], commands[i],
                      commands[i] == FWCTL_CMD_NOVA_CORE_GMCAPI_GET_PARAMS ?
                          GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_CAPS :
                          GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_STATE);
                invalid = targets[0];
                invalid.dbdf = 0;
                send_target(&invalid, 0);
                invalid.dbdf = UINT64_C(0x1009b00);
                send_target(&invalid, 0);
                invalid = targets[0];
                invalid.dbdf = UINT64_C(0x100009b04) | UINT64_C(0x80000000);
                send_target(&invalid, 0);
                send_target(NULL, 0);
        }
        /* Direct helper calls bypass CLI descriptors: even a rejected vGPU
         * scheduler query carries its VF DBDF unchanged, without PF substitution.
         */
        for (i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
                reset(&targets[i], commands[0], GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_CAPS);
                mock.fail = 1;
                assert(fwctl_query_sched_caps(TEST_FD, &targets[i], &caps) != 0);
                assert(mock.calls == 1);
                tests++;
                reset(&targets[i], commands[0], GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_STATE);
                mock.fail = 1;
                assert(fwctl_query_sched_state(TEST_FD, &targets[i], &state) != 0);
                assert(mock.calls == 1);
                tests++;
                reset(&targets[i], commands[0], GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_LOG);
                mock.fail = 1;
                assert(fwctl_query_sched_log(TEST_FD, &targets[i], &log) != 0);
                assert(mock.calls == 1);
                tests++;
        }
        printf("PASS: %u parameter transport target, DBDF and helper propagation cases\n", tests);
        return 0;
}
