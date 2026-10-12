#ifndef MERCENARIES_PLUGIN_H
#define MERCENARIES_PLUGIN_H
#include <stdint.h>
#include <stddef.h>
/* A plugin declares the interface it needs with ApiVersion= in mod.ini; the
 * host then reports that same value in every api_version field it passes to
 * the plugin, and expects it back in MercPlugin.api_version. */
#define MERCENARIES_PLUGIN_API_VERSION 1u
#define MERCENARIES_PLUGIN_API_VERSION_2 2u
#define MERC_PLUGIN_CALL __cdecl
#define MERC_PLUGIN_EXPORT __declspec(dllexport)
#ifdef __cplusplus
extern "C" {
#endif
/* Windows x64 C ABI. Existing v1 fields/signatures must never change. */
typedef struct MercPluginUpdate {
    uint32_t struct_size, api_version;
    uint64_t update_index;
    double wall_delta_seconds; /* Real time, NOT simulation time; advances while paused. */
} MercPluginUpdate;
typedef struct MercPluginHost {
    uint32_t struct_size, api_version;
    void *context;
    const char *plugin_id;          /* UTF-8 strings, valid until shutdown. */
    const char *plugin_directory;   /* Absolute private staged files. */
    const char *settings_directory; /* Absolute persistent folder outside mod-cache. */
    void (MERC_PLUGIN_CALL *log)(void *, const char *);
    int (MERC_PLUGIN_CALL *read_guest)(void *, uint32_t, void *, size_t);
    int (MERC_PLUGIN_CALL *write_guest)(void *, uint32_t, const void *, size_t);
} MercPluginHost;

/* ---- Interface 2 (fork extension) -------------------------------------
 * run_lua compiles and runs Lua 5.0 source in the game's master script VM, with
 * the retail luaL_loadbuffer/lua_pcall. The chunk receives no arguments; its
 * first return value is converted to text and copied to `out`.
 *
 * Call it only from inside on_update. It runs synchronously on the guest thread
 * and the game does not advance while it runs, so keep chunks small.
 *
 * Result conversion: string -> bytes, number -> "%.17g", boolean -> "true" or
 * "false", nil or no value -> "". Other types give MERC_LUA_BAD_RESULT.
 *
 * *out_length (optional) receives the full result length. If it is >= capacity
 * the text in `out` is truncated (still NUL terminated when capacity > 0).
 * On MERC_LUA_ERROR, `out` holds the Lua error message instead. */
enum {
    MERC_LUA_OK = 0,
    MERC_LUA_ERROR = 1,       /* Compile or runtime error; message in out. */
    MERC_LUA_NOT_READY = 2,   /* No master VM yet, or a script transition is pending. */
    MERC_LUA_REJECTED = 3,    /* Wrong thread/context, outside on_update, or nested call. */
    MERC_LUA_NO_MEMORY = 4,   /* Guest heap could not hold the source text. */
    MERC_LUA_BAD_RESULT = 5   /* First return value was a table/function/userdata/thread. */
};
/* Interface 2 UI facilities. Everything below runs on the guest owner thread:
 * the host pumps window messages and presents frames from that thread.
 *
 * window          The game's top-level HWND, valid until shutdown. Use it as the
 *                 owner of plugin windows. Do not subclass or restyle it.
 * register_hotkey Call during init or on_update. callback runs on a non-repeat
 *                 WM_KEYDOWN of virtual_key while the game window has focus.
 *                 F8-F11 are reserved by the recomp; one plugin per key.
 *                 Returns 1 on success. read_guest/run_lua are rejected inside
 *                 the callback: queue work for the next on_update.
 * set_overlay     Call during init or on_update. paint runs once per presented
 *                 frame; pass NULL to remove it.
 * draw_overlay    Only inside paint. Composites premultiplied BGRA pixels at
 *                 (x, y) in a 960x540 logical canvas scaled to the window.
 *                 All overlays share one GPU texture that is resized to the
 *                 latest image, so draw at most one image per frame and skip the
 *                 call entirely when nothing is visible. */
typedef void (MERC_PLUGIN_CALL *MercPluginCallback)(void *userdata);
typedef struct MercPluginHost2 {
    MercPluginHost v1;
    int (MERC_PLUGIN_CALL *run_lua)(void *context, const char *source, size_t source_length,
                                    char *out, size_t capacity, size_t *out_length);
    void *window;
    int (MERC_PLUGIN_CALL *register_hotkey)(void *context, uint32_t virtual_key,
                                            MercPluginCallback callback, void *userdata);
    int (MERC_PLUGIN_CALL *set_overlay)(void *context, MercPluginCallback paint, void *userdata);
    int (MERC_PLUGIN_CALL *draw_overlay)(void *context, const void *bgra_premultiplied,
                                         uint32_t width, uint32_t height, uint32_t x, uint32_t y);
} MercPluginHost2;

typedef struct MercPlugin {
    uint32_t struct_size, api_version;
    void *userdata;
    int (MERC_PLUGIN_CALL *on_update)(void *, const MercPluginUpdate *);
    void (MERC_PLUGIN_CALL *on_shutdown)(void *);
} MercPlugin;
/* Export with C linkage. Different from the legacy MercenariesMod_Init ABI.
 * Return 1 on success, 0 on failure; error is UTF-8/NUL terminated.
 * Fill callbacks/userdata. A supplied shutdown callback is also called if init fails.
 * With ApiVersion=2 the host pointer is really a MercPluginHost2: check
 * host->api_version == 2 and host->struct_size >= sizeof(MercPluginHost2)
 * before casting.
 */
typedef int (MERC_PLUGIN_CALL *MercPluginEntry)(const MercPluginHost *,
    MercPlugin *, char *, uint32_t);
#ifdef MERC_PLUGIN_IMPLEMENTATION
MERC_PLUGIN_EXPORT int MERC_PLUGIN_CALL MercenariesPlugin_Init(
    const MercPluginHost *, MercPlugin *, char *, uint32_t);
#endif
#ifdef __cplusplus
}
#endif
#endif
