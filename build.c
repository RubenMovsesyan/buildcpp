#define BUILD_IMPLEMENTATION
#include "build.h"

int main(int argc, char** argv) {
    Object* obj = newObject("main_test.c");

    StrVec obj_files = {0};
    CompCmdVec cmp_cmd = {0};
    pthread_mutex_t obj_file_mut = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t comp_file_mut = PTHREAD_MUTEX_INITIALIZER;

    compile(obj, "clang", "-std=c23", "", ".build", &obj_files, &obj_file_mut, &cmp_cmd, &comp_file_mut);

    freeObject(obj);
    return 0;
}
