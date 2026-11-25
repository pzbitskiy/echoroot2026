/*
 * D-Bus system bus. Amazon's "lipc" is a thin convention on top of it, as
 * observed with dbus-monitor on the stock daemons:
 *
 *   property write: method call set<Prop>Str(string) / set<Prop>Int(int32)
 *   property read:  method call get<Prop>Str() / get<Prop>Int()
 *   events:         signals
 *
 * all with destination = interface = the service name and path /default.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dbus/dbus.h>

#include "sat.h"

static DBusConnection *conn;
static int lost;

/* Read an integer argument whatever integer type the sender chose. */
static int arg_u64(DBusMessageIter *it, uint64_t *out)
{
    union {
        dbus_uint64_t u64;
        dbus_int64_t i64;
        dbus_uint32_t u32;
        dbus_int32_t i32;
        dbus_uint16_t u16;
        dbus_int16_t i16;
        unsigned char byte;
        dbus_bool_t b;
    } v;

    switch (dbus_message_iter_get_arg_type(it)) {
    case DBUS_TYPE_UINT64: dbus_message_iter_get_basic(it, &v.u64); *out = v.u64; return 1;
    case DBUS_TYPE_INT64:  dbus_message_iter_get_basic(it, &v.i64); *out = (uint64_t)v.i64; return 1;
    case DBUS_TYPE_UINT32: dbus_message_iter_get_basic(it, &v.u32); *out = v.u32; return 1;
    case DBUS_TYPE_INT32:  dbus_message_iter_get_basic(it, &v.i32); *out = (uint64_t)(int64_t)v.i32; return 1;
    case DBUS_TYPE_UINT16: dbus_message_iter_get_basic(it, &v.u16); *out = v.u16; return 1;
    case DBUS_TYPE_INT16:  dbus_message_iter_get_basic(it, &v.i16); *out = (uint64_t)(int64_t)v.i16; return 1;
    case DBUS_TYPE_BYTE:   dbus_message_iter_get_basic(it, &v.byte); *out = v.byte; return 1;
    case DBUS_TYPE_BOOLEAN: dbus_message_iter_get_basic(it, &v.b); *out = v.b; return 1;
    }
    return 0;
}

static const char *arg_str(DBusMessageIter *it)
{
    const char *s = NULL;
    if (dbus_message_iter_get_arg_type(it) == DBUS_TYPE_STRING)
        dbus_message_iter_get_basic(it, &s);
    return s;
}

/* Collect the integer arguments of a message in order, skipping others. */
static int int_args(DBusMessage *m, uint64_t *out, int max)
{
    DBusMessageIter it;
    int n = 0;

    if (!dbus_message_iter_init(m, &it))
        return 0;
    do {
        if (n < max && arg_u64(&it, &out[n]))
            n++;
    } while (dbus_message_iter_next(&it));
    return n;
}

static const char *first_str_arg(DBusMessage *m)
{
    DBusMessageIter it;

    if (!dbus_message_iter_init(m, &it))
        return NULL;
    do {
        const char *s = arg_str(&it);
        if (s)
            return s;
    } while (dbus_message_iter_next(&it));
    return NULL;
}

