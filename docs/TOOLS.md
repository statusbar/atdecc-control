# Tool reference

All tools install to `/usr/local/bin` from the deb (or build into
`build/statusbar/*/`). Addresses accept the device's `@N` alias form
wherever noted.

## statusbar-galaxy-namespace (`controlplane_devices`)

Offline namespace inspection — no device needed.

```
statusbar-galaxy-namespace              path list (handle order)
statusbar-galaxy-namespace --describe   the full `?` describe response
```

The describe response carries the folded live metadata (ranges, units,
enum lists, aliases, device names, defaults) and parses as both a
Python literal and a JavaScript expression.

## statusbar-galaxy-mirror (`controlplane_tools`)

Live GALAXY state dump and watch over ASCII.

```
statusbar-galaxy-mirror <host> [--port n] [--throttle ms]
    (default)             dump every category: path<TAB>value + reconciliation
    --watch <pattern>     subscribe and stream updates
    --describe <pattern>  raw `?` describe response
    --get <pattern>       raw response lines for one get      (accepts @N)
    --set <path> <value>  set one point, print the echo
```

`<host>` may be IPv4, IPv6 (link-local with `%iface`), or mDNS
(`mslg-gx-<model>-<serial>.local`). Patterns pass through to the
device's server-side regex engine.

## statusbar-nadia-mirror (`controlplane_tools`)

Live Nadia CP (Meyer LCS Core) state dump and watch over the Core
text-command port (28034). Read-only unless `--set`/`--raw` is given.

```
statusbar-nadia-mirror <host> [--port n]
    --shape k=v,...       design channel counts: inputs, buses, outputs,
                          listens, vgroups, bands
    --discover [--max n]  probe the counts by read-only gets of the
                          per-channel address points; prints the shape
    (default)             get every pattern: path<TAB>value + reconciliation
    --watch               subscribe to every pattern and stream updates
    --get <addresses>     raw response lines, command form ("input 1-8 level")
    --set <path> <value>  set one point (canonical path), print the echo
    --raw <line>          send one command line verbatim
```

Uses the text-command port only.

## statusbar-galaxy-scene (`controlplane_tools`)

Scene authoring: offline against the schema, or live via the mirror.
Scene files bind to GALAXY paths, never handles. Selection patterns use
the schema grammar (`*` any segment, `#` numeric) or a single `@N`.

```
--author <scene> <pattern...>       from schema defaults (writable scalars)
--validate <scene>                  offline matched/missing/unmatched report
--capture <scene> --host <h> [...]  capture live values
--diff <scene> --host <h>           list drift vs the scene
--recall <scene> --host <h>         set every differing value, settle, re-verify
```

## statusbar-galaxy-proxy (`galaxy_proxy`)

The companion ATDECC entity. See OPERATIONS.md for deployment.

```
statusbar-galaxy-proxy <galaxy-host> <galaxy-entity-id> <interface>
    [--port n] [--throttle ms] [--model GX-408|GX-816|GX-816-AES3]
    [--ping-quiet ms] [--ping-timeout ms]
```

Northbound: ADP advertisement, full AEM descriptor service (1121
descriptors from the generated blob), SET/GET_CONTROL, SET_MATRIX
region writes, snapshot recall via the active-snapshot-id control,
unsolicited notifications. Southbound: ASCII dump + subscription
mirror; controller SETs become `/path='value'` sets with echo
suppression. The proxy's AVB_INTERFACE descriptor and GET_AVB_INFO
mirror the real GALAXY's own 1722.1 state (statusbar-avb
`AvbInfoMirror`: it reads the GALAXY's AVB_INTERFACE descriptors,
registers for its unsolicited notifications and polls its
GET_AVB_INFO), so a controller — the widget bridge included — sees
the GALAXY's gPTP port, grandmaster and MSRP domain through the proxy,
never the Pi's.

