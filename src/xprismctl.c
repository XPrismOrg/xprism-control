/* SPDX-License-Identifier: MIT
 *
 * xprismctl: XPrism's settings from the terminal. See xprismctl(1).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "xpctl.h"

static xpctl *x;
static volatile sig_atomic_t stop;

static void
usage(FILE *f)
{
    fputs(
"Usage: xprismctl [-d DISPLAY] COMMAND\n"
"\n"
"  status                          the server and what it offers\n"
"  displays                        outputs, their modes and colour settings\n"
"  displays save|restore           remember the layout / put it back\n"
"  modes OUTPUT                    the resolutions and rates an output offers\n"
"  display OUTPUT mode WxH[@HZ]    resolution and refresh rate (fastest if no HZ)\n"
"  display OUTPUT position X Y     where it sits on the screen\n"
"  display OUTPUT rotate normal|left|inverted|right\n"
"  display OUTPUT primary          the output panels and new windows go to\n"
"  display OUTPUT on|off\n"
"  display OUTPUT SETTING on|off   SETTING: hdr, edr or wide-gamut\n"
"  apps                            connected applications and their namespaces\n"
"  namespaces                      namespaces and what each may do\n"
"  namespace create NAME [CAP...] [--transient]\n"
"  namespace delete NAME [--close-apps]\n"
"  grant NAMESPACE CAP...          allow a namespace more\n"
"  revoke NAMESPACE CAP...         take permissions away (\"all\" for every one)\n"
"  caps                            the permissions, and what each allows\n"
"  agent                           answer permission prompts here\n"
"\n"
"Namespaces, apps and prompts need a session client: run it with the\n"
"session's $XAUTHORITY, not from inside a sandbox.\n", f);
}

static int
die(void)
{
    fprintf(stderr, "xprismctl: %s\n", xpctl_error(x));
    return 1;
}

static void
print_grants(uint32_t g, bool admin)
{
    if (admin) {
        printf("everything");
        return;
    }
    if (!g) {
        printf("-");
        return;
    }
    const char *sep = "";
    for (int i = 0; i < XPCTL_NUM_CAPS; i++)
        if (g & (1u << i)) {
            printf("%s%s", sep, xpctl_cap_name(i));
            sep = ", ";
        }
}

static const char *
onoff(int v)
{
    return v == XPCTL_UNAVAILABLE ? "-" : v ? "on" : "off";
}

/* ---- commands ------------------------------------------------------ */

static int
cmd_status(void)
{
    xpctl_server s;
    if (xpctl_server_info(x, &s) < 0)
        return die();
    printf("server       %s %s\n", s.vendor, s.version);
    printf("compositor   %s\n", s.compositor ? "built in (XPRISM-SCENE)" : "none built in");
    printf("colour       %s\n", s.color ? "managed (XPRISM-COLOR)" : "not managed");
    if (s.namespaces)
        printf("namespaces   on (X-NAMESPACE 1.%d)\n", s.namespaces_minor);
    else
        printf("namespaces   not available here\n");
    if (!s.is_xprism)
        printf("\nThis is not XPrism: most of xprismctl does nothing here.\n");
    return 0;
}

static const char *
rotation_name(int degrees)
{
    switch (degrees) {
    case 90: return "left";
    case 180: return "inverted";
    case 270: return "right";
    default: return "normal";
    }
}

static int
rotation_parse(const char *name)
{
    if (!strcmp(name, "normal") || !strcmp(name, "0"))
        return 0;
    if (!strcmp(name, "left") || !strcmp(name, "90"))
        return 90;
    if (!strcmp(name, "inverted") || !strcmp(name, "180"))
        return 180;
    if (!strcmp(name, "right") || !strcmp(name, "270"))
        return 270;
    return -1;
}

