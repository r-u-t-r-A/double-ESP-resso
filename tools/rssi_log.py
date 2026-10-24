#!/usr/bin/env python3
"""Log RF node RSSI over its USB console for calibration.

Talks to the RF node's USB-Serial/JTAG console (not the AP node), so the RF
node can be characterized alone: noise floor, gate-pass peak and clipping at
different gains.

    tools/rssi_log.py /dev/ttyACM0 --freq 5732 --gain 30 --seconds 60 -o pass.csv
    tools/rssi_log.py --plot pass.csv

Output CSV columns: host_s, node_ms, rssi, filt_db, band_db, clip_permille, gain_bad
(band_db is one capture's band power in dB re 1 LSB^2, filt_db the median-of-3
+ EMA value that rssi is scaled from; gain_bad counts samples whose gain field
did not match the forced index, which should always be 0).
"""
import argparse
import csv
import sys
import time


def record(args):
    try:
        import serial
    except ImportError:
        sys.exit("pyserial is required: pip install pyserial")

    port = serial.Serial(args.port, 115200, timeout=0.2)
    port.reset_input_buffer()

    def command(line):
        port.write((line + "\n").encode())
        deadline = time.time() + 3.0
        while time.time() < deadline:
            reply = port.readline().decode(errors="replace").strip()
            if reply.startswith("STATUS,"):
                print(reply)
                return reply
        sys.exit(f"no STATUS reply to {line!r}")

    if args.gain is not None:
        command(f"g {args.gain}")
    if args.freq is not None:
        status = command(f"f {args.freq}")
        if ",OK," not in status:
            sys.exit(f"RF node did not tune: {status}")
    command("s")

    port.write(b"r\n")  # toggle stream on
    rows = 0
    start = time.time()
    try:
        with open(args.output, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["host_s", "node_ms", "rssi", "filt_db", "band_db", "clip_permille",
                        "gain_bad"])
            while time.time() - start < args.seconds:
                line = port.readline().decode(errors="replace").strip()
                if not line.startswith("RSSI,"):
                    continue
                parts = line.split(",")
                if len(parts) != 7:
                    continue
                w.writerow([f"{time.time() - start:.3f}"] + parts[1:])
                rows += 1
                if rows % 100 == 0:
                    print(f"\r{rows} samples, rssi {parts[2]:>3} {parts[3]:>6} dB "
                          f"clip {parts[5]:>4}", end="")
    finally:
        port.write(b"r\n")  # toggle stream off
        port.close()
    print(f"\nwrote {rows} samples to {args.output}")


def plot(path):
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("matplotlib is required for --plot")
    t, rssi, band, filt, clip = [], [], [], [], []
    with open(path) as f:
        for row in csv.DictReader(f):
            t.append(float(row["host_s"]))
            rssi.append(int(row["rssi"]))
            band.append(float(row["band_db"]))
            filt.append(float(row["filt_db"]))
            clip.append(int(row["clip_permille"]))
    fig, ax = plt.subplots(3, 1, sharex=True, figsize=(10, 8))
    ax[0].plot(t, rssi, lw=0.8)
    ax[0].set_ylabel("RSSI (0-255)")
    ax[0].set_ylim(0, 260)
    ax[1].plot(t, band, lw=0.5, color="0.6", label="per capture")
    ax[1].plot(t, filt, lw=0.8, label="filtered")
    ax[1].set_ylabel("band power (dB)")
    ax[1].legend(loc="upper right")
    ax[2].plot(t, clip, lw=0.8, color="tab:red")
    ax[2].set_ylabel("clip (permille)")
    ax[2].set_xlabel("time (s)")
    fig.suptitle(path)
    plt.tight_layout()
    plt.show()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="RF node USB port, e.g. /dev/ttyACM0")
    ap.add_argument("--freq", type=int, help="tune to MHz first (5180-5917)")
    ap.add_argument("--gain", type=int, help="set fixed gain index first (0-89)")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("-o", "--output", default="rssi.csv")
    ap.add_argument("--plot", metavar="CSV", help="plot a recorded CSV instead of recording")
    args = ap.parse_args()
    if args.plot:
        plot(args.plot)
    elif args.port:
        record(args)
    else:
        ap.error("give a port to record, or --plot CSV")


if __name__ == "__main__":
    main()
