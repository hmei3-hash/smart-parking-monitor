# Phase 6 Node Firmware

Production node firmware: camera capture, on-device INT8 CNN inference, and
MQTT publish. This is the Phase 5 inference path from
[`firmware/bringup/t4_camera_tflite/`](../bringup/t4_camera_tflite/) merged
with the MQTT layer validated in [`node-sim/`](../../node-sim/).

## Build

```bash
cp src/secrets.h.example src/secrets.h    # fill in SSID, password, broker IP
pio run -e node1 -t upload                # node2, node3 for the other boards
pio device monitor
```

`NODE_ID` is a build flag, not a runtime setting — each board is flashed from
its own env. Without `-e`, PlatformIO builds and uploads all three envs in
sequence to the same port, so the flag is required rather than optional.

With all three boards connected, pin the port per env in `platformio.ini`
(`upload_port` / `monitor_port`) rather than relying on auto-detection.

## What it publishes

```
parking/spot1/node{N}      {"node":1,"spot":1,"occupied":1,"seq":42,"uptime":430}
parking/status/node{N}     "online" | "offline"   (retained, Last Will)
```

`seq` and `uptime` are diagnostics rather than payload: gaps in `seq` measure
publish loss, and a drop in `uptime` identifies a reboot. Both turned out to
matter — see [phase6-mqtt-integration.md](../../docs/phase6-mqtt-integration.md).

## Notes on the merge

Three things changed relative to the two source files:

**`runDetection()` returns `int` instead of `void`.** `1`/`0`/`-1` lets
`loop()` skip the publish on a capture or inference failure, rather than
publishing a stale or fabricated result.

**`loop()` uses the `millis()` pattern instead of `delay()`.** The Phase 5
loop ended in `delay(INFER_PERIOD)`, which would have blocked `mqtt.loop()`
for the whole interval and stalled keepalive.

**Keepalive raised to 60 s.** The default is 15 s, but one inference cycle is
`INFER_PERIOD` (10 s) plus roughly 4 s of `Invoke()`. At ~14 s per cycle the
default leaves almost no margin and the broker drops the connection
mid-inference.

Camera and model are initialised before WiFi. The WiFi stack allocates
~100 KB of internal SRAM and the tensor arena is a 120 KB static buffer;
bringing WiFi up first risks a heap layout where the arena no longer fits.
