# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Build rules for the vgpu-mgmt userspace CLI: compiles the per-subcommand
# .c files plus shared helpers (fwctl_common, nvkv, prc_knob) into a single
# statically linked binary against the in-tree headers under include/.

CC := gcc
PYTHON ?= python3
CFLAGS := -O2 -Iinclude
LDFLAGS :=

TARGET := vgpu-mgmt
TEST_TARGETS := tests/test-set-params tests/test-get-params tests/test-sched-log \
	tests/test-params-transport tests/params-rpc-probe tests/test-params-probe
PARAMS_COMMON_SRCS := nv_params.c fwctl_common.c nvkv.c
TEST_WRAP := -Wl,--wrap=ioctl,--wrap=close,--wrap=posix_memalign \
	-Wl,--wrap=check_nvidia_vendor_dbdf,--wrap=check_is_pf_dbdf \
	-Wl,--wrap=open_nova_core_fwctl_for_dbdf

SRCS := \
	vgpu-mgmt.c \
	fwctl_common.c \
	nv_add_vgpu_type.c \
	nv_assign_vgpu_type.c \
	nv_deassign_vgpu_type.c \
	nv_query_supported_vgpu_types.c \
	nv_query_creatable_vgpu_types.c \
	nv_query_assigned_vf_vgpu_type.c \
	nv_query_vgpu_properties.c \
	nv_set_params.c \
	nv_get_params.c \
	nv_params.c \
	nv_query_vgpu_mode.c \
	nv_set_vgpu_mode.c \
	prc_knob.c \
	nvkv.c

all: $(TARGET)

.PHONY: all test test-params-inputs test-params-inputs-sanitize clean

$(TARGET): $(SRCS) $(wildcard include/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

tests/test-set-params: tests/test_set_params.c nv_set_params.c $(PARAMS_COMMON_SRCS) $(wildcard include/*.h)
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ tests/test_set_params.c nv_set_params.c $(PARAMS_COMMON_SRCS) $(TEST_WRAP) $(LDFLAGS)

tests/test-get-params: tests/test_get_params.c nv_get_params.c $(PARAMS_COMMON_SRCS) $(wildcard include/*.h)
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ tests/test_get_params.c nv_get_params.c $(PARAMS_COMMON_SRCS) $(TEST_WRAP) $(LDFLAGS)

tests/test-sched-log: tests/test_sched_log.c nv_get_params.c $(PARAMS_COMMON_SRCS) $(wildcard include/*.h)
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ tests/test_sched_log.c nv_get_params.c $(PARAMS_COMMON_SRCS) $(TEST_WRAP) -Wl,--wrap=nanosleep $(LDFLAGS)

tests/test-params-transport: tests/test_params_transport.c $(PARAMS_COMMON_SRCS) $(wildcard include/*.h)
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ tests/test_params_transport.c $(PARAMS_COMMON_SRCS) -Wl,--wrap=ioctl $(LDFLAGS)

tests/params-rpc-probe: tests/params_rpc_probe.c fwctl_common.c nvkv.c $(wildcard include/*.h)
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ tests/params_rpc_probe.c fwctl_common.c nvkv.c $(LDFLAGS)

tests/test-params-probe: tests/test_params_probe.c tests/params_rpc_probe.c fwctl_common.c nvkv.c $(wildcard include/*.h)
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ tests/test_params_probe.c fwctl_common.c nvkv.c $(TEST_WRAP) $(LDFLAGS)

tests/test-params-probe-sanitize: tests/test_params_probe.c tests/params_rpc_probe.c fwctl_common.c nvkv.c $(wildcard include/*.h)
	$(CC) -O1 -g -Iinclude -Wall -Wextra -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ tests/test_params_probe.c fwctl_common.c nvkv.c $(TEST_WRAP) $(LDFLAGS)

test-params-inputs-sanitize: tests/test-params-probe-sanitize
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./tests/test-params-probe-sanitize

test-params-inputs: tests/params-rpc-probe tests/test-params-probe
	./tests/test-params-probe
	$(PYTHON) tests/test_params_inputs.py --self-test

test: $(TEST_TARGETS)
	./tests/test-set-params
	./tests/test-get-params
	./tests/test-sched-log
	./tests/test-params-transport
	./tests/test-params-probe
	$(PYTHON) tests/test_params_inputs.py --self-test

clean:
	rm -f $(TARGET) $(TEST_TARGETS) tests/test-params-probe-sanitize
