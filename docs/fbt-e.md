# FBT-E: full-body tracking

This page uses **FBT-E** as a short name for the enhanced full-body-tracking
plumbing in this branch. It is not a new OpenXR extension or a camera/depth
body-pose estimator. The headset runtime remains the source of the provider
body poses; WiVRn transports them, applies lifecycle guards, and optionally
adds a conservative HMD/grip-anchored lying-pose layer for virtual trackers.

The status notes below describe the current checkout. They are deliberately
explicit because the runtime supervisor is implemented, while a few advanced
controls and the status presentation remain narrower than the wire contract.

## End-to-end architecture

```text
Meta/Quest or Pico OpenXR runtime
        │  XR_FB_body_tracking / XR_META_body_tracking_full_body
        │  or XR_BD_body_tracking
        ▼
Android client: provider selection, joint location, optional Meta controls
        │  meta_body / bd_body (stream)
        │  meta_body_skeleton and reset request (control)
        ▼
WiVRn protocol: settings + body_epoch + connection_generation
        ▼
PC server session: generation/epoch fence and body history
        │
        ├── Monado FB/META/BD body device
        └── optional virtual joint trackers (SolarXR/generic tracker clients)
        ▼
OpenXR applications and SteamVR compatibility layers
```

The headset advertises one provider in `headset_info_packet.body_tracking`.
The client locates joints at the tracking pattern's requested time, packs the
sample, and sends it on the stream channel. A Meta/FB skeleton is sent on the
control channel when `skeletonChangedCount` changes. The server converts the
sample into `xrt_body_joint_set`, stores history for time-based reads, and
publishes the body device through its Monado IPC system.

### Providers

| Provider | Client capability check | Wire packet | Monado output |
| --- | --- | --- | --- |
| Meta full body | `XR_FB_body_tracking`, `XR_META_body_tracking_full_body`, and `supportsFullBodyTracking` | `meta_body` with `meta_joints` | FB body input and Meta full-body input |
| Meta/FB body | `XR_FB_body_tracking` and `supportsBodyTracking` | `meta_body` with `fb_joints` | FB body input |
| Pico | `XR_BD_body_tracking` and `supportsBodyTracking` | `bd_body` | BD body input |
| HTC | HTC tracker extensions and discovered generic trackers | `htc_body` | generic tracker devices |

Meta full body is preferred when the runtime reports it; otherwise the FB
joint set is used. If no provider passes the capability checks, body tracking
is disabled. On Quest, the runtime's **Hand and body tracking** setting and
the Android body-tracking permission must still be enabled. If the runtime
chooses to infer a pose from head and controllers, that inference belongs to
the runtime, not to WiVRn.

## Settings contract

The persisted `body_tracking` object is part of `settings_changed` and has
fixed-width enum values so a newer endpoint can reject or ignore an unknown
choice safely. The technical default profile is `Balanced`, the movement
profile is `Current` (preserving the previous behavior), fidelity and
calibration are `Auto`, and the new supervisor is `Off`.

### Profiles

| Profile | Intended trade-off | Current branch status |
| --- | --- | --- |
| `Stable` | More jitter/dropout tolerance, accepting more lag | Active server policy: 0.60 confidence, 180 ms hold, 700 ms hard loss, 5 recovery samples, 6 m/s and 15 rad/s limits, alpha 0.22 |
| `Balanced` | General-purpose default | Active server defaults: 0.50 confidence, 100 ms hold, 500 ms hard loss, 3 recovery samples, 8 m/s and 20 rad/s limits, alpha 0.35 |
| `Responsive` | Less smoothing and lower latency, accepting more noise | Active server policy: 0.35 confidence, 60 ms hold, 350 ms hard loss, 2 recovery samples, 12 m/s and 30 rad/s limits, alpha 0.65 |
| `Custom` | Enables the advanced filter controls below | Active server mapping for confidence, hold, recovery, and smoothing alpha; see limitations below |

