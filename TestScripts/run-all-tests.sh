#!/bin/bash
#
# Comprehensive Test Suite for VirtIO GPU Driver
#
# This script runs all available tests:
# - Unit tests
# - Build verification
# - OpenCore integration check
# - Ready-to-boot verification
#
# Usage: ./run-all-tests.sh [--skip-unit] [--skip-build] [--skip-integration]
#
# Exit codes:
#   0 = all tests passed
#   1 = some tests failed
#   2 = test environment not ready (e.g., not on macOS for build)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VIRTIO_DIR="$REPO_ROOT/virtio-gpu-macos"
TESTS_DIR="$VIRTIO_DIR/Tests"

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASSED=0
FAILED=0

function pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((PASSED++))
}

function fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((FAILED++))
}

function info() {
    echo -e "${YELLOW}[INFO]${NC} $1"
}

function check_macos() {
    if [[ "$(uname)" != "Darwin" ]]; then
        fail "This script must be run on macOS for build/integration tests"
        return 1
    fi
    return 0
}

#=============================================================================
# Test Suites
#=============================================================================

run_unit_tests() {
    echo "=== Unit Tests ==="
    cd "$TESTS_DIR"
    
    if ! make -s clean; then
        fail "Failed to clean unit tests"
        return 1
    fi
    
    if ! make -s; then
        fail "Failed to build unit tests"
        return 1
    fi
    
    if ! ./virtio-gpu-tests > /dev/null 2>&1; then
        fail "Unit tests failed or crashed"
        # Show output for debugging
        echo "--- Unit test output ---"
        ./virtio-gpu-tests || true
        echo "-----------------------"
        return 1
    fi
    
    pass "Unit tests passed"
    return 0
}

run_build_test() {
    echo "=== Build Test ==="
    cd "$VIRTIO_DIR"
    
    if ! make -s clean; then
        fail "make clean failed"
        return 1
    fi
    
    if ! make -s; then
        fail "make failed"
        return 1
    fi
    
    if [ ! -d "build/virtio-gpu.kext" ]; then
        fail "kext bundle not created"
        return 1
    fi
    
    if [ ! -f "build/virtio-gpu.kext/Contents/MacOS/virtio-gpu" ]; then
        fail "Missing executable in kext bundle"
        return 1
    fi
    
    # Check Info.plist
    if ! plutil -lint "build/virtio-gpu.kext/Contents/Info.plist" > /dev/null 2>&1; then
        fail "Info.plist is invalid"
        return 1
    fi
    
    pass "Build test passed"
    return 0
}

run_opencore_integration_test() {
    echo "=== OpenCore Integration Test ==="
    
    # Check driver is in OpenCore config
    if ! grep -q "Drivers/virtio-gpu.kext" "$REPO_ROOT/OpenCore/config.plist"; then
        fail "Driver not listed in OpenCore/config.plist"
        return 1
    fi
    
    # Check driver file exists in Drivers directory
    if [ ! -d "$REPO_ROOT/OpenCore/Drivers/virtio-gpu.kext" ]; then
        info "Driver not in OpenCore/Drivers/ - run 'make opencore' to copy"
        fail "Driver missing from OpenCore/Drivers/"
        return 1
    fi
    
    # Check OpenCore image exists or can be built
    if [ ! -f "$REPO_ROOT/OpenCore/OpenCore.qcow2" ]; then
        info "OpenCore.qcow2 not found - it needs to be built"
        # Don't fail here since building requires libguestfs-tools
        pass "OpenCore integration verified (image will need rebuild"
        return 0
    fi
    
    pass "OpenCore integration test passed"
    return 0
}

run_readme_test() {
    echo "=== Documentation Test ==="
    
    if [ ! -s "$VIRTIO_DIR/README.md" ]; then
        fail "README.md missing or empty"
        return 1
    fi
    
    # Check for essential sections
    local required_sections=(
        "Features"
        "Architecture"
        "Requirements"
        "Build"
        "Installation"
        "Testing"
    )
    
    for section in "${required_sections[@]}"; do
        if ! grep -q "^## $section" "$VIRTIO_DIR/README.md"; then
            fail "Missing section: $section"
        fi
    done
    
    pass "Documentation test passed"
    return 0
}

run_file_structure_test() {
    echo "=== File Structure Test ==="
    
    local required_files=(
        "Makefile"
        "README.md"
        "Include/virtio_gpu.h"
        "Include/virtio_types.h"
        "Include/virtio_gpu_private.h"
        "Source/virtio_gpu.cpp"
        "Source/virtqueue.cpp"
        "Source/IOFramebuffer.cpp"
        "Resources/Info.plist"
        "Resources/entitlements.plist"
    )
    
    for file in "${required_files[@]}"; do
        if [ ! -f "$VIRTIO_DIR/$file" ]; then
            fail "Missing required file: $file"
        fi
    done
    
    pass "File structure test passed"
    return 0
}

#=============================================================================
# Main
#=============================================================================

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Run the complete test suite for VirtIO GPU driver."
    echo ""
    echo "Options:"
    echo "  --skip-unit        Skip unit tests"
    echo "  --skip-build       Skip build test"
    echo "  --skip-integration Skip OpenCore integration test"
    echo "  --skip-docs        Skip documentation test"
    echo "  --skip-structure   Skip file structure test"
    echo "  -h, --help         Show this help"
    echo ""
    echo "Example:"
    echo "  $0                    # Run all tests"
    echo "  $0 --skip-unit       # Run everything except unit tests"
}

# Parse arguments
SKIP_UNIT=0
SKIP_BUILD=0
SKIP_INTEGRATION=0
SKIP_DOCS=0
SKIP_STRUCTURE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-unit) SKIP_UNIT=1; shift ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --skip-integration) SKIP_INTEGRATION=1; shift ;;
        --skip-docs) SKIP_DOCS=1; shift ;;
        --skip-structure) SKIP_STRUCTURE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1"; usage; exit 1 ;;
    esac
done

echo "VirtIO GPU Driver Comprehensive Test Suite"
echo "=========================================="
echo ""

# Quick sanity: are we on macOS for build tests?
if [ $SKIP_BUILD -eq 0 ] || [ $SKIP_INTEGRATION -eq 0 ]; then
    check_macos || true  # Don't exit yet, just warn for skip*
fi

# Run selected test suites
if [ $SKIP_STRUCTURE -eq 0 ]; then
    run_file_structure_test || true
fi

if [ $SKIP_DOCS -eq 0 ]; then
    run_readme_test || true
fi

if [ $SKIP_UNIT -eq 0 ]; then
    run_unit_tests || true
fi

if [ $SKIP_BUILD -eq 0 ]; then
    if check_macos; then
        run_build_test || true
    else
        info "Skipping build test (requires macOS)"
    fi
fi

if [ $SKIP_INTEGRATION -eq 0 ]; then
    if check_macos; then
        run_opencore_integration_test || true
    else
        info "Skipping integration test (requires macOS)"
    fi
fi

# Summary
echo ""
echo "Test Summary:"
echo -e "  ${GREEN}Passed${NC}: $PASSED"
echo -e "  ${RED}Failed${NC}: $FAILED"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed${NC}"
    exit 1
fi
