/* Plugin interface 2: run Lua source in the master script VM for a plugin.
 * Included after mod_gameplay_guest.h in recomp_manual.c, so it shares
 * dev_call, the guest CPU context helpers and the mod_running_script guard.
 *
 * Same retail call sequence as mod_run_lua and recomp_dev_missions_tick:
 * lua_gettop, luaL_loadbuffer, lua_pcall, lua_settop. The only difference is
 * nresults=1 so the chunk's first return value can be copied out.
 *
 * Xbox Lua 5.0 layout used below (also relied on by recomp_lua_pcall_checkpoint):
 *   lua_State+0x08  StkId top (points one past the last TObject)
 *   TObject         16 bytes: int tt at +0, Value at +8 (lua_Number is double)
 *   TString         len (size_t) at +12, characters at +16
 */
/* Status codes mirror the MERC_LUA_* enum in sdk/mercenaries_plugin.h. That
 * header is not included here because it uses MSVC calling-convention keywords. */
#ifndef MERC_LUA_OK
#define MERC_LUA_OK          0
#define MERC_LUA_ERROR       1
#define MERC_LUA_NOT_READY   2
#define MERC_LUA_REJECTED    3
#define MERC_LUA_NO_MEMORY   4
#define MERC_LUA_BAD_RESULT  5
#endif

#define PLUGIN_LUA_VM_POINTER      0x003717BCu
#define PLUGIN_LUA_TRANSITION_FLAG 0x003717D6u
#define PLUGIN_LUA_GETTOP          0x001DC970u
#define PLUGIN_LUA_SETTOP          0x001DC980u
#define PLUGIN_LUA_LOADBUFFER      0x001DC480u
#define PLUGIN_LUA_PCALL           0x001DD5A0u

static void plugin_lua_copy(char *out, size_t capacity, size_t *out_length,
                            const char *text, size_t length)
{
    if (out_length) *out_length = length;
    if (!out || !capacity) return;
    size_t n = length < capacity - 1u ? length : capacity - 1u;
    if (n) memcpy(out, text, n);
    out[n] = 0;
}

/* Converts the TObject just below L->top. Returns a MERC_LUA_* status. */
static int plugin_lua_take_top(uint32_t vm, int had_result, char *out, size_t capacity,
                               size_t *out_length)
{
    uint32_t top = guest_u32(vm + 8u);
    if (!had_result || top < 0x10010u || top > 0x04000000u) {
        plugin_lua_copy(out, capacity, out_length, "", 0);
        return MERC_LUA_OK;
    }
    uint32_t object = top - 16u;
    uint32_t tag = guest_u32(object);
    char text[64];
    switch (tag) {
    case 0: /* nil */
        plugin_lua_copy(out, capacity, out_length, "", 0);
        return MERC_LUA_OK;
    case 1: /* boolean */
        if (guest_u32(object + 8u)) plugin_lua_copy(out, capacity, out_length, "true", 4);
        else plugin_lua_copy(out, capacity, out_length, "false", 5);
        return MERC_LUA_OK;
    case 3: { /* number */
        double value;
        memcpy(&value, guest_ptr(object + 8u), sizeof(value));
        int n = snprintf(text, sizeof(text), "%.17g", value);
        plugin_lua_copy(out, capacity, out_length, text, n > 0 ? (size_t)n : 0u);
        return MERC_LUA_OK;
    }
    case 4: { /* string */
        uint32_t string = guest_u32(object + 8u);
        if (string < 0x10000u || string > 0x03ffffefu) return MERC_LUA_BAD_RESULT;
        uint32_t length = guest_u32(string + 12u);
        if (length > 0x04000000u - string - 16u) return MERC_LUA_BAD_RESULT;
        plugin_lua_copy(out, capacity, out_length, (const char *)guest_ptr(string + 16u), length);
        return MERC_LUA_OK;
    }
    default:
        snprintf(text, sizeof(text), "unsupported result type %u", tag);
        plugin_lua_copy(out, capacity, out_length, text, strlen(text));
        return MERC_LUA_BAD_RESULT;
    }
}

int recomp_plugin_run_lua(const char *source, size_t source_length,
                          char *out, size_t capacity, size_t *out_length)
{
    if (mod_running_script || !g_xbox_mem_offset || g_esp < 0x20000u || g_esp >= 0x4000000u)
        return MERC_LUA_REJECTED;
    uint32_t vm = guest_u32(PLUGIN_LUA_VM_POINTER);
    if (!dev_guest_address(vm, 0xA0u) || guest_u8(PLUGIN_LUA_TRANSITION_FLAG))
        return MERC_LUA_NOT_READY;
    const uint32_t calls[] = {PLUGIN_LUA_GETTOP, PLUGIN_LUA_SETTOP, PLUGIN_LUA_LOADBUFFER, PLUGIN_LUA_PCALL};
    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i)
        if (!recomp_lookup(calls[i])) return MERC_LUA_NOT_READY;

    static const char chunk_name[] = "plugin";
    uint32_t buffer = recomp_title_heap_allocate((uint32_t)source_length + (uint32_t)sizeof(chunk_name) + 1u);
    if (!buffer) return MERC_LUA_NO_MEMORY;
    uint32_t name = buffer + (uint32_t)source_length + 1u;
    memcpy(guest_ptr(buffer), source, source_length);
    *(char *)guest_ptr(buffer + (uint32_t)source_length) = 0;
    memcpy(guest_ptr(name), chunk_name, sizeof(chunk_name));

    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    mod_running_script = 1;
    uint32_t stack = (g_esp - 0x4000u) & ~15u;
    uint32_t top = dev_call(stack, PLUGIN_LUA_GETTOP, 0, 1, &vm);
    uint32_t load[] = {vm, buffer, (uint32_t)source_length, name};
    uint32_t error = dev_call(stack, PLUGIN_LUA_LOADBUFFER, 0, 4, load);
    if (!error) {
        uint32_t call[] = {vm, 0, 1, 0}; /* no arguments, one result, no handler */
        error = dev_call(stack, PLUGIN_LUA_PCALL, 0, 4, call);
    }
    int status;
    if (error) {
        /* Error object is on top. Copy it, then log through the usual checkpoint. */
        plugin_lua_take_top(vm, 1, out, capacity, out_length);
        status = MERC_LUA_ERROR;
    } else {
        status = plugin_lua_take_top(vm, 1, out, capacity, out_length);
    }
    uint32_t restore[] = {vm, top};
    dev_call(stack, PLUGIN_LUA_SETTOP, 0, 2, restore);
    mod_running_script = 0;
    recomp_restore_guest_cpu_context(&saved);
    recomp_title_heap_free(buffer);
    return status;
}
