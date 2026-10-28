<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: MIT

User-facing documentation for the vgpu-mgmt CLI: describes the subcommand
catalog, fwctl device discovery, and the wire-level RPC contract the tool
uses to drive nova-core GMCAPI from userspace.
-->

# fwctl nova-core GMCAPI tools — `vgpu-mgmt`

`vgpu-mgmt` is a single userspace CLI that wraps `fwctl_cmd` RPC opcodes
exposed by the Linux `fwctl` subsystem on top of NVIDIA's nova-core driver.
Each opcode is reached via a subcommand (see [Command catalog](#command-catalog)).

```
vgpu-mgmt <subcommand> [options]
```

GMCAPI subcommands require `-b <bdf>`, open the matching
`/dev/fwctl/fwctlN` character device, validate it is a nova-core endpoint,
and submit commands through `FWCTL_RPC`.

The kernel-side handler routes the request to the GSP-RM firmware over
MCTP/NVDM and returns the raw response.

Subcommand entry points live in their own translation units
(`nv_*.c`, exposing `cmd_*()` functions); device discovery, BDF parsing,
and the shared GSP preflight RPCs live in `fwctl_common.{c,h}`; the
top-level dispatcher lives in `vgpu-mgmt.c`.

## Transport layer (common to every command)

### Device discovery
1. `opendir("/dev/fwctl")`, iterate `fwctlN` entries.

2. `open(path, O_RDWR)` then `ioctl(fd, FWCTL_INFO, &info)` (`fwctl_info` is
   filled with `info.size = sizeof(info)`).

3. Require `info.out_device_type == FWCTL_DEVICE_TYPE_NOVA_CORE` (= 8)
   and match the device's PCI parent in sysfs to the PF selected by `-b`.
   Close nonmatching descriptors and continue.

Commands take `-b <bdf>` and use it to select the matching nova-core
fwctl device. If the BDF is a VF, its `physfn` is used for PF/fwctl matching.

### RPC submission

```c
struct fwctl_rpc rpc = {
    .size    = sizeof(rpc),
    .scope   = FWCTL_RPC_CONFIGURATION,    // every nova-core RPC uses this scope
    .in_len  = in_size,
    .out_len = out_size,
    .in      = (uint64_t)(uintptr_t)in_buf,
    .out     = (uint64_t)(uintptr_t)out_buf,
};
ioctl(fd, FWCTL_RPC, &rpc);
```

Heap-allocated RPC buffers use `posix_memalign` for 16-byte alignment.
The parameter commands also use `_Alignas(16)` for stack response buffers.
Request headers and padding are zero-initialised. The buffers are laid out as

```
in_buf  : struct fwctl_rpc_nova_core + command-specific IN payload
out_buf : struct fwctl_rpc_nova_core + command-specific OUT payload
```

### Header (`fwctl_rpc_nova_core`)

Matches `include/uapi/fwctl/nova-core.h` in the kernel uAPI; used for both
the IN and OUT buffers.

| Field        | Set to                          |
|--------------|---------------------------------|
| `command_id` | one of `FWCTL_CMD_NOVA_CORE_*`  |
| `reserved`   | 0                               |

The kernel handler wraps the request in the MCTP/NVDM transport layer
(NVDM type `0x26` = GMCAPI, NV vendor `0x10DE`, MCTP vendor-PCI `0x7E`) on
its own; userspace does not need to populate those fields.

A failing `FWCTL_RPC` ioctl (negative return) signals an error from the
kernel or firmware; tools convert that to `EXIT_FAILURE`. On success the
OUT payload follows the 8-byte header.

### Preflight helpers

Several subcommands run cheap GSP RPCs up-front so an obviously-bad input
fails with a specific message instead of an opaque `EIO` from the real
operation. The helpers live in `fwctl_common.c`:

| Helper | Underlying RPC | Used by |
|--------|----------------|---------|
| `vgpu_type_is_supported(fd, type_id)`   | `QUERY_SUPPORTED_VGPU_TYPES` | `assign`, `query-props` |
| `vgpu_type_is_creatable(fd, type_id)`   | `QUERY_CREATABLE_VGPU_TYPES` | `assign` |
| `vgpu_query_assigned_type(fd, dbdf, *)` | `QUERY_ASSIGNED_VF_VGPU_TYPE` | `assign` (refuses to clobber) |

`supported` is static (metadata uploaded). `creatable` is dynamic and,
in practice, narrower than "types with a free VF": once *any* VF on the
PF is bound, GSP locks the PF's placement plan and `creatable` shrinks
to the subset of supported types compatible with that plan. Examples on
a PF with 10 supported types and 4 VFs:

- 0 VFs bound → `creatable` = all 10 supported types.
- 1 VF bound to type `X` → `creatable` typically narrows to `{X}` (or
  the family of types co-resident with `X`); the other 9 supported
  types are now "supported but not currently creatable."
- vGPU mode off, no metadata, or wrong PCI device ID for the uploaded
  metadata → `creatable` empty even with 0 VFs bound.

Running both preflights lets `assign` give specific messages:
"not in the supported list" (wrong type ID / metadata wasn't loaded for
this PCI device) vs. "not in the creatable list" (valid type but not
co-resident with the active placement plan, or mode/metadata missing
entirely).

---

## Command catalog

### 1. `FWCTL_CMD_NOVA_CORE_GMCAPI_ADD_VGPU_TYPE` — command `0x00020001`
**Subcommand:** `vgpu-mgmt add-type`

| Direction | Layout |
|-----------|--------|
| **IN**    | request header + one GMC add-vGPU-type payload |
| **OUT**   | response header only (no payload) |

The metadata is read from a file produced by `vgpu_metadata_tools`. The
subcommand validates the identifier and CRC32, finds the nested
`vgpu_type_blob_hdr` whose `device_id` matches `-p`, then uploads its opaque
vGPU info records one per GMC round. Each round prepends the metadata's GSP
build version and sets `vgpuInfoCount` to one; only the first round sets
`discardVgpuTypes`.

**CLI:**
```
vgpu-mgmt add-type -b <dbdf> -f <metadata-file> -p <pci_device_id>
```

`-b`, `-f`, and `-p` are required.

**Flow:**
1. Open device.
2. `mmap` the metadata file; validate the identifier and CRC32 in the header.
3. For each matching `CONFIG_BLOB_VGPU_TYPE`, validate its nested header and
   GSP RMCTRL section.
4. Build and issue one aligned `FWCTL_RPC` request per vGPU info record.
5. Bail on the first ioctl failure.

### 2. `FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_SUPPORTED_VGPU_TYPES` — command `0x00020002`
**Subcommand:** `vgpu-mgmt list-supported`

| Direction | Layout |
|-----------|--------|
| **IN**    | request header only (no payload) |
| **OUT**   | response header + `NvU32 typeIds[]` |

The OUT array is sized `QUERY_VGPU_TYPES_MAX = 0x40` slots (= `MAX_VGPU_TYPES_PER_PGPU`).
Trailing zero entries indicate the end of the populated portion.
The tool stops printing at the first `typeId == 0`.

**CLI:** `vgpu-mgmt list-supported -b <dbdf>`

**Flow:**
1. Open device.
2. Allocate OUT buffer of
   `sizeof(*resp_hdr) + QUERY_VGPU_TYPES_MAX * sizeof(NvU32)`.
3. Issue `FWCTL_RPC` with `in_len = sizeof(req_hdr)`,
   `out_len = sizeof(*resp_hdr) + 0x40*4`.
4. On ioctl success, walk the array printing each type ID until a zero
   sentinel is hit.

### 3. `FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_CREATABLE_VGPU_TYPES` — command `0x00020003`
**Subcommand:** `vgpu-mgmt list-creatable`

Identical wire shape, OUT buffer sizing, and flow as command `0x00020002`.
The filtering performed by the firmware differs. "Supported" returns every
type the GPU can theoretically host; "creatable" returns those that are
*currently compatible with the PF's locked placement plan*. With zero
VFs bound the two lists match; once any VF is bound, the plan locks and
`creatable` narrows to the co-resident family of types. See the
[Preflight helpers](#preflight-helpers) section for examples.

**CLI:** `vgpu-mgmt list-creatable -b <dbdf>`

### 4. `FWCTL_CMD_NOVA_CORE_GMCAPI_ASSIGN_VGPU_TYPE` — command `0x00020004`
**Subcommand:** `vgpu-mgmt assign`

| Direction | Layout |
|-----------|--------|
| **IN**    | request header + `struct GmcapiAssignVgpuTypeInParams` |
| **OUT**   | response header only |

`GmcapiAssignVgpuTypeInParams`:
```c
NvU64 dbdf;          // VF identifier (domain<<32 | bus<<8 | device<<3 | function)
NvU32 vgpuTypeId;    // from QUERY_CREATABLE_VGPU_TYPES
NvU32 swizzId;       // reserved; pass 0 (MIG hook)
NvU16 placementId;   // optional placement hint, 16-bit
```

**CLI:**
```
vgpu-mgmt assign -b <dbdf> -t <type_id> [-s <swizz>] [-p <placement>]
```

`-b` accepts both `dddd:bb:dd.f` and `bb:dd.f` forms (domain defaults to 0)
via `parse_dbdf()` in `fwctl_common.c`.

**Flow:**
1. Parse and validate args (`got_dbdf && got_type` required;
   `placement_id` must fit in 16 bits).
2. Validate `-b` is an NVIDIA VF (`check_nvidia_vendor_dbdf`,
   `check_is_vf_dbdf`).
3. Open device.
4. Preflight chain — bail with a specific error if any fails:
   - `vgpu_type_is_supported(-t)` — rejects unknown type IDs.
   - `vgpu_type_is_creatable(-t)` — rejects supported-but-not-creatable
     types (most commonly: a different vGPU type is already bound to
     a sibling VF and the locked placement plan doesn't permit `-t`;
     also: vGPU mode off, or the metadata uploaded for this PF doesn't
     cover `-t`).
   - `vgpu_query_assigned_type(-b)` — refuses to clobber a VF that
     already has a type bound.
5. Build IN buffer, populate the params struct, zero-fill the rest.
6. Issue `FWCTL_RPC`. On success the firmware now considers the VF
   claimed by that vGPU type until DEASSIGN.

### 5. `FWCTL_CMD_NOVA_CORE_GMCAPI_DEASSIGN_VGPU_TYPE` — command `0x00020005`
**Subcommand:** `vgpu-mgmt deassign`

| Direction | Layout |
|-----------|--------|
| **IN**    | request header + `struct GmcapiDeassignVgpuTypeInParams` (`{ NvU64 dbdf; }`) |
| **OUT**   | response header only |

**CLI:** `vgpu-mgmt deassign -b <dbdf>`

**Flow:** symmetric to ASSIGN but with no type-id needed — the firmware
already tracks which vGPU type currently owns this VF and releases it.

### 6. `FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_VGPU_PROPERTIES` — command `0x00020006`
**Subcommand:** `vgpu-mgmt query-props`

| Direction | Layout |
|-----------|--------|
| **IN**    | request header + `struct GmcapiQueryVgpuPropertiesInParams` (`{ NvU32 vgpuTypeId; }`) |
| **OUT**   | response header + NVKV-encoded `NvU64[]` stream |

The OUT payload is a self-describing NVKV (key/value) stream.
Worst-case wire size: 39 NvU64 words = 312 bytes
(see `VGPUCONFIG_GMCAPI_QUERY_VGPU_NVKV_MAX_OUT_WORDS`).

**CLI:** `vgpu-mgmt query-props -b <dbdf> -t <type_id>`

**Flow:**
1. Parse `-b` and `-t`.
2. Open device.
3. Preflight: `vgpu_type_is_supported(-t)` — bail with "not in the
   supported list" if the type is unknown to GSP.
4. Allocate OUT buffer sized for the 39-word NVKV upper bound.
5. Build IN buf with `vgpuTypeId`.
6. Issue `FWCTL_RPC`.
7. On success, hand the NVKV stream to `nvkvDecode()` (from `nvkv.{h,c}`)
   with `vgpu_props_key_handler` as the callback. The handler uses
   `NVKV_CASE_VAR` / `NVKV_CASE_STRING8` to populate `nvidia_get_vgpu_properties`.
8. `print_vgpu_type()` walks the populated record. If the decoded
   `vgpuTypeId` is `0` (GSP returned an empty record despite the
   preflight letting the type through), an extra
   `(no properties returned -- GSP does not recognize this type)`
   line is appended.

### 7. `FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_ASSIGNED_VF_VGPU_TYPE` — command `0x00020007`
**Subcommand:** `vgpu-mgmt query-vf`

| Direction | Layout |
|-----------|--------|
| **IN**    | request header + `struct GmcapiQueryAssignedVfVgpuTypeInParams` (`{ NvU64 dbdf; }`) |
| **OUT**   | response header + `struct GmcapiQueryAssignedVfVgpuTypeOutParams` |

`GmcapiQueryAssignedVfVgpuTypeOutParams`:
```c
NvU32 vgpuTypeId;    // 0 if no vGPU type is assigned to this VF
NvU32 swizzId;       // reserved
NvU16 placementId;   // reserved
```

**CLI:** `vgpu-mgmt query-vf -b <dbdf>`

**Flow:**
1. Parse `-b`.
2. Open device.
3. Build IN buf with `dbdf`; build OUT buf sized for the OUT struct.
4. Issue `FWCTL_RPC`.
5. Cast `out_buf + sizeof(*resp_hdr)` to the OUT params and print
   `vgpuTypeId`. A returned `0` is printed as `(VF is unassigned)`
   on the line below.

### 8. `vgpu-mgmt query-vgpu-mode` / `set-vgpu-mode`

These two subcommands are *not* GMCAPI RPCs. They poke the FSP PRC
knob directly via an `mmap` of the PF's BAR0 (so they require a PF
dbdf, not a VF). Wiring:

| Subcommand | Action |
|------------|--------|
| `query-vgpu-mode -b <pf-dbdf>` | reads the current vGPU-mode PRC knob |
| `set-vgpu-mode   -b <pf-dbdf> -m <mode>` | writes a new value to the same knob |

The PF gate (`check_is_pf_dbdf`) is required because only the PF
exposes the FSP mailbox in BAR0; running these against a VF reads
invalid registers (typically faults).

---

### 9. `FWCTL_CMD_NOVA_CORE_GMCAPI_SET_PARAMS` — command `0x00050001`

**Subcommand:** `vgpu-mgmt set-params`

`SET_PARAMS` uses a common envelope followed by an NVKV payload. The `-b`
BDF selects the matching nova-core FWCTL device and is sent unchanged as
`dbdf`. It must be the actual nonzero PF or VF DBDF; GSP derives the
target scope and validates ownership against the selected PF. Scheduler
parameters currently require a PF BDF.

Parameter IDs are shared by GET and SET within each target namespace.
`gmcapi_pgpu_params_param_id` defines `SCHED_CAPS = 1`, `SCHED_STATE = 2`
and `SCHED_LOG = 3`; `gmcapi_vgpu_params_param_id` currently contains only
`INVALID = 0`. Separate GET and SET descriptor tables determine which
operations a parameter supports. SCHED_STATE supports both; SCHED_CAPS and
SCHED_LOG currently support GET only.

SET SCHED_STATE now uses parameter ID **2**, matching GET SCHED_STATE.
Use matching FWCTL and GSP firmware; older GSP firmware expects SET ID 1.

The first registered parameter is `sched-state`:

```sh
vgpu-mgmt set-params -b 0000:9b:00.0 -P sched-state \
    --policy 2 \
    -asf 480 -aavg 8
```

This follows production NVML's non-MIG `cDeviceSetVgpuObjschedState_v2`
contract. The tool translates the user policy before sending it; the user
and RM policy enumerations are not interchangeable.

| Input | Meaning and validation | Wire value |
|-------|------------------------|------------|
| `-b`, `--bdf` | Required NVIDIA PF BDF for scheduler parameters; selects the FWCTL endpoint | Actual packed PF `dbdf` |
| `-P`, `--param sched-state` | Required parameter descriptor | `GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_STATE = 2` |
| `-p 1` | Best Effort; rejects either ARR option, including explicit zero | RM policy `2` |
| `-p 2` | Equal Share; accepts optional ARR inputs | RM policy `3` |
| `-p 3` | Fixed Share; accepts optional ARR inputs | RM policy `4` |
| `-asf`, `--arr-frequency` | Optional U32 frequency in Hz; nonzero must be within GPU-reported limits (currently 63..960) | `NVGMC_PARAMS_SCHED_STATE_FREQUENCY_FOR_ARR` |
| `-aavg`, `--arr-avg-factor` | Optional U32 averaging factor; nonzero must be within GPU-reported limits (currently 1..60) | `NVGMC_PARAMS_SCHED_STATE_AVG_FACTOR_FOR_ARR` |

The scheduler engine is always `NV2080_ENGINE_TYPE_GRAPHICS` (`GR0`, value
`1`); there is no engine, GI, timeslice, or ARR enable/disable CLI option.
`-p` / `--policy` is required; omitting it does not select a default policy.
Negative, overflowing, malformed and duplicate inputs are rejected.
An omitted ARR key is not encoded. An explicit zero is encoded and selects
the same firmware defaults: default frequency/timeslices and averaging
factor 33, rather than retaining the previous setting.

After validating CLI syntax and the required policy, the tool sends read-only `GET_PARAMS`
(`0x00050002`) for pGPU `SCHED_STATE` (`paramId = 2`) and GR0. If
`NVGMC_PARAMS_SCHED_STATE_ACTIVE_VGPU_COUNT` is nonzero, it fails with a
message identifying the active count and does not send SET. For example:

```text
error: vGPU scheduler state cannot be configured while vGPU instance is active on this GPU [vGPU count = 1]; stop all vGPU instances and retry
```

With zero active vGPUs, it checks that any ARR options require Equal Share
(`-p 2`) or Fixed Share (`-p 3`), including when those options are zero. Thus
an active-vGPU error takes precedence over the Best Effort/ARR error.
It then queries pGPU `SCHED_CAPS` (`paramId = 1`) and
validates the returned schema, policy support, ARR support, and limits,
matching NVML's capability validation. Only then does it submit `SET_PARAMS`
(`0x00050001`) with the common 16-byte parameter envelope and two to four
NVKV words. A failed or malformed preflight response also prevents SET.
Permission checks for active vGPUs, channels and scheduler state remain
authoritative in GSP-RM, including when a vGPU starts after the preflight.

Both command IDs must be allowed under `FWCTL_RPC_CONFIGURATION` by the
running Nova driver, and the GSP firmware must support the current common
GET/SET_PARAMS ABI. Rebuild and load the updated Nova driver before using
this command; changing only the userspace binary will otherwise fail with
`EPERM`. Firmware rejection is currently surfaced by Nova as `EIO`; the
FWCTL response header does not contain a separate RM status field.

SET returns **status only**, not scheduler settings or an NVKV output
payload. For the example above, successful completion prints:

```text
SET_PARAMS sched-state: OK
```

With default ARR values:

```sh
vgpu-mgmt set-params -b 0000:9b:00.0 -P sched-state -p 3
```

That request contains two NVKV words (engine and policy). Read back
effective settings separately; the success message is not a readback.
The descriptor table in `nv_set_params.c` remains the extension point for
future pGPU parameter encoders and optional preflight checks. No vGPU SET
parameter is registered.

### 10. `FWCTL_CMD_NOVA_CORE_GMCAPI_GET_PARAMS` — command `0x00050002`

**Subcommand:** `vgpu-mgmt get-params`

```sh
vgpu-mgmt get-params -b 0000:9b:00.0 -P sched-caps
vgpu-mgmt get-params -b 0000:9b:00.0 -P sched-state
vgpu-mgmt get-params -b 0000:9b:00.0 -P sched-log
```

Parameter names are exact: use `sched-log` (singular). `sched-logs` is
unsupported and is rejected by the CLI before any RPC is sent.

Both `-b` / `--bdf` and `-P` / `--param` are required. The BDF must identify
an NVIDIA PF and selects one pGPU. Like production NVML's pGPU scheduler
queries, the engine is always `NV2080_ENGINE_TYPE_GRAPHICS` (GR0, value 1).
There are no policy, ARR, engine or GI input options for these queries.

GET and SET infer the target from `-b`; `-t` and `--target` are not
accepted. Descriptors still declare their supported PF/VF scope, so scheduler
parameters reject a VF before sending any RPC. There are no registered vGPU
parameter handlers yet. Future vGPU parameters need their own descriptor,
handler and VF BDF validation; the existing FWCTL device lookup resolves a
VF's `physfn` to select its owning PF endpoint.

GET, SET and SET's preflight queries pass the same actual nonzero DBDF
through `struct fwctl_params_target` to the shared transport. The 16-byte
wire envelope has only `dbdf` and `paramId` as named header fields.
`paramId` is at byte 8; the aligned NVKV word array starts at byte 16, with
compiler padding between them. This parameter envelope has no reserved field
or explicit target type; the separate 8-byte FWCTL header retains its zero
`reserved` field. Zero or noncanonical DBDFs are rejected. Use matching
updated FWCTL and GSP firmware: older firmware
expects an explicit `targetType` and zero for pGPU requests. Builds with the
previous explicit reserved field also use a different `paramId` offset and
must be updated together.

CAPS and STATE each send one GET_PARAMS RPC. Every scheduler query consists of
the 8-byte FWCTL header, the 16-byte common parameter envelope, and one
NVKV word with key `NVGMC_PARAMS_SCHED_STATE_ENGINE_ID`, index zero, and
value `NV2080_ENGINE_TYPE_GRAPHICS` (`1`).
The envelope uses the actual PF `dbdf`. Parameter IDs are:

| CLI parameter | shared pGPU parameter ID | Response payload |
|---------------|-----------------------|------------------|
| `sched-caps` | `GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_CAPS = 1` | Ten NVKV words / 80 bytes |
| `sched-state` | `GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_STATE = 2` | Five NVKV words / 40 bytes |
| `sched-log` | `GMCAPI_PGPU_PARAMS_PARAM_ID_SCHED_LOG = 3` | Six metadata words + 17 words per entry; 48..13,648 bytes |

Here a word is one `NvU64` (8 bytes), including NVKV instruction words.
For SCHED_LOG the maximum is `6 + 100 * 17 = 1,706` words. Its six metadata
values each use one IMM32 word; each entry uses five words for four U64
fields, three words for four packed U32 fields, and nine words for the eight
U64 counters and their array header. These sizes describe the encoded stream,
not the decoded C structures.

The FWCTL response adds its 8-byte header to those payload sizes. Responses
are validated in full before any result is printed: missing, duplicate,
unknown, incorrectly indexed, mistyped or truncated fields cause failure.
GET and SET share the transport and scheduler decoders in `nv_params.c`.
The SET command reuses both queries for its active-vGPU and CAPS validation.

`sched-caps` follows `nvmlDeviceGetVgpuSchedulerCapabilities()` and the
`nvidia-smi vgpu -sc` field set. It prints all three supported-policy slots,
ARR support, maximum/minimum timeslices in **nanoseconds**, scheduling
frequency limits in **Hz**, and ARR averaging-factor limits. Firmware RM
policies are displayed as Unknown, Best Effort, Equal Share or Fixed Share;
raw RM policy numbers are not presented as CLI policy numbers.

`sched-state` follows `nvmlDeviceGetVgpuSchedulerState_v2()` and the
`nvidia-smi vgpu -ss` display rules. For example, an active Fixed Share
configuration could print:

```text
GPU 0000:9B:00.0
    Active vGPUs                    : 2
    Scheduler Policy                : Fixed Share
    Average Factor                  : 33
    Time Slice(ns)                  : 2000000
```

The active count comes from `NVGMC_PARAMS_SCHED_STATE_ACTIVE_VGPU_COUNT`
in the same response. With zero active vGPUs, or policy Best Effort/Unknown,
only the GPU, active count and policy are printed; runtime timeslice and
average fields are omitted, matching production display behavior. The
response's ARR-enabled SDK enum is validated, but is not an additional v2
display field. Current frequency is not returned by GET_STATE and is not
inferred from the runtime timeslice. The heading uses the selected PF BDF
rather than an NVML device index.

`sched-log` follows the weighted-scheduler `nvidia-smi vgpu -sl` display and
polling behavior. It consumes ten batches of up to 100 unread records per
cycle (up to 1,000 rows), then waits one second and repeats until Ctrl-C or
SIGTERM. Change the delay with `-l` / `--loop` (1..10 seconds). Use `--once`
for one ten-read cycle without waiting:

```sh
vgpu-mgmt get-params -b 0000:9b:00.0 -P sched-log --once
vgpu-mgmt get-params -b 0000:9b:00.0 -P sched-log -l 2
```

These polling options apply only to `sched-log`; CAPS and STATE remain
single queries and reject `--once` or `--loop`. A cycle performs ten
SCHED_STATE preflight queries and, when the policy supports logging, ten
SCHED_LOG reads. `--once` is one complete cycle, not one RPC. Best Effort or
Unknown skips the log reads and prints the policy message for each batch.
The engine stays fixed at GR0. Each batch prints the GPU
BDF, engine ID, scheduler policy, averaging factor and timeslice, followed
by the same entry fields as weighted `-sl`:

| Entry column | Value |
|--------------|-------|
| GPU | Selected PF BDF in uppercase |
| SW Runlist Id | Scheduler VM-set/runlist identifier |
| Weight(0x) | Raw hexadecimal scheduler weight |
| Timestamp(ns) | Firmware timestamp |
| Cumulative Run Time(ns) | Signed cumulative runtime, preserving the SDK NvS64 value |
| Prev Timeslice Runtime(ns) | Runtime of the previous timeslice |
| Target Time Slice(ns) | Target timeslice for that entry |
| Cumulative Preempt Time(ns) | Accumulated preemption debt |

The decoder also preserves cumulative idle time and all eight U64 counters
from the wire; those are not normal `-sl` display columns. Entries retain
their firmware order and nanosecond units; rows are not synthesized.

Before each batch, the tool queries SCHED_STATE for policy and active-vGPU
count, since SCHED_LOG's COUNT is the number of records returned. Best
Effort/Unknown prints the current policy and explains that logging is not
supported, without issuing a failing log read. For logging-capable policies
with zero active vGPUs, the log is still read and consumed. The tool prints
the engine, policy, and `GPU <BDF> has no active VM runlist.` without runtime
fields or rows.
An active GPU with an empty buffer prints metadata and column headings with
no rows. State and log reads are separate snapshots; firmware/transport
errors, including a concurrent policy change that rejects a read, stop the
command with failure. Completed earlier batches may already be printed.

The full reply capacity is reserved even for empty reads: 13,648 NVKV bytes
plus the 8-byte FWCTL header, or 13,656 bytes total. Counts over 100, missing
or duplicate fields, entry-index gaps, entries beyond COUNT, wrong engine,
invalid policy/ARR enums and malformed counter arrays are rejected before
printing the batch. Log reads consume the same circular buffer as RM/NVML;
another consumer such as `nvidia-smi -sl` can drain entries concurrently.

This reply fits Nova's existing 16 KiB response budget, so no Nova driver
response-limit change is required. Use the updated GSP firmware with its
100-entry GMCAPI limit and the matching userspace header/decoder. The GSP
applies this limit before decrementing the unread-record count; remaining
records are available to later calls rather than discarded after a read.
Production RM/NVML retains 200 entries per read and five reads per cycle;
the GMCAPI path uses ten smaller reads for the same 1,000-entry cycle.

The running Nova driver must allow GET_PARAMS under the configuration
scope, and GSP-RM must implement this ABI and be in vGPU-host mode.
An ioctl or firmware error returns a nonzero exit status; Nova currently
maps a nonzero GSP status to `EIO`, so a detailed RM status is not available
in the FWCTL reply. CAPS and STATE print the requested data and exit zero;
they do not consume scheduler logs. SCHED_LOG consumes unread entries and
polls until stopped, or exits after one cycle with `--once`. None of these
GET commands changes scheduler settings.

---

## Build / run

```sh
cd vgpu-tools/fwctl
make                                    # builds the single vgpu-mgmt binary
./vgpu-mgmt                             # top-level usage / subcommand list
./vgpu-mgmt list-supported -b 0000:9b:00.0
./vgpu-mgmt query-props -b 0000:9b:00.0 -t 1
./vgpu-mgmt assign -h                   # subcommand-specific help
./vgpu-mgmt set-params -h                # pGPU SET_PARAMS help
./vgpu-mgmt get-params -h                # pGPU GET_PARAMS help
```

`make clean` removes the `vgpu-mgmt` and test binaries. The Makefile is a one-shot
build: every `.c` is recompiled and linked in a single `gcc` invocation,
so no intermediate `.o` or `.d` files are produced.

## Files

| Area                  | Files |
|-----------------------|-------|
| Transport / wire      | `fwctl.h` (Linux UAPI, ioctls), `nvrm.h` (`fwctl_rpc_nova_core` header, opcodes) |
| GMCAPI types          | `nv_vgpu.h` (vGPU params/NVKV keys) and `gmcapi_params.h` (common GET/SET envelope, target-specific parameter IDs, scheduler NVKV keys) |
| Metadata / RM headers | `include/metadata.h` (vGPU metadata binary layout: `METADATA_IDR`, `metadata_hdr`, `metadata_blob_hdr`, `CONFIG_BLOB_VGPU_TYPE`, `crc32_le`); consumed by `add-type` to walk the metadata file produced by that tool |
| NVKV (libraries fork) | `nvkv.h`, `nvkv.c`, `nvkv_stub_defs.h` (userspace stubs for kernel `portMem*`/`REF_*`/`NV_*` machinery) |
| Dispatcher            | `vgpu-mgmt.c` (top-level `main`, subcommand table) |
| Shared helpers        | `fwctl_common.{c,h}` — device discovery (`open_nova_core_fwctl_for_dbdf`), BDF parsing (`parse_dbdf`), PF/VF + vendor gates (`check_*_dbdf`), GSP preflights (`vgpu_query_assigned_type`, `vgpu_type_is_supported`, `vgpu_type_is_creatable`), `QUERY_VGPU_TYPES_MAX`, and the `cmd_*` declarations |
| GET/SET parameter helpers | `nv_params.c`, `fwctl_params.h` — shared parameter RPC submission, scheduler CAPS/STATE/LOG decoding and policy names |
| Parameter subcommands | `nv_set_params.c`, `nv_get_params.c` — SET validation/encoding and GET display/polling |
| Parameter tests | `tests/test_set_params.c`, `tests/test_get_params.c`, `tests/test_sched_log.c`, `tests/test_params_transport.c`, `tests/test_params_probe.c`, `tests/test_params_inputs.py` and `tests/params_rpc_probe.c` |
| Subcommand sources    | one `nv_*.c` per opcode, each exporting a `cmd_*` entry point |
| FSP PRC subcommands   | `nv_query_vgpu_mode.c`, `nv_set_vgpu_mode.c`, `prc_knob.{c,h}` — direct BAR0 mailbox path, not an RPC |
