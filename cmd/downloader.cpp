/*
   Copyright 2021-2022 The Silkworm Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include <iostream>
#include <string>
#include <thread>

#include <CLI/CLI.hpp>

#include <silkworm/buildinfo.h>
#include <silkworm/common/directories.hpp>
#include <silkworm/common/log.hpp>
#include <silkworm/common/settings.hpp>
#include <silkworm/downloader/internals/body_sequence.hpp>
#include <silkworm/downloader/internals/header_retrieval.hpp>
#include <silkworm/downloader/stage_bodies.hpp>
#include <silkworm/downloader/stage_headers.hpp>

#include "common.hpp"

using namespace silkworm;

// stage-loop, forwarding phase
using LastStage = size_t;
template <size_t N>
std::tuple<Stage::Result, LastStage> forward(std::array<Stage*, N> stages, bool first_sync) {
    using Status = Stage::Result;
    Stage::Result result;

    for (size_t i = 0; i < N; ++i) {
        result = stages[i]->forward(first_sync);
        if (result.status == Status::UnwindNeeded) {
            return {result, i};
        }
    }
    return {result, N - 1};
}

// stage-loop, unwinding phase
template <size_t N>
Stage::Result unwind(std::array<Stage*, N> stages, BlockNum unwind_point, Hash bad_block, LastStage last_stage) {
    using Status = Stage::Result;
    Stage::Result result;

    for (size_t i = last_stage; i <= 0; --i) {  // reverse loop
        result = stages[i]->unwind_to(unwind_point, bad_block);
        if (result.status == Status::Error) {
            break;
        }
    }

    return result;
}

// Main
int main(int argc, char* argv[]) {
    using std::string, std::cout, std::cerr, std::optional, std::to_string;
    using namespace std::chrono;

    // Default values
    CLI::App app{"Downloader. Connect to p2p sentry and start header/body downloading process (stages 1 and 2)"};
    int return_value = 0;

    try {
        NodeSettings node_settings{};
        node_settings.sentry_api_addr = "127.0.0.1:9091";

        log::Settings log_settings;
        log_settings.log_threads = true;
        log_settings.log_file = "downloader.log";
        log_settings.log_verbosity = log::Level::kInfo;
        log_settings.log_thousands_sep = '\'';

        // test & measurement only parameters [to remove]
        BodySequence::kMaxBlocksPerMessage = 128;
        BodySequence::kPerPeerMaxOutstandingRequests = 4;
        int requestDeadlineSeconds = 30;     // BodySequence::kRequestDeadline = std::chrono::seconds(30);
        int noPeerDelayMilliseconds = 1000;  // BodySequence::kNoPeerDelay = std::chrono::milliseconds(1000)

        app.add_option("--max_blocks_per_req", BodySequence::kMaxBlocksPerMessage,
                       "Max number of blocks requested to peers in a single request")
            ->capture_default_str();
        app.add_option("--max_requests_per_peer", BodySequence::kPerPeerMaxOutstandingRequests,
                       "Max number of pending request made to each peer")
            ->capture_default_str();
        app.add_option("--request_deadline_s", requestDeadlineSeconds,
                       "Time (secs) after which a response is considered lost and will be re-tried")
            ->capture_default_str();
        app.add_option("--no_peer_delay_ms", noPeerDelayMilliseconds,
                       "Time (msecs) to wait before making a new request when no peer accepted the last")
            ->capture_default_str();

        BodySequence::kRequestDeadline = std::chrono::seconds(requestDeadlineSeconds);
        BodySequence::kNoPeerDelay = std::chrono::milliseconds(noPeerDelayMilliseconds);
        // test & measurement only parameters end

        // Command line parsing
        cmd::parse_silkworm_command_line(app, argc, argv, log_settings, node_settings);

        log::init(log_settings);
        log::set_thread_name("stage-loop    ");

        // Output BuildInfo
        auto build_info{silkworm_get_buildinfo()};
        log::Message(
            "SILKWORM DOWNLOADER",
            {"version", std::string(build_info->git_branch) + std::string(build_info->project_version), "build",
             std::string(build_info->system_name) + "-" + std::string(build_info->system_processor) + " " +
                 std::string(build_info->build_type),
             "compiler", std::string(build_info->compiler_id) + " " + std::string(build_info->compiler_version)});

        log::Message("BlockExchange parameter",
                     {"--max_blocks_per_req", to_string(BodySequence::kMaxBlocksPerMessage)});
        log::Message("BlockExchange parameter",
                     {"--max_requests_per_peer", to_string(BodySequence::kPerPeerMaxOutstandingRequests)});
        log::Message("BlockExchange parameter", {"--request_deadline_s", to_string(requestDeadlineSeconds)});
        log::Message("BlockExchange parameter", {"--no_peer_delay_ms", to_string(noPeerDelayMilliseconds)});

        // Prepare database
        cmd::run_preflight_checklist(node_settings);

        // EIP-2124 based chain identity scheme (networkId + genesis + forks)
        ChainIdentity chain_identity;
        if (node_settings.chain_config->chain_id == kMainnetConfig.chain_id) {
            chain_identity = kMainnetIdentity;
        } else if (node_settings.chain_config->chain_id == kRopstenConfig.chain_id) {
            chain_identity = kRopstenIdentity;
        } else if (node_settings.chain_config->chain_id == kSepoliaConfig.chain_id) {
            chain_identity = kSepoliaIdentity;
        } else {
            // for Rinkeby & Goerli we have not implemented the consensus engine yet
            throw std::logic_error("Chain id=" + std::to_string(node_settings.chain_config->chain_id) +
                                   " not supported");
        }

        log::Message("Chain/db status", {"chain-id", to_string(chain_identity.config.chain_id)});
        log::Message("Chain/db status", {"genesis_hash", to_hex(chain_identity.genesis_hash)});
        log::Message("Chain/db status", {"hard-forks", to_string(chain_identity.distinct_fork_numbers().size())});

        // Database access
        Db db{node_settings.chaindata_env_config};

        // Node current status
        HeaderRetrieval headers(Db::ReadOnlyAccess{db});
        auto [head_hash, head_td] = headers.head_hash_and_total_difficulty();
        auto head_height = headers.head_height();

        log::Message("Chain/db status", {"head hash", head_hash.to_hex()});
        log::Message("Chain/db status", {"head td", intx::to_string(head_td)});
        log::Message("Chain/db status", {"head height", to_string(head_height)});

        // Sentry client - connects to sentry
        SentryClient sentry{node_settings.sentry_api_addr};
        sentry.set_status(head_hash, head_td, chain_identity);
        sentry.hand_shake();
        auto message_receiving = std::thread([&sentry]() { sentry.execution_loop(); });
        auto stats_receiving = std::thread([&sentry]() { sentry.stats_receiving_loop(); });

        // BlockExchange - download headers and bodies from remote peers using the sentry
        BlockExchange block_exchange{sentry, Db::ReadOnlyAccess{db}, chain_identity};
        auto block_downloading = std::thread([&block_exchange]() { block_exchange.execution_loop(); });

        // Stage1 - Header downloader - example code
        bool first_sync = true;  // = starting up silkworm
        HeadersStage header_stage{Db::ReadWriteAccess{db}, block_exchange};
        BodiesStage body_stage{Db::ReadWriteAccess{db}, block_exchange};

        // Sample stage loop with 2 stages
        std::array<Stage*, 2> stages = {&header_stage, &body_stage};

        using Status = Stage::Result;
        Stage::Result result{Status::Unspecified};
        size_t last_stage = 0;

        do {
            std::tie(result, last_stage) = forward(stages, first_sync);

            if (result.status == Status::UnwindNeeded) {
                result = unwind(stages, *result.unwind_point, *result.bad_block, last_stage);
            }

            first_sync = false;
        } while (result.status != Status::Error);

        cout << "Downloader stage-loop ended\n";

        // Wait threads termination
        block_exchange.stop();  // signal exiting
        message_receiving.join();
        stats_receiving.join();
        block_downloading.join();
    } catch (const CLI::ParseError& ex) {
        return_value = app.exit(ex);
    } catch (std::exception& e) {
        cerr << "Exception (type " << typeid(e).name() << "): " << e.what() << "\n";
        return_value = 1;
    }

    return return_value;
}
