/* SPDX-License-Identifier: MIT
 *
 * libxpctl (xpctl.h). Speaks four extensions with plain xcb:
 *
 *   RandR         outputs, modes, and the HDR, WIDE_GAMUT and EDR
 *                 output properties of XPrism's drivers
 *   XPRISM-COLOR  what the compositor shows on each output
 *   X-NAMESPACE   namespaces, grants, clients and permission prompts
 *                 (wire format: xserver/Xext/namespace/namespaceproto.h)
 *   X-Resource    which clients are connected
 */
#include "xpctl.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <xcb/randr.h>
#include <xcb/res.h>
#include <xcb/xcbext.h>

/* X-NAMESPACE minor opcodes and bits */
enum {
    XNS_QUERY_VERSION = 0,
    XNS_LIST_NAMESPACES = 1,
    XNS_CREATE_NAMESPACE = 2,
    XNS_DELETE_NAMESPACE = 3,
    XNS_QUERY_NAMESPACE = 4,
    XNS_GET_CLIENT_INFO = 12,
    XNS_SET_NAMESPACE_GRANTS = 13,
    XNS_SET_PERMISSION_AGENT = 14,
    XNS_ANSWER_PERMISSION = 15,
};
#define XNS_CAP_ADMIN           (1u << 5)
#define XNS_CAPS_FOR_APPS       0x1fu   /* mouse motion, shape, transparency,
                                           XInput, XKB: what apps need */
#define XNS_ATTR_IMMUTABLE      (1u << 0)
#define XNS_ATTR_TRANSIENT      (1u << 1)
#define XNS_CLIENT_SOCKET       (1u << 0)
#define XNS_CLIENT_PID          (1u << 1)

/* XPRISM-COLOR */
enum { XPC_GET_OUTPUT_COLOR_SPACE = 4 };

struct xpctl {
    xcb_connection_t *c;
    xcb_window_t root;
    uint8_t xns_op, xns_event, color_op;
    bool have_xns, have_color, have_scene;
    int xns_minor;
    char error[256];
};

static int
fail(xpctl *x, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->error, sizeof(x->error), fmt, ap);
    va_end(ap);
    return -1;
}

static const char *
x_error_text(int code)
{
    switch (code) {
    case 2: return "a value is out of range";
    case 8: return "it does not match";
    case 10: return "not allowed";
    case 11: return "out of memory";
    case 15: return "no such name, or the name is taken or not allowed";
    case 16: return "bad length";
    default: return "the server refused it";
    }
}

/* ---- connection ---------------------------------------------------- */

static bool
query_ext(xpctl *x, const char *name, uint8_t *op, uint8_t *event)
{
    xcb_query_extension_reply_t *r = xcb_query_extension_reply(x->c,
        xcb_query_extension(x->c, strlen(name), name), NULL);
    bool ok = r && r->present;
    if (ok) {
        *op = r->major_opcode;
        if (event)
            *event = r->first_event;
    }
    free(r);
    return ok;
}

/* An extension request: header + body; reply (malloc'd) or NULL with
   *xerr set (0 for a request without reply that succeeded). */
static uint8_t *
ext_request(xpctl *x, uint8_t op, uint8_t minor, const void *body, size_t len,
            bool has_reply, int *xerr)
{
    size_t total = 4 + ((len + 3) & ~(size_t) 3);
    uint8_t *req = calloc(1, total);
    if (!req) {
        *xerr = 11;
        return NULL;
    }
    req[0] = op;
    req[1] = minor;
    uint16_t words = (uint16_t) (total / 4);
    memcpy(req + 2, &words, 2);
    if (len)
        memcpy(req + 4, body, len);

    struct iovec iov[3] = { { 0 }, { 0 }, { .iov_base = req, .iov_len = total } };
    xcb_protocol_request_t info = { .count = 1, .opcode = minor, .isvoid = !has_reply };
    unsigned seq = xcb_send_request(x->c, XCB_REQUEST_CHECKED | XCB_REQUEST_RAW,
                                    &iov[2], &info);
    free(req);
    *xerr = 0;
    xcb_generic_error_t *err = NULL;
    uint8_t *reply = NULL;
    if (has_reply)
        reply = xcb_wait_for_reply(x->c, seq, &err);
    else
        err = xcb_request_check(x->c, (xcb_void_cookie_t) { seq });
    if (err) {
        *xerr = err->error_code;
        free(err);
        free(reply);
        return NULL;
    }
    if (xcb_connection_has_error(x->c)) {
        *xerr = -1;
        free(reply);
        return NULL;
    }
    return reply;
}

static int
need_xns(xpctl *x)
{
    if (!x->have_xns)
        return fail(x, "X-NAMESPACE is not available: this is not XPrism, "
                    "namespaces are not enabled, or this program runs inside "
                    "a sandbox");
    return 0;
}

xpctl *
xpctl_open(const char *display)
{
    xpctl *x = calloc(1, sizeof(*x));
    if (!x)
        return NULL;
    x->c = xcb_connect(display, NULL);
    if (xcb_connection_has_error(x->c)) {
        fail(x, "cannot connect to the X server %s",
             display ? display : getenv("DISPLAY") ? getenv("DISPLAY") : "");
        return x;
    }
    x->root = xcb_setup_roots_iterator(xcb_get_setup(x->c)).data->root;
    uint8_t dummy;
    x->have_scene = query_ext(x, "XPRISM-SCENE", &dummy, NULL);
    x->have_color = query_ext(x, "XPRISM-COLOR", &x->color_op, NULL);
    x->have_xns = query_ext(x, "X-NAMESPACE", &x->xns_op, &x->xns_event);
    if (x->have_xns) {
        uint16_t v[2] = { 1, 2 };
        int xerr;
        uint8_t *r = ext_request(x, x->xns_op, XNS_QUERY_VERSION, v, 4, true, &xerr);
        if (r) {
            uint16_t minor;
            memcpy(&minor, r + 10, 2);
            x->xns_minor = minor;
            free(r);
        } else
            x->have_xns = false;
    }
    return x;
}

void
xpctl_close(xpctl *x)
{
    if (!x)
        return;
    if (x->c)
        xcb_disconnect(x->c);
    free(x);
}

const char *
xpctl_error(const xpctl *x)
{
    return x->error;
}

xcb_connection_t *
xpctl_connection(xpctl *x)
{
    return xcb_connection_has_error(x->c) ? NULL : x->c;
}

int
xpctl_fd(xpctl *x)
{
    return xcb_get_file_descriptor(x->c);
}

/* ---- the server ---------------------------------------------------- */