Southbound liveness: after `--ping-quiet` ms (default 10000) without a
byte from the device the proxy sends a describe of
`/status/model_string` — a request the device always answers and whose
reply the mirror ignores, so nothing is re-reported northbound. If
nothing at all arrives within `--ping-timeout` ms more (default 5000)
the link is declared lost and the proxy reconnects on its own, every
5 s until the device is back, then re-requests the full state and
re-subscribes; northbound service continues on the last mirrored
state meanwhile. Kernel TCP keepalive (15 s idle, 3 × 5 s probes) is
enabled on the connection as a backstop. `--ping-quiet 0` disables
the application-level check. This exists because a repatch that moves
the device to another segment, or a device power cycle, leaves a TCP
connection that is never written to looking established forever (a
production site,
2026-10-13: four days of stale state until a manual restart).

## statusbar-nadia-cp-proxy (`nadia_proxy`)

The Nadia CP (Meyer LCS Core) companion ATDECC entity: every control
point of the loaded mixer design as an AEM CONTROL (PLAN.md §13).

```
statusbar-nadia-cp-proxy <nadia-host> <nadia-entity-id> <interface>
    [--port n] (--shape inputs=N,buses=N,outputs=N,listens=N,vgroups=N,bands=N
               | --discover [--max n])
    [--ping-quiet ms] [--ping-timeout ms]
```

The AEM model is generated from the design's channel counts at
startup — give them with `--shape`, or `--discover` counts them with
read-only gets on a throwaway connection first. Refuses to start when
the design has no channels or the derived entity id is already live.
Northbound: ADP, generated descriptor service (a CONTROL_BLOCK per
strip, crosspoints as chunked CONTROL_ARRAY rows), GET/SET_CONTROL
served from / translated through the mirror, unsolicited
notifications. Southbound: `get` + `subscribe` of every catalogue
pattern on the text-command port (28034); controller
SETs become `set <address> = <value>` with echo suppression. As with
the GALAXY proxy, AVB_INTERFACE and GET_AVB_INFO mirror the Nadia's
own 1722.1 state over the wire, not the Pi host's. Southbound liveness
works as for the GALAXY proxy (same flags and defaults); the ping is a
`get` of the system level tagged `liveness`, which the Core answers
whether or not the design populates it, and the proxy drops the tagged
reply instead of treating it as a control report.

## statusbar-galaxy-aem (`galaxy_proxy`)

Writes the generated `.aem` blob / dumps the descriptor set.

## statusbar-aem-control (statusbar-avb repo)

Generic CONTROL GET/SET against any ATDECC entity: reads the CONTROL
descriptor to learn the value format first.

```
statusbar-aem-control --interface=eth0 \
  --target-entity-id=70:b3:d5:ed:c2:00:c8:f0 \
  --descriptor-index=N [--set=v1,v2,...]
```

Entity ids use the colon-separated EUI-64 form.

## statusbar-atdecc-monitor (statusbar-avb repo)

ADP/AEM/ACMP monitor; `--enumerate --target=0x...` walks a full
descriptor tree headless (the M5/crawl acceptance tool).

## statusbar-galaxy-model-verify (`galaxy_proxy`)

Live model-store round trip against a running proxy (run on the Pi —
needs L2 access and cap_net_raw/root):

```
statusbar-galaxy-model-verify <interface> <entity-id-hex> <store-dir>
```

Crawls the full descriptor tree over AECP (configuration-driven walk),
persists the raw wire bytes into a nanoavb AemModelStore, and verifies:
wire fingerprint == locally generated fingerprint (per-unit fields
canonicalized), every descriptor loads back byte-identical from the
stored blob, and the derived symbol table and listing are present. On
an identity mismatch it still persists the crawl and names the
differing descriptors with delta offsets — this is what caught the
MATRIX/MATRIX_SIGNAL trailer-truncation serving bugs (avb 05cd61e,
08d2fe14).

The crawl walks the configuration's descriptor_counts and also chases
each LOCALE's STRINGS range through `base_strings` — real devices (the
GALAXY, JOYNED interfaces) count only the LOCALE, so without the chase
a stored model's localized names would dangle.

