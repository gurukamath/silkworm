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

#pragma once

#include <silkworm/downloader/packets/get_block_headers_packet.hpp>

#include "outbound_message.hpp"

namespace silkworm {

class OutboundGetBlockHeaders : public OutboundMessage {
  public:
    OutboundGetBlockHeaders();

    std::string name() const override { return "OutboundGetBlockHeaders"; }
    std::string content() const override;

    void execute(Db::ReadOnlyAccess, HeaderChain&, BodySequence&, SentryClient&) override;

    int sent_request() const;

  private:
    sentry::SentPeers send_packet(SentryClient&, const GetBlockHeadersPacket66&, seconds_t timeout);
    void send_penalization(SentryClient&, const PeerPenalization&, seconds_t timeout);

    int sent_reqs_{0};
    std::string packets_;
};

}  // namespace silkworm
