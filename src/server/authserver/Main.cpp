/*
 * This file is part of the WarheadCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/**
* @file main.cpp
* @brief Authentication Server main program
*
* This file contains the main program for the
* authentication server
*/

#include "AuthSocketMgr.h"
#include "Banner.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DatabaseLoader.h"
#include "DeadlineTimer.h"
#include "IoContext.h"
#include "IPLocation.h"
#include "GitRevision.h"
#include "MySQLThreading.h"
#include "ProcessPriority.h"
#include "RealmList.h"
#include "SecretMgr.h"
#include "SharedDefines.h"
#include "Util.h"
#include <boost/asio/signal_set.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem/operations.hpp>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#include <iostream>
#include <csignal>

using boost::asio::ip::tcp;
using namespace boost::program_options;
namespace fs = boost::filesystem;

#ifndef _WARHEAD_REALM_CONFIG
# define _WARHEAD_REALM_CONFIG  "authserver.conf"
#endif

#if WARHEAD_PLATFORM == WARHEAD_PLATFORM_WINDOWS
#include "ServiceWin32.h"
char serviceName[] = "authserver";
char serviceLongName[] = "WarheadCore auth service";
char serviceDescription[] = "WarheadCore World of Warcraft emulator auth service";
/*
* -1 - not in service mode
*  0 - stopped
*  1 - running
*  2 - paused
*/
int m_ServiceStatus = -1;

void ServiceStatusWatcher(std::weak_ptr<Warhead::Asio::DeadlineTimer> serviceStatusWatchTimerRef, std::weak_ptr<Warhead::Asio::IoContext> ioContextRef, boost::system::error_code const& error);
#endif

bool StartDB();
void StopDB();
void SignalHandler(std::weak_ptr<Warhead::Asio::IoContext> ioContextRef, boost::system::error_code const& error, int signalNumber);
void KeepDatabaseAliveHandler(std::weak_ptr<Warhead::Asio::DeadlineTimer> dbPingTimerRef, int32 dbPingInterval, boost::system::error_code const& error);
void BanExpiryHandler(std::weak_ptr<Warhead::Asio::DeadlineTimer> banExpiryCheckTimerRef, int32 banExpiryCheckInterval, boost::system::error_code const& error);
variables_map GetConsoleArguments(int argc, char** argv, fs::path& configFile, std::string& configService);

