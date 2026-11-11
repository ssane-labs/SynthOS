/* odbc32.dll: the driver manager with no drivers installed (connecting fails with IM002, as on Windows without one),
   and oledlg.dll's busy dialog, which only comes up when a COM server stalls (none does here). */
#include <synth/string.h>

#include "win64.h"

#define SQL_SUCCESS 0
#define SQL_ERROR (-1)
#define SQL_INVALID_HANDLE (-2)
#define SQL_NO_DATA 100
#define ODBC_MAGIC 0x4f444243u

struct odbc {
    uint32_t magic;
    int type;
    struct odbc *parent;
    char state[6];
    const char *message;
};

static struct odbc *odbc_of(void *h)
{
    struct odbc *o = h;
    return (uintptr_t)h >= 0x10000 && o->magic == ODBC_MAGIC ? o : NULL;
}

static void odbc_fail(struct odbc *o, const char *state, const char *message)
{
    str_copy(o->state, state, sizeof(o->state));
    o->message = message;
}

static short WINAPI SQLAllocHandle(int type, void *input, void **out)
{
    type = (short)type;
    struct odbc *in = odbc_of(input);
    if (!out || type < 1 || type > 4 || (type > 1 && !in))
        return type > 1 && !in ? SQL_INVALID_HANDLE : SQL_ERROR;
    struct odbc *o = w32_zalloc(sizeof(*o));
    if (!o) {
        *out = NULL;
        return SQL_ERROR;
    }
    *o = (struct odbc){ODBC_MAGIC, type, in, "", NULL};
    *out = o;
    return SQL_SUCCESS;
}

static short WINAPI SQLAllocEnv(void **out)
{
    return SQLAllocHandle(1, NULL, out);
}

static short WINAPI SQLAllocConnect(void *env, void **out)
{
    return SQLAllocHandle(2, env, out);
}

static short WINAPI SQLFreeHandle(int type, void *h)
{
    UNUSED(type);
    struct odbc *o = odbc_of(h);
    if (!o)
        return SQL_INVALID_HANDLE;
    o->magic = 0;
    w32_free(o);
    return SQL_SUCCESS;
}

static short WINAPI SQLFreeEnv(void *h)
{
    return SQLFreeHandle(1, h);
}

static short WINAPI SQLSetEnvAttr(void *env, int attr, void *value, int len)
{
    UNUSED(attr);
    UNUSED(value);
    UNUSED(len);
    return odbc_of(env) ? SQL_SUCCESS : SQL_INVALID_HANDLE;
}

static short no_driver(void *dbc)
{
    struct odbc *o = odbc_of(dbc);
    if (!o)
        return SQL_INVALID_HANDLE;
    odbc_fail(o, "IM002", "[Microsoft][ODBC Driver Manager] Data source name not found and no default driver "
                          "specified");
    return SQL_ERROR;
}

static short WINAPI SQLDriverConnectW(void *dbc, HANDLE window, const void *in, int in_len, void *out, int out_max,
                                      short *out_len, int completion)
{
    UNUSED(window);
    UNUSED(in);
    UNUSED(in_len);
    UNUSED(out);
    UNUSED(out_max);
    UNUSED(completion);
    if (out_len)
        *out_len = 0;
    return no_driver(dbc);
}

static short WINAPI SQLConnectW(void *dbc, const void *dsn, int dsn_len, const void *user, int user_len,
                                const void *password, int password_len)
{
    UNUSED(dsn);
    UNUSED(dsn_len);
    UNUSED(user);
    UNUSED(user_len);
    UNUSED(password);
    UNUSED(password_len);
    return no_driver(dbc);
}

static short WINAPI SQLDisconnect(void *dbc)
{
    struct odbc *o = odbc_of(dbc);
    if (!o)
        return SQL_INVALID_HANDLE;
    odbc_fail(o, "08003", "[Microsoft][ODBC Driver Manager] Connection not open");
    return SQL_ERROR;
}

/* no data sources and no drivers to list */
static short WINAPI SQLDataSourcesW(void *env, int direction, void *name, int name_max, short *name_len, void *desc,
                                    int desc_max, short *desc_len)
{
    UNUSED(direction);
    UNUSED(name);
    UNUSED(name_max);
    UNUSED(name_len);
    UNUSED(desc);
    UNUSED(desc_max);
    UNUSED(desc_len);
    return odbc_of(env) ? SQL_NO_DATA : SQL_INVALID_HANDLE;
}