The Custom controls have these ranges and units:

| Setting | Range | Meaning |
| --- | --- | --- |
| Position smoothing | UI 0–100%; wire/persisted 0–1000 permille | Smooth position changes; more smoothing adds lag |
| Rotation smoothing | UI 0–100%; wire/persisted 0–1000 permille | Smooth orientation changes; more smoothing adds lag |
| Confidence threshold | UI 0–100%; wire/persisted 0–1000 permille | Reject samples below the threshold |
| Dropout hold | 0–2000 ms | Keep the last valid pose during a brief dropout |
| Recovery samples | 1–32 | Consecutive valid samples required for recovery |
| Prediction | 0–100 ms | Bounded positional prediction horizon; uses reported linear velocity or a Meta pose delta |
| Floor assist | on/off | Conservatively clamp valid foot joints that fall below the floor plane |

The client sends this aggregate in the initial headset information and in
subsequent `settings_changed` packets. The server rebuilds the per-joint
supervisors when the policy changes. Custom position and rotation smoothing
are currently combined into one smoothing alpha. With an active supervisor,
`prediction_ms` applies a bounded positional offset to an accepted pose using
its reported linear velocity, or a finite-difference velocity derived from
consecutive Meta poses. It does not predict orientation, acceleration, or an
invalid/missing pose. `floor_assist` only clamps a valid foot joint's output to
`y >= 0` in WiVRn's floor-relative world; it does not estimate a floor, lower a
raised foot, or fabricate a relation. Both options are bypassed when the
supervisor is `Off`.

### Lying pose assistance

`pose_assist` is an optional postural layer that runs after per-joint
supervision and before virtual trackers. It is intentionally separate from
the OpenXR body device: by default, `get_body_joints` continues to expose the
provider's body set unchanged, while enabled virtual trackers can use the
assisted output.

The layer keeps a short timestamped history of the headset and left/right grip
poses. Automatic mode classifies `Upright`, `Back`, `Front`, or a side pose
from head height relative to the selected floor/bed plane, torso direction,
and head orientation. Manual mode selects one of the same postures. A
hysteresis/transition timer prevents one frame from switching the whole body.
When lying, chest/hip/knee/foot tracker relations are blended toward a
profile-shaped template aligned to the HMD and a stable body axis derived from
the controller span, provider hip, and headset heading. The template preserves
torso/thigh/shin lengths, solves each leg independently, stacks rather than
crosses the legs in side poses, and gives each lower-leg tracker its own bone
orientation. Surface-height disagreements float the coherent template instead
of vertically stretching it. Provider offsets and rotations are capped around
that template before blending, so a malformed estimated leg cannot pull the
assisted trackers into an impossible pose.
Lower-body influence controls how much a provider leg can move the result;
anchor confidence controls the confidence assigned to synthesized relations.
Invalid provider legs therefore stop driving large jumps while HMD/controller
anchors remain usable. A missing HMD anchor disables synthesis for that sample
and the existing virtual-tracker dropout filter remains the final safety net.

The Tracking page in the APK exposes mode, manual posture, support surface,
bed height, transition time, influence/confidence sliders, and independent
Back/Front/Left-side/Right-side hip, knee, leg-spread, and foot-spread
profiles. **Realign lying pose** clears the server's pose state and anchor
history without changing provider calibration. Status telemetry reports
posture, confidence, anchor mask, and synthesized-joint fallback count.

#### Portable Unity pose source

`Pose source` selects either the original procedural templates or a validated
portable pose exported from Unity. The first integrated asset is
`Animation 25`, with UUID `unity.9b8a991000738a04d984a41268587e5a`, and is
authored for the `Back` posture. Selecting it does not run Unity or load a
VRChat avatar at runtime: the server consumes normalized anatomical targets,
then rebuilds the legs with the measured torso, thigh, shin, pelvis, and foot
lengths. The HMD and hand/controller history determine the coherent body frame;
provider lower-body relations are used only as the source of the entry/exit
blend. At full influence the custom solver is the sole writer for all six
virtual tracker relations, preventing competition with normal FBT estimation.

