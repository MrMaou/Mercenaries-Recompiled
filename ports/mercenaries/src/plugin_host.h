#ifndef MERCENARIES_PLUGIN_HOST_H
#define MERCENARIES_PLUGIN_HOST_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int merc_plugins_initialize(void *guest_base, size_t guest_bytes, void *window);
int merc_plugins_hotkey(unsigned virtual_key);
void merc_plugins_paint_overlays(void);
int merc_plugins_update(void);
void merc_plugins_shutdown(void);
const char *merc_plugins_error(void);
void merc_plugins_show_error(void);
#ifdef __cplusplus
}
#endif
#endif