With `--nadia <shape>` (the k=v,... form the proxy logs) or
`--nadia-discover <nadia-host>` (probe the shape from the Nadia's own
text port, exactly as the proxy's `--discover` does) the identity gate
compares against the regenerated nadia-cp model instead of the galaxy
one — proxy and gate then derive from the same live design.

With `--foreign` the generated-model identity gate is skipped: use it
against a device that is not the proxy (the real GALAXY itself) to
exercise the store's vendor-neutral byte-exactness — live-verified
against the Q1's own 78-descriptor model, stored beside the proxy's in
the same fingerprint-keyed directory.

## statusbar-widget-bridge (`widget_bridge`)

    statusbar-widget-bridge <interface> [--port n] [--manifest path]
        [--backend raw|avb] [--controller-id 0x...]

The M8 web control surface (PLAN §12): runs a statusbar ATDECC
controller on `<interface>`, serves HTTP (port 8080 by default) with
the WS subprotocol `statusbar.widgets.v1` on `/ws`, and projects every
discovered entity's CONTROL, SIGNAL_SELECTOR, and MATRIX descriptors
as live widgets:

- linear controls as sliders with unit suffixes (two-state ones,
  including the AEM 0/255 convention, as toggles; small discrete
  integer grids — and anything the label provider enumerates, with the
  device's own enum labels — as selects);
- numeric selector controls and SIGNAL_SELECTORs as selects (mode
  selectors labeled INPUT_MODE_* on galaxy models);
- array controls as read-only meter banks (live bars, dB peak text;
  Kelvin temperatures display as Celsius);
- MATRIX descriptors as crosspoint heat grids with labeled rows,
  output headers, and click-to-set cells;
- UTF8 as read-only text; everything else listed as opaque.

At connect the bridge sweeps every control's live value (PLAN §12.6),
so widgets show the device's actual state, not authored defaults. SETs
round-trip through AECP (the confirming broadcast comes from the
device's response); other controllers' changes arrive as unsolicited
notifications and fan out to every connected client.
Each entity's AVB_INTERFACE descriptors are shown as a gPTP panel —
the authored port identity plus the live GET_AVB_INFO state
(grandmaster, AS-capable/gPTP/SRP/link flags, propagation delay, MSRP
mappings), polled and taken from unsolicited pushes; all of it is what
the entity reports over 1722.1.
`--manifest statusbar/widget_bridge/site/manifest.toml` serves the
built-in single-file reference app at `/` (installed copies live under
`share/statusbar-atdecc-control/widget-bridge-site/`).

The raw backend needs CAP_NET_RAW/root, like the other raw-socket
tools; on macOS the default backend is the system AVB framework.

Offline (PLAN §12.7) — no interface, no network, no capabilities:

```
statusbar-widget-bridge --offline <blob.aem>            any .aem image
statusbar-widget-bridge --offline-store <dir> <fp>      an M7 model-store entry
statusbar-widget-bridge --offline-galaxy [GX-816|...]   the generated model
```

The same widget UI against an in-process model: SETs persist in RAM
for the life of the process and fan out to every connected browser.
Point --offline-store at a statusbar-galaxy-model-verify store to
browse a captured device's model with no hardware (capture a real
device into a store with `statusbar-galaxy-model-verify --foreign`).

`--offline-galaxy` also serves the scene endpoints (PLAN §12.8),
speaking the same M7 scene format as statusbar-galaxy-scene:

```
curl http://host:8091/scene/capture > venue.scene
curl -X POST --data-binary @venue.scene http://host:8091/scene/recall
```

Capture writes every writable point (controls, matrix crosspoints,
mode selectors) as `/path<TAB>value` lines; recall applies a scene
through the model's own SET paths, answers `recall: matched N applied
N read-only N failed N missing N`, and refreshes every connected
browser. The web UI shows "capture scene" / "recall scene" buttons in
the header whenever the endpoints exist (the hello frame advertises
them), so the whole loop works without curl. Author a scene offline
in the web UI, capture it, and recall it onto the live device later
with `statusbar-galaxy-scene --recall`.
