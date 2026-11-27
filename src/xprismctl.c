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

static int
cmd_displays(void)
{
    xpctl_output *o;
    int n;
    if (xpctl_outputs(x, &o, &n) < 0)
        return die();
    printf("%-10s %-20s %-4s %-4s %-10s %s\n", "OUTPUT", "MODE", "HDR", "EDR", "WIDE-GAMUT",
           "SHOWS");
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
        printf("%-10s %-20s %-4s %-4s %-10s %s\n", o[i].name, mode, onoff(o[i].hdr),
               onoff(o[i].edr), onoff(o[i].wide_gamut), shows);
    }
    free(o);
    return 0;
}

static int
cmd_display(int argc, char **argv)
{
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
    else if (!strcmp(cmd, "displays"))
        r = cmd_displays();
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
