#ifndef BUILD_H
#define BUILD_H

#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/_pthread/_pthread_mutex_t.h>
#include <sys/syslimits.h>
#else
#include <limits.h>
#include <pthread.h>
#endif

#define Vector(type)    \
    struct {            \
            type* ptr;  \
            size_t cap; \
            size_t len; \
    }

#define VectorPushBack(type, vector, elem)                                               \
    do {                                                                                 \
        if ((vector)->len == (vector)->cap) {                                            \
            if ((vector)->cap == 0) {                                                    \
                (vector)->cap = 1;                                                       \
            } else {                                                                     \
                (vector)->cap *= 2;                                                      \
            }                                                                            \
            (vector)->ptr = (type*)realloc((vector)->ptr, (vector)->cap * sizeof(type)); \
        }                                                                                \
        (vector)->ptr[(vector)->len] = elem;                                             \
        (vector)->len++;                                                                 \
    } while (0)

#define VectorFree(vector) \
    free((vector)->ptr);

#define RingedQueue(type) \
    struct {              \
            type* start;  \
            type* end;    \
            size_t cap;   \
            size_t len;   \
    }

typedef Vector(char*) StrVec;

typedef struct {
        char* dep_name;
        char** deps;
} DependencyList;

// --== (Command Interface) ==--

typedef struct {
        StrVec command_chain;
        char* exec_dir;
} Command;

// Init and deinit
Command* newCommand(int count, ...);
Command* newCommandFromDir(const char* exec_dir, int count, ...);
void freeCommand(Command* cmd);

void cmdPushBack(Command* cmd, char* chain);
void cmdPrint(Command* cmd);

char* allocString(Command* cmd);

int cmdExec(Command* cmd);
char* allocCmdExecAndCapture(Command* cmd);

typedef Vector(Command) CmdVec;

// --== (Compile Command Interface) ==--

typedef struct {
        Command* src_cmd;
        char* dir;
        char* file;
} CompileCommand;

CompileCommand newCompileCommand(Command* src_cmd, char* filepath);
void freeCompileCommand(CompileCommand* comp_cmd);
void compCmdAddToFile(CompileCommand* comp_cmd, FILE* file);

typedef Vector(CompileCommand) CompCmdVec;

// --== (Include Interface) ==--

typedef struct {
        char* include_path;
} DirectInclude;

typedef struct {
        char* include_path;
        char* symbolic_dir;
} SymbolicInclude;

typedef enum {
    Include_Direct,
    Include_Symbolic
} IncludeType;

typedef struct {
        IncludeType type;
        union {
                DirectInclude direct;
                SymbolicInclude symbolic;
        } include;
} Include;

Include* newDirectInclude(const char* path);
Include* newSymbolicInclude(const char* path, const char* symbolic_dir);
void freeInclude(Include* include);

char* allocIncludePath(Include* inc, const char* symlinks_path);

typedef Vector(Include) IncludeVec;

// --== (Include Interface) ==--

typedef struct {
        char* src_path;
        char* filename;
        char* filename_no_ext;
} Object;

Object* newObject(const char* path);
void freeObject(Object* obj);
char* allocObjectPath(Object* obj);
DependencyList objectListDependencies(Object* obj, const char* compiler, const char* flags, const char* includes);
char** allocBuildCommand(Object* obj, char* build_dir);
void compile(
    Object* obj,
    const char* compiler,
    const char* flags,
    const char* includes,
    const char* build_dir,
    StrVec* obj_files,
    pthread_mutex_t* obj_files_mutex,
    CompCmdVec* compile_commands,
    pthread_mutex_t* comp_cmd_mutex
);

typedef Vector(Object) ObjVec;
typedef RingedQueue(Object) ObjRingQueue;

// --== (Link Interface) ==--
typedef struct {
        char* dep_name;
} DirectLink;

typedef struct {
        char* dep_name;
        char* dir_name;
        /* optional */ char* direct_path;
} PathLink;

// MacOs only
typedef struct {
        char* dep_name;
} Framework;

typedef enum {
    Link_Direct,
    Link_Path,
    Link_Framework
} LinkType;

typedef struct {
        LinkType type;
        union {
                DirectLink direct_link;
                PathLink path_link;
                Framework framework;
        } link;
} Link;

Link* newDirectLink(const char* dep_name);
Link* newPathLink(const char* dir_name);
Link* newPathLinkWithDep(const char* dep_name, const char* dir_name);
Link* newFramework(const char* dep_name);
char* allocLinkable(Link* link);

typedef Vector(Link) LinkVec;

// --== (Build Interface) ==--
typedef enum {
    Os_Invalid,
    Os_MacOS,
    Os_Linux,
    Os_Windows
} Os;

typedef enum {
    Mode_Release,
    Mode_Debug
} Mode;

typedef struct {
        Os os;
        Mode mode;
} Builtin;

