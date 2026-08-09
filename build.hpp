#pragma once

#ifndef COMMON_H
#define COMMON_H

#include <assert.h>
#include <stdint.h>
#include <sys/types.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
typedef size_t usize;
#else
typedef uint64_t usize;
#endif

typedef int8_t  i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
typedef ssize_t isize;
#else
typedef int64_t isize;
#endif

typedef float  f32;
typedef double f64;

static_assert(sizeof(f32) == 4, "Size of float must be 32-bits");
static_assert(sizeof(f64) == 8, "Size of double must be 64-bits");

#define __MAX__(a, b)                                                                                                                                \
    ({                                                                                                                                               \
        __typeof__(a) _a = (a);                                                                                                                      \
        __typeof__(b) _b = (b);                                                                                                                      \
        _a > _b ? _a : _b;                                                                                                                           \
    })

#define __MIN__(a, b)                                                                                                                                \
    ({                                                                                                                                               \
        __typeof__(a) _a = (a);                                                                                                                      \
        __typeof__(b) _b = (b);                                                                                                                      \
        _a < _b ? _a : _b;                                                                                                                           \
    })

#define __STRINGIFY__(x) #x

// __CUDACC__, not __cplusplus: plain C++ projects must not have to find the CUDA runtime
#ifdef __CUDACC__
#include <cuda_runtime.h>
extern cudaStream_t g_compute_stream;
#endif

#endif // COMMON_H

#define RLOG_IMPLEMENTATION

#ifndef RLOG_H
#define RLOG_H

#ifdef __cplusplus
#include <atomic>
extern "C" {
#endif

typedef enum {
    LL_TRACE,
    LL_DEBUG,
    LL_INFO,
    LL_WARN,
    LL_ERROR,
    LL_FATAL,
} LogLevel;

#define __LOG_WHITE "\033[97m"
#define __LOG_BLUE "\033[94m"
#define __LOG_GREEN "\033[92m"
#define __LOG_YELLOW "\033[93m"
#define __LOG_RED "\033[91m"
#define __LOG_PINK "\033[35m"
#define __LOG_RESET "\033[0m"

#define __FATAL_EXIT 404

static LogLevel __global_log_level = LL_INFO;
static bool     __log_verbose = false;

void initLog(u32 buffer_size);
void __Log_impl(LogLevel level, const char* file, u32 line, const char* fmt, ...);
void __Log_file_impl(const char* path, LogLevel level, const char* file, u32 line, const char* fmt, ...);

#ifdef RLOG_IMPLEMENTATION

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
#define RUNNING_LOAD(x) ((x).load())
#define RUNNING_STORE(x, v) ((x).store(v))
#define RUNNING_INIT(x, v) ((x).store(v))
#else
#define RUNNING_LOAD(x) (x)
#define RUNNING_STORE(x, v) ((x) = (v))
#define RUNNING_INIT(x, v) ((x) = (v))
#endif

typedef struct {
    char*           buf;
    u32             capacity;
    u32             write_pos;
    u32             read_pos;
    u32             data_len;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_t       thread;
#ifdef __cplusplus
    std::atomic<bool> running;
#else
    volatile bool running;
#endif
} __RLogState;

#ifndef RLOG_MAX_FILES
#define RLOG_MAX_FILES 8
#endif

typedef struct {
    char  path[256];
    FILE* fp;
} __RLogFileEntry;

static __RLogFileEntry __rlog_files[RLOG_MAX_FILES];
static u32              __rlog_file_count = 0;

static __RLogState __rlog_state;

static void* __rlog_thread_fn(void* arg) {
    (void)arg;
    __RLogState* s = &__rlog_state;

    while (true) {
        pthread_mutex_lock(&s->mutex);

        while (s->data_len == 0 && RUNNING_LOAD(s->running))
            pthread_cond_wait(&s->cond, &s->mutex);

        while (s->data_len > 0) {
            if (s->read_pos + 4 > s->capacity) {
                s->data_len -= s->capacity - s->read_pos;
                s->read_pos = 0;
                continue;
            }

            u32 raw_len;
            memcpy(&raw_len, s->buf + s->read_pos, 4);

            if (raw_len == UINT32_MAX) {
                s->data_len -= s->capacity - s->read_pos;
                s->read_pos = 0;
                continue;
            }

            bool is_file = (raw_len >> 31) & 1;
            u32 len = raw_len & 0x7FFFFFFFu;
            u32 header_size = 4;
            FILE* fp = stderr;

            if (is_file) {
                memcpy(&fp, s->buf + s->read_pos + 4, sizeof(FILE*));
                header_size = 4 + (u32)sizeof(FILE*);
            }

            s->data_len -= header_size + len;
            s->read_pos += header_size;
            fwrite(s->buf + s->read_pos, 1, len, fp);
            s->read_pos += len;
            if (s->read_pos == s->capacity)
                s->read_pos = 0;
        }

        bool still_running = RUNNING_LOAD(s->running);
        pthread_mutex_unlock(&s->mutex);

        if (!still_running)
            break;
    }

    return NULL;
}

static void __rlog_shutdown(void) {
    __RLogState* s = &__rlog_state;
    pthread_mutex_lock(&s->mutex);
    RUNNING_STORE(s->running, false);
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    pthread_join(s->thread, NULL);
    free(s->buf);
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->cond);
    for (u32 i = 0; i < __rlog_file_count; i++) {
        if (__rlog_files[i].fp != NULL) {
            fclose(__rlog_files[i].fp);
        }
    }
}

void initLog(u32 buffer_size) {
    char* log_verbose = getenv("LOG_VERBOSE");
    if (log_verbose != nullptr) {
        __log_verbose = true;
    }

    char* log_level = getenv("LOG_LEVEL");
    if (log_level != nullptr) {
        if (memcmp(log_level, "TRACE", sizeof(char) * 5) == 0) {
            __global_log_level = LL_TRACE;
        } else if (memcmp(log_level, "DEBUG", sizeof(char) * 5) == 0) {
            __global_log_level = LL_DEBUG;
        } else if (memcmp(log_level, "INFO", sizeof(char) * 4) == 0) {
            __global_log_level = LL_INFO;
        } else if (memcmp(log_level, "WARN", sizeof(char) * 4) == 0) {
            __global_log_level = LL_WARN;
        } else if (memcmp(log_level, "ERROR", sizeof(char) * 5) == 0) {
            __global_log_level = LL_ERROR;
        } else if (memcmp(log_level, "FATAL", sizeof(char) * 5) == 0) {
            __global_log_level = LL_FATAL;
        }
    }

    __RLogState* s = &__rlog_state;
    s->buf = (char*)malloc(buffer_size);
    s->capacity = buffer_size;
    s->write_pos = s->read_pos = s->data_len = 0;
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);
    RUNNING_INIT(s->running, true);
    pthread_create(&s->thread, NULL, __rlog_thread_fn, NULL);
    atexit(__rlog_shutdown);
}

