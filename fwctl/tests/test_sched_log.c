/* SPDX-License-Identifier: MIT */
/* Test the real SCHED_LOG decoder and -sl-style CLI without GPU RPCs or waits. */
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fwctl.h"
#include "fwctl_common.h"
#include "fwctl_params.h"
#include "nvkv.h"

#define TEST_FD 12345
#define LOG "get-params -b 0000:9b:00.0 -P sched-log"
#define STAMP UINT64_C(1789715597391072456)

static const struct fwctl_params_target pgpu_target = {
        .dbdf = 0x9b00,
};

enum fault {
        NONE, RPC_FAIL, SHORT_HEADER, LONG_REPLY, UNALIGNED, EMPTY,
        BAD_COMMAND, RESERVED, MISSING_META, DUP_META, INDEXED_META,
        META_TYPE, UNKNOWN_KEY, BAD_OPCODE, BAD_ENGINE, BAD_COUNT,
        BAD_POLICY, BAD_ARR, MISSING_ENTRY, DUP_ENTRY, HOLE_ENTRY,
        EXTRA_ENTRY, HIGH_INDEX, BAD_U64, BAD_U32, BAD_COUNTER_TYPE,
        BAD_COUNTER_COUNT, TRUNCATED,
};
static struct {
        enum fault fault;
        unsigned int entries, active, policy, reverse;
        unsigned int state_calls, log_calls, closes, opens, allocs, alloc_fail;
        unsigned int sleeps, interval, stop_after_sleeps, sleep_eintr, sleep_error;
        unsigned int state_fail, log_fail_after, best_effort_after;
        int vendor_fail, pf_fail, open_fail, stop_signal;
} mock;
static unsigned int tests;

int __real_close(int fd);
int __real_posix_memalign(void **ptr, size_t alignment, size_t size);

int __wrap_posix_memalign(void **ptr, size_t alignment, size_t size)
{
        if (++mock.allocs == mock.alloc_fail)
                return ENOMEM;
        return __real_posix_memalign(ptr, alignment, size);
}
int __wrap_check_nvidia_vendor_dbdf(NvU64 dbdf)
{
        assert(dbdf == 0x9b00);
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
        mock.opens++;
        return mock.open_fail ? -1 : TEST_FD;
}
int __wrap_close(int fd)
{
        if (fd != TEST_FD)
                return __real_close(fd);
        mock.closes++;
        return 0;
}
int __wrap_nanosleep(const struct timespec *delay, struct timespec *remaining)
{
        mock.sleeps++;
        assert(delay->tv_sec == (mock.sleep_eintr && mock.sleeps == 2 ? 0 : mock.interval));
        if (mock.sleep_error) {
                errno = EIO;
                return -1;
        }
        if (mock.sleep_eintr && mock.sleeps == 1) {
                *remaining = (struct timespec){ .tv_nsec = 1 };
                errno = EINTR;
                return -1;
        }
        if (mock.sleeps >= mock.stop_after_sleeps) {
                assert(raise(mock.stop_signal) == 0);
                errno = EINTR;
                return -1;
        }
        return 0;
}

static void metadata(NVKVContext *ctx)
{
        NvU32 values[] = { 1, mock.entries, 4, 2,
                          mock.active ? 33 : 0, mock.active ? 2083333 : 0 };
        unsigned int i;

        if (mock.fault == BAD_ENGINE) values[0] = 2;
        if (mock.fault == BAD_COUNT) values[1] = 101;
        if (mock.fault == EXTRA_ENTRY) values[1] = 1;
        if (mock.fault == BAD_POLICY) values[2] = 99;
        if (mock.fault == BAD_ARR) values[3] = 3;
        for (i = 0; i < 6; i++) {
                if (i == 5 && mock.fault == MISSING_META)
                        continue;
                if (i == 0 && mock.fault == META_TYPE)
                        nvkvSetSeq64_1U(ctx, 0, 1, 1);
                else
                        nvkvSetImm32(ctx, 0, i + 1, values[i]);
        }
}

