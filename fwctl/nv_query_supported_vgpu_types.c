/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Query supported vGPU type IDs from GSP via the fwctl nova-core interface.
 *
 * GMCAPI QUERY_SUPPORTED_VGPU_TYPES:
 *   in:  fwctl_rpc_nova_core_request_hdr (no payload)
 *   out: fwctl_rpc_nova_core_resp_hdr + NvU32[] vGPU type IDs
 *   Valid count N = outParamSize / sizeof(NvU32)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/ioctl.h>

#include "fwctl.h"
#include "nvrm.h"
#include "fwctl_common.h"

static void usage_list_supported(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -b <bdf>      PCI BDF used to select the matching nova-core fwctl device\n"
		"  -h            show this help\n",
		prog);
}

int cmd_list_supported(const char *prog, int argc, char **argv)
{
	int opt, fd = -1, ret = EXIT_FAILURE;
	struct fwctl_rpc rpc = {0};
	struct fwctl_rpc_nova_core req_hdr = {0};
	uint32_t out_payload_size = QUERY_VGPU_TYPES_MAX * sizeof(NvU32);
	uint32_t out_size = sizeof(struct fwctl_rpc_nova_core) + out_payload_size;
	void *out_buf = NULL;
	struct fwctl_rpc_nova_core *resp_hdr;
	NvU32 *vgpu_type_ids;
	uint32_t i, n;
        NvU64 dbdf = 0;
        int got_dbdf = 0;

	while ((opt = getopt(argc, argv, "b:h")) != -1) {
		switch (opt) {
                case 'b':
                        if (parse_dbdf(optarg, &dbdf)) {
                                fprintf(stderr, "error: invalid -b value '%s'\n", optarg);
                                return EXIT_FAILURE;
                        }
                        got_dbdf = 1;
                        break;
		case 'h':
		default:
			usage_list_supported(prog);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (reject_extra_args(prog, argc, argv))
		return EXIT_FAILURE;
        if (!got_dbdf) {
                fprintf(stderr, "error: -b <bdf> is required\n\n");
                usage_list_supported(prog);
                return EXIT_FAILURE;
        }

        if (check_nvidia_vendor_dbdf(dbdf) < 0)
                return EXIT_FAILURE;
        fd = open_nova_core_fwctl_for_dbdf(dbdf);

	if (fd < 0) {
		fprintf(stderr, "no nova-core fwctl device found\n");
		return EXIT_FAILURE;
	}

	ret = posix_memalign(&out_buf, 16, out_size);
	if (ret) {
		fprintf(stderr, "posix_memalign failed: %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	}
	memset(out_buf, 0, out_size);

	req_hdr.command_id = FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_SUPPORTED_VGPU_TYPES;

	rpc.size = sizeof(rpc);
	rpc.scope = FWCTL_RPC_CONFIGURATION;
	rpc.in_len = sizeof(req_hdr);
	rpc.out_len = out_size;
	rpc.in = (uint64_t)(uintptr_t)&req_hdr;
	rpc.out = (uint64_t)(uintptr_t)out_buf;

	if (ioctl(fd, FWCTL_RPC, &rpc)) {
		perror("ioctl(FWCTL_RPC) QUERY_SUPPORTED_VGPU_TYPES");
		ret = EXIT_FAILURE;
		goto out;
	}

	resp_hdr = out_buf;

	/*
	 * Type IDs are written sequentially; the first zero marks the end of
	 * valid entries.  N = outParamSize / sizeof(NvU32).
	 */
	vgpu_type_ids = (NvU32 *)((uint8_t *)out_buf + sizeof(*resp_hdr));
	for (n = 0; n < QUERY_VGPU_TYPES_MAX; n++) {
		if (vgpu_type_ids[n] == 0)
			break;
	}

	fprintf(stdout, "SUPPORTED VGPU TYPE COUNT: %u\n", n);
	for (i = 0; i < n; i++)
		fprintf(stdout, "  [%02u] vGPU type ID: %u (0x%x)\n",
			i + 1, vgpu_type_ids[i], vgpu_type_ids[i]);

	if (n == 0) {
		fprintf(stderr,
			"note: GSP returned no supported vGPU types.\n"
			"      Common causes:\n"
			"        1. metadata not uploaded yet  -> run 'vgpu-mgmt add-type "
			"-f <metadata.bin> -p <pf-device-id>'\n"
			"        2. metadata uploaded for a different PCI device ID\n"
			"        3. GSP state was wiped (driver reload, GPU reset) -- "
			"re-run add-type\n");
	}

	fprintf(stdout, "QUERY_SUPPORTED_VGPU_TYPES: OK\n");
	ret = EXIT_SUCCESS;

out:
	free(out_buf);
	if (fd >= 0)
		close(fd);
	return ret;
}
