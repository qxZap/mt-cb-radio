// CB Radio -- plugins\mt_plugin_CBRadio.dll. Every vehicle has a CB rig (the user, 2026-11-21: "all vehicles shall have
// CB radio" until a CB part exists). Hold Home to talk; End / Ctrl+End channel up / down; Ctrl+Home opens the rig's
// panel (http://127.0.0.1:8095/): frequency, volume, squelch, mic gain, roger beep, the public channels, who is on air.
//
// Tuning is by frequency, like a real rig (the user, 2026-11-21: public channels only, no privacy codes, tune your own
// frequency): 25.615-28.305 MHz in 5 kHz steps, the 40 channels being where the band plan puts them. A station 5 kHz
// off comes through weak and garbled (AM splatter), further off nothing. On foot the radio is the CB Handheld item
// (qxZap_CBRadio_P, sold at the tech store), and it works while it is in your hand (MTCharacter.Net_HoldingItem):
// the player's own inventory is not readable, the held item is.
//
// Two rigs (the user, 2026-11-21): the vehicle's CB and the handheld, each on its own frequency, each switched on or
// off, both heard at once. The CB stays in its vehicle: out of the cab you hear it from the vehicle, fading out by
// 40 m. You transmit on the CB, the handheld or both (in the cab); on foot on the handheld. The handheld counts as
// carried once it has been seen in the hand this session (only the held item is readable, and driving hands are on
// the wheel).
//
// Service channels (the user, 2026-11-21): police, fire, ambulance, towing and taxi channels -- the common ones and
// every town's -- are for those services. Anyone may listen (a scanner); talking takes the role: the CB its vehicle's
// (MTVehicle's Police/FireFighting/Ambulance/Wrecker/Taxi component), the ham radio its wearer's uniform (the
// character's costume: police, medic, firefighter).
//
// Range goes with the rig (the user, 2026-11-21): the handheld 3 km; a vehicle by its weight, 10 km up to 2 t, 15 to
// 5 t, 20 to 12 t, 25 above; a boat 20 km, a ship (over 50 t) 60 km so ships talk across the map. A link reaches as far
// as the SENDER's rig: every packet carries its sender's range (a ship's 1000 km over is heard by a car 500 km away;
// the car answers only from within its own 10 km).
//
// Transport: Steam's ISteamNetworkingMessages, peer to peer by Steam id, on a channel of its own (the game's net driver
// uses Steam *sockets*, not messages, so nothing of the game's is touched). Every few seconds a hello goes to every
// player in the session (their Steam id is read off their PlayerState); whoever answers has the plugin. Voice is 8 kHz
// mu-law, 20 ms frames, 180 bytes a frame, sent only to plugin players within range, with the sender's map position.
// The listener does the radio: the signal strength comes from the distance, and with it hiss, crackle, fading and the
// squelch chattering at the edge of range. Kerchunk on key-up, roger beep and squelch tail on release, a heterodyne
// whistle when two talk at once, half duplex (you do not hear while you talk). tools/cb_radio/preview.py is the same
// chain in Python, for sound previews.
//
// The rig belongs to the vehicle (the user, 2026-11-21): in a vehicle, as driver or passenger, the channel and code are
// that vehicle's (by Net_VehicleId, the same number on every client), kept in [Vehicles] and announced in every hello,
// so whoever rides along listens to the vehicle's radio and a change from any seat reaches the others. On foot the
// handheld's own [Radio] Channel/Code come back. Talking also holds the game's own voice key (G, its push-to-talk:
// the green 'transmitting' line and proximity voice; [Keys] GameVoice, [Radio] GameVoice=1), so the people in the cab hear you in person -- and get you only that way: the CB
// stays quiet for someone in the same vehicle.
//
// Settings: plugins\cb_radio.ini, written with its defaults on first launch (the panel keeps [Radio] up to date):
//   [General] Disabled=1                    the plugin starts nothing
//   [Keys]    Talk=Home  ChannelUp=End  ChannelDown=Ctrl+End  Panel=Ctrl+Home   a key name, Ctrl+/Shift+/Alt+ in front,
//             or a Windows key code (36); GameVoice=G, the game's own voice key held while you talk (empty = none)
//   [Panel]   Port=8095                     the panel's port (the next free one if it is taken); InGame=1 shows it
//             inside the game (the leaderboard's web view), 0 in the outside browser;
//             Width=72 Height=88: the in-game window, percent of the screen
//   [Range]   Handheld=3 UpTo2t=10 UpTo5t=15 UpTo12t=20 Heavy=25 Boat=30 Ship=100 (tugs) BigShip=1000 (tankers,
//             container ships, ferries)   km, how far this rig is heard
//   [Radio]   Frequency (kHz), Volume, Squelch, MicGain, RogerBeep, GameVoice, Transmit (0 CB, 1 handheld, 2 both),
//             CBOn, HandheldOn, MicDevice, SpeakerDevice
//   [Vehicles] each vehicle's CB frequency, by vehicle id

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include "../include/mt_plugin.hpp"

using namespace mt;

static const wchar_t* INI = L"cb_radio.ini";
static const int SR = 8000, FRAME = 160, STEAM_CHANNEL = 27185;  // 27.185 MHz: channel 19
static const uint32_t MAGIC = 0x32524243;                        // "CBR2": packets carry the sender's range

// The 40 US/EU CB channels (MHz). 23 sits between 25 and 26: that is the real band plan.
static const char* FREQ[41] = {"", "26.965", "26.975", "26.985", "27.005", "27.015", "27.025", "27.035", "27.055", "27.065",
    "27.075", "27.085", "27.105", "27.115", "27.125", "27.135", "27.155", "27.165", "27.175", "27.185", "27.205", "27.215",
    "27.225", "27.255", "27.235", "27.245", "27.265", "27.275", "27.285", "27.295", "27.305", "27.315", "27.325", "27.335",
    "27.345", "27.355", "27.365", "27.375", "27.385", "27.395", "27.405"};

