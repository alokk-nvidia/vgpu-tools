/* SPDX-License-Identifier: MIT */
/* Raw FWCTL RPC probe for malformed-input testing. No SET/GET CLI validation. */
#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "fwctl.h"
#include "fwctl_common.h"

int main(int argc, char **argv)
{
        static const struct option options[] = {
                {"bdf", required_argument, NULL, 'b'},
                {"input", required_argument, NULL, 'i'},
                {"out-len", required_argument, NULL, 'o'},
                {NULL, 0, NULL, 0},
        };
        const char *bdf = NULL, *input_path = NULL;
        unsigned char *input = NULL, *output = NULL;
        NvU64 dbdf;
        NvU32 out_len = 16384;
        struct fwctl_rpc rpc = {0};
        FILE *file = NULL;
        long length;
        int opt, fd = -1, result = 2, rc, error;
        size_t i;

        while ((opt = getopt_long(argc, argv, "b:i:o:", options, NULL)) != -1) {
                switch (opt) {
                case 'b': bdf = optarg; break;
                case 'i': input_path = optarg; break;
                case 'o':
                        if (parse_u32("--out-len", optarg, &out_len))
                                return 2;
                        break;
                default: return 2;
                }
        }
        if (!bdf || !input_path || optind != argc || out_len > 1048576 ||
            parse_dbdf(bdf, &dbdf) || dbdf == 0)
                return 2;
        /* Only endpoint selection is validated. The raw request may target
         * zero, another PF, a VF, or any malformed DBDF under test.
         */
        if (check_nvidia_vendor_dbdf(dbdf) || check_is_pf_dbdf(dbdf))
                return 2;
        fd = open_nova_core_fwctl_for_dbdf(dbdf);
        if (fd < 0) {
                fprintf(stderr, "cannot open FWCTL endpoint for %s\n", bdf);
                return 2;
        }
        file = fopen(input_path, "rb");
        if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
            length > 1048576 || fseek(file, 0, SEEK_SET))
                goto done;
        error = posix_memalign((void **)&input, 16, length ? (size_t)length : 16);
        if (!error)
                error = posix_memalign((void **)&output, 16, out_len ? out_len : 16);
        if (error) {
                errno = error;
                goto done;
        }
        memset(output, 0xa5, out_len ? out_len : 16);
        if (fread(input, 1, (size_t)length, file) != (size_t)length)
                goto done;
        rpc.size = sizeof(rpc);
        rpc.scope = FWCTL_RPC_CONFIGURATION;
        rpc.in_len = (uint32_t)length;
        rpc.out_len = out_len;
        rpc.in = (uint64_t)(uintptr_t)input;
        rpc.out = (uint64_t)(uintptr_t)output;
        errno = 0;
        rc = ioctl(fd, FWCTL_RPC, &rpc);
        error = rc < 0 ? errno : 0;
        printf("{\"ioctl_rc\":%d,\"errno\":%d,\"out_len\":%u,"
               "\"capacity\":%u,\"response_hex\":\"", rc, error,
               rpc.out_len, out_len);
        /* FWCTL reports the full response length but copies only what fits. */
        if (rc == 0)
                for (i = 0; i < rpc.out_len && i < out_len; i++)
                        printf("%02x", output[i]);
        printf("\"}\n");
        result = 0;
done:
        if (result)
                fprintf(stderr, "raw RPC probe failed before ioctl: %s\n",
                        strerror(errno));
        if (file)
                fclose(file);
        free(output);
        free(input);
        close(fd);
        return result;
}