static short diag(int type, void *h, int rec, void *state, int *native, void *msg, int max, short *len, bool wide)
{
    UNUSED(type);
    struct odbc *o = odbc_of(h);
    if (!o)
        return SQL_INVALID_HANDLE;
    if ((short)rec != 1 || !o->message)
        return SQL_NO_DATA;
    size_t n = strlen(o->message);
    max = (short)max;
    if (native)
        *native = 0;
    if (len)
        *len = (short)n;
    for (int i = 0; state && i < 6; i++) {
        if (wide)
            ((WCHAR *)state)[i] = (WCHAR)(uint8_t)o->state[i];
        else
            ((char *)state)[i] = o->state[i];
    }
    for (int i = 0; msg && max > 0 && i < max; i++) {
        char c = i < max - 1 && (size_t)i < n ? o->message[i] : 0;
        if (wide)
            ((WCHAR *)msg)[i] = (WCHAR)(uint8_t)c;
        else
            ((char *)msg)[i] = c;
        if (!c)
            break;
    }
    return SQL_SUCCESS;
}

static short WINAPI SQLGetDiagRecW(int type, void *h, int rec, void *state, int *native, void *msg, int max,
                                   short *len)
{
    return diag(type, h, rec, state, native, msg, max, len, true);
}

static short WINAPI SQLGetDiagRec(int type, void *h, int rec, void *state, int *native, void *msg, int max,
                                  short *len)
{
    return diag(type, h, rec, state, native, msg, max, len, false);
}

static short WINAPI SQLDriverConnect(void *dbc, HANDLE window, const void *in, int in_len, void *out, int out_max,
                                     short *out_len, int completion)
{
    return SQLDriverConnectW(dbc, window, in, in_len, out, out_max, out_len, completion);
}

static short WINAPI SQLConnect(void *dbc, const void *dsn, int dsn_len, const void *user, int user_len,
                               const void *password, int password_len)
{
    return SQLConnectW(dbc, dsn, dsn_len, user, user_len, password, password_len);
}

static const struct w32_export odbc_exports[] = {
    W32_FN(SQLAllocHandle), W32_FN(SQLAllocEnv), W32_FN(SQLAllocConnect), W32_FN(SQLFreeHandle),
    W32_FN(SQLFreeEnv), W32_AS("SQLFreeConnect", SQLFreeEnv), W32_FN(SQLSetEnvAttr), W32_FN(SQLDriverConnectW),
    W32_FN(SQLDriverConnect), W32_FN(SQLConnectW), W32_FN(SQLConnect), W32_FN(SQLDisconnect),
    W32_FN(SQLDataSourcesW), W32_AS("SQLDataSources", SQLDataSourcesW), W32_AS("SQLDriversW", SQLDataSourcesW),
    W32_AS("SQLDrivers", SQLDataSourcesW), W32_FN(SQLGetDiagRecW), W32_FN(SQLGetDiagRec),
};

static const char *const odbc_ordinals[] = {
    [1] = "SQLAllocConnect", [2] = "SQLAllocEnv",     [7] = "SQLConnect",         [9] = "SQLDisconnect",
    [14] = "SQLFreeConnect", [15] = "SQLFreeEnv",     [24] = "SQLAllocHandle",    [31] = "SQLFreeHandle",
    [36] = "SQLGetDiagRec",  [41] = "SQLDriverConnect", [57] = "SQLDataSources", [71] = "SQLDrivers",
    [75] = "SQLSetEnvAttr",  [107] = "SQLConnectW",   [136] = "SQLGetDiagRecW",   [141] = "SQLDriverConnectW",
    [157] = "SQLDataSourcesW", [171] = "SQLDriversW",
};

W32_EXPORTS(odbc_module, "odbc32.dll", odbc_exports, .ordinals = odbc_ordinals,
            .ordinal_count = ARRAY_SIZE(odbc_ordinals));

static const struct w64_export odbc64_exports[] = {
    W64_OUT("SQLAllocHandle", 1u << 2), W64_OUT("SQLAllocEnv", 1u << 0), W64_OUT("SQLAllocConnect", 1u << 1),
};

W64_EXPORTS(odbc64_module, "odbc32.dll", odbc64_exports);

#define OLEUI_CANCEL 2

static UINT WINAPI OleUIBusyW(void *info)
{
    return info ? OLEUI_CANCEL : 0;
}

static const struct w32_export oledlg_exports[] = {
    W32_FN(OleUIBusyW),
    W32_AS("OleUIBusyA", OleUIBusyW),
};

static const char *const oledlg_ordinals[] = {[8] = "OleUIBusyA"};

W32_EXPORTS(oledlg_module, "oledlg.dll", oledlg_exports, .ordinals = oledlg_ordinals,
            .ordinal_count = ARRAY_SIZE(oledlg_ordinals));
