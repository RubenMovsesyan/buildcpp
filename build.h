#ifndef BUILD_H
#define BUILD_H

#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char* DEFAULT_COMPILER = "clang";

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

typedef Vector(char*) StrVec;

typedef struct {
        char* dep_name;
        char** deps;
} DependencyList;

void freeDependencyList(DependencyList* dep_list);

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
        char* src_cmd;
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
void destroyInclude(Include* include);

char* allocIncludePath(Include* inc, const char* symlinks_path);

typedef Vector(Include) IncludeVec;

// --== (Include Interface) ==--

typedef struct {
        char* src_path;
        char* filename;
        char* filename_no_ext;
} Object;

Object* newObject(const char* path);
void destroyObject(Object* obj);
char* allocObjectPath(Object* obj);
DependencyList* objectListDependencies(Object* obj, const char* compiler, const char* flags, const char* includes);
char** allocBuildCommand(Object* obj, char* build_dir);
void compile(
    Object* obj,
    char* compiler,
    char* flags,
    char* includes,
    char* build_dir,
    StrVec* obj_files,
    pthread_mutex_t* obj_files_mutex,
    CompCmdVec* compile_commands,
    pthread_mutex_t* comp_cmd_mutex
);

typedef Vector(Object) ObjVec;

// --== (Object Queue Interface) ==--

typedef struct {
        Object** objects;
        Object** start;
        Object** end;
        size_t cap;
} ObjectQueue;

ObjectQueue* newObjectQueue(size_t cap);
void objQPush(ObjectQueue* q, Object* obj);
Object* objQPop(ObjectQueue* q);
void freeObjQueue(ObjectQueue* q);

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
Link* newPathLink(const char* path);
Link* newPathLinkWithDep(const char* dep_name, const char* dir_name);
Link* newFramework(const char* dep_name);
void destroyLink(Link* link);
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

        ObjectQueue* objects;
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

        int argc;
        char** argv;
} Build;

void initCommon(Build* build);

Build* newBuild(const char* dir_name, int argc, char** argv);
Build* newBuildWithCompiler(const char* dir_name, char* compiler, int argc, char** argv);
void freeBuild(Build* build);

void rebuildYourself(Build* build);
void buildExportCompileCommands(Build* build, CompCmdVec* compile_commands);
void buildSkipCompileCommands(Build* build);
void buildAddPrebuildCommand(Build* build, Command* command);
void buildAddObject(Build* build, Object* object);
void buildAddInclude(Build* build, Include* include);
void buildAddLink(Build* build, Link* link);
void buildAddCompilationFlag(Build* build, char* flag);
void buildAddLinkingFlag(Build* build, char* flag);
void buildStep(Build* build);
void buildBuild(Build* build);

// #ifdef BUILD_IMPLEMENTATION

// --== Dependency List Implementation ==--

void freeDependencyList(DependencyList* dep_list) {
    free(dep_list->dep_name);

    for (int i = 0; dep_list->deps[i]; i++) {
        free(dep_list->deps[i]);
    }
    free(dep_list->deps);

    free(dep_list);
}

// --== Command Implementation ==--

char* allocExpandPath(const char* path) {
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

    cmd->exec_dir = allocExpandPath(exec_dir);

    return cmd;
}

void freeCommand(Command* cmd) {
    VectorFree(&cmd->command_chain);
    if (cmd->exec_dir) {
        free(cmd->exec_dir);
    }
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
    const char* stderr_2_out = " 2>&1";
    command = (char*)realloc(command, len + strlen(stderr_2_out) + 1);
    strcpy(command + len, stderr_2_out);

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

    comp_cmd.src_cmd = allocString(src_cmd);
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));
    comp_cmd.dir = strdup(cwd);
    comp_cmd.file = allocExpandPath(filepath);

    return comp_cmd;
}

void freeCompileCommand(CompileCommand* comp_cmd) {
    free(comp_cmd->dir);
    free(comp_cmd->src_cmd);
}