The bundled catalog is installed in `share/wivrn/poses`. A file with the same
UUID in `$XDG_DATA_HOME/wivrn/poses`, or
`~/.local/share/wivrn/poses` when `XDG_DATA_HOME` is unset, overrides it after a
server restart. Files are limited to 256 KiB and 64 catalog entries; schema,
units, category, finite bounds, normalized directions, and contact policy are
validated before use. The APK sends only the bounded catalog selection, never
a filesystem path or JSON payload. If an asset is absent, invalid, or not
authored for the active posture, the server falls back to the procedural solver.
Consequently, `Front` and side postures currently remain procedural even when
`Animation 25` is selected.

This first implementation does not replace the runtime body skeleton or run a
camera/depth neural estimator. The `body_and_trackers` wire value is reserved
for a future opt-in output mode; the current UI exposes the safe
`virtual_trackers` behavior only.

### Fidelity: Auto, Low, High

`Fidelity` maps to `XR_META_body_tracking_fidelity` on a Meta/FB tracker:

- `Low` requests `XR_BODY_TRACKING_FIDELITY_LOW_META`.
- `High` requests `XR_BODY_TRACKING_FIDELITY_HIGH_META`.
- `Auto` currently follows WiVRn's historical behavior and requests **High**;
  it is not a runtime benchmark or dynamic quality selector yet.

The request and the optional status structures are only used when
`XR_META_body_tracking_fidelity` is enabled. A runtime can advertise the
extension and still return `XR_ERROR_FEATURE_UNSUPPORTED` for the active body
device; WiVRn then disables that optional path and keeps tracking alive. Pico
and HTC providers have no equivalent Meta fidelity request. The effective
fidelity is carried in `meta_body` when Meta returns it; a missing optional
status is represented as `Auto`/unknown rather than fabricated.

### Calibration: Auto, Manual height

`Auto` leaves calibration to the headset. `Manual` uses the configured body
height (1000–2500 mm) through
`xrSuggestBodyTrackingCalibrationOverrideMETA`. The calibration extension and
its function are capability-gated in the same way as fidelity. A rejected or
unsupported optional call is logged and does not terminate the tracking
thread.

Meta calibration status is normalized to `unknown`, `valid`, `calibrating`, or
`invalid` and is included in `meta_body`. Pico and HTC do not provide this Meta
status. Manual height and fidelity are applied when a Meta/FB tracker is
created or recreated and can also be reapplied safely by the tracking thread
after an in-stream UI change. Switching from `Manual` back to `Auto` resets
the runtime calibration override instead of leaving the old height active.

### Supervisor mode and path

The visor labels the supervisor modes as:

- `Off`: bypass the FBT-E joint supervisor;
- `Auto`: enable the server supervisor with automatic policy selection;
- `Server`: enable the server supervisor explicitly.

`Supervisor path` is shown in the visor as `Supervised` and is encoded on the
wire as `Filtered` (publish the supervised relation, including adaptive
smoothing), or is shown and encoded as `Raw` (forward an accepted relation
without smoothing).
Both paths still validate, hold, reject, and recover samples. In this branch
`Auto` and `Server` currently select the same PC implementation; the separate
enum leaves room for endpoint-specific negotiation later. The existing
generic virtual trackers also have their independent 100 ms dropout filter,
after the supervised relation reaches them.

## Supervisor state machine and safe output

`server/driver/fbt_supervisor.*` contains the per-joint supervisor and unit
test. `wivrn_body_tracker` owns one instance for every FB, Meta, or Pico joint,
so one missing joint does not invalidate every other joint.

