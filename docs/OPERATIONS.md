# Operations — building, deploying, and live verification

## Topology

The reference deployment: a production GALAXY 816 (nicknamed "Q1" below)
on a venue's AVB network, mirrored by the proxies on a Raspberry Pi 5 on
the same segment. The Pi is reached over ssh; the examples below use the
alias `<pi>` for it. Adapt the device addresses to your own:

- A GALAXY's IPv4 is often on a link-local or otherwise unroutable
  subnet; its **IPv6 link-local** address needs no configuration and is
  derived from the entity_id (`0x001cabfffe00c8f0` becomes
  `fe80::21c:abff:fe00:c8f0%eth0`: flip the universal/local bit of the
  OUI and keep the `fffe` in the middle).
- ASCII port-forward for tools on the workstation:
  `ssh -N -L "25003:[<device-link-local>%eth0]:25003" <pi>`
- Treat a venue device as **in active use**. Reads (`/path`, `?path`)
  are safe; treat processing state as live — no processing writes
  without coordination.

## Building the deb (mac host)

```sh
../container-build.sh atdecc-control   # umbrella, dependency-aware
```

Since 2026-09-01 the umbrella's top-level `./container-build.sh`
discovers this tree (any nested package with its own
`scripts/container-build.sh`) and rebuilds a stale dependency deb
FIRST — always prefer it over calling `scripts/container-build.sh`
directly, which installs whatever `core`/`crypto`/`audio`/`avb` debs
already sit in `../deb-output/` and will silently link an old
library if one of them changed (this bit a live deploy: the bridge
shipped without avb's LOCALE→STRINGS chase while the local build had
it). The umbrella aggregate (`local-build.sh`) also configures,
builds, and tests this tree with everything else. Output:
`../deb-output/statusbar-atdecc-control_<ver>-<buildid>_arm64.deb`.

Shortcut when the dependencies just landed through CI: the autobuild
host leaves the exact arm64 dep set for the landed umbrella commit in
its `deb-output/` (one shared build id, e.g. `20260908033808`). Park
the old set and pull that one in, then the direct script is safe:

```sh
mkdir -p ../deb-output/old-<date>
mv ../deb-output/statusbar-{core,crypto,audio,avb}{,-dev}_*.deb ../deb-output/old-<date>/
rsync -av --include='statusbar-*<buildid>*.deb' --exclude='*' \
    <ci-host>:<autobuild-work>/statusbar/deb-output/ ../deb-output/
scripts/container-build.sh
```

(`scp` no longer expands remote globs — use rsync.) The script globs
`statusbar-<dep>_*.deb`, so two versions of one dep side by side would
both be handed to apt-get; the parking step is not optional.

The deb installs the tools into `/usr/local/bin` and its postinst
grants the proxies raw-network capabilities:

```
setcap cap_net_raw,cap_net_admin=ep /usr/local/bin/statusbar-galaxy-proxy
setcap cap_net_raw,cap_net_admin=ep /usr/local/bin/statusbar-nadia-cp-proxy
```

so the proxies run unprivileged (no sudo, which the Pi's policy blocks
for daemon launches anyway).

## Deploying to the Pi

```sh
scp ../deb-output/statusbar-atdecc-control_<...>_arm64.deb <pi>:/tmp/
ssh <pi> 'sudo dpkg -i /tmp/statusbar-atdecc-control_<...>_arm64.deb'
```

### Proxy restart discipline

One proxy process per mirrored device; name the pidfile and log after
the device's entity_id so instances never collide (the reference
deployment uses the id's low 24 bits, `00c8f0`). **Never `pkill -f
statusbar-galaxy-proxy` over ssh** — the pattern matches the ssh remote
command line itself and kills the session. Restart by pidfile:

```sh
ssh <pi> 'ID=00c8f0; OLD=$(cat /tmp/galaxy-proxy-$ID.pid); kill "$OLD"; sleep 1; \
  setsid nohup /usr/local/bin/statusbar-galaxy-proxy \
    "<device-link-local>%eth0" <device-entity-id> eth0 \
    >/tmp/galaxy-proxy-$ID.log 2>&1 < /dev/null & \
  echo $! > /tmp/galaxy-proxy-$ID.pid'
