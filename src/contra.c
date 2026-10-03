/*
 * contra: replacement libacio.dll for Konami "Contra: Evolution" (AMContra.exe, KC1 2011011801).
 *
 * The exe imports libacio.dll, so replacing it gets our code into the process (no injector, no
 * TeknoParrot). The 35 ac_io_* exports are emulated (I/O board, card reader, security plugs report OK),
 * although this build never actually calls them. From DllMain we then:
 *   - fix a Wine bug: GDI32!GetCharABCWidthsA with a Shift-JIS font and a code > 0xff makes Wine
 *     convert 2 bytes to 2 WCHARs and write 2 ABC structs for first==last, smashing the game's stack
 *     (crash at 0x43506d). We give Wine a scratch buffer and copy back only last-first+1.
 *   - emulate the COM1 serial control panel (115200 8N1): answer the init, send coin events
 *   - hook the USB HID I/O fallback (VID 055c PID b3ab) so it finds a quiet fake device
 *   - report the clock as set (host clock) and remap the DirectInput keyboard table (contra.ini)
 * Player input itself uses the game's native DirectInput keyboard/joystick path.
 */
#include <windows.h>
#include <xinput.h>
#include <setupapi.h>
#include <ddk/hidsdi.h>
#include <string.h>

#define NBTN 25

/* ---------------------------------------------------------------- logging */
static HANDLE g_log = INVALID_HANDLE_VALUE;
static char g_dir[MAX_PATH];
static char g_ini[MAX_PATH];

static void logs(const char *s)
{
    DWORD w;
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, s, lstrlenA(s), &w, NULL);
}

static void logf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    logs(buf);
}

/* ---------------------------------------------------------------- config */
/* Button byte index -> default virtual key + XInput button mask. Indices from the game's reader at
 * 0x4cdf00: P1 = bytes 0..9, P2 = 10..19 (same layout), 20..24 system. Names are best guesses,
 * confirm in test mode; everything is overridable in contra.ini [keys] b<N>=<vk> / [pad] b<N>=<mask>. */
struct btn { const char *name; int vk; int pad; int player; };
static struct btn g_btn[NBTN] = {
    /* 0 */ { "P1 Start",  '1',      XINPUT_GAMEPAD_START,          0 },
    /* 1 */ { "P1 Up",     VK_UP,    XINPUT_GAMEPAD_DPAD_UP,        0 },
    /* 2 */ { "P1 Down",   VK_DOWN,  XINPUT_GAMEPAD_DPAD_DOWN,      0 },
    /* 3 */ { "P1 Left",   VK_LEFT,  XINPUT_GAMEPAD_DPAD_LEFT,      0 },
    /* 4 */ { "P1 Right",  VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT,     0 },
    /* 5 */ { "P1 B1",     'Z',      XINPUT_GAMEPAD_A,              0 },
    /* 6 */ { "P1 B2",     'X',      XINPUT_GAMEPAD_B,              0 },
    /* 7 */ { "P1 B3",     'C',      XINPUT_GAMEPAD_X,              0 },
    /* 8 */ { "P1 B4",     'V',      XINPUT_GAMEPAD_Y,              0 },
    /* 9 */ { "P1 B5",     'B',      XINPUT_GAMEPAD_RIGHT_SHOULDER, 0 },
    /*10 */ { "P2 Start",  '2',      XINPUT_GAMEPAD_START,          1 },
    /*11 */ { "P2 Up",     'I',      XINPUT_GAMEPAD_DPAD_UP,        1 },
    /*12 */ { "P2 Down",   'K',      XINPUT_GAMEPAD_DPAD_DOWN,      1 },
    /*13 */ { "P2 Left",   'J',      XINPUT_GAMEPAD_DPAD_LEFT,      1 },
    /*14 */ { "P2 Right",  'L',      XINPUT_GAMEPAD_DPAD_RIGHT,     1 },
    /*15 */ { "P2 B1",     'A',      XINPUT_GAMEPAD_A,              1 },
    /*16 */ { "P2 B2",     'S',      XINPUT_GAMEPAD_B,              1 },
    /*17 */ { "P2 B3",     'D',      XINPUT_GAMEPAD_X,              1 },
    /*18 */ { "P2 B4",     'F',      XINPUT_GAMEPAD_Y,              1 },
    /*19 */ { "P2 B5",     'G',      XINPUT_GAMEPAD_RIGHT_SHOULDER, 1 },
    /*20 */ { "Sys 20",    0,        0,                             0 },
    /*21 */ { "Sys 21",    0,        0,                             0 },
    /*22 */ { "Service",   VK_F1,    0,                             0 },
    /*23 */ { "Sys 23",    0,        0,                             0 },
    /*24 */ { "Test",      VK_F2,    0,                             0 },
};
static int g_coin_vk[2] = { '5', '6' };
static int g_coin_pad[2] = { XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_BACK };
static int g_debug;

