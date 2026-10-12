/* Local socket bridge of the Archipelago plugin (plugin interface 2).
 *
 * Serves one local TCP client (the Python Archipelago client, or the
 * tools/recomp_repl.py console) and runs its requests on the game thread during
 * on_update. While the in-game client is active, Lua and write requests are
 * refused so two clients cannot deliver the same items. All guest access goes through the host:
 * run_lua for script work, read_guest/write_guest for raw memory.
 *
 * Wire format, all integers little-endian:
 *   request  = u32 body_length, u32 id, u8 kind, payload      (body = id..payload)
 *   response = u32 body_length, u32 id, u8 status, payload
 * kinds:
 *   'I'  info. payload empty. response payload is "key=value\n" text.
 *   'L'  Lua.  payload = source. response status is a MERC_LUA_* code and the
 *              payload is the result text (or the error message).
 *   'R'  read. payload = u32 address, u32 length (<= 64 KiB). status 0 + bytes,
 *              or 3 when the host rejects the range.
 *   'W'  write. payload = u32 address, bytes. status 0, or 3 when rejected.
 * Unknown kinds get status 255.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "socket_bridge.h"

#define BRIDGE_VERSION "1"
#define MAX_BODY (1024u * 1024u)
#define MAX_RESULT (256u * 1024u)
#define MAX_RAW (64u * 1024u)
#define STATUS_UNKNOWN 255u

typedef struct Message {
    struct Message *next;
    uint32_t generation; /* connection the message belongs to */
    uint32_t id;
    uint8_t kind_or_status;
    uint32_t length;
    unsigned char data[1]; /* payload; allocated to length */
} Message;

typedef struct Queue {
    Message *head, *tail;
    size_t count;
} Queue;

static const MercPluginHost2 *host;
static CRITICAL_SECTION lock;
static Queue requests, responses;
static HANDLE io_thread;
static HANDLE wake_event; /* set when responses are queued */
static volatile LONG stopping;
static volatile LONG generation; /* bumped on each new connection */
static SOCKET listener = INVALID_SOCKET;
static unsigned short port = 28012;
static unsigned budget_ms = 4;
static unsigned max_per_update = 64;
static char *result_buffer;
static uint64_t lua_runs, lua_errors, not_ready_count;
static volatile LONG connected;
static int winsock_started;
static int started;
static int changes_allowed = 1;
static const char REFUSED[] = "The in-game Archipelago client is active; disconnect it (F7) to use the bridge.";

static void log_text(const char *text) { host->v1.log(host->v1.context, text); }

static void queue_push(Queue *q, Message *m)
{
    m->next = NULL;
    if (q->tail) q->tail->next = m; else q->head = m;
    q->tail = m;
    q->count++;
}

static Message *queue_pop(Queue *q)
{
    Message *m = q->head;
    if (!m) return NULL;
    q->head = m->next;
    if (!q->head) q->tail = NULL;
    q->count--;
    return m;
}

static void queue_clear(Queue *q)
{
    Message *m;
    while ((m = queue_pop(q)) != NULL) free(m);
}

static Message *message_new(uint32_t gen, uint32_t id, uint8_t tag, const void *data, uint32_t length)
{
    Message *m = (Message *)malloc(offsetof(Message, data) + length + 1u);
    if (!m) return NULL;
    m->next = NULL; m->generation = gen; m->id = id; m->kind_or_status = tag; m->length = length;
    if (length) memcpy(m->data, data, length);
    return m;
}

static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* ---- game thread ---------------------------------------------------- */

static void respond(uint32_t gen, uint32_t id, uint8_t status, const void *data, uint32_t length)
{
    Message *m = message_new(gen, id, status, data, length);
    if (!m) return;
    EnterCriticalSection(&lock);
    if (gen == (uint32_t)generation) queue_push(&responses, m); else free(m);
    LeaveCriticalSection(&lock);
    SetEvent(wake_event);
}

static void handle_info(const Message *m)
{
    char text[512];
    uint32_t vm = 0;
    host->v1.read_guest(host->v1.context, 0x003717BCu, &vm, sizeof(vm));
    int n = snprintf(text, sizeof(text),
        "bridge=" BRIDGE_VERSION "\napi=%u\nconnected=%ld\nvm=%08X\nlua_runs=%llu\nlua_errors=%llu\nnot_ready=%llu\n",
        host->v1.api_version, (long)connected, vm, (unsigned long long)lua_runs, (unsigned long long)lua_errors,
        (unsigned long long)not_ready_count);
    respond(m->generation, m->id, 0, text, n > 0 ? (uint32_t)n : 0u);
}