int
xpctl_server_info(xpctl *x, xpctl_server *out)
{
    memset(out, 0, sizeof(*out));
    if (!xpctl_connection(x))
        return fail(x, "not connected");
    const xcb_setup_t *s = xcb_get_setup(x->c);
    int n = xcb_setup_vendor_length(s);
    if (n >= (int) sizeof(out->vendor))
        n = sizeof(out->vendor) - 1;
    memcpy(out->vendor, xcb_setup_vendor(s), n);
    out->release = s->release_number;
    out->is_xprism = strncmp(out->vendor, "XPrism", 6) == 0;
    /* XPrism: 1 YY MM N PP, i.e. 10000000 + YY*100000 + MM*1000 + NN*100 + PP */
    unsigned r = out->release % 10000000;
    if (out->is_xprism && r % 100)
        snprintf(out->version, sizeof(out->version), "%02u.%02u.%02u.%u", r / 100000,
                 r / 1000 % 100, r / 100 % 10, r % 100);
    else if (out->is_xprism)
        snprintf(out->version, sizeof(out->version), "%02u.%02u.%02u", r / 100000,
                 r / 1000 % 100, r / 100 % 10);
    else
        snprintf(out->version, sizeof(out->version), "%u", out->release);
    out->compositor = x->have_scene;
    out->color = x->have_color;
    out->namespaces = x->have_xns;
    out->namespaces_minor = x->xns_minor;
    return 0;
}

/* ---- displays ------------------------------------------------------ */

static const char *const setting_props[] = { "HDR", "WIDE_GAMUT", "EDR" };
static const char *const setting_names[] = { "hdr", "wide-gamut", "edr" };

const char *
xpctl_setting_name(enum xpctl_setting s)
{
    return setting_names[s];
}

static xcb_atom_t
atom(xpctl *x, const char *name, bool only_if_exists)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(x->c,
        xcb_intern_atom(x->c, only_if_exists, strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

/* An output's HDR / WIDE_GAMUT / EDR: 0 or 1, or XPCTL_UNAVAILABLE.
   XPrism's modesetting driver uses INTEGER 0/1; Xvfb's EDR is a string,
   the headroom or empty. */
static int
setting_value(xpctl *x, xcb_randr_output_t o, xcb_atom_t prop)
{
    if (prop == XCB_NONE)
        return XPCTL_UNAVAILABLE;
    xcb_randr_get_output_property_reply_t *r = xcb_randr_get_output_property_reply(x->c,
        xcb_randr_get_output_property(x->c, o, prop, XCB_ATOM_ANY, 0, 16, 0, 0), NULL);
    int v = XPCTL_UNAVAILABLE;
    if (r && r->type == XCB_ATOM_INTEGER && r->format == 32 && r->num_items >= 1) {
        int32_t i;
        memcpy(&i, xcb_randr_get_output_property_data(r), 4);
        v = i != 0;
    } else if (r && r->type == XCB_ATOM_STRING && r->format == 8)
        v = r->num_items > 0;
    free(r);
    return v;
}

static const char *
primaries_name(int p)
{
    static const char *const names[] = { "custom", "sRGB", "Display P3", "BT.2020" };
    return p >= 0 && p < 4 ? names[p] : "unknown";
}

static const char *
transfer_name(int t)
{
    static const char *const names[] = { "sRGB", "gamma 2.2", "linear", "PQ", "HLG" };
    return t >= 0 && t < 5 ? names[t] : "unknown";
}

static void
output_color(xpctl *x, xpctl_output *o)
{
    if (!x->have_color)
        return;
    int xerr;
    uint8_t *r = ext_request(x, x->color_op, XPC_GET_OUTPUT_COLOR_SPACE, &o->id, 4, true, &xerr);
    if (!r)
        return;
    /* set (byte 1); primaries, transfer at 8; refWhite, minLum, maxLum
       after eight chromaticity words */
    o->color_known = true;
    o->primaries = primaries_name(r[8]);
    o->transfer = transfer_name(r[9]);
    memcpy(&o->ref_white, r + 12 + 32, 4);
    memcpy(&o->max_lum, r + 12 + 40, 4);
    free(r);
}

int
xpctl_outputs(xpctl *x, xpctl_output **outs, int *n)
{
    *outs = NULL;
    *n = 0;
    if (!xpctl_connection(x))
        return fail(x, "not connected");
    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(x->c,
            xcb_randr_get_screen_resources_current(x->c, x->root), NULL);
    if (!res)
        return fail(x, "RandR is not available");
    xcb_atom_t props[3];
    for (int i = 0; i < 3; i++)
        props[i] = atom(x, setting_props[i], true);

    int count = xcb_randr_get_screen_resources_current_outputs_length(res);
    xcb_randr_output_t *ids = xcb_randr_get_screen_resources_current_outputs(res);
    xcb_randr_mode_info_t *modes = xcb_randr_get_screen_resources_current_modes(res);
    int nmodes = xcb_randr_get_screen_resources_current_modes_length(res);
    xpctl_output *list = calloc(count ? count : 1, sizeof(*list));
    if (!list) {
        free(res);
        return fail(x, "out of memory");
    }
    for (int i = 0; i < count; i++) {
        xpctl_output *o = &list[i];
        o->id = ids[i];
        xcb_randr_get_output_info_reply_t *info = xcb_randr_get_output_info_reply(x->c,
            xcb_randr_get_output_info(x->c, ids[i], res->config_timestamp), NULL);
        if (info) {
            int len = xcb_randr_get_output_info_name_length(info);
            if (len >= (int) sizeof(o->name))
                len = sizeof(o->name) - 1;
            memcpy(o->name, xcb_randr_get_output_info_name(info), len);
            o->connected = info->connection == XCB_RANDR_CONNECTION_CONNECTED;
            if (info->crtc) {
                xcb_randr_get_crtc_info_reply_t *crtc = xcb_randr_get_crtc_info_reply(x->c,
                    xcb_randr_get_crtc_info(x->c, info->crtc, res->config_timestamp), NULL);
                if (crtc && crtc->mode) {
                    o->enabled = true;
                    o->width = crtc->width;
                    o->height = crtc->height;
                    for (int m = 0; m < nmodes; m++) {
                        if (modes[m].id != crtc->mode || !modes[m].htotal || !modes[m].vtotal)
                            continue;
                        o->refresh = (double) modes[m].dot_clock /
                                     ((double) modes[m].htotal * modes[m].vtotal);
                    }
                }
                free(crtc);
            }
            free(info);
        }
        o->hdr = setting_value(x, o->id, props[XPCTL_HDR]);
        o->wide_gamut = setting_value(x, o->id, props[XPCTL_WIDE_GAMUT]);
        o->edr = setting_value(x, o->id, props[XPCTL_EDR]);
        output_color(x, o);
    }
    free(res);
    *outs = list;
    *n = count;
    return 0;
}

int
xpctl_output_set(xpctl *x, const char *name, enum xpctl_setting s, bool on)
{
    xpctl_output *list;
    int n;
    if (xpctl_outputs(x, &list, &n) < 0)
        return -1;
    xpctl_output *o = NULL;
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].name, name))
            o = &list[i];
    if (!o) {
        free(list);
        return fail(x, "no output %s", name);
    }
    int now = s == XPCTL_HDR ? o->hdr : s == XPCTL_WIDE_GAMUT ? o->wide_gamut : o->edr;
    uint32_t id = o->id;
    free(list);
    if (now == XPCTL_UNAVAILABLE)
        return fail(x, "%s has no %s setting", name, setting_names[s]);

    xcb_atom_t prop = atom(x, setting_props[s], true);
    xcb_randr_get_output_property_reply_t *cur = xcb_randr_get_output_property_reply(x->c,
        xcb_randr_get_output_property(x->c, id, prop, XCB_ATOM_ANY, 0, 0, 0, 0), NULL);
    bool string = cur && cur->type == XCB_ATOM_STRING;
    free(cur);
    xcb_void_cookie_t ck;
    if (string) {
        /* Xvfb's EDR: the headroom, or empty for none */
        const char *v = on ? "4" : "";
        ck = xcb_randr_change_output_property_checked(x->c, id, prop, XCB_ATOM_STRING, 8,
                                                      XCB_PROP_MODE_REPLACE, strlen(v), v);
    } else {
        int32_t v = on;
        ck = xcb_randr_change_output_property_checked(x->c, id, prop, XCB_ATOM_INTEGER, 32,
                                                      XCB_PROP_MODE_REPLACE, 1, &v);
    }
    xcb_generic_error_t *err = xcb_request_check(x->c, ck);
    if (err) {
        int code = err->error_code;
        free(err);
        if (code == 10)
            return fail(x, "not allowed to change the display settings");
        return fail(x, "%s cannot turn %s %s (the monitor or driver cannot)",
                    name, setting_names[s], on ? "on" : "off");
    }
    return 0;
}

