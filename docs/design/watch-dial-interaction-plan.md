# Watch and Dial: interaction plan

**Revision 04 · shared node experience · design proposal · no firmware changes**

Start with the [annotated design board](watch-dial-ux-plan.html). The
[revision 02 explorer](watch-dial-ux.html) remains a reference, not the target
design. The [research and source audit](watch-dial-ux.md) supplies the hardware,
Gateway, pairing, and authorization constraints retained here.

Build one complete OpenClaw node experience on both devices. Agents,
conversations, device control, activity and settings belong on both. A watch
must not become a voice-only companion, and a Dial must not become only a
single-purpose knob. Each operates independently.

Keep the menu names, icons, ordering, selected context and action semantics
consistent. Adapt layout to the screen and input, and advertise only real
hardware capabilities. The Dial does not acquire a microphone or speech
speaker through feature parity; it still selects agents, reads conversations
and invokes authorized controls. Speech and rich Canvas rendering use the
available hardware without changing the common product structure.

## What went wrong

Making all text smaller did not create hierarchy. The previous revision mixed
large buttons with small reading text, repeated status/navigation, and gave
each capability its own menu destination. More screens made the experience
look more complete without making the common journeys simpler.

Measured at native size in the revision 02 browser:

| Surface | Current allocation | Design consequence |
| --- | --- | --- |
| Watch Home, 448 px tall | 33 px header, 37 px route strip, 119 px fixed actions; 180 px central body | The lobster, status and actual work compete in the remaining center |
| Watch Menu | 44 px header plus 37 px route; 288 px content for a 322 px list | Even the basic menu needs scrolling; the assistant strip appears where it is rarely useful |
| Dial | Every screen gets a 116 px central body and a 38 px instruction footer | Readers, numeric controls and menus all inherit the same cramped shape |
| Both | Mono labels, paragraphs, secondary text and repeated instructions | Words wrap earlier; metadata competes with the task |

These are layout measurements, not usability-test results. The screenshots
also show underused space around the Dial's center and too many outlined
controls on the watch. The next revision should be judged by task clarity and
use of space, not by how many states fit in a screenshot gallery.

## Shared product, focused views

Use four view families: **glance, choose, read, act**. A shared state model does
not require identical screen furniture. Give each family its own space budget.

- **Glance:** identify the current assistant/control, its useful state, and one
  next action. Personality belongs here; status diagnostics do not.
- **Choose:** clear selection and meaningful labels. Show secondary text only
  when it distinguishes items. A compact status bar remains separate from the
  selected agent and task.
- **Read:** give text and cards the largest area. Navigation stays outside the
  content. Actions appear only when the content has an actionable outcome.
- **Act:** make the target, value or consequence unmistakable. A connection
  acknowledgment must not look like completion.

Keep the mobile lobster as the shared Home identity, scaled to each layout,
and during a supported call. Use restrained mono
for short labels and numeric values, a proportional face for prose, and one
accent for selection. Avoid terminal punctuation around every state. Neither
a tiny desktop terminal nor a copied platform skin is the objective.

## One menu on both devices

Use the same five destinations, in the same order. Pair each with a familiar,
consistent outline icon and a visible text label; icons aid recognition and
never replace names. The Dial highlights one item and previews its neighbors;
the watch presents the list. Both expose every destination.

| Menu | Icon concept | Shared behavior |
| --- | --- | --- |
| Agents | Person / assistant | Choose the permitted agent, inspect availability, keep the current selection visible |
| Conversations | Overlapping chat bubbles | Select or resume a thread for that agent; create one only with granted authority |
| Devices | Device with controls | Browse authorized device targets, inspect state, control a target or this device |
| Activity | History / completed item | Read outcomes, incoming content and pending work with its original owner |
| Settings | Gear | Display, connection, capabilities, local audio where present, help and recovery |

Home shows the current agent and conversation, one useful recent task, and
Continue. Both devices can resume that context. The watch may additionally
surface Talk as a shortcut when voice is available. The Dial's encoder is an
input method for the same navigation and actions, not a reason to remove agent
selection or conversations. Help is within Settings on both devices.

