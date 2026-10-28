#!/usr/bin/env python3
"""Raw FWCTL SET/GET input matrix. Live execution requires --run and a healthy ABI."""
import argparse
import errno
import json
import re
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

GET, SET = 0x00050002, 0x00050001
CAPS, STATE, LOG = 1, 2, 3
LOG_CAPACITY = 8 + (6 + 100 * 17) * 8
EVENT = re.compile(r"LibOS Task Watchdog|GSP.*(?:watchdog|HALT|fatal)|"
                   r"GPU.*fallen off the bus|Xid.*(?:119|120)", re.I)


def parse_bdf(text):
    match = re.fullmatch(r"(?:(\w+):)?([0-9a-fA-F]{1,2}):"
                         r"([0-9a-fA-F]{1,2})\.([0-7])", text)
    if not match:
        raise ValueError("invalid PCI BDF: " + text)
    try:
        domain, bus, device, function = [int(x or "0", 16) for x in match.groups()]
    except ValueError:
        raise ValueError("invalid PCI BDF: " + text)
    if domain > 0xffffffff or device > 31:
        raise ValueError("PCI BDF field out of range")
    dbdf = domain << 32 | bus << 8 | device << 3 | function
    if not dbdf:
        raise ValueError("zero DBDF is not a target")
    return dbdf


def canonical(dbdf):
    return "{:04x}:{:02x}:{:02x}.{:x}".format(
        dbdf >> 32, dbdf >> 8 & 255, dbdf >> 3 & 31, dbdf & 7)


def words(*values):
    return b"".join(struct.pack("<Q", value) for value in values)


def imm(key, value, index=0):
    return value << 32 | index << 16 | key


def counted(opcode, key, count, index=0):
    return count << 32 | opcode << 28 | index << 16 | key


ENGINE = imm(1, 1)
POLICY = imm(2, 3)  # SDK Equal Share; CLI policy 2 is a different enumeration.


def frame(command, dbdf, param, payload=b""):
    # Outer FWCTL nova-core header, then the 16-byte GMCAPI parameter header.
    return struct.pack("<IIQI4x", command, 0, dbdf, param) + payload


def case(name, command, dbdf, param, payload=b"", expected="reject",
         layer="gsp", capacity=LOG_CAPACITY):
    return dict(name=name, command=command, frame=frame(command, dbdf, param, payload),
                expected=expected, layer=layer, capacity=capacity)