typedef struct {
        CmdVec pre_step_commands;
        IncludeVec includes;
        ObjVec objects;
        LinkVec links;
        StrVec comp_flags;
        StrVec link_flags;
} BuildStep;

typedef Vector(BuildStep) BuildStepVec;

typedef struct {
        pthread_mutex_t mutex;

        atomic_bool all_jobs_queued;
        atomic_bool all_jobs_complete;

        ObjRingQueue objects;
} BuildJob;

typedef Vector(BuildJob) BuildJobVec;

typedef struct {
        Builtin builtin;

        // Internal use
        char* build_dir_;
        char* sym_link_dir_;
        char* compiler_;

        BuildStepVec build_steps_;
        BuildJobVec thread_queues_;
        size_t jobs_;

        bool skip_compile_commands;
} Build;

void initCommon(Build* build);

Build newBuild(const char* dir_name, int argc, char** argv);
Build newBuildWithCompiler(const char* dir_name, const char* compiler, int argc, char** argv);
void destroyBuild(Build* build);

void rebuildYourself(Build* build);
void buildExportCompileCommands(Build* build, CompCmdVec* compile_commands);
void buildSkipCompileCommands(Build* build);
void buildAddPrebuildCommand(Build* build, Command* command);
void buildAddObject(Build* build, Object* object);
void buildAddInclude(Build* build, Include* include);
void buildAddLink(Build* build, Link* link);
void buildAddCompilationFlag(Build* build, const char* flag);
void buildAddLinkingFlag(Build* build, const char* flag);
void buildStep(Build* build);
void buildBuild(Build* build);

// #ifdef BUILD_IMPLEMENTATION

// --== Command Implementation ==--

char* expandPath(const char* path) {
    char* expanded = (char*)malloc(PATH_MAX);
    if (!expanded) {
        return nullptr;
    }

    if (path[0] == '/') {
        strncpy(expanded, path, PATH_MAX - 1);
        expanded[PATH_MAX - 1] = '\0';
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == nullptr) {
            free(expanded);
            return nullptr;
        }
        snprintf(expanded, PATH_MAX, "%s/%s", cwd, path);
    }

    char* resolved = realpath(expanded, nullptr);
    if (resolved) {
        free(expanded);
        return resolved;
    }

    return expanded;
}

Command* newCommand(int count, ...) {
    Command* cmd = (Command*)calloc(1, sizeof(Command));

    va_list args;
    va_start(args, count);

    for (int i = 0; i < count; i++) {
        VectorPushBack(char*, &cmd->command_chain, va_arg(args, char*));
    }

    va_end(args);

    cmd->exec_dir = nullptr;

    return cmd;
}

Command* newCommandFromDir(const char* exec_dir, int count, ...) {
    Command* cmd = (Command*)calloc(1, sizeof(Command));

    va_list args;
    va_start(args, count);

    for (int i = 0; i < count; i++) {
        VectorPushBack(char*, &cmd->command_chain, va_arg(args, char*));
    }

    va_end(args);

    cmd->exec_dir = expandPath(exec_dir);

    return cmd;
}

void freeCommand(Command* cmd) {
    VectorFree(&cmd->command_chain);
    if (cmd->exec_dir) {
        free(cmd->exec_dir);
    }

    free(cmd);
}

void cmdPushBack(Command* cmd, char* chain) {
    VectorPushBack(char*, &cmd->command_chain, chain);
}

void cmdPrint(Command* cmd) {
    for (int i = 0; i < cmd->command_chain.len; i++) {
        printf("%s ", cmd->command_chain.ptr[i]);
    }
    printf("\n");
}

char* allocString(Command* cmd) {
    Vector(char) str = {0};

    for (int i = 0; i < cmd->command_chain.len; i++) {
        char* cmd_str = cmd->command_chain.ptr[i];
        while (*cmd_str) {
            VectorPushBack(char, &str, *cmd_str);
            cmd_str++;
        }
        VectorPushBack(char, &str, ' ');
    }

    VectorPushBack(char, &str, '\0');
    return str.ptr;
}

int cmdExec(Command* cmd) {
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));

    if (cmd->exec_dir) {
        chdir(cmd->exec_dir);
    }

    char* command = allocString(cmd);
    int result = system(command);

    free(command);

    if (cmd->exec_dir) {
        chdir(cwd);
    }

    return result;
}

char* allocCmdExecAndCapture(Command* cmd) {
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));

    if (cmd->exec_dir) {
        chdir(cmd->exec_dir);
    }

    char* command = allocString(cmd);
    int len = strlen(command);
    command = (char*)realloc(command, len + 5);
    strcpy(command + len, " 2>&1");

    FILE* pipe = popen(command, "r");
    if (!pipe) {
        if (cmd->exec_dir) {
            chdir(cwd);
        }

        return nullptr;
    }

    char* result = nullptr;
    size_t result_len = 0;
    char buffer[128];

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        size_t buf_len = strlen(buffer);
        result = (char*)realloc(result, result_len + buf_len + 1);
        memcpy(result + result_len, buffer, buf_len + 1);
        result_len += buf_len;
    }

    pclose(pipe);
    free(command);

    if (cmd->exec_dir) {
        chdir(cwd);
    }

    return result;
}

