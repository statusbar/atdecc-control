# atdecc-control — Design and Implementation Plan

A control-point data model and an ATDECC control proxy for Meyer Sound Galileo
GALAXY devices.

Status: IMPLEMENTED — milestones M0–M8 are built, tested, and
live-verified against a production GALAXY 816 ("Q1") through the proxy
on a Raspberry Pi (see OPERATIONS.md). This document is kept as the
design record;
§12 describes the newest module, the M8 widget bridge.

---

## 0. Purpose

Two deliverables in one repository, sharing one core:

1. **`controlplane`** — a control-point data model usable unchanged by an
   ATDECC *entity*, an ATDECC *controller*, and an offline scene editor. It is a
   ground-up redesign of the original `ControlPlane`
   (<https://github.com/jdkoftinoff/ControlPlane>), which has been in production
   in Meyer Sound GALAXY since roughly 2014 as the source of truth behind its
   OSC, Muscle, and front-panel interfaces.

2. **`galaxy_proxy`** — a companion ATDECC entity that sits on the network,
   advertises the GALAXY DSP control surface as IEEE 1722.1 AEM CONTROL,
   CONTROL_BLOCK, MATRIX and SIGNAL_SELECTOR descriptors, and translates
   AECP commands into GALAXY ASCII control point traffic. Shipping GALAXY
   firmware exposes no DSP controls over 1722.1; this closes that gap without
   firmware changes.

The proxy is also the cheapest available test rig for the AEM model design:
iterate the descriptor layout against real controllers and real hardware with
no firmware risk, then move the settled model into the device later.

---

## 1. Repository context

### Dependencies

| Package | Role |
|---|---|
| `statusbar-core` | Base utilities, buffers, reactor |
| `statusbar-avb` | IEEE 1722.1 ATDECC (ADP/AECP/ACMP), AEM descriptors, entity stack |
| `statusbar-audio` | Audio types (only if needed) |

Follow the `statusbar-avb` conventions exactly:

- C++23, CMake >= 3.31, `find_package(statusbar-core REQUIRED)` etc.
- Modules live in `statusbar/<module>/`, each with its own `CMakeLists.txt`.
- Targets named `statusbar-<module>`, exported with
  `EXPORT_NAME <module>` so dependents use `statusbar::<module>`.
- Installed executables get a `statusbar-` prefix.
- Version resolved from the latest git tag, flowing into package metadata and
  into entity `firmware_version`.
- `enable_testing()`, per-module tests, bench targets registered into
  `run-all-benches`, honouring `STATUSBAR_AGGREGATE_BUILD`.

### Proposed layout

```
statusbar/
  controlplane/          # core data model, no AVB dependency
  controlplane_devices/  # GALAXY ASCII + Nadia adaptors, schemas, mirrors
  controlplane_aem/      # AEM descriptor <-> handle-space binding
  galaxy_proxy/          # the proxy entity + tool
  controlplane_tools/    # model dump, scene diff, symbol table dump
```

`controlplane` must not depend on `statusbar-avb`. The AEM binding lives in
`controlplane_aem`. This keeps the core usable in a non-AVB device (as the
original is in GALAXY today) and keeps its tests fast.

### Licensing constraint

This project must not incorporate or be derived from GPL/LGPL AVDECC codebases.
Reference material is limited to the IEEE standards, the author's own
repositories (`ControlPlane`, `if3-cpp`, `statusbar-avb`), vendor
documentation, and data files captured from real devices. Where a file format
is needed, prefer the standard's own byte layout (the READ_DESCRIPTOR response
payload, as already used by the `.aem` blob format in `statusbar-avb`) over any
third-party format.

---

## 2. Architecture

Three layers, with a hard boundary between the model and any wire protocol.

```
   AECP (northbound)      ASCII (TCP)        GUI / scene editor
          |                    |                     |
   +------v--------------------v---------------------v------+
   |                     adaptors                            |
   |   address space  <->  ControlPointId (dense uint32)      |
   +---------------------------+-----------------------------+
                               |
   +---------------------------v-----------------------------+
   |                    controlplane core                     |
   |   Model (static, shared, immutable)                      |
   |   Instance (dynamic: desired / reported / version)       |
   |   Selector (bitset)  Subscription  tick()                |
   +---------------------------------------------------------+
```

Key properties:

- **The model is separate from the instance.** One `Model` can back many
  `Instance`s (N identical devices in a controller; one in an entity; one with
  no transport at all for offline editing). A "virtual entity" is an
  `Instance` whose adaptor is null — no special case in the core.
- **No protocol concepts in the core.** No sequence IDs, no inflight timeouts,
  no unsolicited registration state. Adaptors own all of that.
- **Addresses are projections, not identity.** The AEM tuple and the device
  path (GALAXY ASCII, Nadia) are independent indexes into one handle space.

---

## 3. Verified ground truth

Enumerated from two real GALAXY units with `avbdiagnose` on macOS, firmware
`2.10.0-20260211-1241`. **These are facts, not assumptions.**

| | Q1 | Q2 |
|---|---|---|
| `entity_id` | `001cabfffe00c8f0` | `001cabfffe007604` |
| `entity_model_id` | `001cabb80400400a` | `001cabb80400500a` |
| serial | (unit A) | (unit B) |
| input jacks | 4 | 5 |
| clock sources | 23 | 24 |

Everything else is identical between the two.

### Configuration contents (both units)

| Descriptor | Count | Notes |
|---|---|---|
| CONFIGURATION | 1 | **Spacemap is not a second configuration** |
| AUDIO_UNIT | 1 | |
| STREAM_INPUT | 18 | 9 per interface, redundant-paired 0<->9, 1<->10, ... |
| STREAM_OUTPUT | 14 | 7 per interface |
| JACK_INPUT | 4 (Q1) / 5 (Q2) | type `0x000b` AES_EBU; Q2 adds `0x0022` AUDIO_MEDIA_CLOCK |
| JACK_OUTPUT | 0 | analog outputs are not modelled |
| AVB_INTERFACE | 3 | 2 real + "Loop Back Interface", zero MAC, `interface_flags=0000` |
| CLOCK_SOURCE | 23 / 24 | 1 Internal + 4/5 external + 18 input-stream |
| CLOCK_DOMAIN | 1 | |
| CONTROL | 4 | see below |
| CONTROL_BLOCK | 0 | |
| MATRIX / SIGNAL_SELECTOR / MIXER | 0 | |
| EXTERNAL_PORT_* / INTERNAL_PORT_* | 0 | |
| STREAM_PORT_INPUT | 1 | 32 clusters, 32 ch, **0 maps** (dynamic mappings) |
| STREAM_PORT_OUTPUT | 1 | 32 clusters, 32 ch, 2 static maps |
| AUDIO_CLUSTER | 64 | 32 in + 32 out, 1 channel each |
| MEMORY_OBJECT | 7 | 4 firmware, PNG_MANUFACTURER, PNG_ENTITY, LOG_OBJECT |
| LOCALE / STRINGS | 1 / 16 | 112 strings |
| sampling rates | 1 | 96 kHz (`base_frequency=0x00017700`, `pull=0`) |

### Stream names

- Input, per interface: `Audio Sink 1..8`, `Clock Sink 1`
- Output, per interface: `Outputs 1-8`, `Outputs 9-16`, `SIM Outputs`,
  `Input Split`, `48kHz Outputs 1-8`, `48kHz Outputs 9-16`, `Clock Source 1`

Input stream format decodes as AAF, nsr 7 (96 kHz), INT_32, 8 channels. Eight
Audio Sinks x 8 channels = 64 arriving channels, while the stream port exposes
32 mappable clusters — which matches the guide's 8 device inputs + 24 matrix
inputs. **Assumption to verify:** that the 32 clusters are the 32 matrix
sources.

### Existing CONTROL descriptors

