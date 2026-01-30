#define BUILD_IMPLEMENTATION
#include "build.h"

int main(int argc, char** argv) {
    Include* inc = newDirectInclude(".");
    Include* sym_inc = newSymbolicInclude(".", "buildcpp");

    char* inc_str = allocIncludePath(inc, "syms");
    char* sym_inc_ptr = allocIncludePath(sym_inc, "syms");

    free(inc_str);
    free(sym_inc_ptr);

    freeInclude(inc);
    freeInclude(sym_inc);
    return 0;
}
