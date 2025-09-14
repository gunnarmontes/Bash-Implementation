#!/bin/bash
# Basic Echo Expansion Test Commands
# These tests cover the essential functionality without complex nesting.

# Header
echo "=== BASIC ECHO EXPANSION TESTING ==="
echo

# Basic echo functionality
echo "1. Basic Echo"
echo "Hello World"
echo

# Environment variables
echo "2. Environment Variables"
echo "User: $USER"
echo "Home: $HOME"
export TEST_VAR=basic_test
echo "Test variable: $TEST_VAR"
echo

# Special variables
echo "3. Special Variables"
true
echo "Exit status: $?"
echo "Process ID: $$"
echo

# Parameter expansion
echo "4. Parameter Expansion"
echo "Braced: ${USER}"
echo "Default: ${UNDEFINED:-default}"
echo "Length: ${#USER}"
echo

# Command substitution
echo "5. Command Substitution"
echo "Year: $(date +%Y)"
echo "Echo test: $(echo hello)"
echo

# Arithmetic
echo "6. Arithmetic"
echo "Simple: $((2+3))"
echo "With var: $((${#USER}*2))"
echo

# Quotes and escapes
echo "7. Quotes and Escapes"
echo "Double: \"quoted\""
echo "No expansion: '\$USER'"
echo "Escaped: \$USER"
echo

echo "=== BASIC TESTING COMPLETE ==="
