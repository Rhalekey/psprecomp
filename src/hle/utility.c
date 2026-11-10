/* psprecomp -- sceUtility: the system's dialogs.
 *
 * On hardware these draw over the game. Here nothing is drawn yet: each dialog
 * walks through the states a game polls for and then reports having closed. That
 * is enough to get past one, not to use one, and each is logged so it is visible
 * when a game has asked for something the player never saw. A message answers yes
 * to whatever it asked. The list of saves and the keyboard ask the host (psp_savedata_set_chooser,
 * psp_utility_set_osk_handler).
 *
 * Saves do their work for real, in savedata.c, and screenshots through the host's handler.
 */

#include "psprecomp/hle.h"
#include "savedata.h"

#include <stdio.h>
#include <string.h>

/* The states a game sees through *GetStatus. */
#define ST_NONE     0
#define ST_INIT     1
#define ST_VISIBLE  2
#define ST_QUIT     3
#define ST_FINISHED 4

#define DIALOG_KINDS 5
#define KIND_MSGDIALOG 0
#define KIND_SAVEDATA 1
#define KIND_OSK 2
#define KIND_SCREENSHOT 4

/* pspUtilityMsgDialogParams */
#define MSG_TEXT 0x3Cu
#define MSG_BUTTON 0x240u
#define MSG_BUTTON_YES 1u

/* Every dialog's parameters begin with this block, which holds its result. */
#define PARAM_RESULT 0x1Cu

static int g_state[DIALOG_KINDS];
static void (*g_take_screenshot)(void);
static psp_osk_fn g_osk;

void psp_utility_set_screenshot_handler(void (*take)(void)) { g_take_screenshot = take; }
void psp_utility_set_osk_handler(psp_osk_fn fn) { g_osk = fn; }

/* SceUtilityOskParams: after the common block, how many fields and where they are. Each field,
 * SceUtilityOskData, has its description, the text it starts with, and room for the result. */
#define OSK_COUNT 0x30u
#define OSK_DATA 0x34u
#define OSKD_DESC 28u
#define OSKD_INTEXT 32u
#define OSKD_OUTLENGTH 36u
#define OSKD_OUTTEXT 40u
#define OSKD_RESULT 44u
#define OSKD_OUTLIMIT 48u
#define OSK_RESULT_UNCHANGED 0u
#define OSK_RESULT_CANCELLED 1u
#define OSK_RESULT_CHANGED 2u
#define OSK_MOST 256

static void read_utf16(uint32_t at, uint16_t *out, int cap) {
    int n = 0;
    while (at && n + 1 < cap) {
        const uint16_t c = psp_read16(at + (uint32_t)n * 2);
        if (!c) break;
        out[n++] = c;
    }
    out[n] = 0;
}

/* The keyboard's one field (the game asks for one at a time): the host's text, or what it began with. */
static void run_osk(uint32_t param) {
    const uint32_t data = psp_read32(param + OSK_DATA);
    if (!psp_read32(param + OSK_COUNT) || !data) return;
    uint16_t description[OSK_MOST], initial[OSK_MOST], out[OSK_MOST];
    read_utf16(psp_read32(data + OSKD_DESC), description, OSK_MOST);
    read_utf16(psp_read32(data + OSKD_INTEXT), initial, OSK_MOST);
    const uint32_t room = psp_read32(data + OSKD_OUTLENGTH), outtext = psp_read32(data + OSKD_OUTTEXT);
    int limit = (int)psp_read32(data + OSKD_OUTLIMIT);
    if (limit <= 0 || limit > (int)room - 1) limit = (int)room - 1;
    if (limit > OSK_MOST - 1) limit = OSK_MOST - 1;
    if (!outtext || limit <= 0) return;
    int entered = 0;
    if (g_osk) {
        memset(out, 0, sizeof out);
        entered = g_osk(description, initial, limit, out);
    }
    const uint16_t *text = entered ? out : initial;
    int n = 0;
    for (; n < limit && text[n]; n++) psp_write16(outtext + (uint32_t)n * 2, text[n]);
    psp_write16(outtext + (uint32_t)n * 2, 0);
    psp_write32(data + OSKD_RESULT, entered ? OSK_RESULT_CHANGED : g_osk ? OSK_RESULT_CANCELLED : OSK_RESULT_UNCHANGED);
    fprintf(stderr, "utility: keyboard %s\n", entered ? "gave a text" : g_osk ? "backed out of" : "closed, nothing shown");
}

