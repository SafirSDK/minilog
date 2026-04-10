#!/usr/bin/env python3
"""
Generate initial test JSONL data for minilog-web-viewer.
Writes logs/syslog.jsonl and logs/auth.jsonl.
"""

import datetime
import json
import os
import random
import sys


def generate(seed=42, count=400, now=None):
    random.seed(seed)
    if now is None:
        now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)

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

    messages = {
        "nginx": [
            "GET /api/v1/users 200 1ms",
            "POST /api/v1/login 401 2ms",
            "GET /health 200 0ms",
            "upstream timed out (110: Connection timed out) while reading response header",
            "worker process 12345 exited with code 1",
            "client 10.0.0.5 request: GET /api/v2/reports/annual-summary?year=2025&department=engineering&include_subteams=true&format=detailed HTTP/2.0, host: app.example.com, referrer: https://app.example.com/dashboard/reports?tab=annual&view=expanded&filter=active, user-agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36, upstream response time: 12.532s, upstream addr: 10.0.0.6:8080, request_id: a]b3c4d5-e6f7-8901-2345-6789abcdef01",
        ],
        "postgres": [
            "connection received: host=10.0.0.5 port=54321",
            "checkpoint starting: time",
            "checkpoint complete: wrote 142 buffers (0.9%)",
            "duration: 1523.421 ms  statement: SELECT * FROM events WHERE ts > NOW() - INTERVAL '1 day'",
            "ERROR: deadlock detected",
            "LOG: duration: 8234.192 ms  statement: SELECT e.id, e.event_type, e.created_at, e.payload, u.username, u.email, u.department, t.team_name, t.manager_id FROM events e JOIN users u ON e.user_id = u.id LEFT JOIN teams t ON u.team_id = t.id WHERE e.created_at BETWEEN '2025-01-01' AND '2025-12-31' AND e.event_type IN ('login', 'logout', 'password_change', 'role_update', 'permission_grant', 'api_key_rotation') AND u.department = 'engineering' ORDER BY e.created_at DESC LIMIT 10000",
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
            "Call Trace: [<ffffffff81234567>] ? schedule+0x3f/0x90 [<ffffffff81234890>] ? io_schedule+0x16/0x40 [<ffffffff812fab30>] ? get_request_wait+0x1c0/0x250 [<ffffffff812fac10>] ? blk_queue_bio+0x50/0x2b0 [<ffffffff81234567>] ? generic_make_request+0xc0/0x110 [<ffffffff81234890>] ? submit_bio+0x73/0x140 [<ffffffff811fa230>] ? ext4_io_submit+0x40/0x50 [<ffffffff811fa890>] ? ext4_bio_write_page+0x190/0x2a0 [<ffffffff811e3450>] ? mpage_submit_page+0x60/0x80 [<ffffffff811e3890>] ? mpage_map_and_submit_buffers+0x110/0x200",
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

    months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]

    def rfc3164_ts(ts):
        return f"{months[ts.month - 1]} {ts.day:2d} {ts.hour:02d}:{ts.minute:02d}:{ts.second:02d}"

    def rfc5424_ts(ts):
        return ts.strftime("%Y-%m-%dT%H:%M:%SZ")

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
            msg_time = rfc3164_ts(ts)
        else:
            proto = "RFC5424"
            msg_time = rfc5424_ts(ts)
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

    entries = []
    for _ in range(count):
        minutes_ago = random.randint(0, 1440)
        ts = now - datetime.timedelta(minutes=minutes_ago)
        app = random.choice(apps)
        entries.append((ts, make_entry(ts, app)))

    entries.sort(key=lambda x: x[0])

    auth_facs = {"auth", "authpriv"}
    return entries, auth_facs


def write_files(entries, auth_facs, outdir):
    os.makedirs(outdir, exist_ok=True)
    main_path = os.path.join(outdir, "syslog.jsonl")
    auth_path = os.path.join(outdir, "auth.jsonl")
    with open(main_path, "w") as mf, open(auth_path, "w") as af:
        for _, entry in entries:
            line = json.dumps(entry)
            mf.write(line + "\n")
            if entry["facility"] in auth_facs:
                af.write(line + "\n")
    main_count = len(entries)
    auth_count = sum(1 for _, e in entries if e["facility"] in auth_facs)
    print(f"Written {main_count} entries to {main_path}", file=sys.stderr)
    print(f"Written {auth_count} entries to {auth_path}", file=sys.stderr)


if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    entries, auth_facs = generate()
    write_files(entries, auth_facs, os.path.join(script_dir, "logs"))
