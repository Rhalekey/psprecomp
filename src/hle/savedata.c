/* psprecomp -- sceUtilitySavedata: the part that keeps the data.
 *
 * Saves are ordinary files under ms0:/PSP/SAVEDATA/<game><save>/, as on a Memory
 * Stick, except that nothing is encrypted: the game's data file is exactly what it
 * passed in. The description the system would show goes in SFO.BIN.
 *
 * There is no list screen yet. Where a player would choose a slot, a load takes the
 * first slot holding data and a save takes that same slot, or else the first one.
 *
 * Structure offsets are from the pspsdk's psputility_savedata.h.
 */

#include "psprecomp/hle.h"
#include "savedata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#define remove_dir(path) _rmdir(path)
#else
#include <unistd.h>
#define make_dir(path) mkdir(path, 0777)
#define remove_dir(path) rmdir(path)
#endif

/* SceUtilitySavedataParam */
#define P_RESULT    0x1Cu
#define P_MODE      0x30u
#define P_GAME      0x3Cu
#define P_SAVE      0x4Cu
#define P_NAMELIST  0x60u
#define P_FILE      0x64u
#define P_DATA      0x74u
#define P_DATA_CAP  0x78u
#define P_DATA_SIZE 0x7Cu
#define P_SFO       0x80u
#define P_FILES     0x584u   /* icon0, icon1, pic1, snd0: { buf, bufSize, size, - } */
#define P_MSFREE    0x5D0u
#define P_MSDATA    0x5D4u
#define P_USED      0x5D8u

#define SFO_SIZE    0x504u
#define FILE_ENTRY  16u
#define NAME_SIZE   20u
#define MAX_SLOTS   1000
#define MAX_FILE    (16u * 1024u * 1024u)

enum {
    MODE_AUTOLOAD = 0, MODE_AUTOSAVE, MODE_LOAD, MODE_SAVE, MODE_LISTLOAD, MODE_LISTSAVE,
    MODE_LISTDELETE, MODE_LISTALLDELETE, MODE_SIZES, MODE_AUTODELETE, MODE_DELETE,
    MODE_LIST, MODE_FILES, MODE_MAKEDATASECURE, MODE_MAKEDATA, MODE_READDATASECURE,
    MODE_READDATA, MODE_WRITEDATASECURE, MODE_WRITEDATA, MODE_ERASESECURE, MODE_ERASE,
    MODE_DELETEDATA, MODE_GETSIZE
};

#define ERROR_LOAD_NO_DATA 0x80110307u
#define ERROR_SAVE_FAILED  0x8011038Bu   /* the stick could not be written */

static psp_savedata_event_fn g_on_event;

void psp_savedata_set_event_handler(psp_savedata_event_fn fn) { g_on_event = fn; }

static void report_event(int event) {
    if (g_on_event) g_on_event(event);
}

static const char *const EXTRA_FILE[4] = { "ICON0.PNG", "ICON1.PMF", "PIC1.PNG", "SND0.AT3" };

/* The save's folder on the host, for a name read out of guest memory. */
static void slot_dir(uint32_t param, const char *save, char *out, size_t cap) {
    char game[14], guest[128];
    psp_str(param + P_GAME, game, sizeof game);
    snprintf(guest, sizeof guest, "ms0:/PSP/SAVEDATA/%s%s", game, save);
    psp_io_host_path(guest, out, cap);
}