| idx | name | control_type | value_type |
|---|---|---|---|
| 0 | IDENTIFY | `90e0f00000000001` | `0001` |
| 1 | Logger | `001cab0000400005` | `801f` |
| 2 | Erase Identity | `001cab000010002f` | `0001` |
| 3 | Hardware Info | `001cab0000100047` | `8001` |

**Meyer already operates a vendor control-type registry under OUI `00-1c-ab`,**
with what appears to be a category scheme (`001cab0000_10xxxx`,
`001cab0000_40xxxx`). New DSP control types must be allocated from that
registry, not from a parallel invention.

### Model versioning

The programming guide (rev B4) shows `entity_model_id 001cabb804004005`; these
units report `001cabb80400400a`. The pattern reads as
`001cabb80400 <variant> <model-rev>`, variant `40`/`50`, revision `05` -> `0a`.
Meyer does bump `entity_model_id` across firmware revisions, so a fingerprint
cache keyed on it behaves correctly for this vendor.

### Naming

Nearly every `object_name` is empty; naming comes from `localized_description`
into 16 STRINGS descriptors. `localized_description` is part of the static
model and is stable across instances, unlike `object_name`, which is mutable
via SET_NAME. Unnamed descriptors use the `0xFFFF` sentinel.

---

## 4. `controlplane` core design

### 4.1 Dense handles

Every control point interns to a `ControlPointId` (`uint32_t`) at model build
time, assigned by a **deterministic canonical walk** of the model so that
handles are stable across process restarts and across machines for the same
model fingerprint.

Everything downstream is array-indexed by handle: value storage, metadata,
dirty state, subscription interest, write permission, scene contents.

### 4.2 Storage

Structure-of-arrays, not pointers into user structs.

```cpp
using ControlPointId = uint32_t;

// Static, shared, immutable once built. One per entity_model_id.
struct Model {
    std::vector<uint64_t> control_type;    // EUI-64
    std::vector<uint8_t>  value_type;      // CONTROL_LINEAR_* / ARRAY / SELECTOR / UTF8
    std::vector<uint16_t> units;           // UNIT_CODE_*
    std::vector<int8_t>   multiplier;      // power of 10
    std::vector<double>   min, max, step, def;
    std::vector<uint16_t> localized;       // packed (strings_index << 3) | string_index
    std::vector<uint8_t>  flags;           // read_only, volatile, ...
    std::vector<AemRef>   aem;             // (config, desc_type, desc_index, value_index)
    SymbolTable           symbols;         // see 6
    Fingerprint           fingerprint;
};

// Dynamic, per device instance.
struct Instance {
    const Model*          model;
    std::vector<uint64_t> reported;        // raw value bits, authoritative from device
    std::vector<uint64_t> desired;         // locally requested, not yet confirmed
    std::vector<uint32_t> version;         // seqlock: even = stable, odd = writer active
    std::vector<uint64_t> confirmed_at_ns;
    Bitset                pending;         // desired != reported, in flight
    Bitset                dirty;           // changed since last tick
};
```

Array values (meters, UTF-8 strings) spill into a side arena indexed by handle.
Only scalars live in the flat array.

**Entity vs controller is the only behavioural difference.** In an entity,
`desired` and `reported` move together and a local set is authoritative. In a
controller or the proxy, a local set writes `desired` and marks `pending`; the
device response writes `reported`; a timeout or error clears `pending` and
leaves `reported` alone, so the UI snaps back to truth.

### 4.3 Selectors are bitsets

`ControlIdentityComparator` in the original was a virtual hierarchy
(`Unique`/`None`/`All`/`Set`) with a `std::set<ControlIdentity>` payload. Replace
it with a bitset over the handle space. `Unique`, `None` and `All` become
degenerate cases; composition becomes bitwise ops.

One type then serves three uses:

- subscription interest
- write permission
- scene contents (a scene is a mask plus values, since scenes are partial)

Scene recall is a masked write. Scene-versus-live diff is `AND` plus a value
compare.

### 4.4 Change fan-out

`controlChanged(handle)` sets one bit. `tick()` does `dirty & interest` per
subscriber and emits handles. Measured against the original design (10,624
control points, 64 subscribers, 256 points of interest each, 20,000 events):

```
comparator + std::set   6274 ns/change   drain  433.9 us/tick
bitset                   0.9 ns/change   drain   33.5 us/tick
```

Per tick that is ~125 ms versus ~51 us. The benchmarks that produced these
numbers are in `bench/` and should be kept as regression tests.

### 4.5 Subscription timing

Carry forward the original's two intervals, but **rename them** — the originals
(`max_update_period_in_milliseconds`, `min_update_period_in_milliseconds`) mean
the opposite of what they say:

```cpp
struct Subscription {
    Selector interest;
    uint32_t throttle_ms;   // minimum interval between change notifications (was "max")
    uint32_t refresh_ms;    // maximum staleness; forces a full interest-set resend (was "min")
};
```

Defaults from GALAXY: 30 ms throttle, 10 s refresh. The periodic full resend is
the existing answer to unacknowledged, lossy notifications — keep it.

### 4.6 Threading

The original held one recursive mutex across `setValue` *and* the subscription
fan-out, so a fader move cost O(subscribers) under a lock the GUI thread also
needed for every repaint. Instead:

- One owner thread mutates values and marks dirty bits.
- Readers use the per-handle seqlock (`version` even/odd) and never block the
  writer.
- Frame-coherent reads snapshot the dirty set into a per-frame buffer under one
  brief lock, then render from the snapshot.
- Batched updates suppress `tick()` for their duration — the original's
  `ChangeNotifierManagerHold` idea, which is the correct place for atomicity.
  Note that **CONTROL_BLOCK is a layout hint only** and carries no atomicity
  semantics; grouping and transactions are different things.

### 4.7 Addressing

Do not use `std::regex`. Measured on 10,624 control points selecting 256:

```
std::regex construction     20.3 us    (per command)
std::regex scan           1759.1 us
sqlite indexed prepared     14.3 us
selector linear scan        18.1 us
trie walk                    2.3 us
```

Also: `sqlite ... path GLOB '/input/*/gain'` took 825.8 us **and returned the
wrong answer** (2304 hits), because GLOB's `*` crosses `/`.

Implement address resolution as a **segment-level trie over interned symbols**,
with numeric segments held as integers rather than digit chains. The skeleton
from `if3_tree.hpp` is the right shape — mmap-backed buffer so the index can be
built once and mapped read-only, and pattern pre-expansion at insert time
rather than matching at query time. The traits need replacing: character-level
nodes with case-insensitive comparators and a 256-way first-character dispatch
are tuned for URLs, not for `/input/137/gain`.

Selector patterns compile once, at subscribe time, into a bitset.

### 4.8 Adaptors

```cpp
template <typename AddressT>
class Adaptor {
    ControlPointId resolve(const AddressT&) const;
    Selector       resolve_pattern(std::string_view) const;
    AddressT       address_of(ControlPointId) const;
};
```

Instantiations: `OscAddress`, `GalaxyAsciiAddress`, `AemRef`. The original
`SchemaAdaptor<AddressT>` design survives essentially intact.

---

## 5. AEM model for the GALAXY DSP

Source: GALAXY Programming Guide rev B4 and the 2026 web edition. The control
point namespace is identical between the two (verified by diff; 306 vs 302
normalized path shapes, all differences being PDF extraction artifacts).

### 5.1 What is not a CONTROL

A large fraction of the GALAXY namespace must not become CONTROL descriptors.

