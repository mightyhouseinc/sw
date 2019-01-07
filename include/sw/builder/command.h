// Copyright (C) 2017-2018 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <node.h>

#include <primitives/command.h>
#include <primitives/executor.h>

#include <condition_variable>
#include <mutex>

struct BinaryContext;

namespace sw
{

struct FileStorage;
struct Program;

template <class T>
struct CommandData
{
    std::unordered_set<std::shared_ptr<T>> dependencies;

    std::atomic_size_t dependencies_left = 0;
    std::unordered_set<std::shared_ptr<T>> dependendent_commands;

    std::atomic_size_t *current_command = nullptr;
    std::atomic_size_t *total_commands = nullptr;

    virtual ~CommandData() = default;

    virtual void execute() = 0;
    virtual void prepare() = 0;
    //virtual String getName() const = 0;

    void clear()
    {
        dependendent_commands.clear();
        dependencies.clear();
    }
};

struct SW_BUILDER_API ResourcePool
{
    int n = -1; // unlimited
    std::condition_variable cv;
    std::mutex m;

    void lock()
    {
        if (n == -1)
            return;
        std::unique_lock lk(m);
        cv.wait(lk, [this] { return n > 0; });
        --n;
    }

    void unlock()
    {
        if (n == -1)
            return;
        std::unique_lock lk(m);
        ++n;
        lk.unlock();
        cv.notify_one();
    }
};

namespace builder
{

#pragma warning(push)
#pragma warning(disable:4275) // warning C4275: non dll-interface struct 'primitives::Command' used as base for dll-interface struct 'sw::builder::Command'

struct SW_BUILDER_API Command : Node, std::enable_shared_from_this<Command>,
    CommandData<::sw::builder::Command>, primitives::Command // hide?
{
#pragma warning(pop)

    using Base = primitives::Command;
    using Clock = std::chrono::high_resolution_clock;

    FileStorage *fs = nullptr;

    String name;
    String name_short;

    Files inputs;
    // byproducts
    // used only to clean files and pre-create dirs
    Files intermediate;
    // if some commands accept pairs of args, and specific outputs depend on specific inputs
    // C I1 O1 I2 O2
    // then split that command!
    Files outputs;

    fs::file_time_type mtime;
    bool use_response_files = false;
    bool remove_outputs_before_execution = false; // was true
    bool protect_args_with_quotes = true;
    bool silent = false;
    bool always = false;
    // used when command may not update outputs based on some factors
    bool record_inputs_mtime = false;
    int strict_order = 0; // used to execute this before other commands
    ResourcePool *pool = nullptr;

    Clock::time_point t_begin;
    Clock::time_point t_end;

    enum
    {
        MU_FALSE    = 0,
        MU_TRUE     = 1,
        MU_ALWAYS   = 2,
    };
    int maybe_unused = 0;

    Command();
    Command(::sw::FileStorage &fs);
    virtual ~Command();

    void prepare() override;
    void execute() override;
    void execute(std::error_code &ec) override;
    void clean() const;
    bool isExecuted() const { return pid != -1 || executed_; }

    //String getName() const override { return getName(false); }
    String getName(bool short_name = false) const;
    path getProgram() const override;

    virtual bool isOutdated() const;
    bool needsResponseFile() const;

    void setProgram(const path &p);
    void setProgram(std::shared_ptr<Program> p);
    void addInput(const path &p);
    void addInput(const Files &p);
    void addIntermediate(const path &p);
    void addIntermediate(const Files &p);
    void addOutput(const path &p);
    void addOutput(const Files &p);
    path redirectStdin(const path &p);
    path redirectStdout(const path &p);
    path redirectStderr(const path &p);
    size_t getHash() const;
    void updateCommandTime() const;
    void addPathDirectory(const path &p);
    Files getGeneratedDirs() const; // used by generators

    void addInputOutputDeps();

    bool lessDuringExecution(const Command &rhs) const;

    //void load(BinaryContext &bctx);
    //void save(BinaryContext &bctx);

    void onBeforeRun() override;
    void onEnd() override;

    String getResponseFilename() const;
    virtual String getResponseFileContents(bool showIncludes = false) const;

    Strings &getArgs() override;

protected:
    bool prepared = false;
    bool executed_ = false;

    static String escape_cmd_arg(String);

private:
    mutable size_t hash = 0;
    Strings rsp_args;

    virtual void execute1(std::error_code *ec = nullptr);
    virtual size_t getHash1() const;

    void postProcess(bool ok = true);
    virtual void postProcess1(bool ok) {}

    bool beforeCommand();
    void afterCommand();
    virtual bool isTimeChanged() const;
    void printLog() const;
    size_t getHashAndSave() const;
};

} // namespace bulder

using Commands = std::unordered_set<std::shared_ptr<builder::Command>>;

template struct SW_BUILDER_API CommandData<builder::Command>;

#define SW_INTERNAL_INIT_COMMAND(name, target) \
    name->fs = (target).getSolution()->fs;     \
    name->addPathDirectory((target).getOutputDir() / (target).getConfig())

#define SW_MAKE_CUSTOM_COMMAND(type, name, target, ...) \
    auto name = std::make_shared<type>(__VA_ARGS__);    \
    SW_INTERNAL_INIT_COMMAND(name, target)

#ifdef _MSC_VER
#define SW_MAKE_CUSTOM_COMMAND_AND_ADD(type, name, target, ...) \
    SW_MAKE_CUSTOM_COMMAND(type, name, target, __VA_ARGS__);    \
    (target).Storage.push_back(name)
#else
#define SW_MAKE_CUSTOM_COMMAND_AND_ADD(type, name, target, ...) \
    SW_MAKE_CUSTOM_COMMAND(type, name, target, ## __VA_ARGS__);    \
    (target).Storage.push_back(name)
#endif

#define SW_MAKE_COMMAND(name, target) \
    SW_MAKE_CUSTOM_COMMAND(Command, name, target)
#define SW_MAKE_COMMAND_AND_ADD(name, target) \
    SW_MAKE_CUSTOM_COMMAND_AND_ADD(Command, name, target)

#define _SW_MAKE_EXECUTE_COMMAND(name, target) \
    SW_MAKE_CUSTOM_COMMAND(ExecuteCommand, name, target, __FILE__, __LINE__)
#define _SW_MAKE_EXECUTE_COMMAND_AND_ADD(name, target) \
    SW_MAKE_CUSTOM_COMMAND_AND_ADD(ExecuteCommand, name, target, __FILE__, __LINE__)

} // namespace sw

namespace std
{

template<> struct hash<sw::builder::Command>
{
    size_t operator()(const sw::builder::Command &c) const
    {
        return c.getHash();
    }
};

}
