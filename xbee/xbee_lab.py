"""Safe host-side tools for the private XBee 802.15.4 course.

The default commands are read-only or transmit test payloads.  The one command
that changes radio parameters is named ``align-volatile`` and requires
``--apply``; it deliberately never sends ATWR, so it does not persist settings.

Every serial open deasserts DTR/RTS before and after open, uses finite timeouts,
and closes the port in cleanup.  COM names are conveniences: use ``list`` and
optional ``--serial-*`` guards to verify the FTDI identities before testing.
"""

from __future__ import annotations

import argparse
import math
import statistics
import sys
import time
from dataclasses import dataclass
from typing import Iterable

import serial
from serial.tools import list_ports


DEFAULT_BAUD = 9600
QUERY_COMMANDS = (
    "VR", "HV", "SH", "SL", "MY", "ID", "DH", "DL", "BD", "RO",
    "AP", "CE", "AI", "CH",
)


@dataclass(frozen=True)
class PortIdentity:
    device: str
    vid: int | None
    pid: int | None
    serial_number: str | None
    description: str


def identities() -> list[PortIdentity]:
    return [
        PortIdentity(p.device, p.vid, p.pid, p.serial_number, p.description)
        for p in list_ports.comports()
    ]


def require_identity(port: str, expected_serial: str | None) -> PortIdentity:
    matches = [item for item in identities() if item.device.upper() == port.upper()]
    if not matches:
        raise RuntimeError(f"{port} is not currently enumerated")
    item = matches[0]
    if expected_serial and item.serial_number != expected_serial:
        raise RuntimeError(
            f"identity mismatch for {port}: expected USB serial {expected_serial!r}, "
            f"saw {item.serial_number!r}"
        )
    return item


def open_safe(port: str, baud: int) -> serial.Serial:
    handle = serial.Serial()
    handle.port = port
    handle.baudrate = baud
    handle.bytesize = serial.EIGHTBITS
    handle.parity = serial.PARITY_NONE
    handle.stopbits = serial.STOPBITS_ONE
    handle.timeout = 0.05
    handle.write_timeout = 1.0
    handle.dtr = False
    handle.rts = False
    handle.open()
    handle.dtr = False
    handle.rts = False
    return handle


def close_safe(*handles: serial.Serial | None) -> None:
    for handle in handles:
        if handle is None:
            continue
        try:
            handle.dtr = False
            handle.rts = False
        finally:
            handle.close()


def read_until_quiet(
    handle: serial.Serial, max_wait: float = 1.2, quiet: float = 0.15
) -> bytes:
    data = bytearray()
    deadline = time.monotonic() + max_wait
    last_data = time.monotonic()
    while time.monotonic() < deadline:
        chunk = handle.read(512)
        if chunk:
            data.extend(chunk)
            last_data = time.monotonic()
        elif data and time.monotonic() - last_data >= quiet:
            break
    return bytes(data)


def enter_command_mode(handle: serial.Serial, guard: float) -> None:
    handle.reset_input_buffer()
    time.sleep(guard)
    handle.write(b"+++")
    handle.flush()
    reply = read_until_quiet(handle, max_wait=max(1.2, guard))
    if b"OK" not in reply:
        raise RuntimeError(f"command mode did not answer OK; received {reply!r}")


def at(handle: serial.Serial, command: str) -> str:
    handle.write(("AT" + command + "\r").encode("ascii"))
    handle.flush()
    return read_until_quiet(handle, max_wait=0.8).decode("ascii", errors="replace").strip()


def cmd_list(_: argparse.Namespace) -> int:
    for item in sorted(identities(), key=lambda row: row.device):
        vid_pid = (
            f"{item.vid:04X}:{item.pid:04X}"
            if item.vid is not None and item.pid is not None
            else "----:----"
        )
        print(
            f"{item.device:8} VID:PID={vid_pid} serial={item.serial_number or '-':16} "
            f"{item.description}"
        )
    return 0


def cmd_query(args: argparse.Namespace) -> int:
    identity = require_identity(args.port, args.serial)
    print(
        f"opening {identity.device}: USB serial={identity.serial_number or '-'} "
        f"at {args.baud} 8N1"
    )
    handle = None
    try:
        handle = open_safe(args.port, args.baud)
        enter_command_mode(handle, args.guard)
        for command in QUERY_COMMANDS:
            print(f"AT{command}={at(handle, command)}")
        print(f"ATCN={at(handle, 'CN')}")
    finally:
        close_safe(handle)
    return 0