Devices includes This device and authorized remote targets. This device opens
the same canonical local controls used by Settings, such as brightness; it
does not duplicate preference storage. Remote controls describe their target,
property, current value, allowed action and owner. A slider, toggle, choice or
explicit action must match a real capability. Never infer controls from plain
conversation text or a discovered device name.

Agent selection changes the context for new work, not ownership of an existing
control binding, result or call. Device control can be entered from either
form factor. Both use the same permission checks, submission/acceptance/result
states and reassignment confirmation.

## Compact system status bar

Use an Apple Watch / classic phone-style top strip on both devices: Wi-Fi
quality, OpenClaw link, and battery or external-power state. The Wi-Fi icon's
arcs represent signal strength; do not add cellular bars to boards with no
cellular modem. Wi-Fi association and Gateway readiness are distinct facts.

| Indicator | Fact and current source | Unknown or unavailable behavior |
| --- | --- | --- |
| Wi-Fi / signal | Association plus RSSI from the [Wi-Fi status owner](../../components/esp-openclaw-node-provisioning/esp_openclaw_node_wifi.c) | Crossed Wi-Fi when disconnected; neutral quality if RSSI is unavailable; no fabricated bars |
| OpenClaw link | Device's live Gateway/session state | Broken/neutral link when offline or reconnecting; Wi-Fi alone cannot turn it on |
| Battery | [Current device status](../../components/esp-openclaw-node-provisioning/esp_openclaw_node_common_device_node_cmd.c) returns null level and unknown state | Battery outline with a dash; never render unknown as empty, full or 0% |
| Charging / external power | A future board adapter's actual power-source fact | Show only when measured; USB attachment alone does not prove battery charging |

The design board uses synthetic status examples and defaults to unknown
battery. Its sample-battery switch illustrates the future measured state;
it does not establish hardware support. Real battery/power-source providers
are planned work on both boards, including absent-battery detection.

Keep positions stable and reserve space before rendering. Signal changes must
not reflow the title or current action. Detailed percentages and diagnostics
belong in Settings → Connection / Power; a small icon is sufficient normally.
A low battery or relevant connection failure gets text as well as an icon.
Use status text alternatives and a labeled Connection/Power page; do not rely
on color or tiny tap targets. On touch, the whole strip may open device status;
on the Dial, Settings provides the encoder-accessible equivalent.

## Watch layout and input

An activity opens its detail or card directly. Do not interpose a generic
“Summary ready” page that merely offers “Read card.” The source assistant and
conversation remain visible in the detail, not on every unrelated page.

Back returns to the actual parent and restores its scroll/focus. BOOT returns
Home and never ends a call. While a call is active, Home’s primary action becomes
Return to call; it cannot start a second call. A normal subpage has one title
row; during a call,
that row includes a small **Return to call** affordance. Ending the call is a
deliberate action in the call view. Avoid duplicating End in two places.

### Watch spatial templates

All coordinates are draft pixels on a 368 × 448 screen, measured from the top
left. The main horizontal bounds are x=16..352. Reserve a 16 px status strip at
y10–26, then the title row at y34–78. The bottom safe edge remains y432. These
are design
budgets to test on hardware, not claims about accessible physical size.

| Template | Header | Main region | Bottom region |
| --- | --- | --- | --- |
| Home / action | y34–78: 44 px | y90–364: 274 px | y376–432: one 56 px action |
| Menu / choose | y34–78: 44 px | y90–432: 342 px scroll region | No fixed footer |
| Reader / card | y34–78: 44 px | y90–432: 342 px; reserve action space only when needed | Optional 56 px content action |
| Call | y34–78: 44 px, assistant identity | y90–364: 274 px, lobster plus truthful connection state | y376–432: End |
| Setup / confirmation | y34–78: 44 px, step or consequence | y90–364: 274 px, explanation and target | y376–432: primary action; Back/Cancel in header |

