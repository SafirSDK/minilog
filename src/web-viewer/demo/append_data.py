#!/usr/bin/env python3
"""
Continuously appends new log entries to the JSONL files, simulating a live
minilog instance.  Run this alongside minilog-web-viewer for a live demo.

Usage: python3 append_data.py [--interval 2.5] [--burst 1-3]
  --interval  Average seconds between batches     (default: 2.5)
  --burst     Max entries per batch (1..N)         (default: 3)

Ctrl-C to stop.
"""

import argparse
import datetime
import json
import os
import random
import signal
import sys
import time

# Reuse the shared data tables from gen_data.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_data import generate


def appender(interval: float, burst: int):
    script_dir = os.path.dirname(os.path.abspath(__file__))
    main_path = os.path.join(script_dir, "logs", "syslog.jsonl")
    auth_path = os.path.join(script_dir, "logs", "auth.jsonl")

    # Seed appender differently from the generator so we get fresh variety.
    _, auth_facs = generate(seed=0, count=0)

    random.seed()  # use OS entropy for the live appender

    months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
    hosts = [
        "web01.example.com",
        "db02.example.com",
        "app03.example.com",
        "mail.example.com",
        "fw01.example.com",
    ]
    apps = [
        "nginx",
        "postgres",
        "sshd",
        "postfix",
        "kernel",
        "systemd",
        "cron",
        "sudo",
        "dockerd",
        "rsyslogd",
    ]
    srcs = ["192.168.1.10", "192.168.1.20", "10.0.0.5", "10.0.0.6", "172.16.0.1"]
    app_fac = {
        "nginx": "daemon",
        "postgres": "daemon",
        "sshd": "auth",
        "postfix": "mail",
        "kernel": "kern",
        "systemd": "daemon",
        "cron": "daemon",
        "sudo": "authpriv",
        "dockerd": "daemon",
        "rsyslogd": "syslog",
    }
    app_sev_weights = {
        "nginx": [6, 6, 6, 6, 6, 5, 3, 3],
        "postgres": [6, 6, 6, 4, 4, 3, 2, 1],
        "sshd": [6, 6, 6, 4, 4, 3, 2, 1],
        "postfix": [6, 6, 6, 5, 4, 3, 2, 1],
        "kernel": [6, 6, 5, 4, 3, 2, 1, 1],
        "systemd": [8, 8, 8, 4, 2, 1, 1, 1],
        "cron": [9, 9, 9, 9, 5, 2, 1, 1],
        "sudo": [7, 7, 7, 3, 1, 1, 1, 1],
        "dockerd": [7, 7, 6, 4, 3, 2, 1, 1],
        "rsyslogd": [9, 9, 9, 9, 9, 5, 2, 1],
    }
    messages = {
        "nginx": [
            "GET /api/v1/users 200 1ms",
            "POST /api/v1/login 401 2ms",
            "GET /health 200 0ms",
            "upstream timed out (110: Connection timed out) while reading response header",
            "worker process 12345 exited with code 1",
        ],
        "postgres": [
            "connection received: host=10.0.0.5 port=54321",
            "checkpoint starting: time",
            "checkpoint complete: wrote 142 buffers (0.9%)",
            "duration: 1523.421 ms  statement: SELECT * FROM events WHERE ts > NOW() - INTERVAL '1 day'",
            "ERROR: deadlock detected",
        ],
        "sshd": [
            "Accepted publickey for deploy from 10.0.0.5 port 44321 ssh2",
            "Failed password for invalid user admin from 45.33.32.156 port 22 ssh2",
            "Connection closed by 192.168.1.10 port 52100 [preauth]",
            "Received disconnect from 10.0.0.6 port 53412: 11: disconnected by user",
        ],
        "postfix": [
            "connect from mail.example.com[192.168.1.10]",
            "message-id=<20260101120000.12345@example.com>: queued",
            "to=<user@example.com>, status=sent (250 OK)",
            "warning: TLS library problem: error:0A000119",
        ],
        "kernel": [
            "EXT4-fs (sda1): mounted filesystem with ordered data mode",
            'audit: type=1400 audit(1234567890.123:42): apparmor="ALLOWED"',
            "oom_reaper: reaped process 9876 (python3)",
            "TCP: Possible SYN flooding on port 443. Sending cookies.",
        ],
        "systemd": [
            "Started nginx.service.",
            "Stopping PostgreSQL Database Server...",
            "minilog.service: Main process exited, code=killed, status=9/KILL",
            "Reloading.",
        ],
        "cron": [
            "(root) CMD (   cd / && run-parts --report /etc/cron.hourly)",
            "(www-data) CMD (/usr/local/bin/cleanup.sh)",
            "pam_unix(cron:session): session opened for user root",
        ],
        "sudo": [
            "deploy : TTY=pts/0 ; PWD=/opt/app ; USER=root ; COMMAND=/bin/systemctl restart nginx",
            "FAILED su for root by www-data",
        ],
        "dockerd": [
            'level=info msg="Container started" container=abc123',
            'level=warning msg="OOM event for container" container=def456',
            'level=error msg="Handler for POST /containers/create returned error"',
        ],
        "rsyslogd": ["rsyslogd 8.2106.0: running", "imudp: binding to 0.0.0.0:514"],
    }

    sev_names = ["EMERGENCY", "ALERT", "CRITICAL", "ERROR", "WARNING", "NOTICE", "INFO", "DEBUG"]

    def make_entry(ts, app):
        fac_name = app_fac[app]
        sev_code = random.choices(range(8), weights=app_sev_weights[app])[0]
        sev_name = sev_names[sev_code]
        host = random.choice(hosts)
        src = random.choice(srcs)
        msg = random.choice(messages[app])
        if random.random() < 0.6:
            proto = "RFC3164"
            msg_time = (
                f"{months[ts.month - 1]} {ts.day:2d} {ts.hour:02d}:{ts.minute:02d}:{ts.second:02d}"
            )
        else:
            proto = "RFC5424"
            msg_time = ts.strftime("%Y-%m-%dT%H:%M:%SZ")
        return {
            "rcv": ts.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "src": src,
            "proto": proto,
            "facility": fac_name,
            "severity": sev_name,
            "hostname": host,
            "app": app,
            "pid": str(random.randint(100, 65000)),
            "msgid": None,
            "msg_time": msg_time,
            "message": msg,
        }

    print(f"Appender started — writing to {main_path}", file=sys.stderr)
    print(f"  interval ≈ {interval}s, burst 1–{burst}", file=sys.stderr)
    print("  Ctrl-C to stop", file=sys.stderr)

    # Absorb SIGTERM gracefully.
    stop = [False]

    def _stop(sig, frame):
        stop[0] = True

    signal.signal(signal.SIGTERM, _stop)

    total = 0
    try:
        while not stop[0]:
            # Sleep with a little jitter so arrivals look organic.
            jitter = random.uniform(0.5 * interval, 1.5 * interval)
            time.sleep(jitter)

            n = random.randint(1, burst)
            now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
            with open(main_path, "a") as mf, open(auth_path, "a") as af:
                for _ in range(n):
                    app = random.choice(apps)
                    entry = make_entry(now, app)
                    line = json.dumps(entry)
                    mf.write(line + "\n")
                    mf.flush()
                    if entry["facility"] in auth_facs:
                        af.write(line + "\n")
                        af.flush()
                    total += 1
            print(f"  +{n} entries (total {total})", file=sys.stderr)
    except KeyboardInterrupt:
        pass

    print(f"Appender stopped after {total} entries.", file=sys.stderr)


if __name__ == "__main__":
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--interval", type=float, default=2.5, help="average seconds between batches (default: 2.5)"
    )
    p.add_argument("--burst", type=int, default=3, help="max entries per batch (default: 3)")
    args = p.parse_args()
    appender(args.interval, args.burst)