static int
cmd_displays_saved(const char *what)
{
    if (!strcmp(what, "save")) {
        xpctl_layout *l;
        int n;
        if (xpctl_layout_get(x, &l, &n) < 0)
            return die();
        int r = xpctl_layout_save(x, NULL, l, n);
        free(l);
        if (r < 0)
            return die();
        printf("saved to %s\n", xpctl_layout_path());
        return 0;
    }
    if (!strcmp(what, "restore")) {
        if (xpctl_layout_restore(x, NULL) < 0)
            return die();
        printf("restored from %s\n", xpctl_layout_path());
        return 0;
    }
    return usage(stderr), 2;
}

static int
cmd_displays(void)
{
    xpctl_output *o;
    int n;
    if (xpctl_outputs(x, &o, &n) < 0)
        return die();
    xpctl_layout *lay = NULL;
    int nlay = 0;
    xpctl_layout_get(x, &lay, &nlay);
    printf("%-10s %-20s %-4s %-4s %-10s %-24s %s\n", "OUTPUT", "MODE", "HDR", "EDR",
           "WIDE-GAMUT", "SHOWS", "WHERE");
    for (int i = 0; i < n; i++) {
        char mode[32] = "off";
        if (!o[i].connected)
            snprintf(mode, sizeof(mode), "disconnected");
        else if (o[i].enabled && o[i].refresh >= 1)
            snprintf(mode, sizeof(mode), "%dx%d %.0f Hz", o[i].width, o[i].height,
                     o[i].refresh);
        else if (o[i].enabled)
            snprintf(mode, sizeof(mode), "%dx%d", o[i].width, o[i].height);
        char shows[96] = "-";
        if (o[i].color_known) {
            if (!strcmp(o[i].transfer, "PQ") || !strcmp(o[i].transfer, "HLG"))
                snprintf(shows, sizeof(shows), "%s %s, %u cd/m2 peak", o[i].primaries,
                         o[i].transfer, o[i].max_lum);
            else if (o[i].max_lum > o[i].ref_white)
                snprintf(shows, sizeof(shows), "%s, EDR up to %.1fx", o[i].primaries,
                         (double) o[i].max_lum / o[i].ref_white);
            else
                snprintf(shows, sizeof(shows), "%s", o[i].primaries);
        }
        char where[64] = "-";
        for (int j = 0; j < nlay; j++)
            if (!strcmp(lay[j].name, o[i].name) && lay[j].enabled)
                snprintf(where, sizeof(where), "+%d+%d %s%s", lay[j].x, lay[j].y,
                         rotation_name(lay[j].rotation), lay[j].primary ? " primary" : "");
        printf("%-10s %-20s %-4s %-4s %-10s %-24s %s\n", o[i].name, mode, onoff(o[i].hdr),
               onoff(o[i].edr), onoff(o[i].wide_gamut), shows, where);
    }
    free(lay);
    free(o);
    return 0;
}

static int
cmd_modes(int argc, char **argv)
{
    if (argc != 1)
        return usage(stderr), 2;
    xpctl_mode *m;
    int n;
    if (xpctl_output_modes(x, argv[0], &m, &n) < 0)
        return die();
    for (int i = 0; i < n; i++)
        printf("%5dx%-5d %7.2f Hz%s%s\n", m[i].width, m[i].height, m[i].refresh,
               m[i].current ? "  current" : "", m[i].preferred ? "  preferred" : "");
    free(m);
    return 0;
}

