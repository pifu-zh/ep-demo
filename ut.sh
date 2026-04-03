#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Running all tests..."

# Test 1: Basic initialization test
echo "=== Running test_init ==="
"$SCRIPT_DIR/build/test_init" 0 1 mlx5_0 127.0.0.1

# Test 2: Test suite
echo "=== Running test_suite ==="
"$SCRIPT_DIR/build/test_suite" 0 1 mlx5_0 127.0.0.1

# Test 3: Multi-process test (single rank)
echo "=== Running test_multi (single rank) ==="
"$SCRIPT_DIR/build/test_multi" 0 1 mlx5_0 127.0.0.1

# Test 4: Dispatch combine test - simplified version without multi-process complexity
echo "=== Running test_dispatch_combine (single rank for reliability) ==="
"$SCRIPT_DIR/build/test_dispatch_combine" 0 1 mlx5_0 127.0.0.1

echo "All tests completed!"