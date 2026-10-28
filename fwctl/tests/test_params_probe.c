/* SPDX-License-Identifier: MIT */
/* Exercise raw probe buffers at a mocked ioctl boundary; no firmware execution. */
#include <assert.h>
#include <stdarg.h>

#define main params_rpc_probe_main
#include "params_rpc_probe.c"
#undef main

#define TEST_FD 12345
static unsigned int ioctl_calls, cases;
static size_t wanted_length;
static NvU32 wanted_capacity;
static int fail_rpc, allocation_count, fail_allocation;
int __real_posix_memalign(void **pointer, size_t alignment, size_t size);
int __real_close(int fd);

int __wrap_posix_memalign(void **pointer, size_t alignment, size_t size)
{
        if (++allocation_count == fail_allocation)
                return ENOMEM;
        return __real_posix_memalign(pointer, alignment, size);
}

int __wrap_check_nvidia_vendor_dbdf(NvU64 dbdf)
{
        assert(dbdf == 0x9b00);
        return 0;
}

int __wrap_check_is_pf_dbdf(NvU64 dbdf)
{
        assert(dbdf == 0x9b00);
        return 0;
}

int __wrap_open_nova_core_fwctl_for_dbdf(NvU64 dbdf)
{
        assert(dbdf == 0x9b00);
        return TEST_FD;
}

int __wrap_close(int fd)
{
        assert(fd == TEST_FD);
        return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
        va_list args;
        struct fwctl_rpc *rpc;
        size_t i;

        assert(fd == TEST_FD && request == FWCTL_RPC);
        va_start(args, request);
        rpc = va_arg(args, struct fwctl_rpc *);
        va_end(args);
        assert(rpc->scope == FWCTL_RPC_CONFIGURATION);
        assert(rpc->in % 16 == 0 && rpc->out % 16 == 0);
        assert(rpc->in_len == wanted_length && rpc->out_len == wanted_capacity);
        for (i = 0; i < wanted_length; i++)
                assert(((const unsigned char *)(uintptr_t)rpc->in)[i] == 0xa7);
        ioctl_calls++;
        if (fail_rpc) {
                errno = EIO;
                return -1;
        }
        memset((void *)(uintptr_t)rpc->out, 0,
               wanted_capacity < 8 ? wanted_capacity : 8);
        rpc->out_len = 8;
        return 0;
}

int main(void)
{
        const size_t lengths[] = {0, 1, 7, 8, 15, 16, 23, 24, 31, 32, 128, 4096, 16384, 65536};
        const NvU32 capacities[] = {0, 1, 7, 8, 47, 13656};
        unsigned char data[4096];
        char filename[] = "/tmp/params-probe-test-XXXXXX", capacity[32];
        char *argv[] = {"probe", "-b", "9b:00.0", "-i", filename, "-o", capacity, NULL};
        int raw_fd = mkstemp(filename), saved_stdout;
        FILE *file, *capture;
        size_t i, j, remaining;

        assert(raw_fd >= 0);
        file = fdopen(raw_fd, "wb");
        capture = tmpfile();
        assert(file && capture);
        saved_stdout = dup(STDOUT_FILENO);
        assert(saved_stdout >= 0);
        fflush(stdout);
        assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
        memset(data, 0xa7, sizeof(data));
        for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
                assert(ftruncate(raw_fd, 0) == 0 && fseek(file, 0, SEEK_SET) == 0);
                remaining = lengths[i];
                while (remaining) {
                        size_t count = remaining < sizeof(data) ? remaining : sizeof(data);
                        assert(fwrite(data, 1, count, file) == count);
                        remaining -= count;
                }
                assert(fflush(file) == 0);
                for (j = 0; j < sizeof(capacities) / sizeof(capacities[0]); j++) {
                        for (fail_rpc = 0; fail_rpc < 2; fail_rpc++) {
                                unsigned int before = ioctl_calls;
                                long response_start = ftell(capture);
                                char response[512], *hex_start, *hex_end;
                                size_t expected_bytes;
                                wanted_length = lengths[i];
                                wanted_capacity = capacities[j];
                                snprintf(capacity, sizeof(capacity), "%u", wanted_capacity);
                                optind = 0;
                                assert(params_rpc_probe_main(7, argv) == 0);
                                assert(ioctl_calls == before + 1);
                                fflush(stdout);
                                assert(fseek(capture, response_start, SEEK_SET) == 0);
                                assert(fgets(response, sizeof(response), capture));
                                hex_start = strstr(response, "\"response_hex\":\"");
                                assert(hex_start);
                                hex_start += strlen("\"response_hex\":\"");
                                hex_end = strchr(hex_start, '"');
                                expected_bytes = fail_rpc ? 0 :
                                        (wanted_capacity < 8 ? wanted_capacity : 8);
                                assert(hex_end && (size_t)(hex_end - hex_start) ==
                                       expected_bytes * 2);
                                assert(fseek(capture, 0, SEEK_END) == 0);
                                cases++;
                        }
                }
        }
        for (fail_allocation = 1; fail_allocation <= 2; fail_allocation++) {
                unsigned int before = ioctl_calls;
                allocation_count = 0;
                optind = 0;
                assert(params_rpc_probe_main(7, argv) == 2);
                assert(ioctl_calls == before);
                cases++;
        }
        fflush(stdout);
        assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
        __real_close(saved_stdout);
        /* __wrap_close is reserved for the probe's synthetic device FD. */
        fclose(capture);
        fclose(file);
        unlink(filename);
        printf("PASS: %u raw FWCTL probe length/capacity/ioctl cases (no firmware execution)\n", cases);
        return 0;
}
