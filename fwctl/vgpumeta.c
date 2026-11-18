/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <endian.h>
#include <stdio.h>
#include <string.h>

#include "fwctl_common.h"
#include "vgpumeta.h"

enum {
	FILE_HEADER_SIZE = 160,
	CRC_OFFSET = 28,
	VERSION_OFFSET = 32,
	COMMON_GROUP_SIZE = 16,
	VGPU_GROUP_SIZE = 32,
	VGPU_TYPE_GROUP = 1,
	MAX_GROUPS = 1024,
	NVIDIA_VENDOR_ID = 0x10de,
};

static uint16_t read_le16(const uint8_t *p)
{
	uint16_t v;

	memcpy(&v, p, sizeof(v));
	return le16toh(v);
}

static uint32_t read_le32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return le32toh(v);
}

static uint64_t read_le64(const uint8_t *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));
	return le64toh(v);
}

static int invalid(const char *reason)
{
	fprintf(stderr, "invalid VGPUMETA: %s\n", reason);
	return -1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
	size_t i;
	unsigned int bit;

	for (i = 0; i < size; i++) {
		crc ^= data[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
	}
	return crc;
}

static uint32_t file_crc32(const uint8_t *data, size_t size)
{
	static const uint8_t zero_crc[4];
	uint32_t crc = crc32_update(UINT32_MAX, data, CRC_OFFSET);

	crc = crc32_update(crc, zero_crc, sizeof(zero_crc));
	return crc32_update(crc, data + VERSION_OFFSET, size - VERSION_OFFSET) ^
	       UINT32_MAX;
}

int vgpumeta_find_records(const void *data, size_t size,
			  const struct fwctl_pci_id *pci,
			  struct vgpu_type_records *records,
			  const uint8_t **gsp_build_version)
{
	const uint8_t *file = data;
	const uint8_t *version, *nul;
	uint64_t selectors[MAX_GROUPS];
	uint32_t count, i, seen = 0, abi_size = 0;
	struct vgpu_type_records match = {0};
	size_t offset = FILE_HEADER_SIZE;

	if (size < sizeof(VGPUMETA_MAGIC) - 1 ||
	    memcmp(file, VGPUMETA_MAGIC, sizeof(VGPUMETA_MAGIC) - 1))
		return invalid("unsupported metadata identifier (expected VGPUMETA)");
	if (size < FILE_HEADER_SIZE)
		return invalid("truncated file header");
	if (read_le16(file + 8) != 1 || read_le16(file + 10) != 0 ||
	    read_le32(file + 12) != FILE_HEADER_SIZE)
		return invalid("unsupported format version or header size");
	if (read_le64(file + 16) != size)
		return invalid("file length mismatch");

	count = read_le32(file + 24);
	if (!count || count > MAX_GROUPS)
		return invalid("invalid group count");
	if (read_le32(file + CRC_OFFSET) != file_crc32(file, size))
		return invalid("CRC32 mismatch");

	version = file + VERSION_OFFSET;
	nul = memchr(version, '\0', VGPUMETA_VERSION_LEN);
	if (!nul || nul == version)
		return invalid("invalid GSP build version");
	for (const uint8_t *p = nul + 1; p < version + VGPUMETA_VERSION_LEN; p++) {
		if (*p)
			return invalid("GSP build version must be zero-padded");
	}

	for (i = 0; i < count; i++) {
		const uint8_t *group = file + offset;
		uint32_t type, info_size, info_count, j;
		uint16_t header_size;
		uint64_t group_size, selector;

		if (size - offset < COMMON_GROUP_SIZE)
			return invalid("truncated group header");
		type = read_le32(group);
		header_size = read_le16(group + 6);
		group_size = read_le64(group + 8);
		if (!type || header_size < COMMON_GROUP_SIZE ||
		    group_size < header_size || group_size > size - offset)
			return invalid("invalid group bounds");
		offset += (size_t)group_size;

		/* Format 1.0 makes unknown groups optional. */
		if (type != VGPU_TYPE_GROUP)
			continue;
		if (read_le16(group + 4) != 1 || header_size != VGPU_GROUP_SIZE)
			return invalid("unsupported vGPU group version or header size");
		if (read_le16(group + 16) != NVIDIA_VENDOR_ID ||
		    read_le16(group + 20) != NVIDIA_VENDOR_ID)
			return invalid("invalid vGPU group vendor ID");

		info_size = read_le32(group + 24);
		info_count = read_le32(group + 28);
		if (info_size < sizeof(uint32_t) || !info_count ||
		    info_count > VGPUMETA_MAX_RECORDS ||
		    (uint64_t)info_size * info_count != group_size - header_size)
			return invalid("invalid vGPU record layout");
		if (abi_size && abi_size != info_size)
			return invalid("inconsistent vGPU record sizes");
		abi_size = info_size;

		selector = read_le64(group + 16);
		for (j = 0; j < seen; j++) {
			if (selectors[j] == selector)
				return invalid("duplicate PCI selector");
		}
		selectors[seen++] = selector;

		if (read_le16(group + 16) == pci->vendor_id &&
		    read_le16(group + 18) == pci->device_id &&
		    read_le16(group + 20) == pci->subsystem_vendor_id &&
		    read_le16(group + 22) == pci->subsystem_device_id) {
			match.info = group + header_size;
			match.info_size = info_size;
			match.info_count = info_count;
		}
	}

	if (offset != size)
		return invalid("trailing bytes after the last group");
	if (!match.info) {
		fprintf(stderr, "no vGPU group for PCI %04x:%04x:%04x:%04x\n",
			pci->vendor_id, pci->device_id, pci->subsystem_vendor_id,
			pci->subsystem_device_id);
		return -1;
	}

	*records = match;
	*gsp_build_version = version;
	return 0;
}