// Takes an already-started va_list so the std::string overload below can share this too.
static void __Log_vimpl(LogLevel level, const char* file, u32 line, const char* fmt, va_list args) {
    if (level < __global_log_level) {
        return;
    }

    char msg[1024];
    i32  prefix_len;

    if (__log_verbose) {
        switch (level) {
            case LL_TRACE:
                prefix_len = snprintf(msg, sizeof(msg), __LOG_WHITE "[TRACE | %s | %d ]: " __LOG_RESET, file, line);
                break;
            case LL_DEBUG:
                prefix_len = snprintf(msg, sizeof(msg), __LOG_BLUE "[DEBUG | %s | %d ]: " __LOG_RESET, file, line);
                break;
            case LL_INFO:
                prefix_len = snprintf(msg, sizeof(msg), __LOG_GREEN "[INFO | %s | %d ]: " __LOG_RESET, file, line);
                break;
            case LL_WARN:
                prefix_len = snprintf(msg, sizeof(msg), __LOG_YELLOW "[WARN | %s | %d ]: " __LOG_RESET, file, line);
                break;
            case LL_ERROR:
                prefix_len = snprintf(msg, sizeof(msg), __LOG_RED "[ERROR | %s | %d ]: " __LOG_RESET, file, line);
                break;
            case LL_FATAL:
                prefix_len = snprintf(msg, sizeof(msg), __LOG_PINK "[FATAL | %s | %d ]: " __LOG_RESET, file, line);
                break;
            default:
                return;
        }
    } else {
        switch (level) {
            case LL_TRACE:
                prefix_len = snprintf(msg, sizeof(msg), __LOG_WHITE "[TRACE]: " __LOG_RESET);
                break;
            case LL_DEBUG:
                prefix_len = snprintf(msg, sizeof(msg), __LOG_BLUE "[DEBUG]: " __LOG_RESET);
                break;
            case LL_INFO:
                prefix_len = snprintf(msg, sizeof(msg), __LOG_GREEN "[INFO]: " __LOG_RESET);
                break;
            case LL_WARN:
                prefix_len = snprintf(msg, sizeof(msg), __LOG_YELLOW "[WARN]: " __LOG_RESET);
                break;
            case LL_ERROR:
                prefix_len = snprintf(msg, sizeof(msg), __LOG_RED "[ERROR]: " __LOG_RESET);
                break;
            case LL_FATAL:
                prefix_len = snprintf(msg, sizeof(msg), __LOG_PINK "[FATAL]: " __LOG_RESET);
                break;
            default:
                return;
        }
    }

    if (prefix_len < 0 || prefix_len >= (i32)sizeof(msg)) {
        prefix_len = 0;
    }

    i32 body_len = vsnprintf(msg + prefix_len, sizeof(msg) - (u32)prefix_len, fmt, args);

    if (body_len < 0) {
        body_len = 0;
    }

    i32 total = prefix_len + body_len;
    if (total >= (i32)sizeof(msg) - 1) {
        total = (i32)sizeof(msg) - 2;
    }
    msg[total] = '\n';
    total += 1;

    u32 msg_len = (u32)total;
    u32 entry_size = 4 + msg_len;

    __RLogState* s = &__rlog_state;
    pthread_mutex_lock(&s->mutex);

    u32 space_at_end = s->capacity - s->write_pos;

    if (space_at_end < entry_size) {
        if (s->data_len + space_at_end + entry_size > s->capacity) {
            pthread_mutex_unlock(&s->mutex);
            goto fatal_check;
        }
        if (space_at_end >= 4) {
            u32 sentinel = UINT32_MAX;
            memcpy(s->buf + s->write_pos, &sentinel, 4);
        }
        s->data_len += space_at_end;
        s->write_pos = 0;
    }

    if (s->data_len + entry_size > s->capacity) {
        pthread_mutex_unlock(&s->mutex);
        goto fatal_check;
    }

    memcpy(s->buf + s->write_pos, &msg_len, 4);
    memcpy(s->buf + s->write_pos + 4, msg, msg_len);
    s->write_pos += entry_size;
    if (s->write_pos == s->capacity)
        s->write_pos = 0;
    s->data_len += entry_size;

    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);

fatal_check:
    if (level == LL_FATAL) {
        exit(__FATAL_EXIT);
    }
}

void __Log_impl(LogLevel level, const char* file, u32 line, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __Log_vimpl(level, file, line, fmt, args);
    va_end(args);
}

static void __Log_file_vimpl(const char* path, LogLevel level, const char* file, u32 line, const char* fmt, va_list args) {
    if (level < __global_log_level) {
        return;
    }

    char msg[1024];
    i32  prefix_len;

    if (__log_verbose) {
        switch (level) {
            case LL_TRACE:
                prefix_len = snprintf(msg, sizeof(msg), "[TRACE | %s | %d ]: ", file, line);
                break;
            case LL_DEBUG:
                prefix_len = snprintf(msg, sizeof(msg), "[DEBUG | %s | %d ]: ", file, line);
                break;
            case LL_INFO:
                prefix_len = snprintf(msg, sizeof(msg), "[INFO | %s | %d ]: ", file, line);
                break;
            case LL_WARN:
                prefix_len = snprintf(msg, sizeof(msg), "[WARN | %s | %d ]: ", file, line);
                break;
            case LL_ERROR:
                prefix_len = snprintf(msg, sizeof(msg), "[ERROR | %s | %d ]: ", file, line);
                break;
            case LL_FATAL:
                prefix_len = snprintf(msg, sizeof(msg), "[FATAL | %s | %d ]: ", file, line);
                break;
            default:
                return;
        }
    } else {
        switch (level) {
            case LL_TRACE:
                prefix_len = snprintf(msg, sizeof(msg), "[TRACE]: ");
                break;
            case LL_DEBUG:
                prefix_len = snprintf(msg, sizeof(msg), "[DEBUG]: ");
                break;
            case LL_INFO:
                prefix_len = snprintf(msg, sizeof(msg), "[INFO]: ");
                break;
            case LL_WARN:
                prefix_len = snprintf(msg, sizeof(msg), "[WARN]: ");
                break;
            case LL_ERROR:
                prefix_len = snprintf(msg, sizeof(msg), "[ERROR]: ");
                break;
            case LL_FATAL:
                prefix_len = snprintf(msg, sizeof(msg), "[FATAL]: ");
                break;
            default:
                return;
        }
    }

    if (prefix_len < 0 || prefix_len >= (i32)sizeof(msg)) {
        prefix_len = 0;
    }

    i32 body_len = vsnprintf(msg + prefix_len, sizeof(msg) - (u32)prefix_len, fmt, args);

    if (body_len < 0) {
        body_len = 0;
    }

    i32 total = prefix_len + body_len;
    if (total >= (i32)sizeof(msg) - 1) {
        total = (i32)sizeof(msg) - 2;
    }
    msg[total] = '\n';
    total += 1;

    u32 msg_len = (u32)total;
    u32 entry_size = 4 + (u32)sizeof(FILE*) + msg_len;

    __RLogState* s = &__rlog_state;
    pthread_mutex_lock(&s->mutex);

    // Lazy-open: find or create file entry while holding the mutex
    FILE* fp = NULL;
    for (u32 i = 0; i < __rlog_file_count; i++) {
        if (strncmp(__rlog_files[i].path, path, 255) == 0) {
            fp = __rlog_files[i].fp;
            break;
        }
    }
    if (fp == NULL && __rlog_file_count < RLOG_MAX_FILES) {
        fp = fopen(path, "a");
        if (fp != NULL) {
            strncpy(__rlog_files[__rlog_file_count].path, path, 255);
            __rlog_files[__rlog_file_count].path[255] = '\0';
            __rlog_files[__rlog_file_count].fp = fp;
            __rlog_file_count++;
        }
    }

    u32 space_at_end = s->capacity - s->write_pos;

    if (fp == NULL) {
        pthread_mutex_unlock(&s->mutex);
        goto fatal_check;
    }

    if (space_at_end < entry_size) {
        if (s->data_len + space_at_end + entry_size > s->capacity) {
            pthread_mutex_unlock(&s->mutex);
            goto fatal_check;
        }
        if (space_at_end >= 4) {
            u32 sentinel = UINT32_MAX;
            memcpy(s->buf + s->write_pos, &sentinel, 4);
        }
        s->data_len += space_at_end;
        s->write_pos = 0;
    }

    if (s->data_len + entry_size > s->capacity) {
        pthread_mutex_unlock(&s->mutex);
        goto fatal_check;
    }

    {
        u32 raw_len = msg_len | (1u << 31);
        memcpy(s->buf + s->write_pos, &raw_len, 4);
        memcpy(s->buf + s->write_pos + 4, &fp, sizeof(FILE*));
        memcpy(s->buf + s->write_pos + 4 + sizeof(FILE*), msg, msg_len);
        s->write_pos += entry_size;
        if (s->write_pos == s->capacity)
            s->write_pos = 0;
        s->data_len += entry_size;
    }

    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);