| GALAXY path | AEM home |
|---|---|
| `/entity/*` | ENTITY descriptor fields |
| `/device/*/name` | `object_name` + SET_NAME/GET_NAME |
| `/status/clock/gptp/*` | AVB_INTERFACE + GET_AVB_INFO + GET_AS_PATH |
| `/status/avb/*/listener/*` counters | GET_COUNTERS on STREAM_INPUT |
| `/status/avb/*/connected_talker/*` | ACMP state, GET_RX_STATE |
| `/status/clock/system/{source,sample_rate}` | CLOCK_DOMAIN, SET_SAMPLING_RATE |
| `/device/input/N/mode` | SIGNAL_SELECTOR descriptor |
| `/processing/matrix/*` | MATRIX + MATRIX_SIGNAL |

The listener counters map one-for-one onto the standard's STREAM_INPUT counter
block (`early_timestamp_count`, `late_timestamp_count`, `media_reset_count`,
`sequence_mismatch_count`, `unsupported_format_count`, `stream_reset_count`,
`timestamp_uncertain_count`). Meyer's namespace and the standard evidently
share a source.

### 5.2 Descriptors to add

All of these are currently **empty** in the shipping model, so the proxy
appends into empty index arrays with no collision against existing descriptors.

| Descriptor | Count | Purpose |
|---|---|---|
| CONTROL | ~800 | processing, device, status |
| CONTROL_BLOCK | ~36 | 8 input strips, 16 output strips, 12 link groups |
| MATRIX | 4 | gain, delay, delay_bypass, delay_type |
| MATRIX_SIGNAL | 48 | 32 sources + 16 outputs |
| SIGNAL_SELECTOR | 8 | `/device/input/N/mode` |
| LOCALE | 1 | "en-US" over the whole string table |
| STRINGS | 154 | every named descriptor's manufacturer name |

