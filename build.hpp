#pragma once

#include <atomic>
#include <charconv>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <print>
#include <queue>
#include <sstream>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <vector>

constexpr const char* DEFAULT_COMPILER = "clang++";

class Command {
    private:
        std::vector<std::string> command_chain_;
        std::filesystem::path execution_directory_;

    public:
        Command(std::initializer_list<std::string> command_chain)
            : command_chain_(command_chain),
              execution_directory_(std::filesystem::current_path()) {}

        Command(std::initializer_list<std::string> command_chain, std::filesystem::path exec_dir)
            : command_chain_(command_chain), execution_directory_(exec_dir) {}

        Command(const Command& other)
            : command_chain_(other.command_chain_),
              execution_directory_(other.execution_directory_) {}

        Command& operator=(const Command& other) {
            if (this != &other) {
                command_chain_ = other.command_chain_;
                execution_directory_ = other.execution_directory_;
            }

            return *this;
        }

        Command() : execution_directory_(std::filesystem::current_path()) {}

        ~Command() {}

        const std::vector<std::string>& command_chain() const {
            return command_chain_;
        }

        void push_back(std::string& chain) { command_chain_.push_back(chain); }

        void push_back(std::string&& chain) { command_chain_.push_back(chain); }

        void print() {
            for (const auto& chain : command_chain_) {
                std::print("{} ", chain);
            }

            std::println("");
        }

        inline std::string string() {
            std::string command = "";

            for (size_t i = 0; i < command_chain_.size(); ++i) {
                if (i > 0) {
                    command += " ";
                }

                command += command_chain_[i];
            }

            return command;
        }

        int exec() {
            std::filesystem::path original_dir = std::filesystem::current_path();
            std::filesystem::current_path(execution_directory_);

            std::string command = string();

            int result = system(command.c_str());

            std::filesystem::current_path(original_dir);

            return WEXITSTATUS(result);
        }

        std::string exec_and_capture() {
            std::filesystem::path original_dir = std::filesystem::current_path();
            std::filesystem::current_path(execution_directory_);

            std::string command = string();

            command += " 2>&1"; // Redirect to stdout

            FILE* pipe = popen(command.c_str(), "r");
            if (!pipe) {
                std::filesystem::current_path(original_dir);
                return "";
            }

            std::string result;
            char buffer[128];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                result += buffer;
            }

            pclose(pipe);
            std::filesystem::current_path(original_dir);
            return result;
        }
};

class CompileCommand {
    private:
        Command cmd_;
        std::filesystem::path directory_;
        std::filesystem::path file_;

    public:
        // Copy command to be able to use it later
        CompileCommand(Command cmd, std::filesystem::path file)
            : directory_(std::filesystem::current_path()), cmd_(cmd), file_(file) {}
        ~CompileCommand() {}

        void add_to_file(std::ofstream& file) {
            file << "\t{\n";

            file << "\t\t\"directory\": \"" << directory_.string() << "\",\n";
            file << "\t\t\"command\": \"" << cmd_.string() << "\",\n";
            file << "\t\t\"file\": \"" << file_.string() << "\"\n";

            file << "\t}" /* << (i < (compile_commands.size() - 1) ? ",\n" : "\n") */;
        }
};

class Include {
    protected:
        std::filesystem::path include_path_;

    public:
        Include(const std::string& path) : include_path_(path) {}
        ~Include() {}

        virtual std::string path(std::filesystem::path& sym_links) {
            return std::filesystem::relative(include_path_, std::filesystem::current_path())
                .string();
        }
};

class SymbolicInclude : public Include {
    protected:
        std::string symbolic_dir_;

    public:
        SymbolicInclude(const std::string& path, const std::string& symbolic_dir)
            : Include(path), symbolic_dir_(symbolic_dir) {}

        virtual std::string path(std::filesystem::path& sym_links) override {
            Command cmd({
                "ln",
                "-sfn",
                std::filesystem::absolute(include_path_).string(),
                sym_links.string() + std::string("/") + symbolic_dir_,
            });

            cmd.print();
            cmd.exec();

            std::filesystem::path sym_path = sym_links;

            return std::filesystem::relative(sym_path, std::filesystem::current_path())
                .string();
        }
};

