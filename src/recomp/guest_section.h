/* Force-included ahead of each lifted source on macOS (CMakeLists.txt): the
 * title's own code goes into one named section, so the runtime can tell from
 * a program counter whether a thread is in guest code. The one-guest-CPU
 * rule preempts a thread only there (main.c, win32_compat.c). */
#pragma clang section text = "__TEXT,__guest,regular,pure_instructions"
