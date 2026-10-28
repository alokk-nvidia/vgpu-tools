/* SPDX-License-Identifier: MIT */
/* Mock the device boundary: these tests never open a GPU or issue a real RPC. */
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
#include "gmcapi_params.h"
#include "nvkv.h"

#define TEST_FD 12345
#define CAPS_WORDS 10U

enum reply_fault {
        NO_FAULT, STATE_FAIL, STATE_MISSING_ACTIVE, STATE_DUPLICATE_ACTIVE,
        STATE_BAD_TYPE, STATE_BAD_COMMAND,
        GET_FAIL, GET_SHORT, GET_LONG, GET_UNALIGNED, GET_EMPTY,
        GET_BAD_COMMAND, GET_RESERVED, GET_DUPLICATE, GET_UNKNOWN,
        GET_INDEXED, GET_BAD_OPCODE, GET_BAD_ARRAY, GET_BAD_TYPE,
        SET_FAIL, SET_SHORT, SET_LONG, SET_BAD_COMMAND, SET_RESERVED,
};

static struct {
        enum reply_fault fault;
        NvU32 policy[3], arr, min_freq, max_freq, min_avg, max_avg;
        NvU32 active_vgpu_count;
        NvU64 set_kv[4];
        NvU32 set_count;
        int vendor_fail, pf_fail, open_fail, alloc_fail;
        int vendor_calls, open_calls, closes, state_calls, get_calls, set_calls, alloc_calls;
} mock;
static unsigned int tests;
static char error_output[4096];

int __real_posix_memalign(void **ptr, size_t alignment, size_t size);
int __real_close(int fd);