void compCmdAddToFile(CompileCommand* comp_cmd, FILE* file) {
    fprintf(file, "\t{\n");

    fprintf(file, "\t\t\"directory\": \"%s\",\n", comp_cmd->dir);
    fprintf(file, "\t\t\"command\": \"%s\",\n", comp_cmd->src_cmd);
    fprintf(file, "\t\t\"file\": \"%s\"\n", comp_cmd->file);

    fprintf(file, "\t}");
}

// --== Include Implementation ==--
Include* newDirectInclude(const char* path) {
    Include* inc = (Include*)calloc(1, sizeof(Include));

    inc->type = Include_Direct;
    inc->include.direct = (DirectInclude){
        .include_path = allocExpandPath(path)
    };

    return inc;
}

Include* newSymbolicInclude(const char* path, const char* symbolic_dir) {
    Include* inc = (Include*)calloc(1, sizeof(Include));

    inc->type = Include_Symbolic;
    inc->include.symbolic = (SymbolicInclude){
        .include_path = allocExpandPath(path),
        .symbolic_dir = strdup(symbolic_dir),
    };

    return inc;
}

void destroyInclude(Include* include) {
    switch (include->type) {
        case Include_Direct:
            free(include->include.direct.include_path);
            break;
        case Include_Symbolic:
            free(include->include.symbolic.include_path);
            free(include->include.symbolic.symbolic_dir);
            break;
    }
}

char* allocIncludePath(Include* inc, const char* symlinks_path) {
    switch (inc->type) {
        case Include_Direct:
            return strdup(inc->include.direct.include_path);
        case Include_Symbolic:
            char* symlink_path_expanded = allocExpandPath(symlinks_path);
            char sym_path[PATH_MAX];
            snprintf(sym_path, PATH_MAX, "%s/%s", symlink_path_expanded, inc->include.symbolic.symbolic_dir);
            Command* create_symbolic_dir = newCommand(4, "ln", "-sfn", inc->include.symbolic.include_path, sym_path);

            cmdPrint(create_symbolic_dir);
            cmdExec(create_symbolic_dir);

            // Cleanup
            freeCommand(create_symbolic_dir);
            free(symlink_path_expanded);

            return strdup(sym_path);
    }
}

// --== Object Implementation ==--

char* allocExtractFilename(char* absolute_path) {
    size_t index = strlen(absolute_path) - 1;
    while (absolute_path[index] != '/') {
        index--;
    }

    return strdup(absolute_path + index + 1);
}

char* allocRemoveFilenameExtension(char* filename) {
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

    obj->src_path = allocExpandPath(path);
    obj->filename = allocExtractFilename(obj->src_path);
    obj->filename_no_ext = allocRemoveFilenameExtension(obj->filename);

    return obj;
}

// void freeObject(Object* obj) {
//     free(obj->src_path);
//     free(obj->filename);
//     free(obj->filename_no_ext);
//     free(obj);
// }

void destroyObject(Object* obj) {
    free(obj->src_path);
    free(obj->filename);
    free(obj->filename_no_ext);
}

char* allocObjectPath(Object* obj) {
    return strdup(obj->src_path);
}

char** splitWhitespace(const char* str, size_t* out_count) {
    size_t cap = 8;
    size_t len = 0;
    char** result = (char**)malloc(cap * sizeof(char*));

    const char* p = str;
    while (*p) {
        while (*p && (*p == ' ' || *p == '\t'))
            p++;
        if (!*p)
            break;

        const char* start = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;

        size_t word_len = p - start;
        if (word_len > 0) {
            if (len == cap) {
                cap *= 2;
                result = (char**)realloc(result, cap * sizeof(char*));
            }
            result[len] = (char*)malloc(word_len + 1);
            memcpy(result[len], start, word_len);
            result[len][word_len] = '\0';
            len++;
        }
    }

    *out_count = len;
    return result;
}