static int slot_exists(uint32_t param, const char *save) {
    char dir[1024];
    struct stat st;
    slot_dir(param, save, dir, sizeof dir);
    return stat(dir, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static void make_dirs(const char *path) {
    char partial[1024];
    snprintf(partial, sizeof partial, "%s", path);
    for (char *p = partial + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        const char c = *p;
        *p = '\0';
        make_dir(partial);
        *p = c;
    }
    make_dir(partial);
}

/* Written to the side and moved over, so a crash mid-save keeps the old file. */
static int write_file(const char *dir, const char *name, uint32_t addr, uint32_t size) {
    char path[1200], tmp[1210];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    const uint8_t *src = size ? (const uint8_t *)psp_mem_ptr(addr, size) : NULL;
    if (size && !src) return -1;

    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    const int ok = (!size || fwrite(src, 1, size, f) == size);
    if (fclose(f) != 0 || !ok) { remove(tmp); return -1; }
    remove(path);
    return rename(tmp, path) == 0 ? 0 : -1;
}

/* Returns the bytes read into guest memory, or -1 if there is no such file. */
static int read_file(const char *dir, const char *name, uint32_t addr, uint32_t cap) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t *dst = cap ? (uint8_t *)psp_mem_ptr(addr, cap) : NULL;
    const size_t got = dst ? fread(dst, 1, cap, f) : 0;
    fclose(f);
    return (int)got;
}

static void delete_slot(uint32_t param, const char *dir) {
    char path[1200], file[14];
    psp_str(param + P_FILE, file, sizeof file);
    if (file[0]) {
        snprintf(path, sizeof path, "%s/%s", dir, file);
        remove(path);
    }
    snprintf(path, sizeof path, "%s/SFO.BIN", dir);
    remove(path);
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, EXTRA_FILE[i]);
        remove(path);
    }
    remove_dir(dir);
}

/* Choose a slot from the list where a player would. Returns 0 if none will do. */
static int pick_slot(uint32_t param, int need_data, char *save, size_t cap) {
    const uint32_t list = psp_read32(param + P_NAMELIST);
    char first[NAME_SIZE + 1] = { 0 }, name[NAME_SIZE + 1];
    if (!list) return 0;
    for (uint32_t i = 0; i < MAX_SLOTS; i++) {
        psp_str(list + i * NAME_SIZE, name, sizeof name);
        if (!name[0]) break;
        if (!first[0]) snprintf(first, sizeof first, "%s", name);
        if (slot_exists(param, name)) {
            snprintf(save, cap, "%s", name);
            return 1;
        }
    }
    if (need_data || !first[0]) return 0;
    snprintf(save, cap, "%s", first);
    return 1;
}

static void put_name(uint32_t param, const char *save) {
    for (uint32_t i = 0; i < NAME_SIZE; i++) {
        psp_write8(param + P_SAVE + i, (uint8_t)*save);
        if (*save) save++;
    }
}

static uint32_t do_save(uint32_t param, const char *save) {
    char dir[1024], file[14];
    const uint32_t size = psp_read32(param + P_DATA_SIZE);
    if (size > MAX_FILE) return ERROR_SAVE_FAILED;
    slot_dir(param, save, dir, sizeof dir);
    make_dirs(dir);
    psp_str(param + P_FILE, file, sizeof file);

    if (file[0] && write_file(dir, file, psp_read32(param + P_DATA), size) != 0) return ERROR_SAVE_FAILED;
    if (write_file(dir, "SFO.BIN", param + P_SFO, SFO_SIZE) != 0) return ERROR_SAVE_FAILED;
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t entry = param + P_FILES + i * FILE_ENTRY;
        const uint32_t buf = psp_read32(entry), bytes = psp_read32(entry + 8);
        if (buf && bytes && bytes <= MAX_FILE) write_file(dir, EXTRA_FILE[i], buf, bytes);
    }
    fprintf(stderr, "savedata: saved %u bytes to %s\n", size, dir);
    report_event(PSP_SAVEDATA_SAVED);
    return 0;
}

static uint32_t do_load(uint32_t param, const char *save) {
    char dir[1024], file[14];
    slot_dir(param, save, dir, sizeof dir);
    psp_str(param + P_FILE, file, sizeof file);

    const int got = file[0] ? read_file(dir, file, psp_read32(param + P_DATA), psp_read32(param + P_DATA_CAP)) : 0;
    if (got < 0) {
        fprintf(stderr, "savedata: nothing to load in %s\n", dir);
        /* A save that is there and cannot be read is not the same as having none. */
        report_event(slot_exists(param, save) ? PSP_SAVEDATA_UNREADABLE : PSP_SAVEDATA_NO_DATA);
        return ERROR_LOAD_NO_DATA;
    }
    psp_write32(param + P_DATA_SIZE, (uint32_t)got);
    read_file(dir, "SFO.BIN", param + P_SFO, SFO_SIZE);
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t entry = param + P_FILES + i * FILE_ENTRY;
        const uint32_t buf = psp_read32(entry), bytes = psp_read32(entry + 4);
        if (!buf || !bytes) continue;
        const int n = read_file(dir, EXTRA_FILE[i], buf, bytes);
        psp_write32(entry + 8, n > 0 ? (uint32_t)n : 0);
    }
    fprintf(stderr, "savedata: loaded %d bytes to 0x%08X from %s\n", got, psp_read32(param + P_DATA), dir);
    report_event(PSP_SAVEDATA_LOADED);
    return 0;
}

