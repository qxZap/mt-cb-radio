// CB Radio -- plugins\mt_plugin_CBRadio.dll. Every vehicle has a CB rig (the user, 2026-11-21: "all vehicles shall have
// CB radio" until a CB part exists). Hold Home to talk; End / Ctrl+End channel up / down; Ctrl+Home opens the rig's
// panel (http://127.0.0.1:8095/): channel, privacy code, volume, squelch, mic gain, roger beep, presets, who is on air.
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
// Settings: plugins\cb_radio.ini ([General] Disabled=1; [Radio] ...; [Keys] virtual-key codes). The panel writes them.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#include <shellapi.h>
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
static const uint32_t MAGIC = 0x31524243;                        // "CBR1"

// The 40 US/EU CB channels (MHz). 23 sits between 25 and 26: that is the real band plan.
static const char* FREQ[41] = {"", "26.965", "26.975", "26.985", "27.005", "27.015", "27.025", "27.035", "27.055", "27.065",
    "27.075", "27.085", "27.105", "27.115", "27.125", "27.135", "27.155", "27.165", "27.175", "27.185", "27.205", "27.215",
    "27.225", "27.255", "27.235", "27.245", "27.265", "27.275", "27.285", "27.295", "27.305", "27.315", "27.325", "27.335",
    "27.345", "27.355", "27.365", "27.375", "27.385", "27.395", "27.405"};

