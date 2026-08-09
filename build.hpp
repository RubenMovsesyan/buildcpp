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
#include <initializer_list>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

struct CommandOutput {
        i32         exit_code;
        std::string stdout_output;
        std::string stderr_output;
};

class Command {
    private:
        std::vector<std::string> command_chain_;

    public:
        Command() = default;
        Command(std::initializer_list<std::string> command_chain) : command_chain_(command_chain) {}

        template <typename T>
        void push_back(T&& arg) { command_chain_.emplace_back(std::forward<T>(arg)); }

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

class Object {
    private:
        std::filesystem::path source_path_;
        std::filesystem::path output_path_;

    public:
        Object(const std::filesystem::path& source_path) : source_path_(source_path) {}

        const std::filesystem::path& sourcePath() const { return source_path_; }

        template <__IsInclude... Includes>
        CommandOutput compile(std::string& compiler, const std::filesystem::path& build_dir, const std::filesystem::path& sym_links, const Includes&... includes) {
            output_path_ = build_dir / source_path_;
            output_path_.replace_extension(".o");

            std::filesystem::create_directories(output_path_.parent_path());

            Command cmd({compiler});
            (cmd.push_back("-I" + includes.path(sym_links).string()), ...);
            cmd.push_back("-c");
            cmd.push_back(source_path_.string());
            cmd.push_back("-o");
            cmd.push_back(output_path_.string());

            return cmd.exec();
        }

        template <__IsInclude... Includes>
        std::vector<std::filesystem::path> listDependencies(std::string& compiler, const std::filesystem::path& sym_links, const Includes&... includes) {
            Command cmd({compiler});
            (cmd.push_back("-I" + includes.path(sym_links).string()), ...);
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

class BuildTask {
    private:
        Object                  task_;
        std::vector<BuildTask*> children_;
        std::atomic<i32>        parent_count_;

    public:
        BuildTask(Object task) : task_(std::move(task)), parent_count_(0) {}

        // Heap-allocated, not by value: std::atomic member blocks move/copy, and
        // children_ pointers need a fixed address anyway.
        static BuildTask& create(Object task) {
            return *(new BuildTask(std::move(task)));
        }

        BuildTask& depends_on(BuildTask& dependency) {
            dependency.children_.push_back(this);
            ++parent_count_;
            return *this;
        }

        Object& object() { return task_; }

        const std::filesystem::path& sourcePath() const { return task_.sourcePath(); }

        i32 parentCount() const { return parent_count_.load(); }

        void complete() {
            for (BuildTask* child : children_) {
                if (child->parent_count_.load() != 0) {
                    --child->parent_count_;
                }
            }
        }

        void print() const {
            std::ostringstream oss;
            oss << "BuildTask " << sourcePath().filename().string() << ": parents=" << parent_count_.load() << ", children=[";
            for (usize i = 0; i < children_.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << children_[i]->sourcePath().filename().string();
            }
            oss << "]";
            RLOG(LL_DEBUG, oss.str());
        }
};

class __BuildGroupBase {
    public:
        virtual ~__BuildGroupBase() = default;

        virtual std::unordered_map<std::filesystem::path, std::unique_ptr<BuildTask>>& tasks() = 0;
        virtual std::vector<std::filesystem::path> listDependencies(Object& obj, std::string& compiler, const std::filesystem::path& sym_links) = 0;
};

template <__IsInclude... Includes>
class BuildGroup : public __BuildGroupBase {
    private:
        std::unordered_map<std::filesystem::path, std::unique_ptr<BuildTask>> tasks_;
        std::tuple<Includes...>                                               includes_;

    public:
        BuildGroup(Includes... includes) : includes_(includes...) {}

        BuildTask& addTask(BuildTask& task) {
            std::filesystem::path key = task.object().sourcePath().stem();
            tasks_[key] = std::unique_ptr<BuildTask>(&task);
            return task;
        }

        std::unordered_map<std::filesystem::path, std::unique_ptr<BuildTask>>& tasks() override { return tasks_; }

        std::vector<std::filesystem::path> listDependencies(Object& obj, std::string& compiler, const std::filesystem::path& sym_links) override {
            return std::apply(
                [&](const auto&... incs) { return obj.listDependencies(compiler, sym_links, incs...); }, includes_
            );
        }
};

class ThreadPool {
    private:
        static constexpr i32 THREAD_COUNT = 4;

        std::vector<std::thread> threads_;
        std::queue<BuildTask*>   work_queue_;
        std::mutex                mutex_;
        std::condition_variable   cv_;
        std::atomic<bool>         dispatch_complete_;

        void workerLoop() {
            while (true) {
                BuildTask* task = nullptr;

                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return !work_queue_.empty() || dispatch_complete_.load(); });

                    if (!work_queue_.empty()) {
                        task = work_queue_.front();
                        work_queue_.pop();
                    }
                }

                if (task != nullptr) {
                    task->complete();
                } else if (dispatch_complete_.load()) {
                    break;
                }
            }
        }

