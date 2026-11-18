/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Implements the `add-type` subcommand of vgpu-mgmt: loads a vGPU metadata
 * binary from disk, validates its identifier and CRC32, and issues the
 * FWCTL_CMD_NOVA_CORE_GMCAPI_ADD_VGPU_TYPE RPC to register the type with
 * the nova-core driver.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "fwctl.h"
#include "fwctl_common.h"

#include "nvrm.h"
#include "vgpumeta.h"

/* Parameters followed by one opaque NVA081_CTRL_VGPU_INFO record. */
struct gmcapi_add_vgpu_type_params {
	uint8_t discard_vgpu_types;
	uint8_t padding[3];
	uint32_t vgpu_info_count;
};

/* Prefix expected by gmcapiAddVgpuType().  One opaque vGPU info record is
 * appended to this header for each GMC round. */
struct gmcapi_add_vgpu_type_hdr {
	uint8_t gsp_build_version[VGPUMETA_VERSION_LEN];
	struct gmcapi_add_vgpu_type_params params;
};

_Static_assert(sizeof(struct gmcapi_add_vgpu_type_params) == 8,
	       "unexpected GMC add-vGPU-type parameter layout");
_Static_assert(sizeof(struct gmcapi_add_vgpu_type_hdr) == 136,
	       "unexpected GMC add-vGPU-type header layout");
static void usage_add_type(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -b <bdf>      PCI BDF used to select the matching nova-core fwctl device\n"
		"  -f <path>     VGPUMETA 1.0 binary file\n"
		"  -p <hex_id>   PCI device ID to match (e.g. 0x27b8)\n"
		"  -h            show this help\n",
		prog);
}

static int send_vgpu_type(int fd, const uint8_t *info, size_t info_size,
			  const uint8_t *gsp_build_version, bool discard)
{
	struct fwctl_rpc rpc = {0};
	struct fwctl_rpc_nova_core *req_hdr;
	struct fwctl_rpc_nova_core resp_hdr;
	struct gmcapi_add_vgpu_type_hdr *payload;
	size_t payload_size;
	size_t in_size;
	void *in_buf;
	int ret;

	if (info_size > UINT32_MAX - sizeof(*payload) - sizeof(*req_hdr)) {
		fprintf(stderr, "vGPU info record is too large: %zu bytes\n",
			info_size);
		return -EOVERFLOW;
	}
	payload_size = sizeof(*payload) + info_size;
	in_size = sizeof(*req_hdr) + payload_size;

	ret = posix_memalign(&in_buf, 16, in_size);
	if (ret) {
		fprintf(stderr, "failed to allocate %zu bytes for RPC in buffer\n",
			in_size);
		return -ENOMEM;
	}

	memset(in_buf, 0, in_size);

	req_hdr = in_buf;
	req_hdr->command_id = FWCTL_CMD_NOVA_CORE_GMCAPI_ADD_VGPU_TYPE;
	payload = (void *)((uint8_t *)in_buf + sizeof(*req_hdr));

	memcpy(payload->gsp_build_version, gsp_build_version,
	       sizeof(payload->gsp_build_version));
	payload->params.discard_vgpu_types = discard;
	payload->params.vgpu_info_count = 1;
	memcpy((uint8_t *)payload + sizeof(*payload), info, info_size);

	memset(&resp_hdr, 0, sizeof(resp_hdr));

	rpc.size = sizeof(rpc);
	rpc.scope = FWCTL_RPC_CONFIGURATION;
	rpc.in_len = (uint32_t)in_size;
	rpc.out_len = sizeof(resp_hdr);
	rpc.in = (uint64_t)(uintptr_t)in_buf;
	rpc.out = (uint64_t)(uintptr_t)&resp_hdr;

	ret = ioctl(fd, FWCTL_RPC, &rpc);
	if (ret) {
		perror("ioctl(FWCTL_RPC) ADD_VGPU_TYPE");
		free(in_buf);
		return -1;
	}

	free(in_buf);
	return 0;
}

static int send_vgpu_type_records(int fd,
				  const struct vgpu_type_records *records,
				  const uint8_t *gsp_build_version)
{
	uint32_t i;

	fprintf(stderr,
		"uploading %u vGPU types via multi-round GMC "
		"(per-round payload: %zu bytes)\n",
		records->info_count,
		sizeof(struct gmcapi_add_vgpu_type_hdr) + records->info_size);

	for (i = 0; i < records->info_count; i++) {
		const uint8_t *info = records->info + i * records->info_size;

		if (send_vgpu_type(fd, info, records->info_size,
				   gsp_build_version, i == 0))
			return -1;

		fprintf(stderr, "  [%u/%u] uploaded\n", i + 1,
			records->info_count);
	}

	return 0;
}