def input_cases(dbdf, vf=None, include_log=False):
    result = []
    for command, param in [(GET, CAPS), (GET, STATE), (GET, LOG), (SET, STATE)]:
        label = ("GET" if command == GET else "SET") + "/" + str(param)
        valid = words(ENGINE, POLICY) if command == SET else words(ENGINE)
        for name, invalid in [("zero", 0), ("foreign-domain", dbdf ^ (1 << 32)),
                              ("foreign-bus", dbdf ^ (1 << 8)),
                              ("reserved-bit-16", dbdf | 1 << 16),
                              ("reserved-bit-31", dbdf | 1 << 31),
                              ("all-ones", 0xffffffffffffffff)]:
            result.append(case(label + "/dbdf-" + name, command, invalid, param, valid))
        if vf is not None:
            result.append(case(label + "/vgpu-param-mismatch", command, vf, param, valid))
        for invalid in [0, 99, 0xffffffff] + ([CAPS, LOG] if command == SET else []):
            result.append(case(label + "/param-" + str(invalid), command, dbdf, invalid, valid))
        header = frame(command, dbdf, param)
        for length in range(16):
            entry = case(label + "/header-bytes-" + str(length), command, dbdf, param)
            entry["frame"] = header[:8 + length]
            result.append(entry)
        result.append(case(label + "/empty-nvkv", command, dbdf, param))
        for length in range(1, len(valid)):
            result.append(case(label + "/nvkv-bytes-" + str(length), command,
                               dbdf, param, valid[:length]))
        for name, payload in [
                ("engine-zero", words(imm(1, 0), POLICY) if command == SET else words(imm(1, 0))),
                ("engine-max", words(imm(1, 0xffffffff), POLICY) if command == SET else words(imm(1, 0xffffffff))),
                ("duplicate-engine", valid + words(ENGINE)),
                ("indexed-engine", words(imm(1, 1, 4095), POLICY) if command == SET else words(imm(1, 1, 4095))),
                ("unknown-key", valid + words(imm(0xffff, 1))),
                ("key-zero", valid + words(imm(0, 1))),
                ("response-only-key", valid + words(imm(7, 1))),
                ("bad-opcode", valid + words(0xf << 28 | 1)),
                ("many-duplicate-keys", valid + words(*([ENGINE] * 1024)))]:
            result.append(case(label + "/" + name, command, dbdf, param, payload))
        for opcode in range(1, 6):
            for count in [0, 1, 2, 0x7fffffff, 0xffffffff]:
                # Only the instruction word is present: zero count cannot
                # supply the required ENGINE_ID; nonzero counts are truncated.
                result.append(case(label + "/opcode-{}-count-{}".format(opcode, count),
                                   command, dbdf, param, words(counted(opcode, 1, count))))
        for opcode, count, data in [(2, 1, [1]), (3, 4, [1]),
                                     (4, 2, [1]), (5, 1, [1])]:
            payload = words(counted(opcode, 1, count), *data)
            if command == SET:
                payload += words(POLICY)
            result.append(case(label + "/wrong-type-{}-{}".format(opcode, count),
                               command, dbdf, param, payload))
        for value in [0, 1, 0xffffffff]:
            result.append(case(label + "/padding-" + str(value), command, dbdf, param,
                               valid + words(imm(0xffff, value))))

    for policy in [0, 1, 5, 0xffffffff]:
        result.append(case("SET/invalid-policy-" + str(policy), SET, dbdf, STATE,
                           words(ENGINE, imm(2, policy))))
    for key, values in [(3, [1, 62, 961, 0xffffffff]), (4, [61, 0xffffffff])]:
        for value in values:
            result.append(case("SET/arr-{}-{}".format(key, value), SET, dbdf, STATE,
                               words(ENGINE, POLICY, imm(key, value))))
        result.append(case("SET/duplicate-arr-" + str(key), SET, dbdf, STATE,
                           words(ENGINE, POLICY, imm(key, 0), imm(key, 0))))
        result.append(case("SET/best-effort-arr-" + str(key), SET, dbdf, STATE,
                           words(ENGINE, imm(2, 2), imm(key, 0))))
    result.append(case("SET/missing-policy", SET, dbdf, STATE, words(ENGINE)))
    result.append(case("SET/missing-engine", SET, dbdf, STATE, words(POLICY)))
    result.append(case("SET/duplicate-policy", SET, dbdf, STATE, words(ENGINE, POLICY, POLICY)))
    for param in [CAPS, STATE, LOG]:
        for key in [2, 3, 4, 5, 6]:
            result.append(case("GET/{}/set-key-{}".format(param, key), GET, dbdf, param,
                               words(ENGINE, imm(key, 0))))
    # Positive encoding corners use GET only; this suite never intentionally
    # submits a valid SET or changes a scheduler configuration.
    for param in [CAPS, STATE] + ([LOG] if include_log else []):
        for name, payload in [
                ("imm32", words(ENGINE)),
                ("seq32-single", words(counted(1, 1, 1), 1)),
                ("array32-single", words(counted(4, 1, 1), 1)),
                ("seq32-empty-then-engine", words(counted(1, 0xffff, 0), ENGINE)),
                ("seq64-empty-then-engine", words(counted(2, 0xffff, 0), ENGINE)),
                ("unused-high-half", words(counted(4, 1, 1), 0xffffffff00000001))]:
            result.append(case("GET/{}/valid-{}".format(param, name), GET, dbdf, param,
                               payload, expected="ok"))
        padding = case("GET/{}/valid-header-padding".format(param), GET, dbdf, param,
                       words(ENGINE), expected="ok")
        padding["frame"] = padding["frame"][:20] + b"\xff" * 4 + padding["frame"][24:]
        result.append(padding)
        if param != LOG:
            for size in [0, 1, 7, 8, 8 + (80 if param == CAPS else 40) - 1]:
                # FWCTL copies min(capacity, response size) and reports the full
                # response size, including when the caller supplies zero bytes.
                entry = case("GET/{}/out-capacity-{}".format(param, size), GET,
                             dbdf, param, words(ENGINE), expected="ok",
                             layer="transport", capacity=size)
                entry["expected_out_len"] = 88 if param == CAPS else 48
                result.append(entry)
    for length in range(8):
        entry = case("FWCTL/header-bytes-" + str(length), GET, dbdf, STATE, layer="transport")
        entry["frame"] = entry["frame"][:length]
        result.append(entry)
    entry = case("FWCTL/nonzero-reserved", GET, dbdf, STATE, words(ENGINE), layer="transport")
    entry["frame"] = struct.pack("<II", GET, 1) + entry["frame"][8:]
    result.append(entry)
    return result


