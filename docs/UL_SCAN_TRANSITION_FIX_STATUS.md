# UL scan transition fix — 2026-09-08

Branch/worktree: adaptive-rx-UL-DL on sens6. Original worktree untouched.

## Root cause and change

CSS0 bootstrap cleared the configured dci01_scan bit. Dedicated-CORESET discovery
restored DL monitoring but never restored that bit, permanently suppressing the
automatic UL length/width/interpretation pipeline.

The bit now retains the validated operator intent. A single runtime gate permits
DCI 0_1 only in a dedicated UE-specific context with a valid UL BWP. CSS0 and common
contexts remain suppressed; explicit disable and malformed-config disable remain
disabled. The gate is shared by manual scanning and automatic UL discovery.

No CFO/SFO correction is enabled or changed by this fix.

## Verification

- Added a regression that drives real CSS0 configuration followed by actual
  DM-RS-based dedicated acquisition, repeated twice for enabled and disabled UL.
  On the old code both enabled cycles failed: configured scan 1 became 0.
- Fixed code passes the transition, CSS0 suppression, explicit disable, invalid
  BWP, invalid scan value, common-search-space and null-context checks.
- Rebuilt nr-uesoftmodem, the branch's own UHD plugin and test_nr_pdcch_blind_monitor
  on sens6. All 88 GTests from 12 suites passed.
- git diff --check passes. No full-repository suite claim is made.

## Live test status

The user reports three UEs running. A non-claiming UHD discovery query returned
X410 serial 327C1F2, claimed: False. No receiver process was active on sens6.

A bounded receive-only MRC-mode-2/four-RX launch was requested, but the execution
safety reviewer rejected it due to shared-hardware disruption risk and requested
explicit current approval. No OTA process or capture was started. No radio reset,
NIC change, transmitter, gNB data access or unrelated process termination occurred.

Prepared test files: /tmp/adaptive-ota-prep.aIeSXw on sens6 (launch remains blocked).
The intended run disables sensing output and DM-RS/SFO/branch-CFO corrections.
ISAC_CFO_TRACK_APPLY is presence-only in the source and must be absent, not set to 0.

This does not establish a working OTA run or fix the separate single-target UL
controller limitation. Do not report three-UE UL convergence or improved CRC
until actual independent per-RNTI evidence is available.

Logs on sens6:
- /tmp/ul-transition-red-build.log (pre-fix regression build)
- /tmp/ul-transition-green-build.log
- /tmp/ul-transition-green-test.log
