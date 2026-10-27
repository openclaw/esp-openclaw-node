# Standalone watch and Dial UX

**Research and design proposal · 27 October 2026 · no firmware changes.**

The watch and M5Dial are independent, Gateway-backed OpenClaw nodes. Each must
complete setup, explain its capabilities, offer useful everyday interactions,
and recover without the other device. A shared design language is useful;
a shared operating dependency is not. Joint-device experiences are deferred.

Open the [screen explorer](watch-dial-ux.html) for separate watch and Dial
journeys. Every screen is a proposed, simulated UI, including screens requiring
new services. The earlier voice-to-Dial concept is superseded as the product
direction. Working firmware at `7c9384358172ecf3011c3b6db6a614c8c71a557e` is unchanged.

## Product decision

Build **a watch companion and a physical control console**, not two versions
of a connected face. Keep the mobile app's red lobster and turquoise eyes on
the watch. Keep the Dial's restrained control presentation. Personality makes
idle moments pleasant; navigation, state, and outcomes do the actual work.

- **Watch:** start a conversation, read a response or Canvas card, resume the
  current task, reach configured actions when supported, and manage the device.
- **Dial:** find its assigned control, adjust deliberately, know what happened,
  read incoming messages, and manage the device. It does not speak or listen.
- **Both:** show their own identity, Wi-Fi, Gateway, authorization, and available
  features. Offer setup, connection repair, settings, help, and a way home.

“Full node” means a complete experience for the capabilities a device actually
has. It does not mean a miniature phone OS, arbitrary app store, or every
capability of the desktop Gateway. Neither device supplies offline AI inference.

## What the investigation found

### Existing implementation

| Surface | Working foundation | Missing product layer |
| --- | --- | --- |
| Shared setup | NVS-backed Wi-Fi credentials, USB console, setup-code exchange, saved-session reconnect | On-device wizard, phone setup, discovery, local repair/settings pages |
| Watch connections | Independent Wi-Fi, node and operator connections; separate availability facts | User-facing explanation of each layer and guided fixes |
| Watch activities | Talk, wake word, queued speech, animated lobster, image/A2UI Canvas, diagnostics | Launcher, conversation/session selection, readable activity/results, preferences |
| Dial activities | One numeric control, face, message, button events, brightness and buzzer | Clear unassigned state, menu, message reader, local preferences, assignment workflow |
| Lifecycle | Reconnect and explicit console provisioning/reboot | User-facing reset/forget flows, safe update experience |