// ---- settings ----
static std::mutex mu;  // everything below that two threads touch
static const int KHZ_MIN = 25615, KHZ_MAX = 28305;  // export-radio coverage; 5 kHz steps
static int volume = 80, squelchPct = 8, micGain = 100, roger = 1, parrot = 0, parrotS = 60;
static bool holding;  // on foot with the CB Handheld in the hand (under mu)
static int chKhz(int ch) { return ch >= 1 && ch <= 40 ? (int)lround(atof(FREQ[ch]) * 1000) : 0; }
static int chOf(int k) { for (int c = 1; c <= 40; c++) if (chKhz(c) == k) return c; return 0; }
static int snapKhz(int k) { k = (k + 2) / 5 * 5; return k < KHZ_MIN ? KHZ_MIN : k > KHZ_MAX ? KHZ_MAX : k; }
enum { CB = 0, HT = 1 };
enum { R_POLICE = 1, R_FIRE = 2, R_AMBULANCE = 4, R_TOW = 8, R_TAXI = 16 };
static int roles[2];  // under mu: what each rig may talk on -- the CB by its vehicle, the ham radio by the uniform
static int roleOf(int k);  // the role a frequency needs, 0 for an open one
struct Rig {
    int khz = 27185; bool on = true, avail = false;
    double range = 0, x = 0, y = 0;  // km; where its antenna is (cm, map)
    float hear = 1;                  // how loud its speaker is where we stand: 1 in the cab or in the hand
};
static Rig rig[2];        // under mu
static int txMode = 0;    // in the cab: 0 the CB, 1 the handheld, 2 both. On foot always the handheld
static bool carried;      // the handheld has been in the hand once ([Radio] HaveHandheld keeps it across sessions)
static int panelMode = 0; // the panel: 0 detailed, 1 simple ([Panel] Mode)
static std::atomic<int> myRk{0};  // km, the biggest rig we have (a hello's range)
static std::map<uint64_t, int> peerRk;  // under mu: each plugin player's biggest range, from its hellos
static std::map<int64_t, float> vehMass;  // under mu: kg by Net_VehicleId, read on the game thread
static double rHand = 3, r2t = 10, r5t = 15, r12t = 20, rHeavy = 25, rBoat = 30, rShip = 100, rBigShip = 1000;  // [Range], km
static double rangeFor(uintptr_t veh, float kg) {
    if (isBoatVehicle(veh)) {  // by what it is (a boat's body often reports no mass) and by mass, the bigger of the two
        std::string n = objName(classOf(veh));
        for (auto& c : n) c = (char)tolower((unsigned char)c);
        auto has = [&](const char* w) { return n.find(w) != std::string::npos; };
        double byName = has("tanker") || has("containership") || has("cargoship") || has("ship_") || has("ferry") ? rBigShip
                        : has("tug") ? rShip : rBoat;
        double byMass = kg > 1000000 ? rBigShip : kg > 50000 ? rShip : kg > 0 ? rBoat : 0;
        return fmax(byName, byMass);
    }
    return kg <= 0 ? r2t : kg <= 2000 ? r2t : kg <= 5000 ? r5t : kg <= 12000 ? r12t : rHeavy;
}
static int inGamePanel = 1;  // [Panel] InGame
static double panelW = 72, panelH = 88;  // [Panel] Width / Height: the in-game window, percent of the screen
struct Hotkey { int vk = 0; bool ctrl = false, shift = false, alt = false; };
static std::string keyNames[4];  // as the ini spells them, for the panel
static Hotkey kTalk, kUp, kDown, kPanel;
static int kVoice = 'G';  // the game's push-to-talk, pressed for us (no modifiers); 0 = ToggleSpeaking instead
// "Ctrl+End", "Home", "F5", "G", "Numpad3" or a Windows key code ("36"); vk 0 if it is none of those
static Hotkey parseKey(std::wstring s) {
    Hotkey k;
    for (auto& c : s) c = (wchar_t)towupper(c);
    for (;;) {
        size_t plus = s.find(L'+');
        if (plus == std::wstring::npos || plus + 1 >= s.size()) break;
        std::wstring m = s.substr(0, plus);
        while (!m.empty() && m.back() == L' ') m.pop_back();
        if (m == L"CTRL" || m == L"CONTROL") k.ctrl = true; else if (m == L"SHIFT") k.shift = true; else if (m == L"ALT") k.alt = true;
        s = s.substr(plus + 1);
    }
    while (!s.empty() && s.front() == L' ') s.erase(0, 1);
    while (!s.empty() && s.back() == L' ') s.pop_back();
    static const struct { const wchar_t* n; int vk; } names[] = {
        {L"HOME", VK_HOME}, {L"END", VK_END}, {L"INSERT", VK_INSERT}, {L"INS", VK_INSERT}, {L"DELETE", VK_DELETE}, {L"DEL", VK_DELETE},
        {L"PAGEUP", VK_PRIOR}, {L"PAGEDOWN", VK_NEXT}, {L"UP", VK_UP}, {L"DOWN", VK_DOWN}, {L"LEFT", VK_LEFT}, {L"RIGHT", VK_RIGHT},
        {L"SPACE", VK_SPACE}, {L"TAB", VK_TAB}, {L"CAPSLOCK", VK_CAPITAL}, {L"PAUSE", VK_PAUSE}, {L"SCROLLLOCK", VK_SCROLL},
        {L"MOUSE4", VK_XBUTTON1}, {L"MOUSE5", VK_XBUTTON2}, {L"MIDDLEMOUSE", VK_MBUTTON}, {L"BACKSPACE", VK_BACK}, {L"ENTER", VK_RETURN}};
    for (auto& n : names) if (s == n.n) { k.vk = n.vk; return k; }
    if (s.size() >= 2 && s[0] == L'F' && iswdigit(s[1])) { int f = _wtoi(s.c_str() + 1); if (f >= 1 && f <= 24) k.vk = VK_F1 + f - 1; return k; }
    if (s.rfind(L"NUMPAD", 0) == 0 && s.size() == 7 && iswdigit(s[6])) { k.vk = VK_NUMPAD0 + (s[6] - L'0'); return k; }
    if (s.size() == 1 && (iswalpha(s[0]) || iswdigit(s[0]))) { k.vk = s[0]; return k; }
    if (!s.empty() && std::all_of(s.begin(), s.end(), [](wchar_t c) { return iswdigit(c) != 0; })) { k.vk = _wtoi(s.c_str()); if (k.vk > 254) k.vk = 0; }
    return k;
}
// held now, with exactly its modifiers (so Home is not Ctrl+Home)
static bool held(const Hotkey& k) {
    if (!k.vk || !(GetAsyncKeyState(k.vk) & 0x8000)) return false;
    auto on = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    // a modifier the key names must be down; Ctrl and Alt it does not name must be up (Home is not Ctrl+Home). Shift
    // only counts when named: it is the run key, and holding it cut the talk key off mid-over (11-21)
    return on(VK_CONTROL) == k.ctrl && on(VK_MENU) == k.alt && (!k.shift || on(VK_SHIFT));
}
// a [Keys] entry: its default declared by name, an old bare key code from before still read
static Hotkey keyFrom(const wchar_t* name, const wchar_t* def) {
    iniSet(INI, L"Keys", name, def);
    std::wstring v = iniGet(INI, L"Keys", name);
    Hotkey k = parseKey(v.empty() ? def : v);
    if (!k.vk) { MT_LOG("[Keys] %ls=%ls is not a key -- using %ls", name, v.c_str(), def); k = parseKey(def); }
    return k;
}
static std::wstring micDev, spkDev;  // devices by name ("" = the system's default)
static std::atomic<bool> reopenOut{false};
static int footKhz = 27185, gameVoice = 1;  // the handheld's own frequency; key the game's voice too
static int64_t myVeh;                                // Net_VehicleId of the vehicle we sit in, 0 on foot (under mu)
struct VehRadio { int khz; uint32_t ver; };
static std::map<int64_t, VehRadio> vehRadio;          // under mu; [Vehicles] id=khz,ver
static std::map<uint64_t, int64_t> peerVeh;           // under mu: the vehicle each plugin player sits in
static std::atomic<bool> helloNow{false};
static void saveVeh(int64_t id, const VehRadio& r) {
    wchar_t k[24], v[32]; swprintf(k, 24, L"%lld", (long long)id); swprintf(v, 32, L"%d,%u", r.khz, r.ver);
    iniSet(L"cb_radio.ini", L"Vehicles", k, v, true);
}
static bool loadVeh(int64_t id, VehRadio& r) {
    wchar_t k[24]; swprintf(k, 24, L"%lld", (long long)id);
    std::wstring v = iniGet(L"cb_radio.ini", L"Vehicles", k);
    int a = 0, b = 0; unsigned c = 0;
    int n = swscanf(v.c_str(), L"%d,%d,%u", &a, &b, &c);
    if (n == 3) { r.khz = chKhz(a); r.ver = c; return r.khz != 0; }  // before frequencies: ch,code,ver
    if (n == 2 && a >= KHZ_MIN && a <= KHZ_MAX) { r.khz = snapKhz(a); r.ver = (uint32_t)b; return true; }
    return false;
}
static std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &o[0], n, nullptr, nullptr); return o;
}
static std::wstring fromUrl(const std::string& v) {  // %XX / + -> UTF-8 bytes -> wide
    std::string b;
    for (size_t i = 0; i < v.size() && b.size() < 2000; i++) {
        if (v[i] == '%' && i + 2 < v.size()) { b += (char)strtol(v.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else b += v[i] == '+' ? ' ' : v[i];
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, b.c_str(), (int)b.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, b.c_str(), (int)b.size(), &w[0], n); return w;
}
// waveIn / waveOut device names (Windows cuts them at 31 characters) and the id of the one picked.
static std::vector<std::wstring> devices(bool in) {
    std::vector<std::wstring> o;
    for (UINT i = 0, n = in ? waveInGetNumDevs() : waveOutGetNumDevs(); i < n; i++) {
        if (in) { WAVEINCAPSW c; if (waveInGetDevCapsW(i, &c, sizeof c) == MMSYSERR_NOERROR) o.push_back(c.szPname); }
        else { WAVEOUTCAPSW c; if (waveOutGetDevCapsW(i, &c, sizeof c) == MMSYSERR_NOERROR) o.push_back(c.szPname); }
    }
    return o;
}
static UINT deviceId(bool in, const std::wstring& name) {
    if (name.empty()) return WAVE_MAPPER;
    auto all = devices(in);
    for (UINT i = 0; i < all.size(); i++) if (all[i] == name) return i;
    return WAVE_MAPPER;  // unplugged: the default
}
static void saveSettings() {
    auto put = [](const wchar_t* k, int v) { wchar_t b[16]; swprintf(b, 16, L"%d", v); iniSet(INI, L"Radio", k, b, true); };
    put(L"Frequency", footKhz); put(L"GameVoice", gameVoice); put(L"Volume", volume); put(L"Squelch", squelchPct); put(L"MicGain", micGain);
    put(L"RogerBeep", roger); put(L"HaveHandheld", carried); put(L"Transmit", txMode);
    { wchar_t b[8]; swprintf(b, 8, L"%d", panelMode); iniSet(INI, L"Panel", L"Mode", b, true); } put(L"CBOn", rig[CB].on); put(L"HandheldOn", rig[HT].on);
    iniSet(INI, L"Radio", L"MicDevice", micDev.c_str(), true);
    iniSet(INI, L"Radio", L"SpeakerDevice", spkDev.c_str(), true);
}
static void loadSettings() {
    int ch = iniInt(INI, L"Radio", L"Channel", 19);  // before frequencies
    footKhz = snapKhz(iniInt(INI, L"Radio", L"Frequency", chKhz(ch) ? chKhz(ch) : 27185));
    rig[HT].khz = rig[CB].khz = footKhz; gameVoice = iniInt(INI, L"Radio", L"GameVoice", 1);
    txMode = iniInt(INI, L"Radio", L"Transmit", 0) % 3;
    carried = iniInt(INI, L"Radio", L"HaveHandheld", 0) != 0;
    panelMode = iniInt(INI, L"Panel", L"Mode", 0) ? 1 : 0;
    rig[CB].on = iniInt(INI, L"Radio", L"CBOn", 1) != 0; rig[HT].on = iniInt(INI, L"Radio", L"HandheldOn", 1) != 0;
    volume = iniInt(INI, L"Radio", L"Volume", 80); squelchPct = iniInt(INI, L"Radio", L"Squelch", 8);
    micGain = iniInt(INI, L"Radio", L"MicGain", 100); roger = iniInt(INI, L"Radio", L"RogerBeep", 1);
    iniSet(INI, L"Radio", L"MicDevice", L""); iniSet(INI, L"Radio", L"SpeakerDevice", L"");
    micDev = iniGet(INI, L"Radio", L"MicDevice"); spkDev = iniGet(INI, L"Radio", L"SpeakerDevice");
    // keys by name; an old file's bare codes (Talk=36) still read. The old Channel / Panel codes went with Ctrl.
    std::wstring oldCh = iniGet(INI, L"Keys", L"Channel");
    if (!oldCh.empty() && iniGet(INI, L"Keys", L"ChannelUp").empty()) iniSet(INI, L"Keys", L"ChannelUp", oldCh.c_str(), true);
    if (iniGet(INI, L"Keys", L"Panel") == L"36") iniSet(INI, L"Keys", L"Panel", L"Ctrl+Home", true);
    if (iniGet(INI, L"Keys", L"Talk") == L"36") iniSet(INI, L"Keys", L"Talk", L"Home", true);
    if (iniGet(INI, L"Keys", L"ChannelUp") == L"35") iniSet(INI, L"Keys", L"ChannelUp", L"End", true);
    if (iniGet(INI, L"Keys", L"GameVoice") == L"71") iniSet(INI, L"Keys", L"GameVoice", L"G", true);
    kTalk = keyFrom(L"Talk", L"Home"); kUp = keyFrom(L"ChannelUp", L"End"); kDown = keyFrom(L"ChannelDown", L"Ctrl+End");
    kPanel = keyFrom(L"Panel", L"Ctrl+Home");
    for (auto [k, out] : {std::pair<const wchar_t*, std::string*>{L"Talk", &keyNames[0]}, {L"ChannelUp", &keyNames[1]}, {L"ChannelDown", &keyNames[2]}, {L"Panel", &keyNames[3]}})
        *out = utf8(iniGet(INI, L"Keys", k));
    iniSet(INI, L"Keys", L"GameVoice", L"G");
    { std::wstring gv = iniGet(INI, L"Keys", L"GameVoice"); kVoice = gv.empty() ? 0 : parseKey(gv).vk; }
    iniInt(INI, L"Panel", L"Port", 8095);  // declared now, so the file shows it from the first launch
    inGamePanel = iniInt(INI, L"Panel", L"InGame", 1) ? 1 : 0;  // 0: the panel key opens the outside browser
    panelW = iniNum(INI, L"Panel", L"Width", 72); panelH = iniNum(INI, L"Panel", L"Height", 88);
    panelW = panelW < 30 ? 30 : panelW > 100 ? 100 : panelW; panelH = panelH < 30 ? 30 : panelH > 100 ? 100 : panelH;
    rHand = iniNum(INI, L"Range", L"Handheld", 3); r2t = iniNum(INI, L"Range", L"UpTo2t", 10); r5t = iniNum(INI, L"Range", L"UpTo5t", 15);
    r12t = iniNum(INI, L"Range", L"UpTo12t", 20); rHeavy = iniNum(INI, L"Range", L"Heavy", 25);
    if (iniGet(INI, L"Range", L"Boat") == L"20") iniSet(INI, L"Range", L"Boat", L"30", true);   // the old defaults move up
    if (iniGet(INI, L"Range", L"Ship") == L"60") iniSet(INI, L"Range", L"Ship", L"100", true);
    rBoat = iniNum(INI, L"Range", L"Boat", 30); rShip = iniNum(INI, L"Range", L"Ship", 100); rBigShip = iniNum(INI, L"Range", L"BigShip", 1000);
    for (double* r : {&rHand, &r2t, &r5t, &r12t, &rHeavy, &rBoat, &rShip, &rBigShip}) *r = *r < 0.5 ? 0.5 : *r > 5000 ? 5000 : *r;  // km
    // settings from before frequencies and two radios: gone, so the file says only what is read
    std::wstring path = iniPath(INI);
    for (const wchar_t* k : {L"Channel", L"Code", L"Handheld", L"RangeKm", L"Presets"}) WritePrivateProfileStringW(L"Radio", k, nullptr, path.c_str());
    WritePrivateProfileStringW(L"Keys", L"Channel", nullptr, path.c_str());
}
// [Debug]: LoopbackKm (>= 0: everything we send also goes to our own Steam id and comes back as a station that far off), TalkFile (an 8 kHz mono
// 16-bit wav spoken instead of the mic), RecordOut (the speaker's output appended to a raw s16le 8 kHz file).
static double loopKm = -1;
static std::vector<int16_t> talkFile;
static size_t talkAt;
static FILE* recOut;
static void loadDebug() {
    loopKm = iniNum(INI, L"Debug", L"LoopbackKm", -1);
    std::wstring tf = iniGet(INI, L"Debug", L"TalkFile"), ro = iniGet(INI, L"Debug", L"RecordOut");
    if (!tf.empty()) if (FILE* f = _wfopen(tf.c_str(), L"rb")) {
        std::vector<char> b; char t[4096]; size_t n;
        while ((n = fread(t, 1, sizeof t, f)) > 0) b.insert(b.end(), t, t + n);
        fclose(f);
        for (size_t i = 12; i + 8 <= b.size();) {  // the wav's "data" chunk
            uint32_t len; memcpy(&len, &b[i + 4], 4);
            if (!memcmp(&b[i], "data", 4)) { size_t k = (std::min)((size_t)len, b.size() - i - 8) / 2; talkFile.resize(k); memcpy(talkFile.data(), &b[i + 8], k * 2); break; }
            i += 8 + len + (len & 1);
        }
    }
    if (!ro.empty()) recOut = _wfopen(ro.c_str(), L"ab");
    if (loopKm >= 0 || !talkFile.empty() || recOut) MT_LOG("debug: loopback %.1f km, talk file %d samples, record %s", loopKm, (int)talkFile.size(), recOut ? "on" : "off");
}

// ---- the world: who is here, where we are (read off the game's objects, no game thread needed) ----
struct Player { uint64_t sid; std::string name; };
static std::vector<Player> players;  // everyone in the session but us
static uint64_t mySid;
static std::string myName;
static double myX, myY;
static bool inVehicle;
static std::atomic<bool> chatOpen{false};  // typing in the game's chat: Home / End are the text cursor's then
static uintptr_t cChat, gChat, fHasFocus; static int oChatText = -1;
static uintptr_t cPS, cPC, cVeh, fSpeak;
static int oVehId = -1, oSeats = -1, oSeatChar = -1, seatStride = 0, oHolding = -1, oHoldKey = -1, oMassRet = -1;
static int oVehRole[5] = {-1, -1, -1, -1, -1}, oEquip = -1, oEquipSlots = -1, oEqSlot = -1, oEqKey = -1, eqStride = 0;
static int oUid = -1, oName = -1, oPSLoc = -1, oCtrlPS = -1, oPawn = -1, oRoot = -1, oRelLoc = -1;
static uintptr_t fGetMass, massOf;
static int64_t massId;
static void readMass(void*) {  // game thread: the vehicle body's mass (PrimitiveComponent.GetMass on its root)
    uintptr_t v = massOf, root = v && oRoot >= 0 ? get<uintptr_t>(v + oRoot) : 0;
    if (!root || !fGetMass || oMassRet < 0) return;
    unsigned char p[16] = {};
    api->process_event(root, fGetMass, p);
    float kg; memcpy(&kg, p + oMassRet, 4);
    std::lock_guard<std::mutex> g(mu);
    if (kg > 0 && kg < 1e7f) vehMass[massId] = kg;
    MT_LOG("vehicle %lld: %.0f kg", (long long)massId, kg);
}
static uintptr_t cChar;

static std::string fstr(uintptr_t at) {
    struct { uintptr_t d; int n, m; } s{};
    if (!rd(at, &s, sizeof s) || !s.d || s.n <= 0 || s.n > 128) return "";
    wchar_t w[128]; if (!rd(s.d, w, s.n * 2)) return "";
    std::string o; for (int i = 0; i < s.n && w[i]; i++) o += w[i] < 128 ? (char)w[i] : '?';
    return o;
}
// FUniqueNetIdRepl -> the Steam id it wraps: a shared pointer to an id object holding the 64-bit id. Found by its shape
// (individual account ids are 0x01100001xxxxxxxx), and checked against SteamUser's own id for the local player.
static uint64_t steamIdOf(uintptr_t ps) {
    for (int i = 0; i < 6; i++) {
        uint64_t v = get<uint64_t>(ps + oUid + i * 8);
        if ((v >> 32) == 0x01100001) return v;
        if (v < 0x10000 || v > 0x7FFFFFFFFFFF) continue;
        for (int j = 0; j < 8; j++) { uint64_t w = get<uint64_t>(v + j * 8); if ((w >> 32) == 0x01100001) return w; }
    }
    return 0;
}
// ---- on-screen notices: the game's own system message (MotorTownPlayerController.ClientShowSystemMessage, a client
// RPC, so on the owning client it simply runs). Queued from any thread, shown from the game thread.
static uintptr_t gPC, fSysMsg, fToText, textCDO;
static int pSysMsg = -1, pTextIn = -1, pTextRet = -1;
static std::vector<std::wstring> notices;  // under mu
static void notice(const std::string& m) { notices.push_back(std::wstring(m.begin(), m.end())); }
// Is the chat's text box taking the keyboard? (The input row stays visible either way: checked 11-21.) Game thread.
static void checkChat(void*) {
    uintptr_t box = gChat && oChatText >= 0 ? get<uintptr_t>(gChat + oChatText) : 0;
    bool open = false;
    static uintptr_t cBox = api->find_object("Class", "EditableTextBox", "/Script/UMG");
    // only a live-looking pair: the chat widget is rescanned every 3 s, and both still carry their classes
    static uintptr_t fDesc = api->find_object("Function", "HasFocusedDescendants", "Widget"),
                     fAny = api->find_object("Function", "HasAnyUserFocus", "Widget");
    static uintptr_t toldChat;
    if (gChat != toldChat) { toldChat = gChat; MT_LOG("chat widget %p, text box %p (%s)", (void*)gChat, (void*)box, box ? objName(classOf(box)).c_str() : "-"); }
    if (box && fHasFocus && cBox && api->is_a(classOf(gChat), cChat) && api->is_a(classOf(box), cBox) && !isCDO(box)) {
        struct { bool ret; } a{}, b{}, c{};
        api->process_event(box, fHasFocus, &a);
        if (fDesc) api->process_event(box, fDesc, &b);
        if (fAny) api->process_event(box, fAny, &c);
        open = a.ret || b.ret || c.ret;
        static int was = -1, n;
        int now = a.ret | b.ret << 1 | c.ret << 2;
        if (now != was && n++ < 30) { was = now; MT_LOG("chat focus: keyboard %d, descendants %d, any user %d", a.ret, b.ret, c.ret); }
    }
    static int told;
    if (open != chatOpen && told < 20) { told++; MT_LOG("chat %s", open ? "open: CB keys off while typing" : "closed"); }
    chatOpen = open;
}
static void showNotices(void*) {
    std::vector<std::wstring> q;
    { std::lock_guard<std::mutex> g(mu); q.swap(notices); }
    for (auto& w : q) {
        if (!gPC || !fSysMsg || !fToText || pSysMsg < 0 || pTextIn < 0 || pTextRet < 0) return;
        unsigned char a[0x40] = {0}, b[0x40] = {0};
        struct { const wchar_t* d; int n, m; } in = {w.c_str(), (int)w.size() + 1, (int)w.size() + 1};
        memcpy(a + pTextIn, &in, sizeof in);
        api->process_event(textCDO, fToText, a);
        memcpy(b + pSysMsg, a + pTextRet, 16);  // FText is 16 bytes in this build (jmap); it moves over as it is, a reference leaked at worst
        api->process_event(gPC, fSysMsg, b);
    }
}
static std::string presetName(int ch);
static std::string townName(int k);
static std::string label(int k) {  // "CB 19  27.185 MHz  -  Truckers", or "27.190 MHz" between channels
    char b[32]; snprintf(b, sizeof b, "%d.%03d MHz", k / 1000, k % 1000);
    int c = chOf(k);
    if (c) return "CB " + std::to_string(c) + "  " + b + presetName(c);
    std::string n = townName(k);
    return std::string("CB  ") + b + (n.empty() ? "" : "  -  " + n);
}

static bool resolveWorld() {
    fSysMsg = api->find_object("Function", "ClientShowSystemMessage", "MotorTownPlayerController");
    fToText = api->find_object("Function", "Conv_StringToText", "KismetTextLibrary");
    textCDO = api->find_object("KismetTextLibrary", "Default__KismetTextLibrary", "/Script/Engine");
    if (fSysMsg) pSysMsg = off(fSysMsg, "Message");
    if (fToText) { pTextIn = off(fToText, "InString"); pTextRet = off(fToText, "ReturnValue"); }
    if (!fSysMsg || !fToText || !textCDO || pSysMsg < 0 || pTextIn < 0 || pTextRet < 0) MT_LOG("system message calls not found: no on-screen notices");
    cPS = api->find_object("Class", "MotorTownPlayerState", "/Script/MotorTown");
    cPC = api->find_object("Class", "MotorTownPlayerController", "/Script/MotorTown");
    cVeh = api->find_object("Class", "MTVehicle", "/Script/MotorTown");
    fSpeak = api->find_object("Function", "ToggleSpeaking", "PlayerController");
    cChar = api->find_object("Class", "MTCharacter", "/Script/MotorTown");
    fGetMass = api->find_object("Function", "GetMass", "PrimitiveComponent");
    if (fGetMass) oMassRet = off(fGetMass, "ReturnValue");
    if (!fGetMass || oMassRet < 0) MT_LOG("GetMass not found: every vehicle gets the car range");
    if (cChar) oHolding = off(cChar, "Net_HoldingItem");
    if (uintptr_t st = api->find_object("ScriptStruct", "MTHoldingItem", "/Script/MotorTown")) oHoldKey = off(st, "ItemKey");
    if (oHolding < 0 || oHoldKey < 0) MT_LOG("held item not found: no handheld on foot");
    if (cVeh) {
        const char* comps[5] = {"PoliceComponent", "FireFightingComponent", "AmbulanceComponent", "WreckerComponent", "TaxiComponent"};
        for (int i = 0; i < 5; i++) oVehRole[i] = off(cVeh, comps[i]);
    }
    if (cChar) oEquip = off(cChar, "Net_EquipmentInventory");
    if (uintptr_t st = api->find_object("ScriptStruct", "MTEquipmentInventory", "/Script/MotorTown")) oEquipSlots = off(st, "EquipmentSlots");
    if (uintptr_t st = api->find_object("ScriptStruct", "MTEquipmentInventorySlot", "/Script/MotorTown")) {
        oEqSlot = off(st, "Slot"); oEqKey = off(st, "ItemKey"); if (oEqKey >= 0) eqStride = (oEqKey + 8 + 3) & ~3;
    }
    if (oEquip < 0 || oEquipSlots < 0 || oEqSlot < 0 || oEqKey < 0) MT_LOG("costume slot not found: the ham radio has no service roles");
    if (cVeh) { oVehId = off(cVeh, "Net_VehicleId"); oSeats = off(cVeh, "Net_Seats"); }
    if (uintptr_t st = api->find_object("ScriptStruct", "MTNetVehicleSeat", "/Script/MotorTown")) {
        oSeatChar = off(st, "Character"); int t = off(st, "SeatType"); if (t >= 0) seatStride = (t + 1 + 7) & ~7;
    }
    if (oVehId < 0 || oSeats < 0 || oSeatChar < 0 || !seatStride) MT_LOG("vehicle id / seats not found: the radio stays yours, not the vehicle's");
    if (!fSpeak) MT_LOG("ToggleSpeaking not found: talking does not key the game's voice chat");
    uintptr_t cBasePS = api->find_object("Class", "PlayerState", "/Script/Engine");
    uintptr_t cCtrl = api->find_object("Class", "Controller", "/Script/Engine");
    uintptr_t cActor = api->find_object("Class", "Actor", "/Script/Engine");
    uintptr_t cScene = api->find_object("Class", "SceneComponent", "/Script/Engine");
    if (cBasePS) { oUid = off(cBasePS, "UniqueID"); oName = off(cBasePS, "PlayerNamePrivate"); }
    if (cPS) oPSLoc = off(cPS, "Location");
    if (cCtrl) { oCtrlPS = off(cCtrl, "PlayerState"); oPawn = off(cCtrl, "Pawn"); }
    if (cActor) oRoot = off(cActor, "RootComponent");
    if (cScene) oRelLoc = off(cScene, "RelativeLocation");
    cChat = api->find_object("Class", "ChatWidget", "/Script/MotorTown");
    if (cChat) oChatText = off(cChat, "ChatTextBox");
    fHasFocus = api->find_object("Function", "HasKeyboardFocus", "Widget");
    if (oChatText < 0 || !fHasFocus) MT_LOG("chat box not found: Home / End also act while typing in chat");
    bool ok = cPS && cPC && cVeh && oUid >= 0 && oName >= 0 && oPSLoc >= 0 && oCtrlPS >= 0 && oPawn >= 0 && oRoot >= 0 && oRelLoc >= 0;
    MT_LOG("world fields %s (uid+%x name+%x loc+%x)", ok ? "ok" : "MISSING", oUid, oName, oPSLoc);
    return ok;
}
static void scanWorld() {
    static std::vector<uintptr_t> states, vehicles;
    static uintptr_t pc, chat;
    static int pcIdx = -1;
    static DWORD lastFull;
    if (GetTickCount() - lastFull > 3000) {  // the full object walk, every 3 s; positions every pass
        lastFull = GetTickCount();
        states.clear(); vehicles.clear(); pc = 0; chat = 0;
        for (int i = 0, n = api->num_objects(); i < n; i++) {
            uintptr_t o = api->object(i);
            if (!o || isCDO(o)) continue;
            uintptr_t c = classOf(o);
            if (c == cPS || (cPS && api->is_a(c, cPS))) states.push_back(o);
            else if (cVeh && api->is_a(c, cVeh)) vehicles.push_back(o);
            else if (!pc && (c == cPC || api->is_a(c, cPC))) { pc = o; pcIdx = i; }
            else if (!chat && cChat && api->is_a(c, cChat) && oChatText >= 0 && get<uintptr_t>(o + oChatText)) chat = o;  // the menu's has no text box
        }
        std::vector<Player> found;
        uintptr_t mine = pc ? get<uintptr_t>(pc + oCtrlPS) : 0;
        for (uintptr_t s : states) {
            uint64_t sid = steamIdOf(s);
            if (s == mine) {
                std::lock_guard<std::mutex> g(mu); myName = fstr(s + oName);
                static uintptr_t told; if (told != s) { told = s; MT_LOG("own PlayerState: %s, id %llx (%s Steam's)", myName.c_str(), sid, sid == mySid ? "matches" : "DIFFERS FROM"); }
                continue;
            }
            if (sid && sid != mySid) found.push_back({sid, fstr(s + oName)});
        }
        std::lock_guard<std::mutex> g(mu);
        players.swap(found);
    }
    if (pc && (pcIdx < 0 || api->object(pcIdx) != pc)) { pc = 0; chat = 0; lastFull = 0; }  // gone: the world went
    gPC = pc;
    gChat = chat;
    if (!pc || classOf(pc) == 0) return;
    uintptr_t pawn = get<uintptr_t>(pc + oPawn), root = pawn ? get<uintptr_t>(pawn + oRoot) : 0;
    double p[3] = {0, 0, 0};
    bool veh = pawn && api->is_a(classOf(pawn), cVeh);
    if (!(root && rd(root + oRelLoc, p, sizeof p))) {
        uintptr_t ps = get<uintptr_t>(pc + oCtrlPS);
        if (ps) rd(ps + oPSLoc, p, sizeof p);
    }
    // a passenger's pawn is the character: find the vehicle whose seats hold it
    uintptr_t in = veh ? pawn : 0;
    if (!in && pawn && oSeats >= 0 && oSeatChar >= 0 && seatStride)
        for (uintptr_t v : vehicles) {
            struct { uintptr_t data; int num, max; } arr{};
            if (!rd(v + oSeats, &arr, sizeof arr) || arr.num <= 0 || arr.num > 16 || !arr.data) continue;
            for (int i = 0; i < arr.num && !in; i++) { uintptr_t ch = 0; if (rd(arr.data + i * seatStride + oSeatChar, &ch, 8) && ch == pawn) in = v; }
            if (in) break;
        }
    int64_t id = 0;
    if (in && oVehId >= 0) rd(in + oVehId, &id, 8);
    // on foot: the CB Handheld in the hand (its name enters the pool once the item table has loaded)
    static int cbName;
    if (!cbName) cbName = api->name_index("CB_Handheld");
    uint32_t held = 0;
    bool hold = !in && pawn && cbName && oHolding >= 0 && oHoldKey >= 0 && cChar && api->is_a(classOf(pawn), cChar)
                && rd(pawn + oHolding + oHoldKey, &held, 4) && held == (uint32_t)cbName;
    // the uniform: the costume slot of the character on foot (kept while driving: the pawn is the vehicle then)
    static int costume = -1;
    if (!in && pawn && cChar && oEquip >= 0 && oEquipSlots >= 0 && eqStride && api->is_a(classOf(pawn), cChar)) {
        struct { uintptr_t d; int num, max; } a{};
        int found = 0;
        if (rd(pawn + oEquip + oEquipSlots, &a, sizeof a) && a.num > 0 && a.num < 16 && a.d)
            for (int i = 0; i < a.num; i++) {
                uint8_t slot = 0; uint32_t key = 0;
                if (rd(a.d + i * eqStride + oEqSlot, &slot, 1) && slot == 4 && rd(a.d + i * eqStride + oEqKey, &key, 4)) found = (int)key;  // EMTEquipmentSlot::Costume
            }
        if (found != costume) { costume = found; char b[96] = "none"; if (found) api->name(found, b, sizeof b); MT_LOG("costume: %s", b); }
    }
    int htRoles = 0;
    if (costume > 0) {
        char b[96] = ""; api->name(costume, b, sizeof b);
        std::string c = b;
        if (c.rfind("Costume_Police", 0) == 0) htRoles |= R_POLICE;
        if (c.rfind("Costume_Medic", 0) == 0) htRoles |= R_AMBULANCE;
        if (c.rfind("Costume_FireFighter", 0) == 0) htRoles |= R_FIRE;
    }
    // the CB stays with the last vehicle we sat in: where it stands now, if it is still in the world
    static uintptr_t cbObj;
    static int64_t cbId;
    static int cbRoles;
    if (in && id) {
        int r = 0;
        for (int i = 0; i < 5; i++) if (oVehRole[i] >= 0 && get<uintptr_t>(in + oVehRole[i])) r |= 1 << i;  // the role components it carries
        if (cbObj != in || r != cbRoles) MT_LOG("vehicle roles: %s%s%s%s%s", r & R_POLICE ? "police " : "", r & R_FIRE ? "fire " : "",
                                                r & R_AMBULANCE ? "ambulance " : "", r & R_TOW ? "tow " : "", r & R_TAXI ? "taxi" : "");
        cbObj = in; cbId = id; cbRoles = r;
    }
    double vp[3] = {0, 0, 0};
    bool cbThere = false;
    if (cbObj && std::find(vehicles.begin(), vehicles.end(), cbObj) != vehicles.end()) {
        int64_t now = 0;
        uintptr_t vr = get<uintptr_t>(cbObj + oRoot);
        cbThere = rd(cbObj + oVehId, &now, 8) && now == cbId && vr && rd(vr + oRelLoc, vp, sizeof vp);
    }
    std::lock_guard<std::mutex> g(mu);
    if (hold != holding) MT_LOG("handheld %s", hold ? "in hand" : "put away");
    if (hold && !carried) { carried = true; saveSettings(); notice(label(rig[HT].khz) + "  (ham radio)"); }
    myX = p[0]; myY = p[1]; inVehicle = in != 0; holding = hold;
    roles[CB] = cbRoles; roles[HT] = htRoles;
    Rig& cb = rig[CB];
    if (cbId) {
        auto m = vehMass.find(cbId);
        if (m == vehMass.end() && in) { vehMass[cbId] = 0; massOf = in; massId = cbId; api->run_on_game_thread(readMass, nullptr); }
        double r = rangeFor(cbObj, m == vehMass.end() ? 0 : m->second);
        if (r != cb.range) MT_LOG("CB range %.0f km", r);
        cb.range = r; cb.avail = true;
        if (in) { cb.x = myX; cb.y = myY; cb.hear = 1; }
        else if (cbThere) {  // out of the cab: its speaker, heard from where we stand
            cb.x = vp[0]; cb.y = vp[1];
            double m2 = hypot(vp[0] - myX, vp[1] - myY) / 100;
            cb.hear = m2 >= 40 ? 0 : (float)pow(1 - m2 / 40, 1.5);
        } else cb.hear = 0;
    }
    Rig& ht = rig[HT];
    ht.avail = carried; ht.range = rHand; ht.x = myX; ht.y = myY; ht.hear = 1;
    myRk = (int)fmax(cb.avail ? cb.range : 0, ht.avail ? ht.range : 0);
    if (id != myVeh && (id || !in)) {  // got in, changed vehicle, or got out
        myVeh = id;
        if (id) {
            auto it = vehRadio.find(id);
            if (it == vehRadio.end()) { VehRadio r; if (!loadVeh(id, r)) r = {cb.khz, 0}; it = vehRadio.emplace(id, r).first; }
            cb.khz = it->second.khz;
            notice(label(cb.khz) + "  (this vehicle's CB)");
            helloNow = true;
        }
        MT_LOG("vehicle %lld (%s): CB on %d kHz", (long long)id, in ? objName(classOf(in)).c_str() : "-", cb.khz);
    }
}
// under mu: a rig was retuned. The CB's frequency is its vehicle's (and the passengers'), the handheld's is ours.
static void radioChanged(int r) {
    if (r == CB && myVeh) { VehRadio& v = vehRadio[myVeh]; v.khz = rig[CB].khz; v.ver++; saveVeh(myVeh, v); helloNow = true; }
    if (r == HT) footKhz = rig[HT].khz;
    saveSettings();
}
static int activeRig() { return inVehicle || !carried ? CB : HT; }  // what End and the in-game notices act on

// ---- Steam ----
struct Steam {
    void* nm = nullptr;
    int (*send)(void*, const void* id, const void* data, uint32_t n, int flags, int ch) = nullptr;
    int (*recv)(void*, int ch, uintptr_t* out, int max) = nullptr;
    bool (*accept)(void*, const void* id) = nullptr;
    void (*release)(uintptr_t) = nullptr;
    int (*pipe)() = nullptr;
} steam;
// The game shuts Steam down on exit long before the loader says stopping (5 s after the world goes); a call after that
// lands in unloaded Steam code, and the crash handler Steam left behind took the game down on every close (11-21,
// Windows: crashhandler64.dll_unloaded). Every call asks the pipe first; once it is gone the radio is off for good.
static volatile bool steamGone = false;
// The close crash, 11-21 (Windows: crashhandler64.dll_unloaded on every close): the net thread polls Steam every few ms,
// and while the game shuts Steam down its networking is freed before the pipe reads as gone -- one call in that gap
// faults, and the crash handler Steam unloaded turns any fault into a crash. The game takes its world down and closes
// its window BEFORE it shuts Steam down, so Steam is only called while both are there (netLive, set by the world scan).
static std::atomic<bool> netLive{false};
static bool steamOk() {
    if (steamGone || api->stopping() || !steam.pipe) return false;
    if (!netLive) return false;  // between worlds, or closing: not now (nothing is lost, the hellos find everyone again)
    if (!steam.pipe()) { steamGone = true; return false; }
    return true;
}
static HWND gameWindow() {  // the game's main window, found once; IsWindow tells when it closes
    static HWND h;
    if (h && IsWindow(h)) return h;
    struct F { static BOOL CALLBACK cb(HWND w, LPARAM out) {
        DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
        wchar_t cls[64] = L""; GetClassNameW(w, cls, 64);
        if (pid == GetCurrentProcessId() && !wcscmp(cls, L"UnrealWindow") && IsWindowVisible(w)) { *(HWND*)out = w; return FALSE; }
        return TRUE; } };
    HWND found = nullptr;
    EnumWindows(F::cb, (LPARAM)&found);
    if (found) h = found;
    return found;
}
static bool steamUp() {
    HMODULE m = GetModuleHandleW(L"steam_api64.dll");
    auto pipe = m ? (int (*)())GetProcAddress(m, "SteamAPI_GetHSteamPipe") : nullptr;
    if (!pipe || !pipe()) return false;  // asked before Steam is up, the accessors hand back nothing for good
    steam.pipe = pipe;
    auto nmFn = (void* (*)())GetProcAddress(m, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    auto userFn = (void* (*)())GetProcAddress(m, "SteamAPI_SteamUser_v021");
    auto idFn = (uint64_t (*)(void*))GetProcAddress(m, "SteamAPI_ISteamUser_GetSteamID");
    steam.send = (decltype(steam.send))GetProcAddress(m, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    steam.recv = (decltype(steam.recv))GetProcAddress(m, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    steam.accept = (decltype(steam.accept))GetProcAddress(m, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    steam.release = (decltype(steam.release))GetProcAddress(m, "SteamAPI_SteamNetworkingMessage_t_Release");
    if (!nmFn || !userFn || !idFn || !steam.send || !steam.recv || !steam.accept || !steam.release) return false;
    steam.nm = nmFn();
    void* user = userFn();
    mySid = user ? idFn(user) : 0;
    return steam.nm && mySid;
}
struct Identity { int type = 16, size = 8; uint64_t sid; char rest[120] = {0}; };  // SteamNetworkingIdentity, SteamID kind
static_assert(sizeof(Identity) == 136, "SteamNetworkingIdentity is 136 bytes");

#pragma pack(push, 1)
struct Packet { uint32_t magic; uint8_t type, flags; uint16_t seq; uint32_t key; float x, y; uint16_t rk; uint8_t ulaw[FRAME]; };
enum { F_REPLY = 1, F_ROGER = 2, F_HT = 4 };  // flags: a hello's answer; an over ended with the roger beep; sent from a handheld
#pragma pack(pop)
enum { P_HELLO = 1, P_VOICE = 2, P_END = 3 };
static const int HEAD = (int)offsetof(Packet, ulaw);
#pragma pack(push, 1)
struct HelloVeh { int64_t veh; uint16_t khz; uint32_t ver; };  // after a hello's head: the vehicle we sit in and its radio
#pragma pack(pop)
static_assert(sizeof(HelloVeh) == 14, "hello vehicle block is 14 bytes");
static const int HELLO = HEAD + (int)sizeof(HelloVeh);              // older plugins read only the head
static void fillHello(Packet& p) {  // under mu
    HelloVeh h{myVeh, 0, 0};  // the vehicle we sit in
    if (myVeh) { const VehRadio& r = vehRadio[myVeh]; h.khz = (uint16_t)r.khz; h.ver = r.ver; }
    memcpy(p.ulaw, &h, sizeof h);
}
static void sendTo(uint64_t sid, const Packet& p, int n) {
    if (!steamOk()) return;
    Identity id; id.sid = sid;
    Packet q = p; if (!q.rk) q.rk = (uint16_t)myRk.load();  // the sending rig's range (a hello: the biggest we have)
    steam.send(steam.nm, &id, &q, (uint32_t)n, 1 | 32, STEAM_CHANNEL);  // unreliable, no Nagle, restart broken sessions
}

// ---- sound ----
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    float run(float x) { float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    static Biquad make(float f, bool high) {  // RBJ, Q 0.707
        float w = 2 * 3.14159265f * f / SR, c = cosf(w), al = sinf(w) / (2 * 0.7071f), a0 = 1 + al;
        Biquad q;
        q.b0 = (high ? (1 + c) / 2 : (1 - c) / 2) / a0; q.b1 = (high ? -(1 + c) : (1 - c)) / a0; q.b2 = q.b0;
        q.a1 = -2 * c / a0; q.a2 = (1 - al) / a0;
        return q;
    }
};
struct Band {  // 300-2800 Hz, the voice of every CB
    Biquad h1 = Biquad::make(300, true), h2 = Biquad::make(300, true), l1 = Biquad::make(2800, false), l2 = Biquad::make(2800, false);
    float run(float x) { return l2.run(l1.run(h2.run(h1.run(x)))); }
};
static uint8_t ulawEnc(int16_t s) {
    int sign = s < 0 ? 0x80 : 0, m = s < 0 ? -(int)s : s;
    if (m > 32635) m = 32635;
    m += 0x84;
    int exp = 7;
    for (int mask = 0x4000; !(m & mask) && exp > 0; mask >>= 1) exp--;
    return (uint8_t)~(sign | (exp << 4) | ((m >> (exp + 3)) & 0x0F));
}
static int16_t ulawDec(uint8_t u) {
    u = ~u;
    int t = (((u & 0x0F) << 3) + 0x84) << ((u & 0x70) >> 4);
    return (int16_t)((u & 0x80) ? 0x84 - t : t - 0x84);
}
struct Rng {
    uint32_t s = 0x9E3779B9;
    float uni() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s >> 8) / 16777216.0f; }
    float gauss() { return (uni() + uni() + uni() - 1.5f) * 2; }
};
static float tone(float f, int i, int n, float amp) {  // a click-free beep sample
    float env = fminf(1.f, fminf((float)i, (float)(n - i)) / 32);
    return amp * env * sinf(2 * 3.14159265f * f * i / SR);
}

// A station being heard: its frames, and the rig's state machine for it.
struct Station {
    std::deque<std::array<int16_t, FRAME>> q;
    DWORD last = 0;
    enum { IDLE, CHUNK, VOICE, ROGER, TAIL } st = IDLE;
    int n = 0;          // samples left in CHUNK / ROGER / TAIL
    bool ended = false, beep = false;
    float s = 1, dist = 0, gate = 1, gateT = 1, fade = 0, rate = 1, crack = 0, gain = 1;  // gain: the rig's speaker where we stand
    int crackN = 0, hop = 0;
    bool off = false;  // tuned 5 kHz away: AM splatter
    float spl = 0;
    uint16_t lastSeq = 0xFFFF;
    Band hiss;
    Rng r;
    std::string name;
};
static std::map<uint64_t, Station> stations;  // id 1 is the radio check (parrot)
static std::vector<float> local;              // our own beeps, mixed in once
static std::vector<std::array<int16_t, FRAME>> parrotTape;
static float scope[64], txLevel;
static std::atomic<bool> talking{false};
static std::atomic<int> txOn{0};  // the rigs the current over goes out on

static float signalAt(double km, double range) { double v = range > 0 ? 1 - km / range : 0; return v <= 0 ? 0 : (float)pow(v, 0.6); }
static double myRange() { double a = rig[CB].avail && rig[CB].on ? rig[CB].range : 0, b = rig[HT].avail && rig[HT].on ? rig[HT].range : 0; return fmax(a, b); }
static double linkKm(uint64_t sid) { auto r = peerRk.find(sid); int k = r == peerRk.end() || !r->second ? 10 : r->second; return fmin(myRange(), k); }
static float sample(Station& t, int i, const int16_t* f) {
    float s = t.s, h = t.r.gauss(), out = 0, noise = t.hiss.run(h) * (0.03f + 0.32f * powf(1 - s, 1.6f));
    if (t.st == Station::CHUNK || t.st == Station::TAIL) {
        float k = t.st == Station::TAIL ? t.n / (SR * 0.17f) : 1;
        return t.hiss.run(h) * (0.35f + 0.25f * (1 - s)) * (0.2f + 0.8f * k) * 0.8f * 0.4f;  // 60% down (11-21)
    }
    if (t.st == Station::ROGER) {
        int at = SR * 160 / 1000 - t.n, a = SR * 70 / 1000;
        return (at < a ? tone(1200, at, a, 0.16f) : tone(900, at - a, SR * 90 / 1000, 0.16f)) * (0.5f + 0.5f * s);
    }
    float x = f ? f[i] / 32768.f : 0;
    if (t.off) { t.spl += 2 * 3.14159265f * 170 / SR; x = tanhf(4 * x) * 0.5f * (0.55f + 0.45f * sinf(t.spl)); }
    t.fade += 2 * 3.14159265f * t.rate / SR;
    float fade = 1 - (1 - s) * 0.55f * (0.5f + 0.5f * sinf(t.fade));
    if (t.crackN > 0) { t.crackN--; out += t.r.gauss() * t.crack * (1 - s * 0.7f); }
    else if (t.r.uni() < 0.0004f + 0.006f * (1 - s) * (1 - s)) { t.crackN = SR / 250; t.crack = 0.3f + 0.7f * t.r.uni(); }
    if (++t.hop >= SR / 20) { t.hop = 0; t.gateT = (s < 0.35f && t.r.uni() < (0.35f - s) * 1.4f) ? 0.06f : 1; }
    t.gate += (t.gateT - t.gate) * 0.15f;
    return (x * fade * (0.35f + 0.65f * s) + noise + out) * t.gate;
}
// One 20 ms block of everything the speaker plays.
static void mix(float* out) {
    memset(out, 0, FRAME * sizeof(float));
    DWORD now = GetTickCount();
    int open = 0;
    std::lock_guard<std::mutex> g(mu);
    for (auto it = stations.begin(); it != stations.end();) {
        Station& t = it->second;
        if (t.st == Station::IDLE) {
            if (t.q.size() >= 2) {
                t.st = Station::CHUNK; t.n = SR * 35 / 1000;
            }
            else if (t.q.empty() && now - t.last > 5000) { it = stations.erase(it); continue; }
        }
        std::array<int16_t, FRAME> f{};
        bool have = false;
        if (t.st == Station::VOICE && !t.q.empty()) { f = t.q.front(); t.q.pop_front(); have = true; }
        if (t.st == Station::VOICE && t.q.empty() && (t.ended || now - t.last > 400)) {  // carrier gone
            t.st = t.beep ? Station::ROGER : Station::TAIL; t.n = t.beep ? SR * 160 / 1000 : SR * 170 / 1000;
        }
        if (t.st != Station::IDLE) open++;
        for (int i = 0; i < FRAME && t.st != Station::IDLE; i++) {
            float v = sample(t, i, have ? f.data() : nullptr);
            if (!talking || it->first == 2) out[i] += v * t.gain;  // half duplex: keying up drowns everything else (not the loopback test)
            if (t.st == Station::CHUNK && --t.n <= 0) t.st = Station::VOICE;
            else if (t.st == Station::ROGER && --t.n <= 0) { t.st = Station::TAIL; t.n = SR * 170 / 1000; }
            else if (t.st == Station::TAIL && --t.n <= 0) { t.st = Station::IDLE; t.ended = false; t.q.clear(); }
        }
        ++it;
    }
    if (open >= 2 && !talking)  // two carriers on one channel: the heterodyne
        for (int i = 0; i < FRAME; i++) out[i] += 0.02f * sinf(2 * 3.14159265f * 1100 * (now * 8 + i) / SR);
    size_t k = local.size() < FRAME ? local.size() : FRAME;
    for (size_t i = 0; i < k; i++) out[i] += local[i];
    local.erase(local.begin(), local.begin() + k);
    float vol = volume / 100.f;
    for (int i = 0; i < FRAME; i++) out[i] = fmaxf(-1, fminf(1, out[i] * vol * 1.2f));
    for (int j = 0; j < 64; j++) scope[j] = fmaxf(fabsf(out[j * 2]), fabsf(out[j * 2 + 1]));
}
static void localBeep(std::initializer_list<float> freqs, int ms = 45) {
    std::lock_guard<std::mutex> g(mu);
    for (float f : freqs) {
        int n = SR * ms / 1000;
        for (int i = 0; i < n; i++) local.push_back(tone(f, i, n, 0.1f));
        for (int i = 0; i < SR / 30; i++) local.push_back(0);
    }
}
static void playerThread() {
    WAVEFORMATEX wf{WAVE_FORMAT_PCM, 1, SR, SR * 2, 2, 16, 0};
    static int16_t pcm[4][FRAME];
    float blk[FRAME];
  for (;;) {  // once per speaker picked
    reopenOut = false;
    HWAVEOUT wo;
    std::wstring dev; { std::lock_guard<std::mutex> g(mu); dev = spkDev; }
    if (waveOutOpen(&wo, deviceId(false, dev), &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        MT_LOG("no sound output -- the radio is silent until another speaker is picked");
        while (!api->stopping() && !reopenOut) Sleep(200);
        if (api->stopping()) return;
        continue;
    }
    WAVEHDR h[4]{};
    for (int b = 0; b < 4; b++) { h[b].lpData = (LPSTR)pcm[b]; h[b].dwBufferLength = sizeof pcm[b]; h[b].dwFlags = WHDR_DONE; }
    while (!api->stopping() && !reopenOut) {
        bool wrote = false;
        for (int b = 0; b < 4; b++) {
            if (!(h[b].dwFlags & WHDR_DONE)) continue;
            if (h[b].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &h[b], sizeof h[b]);
            mix(blk);
            for (int i = 0; i < FRAME; i++) pcm[b][i] = (int16_t)(blk[i] * 32000);
            if (recOut) fwrite(pcm[b], 2, FRAME, recOut);
            h[b].dwFlags = 0;
            waveOutPrepareHeader(wo, &h[b], sizeof h[b]);
            waveOutWrite(wo, &h[b], sizeof h[b]);
            wrote = true;
        }
        if (!wrote) Sleep(4);
    }
    waveOutReset(wo);
    for (int b = 0; b < 4; b++) if (h[b].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &h[b], sizeof h[b]);
    waveOutClose(wo);
    if (api->stopping()) return;
  }
}

// ---- receiving ----
static std::map<uint64_t, std::pair<DWORD, float>> peers;  // has the plugin: last heard, km away
static void received(uint64_t from, const Packet& p, int n) {
    if (n < HEAD || p.magic != MAGIC) return;
    if (from == mySid) {
        static bool said; if (!said) MT_LOG("steam: a message to ourselves came back -- the transport works end to end"); said = true;
        if (loopKm < 0 || p.type == P_HELLO) return;
        Packet q = p; q.x += (float)(loopKm * 100000); received(2, q, n); return;  // the loopback test, through Steam (station 2)
    }
    std::lock_guard<std::mutex> g(mu);
    double km = hypot(p.x - myX, p.y - myY) / 100000.0;
    if (!peers.count(from)) MT_LOG("station on the air: %llx, %.1f km", from, km);
    peers[from] = {GetTickCount(), (float)km};
    if (from != 2 && p.type == P_HELLO) peerRk[from] = p.rk;
    if (p.type == P_HELLO) {
        if (n >= HELLO) {
            HelloVeh h; memcpy(&h, p.ulaw, sizeof h);
            peerVeh[from] = h.veh;
            if (h.veh && h.veh == myVeh && h.khz >= KHZ_MIN && h.khz <= KHZ_MAX) {  // someone in our vehicle: the newer setting wins
                VehRadio& r = vehRadio[myVeh];
                if (h.ver > r.ver || (h.ver == r.ver && h.khz != r.khz && from < mySid)) {
                    r = {h.khz, h.ver}; saveVeh(myVeh, r);
                    if (rig[CB].khz != h.khz) {
                        int khz = rig[CB].khz = h.khz;
                        std::string who; for (auto& pl : players) if (pl.sid == from) who = pl.name;
                        notice(label(khz) + "  (set by " + (who.empty() ? "a passenger" : who) + ")");
                    }
                }
            }
        }
        if (!(p.flags & F_REPLY)) { Packet r = p; r.flags = F_REPLY; r.rk = 0; r.x = (float)myX; r.y = (float)myY; fillHello(r); sendTo(from, r, HELLO); }  // answer once
        return;
    }
    // which of our rigs hears it: tuned within 5 kHz (off-tune is weak and garbled), in range of the sender's rig
    // rigs, and loudest where we stand
    float s = 0, gain = 0; int df = 0;
    for (int r = 0; r < 2; r++) {
        const Rig& g = rig[r];
        int d = abs((int)p.key - g.khz);
        if (!g.on || !g.avail || g.hear <= 0 || d > 5) continue;
        if (r == CB && gameVoice && myVeh && inVehicle) { auto pv = peerVeh.find(from); if (pv != peerVeh.end() && pv->second == myVeh) continue; }  // in our cab: heard in person
        // the sender's range decides (the user, 11-21): a 1000 km ship is heard 1000 km out; a car's 10 km set
        // reaches the ship only within 10 km -- there the two can talk
        double link = p.rk ? p.rk : 10, km2 = hypot(p.x - g.x, p.y - g.y) / 100000.0;
        float sr = signalAt(km2, link) * (d ? 0.45f : 1);
        if (sr * g.hear > s * gain) { s = sr; gain = g.hear; df = d; km = km2; }
    }
    uint64_t key = from;  // a player's CB and handheld in one: an over sent on both is heard once, at the stronger
    auto it = stations.find(key);
    if (s <= 0 || (it == stations.end() && s * 100 < squelchPct)) return;  // out of range, or under the squelch
    Station& t = stations[key];
    if (p.type == P_VOICE && t.lastSeq == p.seq && GetTickCount() - t.last < 1000) return;  // its other rig's copy
    if (p.type == P_VOICE) t.lastSeq = p.seq;
    t.gain = gain;
    if (t.name.empty()) { t.r.s ^= (uint32_t)from; t.rate = 0.7f + 1.3f * t.r.uni(); for (auto& pl : players) if (pl.sid == from) t.name = pl.name; if (from == 2) t.name = "Loopback"; }
    t.s = s; t.dist = (float)km; t.last = GetTickCount(); t.off = df != 0;
    if (p.type == P_END) { t.ended = true; t.beep = (p.flags & F_ROGER) != 0; return; }
    if (n < (int)sizeof(Packet)) return;
    if (t.st == Station::TAIL || t.st == Station::ROGER) { t.st = Station::IDLE; t.q.clear(); }
    t.ended = false;
    std::array<int16_t, FRAME> f;
    for (int i = 0; i < FRAME; i++) f[i] = ulawDec(p.ulaw[i]);
    if (t.q.size() < 25) t.q.push_back(f);  // half a second at most: a stall drops, it does not lag
}
static void netThread() {
    DWORD lastHello = 0;
    uint16_t seq = 0;
    while (!steamGone && !api->stopping()) {
        if (!steamOk()) { Sleep(20); continue; }  // no world / closing: no Steam calls at all
        uintptr_t msgs[64];
        int n = steam.recv(steam.nm, STEAM_CHANNEL, msgs, 64);
        for (int i = 0; i < n; i++) {
            uintptr_t m = msgs[i];
            Packet p{};
            int size = get<int>(m + 8);
            uint64_t from = get<uint64_t>(m + 24);  // m_identityPeer.m_steamID64
            if (size > 0 && size <= (int)sizeof p && rd(get<uintptr_t>(m), &p, size)) received(from, p, size);
            steam.release(m);
        }
        if (GetTickCount() - lastHello > 3000 || helloNow.exchange(false)) {
            if (!lastHello) { Packet me{MAGIC, P_HELLO, 1, 0, 0}; sendTo(mySid, me, HEAD); }  // to ourselves once: logs if Steam loops it back
            lastHello = GetTickCount();
            std::vector<uint64_t> to;
            Packet h{MAGIC, P_HELLO, 0, seq++, 0};
            { std::lock_guard<std::mutex> g(mu); h.x = (float)myX; h.y = (float)myY; fillHello(h); for (auto& p : players) to.push_back(p.sid);
              for (auto it = peers.begin(); it != peers.end();) it = GetTickCount() - it->second.first > 12000 ? peers.erase(it) : ++it; }
            for (uint64_t sid : to) { if (!steamOk()) break; Identity id; id.sid = sid; steam.accept(steam.nm, &id); sendTo(sid, h, HELLO); }
        }
        Sleep(n ? 1 : 5);
    }
}

// ---- talking ----
static bool gameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static void openPanel();
static void overlayOff(void*);
static std::atomic<bool> overlayOn{false};  // the in-game panel is up
// the game's own push-to-talk, held with ours: the people around (the cab) hear us in person
static void speakOn(void* on) {
    if (!gPC || !fSpeak) return;
    struct { bool b; char pad[15]; } p{on != nullptr};
    api->process_event(gPC, fSpeak, &p);
}
static void speak(bool on) {
    static bool held;
    if (!gameVoice && !held) return;
    if (kVoice) {  // the game's own key, pressed for us: exactly what G does, its green line included
        if (on == held) return;
        held = on;
        keybd_event((BYTE)kVoice, (BYTE)MapVirtualKeyW(kVoice, MAPVK_VK_TO_VSC), on ? 0 : KEYEVENTF_KEYUP, 0);
    } else if (fSpeak && api->game_thread_ready() && !api->stopping()) api->run_on_game_thread(speakOn, on ? (void*)1 : nullptr);
}
static void keyThread() {
    HWAVEIN wi = nullptr;
    static int16_t cap[8][FRAME];
    WAVEHDR h[8]{};
    Band band;
    float env = 0.05f;
    uint16_t seq = 0;
    bool was = false, nextDown = false, panelDown = false, keyDown = false;
    int sentOn = 0;  // the rigs the current over went out on: its end goes to the same
    while (!api->stopping()) {
        bool f = gameFocused() && !chatOpen, ctl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool key = f && held(kTalk);
        int txs;  // the rigs this over goes out on (bit CB, bit HT)
        { std::lock_guard<std::mutex> g(mu);
          bool cbOk = inVehicle && rig[CB].avail && rig[CB].on, htOk = carried && rig[HT].on;
          txs = inVehicle ? ((txMode != 1 && cbOk ? 1 : 0) | (txMode != 0 && htOk ? 2 : 0)) : (htOk ? 2 : 0); }
          // the chosen rig off: nothing goes out and nothing beeps (11-21); Both with one off: that one alone, one beep
        int barred = 0;
        { std::lock_guard<std::mutex> g(mu);
          for (int r = 0; r < 2; r++) { int need = roleOf(rig[r].khz); if ((txs >> r & 1) && need && !(roles[r] & need)) { txs &= ~(1 << r); barred |= need; } } }
        static bool toldBar;
        if (key && barred && !txs && !toldBar) {
            toldBar = true;
            std::lock_guard<std::mutex> g(mu);
            notice(std::string("CB: a ") + (barred & R_POLICE ? "police" : barred & R_FIRE ? "fire" : barred & R_AMBULANCE ? "ambulance" : barred & R_TOW ? "towing" : "taxi")
                   + " channel -- talking needs that service's vehicle (CB) or uniform (ham radio)");
        }
        if (!key) toldBar = false;
        bool want = key && (txs || parrot);
        // the game's own voice goes with the key, not with the radio: the people around hear you even when no radio
        // can send (switched off, a service channel you have no role for, no rig) -- 11-21
        static bool gvHeld;
        if (key != gvHeld) { gvHeld = key; speak(key); }
        bool up = f && held(kUp), dn = f && held(kDown), nx = up || dn, pn = f && held(kPanel);
        if (nx && !nextDown) {
            bool down = dn;
            int c;
            { std::lock_guard<std::mutex> g(mu);  // by channel number (23 sits out of order on the band); off a channel, the nearest
              int r = activeRig(), khz = rig[r].khz;
              if (r == CB && !inVehicle) { notice("CB: get in the vehicle to tune its CB"); nextDown = true; continue; }
              if (!rig[r].on) { notice(r == CB ? "CB: switched off" : "Ham radio: switched off"); nextDown = true; continue; }
              int best = chOf(khz);
              if (best) best = down ? (best + 38) % 40 + 1 : best % 40 + 1;
              else {
                  for (int i = 1; i <= 40; i++) {
                      int k = chKhz(i);
                      if (down ? (k < khz && (!best || k > chKhz(best))) : (k > khz && (!best || k < chKhz(best)))) best = i;
                  }
                  if (!best) best = down ? 40 : 1;
              }
              rig[r].khz = chKhz(best); c = best; radioChanged(r); notice(label(rig[r].khz) + (r == HT ? "  (ham radio)" : "")); }
            localBeep({1750, c == 9 ? 2400.f : c == 19 ? 2000.f : 1750.f});
        }
        if (pn && !panelDown) openPanel();
        static bool escDown;
        bool esc = f && (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
        if (esc && !escDown && overlayOn && api->game_thread_ready()) api->run_on_game_thread(overlayOff, nullptr);
        escDown = esc;
        nextDown = nx; panelDown = pn;
        if (key && !want && !keyDown) { std::lock_guard<std::mutex> g(mu); notice(carried ? "Ham radio: switched off" : "CB: the rig is in your vehicle. On foot, take out a ham radio (tech store)"); }
        keyDown = key;
        if (want && !was) {  // key up
            WAVEFORMATEX wf{WAVE_FORMAT_PCM, 1, SR, SR * 2, 2, 16, 0};
            if (waveInOpen(&wi, [] { std::lock_guard<std::mutex> g(mu); return deviceId(true, micDev); }(), &wf, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
                for (int b = 0; b < 8; b++) {
                    h[b] = WAVEHDR{}; h[b].lpData = (LPSTR)cap[b]; h[b].dwBufferLength = sizeof cap[b];
                    waveInPrepareHeader(wi, &h[b], sizeof h[b]); waveInAddBuffer(wi, &h[b], sizeof h[b]);
                }
                waveInStart(wi);
                talking = true;
                std::lock_guard<std::mutex> g(mu); parrotTape.clear();
            } else { wi = nullptr; MT_LOG("no microphone -- cannot talk"); }
            localBeep({2200}, 12);
            was = true; sentOn = txs;
        }
        if (was && wi) {
            sentOn |= txs;
            Packet out[2]; std::vector<uint64_t> dest[2];  // one packet per rig keyed: its frequency, its range
            {
                std::lock_guard<std::mutex> g(mu);
                for (int r = 0; r < 2; r++) {
                    if (!(txs >> r & 1)) continue;
                    out[r] = Packet{MAGIC, P_VOICE, (uint8_t)(r == HT ? F_HT : 0), 0, (uint32_t)rig[r].khz};
                    out[r].x = (float)myX; out[r].y = (float)myY; out[r].rk = (uint16_t)rig[r].range;
                    for (auto& e : peers) if (e.second.second <= rig[r].range * 1.15) dest[r].push_back(e.first);
                }
            }
            Packet& p = out[txs & 1 ? 0 : 1];
            for (int b = 0; b < 8; b++) {
                if (!(h[b].dwFlags & WHDR_DONE)) continue;
                float lvl = 0, gain = micGain / 100.f;
                std::array<int16_t, FRAME> fr;
                for (int i = 0; i < FRAME; i++) {
                    float x = band.run((talkFile.empty() ? cap[b][i] : talkFile[talkAt++ % talkFile.size()]) / 32768.f);
                    env = fmaxf(fabsf(x), env * 0.9995f);
                    x *= fminf(8.f, 0.5f / fmaxf(env, 0.01f)) * gain;      // levelled, then the preamp slammed
                    x = tanhf(3.2f * x) / tanhf(3.2f);
                    lvl = fmaxf(lvl, fabsf(x));
                    fr[i] = (int16_t)(x * 30000);
                    p.ulaw[i] = ulawEnc(fr[i]);
                }
                txLevel = lvl; txOn = sentOn;
                uint16_t sq = seq++;
                for (int r = 0; r < 2; r++) {
                    if (!(txs >> r & 1)) continue;
                    if (&out[r] != &p) memcpy(out[r].ulaw, p.ulaw, FRAME);
                    out[r].seq = sq;
                    for (uint64_t sid : dest[r]) sendTo(sid, out[r], sizeof(Packet));
                    if (loopKm >= 0) sendTo(mySid, out[r], sizeof(Packet));
                }
                if (parrot) { std::lock_guard<std::mutex> g(mu); if (parrotTape.size() < 50 * 60) parrotTape.push_back(fr); }
                h[b].dwFlags &= ~WHDR_DONE;
                waveInAddBuffer(wi, &h[b], sizeof h[b]);
            }
        }
        if (!want && was) {  // key down: the end of the over, with the roger beep if it is on
            if (wi) {
                waveInReset(wi);
                for (int b = 0; b < 8; b++) waveInUnprepareHeader(wi, &h[b], sizeof h[b]);
                waveInClose(wi); wi = nullptr;
                uint16_t sq = seq++;
                for (int r = 0; r < 2; r++) {
                    if (!(sentOn >> r & 1)) continue;
                    Packet e{MAGIC, P_END, (uint8_t)((roger ? F_ROGER : 0) | (r == HT ? F_HT : 0)), sq, 0};
                    std::vector<uint64_t> to;
                    { std::lock_guard<std::mutex> g(mu); e.key = (uint32_t)rig[r].khz; e.x = (float)myX; e.y = (float)myY; e.rk = (uint16_t)rig[r].range;
                      for (auto& pe : peers) if (pe.second.second <= rig[r].range * 1.15) to.push_back(pe.first); }
                    for (uint64_t sid : to) sendTo(sid, e, HEAD);
                    if (loopKm >= 0) sendTo(mySid, e, HEAD);
                }
            }
            talking = false; txOn = 0; txLevel = 0; was = false;
            std::lock_guard<std::mutex> g(mu);
            if (parrot && !parrotTape.empty()) {  // radio check: our own over comes back as a station would hear it
                Station& t = stations[1];
                t.name = "Radio check"; t.s = parrotS / 100.f; t.dist = (float)(10 * (1 - pow(t.s, 1 / 0.6)));
                t.st = Station::IDLE; t.q.assign(parrotTape.begin(), parrotTape.end()); t.ended = true; t.beep = roger != 0;
                t.last = GetTickCount(); t.rate = 1.1f;
                parrotTape.clear();
            }
        }
        Sleep(was ? 5 : 10);
    }
}

// ---- the panel ----
static const unsigned char PANEL[] = {
#include "cb_radio_panel.inc"
};
static int panelPort;
// " - Truckers" for the built-in channels and the player's own presets ("Name|ch|code;..."); "" for none.
// The named channels (the user, 2026-11-21): one per town, then services, water, road and work. No racing; 31-40 and
// every frequency between the channels are open. Same list as the panel's.
static const char* CHNAME[41] = {"", "Jeju", "Ara", "Gangjung", "Gapa", "Seongsan", "Hallim", "Gwangjin", "Arini",
    "Emergency", "Police", "Fire", "Ambulance", "Mountain rescue", "Towing & recovery", "Roadside help", "Marine calling",
    "Harbours & docks", "Ferries & ships", "Truckers", "Heavy haul & escort", "Taxi & bus", "Delivery & mail",
    "Garbage & utilities", "Construction", "Farming", "Off-road", "Fishing", "Diving & subs", "Air", "Convoy"};
// Every town's own services, above channel 40 (the user, 11-21: "the police in Arini, the ambulance in Gapa"): town t,
// service v at 27.455 + 0.090 t + 0.010 v MHz -- 8 towns x 9 services, 27.455-28.165. Same plan as the panel's.
static const char* TOWNS[8] = {"Jeju", "Ara", "Gangjung", "Gapa", "Seongsan", "Hallim", "Gwangjin", "Arini"};
static const char* SERVICES[9] = {"Police", "Fire", "Ambulance", "Mountain rescue", "Towing", "Harbour", "Diving & subs", "Taxi & bus", "Delivery"};
static std::string townName(int k) {
    int d = k - 27455;
    if (d < 0 || d % 10) return "";
    int t = d / 90, v = d % 90 / 10;
    return t < 8 && v < 9 ? std::string(TOWNS[t]) + " " + SERVICES[v] : "";
}
static int roleOf(int k) {
    switch (chOf(k)) { case 10: return R_POLICE; case 11: return R_FIRE; case 12: return R_AMBULANCE; case 14: return R_TOW; case 21: return R_TAXI; }
    int d = k - 27455;
    if (d < 0 || d % 10 || d / 90 >= 8) return 0;
    static const int bySvc[9] = {R_POLICE, R_FIRE, R_AMBULANCE, 0, R_TOW, 0, 0, R_TAXI, 0};
    return bySvc[d % 90 / 10];
}
static std::string presetName(int ch) {
    return ch >= 1 && ch <= 30 ? std::string("  -  ") + CHNAME[ch] : "";
}
struct FString16 {
    struct { const wchar_t* d; int n, m; } s{};
    std::wstring keep;
    void set(const wchar_t* w) { keep = w; s.d = keep.c_str(); s.n = s.m = (int)keep.size() + 1; }
};
static std::string jesc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 32) o += c; }
    return o;
}
static std::string stateJson() {
    std::lock_guard<std::mutex> g(mu);
    char b[640];
    snprintf(b, sizeof b, "{\"vol\":%d,\"sq\":%d,\"mic\":%d,\"roger\":%d,\"holding\":%d,\"carried\":%d,\"txMode\":%d,\"mode\":%d,"
             "\"parrot\":%d,\"parrotS\":%d,\"tx\":%d,\"txOn\":%d,\"txLevel\":%.2f,\"inVeh\":%d,\"veh\":%d,\"gamevoice\":%d,\"steam\":%d,",
             volume, squelchPct, micGain, roger, holding ? 1 : 0, carried ? 1 : 0, txMode, panelMode, parrot, parrotS, talking ? 1 : 0, txOn.load(),
             txLevel, inVehicle ? 1 : 0, myVeh ? 1 : 0, gameVoice, steam.nm ? 1 : 0);
    std::string j = b;
    for (int r = 0; r < 2; r++) {
        const Rig& g = rig[r];
        float rx = 0;
        for (auto& e : stations) if (e.second.st != Station::IDLE) rx = fmaxf(rx, e.second.s);
        snprintf(b, sizeof b, "\"%s\":{\"avail\":%d,\"on\":%d,\"khz\":%d,\"ch\":%d,\"range\":%.0f,\"hear\":%.2f,\"roles\":%d},",
                 r == CB ? "cb" : "ht", g.avail ? 1 : 0, g.on ? 1 : 0, g.khz, chOf(g.khz), g.range, g.hear, roles[r]);
        j += b;
    }
    // what a CB shows: the signal on the meter -- never who is talking or where
    j += "\"rx\":[";
    bool first = true;
    for (auto& e : stations) {
        if (e.second.st == Station::IDLE) continue;
        snprintf(b, sizeof b, "%s%.2f", first ? "" : ",", e.second.s);
        j += b; first = false;
    }
    j += "],\"keys\":[";
    for (int i = 0; i < 4; i++) j += (i ? ",\"" : "\"") + jesc(keyNames[i]) + "\"";
    snprintf(b, sizeof b, "],\"range\":%.0f}", myRange());  // no count of rigs in range: a CB cannot know it
    return j + b;
}
// The sound devices, asked only when the panel opens its Audio section (enumerating them is the slow part).
static std::string devicesJson() {
    auto mics = devices(true), spks = devices(false);
    std::wstring md, sd;
    { std::lock_guard<std::mutex> g(mu); md = micDev; sd = spkDev; }
    std::string j = "{\"micDev\":\"" + jesc(utf8(md)) + "\",\"spkDev\":\"" + jesc(utf8(sd)) + "\",\"mics\":[";
    for (auto& d : mics) j += "\"" + jesc(utf8(d)) + "\",";
    if (j.back() == ',') j.pop_back();
    j += "],\"spks\":[";
    for (auto& d : spks) j += "\"" + jesc(utf8(d)) + "\",";
    if (j.back() == ',') j.pop_back();
    return j + "]}";
}
static bool apply(const std::string& q) {  // -> the radio was retuned (the click gets its beep)  // k=v&k=v
    std::lock_guard<std::mutex> g(mu);
    int r = q.find("rig=ht") != std::string::npos ? HT : CB;  // the rig the panel's controls act on
    int& khz = rig[r].khz;
    int was = khz;
    size_t at = 0;
    while (at < q.size()) {
        size_t amp = q.find('&', at); if (amp == std::string::npos) amp = q.size();
        std::string kv = q.substr(at, amp - at), k = kv.substr(0, kv.find('=')), v = kv.find('=') == std::string::npos ? "" : kv.substr(kv.find('=') + 1);
        int n = atoi(v.c_str());
        auto clamp = [](int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; };
        // a radio is only set where it is: the CB from its vehicle's seat, the handheld once it is with you
        bool here = r == CB ? inVehicle : carried;
        if ((k == "ch" || k == "khz" || k == "step" || k == "power") && !here) { at = amp + 1; continue; }
        if ((k == "ch" || k == "khz" || k == "step") && !rig[r].on) { at = amp + 1; continue; }  // switched off: nothing to tune
        if (k == "ch") { if (chKhz(n)) khz = chKhz(n); }
        else if (k == "khz") khz = snapKhz(n);
        else if (k == "step") khz = snapKhz(khz + 5 * clamp(n, -200, 200));
        else if (k == "vol") volume = clamp(n, 0, 150);
        else if (k == "sq") squelchPct = clamp(n, 0, 95);
        else if (k == "mic") micGain = clamp(n, 10, 300);
        else if (k == "roger") roger = n ? 1 : 0;
        else if (k == "gamevoice") gameVoice = n ? 1 : 0;
        else if (k == "power") rig[r].on = n != 0;
        else if (k == "txmode") txMode = clamp(n, 0, 2);
        else if (k == "mode") panelMode = clamp(n, 0, 1);
        else if (k == "parrot") parrot = n ? 1 : 0;
        else if (k == "parrotS") parrotS = clamp(n, 1, 100);
        else if (k == "micDev") micDev = fromUrl(v);
        else if (k == "spkDev") { spkDev = fromUrl(v); reopenOut = true; }
        at = amp + 1;
    }
    bool tuned = khz != was;
    if (khz != was) { radioChanged(r); notice(label(khz) + (r == HT ? "  (ham radio)" : "")); }
    saveSettings();
    return tuned;
}
static void serve(SOCKET c) {
    char buf[4096]; int n = recv(c, buf, sizeof buf - 1, 0);
    if (n <= 0) { closesocket(c); return; }
    buf[n] = 0;
    std::string req(buf), path = req.substr(req.find(' ') + 1); path = path.substr(0, path.find(' '));
    std::string body, type = "application/json";
    if (path.rfind("/state", 0) == 0) body = stateJson();
    else if (path.rfind("/devices", 0) == 0) body = devicesJson();
    else if (path.rfind("/set?", 0) == 0) { bool tuned = apply(path.substr(5)); body = stateJson(); if (tuned) localBeep({1750, 1750}); }  // a refused click makes no sound
    else { body.assign((const char*)PANEL, sizeof PANEL); type = "text/html; charset=utf-8"; }
    std::string head = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    send(c, head.data(), (int)head.size(), 0); send(c, body.data(), (int)body.size(), 0);
    shutdown(c, SD_SEND); closesocket(c);
}
static void panelThread() {
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    int first = iniInt(INI, L"Panel", L"Port", 8095);
    for (int port = first; port < first + 16; port++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((u_short)port);
        if (bind(s, (sockaddr*)&a, sizeof a) == SOCKET_ERROR || listen(s, 8) == SOCKET_ERROR) { closesocket(s); continue; }  // no SO_REUSEADDR: a second game must not share it
        panelPort = port;
        MT_LOG("panel: http://127.0.0.1:%d/", port);
        while (!api->stopping()) { SOCKET c = accept(s, nullptr, nullptr); if (c != INVALID_SOCKET) std::thread(serve, c).detach(); }
        return;
    }
    MT_LOG("panel: no free port from %d", first);
}
static void openBrowser() {
    if (!panelPort) return;
    wchar_t url[64]; swprintf(url, 64, L"http://127.0.0.1:%d/", panelPort);
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}
// ---- the panel inside the game (the user, 11-21: "like the leaderboard html in game"). The game's leaderboard,
// W_Leaderboard, is a web browser widget (MotorTownWeb) on a canvas: one of those is made, pointed at our panel,
// put on screen with the mouse; the panel key or Esc takes it off again. [Panel] InGame=0: the outside browser.
static uintptr_t overlay;      // the widget while it is up (game thread)
static int overlayIdx = -1;
static FString16 overlayUrl;   // kept: the engine reads it during LoadURL
static uintptr_t findFn(const char* name, const char* outer) { return api->find_object("Function", name, outer); }
static void overlayOff(void*) {
    uintptr_t pc = gPC;
    if (overlay && api->object(overlayIdx) == overlay) {
        if (uintptr_t f = findFn("RemoveFromParent", "Widget")) { unsigned char p[8] = {}; api->process_event(overlay, f, p); }
    }
    overlay = 0; overlayIdx = -1;
    if (pc) {
        uintptr_t lib = api->find_object("WidgetBlueprintLibrary", "Default__WidgetBlueprintLibrary", "/Script/UMG");
        if (uintptr_t f = findFn("SetInputMode_GameOnly", "WidgetBlueprintLibrary"); f && lib) {
            unsigned char p[32] = {}; int o = off(f, "PlayerController"), fl = off(f, "bFlushInput");
            if (o >= 0) memcpy(p + o, &pc, 8);
            if (fl >= 0) p[fl] = 1;
            api->process_event(lib, f, p);
        }
        if (uintptr_t f = findFn("SetShowMouseCursor", "PlayerController")) { unsigned char p[8] = {0}; api->process_event(pc, f, p); }
    }
    overlayOn = false;
    MT_LOG("overlay: closed");
}
static void overlayUp(void*) {
    uintptr_t pc = gPC;
    if (!pc || !panelPort) return;
    uintptr_t cls = api->find_object("WidgetBlueprintGeneratedClass", "W_Leaderboard_C", "/Game/UI/Leaderboard/W_Leaderboard");
    uintptr_t lib = api->find_object("WidgetBlueprintLibrary", "Default__WidgetBlueprintLibrary", "/Script/UMG");
    uintptr_t fCreate = findFn("Create", "WidgetBlueprintLibrary"), fAdd = findFn("AddToViewport", "UserWidget"),
              fLoad = findFn("LoadURL", "WebBrowser");
    if (!cls || !lib || !fCreate || !fAdd || !fLoad) {
        MT_LOG("overlay: %s not found -- the outside browser instead", !cls ? "W_Leaderboard (not loaded yet?)" : "a UMG function");
        openBrowser(); return;
    }
    unsigned char p[64] = {};
    int oW = off(fCreate, "WorldContextObject"), oT = off(fCreate, "WidgetType"), oP = off(fCreate, "OwningPlayer"), oR = off(fCreate, "ReturnValue");
    if (oW < 0 || oT < 0 || oR < 0) { openBrowser(); return; }
    memcpy(p + oW, &pc, 8); memcpy(p + oT, &cls, 8); if (oP >= 0) memcpy(p + oP, &pc, 8);
    api->process_event(lib, fCreate, p);
    uintptr_t w = 0; memcpy(&w, p + oR, 8);
    if (!w) { MT_LOG("overlay: the widget was not made -- the outside browser instead"); openBrowser(); return; }
    { unsigned char a[8] = {}; int z = 50; memcpy(a, &z, 4); api->process_event(w, fAdd, a); }  // above the HUD
    int oWeb = off(cls, "MotorTownWeb"), oBtn = off(cls, "ReloadWebButton");
    uintptr_t web = oWeb >= 0 ? get<uintptr_t>(w + oWeb) : 0, btn = oBtn >= 0 ? get<uintptr_t>(w + oBtn) : 0;
    if (web) {
        wchar_t url[64]; swprintf(url, 64, L"http://127.0.0.1:%d/", panelPort);
        overlayUrl.set(url);
        unsigned char a[32] = {}; memcpy(a, &overlayUrl.s, sizeof overlayUrl.s);
        api->process_event(web, fLoad, a);
    }
    if (btn) if (uintptr_t f = findFn("SetVisibility", "Widget")) { unsigned char a[8] = {1}; api->process_event(btn, f, a); }  // Collapsed: no "reload the leaderboard"
    // a window, not the whole screen (the user, 11-21): the browser's canvas slot anchored to a centred share of the
    // screen ([Panel] Width / Height, percent), so it keeps its proportion on any resolution
    if (web) {
        uintptr_t slot = 0;
        if (uintptr_t cW = api->find_object("Class", "Widget", "/Script/UMG")) { int o = off(cW, "Slot"); if (o >= 0) slot = get<uintptr_t>(web + o); }
        uintptr_t fAnch = findFn("SetAnchors", "CanvasPanelSlot"), fOffs = findFn("SetOffsets", "CanvasPanelSlot"), fAlign = findFn("SetAlignment", "CanvasPanelSlot");
        if (slot && fAnch && fOffs) {
            float wx = (float)(panelW / 100.0), hy = (float)(panelH / 100.0);
            struct { double minX, minY, maxX, maxY; } an{(1 - wx) / 2, (1 - hy) / 2, (1 + wx) / 2, (1 + hy) / 2};  // FAnchors: two FVector2D (doubles)
            unsigned char a[64] = {}; memcpy(a, &an, sizeof an); api->process_event(slot, fAnch, a);
            unsigned char m[32] = {}; api->process_event(slot, fOffs, m);  // FMargin 0: the anchors are the box
            if (fAlign) { unsigned char z[32] = {}; api->process_event(slot, fAlign, z); }
        } else MT_LOG("overlay: the browser's slot was not found -- it keeps the leaderboard's size");
    }
    if (uintptr_t f = findFn("SetInputMode_GameAndUIEx", "WidgetBlueprintLibrary")) {
        unsigned char a[48] = {}; int o = off(f, "PlayerController"), ow = off(f, "InWidgetToFocus"), hc = off(f, "bHideCursorDuringCapture");
        if (o >= 0) memcpy(a + o, &pc, 8);
        if (ow >= 0) memcpy(a + ow, &w, 8);
        if (hc >= 0) a[hc] = 0;
        api->process_event(lib, f, a);
    }
    if (uintptr_t f = findFn("SetShowMouseCursor", "PlayerController")) { unsigned char a[8] = {1}; api->process_event(pc, f, a); }
    overlay = w; overlayIdx = -1;
    for (int i = 0, n = api->num_objects(); i < n; i++) if (api->object(i) == w) { overlayIdx = i; break; }
    overlayOn = true;
    MT_LOG("overlay: up (browser %s)", web ? "pointed at the panel" : "NOT found");
}
static void openPanel() {    // the panel key: the in-game panel on and off, or the outside browser
    if (!inGamePanel) { openBrowser(); return; }
    if (!api->game_thread_ready()) return;
    api->run_on_game_thread(overlayOn ? overlayOff : overlayUp, nullptr);
}

static bool selfCheck() {
    for (int v = -32000; v <= 32000; v += 97) { int d = ulawDec(ulawEnc((int16_t)v)) - v; if (abs(d) > 1100 || (abs(v) < 200 && abs(d) > 20)) return false; }
    if (chKhz(19) != 27185 || chKhz(9) != 27065 || chKhz(40) != 27405 || chKhz(23) != 27255 || chOf(27190) || chOf(27185) != 19) return false;
    if (townName(27455) != "Jeju Police" || townName(27455 + 90 * 3 + 20) != "Gapa Ambulance" || townName(27455 + 90 * 7 + 80) != "Arini Delivery"
        || !townName(27450).empty() || !townName(27455 + 90 * 8).empty() || !townName(27460).empty()) return false;
    if (roleOf(chKhz(10)) != R_POLICE || roleOf(chKhz(9)) || roleOf(27455 + 90 * 3 + 20) != R_AMBULANCE || roleOf(27455 + 90 * 5 + 30)
        || roleOf(27455 + 90 * 7 + 70) != R_TAXI || roleOf(chKhz(21)) != R_TAXI) return false;
    if (snapKhz(27187) != 27185 || snapKhz(27188) != 27190 || snapKhz(1) != KHZ_MIN || snapKhz(99999) != KHZ_MAX) return false;
    if (parseKey(L"Ctrl+End").vk != VK_END || !parseKey(L"ctrl + end").ctrl || parseKey(L"Home").vk != VK_HOME || parseKey(L"36").vk != 36
        || parseKey(L"F5").vk != VK_F5 || parseKey(L"g").vk != 'G' || parseKey(L"Numpad3").vk != VK_NUMPAD3 || parseKey(L"nope").vk) return false;
    return signalAt(0, 10) == 1 && signalAt(10, 10) == 0 && signalAt(5, 10) > 0.5f && signalAt(5, 10) < 0.8f && signalAt(1, 0) == 0;
}

// One look, once a world is up: is the handheld in the game's item list, and what do the shops sell? (11-21: the item
// did not show at the tech store, twice.) Game thread.
static void shopCheck(void*) {
    int key = api->name_index("CB_Handheld");
    MT_LOG("shop check: name CB_Handheld %s (pool %d)", key ? "known" : "NOT in the name pool", key);
    uintptr_t own = api->find_object("DataTable", "Items_CBRadio", "/Game/DataAsset/Items/Items_CBRadio");
    MT_LOG("shop check: table Items_CBRadio %s", own ? "loaded" : "NOT loaded");
    uintptr_t comp = api->find_object("CompositeDataTable", "Items", "/Game/DataAsset/Items/Items");
    uintptr_t fRow = api->find_object("Function", "DoesDataTableRowExist", "DataTableFunctionLibrary");
    uintptr_t lib = api->find_object("DataTableFunctionLibrary", "Default__DataTableFunctionLibrary", "/Script/Engine");
    for (uintptr_t t : {comp, own}) {
        if (!t || !fRow || !lib || !key) continue;
        int oT = off(fRow, "Table"), oR = off(fRow, "RowName"), oRet = off(fRow, "ReturnValue");
        unsigned char prm[64] = {};
        if (oT < 0 || oR < 0 || oRet < 0) break;
        memcpy(prm + oT, &t, 8);
        uint32_t nm[2] = {(uint32_t)key, 0};
        memcpy(prm + oR, nm, 8);
        api->process_event(lib, fRow, prm);
        MT_LOG("shop check: %s has the row: %s", t == comp ? "Items (composite)" : "Items_CBRadio", prm[oRet] ? "YES" : "no");
    }
    if (!comp) MT_LOG("shop check: the Items composite was not found by name");
    uintptr_t cVend = api->find_object("Class", "MTVendorComponent", "/Script/MotorTown");
    int oSell = cVend ? off(cVend, "SellItems") : -1, laptop = api->name_index("Furniture_Laptop_01"), told = 0;
    for (int i = 0, n = api->num_objects(); i < n && cVend && oSell >= 0 && told < 40; i++) {
        uintptr_t o = api->object(i);
        if (!o || isCDO(o) || !api->is_a(classOf(o), cVend)) continue;
        struct { uintptr_t d; int num, max; } a{};
        if (!rd(o + oSell, &a, sizeof a) || a.num < 0 || a.num > 500) continue;
        bool hasLaptop = false, hasRadio = false;
        for (int k = 0; k < a.num; k++) { uint32_t c = 0; rd(a.d + k * 8, &c, 4); hasLaptop |= c == (uint32_t)laptop; hasRadio |= key && c == (uint32_t)key; }
        uintptr_t outer = outerOf(o);
        MT_LOG("shop check: vendor %s (%s): %d items%s%s", objName(o).c_str(), outer ? objName(outer).c_str() : "-", a.num,
               hasLaptop ? ", the tech store list" : "", hasRadio ? ", WITH the handheld" : "");
        told++;
    }
}
static DWORD WINAPI run(LPVOID) {
    if (!api->wait_engine(INFINITE)) return 0;
    bool world = resolveWorld();
    for (int i = 0; i < 6000 && !steamUp() && !api->stopping(); i++) Sleep(50);
    MT_LOG("steam: %s, me %llx", steam.nm ? "messages up" : "NOT AVAILABLE -- radio check only", mySid);
    std::thread(playerThread).detach();
    std::thread(keyThread).detach();
    std::thread(panelThread).detach();
    if (steam.nm) std::thread(netThread).detach();
    DWORD shopAt = 0;
    while (!api->stopping()) {
        // Closing: the game drops its world and its window before it shuts Steam down, and Steam's unloaded crash
        // handler makes any fault after that a crash (crashhandler64.dll_unloaded on every close, 11-21). So Steam is
        // only called while a world and the window are up, and once the window closes nothing of ours runs.
        static int tick;
        bool slow = tick++ % 5 == 0;  // the world scan every 50 ms (it is what sees the world go); the rest every 250 ms
        if (world) scanWorld();
        static bool sawWindow;
        HWND win = gameWindow();
        sawWindow |= win != nullptr;
        bool live = gPC != 0 && win != nullptr;
        if (live != netLive) { netLive = live; MT_LOG(live ? "on the air: a world is up" : "off the air: no world or the window closed (no Steam calls)"); }
        if (sawWindow && !win) {  // the window closed: the game is on its way out -- nothing of ours runs any more
            steamGone = true; MT_LOG("the game window closed: the radio stops before Steam does");
            break;
        }
        if (!shopAt && gPC) shopAt = GetTickCount() + 20000;
        if (shopAt && shopAt != 1 && (int)(GetTickCount() - shopAt) >= 0 && api->game_thread_ready()) { shopAt = 1; api->run_on_game_thread(shopCheck, nullptr); }
        bool any; { std::lock_guard<std::mutex> g(mu); any = !notices.empty(); }
        if (slow && any && gPC && api->game_thread_ready()) api->run_on_game_thread(showNotices, nullptr);
        if (slow && gChat && api->game_thread_ready()) api->run_on_game_thread(checkChat, nullptr);
        Sleep(50);
    }
    return 0;
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    if (iniDisabled(INI)) return 0;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wcsstr(exe, L"MotorTownServer")) return 0;  // a client's radio: the server has no one to talk
    loadSettings();
    loadDebug();
    if (!selfCheck()) { MT_LOG("self-check FAILED -- not started"); return 0; }
    MT_LOG("CB radio: handheld %s", label(footKhz).c_str());
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
