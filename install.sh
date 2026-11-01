#!/bin/bash
set -e
cd "$(dirname "$0")/src_patched"
chmod +x install.sh
exec ./install.sh
