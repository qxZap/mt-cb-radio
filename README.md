# CB Radio

A 40-channel CB rig in every Motor Town vehicle. Hold **Home** to talk; everyone on your channel within range hears you
through the radio, with hiss, crackle and fading that grow with distance. A plugin for the
[Plugin Loader](https://github.com/qxZap/mt-plugin-loader).

| Key | |
|---|---|
| Home (hold) | talk |
| End / Ctrl+End | next / previous channel (shown on screen) |
| Ctrl+Home | the radio panel, `http://127.0.0.1:8095/` |

## How it works

- **Transport.** Steam's `ISteamNetworkingMessages`, peer to peer by Steam id, on a channel of its own (27185). The
  game's net driver uses Steam *sockets*, so the messages interface is free. Nothing runs on the server.
- **Who is there.** Every `MotorTownPlayerState` in the session; the Steam id is read from `PlayerState.UniqueID`
  (checked against `ISteamUser::GetSteamID` for the local player). A hello goes to each every 3 s; whoever answers
  has the plugin.
- **Voice.** The mic is opened only while talking (waveIn, 8 kHz). 300-2800 Hz band, levelled, the preamp driven into
  `tanh`, mu-law. 20 ms frames, 180-byte packets carrying the sender's map position, sent only to rigs within range.
- **Radio.** The listener's end does it. Signal = `(1 - km / RangeKm)^0.6` (10 km by default), shown in S-units. As it
  drops you get hiss, crackle, slow fading, and below 0.35 the squelch chattering. There is a key-up kerchunk, a roger
  beep and a squelch tail. Two carriers at once make a heterodyne whistle, and it is half duplex.
- **Channels.** The real band plan (26.965-27.405 MHz, 23 between 25 and 26). A privacy code (1-99) must match
  exactly. Presets: 9 Emergency, 19 Truckers, 17 Coast road, 14 Towing, 21 Taxi & bus, 27 Delivery, 33 Racing,
  3 Marine, plus the player's own.
- **On screen.** Channel changes and the station being heard go through the game's own
  `MotorTownPlayerController.ClientShowSystemMessage`.
- **Chat.** Home/End do nothing while you type in the game's chat.
- **Devices.** Microphone and speaker can be picked in the panel (by name; System default otherwise).
- **Panel.** A localhost page embedded in the dll (`plugins/cb_radio_panel.html` -> `.inc` by `tools/embed_panel.py`).

`tools/cb_radio/preview.py` is the same sound chain in Python, used for sound previews (`python preview.py demo`
self-checks it).

## Settings

`MotorTown\Binaries\Win64\plugins\cb_radio.ini`, written by the panel:

- `[Radio]`: `Channel`, `Code`, `Volume`, `Squelch`, `MicGain`, `RogerBeep`, `Handheld` (talk on foot), `RangeKm`, `MicDevice`, `SpeakerDevice`,
  `Presets`.
- `[Keys]`: virtual-key codes `Talk`, `Channel`, `Panel`.
- `[Panel] Port`.
- `[General] Disabled=1`.
- `[Debug]`:
  - `LoopbackKm`: your own over comes back through Steam as a station this far off.
  - `TalkFile`: an 8 kHz mono wav sent instead of the mic.
  - `RecordOut`: raw s16le 8 kHz copy of the speaker.

## Build

`build.bat` (MSVC 2022 x64, Python 3) writes `out\mt_plugin_CBRadio.dll`. Install it in
`MotorTown\Binaries\Win64\plugins\`.