static void load_config(void)
{
    char key[16];
    int i;
    for (i = 0; i < NBTN; i++) {
        wsprintfA(key, "b%d", i);
        g_btn[i].vk = GetPrivateProfileIntA("keys", key, g_btn[i].vk, g_ini);
        g_btn[i].pad = GetPrivateProfileIntA("pad", key, g_btn[i].pad, g_ini);
    }
    g_coin_vk[0] = GetPrivateProfileIntA("keys", "coin1", g_coin_vk[0], g_ini);
    g_coin_vk[1] = GetPrivateProfileIntA("keys", "coin2", g_coin_vk[1], g_ini);
    g_coin_pad[0] = GetPrivateProfileIntA("pad", "coin1", g_coin_pad[0], g_ini);
    g_coin_pad[1] = GetPrivateProfileIntA("pad", "coin2", g_coin_pad[1], g_ini);
    g_debug = GetPrivateProfileIntA("general", "debug", 0, g_ini);
}

/* ---------------------------------------------------------------- input */
typedef DWORD (WINAPI *xinput_get_state_t)(DWORD, XINPUT_STATE *);
static xinput_get_state_t p_XInputGetState;
static int g_xinput_tried;

static WORD pad_buttons(int player)
{
    XINPUT_STATE st;
    WORD b;
    if (!g_xinput_tried) {
        HMODULE h;
        g_xinput_tried = 1;
        if ((h = LoadLibraryA("xinput1_3.dll")) || (h = LoadLibraryA("xinput1_4.dll")))
            p_XInputGetState = (xinput_get_state_t)GetProcAddress(h, "XInputGetState");
        logf("xinput: %s\n", p_XInputGetState ? "ok" : "unavailable");
    }
    if (!p_XInputGetState || p_XInputGetState(player, &st) != ERROR_SUCCESS) return 0;
    b = st.Gamepad.wButtons;
    /* left stick as d-pad */
    if (st.Gamepad.sThumbLX < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (st.Gamepad.sThumbLX > 16000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (st.Gamepad.sThumbLY > 16000) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (st.Gamepad.sThumbLY < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    return b;
}

static int key_down(int vk)
{
    return vk && (GetAsyncKeyState(vk) & 0x8000);
}

static int our_window_active(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static unsigned char g_in[NBTN];
static unsigned g_coinstock[2];
static int g_coin_prev[2];

static void poll_inputs(void)
{
    WORD pad[2];
    int kb = our_window_active();
    int i;
    pad[0] = pad_buttons(0);
    pad[1] = pad_buttons(1);
    for (i = 0; i < NBTN; i++) {
        int on = (kb && key_down(g_btn[i].vk)) || (g_btn[i].pad && (pad[g_btn[i].player] & g_btn[i].pad));
        if (g_debug && on != g_in[i]) logf("input %d (%s) = %d\n", i, g_btn[i].name, on);
        g_in[i] = on;
    }
    for (i = 0; i < 2; i++) {
        int on = (kb && key_down(g_coin_vk[i])) || (g_coin_pad[i] && (pad[i] & g_coin_pad[i]));
        if (on && !g_coin_prev[i]) {
            g_coinstock[i]++;
            logf("coin slot %d -> %u\n", i, g_coinstock[i]);
        }
        g_coin_prev[i] = on;
    }
}

/* ---------------------------------------------------------------- ACIO exports (cdecl) */
typedef unsigned char bool8;

bool8 acio_begin(unsigned char a, const char *ver, int *p, int flags)
{
    logf("ac_io_begin(%d, %s, %p, 0x%x)\n", a, ver ? ver : "", p, flags);
    return 1;
}
bool8 acio_begin_get_status(int a) { return 1; }
bool8 acio_is_active2(int a, int *st) { if (st) *st = 0; return 1; }
bool8 acio_is_active_device(int idx) { return 1; }
void acio_end(void) { logs("ac_io_end\n"); }
void acio_end_get_status(void) {}
void acio_update(void) { poll_inputs(); }
bool8 acio_enable_game_ios(bool8 on) { return 1; }
void acio_set_soft_watch_dog(int ms) {}
void acio_soft_watch_dog_on(void) {}
void acio_soft_watch_dog_off(void) {}
void acio_hbhi_watchdog_off(void) {}
void acio_secplug_set_encodedpasswd(const unsigned char *p, int n) {}

bool8 acio_hbhi_update_control_status_buffer(void) { return 1; }
void acio_hbhi_get_control_status_buffer(unsigned char *buf) { memcpy(buf, g_in, NBTN); }
void acio_hbhi_control_lamp_on(int m) {}
void acio_hbhi_control_lamp_off(int m) {}
void acio_hbhi_control_parallel_on(int m) {}
void acio_hbhi_control_parallel_off(int m) {}

static int slot(int s) { return s ? 1 : 0; }
bool8 acio_hbhi_lock_coincounter(int s) { return 1; }
bool8 acio_hbhi_unlock_coincounter(int s) { return 1; }
void acio_hbhi_control_coin_blocker_open(int s) {}
void acio_hbhi_control_coin_blocker_close(int s) {}
bool8 acio_hbhi_current_coinstock(int s, unsigned *n) { if (n) *n = g_coinstock[slot(s)]; return 1; }
bool8 acio_hbhi_consume_coinstock(int s, unsigned n)
{
    unsigned *c = &g_coinstock[slot(s)];
    *c = n > *c ? 0 : *c - n;
    return 1;
}

/* security plugs: check finished with status 0x12 (OK) */
static char g_softid[] = "GQKC1JAA00000000";
static char g_sysid[] = "0000000000000000";
bool8 acio_hbhi_req_secplug_check_softwareplug(const char *model) { logf("softwareplug %s\n", model); return 1; }
bool8 acio_hbhi_req_secplug_check_systemplug(void) { return 1; }
bool8 acio_hbhi_req_secplug_check_isfinished(int *st) { if (st) *st = 0x12; return 1; }
bool8 acio_hbhi_get_softwareid(char *id) { lstrcpyA(id, g_softid); return 1; }
bool8 acio_hbhi_get_systemid(char *id) { lstrcpyA(id, g_sysid); return 1; }

/* ICCA card reader: present, initialised (status 4), never reports a card */
bool8 acio_icca_cardunit_init(int idx) { return 1; }
bool8 acio_icca_cardunit_init_isfinished(int idx, int *st) { if (st) *st = 4; return 1; }
bool8 acio_icca_req_uid(int idx) { return 0; }
bool8 acio_icca_req_uid_isfinished(int idx, int *st) { if (st) *st = 0; return 0; }
bool8 acio_icca_get_uid(int idx, void *uid) { if (uid) memset(uid, 0, 8); return 0; }

/* ---------------------------------------------------------------- IAT patching / Wine fixes */
static BOOL WINAPI fix_GetCharABCWidthsA(HDC hdc, UINT first, UINT last, ABC *abc)
{
    ABC tmp[514];
    UINT n;
    if (last < first || (n = last - first + 1) > 256) return GetCharABCWidthsA(hdc, first, last, abc);
    memset(tmp, 0, sizeof(tmp));
    if (!GetCharABCWidthsA(hdc, first, last, tmp)) return FALSE;
    memcpy(abc, tmp, n * sizeof(ABC));
    return TRUE;
}

static void *patch_iat(HMODULE mod, const char *dll, const char *name, void *fn)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    if (!d->VirtualAddress) return NULL;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + d->VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA *names, *addrs;
        if (lstrcmpiA((char *)base + imp->Name, dll)) continue;
        names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old;
            void *prev;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (lstrcmpA((char *)ibn->Name, name)) continue;
            VirtualProtect(&addrs->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            prev = (void *)addrs->u1.Function;
            addrs->u1.Function = (ULONG_PTR)fn;
            VirtualProtect(&addrs->u1.Function, sizeof(void *), old, &old);
            logf("iat: %s!%s patched\n", dll, name);
            return prev;
        }
    }
    logf("iat: %s!%s NOT FOUND\n", dll, name);
    return NULL;
}

/* ---------------------------------------------------------------- USB HID I/O board emulation
 * AMContra enumerates HID interfaces (SetupDi, 0x42b150), opens the one with VID 0x055c PID 0xb3ab
 * and talks to it with overlapped WriteFile/ReadFile transactions (0x42b2f0: write N, read N; byte 0
 * of each buffer is the report id). We present one fake device and answer in-process. */
#define FAKE_DEVINFO ((HANDLE)0x13370001)
static const char g_hid_path[] = "\\\\?\\hid#vid_055c&pid_b3ab#contra#{4d1e55b2-f16f-11cf-88cb-001111000030}";
static HANDLE g_hid = INVALID_HANDLE_VALUE;
static HANDLE g_com = INVALID_HANDLE_VALUE;
static unsigned g_com_opens, g_com_nw, g_com_nr;
static unsigned g_hid_nw, g_hid_nr;
static unsigned char g_hid_last[64];
static DWORD g_hid_last_len;

static void hexdump(const char *tag, const unsigned char *b, DWORD n)
{
    char line[400];
    int o = wsprintfA(line, "%s %lu:", tag, n);
    DWORD i;
    for (i = 0; i < n && i < 64; i++) o += wsprintfA(line + o, " %02x", b[i]);
    lstrcpyA(line + o, "\n");
    logs(line);
}

static HANDLE WINAPI h_SetupDiGetClassDevsA(const GUID *g, const char *e, HWND w, DWORD f)
{
    logs("SetupDiGetClassDevsA -> fake\n");
    return FAKE_DEVINFO;
}
static BOOL WINAPI h_SetupDiEnumDeviceInterfaces(HANDLE di, void *dev, const GUID *g, DWORD idx, SP_DEVICE_INTERFACE_DATA *d)
{
    if (di != FAKE_DEVINFO || idx > 0) { SetLastError(ERROR_NO_MORE_ITEMS); return FALSE; }
    d->InterfaceClassGuid = *g;
    d->Flags = SPINT_ACTIVE;
    d->Reserved = 0;
    return TRUE;
}
static BOOL WINAPI h_SetupDiGetDeviceInterfaceDetailA(HANDLE di, SP_DEVICE_INTERFACE_DATA *d,
    SP_DEVICE_INTERFACE_DETAIL_DATA_A *det, DWORD size, DWORD *req, void *dev)
{
    DWORD need = FIELD_OFFSET(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) + sizeof(g_hid_path);
    if (req) *req = need;
    if (!det || size < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    lstrcpyA(det->DevicePath, g_hid_path);
    return TRUE;
}
static BOOL WINAPI h_SetupDiDestroyDeviceInfoList(HANDLE di) { return TRUE; }

static BOOLEAN WINAPI h_HidD_GetAttributes(HANDLE h, HIDD_ATTRIBUTES *a)
{
    if (h != g_hid) return FALSE;
    a->Size = sizeof(*a);
    a->VendorID = 0x055c;
    a->ProductID = 0xb3ab;
    a->VersionNumber = 0x0100;
    return TRUE;
}
static BOOLEAN WINAPI h_HidD_SetNumInputBuffers(HANDLE h, ULONG n) { return TRUE; }
static BOOLEAN WINAPI h_HidD_FlushQueue(HANDLE h) { return TRUE; }

typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileA_t o_CreateFileA;
static HANDLE WINAPI h_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE tmpl)
{
    if (name && !lstrcmpiA(name, g_hid_path)) {
        if (g_hid == INVALID_HANDLE_VALUE) g_hid = CreateEventA(NULL, TRUE, FALSE, NULL);
        else DuplicateHandle(GetCurrentProcess(), g_hid, GetCurrentProcess(), &g_hid, 0, FALSE, DUPLICATE_SAME_ACCESS);
        logf("CreateFileA(usbio) -> %p\n", g_hid);
        return g_hid;
    }
    if (name && (!lstrcmpiA(name, "COM1") || !lstrcmpiA(name, "\\\\.\\COM1"))) {
        if (g_com == INVALID_HANDLE_VALUE) g_com = CreateEventA(NULL, TRUE, FALSE, NULL);
        else DuplicateHandle(GetCurrentProcess(), g_com, GetCurrentProcess(), &g_com, 0, FALSE, DUPLICATE_SAME_ACCESS);
        if (g_com_opens++ < 5) logf("CreateFileA(COM1) -> %p\n", g_com);
        return g_com;
    }
    if (g_debug) logf("CreateFileA(%s)\n", name ? name : "(null)");
    return o_CreateFileA(name, acc, share, sa, disp, fl, tmpl);
}

static void complete(OVERLAPPED *ov, DWORD n, DWORD *done)
{
    if (done) *done = n;
    if (ov) {
        ov->Internal = 0;
        ov->InternalHigh = n;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
}

/* build the board's answer to the last command */
static DWORD hid_reply(unsigned char *buf, DWORD len)
{
    memset(buf, 0, len);
    return len;
}

/* serial I/O board on COM1: game writes requests, we queue the board's answer */
static unsigned char g_rx[4096];
static DWORD g_rx_len;
static void com_queue(const unsigned char *b, DWORD n)
{
    if (g_rx_len + n > sizeof(g_rx)) g_rx_len = 0;
    memcpy(g_rx + g_rx_len, b, n);
    g_rx_len += n;
}
static void com_write(const unsigned char *b, DWORD n)
{
    (void)com_queue;
}
/* coin keys -> board coin events: 0xB0 + code with "pressed" bit (0x04) clear; code 0x0a = slot 1,
 * 0x09 = slot 2 (code->bit table 0x583d10 bit 0, handled at 0x4c62c0 -> 0x4c5840 adds the coin) */
static void com_poll_coins(void)
{
    static const unsigned char codes[2] = { 0x0a, 0x09 };
    WORD pad[2];
    int kb = our_window_active(), i;
    pad[0] = pad_buttons(0);
    pad[1] = pad_buttons(1);
    for (i = 0; i < 2; i++) {
        int on = (kb && key_down(g_coin_vk[i])) || (g_coin_pad[i] && (pad[i] & g_coin_pad[i]));
        if (on && !g_coin_prev[i]) {
            unsigned char ev[2] = { 0xb0, codes[i] };
            com_queue(ev, 2);
            logf("coin slot %d\n", i + 1);
        }
        g_coin_prev[i] = on;
    }
}

static DWORD com_read(unsigned char *b, DWORD n)
{
    com_poll_coins();
    if (n > g_rx_len) n = g_rx_len;
    memcpy(b, g_rx, n);
    memmove(g_rx, g_rx + n, g_rx_len - n);
    g_rx_len -= n;
    return n;
}
static BOOL WINAPI h_GetCommState(HANDLE h, DCB *d)
{
    if (h != g_com) return GetCommState(h, d);
    memset(d, 0, sizeof(*d));
    d->DCBlength = sizeof(*d);
    d->BaudRate = 115200;
    d->ByteSize = 8;
    d->fBinary = 1;
    return TRUE;
}
static BOOL WINAPI h_SetCommState(HANDLE h, DCB *d)
{
    if (h != g_com) return SetCommState(h, d);
    if (g_com_opens <= 2) logf("SetCommState baud=%lu bits=%d parity=%d stop=%d\n", d->BaudRate, d->ByteSize, d->Parity, d->StopBits);
    return TRUE;
}
static BOOL WINAPI h_SetupComm(HANDLE h, DWORD i, DWORD o) { return h == g_com ? TRUE : SetupComm(h, i, o); }
static BOOL WINAPI h_PurgeComm(HANDLE h, DWORD f) { if (h != g_com) return PurgeComm(h, f); if (f & PURGE_RXCLEAR) g_rx_len = 0; return TRUE; }
static BOOL WINAPI h_SetCommTimeouts(HANDLE h, COMMTIMEOUTS *t) { return h == g_com ? TRUE : SetCommTimeouts(h, t); }
static BOOL WINAPI h_ClearCommBreak(HANDLE h) { return h == g_com ? TRUE : ClearCommBreak(h); }
static BOOL WINAPI h_ClearCommError(HANDLE h, DWORD *err, COMSTAT *st)
{
    if (h != g_com) return ClearCommError(h, err, st);
    if (err) *err = 0;
    if (st) { memset(st, 0, sizeof(*st)); st->cbInQue = g_rx_len; }
    return TRUE;
}

typedef BOOL (WINAPI *RW_t)(HANDLE, void *, DWORD, DWORD *, OVERLAPPED *);
static RW_t o_ReadFile, o_WriteFile;
static BOOL WINAPI h_WriteFile(HANDLE h, void *buf, DWORD len, DWORD *done, OVERLAPPED *ov)
{
    if (h == g_com && h != INVALID_HANDLE_VALUE) {
        if (g_com_nw++ < 400 || g_debug > 1) hexdump("COM W", buf, len);
        com_write(buf, len);
        complete(ov, len, done);
        return TRUE;
    }
    if (h != g_hid || h == INVALID_HANDLE_VALUE) return o_WriteFile(h, buf, len, done, ov);
    g_hid_last_len = len < sizeof(g_hid_last) ? len : sizeof(g_hid_last);
    memcpy(g_hid_last, buf, g_hid_last_len);
    if (g_hid_nw++ < 300 || g_debug) hexdump("W", buf, len);
    complete(ov, len, done);
    return TRUE;
}
static BOOL WINAPI h_ReadFile(HANDLE h, void *buf, DWORD len, DWORD *done, OVERLAPPED *ov)
{
    DWORD n;
    if (h == g_com && h != INVALID_HANDLE_VALUE) {
        n = com_read(buf, len);
        if (n && (g_com_nr++ < 400 || g_debug > 1)) hexdump("COM R", buf, n);
        complete(ov, n, done);
        return TRUE;
    }
    if (h != g_hid || h == INVALID_HANDLE_VALUE) return o_ReadFile(h, buf, len, done, ov);
    poll_inputs();
    n = hid_reply(buf, len);
    if (g_hid_nr++ < 300 || g_debug) hexdump("R", buf, n);
    complete(ov, n, done);
    return TRUE;
}

static void hook_usbio(HMODULE exe)
{
    patch_iat(exe, "SETUPAPI.dll", "SetupDiGetClassDevsA", h_SetupDiGetClassDevsA);
    patch_iat(exe, "SETUPAPI.dll", "SetupDiEnumDeviceInterfaces", h_SetupDiEnumDeviceInterfaces);
    patch_iat(exe, "SETUPAPI.dll", "SetupDiGetDeviceInterfaceDetailA", h_SetupDiGetDeviceInterfaceDetailA);
    patch_iat(exe, "SETUPAPI.dll", "SetupDiDestroyDeviceInfoList", h_SetupDiDestroyDeviceInfoList);
    patch_iat(exe, "HID.DLL", "HidD_GetAttributes", h_HidD_GetAttributes);
    patch_iat(exe, "HID.DLL", "HidD_SetNumInputBuffers", h_HidD_SetNumInputBuffers);
    patch_iat(exe, "HID.DLL", "HidD_FlushQueue", h_HidD_FlushQueue);
    patch_iat(exe, "KERNEL32.dll", "GetCommState", h_GetCommState);
    patch_iat(exe, "KERNEL32.dll", "SetCommState", h_SetCommState);
    patch_iat(exe, "KERNEL32.dll", "SetupComm", h_SetupComm);
    patch_iat(exe, "KERNEL32.dll", "PurgeComm", h_PurgeComm);
    patch_iat(exe, "KERNEL32.dll", "SetCommTimeouts", h_SetCommTimeouts);
    patch_iat(exe, "KERNEL32.dll", "ClearCommBreak", h_ClearCommBreak);
    patch_iat(exe, "KERNEL32.dll", "ClearCommError", h_ClearCommError);
    o_CreateFileA = patch_iat(exe, "KERNEL32.dll", "CreateFileA", h_CreateFileA);
    o_ReadFile = patch_iat(exe, "KERNEL32.dll", "ReadFile", h_ReadFile);
    o_WriteFile = patch_iat(exe, "KERNEL32.dll", "WriteFile", h_WriteFile);
}

/* ---------------------------------------------------------------- game patches
 * Clock: the game keeps a "clock set" flag (setting 0x36) + an offset vs the PC clock (0x37/0x38),
 * written by the test-menu clock screen (0x456da0). Unset -> boot error 5-2503 "clock un-setting up".
 * The host clock is right on a PC, so [general] clock_set=1 (default) reports the flag as set by
 * redirecting the settings getter (0x463a50: return settings[idx]). */
static int g_clock_set;
static int **g_settings = (int **)0x5e1ccc;
static int __cdecl settings_get(int idx)
{
    if (idx == 0x36 && g_clock_set) return 1;
    return (*g_settings)[idx];
}

static int write_jmp(void *at, void *to, const unsigned char *expect, int n)
{
    DWORD old;
    unsigned char *p = at;
    if (memcmp(p, expect, n)) { logf("patch %p: unexpected bytes, skipped\n", at); return 0; }
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xe9;
    *(DWORD *)(p + 1) = (DWORD)to - (DWORD)p - 5;
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

/* debug call counters: [general] count=1 hooks a few game functions and logs calls/second */
static volatile LONG g_cnt[4];
static void *g_tramp[4];
#define COUNTER_STUB(n) \
    __attribute__((naked)) static void cnt_stub##n(void) \
    { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_cnt[n]), "m"(g_tramp[n])); }
COUNTER_STUB(0)
/* stub 1 also probes ButtonMan action flags each test-menu frame */
void __cdecl probe_buttons(void);
__attribute__((naked)) static void cnt_stub1(void)
{
    __asm__ volatile("lock incl %0\n\tpushal\n\tcall _probe_buttons\n\tpopal\n\tjmp *%1"
                     :: "m"(g_cnt[1]), "m"(g_tramp[1]));
}
COUNTER_STUB(2)
COUNTER_STUB(3)
static void *g_stubs[4] = { cnt_stub0, cnt_stub1, cnt_stub2, cnt_stub3 };
static const char *g_cnt_name[4];
static unsigned g_frame;
void __cdecl probe_buttons(void)
{
    static const int acts[] = { 6, 7, 8, 9, 20, 38, 39, 44, 45 };
    unsigned char *bm = *(unsigned char **)0x5e1568;
    char line[300];
    int o, i, any = 0;
    g_frame++;
    if (!bm) return;
    o = wsprintfA(line, "f%u", g_frame);
    for (i = 0; i < 9; i++) {
        unsigned char *r = bm + (acts[i] + 0x3a) * 16;
        if (r[0] | r[1] | r[2]) any = 1;
        o += wsprintfA(line + o, " a%d=%d%d%d", acts[i], r[0], r[1], r[2]);
    }
    if (any) { lstrcpyA(line + o, "\n"); logs(line); }
}

/* hook a function whose first 5 bytes are whole instructions */
static void hook_count(int n, DWORD addr, const char *name)
{
    unsigned char *t = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(t, (void *)addr, 5);
    t[5] = 0xe9;
    *(DWORD *)(t + 6) = addr + 5 - (DWORD)(t + 10);
    g_tramp[n] = t;
    g_cnt_name[n] = name;
    write_jmp((void *)addr, g_stubs[n], t, 5);
}

static DWORD WINAPI counter_thread(void *arg)
{
    for (;;) {
        LONG v[4];
        int i;
        Sleep(1000);
        for (i = 0; i < 4; i++) v[i] = InterlockedExchange(&g_cnt[i], 0);
        logf("calls/s: %s=%ld %s=%ld %s=%ld\n", g_cnt_name[0], v[0], g_cnt_name[1], v[1],
             g_cnt_name[2] ? g_cnt_name[2] : "-", v[2]);
    }
    return 0;
}

/* Keyboard map: CButtonMan's {DIK code, action} table at 0x55cea8..0x55cfd4 (.rdata).
 * Actions: 0/1 start P1/P2, 2/3 left, 4/5 right, 6/7 down, 8/9 up, 10-17 buttons (even P1, odd P2),
 * 20 test, 21 service, 38-54 test-menu navigation. [keyboard] aN=<DIK> overrides every entry of action N. */
#define KEYTAB_START 0x55cea8
#define KEYTAB_END 0x55cfd4
static const struct { int action, dik; } g_keymap_default[] = {
    { 0, 0x02 },   /* P1 start: 1 (was Enter) */
    { 1, 0x03 },   /* P2 start: 2 (was numpad Enter) */
    { 12, 0x12 },  /* P1 button 2: E (was J) */
    { 14, 0x13 },  /* P1 button 3: R (was K) */
    { 10, 0x14 },  /* P1 button 1: T (was U) */
    { 13, 0x21 },  /* P2 button 2: F (was numpad 1) */
    { 15, 0x22 },  /* P2 button 3: G (was numpad 2) */
    { 11, 0x23 },  /* P2 button 1: H (was numpad +) */
    { 17, 0xc7 },  /* P2 button 4: Home (was numpad -) */
    { 20, 0x3c },  /* test: F2 (was F1) */
    { 21, 0x3b },  /* service: F1 (was Space) */
};

static void remap_keyboard(void)
{
    DWORD *tab = (DWORD *)KEYTAB_START, old;
    int n = (KEYTAB_END - KEYTAB_START) / 8, i, a;
    char key[8];
    if (tab[0] != 0x11 || tab[1] != 8) { logs("keymap: unexpected table, skipped\n"); return; }
    VirtualProtect(tab, n * 8, PAGE_READWRITE, &old);
    for (a = 0; a < 64; a++) {
        int dik = -1;
        for (i = 0; i < (int)(sizeof(g_keymap_default) / sizeof(g_keymap_default[0])); i++)
            if (g_keymap_default[i].action == a) dik = g_keymap_default[i].dik;
        wsprintfA(key, "a%d", a);
        dik = GetPrivateProfileIntA("keyboard", key, dik, g_ini);
        if (dik < 0) continue;
        for (i = 0; i < n; i++)
            if (tab[i * 2 + 1] == (DWORD)a && tab[i * 2]) {
                logf("keymap: action %d DIK 0x%02lx -> 0x%02x\n", a, tab[i * 2], dik);
                tab[i * 2] = dik;
            }
    }
    VirtualProtect(tab, n * 8, old, &old);
}

static void patch_game(void)
{
    static const unsigned char getter[] = { 0x8b, 0x44, 0x24, 0x04, 0x8b, 0x0d, 0xcc, 0x1c, 0x5e, 0x00 };
    g_clock_set = GetPrivateProfileIntA("general", "clock_set", 1, g_ini);
    if (g_clock_set && write_jmp((void *)0x463a50, settings_get, getter, sizeof(getter)))
        logs("patch: clock reported as set (host clock)\n");
    remap_keyboard();
    if (GetPrivateProfileIntA("general", "count", 0, g_ini)) {
        hook_count(0, 0x439e60, "ButtonMan");
        hook_count(1, 0x4629e0, "TestMenuInput");
        hook_count(2, 0x43a740, "DebugInput");
        CloseHandle(CreateThread(NULL, 0, counter_thread, NULL, 0, NULL));
    }
}

/* debug: [general] sample=N -> every N seconds log the main thread's EIP and exe return addresses */
static DWORD g_main_tid;
static DWORD WINAPI sampler(void *arg)
{
    int period = (int)(INT_PTR)arg;
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_main_tid);
    for (;;) {
        CONTEXT c;
        DWORD *sp, i;
        char line[600];
        int o, n = 0;
        Sleep(period * 1000);
        if (SuspendThread(t) == (DWORD)-1) break;
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        GetThreadContext(t, &c);
        o = wsprintfA(line, "sample eip=%08lx:", c.Eip);
        sp = (DWORD *)c.Esp;
        for (i = 0; i < 4096 && n < 24; i++) {
            MEMORY_BASIC_INFORMATION mi;
            if (((ULONG_PTR)&sp[i] & 0xfff) == 0 &&
                (!VirtualQuery(&sp[i], &mi, sizeof(mi)) || mi.State != MEM_COMMIT)) break;
            if (sp[i] >= 0x401000 && sp[i] < 0x557000) { o += wsprintfA(line + o, " %lx", sp[i]); n++; }
        }
        ResumeThread(t);
        lstrcpyA(line + o, "\n");
        logs(line);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH];
        char *p;
        int i;
        HMODULE exe = GetModuleHandleA(NULL);
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(exe, g_dir, MAX_PATH);
        if ((p = strrchr(g_dir, '\\'))) p[1] = 0;
        wsprintfA(g_ini, "%scontra.ini", g_dir);
        wsprintfA(path, "%scontra.log", g_dir);
        g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        logs("contra loader (libacio replacement)\n");
        load_config();
        patch_iat(exe, "GDI32.dll", "GetCharABCWidthsA", fix_GetCharABCWidthsA);
        hook_usbio(exe);
        patch_game();
        g_main_tid = GetCurrentThreadId();
        if ((i = GetPrivateProfileIntA("general", "sample", 0, g_ini)) > 0)
            CloseHandle(CreateThread(NULL, 0, sampler, (void *)(INT_PTR)i, 0, NULL));
    }
    return TRUE;
}