/* ---- capabilities -------------------------------------------------- */

static const struct {
    const char *name, *what;
} caps[XPCTL_NUM_CAPS] = {
    { "capture.screen",     "take pictures of the whole screen" },
    { "capture.window",     "read what other applications' windows show" },
    { "input.inject",       "type and click on your behalf" },
    { "input.monitor",      "see what you type and click in other applications" },
    { "input.virtual_seat", "add input devices of its own" },
    { "window.enumerate",   "list the windows of other applications" },
    { "window.inspect",     "read details of other applications' windows" },
    { "window.manage",      "move, resize and manage other applications' windows" },
    { "clipboard.read",     "read the clipboard" },
    { "clipboard.write",    "change the clipboard" },
    { "display.configure",  "change the display settings" },
    { "compositor.legacy",  "take over drawing the screen" },
};

const char *
xpctl_cap_name(int bit)
{
    return bit >= 0 && bit < XPCTL_NUM_CAPS ? caps[bit].name : "unknown";
}

const char *
xpctl_cap_what(int bit)
{
    return bit >= 0 && bit < XPCTL_NUM_CAPS ? caps[bit].what : "do something";
}

int
xpctl_cap_parse(const char *name)
{
    for (int i = 0; i < XPCTL_NUM_CAPS; i++)
        if (!strcmp(name, caps[i].name))
            return i;
    return -1;
}

/* ---- namespaces ---------------------------------------------------- */

/* fixed fields, then name length (CARD16) + pad, then the name, padded */
static size_t
put_name(uint8_t *buf, size_t at, const char *name)
{
    size_t len = strlen(name);
    memcpy(buf + at, name, len);
    return at + ((len + 3) & ~(size_t) 3);
}

static int
xns_ack(xpctl *x, uint8_t minor, const uint8_t *body, size_t len, const char *what)
{
    int xerr;
    uint8_t *r = ext_request(x, x->xns_op, minor, body, len, true, &xerr);
    if (!r)
        return fail(x, "%s: %s", what, xerr < 0 ? "the connection closed" : x_error_text(xerr));
    free(r);
    return 0;
}

static int
check_name(xpctl *x, const char *name)
{
    size_t len = strlen(name);
    if (!len || len > 255)
        return fail(x, "a namespace name has 1 to 255 characters");
    return 0;
}

int
xpctl_namespaces(xpctl *x, xpctl_namespace **out, int *n)
{
    *out = NULL;
    *n = 0;
    if (need_xns(x) < 0)
        return -1;
    int xerr;
    uint8_t *r = ext_request(x, x->xns_op, XNS_LIST_NAMESPACES, NULL, 0, true, &xerr);
    if (!r)
        return fail(x, "cannot list namespaces: %s", x_error_text(xerr));
    uint32_t length, count;
    memcpy(&length, r + 4, 4);
    memcpy(&count, r + 8, 4);
    xpctl_namespace *list = calloc(count ? count : 1, sizeof(*list));
    if (!list) {
        free(r);
        return fail(x, "out of memory");
    }
    const uint8_t *p = r + 32, *end = r + 32 + 4 * (size_t) length;
    uint32_t got = 0;
    while (got < count && p + 20 <= end) {
        xpctl_namespace *ns = &list[got];
        uint32_t f[4];
        uint16_t len;
        memcpy(f, p, 16);
        memcpy(&len, p + 16, 2);
        p += 20;
        if (p + len > end)
            break;
        memcpy(ns->name, p, len < 255 ? len : 255);
        p += (len + 3) & ~3u;
        ns->admin = f[0] & XNS_CAP_ADMIN;
        ns->builtin = f[1] & XNS_ATTR_IMMUTABLE;
        ns->transient = f[1] & XNS_ATTR_TRANSIENT;
        ns->clients = f[2];
        got++;
    }
    free(r);
    /* grants: from QueryNamespace */
    for (uint32_t i = 0; i < got; i++) {
        uint8_t body[8 + 256] = { 0 };
        uint16_t len = strlen(list[i].name);
        memcpy(body, &len, 2);
        size_t blen = put_name(body, 4, list[i].name);
        uint8_t *q = ext_request(x, x->xns_op, XNS_QUERY_NAMESPACE, body, blen, true, &xerr);
        if (q) {
            memcpy(&list[i].grants, q + 24, 4);
            free(q);
        }
    }
    *out = list;
    *n = got;
    return 0;
}