/* changes one output's entry in the current layout and applies it */
static int
change_layout(const char *output, int (*edit)(xpctl_layout *l, char **args), char **args)
{
    xpctl_layout *l;
    int n;
    if (xpctl_layout_get(x, &l, &n) < 0)
        return die();
    xpctl_layout *mine = NULL;
    for (int i = 0; i < n; i++)
        if (!strcmp(l[i].name, output))
            mine = &l[i];
    if (!mine) {
        free(l);
        fprintf(stderr, "xprismctl: no connected output %s\n", output);
        return 1;
    }
    int r = edit(mine, args);
    if (r) {
        free(l);
        return r;
    }
    if (!mine->enabled || !mine->width) {
        /* turned on: its preferred mode, right of the others */
        if (mine->enabled) {
            xpctl_mode *m;
            int nm;
            if (xpctl_output_modes(x, output, &m, &nm) < 0 || !nm) {
                free(l);
                return die();
            }
            int best = 0;
            for (int i = 0; i < nm; i++)
                if (m[i].preferred) {
                    best = i;
                    break;
                }
            mine->width = m[best].width;
            mine->height = m[best].height;
            mine->refresh = m[best].refresh;
            free(m);
            int right = 0;
            for (int i = 0; i < n; i++)
                if (&l[i] != mine && l[i].enabled) {
                    int w = (l[i].rotation % 180) ? l[i].height : l[i].width;
                    if (l[i].x + w > right)
                        right = l[i].x + w;
                }
            mine->x = right;
            mine->y = 0;
        }
    }
    r = xpctl_layout_apply(x, mine, 1);
    free(l);
    return r < 0 ? die() : 0;
}

static int
edit_mode(xpctl_layout *l, char **args)
{
    int w, h;
    double hz = 0;
    if (sscanf(args[0], "%dx%d@%lf", &w, &h, &hz) < 2 || w <= 0 || h <= 0) {
        fprintf(stderr, "xprismctl: give the mode as WIDTHxHEIGHT or WIDTHxHEIGHT@HZ\n");
        return 2;
    }
    l->enabled = true;
    l->width = w;
    l->height = h;
    l->refresh = hz;
    return 0;
}

static int
edit_position(xpctl_layout *l, char **args)
{
    char *end1, *end2;
    long px = strtol(args[0], &end1, 10), py = strtol(args[1], &end2, 10);
    if (*end1 || *end2 || px < 0 || py < 0) {
        fprintf(stderr, "xprismctl: the position is two numbers, 0 or more\n");
        return 2;
    }
    l->x = px;
    l->y = py;
    return 0;
}

static int
edit_rotate(xpctl_layout *l, char **args)
{
    int d = rotation_parse(args[0]);
    if (d < 0) {
        fprintf(stderr, "xprismctl: rotate normal, left, inverted or right\n");
        return 2;
    }
    l->rotation = d;
    return 0;
}

static int
edit_primary(xpctl_layout *l, char **args)
{
    (void) args;
    l->primary = true;
    return 0;
}

static int
edit_on(xpctl_layout *l, char **args)
{
    (void) args;
    if (!l->enabled) {
        l->enabled = true;
        l->width = 0;       /* change_layout picks the preferred mode */
    }
    return 0;
}

static int
edit_off(xpctl_layout *l, char **args)
{
    (void) args;
    l->enabled = false;
    return 0;
}

static int
cmd_display(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "mode"))
        return change_layout(argv[0], edit_mode, argv + 2);
    if (argc == 4 && !strcmp(argv[1], "position"))
        return change_layout(argv[0], edit_position, argv + 2);
    if (argc == 3 && !strcmp(argv[1], "rotate"))
        return change_layout(argv[0], edit_rotate, argv + 2);
    if (argc == 2 && !strcmp(argv[1], "primary"))
        return change_layout(argv[0], edit_primary, NULL);
    if (argc == 2 && !strcmp(argv[1], "on"))
        return change_layout(argv[0], edit_on, NULL);
    if (argc == 2 && !strcmp(argv[1], "off"))
        return change_layout(argv[0], edit_off, NULL);
    if (argc != 3)
        return usage(stderr), 2;
    enum xpctl_setting s;
    if (!strcasecmp(argv[1], "hdr"))
        s = XPCTL_HDR;
    else if (!strcasecmp(argv[1], "edr"))
        s = XPCTL_EDR;
    else if (!strcasecmp(argv[1], "wide-gamut"))
        s = XPCTL_WIDE_GAMUT;
    else {
        fprintf(stderr, "xprismctl: no setting %s (hdr, edr, wide-gamut)\n", argv[1]);
        return 2;
    }
    bool on;
    if (!strcmp(argv[2], "on"))
        on = true;
    else if (!strcmp(argv[2], "off"))
        on = false;
    else
        return usage(stderr), 2;
    if (xpctl_output_set(x, argv[0], s, on) < 0)
        return die();
    printf("%s: %s %s\n", argv[0], xpctl_setting_name(s), on ? "on" : "off");
    return 0;
}