int main(int argc, char** argv)
{
    Warhead::Impl::CurrentServerProcessHolder::_type = SERVER_PROCESS_AUTHSERVER;
    signal(SIGABRT, &Warhead::AbortHandler);

    auto configFile = fs::path(sConfigMgr->GetConfigPath() + std::string(_WARHEAD_REALM_CONFIG));
    std::string configService;
    auto vm = GetConsoleArguments(argc, argv, configFile, configService);
    // exit if help or version is enabled
    if (vm.count("help") || vm.count("version"))
        return 0;

#if WARHEAD_PLATFORM == WARHEAD_PLATFORM_WINDOWS
    if (configService.compare("install") == 0)
        return WinServiceInstall() == true ? 0 : 1;
    else if (configService.compare("uninstall") == 0)
        return WinServiceUninstall() == true ? 0 : 1;
    else if (configService.compare("run") == 0)
        return WinServiceRun() ? 0 : 1;
#endif

    // Add file and args in config
    sConfigMgr->Configure(configFile.generic_string(), std::vector<std::string>(argv, argv + argc));

    if (!sConfigMgr->LoadAppConfigs())
        return 1;

    // Init all logs
    sLog->Initialize();

    Warhead::Banner::Show("authserver",
        [](char const* text)
        {
            LOG_INFO("server.authserver", "%s", text);
        },
        []()
        {
            LOG_INFO("server.authserver", "> Using configuration file:       %s", sConfigMgr->GetFilename().c_str());
            LOG_INFO("server.authserver", "> Using SSL version:              %s (library: %s)", OPENSSL_VERSION_TEXT, SSLeay_version(SSLEAY_VERSION));
            LOG_INFO("server.authserver", "> Using Boost version:            %i.%i.%i", BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100);
        }
    );

    // authserver PID file creation
    std::string pidFile = sConfigMgr->GetOption<std::string>("PidFile", "");
    if (!pidFile.empty())
    {
        if (uint32 pid = CreatePIDFile(pidFile))
            LOG_INFO("server.authserver", "Daemon PID: %u\n", pid);
        else
        {
            LOG_ERROR("server.authserver", "Cannot create PID file %s.\n", pidFile.c_str());
            return 1;
        }
    }

    // Initialize the database connection
    if (!StartDB())
        return 1;

    sSecretMgr->Initialize();

    // Load IP Location Database
    sIPLocation->Load();

    std::shared_ptr<void> dbHandle(nullptr, [](void*) { StopDB(); });

    std::shared_ptr<Warhead::Asio::IoContext> ioContext = std::make_shared<Warhead::Asio::IoContext>();

    // Get the list of realms for the server
    sRealmList->Initialize(*ioContext, sConfigMgr->GetOption<int32>("RealmsStateUpdateDelay", 20));

    std::shared_ptr<void> sRealmListHandle(nullptr, [](void*) { sRealmList->Close(); });

    if (sRealmList->GetRealms().empty())
    {
        LOG_ERROR("server.authserver", "No valid realms specified.");
        return 1;
    }

    // Start the listening port (acceptor) for auth connections
    int32 port = sConfigMgr->GetOption<int32>("RealmServerPort", 3724);
    if (port < 0 || port > 0xFFFF)
    {
        LOG_ERROR("server.authserver", "Specified port out of allowed range (1-65535)");
        return 1;
    }

    std::string bindIp = sConfigMgr->GetOption<std::string>("BindIP", "0.0.0.0");

    if (!sAuthSocketMgr.StartNetwork(*ioContext, bindIp, port))
    {
        LOG_ERROR("server.authserver", "Failed to initialize network");
        return 1;
    }

    std::shared_ptr<void> sAuthSocketMgrHandle(nullptr, [](void*) { sAuthSocketMgr.StopNetwork(); });

    // Set signal handlers
    boost::asio::signal_set signals(*ioContext, SIGINT, SIGTERM);
#if WARHEAD_PLATFORM == WARHEAD_PLATFORM_WINDOWS
    signals.add(SIGBREAK);
#endif
    signals.async_wait(std::bind(&SignalHandler, std::weak_ptr<Warhead::Asio::IoContext>(ioContext), std::placeholders::_1, std::placeholders::_2));

    // Set process priority according to configuration settings
    SetProcessPriority("server.authserver", sConfigMgr->GetOption<int32>(CONFIG_PROCESSOR_AFFINITY, 0), sConfigMgr->GetOption<bool>(CONFIG_HIGH_PRIORITY, false));

    // Enabled a timed callback for handling the database keep alive ping
    int32 dbPingInterval = sConfigMgr->GetOption<int32>("MaxPingTime", 30);
    std::shared_ptr<Warhead::Asio::DeadlineTimer> dbPingTimer = std::make_shared<Warhead::Asio::DeadlineTimer>(*ioContext);
    dbPingTimer->expires_from_now(boost::posix_time::minutes(dbPingInterval));
    dbPingTimer->async_wait(std::bind(&KeepDatabaseAliveHandler, std::weak_ptr<Warhead::Asio::DeadlineTimer>(dbPingTimer), dbPingInterval, std::placeholders::_1));

    int32 banExpiryCheckInterval = sConfigMgr->GetOption<int32>("BanExpiryCheckInterval", 60);
    std::shared_ptr<Warhead::Asio::DeadlineTimer> banExpiryCheckTimer = std::make_shared<Warhead::Asio::DeadlineTimer>(*ioContext);
    banExpiryCheckTimer->expires_from_now(boost::posix_time::seconds(banExpiryCheckInterval));
    banExpiryCheckTimer->async_wait(std::bind(&BanExpiryHandler, std::weak_ptr<Warhead::Asio::DeadlineTimer>(banExpiryCheckTimer), banExpiryCheckInterval, std::placeholders::_1));

#if WARHEAD_PLATFORM == WARHEAD_PLATFORM_WINDOWS
    std::shared_ptr<Warhead::Asio::DeadlineTimer> serviceStatusWatchTimer;
    if (m_ServiceStatus != -1)
    {
        serviceStatusWatchTimer = std::make_shared<Warhead::Asio::DeadlineTimer>(*ioContext);
        serviceStatusWatchTimer->expires_from_now(boost::posix_time::seconds(1));
        serviceStatusWatchTimer->async_wait(std::bind(&ServiceStatusWatcher,
            std::weak_ptr<Warhead::Asio::DeadlineTimer>(serviceStatusWatchTimer),
            std::weak_ptr<Warhead::Asio::IoContext>(ioContext),
            std::placeholders::_1));
    }
#endif

    // Start the io service worker loop
    ioContext->run();

    banExpiryCheckTimer->cancel();
    dbPingTimer->cancel();

    LOG_INFO("server.authserver", "Halting process...");

    signals.cancel();

    return 0;
}

/// Initialize connection to the database
bool StartDB()
{
    MySQL::Library_Init();

    // Load databases
    // NOTE: While authserver is singlethreaded you should keep synch_threads == 1.
    // Increasing it is just silly since only 1 will be used ever.
    DatabaseLoader loader("server.authserver", DatabaseLoader::DATABASE_NONE);
    loader
        .AddDatabase(LoginDatabase, "Login");

    if (!loader.Load())
        return false;

    LOG_INFO("server.authserver", "Started auth database connection pool.");
    LOG_INFO("server.authserver", "");

    return true;
}