static int upload_vgpumeta(int fd, const void *data, size_t size,
			   const struct fwctl_pci_id *pci)
{
	struct vgpu_type_records records;
	const uint8_t *version;

	if (vgpumeta_find_records(data, size, pci, &records, &version))
		return -1;

	fprintf(stderr,
		"metadata :    VGPUMETA 1.0\ngsp_build:    %s\npci:          %04x:%04x:%04x:%04x\n",
		version, pci->vendor_id, pci->device_id, pci->subsystem_vendor_id,
		pci->subsystem_device_id);
	if (send_vgpu_type_records(fd, &records, version))
		return -1;

	fprintf(stderr, "\n%u vGPU Types Uploaded. ", records.info_count);
	return 0;
}

int cmd_add_type(const char *prog, int argc, char **argv)
{
	const char *meta_path = NULL;
	NvU32 device_id = 0;
	struct fwctl_pci_id pci;
        NvU64 dbdf = 0;
	int got_pci = 0;
        int got_dbdf = 0;
	int opt, fd = -1, meta_fd = -1, ret = EXIT_FAILURE;
	struct stat st;
	void *meta_mem = MAP_FAILED;

	while ((opt = getopt(argc, argv, "b:f:p:h")) != -1) {
		switch (opt) {
                case 'b':
                        if (parse_dbdf(optarg, &dbdf)) {
                                fprintf(stderr, "error: invalid -b value '%s'\n", optarg);
                                return EXIT_FAILURE;
                        }
                        got_dbdf = 1;
                        break;
		case 'f':
			meta_path = optarg;
			break;
		case 'p':
			if (parse_u32("-p", optarg, &device_id))
				return EXIT_FAILURE;
			got_pci = 1;
			break;
		case 'h':
		default:
			usage_add_type(prog);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (reject_extra_args(prog, argc, argv))
		return EXIT_FAILURE;

	if (!meta_path || !got_pci) {
		fprintf(stderr, "error: -f <metadata_file> and -p <device_id> are required\n\n");
		usage_add_type(prog);
		return EXIT_FAILURE;
	}

	/* Open fwctl device */
        if (!got_dbdf) {
                fprintf(stderr, "error: -b <bdf> is required\n\n");
                usage_add_type(prog);
                return EXIT_FAILURE;
        }

        if (check_nvidia_vendor_dbdf(dbdf) < 0)
                return EXIT_FAILURE;
        fd = open_nova_core_fwctl_for_dbdf(dbdf);

	if (fd < 0) {
		fprintf(stderr, "no nova-core fwctl device found\n");
		return EXIT_FAILURE;
	}

	/* Verify -p against the opened device before reading metadata. */
	if (fwctl_get_pci_id(fd, &pci))
		goto out;

	if (device_id != pci.device_id) {
		fprintf(stderr,
			"error: -p 0x%04x does not match fwctl device PCI ID 0x%04x\n",
			device_id, pci.device_id);
		goto out;
	}

	/* Load and mmap metadata file */
	meta_fd = open(meta_path, O_RDONLY);
	if (meta_fd < 0) {
		perror("open metadata file");
		goto out;
	}

	if (fstat(meta_fd, &st)) {
		perror("fstat metadata file");
		goto out;
	}

	if (st.st_size <= 0 || (uintmax_t)st.st_size > SIZE_MAX) {
		fprintf(stderr, "invalid metadata file size (%jd bytes)\n",
			(intmax_t)st.st_size);
		goto out;
	}

	meta_mem = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, meta_fd, 0);
	if (meta_mem == MAP_FAILED) {
		perror("mmap metadata file");
		goto out;
	}

	if (upload_vgpumeta(fd, meta_mem, (size_t)st.st_size, &pci))
		goto out;

	fprintf(stderr, "ADD_VGPU_TYPE: OK\n");
	ret = EXIT_SUCCESS;

out:
	if (meta_mem != MAP_FAILED)
		munmap(meta_mem, st.st_size);
	if (meta_fd >= 0)
		close(meta_fd);
	if (fd >= 0)
		close(fd);
	return ret;
}
