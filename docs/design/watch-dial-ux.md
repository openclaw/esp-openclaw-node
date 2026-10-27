# Standalone watch and Dial UX

**Research and design proposal · revision 02 · 27 October 2026 · no firmware changes.**

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
    Assistants: Automatic or permitted agent → new/existing conversation
    Settings
      Connection: Wi-Fi, Gateway, pairing, Talk readiness
      Audio & privacy: output level, wake word / mic state [new controls]
      Display & text: layout density, text size, typeface, brightness, motion
      Recovery: change network, reconnect, forget Gateway, reset
    Help & device: identity, firmware, diagnostics, guided checks
```

Use a vertical text list, not a honeycomb of tiny icons. Home has prominent
**Talk** and clear **Menu** actions. A current task can replace the idle subtitle
with **View activity**; it must not remove navigation or End. Inside Menu, show
as many readable rows as the chosen density permits and scroll for more. Everyday tasks are one or two
selections away; settings may need one further level.

| Screen | Content and primary action | Failure / empty behavior |
| --- | --- | --- |
| Home | Lobster, short readiness, Talk, Menu | “Set up Wi-Fi” or “Fix connection”; Menu remains available |
| Talk | Connecting / In conversation / Ending, visible End | Specific setup/audio failure; return Home without phantom active state |
| Activity | Current request and confirmed result; open related card | “No activity yet”; no fabricated conversation history |
| Canvas | Latest supported content with reserved navigation | “No card yet”; unsupported content explains its limit |
| Actions | Named, authorized actions with consequence preview | Hide until a real configured action source exists |
| Assistants / conversations | Permitted agent and scoped conversation | Loading/offline/denied states; device integration remains new |
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
    Assignment: agent → authorized control → accepted configuration
    Messages: bounded readable message / result
    Activity: current request and latest outcome
    Settings
      Connection: Wi-Fi, Gateway, pairing
      Display & text: layout density, text size, typeface, brightness
      Feedback sound: local preference
      Recovery: network / Gateway repairs and reset
    Help & device: identity, firmware, local practice
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

Default to **Compact + Mono** in the revised concept. Density and text size are
separate, per-device preferences. Comfortable adds spacing and a larger base
font; Small/Default/Large adjusts reading size without changing navigation.

| Role | Watch compact / comfortable | Dial compact / comfortable |
| --- | --- | --- |
| Default body | 18 / 20 px | 15 / 17 px |
| Default title | 22 / 24 px | 17 / 19 px |
| Text-size adjustment | Small −2 px; Large +3 px | Small −2 px; Large +3 px |
| List interaction | 56 / 66 px minimum rows; vertical scroll | Up to three / two focused rows; fewer when needed |
| Primary action | Full-width hit region independent of glyph size | Physical press with labeled touch equivalent |

These are browser design values, not final raster metrics or physical
accessibility claims. Qualify fonts and hit areas on hardware. The previous
24–32 px body everywhere crowded the interface; its 76–88 px rows are no longer
the default. Apple separates text-size adjustment from other display settings;
borrow the control model, not point values as raw ESP pixels.
[Apple Watch display controls](https://support.apple.com/en-gb/guide/watch/apd127ec93ac/26/watchos/26).

Use mono for labels, status, values and short text, with a humanist option for
readability. Restrained `>`, `[x]`, `[online]`, thin rules and aligned values
provide structure without decorative terminal prompts. Keep the mobile lobster.
Preserve Unicode in incoming text and agent names: an ASCII-style shell must
not become an ASCII-only data contract. Long labels wrap or paginate instead
of losing their target, unit or important status.

The browser uses local system mono (Menlo/SF Mono where available); no Apple
font is embedded or redistributed. Current firmware uses Montserrat 16/20/24/28
on watch and 16/24 on Dial. LVGL supports generated bitmap fonts and ASCII-only
UNSCII; UNSCII is a diagnostic-style candidate, not a general message font.
Before firmware work, qualify a licensed mono family at bounded sizes with
explicit fallback for missing glyphs. Do not scale one tiny raster or load
arbitrary fonts during calls.
[LVGL font support](https://lvgl.io/docs/open/9.3/details/main-modules/font).

The explorer reviews hierarchy, wording and interaction. It is not a final
visual system, hardware performance result, or proof that browser CSS fits
firmware fonts. All example labels and values are synthetic.

## Revision 02: user walkthrough and agent selection

The review follows navigation, not just isolated screenshots. The explorer's
on-device Display settings and outside review shortcuts change the same
per-device preferences. Values last for this browser page session; this does
not claim firmware persistence.

| Pane family | Issue found | Revised behavior |
| --- | --- | --- |
| Home / Talk | Oversized text and hidden active-call context | Compact summary, named assistant, persistent Return/End strip |
| Menu / Back | Fixed Back destinations lost entry context and could clear calls | History-aware Back restores parent scroll/focus; Home stays separate |
| Assistant / conversation | Static default-session placeholder | Permitted assistant → new/existing conversation; explicit Automatic routing |
| Display | One oversized layout | Independent density, text size and typeface, with visible selected values |
| Audio / Help | Edits reset; audio checks opened volume | Retained simulation values; deliberate speaker/mic checks; explicit privacy state |
| Setup / repair | Repair cleared unrelated state | Distinct initial setup and Wi-Fi change; preserve identity and Dial assignment |
| Dial menu | No one-level Back; lost focus; hidden items | Numbered position, focus, Back and parent selection restoration |
| Dial practice | Claimed remote sends/results; changed a real draft | Separate local draft; no remote request or overwritten Activity |
| Dial assignment | No reachable agent/control flow | Agent → control → target/range → accepted configuration; reject/offline keeps current binding |
| Dial requests | Pending state tied to screen; results stole focus | Request survives navigation; Activity marker and frozen submitted context |
| Messages / large text | Fixed-height clipping | Pagination, page position and unchanged control value while reading |
| Forget / reset | Simulator language and unclear scope | Read consequences, then Cancel-first choices; invalidate Gateway-owned routing and permissions |

The service simulator stays outside the device. It supplies synthetic call,
connection, result and permission events; device buttons never manufacture
remote success. Walk through the device controls for ordinary use, then use
the screen index for exceptional and empty states.

### Prototype validation

Reviewed all 40 watch and 37 Dial panes, including setup, empty/error states,
preferences and destructive-action explanations. Browser geometry checks cover
all 77 panes across both densities, all three text sizes and both typefaces
(924 combinations), plus every Dial menu focus position. Long details paginate;
watch lists scroll with a visible cue. Checked four browser widths and found
no page overflow, JavaScript errors or external requests.

Interaction checks cover parent navigation, retained preferences, active-call
switch/cancel, pairing invalidation, denied creation, local practice isolation,
late results without page changes, pending versus prior results, and rejected
or offline assignments retaining the old control. These are prototype checks,
not firmware, physical font, audio or hardware qualification.

### Watch: assistant → conversation → call

Use **assistant** for who handles the work and **conversation** for its context.
Keep IDs/session keys out of ordinary labels. The current choice is visible on
Home and reachable from Menu. Automatic means Gateway routing; never silently
choose the first roster entry when explicit selection is required.

1. Load the permitted roster. Distinguish loading, empty, offline/cache, request
   failure and permission failure. Cached labels do not authorize a new choice.
2. Select an assistant, then New conversation or one of its existing threads.
   Do not mix other assistants' threads or infer capabilities from names.
3. Resume with the returned canonical key. Creating a thread is a write action;
   read/talk access alone does not grant it. Otherwise explain how to resume an
   existing thread or create one in OpenClaw.
4. Change the selected route only after success. Keep the prior choice on error.
   Retry the same creation intent with the same idempotency key; use a new one
   for a new creation intent.
5. Copy the route into each call. Changing assistant or conversation during a
   call stages the choice and offers Keep current call or End and switch.
   Apply the next route only after the old call closes. Existing audio, results,
   callbacks and Canvas actions retain their original owners.
   Re-pairing explicitly ends the old call and clears Gateway-owned selection
   and creation permissions before validating the new connection.
6. Show that original assistant/conversation on results, even after the next
   selected route changes. Do not imply provider readiness from roster presence.

The core audit used locally cached main
[`d777ae1`](https://github.com/openclaw/openclaw/tree/d777ae1239625a8cadb3da17e7a057d0c4de71fa),
not a claim about the serving Gateway revision:

| Operation | Existing seam | Missing device integration |
| --- | --- | --- |
| Roster | [`agents.list`](https://github.com/openclaw/openclaw/blob/d777ae1239625a8cadb3da17e7a057d0c4de71fa/src/gateway/server-methods/agents.ts#L118), filtered by current grants | Bounded picker, refresh and errors |
| Resume | [`sessions.list` agent filter](https://github.com/openclaw/openclaw/blob/d777ae1239625a8cadb3da17e7a057d0c4de71fa/packages/gateway-protocol/src/schema/sessions-list.ts#L18) | Pagination, titles and canonical-key retention |
| Create | [`sessions.create`](https://github.com/openclaw/openclaw/blob/d777ae1239625a8cadb3da17e7a057d0c4de71fa/src/gateway/session-create-service.ts#L343) | Write authority, idempotency and result handling |
| Voice | [Explicit Talk session key](../../components/esp-openclaw-talk/include/esp_openclaw_talk.h) | Room app currently uses automatic routing; picker wiring is new |
| Dial owner | [`dial.configure` invocation session](../../examples/m5stack-dial-node/main/app_main.c) | Authorized control catalog and assignment handshake |

The prototype includes a denied New conversation path; its external service
simulator can enable the hypothetical granted case. The device selector never
grants itself access. Creation must produce a distinct thread, not reset an
existing main conversation.

### Dial: agent + control assignment

The Dial selects an **agent for a control**, not a voice partner. Its assignment
identifies owner, target, unit and range. The two example controls are synthetic;
the catalog remains a new contract. Only accepted configuration replaces the
old binding. A menu choice alone cannot retarget a control or grant permissions.

During transport send, another press and reconfiguration are blocked. After
acceptance, completion remains pending, but a new press can supersede it and
reconfiguration can invalidate its old result token. Warn that previous work
may still run. No step promises external cancellation or rollback.

Hold Home, open Settings or change typography while waiting: the request keeps
its submitted agent/target/value. A result marks Activity without changing the
focused page or replacing a newer draft. Connection loss means Outcome unknown.
Home values say agent reported because they are not physical telemetry.

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