// ---- settings ----
static std::mutex mu;  // everything below that two threads touch
static int channel = 19, code = 0, volume = 80, squelchPct = 8, micGain = 100, roger = 1, handheld = 0, parrot = 0, parrotS = 60;
static double rangeKm = 10;
static int kTalk = VK_HOME, kNext = VK_END, kPanel = VK_HOME;  // Ctrl+End = channel down, Ctrl+Home = panel
static std::wstring presets, micDev, spkDev;  // devices by name ("" = the system's default)
static std::atomic<bool> reopenOut{false};
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
    put(L"Channel", channel); put(L"Code", code); put(L"Volume", volume); put(L"Squelch", squelchPct); put(L"MicGain", micGain);
    put(L"RogerBeep", roger); put(L"Handheld", handheld);
    iniSet(INI, L"Radio", L"Presets", presets.c_str(), true);
    iniSet(INI, L"Radio", L"MicDevice", micDev.c_str(), true);
    iniSet(INI, L"Radio", L"SpeakerDevice", spkDev.c_str(), true);
}
static void loadSettings() {
    channel = iniInt(INI, L"Radio", L"Channel", 19); if (channel < 1 || channel > 40) channel = 19;
    code = iniInt(INI, L"Radio", L"Code", 0) % 100;
    volume = iniInt(INI, L"Radio", L"Volume", 80); squelchPct = iniInt(INI, L"Radio", L"Squelch", 8);
    micGain = iniInt(INI, L"Radio", L"MicGain", 100); roger = iniInt(INI, L"Radio", L"RogerBeep", 1);
    handheld = iniInt(INI, L"Radio", L"Handheld", 0);
    rangeKm = iniNum(INI, L"Radio", L"RangeKm", 10); if (rangeKm < 0.5) rangeKm = 0.5;
    iniSet(INI, L"Radio", L"Presets", L"");
    presets = iniGet(INI, L"Radio", L"Presets");
    iniSet(INI, L"Radio", L"MicDevice", L""); iniSet(INI, L"Radio", L"SpeakerDevice", L"");
    micDev = iniGet(INI, L"Radio", L"MicDevice"); spkDev = iniGet(INI, L"Radio", L"SpeakerDevice");
    kTalk = iniInt(INI, L"Keys", L"Talk", VK_HOME); kNext = iniInt(INI, L"Keys", L"Channel", VK_END);
    kPanel = iniInt(INI, L"Keys", L"Panel", VK_HOME);
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
static uint32_t myKey() { return (uint32_t)(channel * 100 + code); }

// ---- the world: who is here, where we are (read off the game's objects, no game thread needed) ----
struct Player { uint64_t sid; std::string name; };
static std::vector<Player> players;  // everyone in the session but us
static uint64_t mySid;
static std::string myName;
static double myX, myY;
static bool inVehicle;
static std::atomic<bool> chatOpen{false};  // typing in the game's chat: Home / End are the text cursor's then
static uintptr_t cChat, gChat, fHasFocus; static int oChatText = -1;
static uintptr_t cPS, cPC, cVeh;
static int oUid = -1, oName = -1, oPSLoc = -1, oCtrlPS = -1, oPawn = -1, oRoot = -1, oRelLoc = -1;

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
static std::string presetName(int ch, int cd);

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
    static std::vector<uintptr_t> states;
    static uintptr_t pc, chat;
    static DWORD lastFull;
    if (GetTickCount() - lastFull > 3000) {  // the full object walk, every 3 s; positions every pass
        lastFull = GetTickCount();
        states.clear(); pc = 0; chat = 0;
        for (int i = 0, n = api->num_objects(); i < n; i++) {
            uintptr_t o = api->object(i);
            if (!o || isCDO(o)) continue;
            uintptr_t c = classOf(o);
            if (c == cPS || (cPS && api->is_a(c, cPS))) states.push_back(o);
            else if (!pc && (c == cPC || api->is_a(c, cPC))) pc = o;
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
    std::lock_guard<std::mutex> g(mu);
    myX = p[0]; myY = p[1]; inVehicle = veh;
}

// ---- Steam ----
struct Steam {
    void* nm = nullptr;
    int (*send)(void*, const void* id, const void* data, uint32_t n, int flags, int ch) = nullptr;
    int (*recv)(void*, int ch, uintptr_t* out, int max) = nullptr;
    bool (*accept)(void*, const void* id) = nullptr;
    void (*release)(uintptr_t) = nullptr;
} steam;
static bool steamUp() {
    HMODULE m = GetModuleHandleW(L"steam_api64.dll");
    auto pipe = m ? (int (*)())GetProcAddress(m, "SteamAPI_GetHSteamPipe") : nullptr;
    if (!pipe || !pipe()) return false;  // asked before Steam is up, the accessors hand back nothing for good
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
struct Packet { uint32_t magic; uint8_t type, flags; uint16_t seq; uint32_t key; float x, y; uint8_t ulaw[FRAME]; };
#pragma pack(pop)
enum { P_HELLO = 1, P_VOICE = 2, P_END = 3 };
static const int HEAD = (int)offsetof(Packet, ulaw);
static void sendTo(uint64_t sid, const Packet& p, int n) {
    Identity id; id.sid = sid;
    steam.send(steam.nm, &id, &p, (uint32_t)n, 1 | 32, STEAM_CHANNEL);  // unreliable, no Nagle, restart broken sessions
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
    float s = 1, dist = 0, gate = 1, gateT = 1, fade = 0, rate = 1, crack = 0;
    int crackN = 0, hop = 0;
    Band hiss;
    Rng r;
    std::string name;
};
static std::map<uint64_t, Station> stations;  // id 1 is the radio check (parrot)
static std::vector<float> local;              // our own beeps, mixed in once
static std::vector<std::array<int16_t, FRAME>> parrotTape;
static float scope[64], txLevel;
static std::atomic<bool> talking{false}, pttPanel{false};

static float signalAt(double km) { double v = 1 - km / rangeKm; return v <= 0 ? 0 : (float)pow(v, 0.6); }
static float sample(Station& t, int i, const int16_t* f) {
    float s = t.s, h = t.r.gauss(), out = 0, noise = t.hiss.run(h) * (0.03f + 0.32f * powf(1 - s, 1.6f));
    if (t.st == Station::CHUNK || t.st == Station::TAIL) {
        float k = t.st == Station::TAIL ? t.n / (SR * 0.17f) : 1;
        return t.hiss.run(h) * (0.35f + 0.25f * (1 - s)) * (0.2f + 0.8f * k) * 0.8f;
    }
    if (t.st == Station::ROGER) {
        int at = SR * 160 / 1000 - t.n, a = SR * 70 / 1000;
        return (at < a ? tone(1200, at, a, 0.4f) : tone(900, at - a, SR * 90 / 1000, 0.4f)) * (0.5f + 0.5f * s);
    }
    float x = f ? f[i] / 32768.f : 0;
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
                char m[160]; snprintf(m, sizeof m, "CB %d  <  %s  %.1f km  S%d", channel, t.name.empty() ? "Unknown station" : t.name.c_str(), t.dist, (int)fmaxf(1, roundf(t.s * 9)));
                notice(m);
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
            if (!talking || it->first == 2) out[i] += v;  // half duplex: keying up drowns everything else (not the loopback test)
            if (t.st == Station::CHUNK && --t.n <= 0) t.st = Station::VOICE;
            else if (t.st == Station::ROGER && --t.n <= 0) { t.st = Station::TAIL; t.n = SR * 170 / 1000; }
            else if (t.st == Station::TAIL && --t.n <= 0) { t.st = Station::IDLE; t.ended = false; t.q.clear(); }
        }
        ++it;
    }
    if (open >= 2 && !talking)  // two carriers on one channel: the heterodyne
        for (int i = 0; i < FRAME; i++) out[i] += 0.05f * sinf(2 * 3.14159265f * 1100 * (now * 8 + i) / SR);
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
        for (int i = 0; i < n; i++) local.push_back(tone(f, i, n, 0.25f));
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
        Packet q = p; q.x += (float)(loopKm * 100000); received(2, q, n); return;  // the loopback test, through Steam
    }
    std::lock_guard<std::mutex> g(mu);
    double km = hypot(p.x - myX, p.y - myY) / 100000.0;
    if (!peers.count(from)) MT_LOG("station on the air: %llx, %.1f km", from, km);
    peers[from] = {GetTickCount(), (float)km};
    if (p.type == P_HELLO) {
        if (!(p.flags & 1)) { Packet r = p; r.flags = 1; r.x = (float)myX; r.y = (float)myY; sendTo(from, r, HEAD); }  // answer once
        return;
    }
    if (p.key != myKey() || (!inVehicle && !handheld)) return;
    float s = signalAt(km);
    auto it = stations.find(from);
    if (s <= 0 || (it == stations.end() && s * 100 < squelchPct)) return;  // out of range, or under the squelch
    Station& t = stations[from];
    if (t.name.empty()) { t.r.s ^= (uint32_t)from; t.rate = 0.7f + 1.3f * t.r.uni(); for (auto& pl : players) if (pl.sid == from) t.name = pl.name; if (from == 2) t.name = "Loopback"; }
    t.s = s; t.dist = (float)km; t.last = GetTickCount();
    if (p.type == P_END) { t.ended = true; t.beep = (p.flags & 2) != 0; return; }
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
    while (!api->stopping()) {
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
        if (GetTickCount() - lastHello > 3000) {
            if (!lastHello) { Packet me{MAGIC, P_HELLO, 1, 0, 0}; sendTo(mySid, me, HEAD); }  // to ourselves once: logs if Steam loops it back
            lastHello = GetTickCount();
            std::vector<uint64_t> to;
            Packet h{MAGIC, P_HELLO, 0, seq++, myKey()};
            { std::lock_guard<std::mutex> g(mu); h.x = (float)myX; h.y = (float)myY; for (auto& p : players) to.push_back(p.sid);
              for (auto it = peers.begin(); it != peers.end();) it = GetTickCount() - it->second.first > 12000 ? peers.erase(it) : ++it; }
            for (uint64_t sid : to) { Identity id; id.sid = sid; steam.accept(steam.nm, &id); sendTo(sid, h, HEAD); }
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
static void keyThread() {
    HWAVEIN wi = nullptr;
    static int16_t cap[8][FRAME];
    WAVEHDR h[8]{};
    Band band;
    float env = 0.05f;
    uint16_t seq = 0;
    bool was = false, nextDown = false, panelDown = false, keyDown = false;
    std::vector<uint64_t> to;
    while (!api->stopping()) {
        bool f = gameFocused() && !chatOpen, ctl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool key = f && !ctl && (GetAsyncKeyState(kTalk) & 0x8000);
        bool want = (key || pttPanel) && (inVehicle || handheld || parrot);
        bool nx = f && (GetAsyncKeyState(kNext) & 0x8000) != 0, pn = f && ctl && (GetAsyncKeyState(kPanel) & 0x8000) != 0;
        if (nx && !nextDown) {
            { std::lock_guard<std::mutex> g(mu); channel = ctl ? (channel + 38) % 40 + 1 : channel % 40 + 1; saveSettings();
              notice("CB " + std::to_string(channel) + "  " + FREQ[channel] + " MHz" + presetName(channel, code)); }
            localBeep({1750, channel == 9 ? 2400.f : channel == 19 ? 2000.f : 1750.f});
        }
        if (pn && !panelDown) openPanel();
        nextDown = nx; panelDown = pn;
        if (key && !want && !keyDown) { std::lock_guard<std::mutex> g(mu); notice("CB: the rig is in your vehicle. On foot: Handheld in the panel (Ctrl+Home)"); }
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
            was = true;
        }
        if (was && wi) {
            to.clear();
            Packet p{MAGIC, P_VOICE, 0, 0, myKey()};
            {
                std::lock_guard<std::mutex> g(mu);
                p.x = (float)myX; p.y = (float)myY;
                for (auto& e : peers) if (e.second.second <= rangeKm * 1.15) to.push_back(e.first);
            }
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
                txLevel = lvl;
                p.seq = seq++;
                for (uint64_t sid : to) sendTo(sid, p, sizeof p);
                if (loopKm >= 0) sendTo(mySid, p, sizeof p);
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
                Packet e{MAGIC, P_END, (uint8_t)(roger ? 2 : 0), seq++, myKey()};
                to.clear();
                { std::lock_guard<std::mutex> g(mu); e.x = (float)myX; e.y = (float)myY;
                  for (auto& pe : peers) if (pe.second.second <= rangeKm * 1.15) to.push_back(pe.first); }
                for (uint64_t sid : to) sendTo(sid, e, HEAD);
                if (loopKm >= 0) sendTo(mySid, e, HEAD);
            }
            talking = false; txLevel = 0; was = false;
            std::lock_guard<std::mutex> g(mu);
            if (parrot && !parrotTape.empty()) {  // radio check: our own over comes back as a station would hear it
                Station& t = stations[1];
                t.name = "Radio check"; t.s = parrotS / 100.f; t.dist = (float)(rangeKm * (1 - pow(t.s, 1 / 0.6)));
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
static std::string presetName(int ch, int cd) {
    static const struct { int ch; const char* n; } fixed[] = {{9, "Emergency"}, {19, "Truckers"}, {17, "Coast road"}, {14, "Towing"},
        {21, "Taxi & bus"}, {27, "Delivery"}, {33, "Racing"}, {3, "Marine"}};
    std::string all(presets.begin(), presets.end()), out;
    for (size_t at = 0; at < all.size();) {
        size_t e = all.find(';', at); if (e == std::string::npos) e = all.size();
        std::string it = all.substr(at, e - at);
        size_t a = it.find('|'), b = a == std::string::npos ? a : it.find('|', a + 1);
        if (b != std::string::npos && atoi(it.c_str() + a + 1) == ch && atoi(it.c_str() + b + 1) == cd) return "  -  " + it.substr(0, a);
        at = e + 1;
    }
    if (cd) return "  -  privacy " + std::to_string(cd);
    for (auto& f : fixed) if (f.ch == ch) return std::string("  -  ") + f.n;
    return "";
}
static std::string jesc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 32) o += c; }
    return o;
}
static std::string stateJson() {
    static std::vector<std::wstring> mics, spks;  // asked again every few seconds: a headset plugged in mid-game shows up
    static DWORD asked;
    std::lock_guard<std::mutex> g(mu);  // also guards the two lists: every request has its own thread
    if (!asked || GetTickCount() - asked > 3000) { asked = GetTickCount(); mics = devices(true); spks = devices(false); }
    char b[640];
    snprintf(b, sizeof b, "{\"ch\":%d,\"freq\":\"%s\",\"code\":%d,\"vol\":%d,\"sq\":%d,\"mic\":%d,\"roger\":%d,\"handheld\":%d,"
             "\"parrot\":%d,\"parrotS\":%d,\"range\":%.1f,\"tx\":%d,\"txLevel\":%.2f,\"inVeh\":%d,\"steam\":%d,\"me\":\"%s\",\"inSession\":%d,\"x\":%.0f,\"y\":%.0f,",
             channel, FREQ[channel], code, volume, squelchPct, micGain, roger, handheld, parrot, parrotS, rangeKm, talking ? 1 : 0,
             txLevel, inVehicle ? 1 : 0, steam.nm ? 1 : 0, jesc(myName).c_str(), (int)players.size(), myX, myY);
    std::string j = b;
    j += "\"presets\":\"" + jesc(utf8(presets)) + "\",\"micDev\":\"" + jesc(utf8(micDev)) + "\",\"spkDev\":\"" + jesc(utf8(spkDev)) + "\",\"mics\":[";
    for (auto& d : mics) j += "\"" + jesc(utf8(d)) + "\",";
    if (j.back() == ',') j.pop_back();
    j += "],\"spks\":[";
    for (auto& d : spks) j += "\"" + jesc(utf8(d)) + "\",";
    if (j.back() == ',') j.pop_back();
    j += "],\"stations\":[";
    bool first = true;
    for (auto& e : stations) {
        if (e.second.st == Station::IDLE) continue;
        snprintf(b, sizeof b, "%s{\"n\":\"%s\",\"km\":%.2f,\"s\":%.2f}", first ? "" : ",", jesc(e.second.name.empty() ? "Unknown station" : e.second.name).c_str(), e.second.dist, e.second.s);
        j += b; first = false;
    }
    j += "],\"peers\":[";
    first = true;
    for (auto& e : peers) {
        std::string n = "?"; for (auto& p : players) if (p.sid == e.first) n = p.name;
        snprintf(b, sizeof b, "%s{\"n\":\"%s\",\"km\":%.2f,\"s\":%.2f}", first ? "" : ",", jesc(n).c_str(), e.second.second, signalAt(e.second.second));
        j += b; first = false;
    }
    j += "],\"scope\":[";
    for (int i = 0; i < 64; i++) { snprintf(b, sizeof b, "%s%.3f", i ? "," : "", scope[i]); j += b; }
    return j + "]}";
}
static void apply(const std::string& q) {  // k=v&k=v
    std::lock_guard<std::mutex> g(mu);
    int wasCh = channel, wasCode = code;
    size_t at = 0;
    while (at < q.size()) {
        size_t amp = q.find('&', at); if (amp == std::string::npos) amp = q.size();
        std::string kv = q.substr(at, amp - at), k = kv.substr(0, kv.find('=')), v = kv.find('=') == std::string::npos ? "" : kv.substr(kv.find('=') + 1);
        int n = atoi(v.c_str());
        auto clamp = [](int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; };
        if (k == "ch") channel = clamp(n, 1, 40);
        else if (k == "code") code = clamp(n, 0, 99);
        else if (k == "vol") volume = clamp(n, 0, 150);
        else if (k == "sq") squelchPct = clamp(n, 0, 95);
        else if (k == "mic") micGain = clamp(n, 10, 300);
        else if (k == "roger") roger = n ? 1 : 0;
        else if (k == "handheld") handheld = n ? 1 : 0;
        else if (k == "parrot") parrot = n ? 1 : 0;
        else if (k == "parrotS") parrotS = clamp(n, 1, 100);
        else if (k == "ptt") pttPanel = n != 0;
        else if (k == "presets") presets = fromUrl(v).substr(0, 900);  // "Name|ch|code;..."
        else if (k == "micDev") micDev = fromUrl(v);
        else if (k == "spkDev") { spkDev = fromUrl(v); reopenOut = true; }
        at = amp + 1;
    }
    if (channel != wasCh || code != wasCode) notice("CB " + std::to_string(channel) + "  " + FREQ[channel] + " MHz" + presetName(channel, code));
    saveSettings();
}
static void serve(SOCKET c) {
    char buf[4096]; int n = recv(c, buf, sizeof buf - 1, 0);
    if (n <= 0) { closesocket(c); return; }
    buf[n] = 0;
    std::string req(buf), path = req.substr(req.find(' ') + 1); path = path.substr(0, path.find(' '));
    std::string body, type = "application/json";
    if (path.rfind("/state", 0) == 0) body = stateJson();
    else if (path.rfind("/set?", 0) == 0) { bool beep = path.find("ch=") != std::string::npos; apply(path.substr(5)); body = stateJson(); if (beep) localBeep({1750, 1750}); }
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
static void openPanel() {
    if (!panelPort) return;
    wchar_t url[64]; swprintf(url, 64, L"http://127.0.0.1:%d/", panelPort);
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

static bool selfCheck() {
    for (int v = -32000; v <= 32000; v += 97) { int d = ulawDec(ulawEnc((int16_t)v)) - v; if (abs(d) > 1100 || (abs(v) < 200 && abs(d) > 20)) return false; }
    if (strcmp(FREQ[19], "27.185") || strcmp(FREQ[9], "27.065") || strcmp(FREQ[40], "27.405")) return false;
    double r = rangeKm; rangeKm = 10;
    bool ok = signalAt(0) == 1 && signalAt(10) == 0 && signalAt(5) > 0.5f && signalAt(5) < 0.8f;
    rangeKm = r;
    return ok;
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
    while (!api->stopping()) {
        if (world) scanWorld();
        bool any; { std::lock_guard<std::mutex> g(mu); any = !notices.empty(); }
        if (any && gPC && api->game_thread_ready()) api->run_on_game_thread(showNotices, nullptr);
        if (gChat && api->game_thread_ready()) api->run_on_game_thread(checkChat, nullptr);
        Sleep(250);
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
    MT_LOG("CB radio: channel %d (%s MHz), code %d, range %.1f km", channel, FREQ[channel], code, rangeKm);
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
