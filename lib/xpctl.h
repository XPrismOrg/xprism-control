/* SPDX-License-Identifier: MIT
 *
 * libxpctl: what xprismctl and the control panel ask an XPrism server.
 *
 * One connection, plain functions, plain structs. Every function returns
 * 0 on success and -1 on failure; xpctl_error() then says why, in a
 * sentence that can be shown to the user.
 *
 * Namespaces, apps and permission prompts need the X-NAMESPACE
 * extension, which XPrism shows only to clients of the session itself
 * (the root namespace: the session's $XAUTHORITY). Sandboxed
 * applications cannot use any of this.
 */
#ifndef XPCTL_H
#define XPCTL_H

#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>

typedef struct xpctl xpctl;

xpctl *xpctl_open(const char *display);   /* NULL: $DISPLAY */
void xpctl_close(xpctl *x);
const char *xpctl_error(const xpctl *x);
xcb_connection_t *xpctl_connection(xpctl *x);
/* the fd to watch for permission prompts (xpctl_agent_next) */
int xpctl_fd(xpctl *x);

/* ---- the server ---------------------------------------------------- */

typedef struct {
    char vendor[64];
    uint32_t release;           /* the X release number */
    char version[24];           /* XPrism's YY.MM.NN[.PP], e.g. "26.11.01" */
    bool is_xprism;
    bool compositor;            /* XPRISM-SCENE: the built-in compositor */
    bool color;                 /* XPRISM-COLOR */
    bool namespaces;            /* X-NAMESPACE, as a session client */
    int namespaces_minor;       /* 1.N */
} xpctl_server;

int xpctl_server_info(xpctl *x, xpctl_server *out);

/* ---- displays ------------------------------------------------------ */

enum { XPCTL_UNAVAILABLE = -1 };

typedef struct {
    uint32_t id;                /* RandR output */
    char name[32];
    bool connected;
    bool enabled;               /* has a mode */
    int width, height;
    double refresh;             /* Hz */
    /* each 0 or 1, or XPCTL_UNAVAILABLE where the driver has no such
       setting for the output */
    int hdr, wide_gamut, edr;
    /* what the compositor shows on it (XPRISM-COLOR) */
    bool color_known;
    const char *primaries;      /* "sRGB", "Display P3", "BT.2020", ... */
    const char *transfer;       /* "sRGB", "PQ", ... */
    uint32_t ref_white, max_lum;        /* cd/m2 */
} xpctl_output;

/* *outs is malloc'd; free() it */
int xpctl_outputs(xpctl *x, xpctl_output **outs, int *n);

enum xpctl_setting { XPCTL_HDR, XPCTL_WIDE_GAMUT, XPCTL_EDR };
const char *xpctl_setting_name(enum xpctl_setting s);     /* "hdr", ... */
int xpctl_output_set(xpctl *x, const char *output, enum xpctl_setting s, bool on);

/* ---- capabilities -------------------------------------------------- */

#define XPCTL_NUM_CAPS 12
const char *xpctl_cap_name(int bit);    /* "capture.screen", ... */
const char *xpctl_cap_what(int bit);    /* "take pictures of the whole screen" */
int xpctl_cap_parse(const char *name);  /* bit, or -1 */

/* ---- namespaces ---------------------------------------------------- */

typedef struct {
    char name[256];
    uint32_t grants;            /* bit per capability */
    uint32_t clients;
    bool builtin;               /* root and anon: fixed */
    bool admin;                 /* superpower: holds everything */
    bool transient;             /* goes away with its last client */
} xpctl_namespace;

int xpctl_namespaces(xpctl *x, xpctl_namespace **list, int *n);
int xpctl_namespace_create(xpctl *x, const char *name, uint32_t grants, bool transient);
int xpctl_namespace_delete(xpctl *x, const char *name, bool close_clients);
int xpctl_namespace_set_grants(xpctl *x, const char *name, uint32_t grants);

/* ---- apps (connected clients) -------------------------------------- */

typedef struct {
    uint32_t base;              /* resource base: identifies the client */
    char ns[256];
    char app_id[256];           /* from its launcher, may be empty */
    char engine[64];            /* "flatpak", "bwrap", ... may be empty */
    int pid;                    /* -1: unknown */
    char command[64];           /* /proc/PID/comm, may be empty */
    uint32_t grants;            /* what it holds now */
    bool sandboxed;             /* came through a launcher's socket */
} xpctl_client;

int xpctl_clients(xpctl *x, xpctl_client **list, int *n);
int xpctl_client_info(xpctl *x, uint32_t resource, xpctl_client *out);

/* ---- permission prompts -------------------------------------------- */

enum xpctl_answer {
    XPCTL_DENY_ONCE = 0,
    XPCTL_ALLOW_ONCE = 1,
    XPCTL_ALLOW_ALWAYS = 2,     /* while the app runs */
    XPCTL_DENY_ALWAYS = 3,      /* and do not ask again */
};

typedef struct {
    uint32_t id;
    int cap;                    /* bit */
    xpctl_client client;
} xpctl_prompt;

/* Become the one permission agent (or stop): confined apps' requests for
   capabilities they lack then wait for an answer. */
int xpctl_agent(xpctl *x, bool enable);
/* Waits up to timeout_ms (-1: forever) for the next prompt: 1 got one,
   0 none yet, -1 error (the connection closed). */
int xpctl_agent_next(xpctl *x, xpctl_prompt *p, int timeout_ms);
int xpctl_agent_answer(xpctl *x, uint32_t id, enum xpctl_answer a);

#endif