int
xpctl_namespace_create(xpctl *x, const char *name, uint32_t grants, bool transient)
{
    if (need_xns(x) < 0 || check_name(x, name) < 0)
        return -1;
    uint8_t body[12 + 256] = { 0 };
    uint32_t f[2] = { XNS_CAPS_FOR_APPS, transient ? XNS_ATTR_TRANSIENT : 0 };
    uint16_t len = strlen(name);
    memcpy(body, f, 8);
    memcpy(body + 8, &len, 2);
    size_t blen = put_name(body, 12, name);
    if (xns_ack(x, XNS_CREATE_NAMESPACE, body, blen, "cannot create the namespace") < 0)
        return -1;
    return grants ? xpctl_namespace_set_grants(x, name, grants) : 0;
}

int
xpctl_namespace_delete(xpctl *x, const char *name, bool close_clients)
{
    if (need_xns(x) < 0 || check_name(x, name) < 0)
        return -1;
    uint8_t body[4 + 256] = { 0 };
    uint16_t len = strlen(name);
    body[0] = close_clients;
    memcpy(body + 2, &len, 2);
    size_t blen = put_name(body, 4, name);
    int xerr;
    uint8_t *r = ext_request(x, x->xns_op, XNS_DELETE_NAMESPACE, body, blen, true, &xerr);
    if (!r) {
        if (xerr == 10)
            return fail(x, "cannot delete %s: it is built in, or apps still use it "
                        "(--close-apps closes them)", name);
        return fail(x, "cannot delete %s: %s", name, x_error_text(xerr));
    }
    free(r);
    return 0;
}

int
xpctl_namespace_set_grants(xpctl *x, const char *name, uint32_t grants)
{
    if (need_xns(x) < 0 || check_name(x, name) < 0)
        return -1;
    uint8_t body[8 + 256] = { 0 };
    uint16_t len = strlen(name);
    memcpy(body, &grants, 4);
    memcpy(body + 4, &len, 2);
    size_t blen = put_name(body, 8, name);
    int xerr;
    uint8_t *r = ext_request(x, x->xns_op, XNS_SET_NAMESPACE_GRANTS, body, blen, true, &xerr);
    if (!r) {
        if (xerr == 10)
            return fail(x, "%s is built in: its permissions are fixed", name);
        return fail(x, "cannot change %s: %s", name, x_error_text(xerr));
    }
    free(r);
    return 0;
}

/* ---- apps ---------------------------------------------------------- */

static const uint8_t *
take_str(char *dst, size_t size, const uint8_t *p, uint16_t len, const uint8_t *end)
{
    if (p + len > end)
        return end;
    size_t n = len < size - 1 ? len : size - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
    return p + ((len + 3) & ~3u);
}

/* the program's name: argv[0] without its directory, else comm (which
   the kernel cuts to 15 characters) */
static void
command_of(int pid, char *out, size_t size)
{
    char path[64], buf[256];
    out[0] = '\0';
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE *f = fopen(path, "re");
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        if (n && buf[0]) {
            const char *base = strrchr(buf, '/');
            base = base ? base + 1 : buf;
            size_t len = strnlen(base, size - 1);
            memcpy(out, base, len);
            out[len] = '\0';
            return;
        }
    }
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    f = fopen(path, "re");
    if (!f)
        return;
    if (fgets(out, size, f))
        out[strcspn(out, "\n")] = '\0';
    fclose(f);
}

int
xpctl_client_info(xpctl *x, uint32_t resource, xpctl_client *c)
{
    memset(c, 0, sizeof(*c));
    c->pid = -1;
    if (need_xns(x) < 0)
        return -1;
    int xerr;
    uint8_t *r = ext_request(x, x->xns_op, XNS_GET_CLIENT_INFO, &resource, 4, true, &xerr);
    if (!r)
        return fail(x, "no such client");
    uint32_t length, pid;
    uint16_t lens[4];
    memcpy(&length, r + 4, 4);
    memcpy(&pid, r + 8, 4);
    memcpy(lens, r + 16, 8);
    memcpy(&c->grants, r + 24, 4);
    c->base = resource;
    c->sandboxed = r[1] & XNS_CLIENT_SOCKET;
    if ((r[1] & XNS_CLIENT_PID) && pid) {
        c->pid = (int) pid;
        command_of(c->pid, c->command, sizeof(c->command));
    }
    const uint8_t *p = r + 32, *end = r + 32 + 4 * (size_t) length;
    char instance[256];
    p = take_str(c->ns, sizeof(c->ns), p, lens[0], end);
    p = take_str(c->app_id, sizeof(c->app_id), p, lens[1], end);
    p = take_str(instance, sizeof(instance), p, lens[2], end);
    take_str(c->engine, sizeof(c->engine), p, lens[3], end);
    free(r);
    return 0;
}

int
xpctl_clients(xpctl *x, xpctl_client **out, int *n)
{
    *out = NULL;
    *n = 0;
    if (need_xns(x) < 0)
        return -1;
    xcb_res_query_clients_reply_t *r = xcb_res_query_clients_reply(x->c,
        xcb_res_query_clients(x->c), NULL);
    if (!r)
        return fail(x, "X-Resource is not available");
    int count = xcb_res_query_clients_clients_length(r);
    xcb_res_client_t *cl = xcb_res_query_clients_clients(r);
    xpctl_client *list = calloc(count ? count : 1, sizeof(*list));
    if (!list) {
        free(r);
        return fail(x, "out of memory");
    }
    uint32_t self = xcb_get_setup(x->c)->resource_id_base;
    int got = 0;
    for (int i = 0; i < count; i++) {
        /* not the server itself, not us */
        if (!cl[i].resource_base || cl[i].resource_base == self)
            continue;
        if (xpctl_client_info(x, cl[i].resource_base, &list[got]) == 0)
            got++;
    }
    free(r);
    x->error[0] = '\0';
    *out = list;
    *n = got;
    return 0;
}

/* ---- permission prompts -------------------------------------------- */

int
xpctl_agent(xpctl *x, bool enable)
{
    if (need_xns(x) < 0)
        return -1;
    if (x->xns_minor < 2)
        return fail(x, "this XPrism has no permission prompts (X-NAMESPACE 1.%d)",
                    x->xns_minor);
    uint8_t body[4] = { enable };
    int xerr;
    ext_request(x, x->xns_op, XNS_SET_PERMISSION_AGENT, body, 4, false, &xerr);
    if (xerr == 10)
        return fail(x, "another program already answers permission prompts");
    if (xerr)
        return fail(x, "cannot answer permission prompts: %s", x_error_text(xerr));
    return 0;
}