// --== Compile Command Implementation ==--

CompileCommand newCompileCommand(Command* src_cmd, char* filepath) {
    CompileCommand comp_cmd = {0};

    comp_cmd.src_cmd = src_cmd;
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));
    comp_cmd.dir = strdup(cwd);
    comp_cmd.file = expandPath(filepath);

    return comp_cmd;
}

void freeCompileCommand(CompileCommand* comp_cmd) {
    free(comp_cmd->dir);
}

void compCmdAddToFile(CompileCommand* comp_cmd, FILE* file) {
    char* command_str = allocString(comp_cmd->src_cmd);

    fprintf(file, "\t{\n");

    fprintf(file, "\t\t\"directory\": \"%s\",\n", comp_cmd->dir);
    fprintf(file, "\t\t\"command\": \"%s\",\n", command_str);
    fprintf(file, "\t\t\"file\": \"%s\"\n", comp_cmd->file);

    fprintf(file, "\t}\n");

    free(command_str);
}

// --== Include Implementation ==--
Include* newDirectInclude(const char* path) {
    Include* inc = (Include*)calloc(1, sizeof(Include));

    inc->type = Include_Direct;
    inc->include.direct = (DirectInclude){
        .include_path = expandPath(path)
    };

    return inc;
}

Include* newSymbolicInclude(const char* path, const char* symbolic_dir) {
    Include* inc = (Include*)calloc(1, sizeof(Include));

    inc->type = Include_Symbolic;
    inc->include.symbolic = (SymbolicInclude){
        .include_path = expandPath(path),
        .symbolic_dir = strdup(symbolic_dir),
    };

    return inc;
}

void freeInclude(Include* include) {
    switch (include->type) {
        case Include_Direct:
            free(include->include.direct.include_path);
            break;
        case Include_Symbolic:
            free(include->include.symbolic.include_path);
            free(include->include.symbolic.symbolic_dir);
            break;
    }

    free(include);
}

char* allocIncludePath(Include* inc, const char* symlinks_path) {
    switch (inc->type) {
        case Include_Direct:
            return strdup(inc->include.direct.include_path);
        case Include_Symbolic:
            char* symlink_path_expanded = expandPath(symlinks_path);
            char sym_path[PATH_MAX];
            sprintf(sym_path, "%s/%s", symlink_path_expanded, inc->include.symbolic.symbolic_dir);
            free(symlink_path_expanded);
            Command* create_symbolic_dir = newCommand(4, "ln", "-sfn", inc->include.symbolic.include_path, sym_path);

            cmdPrint(create_symbolic_dir);
            cmdExec(create_symbolic_dir);

            freeCommand(create_symbolic_dir);

            return strdup(sym_path);
    }
}

// --== Object Implementation ==--

char* extractFilename(char* absolute_path) {
    size_t index = strlen(absolute_path) - 1;
    while (absolute_path[index] != '/') {
        index--;
    }

    return strdup(absolute_path - index);
}

char* removeFilenameExtension(char* filename) {
    size_t index = strlen(filename) - 1;
    while (filename[index] != '.') {
        index--;
    }

    char* extended = strdup(filename);
    extended[index] = '\0';

    return extended;
}

Object* newObject(const char* path) {
    Object* obj = (Object*)calloc(1, sizeof(Object));

    obj->src_path = expandPath(path);
    obj->filename = extractFilename(obj->src_path);
    obj->filename_no_ext = removeFilenameExtension(obj->filename);

    return obj;
}

void freeObject(Object* obj) {
    free(obj->src_path);
    free(obj->filename);
    free(obj->filename_no_ext);
    free(obj);
}

char* allocObjectPath(Object* obj) {
    return strdup(obj->src_path);
}

DependencyList objectListDependencies(Object* obj, const char* compiler, const char* flags, const char* includes) {
    Command* dep_cmd = newCommand(5, compiler, flags, includes, "-MM", obj->src_path);
    char* output = allocCmdExecAndCapture(dep_cmd);

    freeCommand(dep_cmd);
    free(output);
}

char** allocBuildCommand(Object* obj, char* build_dir);
void compile(
    Object* obj,
    const char* compiler,
    const char* flags,
    const char* includes,
    const char* build_dir,
    StrVec* obj_files,
    pthread_mutex_t* obj_files_mutex,
    CompCmdVec* compile_commands,
    pthread_mutex_t* comp_cmd_mutex
);

// #endif // BUILD_IMPLEMENTATION
#endif // BUILD_H