```text
warming_up ── valid sample ──▶ tracking
tracking ── invalid/dropout ─▶ holding ── hold timeout ─▶ lost
lost ── valid sample ─────────▶ recovering ── N valid ──▶ tracking
```

The default standalone policy is conservative: confidence at least 0.5,
100 ms hold, 500 ms hard loss, three valid samples for recovery, maximum
linear speed 8 m/s, maximum angular speed 20 rad/s, and quaternion norm
tolerance 0.1. It rejects missing required relation flags, contradictory
tracked/valid flags, non-finite values, invalid quaternions, invalid or low
confidence, and excessive velocity. Accepted quaternions are normalized;
optional smoothing uses the previous output and raises its response for
motion.

During a short dropout it can publish the cached relation with an explicit
`held` result. After hold expiry it publishes no valid relation. A hard loss
publishes no pose; recovery is gated so one plausible sample cannot immediately
reintroduce a bad joint. Equal or older packet timestamps are ignored.

This behavior is active before a relation enters `body_joints_list`. The
server then applies timestamped body history, and enabled virtual trackers add
their separate dropout filter. The standalone test still matters because it
covers edge cases without requiring an OpenXR runtime.

## Epochs, connection generations, and reset ACKs

FBT-E uses three independent monotonic identifiers:

| Identifier | Owner and purpose |
| --- | --- |
| `connection_generation` | Transport session generation. It changes on reconnect and fences every body packet from an older connection. |
| `body_epoch` | Body coordinate/calibration generation. It increments when the visor requests a body reset or origin changes. |
| `request_id` | Reset command identity. It lets the server replay an ACK without applying the same reset twice. |

The visor also starts a new body epoch when Meta calibration transitions back
to `valid`, so an automatic recalibration cannot interpolate against poses
from the previous body origin.

For `meta_body`, `meta_body_skeleton`, and `bd_body`, the server checks the
connection generation first and then the body epoch:

1. An old connection generation is discarded.
2. An older body epoch is discarded.
3. A newer body epoch clears body history and virtual tracker history before
   accepting the sample.

The server also clears body state when a reconnect creates a new generation.
`htc_body` carries the connection generation but has no FBT-E body epoch,
because HTC generic trackers do not implement the body reset contract.

The in-visor Tracking page defines three reset scopes:

- **Reset tracking** recreates a Meta/FB tracker where possible and starts a
  new epoch.
- **Reset calibration** calls `xrResetBodyTrackingCalibrationMETA` where the
  Meta calibration extension is available, reapplies the selected fidelity and
  manual height, and starts a new epoch.
- **Reset all** combines both operations.

Pico has no equivalent OpenXR reset call; its server history is still cleared
and the new epoch prevents old samples from resurfacing. HTC generic trackers
also clear server history, but `htc_body` has no `body_epoch`, so only
`connection_generation` fences transport data for that provider.

Each request carries `{request_id, reset, body_epoch, connection_generation}`.
The server accepts only the current generation and a current/new epoch. A
duplicate request ID in that generation returns the cached
`body_tracking_reset_ack` with the original result and does not reset twice.
The ACK includes `accepted`, the selected epoch, and the generation. The
client validates the ACK generation, advances its local epoch when accepted,
and shows a success or rejection toast. A reset request remains best effort if
the provider itself does not implement the requested optional Meta operation,
but the server ACK still reports whether its history reset was accepted.

## States, reasons, and telemetry

The wire enums intentionally use coarse, provider-neutral names:

- Supervisor states: `disabled`, `warming_up`, `tracking`, `holding`, `lost`,
  `recovering`.
- Reasons: `none`, `invalid_pose`, `reordered`, `low_confidence`,
  `excessive_velocity`, `excessive_angular_velocity`, `dropout`,
  `calibration`, `reset`, `unsupported`, and `internal_error`.
- Calibration states: `unknown`, `valid`, `calibrating`, and `invalid`.