static int
cmd_apps(void)
{
    xpctl_client *c;
    int n;
    if (xpctl_clients(x, &c, &n) < 0)
        return die();
    printf("%-10s %-16s %-24s %-8s %s\n", "CLIENT", "NAMESPACE", "APP", "PID", "MAY ALSO");
    for (int i = 0; i < n; i++) {
        const char *app = c[i].app_id[0] ? c[i].app_id : c[i].command[0] ? c[i].command : "-";
        char pid[16] = "-";
        if (c[i].pid > 0)
            snprintf(pid, sizeof(pid), "%d", c[i].pid);
        printf("0x%-8x %-16s %-24s %-8s ", c[i].base, c[i].ns, app, pid);
        print_grants(c[i].grants, !strcmp(c[i].ns, "root"));
        printf("\n");
    }
    free(c);
    return 0;
}

static int
cmd_namespaces(void)
{
    xpctl_namespace *ns;
    int n;
    if (xpctl_namespaces(x, &ns, &n) < 0)
        return die();
    printf("%-16s %-5s %-10s %s\n", "NAMESPACE", "APPS", "KIND", "MAY");
    for (int i = 0; i < n; i++) {
        const char *kind = ns[i].builtin ? "built in" : ns[i].transient ? "transient" : "";
        printf("%-16s %-5u %-10s ", ns[i].name, ns[i].clients, kind);
        if (!strcmp(ns[i].name, "anon"))
            printf("nothing (unauthenticated clients)");
        else
            print_grants(ns[i].grants, ns[i].admin);
        printf("\n");
    }
    free(ns);
    return 0;
}

/* CAP... into bits; "all" for every one */
static int
parse_caps(int argc, char **argv, uint32_t *out)
{
    *out = 0;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '-')
            continue;
        if (!strcmp(argv[i], "all")) {
            *out = (1u << XPCTL_NUM_CAPS) - 1;
            continue;
        }
        int bit = xpctl_cap_parse(argv[i]);
        if (bit < 0) {
            fprintf(stderr, "xprismctl: no permission %s (xprismctl caps lists them)\n",
                    argv[i]);
            return -1;
        }
        *out |= 1u << bit;
    }
    return 0;
}

static bool
has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], flag))
            return true;
    return false;
}

static int
cmd_namespace(int argc, char **argv)
{
    if (argc < 2)
        return usage(stderr), 2;
    const char *name = argv[1];
    if (!strcmp(argv[0], "create")) {
        uint32_t caps;
        if (parse_caps(argc - 2, argv + 2, &caps) < 0)
            return 2;
        if (xpctl_namespace_create(x, name, caps, has_flag(argc, argv, "--transient")) < 0)
            return die();
        printf("created %s\n", name);
        return 0;
    }
    if (!strcmp(argv[0], "delete")) {
        if (xpctl_namespace_delete(x, name, has_flag(argc, argv, "--close-apps")) < 0)
            return die();
        printf("deleted %s\n", name);
        return 0;
    }
    return usage(stderr), 2;
}

static int
cmd_grant(int argc, char **argv, bool grant)
{
    if (argc < 2)
        return usage(stderr), 2;
    uint32_t caps;
    if (parse_caps(argc - 1, argv + 1, &caps) < 0)
        return 2;
    xpctl_namespace *ns;
    int n;
    if (xpctl_namespaces(x, &ns, &n) < 0)
        return die();
    int found = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(ns[i].name, argv[0]))
            found = i;
    if (found < 0) {
        free(ns);
        fprintf(stderr, "xprismctl: no namespace %s\n", argv[0]);
        return 1;
    }
    uint32_t now = grant ? ns[found].grants | caps : ns[found].grants & ~caps;
    free(ns);
    if (xpctl_namespace_set_grants(x, argv[0], now) < 0)
        return die();
    printf("%s may: ", argv[0]);
    print_grants(now, false);
    printf("\n");
    return 0;
}