static void put_text(uint32_t addr, const char *text, uint32_t cap) {
    for (uint32_t i = 0; i < cap; i++) {
        psp_write8(addr + i, (uint8_t)*text);
        if (*text) text++;
    }
}

/* Report a roomy, otherwise empty stick. A game only asks to warn about space. */
static void fill_sizes(uint32_t param) {
    const uint32_t CLUSTER = 32768u;
    const uint32_t free_info = psp_read32(param + P_MSFREE);
    if (free_info) {
        psp_write32(free_info + 0, CLUSTER);
        psp_write32(free_info + 4, 65536u);
        psp_write32(free_info + 8, 2097152u);
        put_text(free_info + 12, "2 GB", 8);
    }
    const uint32_t used = psp_read32(param + P_USED);
    if (used) {
        const uint32_t kb = (psp_read32(param + P_DATA_SIZE) + 1023u) / 1024u + 32u;
        psp_write32(used + 0, kb / 32u + 1u);
        psp_write32(used + 4, kb);
        put_text(used + 8, "1 MB", 8);
        psp_write32(used + 16, kb);
        put_text(used + 20, "1 MB", 8);
    }
}

uint32_t psp_savedata_run(uint32_t param) {
    const uint32_t mode = psp_read32(param + P_MODE);
    char save[NAME_SIZE + 1];
    psp_str(param + P_SAVE, save, sizeof save);
    fprintf(stderr, "savedata: mode %u, save \"%s\"\n", mode, save);

    switch (mode) {
    case MODE_AUTOSAVE: case MODE_SAVE: case MODE_MAKEDATA: case MODE_MAKEDATASECURE:
    case MODE_WRITEDATA: case MODE_WRITEDATASECURE:
        {
            const uint32_t result = do_save(param, save);
            if (result) report_event(PSP_SAVEDATA_FAILED);
            return result;
        }

    case MODE_AUTOLOAD: case MODE_LOAD: case MODE_READDATA: case MODE_READDATASECURE:
        return do_load(param, save);

    case MODE_LISTSAVE:
        if (!pick_slot(param, 0, save, sizeof save)) return ERROR_SAVE_FAILED;
        put_name(param, save);
        {
            const uint32_t result = do_save(param, save);
            if (result) report_event(PSP_SAVEDATA_FAILED);
            return result;
        }

    case MODE_LISTLOAD:
        if (!pick_slot(param, 1, save, sizeof save)) {
            report_event(PSP_SAVEDATA_NO_DATA);
            return ERROR_LOAD_NO_DATA;
        }
        put_name(param, save);
        return do_load(param, save);

    case MODE_LISTDELETE:
        if (!pick_slot(param, 1, save, sizeof save)) return ERROR_LOAD_NO_DATA;
        put_name(param, save);
        /* fall through */
    case MODE_AUTODELETE: case MODE_DELETE: case MODE_DELETEDATA: {
        char dir[1024];
        slot_dir(param, save, dir, sizeof dir);
        if (!slot_exists(param, save)) return ERROR_LOAD_NO_DATA;
        delete_slot(param, dir);
        fprintf(stderr, "savedata: deleted %s\n", dir);
        report_event(PSP_SAVEDATA_DELETED);
        return 0;
    }

    case MODE_SIZES:
        fill_sizes(param);
        return 0;

    default:
        fprintf(stderr, "savedata: mode %u is not implemented, reporting success\n", mode);
        return 0;
    }
}