DependencyList* objectListDependencies(Object* obj, const char* compiler, const char* flags, const char* includes) {
    Command* dep_cmd = newCommand(5, compiler, flags, includes, "-MM", obj->src_path);
    char* output = allocCmdExecAndCapture(dep_cmd);
    freeCommand(dep_cmd);

    if (!output) {
        return (DependencyList*)calloc(1, sizeof(DependencyList));
    }

    char* dst = output;
    for (char* src = output; *src; src++) {
        if (*src != '\\' && *src != '\n') {
            *dst++ = *src;
        }
    }
    *dst = '\0';

    char* colon = strchr(output, ':');
    if (!colon) {
        free(output);
        return (DependencyList*)calloc(1, sizeof(DependencyList));
    }

    // Extract object file name (before colon)
    *colon = '\0';
    char* dep_name = strdup(output);

    size_t dep_count;
    char** deps = splitWhitespace(colon + 1, &dep_count);

    // Null terminate the array
    deps = (char**)realloc(deps, (dep_count + 1) * sizeof(char*));
    deps[dep_count] = nullptr;

    free(output);

    DependencyList* dep_output = (DependencyList*)calloc(1, sizeof(DependencyList));
    dep_output->dep_name = dep_name;
    dep_output->deps = deps;

    return dep_output;
}

char* replaceFileExt(char* path, const char* new_ext) {
    size_t len = strlen(path);
    size_t orig_len = len;
    while (path[len] != '.') {
        len--;
    }

    size_t ext_len = strlen(new_ext);

    if (len + ext_len + 1 > orig_len) {
        path = (char*)realloc(path, len + ext_len + 2);
    }

    // Make sure to copy over the null terminator
    memcpy(path + len + 1, new_ext, ext_len);
    path[len + 1 + ext_len] = '\0';
    return path;
}

char* removeFilename(char* path) {
    char* filename_removed = strdup(path);
    size_t index = strlen(filename_removed);
    while (filename_removed[index] != '/') {
        index--;
    }

    filename_removed[index] = '\0';
    return filename_removed;
}

char** allocBuildCommand(Object* obj, char* build_dir) {
    // Find the relative path to the source file
    char* relative_path = nullptr;
    char cwd[PATH_MAX];
    getcwd(cwd, PATH_MAX);
    size_t offset = 0;
    while (cwd[offset] == obj->src_path[offset]) {
        offset++;
    }
    relative_path = obj->src_path + offset + 1;

    char* expanded_build_dir = allocExpandPath(build_dir);

    char* new_path = strcat(expanded_build_dir, "/");
    new_path = strcat(new_path, relative_path);
    char* filename_removed = removeFilename(new_path);

    Command* make_path = newCommand(3, "mkdir", "-p", filename_removed);
    cmdExec(make_path);
    freeCommand(make_path);
    free(filename_removed);

    new_path = replaceFileExt(new_path, "o");

    char** output = (char**)calloc(4, sizeof(char*));

    // Duplicate for simpler freeing later
    output[0] = strdup("-c");
    output[1] = strdup(relative_path);
    output[2] = strdup("-o");
    output[3] = new_path;

    return output;
}

void compile(
    Object* obj,
    char* compiler,
    char* flags,
    char* includes,
    char* build_dir,
    StrVec* obj_files,
    pthread_mutex_t* obj_files_mutex,
    CompCmdVec* compile_commands,
    pthread_mutex_t* comp_cmd_mutex
) {
    DependencyList* deps_list = objectListDependencies(
        obj,
        compiler,
        flags,
        includes
    );

    Command* cmd = newCommand(3, compiler, flags, includes);
    char** build_commands = allocBuildCommand(obj, build_dir);

    for (int i = 0; i < 4; i++) {
        cmdPushBack(cmd, build_commands[i]);
    }

    CompileCommand comp_cmd = newCompileCommand(cmd, obj->src_path);

    pthread_mutex_lock(comp_cmd_mutex);
    VectorPushBack(CompileCommand, compile_commands, comp_cmd);
    pthread_mutex_unlock(comp_cmd_mutex);

    bool needs_rebuild = true;
    struct stat attr;
    if (stat(build_commands[3], &attr) == 0) {
        time_t object_time = attr.st_mtime;
        needs_rebuild = false;

        struct stat dep_attr;
        for (int i = 0; deps_list->deps[i]; i++) {
            char* dep_to_check = deps_list->deps[i];
            if (stat(dep_to_check, &dep_attr) == 0) {
                // Add 1 second incase the file was just written to
                time_t dep_time = dep_attr.st_mtime + 1;
                if (dep_time > object_time) {
                    needs_rebuild = true;
                    break;
                }
            } else {
                needs_rebuild = true;
                break;
            }
        }
    }

    pthread_mutex_lock(obj_files_mutex);
    VectorPushBack(char*, obj_files, build_commands[3]);
    pthread_mutex_unlock(obj_files_mutex);

    if (!needs_rebuild) {
        goto object_compile_cleanup;
    }

    cmdPrint(cmd);
    int ret_code = cmdExec(cmd);

    if (ret_code != 0) {
        printf("Object file build failed\n");
        exit(1);
    }

object_compile_cleanup:
    freeCommand(cmd);
    freeDependencyList(deps_list);
    for (size_t i = 0; i < 2; i++) {
        free(build_commands[i]);
    }
    free(build_commands);
}