static void handle_lua(const Message *m)
{
    if (!changes_allowed) {
        respond(m->generation, m->id, MERC_LUA_REJECTED, REFUSED, (uint32_t)(sizeof(REFUSED) - 1u));
        return;
    }
    size_t length = 0;
    int status = host->run_lua(host->v1.context, (const char *)m->data, m->length,
                               result_buffer, MAX_RESULT, &length);
    if (length >= MAX_RESULT) length = MAX_RESULT - 1u;
    lua_runs++;
    if (status == MERC_LUA_ERROR) lua_errors++;
    if (status == MERC_LUA_NOT_READY) not_ready_count++;
    if (status != MERC_LUA_OK && status != MERC_LUA_ERROR && status != MERC_LUA_BAD_RESULT) length = 0;
    respond(m->generation, m->id, (uint8_t)status, result_buffer, (uint32_t)length);
}

static void handle_read(const Message *m)
{
    if (m->length != 8) { respond(m->generation, m->id, MERC_LUA_REJECTED, NULL, 0); return; }
    uint32_t address = get_u32(m->data), length = get_u32(m->data + 4);
    if (!length || length > MAX_RAW ||
        !host->v1.read_guest(host->v1.context, address, result_buffer, length)) {
        respond(m->generation, m->id, MERC_LUA_REJECTED, NULL, 0);
        return;
    }
    respond(m->generation, m->id, 0, result_buffer, length);
}

static void handle_write(const Message *m)
{
    if (m->length < 5 || m->length - 4u > MAX_RAW) {
        respond(m->generation, m->id, MERC_LUA_REJECTED, NULL, 0);
        return;
    }
    if (!changes_allowed) {
        respond(m->generation, m->id, MERC_LUA_REJECTED, REFUSED, (uint32_t)(sizeof(REFUSED) - 1u));
        return;
    }
    uint32_t address = get_u32(m->data);
    int ok = host->v1.write_guest(host->v1.context, address, m->data + 4, m->length - 4u);
    respond(m->generation, m->id, ok ? 0 : MERC_LUA_REJECTED, NULL, 0);
}

void archipelago_bridge_update(int allow_changes)
{
    if (!started) return;
    changes_allowed = allow_changes;
    ULONGLONG start = GetTickCount64();
    for (unsigned handled = 0; handled < max_per_update; ++handled) {
        EnterCriticalSection(&lock);
        Message *m = queue_pop(&requests);
        LeaveCriticalSection(&lock);
        if (!m) break;
        if (m->generation == (uint32_t)generation) {
            switch (m->kind_or_status) {
            case 'I': handle_info(m); break;
            case 'L': handle_lua(m); break;
            case 'R': handle_read(m); break;
            case 'W': handle_write(m); break;
            default: respond(m->generation, m->id, STATUS_UNKNOWN, NULL, 0); break;
            }
        }
        free(m);
        if (GetTickCount64() - start >= budget_ms) break;
    }
}

/* ---- I/O thread ------------------------------------------------------ */

static int send_all(SOCKET s, const unsigned char *data, size_t length)
{
    while (length) {
        int n = send(s, (const char *)data, length > 0x10000u ? 0x10000 : (int)length, 0);
        if (n <= 0) return 0;
        data += n; length -= (size_t)n;
    }
    return 1;
}

static int flush_responses(SOCKET s)
{
    for (;;) {
        EnterCriticalSection(&lock);
        Message *m = queue_pop(&responses);
        LeaveCriticalSection(&lock);
        if (!m) return 1;
        unsigned char header[13];
        put_u32(header, 5u + m->length);
        put_u32(header + 4, m->id);
        header[8] = m->kind_or_status;
        int ok = send_all(s, header, 9) && (!m->length || send_all(s, m->data, m->length));
        free(m);
        if (!ok) return 0;
    }
}

/* Parses complete frames out of buf; returns bytes consumed or -1 on protocol error. */
static long parse_frames(const unsigned char *buf, size_t have, uint32_t gen)
{
    size_t used = 0;
    while (have - used >= 4) {
        uint32_t body = get_u32(buf + used);
        if (body < 5 || body > MAX_BODY) return -1;
        if (have - used - 4 < body) break;
        const unsigned char *p = buf + used + 4;
        Message *m = message_new(gen, get_u32(p), p[4], p + 5, body - 5u);
        if (!m) return -1;
        EnterCriticalSection(&lock);
        queue_push(&requests, m);
        LeaveCriticalSection(&lock);
        used += 4u + body;
    }
    return (long)used;
}

