#!/usr/bin/env bash
# Linux stand-in for tools/wrapper.ps1. See tools/linux-rabi.sh.
#   tools/wrapper.sh status|on|off
action="${1:-status}"
exec "$(cd "$(dirname "$0")" && pwd)/linux-rabi.sh" "$action"