```

Optional: `--model GX-408|GX-816|GX-816-AES3` sizes the AEM meter
banks (default GX-816 — correct for the Q1). Healthy startup log:

```
galaxy-proxy: entity_id 0x70b3d5edc200c8f0 (mirroring 0x001cabfffe00c8f0 ...)
southbound connected; state requested
advertising on eth0
```

The proxy's entity_id is derived: `0x70B3D5EDC2` (JDKS OUI-36 bank
0xC2) + the low 24 bits of the mirrored device's id.

### Network outages and device reboots: no restart needed

Both proxies watch their southbound connection themselves (TOOLS.md,
"Southbound liveness"): a device that goes quiet is pinged, one that
does not answer is dropped, and the proxy reconnects every 5 s until
it is back, then re-requests the full state. The log shows the whole
episode:

```
southbound connection lost: no reply to liveness ping within 5000 ms (nothing received for 15010 ms) — reconnecting
reconnect failed (attempt 1): connect timeout; retrying every 5 s
reconnect failed (attempt 120): No route to host; retrying every 5 s
southbound reconnected after 212 failed attempt(s); state requested
```

Northbound service continues throughout on the last mirrored state,
so the widget bridge keeps the proxy entity (its values are stale
until the reconnect). A restart is still the thing to do when the
device's *model* changed (a different Nadia design, a GALAXY of
another `--model`), never for a cable, switch or power event.

### Multiple devices, multiple instances

Run one proxy per GALAXY, each with its own device address, entity_id
argument, and pidfile/log (all on one Pi, or spread across Pis).
Instances coexist on one interface: each derives a unique northbound
entity_id, ignores AECP not addressed to it, and owns its own
southbound ASCII connection — the only hard requirement is that the
device's ASCII port is reachable from the host. Safety rails:

- launch preflight: the tool sends a targeted ENTITY_DISCOVER and
  refuses to start if the derived entity_id is already advertised by a
  live entity (a second instance for the same device, or a low-24-bit
  derivation collision);
- each `--model` variant serves its own `entity_model_id`
  (GalaxyModelBanks), so mixed-model fleets don't poison controller
  model caches.

### Nadia CP proxy

`statusbar-nadia-cp-proxy` follows the same discipline (its own
pidfile/log named after the Nadia's low 24 bits; entity_id bank
`0x70B3D5EDC4`), plus a shape: the AEM is generated from the loaded
design's channel counts, so restart the proxy whenever a different
design is loaded. `--discover` counts the channels with read-only
gets on the text-command port 28034.

```sh
ssh <pi> 'ID=959361; OLD=$(cat /tmp/nadia-cp-proxy-$ID.pid 2>/dev/null); [ -n "$OLD" ] && kill "$OLD"; sleep 1; \
  setsid nohup /usr/local/bin/statusbar-nadia-cp-proxy \
    "<core-link-local>%eth0" <core-entity-id> eth0 --discover \
    >/tmp/nadia-cp-proxy-$ID.log 2>&1 < /dev/null & \
  echo $! > /tmp/nadia-cp-proxy-$ID.pid'
```

The tool refuses to start when the design has no channels (nothing
loaded), so an empty Nadia leaves no half-configured entity behind.

## Live verification (all read-only)

From the Pi (tools from the `statusbar-avb` deb):

```sh
# Full AEM crawl of the proxy — expect 1121 descriptors, 0 failures in
# the proxy's section. ("Failed to parse descriptor" lines under the
# REAL GALAXY's entity are a monitor-tool limitation on STRINGS
# descriptors, not a proxy fault.)
sudo statusbar-atdecc-monitor --interface=eth0 --enumerate --target=0x70b3d5edc200c8f0

# GET a control and compare with the device's ASCII value.
sudo statusbar-aem-control --interface=eth0 \
  --target-entity-id=70:b3:d5:ed:c2:00:c8:f0 --descriptor-index=1
# CONTROL[1] 'Input 1 Gain' ... must equal `/processing/input/1/gain`
# read over ASCII (port 25003).

# Meter banks (read-only telemetry served from mirrored state):
# index 1004 = 'Input Meters', 8 x CONTROL_ARRAY_FLOAT; values track
# the live program feed between reads.
sudo statusbar-aem-control --interface=eth0 \
  --target-entity-id=70:b3:d5:ed:c2:00:c8:f0 --descriptor-index=1004