class Object {
    private:
        std::filesystem::path source_path_;
        std::string file_name_;
        std::string file_name_not_ext_;

        // Helper functions
        std::vector<std::string> split(const std::string& str) {
            std::vector<std::string> tokens;
            std::istringstream ss(str);
            std::string token;

            while (ss >> token) {
                tokens.push_back(token);
            }

            return tokens;
        }

    public:
        Object(const std::string& path) : source_path_(path) {
            file_name_ = source_path_.filename();
            file_name_not_ext_ = file_name_.substr(0, file_name_.find_last_of("."));
        }

        ~Object() {}

        std::string path() const { return source_path_.string(); }

        std::pair<std::string, std::vector<std::string>>
        list_all_dependencies(std::string& compiler, std::string& flags, std::string& includes) {
            Command cmd({compiler, flags, includes, "-MM", source_path_.string()});
            std::string output = cmd.exec_and_capture();
            output.erase(std::remove_if(output.begin(), output.end(), [](char c) { return c == '\\' || c == '\n'; }), output.end());

            std::string object_file = output.substr(0, output.find_first_of(":"));
            std::string rest = output.substr(output.find_first_of(":") + 1);
            std::vector<std::string> dependencies = split(rest);

            return {object_file, dependencies};
        }

        std::array<std::string, 4> build_command(std::filesystem::path& build_dir) {
            std::filesystem::path relative_path = std::filesystem::relative(
                source_path_, std::filesystem::current_path()
            );
            auto path_parts = relative_path;
            auto iter = path_parts.begin();
            if (iter != path_parts.end()) {
                ++iter; // Skip the first directory
            }

            std::filesystem::path new_path = build_dir;
            for (auto it = iter; it != path_parts.end(); ++it) {
                new_path /= *it;
            }

            std::filesystem::create_directories(new_path.parent_path());
            new_path.replace_filename(file_name_not_ext_.append(".o"));

            return {"-c", relative_path.string(), "-o", new_path.string()};
        }

