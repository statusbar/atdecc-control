# atdecc-control

A control-point data model (`controlplane`) and companion IEEE 1722.1
(ATDECC) entities that put Meyer Sound DSP control surfaces on the AVB
control plane: the GALAXY proxy advertises a Galileo GALAXY's control
points as AEM descriptors and translates AECP commands into the device's
ASCII control protocol; the Nadia CP proxy does the same for a Meyer LCS Core
mixer design; the widget bridge serves every entity on the segment as a
browser control surface over WebSocket.

Docs:

- [docs/PLAN.md](docs/PLAN.md) — the design (M0–M8, all landed and
  live-verified against a production GALAXY 816)
- [docs/DATA.md](docs/DATA.md) — provenance of the folded schema data
  (captures, fold layers, LIVE_DRIFT, per-model meter banks, gates)
- [docs/OPERATIONS.md](docs/OPERATIONS.md) — building the deb,
  deploying to a Raspberry Pi on the AVB segment, restart discipline,
  live verification
- [docs/TOOLS.md](docs/TOOLS.md) — command-line tool reference

## Layout

| module | role |
| --- | --- |
| `statusbar/controlplane` | core data model incl. the AEM binding adaptor — no AVB dependency, ever |
| `statusbar/controlplane_devices` | GALAXY ASCII and Nadia text-protocol adaptors, folded device schemas, device mirrors |
| `statusbar/galaxy_proxy` | the GALAXY proxy entity + tool |
| `statusbar/nadia_proxy` | the Nadia CP proxy entity + tool |
| `statusbar/widget_bridge` | the web control surface (HTTP + WebSocket bridge to every entity) |
| `statusbar/controlplane_tools` | model dump, scene diff, symbol table dump |

## Building

Depends on `statusbar-core` and `statusbar-avb` (`statusbar-crypto` and
`statusbar-audio` arrive transitively via avb's package config). Build each
dependency standalone first (each repo has its own `local-build.sh`), then:

```sh
./local-build.sh
```

The script resolves sibling dependency build dirs automatically — this repo
is designed to sit beside the statusbar umbrella checkout's packages, so
`../core/build`, `../crypto/build`, `../audio/build` and `../avb/build` are
the expected locations. Override any of them with `STATUSBAR_<DEP>_DIR` or an
explicit `-Dstatusbar-<dep>_DIR=...`. Dropped into the umbrella checkout as
`atdecc-control/`, it is also discovered and built by the umbrella's
aggregate build and container package builds.

## Security

The proxies and the widget bridge carry no authentication: anyone who can
reach the bridge's HTTP/WebSocket port, or send AECP on the segment, can
change device state. Run them on an isolated control network and reach the
bridge through an ssh tunnel, as OPERATIONS.md describes.

## Trademarks and affiliation

Meyer Sound, Galileo, GALAXY, Nadia, LCS and Compass are trademarks of
Meyer Sound Laboratories, Inc. This project is independent work: it is not
affiliated with, endorsed by or supported by Meyer Sound. The GALAXY
protocol handling follows Meyer's published programming guide; the Nadia
text-command handling was derived from observing a live Core. The device
captures under `captures/` were recorded from a venue's GALAXY 816 with its
owner's permission.

## License

MIT — see [LICENSE](LICENSE).