static int
cmd_caps(void)
{
    for (int i = 0; i < XPCTL_NUM_CAPS; i++)
        printf("%-20s %s\n", xpctl_cap_name(i), xpctl_cap_what(i));
    return 0;
}

static void
on_signal(int sig)
{
    (void) sig;
    stop = 1;
}

static int
cmd_agent(void)
{
    if (xpctl_agent(x, true) < 0)
        return die();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("Answering permission prompts. Ctrl+C stops.\n");
    fflush(stdout);
    while (!stop) {
        xpctl_prompt p;
        int r = xpctl_agent_next(x, &p, 500);
        if (r < 0)
            return die();
        if (r == 0)
            continue;
        const char *who = p.client.app_id[0] ? p.client.app_id
                        : p.client.command[0] ? p.client.command : "An application";
        printf("\n%s (namespace %s", who, p.client.ns);
        if (p.client.pid > 0)
            printf(", pid %d", p.client.pid);
        printf(") wants to %s.\n", xpctl_cap_what(p.cap));
        printf("[a]llow once, allow [w]hile it runs, [d]eny, deny [f]orever? ");
        fflush(stdout);
        char line[32];
        enum xpctl_answer a = XPCTL_DENY_ONCE;
        if (fgets(line, sizeof(line), stdin)) {
            switch (line[0]) {
            case 'a': case 'y': a = XPCTL_ALLOW_ONCE; break;
            case 'w': a = XPCTL_ALLOW_ALWAYS; break;
            case 'f': a = XPCTL_DENY_ALWAYS; break;
            default: a = XPCTL_DENY_ONCE; break;
            }
        } else
            stop = 1;           /* stdin closed: deny and stop */
        if (xpctl_agent_answer(x, p.id, a) < 0)
            return die();
        static const char *const said[] = { "denied", "allowed once",
                                            "allowed while it runs", "denied for good" };
        printf("%s\n", said[a]);
        fflush(stdout);
    }
    xpctl_agent(x, false);
    return 0;
}

int
main(int argc, char **argv)
{
    const char *display = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "+d:h")) != -1) {
        switch (opt) {
        case 'd': display = optarg; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    argc -= optind;
    argv += optind;
    if (argc < 1)
        return usage(stderr), 2;
    const char *cmd = argv[0];
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help"))
        return usage(stdout), 0;
    if (!strcmp(cmd, "caps"))
        return cmd_caps();

    x = xpctl_open(display);
    if (!x)
        return 1;
    if (!xpctl_connection(x)) {
        int r = die();
        xpctl_close(x);
        return r;
    }
    int r;
    if (!strcmp(cmd, "status"))
        r = cmd_status();
    else if (!strcmp(cmd, "displays") && argc == 2)
        r = cmd_displays_saved(argv[1]);
    else if (!strcmp(cmd, "displays"))
        r = cmd_displays();
    else if (!strcmp(cmd, "modes"))
        r = cmd_modes(argc - 1, argv + 1);
    else if (!strcmp(cmd, "display"))
        r = cmd_display(argc - 1, argv + 1);
    else if (!strcmp(cmd, "apps"))
        r = cmd_apps();
    else if (!strcmp(cmd, "namespaces"))
        r = cmd_namespaces();
    else if (!strcmp(cmd, "namespace"))
        r = cmd_namespace(argc - 1, argv + 1);
    else if (!strcmp(cmd, "grant"))
        r = cmd_grant(argc - 1, argv + 1, true);
    else if (!strcmp(cmd, "revoke"))
        r = cmd_grant(argc - 1, argv + 1, false);
    else if (!strcmp(cmd, "agent"))
        r = cmd_agent();
    else {
        fprintf(stderr, "xprismctl: no command %s\n\n", cmd);
        usage(stderr);
        r = 2;
    }
    xpctl_close(x);
    return r;
}