// --== ObjectQueue Implementation ==--

ObjectQueue* newObjectQueue(size_t cap) {
    ObjectQueue* q = (ObjectQueue*)calloc(1, sizeof(ObjectQueue));

    q->cap = cap;
    q->objects = (Object**)calloc(cap, sizeof(Object*));
    q->start = q->objects;
    q->end = q->objects;

    return q;
}
void freeObjQueue(ObjectQueue* q) {
    free(q->objects);
    free(q);
}

void objQPush(ObjectQueue* q, Object* obj) {
    Object** next_end = q->end + 1;
    if (next_end >= q->objects + q->cap) {
        next_end = q->objects;
    }

    if (next_end == q->start) {
    obj_q_push_rerun:
        q->objects = (Object**)realloc(q->objects, q->cap * 2);
        q->cap *= 2;
        // Put the pointers back
        q->start = q->objects + (q->start - q->objects);
        q->end = q->objects + (q->end - q->objects);
        next_end = q->end + 1;
        if (next_end >= q->objects + q->cap) {
            goto obj_q_push_rerun;
        }
    }

    *q->end = obj;
    q->end = next_end;
}

Object* objQPop(ObjectQueue* q) {
    if (q->start == q->end) {
        return nullptr;
    }

    Object* obj = *q->start;
    q->start++;
    if (q->start >= q->objects + q->cap) {
        q->start = q->objects;
    }
    return obj;
}

// --== Link Implementation ==--

Link* newDirectLink(const char* dep_name) {
    Link* link = (Link*)calloc(1, sizeof(Link));

    link->type = Link_Direct;
    link->link.direct_link.dep_name = strdup(dep_name);

    return link;
}

Link* newPathLink(const char* path) {
    Link* link = (Link*)calloc(1, sizeof(Link));

    link->type = Link_Path;
    link->link.path_link.dir_name = nullptr;
    link->link.path_link.direct_path = strdup(path);
    link->link.path_link.dep_name = nullptr;

    return link;
}

Link* newPathLinkWithDep(const char* dep_name, const char* dir_name) {
    Link* link = (Link*)calloc(1, sizeof(Link));

    link->type = Link_Path;
    link->link.path_link.dir_name = allocExpandPath(dir_name);
    link->link.path_link.direct_path = nullptr,
    link->link.path_link.dep_name = strdup(dep_name);

    return link;
}

Link* newFramework(const char* dep_name) {
    Link* link = (Link*)calloc(1, sizeof(Link));

    link->type = Link_Framework;
    link->link.framework.dep_name = strdup(dep_name);

    return link;
}

void destroyLink(Link* link) {
    switch (link->type) {
        case Link_Direct:
            free(link->link.direct_link.dep_name);
            break;
        case Link_Path:
            free(link->link.path_link.dir_name);
            free(link->link.path_link.direct_path);
            free(link->link.path_link.dep_name);
            break;
        case Link_Framework:
            free(link->link.framework.dep_name);
            break;
    }
}

