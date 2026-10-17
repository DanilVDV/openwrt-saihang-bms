# Installation and usage

Copy the built `.apk` to the router, then:

```sh
apk add --allow-untrusted /tmp/saihang-bms*.apk
saihang-bms --version
saihang-bms -p adapter status
saihang-bms -v scan --timeout 8
saihang-bms read-all --timeout 8
```

If upgrading an already installed locally built package requires it, investigate `apk` conflicts before considering `--force-overwrite`.

Commands:

```sh
saihang-bms scan --timeout 10
saihang-bms scan --timeout 10 --all
saihang-bms -p read <BMS_MAC>
saihang-bms read-all --timeout 8
saihang-bms watch --interval 5 <BMS_MAC>
saihang-bms -p registers <BMS_MAC> 0 72
saihang-bms adapter status
saihang-bms adapter reset
saihang-bms adapter reset --hard
saihang-bms --no-recover read-all --timeout 8
saihang-bms --recover-retries 1 read-all --timeout 8
```

Output is JSON on stdout, diagnostics on stderr. `watch` uses newline-delimited JSON. Bash example:

```sh
json="$(saihang-bms read-all --timeout 8)" || exit 1
printf '%s\n' "$json" | jsonfilter -e '@[0].pack.soc_pct'
```

For Python, see [`examples/read.py`](../saihang-bms/examples/read.py). Avoid embedding a single fixed BMS MAC in application code; use scan/read-all or external configuration.
