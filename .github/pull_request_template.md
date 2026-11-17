<!--
Write for someone who uses esp-openclaw-node but has not read the code. The title
and opening problem statement should make sense without the diff or linked issue.
Add a visible `Closes #<issue-number>` or `Related: #<issue-number>` line when useful.

Name the affected task and the symptom or outcome, not the implementation.
Illustrative titles (not claims about current behavior):
Good: fix: reconnect an ESP32 node after Wi-Fi returns
Bad: fix: reset the transport state flag
Good: docs: explain how to approve a board after manual pairing
Bad: docs: update the pairing section

For internal-only work, name the actual developer or operator workflow affected.
-->

## What Problem This Solves

<!--
Describe the concrete user, product, or operational problem.
In 1–3 short, plain-language sentences, explain what someone is trying to do,
what goes wrong or is missing, and why it matters. For a fix, include the trigger
when known. For other changes, describe the need without inventing a bug.

Illustrative problem statement:
Good: "After Wi-Fi returns, the board stays disconnected from the gateway, so
operators cannot send it commands until they reconnect it manually."
Bad: "Fixes an incorrect transport state transition."

Describe the experienced problem, not just the code change. Save function names,
missing checks, and the root-cause walkthrough for the explanation below.
-->

## User Impact

<!--
Lead with what users, operators, or developers can now do or expect, usually
in one sentence. For internal-only changes, say there is no user-visible change;
do not invent a benefit. Keep important risks, breaking changes, migrations,
and required user actions visible here.
-->

## Why This Change Was Made

<!--
Briefly explain how the change addresses the problem without repeating the impact.
Keep the body short. Leave file lists, internal acronyms, and root-cause walkthroughs
in the diff or optional <details>; include technical detail when it explains
behavior or a material tradeoff. Do not hide risks or required actions in <details>.
-->

## Evidence

<!--
Start with the behavior checked and the result in plain language. Reviewers will
inspect the code, tests, and CI; do not make them decode command lists to learn
what was verified. Use proof appropriate to the change: focused tests, firmware
builds, board observations, screenshots, redacted serial logs, or documentation
checks. Distinguish build success from behavior actually checked on hardware.
Identify the board and relevant firmware or SDK version when they affect the result.

Put exact commands, commit hashes, and long output in links or optional <details>.
Keep the useful evidence summary and meaningful gaps visible, including hardware
behavior not tested. Do not include Wi-Fi credentials, setup codes, or device tokens.
-->
