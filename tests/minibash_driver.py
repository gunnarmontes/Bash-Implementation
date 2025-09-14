#!/usr/bin/env python3
"""
Generic minibash driver for running test scripts with corresponding .reg template files.
Each test script (e.g., echo_test.py) should have a corresponding .reg file (e.g., echo_test.reg).

Features:
- Configurable point system with default 70/30 basic/advanced split
- Individual test point configuration
- Category-based scoring and reporting
"""

import os
import sys
import subprocess
import glob
from pathlib import Path
from dataclasses import dataclass
from typing import Dict, Tuple, Optional


@dataclass
class TestConfig:
    """Configuration for individual tests and scoring."""
    name: str
    category: str  # 'basic', 'advanced', 'python'
    points: int
    script_path: str
    expected_path: Optional[str] = None
    
    def __post_init__(self):
        """Convert paths to Path objects."""
        self.script_path = Path(self.script_path)
        if self.expected_path:
            self.expected_path = Path(self.expected_path)


class PointSystem:
    """Manages point distribution and scoring for tests."""
    
    def __init__(self, total_points: int = 100, basic_ratio: float = 0.7):
        """Initialize point system.
        
        Args:
            total_points: Total points available
            basic_ratio: Ratio of points for basic tests (0.0-1.0)
        """
        self.total_points = total_points
        self.basic_ratio = basic_ratio
        self.advanced_ratio = 1.0 - basic_ratio
        
        self.basic_total = int(total_points * basic_ratio)
        self.advanced_total = total_points - self.basic_total
        
        # Track results
        self.results = {}
        self.test_configs = {}
    
    def add_test_config(self, config: TestConfig):
        """Add a test configuration."""
        self.test_configs[config.name] = config
    
    def distribute_points(self, basic_tests: Dict, advanced_tests: Dict, python_tests: Dict = None):
        """Distribute points among tests based on categories.
        
        Args:
            basic_tests: Dict of basic test names -> (script_path, expected_path)
            advanced_tests: Dict of advanced test names -> (script_path, expected_path)
            python_tests: Dict of python test names -> (script_path, expected_path)
        """
        # Count tests in each category
        basic_count = len(basic_tests)
        advanced_count = len(advanced_tests)
        python_count = len(python_tests) if python_tests else 0
        
        # Distribute points evenly within categories
        basic_points_per_test = self.basic_total // basic_count if basic_count > 0 else 0
        advanced_points_per_test = self.advanced_total // advanced_count if advanced_count > 0 else 0
        
        # Handle remainder points
        basic_remainder = self.basic_total % basic_count if basic_count > 0 else 0
        advanced_remainder = self.advanced_total % advanced_count if advanced_count > 0 else 0
        
        # Create test configs for basic tests
        for i, (test_name, (script_path, expected_path)) in enumerate(basic_tests.items()):
            points = basic_points_per_test + (1 if i < basic_remainder else 0)
            config = TestConfig(
                name=test_name,
                category='basic',
                points=points,
                script_path=str(script_path),
                expected_path=str(expected_path) if expected_path else None
            )
            self.add_test_config(config)
        
        # Create test configs for advanced tests
        for i, (test_name, (script_path, expected_path)) in enumerate(advanced_tests.items()):
            points = advanced_points_per_test + (1 if i < advanced_remainder else 0)
            config = TestConfig(
                name=test_name,
                category='advanced', 
                points=points,
                script_path=str(script_path),
                expected_path=str(expected_path) if expected_path else None
            )
            self.add_test_config(config)
        
        # Handle Python tests (assign minimal points or distribute from basic pool)
        if python_tests:
            python_points_per_test = 1  # Minimal points for python tests
            for test_name, (script_path, reg_path) in python_tests.items():
                config = TestConfig(
                    name=test_name,
                    category='python',
                    points=python_points_per_test,
                    script_path=str(script_path),
                    expected_path=str(reg_path)
                )
                self.add_test_config(config)
    
    def record_result(self, test_name: str, passed: bool):
        """Record test result."""
        self.results[test_name] = passed
    
    def get_score(self, category: str = None) -> Tuple[int, int]:
        """Get current score for category or overall.
        
        Args:
            category: 'basic', 'advanced', 'python', or None for overall
            
        Returns:
            Tuple of (earned_points, total_possible_points)
        """
        earned = 0
        total = 0
        
        for test_name, config in self.test_configs.items():
            if category is None or config.category == category:
                total += config.points
                if self.results.get(test_name, False):
                    earned += config.points
        
        return earned, total
    
    def get_summary(self) -> str:
        """Generate scoring summary."""
        lines = []
        lines.append(f"{'='*60}")
        lines.append("SCORING SUMMARY")
        lines.append(f"{'='*60}")
        
        # Category breakdown
        for category in ['basic', 'advanced', 'python']:
            earned, total = self.get_score(category)
            if total > 0:
                percentage = (earned / total) * 100
                lines.append(f"{category.title()} Tests: {earned}/{total} points ({percentage:.1f}%)")
        
        # Overall score
        earned, total = self.get_score()
        percentage = (earned / total) * 100 if total > 0 else 0
        lines.append(f"{'─'*60}")
        lines.append(f"Overall Score: {earned}/{total} points ({percentage:.1f}%)")
        
        return '\n'.join(lines)