int __wrap_posix_memalign(void **ptr, size_t alignment, size_t size)
{
        if (++mock.alloc_calls == mock.alloc_fail)
                return ENOMEM;
        return __real_posix_memalign(ptr, alignment, size);
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
        assert(fd == TEST_FD);
        mock.closes++;
        return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
        struct fwctl_rpc *rpc;
        struct fwctl_rpc_nova_core *in, *out;
        struct GmcapiParamsInParams *params;
        const NvU64 *kv;
        NvU32 count;
        va_list ap;

        assert(fd == TEST_FD && request == FWCTL_RPC);
        va_start(ap, request);
        rpc = va_arg(ap, struct fwctl_rpc *);
        va_end(ap);
        assert(rpc->size == sizeof(*rpc) && rpc->scope == FWCTL_RPC_CONFIGURATION);
        assert(rpc->in % 16 == 0 && rpc->out % 16 == 0);
        in = (void *)(uintptr_t)rpc->in;
        out = (void *)(uintptr_t)rpc->out;
        params = (void *)(in + 1);
        kv = (const void *)params->nvkvPayload;
        assert(in->reserved == 0);
        assert(params->dbdf == 0x9b00);
        assert(rpc->in_len >= sizeof(*in) + 16);
        assert((rpc->in_len - sizeof(*in) - 16) % sizeof(NvU64) == 0);
        count = (rpc->in_len - sizeof(*in) - 16) / sizeof(NvU64);
        assert(count >= 1 && count <= 4);
        assert(kv[0] == NVKV_KEY_IMM32(0, NVGMC_PARAMS_SCHED_STATE_ENGINE_ID, 1));
        out->command_id = in->command_id;
        out->reserved = 0;

        if (in->command_id == FWCTL_CMD_NOVA_CORE_GMCAPI_GET_PARAMS) {
                NvU64 *out_kv = (void *)(out + 1);
                NVKVContext ctx = NVKV_BEGIN(out_kv, 0, CAPS_WORDS);

                if (params->paramId == GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_STATE) {
                        assert(mock.state_calls == 0 && mock.get_calls == 0 &&
                               mock.set_calls == 0);
                        mock.state_calls++;
                        assert(count == 1 && rpc->out_len == sizeof(*out) +
                               FWCTL_SCHED_STATE_NVKV_WORDS * sizeof(NvU64));
                        if (mock.fault == STATE_FAIL) {
                                errno = EIO;
                                return -1;
                        }
                        ctx = NVKV_BEGIN(out_kv, 0, FWCTL_SCHED_STATE_NVKV_WORDS);
                        nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_STATE_SCHED_POLICY,
                                     NV2080_CTRL_CMD_VGPU_SCHEDULER_POLICY_EQUAL_SHARE);
                        nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_STATE_AVG_FACTOR_FOR_ARR, 33);
                        nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_STATE_ARR_ENABLED,
                                     NV2080_CTRL_CMD_VGPU_SCHEDULER_ARR_ENABLE);
                        nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_STATE_TARGET_TIMESLICE_NS, 2083333);
                        nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_STATE_ACTIVE_VGPU_COUNT,
                                     mock.active_vgpu_count);
                        assert(NVKV_END(&ctx) == FWCTL_SCHED_STATE_NVKV_WORDS);
                        switch (mock.fault) {
                        case STATE_MISSING_ACTIVE: rpc->out_len -= sizeof(NvU64); break;
                        case STATE_DUPLICATE_ACTIVE: out_kv[3] = out_kv[4]; break;
                        case STATE_BAD_TYPE: out_kv[4] = NVKV_KEY_COUNT(_SEQ64, 0, 7, 1); break;
                        case STATE_BAD_COMMAND: out->command_id++; break;
                        default: break;
                        }
                        return 0;
                }
                assert(mock.state_calls == 1 && mock.active_vgpu_count == 0);
                mock.get_calls++;
                assert(params->paramId == GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_CAPS);
                assert(count == 1 && rpc->out_len == sizeof(*out) + 80);
                if (mock.fault == GET_FAIL) {
                        errno = EIO;
                        return -1;
                }
                nvkvSetArray32(&ctx, 0, NVGMC_PARAMS_SCHED_CAPS_SUPPORTED_SCHEDULERS,
                               mock.policy, 3);
                nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_CAPS_IS_ARR_MODE_SUPPORTED, mock.arr);
                nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_CAPS_MAX_TIMESLICE_NS, 30000000);
                nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_CAPS_MIN_TIMESLICE_NS, 1000000);
                nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_CAPS_MAX_FREQUENCY_FOR_ARR, mock.max_freq);
                nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_CAPS_MIN_FREQUENCY_FOR_ARR, mock.min_freq);
                nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_CAPS_MAX_AVG_FACTOR_FOR_ARR, mock.max_avg);
                nvkvSetImm32(&ctx, 0, NVGMC_PARAMS_SCHED_CAPS_MIN_AVG_FACTOR_FOR_ARR, mock.min_avg);
                assert(NVKV_END(&ctx) == CAPS_WORDS);
                rpc->out_len = sizeof(*out) + 80;
                switch (mock.fault) {
                case GET_SHORT: rpc->out_len = 7; break;
                case GET_LONG: rpc->out_len++; break;
                case GET_UNALIGNED: rpc->out_len--; break;
                case GET_EMPTY: rpc->out_len = sizeof(*out); break;
                case GET_BAD_COMMAND: out->command_id++; break;
                case GET_RESERVED: out->reserved = 1; break;
                case GET_DUPLICATE: out_kv[9] = NVKV_KEY_IMM32(0, 8, 1); break;
                case GET_UNKNOWN: out_kv[9] = NVKV_KEY_IMM32(0, 10, 1); break;
                case GET_INDEXED: out_kv[9] = NVKV_KEY_IMM32(1, 9, 1); break;
                case GET_BAD_OPCODE: out_kv[9] = 0xf0000000; break;
                case GET_BAD_ARRAY: out_kv[0] = NVKV_KEY_COUNT(_ARRAY32, 0, 2, 2); break;
                case GET_BAD_TYPE: out_kv[9] = NVKV_KEY_COUNT(_SEQ64, 0, 9, 1); break;
                default: break;
                }
        } else {
                assert(in->command_id == FWCTL_CMD_NOVA_CORE_GMCAPI_SET_PARAMS);
                assert(params->paramId == GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_STATE);
                assert(mock.state_calls == 1 && mock.get_calls == 1 && count >= 2);
                assert(rpc->out_len == sizeof(*out));
                mock.set_calls++;
                mock.set_count = count;
                memcpy(mock.set_kv, kv, count * sizeof(NvU64));
                if (mock.fault == SET_FAIL) {
                        errno = EIO;
                        return -1;
                }
                switch (mock.fault) {
                case SET_SHORT: rpc->out_len--; break;
                case SET_LONG: rpc->out_len += sizeof(NvU64); break;
                case SET_BAD_COMMAND: out->command_id++; break;
                case SET_RESERVED: out->reserved = 1; break;
                default: break;
                }
        }
        return 0;
}

