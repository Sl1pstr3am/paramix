# QML localization & accessibility — conventions and coverage

Workstream W2 (accessibility and localization of the shared QML UI). This document records
the conventions applied to `res/qml/**`, the coverage so far, and the strings that were
deliberately left unwrapped.

## 1. Conventions applied

### 1.1 `qsTr()` for user-visible strings

- Every user-visible string literal in the shared QML components is wrapped in `qsTr("...")`.
- Dynamic strings use placeholders, never concatenation of translatable text:

  ```qml
  // before
  text: "AUX " + root.unitNumber
  text: "FX" + (index + 1)

  // after
  text: qsTr("AUX %1").arg(root.unitNumber)
  text: qsTr("FX%1").arg(index + 1)
  ```

- A `?\n` inside a translatable literal is kept literal inside `qsTr("...")` (the translator
  sees the `\n`, the UI renders the line break), for example `qsTr("Play\nButton")` and
  `qsTr("Press to\nset %1 visual")`.

### 1.2 `Accessible.*` on interactive controls

The QML layer had zero `Accessible.*` usage before this work. Two categories were added:

1. **Custom `Item`-based controls** (which Qt does not make accessible automatically) got an
   explicit role, value range, and increase/decrease actions:
   - `Mixxx/Controls/Knob.qml` → `Accessible.role: Accessible.Dial` plus `value`,
     `minimumValue`, `maximumValue`, `stepSize`, `onIncreaseAction` / `onDecreaseAction`.
   - `Mixxx/Controls/Slider.qml` → `Accessible.role: Accessible.Slider` plus the same value/
     range/step/action bindings. These two base components cover every knob and fader in the
     skin, so a screen reader can now announce "dial/slider, 45%" without per-instance work.
2. **Button-style controls** got an explicit name so that label-driven names are captured even
   when the button draws a custom `contentItem` that overrides `AbstractButton.text`:
   - `Button.qml`, `FormButton.qml` → `Accessible.name: text` (plus `Accessible.role: Accessible.Button`).
   - `ActionButton.qml` → `Accessible.name: labelField.text` (it renders via a `label` alias,
     not `text`, so it would otherwise be unnamed).
   - Icon-only buttons (no text) got literal names, e.g. `Deck/Toolbar.qml` ("Reverse",
     "Eject"), `Deck/PlayButton.qml` ("Play"), `Deck/Loop.qml` ("Halve loop", "Double loop"),
     `Deck/BeatJump.qml` ("Beatjump backward/forward", "Halve/Double beatjump size"),
     `MainWindow.qml` (the gear button → "Preferences").

### 1.3 Not wrapped (by rule)

ControlObject group/key identifiers, `objectName`s, property names, URLs/paths, color values,
state names, and code tokens are left untouched. See section 4 for the specific gray areas.

## 2. Coverage summary

- **Files changed:** 46 under `res/qml/`.
- **High-traffic screens localized:** the deck (`Deck.qml`, `Deck/*`), mixer columns, effects
  unit/slot, sampler, aux/microphone units, main window toolbar, splash, developer tools, and
  the full Settings stack (`Settings/*`).
- **Reusable controls made accessible:** `Button`, `ActionButton`, `FormButton`, the custom
  `Knob` and `Slider`, and the waveform overview marker labels.

### 2.1 Already-localized files (verified, not reworked)

A subset already used `qsTr()`/`qsTranslate()` before this workstream and was left as-is:
`AboutDialog.qml`, `ApplicationMenuActions.qml`, `ApplicationMenuCommands.qml`,
`MainMenuBar.qml`, `Library.qml`, `Library/Track.qml`, `Library/SourceTree.qml`,
`Effects/EffectChainPresetPopup.qml`, `Effects/EffectSlotPresetMenu.qml`,
`Settings/ControllerScreens.qml`, and large parts of `Settings/Library.qml` and
`Settings/SoundHardware.qml`.

### 2.2 Remaining / not finished

- **`Settings/AudioRouter.qml`** is only partially localized by design (see 4.1). The
  entity/channel/gateway identifiers are code tokens and stay in English.
- A handful of low-value or risky spots are left (see section 4). Everything else with a
  clearly user-visible `text`/`title`/`label`/`placeholderText`/`tooltip` literal is wrapped.
- No `accessibleName` was invented for knobs/faders whose only identity is a ControlObject
  `group`/`key` (e.g. `ControlKnob`, `ControlFader`). They now expose role + value + keyboard
  increase/decrease via the base components, but a human-friendly name would need a mapping
  from ControlObject key → display name, which is out of scope here.

## 3. Examples

```qml
// Deck/Toolbar.qml
text: "Beatgrid"                        ->  text: qsTr("Beatgrid")
// (eject button, icon-only)
key: "eject"                            ->  key: "eject"
                                            Accessible.name: qsTr("Eject")

// SyncButton.qml
return "Leader";                        ->  return qsTr("Leader");

// Sampler.qml
text: "FX" + (index + 1)                ->  text: qsTr("FX%1").arg(index + 1)

// Button.qml (base for most buttons)
AbstractButton {                        ->  AbstractButton {
    ...                                     Accessible.role: Accessible.Button
                                            Accessible.name: text
                                            ...
```

