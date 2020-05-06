/*
 * SW - Build System and Package Manager
 * Copyright (C) 2016-2018 Egor Pugin
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <map>

#include "remote.h"
#include "version.h"
#include "yaml.h"

#include <sw/support/enums.h>
#include <sw/support/filesystem.h>

#include <primitives/http.h>
#include <primitives/sw/settings.h>

namespace sw
{

struct SW_MANAGER_API Settings
{
    // connection
    Remotes remotes{ get_default_remotes() };
    ProxySettings proxy;

    path storage_dir;

    // do not check for new cppan version
    bool disable_update_checks = false;
    bool can_update_packages_db = true;
    //bool verify_all = false;
    bool record_commands = false;
    bool record_commands_in_current_dir = false;

public:
    Settings();

    void load(const path &p, const SettingsType type);
    void load(const yaml &root, const SettingsType type);
    void save(const path &p) const;

    bool checkForUpdates() const;

private:
    void load_main(const yaml &root, const SettingsType type);

public:
    static Settings &get(SettingsType type);
    static Settings &get_system_settings();
    static Settings &get_user_settings();
    static Settings &get_local_settings();
    static void clear_local_settings();
};

}