static DBusHandlerResult filter(DBusConnection *c, DBusMessage *m, void *user)
{
    (void)c;
    (void)user;
    uint64_t a[9];
    const char *s;

    if (dbus_message_is_signal(m, DBUS_INTERFACE_LOCAL, "Disconnected")) {
        lost = 1;
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_SIGNAL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (dbus_message_is_signal(m, SVC_ASR, "FoundKeyword")) {
        /* Integer arguments 1, 4 and 5: stream start, wake word begin and
         * end relative to it. See ASRD.md. Argument 7 is a string, so the
         * fourth and fifth integers are still at index 3 and 4. */
        int n = int_args(m, a, 9);
        if (n >= 5)
            sat_keyword(a[0], a[3], a[4]);
        else
            logw("bus: FoundKeyword with %d integer arguments, ignored", n);
    } else if (dbus_message_is_signal(m, SVC_ASR, "FoundCmd")) {
        if ((s = first_str_arg(m)))
            sat_local_command(s);
    } else if (dbus_message_is_signal(m, SVC_ASR, "ReadyChanged")) {
        if ((s = first_str_arg(m)))
            sat_asr_ready(!strcmp(s, "Yes"));
    } else if (dbus_message_is_signal(m, SVC_BUTTOND, "button")) {
        s = first_str_arg(m);
        if (s && int_args(m, a, 1) == 1)
            sat_button(s, (int)a[0]);
    } else if (dbus_message_is_signal(m, SVC_AUDIOD, "MainVolumeChanged")) {
        if (int_args(m, a, 1) == 1)
            sat_volume_changed((int)a[0]);
    } else if (dbus_message_is_signal(m, SVC_AUDIOD, "MuteChanged")) {
        if (int_args(m, a, 1) == 1)
            sat_audio_mute_changed((int)a[0]);
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int bus_init(void)
{
    static const char *const rules[] = {
        "type='signal',interface='" SVC_ASR "'",
        "type='signal',interface='" SVC_BUTTOND "'",
        "type='signal',interface='" SVC_AUDIOD "'",
    };
    DBusError err;

    dbus_error_init(&err);
    conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
    if (!conn) {
        loge("bus: cannot connect to the system bus: %s", err.message ? err.message : "?");
        dbus_error_free(&err);
        return -1;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    if (!dbus_connection_add_filter(conn, filter, NULL, NULL))
        return -1;
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        dbus_bus_add_match(conn, rules[i], &err);
        if (dbus_error_is_set(&err)) {
            loge("bus: add match failed: %s", err.message);
            dbus_error_free(&err);
            return -1;
        }
    }
    dbus_connection_flush(conn);
    return 0;
}

int bus_fd(void)
{
    int fd = -1;
    if (!conn || !dbus_connection_get_unix_fd(conn, &fd))
        return -1;
    return fd;
}

int bus_dispatch(void)
{
    if (!conn)
        return -1;
    dbus_connection_read_write(conn, 0);
    while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS)
        ;
    return lost ? -1 : 0;
}

static DBusMessage *prop_call(const char *svc, const char *verb, const char *prop, const char *type)
{
    char member[96];

    snprintf(member, sizeof(member), "%s%s%s", verb, prop, type);
    return dbus_message_new_method_call(svc, "/default", svc, member);
}

static void send_noreply(DBusMessage *m)
{
    dbus_message_set_no_reply(m, TRUE);
    dbus_connection_send(conn, m, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(m);
}

void bus_set_str(const char *svc, const char *prop, const char *val)
{
    DBusMessage *m;

    if (!conn || lost || !(m = prop_call(svc, "set", prop, "Str")))
        return;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &val, DBUS_TYPE_INVALID);
    logd("bus: %s %s = %s", svc, prop, val);
    send_noreply(m);
}

void bus_set_int(const char *svc, const char *prop, int val)
{
    DBusMessage *m;
    dbus_int32_t v = val;

    if (!conn || lost || !(m = prop_call(svc, "set", prop, "Int")))
        return;
    dbus_message_append_args(m, DBUS_TYPE_INT32, &v, DBUS_TYPE_INVALID);
    logd("bus: %s %s = %d", svc, prop, val);
    send_noreply(m);
}

/*
 * Blocking read, only used at start-up and on rare events. The reply is
 * (uint32 status, value); status 0 means success.
 */
static DBusMessage *get_call(const char *svc, const char *prop, const char *type, DBusMessageIter *value)
{
    DBusMessage *m, *reply;
    DBusError err;
    uint64_t status = 1;

    if (!conn || lost || !(m = prop_call(svc, "get", prop, type)))
        return NULL;
    dbus_error_init(&err);
    reply = dbus_connection_send_with_reply_and_block(conn, m, 500, &err);
    dbus_message_unref(m);
    if (!reply) {
        logd("bus: get %s %s: %s", svc, prop, err.message ? err.message : "no reply");
        dbus_error_free(&err);
        return NULL;
    }
    if (!dbus_message_iter_init(reply, value) || !arg_u64(value, &status) || status != 0 ||
        !dbus_message_iter_next(value)) {
        logd("bus: get %s %s: status %llu, reply signature \"%s\"", svc, prop,
             (unsigned long long)status, dbus_message_get_signature(reply));
        dbus_message_unref(reply);
        return NULL;
    }
    return reply;
}

int bus_get_int(const char *svc, const char *prop, int *out)
{
    DBusMessageIter it;
    DBusMessage *reply = get_call(svc, prop, "Int", &it);
    uint64_t v;
    int ok;

    if (!reply)
        return -1;
    ok = arg_u64(&it, &v);
    dbus_message_unref(reply);
    if (!ok)
        return -1;
    *out = (int)v;
    logd("bus: %s %s is %d", svc, prop, *out);
    return 0;
}

int bus_get_str(const char *svc, const char *prop, char *out, size_t cap)
{
    DBusMessageIter it;
    DBusMessage *reply = get_call(svc, prop, "Str", &it);
    const char *s;

    if (!reply)
        return -1;
    s = arg_str(&it);
    if (s) {
        strcopy(out, cap, s);
        logd("bus: %s %s is \"%s\"", svc, prop, out);
    }
    dbus_message_unref(reply);
    return s ? 0 : -1;
}
