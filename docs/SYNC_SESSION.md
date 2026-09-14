# Sync session

A LAN review session: one peer **hosts**, the rest join as **spectators** and
follow along. The host's project is pushed to each joiner, so nobody has to open
anything first.

## Turning it on

Off by default — no socket is opened until you say so.

- In the **Sync Review** panel (icon strip, or **View ▸ Panels ▸ Sync Review**),
  tick **Enable network**. The rest of the panel is disabled until you do.
- The same switch is **Settings ▸ Advanced ▸ Enable Sync Review Socket** — one
  setting, two places, so they can never disagree.
- As a site default:

  ```ini
  [sync]
  enabled = true
  ```

## Hosting

Fill in a **username**, optionally change the **port**, and press **Create
Session**. The host then broadcasts a small UDP beacon carrying its hostname,
username and TCP port, so browsing spectators can find it.

The panel lists connected spectators by name and host. One host per machine and
port; a second local host fails to bind and says so.

## Joining

Either:

- pick a discovered host from the list — in the panel, or from the **SYNC
  SESSION** tab on the launcher — or
- type a host into the **Join** field. An IPv4 or IPv6 literal, or a resolvable
  name (DNS, mDNS `.local`, NetBIOS). Add `:port` only for a host that is not on
  the default port; beacons carry the port, so a browsed join never needs it.

On connect the host immediately streams its serialized project, and the
spectator loads it.

**Leave** returns to free viewing. If the host drops, spectators detect it and
fall back to free viewing on their own — there is no host election.

## What is synced

The host drives:

| | |
| --- | --- |
| Transport | play, pause, seek |
| Sequence | which sequence the view is scoped to |
| Frame view | zoom and the normalized image point held at the player centre, so the framing reproduces at any window size |
| Exposure | viewer exposure in stops, and gamma |
| Letterbox matte | ratio, opacity and Fit View — it travels with the view, because Fit View frames on the masked region |

Trying to scrub, play or change exposure as a spectator tells you the host owns
it.

**Annotations are two-way.** A spectator draws, its completed stroke goes to the
host, the host merges everything for that frame and broadcasts the combined set
back, and spectators replace their markup with it. A clear travels either
direction. See [draw tool](DRAW_TOOL.md).

**Colour is deliberately local.** The grade, the tech-check mode and OCIO are
not synced — each viewer keeps their own. Only the exposure/gamma the host owns
travels.

## Ports and protocol

| | |
| --- | --- |
| Discovery | UDP **45777**, broadcast beacon |
| Control | TCP **45778** by default, overridable per host |

Framing is `[u8 type][u32 payloadLen][payload]`, little-endian. Message kinds:
project bytes, play, pause, seek, sequence, stroke, clear, annotations, hello,
view, exposure, matte.
