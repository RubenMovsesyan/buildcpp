#define BUILD_IMPLEMENTATION
#include "build.h"

int main(int argc, char** argv) {
    Command* cmd = newCommand(2, "cat", "build.c");

    char* cmd_str = allocString(cmd);
    printf("Cmd Str: %s", cmd_str);

    char* captured = allocExecAndCapture(cmd);

    printf("Captured\n%s", captured);

    freeCommand(cmd);
    free(cmd_str);
    free(captured);
    return 0;
}