def state_reply(reply):
    if reply.get("ioctl_rc") != 0:
        raise RuntimeError("GET_STATE ioctl rejected: errno={} ({})".format(reply.get("errno"), errno.errorcode.get(reply.get("errno"), "unknown")))
    data = bytes.fromhex(reply["response_hex"])
    if len(data) != 48 or struct.unpack_from("<II", data) != (GET, 0):
        raise RuntimeError("GET_STATE reply has incompatible header/size")
    decoded = {}
    for value in struct.unpack_from("<5Q", data, 8):
        key, index, opcode, scalar = value & 0xffff, value >> 16 & 4095, value >> 28 & 15, value >> 32
        if opcode or index or key in decoded:
            raise RuntimeError("GET_STATE reply has incompatible NVKV schema")
        decoded[key] = scalar
    if set(decoded) != {2, 4, 5, 6, 7} or decoded[2] not in [0, 2, 3, 4]:
        raise RuntimeError("GET_STATE response keys/policy do not match this ABI")
    return decoded


def caps_reply(reply):
    if reply.get("ioctl_rc") != 0:
        raise RuntimeError("GET_CAPS ioctl rejected: errno={}".format(reply.get("errno")))
    data = bytes.fromhex(reply["response_hex"])
    if len(data) != 88 or struct.unpack_from("<II", data) != (GET, 0):
        raise RuntimeError("GET_CAPS reply has incompatible header/size")
    values = struct.unpack_from("<10Q", data, 8)
    decoded, offset = {}, 0
    while offset < len(values):
        word = values[offset]
        offset += 1
        key, index, opcode, value = word & 0xffff, word >> 16 & 4095, word >> 28 & 15, word >> 32
        if index or key in decoded:
            raise RuntimeError("GET_CAPS reply has duplicate/indexed keys")
        if opcode == 4 and key == 2 and value == 3 and offset + 2 <= len(values):
            packed = words(*values[offset:offset + 2])
            decoded[key] = list(struct.unpack_from("<3I", packed))
            offset += 2
        elif opcode == 0 and 3 <= key <= 9:
            decoded[key] = value
        else:
            raise RuntimeError("GET_CAPS reply has incompatible NVKV schema")
    if set(decoded) != set(range(2, 10)):
        raise RuntimeError("GET_CAPS reply is missing required keys")
    return decoded


class Probe:
    def __init__(self, executable, bdf, timeout):
        self.executable, self.bdf, self.timeout = str(executable), bdf, timeout

    def __call__(self, entry):
        with tempfile.NamedTemporaryFile(prefix="params-input-", suffix=".bin") as file:
            file.write(entry["frame"])
            file.flush()
            result = subprocess.run([self.executable, "-b", self.bdf, "-i", file.name,
                                     "-o", str(entry["capacity"])], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, universal_newlines=True,
                                    timeout=self.timeout)
        if result.returncode:
            raise RuntimeError("raw probe failed before returning ioctl result: " + result.stderr.strip())
        reply = json.loads(result.stdout)
        reply["stderr"] = result.stderr
        return reply


class LogWatch:
    def __init__(self, path):
        self.path = Path(path)
        self.inode = None
        self.offset = 0
        self.available = False
        try:
            info = self.path.stat()
            self.inode, self.offset = info.st_ino, info.st_size
            self.available = True
        except OSError:
            pass

    def read(self):
        try:
            with self.path.open("rb") as file:
                info = self.path.stat()
                if info.st_ino != self.inode or info.st_size < self.offset:
                    self.offset = 0
                self.inode = info.st_ino
                file.seek(self.offset)
                lines = file.read().decode(errors="replace").splitlines()
                self.offset = file.tell()
                self.available = True
            return [line for line in lines if EVENT.search(line)]
        except OSError:
            self.available = False
            return []