fatal_check:
    if (level == LL_FATAL) {
        exit(__FATAL_EXIT);
    }
}

void __Log_file_impl(const char* path, LogLevel level, const char* file, u32 line, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __Log_file_vimpl(path, level, file, line, fmt, args);
    va_end(args);
}

#endif // RLOG_IMPLEMENTATION

#define RLOG(level, fmt, ...) __Log_impl(level, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define RLOG_FILE(path, level, fmt, ...) __Log_file_impl(path, level, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}

// std::string overload of fmt, C++ only. Plain "..." variadic, not a template — mixed
// with the char* version's "...", overload resolution can't pick a winner per-argument.
// fmt is by value, not const&: va_start's last named param can't be a reference type.
#include <string>

inline void __Log_impl(LogLevel level, const char* file, u32 line, std::string fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __Log_vimpl(level, file, line, fmt.c_str(), args);
    va_end(args);
}

inline void __Log_file_impl(const char* path, LogLevel level, const char* file, u32 line, std::string fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __Log_file_vimpl(path, level, file, line, fmt.c_str(), args);
    va_end(args);
}
#endif

#endif // RLOG_H

#include <atomic>
#include <concepts>
#include <cstdio>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <forward_list>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

struct CommandOutput {
        i32         exit_code;
        std::string stdout_output;
        std::string stderr_output;
};

class Command {
    private:
        std::vector<std::string>             command_chain_;
        std::optional<std::filesystem::path> exec_dir_;
        // Only meaningful when this Command is going into an Output (see
        // Output::assignCommandName) — a bare Command used internally (e.g.
        // Object::compileCommand) never sets this.
        std::filesystem::path                name_;

    public:
        Command() = default;
        Command(std::initializer_list<std::string> command_chain) : command_chain_(command_chain) {}

        template <typename T>
        void push_back(T&& arg) { command_chain_.emplace_back(std::forward<T>(arg)); }

        // Unset (default) runs in the current directory, same as before this existed.
        // Scoped to this one subprocess via a shell "cd X && ..." prefix rather than an
        // actual chdir() call — chdir() is process-wide, and exec() runs concurrently
        // across the thread pool's worker threads, so a real chdir() would race across
        // threads compiling different files.
        void setExecDir(const std::filesystem::path& dir) { exec_dir_ = dir; }

        void setName(const std::filesystem::path& name) { name_ = name; }

        const std::filesystem::path& name() const { return name_; }

        // Symbolic only — a Command has no predictable single output file, so this is
        // never expected to point at anything real. It only exists so Output's
        // sourcePath()/outputPath() dispatch can treat Command the same as Binary/
        // Library without a special case.
        std::filesystem::path path(const std::filesystem::path& build_dir) const { return build_dir / name_; }

        const std::vector<std::string>& command_chain() const { return command_chain_; }

        std::string string() const {
            std::string result;

            for (usize i = 0; i < command_chain_.size(); ++i) {
                if (i > 0) {
                    result += " ";
                }

                result += command_chain_[i];
            }

            return result;
        }

        CommandOutput exec() const {
            std::filesystem::path stderr_path = std::filesystem::temp_directory_path() / "buildcpp_stderr_XXXXXX";
            std::string           stderr_template = stderr_path.string();

            i32 stderr_fd = mkstemp(stderr_template.data());
            close(stderr_fd);

            std::string command = string() + " 2>" + stderr_template;
            if (exec_dir_.has_value()) {
                command = "cd " + exec_dir_->string() + " && " + command;
            }

            FILE* pipe = popen(command.c_str(), "r");

            CommandOutput output;
            char          buffer[256];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                output.stdout_output += buffer;
            }

            i32 result = pclose(pipe);

            std::ifstream     stderr_file(stderr_template);
            std::stringstream stderr_stream;
            stderr_stream << stderr_file.rdbuf();
            output.stderr_output = stderr_stream.str();

            std::filesystem::remove(stderr_template);

            output.exit_code = WEXITSTATUS(result);
            return output;
        }

        // Unlike exec(), doesn't capture output — runs via system(), which inherits
        // the caller's stdout/stderr and streams it live. For handing off to another
        // process the user should actually see running (e.g. self-rebuild's re-exec),
        // not for anything whose output needs to be inspected afterward.
        i32 run() const {
            std::string command = string();
            if (exec_dir_.has_value()) {
                command = "cd " + exec_dir_->string() + " && " + command;
            }

            i32 result = system(command.c_str());
            return WEXITSTATUS(result);
        }
};

// Leading __ marks these internal: not meant to be named directly, use Include<Kind>.
template <typename Derived>
class __IncludeBase {
    protected:
        std::filesystem::path include_path_;

    public:
        __IncludeBase(const std::filesystem::path& include_path) : include_path_(include_path) {}

