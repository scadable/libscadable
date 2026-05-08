# esp-idf-hello-world

Minimum viable libscadable firmware. Boots, publishes "world" once, sits idle.

## Build

```bash
idf.py set-target esp32-s3
idf.py build
idf.py -p /dev/cu.usbserial-* flash monitor
```

You should see `scadable hello-world running` in the monitor.

For real projects, **don't start here** — fork [scadable/esp-idf-starter](https://github.com/scadable/esp-idf-starter) instead. It has the full `.scadable/` folder + GitHub Actions verify + flash partitions + secure boot configured.