def receive_exact(handle: serial.Serial, token: bytes, timeout: float) -> tuple[bool, bytes]:
    deadline = time.monotonic() + timeout
    data = bytearray()
    # A previous RF packet may arrive late.  Do not stop merely because the
    # buffer has reached the expected token length; keep looking for this
    # sequence number until the finite deadline.
    while time.monotonic() < deadline:
        data.extend(handle.read(512))
        if token in data:
            return True, bytes(data)
        if len(data) > 16384:
            del data[:-8192]
    return token in data, bytes(data)


def direction_test(
    source: serial.Serial,
    target: serial.Serial,
    label: str,
    count: int,
    timeout: float,
) -> tuple[int, int, list[str]]:
    passed = 0
    corrupt = 0
    examples: list[str] = []
    source.reset_input_buffer()
    target.reset_input_buffer()
    for sequence in range(count):
        token = f"XBEE-LAB|{label}|{sequence:05d}|A55A\r\n".encode("ascii")
        source.write(token)
        source.flush()
        ok, received = receive_exact(target, token, timeout)
        if ok:
            passed += 1
        elif received:
            corrupt += 1
            if len(examples) < 3:
                examples.append(f"seq={sequence}: {received.hex()}")
    return passed, corrupt, examples


def print_direction(label: str, sent: int, result: tuple[int, int, list[str]]) -> bool:
    passed, corrupt, examples = result
    lost = sent - passed - corrupt
    print(
        f"{label}: sent={sent} received_exact={passed} corrupt={corrupt} "
        f"no_matching_bytes={lost} success={passed / sent:.1%}"
    )
    for example in examples:
        print(f"  sample {example}")
    return passed == sent


def cmd_pair_test(args: argparse.Namespace) -> int:
    identity_a = require_identity(args.port_a, args.serial_a)
    identity_b = require_identity(args.port_b, args.serial_b)
    if identity_a.device.upper() == identity_b.device.upper():
        raise RuntimeError("port A and port B must be different")
    print(
        f"A={identity_a.device}/{identity_a.serial_number or '-'} "
        f"B={identity_b.device}/{identity_b.serial_number or '-'} "
        f"baud={args.baud} count={args.count}"
    )
    a = b = None
    try:
        a = open_safe(args.port_a, args.baud)
        b = open_safe(args.port_b, args.baud)
        time.sleep(0.3)
        result_ab = direction_test(a, b, "A>B", args.count, args.timeout)
        result_ba = direction_test(b, a, "B>A", args.count, args.timeout)
    finally:
        close_safe(a, b)
    pass_ab = print_direction("A -> B", args.count, result_ab)
    pass_ba = print_direction("B -> A", args.count, result_ba)
    return 0 if pass_ab and pass_ba else 2


def validate_hex_fields(values: Iterable[tuple[str, str]]) -> None:
    for name, value in values:
        try:
            int(value, 16)
        except ValueError as exc:
            raise RuntimeError(f"{name} must be hexadecimal, got {value!r}") from exc


def cmd_align_volatile(args: argparse.Namespace) -> int:
    validate_hex_fields((("pan", args.pan), ("dest-sh", args.dest_sh), ("dest-sl", args.dest_sl)))
    identity = require_identity(args.port, args.serial)
    print(
        f"target={identity.device}/{identity.serial_number or '-'} "
        f"ID={args.pan.upper()} DH={args.dest_sh.upper()} DL={args.dest_sl.upper()}"
    )
    print("This command never sends ATWR; settings are not persisted by this tool.")
    if not args.apply:
        print("preview only; add --apply to change volatile settings")
        return 0
    handle = None
    try:
        handle = open_safe(args.port, args.baud)
        enter_command_mode(handle, args.guard)
        originals = {name: at(handle, name) for name in ("ID", "DH", "DL")}
        print("original " + " ".join(f"{key}={value}" for key, value in originals.items()))
        for command in (f"ID{args.pan}", f"DH{args.dest_sh}", f"DL{args.dest_sl}"):
            answer = at(handle, command)
            print(f"AT{command}={answer}")
            if answer != "OK":
                raise RuntimeError(f"AT{command} was rejected: {answer!r}")
        current = {name: at(handle, name) for name in ("ID", "DH", "DL")}
        print("current  " + " ".join(f"{key}={value}" for key, value in current.items()))
        print(f"ATCN={at(handle, 'CN')}")
    finally:
        close_safe(handle)
    return 0


def crc16_ccitt(data: bytes, initial: int = 0xFFFF) -> int:
    """Return CRC-16/CCITT-FALSE for application-layer lab packets."""
    crc = initial
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


@dataclass(frozen=True)
class LabPacket:
    kind: str
    sequence: int
    payload: str


def encode_lab_packet(kind: str, sequence: int, payload: str) -> bytes:
    if "|" in kind or "|" in payload or "\r" in payload or "\n" in payload:
        raise RuntimeError("kind and payload must not contain pipe or newline characters")
    body = f"XBL1|{kind}|{sequence:05d}|{payload}".encode("ascii")
    return body + f"|{crc16_ccitt(body):04X}\r\n".encode("ascii")


