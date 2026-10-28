/* SPDX-License-Identifier: MIT */
/* Exercise real GET encoding/decoding and display through a mocked FWCTL boundary. */
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fwctl.h"
#include "fwctl_common.h"
#include "fwctl_params.h"
#include "nvkv.h"

#define TEST_FD 12345
#define CAPS "get-params -b 0000:9b:00.0 -P sched-caps"
#define STATE "get-params -b 0000:9b:00.0 -P sched-state"

enum reply_fault {
        NO_FAULT, RPC_FAIL, SHORT_HEADER, LONG_REPLY, UNALIGNED, EMPTY,
        BAD_COMMAND, RESERVED, DUPLICATE, UNKNOWN_KEY, INDEXED,
        BAD_OPCODE, BAD_TYPE, BAD_ARRAY, TRUNCATED, QUERY_KEY,
};
static struct {
        enum reply_fault fault;
        NvU32 param_id, policy, arr_enabled, active, avg, timeslice;
        NvU32 policies[3], arr_supported, max_slice, min_slice;
        NvU32 min_freq, max_freq, min_avg, max_avg;
        int vendor_fail, pf_fail, open_fail, alloc_fail;
        int vendor_calls, pf_calls, open_calls, closes, rpc_calls, alloc_calls;
        int reverse;
} mock;
static unsigned int tests;

int __real_close(int fd);
int __real_posix_memalign(void **ptr, size_t alignment, size_t size);