char* allocLinkable(Link* link) {
    switch (link->type) {
        case Link_Direct:
            return strcat(strdup("-l"), link->link.direct_link.dep_name);
        case Link_Path:
            if (link->link.path_link.direct_path) {
                return link->link.path_link.direct_path;
            }

            return strcat(strdup("-L"), strcat(link->link.path_link.dir_name, strcat(strdup(" -l"), link->link.path_link.dep_name)));
        case Link_Framework:
            return strcat(strdup("-framework "), link->link.framework.dep_name);
    }
}

// --== Build Implementation ==--

void initCommon(Build* build) {
    const char* syms = "/sym_links";
    build->sym_link_dir_ = strdup(build->build_dir_);
    build->sym_link_dir_ = (char*)realloc(build->sym_link_dir_, strlen(build->sym_link_dir_) + strlen(syms) + 1);
    strcat(build->sym_link_dir_, syms);

    Command* create_sym_link_dir = newCommand(3, "mkdir", "-p", build->sym_link_dir_);
    cmdPrint(create_sym_link_dir);
    cmdExec(create_sym_link_dir);
    freeCommand(create_sym_link_dir);

    VectorPushBack(BuildStep, &build->build_steps_, (BuildStep){0});

    BuildStep* step = &build->build_steps_.ptr[build->build_steps_.len - 1];
    step->pre_step_commands = (CmdVec){0};
    step->comp_flags = (StrVec){0};
    step->link_flags = (StrVec){0};
    step->links = (LinkVec){0};
    step->includes = (IncludeVec){0};
    step->objects = (ObjVec){0};

    build->skip_compile_commands = false;
    build->jobs_ = 1;
    build->thread_queues_ = (BuildJobVec){0};

    build->builtin = (Builtin){
        .os = Os_Invalid,
        .mode = Mode_Release,
    };

#if defined(__linux__)
    build->builtin.os = Os_Linux;
#elif defined(__APPLE__)
    build->builtin.os = Os_MacOS;
#elif defined(__WIN32)
    build->builtin.os = Os_Windows;
#elif defined(__WIN64)
    build->builtin.os = Os_Windows;
#endif

    // Argparse
    for (size_t i = 0; i < build->argc; i++) {
        char* arg = build->argv[i];

        if (strcmp(arg, "-Debug") == 0) {
            build->builtin.mode = Mode_Debug;
        }

        if (strcmp(arg, "-j") == 0) {
            i++;
            errno = 0;
            char* end;
            long threads = strtol(arg, &end, 10);

            if (arg == end) {
                continue;
            } else if (errno == ERANGE) {
                printf("%s is not a valid number of jobs\n", arg);
                exit(1);
            } else if (*end != '\0') {
                printf("Error: extra characters after number of jobs: %s\n", end);
                exit(1);
            }

            build->jobs_ = threads;
        }
    }

    rebuildYourself(build);
}

Build* newBuild(const char* dir_name, int argc, char** argv) {
    Build* build = (Build*)calloc(1, sizeof(Build));

    build->argc = argc;
    build->argv = argv;
    build->build_dir_ = allocExpandPath(dir_name);
    build->compiler_ = (char*)DEFAULT_COMPILER;

    initCommon(build);

    return build;
}

Build* newBuildWithCompiler(const char* dir_name, char* compiler, int argc, char** argv) {
    Build* build = (Build*)calloc(1, sizeof(Build));

    build->argc = argc;
    build->argv = argv;
    build->build_dir_ = allocExpandPath(dir_name);
    build->compiler_ = compiler;

    initCommon(build);

    return build;
}