```

# Model-store round trip (the full-loop check):
sudo statusbar-galaxy-model-verify eth0 0x70b3d5edc200c8f0 /tmp/galaxy-models
# expects: 1121 descriptors, wire fingerprint == generated, PASS.

ASCII spot checks (via the port-forward, or python on the Pi): a bare
`/path` line returns `/path='value'`; `?path` returns the describe
dict; `@N` alias addressing is accepted and answered with the
canonical path.

## Operational notes

- **Supervisor state machine**: `start()` calls `host_.on_link_up()`
  once — that (via poll) starts ADP. Never feed the supervisor's
  `on_timeout` from the poll loop: Init's Timeout event is the gPTP
  watchdog back to Down and kills ADP.
- **Southbound sync**: on connect the proxy dumps and subscribes to
  all six top-level categories. Mirrored values are stored as AEM
  control state via the same path as a controller SET (self-applies
  bypass the northbound read-only rejection); the device does not echo
  sets to the setting connection and applies them asynchronously.
- **ASCII server hiccup**: the Q1's text server has been observed to
  go silent (accepts TCP, no replies) and recover on its own after
  ~8 minutes; retry before assuming the tunnel is down.
- **Dependency versions**: the proxy needs statusbar-avb with the
  raised control-surface caps and the deferred-send aem-control tool
  (avb branch `ci/nanoavb-matrix-grid-size`: db4b823, 4c8674f,
  8ce6dce, d7bb00e).

## Widget bridge on the Pi

The M8 web control surface runs beside the proxy (the deb grants it
raw-net caps; the reference site installs under
`/usr/local/share/statusbar-atdecc-control/widget-bridge-site/`):

```sh
ssh <pi> 'setsid nohup /usr/local/bin/statusbar-widget-bridge eth0 \
    --port 8091 --backend raw \
    --manifest /usr/local/share/statusbar-atdecc-control/widget-bridge-site/manifest.toml \
    >/tmp/widget-bridge.log 2>&1 < /dev/null & echo $! > /tmp/widget-bridge.pid'
```

Reach it from the mac with `ssh -N -L 8091:localhost:8091 <pi>`
then open `http://localhost:8091/`. Stop by pidfile (never `pkill -f`
over ssh — same trap as the proxy):

```sh
ssh <pi> 'kill $(cat /tmp/widget-bridge.pid)'
```

Live-verified 2026-08-30/31: discovers the Q1 (4 controls), the proxy
(1033 widgets — names, folded ranges/units, and symbols all correct,
e.g. `Input 1 Gain` −90..+10 dB step 0.1 as `cfg0/au0/cb0/ctl:gain/0`)
and a third-party JOYNED MU16. GET returns the live mirrored value;
SET round-trips with the device's confirming echo (test writes only
set a control to its current value — the proxy's echo suppression then
sends nothing southbound, zero venue impact); a SET from a second
controller (`statusbar-aem-control`) arrives at every WS client as an
unsolicited push; and the Q1's telemetry controls stream continuously
as live unsolicited updates.

2026-08-31 additions, all live-verified the same way: mode selectors
(authored sources, INPUT_MODE_* labels, the venue's live mode
selected), enum-labeled filter-type selects, dB meter banks,
temperatures in °C, the four crosspoint matrices with labeled rows
carrying the venue's real routing (set-to-current on a live 0 dB
crosspoint confirmed, nothing southbound), and the connect-time live
value sweep (`Input 1 Gain` showing the venue's −17.1 dB on a cold
start, cross-checked against `statusbar-galaxy-mirror --get`).

2026-09-01 additions: localized names preferred over object_name
everywhere (the Q1's four "orphan" controls turned out to be the
Milan-mandated IDENTIFY plus Meyer's Logger / Erase Identity /
Hardware Info — leave Erase Identity alone), identify wink buttons on
all three entities, the light/dark theme toggle, stacked
name/path/control widget rows, and offline scene capture/recall
(PLAN §12.8; buttons appear only when the hello frame advertises the
endpoints).

**Identify quirks (2026-09-02):** the JOYNED MU16 pads its 1 x UINT8
IDENTIFY GET/SET_CONTROL responses to 13 bytes; the bridge now accepts
padded bodies (it used to drop them, so identify clicks were never
confirmed). All three venue devices report `reset_time` 0 on IDENTIFY —
none self-clears — so a lit button means the device is identifying
right now and the next click turns it off (write min). When a button
seems stuck lit, that is the device's real state, not a bridge fault;
confirm with `statusbar-aem-control … --descriptor-index=0`.

**Proxy/device state divergence:** if a widget's state and the
device disagree (the 2026-09-01 mute incident — an unmute suppressed
against a stale mirror, fixed by the optimistic local echo, PLAN
§7.3), the direct repair path is the mirror tool against the device
itself:

```sh
statusbar-galaxy-mirror <host> --get /processing/input/2/mute
statusbar-galaxy-mirror <host> --set /processing/input/2/mute false
```

Write the value the proxy already displays — that reconverges device
and proxy without another northbound round trip.

**Model id note:** the proxy's entity_model_id bank is
`0x70B3D5EDC10000{23,24,25}` since the localized string table
(2026-09-01; `{20,21,22}` were the authored mode-selector generation
of 2026-08-31); upgrading the proxy binary requires the pidfile
restart above to serve the new model, and controllers cache by this
id.

**Startup ordering:** after a proxy restart it spends a couple of
minutes consuming the GALAXY's full-state dump. The bridge tolerates
this (grids and control values re-read once its unsolicited
registration confirms — PLAN §12.6), but expect served values to
settle only after the dump completes.
