export const meta = {
  name: 'firmware-wave2',
  description: 'App state machine + second half of the UI (keyboard, settings, QR, onboarding) in parallel worktrees, then adversarial review',
  phases: [
    { title: 'Implement', detail: 'app / ui2 agents, one git worktree each, must build' },
    { title: 'Review', detail: 'adversarial code review of each branch, fixes committed on the branch' },
  ],
}

const COMMON = `You are a senior embedded engineer working on the ESP32-S3 "Round Assistant" firmware (ESP-IDF v6.1, C, LVGL 9.5).
Your git worktree is WT_PATH (branch WT_BRANCH, created from the latest commit of the main branch of C:\\esp32-ai-assistant, which already contains the merged wave-1 work: components/ai + orhttp (OpenRouter chat/TTS/STT clients, verified on hardware), main/ui (UI foundation: theme, fonts, orb, rim, Home/HIST/RD/touch screens, ui console), dev console protocol v2 + tools/dev.py v2, components/ota, main/selftest.c, audio injection). Your shell does NOT start there: work ONLY inside this worktree. Use absolute paths under WT_PATH for Read/Write/Edit; start every PowerShell command with "Set-Location WT_PATH;" and every Bash command with a cd into it (in Bash the path is /c/ra-wt/<name>). Never modify C:\\esp32-ai-assistant itself or the other worktrees under C:\\ra-wt.

Read first (files inside WT_PATH): docs/requirements.md, docs/hardware.md, docs/architecture.md (authoritative), docs/protocol.md, docs/ui.md (authoritative UI spec), and the existing code: main/main.c, main/ui/*.h (ui.h is the UI API: ui_post/ui_post_text messages, ui_set_input_cb input events, ui_lock), components/ai/*.h (ai_client.h: async turns with events; tts_client.h), components/audio/*.h, components/settings/settings_table.inc, components/net/*.h, components/devconsole/devconsole.h.

Facts verified on the real board: display/touch/screenshots OK; mic right slot, noise floor about -45 dBFS with a PC fan nearby, speech peaks about -22 dBFS; Wi-Fi up via saved network; OpenRouter reached through the relay in setting ai_url; ai ask / ai voice / tts work (ai 0.9-2.8 s, TTS first audio 0.6-1.1 s); uploads 250 KB/s on a reused connection, the first request after boot pays about 2-3 s of TLS handshake (so pre-warm on PTT press). IMPORTANT finding: on a clip with only fan noise the model hallucinated a transcript ("из-за предательства"), so the app must gate clips with no speech before sending (energy/VAD based: e.g. require enough 20 ms frames above the noise floor + 12 dB and a minimum speech duration; then show the "not heard" message without calling the AI).
The old console REPL dropped non-ASCII input; console v2 reads raw UTF-8 lines.

Build: PowerShell tool: Set-Location WT_PATH; & .\\tools\\idf.ps1 build *> build.log; "exit=$LASTEXITCODE"  then Select-String -Path build.log -Pattern 'error:|warning:' . First build of a worktree takes 5-15 minutes. -Werror: fix every warning in code you touch. Do not commit build.log, build/, managed_components/ or secrets.json.

Hard rules:
- NEVER flash, NEVER open COM7 or any serial port, NEVER run tools/dev.py against the device: the lead engineer flashes and tests your branch.
- The user is asleep next to the device: nothing you add may play sound by default at boot (no startup earcon unless volume setting > 0 and text_only == 0 — keep the existing settings semantics).
- Write and edit C sources with the Write/Edit tools, never via shell heredocs or Python string literals (escapes get mangled in this shell).
- Do not install anything; no downloads (fonts already exist; if you need extra glyphs, regenerate with the existing tools/mkfonts script and the fonts already in assets/fonts).
- Stay inside your area; integration edits elsewhere must be minimal and listed in your report. Follow the existing code style (plain C, 4-space indent, short why-comments, ESP_RETURN_ON_ERROR, public functions declared in headers with one-line comments).
- Commit on your branch when it builds (git add -A; git commit -m "<summary>" with a body ending with the line: Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>). Do not push or touch other branches.`

