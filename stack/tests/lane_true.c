/* A static musl `true` for lane tests (M2-01): what libc_basic execs on Linux, so
 * the traced child is an arc_os-bound program (stack/tests/CMakeLists.txt). */
int main(void) {
    return 0;
}
