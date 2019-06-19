#include "channel_encryption.hpp"
#include "http_connection.h"
#include "lokid_key.h"
#include "rate_limiter.h"
#include "service_node.h"
#include "swarm.h"
#include "version.h"

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <sodium.h>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility> // for std::pair
#include <vector>

using namespace service_node;
namespace po = boost::program_options;
namespace logging = boost::log;

using LogLevelPair = std::pair<std::string, logging::trivial::severity_level>;
using LogLevelMap = std::vector<LogLevelPair>;
static const LogLevelMap logLevelMap{
    {"trace", logging::trivial::severity_level::trace},
    {"debug", logging::trivial::severity_level::debug},
    {"info", logging::trivial::severity_level::info},
    {"warning", logging::trivial::severity_level::warning},
    {"error", logging::trivial::severity_level::error},
    {"fatal", logging::trivial::severity_level::fatal},
};

static void print_usage(const po::options_description& desc, char* argv[]) {

    std::cerr << std::endl;
    std::cerr << "Usage: " << argv[0] << " <address> <port> [...]\n\n";

    desc.print(std::cerr);

    std::cerr << std::endl;
    std::cerr << "  Log Levels:\n";
    for (const auto& logLevel : logLevelMap) {
        std::cerr << "    " << logLevel.first << "\n";
    }
}

bool parseLogLevel(const std::string& input,
                   logging::trivial::severity_level& logLevel) {

    const auto it = std::find_if(
        logLevelMap.begin(), logLevelMap.end(),
        [&](const LogLevelPair& pair) { return pair.first == input; });
    if (it != logLevelMap.end()) {
        logLevel = it->second;
        return true;
    }
    return false;
}


static boost::optional<boost::filesystem::path> get_home_dir() {

    /// TODO: support default dir for Windows
#ifdef WIN32
    return boost::none;
#endif

    char* pszHome = getenv("HOME");
    if (pszHome == NULL || strlen(pszHome) == 0)
        return boost::none;

    return boost::filesystem::path(pszHome);
}

int main(int argc, char* argv[]) {
    try {
        // Check command line arguments.
        std::string lokid_key_path;

        namespace fs = boost::filesystem;

        const auto home_path = get_home_dir();
        const fs::path storage_path = home_path ? (*home_path / ".loki" / "storage") : ".";

        std::string db_location = storage_path.string();
        std::string log_location;
        std::string log_level_string("info");
        bool print_version = false;
        uint16_t lokid_rpc_port = 22023;

        po::options_description desc;
        // clang-format off
        desc.add_options()
            ("lokid-key", po::value(&lokid_key_path), "Path to the Service Node key file")
            ("db-location", po::value(&db_location),"Path to the Storage Server database location")
            ("output-log", po::value(&log_location), "Path to the log file")
            ("log-level", po::value(&log_level_string), "Log verbosity level, see Log Levels below for accepted values")
            ("version,v", po::bool_switch(&print_version), "Print the version of this binary")
            ("lokid-rpc-port", po::value(&lokid_rpc_port), "RPC port on which the local Loki daemon is listening");
        // clang-format on

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);

        if (argc < 2) {
            print_usage(desc, argv);
            return EXIT_FAILURE;
        }

        std::cout << "Loki Storage Server v" << STORAGE_SERVER_VERSION_STRING
                  << std::endl
                  << " git commit hash: " << STORAGE_SERVER_GIT_HASH_STRING
                  << std::endl
                  << " build time: " << STORAGE_SERVER_BUILD_TIME << std::endl;

        if (print_version) {
            return EXIT_SUCCESS;
        }

        if (argc < 3) {
            print_usage(desc, argv);
            return EXIT_FAILURE;
        }

        const auto port = static_cast<uint16_t>(std::atoi(argv[2]));
        std::string ip = argv[1];

        if (vm.count("output-log")) {

            // TODO: remove this line once confirmed that no one
            // is relying on this
            log_location += ".out";

            // Hacky, but I couldn't find a way to recover from
            // boost throwing on invalid file and apparently poisoning
            // the logging mechanism...
            std::ofstream input(log_location);

            if (input.is_open()) {
                input.close();
                logging::add_common_attributes();
                auto sink = logging::add_file_log(log_location);
                sink->set_formatter(
                    logging::expressions::stream
                    << logging::expressions::format_date_time<
                           boost::posix_time::ptime>("TimeStamp",
                                                     "[%Y-%m-%d %H:%M:%S:%f]")
                    << " [" << logging::trivial::severity << "]\t"
                    << logging::expressions::smessage);

                sink->locked_backend()->auto_flush(true);
                BOOST_LOG_TRIVIAL(info)
                    << "Outputting logs to " << log_location;
            } else {
                BOOST_LOG_TRIVIAL(error) << "Could not open " << log_location;
            }
        }

        logging::trivial::severity_level logLevel;
        if (!parseLogLevel(log_level_string, logLevel)) {
            BOOST_LOG_TRIVIAL(error)
                << "Incorrect log level" << log_level_string;
            print_usage(desc, argv);
            return EXIT_FAILURE;
        }

        // TODO: consider adding auto-flushing for logging
        logging::core::get()->set_filter(logging::trivial::severity >=
                                         logLevel);
        BOOST_LOG_TRIVIAL(info) << "Setting log level to " << log_level_string;

        if (vm.count("lokid-key")) {
            BOOST_LOG_TRIVIAL(info)
                << "Setting Lokid key path to " << lokid_key_path;
        }

        if (vm.count("db-location")) {
            BOOST_LOG_TRIVIAL(info)
                << "Setting database location to " << db_location;
        } else {
            BOOST_LOG_TRIVIAL(info)
                << "Using the default database location: " << db_location;
        }

        if (!boost::filesystem::exists(db_location)) {
            BOOST_LOG_TRIVIAL(info) << "Directory <" << db_location << "> does not exist, creating it...";
            fs::create_directories(db_location);
        }

        if (vm.count("lokid-rpc-port")) {
            BOOST_LOG_TRIVIAL(info)
                << "Setting lokid RPC port to " << lokid_rpc_port;
        }

        BOOST_LOG_TRIVIAL(info)
            << "Listening at address " << ip << " port " << port;

        boost::asio::io_context ioc{1};
        boost::asio::io_context worker_ioc{1};

        if (sodium_init() != 0) {
            BOOST_LOG_TRIVIAL(fatal) << "Could not initialize libsodium";
            return EXIT_FAILURE;
        }

        // ed25519 key
        const auto private_key = loki::parseLokidKey(lokid_key_path);
        const auto public_key = loki::calcPublicKey(private_key);

        // TODO: avoid conversion to vector
        const std::vector<uint8_t> priv(private_key.begin(), private_key.end());
        ChannelEncryption<std::string> channel_encryption(priv);

        loki::lokid_key_pair_t lokid_key_pair{private_key, public_key};

        loki::ServiceNode service_node(ioc, worker_ioc, port, lokid_key_pair,
                                       db_location, lokid_rpc_port);
        RateLimiter rate_limiter;

        /// Should run http server
        loki::http_server::run(ioc, ip, port, db_location, service_node,
                               channel_encryption, rate_limiter);

    } catch (const std::exception& e) {
        // It seems possible for logging to throw its own exception,
        // in which case it will be propagated to libc...
        BOOST_LOG_TRIVIAL(fatal) << "Exception caught in main: " << e.what();
        return EXIT_FAILURE;
    } catch (...) {
        BOOST_LOG_TRIVIAL(fatal) << "Unknown exception caught in main.";
        return EXIT_FAILURE;
    }
}
