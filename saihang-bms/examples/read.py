#!/usr/bin/env python3
import json
import subprocess
import sys

args = ["saihang-bms", "read-all", "--timeout", sys.argv[1] if len(sys.argv) > 1 else "8"]
proc = subprocess.run(args, check=True, capture_output=True, text=True)
for bms in json.loads(proc.stdout):
    print(bms)