        // sym_links is unused on the Direct side, kept for a uniform signature.
        std::filesystem::path path(const std::filesystem::path& sym_links) const;
};

class __IncludeDirect : public __IncludeBase<__IncludeDirect> {
    public:
        using __IncludeBase::__IncludeBase;

        // Hides __IncludeBase::path rather than specializing it: the base can't reach
        // symbolic_dir_ on the Symbolic side.
        std::filesystem::path path(const std::filesystem::path& sym_links) const {
            (void)sym_links;
            return std::filesystem::relative(include_path_, std::filesystem::current_path());
        }
};

class __IncludeSymbolic : public __IncludeBase<__IncludeSymbolic> {
    private:
        std::filesystem::path symbolic_dir_;

    public:
        __IncludeSymbolic(const std::filesystem::path& include_path, const std::filesystem::path& symbolic_dir)
            : __IncludeBase(include_path), symbolic_dir_(symbolic_dir) {}

        std::filesystem::path path(const std::filesystem::path& sym_links) const {
            std::filesystem::path symlink_path = sym_links / symbolic_dir_;

            // symlink_status, not exists(): exists() follows the link and misreports a
            // dangling symlink as absent, which would make create_directory_symlink throw.
            if (!std::filesystem::exists(std::filesystem::symlink_status(symlink_path))) {
                std::filesystem::create_directories(symlink_path.parent_path());
                std::filesystem::create_directory_symlink(std::filesystem::absolute(include_path_), symlink_path);
            }

            return symlink_path;
        }
};

// No default Kind: Include<Direct>/Include<Symbolic> must always be spelled out.
struct Direct {};
struct Symbolic {};

template <typename Kind>
struct __IncludeTypeOf;

template <>
struct __IncludeTypeOf<Direct> {
        using type = __IncludeDirect;
};

template <>
struct __IncludeTypeOf<Symbolic> {
        using type = __IncludeSymbolic;
};

template <typename Kind>
using Include = typename __IncludeTypeOf<Kind>::type;

template <typename T>
concept __IsInclude = std::same_as<T, __IncludeDirect> || std::same_as<T, __IncludeSymbolic>;

using __IncludeVariant = std::variant<__IncludeDirect, __IncludeSymbolic>;

struct CompileCommandEntry {
        std::filesystem::path directory;
        std::string           command;
        std::filesystem::path file;
};

class Object {
    private:
        std::filesystem::path source_path_;
        std::filesystem::path output_path_;

    public:
        Object(const std::filesystem::path& source_path) : source_path_(source_path) {}

        const std::filesystem::path& sourcePath() const { return source_path_; }

        std::filesystem::path outputPath(const std::filesystem::path& build_dir) const {
            std::filesystem::path path = build_dir / source_path_;
            path.replace_extension(".o");
            return path;
        }

        CommandOutput compile(const std::string& compiler, const std::filesystem::path& build_dir, const std::vector<std::filesystem::path>& include_paths, const std::vector<std::string>& compile_flags) {
            RLOG(LL_INFO, "Compiling: " + source_path_.string());
            output_path_ = outputPath(build_dir);

            std::filesystem::create_directories(output_path_.parent_path());

            return compileCommand(compiler, build_dir, include_paths, compile_flags).exec();
        }

        // What compile() would run, without running it — shared so compile_commands.json
        // generation and the real compile can never drift apart.
        CompileCommandEntry compileCommandEntry(const std::string& compiler, const std::filesystem::path& build_dir, const std::vector<std::filesystem::path>& include_paths, const std::vector<std::string>& compile_flags) const {
            return CompileCommandEntry{
                std::filesystem::current_path(),
                compileCommand(compiler, build_dir, include_paths, compile_flags).string(),
                std::filesystem::absolute(source_path_),
            };
        }

        // compile_flags are included here too (not just compile()): a -D flag can gate
        // an #include behind a macro, so dependency discovery run without the same
        // flags as the real compile could silently diverge from it.
        std::vector<std::filesystem::path> listDependencies(const std::string& compiler, const std::vector<std::filesystem::path>& include_paths, const std::vector<std::string>& compile_flags) {
            Command cmd({compiler});
            for (const auto& flag : compile_flags) {
                cmd.push_back(flag);
            }
            for (const auto& include_path : include_paths) {
                cmd.push_back("-I" + include_path.string());
            }
            cmd.push_back("-MM");
            cmd.push_back(source_path_.string());

            CommandOutput out = cmd.exec();

            std::string deps_output = out.stdout_output;
            std::erase_if(deps_output, [](char c) { return c == '\\' || c == '\n'; });

            usize colon = deps_output.find(':');
            std::string deps_list = colon != std::string::npos ? deps_output.substr(colon + 1) : "";

            std::vector<std::filesystem::path> dependencies;
            std::istringstream               iss(deps_list);
            std::string                      token;
            while (iss >> token) {
                dependencies.push_back(token);
            }

            return dependencies;
        }

    private:
        Command compileCommand(const std::string& compiler, const std::filesystem::path& build_dir, const std::vector<std::filesystem::path>& include_paths, const std::vector<std::string>& compile_flags) const {
            Command cmd({compiler});
            for (const auto& flag : compile_flags) {
                cmd.push_back(flag);
            }
            for (const auto& include_path : include_paths) {
                cmd.push_back("-I" + include_path.string());
            }
            cmd.push_back("-c");
            cmd.push_back(source_path_.string());
            cmd.push_back("-o");
            cmd.push_back(outputPath(build_dir).string());

            return cmd;
        }
};

template <typename Derived>
class __LinkBase {
    protected:
        std::string dep_name_;

    public:
        __LinkBase(const std::string& dep_name) : dep_name_(dep_name) {}

        std::string linkable() const;
};

class __LinkDependency : public __LinkBase<__LinkDependency> {
    public:
        using __LinkBase::__LinkBase;

        std::string linkable() const {
            return "-l" + dep_name_;
        }
};

class __LinkPath : public __LinkBase<__LinkPath> {
    private:
        std::filesystem::path directory_;

    public:
        __LinkPath(const std::string& dep_name, const std::filesystem::path& directory)
            : __LinkBase(dep_name), directory_(directory) {}

        std::string linkable() const {
            return "-L" + directory_.string() + " -l" + dep_name_;
        }
};

#if defined(__APPLE__)
class __LinkFramework : public __LinkBase<__LinkFramework> {
    public:
        using __LinkBase::__LinkBase;

        std::string linkable() const {
            return "-framework " + dep_name_;
        }
};
#endif

struct Dependency {};
struct Path {};
#if defined(__APPLE__)
struct Framework {};
#endif

template <typename Kind>
struct __LinkTypeOf;

template <>
struct __LinkTypeOf<Dependency> {
        using type = __LinkDependency;
};

template <>
struct __LinkTypeOf<Path> {
        using type = __LinkPath;
};

#if defined(__APPLE__)
template <>
struct __LinkTypeOf<Framework> {
        using type = __LinkFramework;
};
#endif