Home's 274 px body contains a 44 px agent/conversation selector, an 88 px
lobster, a short availability label and one recent-task card with clear gaps.

When there is no recent work, omit that card and center the simpler composition.
Do not invent activity to fill space. Menu is a header action, separate from
Continue or an optional Talk shortcut.

Five 64 px icon-and-label rows fit in the 342 px browse region. Comfortable
spacing uses 68 px rows; larger text scrolls rather than hiding a destination.
Rows are at least 44 px high even in Compact. Prefer one-line names and one
short useful summary. Longer user-generated titles may use two lines; scroll
the list rather than reducing their text size or clipping the selected title.

### Agent and conversation selection on both devices

Daily use must not require choosing an agent again. Continue resumes the
last valid selected context; Talk uses that same context where available.
Home shows a human name and conversation title;
Automatic is a real, explicit Gateway-routing choice, not a guessed agent.

1. Open Agents on either device to change who handles new work, or use Home's
   current-agent shortcut. The list shows the current selection and availability.
2. Open Conversations to see that agent's recent threads and New conversation.
   The agent is still visible and changeable from this view.
3. Choose an allowed agent, then an existing thread or New. A Dial uses focus
   and press; a watch uses touch. Selection semantics are identical.
4. Commit the new route only after the required response succeeds. Keep the
   prior choice on failure. Creation retains one idempotency key per intent.
5. If a call is active, stage the choice. Show Keep current call and End and
   switch. The old call must close before the new context becomes active.

Do not automatically select the first roster entry or make a new conversation
by resetting an existing main thread. A listed assistant is not proof of voice
availability. Loading, empty history, no permitted assistants, creation denied
and voice unavailable need distinct copy and a useful next action.

When creation is not granted, explain the restriction next to New conversation
and offer existing threads or the OpenClaw setup path. Do not make the user
discover an unexplained disabled button. Settings must never grant authority.

## Dial layout and input

Home presents the same agent, conversation and recent-task context as the
watch. Turn opens the shared menu; press continues the current conversation.
The user reaches Devices to select a control. A configured control can be
pinned as a Home shortcut on either device; it must not replace access to the
common menu. Pinning/persistence needs a defined owner before implementation.

Inside a numeric control editor, turn changes only the draft and press applies.
Watch touch/slider controls submit through the same action contract. Both
show Draft, Sending, Waiting and an agent-reported result distinctly. A control
belongs to its configuring owner and target; changing the selected agent does
not silently retarget it.

Use a **focused list**: one clearly selected item, with the previous and next
items visible in lower contrast. Neighbor previews are not three equal
buttons. At boundaries, do not wrap to a different action unexpectedly. Long
selected labels may wrap to two lines and displace a neighbor.

Short press selects; turn moves; hold returns Home. Include a Back item for
encoder-only navigation. Touching a named item selects that item, not a
different currently focused item. Retain menu focus on return. Exiting a
control never sends it. A retained draft is marked on Home and can be resumed
or explicitly discarded from Devices; a reported value stays separate.

### Dial spatial templates

The screen is a circle of radius 120 px. Keep content inside radius 112 px,
leaving an 8 px edge margin. Available width changes with height:
2 × sqrt(112² − (y − 120)²). This is a clipping constraint, not permission to
fill every edge with text.

| Region | Draft bounds | Purpose |
| --- | --- | --- |
| Status strip | x78–162, y20–34 | Wi-Fi quality, Gateway link, battery/power |
| Title | x44–196, y43–63 | Short context; one line |
| Wide center | x32–208, y76–184 | Main content; all four corners inside safe circle |
| Bottom label | x64–176, y194–214 | One short state/action or page position |
| Optional arc | Radius 106–110 | Value progress or position only; never the sole state cue |