The setup code is **not a short PIN**. It contains a Gateway address and a
short-lived bootstrap token. Current setup normally completes approvals, and
each device needs its own code. Manual authentication can leave device and node
approvals pending. A watch additionally needs its operator connection for Talk.
Do not show a generic “Paired” checkmark after merely joining Wi-Fi.
[Current pairing contract](../getting-started.md#pair-the-board-with-the-gateway).

The watch's current `SPEAKING` state begins on WebRTC connection. Until genuine
turn facts exist, the honest label is **In conversation**. M5Dial's transport
acknowledgement means **Request sent**, not successful execution. A matching
`dial.complete` supplies an agent-reported outcome, not physical verification
of an external actuator. These boundaries govern the entire design.
[Watch producer](../../components/esp-openclaw-room-node/esp_openclaw_room_node.c),
[Dial command owner](../../examples/m5stack-dial-node/main/app_main.c).

### Relevant hardware, without imaginary features

| | Watch | M5Dial |
| --- | --- | --- |
| Screen | 368×448, 1.8-inch AMOLED | 240×240, 1.28-inch round LCD |
| Primary input | Touch, BOOT button | Encoder, center press, touch |
| Audio | Microphone and speech speaker | Buzzer; no microphone or speech speaker |
| Memory | 16 MB flash, 8 MB PSRAM | 8 MB flash, no PSRAM |
| Current content | Image/A2UI Canvas, not a general browser | Text/control UI; no Canvas |
| Possible board features | PMU, RTC, IMU, battery connector | RTC, RFID, battery connector |
| Not yet a product feature | Battery telemetry, clock, motion wake, SD files, power policy | Battery telemetry, clock, RFID workflow, power policy |

Board capability is not firmware support or proof of the attached revision.
Waveshare's public pages describe different display/touch revisions; the
working BSP is the implementation reference. Do not add battery percentages,
time, health metrics, vibration, camera, or raise-to-wake badges without a real
provider and hardware qualification. Neither board is a relay device.
[Watch profile](../../examples/waveshare-esp32-s3-touch-amoled-1.8-room-node/README.md),
[Dial profile](../../examples/m5stack-dial-node/README.md),
[Waveshare board reference](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8),
[M5Dial hardware reference](https://docs.m5stack.com/en/core/Dial).

### Inspiration: take the interaction, check the implementation

| Reference | Evidence worth borrowing | What does not transfer |
| --- | --- | --- |
| Muse SDK | Named device during pairing; foreground confirmation; local Wi-Fi, connection, sound, sleep and recovery pages; current-value summaries | Its app/BLE onboarding is not present here. Different boards and inputs. No permission to reuse Jollybot art. |
| Grok Bot ESP32 bridge | Separate network/bridge/voice state, discoverable settings, bounded recording/cancellation, rollback after failed Wi-Fi change | Mac-dependent companion architecture; three pages are not a full node UX. Its interruption model does not fit this watch's audio path. |
| Grok Gadgets | Separate network setup, ownership claim, network reset, and ownership transfer | Different OLED product. Its cloud claim code is not OpenClaw's setup-code protocol. |
| Apple watchOS | Glanceable tasks, shallow vertical navigation, clear primary action, contextual teaching, visible rotary feedback | Apple assets, phone dependence, Crown hardware on the watch, haptics, health features, and points copied as pixels |

**Muse**, inspected at
[`729d0c8`](https://github.com/facebookincubator/muse-gadget-sdk/tree/729d0c8b3113c0cdbd7efb3b8aad8a313839a1bd):
the [pairing overlay](https://github.com/facebookincubator/muse-gadget-sdk/blob/729d0c8b3113c0cdbd7efb3b8aad8a313839a1bd/esp32/components/muse/muse_ui.c#L1363)
shows device identity and either a passkey or explicit confirmation. Pairing
consumes the confirming input; it must not accidentally start another action.
Its [settings implementation](https://github.com/facebookincubator/muse-gadget-sdk/blob/729d0c8b3113c0cdbd7efb3b8aad8a313839a1bd/esp32/components/muse/muse_settings_ui.c#L1419)
separates Wi-Fi from service connectivity and includes real recovery pages.
Its [compact menu](https://github.com/facebookincubator/muse-gadget-sdk/blob/729d0c8b3113c0cdbd7efb3b8aad8a313839a1bd/esp32/components/muse/muse_menu.c#L319)
is useful inspiration for focused selection. However, the
[Watcher wheel adapter](https://github.com/facebookincubator/muse-gadget-sdk/blob/729d0c8b3113c0cdbd7efb3b8aad8a313839a1bd/esp32/components/muse/boards/board_sensecap_watcher.c#L935)
collapses rotation into button pulses: it is not a rotary browser to copy.
The [license exception](https://github.com/facebookincubator/muse-gadget-sdk/blob/729d0c8b3113c0cdbd7efb3b8aad8a313839a1bd/README.md#license)
excludes Jollybot art. Retain our existing mobile-lobster provenance.

**Grok Bot**, inspected at
[`fcbd820`](https://github.com/vliuyt/grok-bot-esp32-bridge/tree/fcbd820761e7d3612d389dff6c3d459e233a099f):
its [UI](https://github.com/vliuyt/grok-bot-esp32-bridge/blob/fcbd820761e7d3612d389dff6c3d459e233a099f/firmware/pet/components/pet_ui/pet_ui.c#L37)
is home, settings, and connections. The
[input design note](https://github.com/vliuyt/grok-bot-esp32-bridge/blob/fcbd820761e7d3612d389dff6c3d459e233a099f/docs/VOICE_INPUT.md#L16)
records a failed small touch target and the move to BOOT push-to-talk. This is
evidence to test our hit areas, not a reason to silently change our button
behavior. Its [validation notes](https://github.com/vliuyt/grok-bot-esp32-bridge/blob/fcbd820761e7d3612d389dff6c3d459e233a099f/docs/VALIDATION.md#L11)
distinguish implementation from physical proof. Do the same here.

**Grok Gadgets** is a separate project, inspected at
[`0875b80`](https://github.com/adidshaft/grok-gadgets/tree/0875b8009379eb50980ddbc9054d95317af042d8).
Its [phone setup](https://github.com/adidshaft/grok-gadgets/blob/0875b8009379eb50980ddbc9054d95317af042d8/docs/getting-started/phone-setup.md),
[ownership claim](https://github.com/adidshaft/grok-gadgets/blob/0875b8009379eb50980ddbc9054d95317af042d8/docs/getting-started/claim-your-gadget.md),
and [transfer flow](https://github.com/adidshaft/grok-gadgets/blob/0875b8009379eb50980ddbc9054d95317af042d8/docs/getting-started/hand-a-gadget-to-someone.md)
support separating those intents instead of using one destructive reset.

**Apple:** adopt principles from
[Designing for watchOS](https://developer.apple.com/design/human-interface-guidelines/designing-for-watchos),
[Onboarding](https://developer.apple.com/design/human-interface-guidelines/onboarding),
[Digital Crown](https://developer.apple.com/design/human-interface-guidelines/digital-crown),
[Layout](https://developer.apple.com/design/human-interface-guidelines/layout), and
[Settings](https://developer.apple.com/design/human-interface-guidelines/settings).
The watch should use a short touch list; the Dial should use a focused rotary
list. Both should teach through one successful action rather than a slideshow.

The investigation used the pinned source above; no external project code was
executed. Implemented patterns and proposed adaptations are separated here.

## First use and pairing

### Recommended setup route

Recommend **temporary, protected Wi-Fi setup with a local browser**, shared by
both devices, as the first consumer setup transport. It works from a phone or
laptop without requiring an OpenClaw mobile-app release and avoids typing long
secrets on a watch or rotary keyboard. Keep USB as a complete alternative.

This is new firmware/client work, not an existing feature. Espressif provides
[SoftAP and BLE provisioning transports](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/api-reference/provisioning/wifi_provisioning.html);
a browser UI, Gateway bootstrap exchange and OpenClaw ownership flow still need
implementation. Do not assume an example transport supplies the whole product.

| Route | Decision | Tradeoff |
| --- | --- | --- |
| Protected SoftAP + local page | Recommended first consumer route | Temporary network switch; needs reconnection instructions, session protection and bounded memory |
| BLE + native mobile app | Later refinement if mobile owns device setup | Better phone continuity; requires app work, permission handling and a secure protocol |
| On-device keyboard | Optional watch recovery only | Long passwords/setup codes are unpleasant; rotary text entry is a poor default |
| USB console | Keep throughout | Dependable developer/recovery route; not consumer onboarding |

Setup mode must require physical entry, display the exact device name and
session-specific proof of possession, expire, and close after success or Cancel.
Use a unique protected setup network and a reviewed provisioning protocol;
do not expose an unauthenticated configuration server on the normal LAN.
Never put Wi-Fi passwords or Gateway bootstrap tokens into URL query strings,
browser history, analytics, screenshots, or ordinary logs. The setup-network
credential is not permission to control the Gateway. The transport/security
design gets its own review before implementation.

### Complete first-use journey, on either device

1. **Welcome:** identify “OpenClaw Watch” or “OpenClaw Dial” and its unique short
   suffix. Offer **Set up** and **Use USB**. Explain its purpose in one sentence.
2. **Choose setup route:** phone/laptop opens the temporary local setup page;
   the device shows the same identity and confirms the session. Supply manual
   network/address instructions as well as QR. Do not assume a QR-scanning camera
   exists on either device.
3. **Connect Wi-Fi:** choose a supported 2.4 GHz network or enter a hidden SSID;
   mask the password with a deliberate reveal control. Explain wrong password,
   network not found, unsupported enterprise/captive network, and lost setup
   connection separately. Initially support one saved network, matching today.
4. **Connect OpenClaw:** paste that device's fresh setup code into the local
   page. The code supplies the endpoint; discovery is optional future work.
   Show the intended Gateway and device identity before proceeding. Regenerate
   an expired code; never replay it as an automatic reconnect credential.
5. **Resolve authorization:** distinguish joining, connecting, waiting for
   approval, rejected credentials, and ready. Setup codes normally complete
   approval; show “Approve in OpenClaw” only for a real pending request.
   Never invent an approval PIN or grant server privileges from a local switch.
6. **Prove one useful action:** watch offers a short Talk check with separate
   microphone/speaker feedback and visible End. Dial offers a local turn/press
   exercise labeled **Practice — no action sent**, then an assigned control
   when available. Pairing succeeds even if an optional voice test fails;
   explain which capability remains unavailable.
7. **Finish at Home:** show actual readiness. Teach Menu/Back once next to their
   controls. Optional name, sound, motion and appearance changes can wait.

The setup UI may report “Network saved; reconnecting” while the phone leaves
the temporary network. The device display is the authoritative completion
surface if the browser loses contact. Reopening setup resumes from the last
valid state instead of overwriting working identity or erasing pairing.

When changing a configured device's network, warn that connectivity will be
interrupted. Keep previous credentials until the new network is proved;
rollback is a proposed lifecycle change, not a current setter guarantee.
Wi-Fi scans currently refuse while connected/reconnecting, so “Scan networks”
requires an explicit temporary disconnect flow.

## Watch information architecture

```text
Home: lobster, actual readiness, Talk, Menu
  Talk: current conversation, End, audio options
  Menu
    Activity: current task / recent session-only results
    Canvas: latest retained image or supported A2UI card
    Actions: only configured, authorized actions [later contract]
    Conversations: current/default session [chooser later]
    Settings
      Connection: Wi-Fi, Gateway, pairing, Talk readiness
      Audio & privacy: output level, wake word / mic state [new controls]
      Display: brightness, sleep preference, reduced motion [new preferences]
      Help & device: identity, firmware, diagnostics, guided checks
      Recovery: change network, reconnect, forget Gateway, reset
```

Use a vertical text list, not a honeycomb of tiny icons. Home has prominent
**Talk** and clear **Menu** actions. A current task can replace the idle subtitle
with **View activity**; it must not remove navigation or End. Inside Menu, show
three large rows at once and scroll for more. Everyday tasks are one or two
selections away; settings may need one further level.

| Screen | Content and primary action | Failure / empty behavior |
| --- | --- | --- |
| Home | Lobster, short readiness, Talk, Menu | “Set up Wi-Fi” or “Fix connection”; Menu remains available |
| Talk | Connecting / In conversation / Ending, visible End | Specific setup/audio failure; return Home without phantom active state |
| Activity | Current request and confirmed result; open related card | “No activity yet”; no fabricated conversation history |
| Canvas | Latest supported content with reserved navigation | “No card yet”; unsupported content explains its limit |
| Actions | Named, authorized actions with consequence preview | Hide until a real configured action source exists |
| Conversations | Current session and destination | Current/default only until list/create/select APIs exist |
| Connection | Wi-Fi, Gateway, pairing, Talk as separate rows | Each failed row opens its repair |
| Audio & privacy | Actual output level and input/wake-word state | No mic-off claim until capture and wake admission are disabled |
| Help & device | Identity, firmware version, diagnostics, speaker test | No fabricated temperature or battery measurements |
| Recovery | Targeted repair with consequence text | One-button erase is not the default fix |

### Watch interaction rules

- Tapping a dark screen only wakes it. It never starts Talk or activates a card.
- Tap a labeled action; vertical drag scrolls the current list. Back is always
  visible on subpages. Gestures are optional shortcuts.
- Proposed BOOT mapping: wake when dark; otherwise go Home. This deliberately
  changes today's view toggle and requires migration/physical testing. It does
  not silently end a call. During a call, Home has **Return to call** and **End**;
  **Menu** remains reachable.
- Keep the existing long-press Diagnostics shortcut during migration and add
  a visible Help entry. No new destructive hold gestures.
- Canvas owns touches inside its content. Reserve navigation space outside it;
  no global swipe-to-back over sliders/buttons or controls covering card text.
- A release cannot activate a different action after a state/page transition.
  Preserve pressed-action validation and consume wake/setup input.

Initial Talk uses **In conversation**. A later adapter may add Listening,
Working and Replying only from actual capture, agent and playback facts.
Silence is not “thinking”; connected transport is not “speaking.” During
playback the watch suppresses microphone PCM and a 300 ms tail; no voice
interruption. Keep End usable. Preserve the call-time animation budget: quiet
body, optional eye/mouth motion, no image decoding or large animated backgrounds
competing with audio.

## Dial information architecture

```text
Home: assigned control summary OR “Choose a control”, Menu
  Menu (rotary list)
    Control: one current assignment first
    Messages: bounded readable message / result
    Activity: current request and latest outcome
    Settings
      Connection: Wi-Fi, Gateway, pairing
      Display & sound: brightness, feedback preference
      Help & device: identity, firmware, input test
      Recovery: network / Gateway repairs and reset
  Future: multiple named controls after a real assignment/catalog contract
```

The first complete Dial release can have **one configured control**. It must
not pretend a generic 0–100 value controls anything. Until assigned, Home says
**No control assigned** and explains how to configure it from OpenClaw. An
optional practice dial is explicitly local and cannot send a real action.

A future **Choose control** flow needs a Gateway-owned source of available,
authorized controls and durable assignment. Do not invent a list from
conversation text or bind a stale agent session after reboot. Multiple controls
need defined selection, persistence, reauthorization and removal. Local
brightness remains useful regardless of remote assignment.

### Dial input model

| Context | Turn / drag | Short press / explicit tap | Hold |
| --- | --- | --- | --- |
| Home | Browse menu, current control first | Open current control; if unassigned, open setup/help | Home, harmless |
| Menu/list | Move highlighted row, one detent at a time | Enter highlighted destination | Home |
| Control editor | Change draft value; show “Editing” | Send exact displayed value | Home; retain draft, do not send |
| Message reader | Scroll/paginate text | Back or explicit message action, never implicit confirm | Home |
| Local preference | Adjust preview | Save/Done | Home; restore unsaved preview |
| Confirmation | Move selection, default Cancel | Activate selection on a fresh press | Cancel and Home |

Browsing and editing are distinct modes with distinct headings and hints. Do
not navigate away because the user turned while editing. Do not send because
they were selecting an item. The physical ring is primary; touch is an
equivalent option. Keep drag-versus-tap disambiguation and held-input invalidation
on view/configuration changes. Proposed Home/menu behavior replaces today's
turn-to-open-control shortcut; keep fast access with one Home press.

### Requests, messages and interruptions

Show **Editing 55 · 42 pending** when a new draft differs from an outstanding
request. Distinguish Sending, Request sent, agent-reported completion, failure
and connection loss. Another press after transport acceptance may supersede
the old request, matching the existing contract; no hidden completion lock.
Late/stale replies must not confirm the newer value.

No completion timeout currently exists. Proposed **Still waiting** wording after
a bounded delay does not prove failure or justify automatic resend. A timeout
or cancel protocol is separate work. On disconnect invalidate the request and
show **Outcome unknown** where an external action may already have run. Never
replay on reconnect.

The message command accepts 512 bytes but uses a fixed 90-pixel label. A real
reader needs scrolling or pagination, a title, and explicit Back. Wrap complete
Unicode text; essential meaning cannot depend on a marquee. The encoder scrolls
messages instead of adjusting the prior control. Incoming messages and remote
display commands must not steal focus mid-turn, mid-press, in setup, or during
confirmation. Show a bounded new-message marker and defer presentation; define
whether “latest only” replaces older content. No persistent inbox is needed yet.

## Shared settings, privacy and recovery

| Intent | Expected outcome | Current/new boundary |
| --- | --- | --- |
| See connection | Identify Wi-Fi vs Gateway vs authorization failure | Existing facts; new presentation |
| Reconnect Gateway | Explicitly reuse saved session | Existing reconnect; new local control |
| Change Wi-Fi | Test replacement, retain old network on failure | Existing setter; safe replacement lifecycle is new |
| Adjust display | Immediate preview, clear Save/Cancel or Done | Runtime brightness exists; persistence is new |
| Adjust sound | Watch output level; Dial feedback preference | New UI/persistence; Dial never gains speech |
| Disable microphone | Stop admitted capture and wake listening; visible state | New lifecycle control, not speaker mute or hidden icon |
| Reduce motion | Quiet lobster with readable states | New preference; keep call budget in every mode |
| Inspect device | Actual firmware, identity, diagnostics, capabilities | Existing foundation; omit placeholder telemetry |
| Forget Gateway | Remove reconnect credentials and return to pairing | New local lifecycle; remote revocation is separate |
| Forget Wi-Fi | Clear network, retain device identity/Gateway trust | Clear API exists; new deliberate UI |
| Factory reset | List erased credentials/preferences and identity policy | New destructive lifecycle; not arbitrary NVS erase |
| Check/update firmware | Version and supported USB instructions first | Factory-only partitions today; OTA is a separate design |

No local switch grants Gateway permissions. Optional watch action mode currently
requests broad `operator.admin`; never label it as a narrow “allow this device
to control lights” permission. A page can explain granted capabilities and
direct the owner to OpenClaw. Safer scoped actions require a separate contract.
[Current action mode](../../components/esp-openclaw-room-node/Kconfig).

Show concise task summaries by default; full transcripts, history and tokens
should not sit on an unattended screen. Future history needs explicit retention,
clear behavior and bounded storage. Hardware-test audio starts deliberately and
stops on exit. “Muted” identifies what is muted: speaker, buzzer, mic or wake word.

### Recovery matrix

| Observed condition | User sees | Next action |
| --- | --- | --- |
| No network | “Set up Wi-Fi” | Setup or USB guide |
| Bad password / missing AP | Specific network failure | Edit/choose network; retain Gateway identity |
| Gateway unreachable | “Wi-Fi connected · OpenClaw unavailable” | Reconnect, inspect endpoint, then deliberately change Gateway |
| Pairing pending | “Waiting for approval in OpenClaw” | Show device identity and actual pending state |
| Rejected/revoked credentials | “Pair again with OpenClaw” | Fresh code; no endless expired-bootstrap retries |
| Watch node ready, Talk unavailable | “Connected · Talk unavailable” | Explain operator/media reason; keep Canvas/Settings |
| Voice startup fails | Specific failure, no phantom call | Home; guided audio check |
| Lost remote result | “Outcome unknown” | Inspect target/Gateway before manual retry |
| No assigned control | “No control assigned” | Assignment instructions or local practice |
| Reboot/power loss | Restore valid credentials, truthful Home | Do not resume a Talk call, replay an action, or reuse setup confirmation; show actual wake-listening state |

Offline retains Home, Settings, Help, repair and local brightness/input checks.
Retained Canvas content needs a stale/offline label and disabled remote actions.
No new unsent voice queue or command replay. Timers, clock faces and battery
widgets may be useful later, but require real local services first.
Current startup enables microphone capture for wake-word processing; ending
Talk or rebooting is not equivalent to disabling the microphone.

## Visual and accessibility rules

Use black backgrounds, high-contrast text, restrained color and one clear
primary action. Red belongs to the lobster and deliberate warning accents,
not every button. Give settings and tasks room to breathe. Avoid tiny status
dashboards, decorative waveforms, dense icon grids and endless cheerful errors.

On the watch, start with 24–32 px body/action text and roughly 76–88 px primary
hit regions, then validate physically. These are hypotheses, not certification.
A nominal 1.8-inch, 368×448 panel is approximately 12.7 pixels/mm: the old 52 px
button was only about 4.1 mm high. Browser zoom hides that problem. Apple's
[accessibility guidance](https://developer.apple.com/design/human-interface-guidelines/accessibility)
uses points; it does not mean “44 raw pixels is enough.” Prefer full-width
primary controls and at most two short parallel actions, following its
[button guidance](https://developer.apple.com/design/human-interface-guidelines/buttons).

On Dial, keep essential text in the circle's safe center; start near 20–24 px
labels, a much larger value, and unmistakable row focus. The physical main path
reduces dependence on touch precision; touch still needs testing. Long labels
wrap or open details; do not truncate away target or unit. State uses text and
shape as well as color/sound. Text-size and motion options must work across the
whole screen set without clipping.

The explorer reviews hierarchy, wording and interaction. It is not a final
visual system, hardware performance result, or proof that browser CSS fits
firmware fonts. All example labels and values are synthetic.

## Implementation sequence and completion criteria

| Slice | Deliverable | Exit evidence |
| --- | --- | --- |
| 1. Independent shells | Home/Menu/Back, Connection, Help on both; honest Talk state; unassigned Dial | Each works with the other off; reachable destinations; wake/held-input/Canvas paths intact |
| 2. Readable daily use | Dial reader, draft/request/result distinctions; watch Canvas navigation/current activity | Long text, empty state, late result, interrupted editing and offline content |
| 3. Setup and repair | Protected phone/laptop setup, device confirmation, USB alternative, separate repairs | Fresh setup, expired code, bad password, pending approval, power loss, network rollback |
| 4. Local preferences | Display/audio/privacy controls with owned persistence/reset semantics | Actual hardware state, documented reboot behavior, no capture/command races |
| 5. Richer node workflows | Session choice, configured actions, multiple controls from authorized sources | Reviewed contracts; stale assignments/session changes cannot target the wrong destination |
| 6. Finish and qualification | Native typography, motion, accessibility, optional hardware-backed widgets | Physical input/audio, sustained calls, reconnect, bounded heap, camera evidence |

Each small PR should complete one usable vertical slice. Do not ship a menu full
of dead destinations. The map is the destination; early firmware exposes only
supported entries and retains USB until consumer provisioning is qualified.

New provisioning security, settings persistence/reset, action/session/catalog
contracts and OTA partitions need their own design review before implementation.
This document proposes those boundaries; it does not change them or assume
approval for unrelated storage or authorization behavior.

### Standalone acceptance journeys

- **Watch only:** Dial off; setup → approval → Talk → End → Canvas → Settings →
  network loss → repair → reboot. No Dial reference or dependency.
- **Dial only:** watch off; setup → assignment/help → turn → confirm → pending/
  result → long message → Settings → reconnect. No speech requirement.
- **Readiness:** network success cannot mask authorization failure; a working
  node connection cannot mask unavailable watch Talk.
- **Input:** wake, setup approval, drag and held press never submit an action
  after page/configuration change. Home and Back stay discoverable.
- **Trust:** stale results cannot confirm new values; reconnect never replays;
  unknown external outcomes are neither success nor guaranteed failure.
- **Recovery:** Wi-Fi repair retains Gateway identity; forgetting Gateway does
  not claim remote revocation; reset explains exactly what changes.
- **Performance:** bounded UI/content; no new audio dropouts, watchdog resets,
  credential leaks or inaccessible controls during real use.

## Implementation source map

| Owner | Source |
| --- | --- |
| Setup | [Provisioning](../../components/esp-openclaw-node-provisioning/README.md), [commands](../../components/esp-openclaw-node-provisioning/esp_openclaw_node_example_repl_cmd.c), [Wi-Fi lifecycle](../../components/esp-openclaw-node-provisioning/esp_openclaw_node_wifi.c) |
| Reconnect/status | [Sessions](../../components/esp-openclaw-node-provisioning/esp_openclaw_node_example_saved_session_reconnect.c), [status](../../components/esp-openclaw-node-provisioning/esp_openclaw_node_common_device_node_cmd.c) |
| Watch input/views | [Controller](../../components/esp-openclaw-room-node/room_ui_controller.c), [board input](../../examples/waveshare-esp32-s3-touch-amoled-1.8-room-node/main/main.c) |
| Watch sessions | [Talk config](../../components/esp-openclaw-talk/include/esp_openclaw_talk.h), [implementation](../../components/esp-openclaw-talk/src/esp_openclaw_talk.c) |
| Canvas | [Commands](../../components/esp-openclaw-room-node/room_canvas_node_cmd.c), [renderer](../../components/esp-openclaw-room-node/room_canvas_lvgl_renderer.c) |
| Diagnostics/art | [Diagnostics](../../components/esp-openclaw-room-node/room_diagnostics.c), [lobster](../../components/esp-openclaw-room-node/room_lobster.c), [notice](../../components/esp-openclaw-room-node/assets/NOTICE.md) |
| Dial | [App](../../examples/m5stack-dial-node/main/app_main.c), [input](../../examples/m5stack-dial-node/main/dial_input.c), [control](../../examples/m5stack-dial-node/main/dial_control.c), [actions](../../examples/m5stack-dial-node/main/dial_action.c) |
| Update limits | [Watch partitions](../../examples/waveshare-esp32-s3-touch-amoled-1.8-room-node/partitions.csv), [Dial partitions](../../examples/m5stack-dial-node/partitions.csv) |
