1. Project structure
Root is a Mixxx 2.7.0 checkout (CMake project(mixxx VERSION 2.7.0), prerelease alpha), ~6,000 files:
Directory
Contents
src/
C++20 source: ~964 .cpp + ~1,015 .h, organized by subsystem
res/
Resources: skins, QML, controller mappings, translations, shaders, DB schema
lib/
Vendored third-party code: portaudio, qm-dsp, xwax, hidapi, fidlib, kaitai, libshout-idjc, reverb, rigtorp, apple, mp3guessenc, etc.
cmake/
CMake modules/scripts
tools/
Python + shell build/helper scripts
packaging/
Debian, Flatpak, AppImage, WiX/MSI, macOS, Android, iOS packaging
Source subsystems in src: engine/ (59 cpp — real-time audio), library/ (116 cpp — largest), controllers/ (62), effects/ (57), waveform/ (73), widget/ (96), library/ includes track/, analyzer/, rendergraph/ (33), qml/ (41), preferences/ (33), mixer/, soundio/, vinylcontrol/, broadcast/, recording/, network/, musicbrainz/, test/ (137 test cpp files), plus main.cpp, mixxxapplication, mixxxmainwindow, coreservices.
Other file counts: 296 .qml, 152 .js controller scripts, 555 .xml (controller mappings), 19 .py, 328 files in res/controllers.
Important repo-state caveat: despite the directory being named paramix and origin being github.com/Sl1pstr3am/paramix, the tree is an uncommitted copy of Mixxx. git status shows M README.md plus every other file untracked; the only commit is still 96f541b ("Initial commit", containing just # paramix). Also missing from this copy: .clang-format, .pre-commit-config.yaml, .gitignore, .clang-tidy, .github/ — all referenced by CONTRIBUTING.md, so it was copied without dotfiles.
2. README
README.md (86 lines, modified vs. HEAD) is the upstream Mixxx README: "Mixxx is Free DJ software that gives you everything you need to perform live DJ mixes" on Linux, Windows, macOS. Covers quick start (stable/testing downloads, git clone https://github.com/mixxxdj/mixxx.git), GitHub Issues bug tracker, build pointer to CONTRIBUTING.md, docs, Transifex translation, community (Zulip, Mastodon, Bluesky), and GPLv2 license.
Related docs: CONTRIBUTING.md (fork/branch workflow, per-platform dependency scripts, mkdir build && cmake .. && cmake --build ., ctest, pre-commit, .clang-format style, PR rules), AGENTS.md, CHANGELOG.md, CODE_OF_CONDUCT.md.
3. Main application entry point
main() at src/main.cpp:211 — the executable target is mixxx, defined at CMakeLists.txt:2380 (Android) / :2454 (qt_add_executable(mixxx WIN32 src/main.cpp ...)), linking the static mixxx-lib.
Startup chain:
	1	main() — sets org domain mixxx.org, HiDPI policy, app name/version from VersionStore, parses CmdlineArgs, creates ErrorDialogHandler, macOS sandbox/settings migration, constructs MixxxApplication app(argc, argv).
	2	runMixxx() (src/main.cpp:61) — creates mixxx::CoreServices (the owner of all subsystems: settings, engine, controller manager, library, etc.).
	3	Two UI paths:
	◦	QML (when built with MIXXX_USE_QML and --qml or a QML skin is configured): mixxx::qml::QmlApplication.
	◦	Default widget UI: MixxxMainWindow mainWindow(pCoreServices), then pCoreServices->initialize(), mainWindow.initialize() (or initializeQOpenGL()), ControllerManager::setUpDevices(), mainWindow.show().
	4	pApp->exec() runs the Qt event loop. Exit codes: 1 fatal startup error, 2 bad command-line args.
A second entry point is the test binary mixxx-test (src/test/main.cpp, target at CMakeLists.txt:3014).
4. Technology stack
Layer
Technology
Language
C++20 (CMAKE_CXX_STANDARD 20 at CMakeLists.txt:597)
Build
CMake ≥ 3.22 (CMakeLists.txt:2); CPack packaging; Clang-Tidy hook
GUI framework
Qt 6 — min 6.2, Qt ≥ 6.4 required for QML (ComponentBehavior: Bound); modules: Concurrent, Core, Gui, Network, OpenGL, PrintSupport, Qml, Sql, Svg, Test, Widgets, Xml + ShaderTools, SvgWidgets
UI tech
Qt Widgets + custom skin engine (Deere, LateNight, Shade, Tango, QML skins) + QML/Qt Quick (res/qml, LateNightQML skin, QML settings)
Audio I/O
PortAudio (primary), PipeWire backend on Linux, iOS/CoreAudio path, vendored pa_ringbuffer
Audio processing
Engine graph (src/engine, src/rendergraph), qm-dsp, SoundTouch, rubberband, reverb, fidlib, FFTW3
File decoding
FFmpeg, libsndfile, FLAC, Ogg/Vorbis, Opus, MP3 (mp3lame, libmad, modplug), WavPack, MP4/AAC, ID3Tag, libjpeg-turbo
Analysis
libKeyFinder (key), Chromaprint (fingerprint), Ebur128 (loudness), analyzer plugins (Queen Mary)
Library DB
SQLite3 via Qt SQL, schema res/schema.xml
Controllers
MIDI (PortMidi), HIDAPI, USB bulk (libusb), HSS1394; mappings in JavaScript + XML (res/controllers, 328 files)
Broadcast
libshout-idjc; encoders: MP3, Vorbis, Opus, FLAC, WAV, FDK-AAC, FFmpeg
Networking/meta
Qt Network, MusicBrainz, Chromaprint, Rekordbox metadata (kaitai)
Optional features
FFmpeg, STEM, OPUS, HID, BULK, VINYLCONTROL, KEYFINDER, PIPEWIRE, PORTMIDI
Testing
GoogleTest/GMock (mixxx-test, 137 test sources), CTest; BUILD_TESTING
Tooling
Python (pyproject.toml → black, line-length 79), ESLint (eslint.config.cjs) for controller JS, Doxygen, pre-commit
Architecture conventions (per AGENTS.md): ControlObject/ControlProxy with [Group], key_name for inter-component messaging; the engine thread is real-time (no allocations, no locks, may emit but not receive Qt signals); parented_ptr/make_parented for Qt object-tree ownership.
5. Modifications
None. I only read files (cat/grep/git log/git diff); git status still shows the same M README.md + untracked tree I found, unchanged by me. No files created, edited, staged, or committed.
Note on AGENTS.md: I've read the Mixxx AI policy — no autonomous commits, pushes, PRs, issues, or PR-comment replies; AI-generated text must be disclaimed. The exploration above complied (read-only, no repo text written). Tell me what you'd like to do next — e.g. verify the copy against upstream Mixxx, restore the missing dotfiles, or start an actual task — and I'll proceed within that policy.





Usage 333K tok
13:10
The agents.md has been updated for this fork. Check only that again and internalize the new policy.
13:13

Completed
Completed in 13s
























Checked AGENTS.md on disk — it matches the updated instructions exactly (24 lines, 1,003 bytes, modified today 13:13, untracked). Internalized.
What changed: the entire long AI policy (no autonomous PRs/issues, no PR-comment replies, mandatory AI-generated-text disclaimers) is replaced by a single statement:
This fork of the Mixxx project is aided by the use of AI agents for development, and not all AI-generated code will be disclosed.
What that means operationally from here:
	•	Disclaimers: no longer required in this fork. I won't prepend/append "autonomously generated by AI" disclaimers to commit messages, PR descriptions, comments, or docs unless you ask.
	•	Disclosure: I won't treat undisclosed AI involvement as something I need to correct or flag in this fork's own history/artifacts.
	•	Autonomy: the old bans are gone, but that's removal of a prohibition, not a grant of authority — I'll still not git commit, git push, open PRs/issues, or post anything outward-facing without you explicitly asking. Say the word and I'll do it.
	•	Unchanged: architecture conventions (ControlObject/ControlProxy, real-time engine thread rules, parented_ptr/make_parented) and project layout stand as before.
Two boundaries I'll keep regardless, stated plainly so there are no surprises:
	1	Upstream: if work is ever aimed at mixxxdj/mixxx (a PR, patch, or upstream issue) rather than this fork, upstream's stricter policy governs, and I'll flag that at the time.
	2	Direct questions: if you or anyone asks me point-blank who wrote specific code, I'll answer truthfully. Non-disclosure as a default practice is your call on your fork; actively misrepresenting authorship is not something I'll do.
Nothing else re-explored, and the repo remains unmodified (AGENTS.md still untracked, tree as I left it).





Usage 79.9K tok
13:14
For this fork, we want to change the beatgrid engine, add commercial-grade stem separation similar to Native Instruments' Traktor, change the waveform display to show individual 4-stem waveforms, and generally change the UI. Create a comprehensive multi-agent plan to achieve this, and propose different options with descriptive summaries. This is an open source project, so feel free to explore open source options for adding features.
13:19

Completed
Completed in 13m 39s









This is a substantial architecture program, so I'll ground the plan in the actual code and in current open-source options. I'll fan out six read-only research agents (three codebase recon, three external/open-source) while I do my own spot checks.




















































































































































































































































The workflow output exceeded the harness limit and truncated after the first two agents. The beatgrid and stems recon is complete (very detailed); I'll re-run the remaining four tracks as focused, concise subagents.






































































































































































































All research is in except the DJ-UX survey (minor, I can characterize UX from the code and known market patterns). Writing the plan document now.















The UX research confirms the architecture and adds concrete requirements (both waveform modes, non-colour state, ~5× storage, 30–60 s separation). Folding those into the plan.























































































































































I ran six parallel research agents (three read-only codebase recon, three web research on separation, beat tracking and DJ UX) and turned the findings into a full program plan: docs/paramix-stem-beatgrid-ui-plan.md. Only that document was added — no source files were touched.
What the recon changed about the brief
Three facts reframe the work:
	1	Half of "stem support" already exists. The fork already plays NI .stem.mp4: 8-channel deck path (EngineDeck::processStem), four per-stem control groups ([ChannelN_Stem1..4] with gain/mute/QuickFX/VU), per-stem waveform peaks and a working 4-stem renderer. The gap is generation (separation), cache/persistence, and polish.
	2	The beatgrid model is already tempo-map capable. mixxx::Beats stores markers + terminal tempo, so variable tempo is representable. What is missing is detection quality (Queen Mary/SoundTouch only), downbeats/bars, confidence, and non-destructive editing — not a new data structure from scratch.
	3	Licences decide the ML stack. Mixxx is GPL-2.0. HTDemucs v4 (code and weights MIT) and Beat This! (code and weights MIT, 82 MB ONNX export with beat + downbeat logits) are the only commercial-quality options that are safe to bundle. madmom/Essentia weights are CC BY-NC-SA, Essentia is AGPLv3, aubio is GPLv3, and BS-RoFormer/MDX23C community weights ship with no licence — those become optional user-supplied tiers, never bundled.
Program options

A — Momentum
B — Traktor-class (recommended)
C — Platform leap
Scope
ML beat detector, HTDemucs separation, cache, stem waveform polish, skin refresh
A + BeatGrid-3.0 (downbeats/confidence/dual-write), quality tiers + model manager, 4-lane waveform redesign, new QML skin/design system, .stem.mp4 export
B + real-time separation, warp/tempo-curve grids, full QML rewrite, mobile
Time / agents
10–12 wks / 3–4
20–24 wks / 6–8
9–12 months / 10+
Trade-off
Fast, low risk, no bars
Matches the brief end-to-end
Real-time separation isn't commercially viable yet (0.5–2 s latent, degraded); even Traktor pre-computes stems via iZotope RX
Recommendation: fund B but make A its first shippable milestone. Defer C's real-time piece to a spike.
Key technical choices in the plan
	•	Beatgrid: additive BeatGrid-3.0 (downbeats, confidence, source, hypotheses) with a dual-written legacy blob so downgrades don't lose phase; Beat This! behind the existing analyzer-plugin interface, Queen Mary as fallback; non-destructive editor; bar-aware lines/quantise/loops as optional.
	•	Separation: HTDemucs v4 via ONNX Runtime (CoreML / DirectML / CUDA / CPU), offline background queue with progress/cancel/resume; optional user-supplied RoFormer-class "Max" tier behind a licence gate.
	•	Cache: 8-channel FLAC + JSON at the track's original rate, atomic writes and LRU GC (~5× MP3, 50–120 MB/track); optional .stem.mp4 export for Traktor interop.
	•	Playback: hook SoundSourceProxy::openAudioSource() (it already holds the TrackPointer) to return a stem source when a cache exists — the existing 8-channel engine path then works unchanged.
	•	Waveform: both stacked lanes and tinted composite (market research says users want both), muted stems visible but dimmed, labels + colour-blind-safe palette (never colour alone), downbeat accents, performance fix for the scene-graph precision workaround.
	•	UI: new "Paramix" QML skin on a design system, hybrid shell (legacy library/preferences stay widgets initially), stem mixer strip, instant acapella/instrumental, separation status, model manager.
Multi-agent execution
Ten roles (architect/integrator, beatgrid core, beat ML, stem runtime, stem pipeline/DB, audio engine, waveform, QML/UX, QA, compliance/packaging), each owning disjoint directories. Six phases: baseline (−1) → spikes/ADRs (0) → contract freeze (1) → three build waves (2.1–2.3) → integration (3) → hardening (4) → release (5), with nightly integration, feature flags, peer-agent review and a human audio gate. Critical path: separator → cache/queue → playback → waveform → integration; beatgrid and UI run in parallel. The plan includes a concrete 10-day backlog and a file-level touch map.
Decisions I need from you before Phase 1
	1	Quality default: htdemucs (fast) vs htdemucs_ft (slower, slightly better)?
	2	Auto-separate on import, on demand, or both?
	3	Is downbeat/bar support required for v1, or is better BPM/phase enough?
	4	.stem.mp4 export required for Traktor interop, or internal cache only?
	5	New QML skin as default, or opt-in with widget skins retained?
	6	Desktop-only (macOS/Windows/Linux) for v1?
	7	Model hosting: project server, GitHub Releases, or bundle in installers?
	8	Keep "Mixxx" branding or rebrand the fork as "Paramix"?
The full document covers the detailed options, interfaces, schema, licence matrix, risks and acceptance metrics. If you want, I can turn the 10-day backlog into actual issues/tasks next — still without modifying source until you say go.

Edited paramix-stem-beatgrid-ui-plan.md
+560
-0


paramix-stem-beatgrid-ui-plan.md
Comprehensive program plan: options, target architecture, multi-agent execution waves, licensing, risks and first-10-days backlog for beatgrid, stem separation and UI.







Usage 4.2M tok
13:33
1. Go with htdemucs_ft. 2. Separate on demand, but include an option to batch separate. 3. Downbeat/bar support is necessary. 4. Should be interoperable with Traktor. 5. New QML skin is default, but should be highly customizable. 6. Should support Windos, Linux, MacOS ARM, and MacOS intel. 7. Clarification required. 8. Rebrand as Paramix and make appropriate UI changes. Before doing anything, clarify what you're asking about model hosting, then we will proceed
13:44

Completed
Completed in 7m 15s









Holding all work until this is settled. Here's exactly what I meant by "model hosting."
The distinction
"Model hosting" is not about ONNX Runtime (the ~30–60 MB inference library per platform, which always ships inside the app). It's about the neural network weights — for your choice, htdemucs_ft, which is not one file but a bag of four specialist models (~1.26 GB fp32, ~630 MB fp16). Those weights are MIT-licensed, versioned independently of the app, and are the single biggest thing you'd add to a download.
So the question is two questions:
	1	Do the weights travel inside the installer, or get fetched after install?
	2	If fetched, from whose server?
Why it matters
Concern
Effect
Installer size
+0 (download) to +0.6–1.3 GB (bundled) on Windows/Linux/macOS
First-run experience
Bundled = offline "just works"; download = needs internet once (or an offline bundle)
Updates
Models can be updated independently (download) or require a full app release (bundled)
Packaging
Bundled models complicate Flatpak/AppImage/DEB and macOS notarization; runtime download needs network permission and a proxy story
Cost/ops
Self-hosting 630 MB–1.3 GB per user needs bandwidth; GitHub Releases is free but not a CDN; Hugging Face is easy but a third-party dependency
Trust
Runtime download needs SHA-256, a licence notice (MIT), version pinning, and cache invalidation when the model changes
The three coherent policies
A. Hybrid (my recommendation). Bundle the single htdemucs fp16 model (~166 MB) so stems work offline immediately; download htdemucs_ft (~630 MB fp16) on first use for maximum quality, with progress/resume/checksum and an MIT licence dialog. Optionally publish an "offline bundle" installer/zip for users who want everything preinstalled. Smaller base installer, instant basic function, best quality on demand, and a clean upgrade path.
B. Download-only. Ship no model. Smallest installer; first separation requires internet (or a separate offline bundle). Simplest packaging, but the first "Separate" click becomes a 630 MB download for a user who may be offline in a club.
C. Full bundle. Put htdemucs_ft in every installer. True offline out of the box, but +0.6–1.3 GB per platform, every model update becomes an app release, and Linux/Flatpak packaging gets awkward.
For the hosting channel itself
	•	GitHub Releases — free, versioned, supports checksums and per-file assets; appropriate for a fork, but it is not a CDN and very high download volume can be throttled.
	•	Self-hosted server/CDN — control and predictable bandwidth; requires you to provide the URL/infrastructure and pay for egress.
	•	Hugging Face Hub — easy model distribution; third-party availability, and mirrors can carry incorrect licence metadata, so pin exact files and checksums.
	•	Distro repos (Linux) — models as a separate package; good for packagers, extra maintenance.
A detail that affects the answer: because htdemucs_ft is ~630 MB fp16 rather than ~166 MB, bundling it in every installer is a much bigger commitment than bundling the single model. That's why I'd separate "fallback model" from "max-quality model."








Model hosting
Where should the htdemucs_ft weights be downloaded from?
GitHub Releases (Recommended)
Packaging
How should the models ship with Paramix?
Download-only
13:49






















































































































































Locked in: models on GitHub Releases, download-only. I've folded all eight answers into the plan (docs/paramix-stem-beatgrid-ui-plan.md, now v0.2 with a decisions table, a dedicated model-distribution section, and a rebrand section).
What the hosting answer means in practice
	•	Nothing model-related ships in the installers or git. Only ONNX Runtime travels with the app. On first "Separate" (or Settings → Stems → Download), Paramix fetches htdemucs_ft (~630 MB fp16) with an MIT licence prompt, SHA-256 verification, resume and progress. I added Pre-download and Install from file so a DJ can prepare offline; an optional offline bundle can be added later without changing the architecture.
	•	One consequence worth naming: download-only means a machine that has never been online has no stems until the model is installed. The UI must say that plainly rather than fail at the first click.
	•	htdemucs_ft is the 4-model bag, so expect ~2 minutes per 4-minute track on an M-series CPU and seconds with CoreML/DirectML — slower than Traktor's iZotope RX (~30–60 s) on CPU-only, which is why the plan targets GPU acceleration by default. The single htdemucs model remains only a low-end fallback, not bundled.
	•	Sub-decision left open: whether the weights live on the existing Sl1pstr3am/paramix Releases or a separate paramix-models repo. I defaulted to a dedicated repo so model assets don't bloat the app repo; say if you prefer otherwise.
The rest, as locked
On-demand + batch (no auto-separate on import) · downbeats/bars in v1 · .stem.mp4 export in v1 for Traktor · Paramix QML skin as default, with a documented customisation contract (tokens, layout variants, colour schemes, runtime QML overrides) · Windows x64, Linux x64, macOS arm64 + x86_64 universal · rebrand as Paramix, with the recommendation to keep the ~/.mixxx data directory in v1 so existing libraries, controller mappings and settings keep working (brand-only rebrand), and migrate later if you want.
Before I start building
No source files have been touched — only the plan document. Phase −1 (the baseline) makes the first real repo changes: committing the Mixxx import on a branch, restoring .clang-format, .pre-commit-config.yaml, .gitignore and GitHub Actions, then running builds/tests on all four targets. Because that creates the first commits, I want one explicit go-ahead:
Reply "start Phase −1" and I'll execute the baseline, stop, and report results before any feature work. If you'd rather skip the baseline and go straight to the model/beatgrid spikes (no commits, just throwaway benchmarks), say "start spikes" instead.

Edited paramix-stem-beatgrid-ui-plan.md
+94
-39





Usage 2.3M tok
13:51
start Phase -1. Use as many agents as necessary to work quickly, but remain accurate with your work.
13:56

Failed
Failed


Analysis completed





Starting Phase −1. First, a quick repository and environment audit before touching git history.


Ran commands





































Environment check: no CMake, Ninja, Qt, Homebrew, or clang-format — only Apple clang. I'll fan out read-only audits (build probe, CI design, hygiene/licensing) while I prepare the git baseline.


Ran commands, coordinated subagents, wrote files, etc.




















































































































































This turn failedRequest quota exhausted.
ACCOUNT_QUOTA

Edited 8 files
+875
-272
	•	.clang-format +80 -0 
	•	.clang-tidy +25 -0 
	•	.gitattributes +31 -0 
	•	.github/workflows/ci.yml +182 -0 
All 8 files





Available only on the last message of a completed turn
13:57
start Phase -1. Use as many agents as necessary to work quickly, but remain accurate with your work.
