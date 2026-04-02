#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RANK="${1:-0}"
NUM_RANKS="${2:-1}"
DEVICE="${MC_IBGDA_DEVICE:-mlx5_0}"
MY_IP="${3:-127.0.0.1}"

echo "Running test_multi with rank=$RANK, num_ranks=$NUM_RANKS, device=$DEVICE, ip=$MY_IP"
"$SCRIPT_DIR/build/test_multi" "$RANK" "$NUM_RANKS" "$DEVICE" "$MY_IP"
