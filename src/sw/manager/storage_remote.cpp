// Copyright (C) 2016-2019 Egor Pugin <egor.pugin@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "storage.h"

#include "database.h"
#include "settings.h"

#include <primitives/command.h>
#include <primitives/exceptions.h>
#include <primitives/lock.h>
#include <primitives/pack.h>
#include <sqlite3.h>
#include <sqlpp11/sqlite3/connection.h>

#include <fstream>

#include <primitives/log.h>
DECLARE_STATIC_LOGGER(logger, "storage");

#define PACKAGES_DB_REFRESH_TIME_MINUTES 15

#define PACKAGES_DB_SCHEMA_VERSION 4
#define PACKAGES_DB_SCHEMA_VERSION_FILE "schema.version"
#define PACKAGES_DB_VERSION_FILE "db.version"
#define PACKAGES_DB_DOWNLOAD_TIME_FILE "packages.time"

const String db_repo_name = "SoftwareNetwork/database";
const String db_repo_url = "https://github.com/" + db_repo_name;
const String db_master_url = db_repo_url + "/archive/master.zip";
const String db_version_url = "https://raw.githubusercontent.com/" + db_repo_name + "/master/" PACKAGES_DB_VERSION_FILE;

// save current time during crt startup
// it is used for detecting young packages
static TimePoint tstart;

bool gForceServerQuery;

static const String packages_db_name = "packages.db";