void psp_utility_init(void) { memset(g_state, 0, sizeof g_state); }

/* The save's screen or the keyboard: while either is open, the host may be showing its own screen
 * over the game's window, which the renderer must then not draw over. */
int psp_utility_savedata_open(void) { return g_state[KIND_SAVEDATA] != 0 || g_state[KIND_OSK] != 0; }

static void dialog_init(int kind, const char *name) {
    const uint32_t param = psp_arg(0);
    uint32_t result = 0;
    /* Open before the host's screen shows, so that the renderer leaves the window to it. */
    g_state[kind] = ST_INIT;
    if (kind == KIND_SAVEDATA) {
        result = psp_savedata_run(param);
    } else if (kind == KIND_MSGDIALOG) {
        char text[128];
        psp_str(param + MSG_TEXT, text, sizeof text);
        fprintf(stderr, "utility: message not shown, answering yes: %s\n", text);
        psp_write32(param + MSG_BUTTON, MSG_BUTTON_YES);
    } else if (kind == KIND_OSK) {
        run_osk(param);
    } else if (kind == KIND_SCREENSHOT && g_take_screenshot) {
        /* The picture is the host's to keep, on the player's computer; the game sees it taken. */
        fprintf(stderr, "utility: screenshot asked for\n");
        g_take_screenshot();
    } else {
        fprintf(stderr, "utility: %s opened (nothing is shown)\n", name);
    }
    psp_write32(param + PARAM_RESULT, result);
    g_state[kind] = ST_INIT;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void dialog_status(int kind) {
    const int s = g_state[kind];
    if (s == ST_INIT) g_state[kind] = ST_VISIBLE;
    else if (s == ST_FINISHED) g_state[kind] = ST_NONE;
    psp_ret((uint32_t)s);
}

static void dialog_update(int kind) {
    if (g_state[kind] == ST_VISIBLE) g_state[kind] = ST_QUIT;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void dialog_shutdown(int kind) {
    if (g_state[kind] == ST_QUIT) g_state[kind] = ST_FINISHED;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_MsgDialog_init(void)     { dialog_init(0, "MsgDialog"); }
static void hle_MsgDialog_shutdown(void) { dialog_shutdown(0); }
static void hle_MsgDialog_update(void)   { dialog_update(0); }
static void hle_MsgDialog_status(void)   { dialog_status(0); }
static void hle_Savedata_init(void)     { dialog_init(1, "Savedata"); }
static void hle_Savedata_shutdown(void) { dialog_shutdown(1); }
static void hle_Savedata_update(void)   { dialog_update(1); }
static void hle_Savedata_status(void)   { dialog_status(1); }
static void hle_Osk_init(void)     { dialog_init(2, "Osk"); }
static void hle_Osk_shutdown(void) { dialog_shutdown(2); }
static void hle_Osk_update(void)   { dialog_update(2); }
static void hle_Osk_status(void)   { dialog_status(2); }
static void hle_Netconf_init(void)     { dialog_init(3, "Netconf"); }
static void hle_Netconf_shutdown(void) { dialog_shutdown(3); }
static void hle_Netconf_update(void)   { dialog_update(3); }
static void hle_Netconf_status(void)   { dialog_status(3); }
static void hle_Screenshot_init(void)     { dialog_init(4, "Screenshot"); }
static void hle_Screenshot_shutdown(void) { dialog_shutdown(4); }
static void hle_Screenshot_update(void)   { dialog_update(4); }
static void hle_Screenshot_status(void)   { dialog_status(4); }

static void hle_ok(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* (id, int *out): the console's settings, set to a Japanese console's defaults. */
#define PARAM_TIMEZONE 6
#define TIMEZONE_JAPAN 540

static void hle_GetParamInt(void) {
    const uint32_t id = psp_arg(0), out = psp_arg(1);
    psp_write32(out, id == PARAM_TIMEZONE ? TIMEZONE_JAPAN : 0);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, char *out, int len): the only string setting is the nickname. */
static void hle_GetParamString(void) {
    static const char NAME[] = "Player";
    const uint32_t out = psp_arg(1), len = psp_arg(2);
    for (uint32_t i = 0; i < sizeof NAME && i < len; i++) psp_write8(out + i, (uint8_t)NAME[i]);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_utility_register(void) {
    psp_hle_register(0x2AD8E239, "sceUtility", "sceUtilityMsgDialogInitStart", hle_MsgDialog_init);
    psp_hle_register(0x67AF3428, "sceUtility", "sceUtilityMsgDialogShutdownStart", hle_MsgDialog_shutdown);
    psp_hle_register(0x95FC253B, "sceUtility", "sceUtilityMsgDialogUpdate", hle_MsgDialog_update);
    psp_hle_register(0x9A1C91D7, "sceUtility", "sceUtilityMsgDialogGetStatus", hle_MsgDialog_status);
    psp_hle_register(0x50C4CD57, "sceUtility", "sceUtilitySavedataInitStart", hle_Savedata_init);
    psp_hle_register(0x9790B33C, "sceUtility", "sceUtilitySavedataShutdownStart", hle_Savedata_shutdown);
    psp_hle_register(0xD4B95FFB, "sceUtility", "sceUtilitySavedataUpdate", hle_Savedata_update);
    psp_hle_register(0x8874DBE0, "sceUtility", "sceUtilitySavedataGetStatus", hle_Savedata_status);
    psp_hle_register(0xF6269B82, "sceUtility", "sceUtilityOskInitStart", hle_Osk_init);
    psp_hle_register(0x3DFAEBA9, "sceUtility", "sceUtilityOskShutdownStart", hle_Osk_shutdown);
    psp_hle_register(0x4B85C861, "sceUtility", "sceUtilityOskUpdate", hle_Osk_update);
    psp_hle_register(0xF3F76017, "sceUtility", "sceUtilityOskGetStatus", hle_Osk_status);
    psp_hle_register(0x4DB1E739, "sceUtility", "sceUtilityNetconfInitStart", hle_Netconf_init);
    psp_hle_register(0xF88155F6, "sceUtility", "sceUtilityNetconfShutdownStart", hle_Netconf_shutdown);
    psp_hle_register(0x91E70E35, "sceUtility", "sceUtilityNetconfUpdate", hle_Netconf_update);
    psp_hle_register(0x6332AA39, "sceUtility", "sceUtilityNetconfGetStatus", hle_Netconf_status);
    psp_hle_register(0x0251B134, "sceUtility", "sceUtilityScreenshotInitStart", hle_Screenshot_init);
    psp_hle_register(0xF9E0008C, "sceUtility", "sceUtilityScreenshotShutdownStart", hle_Screenshot_shutdown);
    psp_hle_register(0xAB083EA9, "sceUtility", "sceUtilityScreenshotUpdate", hle_Screenshot_update);
    psp_hle_register(0xD81957B7, "sceUtility", "sceUtilityScreenshotGetStatus", hle_Screenshot_status);
    psp_hle_register(0x2A2B3DE0, "sceUtility", "sceUtilityLoadModule", hle_ok);
    psp_hle_register(0xE49BFE92, "sceUtility", "sceUtilityUnloadModule", hle_ok);
    psp_hle_register(0xA5DA2406, "sceUtility", "sceUtilityGetSystemParamInt", hle_GetParamInt);
    psp_hle_register(0x34B78343, "sceUtility", "sceUtilityGetSystemParamString", hle_GetParamString);
}