/* a PermissionRequest: fill p and return 1; anything else is dropped */
static int
prompt_from(xpctl *x, xcb_generic_event_t *ev, xpctl_prompt *p)
{
    const uint8_t *e = (const uint8_t *) ev;
    int ours = (ev->response_type & 0x7f) == x->xns_event;
    if (ours) {
        uint32_t resource;
        memset(p, 0, sizeof(*p));
        p->cap = e[1];
        memcpy(&p->id, e + 4, 4);
        memcpy(&resource, e + 8, 4);
        if (xpctl_client_info(x, resource, &p->client) < 0)
            snprintf(p->client.ns, sizeof(p->client.ns), "?");
    }
    free(ev);
    return ours;
}

int
xpctl_agent_next(xpctl *x, xpctl_prompt *p, int timeout_ms)
{
    for (bool waited = false;; waited = true) {
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(x->c)))
            if (prompt_from(x, ev, p))
                return 1;
        if (xcb_connection_has_error(x->c))
            return fail(x, "the X connection closed");
        if (waited || timeout_ms == 0)
            return 0;
        struct pollfd pfd = { .fd = xcb_get_file_descriptor(x->c), .events = POLLIN };
        if (poll(&pfd, 1, timeout_ms) < 0)
            return errno == EINTR ? 0 : fail(x, "poll failed: %s", strerror(errno));
    }
}

int
xpctl_agent_answer(xpctl *x, uint32_t id, enum xpctl_answer a)
{
    uint8_t body[8] = { 0 };
    memcpy(body, &id, 4);
    body[4] = (uint8_t) a;
    int xerr;
    ext_request(x, x->xns_op, XNS_ANSWER_PERMISSION, body, 8, false, &xerr);
    if (xerr)
        return fail(x, "cannot answer: %s", x_error_text(xerr));
    return 0;
}

/* ---- display layout ------------------------------------------------ */

static double
mode_refresh(const xcb_randr_mode_info_t *m)
{
    if (!m->htotal || !m->vtotal)
        return 0;
    double v = m->vtotal;
    if (m->mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN)
        v *= 2;
    if (m->mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE)
        v /= 2;
    return (double) m->dot_clock / ((double) m->htotal * v);
}

static uint16_t
rotation_bits(int degrees)
{
    switch (degrees) {
    case 90: return XCB_RANDR_ROTATION_ROTATE_90;
    case 180: return XCB_RANDR_ROTATION_ROTATE_180;
    case 270: return XCB_RANDR_ROTATION_ROTATE_270;
    default: return XCB_RANDR_ROTATION_ROTATE_0;
    }
}

static int
rotation_degrees(uint16_t bits)
{
    if (bits & XCB_RANDR_ROTATION_ROTATE_90)
        return 90;
    if (bits & XCB_RANDR_ROTATION_ROTATE_180)
        return 180;
    if (bits & XCB_RANDR_ROTATION_ROTATE_270)
        return 270;
    return 0;
}

static double
absd(double v)
{
    return v < 0 ? -v : v;
}

typedef struct {
    xcb_randr_get_screen_resources_current_reply_t *res;
    xcb_randr_mode_info_t *modes;
    int nmodes;
    xcb_randr_output_t *outputs;
    int noutputs;
} resources;

static int
resources_get(xpctl *x, resources *r)
{
    memset(r, 0, sizeof(*r));
    if (!xpctl_connection(x))
        return fail(x, "not connected");
    r->res = xcb_randr_get_screen_resources_current_reply(x->c,
        xcb_randr_get_screen_resources_current(x->c, x->root), NULL);
    if (!r->res)
        return fail(x, "RandR is not available");
    r->modes = xcb_randr_get_screen_resources_current_modes(r->res);
    r->nmodes = xcb_randr_get_screen_resources_current_modes_length(r->res);
    r->outputs = xcb_randr_get_screen_resources_current_outputs(r->res);
    r->noutputs = xcb_randr_get_screen_resources_current_outputs_length(r->res);
    return 0;
}

static const xcb_randr_mode_info_t *
resources_mode(const resources *r, xcb_randr_mode_t id)
{
    for (int i = 0; i < r->nmodes; i++)
        if (r->modes[i].id == id)
            return &r->modes[i];
    return NULL;
}

static xcb_randr_get_output_info_reply_t *
output_info(xpctl *x, const resources *r, xcb_randr_output_t id)
{
    return xcb_randr_get_output_info_reply(x->c,
        xcb_randr_get_output_info(x->c, id, r->res->config_timestamp), NULL);
}

static void
info_name(xcb_randr_get_output_info_reply_t *info, char *out, size_t size)
{
    int len = xcb_randr_get_output_info_name_length(info);
    if (len >= (int) size)
        len = size - 1;
    memcpy(out, xcb_randr_get_output_info_name(info), len);
    out[len] = 0;
}

/* the output called name: its id and info (free it) */
static xcb_randr_get_output_info_reply_t *
find_output(xpctl *x, const resources *r, const char *name, xcb_randr_output_t *id)
{
    for (int i = 0; i < r->noutputs; i++) {
        xcb_randr_get_output_info_reply_t *info = output_info(x, r, r->outputs[i]);
        if (!info)
            continue;
        char n[32];
        info_name(info, n, sizeof(n));
        if (!strcmp(n, name)) {
            *id = r->outputs[i];
            return info;
        }
        free(info);
    }
    return NULL;
}

static int
mode_cmp(const void *a, const void *b)
{
    const xpctl_mode *m = a, *o = b;
    long am = (long) m->width * m->height, ao = (long) o->width * o->height;
    if (am != ao)
        return am > ao ? -1 : 1;
    if (m->width != o->width)
        return m->width > o->width ? -1 : 1;
    if (m->refresh != o->refresh)
        return m->refresh > o->refresh ? -1 : 1;
    return 0;
}