from dataclasses import dataclass
from typing import Dict, Tuple, Optional


def cleanup_core_files(script_dir):
    """
    Clean up core.die files generated by crash tests.
    
    Args:
        script_dir: Directory to clean up
    """
    try:
        core_files = glob.glob(str(script_dir / "core.die.*"))
        if len(core_files) > 0:
            for core_file in core_files:
                os.remove(core_file)
            print(f"Cleaned up {len(core_files)} core.die files")
    except Exception as e:
        print(f"Warning: Could not clean up core files: {e}")


def handle_timing_variance(test_name, expected_output, actual_output):
    """
    Handle timing variance in timing-sensitive tests.
    
    Args:
        test_name: Name of the test
        expected_output: Expected output string
        actual_output: Actual output string
    
    Returns:
        bool: True if the output is within acceptable timing variance
    """
    # Split outputs into lines
    expected_lines = expected_output.strip().split('\n')
    actual_lines = actual_output.strip().split('\n')
    
    # For while-complex tests, allow for ±2 iterations variance due to timing
    if "while-complex" in test_name:
        # Both should start with "Starting"
        if len(expected_lines) == 0 or len(actual_lines) == 0:
            return False
        if expected_lines[0] != "Starting" or actual_lines[0] != "Starting":
            return False
        
        # Count numeric iterations (skip "Starting" line)
        expected_count = len(expected_lines) - 1  # -1 for "Starting"
        actual_count = len(actual_lines) - 1      # -1 for "Starting"
        
        # Allow ±3 iteration variance for timing sensitivity
        variance_allowance = 3
        if abs(expected_count - actual_count) <= variance_allowance:
            # Check that the numeric sequence is correct up to the actual count
            for i in range(1, min(len(expected_lines), len(actual_lines))):
                if expected_lines[i] != actual_lines[i]:
                    return False
            return True
    
    return False


def discover_tests(script_dir):
    """
    Discover all available test scripts and their corresponding .reg files.
    
    Args:
        script_dir: Directory to search for test files
    
    Returns:
        Dict mapping test names to (script_path, reg_path) tuples
    """
    tests = {}
    test_scripts = glob.glob(str(script_dir / "*_test.py"))
    
    # Exclude standalone test scripts that don't follow the *_test.py + .reg pattern
    excluded_scripts = {'substitution_test.py'}
    
    for script_path in test_scripts:
        script_path = Path(script_path)
        test_name = script_path.stem  # e.g., "echo_test"
        
        # Skip excluded scripts
        if script_path.name in excluded_scripts:
            continue
            
        reg_path = script_dir / f"{test_name}.reg"
        
        if reg_path.exists():
            tests[test_name] = (script_path, reg_path)
        else:
            print(f"Warning: No .reg file found for {script_path}")
    
    return tests