template <typename Kind>
using Link = typename __LinkTypeOf<Kind>::type;

template <typename T>
concept __IsLink = std::same_as<T, __LinkDependency> || std::same_as<T, __LinkPath>
#if defined(__APPLE__)
    || std::same_as<T, __LinkFramework>
#endif
    ;

#if defined(__APPLE__)
using __LinkVariant = std::variant<__LinkDependency, __LinkPath, __LinkFramework>;
#else
using __LinkVariant = std::variant<__LinkDependency, __LinkPath>;
#endif

// Only holds a name, not a path: the actual location (<build_dir>/bin/<name>) isn't
// known until build_dir shows up at execute() time, same as Object's source vs. output
// path split.
class Binary {
    private:
        std::filesystem::path name_;

    public:
        Binary(const std::filesystem::path& name) : name_(name) {}

        const std::filesystem::path& name() const { return name_; }

        std::filesystem::path path(const std::filesystem::path& build_dir) const { return build_dir / "bin" / name_; }

        // linkables (-lfoo/-L.../-framework Foo) go after the object files that need
        // their symbols, matching normal linker convention.
        CommandOutput link(
            const std::string& compiler, const std::filesystem::path& build_dir, const std::vector<std::filesystem::path>& object_files,
            const std::vector<std::string>& link_flags, const std::vector<std::string>& linkables
        ) {
            std::filesystem::path output_path = path(build_dir);
            std::filesystem::create_directories(output_path.parent_path());

            Command cmd({compiler});
            for (const auto& flag : link_flags) {
                cmd.push_back(flag);
            }
            for (const auto& obj : object_files) {
                cmd.push_back(obj.string());
            }
            for (const auto& linkable : linkables) {
                cmd.push_back(linkable);
            }
            cmd.push_back("-o");
            cmd.push_back(output_path.string());

            return cmd.exec();
        }
};

enum class Linkage { Shared, Static };

class Library {
    private:
        std::filesystem::path name_;
        Linkage                linkage_;

    public:
        Library(const std::filesystem::path& name, Linkage linkage) : name_(name), linkage_(linkage) {}

        const std::filesystem::path& name() const { return name_; }

        Linkage linkage() const { return linkage_; }

        std::filesystem::path path(const std::filesystem::path& build_dir) const {
            std::filesystem::path filename = "lib" + name_.string();
#if defined(__APPLE__)
            filename += linkage_ == Linkage::Static ? ".a" : ".dylib";
#else
            filename += linkage_ == Linkage::Static ? ".a" : ".so";
#endif
            return build_dir / "lib" / filename;
        }

        // link_flags/linkables only apply to the shared-library path — ar (static
        // archiving) has no notion of compiler/linker flags or external libraries.
        CommandOutput link(
            const std::string& compiler, const std::filesystem::path& build_dir, const std::vector<std::filesystem::path>& object_files,
            const std::vector<std::string>& link_flags, const std::vector<std::string>& linkables
        ) {
            std::filesystem::path output_path = path(build_dir);
            std::filesystem::create_directories(output_path.parent_path());

            if (linkage_ == Linkage::Static) {
                std::filesystem::remove(output_path);

                Command cmd({"ar", "rcs", output_path.string()});
                for (const auto& obj : object_files) {
                    cmd.push_back(obj.string());
                }
                return cmd.exec();
            }

            Command cmd({compiler});
            for (const auto& flag : link_flags) {
                cmd.push_back(flag);
            }
#if defined(__APPLE__)
            cmd.push_back("-dynamiclib");
#else
            cmd.push_back("-shared");
#endif
            for (const auto& obj : object_files) {
                cmd.push_back(obj.string());
            }
            for (const auto& linkable : linkables) {
                cmd.push_back(linkable);
            }
            cmd.push_back("-o");
            cmd.push_back(output_path.string());

            return cmd.exec();
        }
};

class Build;

// Wraps whatever a task ultimately produces: a compiled Object, a linked Binary/
// Library, or a bare Command (a pre/post-build step with no compile or link semantics
// of its own — see Output::assignCommandName).
class Output {
    private:
        std::variant<Object, Binary, Library, Command> value_;

    public:
        Output(Object object) : value_(std::move(object)) {}
        Output(Binary binary) : value_(std::move(binary)) {}
        Output(Library library) : value_(std::move(library)) {}
        Output(Command command) : value_(std::move(command)) {}

        // Sorts by variant: Object gets compile_flags; Binary/Library get link_flags and
        // linkables (-lfoo/-L.../-framework Foo); Command just runs — none of the
        // compile/link machinery applies to it.
        CommandOutput execute(
            const std::string&                         compiler,
            const std::filesystem::path&                build_dir,
            const std::vector<std::filesystem::path>&   include_paths,
            const std::vector<std::filesystem::path>&   object_files,
            const std::vector<std::string>&              compile_flags,
            const std::vector<std::string>&              link_flags,
            const std::vector<std::string>&              linkables
        ) {
            return std::visit(
                [&](auto& out) -> CommandOutput {
                    using T = std::decay_t<decltype(out)>;

                    if constexpr (std::same_as<T, Object>) {
                        return out.compile(compiler, build_dir, include_paths, compile_flags);
                    } else if constexpr (std::same_as<T, Command>) {
                        return out.exec();
                    } else {
                        return out.link(compiler, build_dir, object_files, link_flags, linkables);
                    }
                },
                value_
            );
        }

        std::vector<std::filesystem::path> listDependencies(const std::string& compiler, const std::vector<std::filesystem::path>& include_paths, const std::vector<std::string>& compile_flags) {
            return std::visit(
                [&](auto& out) -> std::vector<std::filesystem::path> {
                    using T = std::decay_t<decltype(out)>;

                    if constexpr (std::same_as<T, Object>) {
                        return out.listDependencies(compiler, include_paths, compile_flags);
                    } else {
                        return {};
                    }
                },
                value_
            );
        }

        // Object's own source file for the Object variant; Binary/Library have no
        // source file, so their name stands in — this is only ever used as a DAG key
        // (see BuildGroup::addTask), not as something fed to the compiler.
        const std::filesystem::path& sourcePath() const {
            return std::visit(
                [](const auto& out) -> const std::filesystem::path& {
                    using T = std::decay_t<decltype(out)>;

                    if constexpr (std::same_as<T, Object>) {
                        return out.sourcePath();
                    } else {
                        return out.name();
                    }
                },
                value_
            );
        }

        std::filesystem::path outputPath(const std::filesystem::path& build_dir) const {
            return std::visit(
                [&](const auto& out) -> std::filesystem::path {
                    using T = std::decay_t<decltype(out)>;

                    if constexpr (std::same_as<T, Object>) {
                        return out.outputPath(build_dir);
                    } else {
                        return out.path(build_dir);
                    }
                },
                value_
            );
        }