/* Encode the same packed sequences and counter array emitted by GSP-RM. */
static void entries(NVKVContext *ctx)
{
        NvU64 counters[NVGMC_PARAMS_SCHED_LOG_COUNTER_COUNT];
        unsigned int n, j;

        for (n = 0; n < mock.entries; n++) {
                unsigned int i = mock.reverse ? mock.entries - 1 - n : n;
                unsigned int idx = mock.fault == HOLE_ENTRY ? i + 1 : i;
                NvU64 total = i ? UINT64_C(9000000000) + i : (NvU64)(int64_t)-42;

                if (mock.fault == HIGH_INDEX && i == 0)
                        idx = NVGMC_PARAMS_SCHED_LOG_MAX_ENTRIES;
                if (mock.fault == MISSING_ENTRY && i == 0)
                        nvkvSetSeq64_3U(ctx, idx, 0x10, STAMP + i, total, 4200000000U + i);
                else if (mock.fault == BAD_U64 && i == 0)
                        nvkvSetSeq32_4U(ctx, idx, 0x10, 1, 2, 3, 4, 4);
                else
                        nvkvSetSeq64_4U(ctx, idx, 0x10, STAMP + i, total,
                                        4200000000U + i, UINT64_C(5000000000) + i);
                if (mock.fault == BAD_U32 && i == 0)
                        nvkvSetSeq64_4U(ctx, idx, 0x14, 1000 + i, i ? 33 : 0, 2083333, 0x0c000000);
                else
                        nvkvSetSeq32_4U(ctx, idx, 0x14, 1000 + i, i ? 33 : 0,
                                        2083333, i ? 0x04000000 : 0x0c000000, 4);
                for (j = 0; j < NVGMC_PARAMS_SCHED_LOG_COUNTER_COUNT; j++)
                        counters[j] = UINT64_C(0x100000000) + i * 10 + j;
                if (mock.fault == BAD_COUNTER_TYPE && i == 0) {
                        NvU32 bad[8] = {0};
                        nvkvSetArray32(ctx, idx, 0x18, bad, 8);
                } else {
                        nvkvSetArray64(ctx, idx, 0x18, counters,
                                        mock.fault == BAD_COUNTER_COUNT && i == 0 ? 7 : 8);
                }
        }
        if (mock.fault == DUP_ENTRY)
                nvkvSetImm32(ctx, 0, 0x17, 0);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
        struct fwctl_rpc *rpc;
        struct fwctl_rpc_nova_core *in, *out;
        const struct GmcapiParamsInParams *params;
        NvU64 *kv, words;
        NVKVContext ctx;
        va_list ap;

        assert(fd == TEST_FD && request == FWCTL_RPC);
        va_start(ap, request);
        rpc = va_arg(ap, struct fwctl_rpc *);
        va_end(ap);
        assert(rpc->scope == FWCTL_RPC_CONFIGURATION && rpc->in_len == 32);
        assert(rpc->in % 16 == 0 && rpc->out % 16 == 0);
        in = (void *)(uintptr_t)rpc->in;
        out = (void *)(uintptr_t)rpc->out;
        params = (const void *)(in + 1);
        assert(in->command_id == 0x00050002 && in->reserved == 0);
        assert(params->dbdf == 0x9b00);
        assert(*(const NvU64 *)params->nvkvPayload == NVKV_KEY_IMM32(0, 1, 1));
        out->command_id = in->command_id;
        out->reserved = 0;
        kv = (void *)(out + 1);
        if (params->paramId == GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_STATE) {
                NvU32 policy = mock.best_effort_after && mock.state_calls >= mock.best_effort_after ? 2 : mock.policy;
                mock.state_calls++;
                assert(rpc->out_len == 48);
                if (mock.state_fail) {
                        errno = EIO;
                        return -1;
                }
                ctx = NVKV_BEGIN(kv, 0, 5);
                nvkvSetImm32(&ctx, 0, 2, policy);
                nvkvSetImm32(&ctx, 0, 4, 33);
                nvkvSetImm32(&ctx, 0, 5, 2);
                nvkvSetImm32(&ctx, 0, 6, 2083333);
                nvkvSetImm32(&ctx, 0, 7, mock.active);
                assert(NVKV_END(&ctx) == 5);
                return 0;
        }
        assert(params->paramId == GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_LOG);
        mock.log_calls++;
        /* Capacity must always hold 100 entries, including an empty read. */
        assert(NVGMC_PARAMS_SCHED_LOG_MAX_RESPONSE_SIZE == 13648);
        assert(rpc->out_len == sizeof(*out) + 13648);
        if (mock.fault == RPC_FAIL || mock.log_calls == mock.log_fail_after) {
                errno = EIO;
                return -1;
        }
        ctx = NVKV_BEGIN(kv, 0, 13648 / sizeof(NvU64));
        if (mock.reverse) { entries(&ctx); metadata(&ctx); }
        else { metadata(&ctx); entries(&ctx); }
        words = NVKV_END(&ctx);
        assert(words * sizeof(NvU64) <= 13648);
        rpc->out_len = sizeof(*out) + words * sizeof(NvU64);
        switch (mock.fault) {
        case SHORT_HEADER: rpc->out_len = 7; break;
        case LONG_REPLY: rpc->out_len = sizeof(*out) + 13649; break;
        case UNALIGNED: rpc->out_len--; break;
        case EMPTY: rpc->out_len = sizeof(*out); break;
        case BAD_COMMAND: out->command_id--; break;
        case RESERVED: out->reserved = 1; break;
        case DUP_META: kv[5] = kv[4]; break;
        case INDEXED_META: kv[0] = NVKV_KEY_IMM32(1, 1, 1); break;
        case UNKNOWN_KEY: kv[0] = NVKV_KEY_IMM32(0, 15, 1); break;
        case BAD_OPCODE: kv[0] = 0xf0000000; break;
        case TRUNCATED: rpc->out_len -= sizeof(NvU64); break;
        default: break;
        }
        return 0;
}