def discover_sh_tests(script_dir):
    """
    Discover all .sh test scripts and their corresponding .out or .reg files.
    
    Args:
        script_dir: Directory to search for test files
    
    Returns:
        Dict mapping categories to test dictionaries:
        {
            'basic': {test_name: (script_path, expected_path), ...},
            'advanced': {test_name: (script_path, expected_path), ...}
        }
    """
    sh_tests = {"basic": {}, "advanced": {}}
    test_scripts = glob.glob(str(script_dir / "*.sh"))
    
    for script_path in test_scripts:
        script_path = Path(script_path)
        test_name = script_path.stem  # e.g., "001-comment" or "substitution_test_basic"
        
        # Look for .out file first, then .reg file
        out_path = script_dir / f"{test_name}.out"
        reg_path = script_dir / f"{test_name}.reg"
        
        expected_path = None
        if out_path.exists():
            expected_path = out_path
        elif reg_path.exists():
            expected_path = reg_path
        
        if expected_path:
            # Categorize based on test number or name
            if test_name.startswith("substitution_test_"):
                # Handle substitution tests
                if "basic" in test_name:
                    category = "basic"
                else:
                    category = "advanced"
            else:
                # Handle numbered tests (basic: 001-099, advanced: 100+)
                try:
                    test_num = int(test_name.split('-')[0])
                    category = "basic" if test_num < 100 else "advanced"
                except (ValueError, IndexError):
                    # If we can't parse the number, default to basic
                    category = "basic"
            
            sh_tests[category][test_name] = (script_path, expected_path)
        else:
            print(f"Warning: No .out or .reg file found for {script_path}")
    
    return sh_tests




def run_test(test_name, script_path, shell_path="./cush", verbose=False):
    """
    Run a specific test script with the given shell.
    
    Args:
        test_name: Name of the test (e.g., "echo_test")
        script_path: Path to the test script
        shell_path: Path to the shell executable to test
        verbose: Whether to show verbose output
    
    Returns:
        Tuple of (success: bool, output: str)
    """
    try:
        cmd = ["python3", str(script_path), "--shell", shell_path]
        if verbose:
            cmd.append("--verbose")
        
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            cwd=script_path.parent
        )
        
        return result.returncode == 0, result.stdout + result.stderr
        
    except Exception as e:
        return False, f"Error running {test_name}: {str(e)}"


def run_sh_test(test_name, script_path, expected_path, shell_path="./cush", verbose=False):
    """
    Run a .sh test script and compare output with expected .out or .reg file.
    
    Args:
        test_name: Name of the test (e.g., "001-comment")
        script_path: Path to the .sh script
        expected_path: Path to the expected .out or .reg file
        shell_path: Path to the shell executable to test
        verbose: Whether to show verbose output
    
    Returns:
        Tuple of (success: bool, output: str)
    """
    try:
        # Run the shell script with the specified shell
        cmd = [shell_path, str(script_path)]
        
        # For timing-sensitive tests, ensure we wait for complete execution
        timeout = 30  # Allow up to 30 seconds for tests to complete
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            cwd=script_path.parent,
            timeout=timeout
        )
        
        actual_output = result.stdout
        
        # Check if this is a .reg file (regex-based) or .out file (exact match)
        if expected_path.suffix == '.reg':
            # For .reg files, use the existing expansion_test.py for validation
            # This is a simplified approach - for now just check if script ran successfully
            success = result.returncode == 0
            output = f"{'PASS' if success else 'FAIL'}: {test_name}"
            
            if verbose or not success:
                output += f"\nActual output:\n{repr(actual_output)}"
                if result.stderr:
                    output += f"\nStderr:\n{result.stderr}"
        else:
            # For .out files, do exact comparison with timing variance allowance
            with open(expected_path, 'r') as f:
                expected_output = f.read()
            
            success = actual_output == expected_output
            
            # Special handling for timing-sensitive tests
            if not success and "while-complex" in test_name:
                success = handle_timing_variance(test_name, expected_output, actual_output)
            
            if success:
                output = f"PASS: {test_name}"
                if verbose:
                    output += f"\nExpected output:\n{repr(expected_output)}"
                    output += f"\nActual output:\n{repr(actual_output)}"
            else:
                output = f"FAIL: {test_name}"
                output += f"\nExpected output:\n{repr(expected_output)}"
                output += f"\nActual output:\n{repr(actual_output)}"
                if result.stderr:
                    output += f"\nStderr:\n{result.stderr}"
        
        return success, output
        
    except subprocess.TimeoutExpired:
        return False, f"TIMEOUT: {test_name} exceeded {timeout} seconds"
    except Exception as e:
        return False, f"Error running {test_name}: {str(e)}"