        // Doesn't consider header includes yet (see Object::listDependencies) — only
        // the Object variant's own source file, or the Binary/Library variant's object
        // files. Missing output, or any input newer than the output, means stale.
        bool isStale(const std::filesystem::path& build_dir, const std::vector<std::filesystem::path>& object_files) const {
            std::filesystem::path output_path = outputPath(build_dir);

            if (!std::filesystem::exists(output_path)) {
                return true;
            }

            std::filesystem::file_time_type output_time = std::filesystem::last_write_time(output_path);

            return std::visit(
                [&](const auto& out) -> bool {
                    using T = std::decay_t<decltype(out)>;

                    if constexpr (std::same_as<T, Object>) {
                        return std::filesystem::last_write_time(out.sourcePath()) > output_time;
                    } else if constexpr (std::same_as<T, Command>) {
                        // No principled way to know if a shell command's effects are
                        // up to date without it telling us what it reads/writes —
                        // matches build.h's own pre-build commands, which also always
                        // rerun unconditionally.
                        return true;
                    } else {
                        for (const auto& object_file : object_files) {
                            if (!std::filesystem::exists(object_file) || std::filesystem::last_write_time(object_file) > output_time) {
                                return true;
                            }
                        }
                        return false;
                    }
                },
                value_
            );
        }

        bool isObject() const { return std::holds_alternative<Object>(value_); }

        // Defined out-of-line, after Build, since Build isn't a complete type yet here.
        // No-op for every variant except Command, which gets "cmd_<Build::nextCommandId()>".
        void assignCommandName(Build& build);

        // Only the Object variant ever produces a compile-commands entry — Binary/Library
        // link steps aren't compilations of a translation unit.
        std::optional<CompileCommandEntry> compileCommandEntry(const std::string& compiler, const std::filesystem::path& build_dir, const std::vector<std::filesystem::path>& include_paths, const std::vector<std::string>& compile_flags) const {
            return std::visit(
                [&](const auto& out) -> std::optional<CompileCommandEntry> {
                    using T = std::decay_t<decltype(out)>;

                    if constexpr (std::same_as<T, Object>) {
                        return out.compileCommandEntry(compiler, build_dir, include_paths, compile_flags);
                    } else {
                        return std::nullopt;
                    }
                },
                value_
            );
        }
};

class Task;
class BuildGroup;
class Build;

// A single task: owns the Output it produces (an Object, Binary, or Library), plus its
// place in the dependency DAG. Set once the task is registered (see
// BuildGroup::addTask). execute() reaches build_dir through group_ -> Build rather than
// taking it as a parameter, since nothing here owns it.
class Task {
    private:
        Output                    output_;
        BuildGroup*                group_ = nullptr;
        std::vector<Task*>        children_;
        std::vector<Task*>        parents_;
        std::atomic<i32>          parent_count_;
        std::optional<bool>       needs_rebuild_;

    public:
        Task(Output output) : output_(std::move(output)), parent_count_(0) {}

        Task& depends_on(Task& dependency) {
            dependency.children_.push_back(this);
            parents_.push_back(&dependency);
            ++parent_count_;
            return *this;
        }

        void setGroup(BuildGroup& group) { group_ = &group; }

        // True on success. CommandOutput's exit_code was previously discarded here —
        // this is the one place that actually inspects it. Defined out-of-line, after
        // BuildGroup, since BuildGroup isn't a complete type yet here.
        bool execute();

        std::vector<std::filesystem::path> listDependencies(const std::filesystem::path& build_dir);

        std::optional<CompileCommandEntry> compileCommandEntry();

        // Memoized: own staleness OR any parent's (recursive). Safe to call from
        // multiple worker threads without locking the cache — a task is only ever
        // dispatched after every parent's needsRebuild()+complete() has already run on
        // its own thread, and complete()'s atomic decrement of parent_count_ is what
        // publishes that thread's writes (including needs_rebuild_) before this task's
        // parentCount() is observed as 0 and it gets picked up.
        bool needsRebuild();

        const std::filesystem::path& sourcePath() const { return output_.sourcePath(); }

        std::filesystem::path outputPath(const std::filesystem::path& build_dir) const { return output_.outputPath(build_dir); }

        bool isObject() const { return output_.isObject(); }

        const std::vector<Task*>& parents() const { return parents_; }

        i32 parentCount() const { return parent_count_.load(); }

        void complete() {
            for (Task* child : children_) {
                if (child->parent_count_.load() != 0) {
                    --child->parent_count_;
                }
            }
        }

        void print() const {
            std::ostringstream oss;
            oss << "Task " << sourcePath().filename().string() << ": parents=" << parent_count_.load() << ", children=[";
            for (usize i = 0; i < children_.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << children_[i]->sourcePath().filename().string();
            }
            oss << "]";
            RLOG(LL_DEBUG, oss.str());
        }

    private:
        // Walks parents() transitively, collecting each upstream task's compiled
        // output path, deduplicated for diamond dependencies. Read-only: the DAG edges
        // themselves are built once by depends_on(), not here. Only Object-backed
        // tasks contribute — a Command (or, transitively, another Binary/Library)
        // sitting somewhere in the ancestor chain has no real object file to hand the
        // linker.
        std::vector<std::filesystem::path> collectObjectFiles(const std::filesystem::path& build_dir) const {
            std::vector<std::filesystem::path> object_files;
            std::unordered_set<const Task*>    visited;

            std::function<void(const Task*)> collect = [&](const Task* task) {
                if (visited.contains(task)) {
                    return;
                }
                visited.insert(task);

                if (task->isObject()) {
                    object_files.push_back(task->outputPath(build_dir));
                }

                for (Task* parent : task->parents()) {
                    collect(parent);
                }
            };

            for (Task* parent : parents_) {
                collect(parent);
            }

            return object_files;
        }
};

// Concrete (not templated on Include/Link kinds — see __IncludeVariant/__LinkVariant):
// this is what lets Build own a homogeneous collection of these directly instead of
// needing a type-erased base class.
class BuildGroup {
    private:
        std::unordered_map<std::filesystem::path, std::unique_ptr<Task>> tasks_;
        std::vector<__IncludeVariant>                                    includes_;
        std::optional<std::string>                                       compiler_;
        std::vector<std::string>                                         compile_flags_;
        std::vector<std::string>                                         link_flags_;
        std::vector<__LinkVariant>                                       links_;

        // Set once the group is registered (see Build::addGroup).
        Build* build_ = nullptr;

    public:
        // No-op if the group already has its own compiler (see setCompiler) — only
        // fills in Build's default when one wasn't explicitly set.
        void setDefaultCompiler(const std::string& compiler) {
            if (!compiler_.has_value()) {
                compiler_ = compiler;
            }
        }

        void setCompiler(const std::string& compiler) { compiler_ = compiler; }

        void setBuild(Build& build) { build_ = &build; }

        // Defined out-of-line, after Build, since Build isn't a complete type yet here.
        const std::filesystem::path& buildDir() const;

