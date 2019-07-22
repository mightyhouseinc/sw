// Copyright (C) 2017-2019 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "driver.h"

#include "build.h"
#include "module.h"

#include <sw/core/input.h>

#include <primitives/log.h>
DECLARE_STATIC_LOGGER(logger, "driver.cpp");

namespace sw
{

std::optional<path> findConfig(const path &dir, const FilesOrdered &fe_s)
{
    for (auto &fn : fe_s)
    {
        if (fs::exists(dir / fn))
            return dir / fn;
    }
    return {};
}

namespace driver::cpp
{

Driver::Driver()
{
    //build = std::make_unique<Build>(b.swctx, b, *this);
    module_storage = std::make_unique<ModuleStorage>();
}

Driver::~Driver()
{
    // do not clear modules on exception, because it may come from there
    // TODO: cleanup modules data first
    // copy exception here and pass further
    //if (std::uncaught_exceptions())
        //module_storage.release();
}

PackageId Driver::getPackageId() const
{
    return "org.sw.sw.driver.cpp-0.3.0";
}

bool Driver::canLoad(const Input &i) const
{
    switch (i.getType())
    {
    case InputType::SpecificationFile:
    {
        auto &fes = Build::getAvailableFrontendConfigFilenames();
        return std::find(fes.begin(), fes.end(), i.getPath().filename().u8string()) != fes.end();
    }
    case InputType::InlineSpecification:
        SW_UNIMPLEMENTED;
        break;
    case InputType::DirectorySpecificationFile:
        return !!findConfig(i.getPath(), Build::getAvailableFrontendConfigFilenames());
    case InputType::Directory:
        SW_UNIMPLEMENTED;
        break;
    default:
        SW_UNREACHABLE;
    }
    return false;
}

static String spec;

void Driver::load(SwBuild &b, const std::set<Input> &inputs)
{
    auto bb = new Build(b.swctx, b, *this);
    auto &build = *bb;

    PackageIdSet pkgsids;
    for (auto &i : inputs)
    {
        switch (i.getType())
        {
        case InputType::InstalledPackage:
        {
            pkgsids.insert(i.getPackageId());
            break;
        }
        case InputType::DirectorySpecificationFile:
        {
            auto p = *findConfig(i.getPath(), Build::getAvailableFrontendConfigFilenames());

            auto settings = i.getSettings();
            for (auto s : settings)
                s.erase("driver");

            build.DryRun = (*i.getSettings().begin())["driver"]["dry-run"] == "true";

            for (auto &[h, d] : (*i.getSettings().begin())["driver"]["source-dir-for-source"].getSettings())
                build.source_dirs_by_source[h] = d.getValue();

            spec = read_file(p);
            build.load_spec_file(p, settings);
            break;
        }
        default:
            SW_UNREACHABLE;
        }
    }

    if (!pkgsids.empty())
        build.load_packages(pkgsids);
}

/*void Driver::execute()
{
    build->execute();
}

bool Driver::prepareStep()
{
    return build->prepareStep();
}*/

String Driver::getSpecification() const
{
    return spec;
}

ChecksStorage &Driver::getChecksStorage(const String &config) const
{
    auto i = checksStorages.find(config);
    if (i == checksStorages.end())
    {
        auto [i, _] = checksStorages.emplace(config, std::make_unique<ChecksStorage>());
        return *i->second;
    }
    return *i->second;
}

ChecksStorage &Driver::getChecksStorage(const String &config, const path &fn) const
{
    auto i = checksStorages.find(config);
    if (i == checksStorages.end())
    {
        auto [i, _] = checksStorages.emplace(config, std::make_unique<ChecksStorage>());
        i->second->load(fn);
        return *i->second;
    }
    return *i->second;
}

ModuleStorage &Driver::getModuleStorage() const
{
    return *module_storage;
}

}

}