    public:
        ThreadPool() : dispatch_complete_(false) {
            for (i32 i = 0; i < THREAD_COUNT; ++i) {
                threads_.emplace_back(&ThreadPool::workerLoop, this);
            }
        }

        ~ThreadPool() {
            signalDispatchComplete();
            for (auto& t : threads_) {
                if (t.joinable()) {
                    t.join();
                }
            }
        }

        void pushWork(BuildTask* task) {
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
};

class Build {
    private:
        std::filesystem::path          build_dir_;
        std::vector<__BuildGroupBase*> groups_;
        ThreadPool                     thread_pool_;

    public:
        Build(const std::filesystem::path& build_dir) : build_dir_(build_dir) {
            std::filesystem::create_directories(build_dir_);
        }

        Build(const std::filesystem::path& build_dir, std::initializer_list<__BuildGroupBase*> groups) : Build(build_dir) {
            groups_ = groups;
        }

        void addGroup(__BuildGroupBase* group) {
            groups_.push_back(group);
        }

        void print() const {
            usize total = 0;
            for (auto* group : groups_) {
                total += group->tasks().size();
            }
            RLOG(LL_DEBUG, "Build: " + std::to_string(total) + " tasks");

            for (auto* group : groups_) {
                for (auto& [key, task] : group->tasks()) {
                    task->print();
                }
            }
        }

        void build(std::string& compiler, const std::filesystem::path& sym_links) {
            buildDAG(compiler, sym_links);

            std::forward_list<BuildTask*> pending = collectTasks();

            while (!pending.empty()) {
                auto prev = pending.before_begin();
                auto it   = pending.begin();

                while (it != pending.end()) {
                    BuildTask* task = *it;

                    if (task->parentCount() == 0) {
                        thread_pool_.pushWork(task);
                        it = pending.erase_after(prev);
                    } else {
                        prev = it;
                        ++it;
                    }
                }
            }
        }

    private:
        std::forward_list<BuildTask*> collectTasks() {
            std::forward_list<BuildTask*> all;
            for (auto* group : groups_) {
                for (auto& [key, task] : group->tasks()) {
                    all.push_front(task.get());
                }
            }
            return all;
        }

        void buildDAG(std::string& compiler, const std::filesystem::path& sym_links) {
            std::unordered_map<std::filesystem::path, BuildTask*> combined;
            for (BuildTask* task : collectTasks()) {
                combined[task->sourcePath().stem()] = task;
            }

            for (auto* group : groups_) {
                for (auto& [key, task] : group->tasks()) {
                    auto deps = group->listDependencies(task->object(), compiler, sym_links);

                    for (const auto& dep : deps) {
                        auto it = combined.find(dep.stem());
                        if (it == combined.end()) {
                            continue;
                        }

                        BuildTask& other = *it->second;
                        if (&other == task.get()) {
                            continue;
                        }

                        task->depends_on(other);
                    }
                }
            }
        }
};
