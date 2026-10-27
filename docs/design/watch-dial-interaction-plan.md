# Watch and Dial: interaction plan

**Revision 03 · design proposal · no firmware changes**

Start with the [annotated design board](watch-dial-ux-plan.html). The
[revision 02 explorer](watch-dial-ux.html) remains a reference, not the target
design. The [research and source audit](watch-dial-ux.md) supplies the hardware,
Gateway, pairing, and authorization constraints retained here.

Build two complete products around a few excellent interactions. The watch is
a conversational companion that also reads and resumes work. The Dial is a
physical controller with understandable assignments and outcomes. Neither
depends on the other.

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

## Direction: one purpose per view

Use four view families: **glance, choose, read, act**. A shared state model does
not require identical screen furniture. Give each family its own space budget.

- **Glance:** identify the current assistant/control, its useful state, and one
  next action. Personality belongs here; status diagnostics do not.
- **Choose:** clear selection and meaningful labels. Show secondary text only
  when it distinguishes items. No always-present assistant or connection bar.
- **Read:** give text and cards the largest area. Navigation stays outside the
  content. Actions appear only when the content has an actionable outcome.
- **Act:** make the target, value or consequence unmistakable. A connection
  acknowledgment must not look like completion.

Keep the mobile lobster on watch Home and during a call. Use restrained mono
for short labels and numeric values, a proportional face for prose, and one
accent for selection. Avoid terminal punctuation around every state. Neither
a tiny desktop terminal nor an Apple Watch imitation is the objective.

## Watch navigation

Home has one primary **Talk** action, a contextual conversation selector, one
recent-work card when available, and a 44 px Menu target in the header. Menu
opens a short list: **Conversations, Activity, Settings, Help**. Configured
actions may appear later when a real action source exists.

| Destination | Owns | Does not need another top-level entry |
| --- | --- | --- |
| Conversations | Current assistant, existing threads, new thread | A separate global Assistants menu |
| Activity | Recent outcomes, text, retained image/A2UI cards | Canvas as a technical category |
| Settings | Display, audio/privacy, connection and recovery | Diagnostics mixed into ordinary settings |
| Help | Device identity, input/audio checks, setup guidance | Repeating every setting |

An activity opens its detail or card directly. Do not interpose a generic
“Summary ready” page that merely offers “Read card.” The source assistant and
conversation remain visible in the detail, not on every unrelated page.

Back returns to the actual parent and restores its scroll/focus. BOOT returns
Home and never ends a call. While a call is active, Home’s primary action becomes
Return to call; it cannot start a second call. A normal subpage has one title row; during a call,
that row includes a small **Return to call** affordance. Ending the call is a
deliberate action in the call view. Avoid duplicating End in two places.

### Watch spatial templates

All coordinates are draft pixels on a 368 × 448 screen, measured from the top
left. The inner area is x=16..352, y=16..432: 336 × 416. These are design
budgets to test on hardware, not claims about accessible physical size.

| Template | Header | Main region | Bottom region |
| --- | --- | --- | --- |
| Home / action | y16–60: 44 px | y72–364: 292 px | y376–432: one 56 px action |
| Menu / choose | y16–60: 44 px | y72–432: 360 px scroll region | No fixed footer |
| Reader / card | y16–60: 44 px | y72–432: 360 px; reserve action space only when needed | Optional 56 px content action |
| Call | y16–60: 44 px, assistant identity | y72–364: 292 px, lobster plus truthful connection state | y376–432: End |
| Setup / confirmation | y16–60: 44 px, step or consequence | y72–364: 292 px, explanation and target | y376–432: primary action; Back/Cancel in header |

Home's 292 px body can contain a 48 px conversation selector, a 104 px lobster,
24 px availability text, an 80 px recent-work card and three 12 px gaps.
When there is no recent work, omit that card and center the simpler composition.
Do not invent activity to fill space. Menu is a header action, not a second
full-width primary button competing with Talk.

Four 72 px menu rows fit in the 360 px browse region with room for boundaries.
Rows are at least 44 px high even in Compact. Prefer one-line names and one
short useful summary. Longer user-generated titles may use two lines; scroll
the list rather than reducing their text size or clipping the selected title.

### Assistant and conversation selection

Daily use must not require choosing an assistant again. Talk starts with the
last valid selected context. Home shows a human name and conversation title;
Automatic is a real, explicit Gateway-routing choice, not a guessed agent.

1. Tap Home's conversation selector, or open Conversations from Menu.
2. The current assistant appears as one changeable row. Below it are that
   assistant's recent threads and a New conversation action.
3. Tap the assistant row only when changing who handles the work. Choose an
   allowed assistant, return to the thread view, then select a thread or New.
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

## Dial navigation

Home presents **target → value → state**. Turn opens the menu without editing.
Press enters the current control editor. In the editor, turn changes only the
draft; press submits. The headline state distinguishes Draft, Sending, Waiting
and an agent-reported result. Teach this transition once during setup and keep
it available in Help.

The rotary menu has **Control, Messages, Activity, Settings, Back**. Control
offers edit/resume, discard draft and assignment management. Settings owns
display, feedback sound, connectivity, device information and recovery. A
control belongs to an assistant and target; changing an assistant alone must
never retarget it.

Use a **focused list**: one clearly selected item, with the previous and next
items visible in lower contrast. Neighbor previews are not three equal
buttons. At boundaries, do not wrap to a different action unexpectedly. Long
selected labels may wrap to two lines and displace a neighbor.