namespace sw
{

static int readPackagesDbSchemaVersion(const path &dir)
{
    auto p = dir / PACKAGES_DB_SCHEMA_VERSION_FILE;
    if (!fs::exists(p))
        return 0;
    return std::stoi(read_file(p));
}

static void writePackagesDbSchemaVersion(const path &dir)
{
    write_file(dir / PACKAGES_DB_SCHEMA_VERSION_FILE, std::to_string(PACKAGES_DB_SCHEMA_VERSION));
}

static int readPackagesDbVersion(const path &dir)
{
    auto p = dir / PACKAGES_DB_VERSION_FILE;
    if (!fs::exists(p))
        return 0;
    return std::stoi(read_file(p));
}

static void writePackagesDbVersion(const path &dir, int version)
{
    write_file(dir / PACKAGES_DB_VERSION_FILE, std::to_string(version));
}

RemoteStorage::RemoteStorage(LocalStorage &ls, const String &name, const path &db_dir)
    : StorageWithPackagesDatabase(name, db_dir), ls(ls)
{
    db_repo_dir = db_dir / name / "repository";

    static const auto db_loaded_var = "db_loaded";

    if (!pkgdb->getIntValue(db_loaded_var))
    {
        LOG_DEBUG(logger, "Packages database was not found");
        download();
        load();
        pkgdb->setIntValue(db_loaded_var, 1);
    }
    else
        updateDb();

    // at the end we always reopen packages db as read only
    pkgdb->open(true);
}

RemoteStorage::~RemoteStorage() = default;

int RemoteStorage::getHashSchemaVersion() const
{
    return 1;
}

int RemoteStorage::getHashPathFromHashSchemaVersion() const
{
    return 1;
}

std::unordered_map<UnresolvedPackage, Package>
RemoteStorage::resolve(const UnresolvedPackages &pkgs, UnresolvedPackages &unresolved_pkgs) const
{
    preInitFindDependencies();
    return StorageWithPackagesDatabase::resolve(pkgs, unresolved_pkgs);
}

void RemoteStorage::download() const
{
    LOG_INFO(logger, "Downloading database");

    fs::create_directories(db_repo_dir);

    auto download_archive = [this]()
    {
        auto fn = get_temp_filename();
        download_file(db_master_url, fn, 1_GB);
        auto unpack_dir = get_temp_filename();
        auto files = unpack_file(fn, unpack_dir);
        for (auto &f : files)
            fs::copy_file(f, db_repo_dir / f.filename(), fs::copy_options::overwrite_existing);
        fs::remove_all(unpack_dir);
        fs::remove(fn);
    };

    const String git = "git";
    if (!primitives::resolve_executable(git).empty())
    {
        auto git_init = [this, &git]() {
            primitives::Command::execute({git, "-C", db_repo_dir.string(), "init", "."});
            primitives::Command::execute({git, "-C", db_repo_dir.string(), "remote", "add", "github", db_repo_url});
            primitives::Command::execute({git, "-C", db_repo_dir.string(), "pull", "github", "master"});
        };

        try
        {
            if (!fs::exists(db_repo_dir / ".git"))
            {
                git_init();
            }
            else
            {
                std::error_code ec1, ec2;
                primitives::Command::execute({git, "-C", db_repo_dir.string(), "pull", "github", "master"}, ec1);
                primitives::Command::execute({git, "-C", db_repo_dir.string(), "reset", "--hard"}, ec2);
                if (ec1 || ec2)
                {
                    // can throw
                    fs::remove_all(db_repo_dir);
                    git_init();
                }
            }
        }
        catch (const std::exception &)
        {
            // cannot throw
            error_code ec;
            fs::remove_all(db_repo_dir, ec);

            download_archive();
        }
    }
    else
    {
        download_archive();
    }

    writeDownloadTime();
}

static std::istream &safe_getline(std::istream &i, String &s)
{
    std::getline(i, s);
    if (!s.empty() && s.back() == '\r')
        s.resize(s.size() - 1);
    return i;
}

void RemoteStorage::load() const
{
    /*auto &sdb = ls.getServiceDatabase();
    auto sver_old = sdb.getPackagesDbSchemaVersion();
    int sver = readPackagesDbSchemaVersion(db_repo_dir);
    if (sver && sver != PACKAGES_DB_SCHEMA_VERSION)
    {
    if (sver > PACKAGES_DB_SCHEMA_VERSION)
    throw SW_RUNTIME_ERROR("Client's packages db schema version is older than remote one. Please, upgrade the cppan client from site or via --self-upgrade");
    if (sver < PACKAGES_DB_SCHEMA_VERSION)
    throw SW_RUNTIME_ERROR("Client's packages db schema version is newer than remote one. Please, wait for server upgrade");
    }
    if (sver > sver_old)
    {
    // recreate(); ?
    sdb.setPackagesDbSchemaVersion(sver);
    }*/

    auto mdb = pkgdb->db->native_handle();
    sqlite3_stmt *stmt = nullptr;

    // load only known tables
    // alternative: read csv filenames by mask and load all
    // but we don't do this
    Strings data_tables;
    sqlite3 *db2;
    if (sqlite3_open_v2(pkgdb->fn.u8string().c_str(), &db2, SQLITE_OPEN_READONLY, 0) != SQLITE_OK)
        throw SW_RUNTIME_ERROR("cannot open db: " + pkgdb->fn.u8string());
    int rc = sqlite3_exec(db2, "select name from sqlite_master as tables where type='table' and name not like '/_%' ESCAPE '/';",
        [](void *o, int, char **cols, char **)
        {
            Strings &data_tables = *(Strings *)o;
            data_tables.push_back(cols[0]);
            return 0;
        }, &data_tables, 0);
    sqlite3_close(db2);
    if (rc != SQLITE_OK)
        throw SW_RUNTIME_ERROR("cannot query db for tables: " + pkgdb->fn.u8string());

    pkgdb->db->execute("PRAGMA foreign_keys = OFF;");
    pkgdb->db->execute("BEGIN;");

    auto split_csv_line = [](auto &s)
    {
        std::replace(s.begin(), s.end(), ',', '\0');
    };

    for (auto &td : data_tables)
    {
        pkgdb->db->execute("delete from " + td);

        auto fn = db_repo_dir / (td + ".csv");
        std::ifstream ifile(fn);
        if (!ifile)
            throw SW_RUNTIME_ERROR("Cannot open file " + fn.string() + " for reading");

        String s;
        int rc;

        // read first line
        safe_getline(ifile, s);
        split_csv_line(s);

        // read fields
        auto b = s.c_str();
        auto e = &s.back() + 1;
        Strings cols;
        cols.push_back(b);
        while (1)
        {
            if (b == e)
                break;
            if (*b++ == 0)
                cols.push_back(b);
        }
        auto n_cols = cols.size();

        // add only them
        String query = "insert into " + td + " (";
        for (auto &c : cols)
            query += c + ", ";
        query.resize(query.size() - 2);
        query += ") values (";
        for (size_t i = 0; i < n_cols; i++)
            query += "?, ";
        query.resize(query.size() - 2);
        query += ");";

        if (sqlite3_prepare_v2(mdb, query.c_str(), (int)query.size() + 1, &stmt, 0) != SQLITE_OK)
            throw SW_RUNTIME_ERROR(sqlite3_errmsg(mdb));

        while (safe_getline(ifile, s))
        {
            auto b = s.c_str();
            split_csv_line(s);

            for (int i = 1; i <= n_cols; i++)
            {
                if (*b)
                    sqlite3_bind_text(stmt, i, b, -1, SQLITE_TRANSIENT);
                else
                    sqlite3_bind_null(stmt, i);
                while (*b++);
            }

            rc = sqlite3_step(stmt);
            if (rc != SQLITE_DONE)
                throw SW_RUNTIME_ERROR("sqlite3_step() failed: "s + sqlite3_errmsg(mdb));
            rc = sqlite3_reset(stmt);
            if (rc != SQLITE_OK)
                throw SW_RUNTIME_ERROR("sqlite3_reset() failed: "s + sqlite3_errmsg(mdb));
        }

        if (sqlite3_finalize(stmt) != SQLITE_OK)
            throw SW_RUNTIME_ERROR("sqlite3_finalize() failed: "s + sqlite3_errmsg(mdb));
    }

    pkgdb->db->execute("COMMIT;");
    pkgdb->db->execute("PRAGMA foreign_keys = ON;");
}

void RemoteStorage::updateDb() const
{
    if (!gForceServerQuery)
    {
        if (!Settings::get_system_settings().can_update_packages_db || !isCurrentDbOld())
            return;
    }

    static int version_remote = []()
    {
        LOG_TRACE(logger, "Checking remote version");
        try
        {
            return std::stoi(download_file(db_version_url));
        }
        catch (std::exception &e)
        {
            LOG_DEBUG(logger, "Couldn't download db version file: " << e.what());
        }
        return 0;
    }();
    if (version_remote > readPackagesDbVersion(db_repo_dir))
    {
        // multiprocess aware
        single_process_job(pkgdb->fn.parent_path() / "db_update", [this] {
            download();
            load();
        });
    }
}

void RemoteStorage::preInitFindDependencies() const
{
    // !
    updateDb();

    // set current time
    tstart = getUtc();
}

void RemoteStorage::writeDownloadTime() const
{
    auto tp = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(tp);
    write_file(pkgdb->fn.parent_path() / PACKAGES_DB_DOWNLOAD_TIME_FILE, std::to_string(time));
}

TimePoint RemoteStorage::readDownloadTime() const
{
    auto fn = pkgdb->fn.parent_path() / PACKAGES_DB_DOWNLOAD_TIME_FILE;
    String ts = "0";
    if (fs::exists(fn))
        ts = read_file(fn);
    auto tp = std::chrono::system_clock::from_time_t(std::stoll(ts));
    return tp;
}

bool RemoteStorage::isCurrentDbOld() const
{
    auto tp_old = readDownloadTime();
    auto tp = std::chrono::system_clock::now();
    return (tp - tp_old) > std::chrono::minutes(PACKAGES_DB_REFRESH_TIME_MINUTES);
}

/*LocalPackage RemoteStorage::download(const PackageId &id) const
{
    ls.get(*this, id, StorageFileType::SourceArchive);
    return LocalPackage(ls, id);
}*/

LocalPackage RemoteStorage::install(const Package &id) const
{
    LocalPackage p(ls, id);
    if (ls.getPackagesDatabase().isPackageInstalled(id))
        return p;

    // actually we may want to remove only stamps, hashes etc.
    // but remove everything for now
    std::error_code ec;
    fs::remove_all(p.getDir(), ec);

    ls.get(*this, id, StorageFileType::SourceArchive);
    ls.getPackagesDatabase().installPackage(id);
    return p;
}

std::unique_ptr<vfs::File> RemoteStorage::getFile(const PackageId &id, StorageFileType t) const
{
    struct RemoteFileWithHashVerification : vfs::File
    {
        Strings urls;
        String hash;

        bool copy(const path &fn) const override
        {
            auto download_from_source = [&](const auto &url)
            {
                try
                {
                    LOG_TRACE(logger, "Downloading file: " << url);
                    download_file(url, fn);
                }
                catch (std::exception &e)
                {
                    LOG_TRACE(logger, "Downloading file: " << url << ", error: " << e.what());
                    return false;
                }
                return true;
            };

            for (auto &url : urls)
            {
                if (!download_from_source(url))
                    continue;
                auto sfh = get_strong_file_hash(fn, hash);
                if (sfh == hash)
                {
                    return true;
                }
                auto fh = get_file_hash(fn);
                if (fh == hash)
                {
                    return true;
                }
                LOG_TRACE(logger, "Downloaded file: " << url << " hash = " << sfh);
            }

            return false;
        }
    };

    switch (t)
    {
    case StorageFileType::SourceArchive:
    {
        auto provs = pkgdb->getDataSources();
        Package pkg(*this, id);
        auto rf = std::make_unique<RemoteFileWithHashVerification>();
        rf->hash = pkg.getData().hash;
        for (auto &p : provs)
            rf->urls.push_back(p.getUrl(pkg));
        return rf;
    }
    default:
        SW_UNIMPLEMENTED;
    }
}

}