int
xpctl_output_modes(xpctl *x, const char *name, xpctl_mode **modes, int *n)
{
    *modes = NULL;
    *n = 0;
    resources r;
    if (resources_get(x, &r) < 0)
        return -1;
    xcb_randr_output_t id;
    xcb_randr_get_output_info_reply_t *info = find_output(x, &r, name, &id);
    if (!info) {
        free(r.res);
        return fail(x, "no output %s", name);
    }
    xcb_randr_mode_t current = 0;
    if (info->crtc) {
        xcb_randr_get_crtc_info_reply_t *c = xcb_randr_get_crtc_info_reply(x->c,
            xcb_randr_get_crtc_info(x->c, info->crtc, r.res->config_timestamp), NULL);
        if (c)
            current = c->mode;
        free(c);
    }
    int count = xcb_randr_get_output_info_modes_length(info);
    xcb_randr_mode_t *ids = xcb_randr_get_output_info_modes(info);
    xpctl_mode *list = calloc(count ? count : 1, sizeof(*list));
    if (!list) {
        free(info);
        free(r.res);
        return fail(x, "out of memory");
    }
    int k = 0;
    for (int i = 0; i < count; i++) {
        const xcb_randr_mode_info_t *m = resources_mode(&r, ids[i]);
        if (!m)
            continue;
        xpctl_mode mode = {
            .width = m->width, .height = m->height, .refresh = mode_refresh(m),
            .preferred = i < info->num_preferred, .current = ids[i] == current,
        };
        /* the same size and rate twice (other timings): keep one */
        int dup = -1;
        for (int j = 0; j < k; j++)
            if (list[j].width == mode.width && list[j].height == mode.height &&
                absd(list[j].refresh - mode.refresh) < 0.01)
                dup = j;
        if (dup >= 0) {
            list[dup].preferred |= mode.preferred;
            list[dup].current |= mode.current;
            continue;
        }
        list[k++] = mode;
    }
    qsort(list, k, sizeof(*list), mode_cmp);
    free(info);
    free(r.res);
    *modes = list;
    *n = k;
    return 0;
}

int
xpctl_layout_get(xpctl *x, xpctl_layout **out, int *n)
{
    *out = NULL;
    *n = 0;
    resources r;
    if (resources_get(x, &r) < 0)
        return -1;
    xcb_randr_get_output_primary_reply_t *pr = xcb_randr_get_output_primary_reply(x->c,
        xcb_randr_get_output_primary(x->c, x->root), NULL);
    xcb_randr_output_t primary = pr ? pr->output : 0;
    free(pr);
    xpctl_layout *list = calloc(r.noutputs ? r.noutputs : 1, sizeof(*list));
    if (!list) {
        free(r.res);
        return fail(x, "out of memory");
    }
    int k = 0;
    for (int i = 0; i < r.noutputs; i++) {
        xcb_randr_get_output_info_reply_t *info = output_info(x, &r, r.outputs[i]);
        if (!info)
            continue;
        if (info->connection != XCB_RANDR_CONNECTION_CONNECTED) {
            free(info);
            continue;
        }
        xpctl_layout *l = &list[k++];
        info_name(info, l->name, sizeof(l->name));
        l->primary = r.outputs[i] == primary;
        if (info->crtc) {
            xcb_randr_get_crtc_info_reply_t *c = xcb_randr_get_crtc_info_reply(x->c,
                xcb_randr_get_crtc_info(x->c, info->crtc, r.res->config_timestamp), NULL);
            const xcb_randr_mode_info_t *m = c ? resources_mode(&r, c->mode) : NULL;
            if (c && m) {
                l->enabled = true;
                l->width = m->width;
                l->height = m->height;
                l->refresh = mode_refresh(m);
                l->x = c->x;
                l->y = c->y;
                l->rotation = rotation_degrees(c->rotation);
            }
            free(c);
        }
        free(info);
    }
    free(r.res);
    *out = list;
    *n = k;
    return 0;
}

#define MAX_CRTC_OUTPUTS 8

typedef struct {
    xcb_randr_crtc_t id;
    uint16_t rotations;         /* what it can do */
    /* now */
    xcb_randr_mode_t mode;
    int16_t x, y;
    uint16_t rotation;
    int w, h;                   /* on the screen (rotated) */
    xcb_randr_output_t out[MAX_CRTC_OUTPUTS];
    int nout;
    /* planned */
    bool changed;
    xcb_randr_mode_t nmode;
    int16_t nx, ny;
    uint16_t nrot;
    int nw, nh;
    xcb_randr_output_t nout_ids[MAX_CRTC_OUTPUTS];
    int nnout;
} crtc_plan;

static void
rotated_size(const xcb_randr_mode_info_t *m, uint16_t rot, int *w, int *h)
{
    bool swap = rot & (XCB_RANDR_ROTATION_ROTATE_90 | XCB_RANDR_ROTATION_ROTATE_270);
    *w = m ? (swap ? m->height : m->width) : 0;
    *h = m ? (swap ? m->width : m->height) : 0;
}

static bool
set_crtc(xpctl *x, const resources *r, xcb_randr_crtc_t id, xcb_randr_mode_t mode,
         int16_t cx, int16_t cy, uint16_t rot, int nout, const xcb_randr_output_t *outs)
{
    xcb_randr_set_crtc_config_reply_t *rep = xcb_randr_set_crtc_config_reply(x->c,
        xcb_randr_set_crtc_config(x->c, id, XCB_CURRENT_TIME, r->res->config_timestamp,
                                  cx, cy, mode, mode ? rot : XCB_RANDR_ROTATION_ROTATE_0,
                                  mode ? nout : 0, outs), NULL);
    bool ok = rep && rep->status == XCB_RANDR_SET_CONFIG_SUCCESS;
    free(rep);
    return ok;
}

static bool
set_screen_size(xpctl *x, int w, int h)
{
    const xcb_setup_t *setup = xcb_get_setup(x->c);
    xcb_screen_t *s = xcb_setup_roots_iterator(setup).data;
    for (xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup); it.rem;
         xcb_screen_next(&it))
        if (it.data->root == x->root)
            s = it.data;
    /* keep the DPI the screen has */
    uint32_t mmw = s->width_in_pixels ? (uint32_t) w * s->width_in_millimeters / s->width_in_pixels
                                      : (uint32_t) w * 254 / 960;
    uint32_t mmh = s->height_in_pixels ? (uint32_t) h * s->height_in_millimeters / s->height_in_pixels
                                       : (uint32_t) h * 254 / 960;
    xcb_generic_error_t *err = xcb_request_check(x->c,
        xcb_randr_set_screen_size_checked(x->c, x->root, w, h, mmw ? mmw : 1, mmh ? mmh : 1));
    free(err);
    return !err;
}