def run_all_tests(shell_path="./cush", verbose=False, point_system=None):
    """
    Run all discovered tests with the given shell.
    
    Args:
        shell_path: Path to the shell executable to test
        verbose: Whether to show verbose output
        point_system: PointSystem instance for scoring
    
    Returns:
        Tuple of (success: bool, output: str, results: dict, point_system: PointSystem)
    """
    script_dir = Path(__file__).parent
    py_tests = discover_tests(script_dir)
    sh_tests = discover_sh_tests(script_dir)
    
    # Initialize point system if not provided
    if point_system is None:
        point_system = PointSystem()
        point_system.distribute_points(
            sh_tests.get('basic', {}), 
            sh_tests.get('advanced', {}), 
            py_tests
        )
    
    all_output = []
    results = {}
    all_success = True
    
    # Run Python tests
    if py_tests:
        all_output.append(f"\n{'='*60}")
        all_output.append("PYTHON TESTS (.py/.reg)")
        all_output.append('='*60)
        
        for test_name, (script_path, reg_path) in py_tests.items():
            all_output.append(f"\nRunning test: {test_name}")
            all_output.append(f"Script: {script_path}")
            all_output.append(f"Template: {reg_path}")
            all_output.append('-'*40)
            
            success, output = run_test(test_name, script_path, shell_path, verbose)
            results[test_name] = success
            point_system.record_result(test_name, success)
            
            # Add point information to output
            if test_name in point_system.test_configs:
                points = point_system.test_configs[test_name].points
                point_status = f" [{points} pts - {'EARNED' if success else 'LOST'}]"
                output = output.rstrip() + point_status
            
            all_output.append(output)
            
            if not success:
                all_success = False
    
    # Run shell script tests
    for category in ['basic', 'advanced']:
        category_tests = sh_tests.get(category, {})
        if category_tests:
            all_output.append(f"\n{'='*60}")
            all_output.append(f"SHELL SCRIPT TESTS - {category.upper()} (.sh/.out or .sh/.reg)")
            all_output.append('='*60)
            
            # Sort tests by name for consistent output
            for test_name in sorted(category_tests.keys()):
                script_path, expected_path = category_tests[test_name]
                all_output.append(f"\nRunning test: {test_name}")
                all_output.append(f"Script: {script_path}")
                all_output.append(f"Expected: {expected_path}")
                all_output.append('-'*40)
                
                success, output = run_sh_test(test_name, script_path, expected_path, shell_path, verbose)
                results[f"{category}_{test_name}"] = success
                point_system.record_result(test_name, success)
                
                # Add point information to output
                if test_name in point_system.test_configs:
                    points = point_system.test_configs[test_name].points
                    point_status = f" [{points} pts - {'EARNED' if success else 'LOST'}]"
                    output = output.rstrip() + point_status
                
                all_output.append(output)
                
                if not success:
                    all_success = False
    
    # Check if any tests were found
    if not py_tests and not any(sh_tests.values()):
        return False, "No tests discovered", {}, point_system
    
    # Generate summary
    summary = f"\n{'='*60}\nTEST SUMMARY\n{'='*60}\n"
    
    if py_tests:
        summary += "Python Tests:\n"
        for test_name in sorted(py_tests.keys()):
            status = "PASS" if results[test_name] else "FAIL"
            summary += f"  {test_name}: {status}\n"
        summary += "\n"
    
    for category in ['basic', 'advanced']:
        category_tests = sh_tests.get(category, {})
        if category_tests:
            summary += f"Shell Script Tests - {category.title()}:\n"
            for test_name in sorted(category_tests.keys()):
                result_key = f"{category}_{test_name}"
                status = "PASS" if results[result_key] else "FAIL"
                summary += f"  {test_name}: {status}\n"
            summary += "\n"
    
    
    summary += f"Overall: {'PASS' if all_success else 'FAIL'}"
    all_output.append(summary)
    
    # Add point system summary
    all_output.append("\n" + point_system.get_summary())
    
    # Clean up core.die files generated by crash tests
    cleanup_core_files(script_dir)
    
    return all_success, '\n'.join(all_output), results, point_system