The `to_headset::body_tracking_status` packet contains the selected
profile/fidelity, calibration and supervisor state/reason, `active`, accepted,
rejected, and held counters, plus body epoch and connection generation.
`fbt_supervisor_metrics` is more detailed: total/accepted/rejected samples,
reordered samples, holds and expirations, hard losses, recovery attempts and
successes, smoothing updates, resets, timeout polls, per-reason counters, and
the last input/result reason.

The server emits status immediately after policy changes and resets, and at
most once per second as body samples arrive. The client validates its
connection generation, stores the latest status for the stream, and renders a
compact summary in the visor statistics table: profile/state/reason,
calibration/fidelity, accepted/rejected/held counters, and body epoch. The
full per-reason metrics remain a server-side diagnostic surface rather than a
dedicated visor panel. When `supervisor=Off`, the relation path bypasses
supervision and status reports `disabled`.

## Current safe behavior and limitations

Implemented behavior that is safe to rely on:

- provider capability checks happen before creating an OpenXR body tracker;
- optional Meta calibration/fidelity structures and calls are removed after a
  runtime reports them unsupported;
- invalid or stale transport samples cannot overwrite a newer body epoch or
  connection generation;
- active supervisor validation prevents malformed relations, implausible
  velocities, and unconfirmed recovery from reaching Monado;
- Meta `calibrating` and `invalid` states immediately publish no body or
  virtual-tracker pose, even when the supervisor is `Off`;
- when Meta omits velocity-valid flags, the supervisor derives apparent linear
  and angular speeds from consecutive poses while tracking or holding, so a
  one-frame teleport is rejected; after hard loss, recovery may begin at a new
  origin;
- bounded positional prediction uses reported or Meta delta-derived linear
  velocity only, and floor assist clamps only valid feet below `y = 0`;
- server reset and reconnect paths clear body history and virtual trackers;
- lying-pose assistance is opt-in, uses HMD/grip anchors with transition
  hysteresis, and only changes virtual tracker relations;
- invalid or missing lower-body provider relations fall back to a bounded
  posture template instead of being extrapolated as free-running trackers;
- disabled virtual trackers remain present but return an untracked relation;
- body reads use timestamped history and relation flags rather than inventing a
  pose when no valid provider sample exists.

FBT-E does not currently:

- estimate a skeleton from RGB, depth, IMU, or a camera;
- run inverse kinematics or correct a provider's joint placement;
- guarantee that a runtime's inferred body pose is physically accurate;
- make Pico/HTC providers expose Meta calibration or fidelity status;
- expose the transported Meta calibration/fidelity controls through the PC
  Monado runtime to VR applications; these values are currently WiVRn
  client/server telemetry and visor controls;
- predict orientation or recover an invalid pose: `prediction_ms` only shifts
  the position of an accepted pose for the configured short horizon, and the
  first Meta sample has no prior delta from which to derive velocity;
- estimate a floor or force raised feet down: `floor_assist` only clamps valid
  foot joints below `y = 0` and never fabricates a missing pose; Custom
  smoothing is reduced to one combined alpha rather than separate
  position/rotation filters;
- provide a full body status dashboard (a compact summary is rendered in the
  streaming statistics table; reset ACK results are shown as toasts).

For the stable path, enable the headset provider and select only the virtual
trackers required by the application. `Balanced` with `Filtered` is the
general-purpose supervised default; choose `Off` only when comparing against
the unsupervised provider relation.

### Movement profiles and micro-motion filter

The movement profile is independent from the technical
`Stable/Balanced/Responsive/Custom` profile. `Current` is the default and
deliberately leaves the existing technical policy unchanged. `Auto` classifies
recent HMD, hand, and body-joint velocities with a 400 ms dwell before
switching policy; `Sprint` favors fast movement and quick recovery, `Chill`
favors rejection of brief noise and stronger smoothing, and `Flow` is tuned
for expressive hand or head motion without the sprint limits. Switching
profiles reconfigures the existing supervisors in place, so it does not erase
a valid pose or trigger a calibration reset.