const REPORT = {
  type: 'object',
  properties: {
    branch: { type: 'string' }, commit: { type: 'string' }, build_ok: { type: 'boolean' },
    summary: { type: 'string' }, files: { type: 'array', items: { type: 'string' } },
    public_api: { type: 'string' }, integration_edits: { type: 'array', items: { type: 'string' } },
    hardware_tests: { type: 'array', items: { type: 'string' }, description: 'Exact dev.py commands (console v2) and expected results; prefer screenshots, injected taps and injected audio (mic load) over anything needing a human' },
    known_gaps: { type: 'array', items: { type: 'string' } },
  },
  required: ['branch', 'commit', 'build_ok', 'summary', 'files', 'public_api', 'integration_edits', 'hardware_tests', 'known_gaps'],
}
const REVIEW = {
  type: 'object',
  properties: {
    verdict: { type: 'string', enum: ['ready', 'fixed', 'needs_work'] },
    fixes_committed: { type: 'array', items: { type: 'string' } },
    remaining_issues: { type: 'array', items: { type: 'string' } },
    commit: { type: 'string' },
  },
  required: ['verdict', 'fixes_committed', 'remaining_issues', 'commit'],
}

const WT = k => ({ path: 'C:\\ra-wt\\' + k, branch: 'wave2-' + k })
const common = k => COMMON.split('WT_PATH').join(WT(k).path).split('WT_BRANCH').join(WT(k).branch)