        template <__IsInclude T>
        void addInclude(T include) { includes_.emplace_back(std::move(include)); }

        void addCompileFlag(const std::string& flag) { compile_flags_.push_back(flag); }

        void addLinkFlag(const std::string& flag) { link_flags_.push_back(flag); }

        template <__IsLink T>
        void addLink(T link) { links_.emplace_back(std::move(link)); }

        Task& addTask(Output output) {
            output.assignCommandName(*build_);

            Task&                  new_task = *(new Task(std::move(output)));
            new_task.setGroup(*this);
            std::filesystem::path key = new_task.sourcePath().stem();

            if (tasks_.contains(key)) {
                RLOG(LL_FATAL, "Task name collision: \"" + key.string() + "\" is already registered in this group — choose a different name");
            }

            tasks_[key] = std::unique_ptr<Task>(&new_task);
            return new_task;
        }

        Task& addTask(Output output, std::initializer_list<std::reference_wrapper<Task>> dependencies) {
            Task& new_task = addTask(std::move(output));
            for (Task& dependency : dependencies) {
                new_task.depends_on(dependency);
            }
            return new_task;
        }

        std::unordered_map<std::filesystem::path, std::unique_ptr<Task>>& tasks() { return tasks_; }

        std::vector<std::filesystem::path> includePaths(const std::filesystem::path& sym_links) {
            std::vector<std::filesystem::path> paths;
            for (auto& include : includes_) {
                paths.push_back(std::visit([&](const auto& inc) { return inc.path(sym_links); }, include));
            }
            return paths;
        }

        std::string& compiler() { return *compiler_; }

        const std::vector<std::string>& compileFlags() { return compile_flags_; }

        const std::vector<std::string>& linkFlags() { return link_flags_; }

        std::vector<std::string> linkables() {
            std::vector<std::string> result;
            for (auto& link : links_) {
                result.push_back(std::visit([](const auto& l) { return l.linkable(); }, link));
            }
            return result;
        }
};

class ThreadPool {
    public:
        static constexpr i32 DEFAULT_THREAD_COUNT = 4;

    private:
        Build*                    build_ = nullptr;
        std::vector<std::thread> threads_;
        std::queue<Task*>        work_queue_;
        std::mutex                mutex_;
        std::condition_variable   cv_;
        std::atomic<bool>         dispatch_complete_;

        // Defined out-of-line, after Build, since the body needs Build::recordCompileCommand.
        void workerLoop();

    public:
        ThreadPool() : dispatch_complete_(false) {}

        ~ThreadPool() { waitAll(); }

        void setBuild(Build& build) { build_ = &build; }

        // Threads aren't spawned at construction — ThreadPool is a Build member, built
        // before Build's own constructor body runs, before -j has even been parsed.
        // Called explicitly from Build::build(), once the thread count is known and
        // there's actually a DAG ready to dispatch.
        void start(i32 thread_count = DEFAULT_THREAD_COUNT) {
            for (i32 i = 0; i < thread_count; ++i) {
                threads_.emplace_back(&ThreadPool::workerLoop, this);
            }
        }

        void pushWork(Task* task) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                work_queue_.push(task);
            }
            cv_.notify_one();
        }

        void signalDispatchComplete() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                dispatch_complete_.store(true);
            }
            cv_.notify_all();
        }

        // Blocks until every already-dispatched task has actually run (not just been
        // queued) — signals workers there's no more work coming, then joins them.
        // joinable() guards against the destructor re-joining if this already ran.
        void waitAll() {
            signalDispatchComplete();
            for (auto& t : threads_) {
                if (t.joinable()) {
                    t.join();
                }
            }
        }
};

class Build {
    private:
        std::filesystem::path                                           build_dir_;
        std::string                                                     default_compiler_;
        // list, not vector: addGroup() returns a BuildGroup& that tasks then store a
        // pointer into (see Task::group_). A vector would dangle every such pointer the
        // moment a later addGroup() call triggered a reallocation; list never
        // reallocates, so those addresses stay stable for the Build's whole lifetime.
        std::list<BuildGroup>                                           groups_;
        ThreadPool                                                      thread_pool_;
        std::unordered_map<std::filesystem::path, CompileCommandEntry> compile_commands_;
        std::mutex                                                      compile_commands_mutex_;
        std::atomic<bool>                                               failed_ = false;
        // Not atomic: only ever touched from addTask() during single-threaded build
        // setup, before the thread pool has any work to race over.
        usize                                                            command_counter_ = 0;
        i32                                                              jobs_;

    public:
        Build(const std::filesystem::path& build_dir, const std::string& compiler, int argc, char** argv)
            : build_dir_(build_dir), default_compiler_(compiler) {
            selfRebuild(argc, argv);

            jobs_ = parseJobs(argc, argv);

            std::filesystem::create_directories(build_dir_);
            thread_pool_.setBuild(*this);
        }

        BuildGroup& addGroup() {
            groups_.emplace_back();
            BuildGroup& group = groups_.back();
            group.setDefaultCompiler(default_compiler_);
            group.setBuild(*this);
            return group;
        }

        const std::filesystem::path& buildDir() const { return build_dir_; }

        // Not what actually stops the other worker threads — RLOG(LL_FATAL, ...) does
        // that for free by calling exit() the instant any thread hits a failure, which
        // is process-wide. This just lets a thread skip starting new work in the narrow
        // window before that exit() call lands, and keeps two near-simultaneous
        // failures from racing to log a garbled fatal message.
        bool hasFailed() const { return failed_.load(); }

        void reportFailure() { failed_.store(true); }

        // 1-indexed: after N commands have been added, the Nth one is "cmd_N".
        usize nextCommandId() { return ++command_counter_; }

        // Recorded unconditionally by every worker before checking needsRebuild(), so
        // compile_commands.json stays complete even when most tasks are skipped on an
        // incremental build.
        void recordCompileCommand(CompileCommandEntry entry) {
            std::lock_guard<std::mutex> lock(compile_commands_mutex_);
            compile_commands_[entry.file] = std::move(entry);
        }

        void exportCompileCommands(const std::filesystem::path& path = "compile_commands.json") const {
            std::ofstream file(path);
            if (!file) {
                RLOG(LL_ERROR, "Failed to open " + path.string());
                return;
            }

            file << "[\n";

            usize i     = 0;
            usize total = compile_commands_.size();
            for (const auto& [key, entry] : compile_commands_) {
                file << "\t{\n";
                file << "\t\t\"directory\": \"" << entry.directory.string() << "\",\n";
                file << "\t\t\"command\": \"" << entry.command << "\",\n";
                file << "\t\t\"file\": \"" << entry.file.string() << "\"\n";
                file << "\t}";
                file << (++i < total ? ",\n" : "\n");
            }

            file << "]";
        }