def decode_lab_packet(line: bytes) -> LabPacket:
    clean = line.strip(b"\r\n")
    try:
        body, crc_text = clean.rsplit(b"|", 1)
        fields = body.decode("ascii").split("|", 3)
        received_crc = int(crc_text, 16)
    except (ValueError, UnicodeDecodeError) as exc:
        raise RuntimeError(f"malformed XBL1 packet: {clean!r}") from exc
    if len(fields) != 4 or fields[0] != "XBL1":
        raise RuntimeError(f"unexpected packet header: {clean!r}")
    calculated_crc = crc16_ccitt(body)
    if received_crc != calculated_crc:
        raise RuntimeError(
            f"CRC mismatch: received={received_crc:04X} calculated={calculated_crc:04X}"
        )
    try:
        sequence = int(fields[2], 10)
    except ValueError as exc:
        raise RuntimeError(f"invalid sequence number: {fields[2]!r}") from exc
    return LabPacket(fields[1], sequence, fields[3])


class LineReader:
    def __init__(self, handle: serial.Serial):
        self.handle = handle
        self.buffer = bytearray()

    def read_line(self, timeout: float) -> bytes | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            newline = self.buffer.find(b"\n")
            if newline >= 0:
                line = bytes(self.buffer[: newline + 1])
                del self.buffer[: newline + 1]
                return line
            chunk = self.handle.read(512)
            if chunk:
                self.buffer.extend(chunk)
                if len(self.buffer) > 32768:
                    del self.buffer[:-16384]
        return None


def wait_for_packet(
    reader: LineReader, kind: str, sequence: int, timeout: float
) -> tuple[LabPacket | None, int]:
    deadline = time.monotonic() + timeout
    rejected = 0
    while time.monotonic() < deadline:
        line = reader.read_line(max(0.01, deadline - time.monotonic()))
        if line is None:
            break
        try:
            packet = decode_lab_packet(line)
        except RuntimeError:
            rejected += 1
            continue
        if packet.kind == kind and packet.sequence == sequence:
            return packet, rejected
    return None, rejected


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return float("nan")
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))
    return ordered[index]


def cmd_reliable_test(args: argparse.Namespace) -> int:
    identity_a = require_identity(args.port_a, args.serial_a)
    identity_b = require_identity(args.port_b, args.serial_b)
    if identity_a.device.upper() == identity_b.device.upper():
        raise RuntimeError("port A and port B must be different")
    print(
        f"reliable-link A={identity_a.device}/{identity_a.serial_number or '-'} "
        f"B={identity_b.device}/{identity_b.serial_number or '-'} count={args.count} "
        f"timeout={args.timeout:.3f}s retries={args.retries}"
    )
    a = b = None
    sent_frames = retries_used = duplicates = crc_rejected = failures = 0
    delivered: set[int] = set()
    rtt_ms: list[float] = []
    try:
        a = open_safe(args.port_a, args.baud)
        b = open_safe(args.port_b, args.baud)
        time.sleep(0.3)
        a.reset_input_buffer()
        b.reset_input_buffer()
        read_a, read_b = LineReader(a), LineReader(b)
        for sequence in range(args.count):
            success = False
            for attempt in range(args.retries + 1):
                started = time.monotonic()
                data = encode_lab_packet("D", sequence, f"PAYLOAD-{sequence:05d}")
                if args.corrupt_every and sequence % args.corrupt_every == 0 and attempt == 0:
                    data = data[:-6] + b"0000\r\n"
                a.write(data)
                a.flush()
                sent_frames += 1
                packet, rejected = wait_for_packet(read_b, "D", sequence, args.timeout)
                crc_rejected += rejected
                if packet is None:
                    if attempt < args.retries:
                        retries_used += 1
                    continue
                is_duplicate = sequence in delivered
                if is_duplicate:
                    duplicates += 1
                else:
                    delivered.add(sequence)
                drop_ack = (
                    args.drop_ack_every
                    and sequence % args.drop_ack_every == 0
                    and attempt == 0
                )
                if not drop_ack:
                    b.write(encode_lab_packet("A", sequence, "OK"))
                    b.flush()
                ack, rejected = wait_for_packet(read_a, "A", sequence, args.timeout)
                crc_rejected += rejected
                if ack is not None:
                    rtt_ms.append((time.monotonic() - started) * 1000)
                    success = True
                    break
                if attempt < args.retries:
                    retries_used += 1
            if not success:
                failures += 1
        print(
            f"delivered_unique={len(delivered)}/{args.count} sender_confirmed="
            f"{args.count - failures}/{args.count} data_frames={sent_frames} retries={retries_used} "
            f"duplicates_suppressed={duplicates} crc_rejected={crc_rejected} failures={failures}"
        )
        if rtt_ms:
            print(
                f"rtt_ms min={min(rtt_ms):.1f} mean={statistics.fmean(rtt_ms):.1f} "
                f"p50={statistics.median(rtt_ms):.1f} p95={percentile(rtt_ms, .95):.1f} "
                f"max={max(rtt_ms):.1f}"
            )
    finally:
        close_safe(a, b)
    return 0 if failures == 0 and len(delivered) == args.count else 2


