# nyq

named after Nyquist Theorem

A small daemon that monitors PipeWire audio sinks and MPRIS media players and
exposes their state over a Unix socket as newline-delimited JSON.

made for building status bars and other desktop widgets.

## Dependencies

- PipeWire
- libsystemd (sd-bus)
- cJSON
- pthread

## Building

```bash
make
```

## Usage

Start the daemon (keep it running in the background or as a systemd user service):

```bash
nyq daemon
```

Listen for events:

```bash
nyq listen
nyq listen --type sink
nyq listen --type player
nyq listen --type player --player spotify
```

### Sink commands

```bash
nyq sink-status
nyq sink-vol-up
nyq sink-vol-down
nyq sink-mute
nyq sink-next      # cycle to next sink
nyq sink-prev      # cycle to previous sink
```

### Player commands

`NAME` is an optional partial match against the player name (e.g. `spotify`, `brave`).

```bash
nyq player-status      [NAME]
nyq player-play-pause  [NAME]
nyq player-vol-up      [NAME]
nyq player-vol-down    [NAME]
nyq player-track-next  [NAME]
nyq player-track-prev  [NAME]
nyq player-cycle-next          # switch active player forward
nyq player-cycle-prev          # switch active player backward
```

## Event format

All events are JSON objects terminated by a newline.

**Sink:**

```json
{
  "type": "sink",
  "name": "alsa_output.pci-...",
  "level": 0.75,
  "muted": false,
  "icon": "audio-volume-high",
  "default": true
}
```

**Sink switch:**

```json
{ "type": "sink-switch", "name": "alsa_output.pci-..." }
```

**Player:**

```json
{
  "type": "player",
  "name": "spotify",
  "title": "Scatterbrain",
  "artist": "Emei",
  "status": "Playing",
  "volume": 1.0
}
```

`level` is perceptual (cubic root of linear), in the range `0.0–1.0`. `icon` is
a freedesktop icon name.

## Known issues

- Spotify exposes `Volume` via MPRIS but reports a low value instead of true mute.
  Muted audio shows `"level":0.0` rather than `"muted":true`.
- Brave/Chromium browsers expose one MPRIS interface for the whole browser.
  Multiple tabs map to the same player name, so per-tab title/artist is
  impossible.
