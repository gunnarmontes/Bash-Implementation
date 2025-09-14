#!/bin/bash
# Advanced Echo Expansion Test Commands
# Standalone script with dynamic values generated at runtime

# Generate dynamic values for anti-hardcoding
RAND_A=$((RANDOM % 40 + 10))  # Random number between 10-49
RAND_B=$((RANDOM % 49 + 51))  # Random number between 51-99

# Header
echo "=== ADVANCED EXPANSION TESTING ==="
echo

# Dynamic variables (anti-hardcoding)
echo "1. Dynamic Variables"
export DYN_VAR=test_${RAND_A}
echo "Dynamic var: $DYN_VAR"
CALC_VAR=${RAND_B}
echo "Calc var: $CALC_VAR"
echo "Combined: $DYN_VAR with $CALC_VAR"
echo

# Process-specific tests (anti-hardcoding)
echo "2. Process-Specific Tests"
echo "Current PID: $$"
false
echo "Exit status: $?"
echo "Current second: $(date +%S)"
echo

# Dynamic arithmetic (anti-hardcoding)
echo "3. Dynamic Arithmetic"
echo "Sum: $(($RAND_A+$RAND_B))"
echo "Product: $(($RAND_A*2))"
echo "With variable: $(($CALC_VAR/2))"
echo

# Dynamic command substitution (anti-hardcoding)
echo "4. Dynamic Command Substitution"
echo "Dynamic echo: $(echo value_${RAND_A})"
echo "Time-based: $(date +%H:%M)"
echo "Nested calc: $(echo $(($RAND_A+10)))"
echo

# Complex parameter expansion
echo "5. Complex Parameter Expansion"
echo "Default value: ${UNDEFINED:-fallback}"
echo "Dynamic default: ${MISSING:-backup_${RAND_B}}"
echo "Variable length: ${#USER}"
echo

# Anti-hardcoding validation
echo "6. Anti-Hardcoding Validation"
eval "ANTI_VAR_${RAND_A}=secured"
eval "echo \"Anti-hardcode test: \$ANTI_VAR_${RAND_A}\""
echo "Runtime check: $(($RAND_A*$RAND_B))"
true; echo "Status check: $?"
echo

echo "=== ADVANCED TESTING COMPLETE ==="