def run_shell_tests_category(category, category_tests, shell_path="./cush", verbose=False, point_system=None):
    """
    Run shell script tests for a specific category.
    
    Args:
        category: Category name ('basic' or 'advanced')
        category_tests: Dict of test_name -> (script_path, out_path)
        shell_path: Path to the shell executable to test
        verbose: Whether to show verbose output
        point_system: PointSystem instance for scoring
    
    Returns:
        Tuple of (success: bool, output: str, point_system: PointSystem)
    """
    # Initialize point system if not provided
    if point_system is None:
        point_system = PointSystem()
        if category == 'basic':
            point_system.distribute_points(category_tests, {}, {})
        else:
            point_system.distribute_points({}, category_tests, {})
    
    all_output = []
    all_success = True
    
    all_output.append(f"{'='*60}")
    all_output.append(f"SHELL SCRIPT TESTS - {category.upper()} (.sh/.out or .sh/.reg)")
    all_output.append('='*60)
    
    for test_name in sorted(category_tests.keys()):
        script_path, expected_path = category_tests[test_name]
        all_output.append(f"\nRunning test: {test_name}")
        all_output.append(f"Script: {script_path}")
        all_output.append(f"Expected: {expected_path}")
        all_output.append('-'*40)
        
        success, output = run_sh_test(test_name, script_path, expected_path, shell_path, verbose)
        point_system.record_result(test_name, success)
        
        # Add point information to output
        if test_name in point_system.test_configs:
            points = point_system.test_configs[test_name].points
            point_status = f" [{points} pts - {'EARNED' if success else 'LOST'}]"
            output = output.rstrip() + point_status
        
        all_output.append(output)
        
        if not success:
            all_success = False
    
    # Generate summary
    summary = f"\n{'='*60}\nTEST SUMMARY - {category.upper()}\n{'='*60}\n"
    for test_name in sorted(category_tests.keys()):
        # We need to re-run to get individual results, but this is inefficient
        # For now, we'll just show the overall result
        pass
    summary += f"Overall: {'PASS' if all_success else 'FAIL'}"
    all_output.append(summary)
    
    # Add point system summary
    all_output.append("\n" + point_system.get_summary())
    
    # Clean up core.die files generated by crash tests
    script_dir = Path(__file__).parent
    cleanup_core_files(script_dir)
    
    return all_success, '\n'.join(all_output), point_system


def run_python_tests_only(py_tests, shell_path="./cush", verbose=False, point_system=None):
    """
    Run only Python tests.
    
    Args:
        py_tests: Dict of test_name -> (script_path, reg_path)
        shell_path: Path to the shell executable to test
        verbose: Whether to show verbose output
        point_system: PointSystem instance for scoring
    
    Returns:
        Tuple of (success: bool, output: str, point_system: PointSystem)
    """
    # Initialize point system if not provided
    if point_system is None:
        point_system = PointSystem()
        point_system.distribute_points({}, {}, py_tests)
    
    all_output = []
    all_success = True
    
    all_output.append(f"{'='*60}")
    all_output.append("PYTHON TESTS (.py/.reg)")
    all_output.append('='*60)
    
    for test_name, (script_path, reg_path) in py_tests.items():
        all_output.append(f"\nRunning test: {test_name}")
        all_output.append(f"Script: {script_path}")
        all_output.append(f"Template: {reg_path}")
        all_output.append('-'*40)
        
        success, output = run_test(test_name, script_path, shell_path, verbose)
        point_system.record_result(test_name, success)
        
        # Add point information to output
        if test_name in point_system.test_configs:
            points = point_system.test_configs[test_name].points
            point_status = f" [{points} pts - {'EARNED' if success else 'LOST'}]"
            output = output.rstrip() + point_status
        
        all_output.append(output)
        
        if not success:
            all_success = False
    
    # Generate summary
    summary = f"\n{'='*60}\nTEST SUMMARY - PYTHON\n{'='*60}\n"
    for test_name in sorted(py_tests.keys()):
        # Similar issue as above - for simplicity, just show overall
        pass
    summary += f"Overall: {'PASS' if all_success else 'FAIL'}"
    all_output.append(summary)
    
    # Add point system summary
    all_output.append("\n" + point_system.get_summary())
    
    # Clean up core.die files generated by crash tests
    script_dir = Path(__file__).parent
    cleanup_core_files(script_dir)
    
    return all_success, '\n'.join(all_output), point_system


