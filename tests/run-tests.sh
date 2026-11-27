#!/usr/bin/env bash
# xprismctl against a real XPrism Xvfb with namespaces.
#   run-tests.sh XPRISMCTL XWIN
# XPRISM_XVFB: the Xvfb to use (default ../xserver/build/hw/vfb/Xvfb).
# XPRISM_PANEL: the control panel binary, to check that it starts.
set -u
ctl=$1 xwin=$2
here=$(cd "$(dirname "$0")/.." && pwd)
xvfb=${XPRISM_XVFB:-$here/../xserver/build/hw/vfb/Xvfb}
if [ ! -x "$xvfb" ]; then
    echo "no XPrism Xvfb at $xvfb (set XPRISM_XVFB): skipping"
    exit 77
fi

tmp=$(mktemp -d)
pids=()
cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done; rm -rf "$tmp"; }
trap cleanup EXIT

root=11111111111111111111111111111111
box=22222222222222222222222222222222
cat > "$tmp/ns.conf" <<CONF
auth MIT-MAGIC-COOKIE-1 $root
namespace sandbox root
  auth MIT-MAGIC-COOKIE-1 $box
CONF
for d in $(seq 70 99); do [ -e /tmp/.X11-unix/X$d ] || [ -e /tmp/.X$d-lock ] || break; done
export DISPLAY=:$d
xauth -f "$tmp/root" add "$DISPLAY" MIT-MAGIC-COOKIE-1 $root
xauth -f "$tmp/box" add "$DISPLAY" MIT-MAGIC-COOKIE-1 $box
"$xvfb" "$DISPLAY" -auth "$tmp/root" -namespace "$tmp/ns.conf" -capmode enforce \
    -screen 0 1280x800x24 -nolisten tcp >"$tmp/xvfb.log" 2>&1 &
pids+=($!)
export XAUTHORITY=$tmp/root
for _ in $(seq 50); do "$ctl" status >/dev/null 2>&1 && break; sleep 0.1; done

fails=0
check() {   # check NAME EXPECTED-REGEX COMMAND...
    local name=$1 want=$2; shift 2
    local out; out=$("$@" 2>&1)
    if grep -Eq -- "$want" <<<"$out"; then echo "ok   $name"
    else echo "FAIL $name: wanted /$want/, got:"; sed 's/^/     /' <<<"$out"; fails=$((fails+1)); fi
}
fails_with() {  # fails_with NAME STATUS REGEX COMMAND...
    local name=$1 status=$2 want=$3; shift 3
    local out; out=$("$@" 2>&1); local got=$?
    if [ $got -eq "$status" ] && grep -Eq -- "$want" <<<"$out"; then echo "ok   $name"
    else echo "FAIL $name: wanted exit $status and /$want/, got $got:"; sed 's/^/     /' <<<"$out"; fails=$((fails+1)); fi
}

check status            "namespaces +on \(X-NAMESPACE 1\.2\)" "$ctl" status
check status-xprism     "^server +XPrism [0-9]{2}\.(0[1-9]|1[0-2])\.[0-9]{2}"                    "$ctl" status
check displays          "^screen +1280x800 +-"                  "$ctl" displays
check namespaces        "^sandbox +0 "                        "$ctl" namespaces
check root-everything   "^root .*everything"                  "$ctl" namespaces
check create            "created web"                         "$ctl" namespace create web clipboard.read --transient
check created           "^web +0 +transient +clipboard.read$" "$ctl" namespaces
check grant             "web may: clipboard.read, clipboard.write, display.configure" \
                        "$ctl" grant web clipboard.write display.configure
check revoke            "web may: clipboard.write, display.configure" "$ctl" revoke web clipboard.read
check revoke-all        "web may: -"                          "$ctl" revoke web all
fails_with bad-cap 2    "no permission nope"                  "$ctl" grant web nope
fails_with builtin 1    "root is built in"                    "$ctl" grant root capture.screen
fails_with dup 1        "cannot create"                       "$ctl" namespace create web
check delete            "deleted web"                         "$ctl" namespace delete web
fails_with gone 1       "no namespace web"                    "$ctl" grant web clipboard.read
fails_with no-cmd 2     "no command frob"                     "$ctl" frob

# an app in the sandbox
coproc OWNER { XAUTHORITY=$tmp/box exec "$xwin"; }
pids+=($OWNER_PID)
read -r wid <&"${OWNER[0]}"
check apps              "^0x[0-9a-f]+ +sandbox +xwin +[0-9]+ +-$" "$ctl" apps
fails_with busy 1       "apps still use it"                   "$ctl" namespace delete sandbox
check sandbox-count     "^sandbox +1 "                        "$ctl" namespaces

# a sandboxed client inspects another one's window: a prompt for
# window.inspect, then one for window.enumerate (xwininfo's QueryTree)
spy() { XAUTHORITY=$tmp/box timeout 10 xwininfo -id "$wid" >/dev/null 2>&1 && echo allowed || echo refused; }
check unanswered-is-refused "refused"   spy
(sleep 1.5; echo w; echo w; sleep 2; echo d) | timeout --preserve-status 6 "$ctl" agent >"$tmp/agent.out" 2>&1 &
agent=$!
sleep 0.5
check allow-while-runs  "allowed"       spy
check deny-once         "refused"       spy
wait $agent; agent_status=$?
check agent-said        "^xwininfo \(namespace sandbox, pid [0-9]+\) wants to read details of other applications' windows" cat "$tmp/agent.out"
check agent-answers     "read details.*allowed while it runs .*list the windows.*allowed while it runs .*denied" tr '\n' ' ' <"$tmp/agent.out"
check agent-exit        "^0$"           echo $agent_status
fails_with agent-sandbox 1 "X-NAMESPACE is not available" env XAUTHORITY="$tmp/box" "$ctl" namespaces

check close-apps        "deleted sandbox"                     "$ctl" namespace delete sandbox --close-apps

if [ -n "${XPRISM_PANEL:-}" ] && [ -x "$XPRISM_PANEL" ]; then
    GDK_BACKEND=x11 GSK_RENDERER=cairo "$XPRISM_PANEL" >"$tmp/panel.log" 2>&1 &
    panel=$!; pids+=($panel)
    sleep 3
    if kill -0 $panel 2>/dev/null && xwininfo -name "XPrism Control Panel" >/dev/null 2>&1; then
        echo "ok   panel"
    else
        echo "FAIL panel:"; cat "$tmp/panel.log"; fails=$((fails+1))
    fi
fi

echo "$fails failed"
[ $fails -eq 0 ]