def cmd_telemetry_send(args: argparse.Namespace) -> int:
    identity = require_identity(args.port, args.serial)
    print(
        f"telemetry source={identity.device}/{identity.serial_number or '-'} "
        f"count={args.count} interval={args.interval:.3f}s"
    )
    handle = None
    try:
        handle = open_safe(args.port, args.baud)
        for sequence in range(args.count):
            temperature = args.temperature + 1.7 * math.sin(sequence / 7)
            humidity = args.humidity + 4.2 * math.sin(sequence / 11 + 0.8)
            payload = f"MS={int(time.time() * 1000)},T={temperature:.2f},H={humidity:.2f}"
            packet = encode_lab_packet("T", sequence, payload)
            handle.write(packet)
            handle.flush()
            print(packet.decode("ascii").strip())
            if sequence + 1 < args.count:
                time.sleep(args.interval)
    finally:
        close_safe(handle)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    list_parser = sub.add_parser("list", help="list current serial identities")
    list_parser.set_defaults(func=cmd_list)

    query = sub.add_parser("query", help="read XBee AT parameters; never writes settings")
    query.add_argument("port")
    query.add_argument("--serial", help="required USB serial number")
    query.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    query.add_argument("--guard", type=float, default=1.2)
    query.set_defaults(func=cmd_query)

    pair = sub.add_parser("pair-test", help="send sequenced payloads in both directions")
    pair.add_argument("port_a")
    pair.add_argument("port_b")
    pair.add_argument("--serial-a")
    pair.add_argument("--serial-b")
    pair.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    pair.add_argument("--count", type=int, default=50)
    pair.add_argument("--timeout", type=float, default=0.5)
    pair.set_defaults(func=cmd_pair_test)

    align = sub.add_parser(
        "align-volatile", help="change RAM settings only; requires --apply and never sends ATWR"
    )
    align.add_argument("port")
    align.add_argument("--serial")
    align.add_argument("--pan", required=True, help="hex PAN ID")
    align.add_argument("--dest-sh", required=True, help="hex destination high address")
    align.add_argument("--dest-sl", required=True, help="hex destination low address")
    align.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    align.add_argument("--guard", type=float, default=1.2)
    align.add_argument("--apply", action="store_true")
    align.set_defaults(func=cmd_align_volatile)

    reliable = sub.add_parser(
        "reliable-test", help="stop-and-wait DATA/ACK with CRC, retries, and RTT statistics"
    )
    reliable.add_argument("port_a")
    reliable.add_argument("port_b")
    reliable.add_argument("--serial-a")
    reliable.add_argument("--serial-b")
    reliable.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    reliable.add_argument("--count", type=int, default=100)
    reliable.add_argument("--timeout", type=float, default=0.8)
    reliable.add_argument("--retries", type=int, default=3)
    reliable.add_argument(
        "--drop-ack-every", type=int, default=0,
        help="teaching fault injection: drop the first ACK for every Nth sequence",
    )
    reliable.add_argument(
        "--corrupt-every", type=int, default=0,
        help="teaching fault injection: corrupt the first DATA CRC for every Nth sequence",
    )
    reliable.set_defaults(func=cmd_reliable_test)

    telemetry = sub.add_parser(
        "telemetry-send", help="send CRC-protected simulated temperature/humidity records"
    )
    telemetry.add_argument("port")
    telemetry.add_argument("--serial")
    telemetry.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    telemetry.add_argument("--count", type=int, default=60)
    telemetry.add_argument("--interval", type=float, default=1.0)
    telemetry.add_argument("--temperature", type=float, default=24.0)
    telemetry.add_argument("--humidity", type=float, default=60.0)
    telemetry.set_defaults(func=cmd_telemetry_send)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if getattr(args, "count", 1) < 1:
        raise RuntimeError("--count must be at least 1")
    if getattr(args, "retries", 0) < 0:
        raise RuntimeError("--retries must not be negative")
    if getattr(args, "timeout", 0.1) <= 0 or getattr(args, "interval", 0.1) <= 0:
        raise RuntimeError("timeouts and intervals must be positive")
    return args.func(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, serial.SerialException) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