        void print() {
            usize total = 0;
            for (auto& group : groups_) {
                total += group.tasks().size();
            }
            RLOG(LL_DEBUG, "Build: " + std::to_string(total) + " tasks");

            for (auto& group : groups_) {
                for (auto& [key, task] : group.tasks()) {
                    task->print();
                }
            }
        }

        void build() {
            buildDAG();
	    print();

            thread_pool_.start(jobs_);

            std::forward_list<Task*> pending = collectTasks();

            while (!pending.empty()) {
                auto prev = pending.before_begin();
                auto it   = pending.begin();

                while (it != pending.end()) {
                    Task* task = *it;

                    if (task->parentCount() == 0) {
                        thread_pool_.pushWork(task);
                        it = pending.erase_after(prev);
                    } else {
                        prev = it;
                        ++it;
                    }
                }
            }

            thread_pool_.waitAll();
            exportCompileCommands();
        }

    private:
        // Dedicated, hardcoded parse — separate from the generic defineArg()/parseArgs()
        // system, since -j is a build-tool built-in, not something a script opts into.
        // First occurrence wins; a missing/unparseable value falls back to the default
        // and logs an error rather than blocking the rest of the build.
        i32 parseJobs(int argc, char** argv) const {
            for (int i = 1; i < argc - 1; ++i) {
                if (std::string(argv[i]) == "-j") {
                    try {
                        return static_cast<i32>(std::stoi(argv[i + 1]));
                    } catch (...) {
                        RLOG(LL_ERROR, "Invalid value for -j: " + std::string(argv[i + 1]));
                        return ThreadPool::DEFAULT_THREAD_COUNT;
                    }
                }
            }
            return ThreadPool::DEFAULT_THREAD_COUNT;
        }

        // Compares this build script's own source (found via __BASE_FILE__ — the
        // actual top-level file passed to the compiler, whatever it's named, not a
        // hardcoded "build.cpp") and build.hpp itself (via __FILE__, the same trick
        // build.h used) against a fixed "build" binary name. If either is newer, or
        // "build" doesn't exist yet, recompiles, re-execs "./build" with the original
        // argv, and exits — so the rest of this (now-stale) process's build script
        // never runs, and the fresh process runs the whole thing once instead.
        void selfRebuild(int argc, char** argv) {
            std::filesystem::path source  = __BASE_FILE__;
            std::filesystem::path library = __FILE__;
            std::filesystem::path binary  = "build";

            bool missing = !std::filesystem::exists(binary);
            bool stale   = missing || std::filesystem::last_write_time(source) > std::filesystem::last_write_time(binary)
                         || std::filesystem::last_write_time(library) > std::filesystem::last_write_time(binary);

            if (!stale) {
                return;
            }

            if (missing) {
                RLOG(LL_WARN, "No ./" + binary.string() + " binary found in this directory — after this run, invoke ./" + binary.string()
                                  + " directly instead of recompiling by hand.");
            }

            RLOG(LL_INFO, "Rebuilding " + binary.string() + "...");

            Command compile_cmd({default_compiler_, "-std=c++23", source.string(), "-o", binary.string()});
            CommandOutput compile_result = compile_cmd.exec();
            if (compile_result.exit_code != 0) {
                RLOG(LL_FATAL, "Failed to rebuild " + binary.string() + ": " + compile_result.stderr_output);
            }

            Command run_cmd({"./" + binary.string()});
            for (int i = 1; i < argc; ++i) {
                run_cmd.push_back(std::string(argv[i]));
            }

            if (run_cmd.run() != 0) {
                RLOG(LL_FATAL, "Rebuilt build script failed");
            }

            std::exit(0);
        }

        std::forward_list<Task*> collectTasks() {
            std::forward_list<Task*> all;
            for (auto& group : groups_) {
                for (auto& [key, task] : group.tasks()) {
                    all.push_front(task.get());
                }
            }
            return all;
        }

        void buildDAG() {
            std::unordered_map<std::filesystem::path, Task*> combined;
            for (Task* task : collectTasks()) {
                combined[task->sourcePath().stem()] = task;
            }

            for (Task* task : collectTasks()) {
                auto deps = task->listDependencies(build_dir_);

                for (const auto& dep : deps) {
                    auto it = combined.find(dep.stem());
                    if (it == combined.end()) {
                        continue;
                    }

                    Task& other = *it->second;
                    if (&other == task) {
                        continue;
                    }

                    task->depends_on(other);
                }
            }
        }
};

inline const std::filesystem::path& BuildGroup::buildDir() const { return build_->buildDir(); }

inline void Output::assignCommandName(Build& build) {
    std::visit(
        [&](auto& out) {
            using T = std::decay_t<decltype(out)>;

            if constexpr (std::same_as<T, Command>) {
                out.setName("cmd_" + std::to_string(build.nextCommandId()));
            }
        },
        value_
    );
}

inline bool Task::execute() {
    std::filesystem::path build_dir = group_->buildDir();
    std::filesystem::path sym_links = build_dir / "sym_links";
    CommandOutput result = output_.execute(
        group_->compiler(), build_dir, group_->includePaths(sym_links), collectObjectFiles(build_dir),
        group_->compileFlags(), group_->linkFlags(), group_->linkables()
    );
    return result.exit_code == 0;
}

inline std::vector<std::filesystem::path> Task::listDependencies(const std::filesystem::path& build_dir) {
    std::filesystem::path sym_links = build_dir / "sym_links";
    return output_.listDependencies(group_->compiler(), group_->includePaths(sym_links), group_->compileFlags());
}

inline std::optional<CompileCommandEntry> Task::compileCommandEntry() {
    std::filesystem::path build_dir = group_->buildDir();
    std::filesystem::path sym_links = build_dir / "sym_links";
    return output_.compileCommandEntry(group_->compiler(), build_dir, group_->includePaths(sym_links), group_->compileFlags());
}

inline bool Task::needsRebuild() {
    if (needs_rebuild_.has_value()) {
        return *needs_rebuild_;
    }

    std::filesystem::path build_dir = group_->buildDir();
    bool                  stale     = output_.isStale(build_dir, collectObjectFiles(build_dir));

    if (!stale) {
        for (Task* parent : parents_) {
            if (parent->needsRebuild()) {
                stale = true;
                break;
            }
        }
    }

    needs_rebuild_ = stale;
    return stale;
}

inline void ThreadPool::workerLoop() {
    while (true) {
        Task* task = nullptr;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !work_queue_.empty() || dispatch_complete_.load(); });

            if (!work_queue_.empty()) {
                task = work_queue_.front();
                work_queue_.pop();
            }
        }

        if (task != nullptr) {
            if (!build_->hasFailed()) {
                if (auto entry = task->compileCommandEntry()) {
                    build_->recordCompileCommand(std::move(*entry));
                }
                if (task->needsRebuild() && !task->execute()) {
                    build_->reportFailure();
                    RLOG(LL_FATAL, "Build step failed: " + task->sourcePath().string());
                }
            }
            task->complete();
        } else if (dispatch_complete_.load()) {
            break;
        }
    }
}