static void reset(void)
{
        memset(&mock, 0, sizeof(mock));
        mock.policy[0] = 2;
        mock.policy[1] = 3;
        mock.policy[2] = 4;
        mock.arr = 1;
        mock.min_freq = 63;
        mock.max_freq = 960;
        mock.min_avg = 1;
        mock.max_avg = 60;
}

static void run(const char *args, int expected, int state_gets, int gets, int sets)
{
        char *text = strdup(args), *p, *save;
        char *argv[64];
        FILE *errors = tmpfile();
        int argc = 0, ret, saved_stderr;
        size_t error_size;

        assert(text && errors);
        for (p = strtok_r(text, " ", &save); p; p = strtok_r(NULL, " ", &save)) {
                assert(argc < 63);
                argv[argc++] = p;
        }
        argv[argc] = NULL;
        optind = 0;
        fflush(stderr);
        saved_stderr = dup(STDERR_FILENO);
        assert(saved_stderr >= 0);
        assert(dup2(fileno(errors), STDERR_FILENO) >= 0);
        ret = cmd_set_params("vgpu-mgmt set-params", argc, argv);
        fflush(stderr);
        assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
        assert(__real_close(saved_stderr) == 0);
        rewind(errors);
        error_size = fread(error_output, 1, sizeof(error_output) - 1, errors);
        error_output[error_size] = '\0';
        fclose(errors);
        assert(ret == expected);
        assert(mock.state_calls == state_gets && mock.get_calls == gets &&
               mock.set_calls == sets);
        assert(mock.closes == mock.open_calls - !!mock.open_fail);
        free(text);
        tests++;
}

#define BASE "set-params -b 0000:9b:00.0 -P sched-state "