## 4. Strings deliberately left unwrapped (and why)

### 4.1 Code tokens in the audio router

`Settings/AudioRouter.qml` and the save/load path in `Settings/SoundHardware.qml` compare
entity/channel/address strings directly (`connection.source.address == "Main"`,
`connection.source.entity.name == "Mixer"`, etc.). Translating these would break routing:

- `audioTypeMap` values: `"Mixer"`, `"Main"`, `"PFL"`, `"Booth"`, `"Bus"`, `"Deck"`,
  `"Output"`, `"Vinyl Control"`, `"Microphone"`, `"Auxiliary"`, `"Record"`,
  `"Additional input"`.
- Gateway `name:` values (`"Main"`, `"PFL"`, `"Booth"`, `"Left Bus"`, `"Center Bus"`,
  `"Right Bus"`, `"Auxiliary"`, `"Microphone"`, `"Alternative input"`, `"Mixer"`,
  `"Record/Broadcast"`, `"Deck N"`).
- The mixer entity's `Mixxx.SettingParameter` labels `"PFL"`, `"Main"`, `"Booth"`, `"Mixer"`,
  `"Decks"`, `"Input"`, `"Output"`, `"Broadcast"` — these mirror the untranslated entity names
  used for the visual labels, so translating only the search label would create a mismatch.
  The display-only headers ("Inputs", "Outputs", "Multi-Soundcard Synchronization") and the
  multi-soundcard tooltips *were* wrapped.

### 4.2 `RatioChoice.options` / boolean value tokens

`RatioChoice` uses `options` both as the display text and as the compared `selected` value, and
the settings save/load code compares them by string (`selected === "on"`, `options[1] ==
selected`, `options.indexOf(selected)`). Translating the options would change the stored value
semantics, so all `options: [...]` arrays are left in English until `RatioChoice` is refactored
to separate display text from a stable value:

- `["on","off"]`, `["off","on"]`, `["mono","stereo"]`, `["soundcard","network"]`,
  `["simple","advanced","advanced (!)","legacy"]`, `["experimental","default","disabled"]`,
  `["off","library","all"]`, `["no","while running","while playing"]`,
  `["elapsed","remaining","both"]`, `["reject","allow","when stopped"]`,
  `["none","key","both","tempo"]`, `["follow soft leader","use steady"]`,
  `["down","up"]`, `["original key","current key"]`, `["reset key","keep key"]`,
  `["abrupt jump","smooth ramping"]`, `["normalize","global gain"]`, `["overlap","stacked"]`.

### 4.3 Proper nouns, brand names, technical abbreviations

- Graphics API names `"OpenGL"`, `"Metal"`, `"Direct3D 11"`, `"Direct3D 12"`, `"Vulkan"`
  (in `Settings/Waveform.qml`) — proper nouns. The paired `"Software"`, `"Null"`, `"Unknown"`
  *were* wrapped.
- Controller device type badges `"MIDI"`, `"HID"`, `"BULK"`, `"UNKN"`, `"N/A"` — protocol
  abbreviations / placeholder, left as-is.
- Cue-mode names `"Mixxx"`, `"Pioneer"`, `"Denon"`, `"Numark"` and slider-range strings like
  `"8% (Technics SL-1210)"` — contain brand names; left as-is.
- Unit suffixes `" ms"`, `" fps"`, `"x"`, `"s"`, `"%"`, `"px"` and slider markers
  `"0ms"`, `"100ms"`, `"1s"`, `"10s"` — universal units/abbreviations.
- Short technical button labels were still wrapped for consistency (`"PFL"`, `"D/W"`, `"ON"`,
  `"FX"`, `"F"`, `"I"`, `"LINK %1"`); these are the kind of strings translators typically
  leave unchanged but marking them keeps `lupdate` output complete.

### 4.4 Other

- `Deck/InfoBar.qml` `availableData` list `["none","title","year","time","artist","rating"]`
  is a data-role identifier list (used in `indexOf`) — not display text.
- `Mixxx/Controls/WaveformOverview.qml` marker `cueText: "C"` and the `▲`/`▼`/`<`/`>`/`+`/`-`
  glyphs used as indicators are symbols; left as-is (the surrounding `"IN"`/`"OUT"`/`"LOOP"`
  marker text was wrapped).
- `AudioRouter.qml` pluralized tooltip
  (`${n} connection${s} hidden\nUse the advanced mode to view them`) is left as a template
  literal — proper handling needs a `%n` plural form, deferred rather than done poorly.
- `PreviewDeck.qml` `"PreviewDeck placeholder"` is wrapped but flagged as a placeholder the
  main workstream will replace with a real deck.