void freeBuild(Build* build) {
    for (int i = 0; i < build->build_steps_.len; i++) {
        BuildStep* step = &build->build_steps_.ptr[i];

        //                                          SO META OMG
        for (int c = 0; c < step->pre_step_commands.len; c++) {
            freeCommand(&step->pre_step_commands.ptr[c]);
        }
        VectorFree(&step->pre_step_commands);

        for (int o = 0; o < step->objects.len; o++) {
            destroyObject(&step->objects.ptr[o]);
        }
        VectorFree(&step->objects);

        for (int i = 0; i < step->includes.len; i++) {
            destroyInclude(&step->includes.ptr[i]);
        }
        VectorFree(&step->includes);

        for (int l = 0; l < step->links.len; l++) {
            destroyLink(&step->links.ptr[l]);
        }
        VectorFree(&step->links);

        VectorFree(&step->comp_flags);
        VectorFree(&step->link_flags);
    }
    VectorFree(&build->build_steps_);
    VectorFree(&build->thread_queues_);

    free(build->sym_link_dir_);
    free(build->build_dir_);
    free(build);
}

void rebuildYourself(Build* build) {
    const char* build_c = "build.c";
    const char* build_h = "build.h";
#ifdef _WIN32
    const char* build_exe = "build.exe";
#else
    const char* build_exe = "build";
#endif

    struct stat build_c_attr;
    struct stat build_h_attr;

    if (stat(build_c, &build_c_attr) != 0 || stat(build_h, &build_h_attr) != 0) {
        printf("Error: build.c or build.h file not found");
        exit(1);
    }

    time_t build_c_time = build_c_attr.st_mtime;
    time_t build_h_time = build_h_attr.st_mtime;

    bool rebuild = false;
    struct stat build_exe_attr;
    if (stat(build_exe, &build_exe_attr) == 0) {
        time_t build_exe_time = build_exe_attr.st_mtime;

        if (build_c_time > build_exe_time || build_h_time > build_exe_time) {
            rebuild = true;
        }
    } else {
        rebuild = true;
    }

    if (rebuild) {
        printf("Rebuilding build system...\n");
        Command* cmd = newCommand(5, build->compiler_, "-std=c23", build_c, "-o", build_exe);
        cmdPrint(cmd);

        int result = cmdExec(cmd);

        if (result != 0) {
            printf("Build System rebuild failed\n");
            exit(1);
        }

#ifdef __WIN32
        Command* run_cmd = newCommand(1, ".\\build.exe");
#else
        Command* run_cmd = newCommand(1, "./build");
#endif

        for (int i = 1; i < build->argc; i++) {
            cmdPushBack(run_cmd, build->argv[i]);
        }

        cmdExec(run_cmd);
        freeCommand(cmd);
        freeCommand(run_cmd);
        exit(0);
    }
}

void buildExportCompileCommands(Build* build, CompCmdVec* compile_commands) {
    if (build->skip_compile_commands) {
        return;
    }

    FILE* compile_commands_file = fopen("compile_commands.json", "w");
    if (!compile_commands_file) {
        printf("Failed to open compile_commands.json");
        return;
    }

    fprintf(compile_commands_file, "[\n");

    for (size_t i = 0; i < compile_commands->len; i++) {
        compCmdAddToFile(&compile_commands->ptr[i], compile_commands_file);
        if (i < compile_commands->len - 1) {
            fprintf(compile_commands_file, ",\n");
        } else {
            fprintf(compile_commands_file, "\n");
        }
    }

    fprintf(compile_commands_file, "]");
    fclose(compile_commands_file);
}

void buildSkipCompileCommands(Build* build) {
    build->skip_compile_commands = true;
}

void buildAddPrebuildCommand(Build* build, Command* command) {
    size_t build_step_index = build->build_steps_.len - 1;
    CmdVec* vec = &build->build_steps_.ptr[build_step_index].pre_step_commands;
    VectorPushBack(Command, vec, *command);
}

void buildAddObject(Build* build, Object* object) {
    size_t build_step_index = build->build_steps_.len - 1;
    ObjVec* vec = &build->build_steps_.ptr[build_step_index].objects;
    VectorPushBack(Object, vec, *object);
}

void buildAddInclude(Build* build, Include* include) {
    size_t build_step_index = build->build_steps_.len - 1;
    IncludeVec* vec = &build->build_steps_.ptr[build_step_index].includes;
    VectorPushBack(Include, vec, *include);
}