int main(void)
{
        static const char *invalid[] = {
                "set-params", "set-params -b 0000:9b:00.0 -p 2",
                "set-params -P sched-state -p 2", BASE,
                BASE "-p 0", BASE "-p 4", BASE "-p -1",
                BASE "-p 4294967296", BASE "-p abc",
                BASE "-p 2 -asf -1", BASE "-p 2 -aavg 4294967296",
                BASE "-p 2 -aavg nope", BASE "-p 2 --engine-id 1",
                BASE "-p 2 -d /dev/fwctl/fwctl0", BASE "-p 2 junk",
                BASE "-p 2 -p 3", BASE "-p 2 -asf 63 --arr-frequency 64",
                BASE "-p 2 -aavg 1 --arr-avg-factor 2",
                BASE "-p 2 -b 9b:00.0", BASE "-p 2 -P sched-state",
                "set-params -b bad -P sched-state -p 2",
                "set-params -b 9b:00.0 -P unknown -p 2",
                "set-params -b 0000:00:00.0 -P sched-state -p 2",
                BASE "-p 2 -t pgpu", BASE "-p 2 --target pgpu",
                BASE "-p 2 -t vgpu", BASE "-p 2 --target vgpu",
                BASE "-p 2 --target unknown", BASE "-p 2 --target PGPU",
                BASE "-p 2 --target", BASE "-p 2 -t pgpu --target vgpu",
                BASE "-p 2 --target pgpu -t pgpu",
        };
        static const char *valid[] = {
                BASE "-p 2 -asf 63 -aavg 1",
                BASE "-p 3 -asf 960 -aavg 60",
                BASE "-p 2 -asf 0 -aavg 0",
                BASE "--policy 3 --arr-frequency 480 --arr-avg-factor 33",
                BASE "-p 2 -asf 240", BASE "-p 3 -aavg 33",
        };
        static const char *ranges[] = {
                BASE "-p 2 -asf 62", BASE "-p 2 -asf 961",
                BASE "-p 3 -aavg 61", BASE "-p 3 -asf 4294967295",
        };
        static const char *best_effort_arr[] = {
                BASE "-p 1 -asf 0", BASE "-p 1 -aavg 0",
                BASE "-p 1 -asf 480", BASE "-p 1 -aavg 33",
                BASE "-p 1 -asf 0 -aavg 0",
                BASE "-p 1 -asf 480 -aavg 33",
        };
        const NvU32 valid_counts[] = { 4, 4, 4, 4, 3, 3 };
        const NvU32 valid_freqs[] = { 63, 960, 0, 480, 240, 0 };
        const NvU32 valid_avgs[] = { 1, 60, 0, 33, 0, 33 };
        const NvU32 valid_policies[] = { 3, 4, 3, 4, 3, 4 };
        const char *policies[] = { BASE "-p 1", BASE "-p 2", BASE "-p 3" };
        unsigned int i;

        for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
                reset();
                run(invalid[i], EXIT_FAILURE, 0, 0, 0);
                assert(mock.vendor_calls == 0);
        }
        reset();
        run("set-params --help", EXIT_SUCCESS, 0, 0, 0);
        reset();
        run(BASE "-p 2", EXIT_SUCCESS, 1, 1, 1);
        reset();
        run(BASE "-p 3", EXIT_SUCCESS, 1, 1, 1);
        reset();
        run(BASE "-p 2 -t vgpu", EXIT_FAILURE, 0, 0, 0);
        assert(strstr(error_output, "Usage:"));
        assert(mock.vendor_calls == 0);
        for (i = 0; i < 3; i++) {
                reset();
                run(policies[i], EXIT_SUCCESS, 1, 1, 1);
                assert(mock.set_count == 2);
                assert(mock.set_kv[1] == NVKV_KEY_IMM32(0, 2, i + 2));
        }
        for (i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
                reset();
                run(valid[i], EXIT_SUCCESS, 1, 1, 1);
                assert(mock.set_count == valid_counts[i]);
                assert(mock.set_kv[1] == NVKV_KEY_IMM32(0, 2, valid_policies[i]));
                if (i < 5)
                        assert(mock.set_kv[2] == NVKV_KEY_IMM32(0, 3, valid_freqs[i]));
                if (i < 4)
                        assert(mock.set_kv[3] == NVKV_KEY_IMM32(0, 4, valid_avgs[i]));
                if (i == 5)
                        assert(mock.set_kv[2] == NVKV_KEY_IMM32(0, 4, valid_avgs[i]));
        }
        for (i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
                reset();
                run(ranges[i], EXIT_FAILURE, 1, 1, 0);
        }
        for (i = GET_FAIL; i <= SET_RESERVED; i++) {
                reset();
                mock.fault = i;
                run(BASE "-p 2", EXIT_FAILURE, 1, 1, i >= SET_FAIL);
        }
        /* Active-vGPU errors take precedence over ARR/policy compatibility. */
        for (i = 0; i < sizeof(best_effort_arr) / sizeof(best_effort_arr[0]); i++) {
                reset(); mock.active_vgpu_count = 1;
                run(best_effort_arr[i], EXIT_FAILURE, 1, 0, 0);
                assert(strstr(error_output, "vGPU instance is active on this GPU [vGPU count = 1]"));
                assert(!strstr(error_output, "require Equal Share"));
                reset();
                run(best_effort_arr[i], EXIT_FAILURE, 1, 0, 0);
                assert(strstr(error_output, "require Equal Share (-p 2) or Fixed Share (-p 3)"));
                assert(!strstr(error_output, "vGPU instance is active"));
        }
        reset(); mock.fault = STATE_FAIL;
        run(BASE "-p 1 -asf 0 -aavg 0", EXIT_FAILURE, 1, 0, 0);
        assert(strstr(error_output, "FWCTL command 0x00050002 failed"));
        assert(!strstr(error_output, "require Equal Share"));
        /* An active vGPU blocks every policy, without CAPS or SET RPCs. */
        for (i = 0; i < 3; i++) {
                reset(); mock.active_vgpu_count = 1;
                run(policies[i], EXIT_FAILURE, 1, 0, 0);
                assert(strstr(error_output, "vGPU instance is active on this GPU [vGPU count = 1]"));
                assert(strstr(error_output, "stop all vGPU instances and retry"));
        }
        reset(); mock.active_vgpu_count = 2;
        run(BASE "-p 3 -asf 480 -aavg 33", EXIT_FAILURE, 1, 0, 0);
        assert(strstr(error_output, "vGPU instances are active on this GPU [vGPU count = 2]"));
        reset(); mock.active_vgpu_count = UINT32_MAX;
        run(BASE "-p 2", EXIT_FAILURE, 1, 0, 0);
        assert(strstr(error_output, "vGPU instances are active on this GPU [vGPU count = 4294967295]"));
        /* Unknown/malformed active counts must never be treated as zero. */
        for (i = STATE_FAIL; i <= STATE_BAD_COMMAND; i++) {
                reset(); mock.fault = i;
                run(BASE "-p 2", EXIT_FAILURE, 1, 0, 0);
                assert(error_output[0] != '\0');
        }
        reset(); mock.max_freq = 240;
        run(BASE "-p 2 -asf 480", EXIT_FAILURE, 1, 1, 0);
        reset(); mock.max_avg = 10;
        run(BASE "-p 2 -aavg 33", EXIT_FAILURE, 1, 1, 0);
        reset(); mock.min_freq = 961;
        run(BASE "-p 2", EXIT_FAILURE, 1, 1, 0);
        reset(); mock.min_avg = 61;
        run(BASE "-p 2", EXIT_FAILURE, 1, 1, 0);
        reset(); mock.arr = 0;
        run(BASE "-p 2 -asf 0", EXIT_FAILURE, 1, 1, 0);
        reset(); mock.arr = 2;
        run(BASE "-p 2", EXIT_FAILURE, 1, 1, 0);
        reset(); mock.policy[2] = 0;
        run(BASE "-p 3", EXIT_FAILURE, 1, 1, 0);
        reset(); mock.vendor_fail = 1;
        run(BASE "-p 2", EXIT_FAILURE, 0, 0, 0);
        reset(); mock.pf_fail = 1;
        run(BASE "-p 2", EXIT_FAILURE, 0, 0, 0);
        reset(); mock.open_fail = 1;
        run(BASE "-p 2", EXIT_FAILURE, 0, 0, 0);
        reset(); mock.alloc_fail = 1;
        run(BASE "-p 2", EXIT_FAILURE, 0, 0, 0);
        reset(); mock.alloc_fail = 2;
        run(BASE "-p 2", EXIT_FAILURE, 1, 0, 0);
        reset(); mock.alloc_fail = 3;
        run(BASE "-p 2", EXIT_FAILURE, 1, 1, 0);
        printf("PASS: %u SET_PARAMS CLI, active-vGPU, NVKV, capability and transport cases\n", tests);
        return 0;
}
