#define BUILD_IMPLEMENTATION
#include "build.h"

const char* BUILD_DIR = ".build";

int main(int argc, char** argv) {
    Build* build = newBuildWithCompiler(BUILD_DIR, "clang", argc, argv);
    rebuildYourself(build);

    buildAddInclude(build, newDirectInclude("test_src"));

    buildAddObject(build, newObject("test_src/incs/foo.c"));
    buildAddObject(build, newObject("test_src/incs/bar.c"));
    buildAddObject(build, newObject("test_src/main.c"));

    buildBuild(build);
    freeBuild(build);

    return 0;
}