int
xpctl_layout_apply(xpctl *x, const xpctl_layout *list, int n)
{
    resources r;
    if (resources_get(x, &r) < 0)
        return -1;
    int ncrtc = xcb_randr_get_screen_resources_current_crtcs_length(r.res);
    xcb_randr_crtc_t *crtc_ids = xcb_randr_get_screen_resources_current_crtcs(r.res);
    crtc_plan *p = calloc(ncrtc ? ncrtc : 1, sizeof(*p));
    if (!p) {
        free(r.res);
        return fail(x, "out of memory");
    }
    int ret = -1;
    int old_w = 0, old_h = 0;
    for (int i = 0; i < ncrtc; i++) {
        p[i].id = crtc_ids[i];
        xcb_randr_get_crtc_info_reply_t *c = xcb_randr_get_crtc_info_reply(x->c,
            xcb_randr_get_crtc_info(x->c, crtc_ids[i], r.res->config_timestamp), NULL);
        if (!c)
            continue;
        p[i].rotations = c->rotations;
        p[i].mode = c->mode;
        p[i].x = c->x;
        p[i].y = c->y;
        p[i].rotation = c->rotation;
        p[i].w = c->width;
        p[i].h = c->height;
        int no = xcb_randr_get_crtc_info_outputs_length(c);
        if (no > MAX_CRTC_OUTPUTS)
            no = MAX_CRTC_OUTPUTS;
        memcpy(p[i].out, xcb_randr_get_crtc_info_outputs(c), no * sizeof(*p[i].out));
        p[i].nout = no;
        free(c);
        /* the plan starts as what is there */
        p[i].nmode = p[i].mode;
        p[i].nx = p[i].x;
        p[i].ny = p[i].y;
        p[i].nrot = p[i].rotation;
        p[i].nw = p[i].w;
        p[i].nh = p[i].h;
        memcpy(p[i].nout_ids, p[i].out, sizeof(p[i].out));
        p[i].nnout = p[i].nout;
        if (p[i].mode) {
            if (p[i].x + p[i].w > old_w)
                old_w = p[i].x + p[i].w;
            if (p[i].y + p[i].h > old_h)
                old_h = p[i].y + p[i].h;
        }
    }

    xcb_randr_output_t primary = 0;
    for (int li = 0; li < n; li++) {
        const xpctl_layout *L = &list[li];
        xcb_randr_output_t oid;
        xcb_randr_get_output_info_reply_t *info = find_output(x, &r, L->name, &oid);
        if (!info) {
            fail(x, "no output %s", L->name);
            goto out;
        }
        crtc_plan *cur = NULL;
        for (int i = 0; i < ncrtc; i++)
            for (int k = 0; k < p[i].nnout; k++)
                if (p[i].nout_ids[k] == oid)
                    cur = &p[i];
        if (!L->enabled) {
            if (cur) {
                int k = 0;
                for (int j = 0; j < cur->nnout; j++)
                    if (cur->nout_ids[j] != oid)
                        cur->nout_ids[k++] = cur->nout_ids[j];
                cur->nnout = k;
                if (!k)
                    cur->nmode = 0;
                cur->changed = true;
            }
            free(info);
            continue;
        }
        if (info->connection != XCB_RANDR_CONNECTION_CONNECTED) {
            free(info);
            fail(x, "%s is not connected", L->name);
            goto out;
        }
        /* the mode: that size, at the rate asked for or the fastest */
        int count = xcb_randr_get_output_info_modes_length(info);
        xcb_randr_mode_t *ids = xcb_randr_get_output_info_modes(info);
        const xcb_randr_mode_info_t *best = NULL;
        for (int i = 0; i < count; i++) {
            const xcb_randr_mode_info_t *m = resources_mode(&r, ids[i]);
            if (!m || m->width != L->width || m->height != L->height)
                continue;
            if (!best)
                best = m;
            else if (L->refresh > 0 ? absd(mode_refresh(m) - L->refresh) <
                                          absd(mode_refresh(best) - L->refresh)
                                    : mode_refresh(m) > mode_refresh(best))
                best = m;
        }
        if (!best) {
            free(info);
            fail(x, "%s has no %dx%d mode", L->name, L->width, L->height);
            goto out;
        }
        if (L->refresh > 0 && absd(mode_refresh(best) - L->refresh) > 0.6) {
            free(info);
            fail(x, "%s has no %dx%d mode at %.2f Hz (closest: %.2f Hz)", L->name,
                 L->width, L->height, L->refresh, mode_refresh(best));
            goto out;
        }
        /* the CRTC: the one it is on, or a free one it can use */
        crtc_plan *use = cur;
        if (!use) {
            int nposs = xcb_randr_get_output_info_crtcs_length(info);
            xcb_randr_crtc_t *poss = xcb_randr_get_output_info_crtcs(info);
            for (int j = 0; j < nposs && !use; j++)
                for (int i = 0; i < ncrtc; i++)
                    if (p[i].id == poss[j] && !p[i].nmode && !p[i].nnout)
                        use = &p[i];
        }
        free(info);
        if (!use) {
            fail(x, "no display controller is free for %s", L->name);
            goto out;
        }
        uint16_t rot = rotation_bits(L->rotation);
        if (use->rotations && !(use->rotations & rot)) {
            fail(x, "%s cannot be rotated %d degrees", L->name, L->rotation);
            goto out;
        }
        if (L->x < 0 || L->y < 0 || L->x > 32767 || L->y > 32767) {
            fail(x, "position %d,%d is outside the screen", L->x, L->y);
            goto out;
        }
        use->nmode = best->id;
        use->nx = L->x;
        use->ny = L->y;
        use->nrot = rot;
        rotated_size(best, rot, &use->nw, &use->nh);
        use->nout_ids[0] = oid;
        use->nnout = 1;
        use->changed = true;
        if (L->primary)
            primary = oid;
    }

    /* the new screen: big enough for every CRTC */
    int W = 0, H = 0;
    for (int i = 0; i < ncrtc; i++) {
        if (!p[i].nmode)
            continue;
        if (p[i].nx + p[i].nw > W)
            W = p[i].nx + p[i].nw;
        if (p[i].ny + p[i].nh > H)
            H = p[i].ny + p[i].nh;
    }
    if (!W || !H) {
        fail(x, "refusing to turn every display off");
        goto out;
    }
    xcb_randr_get_screen_size_range_reply_t *range = xcb_randr_get_screen_size_range_reply(x->c,
        xcb_randr_get_screen_size_range(x->c, x->root), NULL);
    if (range) {
        bool too_big = W > range->max_width || H > range->max_height;
        int mw = range->max_width, mh = range->max_height;
        if (W < range->min_width)
            W = range->min_width;
        if (H < range->min_height)
            H = range->min_height;
        free(range);
        if (too_big) {
            fail(x, "the screen would be %dx%d; this GPU shows at most %dx%d", W, H, mw, mh);
            goto out;
        }
    }

    /* xrandr's order: off what will not fit, resize, then set */
    xcb_grab_server(x->c);
    bool ok = true, resized = false;
    for (int i = 0; i < ncrtc && ok; i++)
        if (p[i].mode && (p[i].x + p[i].w > W || p[i].y + p[i].h > H)) {
            ok = set_crtc(x, &r, p[i].id, 0, 0, 0, 0, 0, NULL);
            p[i].changed = true;
        }
    if (ok && (W != old_w || H != old_h)) {
        ok = set_screen_size(x, W, H);
        resized = ok;
    }
    for (int i = 0; i < ncrtc && ok; i++)
        if (p[i].changed)
            ok = set_crtc(x, &r, p[i].id, p[i].nmode, p[i].nx, p[i].ny, p[i].nrot,
                          p[i].nnout, p[i].nout_ids);
    if (ok && primary) {
        xcb_generic_error_t *err = xcb_request_check(x->c,
            xcb_randr_set_output_primary_checked(x->c, x->root, primary));
        ok = !err;
        free(err);
    }
    if (!ok) {
        /* put back what was there: the changed CRTCs off, the old size,
           then the old CRTCs */
        resources now;
        if (resources_get(x, &now) == 0) {
            for (int i = 0; i < ncrtc; i++)
                if (p[i].changed)
                    set_crtc(x, &now, p[i].id, 0, 0, 0, 0, 0, NULL);
            if (resized)
                set_screen_size(x, old_w, old_h);
            free(now.res);
        }
        if (resources_get(x, &now) == 0) {
            for (int i = 0; i < ncrtc; i++)
                if (p[i].changed && p[i].mode)
                    set_crtc(x, &now, p[i].id, p[i].mode, p[i].x, p[i].y, p[i].rotation,
                             p[i].nout, p[i].out);
            free(now.res);
        }
        fail(x, "the display driver refused this layout; the old one is back");
    }
    xcb_ungrab_server(x->c);
    xcb_flush(x->c);
    ret = ok ? 0 : -1;
out:
    free(p);
    free(r.res);
    return ret;
}

