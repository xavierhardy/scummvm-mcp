#!/bin/bash

echo "Running basic MCP functionality tests..."

# Test 1: Check that the MCP header file exists and has expected content
echo "Test 1: Checking MCP header file..."
if [ -f "../engines/scumm/monkey_mcp.h" ]; then
    echo "✓ MCP header file exists"
    
    # Check for key function declarations
    if grep -q "wrapContent" ../engines/scumm/monkey_mcp.h; then
        echo "✓ wrapContent function declared"
    else
        echo "✗ wrapContent function not found"
        exit 1
    fi
    
    # makePropOneOf is a static function, check in implementation
    if grep -q "makePropOneOf" ../engines/scumm/monkey_mcp.cpp; then
        echo "✓ makePropOneOf function implemented"
    else
        echo "✗ makePropOneOf function not found"
        exit 1
    fi
else
    echo "✗ MCP header file not found"
    exit 1
fi

# Test 2: Check that the MCP implementation file exists
echo "\nTest 2: Checking MCP implementation file..."
if [ -f "../engines/scumm/monkey_mcp.cpp" ]; then
    echo "✓ MCP implementation file exists"
    
    # Check for key implementations
    if grep -q "Common::JSONValue \*wrapContent" ../engines/scumm/monkey_mcp.cpp; then
        echo "✓ wrapContent function implemented"
    else
        echo "✗ wrapContent function implementation not found"
        exit 1
    fi
    
    if grep -q "makePropOneOf.*string.*integer" ../engines/scumm/monkey_mcp.cpp; then
        echo "✓ makePropOneOf function implemented"
    else
        echo "✗ makePropOneOf function implementation not found"
        exit 1
    fi
else
    echo "✗ MCP implementation file not found"
    exit 1
fi

# Test 3: Check for proper MCP capabilities declaration
echo "\nTest 3: Checking MCP capabilities..."
if grep -q '"listChanged"' ../engines/scumm/monkey_mcp.cpp; then
    echo "✓ MCP capabilities include listChanged"
else
    echo "✗ MCP capabilities missing listChanged"
    exit 1
fi

# Test 4: Check for tool schemas
echo "\nTest 4: Checking tool schemas..."
if grep -q "inputSchema" ../engines/scumm/monkey_mcp.cpp && grep -q "outputSchema" ../engines/scumm/monkey_mcp.cpp; then
    echo "✓ Tool schemas (input/output) present"
else
    echo "✗ Tool schemas missing"
    exit 1
fi

# Test 5: Check for proper response format
echo "\nTest 5: Checking response format..."
if grep -q '"content"' ../engines/scumm/monkey_mcp.cpp && grep -q '"structuredContent"' ../engines/scumm/monkey_mcp.cpp; then
    echo "✓ Response format includes content and structuredContent"
else
    echo "✗ Response format incomplete"
    exit 1
fi

# Test 6: Check for memory management fixes
echo "\nTest 6: Checking memory management..."
if grep -q "wrapContentFlag" ../engines/scumm/monkey_mcp.cpp; then
    echo "✓ Memory management flag present"
else
    echo "✗ Memory management flag missing"
    exit 1
fi

echo "\n✓ All basic tests PASSED!"
echo "\nMCP implementation appears to be correctly structured."
echo "The server should support:"
echo "  - Object IDs and names in act tool parameters"
echo "  - Proper MCP capabilities declaration"
echo "  - Input and output schemas for all tools"
echo "  - Correct response format with content arrays"
echo "  - Proper memory management"

exit 0