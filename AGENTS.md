# AGENTS.md -- CB Radio

Source of truth is `D:\MTLiveMap\mt_loader\plugins\cb_radio.cpp` (+ `cb_radio_panel.html`); copy changes here by hand.

- `SteamNetworkingMessage_t`: data ptr at +0, size at +8, `m_identityPeer.m_steamID64` at +24 (Steamworks 1.57 layout;
  the game ships `Steamv157`). Released with `SteamAPI_SteamNetworkingMessage_t_Release`.
- `SteamNetworkingIdentity` is 136 bytes; type 16 = SteamID. Send flags `1|32` (no Nagle, restart broken sessions).
- Sessions are mutual, because both ends send hellos. `AcceptSessionWithUser` is also called for every player on each
  hello round.
- FText is 16 bytes in this build (jmap); `ClientShowSystemMessage` takes it by value from `Conv_StringToText`.
- Never call `received()` while holding `mu` (it locks): the loopback path sends to our own Steam id instead.
- Tested 2026-11-21 alone:
  - Steam loops a message to our own id.
  - The own PlayerState id matches Steam's.
  - End/Home and the notices work.
  - A loopback over was recorded and sounds right.
- Chat: Home/End are ignored while the chat's `ChatTextBox` has keyboard focus (`Widget.HasKeyboardFocus`, game thread,
  4x a second). Take the chat widget that *has* a text box: the main menu leaves one without. The `ChatInputBox`
  visibility does not change when chat opens.
- Devices are picked by waveIn/waveOut name (31 characters max); one unplugged falls back to the default.
- Untested: two real players.
