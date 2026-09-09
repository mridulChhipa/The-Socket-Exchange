#!/bin/sh
# measure.sh -- snapshot server resource usage for the bonus table (Section 6.9)
#
# Usage:  ./measure.sh <server-pid> <server-port>
# Prints one snapshot to stdout. Run this while idle_clients.py is holding
# the connections open. Save the output for the 10k / 40k / 70k screenshots.

set -eu

if [ $# -lt 2 ]; then
  echo "usage: $0 <server-pid> <server-port>" >&2
  exit 2
fi

SPID="$1"
SPORT="$2"

banner() { printf '\n==== %s ====\n' "$1"; }

banner "date / uname"
date
uname -a

banner "established connections to server port $SPORT"
netstat -an -p tcp | awk -v p=".$SPORT" '$0 ~ p && $6=="ESTABLISHED"' | wc -l | \
  awk '{printf "ESTABLISHED_to_%s = %s\n", '"$SPORT"', $1}'

banner "server open FDs (procstat -f | wc -l)"
procstat -f "$SPID" | tail -n +2 | wc -l | awk '{print "server_open_fds =", $1}'

banner "server memory / cpu (ps)"
ps -o pid,rss,vsz,%cpu,%mem,nlwp,command -p "$SPID"

banner "system-wide open files (sysctl kern.openfiles / kern.maxfiles)"
sysctl kern.openfiles kern.maxfiles kern.maxfilesperproc

banner "mbuf / socket-buffer usage (netstat -m)"
netstat -m | sed -n '1,20p'

banner "socket-buffer sysctls"
sysctl kern.ipc.maxsockbuf net.inet.tcp.sendspace net.inet.tcp.recvspace kern.ipc.somaxconn

banner "server kernel stack (where is it blocked?)"
procstat -k "$SPID" | head -n 40

banner "sockstat summary for server (count)"
sockstat -4 | awk -v pid="$SPID" '$3 == pid' | wc -l | \
  awk '{print "server_sockets_seen_by_sockstat =", $1}'
