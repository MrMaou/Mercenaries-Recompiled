/* Local socket bridge of the Archipelago plugin; see socket_bridge.c. */
#ifndef ARCHIPELAGO_SOCKET_BRIDGE_H
#define ARCHIPELAGO_SOCKET_BRIDGE_H
#include <stddef.h>
#include "../../../sdk/mercenaries_plugin.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Call from init. Returns 0 with a message in error on failure. */
int archipelago_bridge_start(const MercPluginHost2 *host, unsigned short port, unsigned update_budget_ms,
                             unsigned requests_per_update, char *error, size_t capacity);
/* Call from every on_update. allow_changes=0 refuses Lua and write requests. */
void archipelago_bridge_update(int allow_changes);
/* Call from shutdown. Safe to call when start failed or was never called. */
void archipelago_bridge_stop(void);
#ifdef __cplusplus
}
#endif
#endif