def run_cases(probe, watch, dbdf, entries, settle=0.05):
    report = dict(dbdf=canonical(dbdf), cases=[], status="blocked", malformed_requests_sent=0,
                  baseline={}, limitations=[
                      "Nova maps nonzero firmware status to EIO; exact NV_STATUS is unavailable.",
                      "Kernel-rejected requests do not establish coverage of GSP validation.",
                      "Absence of watchdog/HALT logs is not proof that every possible input is safe."])
    health = case("health", GET, dbdf, STATE, words(ENGINE), expected="ok")
    try:
        report["baseline"]["state"] = probe(health)
        initial = state_reply(report["baseline"]["state"])
        caps = probe(case("baseline-caps", GET, dbdf, CAPS, words(ENGINE), expected="ok"))
        report["baseline"]["caps"] = caps
        caps_reply(caps)
        events = watch.read()
        if events:
            report["baseline"]["firmware_events"] = events
            raise RuntimeError("new firmware event during baseline; no malformed requests sent")
    except (RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        report["reason"] = str(error)
        report["baseline"]["firmware_events"] = report["baseline"].get("firmware_events", []) + watch.read()
        report["crash_assessment"] = "not assessed: known-good baseline failed"
        report["syslog_available"] = watch.available
        return report
    report["status"] = "passed"
    for entry in entries:
        record = {key: value for key, value in entry.items() if key != "frame"}
        record["request_hex"] = entry["frame"].hex()
        report["cases"].append(record)
        try:
            reply = probe(entry)
            record["reply"] = reply
            record["observed_rejection"] = ("EIO: firmware status or command-queue error; raw status unavailable"
                if reply.get("errno") == errno.EIO else "transport errno or success")
            report["malformed_requests_sent"] += entry["expected"] == "reject"
            if reply.get("errno") in [errno.EPERM, errno.ENOTTY, errno.ENODEV]:
                raise RuntimeError("driver access/device gate blocked test; GSP rejection is not established")
            accepted = reply.get("ioctl_rc") == 0
            record["passed"] = accepted == (entry["expected"] == "ok")
            if accepted:
                response = bytes.fromhex(reply["response_hex"])
                if "expected_out_len" in entry:
                    param = struct.unpack_from("<I", entry["frame"], 16)[0]
                    baseline = report["baseline"]["caps" if param == CAPS else "state"]
                    expected_prefix = bytes.fromhex(baseline["response_hex"])[:entry["capacity"]]
                    if (reply["out_len"] != entry["expected_out_len"] or
                            response != expected_prefix):
                        record["passed"] = False
                        record["reason"] = "incorrect FWCTL full length or truncated response prefix"
                elif (reply["out_len"] > entry["capacity"] or
                      len(response) != reply["out_len"]):
                    record["passed"] = False
                    record["reason"] = "driver returned an invalid response length"
            if settle:
                time.sleep(settle)
            after = probe(health)
            state = state_reply(after)
            record["health_after"] = after
            record["firmware_events"] = watch.read()
            record["syslog_available"] = watch.available
            if record["firmware_events"]:
                raise RuntimeError("new watchdog/HALT/fatal log during the test window")
            if state[2] != initial[2]:
                raise RuntimeError("scheduler policy changed during negative-input testing")
            if not record["passed"]:
                report["status"] = "failed"
                report["reason"] = "unexpected acceptance/rejection: " + entry["name"]
                break
            print("PASS " + entry["name"], flush=True)
        except (RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            record["passed"] = False
            record["reason"] = str(error)
            record["firmware_events"] = record.get("firmware_events", []) + watch.read()
            report["status"] = "failed"
            report["reason"] = entry["name"] + ": " + str(error)
            break
    report["syslog_available"] = watch.available
    report["crash_assessment"] = (
        "No health loss or new monitored firmware event observed in executed cases."
        if report["status"] == "passed" and watch.available else
        "Inspect per-case health, timeout and log evidence; no blanket no-crash conclusion.")
    return report


class SelfTests(unittest.TestCase):
    @staticmethod
    def good_state():
        payload = words(imm(2, 3), imm(4, 33), imm(5, 2), imm(6, 2083333), imm(7, 0))
        return dict(ioctl_rc=0, errno=0, out_len=48,
                    response_hex=(struct.pack("<II", GET, 0) + payload).hex())

    @staticmethod
    def good_caps():
        payload = words(counted(4, 2, 3), 2 | 3 << 32, 4,
                        *[imm(key, 1) for key in range(3, 10)])
        return dict(ioctl_rc=0, errno=0, out_len=88,
                    response_hex=(struct.pack("<II", GET, 0) + payload).hex())

    def test_dbdf(self):
        self.assertEqual(parse_bdf("9b:00.4"), 0x9b04)
        self.assertEqual(parse_bdf("0001:9b:00.4"), 0x100009b04)
        for invalid in ["bad", "00:20.0", "9b:00.8", "0000:00:00.0"]:
            with self.assertRaises(ValueError):
                parse_bdf(invalid)

    def test_header_layout(self):
        data = frame(GET, 0x9b00, STATE, words(ENGINE))
        self.assertEqual(len(data), 32)
        self.assertEqual(struct.unpack_from("<Q", data, 8)[0], 0x9b00)
        self.assertEqual(struct.unpack_from("<I", data, 16)[0], STATE)
        self.assertEqual(struct.unpack_from("<Q", data, 24)[0], ENGINE)

    def test_matrix(self):
        entries = input_cases(0x9b00, 0x9b04)
        self.assertGreater(len(entries), 300)
        self.assertEqual(len(entries), len({entry["name"] for entry in entries}))
        self.assertTrue(all(entry["command"] != SET or entry["expected"] == "reject" for entry in entries))
        self.assertTrue(any("4294967295" in entry["name"] for entry in entries))
        self.assertTrue(any("vgpu-param-mismatch" in entry["name"] for entry in entries))

    def test_baseline_failure_stops_before_inputs(self):
        calls = []
        def reject(entry):
            calls.append(entry["name"])
            return dict(ioctl_rc=-1, errno=errno.EIO, response_hex="", out_len=0)
        watch = LogWatch("/does/not/exist")
        report = run_cases(reject, watch, 0x9b00, input_cases(0x9b00), settle=0)
        self.assertEqual(report["status"], "blocked")
        self.assertEqual(report["malformed_requests_sent"], 0)
        self.assertEqual(calls, ["health"])

    def test_fwctl_small_output_contract(self):
        entries = [entry for entry in input_cases(0x9b00, 0x9b04)
                   if "expected_out_len" in entry]
        self.assertEqual(len(entries), 10)
        for failure in ["none", "wrong-length", "wrong-prefix"]:
            class Watch:
                available = True

                def read(self):
                    return []

            def probe(entry):
                if entry["name"] == "health":
                    return self.good_state()
                if entry["name"] == "baseline-caps":
                    return self.good_caps()
                reply = self.good_caps() if entry["expected_out_len"] == 88 else self.good_state()
                reply["response_hex"] = reply["response_hex"][:entry["capacity"] * 2]
                if failure == "wrong-length":
                    reply["out_len"] -= 1
                elif failure == "wrong-prefix" and entry["capacity"]:
                    reply["response_hex"] = "ff" + reply["response_hex"][2:]
                return reply

            report = run_cases(probe, Watch(), 0x9b00, entries, settle=0)
            self.assertEqual(report["status"], "passed" if failure == "none" else "failed")
            if failure == "none":
                self.assertEqual(len(report["cases"]), 10)

    def test_state_schema(self):
        reply = self.good_state()
        self.assertEqual(state_reply(reply)[2], 3)
        reply["response_hex"] = reply["response_hex"][:-2]
        with self.assertRaises(RuntimeError):
            state_reply(reply)

    def test_caps_schema(self):
        reply = self.good_caps()
        self.assertEqual(caps_reply(reply)[2], [2, 3, 4])
        reply["response_hex"] = (struct.pack("<II", GET, 0) + bytes(80)).hex()
        with self.assertRaises(RuntimeError):
            caps_reply(reply)

    def test_health_and_failure_gates(self):
        for failure in ["none", "accepted", "health", "timeout", "permission", "watchdog"]:
            calls = []
            class Watch:
                available = True
                reads = 0

                def read(self):
                    self.reads += 1
                    return ["GPU0 GSP HALT"] if failure == "watchdog" and self.reads == 2 else []

            def probe(entry):
                calls.append(entry["name"])
                if entry["name"] == "health":
                    if failure == "health" and calls.count("health") > 1:
                        return dict(ioctl_rc=-1, errno=errno.EIO, response_hex="")
                    return self.good_state()
                if entry["name"] == "baseline-caps":
                    return self.good_caps()
                if failure == "timeout":
                    raise subprocess.TimeoutExpired("raw-probe", 1)
                accepted = failure == "accepted"
                return dict(ioctl_rc=0 if accepted else -1,
                            errno=errno.EPERM if failure == "permission" else (0 if accepted else errno.EIO),
                            out_len=0, response_hex="")

            entries = [case("bad-1", GET, 0, STATE, words(ENGINE)),
                       case("bad-2", SET, 0, STATE, words(ENGINE))]
            report = run_cases(probe, Watch(), 0x9b00, entries, settle=0)
            self.assertEqual(report["status"], "passed" if failure == "none" else "failed")
            if failure != "none":
                self.assertNotIn("bad-2", calls)

    def test_log_watch_rotation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "syslog"
            path.write_text("old GSP watchdog\n")
            watch = LogWatch(path)
            self.assertEqual(watch.read(), [])
            with path.open("a") as file:
                file.write("GPU0 LibOS Task Watchdog Report\n")
            self.assertEqual(len(watch.read()), 1)
            path.unlink()
            path.write_text("GPU0 GSP HALT\n")
            self.assertEqual(len(watch.read()), 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--list", action="store_true", help="list cases without any RPC")
    mode.add_argument("--self-test", action="store_true", help="offline matrix/runner tests")
    mode.add_argument("--run", action="store_true", help="send raw FWCTL requests after ABI preflight")
    parser.add_argument("-b", "--bdf", help="PF BDF; required with --run")
    parser.add_argument("--vf-bdf", help="existing VF under the selected PF, for target mismatch cases")
    parser.add_argument("--include-log", action="store_true", help="include valid consuming SCHED_LOG queries")
    parser.add_argument("--probe", type=Path, default=Path(__file__).with_name("params-rpc-probe"))
    parser.add_argument("--syslog", default="/var/log/syslog")
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--report", type=Path, default=Path("params-input-results.json"))
    args = parser.parse_args()
    if args.self_test:
        return 0 if unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(SelfTests)).wasSuccessful() else 1
    if args.run and not args.bdf:
        parser.error("--run requires -b <PF-BDF>")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        dbdf = parse_bdf(args.bdf or "0000:9b:00.0")
        vf = parse_bdf(args.vf_bdf) if args.vf_bdf else None
        if vf is not None:
            link = Path("/sys/bus/pci/devices") / canonical(vf) / "physfn"
            if args.run and (not link.is_symlink() or link.resolve().name.lower() != canonical(dbdf)):
                parser.error("--vf-bdf must be an existing VF of the selected PF")
        entries = input_cases(dbdf, vf, args.include_log)
    except ValueError as error:
        parser.error(str(error))
    if args.list:
        for entry in entries:
            print("{} expected={} layer={} in={} out={}".format(
                entry["name"], entry["expected"], entry["layer"], len(entry["frame"]), entry["capacity"]))
        print("{} cases; no RPCs sent".format(len(entries)))
        return 0
    if not args.probe.is_file():
        parser.error("build the raw probe first: make tests/params-rpc-probe")
    report = run_cases(Probe(args.probe.resolve(), canonical(dbdf), args.timeout),
                       LogWatch(args.syslog), dbdf, entries)
    report["planned_cases"] = len(entries)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print("{}: executed {}/{} cases; report {}".format(
        report["status"].upper(), len(report["cases"]), len(entries), args.report))
    if report.get("reason"):
        print(report["reason"], file=sys.stderr)
    return 0 if report["status"] == "passed" else (2 if report["status"] == "blocked" else 1)


if __name__ == "__main__":
    sys.exit(main())
