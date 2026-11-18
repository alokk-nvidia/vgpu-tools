/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 */
#ifndef VGPUMETA_H
#define VGPUMETA_H

#include <stddef.h>
#include <stdint.h>

#define VGPUMETA_MAGIC "VGPUMETA"
#define VGPUMETA_VERSION_LEN 128
#define VGPUMETA_MAX_RECORDS 128

struct fwctl_pci_id;

struct vgpu_type_records {
	const uint8_t *info;
	size_t info_size;
	uint32_t info_count;
};

/* Returned pointers are valid while data remains mapped. */
int vgpumeta_find_records(const void *data, size_t size,
			  const struct fwctl_pci_id *pci,
			  struct vgpu_type_records *records,
			  const uint8_t **gsp_build_version);

#endif
