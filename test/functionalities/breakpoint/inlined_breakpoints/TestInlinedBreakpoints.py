"""
Test that inlined breakpoints (breakpoint set on a file/line included from
another source file) works correctly.
"""

import os, time
import unittest2
import lldb
from lldbtest import *
import lldbutil

class InlinedBreakpointsTestCase(TestBase):
    """Bug fixed: rdar://problem/8464339"""

    mydir = TestBase.compute_mydir(__file__)

    @unittest2.skipUnless(sys.platform.startswith("darwin"), "requires Darwin")
    @dsym_test
    def test_with_dsym_and_run_command(self):
        """Test 'b basic_types.cpp:176' does break (where int.cpp includes basic_type.cpp)."""
        self.buildDsym()
        self.inlined_breakpoints()

    @expectedFailureFreeBSD("llvm.org/pr21142")
    @dwarf_test
    def test_with_dwarf_and_run_command(self):
        """Test 'b basic_types.cpp:176' does break (where int.cpp includes basic_type.cpp)."""
        self.buildDwarf()
        self.inlined_breakpoints()

    def setUp(self):
        # Call super's setUp().
        TestBase.setUp(self)
        # Find the line number to break inside basic_type.cpp.
        self.line = line_number('basic_type.cpp', '// Set break point at this line.')

    def inlined_breakpoints(self):
        """Test 'b basic_types.cpp:176' does break (where int.cpp includes basic_type.cpp)."""
        exe = os.path.join(os.getcwd(), "a.out")
        self.runCmd("file " + exe, CURRENT_EXECUTABLE_SET)

        # With the inline-breakpoint-strategy, our file+line breakpoint should not resolve to a location.
        self.runCmd('settings set target.inline-breakpoint-strategy headers')

        # Set a breakpoint and fail because it is in an inlined source implemenation file
        lldbutil.run_break_set_by_file_and_line (self, "basic_type.cpp", self.line, num_expected_locations=0)

        # Now enable breakpoints in implementation files and see the breakpoint set succeed
        self.runCmd('settings set target.inline-breakpoint-strategy always')
        # And add hooks to restore the settings during tearDown().
        self.addTearDownHook(
            lambda: self.runCmd("settings set target.inline-breakpoint-strategy always"))

        lldbutil.run_break_set_by_file_and_line (self, "basic_type.cpp", self.line, num_expected_locations=1, loc_exact=True)

        self.runCmd("run", RUN_SUCCEEDED)

        # The stop reason of the thread should be breakpoint.
        # And it should break at basic_type.cpp:176.
        self.expect("thread list", STOPPED_DUE_TO_BREAKPOINT,
            substrs = ['stopped',
                       'stop reason = breakpoint',
                       'basic_type.cpp:%d' % self.line])


if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lambda: lldb.SBDebugger.Terminate())
    unittest2.main()