Short press selects; turn moves; hold returns Home. Include a Back item for
encoder-only navigation. Touching a named item selects that item, not a
different currently focused item. Retain menu focus on return. Exiting a
control never sends it. A retained draft is marked on Home and can be resumed
or explicitly discarded from Control; a reported value stays separate.

### Dial spatial templates

The screen is a circle of radius 120 px. Keep content inside radius 112 px,
leaving an 8 px edge margin. Available width changes with height:
2 × sqrt(112² − (y − 120)²). This is a clipping constraint, not permission to
fill every edge with text.

| Region | Draft bounds | Purpose |
| --- | --- | --- |
| Top label | x64–176, y24–44 | Short context; one line |
| Wide center | x32–208, y56–184 | Main content; all four corners inside safe circle |
| Bottom label | x64–176, y194–214 | One short state/action or page position |
| Optional arc | Radius 106–110 | Value progress or position only; never the sole state cue |

| Template | Composition | Space priority |
| --- | --- | --- |
| Value / editor | Target at y52; value at y84–144; unit/state at y152; short action at y194 | Value 46–56 px; state 14–16 px; an arc only for a known numeric range |
| Focused list | Context at y24; previous item near y68; selected item in y96–144; next near y166; position at y194 | Selected 22 px, neighbors 16 px; remove a neighbor before shrinking selection |
| Reader | Context at y24; variable-width text y54–186; position/action at y194 | Proportional 16–18 px text, word-aware pages, no menu chrome |
| Setup / consequence | Short context; target/consequence in the center; explicit action below | Plain language, no numeric control or decorative arc |

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
Mono as an optional interface preference with real reflow. An ASCII-inspired
character can come from fine rules, alignment and restrained symbols; it does
not require ASCII-only data or a terminal prompt on every row.

| Role | Watch draft size | Dial draft size |
| --- | --- | --- |
| Screen title | 22–24 px, medium | 14–16 px context |
| Choice / primary action | 20 px | 22 px focused, 16 px neighbor |
| Reading text | 18–20 px, proportional | 16–18 px, proportional |
| Secondary / state | 14–16 px | 14 px; do not demote a failure to metadata |
| Prominent numeric value | 32–40 px, tabular | 46–56 px, tabular/mono |

**Layout: Compact / Comfortable** changes row padding, gaps and optional
secondary details. It does not change font sizes. **Text size** scales reading
and control labels independently; use scroll or fewer visible neighbors when
larger text needs space. Keep touch targets at least 44 draft pixels; device
testing still decides their physical adequacy. Selected values in Settings
must preview in place and persist only through an approved settings owner.

Limit color to lobster red, a quiet selection accent, and explicit warnings.
Do not encode offline, pending, failure and selection only by color. Avoid
cards around every row, permanently outlined buttons, tiny uppercase prose,
fake clocks/batteries and repeated healthy connection labels. Show a compact
failure banner only when it affects the current task; put layer diagnostics
under Connection.

## Journeys that define the product

| Journey | Happy path | Required interruption / recovery |
| --- | --- | --- |
| First use, either device | Identify device → protected phone/laptop setup or USB → Wi-Fi → Gateway approval → first useful action | Bad password, expired code and pending approval keep prior progress; do not claim ready from Wi-Fi alone |
| Watch daily conversation | Wake → see context → Talk → use call → End → result in Activity | Home/Back preserves the call; failure explains connection/audio repair; microphone privacy is real capture state |
| Change who handles work | Context selector → assistant only if needed → existing/new thread → Talk | No access or no voice keeps old context; active call requires an explicit switch |
| Read and resume work | Home recent card or Activity → full content → source conversation | Empty/offline content is honest; card actions retain their original owner |
| Dial adjustment | Home → press Edit → turn draft → press Apply → Waiting → result | Navigate while pending; retain new draft; unknown result requires checking target |
| Dial reassignment | Control → assignment → assistant → target → preview → accepted binding | Reject/offline retains old binding; warn about already submitted work |
| Preferences and repair | Settings → preview text/layout or repair the failed layer → return to prior task | Wi-Fi changes retain Gateway identity; re-pair clears old call, routing and grants; reset states exact consequences |

Teaching should happen through the first successful task. Show a short
contextual input cue the first time the Dial editor opens, not a setup
slideshow. Do not block an experienced user with repeated instructions.

## Build sequence and review gates

1. **Approve the interaction direction through the design board.** Compare
   Home, menu, context selection, reader, numeric editor and confirmation at
   native size. This is the immediate deliverable, not another firmware patch.
2. **Prototype these templates and the seven journeys.** Reuse state/ownership
   contracts from revision 02; replace its layout and menu composition. The
   old explorer stays labeled as a superseded study during this work.
3. **Implement one watch and one Dial vertical slice.** Home → task → outcome
   → Back/Home, including offline and pending states. Add other destinations
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
- Can everyday Talk start in one tap, and can the Dial reach its editor in
  one press? Do infrequent choices stay out of those paths?
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
[Typography](https://developer.apple.com/design/human-interface-guidelines/typography).
The Dial's shape and rotary/touch inputs are documented by
[M5Stack](https://docs.m5stack.com/en/core/Dial). These sources guide the design;
they do not validate the proposed pixel sizes on our devices.

Unresolved before firmware: licensed font/raster memory cost; long-label and
Unicode coverage; accepted persistent preference storage; the authorized
control catalog/assignment entry point; actual voice capability/error facts;
and the first-use teaching flag. Clock, battery, sleep/wake policy, haptics and
multi-device behaviors remain separate future scope.

The requested Claude design critique is pending authentication. No Claude
critique or approval is claimed. The source and visual analysis above is
independent; add the second-model critique before treating it as reviewed.
