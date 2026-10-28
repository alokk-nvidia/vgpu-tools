# Raw FWCTL SET_PARAMS / GET_PARAMS input tests

These tests bypass `vgpu-mgmt`'s input validation and construct the actual bytes
submitted through `ioctl(FWCTL_RPC)`. They use the current request layout:

```text
FWCTL nova-core header: command_id U32, reserved U32
GMCAPI parameters:      dbdf U64, paramId U32, compiler padding 4 bytes
NVKV:                  U64 words, starting 16 bytes after the GMCAPI header
```

The `reserved` field above belongs to the existing kernel FWCTL UAPI, not the
GMCAPI parameter structure. The latter has no reserved field or target selector.

## Build and offline tests

From the `fwctl` directory:

```sh
make test-params-inputs
make test-params-inputs-sanitize
python3 tests/test_params_inputs.py --list -b 0000:9b:00.0 \
    --vf-bdf 0000:9b:00.4
```

`make test` also builds the raw probe and runs the offline tests alongside the
existing SET, GET, scheduler-log and transport tests. Offline tests check the
wire layout, matrix generation, baseline gating, response parser and log monitor;
they do not execute GSP firmware and are not evidence of firmware crash safety.
The sanitizer target runs the raw C probe's buffer/allocation/ioctl tests under
AddressSanitizer and UndefinedBehaviorSanitizer. It does not inject requests
into the live GPU or instrument GSP firmware.

## Live execution

```sh
sudo python3 tests/test_params_inputs.py --run -b 0000:9b:00.0 \
    --vf-bdf 0000:9b:00.4 --report /tmp/params-input-results.json
```

The PF BDF selects the existing Nova FWCTL endpoint. `--vf-bdf`, when provided,
must resolve through `physfn` to that PF. It adds cases that send a valid VF DBDF
with pGPU-only parameter IDs. No VM is started, stopped, or rebooted by this test.

Before any malformed request, the runner requires successful GET_STATE and
GET_CAPS requests with response sizes and schemas matching this interface. If
that baseline fails, execution stops with exit code 2 and a JSON report stating
that crash behavior was **not assessed**. Do not treat a failed baseline as a
successful negative-input test.

After each executed case, it sends a known-good GET_STATE request, checks that
the scheduler policy did not change, and reads newly appended `/var/log/syslog`
entries. The runner stops on unexpected acceptance/rejection, probe timeout,
health-query failure, policy change, or new watchdog/HALT/fatal messages. The
report retains the request bytes, ioctl errno, returned length, health reply,
and relevant log lines for each executed case.

The default matrix contains malformed SETs and GETs plus read-only successful
GET encoding corners. It never intentionally sends a valid scheduler SET.
`--include-log` additionally runs successful, consuming SCHED_LOG reads; those
reads share the production scheduler log buffer with other consumers.

## Coverage

- Zero, foreign-domain, foreign-bus, noncanonical and maximum DBDFs; optional
  valid-VF/pGPU-parameter mismatches.
- Invalid IDs, SET requests using GET-only IDs, and maximum ID values.
- Every truncated parameter-header length; empty and non-word-sized payloads.
- Missing, duplicated, unknown, indexed, response-only and wrongly typed keys.
- All NVKV count-bearing opcodes with zero, truncated, large and maximum counts.
- Invalid engines, policy wire values, ARR boundaries and duplicate ARR keys.
- Alternative valid scalar encodings, ignored zero-length sequences and
  nonzero unnamed header padding.
- Outer FWCTL header truncation/reserved validation and small output buffers.

Small output buffers are successful truncation tests: FWCTL copies only the
prefix that fits and reports the full response length in `out_len`. The runner
checks that full length and compares the copied prefix with the baseline GET
response. A zero-byte output buffer therefore succeeds and reports the required
size; it is not expected to reject the RPC.

## Interpreting crash and error results

- Nova currently turns nonzero GSP status into `EIO`. The raw `NV_STATUS` is
  unavailable to this tool; `EIO` alone cannot establish the exact rejection
  reason or prove which handler ran.
- Some malformed requests are rejected in the kernel before reaching GSP.
  Those cases test transport validation, not firmware parsing. EPERM, ENOTTY,
  and ENODEV stop the run rather than count as successful firmware rejection.
- The current Nova driver advertises a fixed response budget to GSP. Changing
  this test's userspace output buffer does not directly change GSP's
  `outKvCapacity`; small-buffer cases exercise the FWCTL response boundary.
- New watchdog/HALT messages are correlated with the test window, not proof
  of causation. A timeout or failed subsequent health query is reported as
  possible health loss, not automatically as a confirmed firmware crash.
- If syslog cannot be read, the report explicitly marks log coverage missing.
  Passing a finite matrix cannot prove that every possible input is crash-safe.

Exit codes: 0 = all executed cases passed; 1 = failed case or health/event check;
2 = baseline/setup blocked. The suite performs no automatic recovery or reset.