int __wrap_posix_memalign(void **ptr, size_t alignment, size_t size)
{
        mock.alloc_calls++;
        return mock.alloc_fail ? ENOMEM : __real_posix_memalign(ptr, alignment, size);
}
int __wrap_check_nvidia_vendor_dbdf(NvU64 dbdf)
{
        assert(dbdf == 0x9b00);
        mock.vendor_calls++;
        return mock.vendor_fail ? -1 : 0;
}
int __wrap_check_is_pf_dbdf(NvU64 dbdf)
{
        assert(dbdf == 0x9b00);
        mock.pf_calls++;
        return mock.pf_fail ? -1 : 0;
}
int __wrap_open_nova_core_fwctl_for_dbdf(NvU64 dbdf)
{
        assert(dbdf == 0x9b00);
        mock.open_calls++;
        return mock.open_fail ? -1 : TEST_FD;
}
int __wrap_close(int fd)
{
        if (fd != TEST_FD)
                return __real_close(fd);
        mock.closes++;
        return 0;
}
int __wrap_ioctl(int fd, unsigned long request, ...)
{
        struct fwctl_rpc *rpc;
        struct fwctl_rpc_nova_core *in, *out;
        const struct GmcapiParamsInParams *params;
        const NvU64 *query;
        NvU64 *kv, words;
        NVKVContext ctx;
        va_list ap;
        unsigned int i;

        assert(fd == TEST_FD && request == FWCTL_RPC);
        va_start(ap, request);
        rpc = va_arg(ap, struct fwctl_rpc *);
        va_end(ap);
        mock.rpc_calls++;
        assert(rpc->size == sizeof(*rpc) && rpc->scope == FWCTL_RPC_CONFIGURATION);
        assert(rpc->in % 16 == 0 && rpc->out % 16 == 0);
        assert(rpc->in_len == 32);
        in = (void *)(uintptr_t)rpc->in;
        out = (void *)(uintptr_t)rpc->out;
        params = (const void *)(in + 1);
        query = (const void *)params->nvkvPayload;
        assert(in->command_id == 0x00050002 && in->reserved == 0);
        assert(params->dbdf == 0x9b00);
        assert(params->paramId == mock.param_id);
        assert(query[0] == NVKV_KEY_IMM32(0, 1, 1));

        words = params->paramId == 1 ? 10 : 5;
        assert(rpc->out_len == sizeof(*out) + words * sizeof(NvU64));
        if (mock.fault == RPC_FAIL) {
                errno = EIO;
                return -1;
        }
        out->command_id = in->command_id;
        out->reserved = 0;
        kv = (void *)(out + 1);
        ctx = NVKV_BEGIN(kv, 0, words);
        if (params->paramId == 1) {
                nvkvSetArray32(&ctx, 0, 2, mock.policies, 3);
                nvkvSetImm32(&ctx, 0, 3, mock.arr_supported);
                nvkvSetImm32(&ctx, 0, 4, mock.max_slice);
                nvkvSetImm32(&ctx, 0, 5, mock.min_slice);
                nvkvSetImm32(&ctx, 0, 6, mock.max_freq);
                nvkvSetImm32(&ctx, 0, 7, mock.min_freq);
                nvkvSetImm32(&ctx, 0, 8, mock.max_avg);
                nvkvSetImm32(&ctx, 0, 9, mock.min_avg);
        } else {
                assert(params->paramId == 2);
                nvkvSetImm32(&ctx, 0, 2, mock.policy);
                nvkvSetImm32(&ctx, 0, 5, mock.arr_enabled);
                nvkvSetImm32(&ctx, 0, 6, mock.timeslice);
                nvkvSetImm32(&ctx, 0, 4, mock.avg);
                nvkvSetImm32(&ctx, 0, 7, mock.active);
        }
        assert(NVKV_END(&ctx) == words);
        if (mock.reverse) {
                /* Reorder scalar keys without changing the CAPS array payload. */
                unsigned int first = params->paramId == 1 ? 3 : 0;
                for (i = first; i < first + (words - first) / 2; i++) {
                        NvU64 tmp = kv[i];
                        kv[i] = kv[words + first - i - 1];
                        kv[words + first - i - 1] = tmp;
                }
        }
        rpc->out_len = sizeof(*out) + words * sizeof(NvU64);
        switch (mock.fault) {
        case SHORT_HEADER: rpc->out_len = 7; break;
        case LONG_REPLY: rpc->out_len++; break;
        case UNALIGNED: rpc->out_len--; break;
        case EMPTY: rpc->out_len = sizeof(*out); break;
        case BAD_COMMAND: out->command_id--; break;
        case RESERVED: out->reserved = 1; break;
        case DUPLICATE: kv[words - 1] = kv[words - 2]; break;
        case UNKNOWN_KEY: kv[words - 1] = NVKV_KEY_IMM32(0, 0xffff, 0); break;
        case INDEXED: kv[words - 1] = NVKV_KEY_IMM32(1, params->paramId == 1 ? 9 : 7, 1); break;
        case BAD_OPCODE: kv[words - 1] = 0xf0000000; break;
        case BAD_TYPE: kv[words - 1] = NVKV_KEY_COUNT(_SEQ64, 0, 7, 1); break;
        case BAD_ARRAY:
                kv[0] = NVKV_KEY_COUNT(_ARRAY32, 0, 2, params->paramId == 1 ? 2 : 0);
                break;
        case TRUNCATED: rpc->out_len -= sizeof(NvU64); break;
        case QUERY_KEY: kv[words - 1] = NVKV_KEY_IMM32(0, 1, 1); break;
        default: break;
        }
        return 0;
}
static void reset(NvU32 param_id)
{
        memset(&mock, 0, sizeof(mock));
        mock.param_id = param_id;
        mock.policy = NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_FIXED_SHARE;
        mock.arr_enabled = NV2080_CTRL_CMD_VGPU_SCHEDULER_ARR_ENABLE;
        mock.active = 2;
        mock.avg = 33;
        mock.timeslice = 2000000;
        mock.policies[0] = 2; mock.policies[1] = 3; mock.policies[2] = 4;
        mock.arr_supported = 1;
        mock.max_slice = 0xffffffff; mock.min_slice = 1000000;
        mock.min_freq = 63; mock.max_freq = 960;
        mock.min_avg = 1; mock.max_avg = 60;
}
/* Capture actual CLI output and verify no output is printed before validation. */
static char *run(const char *args, int expected, int rpc_calls)
{
        char *text = strdup(args), *p, *save, *output;
        char *argv[64];
        int argc = 0, ret, saved_stdout;
        FILE *capture = tmpfile();
        long size;

        assert(text && capture);
        for (p = strtok_r(text, " ", &save); p; p = strtok_r(NULL, " ", &save))
                argv[argc++] = p;
        argv[argc] = NULL;
        optind = 0;
        fflush(stdout);
        saved_stdout = dup(STDOUT_FILENO);
        assert(saved_stdout >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
        ret = cmd_get_params("vgpu-mgmt get-params", argc, argv);
        fflush(stdout);
        assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
        __real_close(saved_stdout);
        assert(ret == expected && mock.rpc_calls == rpc_calls);
        assert(mock.closes == mock.open_calls - !!mock.open_fail);
        assert(fseek(capture, 0, SEEK_END) == 0);
        size = ftell(capture);
        assert(size >= 0 && fseek(capture, 0, SEEK_SET) == 0);
        output = calloc((size_t)size + 1, 1);
        assert(output && fread(output, 1, size, capture) == (size_t)size);
        fclose(capture);
        if (expected == EXIT_FAILURE)
                assert(size == 0);
        free(text);
        tests++;
        return output;
}
static void has(const char *output, const char *text)
{
        assert(strstr(output, text));
}

int main(void)
{
        static const char *invalid[] = {
                "get-params", "get-params -b 9b:00.0", "get-params -P sched-state",
                "get-params -b bad -P sched-caps", CAPS " -P sched-caps",
                STATE " --bdf 9b:00.0", STATE " --engine-id 1",
                STATE " -p 2", STATE " -asf 480", STATE " -aavg 33",
                STATE " -d /dev/fwctl/fwctl0", STATE " extra",
                "get-params -b 9b:00.0 -P unknown", STATE " -P",
                "get-params -b 0000:00:00.0 -P sched-state",
                STATE " -t pgpu", STATE " --target pgpu",
                STATE " -t vgpu", CAPS " --target vgpu",
                STATE " --target unknown", STATE " --target PGPU",
                STATE " --target", STATE " -t pgpu --target vgpu",
                STATE " --target pgpu -t pgpu",
        };
        char *output;
        unsigned int i, param;

        for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
                reset(2);
                free(run(invalid[i], EXIT_FAILURE, 0));
                assert(mock.vendor_calls == 0);
        }
        reset(2);
        free(run("get-params --help", EXIT_SUCCESS, 0));
        assert(mock.vendor_calls == 0);
        reset(2);
        output = run(STATE, EXIT_SUCCESS, 1);
        has(output, "Scheduler Policy                : Fixed Share");
        free(output);
        reset(1);
        output = run(CAPS, EXIT_SUCCESS, 1);
        has(output, "Supported Policies");
        free(output);
        reset(1);
        output = run(CAPS, EXIT_SUCCESS, 1);
        has(output, "GPU 0000:9B:00.0");
        has(output, "Best Effort"); has(output, "Equal Share"); has(output, "Fixed Share");
        has(output, "ARR Mode                        : Supported");
        has(output, "Maximum Timeslice           : 4294967295 ns");
        has(output, "Minimum Timeslice           : 1000000 ns");
        has(output, "Maximum Frequency           : 960 Hz");
        has(output, "Minimum Frequency           : 63 Hz");
        has(output, "Maximum Avg Factor          : 60");
        has(output, "Minimum Avg Factor          : 1");
        free(output);
        reset(1); mock.arr_supported = 0; mock.policies[1] = mock.policies[2] = 0;
        output = run(CAPS, EXIT_SUCCESS, 1);
        has(output, "Not Supported"); has(output, "Unknown");
        free(output);
        reset(2);
        output = run(STATE, EXIT_SUCCESS, 1);
        has(output, "Active vGPUs                    : 2");
        has(output, "Scheduler Policy                : Fixed Share");
        has(output, "Average Factor                  : 33");
        has(output, "Time Slice(ns)                  : 2000000");
        assert(!strstr(output, "Frequency") && !strstr(output, "ARR Mode"));
        free(output);
        reset(2); mock.policy = 3;
        output = run("get-params --bdf 9b:00.0 --param sched-state", EXIT_SUCCESS, 1);
        has(output, "Equal Share"); free(output);
        for (i = 0; i < 3; i++) {
                reset(2);
                if (i == 0) mock.active = 0;
                if (i == 1) mock.policy = 2;
                if (i == 2) mock.policy = 0;
                output = run(STATE, EXIT_SUCCESS, 1);
                assert(!strstr(output, "Average Factor") && !strstr(output, "Time Slice"));
                if (i == 0) has(output, "Active vGPUs                    : 0");
                if (i == 1) has(output, "Best Effort");
                if (i == 2) has(output, "Unknown");
                free(output);
        }
        for (param = 1; param <= 2; param++) {
                const char *cmd = param == 1 ? CAPS : STATE;
                for (i = RPC_FAIL; i <= QUERY_KEY; i++) {
                        reset(param); mock.fault = i;
                        free(run(cmd, EXIT_FAILURE, 1));
                }
                for (i = 0; i < 4; i++) {
                        reset(param);
                        if (i == 0) mock.vendor_fail = 1;
                        if (i == 1) mock.pf_fail = 1;
                        if (i == 2) mock.open_fail = 1;
                        if (i == 3) mock.alloc_fail = 1;
                        free(run(cmd, EXIT_FAILURE, 0));
                }
                reset(param); mock.reverse = 1;
                free(run(cmd, EXIT_SUCCESS, 1));
        }
        reset(1); mock.arr_supported = 2; free(run(CAPS, EXIT_FAILURE, 1));
        reset(1); mock.policies[0] = 1; free(run(CAPS, EXIT_FAILURE, 1));
        reset(1); mock.min_slice = mock.max_slice; mock.max_slice--;
        free(run(CAPS, EXIT_FAILURE, 1));
        reset(1); mock.min_freq = mock.max_freq + 1; free(run(CAPS, EXIT_FAILURE, 1));
        reset(1); mock.min_avg = mock.max_avg + 1; free(run(CAPS, EXIT_FAILURE, 1));
        reset(2); mock.policy = 1; free(run(STATE, EXIT_FAILURE, 1));
        reset(2); mock.arr_enabled = 3; free(run(STATE, EXIT_FAILURE, 1));
        reset(2); mock.active = 0xffffffff; mock.timeslice = 0xffffffff;
        output = run(STATE, EXIT_SUCCESS, 1);
        has(output, "4294967295"); free(output);
        printf("PASS: %u GET_PARAMS CLI, wire, response and output cases\n", tests);
        return 0;
}