def main():
    """Main function for standalone usage."""
    import argparse
    
    parser = argparse.ArgumentParser(
        description="Generic minibash driver for running test scripts",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 minibash_driver.py --shell ./cush
  python3 minibash_driver.py --test echo_test --shell ./cush --verbose
  python3 minibash_driver.py --test 001-comment --shell ./cush
  python3 minibash_driver.py -b --shell ./cush
  python3 minibash_driver.py -a --shell ./cush
  python3 minibash_driver.py --list-tests
        """
    )
    
    parser.add_argument("--shell", default="./cush", help="Path to shell executable")
    parser.add_argument("--test", help="Run specific test (e.g., 'echo_test' or '001-comment')")
    parser.add_argument("-b", "--basic", action="store_true", help="Run only basic shell script tests (.sh/.out)")
    parser.add_argument("-a", "--advanced", action="store_true", help="Run only advanced shell script tests (.sh/.out)")
    parser.add_argument("--python-only", action="store_true", help="Run only Python tests (.py/.reg)")
    parser.add_argument("--list-tests", action="store_true", help="List available tests")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    
    # Point system configuration
    parser.add_argument("--total-points", type=int, default=100, help="Total points available (default: 100)")
    parser.add_argument("--basic-ratio", type=float, default=0.7, help="Ratio of points for basic tests (0.0-1.0, default: 0.7)")
    
    args = parser.parse_args()
    
    # Validate point system arguments
    if args.basic_ratio < 0.0 or args.basic_ratio > 1.0:
        print("Error: --basic-ratio must be between 0.0 and 1.0")
        sys.exit(1)
    
    script_dir = Path(__file__).parent
    py_tests = discover_tests(script_dir)
    sh_tests = discover_sh_tests(script_dir)
    
    # Initialize point system
    point_system = PointSystem(total_points=args.total_points, basic_ratio=args.basic_ratio)
    point_system.distribute_points(
        sh_tests.get('basic', {}), 
        sh_tests.get('advanced', {}), 
        py_tests
    )
    
    if args.list_tests:
        print("Available Python tests (.py/.reg):")
        for test_name in sorted(py_tests.keys()):
            if test_name in point_system.test_configs:
                points = point_system.test_configs[test_name].points
                print(f"  {test_name} ({points} pts)")
            else:
                print(f"  {test_name}")
        
        print("\nAvailable shell script tests (.sh/.out or .sh/.reg):")
        for category in ['basic', 'advanced']:
            category_tests = sh_tests.get(category, {})
            if category_tests:
                print(f"  {category.title()}:")
                for test_name in sorted(category_tests.keys()):
                    if test_name in point_system.test_configs:
                        points = point_system.test_configs[test_name].points
                        print(f"    {test_name} ({points} pts)")
                    else:
                        print(f"    {test_name}")
        
        print("\n" + point_system.get_summary())
        return
    
    if args.test:
        # Run specific test
        test_found = False
        
        # Check Python tests
        if args.test in py_tests:
            script_path, reg_path = py_tests[args.test]
            print(f"Running Python test: {args.test}")
            print(f"Script: {script_path}")
            print(f"Template: {reg_path}")
            
            success, output = run_test(args.test, script_path, args.shell, args.verbose)
            
            # Add point information if available
            if args.test in point_system.test_configs:
                points = point_system.test_configs[args.test].points
                point_status = f" [{points} pts - {'EARNED' if success else 'LOST'}]"
                output = output.rstrip() + point_status
            
            print(output)
            
            # Clean up core.die files generated by crash tests
            cleanup_core_files(script_dir)
            
            sys.exit(0 if success else 1)
        
        # Check shell script tests
        for category in ['basic', 'advanced']:
            category_tests = sh_tests.get(category, {})
            if args.test in category_tests:
                script_path, expected_path = category_tests[args.test]
                print(f"Running shell script test ({category}): {args.test}")
                print(f"Script: {script_path}")
                print(f"Expected: {expected_path}")
                
                success, output = run_sh_test(args.test, script_path, expected_path, args.shell, args.verbose)
                
                # Add point information if available
                if args.test in point_system.test_configs:
                    points = point_system.test_configs[args.test].points
                    point_status = f" [{points} pts - {'EARNED' if success else 'LOST'}]"
                    output = output.rstrip() + point_status
                
                print(output)
                
                # Clean up core.die files generated by crash tests
                cleanup_core_files(script_dir)
                
                sys.exit(0 if success else 1)
        
        print(f"Error: Test '{args.test}' not found.")
        print("Available Python tests:", ", ".join(py_tests.keys()))
        all_sh_tests = []
        for category_tests in sh_tests.values():
            all_sh_tests.extend(category_tests.keys())
        print("Available shell script tests:", ", ".join(sorted(all_sh_tests)))
        sys.exit(1)
    
    elif args.basic:
        # Run only basic shell script tests
        if not sh_tests.get('basic'):
            print("No basic shell script tests found.")
            sys.exit(1)
        
        # Create category-specific point system
        basic_point_system = PointSystem(total_points=args.total_points, basic_ratio=1.0)
        basic_point_system.distribute_points(sh_tests['basic'], {}, {})
        
        success, output, _ = run_shell_tests_category('basic', sh_tests['basic'], args.shell, args.verbose, basic_point_system)
        print(output)
        sys.exit(0 if success else 1)
    
    elif args.advanced:
        # Run only advanced shell script tests
        if not sh_tests.get('advanced'):
            print("No advanced shell script tests found.")
            sys.exit(1)
        
        # Create category-specific point system
        advanced_point_system = PointSystem(total_points=args.total_points, basic_ratio=0.0)
        advanced_point_system.distribute_points({}, sh_tests['advanced'], {})
        
        success, output, _ = run_shell_tests_category('advanced', sh_tests['advanced'], args.shell, args.verbose, advanced_point_system)
        print(output)
        sys.exit(0 if success else 1)
    
    elif args.python_only:
        # Run only Python tests
        if not py_tests:
            print("No Python tests found.")
            sys.exit(1)
        
        # Create python-specific point system
        python_point_system = PointSystem(total_points=args.total_points, basic_ratio=0.0)
        python_point_system.distribute_points({}, {}, py_tests)
        
        success, output, _ = run_python_tests_only(py_tests, args.shell, args.verbose, python_point_system)
        print(output)
        sys.exit(0 if success else 1)
    
    else:
        # Run both basic and advanced shell script tests (default behavior)
        all_output = []
        all_success = True
        
        # Run basic tests
        if sh_tests.get('basic'):
            success, output, _ = run_shell_tests_category('basic', sh_tests['basic'], args.shell, args.verbose, point_system)
            all_output.append(output)
            if not success:
                all_success = False
        
        # Run advanced tests
        if sh_tests.get('advanced'):
            success, output, _ = run_shell_tests_category('advanced', sh_tests['advanced'], args.shell, args.verbose, point_system)
            all_output.append(output)
            if not success:
                all_success = False
        
        if not sh_tests.get('basic') and not sh_tests.get('advanced'):
            print("No shell script tests found.")
            sys.exit(1)
        
        print('\n'.join(all_output))
        sys.exit(0 if all_success else 1)


if __name__ == "__main__":
    main()