void buildAddLink(Build* build, Link* link) {
    size_t build_step_index = build->build_steps_.len - 1;
    LinkVec* vec = &build->build_steps_.ptr[build_step_index].links;
    VectorPushBack(Link, vec, *link);
}

void buildAddCompilationFlag(Build* build, char* flag) {
    size_t build_step_index = build->build_steps_.len - 1;
    StrVec* vec = &build->build_steps_.ptr[build_step_index].comp_flags;
    VectorPushBack(char*, vec, flag);
}

void buildAddLinkingFlag(Build* build, char* flag) {
    size_t build_step_index = build->build_steps_.len - 1;
    StrVec* vec = &build->build_steps_.ptr[build_step_index].link_flags;
    VectorPushBack(char*, vec, flag);
}

void buildStep(Build* build) {
    VectorPushBack(BuildStep, &build->build_steps_, (BuildStep){0});
}

typedef struct {
        Build* build;
        StrVec includes;
        BuildStep* step;
        StrVec* obj_files;
        pthread_mutex_t* obj_mutex;
        CompCmdVec* compile_cmds;
        pthread_mutex_t* comp_cmd_mutex;
        size_t job_number;
} CompileThreadArgs;

void* threadedCompile(void* arg) {
    CompileThreadArgs* args = (CompileThreadArgs*)arg;
    BuildJob* thread_queues = args->build->thread_queues_.ptr;
    size_t jobs_len = args->build->thread_queues_.len;

    ObjectQueue* q = thread_queues[args->job_number].objects;

    bool queue_is_empty = q->start == q->end;
    bool all_jobs_queued = thread_queues[args->job_number].all_jobs_queued;

    while (!queue_is_empty || !all_jobs_queued) {
        if (!queue_is_empty) {
            pthread_mutex_lock(&thread_queues[args->job_number].mutex);
            Object* obj = objQPop(thread_queues[args->job_number].objects);
            pthread_mutex_unlock(&thread_queues[args->job_number].mutex);

            size_t inc_len = 1;
            for (size_t i = 0; i < args->includes.len; i++) {
                inc_len += strlen(args->includes.ptr[i]) + 1;
            }
            char* includes_str = (char*)calloc(inc_len, sizeof(char));
            for (size_t i = 0; i < args->includes.len; i++) {
                char* inc = args->includes.ptr[i];
                strcat(includes_str, inc);
                strcat(includes_str, " ");
            }

            size_t comp_flags_len = 1;
            for (size_t i = 0; i < args->step->comp_flags.len; i++) {
                comp_flags_len += strlen(args->step->comp_flags.ptr[i]) + 1;
            }
            char* comp_flags_str = (char*)calloc(comp_flags_len, sizeof(char));
            for (size_t i = 0; i < args->step->comp_flags.len; i++) {
                char* flag = args->step->comp_flags.ptr[i];
                strcat(comp_flags_str, flag);
                strcat(comp_flags_str, " ");
            }

            compile(
                obj,
                args->build->compiler_,
                comp_flags_str,
                includes_str,
                args->build->build_dir_,
                args->obj_files,
                args->obj_mutex,
                args->compile_cmds,
                args->comp_cmd_mutex
            );
        }

        queue_is_empty = q->start == q->end;
        all_jobs_queued = thread_queues[args->job_number].all_jobs_queued;
    }

    thread_queues[args->job_number].all_jobs_complete = true;

    return nullptr;
}

