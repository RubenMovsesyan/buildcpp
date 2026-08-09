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

        std::filesystem::path outputPath(const std::filesystem::path& build_dir) const {
            std::filesystem::path path = build_dir / source_path_;
            path.replace_extension(".o");
            return path;
        }

        CommandOutput compile(const std::string& compiler, const std::filesystem::path& build_dir, const std::vector<std::filesystem::path>& include_paths) {
            RLOG(LL_INFO, "Compiling: " + source_path_.string());
            output_path_ = outputPath(build_dir);

            std::filesystem::create_directories(output_path_.parent_path());

            Command cmd({compiler});
            for (const auto& include_path : include_paths) {
                cmd.push_back("-I" + include_path.string());
            }
            cmd.push_back("-c");
            cmd.push_back(source_path_.string());
            cmd.push_back("-o");
            cmd.push_back(output_path_.string());

            return cmd.exec();
        }

        std::vector<std::filesystem::path> listDependencies(const std::string& compiler, const std::vector<std::filesystem::path>& include_paths) {
            Command cmd({compiler});
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

        CommandOutput link(const std::string& compiler, const std::filesystem::path& build_dir, const std::vector<std::filesystem::path>& object_files) {
            std::filesystem::path output_path = path(build_dir);
            std::filesystem::create_directories(output_path.parent_path());

            Command cmd({compiler});
            for (const auto& obj : object_files) {
                cmd.push_back(obj.string());
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

        CommandOutput link(const std::string& compiler, const std::filesystem::path& build_dir, const std::vector<std::filesystem::path>& object_files) {
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
#if defined(__APPLE__)
            cmd.push_back("-dynamiclib");
#else
            cmd.push_back("-shared");
#endif
            for (const auto& obj : object_files) {
                cmd.push_back(obj.string());
            }
            cmd.push_back("-o");
            cmd.push_back(output_path.string());

            return cmd.exec();
        }
};

// Wraps whatever a task ultimately produces. Not slotted into Task yet — this is just
// the shape: once it is, Task will hold an Output in place of a bare Object, and
// object_files (this task's transitive parents' compiled outputs) will come from the
// DAG walk Task already does for LinkTask/LibraryTask today.
class Output {
    private:
        std::variant<Object, Binary, Library> value_;

    public:
        Output(Object object) : value_(std::move(object)) {}
        Output(Binary binary) : value_(std::move(binary)) {}
        Output(Library library) : value_(std::move(library)) {}

        CommandOutput execute(
            const std::string&                         compiler,
            const std::filesystem::path&                build_dir,
            const std::vector<std::filesystem::path>&   include_paths,
            const std::vector<std::filesystem::path>&   object_files
        ) {
            return std::visit(
                [&](auto& out) -> CommandOutput {
                    using T = std::decay_t<decltype(out)>;

                    if constexpr (std::same_as<T, Object>) {
                        return out.compile(compiler, build_dir, include_paths);
                    } else {
                        return out.link(compiler, build_dir, object_files);
                    }
                },
                value_
            );
        }

        std::vector<std::filesystem::path> listDependencies(const std::string& compiler, const std::vector<std::filesystem::path>& include_paths) {
            return std::visit(
                [&](auto& out) -> std::vector<std::filesystem::path> {
                    using T = std::decay_t<decltype(out)>;

                    if constexpr (std::same_as<T, Object>) {
                        return out.listDependencies(compiler, include_paths);
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
};

class Task;
class Build;

// Not just Object&: a group's includes_ tuple type varies per BuildGroup<Includes...>
// instantiation, so this is the only generically-typed handle a task can hold onto its
// owning group with. The group now owns the compiler (see BuildGroup), so these no
// longer take one as a parameter.
class __BuildGroupBase {
    private:
        // Set once the group is registered (see Build::addGroup). Non-virtual: unlike
        // tasks()/includePaths()/compiler(), this doesn't vary per BuildGroup<Includes...>
        // instantiation, so every group can just share the same storage here.
        Build* build_ = nullptr;

    public:
        virtual ~__BuildGroupBase() = default;

        virtual std::unordered_map<std::filesystem::path, std::unique_ptr<Task>>& tasks() = 0;
        virtual std::vector<std::filesystem::path> includePaths(const std::filesystem::path& sym_links) = 0;
        virtual std::string& compiler() = 0;

        // No-op if the group already has its own compiler (see BuildGroup's two
        // constructors) — only fills in Build's default when one wasn't specified.
        virtual void setDefaultCompiler(const std::string& compiler) = 0;

        void setBuild(Build& build) { build_ = &build; }

        // Defined out-of-line, after Build, since Build isn't a complete type yet here.
        const std::filesystem::path& buildDir() const;
};

// A single task: owns the Output it produces (an Object, Binary, or Library), plus its
// place in the dependency DAG. Set once the task is registered (see
// BuildGroup::addTask). execute() reaches build_dir through group_ -> Build rather than
// taking it as a parameter, since nothing here owns it.
class Task {
    private:
        Output                    output_;
        __BuildGroupBase*         group_ = nullptr;
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

        void setGroup(__BuildGroupBase& group) { group_ = &group; }

        void execute() {
            std::filesystem::path build_dir = group_->buildDir();
            std::filesystem::path sym_links = build_dir / "sym_links";
            output_.execute(group_->compiler(), build_dir, group_->includePaths(sym_links), collectObjectFiles(build_dir));
        }

        std::vector<std::filesystem::path> listDependencies(const std::filesystem::path& build_dir) {
            std::filesystem::path sym_links = build_dir / "sym_links";
            return output_.listDependencies(group_->compiler(), group_->includePaths(sym_links));
        }

        // Memoized: own staleness OR any parent's (recursive). Safe to call from
        // multiple worker threads without locking the cache — a task is only ever
        // dispatched after every parent's needsRebuild()+complete() has already run on
        // its own thread, and complete()'s atomic decrement of parent_count_ is what
        // publishes that thread's writes (including needs_rebuild_) before this task's
        // parentCount() is observed as 0 and it gets picked up.
        bool needsRebuild() {
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

        const std::filesystem::path& sourcePath() const { return output_.sourcePath(); }

        std::filesystem::path outputPath(const std::filesystem::path& build_dir) const { return output_.outputPath(build_dir); }

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
        // themselves are built once by depends_on(), not here.
        std::vector<std::filesystem::path> collectObjectFiles(const std::filesystem::path& build_dir) const {
            std::vector<std::filesystem::path> object_files;
            std::unordered_set<const Task*>    visited;

            std::function<void(const Task*)> collect = [&](const Task* task) {
                if (visited.contains(task)) {
                    return;
                }
                visited.insert(task);

                object_files.push_back(task->outputPath(build_dir));

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

template <__IsInclude... Includes>
class BuildGroup : public __BuildGroupBase {
    private:
        std::unordered_map<std::filesystem::path, std::unique_ptr<Task>> tasks_;
        std::tuple<Includes...>                                          includes_;
        std::optional<std::string>                                       compiler_;

    public:
        BuildGroup(Includes... includes) : includes_(includes...) {}

        BuildGroup(const std::string& compiler, Includes... includes) : includes_(includes...), compiler_(compiler) {}

        void setDefaultCompiler(const std::string& compiler) override {
            if (!compiler_.has_value()) {
                compiler_ = compiler;
            }
        }

        Task& addTask(Output output) {
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

        std::unordered_map<std::filesystem::path, std::unique_ptr<Task>>& tasks() override { return tasks_; }

        std::vector<std::filesystem::path> includePaths(const std::filesystem::path& sym_links) override {
            std::vector<std::filesystem::path> paths;
            std::apply([&](const auto&... incs) { (paths.push_back(incs.path(sym_links)), ...); }, includes_);
            return paths;
        }

        std::string& compiler() override { return *compiler_; }
};

class ThreadPool {
    private:
        static constexpr i32 THREAD_COUNT = 4;

        std::vector<std::thread> threads_;
        std::queue<Task*>        work_queue_;
        std::mutex                mutex_;
        std::condition_variable   cv_;
        std::atomic<bool>         dispatch_complete_;

        void workerLoop() {
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
                    if (task->needsRebuild()) {
                        task->execute();
                    }
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
};

class Build {
    private:
        std::filesystem::path          build_dir_;
        std::string                    default_compiler_;
        std::vector<__BuildGroupBase*> groups_;
        ThreadPool                     thread_pool_;

    public:
        Build(const std::filesystem::path& build_dir, const std::string& compiler)
            : build_dir_(build_dir), default_compiler_(compiler) {
            std::filesystem::create_directories(build_dir_);
        }

        Build(const std::filesystem::path& build_dir, const std::string& compiler, std::initializer_list<__BuildGroupBase*> groups)
            : Build(build_dir, compiler) {
            for (__BuildGroupBase* group : groups) {
                addGroup(group);
            }
        }

        void addGroup(__BuildGroupBase* group) {
            group->setDefaultCompiler(default_compiler_);
            group->setBuild(*this);
            groups_.push_back(group);
        }

        const std::filesystem::path& buildDir() const { return build_dir_; }

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

        void build() {
            buildDAG();
	    print();

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
        }

    private:
        std::forward_list<Task*> collectTasks() {
            std::forward_list<Task*> all;
            for (auto* group : groups_) {
                for (auto& [key, task] : group->tasks()) {
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

inline const std::filesystem::path& __BuildGroupBase::buildDir() const { return build_->buildDir(); }