The `Micro-motion filter` is a small positional dead zone for virtual
trackers. While the body is otherwise still, displacements below the configured
0-40 mm threshold are held at the last accepted position. The default is 6 mm;
`Sprint` and active sprint detection reduce the dead zone, while lying-pose
assistance bypasses it because that layer already emits a bounded trajectory.

## Tests

The branch includes these focused tests when `WIVRN_BUILD_TEST=ON`:

| Test | Coverage |
| --- | --- |
| `body-tracking-protocol-test` | Serialization of settings, reset request/ACK, status defaults, epochs, generations, and Meta effective metadata |
| `fbt-supervisor-test` | Validation, rejection reasons, hold expiry, hard loss, gated recovery, reordering, smoothing-safe output, Meta delta-velocity derivation, and reset |
| `virtual-tracker-dropout-filter-test` | Existing 100 ms grace period, sustained invalid publication, recovery, and reordered samples |
| `tracking-history-test` | Timestamped history/interpolation behavior used by Monado body reads |
| `body-pose-assist` | Automatic/manual posture classification, transition hysteresis, anchor state, synthesis fallback, and passthrough |
| `body-motion-classifier` | Movement classification hysteresis, fast/quiet profile transitions, and reconnect-gap handling |

For a configured build tree:

```bash
cmake --build build-server-no-system-boost -j4 \
  --target body-tracking-protocol-test fbt-supervisor-test \
  virtual-tracker-dropout-filter-test tracking-history-test body-pose-assist-test
ctest --test-dir build-server-no-system-boost --output-on-failure \
  -R 'body-tracking-protocol|fbt-supervisor|virtual-tracker-dropout-filter|tracking-history|body-pose-assist'
```

These tests prove the isolated contracts and serialization. They do not
replace a physical Meta/Pico acceptance run or exercise every provider/runtime
combination; the compact status summary does not expose every server-side
metric or provider-specific condition.

## Diagnostics

1. On Quest, enable **Hand and body tracking** in Movement Tracking, enable
   WiVRn's **Body tracking** permission, and verify the expected provider is
   selected in the client log.
2. Run the PC server in the foreground for detailed OpenXR/Monado messages:

   ```bash
   XRT_LOG=debug XRT_COMPOSITOR_LOG=debug wivrn-server --no-fork
   ```

3. Filter headset logs while starting a stream or pressing a reset control:

   ```bash
   adb logcat -v threadtime | \
     rg -i 'WiVRn|body|fbt|calibrat|fidelity|tracking|epoch|reset'
   ```

   Useful client messages include failure to create or locate joints, an
   unsupported optional Meta operation, and the tracking pattern containing
   `BODY`.

4. Filter server logs for the generation/epoch fence and reset result:

   ```bash
   journalctl --user -u wivrn -f | \
     rg -i 'body|epoch|reset|tracker|generation|discard'
   ```

   `Discarding stale body packet epoch=...`, `Body tracking epoch changed ...`,
   `Applied body tracking reset ...`, and `Rejected body tracking reset ...`
   distinguish stale data, a new origin, and an unsupported or old reset.
   `Virtual body tracker ... summary` is emitted when a generic tracker is
   destroyed and summarizes its active dropout filter.

5. If the body device is absent in an OpenXR application, confirm that the
   client advertised a non-`none` `body_type`, that the server and client
   share the same protocol revision/build, and that the application requests
   the corresponding FB, Meta, or BD body input. With a working provider, a
   `body_tracking_status` packet should arrive after body samples and after
   policy/reset changes. Its `active=false`, `lost`, or `calibration` reason
   indicates a provider/data issue rather than a missing status consumer.

For the existing virtual-tracker mask workflow, see the
[Virtual Body Trackers section in debugging.md](debugging.md#virtual-body-trackers).
