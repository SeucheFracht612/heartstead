#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $0 apply|clear INTERFACE [DELAY_MS] [LOSS_PERCENT]" >&2
}

if [[ $# -lt 2 || $# -gt 4 ]]; then
  usage
  exit 2
fi

action=$1
interface=$2
delay_ms=${3:-100}
loss_percent=${4:-2}

if [[ ! $interface =~ ^[[:alnum:]_.:-]+$ ]] ||
   [[ ! $delay_ms =~ ^[0-9]+$ ]] ||
   [[ ! $loss_percent =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "interface, delay, or loss value is invalid" >&2
  exit 2
fi

ip link show dev "$interface" >/dev/null

case "$action" in
  apply)
    sudo tc qdisc replace dev "$interface" root netem \
      delay "${delay_ms}ms" loss "${loss_percent}%"
    tc qdisc show dev "$interface"
    ;;
  clear)
    sudo tc qdisc delete dev "$interface" root
    ;;
  *)
    usage
    exit 2
    ;;
esac
