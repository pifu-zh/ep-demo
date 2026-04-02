#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

IB_DEVICE="${MC_IBGDA_DEVICE:-mlx5_0}"
RANK="${1:-0}"
NUM_RANKS="${2:-1}"
TEST_NAME="${3:-test_init}"

echo "Running $TEST_NAME with rank=$RANK, num_ranks=$NUM_RANKS, device=$IB_DEVICE"
"$SCRIPT_DIR/build/$TEST_NAME" "$RANK" "$NUM_RANKS" "$IB_DEVICE"