| Template | Composition | Space priority |
| --- | --- | --- |
| Value / editor | Target in the title; value near y94–154; unit/state near y162; short action at y194 | Value 46–56 px; state 14–16 px; an arc only for a known numeric range |
| Focused list | Title at y43; previous item near y76; selected item in y108–160; next near y166; position at y194 | Selected 22 px, neighbors 16 px; remove a neighbor before shrinking selection |
| Reader | Title at y43; variable-width text y76–186; position/action at y194 | Proportional 16–18 px text, word-aware pages, no menu chrome |
| Setup / consequence | Status and title; target/consequence in the center; explicit action below | Plain language, no numeric control or decorative arc |

The reader may exploit wider central lines, or use the conservative 176 px
column initially. Page based on final glyph metrics and preserve words and
Unicode graphemes. Do not squeeze a desktop paragraph between two permanent
instruction bars. A summary on the Dial can link to the full artifact in
OpenClaw, but ordinary short messages must still be readable on the device.

The normal footer is a single short cue such as Edit, Apply, Waiting or 2/4.
After initial teaching, remove “Turn: select / Press: select” prose from every
page. Input help remains discoverable through Help. Critical consequence and
outcome labels always remain visible.

### Assignment and result ownership

Assignment follows **assistant → authorized target → inspect → accept**. The
preview names the target, property, range and assistant. It is not successful
until the configuring owner accepts it. Offline, denied or rejected changes
keep the old binding. No fabricated control catalog ships with firmware.

A request retains its submitted target, value, assistant and connection owner
when the user navigates away. Transport acceptance means Waiting, not Done.
Matching completion updates Activity without stealing the screen or replacing
a newer draft. A pending current request must not open an older result.

Before transport acceptance, another submission or assignment is blocked.
After acceptance, replacing the binding can invalidate its result token; warn
that the previous action may still run. Connection loss produces an unknown
outcome with Check before retry, not a promise of rollback. Practice has its
own draft and cannot alter real-control state, requests or Activity.

## Typography, density and appearance

**Recommend Mixed as the default type system.** Use a proportional face for
reading and long names; use mono for values and selected short labels. Keep
mono sparse; do not offer an all-mono default as a substitute for hierarchy.
An ASCII-inspired
character can come from fine rules, alignment and restrained symbols; it does
not require ASCII-only data or a terminal prompt on every row.

| Role | Watch draft size | Dial draft size |
| --- | --- | --- |
| Screen title | 22–24 px, medium | 14–16 px context |
| Choice / primary action | 20 px | 22 px focused, 16 px neighbor |
| Reading text | 18–20 px, proportional | 16–18 px, proportional |
| Secondary / state | 14–16 px | 14 px; do not demote a failure to metadata |
| Prominent numeric value | 44–52 px, tabular | 46–56 px, tabular/mono |

**Layout: Compact / Comfortable** changes row padding, gaps and optional
secondary details. It does not change font sizes. **Text size** scales reading
and control labels independently; use scroll or fewer visible neighbors when
larger text needs space. Keep touch targets at least 44 draft pixels; device
testing still decides their physical adequacy. Selected values in Settings
must preview in place and persist only through an approved settings owner.

Limit color to lobster red, a quiet selection accent, and explicit warnings.
Do not encode offline, pending, failure and selection only by color. Avoid
cards around every row, permanently outlined buttons, tiny uppercase prose,
invented clocks/battery readings and repeated healthy connection prose. The
small status strip carries system facts consistently; show a failure banner
only when it affects the current task and put detail under Connection / Power.

## Journeys that define the product

| Journey | Happy path | Required interruption / recovery |
| --- | --- | --- |
| First use, either device | Identify device → protected phone/laptop setup or USB → Wi-Fi → Gateway approval → first useful action | Bad password, expired code and pending approval keep prior progress; do not claim ready from Wi-Fi alone |
| Daily conversation, both | Wake → see agent/context → Continue → read or act; Talk where supported → result in Activity | Home/Back preserves the call; failure explains connection/audio repair; microphone privacy is real capture state |
| Change who handles work | Agents → choose permitted agent → existing/new thread → Continue | No access keeps old context; active call requires an explicit switch; voice is offered only where available |
| Read and resume work | Home recent card or Activity → full content → source conversation | Empty/offline content is honest; card actions retain their original owner |
| Device control, both | Devices → target → edit draft → Apply → Waiting → result | Navigate while pending; retain new draft; unknown result requires checking target |
| Reassignment, both | Devices → assignment → agent → target → preview → accepted binding | Reject/offline retains old binding; warn about already submitted work |
| Preferences and repair | Settings → preview text/layout or repair the failed layer → return to prior task | Wi-Fi changes retain Gateway identity; re-pair clears old call, routing and grants; reset states exact consequences |