/* ---- the saved layout ---------------------------------------------- */

const char *
xpctl_layout_path(void)
{
    static char path[4096];
    const char *cfg = getenv("XDG_CONFIG_HOME");
    if (cfg && *cfg)
        snprintf(path, sizeof(path), "%s/xprism/displays.conf", cfg);
    else
        snprintf(path, sizeof(path), "%s/.config/xprism/displays.conf",
                 getenv("HOME") ? getenv("HOME") : ".");
    return path;
}

int
xpctl_layout_save(xpctl *x, const char *path, const xpctl_layout *list, int n)
{
    if (!path)
        path = xpctl_layout_path();
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        for (char *q = dir + 1; *q; q++)    /* mkdir -p */
            if (*q == '/') {
                *q = 0;
                mkdir(dir, 0755);
                *q = '/';
            }
        mkdir(dir, 0755);
    }
    char tmp[4200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return fail(x, "cannot write %s: %s", tmp, strerror(errno));
    fprintf(f, "# XPrism display layout: xprismctl displays save / restore\n"
               "# OUTPUT WIDTHxHEIGHT@HZ +X+Y rotate DEGREES [primary]   or   OUTPUT off\n");
    for (int i = 0; i < n; i++) {
        if (!list[i].enabled)
            fprintf(f, "%s off\n", list[i].name);
        else
            fprintf(f, "%s %dx%d@%.2f +%d+%d rotate %d%s\n", list[i].name, list[i].width,
                    list[i].height, list[i].refresh, list[i].x, list[i].y, list[i].rotation,
                    list[i].primary ? " primary" : "");
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        remove(tmp);
        return fail(x, "cannot write %s: %s", path, strerror(errno));
    }
    return 0;
}

static bool
parse_layout_line(char *line, xpctl_layout *l)
{
    memset(l, 0, sizeof(*l));
    char *save = NULL;
    char *name = strtok_r(line, " \t\r\n", &save);
    if (!name || name[0] == '#')
        return false;
    snprintf(l->name, sizeof(l->name), "%s", name);
    char *mode = strtok_r(NULL, " \t\r\n", &save);
    if (!mode)
        return false;
    if (!strcmp(mode, "off"))
        return true;
    if (sscanf(mode, "%dx%d@%lf", &l->width, &l->height, &l->refresh) < 2)
        return false;
    l->enabled = true;
    for (char *t; (t = strtok_r(NULL, " \t\r\n", &save));) {
        if (t[0] == '+')
            sscanf(t, "+%d+%d", &l->x, &l->y);
        else if (!strcmp(t, "rotate")) {
            char *d = strtok_r(NULL, " \t\r\n", &save);
            l->rotation = d ? atoi(d) : 0;
        } else if (!strcmp(t, "primary"))
            l->primary = true;
    }
    return l->width > 0 && l->height > 0;
}

int
xpctl_layout_load(const char *path, xpctl_layout **out, int *n)
{
    *out = NULL;
    *n = 0;
    if (!path)
        path = xpctl_layout_path();
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;           /* nothing saved */
    int cap = 8, k = 0;
    xpctl_layout *list = malloc(cap * sizeof(*list));
    char line[512];
    while (list && fgets(line, sizeof(line), f)) {
        if (k == cap) {
            xpctl_layout *bigger = realloc(list, 2 * cap * sizeof(*list));
            if (!bigger)
                break;
            list = bigger;
            cap *= 2;
        }
        if (parse_layout_line(line, &list[k]))
            k++;
    }
    fclose(f);
    *out = list;
    *n = list ? k : 0;
    return 0;
}

int
xpctl_layout_restore(xpctl *x, const char *path)
{
    xpctl_layout *saved, *now;
    int ns, nn;
    if (xpctl_layout_load(path, &saved, &ns) < 0)
        return -1;
    if (xpctl_layout_get(x, &now, &nn) < 0) {
        free(saved);
        return -1;
    }
    /* only the outputs connected now: a monitor that is not plugged in
       keeps its line for next time */
    int k = 0;
    for (int i = 0; i < ns; i++) {
        bool connected = false;
        for (int j = 0; j < nn; j++)
            if (!strcmp(saved[i].name, now[j].name))
                connected = true;
        if (connected)
            saved[k++] = saved[i];
    }
    free(now);
    int ret = k ? xpctl_layout_apply(x, saved, k) : 0;
    free(saved);
    return ret;
}