/// Close the connection to the database
void StopDB()
{
    LoginDatabase.Close();
    MySQL::Library_End();
}

void SignalHandler(std::weak_ptr<Warhead::Asio::IoContext> ioContextRef, boost::system::error_code const& error, int /*signalNumber*/)
{
    if (!error)
        if (std::shared_ptr<Warhead::Asio::IoContext> ioContext = ioContextRef.lock())
            ioContext->stop();
}

void KeepDatabaseAliveHandler(std::weak_ptr<Warhead::Asio::DeadlineTimer> dbPingTimerRef, int32 dbPingInterval, boost::system::error_code const& error)
{
    if (!error)
    {
        if (std::shared_ptr<Warhead::Asio::DeadlineTimer> dbPingTimer = dbPingTimerRef.lock())
        {
            LOG_INFO("server.authserver", "Ping MySQL to keep connection alive");
            LoginDatabase.KeepAlive();

            dbPingTimer->expires_from_now(boost::posix_time::minutes(dbPingInterval));
            dbPingTimer->async_wait(std::bind(&KeepDatabaseAliveHandler, dbPingTimerRef, dbPingInterval, std::placeholders::_1));
        }
    }
}

void BanExpiryHandler(std::weak_ptr<Warhead::Asio::DeadlineTimer> banExpiryCheckTimerRef, int32 banExpiryCheckInterval, boost::system::error_code const& error)
{
    if (!error)
    {
        if (std::shared_ptr<Warhead::Asio::DeadlineTimer> banExpiryCheckTimer = banExpiryCheckTimerRef.lock())
        {
            LoginDatabase.Execute(LoginDatabase.GetPreparedStatement(LOGIN_DEL_EXPIRED_IP_BANS));
            LoginDatabase.Execute(LoginDatabase.GetPreparedStatement(LOGIN_UPD_EXPIRED_ACCOUNT_BANS));

            banExpiryCheckTimer->expires_from_now(boost::posix_time::seconds(banExpiryCheckInterval));
            banExpiryCheckTimer->async_wait(std::bind(&BanExpiryHandler, banExpiryCheckTimerRef, banExpiryCheckInterval, std::placeholders::_1));
        }
    }
}

#if WARHEAD_PLATFORM == WARHEAD_PLATFORM_WINDOWS
void ServiceStatusWatcher(std::weak_ptr<Warhead::Asio::DeadlineTimer> serviceStatusWatchTimerRef, std::weak_ptr<Warhead::Asio::IoContext> ioContextRef, boost::system::error_code const& error)
{
    if (!error)
    {
        if (std::shared_ptr<Warhead::Asio::IoContext> ioContext = ioContextRef.lock())
        {
            if (m_ServiceStatus == 0)
                ioContext->stop();
            else if (std::shared_ptr<Warhead::Asio::DeadlineTimer> serviceStatusWatchTimer = serviceStatusWatchTimerRef.lock())
            {
                serviceStatusWatchTimer->expires_from_now(boost::posix_time::seconds(1));
                serviceStatusWatchTimer->async_wait(std::bind(&ServiceStatusWatcher, serviceStatusWatchTimerRef, ioContextRef, std::placeholders::_1));
            }
        }
    }
}
#endif

variables_map GetConsoleArguments(int argc, char** argv, fs::path& configFile, std::string& configService)
{
    options_description all("Allowed options");
    all.add_options()
        ("help,h", "print usage message")
        ("version,v", "print version build info")
        ("config,c", value<fs::path>(&configFile)->default_value(fs::path(sConfigMgr->GetConfigPath() + std::string(_WARHEAD_REALM_CONFIG))),
                     "use <arg> as configuration file")
        ;
#if WARHEAD_PLATFORM == WARHEAD_PLATFORM_WINDOWS
    options_description win("Windows platform specific options");
    win.add_options()
        ("service,s", value<std::string>(&configService)->default_value(""), "Windows service options: [install | uninstall]")
        ;

    all.add(win);
#else
    (void)configService;
#endif
    variables_map variablesMap;
    try
    {
        store(command_line_parser(argc, argv).options(all).allow_unregistered().run(), variablesMap);
        notify(variablesMap);
    }
    catch (std::exception& e)
    {
        std::cerr << e.what() << "\n";
    }

    if (variablesMap.count("help"))
        std::cout << all << "\n";
    else if (variablesMap.count("version"))
        std::cout << GitRevision::GetFullVersion() << "\n";

    return variablesMap;
}