Teaching should happen through the first successful task. Show a short
contextual input cue the first time the Dial editor opens, not a setup
slideshow. Do not block an experienced user with repeated instructions.

## Build sequence and review gates

1. **Approve the interaction direction through the design board.** Compare
   Home, menu, agent/conversation selection, devices, numeric editor, activity
   and settings at native size. This is the immediate deliverable, not another firmware patch.
2. **Prototype these templates and the seven journeys.** Reuse state/ownership
   contracts from revision 02; replace its layout and menu composition. The
   old explorer stays labeled as a superseded study during this work.
3. **Implement one shared vertical slice on both devices.** Agents →
   Conversations → Devices → outcome → Back/Home, including offline and pending
   states. Keep shared menu/capability/action contracts with board-specific
   renderers. Add other destinations
   only when their underlying behavior works.
4. **Qualify device rendering and inputs.** Font raster/glyph coverage, round
   clipping, touch targets, held-input transitions, wake behavior and long
   text. Then revisit defaults with real camera/physical evidence.
5. **Expand settings, provisioning and assignment through their owners.** New
   persistence, provisioning, catalog or authorization contracts need their
   own explicit design; this plan does not implement them by implication.

### Acceptance rubric

- Within five seconds, can someone identify the current target/context and
  the primary action without reading a diagnostic label?
- Can both devices resume the current conversation directly and find the
  same agent and device controls without learning another menu? Do infrequent
  choices stay out of those paths?
- Does the entire root menu fit or clearly reveal what continues? Is the
  selected Dial item readable without interpreting its neighbors?
- Does every screen have a real parent or Home path? Does navigation preserve
  calls, ownership, drafts and focus according to the stated contract?
- Can large text read every important name, warning and consequence without
  shrinking the font or covering content with navigation?
- Can a person distinguish a draft, submitted request and confirmed outcome?
  Does a late result leave their current work alone?
- Are empty, denied, offline and destructive paths as deliberate as success?

These are proposed evaluation questions, not completed user tests. Measure
task success and wrong actions alongside speed; a fast accidental send fails.

## Evidence and open decisions

Apple's guidance supports brief focused interactions, shallow hierarchy,
legibility and independent use. The adaptations here use the hardware we
actually have; the watch gains no Crown, haptics or watchOS APIs.
[Designing for watchOS](https://developer.apple.com/design/human-interface-guidelines/designing-for-watchos),
[Typography](https://developer.apple.com/design/human-interface-guidelines/typography),
[Status bars](https://developer.apple.com/design/human-interface-guidelines/status-bars),
and [Menus](https://developer.apple.com/design/human-interface-guidelines/menus).
The Dial's shape and rotary/touch inputs are documented by
[M5Stack](https://docs.m5stack.com/en/core/Dial). These sources guide the design;
they do not validate the proposed pixel sizes on our devices.

Unresolved before firmware: licensed font/raster memory cost; long-label and
Unicode coverage; accepted persistent preference storage; the authorized
control catalog/assignment entry point; actual voice capability/error facts;
the first-use teaching flag; and battery/power providers for the shared status
strip. Clock, sleep/wake policy, haptics and linked-device behaviors remain
separate future scope. Product parity does not depend on linking the devices.

The requested Claude design critique is pending authentication. No Claude
critique or approval is claimed. The source and visual analysis above is
independent; add the second-model critique before treating it as reviewed.
