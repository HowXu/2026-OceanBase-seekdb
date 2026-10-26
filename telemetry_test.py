#!/usr/bin/env python3
import argparse
import json
import time
import urllib.error
import urllib.request
import uuid

URL = "https://openwebapi.oceanbase.com/api/web/oceanbase/report"

parser = argparse.ArgumentParser(description="Submit seekdb telemetry test payloads")
parser.add_argument("--count", type=int, default=1, help="提交次数，默认 1")
parser.add_argument("--interval", type=float, default=1.0, help="请求间隔（秒），默认 1")
parser.add_argument(
    "--id",
    default=str(uuid.uuid4()),
    help="测试实例 ID；不指定时本次运行生成一个 UUID",
)
args = parser.parse_args()

if args.count < 1 or args.interval < 0:
    parser.error("--count 必须大于 0，--interval 不能小于 0")

payload = {
    "component": "seekdb",
    "content": {
        "id": args.id,
        "version": "test",
        "reporter": "telemetry-test",
        "host": {
            "os": "Linux",
            "osVersion": "test",
            "cpu": "test",
            "cpuCount": 1,
            "memorySize": "1G",
        },
        "instance": {
            "hostHash": "0" * 64,
            "port": 2881,
            "createTimestamp": int(time.time() * 1_000_000),
        },
        "resource": {
            "cpuCount": 1,
            "memoryLimit": "1G",
            "logDiskSize": "1G",
            "dataFileSize": "1G",
        },
        "event": "bootstraped",
        "telemetryVersion": 6,
    },
}

body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
print(f"test id: {args.id}")
print(f"target: {URL}")

for i in range(args.count):
    request = urllib.request.Request(
        URL,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            print(f"[{i + 1}/{args.count}] HTTP {response.status}: "
                  f"{response.read().decode('utf-8', errors='replace')}")
    except urllib.error.HTTPError as error:
        print(f"[{i + 1}/{args.count}] HTTP {error.code}: "
              f"{error.read().decode('utf-8', errors='replace')}")
    except urllib.error.URLError as error:
        print(f"[{i + 1}/{args.count}] 请求失败: {error.reason}")

    if i + 1 < args.count:
        time.sleep(args.interval)
