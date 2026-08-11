# Connection lifecycle

This document defines the connection, reconnection, and voluntary-disconnect
behavior implemented by this fork. It covers the headset transport and the PC
streaming session; an OpenXR application connecting to the PC runtime is a
separate lifecycle.

## State models

The server publishes these states:

| State | Meaning |
| --- | --- |
| `listening` | No accepted headset session; the server can accept a new connection. |
| `handshaking` | Crypto, pairing, transport, headset capabilities, and resume data are being validated. |
| `streaming` | The complete handshake is valid and the headset session can exchange media and tracking. |
| `reconnecting` | An unexpected link loss occurred and the active PC session is retained for up to 30 seconds. |
| `paused` | A voluntary pause or expired automatic retry window retained the PC OpenXR session until the paired headset resumes or WiVRn stops. |
| `stopping` | Ordered server shutdown is in progress. |
| `failed` | A terminal connection or resume error was classified and resources are being released. |

The client publishes `lobby`, `connecting`, `streaming`, `reconnecting`, and
`disconnecting`. A transition is emitted once; duplicate notifications do not
create duplicate events.

Close reasons are `user_requested`, `peer_closed`, `network_timeout`,
`transport_error`, `protocol_error`, `incompatible_resume`, and
`server_shutdown`. `none` is used when a transition has no close cause.

## Connected means handshake-complete

The public D-Bus property `HeadsetConnected` remains for compatibility, but it
becomes true only after crypto, pairing, transport setup, headset-info
validation, and Monado-session setup have completed. A TCP accept or partial
handshake is never reported as connected.

Each successful streaming connection receives a monotonically increasing
generation. Media, tracking, feedback, audio, heartbeats, and other
generation-bearing packets from an older connection are discarded. This stops
an old worker or delayed UDP datagram from writing into a resumed session.

## Voluntary disconnect

The wire protocol contains:

- `disconnect_request { mode, reason }`;
- `disconnect_ack { mode }`;
- `connection_state { state, reason, connection_generation, retry_remaining_ms }`.

`mode` can be `default`, `pause`, or `terminate`. `default` resolves through
the server's `disconnect-policy`, which defaults to `pause`.

In `pause` mode the server keeps the PC OpenXR application alive, invisible,
and unfocused. Video delivery, audio, telemetry, pacing, time synchronization,
and temporal frame history are paused or cleared. In `terminate` mode the
server closes the OpenXR session and releases its media and transport
resources. Pause, resume, and shutdown operations are idempotent.

The headset Disconnect button and `wivrnctl` wait briefly for the ACK before
releasing local resources:

```bash
wivrnctl disconnect --mode default
wivrnctl disconnect --mode pause
wivrnctl disconnect --mode terminate
```

## Unexpected link loss

An unexpected TCP/UDP loss follows this sequence:

```text
server: streaming -> reconnecting -> handshaking -> streaming
client: streaming -> reconnecting -> streaming
```

The server retains the active PC session for 30 seconds. The headset retries
after 0, 250 ms, 500 ms, 1 second, and then every 2 seconds until the deadline.
While reconnecting, the headset uses its local lobby/countdown and does not
display the last streamed world frame.

A resume is accepted only when all of the following match the retained
session:

- the paired headset public key;
- the active session token;
- codec and bit depth;
- stream resolution and headset features;
- FOV and supported refresh rates;
- required audio and transport capabilities.

The IP address may change. A different or incompatible headset is rejected
without taking ownership of the paused session.

On success, WiVRn recreates or clears the headset decoders and `AImageReader`
objects, recreates PipeWire streams, resets time synchronization, pacing and
temporal history, rejects stale-generation packets, and requests an IDR. Focus
and visibility are restored only after fresh stereo video is decoded.

On deadline expiry, automatic retries stop and the server transitions to
`paused reason=network_timeout`. The compositor, encoders, audio, tracking, and
OpenXR visibility remain paused, but the service keeps listening indefinitely.
Opening WiVRn later resumes the retained session; an explicit terminate request
or server shutdown still releases all resources. This also keeps `--no-fork`
usable after the headset application is closed or the headset is powered off.

After every successful handshake, the headset also stores the numeric address
and port as a manual fallback. If multicast DNS is blocked or its cache expires,
the computer remains selectable in the lobby without requiring another URI or
manual address entry. The fallback can be edited, deleted, or marked for
autoconnect like any other manual computer.

## Android OpenXR fallback

Meta Quest runtimes can continue returning `XR_ERROR_RUNTIME_FAILURE` from
`xrWaitFrame` after network transport and decoding have recovered. WiVRn
retries this error only when a reconnection is active; unrelated OpenXR errors
remain fatal.

If the network has been healthy again for two seconds but the local frame loop
is still stuck, WiVRn recreates the Android XR activity through a short-lived
helper process. The session token is stored in the application's private
`resume_token` file as 64 lowercase hexadecimal characters with mode `0600`,
so the clean activity can authenticate to the same retained PC session. The
helper terminates after launching the new activity.

This fallback can produce two server resume generations:

```text
generation 1: original stream
generation 2: network transport recovered, local Meta frame loop still failed
generation 3: clean Android XR activity resumed the same PC application
```

## Observability

Every server transition is logged with state, reason, generation, and time in
the previous state:

```text
Connection state=reconnecting reason=network_timeout generation=1 previous_duration_ms=30580
```

The `io.github.wivrn.Server` D-Bus interface exposes:

- `HeadsetConnected`;
- `ConnectionState` and `ConnectionReason`;
- `ConnectionGeneration`;
- `ConnectionAttempts`;
- `SuccessfulReconnections`;
- `ReconnectionExpirations`;
- `DiscardedPackets`;
- `SessionRunning`.

For example:

```bash
for property in HeadsetConnected ConnectionState ConnectionReason \
  ConnectionGeneration ConnectionAttempts SuccessfulReconnections \
  ReconnectionExpirations DiscardedPackets SessionRunning; do
  busctl --user get-property io.github.wivrn.Server \
    /io/github/wivrn/Server io.github.wivrn.Server "$property"
done
```

## Validation status

Verified on 2026-08-09 with a Quest 3S, Android NDK `29.0.14206865`, and the
matching build-tree server:

- debug and release Android builds completed;
- the deterministic lifecycle unit test passed, including duplicate events,
  concurrent close notification, both policies, and 100 pause/resume cycles;
- a 20,000-frame structural OpenXR harness run survived a voluntary
  disconnect/resume;
- a physical Wi-Fi cut resumed the same PC OpenXR application through the Meta
  fallback and reached generation 3;
- the final snapshot was `HeadsetConnected=true`, `streaming`, 3 attempts,
  2 successful reconnections, and 0 expirations;
- server shutdown released the native session and PipeWire streams cleanly.

Still required before a release claim:

- a two-hour physical session;
- twenty physical Wi-Fi cuts;
- twenty voluntary headset disconnects;
- the complete TCP/UDP fault matrix, including incompatible headsets and
  source-address changes;
- leak checks for native threads, locks, and persistent MediaCodec failures.

FSR spatial upscaling has no temporal state to restore. A reconnect creates a
fresh compositor and FSR resources, and the encoder starts again with an IDR;
see [fsr-spatial.md](fsr-spatial.md).