Every named descriptor also carries a `localized_description` into the
string table (the GALAXY's own convention, §3 Naming): the localized
string is the fixed manufacturer identity, `object_name` is the sticker
a user can later relabel via SET_NAME without losing it. The table
holds one string per named descriptor (1021 controls + 8 selectors +
4 matrices + 36 blocks + entity vendor/model + configuration + audio
unit + interface = 1074), packed 7 per STRINGS descriptor as
`(strings_index << 3) | offset`. Both types are listed in the
CONFIGURATION's descriptor_counts — more than the GALAXY itself does
(its STRINGS are reachable only through LOCALE `base_strings`, which
is why a counts-driven crawl of the device sees the LOCALE but not
the 16 STRINGS behind it).

### 5.3 Control mapping (abridged)

Standard control types that fit exactly: `MUTE`, `GAIN`, `INVERT`
(polarity_reversal), `DELAY`, `ENABLE` (bypasses), `FREQUENCY`,
`FILTER_RESPONSE`, `IDENTIFY`, `BRIGHTNESS`, `AUDIO_METERS`, `AUDIO_SCALE`,
`SRC_MODE` (AES ASRC), `FAN_STATUS`, `TEMPERATURE`, `ALTITUDE`,
`RELATIVE_HUMIDITY`, `LATENCY_COMPENSATION`.

The atmospheric block is the cleanest fit in the whole model: ALTITUDE,
RELATIVE_HUMIDITY and TEMPERATURE are standard types, and GALAXY already stores
temperature in Kelvin (`293.15`), which is what `UNIT_CODE_KELVIN` expects.

**EQ bands group into one CONTROL each**: `CONTROL_LINEAR_FLOAT`,
`number_of_values = 3`, per-value units HERTZ / OCTAVES / DB. AEM's linear
value block carries per-value min/max/step/default/units/string, so mixed units
work inside one descriptor. Rationale: one SET_CONTROL delivers a coherent
parameter set so the DSP recomputes coefficients once instead of passing
through intermediate filter states; it halves the descriptor count for the
dominant section; and the path adaptor still exposes
`/processing/input/1/eq/1/frequency` by mapping the path to
`(descriptor_index, value_index)`. `band_bypass` stays separate.

Constraint: `value_details` is capped at 404 bytes and a LINEAR_FLOAT value
block is 24 bytes, so a linear-float CONTROL holds at most 16 values.

**Meters are arrays**, not per-channel controls. One `AUDIO_METERS` control
with `number_of_values = 16` replaces sixteen descriptors and collapses
metering into one notification per update.

**The matrix needs four MATRIX descriptors.** A MATRIX carries one
`control_type` and one `control_value_type` across the whole grid, so gain,
delay, delay_bypass and delay_type cannot share one. All four reference the
same 48 MATRIX_SIGNAL descriptors, `width = 16`, `height = 32`.

Two things AEM cannot express, which the proxy must handle at command time:

- The 232-crosspoint ceiling (`/status/matrix_crosspoints_used`). AEM has no
  resource-budget concept; reject with NOT_SUPPORTED or BAD_ARGUMENTS and
  expose the used count as a read-only CONTROL.
- The default asymmetry: only matrix input 1 crosspoints 1-8 and input 2
  crosspoints 9-16 default to 0 dB, everything else to -90. MATRIX declares one
  default for the grid.

**Vendor control types needed** (allocate from the existing `00-1c-ab`
registry): U-Shaping band, U-Shaping band-5 gain, filter alignment type, delay
display type, atmospheric distance, link group assignment and bypass, isolate,
select, mute relay, display colour, front panel lockout, LED state, board
voltage, log message, matrix crosspoint delay type, crosspoints-used counter.

### 5.4 Not representable

- **Snapshots.** `/project/snapshot/0..255/*` plus create/update/delete/recall
  and the exclusion bitmask. `MEMORY_OBJECT_TYPE_SNAPSHOT_SETTINGS` (0x0005)
  exists but is an opaque blob, not queryable metadata. Expose recall as a
  `SNAPSHOT` control plus a read-only current-snapshot-id; leave management on
  the vendor channel.
- **Network configuration.** `/system/network/N/static/*`. No AEM equivalent.
- **Link group semantics.** AEM cannot declare that moving one channel's gain
  moves others. Tolerable live (unsolicited notifications show all affected
  controls change) but a real limitation for offline scene authoring.
- **Project metadata.** `/project/*`.

### 5.5 Descriptor budget

~1100 descriptors total for a GALAXY 816 with EQ bands grouped. At ~1100 serial
READ_DESCRIPTOR round trips, enumeration is user-visible, and a rack multiplies
it. This is the concrete case for the fingerprint-keyed model cache in §6.

---

## 6. Model store, symbols, and scenes

### 6.1 Model files

Persist models as a manifest plus the raw READ_DESCRIPTOR response bytes —
the `.aem` blob format already defined in `statusbar-avb`
(`docs/AEM_ENTITY_MODEL.md`). Vendor-neutral, defined by the standard,
byte-verifiable against a live device, and trivially fingerprinted.

The relational or handle-space representation is a derived index, rebuildable
at any time. SQLite is appropriate here — offline, disk-backed, read-mostly,
latency-insensitive, and genuinely useful for cross-model queries — but it must
never appear on the runtime value path.

### 6.2 Fingerprints

Hash the descriptor bytes in canonical order with per-unit volatile fields
zeroed first: `entity_id`, `entity_name`, `group_name`, `current_configuration`,
`association_id`, AVB_INTERFACE MAC address and link state, and
SIGNAL_SELECTOR `current_signal` (runtime state served back through
READ_DESCRIPTOR by entities that track it in descriptor state — live
venue input modes made two crawls of one unit fingerprint differently
until avb 2e894ce zeroed it). Miss any of those and every physical unit
fingerprints differently, collapsing the dedup scheme back into
per-device enumeration.

Compute the fingerprint over a normalized canonical form, not raw blob bytes,
so padding and authoring-tool quirks do not leak into identity. Note the
existing CONFIGURATION `descriptor_counts` trailer discrepancy in
`statusbar-avb`: a builder-produced model emits 74 bytes where a blob-backed
one emits `74 + 4N`, which would fingerprint the same logical model two ways.
Fix that before any project file is written.

### 6.3 Symbol derivation

Project files bind to symbols, never to `descriptor_index` and never to a raw
handle. Entity-side `.aem` files carry an authored symbol table; that table is
local and does not cross the wire, so a controller must derive its own from
what READ_DESCRIPTOR returns.

Rules:

1. **Never derive from a flat `descriptor_index`.** A device with 8 in / 8 out
   allocates AUDIO_CLUSTER 0-7 to inputs and 8-15 to outputs; going to 16 inputs
   shifts every output cluster.
2. **Walk the ownership tree top-down** using `base_x` / `number_of_x`, emitting
   `(owner role, local ordinal)` segments:
   `cfg0/au0/spin0/clus3`, `cfg0/au0/spin0/clus3/ctl:gain/0`.
3. **Discriminate CONTROLs by `control_type`**, ordinal within siblings of the
   same type, so adding a delay control does not renumber the gains.
4. **Never key on `object_name`** — mutable via SET_NAME, often user-set at the
   venue. `localized_description` *is* stable (part of the static model) and is
   acceptable corroboration.
5. **MATRIX/MIXER crosspoints** extend the path with h/w position.
6. **Unreachable descriptors** get `orphan/<type>/<index>` and are flagged, not
   dropped.

**Store the generated symbol table with the cached model.** Regenerate only for
unseen fingerprints. Version the generator and record that version in both the
stored table and the project file, so an old project resolves through the table
it was authored against. Otherwise any refinement to the derivation rule
silently invalidates every project file in the field.

### 6.4 Scenes and offline authoring

Three value layers:

- model defaults (static)
- scene / desired (persistent, authored offline — this is the document)
- reported (volatile, live only — never persisted; re-read on reconnect)

A virtual entity for offline editing is an `Instance` with a null adaptor.
Value validation offline uses the model's min/max/step/units, so the editor
clamps exactly as the device would — though devices also clamp in ways the
descriptor cannot express (interdependent controls, crossover ordering), so the
deploy path must collect per-control failures rather than aborting a recall.

Deployment resolves symbols against the live model and produces three sets:
**matched**, **missing** (in scene, not on device), **unmatched** (on device,
not in scene). Surface that as a report; it is a feature, not an error path.

Model migration between `entity_model_id` values is derived, not authored: auto-
match by symbol, present the residue for a human decision, and store the
resulting mapping keyed by *both* endpoint fingerprints in the model store.
Migration must be non-destructive and bidirectional — a touring fleet has mixed
firmware and one show file may need to target two model revisions at once, so
never rewrite the project in place; resolve through a mapping at deploy time.

---

## 7. `galaxy_proxy`

### 7.1 Identity: a companion entity, not a transparent proxy

A transparent proxy answering on the GALAXY's own `entity_id` does not work.
The GALAXY keeps sending its own ADPDUs and cannot be silenced without firmware
changes, so two sources would advertise one ID from two MACs. Filtering the
GALAXY's ADP by going inline with two NICs means inserting a non-AVB hop into a
path carrying gPTP and SRP.

Instead:

- The proxy advertises **its own `entity_id`**, derived deterministically from
  the GALAXY's `entity_id` or serial number. Never allocate at startup — every
  restart would otherwise invalidate every project file referencing it.
- **No TALKER or LISTENER capabilities.** No streams means no ACMP, no format
  compatibility, no stream-ID rewriting.
- Its model contains only AUDIO_UNIT, CONTROL_BLOCK, CONTROL, MATRIX and
  SIGNAL_SELECTOR descriptors.
- `association_id` links it to the real device. Set the proxy's to the GALAXY's
  entity ID until firmware can set a common value on both.
- **Version its `entity_model_id`** whenever the generation logic changes, or
  controllers will serve stale cached models.
- Stop advertising when the GALAXY becomes unreachable, so no phantom entity
  remains.

### 7.2 Mirror, do not forward

There is **no AECP-to-AECP path**. The proxy owns its whole model and serves
READ_DESCRIPTOR from local state; southbound it speaks the GALAXY ASCII
protocol (TCP 25003). The device's OSC ports are not used: the ASCII
protocol covers the whole namespace.

Forwarding AECP has a timeout problem with no clean solution: the AEM inflight
timeout is 250 ms and applies at both hops, so a forwarding proxy must answer
within 250 ms while itself waiting up to 250 ms, and the outer budget expires
first under load. It would also inherit per-direction sequence-ID rewriting,
two-level unsolicited registration, and ambiguous ACQUIRE/LOCK ownership.
Mirroring makes all of that disappear: the proxy is an entity implementation
whose backing store happens to be a socket.

### 7.3 Southbound behaviour

- A northbound SET_CONTROL writes `desired`, emits the ASCII set, and answers
  from `reported` once the GALAXY echoes.
- **Echo suppression compares against the mirror INCLUDING our own sent
  sets** (`GalaxyMirror::record_local_set`, the optimistic local echo).
  The device does not push every self-inflicted change back on the
  proxy's connection, so suppressing against device reports alone
  silently dropped the write that undoes a previous one — live incident
  2026-09-01: a web-UI mute went southbound, the device never reported
  it, and the unmute equaled the stale pre-mute report and was
  suppressed, leaving the widget unmuted and the venue input muted.
  With the local echo recorded, toggles always round-trip, a later real
  device report overwrites, and set-to-current (the safe live-test
  discipline) stays suppressed. Regression: the loopback fake's
  `echo_sets=false` mode.
- Subscribe southbound (`+/path`) for everything any northbound controller has
  registered for; unsubscribe when the last registration drops.
- Honour `/status/snapshot/recall_in_progress` — return IN_PROGRESS rather
  than timing out or guessing.
- Rate-limit metering per northbound subscriber. A 30 ms device subscription fanned
  out as AECP unsolicited notifications to several controllers is a lot of
  traffic; use `throttle_ms` / `refresh_ms`.
- Reject writes that would exceed the 232-crosspoint ceiling.

### 7.4 Configuration

The proxy needs, per device: GALAXY address (IPv4/IPv6/mDNS) and the model
variant; the transport is always TCP ASCII. mDNS names follow
`<entity>.<group>.local` or `mslg-gx-<type>-<serial>.local`.

---

## 8. Milestones

**M0 — skeleton.** Repo, CMake, packaging, CI, `find_package` against
`statusbar-core` / `statusbar-avb`. Empty modules that build and test clean.

**M1 — core.** `Model`, `Instance`, `Bitset`, `Selector`, `Subscription`,
`tick()`, seqlock reads. Port the two benchmarks as regression tests; assert
the bitset fan-out stays within an order of magnitude of the measured numbers.

**M2 — addressing.** Segment trie over interned symbols, pattern compilation to
`Selector`, `Adaptor<AddressT>`. Bench against the numbers in §4.7. Acceptance:
selection of 256 of 10k control points under 10 us.

**M3 — GALAXY schema.** Generate the control point model from the programming
guide namespace. Acceptance: the full 816 namespace resolves, and a
`?`-equivalent describe response round-trips as both Python and JavaScript
(every value quoted — that is what keeps the two literal grammars from
diverging; do not "improve" it into bare literals).

**M4 — ASCII adaptor.** Connect to a real GALAXY, subscribe, mirror state.
Acceptance: `controlplane_tools` dumps live values matching Compass.

**M5 — AEM binding.** Generate the descriptor set from §5, emit as an `.aem`
blob, validate with `statusbar-avb`'s `atdecc_aem_validate`. Acceptance: model
enumerates cleanly in a third-party controller.

**M6 — proxy.** Full entity: ADP advertisement, AECP AEM command handling,
unsolicited notifications, southbound mirroring. Acceptance: a controller can
set an input gain and see it move in Compass, and vice versa.

**M7 — offline.** Model store, symbol table, scene author/recall/diff against a
virtual entity with no device present.

**M8 — widget bridge.** `widget_bridge` (§12): an HTTP+WebSocket server on
`statusbar-core`'s `http` module that discovers 1722.1 entities with the
statusbar controller and projects their CONTROL, SIGNAL_SELECTOR, and
MATRIX descriptors as live widgets over a JSON WS subprotocol —
round-tripping sets from the browser, sweeping live values at connect,
and pushing unsolicited changes to every client. SHIPPED and
live-verified 2026-08-31 against the proxy (1033 widgets: labeled
sliders/toggles/selects, mode selectors, dB meter banks, four labeled
routing grids), the real Q1, and a third-party device on the segment;
the loopback acceptance test drives a fake controller service through
the same story with no network. Side effects: the proxy grew authored
mode-selector sources and bidirectional selector/matrix mirroring
(entity_model_id bank bumped to 0x…20/21/22), and statusbar-avb grew
the controller-side plumbing in §12.4.

---

## 9. Test strategy

- **Unit tests per module**, following `statusbar-avb` conventions.
- **Benchmarks as regression tests** — the fan-out and selection numbers in
  §4.5 and §4.7 are the design's justification and should not silently rot.
- **Golden model tests** against the two captured `.aemxml` files: parse,
  fingerprint, derive symbols, and assert stability. These are the only real
  device models available; treat them as fixtures.
- **Synthetic model generator** for scene-binding tests. Generate models of
  arbitrary shape, mutate them (add channels, add a control type, insert a
  stream port), and assert the symbol table survives additive change and that
  reconciliation classifies the residue correctly. This finds more binding bugs
  than a handful of real devices would.
- **Loopback proxy test**: run the proxy against a simulated GALAXY ASCII server
  built from the same schema, so CI does not need hardware.

---

## 10. Open decisions

1. Whether the 32 STREAM_PORT_INPUT clusters are the 32 matrix sources
   (8 device inputs + 24 matrix inputs). Verify against a live device.
2. Reconcile the guide's `input_channel_count` 64 with the 32-cluster stream
   port; the guide is model revision `05` and the units are `0a`.
3. Whether `band_bypass` folds into the band control as a fourth float value,
   saving ~240 descriptors at the cost of a boolean-as-float.
4. Vendor control-type allocation: obtain the existing Meyer `00-1c-ab`
   registry rather than guessing values.
5. Whether the proxy should also expose the GALAXY's snapshot recall, and how
   to represent the exclusion bitmask.
6. Whether `controlplane` should ship a SQLite virtual-table module exposing the
   live handle arrays for debugging and cross-entity queries (zero copy, no
   second source of truth) — attractive, but strictly optional and off the hot
   path.

---

## 11. References

- IEEE Std 1722.1 (ATDECC), descriptor and control type tables
- `statusbar-avb` — `docs/AEM_ENTITY_MODEL.md`, `statusbar/atdecc/`
- `ControlPlane` — <https://github.com/jdkoftinoff/ControlPlane>
- `if3-cpp` — `include/if3_tree.hpp`, and
  <https://www.jdkoftinoff.com/main/Articles/Fast_Tree/>
- Galileo GALAXY Programming Guide, PN 05.230.008.01 Rev B4, and the 2026
  web edition
- Captured models: `Q1-0x001cabfffe00c8f0.aemxml`,
  `Q2-0x001cabfffe007604.aemxml`

---

## 12. `widget_bridge` — web widgets for discovered entities

The M8 module (and the first consumer of `statusbar-core`'s `http`
module, whose HTTP_PLAN.md lists this as its own M6). A control-surface
web server: browsers connect over HTTP/WS, the bridge runs a statusbar
ATDECC controller on the AVB network, and every discovered entity's
CONTROL descriptors appear as live widgets.

### 12.1 Architecture

```
browser ── HTTP GET (manifest site) ──┐
browser ── WS "statusbar.widgets.v1" ─┤    WidgetBridge
                                      ├──  ├─ http::HttpServer   (Pollable)
                                      │    ├─ ControllerSimple   (Pollable)
                                      │    └─ WidgetRegistry
AVB network ── ADP/AECP ──────────────┘
```

- One `MessageReactor`, two Pollables. The tool's main loop is the ctl
  tool's pump: `reactor.poll_once(20)` + `bridge.pump(now_ns)`, which
  drains `ControllerSimple::drain_events()` and feeds the registry and
  the WS clients. Tests call `pump()` by hand with a fake service.
- `WidgetBridge` takes a `std::unique_ptr<ControllerService>` (so tests
  inject a fake) and an `HttpServer` configuration (bind, limits,
  optional static manifest for the web app).
- The registry is keyed by entity_id; per entity it holds the parsed
  ENTITY name and one `WidgetSpec` per CONTROL, SIGNAL_SELECTOR, and
  MATRIX descriptor — three separate index namespaces — identified
  both by `descriptor_index` and by the `derive_aem_symbols` symbol
  (stable across renumbering — the long-term binding key).

### 12.2 Crawl and projection

ControllerSimple's background crawl caches every descriptor the
CONFIGURATION advertises. The bridge reconciles the entity directory
each pump; an entity is *pending* until its ENTITY + CONFIGURATION +
every advertised CONTROL, SIGNAL_SELECTOR, and MATRIX descriptor is
cached (the CONFIGURATION's descriptor_counts table is the completion
criterion), then its widgets build:

- linear value types → `kind:"slider"` with min/max/step/default/
  current and `ControlUnits` code+multiplier — except single-value
  integer controls with a small exact state grid: exactly two
  reachable values (0/1, or the AEM 0/255 mute-and-identify
  convention, step == span) → `"toggle"` carrying its own on/off
  values in max/min; 3–16 grid steps (a mode, a slope, a delay type)
  → `"select"` over the generated grid;
- selector value types → `kind:"select"` with the option list;
- array value types → `kind:"meter"`: a read-only value bank with
  min/max/step and units (the GALAXY meter banks, −90..+20 dB);
- SIGNAL_SELECTOR descriptors → `kind:"select"` whose options index
  the authored source list, labels resolved from the source
  descriptors' names (fallback `TYPE[i]`), value = the current
  source's position; an unauthored selector (no sources) is shown
  read-only;
- MATRIX descriptors → `kind:"matrix"`: a width × height crosspoint
  grid, cell metadata (min/max/step/unit) parsed from the
  value_details trailer (a standard linear entry) by staging it
  through the linear projection;
- CONTROL_UTF8 → `kind:"text"` (read-only in v1);
- the standard IDENTIFY control type (`90:e0:f0::01`, the one Milan
  mandates per entity) → `kind:"identify"`: a momentary wink button —
  the site writes max, the device blinks its LED and self-resets per
  reset_time, and value pushes light the button while the wink is
  active; clicking a lit button writes min, because a device that
  stores the value verbatim (the proxy) never self-resets. Shape-typed
  by control_type, so it wins over the toggle heuristic;
- everything else → `kind:"opaque"`, listed but not settable;
- read_only bit → `"ro":true` (rendered, never sent).

Naming (Clause 7.3.7: the localized string is the manufacturer's
fixed identity, `object_name` the user-relabelable SET_NAME sticker):
the display name prefers `object_name` when non-blank, else the
localized string, else the symbol — and the localized name always
travels alongside as the widget's `"mfr"` field, so the site can
toggle between user and factory names. This covers controls,
selectors, matrices, CONTROL_BLOCK group names, and selector option
labels; the site's section hierarchy classifies on the stable factory
name so a rename never moves a widget. The STRINGS behind a LOCALE
are chased by ControllerSimple's crawl even when the entity doesn't
count them (avb 9077fc4), and entity build waits a bounded grace
period (10 s after everything else is cached) for them — building
anyway on expiry so a refusing device still appears.

Renaming: the client `rename` op maps to AEM SET_NAME (name_index 0)
on the widget's descriptor; an empty name asks the entity to clear
back to its authored `object_name` (the bridge remembers the authored
name, since the SET_NAME response echoes the command). The confirming
response — or another controller's unsolicited SET_NAME — re-resolves
the display name and fans the updated widget spec out. Entity-side,
DescriptorStorageHandler's opt-in `manage_object_names()` (avb) stores
RAM overrides for CONTROL / SIGNAL_SELECTOR / MATRIX / CONTROL_BLOCK,
with READ_DESCRIPTOR, GET_NAME, and unsolicited SET_NAME all agreeing;
the proxy and the offline service both opt in (names reset on
restart, like every other RAM SET).

A `WidgetLabelProvider` (widget_labels.hpp) decorates what AEM cannot
carry:

- `options_for(model_id, index, selector)` — a control it enumerates
  becomes a `"select"` regardless of grid size, with a `labels` array
  beside `options` (the GALAXY delay-integration type is a 162-entry
  enum a range heuristic would call a slider); for SIGNAL_SELECTORs
  the option values are source-list positions.
- `matrix_row_labels(model_id, matrix, rows)` — MATRIX_SIGNAL
  descriptors are nameless by the standard and the device's own
  describe names are generic, so row identity is schema knowledge.

`GalaxyWidgetLabels` recognizes the proxy's entity_model_ids and
answers from the folded schema: enum tables (verbatim device
"value:NAME" labels) via CONTROL/SIGNAL_SELECTOR index → AemBinding →
handle → enum_labels on a lazily regenerated model; INPUT_MODE_* for
the mode selectors (positions == modes by the authored-source
convention); and the 32 matrix rows as the 8 processed inputs then the
24 matrix inputs (PLAN §3), shared by all four crosspoint-parameter
matrices.

### 12.3 WS subprotocol (`statusbar.widgets.v1`)

One flat JSON object per text frame (widget_json.{hpp,cpp}: emitter +
strict flat scanner — scalar fields plus flat arrays whose elements
are all numbers or all strings; entity_ids travel as "0x…" hex strings
because binary64 cannot carry 64 significant bits).

Widget identity is (entity, `d`, control): `d` is absent for CONTROLs,
`"sel"` for SIGNAL_SELECTORs, `"mx"` for MATRIX descriptors.

Server → client: `hello` (version; `"scenes": true` when the offline
scene endpoints §12.8 are registered), `entity` (id, name, model),
`widget` (one spec — selects add `options`/`labels`, multi-value
sliders add `labels` parallel to `values` from the entries' localized
string refs — the Meyer Temperature control's NVME / CPU / Case — and
matrices add `width`/`height`/`rows`), `groups` (jumbo entities only —
see below), `entity_done`, `departed`, `value` (entity,
control, values[] — GET result, SET confirmation, or unsolicited;
matrix values are `at`-addressed contiguous runs patched into the
grid), `error` (echoed request context + reason), and — per
AVB_INTERFACE descriptor, sent right after `entity` and on replay —
`interface` (entity, interface index, name, `mac` as "xx:xx:…",
`clock_id` "0x…", `flags`, `priority1`, `priority2`, `clock_class`,
`clock_accuracy`, `log_variance`, `domain`, `log_sync`,
`log_announce`, `log_pdelay`, `port`: the authored gPTP port
identity, 1722.1 Clause 7.2.8) and `avb_info` (entity, interface,
`gm` "0x…", `prop_delay` ns, `domain`, `as_capable`, `gptp_enabled`,
`srp_enabled`, `avtp_down` when the entity marks it valid, and the
MSRP mappings as the parallel arrays `msrp_tc`/`msrp_pcp`/`msrp_vlan`:
the live GET_AVB_INFO answer, Clause 7.4.40, sent whenever it changes).
Everything the bridge shows about an interface is what the entity
says over 1722.1 — the bridge never inspects the host's own gPTP.

Client → server: `get`/`set` (entity, control, `d`, values[]; a
matrix set names one crosspoint with `at` = row·width+column; a
matrix get restarts the chunked grid refresh; a control get always
answers — with a `value` even when unchanged, or an `error`), `rename` (entity,
control, `d`, name — SET_NAME on the widget's object_name; "" clears
to the authored name; the confirmation arrives as a re-sent `widget`
spec), `refresh` (replay all entities to this client),
`open`/`close` (entity, group — jumbo entities only).

Jumbo entities: past `JUMBO_WIDGET_THRESHOLD` (512) widgets — the
nadia-CP proxy serves 55k+ CONTROLs — the bridge never dumps specs.
Discovery and replay send `groups` instead: `{op:"groups", entity,
groups:[{name, count}]}` in first-widget order, `name` being the
containing CONTROL_BLOCK's object_name ("" for block-less widgets).
A client `open`s the groups it is viewing — the bridge replays that
group's `widget` specs and current `value`s, then `entity_done` — and
`close`s them to stop the stream; spec, value and matrix-run pushes
for a jumbo entity's widget reach only clients with the widget's
group open. The `groups` payload is an array of objects (past the
flat scanner's grammar), so it is emitter-only, and the summary is
chunked — `seq`-numbered frames each under ~2 KB, the final one
`"last": true`, the client accumulating — because ws_send refuses a
payload larger than the per-connection tx buffer *permanently* (not
just while draining), which would wedge the outbox behind it;
flush_outboxes also drops any over-budget message outright for the
same reason. A rebuilt jumbo entity re-sends `groups` (a `seq` of 0
resets the client's accumulation) and invalidates every client's
open set — clients re-open on demand. The reference web app renders the summary
as collapsible groups and drops a group's widgets on collapse.

SETs are validated against the spec (range/read-only; selector sets
must name an authored source position, matrix cells are range-checked)
before encoding; the confirmed value broadcast comes from the device's
response, not the request — the device is the source of truth, exactly
like the proxy's southbound echo discipline.

### 12.4 Prerequisites added in statusbar-avb

The controller stack needed four additions (all found designing or
live-testing M8, all landed through the ci/ flow):

1. **Unsolicited delivery**: `NanoAvbAemController::receive_aecp`
   dropped any response with no matching in-flight sequence — which is
   every unsolicited notification. New `on_unsolicited` callback,
   plumbed through `ControllerServiceSink` and surfaced from
   `ControllerSimple` as `UnsolicitedEvent`; `register_unsolicited`
   exposed on `ControllerService`.
2. **Generic control actions**: `ControllerSimple::dispatch` gained
   `GetControl`/`SetControl` actions (raw value payload), alongside the
   existing typed actions.
3. **Matrix actions**: `GetMatrix`/`SetMatrix` — raw passthrough of
   the Clause 7.4.33/7.4.34 region payload after the descriptor
   header, for the bridge's chunked grid reads and cell writes.
4. **SIGNAL_SELECTOR trailer serving**: READ_DESCRIPTOR truncated the
   sources trailer (same class as the earlier MATRIX trailer bug);
   now served through `dispatch_fixed_with_raw_trailer` with the
   runtime current_signal intact in the head.

### 12.5 The served site

`site/index.html` (+ `manifest.toml`): a single-file vanilla-JS app —
no framework, no build step; a reference client, not a product UI.
Entities render as cards; each widget stacks three rows — name, the
full symbol path on its own monospace line (wrapping, never crowding
a long name), then the control. Widgets: sliders (value text rounded
to the control's step precision, with AEM unit suffixes — Kelvin
values display as Celsius), toggles carrying their own on/off values,
labeled selects, meter banks as live bars with a peak readout,
identify buttons (write max to wink; the button lights on value
pushes and clicking a lit one writes min, so a device that does not
self-reset can always be turned off), and matrices as full-width heat
grids with a row-label column, an output-number header, in→out
tooltips, and click-to-set cells. `set` goes out on input; `value`
broadcasts (own confirmations included) patch in place.

The header carries a light/dark theme toggle (dark default; every
color routes through the `:root` tokens, so the light palette is one
`data-theme="light"` override block plus `color-scheme` for native
controls; the choice persists per browser in localStorage), a
user/factory names toggle (user shows `object_name` stickers, factory
shows the localized manufacturer names; per-browser persistent) and,
in offline mode, the scene capture/recall buttons (§12.8). Each
widget's name row has a pencil: inline-edit the name and Enter sends
the `rename` op (Escape cancels, empty resets to the factory-authored
name); the confirmed spec patches the label in place on every
connected client.

Below the entity meta, each AVB_INTERFACE gets a panel from the
`interface`/`avb_info` ops: the authored gPTP port facts (MAC, clock
identity, priorities, clock quality, domain, log intervals, port
number) and a live row of chips — AS-capable, gPTP, SRP, AVTP link —
with the current grandmaster, propagation delay and MSRP class/PCP/VLAN
mappings, patched in place as `avb_info` pushes arrive.

### 12.6 Live state and consistency

Crawled descriptors carry *authored* values, not runtime state, so
the bridge treats the confirmed unsolicited registration as its
consistency point — everything after it arrives as a push:

- **Registration**: dispatched at entity build;
  REGISTER_UNSOLICITED_NOTIFICATION's completion triggers the loads
  below. (The build-time loads run too, but a freshly started entity
  may still be applying its own initial state — seen live against the
  proxy mid-dump — so the post-registration re-read is authoritative.)
- **Matrix grids**: a full 32×16 float grid exceeds one AECP payload,
  so grids load as row-sliced GET_MATRIX regions (~450-byte value
  budget per chunk), one region in flight per matrix; GET responses,
  SET confirmations, and unsolicited regions all decode through one
  Clause 7.4.33 region walker (direction/rep/item_offset).
- **Control sweep**: every decodable control (text/opaque skipped) is
  GET_CONTROLed, strictly one in flight per entity — the AEM window
  (8) is shared with the crawl, grid reads, and other entities, and a
  failed send's completion names no control index, so serializing is
  what makes every control retryable (bounded, 3) instead of silently
  skipped. ~60 s for the proxy's 1010 sweepable controls.
- **AVB info poll**: once registration confirms, every AVB_INTERFACE
  is GET_AVB_INFOed, then re-polled every `AVB_INFO_POLL_NS` (5 s);
  unsolicited GET_AVB_INFO responses are folded in as they arrive. An
  answer broadcasts `avb_info` only when it differs from what clients
  already hold (grandmaster, delay, domain, flags or mappings). The
  AVB_INTERFACE descriptor itself is re-read (`ReadDescriptor` action)
  at registration and after every unsolicited GET_AVB_INFO, because a
  proxy fills its gPTP port fields in from its device moments after it
  starts advertising — the crawl's copy can predate them (seen live:
  the bridge and the proxies restarted together); a changed descriptor
  re-sends `interface`.
- **Broadcast discipline**: sweep GET results and unsolicited pushes
  suppress values clients already hold; a SET's confirming echo always
  broadcasts — the setter deserves the answer, and the set-to-current
  live-test discipline (OPERATIONS) depends on it — and so does the
  GET result a client asked for by name (`WidgetSpec::refresh_pending`,
  set by the `get` op, consumed by the next GET_CONTROL result or
  error for that control): a refresh answered with silence is
  indistinguishable from one that was lost.

### 12.7 Offline mode

`OfflineControllerService` (offline_service.{hpp,cpp}) implements the
ControllerService seam with no network at all: the "discovered entity"
is an in-process AEM model served from a `.aem` blob through the same
DescriptorStorageHandler + AemEntityModel the proxy serves with, so
the entire bridge pipeline — crawl, projection, labels, the live-value
sweep, SET round trips, matrices, selectors — runs unchanged. SETs
persist in the handler's RAM state for the life of the process, every
connected browser sees every change, and command completions are
queued to tick() to preserve the seam's asynchronous contract. Blobs
whose authored entity_id is zero (generated models) get a synthesized
identity from their entity_model_id.

Three tool modes feed it: `--offline <blob.aem>` (any descriptor
image), `--offline-store <dir> <fingerprint>` (the M7 model store —
including a real device's captured crawl, browsable and editable on a
laptop), and `--offline-galaxy [model]` (the generated GALAXY model,
labels included). Capturing a real device into the store is
`statusbar-galaxy-model-verify --foreign <iface> <entity-id> <dir>`:
it crawls any entity over AECP and persists `<fingerprint>.aem` +
`<fingerprint>.symbols.tsv`.

### 12.8 Offline scenes

`OfflineGalaxyScenes` (offline_scenes.{hpp,cpp}) wires the M7 scene
machinery (§6.4) to the offline bridge's in-process state — the scene
author/capture/recall milestone, HTTP-shaped. Two routes register on
the bridge's own server in `--offline-galaxy` mode:

- `GET /scene/capture` — the current offline state as an M7 scene file
  (`# statusbar-scene v1`, `/path<TAB>value` lines) over every
  writable AEM-bound point: controls (read via GET_CONTROL and
  `decode_linear_elements` at the bound value index), matrix
  crosspoints (1×1 region GETs), and mode selectors (current source
  mapped back through `galaxy_input_mode_of`). Booleans serialize as
  0/1, not the AEM 0/255 wire form.
- `POST /scene/recall` — parse with `scene_from_text`, then apply each
  masked point through the model's own SET paths (read-modify-write
  GET_CONTROL→SET_CONTROL for multi-value controls, 1×1 SET_MATRIX
  regions, SET_SIGNAL_SELECTOR triples), so entity-side clamping and
  persistence behave exactly like a widget SET. The response is a
  plain-text report: `recall: matched N applied N read-only N failed
  N missing N`. After the writes, `WidgetBridge::refresh_entity_values`
  restarts the control sweep, selector GETs, and matrix grid loads, so
  every connected browser converges on the recalled state (unchanged
  values stay quiet).

The path↔handle mapping comes from the generated namespace and the
handle↔AEM tuple from the model's binding, so the component borrows
both plus the OfflineControllerService and the bridge; it attaches
only when the namespace generation matches the blob. A captured scene
is the same format `statusbar-galaxy-scene --recall` speaks, so a
scene authored offline on a laptop recalls onto the live device.
Because a full GX-816 scene is ~130 KiB, the tool raises
`limits.max_body` to 1 MiB and the capture response goes out via
`send_external` from per-slot handler storage (the inline tx buffer
only fits `max_response_head`).

The round trip is tested against the real generated model
(offline_scenes_test.cpp): capture → targeted recall (applied +
missing counters) → re-capture reads the value back → recalling the
full capture verbatim applies with zero failures.

The site surfaces both endpoints as header buttons: registering the
routes sets `"scenes": true` in the hello frame, and only then do
"capture scene" (downloads the scene file, timestamp-named) and
"recall scene" (file picker → POST, report shown in the header)
appear — a live bridge never shows them. Recalled values reach the
open page as ordinary value pushes from the post-recall refresh.

## 13. `nadia` — the Nadia CP (Meyer LCS Core) as a 1722.1 entity

The second device behind the same discipline as the GALAXY (§7): the
Nadia CP is a Meyer LCS Core running a mixer design, reachable as a
Milan entity (entity `0x000732fffe959361`, "RZ") whose AEM exposes
streams and clocking but not the mixer, and over a text-command TCP
port that exposes every control point the design declares. The proxy
mirrors the text side onto the controlplane core and (back half,
pending) publishes it as a companion ATDECC entity.

### 13.1 The Core text protocol

Port 28034 (alternate 28134) is a line-oriented command port; the
grammar comes from the Core "text commands" document. What the proxy
uses:

```
get [tag =] <addresses>          get input 1-8 level
set <addresses> = <value>        set output 5 level = -3.6
subscribe <addresses>            push "Got:" lines on change
unsubscribe <addresses> / unsubscribeall
```

Addresses are token lists with one-based indices, ranges (`1-8`) and
lists (`5,7,9-11`). Replies, as captured from a live Core:

```
Got (req-13): Input 1 Level = -3.6     untagged get: auto tag req-N
Got (foo): Input 1 Level = -3.6        tagged get
Got: Input 1 Mute = true               subscription push
Got (req-13): Input 1 Level =          address exists, design leaves it empty
Got (req-18):  = unset                 unknown address
```

Three spellings of one address are kept deliberately: the canonical
path `/bus/6/output/4/level` (the controlplane PathAdaptor address),
the command form `bus 6 output 4 level` (what `get`/`set` take) and
the friendly name `Bus 6 Output 4 Level` (what replies carry).
`nadia_name_key` (lower-case, whitespace and slashes removed) matches
replies to handles so the device's exact capitalisation of multi-word
tokens (`NoiseGate`, `EQ`, `VGroup`) does not matter.

### 13.2 Namespace from catalogue × shape

Unlike the GALAXY, a Core has no fixed namespace: the catalogue (364
address patterns) gives the shapes, the loaded design gives the
counts. `NadiaShape{inputs, buses, outputs, listens, vgroups,
eq_bands, system}` drives `build_nadia_namespace`, which expands the
v1 subset — the Input/Bus/Output/Listen/VGroup strips (level, trim,
mute, solo, solo/safe, isolate, name, address, effective/*, touched;
strips add invert, delay, delay/trim, meters, EQ bypass, parametric
bands, dynamics bypass/makeupgain and the noisegate / compressor /
limiter / expander sections), pre/post fader meters, the Input×Bus and
Bus×Output crosspoint matrices and the System master — into a
controlplane Model + PathAdaptor with a value kind per handle
(boolean, decibels, nanoseconds, hertz, ratio, integer, text) and one
ranged command per pattern (`input 1-16 parametric 1-8 level`) so a
full read or subscription stays one line per pattern. Cues, SpaceMap,
trajectories, WildTracks, VRAS, AGC, U-Shaping and the surface
bookkeeping are out of v1.

The shape is discovered from the ASCII port alone: `get input 1-64
address` and count the non-empty replies, per family; bands from
`get input 1 parametric 1-32 frequency`.

### 13.3 Mirror

`NadiaMirror` is `GalaxyMirror` over Core replies: the verbatim text
per handle, a scalar projection into the Instance (booleans 0/1,
numbers as themselves, `-inf` as the point's floor, a trailing unit
suffix tolerated), a `reported` bitset (answered at all) and a
`populated` bitset (answered with a value — what the design actually
declares), unknown-name collection for catalogue drift, and the
optimistic local echo for sets.

### 13.4 AEM model from the namespace (`nadia_proxy`)

`build_nadia_proxy_aem_model(NadiaNamespace)` generates the entity's
AEM straight from the shape, so there is no authored blob per design:
ENTITY / CONFIGURATION / AUDIO_UNIT / AVB_INTERFACE, a CONTROL_BLOCK
per channel strip ("Input 1" … "Vgroup 3") plus one for the system
points, and CONTROLs in catalogue order inside each block. The
`entity_model_id` is the JDKS bank `0x70B3D5EDC38xxxxx` with the low
23 bits an FNV-1a hash of the generator generation and the shape's
counts — a different design loads as a different model, so controllers
never reuse a cached one.

Value types follow the point kind: floats are CONTROL_LINEAR_FLOAT
(dB step 0.1, ratio step 0.01, hertz default 1000), booleans
CONTROL_LINEAR_UINT8 0/255 step 255, delays CONTROL_LINEAR_INT64 in
nanoseconds (unit SECONDS × 10⁻⁹), integers INT32, names and addresses
CONTROL_UTF8. Read-only points (address, meters, effective values,
status) are marked read-only in `control_value_type`. Vendor control
types in the `70:B3:D5:ED:C3:00:00:xx` bank name the semantics the
standard set lacks (solo, isolate, bypass, EQ band, dynamics section
parameters, system hold, faults, status). The three parametric-band
parameters share one three-value control; the crosspoints are
CONTROL_ARRAY rows per source strip ("Input 1 Bus Levels", "Bus 2
Output Delays"), chunked at 32 values ("… 33-64") so every descriptor
stays well under the 404-byte value-details cap — no MATRIX
descriptors, so the storage handler's `MAX_MATRIX_STATES = 4` is never
touched. Every point in the namespace binds to a (CONTROL, value
index) through `AemBinding`; the generator asserts the binding is
total.

A venue-sized design (96 inputs, 48 buses, 64 outputs, 8 listens, 16
vgroups, 8 bands) is 60,369 control points in 19,858 CONTROLs (4.3 MB
blob) and validates cleanly (`full_design_scales`).

### 13.5 The companion entity (`NadiaProxy`, `statusbar-nadia-cp-proxy`)

Same shape as the GALAXY proxy (§7.3) with one structural difference:
the storage handler's per-control RAM state (`MAX_CONTROL_STATES =
1024`) cannot hold a design, so a `ProxyHandler` subclass hooks
GET_CONTROL, SET_CONTROL and READ_DESCRIPTOR(CONTROL) for every
control but IDENTIFY and bypasses that state entirely:

- GET_CONTROL (and the CURRENT fields of a READ_DESCRIPTOR reply) are
  encoded from the `NadiaMirror` — the mirrored scalar, or the
  authored default when the device has not reported the point.
- SET_CONTROL is validated against the served format (size, range,
  read-only → NOT_SUPPORTED, newline in a name → BAD_ARGUMENTS),
  translated per value into `set <command address> = <value>` with
  echo suppression against the mirror (floats compared at single
  precision and formatted `%.7g`, so writing the current value back
  sends nothing), answered SUCCESS without storing, and the control
  is queued for fan-out. The device's own report is what changes the
  served value.
- Southbound reports feed the mirror on every `poll`; changed bound
  controls fan out as unsolicited SET_CONTROL notifications once the
  control plane is running.

Entity id `0x70B3D5EDC4xxxxxx` (bank 4, low 24 bits of the Nadia's
entity id) with `association_id` = the Nadia's. Startup: `--shape` or
a read-only `--discover` probe on a throwaway connection (counts the
per-channel `address` points; `--max` caps the range), refuse when the
design has no channels, the same live-entity-id preflight as the
GALAXY tool, `get` + `subscribe` for every catalogue pattern, then the
reactor loop with non-blocking southbound drain and 5 s reconnect.

Loopback test (`nadia_proxy_loopback_test`): a fake Core echoing
`Got: <friendly name> = <value>` verifies set translation for every
value kind, echo suppression, range rejection, read-only rejection,
name round-trip, report → GET for scalars / array rows / text, and
that reports never generate southbound sets.

Known caps in the avb storage handler that a full design could hit:
`MAX_OBJECT_NAMES = 1088` (SET_NAME stickers on channel-strip blocks —
allocated lazily, NO_RESOURCES beyond), and the descriptor TOC lookup
being a linear scan over ~23k entries per request (fine on the Pi so
far; a sorted TOC + binary search in avb if a full crawl proves slow).

### 13.6 Status

Front half (`nadia_ascii`, `nadia_schema`, `nadia_mirror`,
`statusbar-nadia-mirror`) and back half (`nadia_proxy`,
`statusbar-nadia-cp-proxy`) landed. Live validation is blocked: the
reference Core reported "Current control points array has 0 items in
it" on 2026-09-01 — no design loaded — so value formats (delay units,
`-inf` floors, band counts, name capitalisation) are catalogue-derived
until a design is up, and the proxy refuses to start against an empty
design by construction. Deployment beside the GALAXY proxy on the Pi
follows OPERATIONS.md once a design is loaded.
