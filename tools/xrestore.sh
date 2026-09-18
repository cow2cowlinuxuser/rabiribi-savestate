#!/usr/bin/env bash
# Linux stand-in for tools/xrestore.ps1. See tools/linux-rabi.sh.
exec "$(cd "$(dirname "$0")" && pwd)/linux-rabi.sh" xrestore "$@"