static void reset(void)
{
        memset(&mock, 0, sizeof(mock));
        mock.entries = 2;
        mock.active = 1;
        mock.policy = 4;
        mock.interval = 1;
        mock.stop_after_sleeps = 1;
        mock.stop_signal = SIGINT;
}

static char *run(const char *args, int expected, unsigned int states, unsigned int logs)
{
        char *text = strdup(args), *p, *save, *output;
        char *argv[64];
        FILE *capture = tmpfile();
        struct sigaction before_int, after_int, before_term, after_term;
        int argc = 0, ret, saved_stdout;
        long size;

        assert(text && capture);
        for (p = strtok_r(text, " ", &save); p; p = strtok_r(NULL, " ", &save))
                argv[argc++] = p;
        argv[argc] = NULL;
        optind = 0;
        sigaction(SIGINT, NULL, &before_int);
        sigaction(SIGTERM, NULL, &before_term);
        fflush(stdout);
        saved_stdout = dup(STDOUT_FILENO);
        assert(saved_stdout >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
        ret = cmd_get_params("vgpu-mgmt get-params", argc, argv);
        fflush(stdout);
        assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
        __real_close(saved_stdout);
        sigaction(SIGINT, NULL, &after_int);
        sigaction(SIGTERM, NULL, &after_term);
        assert(before_int.sa_handler == after_int.sa_handler);
        assert(before_term.sa_handler == after_term.sa_handler);
        assert(ret == expected && mock.state_calls == states && mock.log_calls == logs);
        assert(mock.closes == mock.opens - !!mock.open_fail);
        assert(fseek(capture, 0, SEEK_END) == 0);
        size = ftell(capture);
        assert(size >= 0 && fseek(capture, 0, SEEK_SET) == 0);
        output = calloc((size_t)size + 1, 1);
        assert(output && fread(output, 1, size, capture) == (size_t)size);
        fclose(capture);
        free(text);
        tests++;
        return output;
}

static unsigned int occurrences(const char *text, const char *needle)
{
        unsigned int count = 0;
        while ((text = strstr(text, needle)) != NULL) { count++; text += strlen(needle); }
        return count;
}

int main(void)
{
        static const unsigned int counts[] = { 0, 1, 2, 100 };
        static const char *invalid[] = {
                LOG " --loop 0", LOG " -l 11", LOG " -l -1", LOG " -l abc",
                LOG " -l 4294967296", LOG " --once --once", LOG " -l 1 --loop 2",
                "get-params -b 9b:00.0 -P sched-state --once",
                "get-params -b 9b:00.0 -P sched-caps -l 1", LOG " --engine-id 1",
                "get-params -b 0000:00:00.0 -P sched-log --once",
                LOG " --target pgpu --once", LOG " -t pgpu --once",
                LOG " --target vgpu --once", LOG " --target invalid --once",
                LOG " -t pgpu --target vgpu --once",
        };
        struct fwctl_sched_log *log = calloc(1, sizeof(*log));
        char *output;
        unsigned int i, j;

        assert(log);
        for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
                reset(); mock.entries = counts[i];
                assert(fwctl_query_sched_log(TEST_FD, &pgpu_target, log) == 0);
                assert(mock.log_calls == 1 && log->count == counts[i]);
                assert(log->engine_id == 1 && log->policy == 4 && log->avg_factor == 33);
                for (j = 0; j < log->count; j++) {
                        assert(log->entries[j].timestamp_ns == STAMP + j);
                        assert(log->entries[j].time_run_total_ns == (j ? INT64_C(9000000000) + j : -42));
                        assert(log->entries[j].cumulative_idle_ns == UINT64_C(5000000000) + j);
                        assert(log->entries[j].counters[7] == UINT64_C(0x100000000) + j * 10 + 7);
                }
                tests++;
        }
        reset(); mock.reverse = 1;
        assert(fwctl_query_sched_log(TEST_FD, &pgpu_target, log) == 0 && log->entries[1].swrl_id == 33);
        tests++;
        for (i = RPC_FAIL; i <= TRUNCATED; i++) {
                reset(); mock.fault = i;
                assert(fwctl_query_sched_log(TEST_FD, &pgpu_target, log) != 0);
                tests++;
        }
        for (i = 1; i <= 2; i++) {
                reset(); mock.alloc_fail = i;
                assert(fwctl_query_sched_log(TEST_FD, &pgpu_target, log) != 0 && mock.log_calls == 0);
                tests++;
        }
        for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
                reset(); output = run(invalid[i], EXIT_FAILURE, 0, 0);
                assert(!output[0]); free(output);
        }
        reset(); output = run(LOG " --once", EXIT_SUCCESS, 10, 10);
        assert(mock.sleeps == 0);
        assert(occurrences(output, "GPU 0000:9B:00.0\n") == 10);
        free(output);
        reset(); output = run(LOG " --once", EXIT_SUCCESS, 10, 10);
        assert(mock.sleeps == 0);
        assert(occurrences(output, "GPU 0000:9B:00.0\n") == 10);
        assert(occurrences(output, "1789715597391072456") == 10);
        assert(strstr(output, "SW Runlist Id") && strstr(output, "Weight(0x)"));
        assert(strstr(output, "0c000000") && strstr(output, "04000000") && strstr(output, "-42"));
        assert(strstr(output, "Avg Factor         33") && strstr(output, "Time Slice(ns)     2083333"));
        free(output);
        reset(); mock.entries = 100;
        output = run(LOG " --once", EXIT_SUCCESS, 10, 10);
        assert(occurrences(output, "0c000000") == 10 && occurrences(output, "04000000") == 990);
        free(output);
        reset(); mock.entries = 0;
        output = run(LOG " --once", EXIT_SUCCESS, 10, 10);
        assert(occurrences(output, "Timestamp(ns)") == 10 && !strstr(output, "1789715597391072456"));
        free(output);
        reset(); mock.active = 0;
        output = run(LOG " --once", EXIT_SUCCESS, 10, 10);
        assert(occurrences(output, "has no active VM runlist") == 10);
        assert(!strstr(output, "Avg Factor") && !strstr(output, "1789715597391072456"));
        free(output);
        for (i = 0; i < 2; i++) {
                reset(); mock.policy = i ? 0 : 2;
                output = run(LOG " --once", EXIT_SUCCESS, 10, 0);
                assert(strstr(output, i ? "policy Unknown" : "policy Best Effort"));
                assert(!strstr(output, "Avg Factor")); free(output);
        }
        reset(); output = run(LOG, EXIT_SUCCESS, 10, 10);
        assert(mock.sleeps == 1); free(output);
        reset(); mock.interval = 10; mock.stop_signal = SIGTERM; mock.stop_after_sleeps = 2;
        output = run(LOG " -l 10", EXIT_SUCCESS, 20, 20);
        assert(mock.sleeps == 2); free(output);
        reset(); mock.sleep_eintr = 1;
        output = run(LOG, EXIT_SUCCESS, 10, 10);
        assert(mock.sleeps == 2); free(output);
        reset(); mock.sleep_error = 1;
        free(run(LOG, EXIT_FAILURE, 10, 10));
        reset(); mock.best_effort_after = 2;
        output = run(LOG " --once", EXIT_SUCCESS, 10, 2);
        assert(occurrences(output, "policy Best Effort") == 8); free(output);
        reset(); mock.state_fail = 1;
        output = run(LOG " --once", EXIT_FAILURE, 1, 0);
        assert(!output[0]); free(output);
        reset(); mock.fault = BAD_COUNTER_COUNT;
        output = run(LOG " --once", EXIT_FAILURE, 1, 1);
        assert(!output[0]); free(output);
        reset(); mock.log_fail_after = 2;
        output = run(LOG " --once", EXIT_FAILURE, 2, 2);
        assert(occurrences(output, "1789715597391072456") == 1); free(output);
        for (i = 0; i < 3; i++) {
                reset();
                if (i == 0) mock.vendor_fail = 1;
                if (i == 1) mock.pf_fail = 1;
                if (i == 2) mock.open_fail = 1;
                output = run(LOG " --once", EXIT_FAILURE, 0, 0);
                assert(!output[0]); free(output);
        }
        free(log);
        printf("PASS: %u SCHED_LOG wire, decoder, display, polling and error cases\n", tests);
        return 0;
}