        void compile(std::string& compiler, std::string& flags, std::string& includes, std::filesystem::path& build_dir, std::vector<std::string>& object_files, std::mutex& object_files_mutex, std::vector<CompileCommand>& compile_commands, std::mutex& compile_commands_mutex) {
            std::pair<std::string, std::vector<std::string>> self_and_deps =
                list_all_dependencies(compiler, flags, includes);

            std::vector<std::filesystem::path> deps_paths;
            for (const auto& dep : self_and_deps.second) {
                deps_paths.push_back(std::filesystem::current_path() / dep);
            }

            auto build_commands = build_command(build_dir);

            // Get the last modification time of the object file
            // std::filesystem::path object_path = build_dir / self_and_deps.first;
            std::filesystem::path object_path = build_commands[3];

            Command cmd({compiler, flags, includes});
            for (auto& command_arg : build_commands) {
                cmd.push_back(command_arg);
            }

            compile_commands_mutex.lock();
            compile_commands.emplace_back(cmd, path());
            compile_commands_mutex.unlock();

            bool needs_rebuild = true;
            if (std::filesystem::exists(object_path)) {
                auto object_time = std::filesystem::last_write_time(object_path);
                needs_rebuild = false;

                for (const auto& dep_path : deps_paths) {
                    if (std::filesystem::exists(dep_path)) {
                        // Add a second buffer in case the file was saved right away
                        auto dep_time = std::filesystem::last_write_time(dep_path) + std::chrono::seconds(1);
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

            if (!needs_rebuild) {
                object_files_mutex.lock();
                object_files.push_back(std::filesystem::relative(object_path).string());
                object_files_mutex.unlock();
                return;
            }

            object_files_mutex.lock();
            object_files.emplace_back(cmd.command_chain().back());
            object_files_mutex.unlock();

            cmd.print();
            int ret_code = cmd.exec();
            if (ret_code != 0) {
                std::println("Object file build failed");
                exit(1);
            }
        }
};

class Link {
    protected:
        std::string dep_name_;

    public:
        Link(const std::string& dep_name) : dep_name_(dep_name) {}
        ~Link() {}

        virtual std::string linkable() { return std::string("-l") + dep_name_; }
};

class PathLink : public Link {
    private:
        std::string dir_name_;
        std::optional<std::filesystem::path> direct_path_;

    public:
        PathLink(const std::string& dep_name, const std::string& dir_name)
            : Link(dep_name), dir_name_(dir_name), direct_path_(std::nullopt) {}
        PathLink(const std::filesystem::path path)
            : Link(""), dir_name_(), direct_path_(path) {}

        std::string linkable() override {
            if (direct_path_) {
                return (*direct_path_).string();
            }

            return std::string("-L") + dir_name_ + std::string(" -l") + dep_name_;
        }
};

// MacOs only
class Framework : public Link {
    public:
        Framework(const std::string& dep_name) : Link(dep_name) {}

        std::string linkable() override {
#if defined(__APPLE__)
            return std::string("-framework ") + dep_name_;
#else
            return "";
#endif
        }
};

enum class Os { Invalid,
                MacOS,
                Linux,
                Windows };

enum class Mode { Release,
                  Debug };

struct Builtin {
        Os os;
        Mode mode;
};

class Build {
    public:
        // Builtin system variables
        Builtin builtin;

    private:
        // This is where the project will build
        std::filesystem::path build_dir_;
        // This is where any symbolic includes will go
        std::filesystem::path sym_link_dir_;

        std::string compiler_;

        struct BuildStep {
                // Commands that get run before the compilation of the build step
                std::vector<Command*> pre_step_commands;
                // These are all the include directories (symbolic and non-symbolic)
                std::vector<Include*> includes;
                // These are all the object files to compile into the build
                std::vector<Object*> objects;
                // These are all the libraries to link to
                std::vector<Link*> links;
                // Flags to use during compilation
                std::vector<std::string> compilation_flags;
                // Flags to use during linking
                std::vector<std::string> linking_flags;
        };

        std::vector<BuildStep> build_steps_;

        struct BuildJob {
                std::mutex mutex;
                // std::atomic<bool> job_started;

                std::atomic<bool> all_jobs_queued;
                std::atomic<bool> all_jobs_complete;

                // std::queue<Command> commands;
                std::queue<Object*> objects;
        };
        // Thread pool to use based on -j argument (dispatch compilations round robin
        // style) std::deque<std::pair<std::mutex, std::queue<Command>>>
        // thread_queues_;
        std::deque<BuildJob*> thread_queues_;
        size_t jobs_ = 1;

        bool skip_compile_commands;

        void export_compile_commands(std::vector<CompileCommand>& compile_commands) {
            if (skip_compile_commands) {
                return;
            }

            std::ofstream compile_commands_file;

            compile_commands_file.open("compile_commands.json");

            if (compile_commands_file.is_open()) {
                compile_commands_file << "[\n";

                for (size_t i = 0; i < compile_commands.size(); i++) {
                    compile_commands[i].add_to_file(compile_commands_file);
                    compile_commands_file
                        << (i < (compile_commands.size() - 1) ? ",\n" : "\n");
                }

                compile_commands_file << "]\n";
                compile_commands_file.close();
            } else {
                exit(1);
            }
        }

        void init_common() {
            sym_link_dir_ = build_dir_ / "sym_links";
            std::filesystem::create_directories(sym_link_dir_);
            build_steps_.push_back(BuildStep{});
            skip_compile_commands = false;

            builtin = Builtin{
                .os = Os::Invalid,
                .mode = Mode::Release // REVIEW: Should the default be release mode?
            };

            // Set up which Os we are on
#if defined(__linux__)
            builtin.os = Os::Linux;
#elif defined(__APPLE__)
            builtin.os = Os::MacOS;
#elif defined(__WIN32)
            builtin.os = Os::Windows;
#elif defined(__WIN64)
            builtin.os = Os::Windows;
#endif
        }

        void rebuildYourself(int argc, char** argv) {
            std::filesystem::path build_cpp = "build.cpp";
            std::filesystem::path build_hpp = "build.hpp";
#ifdef __WIN32
            std::filesystem::path build_exe = "build.exe";
#else
            std::filesystem::path build_exe = "build";
#endif

            if (!std::filesystem::exists(build_cpp) || !std::filesystem::exists(build_hpp)) {
                std::println("Error: build.cpp or build.hpp not found");
                return;
            }

            auto build_cpp_time = std::filesystem::last_write_time(build_cpp);
            auto build_hpp_time = std::filesystem::last_write_time(build_hpp);

            auto rebuild_func = [this, argc, argv]() {
                std::println("Rebuilding build system...");
                Command cmd({compiler_, "-std=c++23", "build.cpp", "-o", "build"});
                cmd.print();
                int result = cmd.exec();

                if (result != 0) {
                    std::println("Build system rebuild failed");
                    exit(1);
                }

                Command run_cmd({"./build"});
                for (int i = 1; i < argc; i++) {
                    run_cmd.push_back(argv[i]);
                }
                run_cmd.exec();
                exit(0);
            };

            // Check if build executable exists and compare times
            if (std::filesystem::exists(build_exe)) {
                auto build_exe_time = std::filesystem::last_write_time(build_exe);

                // If either source file is newer than the executable, rebuild
                if (build_cpp_time > build_exe_time || build_hpp_time > build_exe_time) {
                    rebuild_func();
                }
            } else {
                // No executable exists, need to build
                rebuild_func();
            }
        }

        void argParse(int argc, char** argv) {
            std::vector<std::string> args;
            for (int i = 0; i < argc; i++) {
                args.push_back(std::string(argv[i]));
            }

            // TODO: Check for opposite flags
            for (size_t i = 0; i < args.size(); i++) {
                const auto& arg = args[i];
                if (arg == "-Debug") {
                    builtin.mode = Mode::Debug;
                } else if (arg == "-Release") {
                    builtin.mode = Mode::Release;
                }

                if (arg == "-j") {
                    i++;

                    auto [ptr, ec] = std::from_chars(args[i].data(), args[i].data() + args[i].size(), jobs_);

                    if (ec == std::errc::invalid_argument) {
                        std::println("Invalid Argument for -j flag: \"{}\"", ptr);
                        exit(1);
                    } else if (ec == std::errc::result_out_of_range) {
                        std::println("Argument for -j flag out of range of int: \"{}\"", ptr);
                        exit(1);
                    }
                }
            }
        }

    public:
        // The Build always starts with 1 build step
        Build(const std::string& directory_name, int argc, char** argv)
            : build_dir_(directory_name), compiler_(DEFAULT_COMPILER) {
            init_common();
            argParse(argc, argv);
            rebuildYourself(argc, argv);
        }

        Build(const std::string& directory_name, const std::string& compiler, int argc, char** argv)
            : build_dir_(directory_name), compiler_(compiler) {
            init_common();
            argParse(argc, argv);
            rebuildYourself(argc, argv);
        }

        // delete the command, objects, includes, and links
        ~Build() {
            for (const auto& step : build_steps_) {
                for (Command* cmd : step.pre_step_commands) {
                    delete cmd;
                }

                for (Object* obj : step.objects) {
                    delete obj;
                }

                for (Include* inc : step.includes) {
                    delete inc;
                }

                for (Link* lnk : step.links) {
                    delete lnk;
                }
            }

            for (size_t i = 0; i < jobs_; i++) {
                BuildJob* job = thread_queues_[i];
                delete job;
            }
        }

        void skipCompileCommands() {
            skip_compile_commands = true;
        }

        /*
         * Usage: add_prebuild_command(new Command(...))
         */
        void addPrebuildCommand(Command* command) {
            build_steps_.back().pre_step_commands.push_back(command);
        }

        /*
         * Usage: add_object(new Object(...))
         */
        void addObject(Object* object) {
            build_steps_.back().objects.push_back(object);
        }

        /*
         * Usage: add_include(new Include(...))
         */
        void addInclude(Include* include) {
            build_steps_.back().includes.push_back(include);
        }

        /*
         * Usage: add_link(new Link(...))
         */
        void addLink(Link* link) { build_steps_.back().links.push_back(link); }

        void addCompilationFlag(std::string&& flag) {
            build_steps_.back().compilation_flags.push_back(flag);
        }

        void addLinkingFlag(std::string&& flag) {
            build_steps_.back().linking_flags.push_back(flag);
        }

        void step() { build_steps_.push_back({}); }

        void build() {
            for (size_t i = 0; i < jobs_; i++) {
                thread_queues_.emplace_back(new BuildJob);
            }

            std::vector<CompileCommand> compile_commands;
            std::mutex compile_commands_mutex;

            for (const auto& step : build_steps_) {
                // Run any prebuild commands (these will be run not multithreaded)
                for (const auto& command : step.pre_step_commands) {
                    command->print();
                    command->exec();
                }

                // Create the build step include paths
                std::vector<std::string> includes;
                for (const auto& include : step.includes) {
                    includes.push_back("-I");
                    includes.push_back(include->path(sym_link_dir_));
                }

                std::vector<std::string> object_files;
                std::mutex object_files_mutex;

                // Create jobs_ number of threads that will wait for commands to fill
                // their queue and then execute those commands
                std::atomic<bool> build_started = false;
                std::atomic<bool> build_complete = false;

                std::vector<std::thread> threads;

                for (size_t i = 0; i < jobs_; i++) {
                    thread_queues_[i]->all_jobs_queued.store(false, std::memory_order_seq_cst);
                    thread_queues_[i]->all_jobs_complete.store(false, std::memory_order_seq_cst);

                    threads.emplace_back([this, i, &includes, &step,
                                          &object_files, &object_files_mutex, &compile_commands, &compile_commands_mutex]() {
                        size_t job_number = i;
                        bool queue_is_empty = thread_queues_[job_number]->objects.empty();
                        bool all_jobs_queued = thread_queues_[job_number]->all_jobs_queued.load(std::memory_order_seq_cst);

                        while (!queue_is_empty || !all_jobs_queued) {
                            if (!queue_is_empty) {
                                thread_queues_[job_number]->mutex.lock();
                                Object* object = thread_queues_[job_number]->objects.front();
                                thread_queues_[job_number]->objects.pop();
                                thread_queues_[job_number]->mutex.unlock();

                                std::string includes_string = "";
                                for (const auto& include_command : includes) {
                                    includes_string += include_command + " ";
                                }

                                std::string compilation_flags_string = "";
                                for (const auto& flag : step.compilation_flags) {
                                    compilation_flags_string += flag + " ";
                                }

                                object->compile(compiler_, compilation_flags_string, includes_string, build_dir_, object_files, object_files_mutex, compile_commands, compile_commands_mutex);
                            }

                            queue_is_empty = thread_queues_[job_number]->objects.empty();
                            all_jobs_queued = thread_queues_[job_number]->all_jobs_queued.load(std::memory_order_seq_cst);
                        }

                        thread_queues_[job_number]->all_jobs_complete.store(true, std::memory_order_seq_cst);
                    });
                }

                // Compile the objects
                size_t job_index_ = 0;
                for (const auto& object : step.objects) {
                    thread_queues_[job_index_]->mutex.lock();
                    thread_queues_[job_index_]->objects.push(object);
                    thread_queues_[job_index_]->mutex.unlock();

                    job_index_ = (job_index_ + 1) % jobs_;
                }

                // After all the current job steps have been queued then let each thread know that
                // there will be no more work
                for (size_t i = 0; i < jobs_; i++) {
                    thread_queues_[i]->all_jobs_queued.store(true, std::memory_order_seq_cst);
                }

                for (auto& t : threads) {
                    t.join();
                }

                // Run the linking step of the build step before continuing to the next
                // build step
                Command linking_cmd({compiler_});

                // If there are no linking flags then just use the default -std=c++23
                if (step.linking_flags.empty()) {
                    linking_cmd.push_back("-std=c++23");
                } else {
                    // Copy the flag to the command
                    for (auto flag : step.linking_flags) {
                        linking_cmd.push_back(flag);
                    }
                }

                linking_cmd.push_back("-o");
                linking_cmd.push_back(build_dir_.string() + "/main");

                for (auto& object_file : object_files) {
                    linking_cmd.push_back(object_file);
                }

                for (auto& link : step.links) {
                    linking_cmd.push_back(link->linkable());
                }

                linking_cmd.print();
                linking_cmd.exec();
            }

            export_compile_commands(compile_commands);
        }
};