static void serve_client(SOCKET client)
{
    uint32_t gen = (uint32_t)InterlockedIncrement(&generation);
    EnterCriticalSection(&lock);
    queue_clear(&requests);
    queue_clear(&responses);
    LeaveCriticalSection(&lock);
    connected = 1;
    u_long nonblocking = 1;
    ioctlsocket(client, FIONBIO, &nonblocking);
    BOOL nodelay = TRUE;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    size_t capacity = 64u * 1024u, have = 0;
    unsigned char *buf = (unsigned char *)malloc(capacity);
    WSAEVENT net = WSACreateEvent();
    WSAEventSelect(client, net, FD_READ | FD_CLOSE);
    HANDLE waits[2] = {net, wake_event};
    while (buf && !stopping) {
        DWORD which = WaitForMultipleObjects(2, waits, FALSE, 100);
        if (which == WAIT_OBJECT_0) WSAResetEvent(net);
        if (!flush_responses(client)) break;
        int closed = 0;
        for (;;) {
            if (capacity - have < 4096) {
                if (capacity >= MAX_BODY * 2u) { closed = 1; break; }
                unsigned char *grown = (unsigned char *)realloc(buf, capacity * 2u);
                if (!grown) { closed = 1; break; }
                buf = grown; capacity *= 2u;
            }
            int n = recv(client, (char *)buf + have, (int)(capacity - have), 0);
            if (n == 0) { closed = 1; break; }
            if (n < 0) { if (WSAGetLastError() != WSAEWOULDBLOCK) closed = 1; break; }
            have += (size_t)n;
            long used = parse_frames(buf, have, gen);
            if (used < 0) { closed = 1; break; }
            memmove(buf, buf + used, have - (size_t)used);
            have -= (size_t)used;
        }
        if (closed) break;
    }
    WSACloseEvent(net);
    free(buf);
    closesocket(client);
    InterlockedIncrement(&generation); /* drop anything still queued for this client */
    connected = 0;
}

static DWORD WINAPI io_main(LPVOID unused)
{
    (void)unused;
    while (!stopping) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        struct timeval tv = {0, 200000};
        int ready = select(0, &set, NULL, NULL, &tv);
        if (ready <= 0) continue;
        SOCKET client = accept(listener, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        serve_client(client);
    }
    return 0;
}

/* ---- lifecycle ------------------------------------------------------- */

void archipelago_bridge_stop(void)
{
    if (!started) return;
    stopping = 1;
    if (wake_event) SetEvent(wake_event);
    if (listener != INVALID_SOCKET) { closesocket(listener); listener = INVALID_SOCKET; }
    if (io_thread) { WaitForSingleObject(io_thread, 5000); CloseHandle(io_thread); io_thread = NULL; }
    if (wake_event) { CloseHandle(wake_event); wake_event = NULL; }
    EnterCriticalSection(&lock);
    queue_clear(&requests);
    queue_clear(&responses);
    LeaveCriticalSection(&lock);
    DeleteCriticalSection(&lock);
    free(result_buffer); result_buffer = NULL;
    if (winsock_started) { WSACleanup(); winsock_started = 0; }
    log_text("Archipelago bridge stopped.");
    started = 0;
    host = NULL;
}

int archipelago_bridge_start(const MercPluginHost2 *plugin_host, unsigned short listen_port,
                             unsigned update_budget_ms, unsigned requests_per_update,
                             char *error, size_t capacity)
{
    if (started) return 1;
    host = plugin_host;
    port = listen_port;
    budget_ms = update_budget_ms < 1 ? 1 : update_budget_ms > 16 ? 16 : update_budget_ms;
    max_per_update = requests_per_update < 1 ? 1 : requests_per_update > 1024 ? 1024 : requests_per_update;
    stopping = 0;
    InitializeCriticalSection(&lock);
    started = 1; /* from here on, archipelago_bridge_stop() cleans up */
    result_buffer = (char *)malloc(MAX_RESULT);
    wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    WSADATA wsa;
    if (result_buffer && wake_event && WSAStartup(MAKEWORD(2, 2), &wsa) == 0) winsock_started = 1;
    if (!winsock_started) {
        if (error && capacity) snprintf(error, capacity, "Could not initialize memory or Winsock.");
        archipelago_bridge_stop();
        return 0;
    }
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));
    if (listener == INVALID_SOCKET || bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        if (error && capacity)
            snprintf(error, capacity, "Could not listen on 127.0.0.1:%u (Winsock error %d). Change Port in archipelago.ini.",
                     port, WSAGetLastError());
        archipelago_bridge_stop();
        return 0;
    }
    io_thread = CreateThread(NULL, 0, io_main, NULL, 0, NULL);
    if (!io_thread) {
        if (error && capacity) snprintf(error, capacity, "Could not start the network thread.");
        archipelago_bridge_stop();
        return 0;
    }
    char text[128];
    snprintf(text, sizeof(text), "Archipelago bridge listening on 127.0.0.1:%u.", port);
    log_text(text);
    return 1;
}