const TASKS = [
  {
    key: 'app',
    prompt: `YOUR AREA: main/app/* (new), main/main.c, board buttons/battery helpers in components/board (buttons.c, battery.c per architecture §1.2), and a speech gate (components/audio/vad.c or inside main/app).
Implement the application layer of architecture §1.12 and §2 (task list, message flow, state machine §2.3) on top of the existing APIs, so the device works end to end without the console:
- Assistant mode: push-to-talk exactly as docs/ui.md §5.1 (hold and tap both work; on-screen orb via ui_set_input_cb events, and the physical BOOT button); ai_client_prewarm() on press; recording with audio_in; on release: speech gate (reject silence/noise-only clips -> "not heard" UI state, no AI call) -> ai_client_submit -> UI states (listening, thinking, speaking, error) and chat bubbles (user transcript, assistant reply, mood if available) through ui_post -> if text_only == 0, tts_client_speak with the reply; barge-in (pressing while speaking stops TTS and starts a new recording); cancel; errors mapped to the UI error presentation (ui.md §3.14/§5.6) using or_err_t msg_ru; history clear rules (protocol §4.3).
- Translator mode (ui.md §3.4/§5.5): one auto-detect PTT plus the language chips (hold a chip = forced source language), ai translator turns with tr_lang_a/tr_lang_b, translation spoken in the target language when voice is on, "wrong language" re-run of the stored clip.
- Text-only toggle (setting text_only, UI control as ui.md specifies), mode switch (setting mode), volume/brightness apply on change (settings_on_change), PWR button semantics (ui.md §2.3: back / long-press power off with the latch, armed only after first release), battery voltage (ADC1 ch7 GPIO8, x3) shown where ui.md puts it, Wi-Fi state into the UI status, low-heap safety.
- Dev console commands (devconsole_register): "app state", "ptt down", "ptt up", "ptt tap", "cancel", "mode assistant|translator", "textonly 0|1", "btn boot|pwr click|long|down|up" (simulate buttons through the same code path), "app say <text>" (run a typed assistant turn through the whole pipeline incl. UI bubbles). They must make the whole flow testable with injected audio: e.g. "mic src inject fast" + "mic load <wav>" (existing) then "ptt tap" or "ptt down"/"ptt up".
Keep main.c boot order from the infra work (self-test, safe mode, READY event) and replace the remaining test glue.`,
  },
  {
    key: 'ui2',
    prompt: `YOUR AREA: main/ui/* only (new files for the screens below, plus the minimal hooks in the existing UI navigation table).
Implement the second half of docs/ui.md on top of the existing UI foundation (theme, fonts, chrome, rim, subjects, navigation, ui console are already there — read them and reuse them; do not restyle what exists):
- §4 the round keyboard: two-step "group -> letter" keypad on the Russian phone-keypad groups, EN layout, digits/symbols, the hex keypad for API keys with the "Aa" escape, exactly per §4.1-§4.6 (geometry verified against LVGL 9.5 update_map, kb_apply re-applying dynamic ctrl flags after every map swap).
- §3.8 text entry template (T_TEXT) around the keyboard.
- §3.7 list template (T_LIST) and the Settings tree from §2.1/§2.2: Wi-Fi (scan list via net_scan, connect with password entry, saved networks, forget), AI (provider base URL, API key entry, model picker fed by ai_client_list_models, key/credit info via ai_client_key_info), Voice (text-only toggle, TTS model/voice picker via ai_client_list_tts_models/ai_client_tts_voices, speed), Translator languages (tr_lang_a/tr_lang_b pickers with the language table of protocol §4.2), Display (brightness), Sound (volume), Language (ui_lang ru/en), About/diagnostics (version, panel id, heap, IP, partition, rollback state). Every change goes through settings_set_* (they persist to NVS).
- §3.9 adjuster (T_ADJUST) for volume and brightness.
- §3.10 phone setup screen (T_QR) with lv_qrcode of WIFI:T:WPA;S:<ssid>;P:<pass>;; from portal_start()/portal_credentials(), and the portal URL; leaving the screen stops the portal.
- §3.11 confirm dialog, §3.12 step template + first-boot onboarding (§5.7) shown only when no Wi-Fi network is saved or no ai_key is set, §3.13 ambient/sleep screen and the dim/off timers of §5.8 (settings dim_s), earcons/hints of §5.9 only as visuals (no sound now).
- Extend the ui console: "ui goto <any new screen>", "ui kb <utf8 text>" (types text by injecting taps on the real key areas, including group/letter steps and page/language switches) and keep "ui tree"/"ui tap <name>" working for all new objects (give objects names).
Run network/NVS calls off the LVGL task (the UI must never block on HTTP or flash writes); follow §7.7 thread-safety rules.`,
  },
]

phase('Implement')
const results = await pipeline(
  TASKS,
  t => agent(`${common(t.key)}\n\n${t.prompt}`, { label: `impl:${t.key}`, phase: 'Implement', schema: REPORT }),
  (r, t) => {
    if (!r) return { key: t.key, impl: null, review: null }
    return agent(`${common(t.key)}\n\nYou are an adversarial reviewer. Another engineer implemented the "${t.key}" work package in this same worktree on branch ${r.branch} (commit ${r.commit}). Review git -C ${WT(t.key).path} diff main...${r.branch} hard against docs/architecture.md, docs/ui.md, docs/protocol.md and the task below. Look for bugs (races, missing LVGL locks or blocking calls in the LVGL task, buffer overflows, UTF-8 handling, leaks, wrong IDF 6.1 / LVGL 9.5 API use), state-machine holes (double press, press during thinking/speaking, cancel during upload, Wi-Fi drop mid-turn, no key, no speaker), spec deviations, internal-RAM waste, and broken existing behaviour. Fix what you can directly on the branch, rebuild until it passes, commit the fixes on that branch, and report.\n\nImplementer's report:\n${JSON.stringify(r, null, 1)}\n\nTask:\n${t.prompt}`, { label: `review:${t.key}`, phase: 'Review', schema: REVIEW })
      .then(v => ({ key: t.key, impl: r, review: v }))
  },
)
return results