void buildBuild(Build* build) {
    for (size_t i = 0; i < build->jobs_; i++) {
        BuildJob job = (BuildJob){
            .mutex = PTHREAD_MUTEX_INITIALIZER,
            .all_jobs_queued = false,
            .all_jobs_complete = false,
            .objects = newObjectQueue(100),
        };

        VectorPushBack(BuildJob, &build->thread_queues_, job);
    }

    CompCmdVec compile_commands = (CompCmdVec){0};
    pthread_mutex_t comp_commands_mutex = PTHREAD_MUTEX_INITIALIZER;

    for (int i = 0; i < build->build_steps_.len; i++) {
        BuildStep* step = &build->build_steps_.ptr[i];
        // Run any prebuild commands
        for (int j = 0; j < step->pre_step_commands.len; j++) {
            Command* cmd = &step->pre_step_commands.ptr[j];
            cmdPrint(cmd);
            int result = cmdExec(cmd);

            freeCommand(cmd);
            if (result != 0) {
                printf("Error executing command\n");
                exit(1);
            }
        }

        // Create the build step include paths
        StrVec includes = (StrVec){0};
        for (int j = 0; j < step->includes.len; j++) {
            Include* inc = &step->includes.ptr[j];

            VectorPushBack(char*, &includes, strdup("-I"));
            char* inc_path = allocIncludePath(inc, build->sym_link_dir_);
            VectorPushBack(char*, &includes, inc_path);
        }

        StrVec object_files = (StrVec){0};
        pthread_mutex_t obj_files_mutex = PTHREAD_MUTEX_INITIALIZER;

        pthread_t* threads = (pthread_t*)calloc(build->jobs_, sizeof(pthread_t));
        CompileThreadArgs* args = (CompileThreadArgs*)calloc(build->jobs_, sizeof(CompileThreadArgs));

        for (size_t t = 0; t < build->jobs_; t++) {
            build->thread_queues_.ptr[t].all_jobs_queued = false;
            build->thread_queues_.ptr[t].all_jobs_complete = false;

            args[t] = (CompileThreadArgs){
                build,
                includes,
                step,
                &object_files,
                &obj_files_mutex,
                &compile_commands,
                &comp_commands_mutex,
                t
            };

            pthread_create(
                &threads[t],
                nullptr,
                threadedCompile,
                &args[t]
            );
        }

        size_t job_index = 0;
        for (size_t j = 0; j < step->objects.len; j++) {
            Object* obj = &step->objects.ptr[j];
            pthread_mutex_lock(&build->thread_queues_.ptr[job_index].mutex);
            objQPush(build->thread_queues_.ptr[job_index].objects, obj);
            pthread_mutex_unlock(&build->thread_queues_.ptr[job_index].mutex);

            job_index = (job_index + 1) % build->jobs_;
        }

        // After the current job steps have been queued the let each thread know that
        // there will be no more work
        for (size_t j = 0; j < build->jobs_; j++) {
            build->thread_queues_.ptr[j].all_jobs_queued = true;
        }

        for (size_t j = 0; j < build->jobs_; j++) {
            pthread_join(threads[j], nullptr);
        }

        Command* linking_cmd = newCommand(1, build->compiler_);

        if (step->link_flags.len == 0) {
            cmdPushBack(linking_cmd, strdup("-std=c23"));
        } else {
            for (int k = 0; k < step->link_flags.len; k++) {
                cmdPushBack(linking_cmd, strdup(step->link_flags.ptr[k]));
            }
        }

        cmdPushBack(linking_cmd, strdup("-o"));
        char* output_file;
        asprintf(&output_file, "%s/main", build->build_dir_);
        cmdPushBack(linking_cmd, output_file);

        for (int k = 0; k < object_files.len; k++) {
            cmdPushBack(linking_cmd, object_files.ptr[k]);
        }

        for (int k = 0; k < step->links.len; k++) {
            char* linkable = allocLinkable(&step->links.ptr[k]);
            cmdPushBack(linking_cmd, linkable);
        }

        cmdPrint(linking_cmd);
        cmdExec(linking_cmd);

        for (int j = 0; j < includes.len; j++) {
            free(includes.ptr[j]);
        }
        VectorFree(&includes);
        VectorFree(&object_files);

        freeCommand(linking_cmd);
        for (size_t i = 0; i < build->jobs_; i++) {
            freeObjQueue(build->thread_queues_.ptr[i].objects);
        }
        free(threads);
        free(args);
    }

    buildExportCompileCommands(build, &compile_commands);
    for (size_t k = 0; k < compile_commands.len; k++) {
        freeCompileCommand(&compile_commands.ptr[k]);
    }
}

// #endif // BUILD_IMPLEMENTATION
#endif // BUILD_H
