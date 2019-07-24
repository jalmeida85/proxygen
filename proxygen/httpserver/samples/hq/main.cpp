/*
 *  Copyright (c) 2019-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <folly/portability/GFlags.h>

#include <folly/init/Init.h>
#include <folly/ssl/Init.h>

#include <proxygen/httpserver/samples/hq/ConnIdLogger.h>
#include <proxygen/httpserver/samples/hq/HQClient.h>
#include <proxygen/httpserver/samples/hq/HQServer.h>
#include <proxygen/lib/transport/PersistentQuicPskCache.h>

DEFINE_string(host, "::1", "HQ server hostname/IP");
DEFINE_int32(port, 6666, "HQ server port");
DEFINE_int32(h2port, 6667, "HTTP/2 server port");
DEFINE_string(mode, "server", "Mode to run in: 'client' or 'server'");
DEFINE_string(body, "", "Filename to read from for POST requests");
DEFINE_string(path,
              "/",
              "(HQClient) url-path to send the request to, "
              "or a comma separated list of paths to fetch in parallel");
DEFINE_string(httpversion, "1.1", "HTTP version string");
DEFINE_string(protocol, "", "HQ protocol version e.g. h1q-fb or h1q-fb-v2");
DEFINE_int32(draft_version, 0, "Draft version to use, 0 is default");
DEFINE_bool(use_draft, true, "Use draft version as first version");
DEFINE_string(logdir, "/tmp/logs", "Directory to store connection logs");
DEFINE_string(congestion, "cubic", "newreno/cubic/bbr/none");
DEFINE_int32(conn_flow_control, 1024 * 1024, "Connection flow control");
DEFINE_int32(stream_flow_control, 65 * 1024, "Stream flow control");
DEFINE_int32(max_receive_packet_size,
             quic::kDefaultUDPReadBufferSize,
             "Max UDP packet size Quic can receive");
DEFINE_int32(txn_timeout, 120000, "HTTP Transaction Timeout");
DEFINE_string(headers, "", "List of N=V headers separated by ,");
DEFINE_bool(pacing, false, "Whether to enable pacing on HQServer");
DEFINE_string(psk_file, "", "Cache file to use for QUIC psks");
DEFINE_bool(early_data, false, "Whether to use 0-rtt");
DEFINE_uint32(quic_batching_mode,
              static_cast<uint32_t>(quic::QuicBatchingMode::BATCHING_MODE_NONE),
              "QUIC batching mode");
DEFINE_uint32(quic_batch_size,
              quic::kDefaultQuicMaxBatchSize,
              "Maximum number of packets that can be batched in Quic");
DEFINE_string(cert, "", "Certificate file path");
DEFINE_string(key, "", "Private key file path");
DEFINE_string(qlogger_path, "", "Path to the directory where qlog files"
                                "will be written. File is called <CID>.qlog");
DEFINE_bool(pretty_json,
            true,
            "Whether to use pretty json for QLogger output");

// Partially reliable flags.
DEFINE_bool(use_pr, false, "Use partial reliability");
DEFINE_uint32(pr_chunk_size,
              16,
              "Chunk size to use for partially realible server handler");
DEFINE_uint32(pr_chunk_delay_ms,
              0,
              "Max delay for the body chunks in partially reliable mode");
// Example of starting a server streaming body in chunks in partially realible
// mode (serve 17-byte body chunks with random delay from 0 to 500 ms):
//    hq -mode server -use_pr -protocol="h3-20" -pr_chunk_size 17 -pr_chunk_delay_ms 500
// Example of starting a client requesting a partial reliable streaming with
// delay cap of 150 ms:
//    hq -mode client -use_pr -protocol="h3-20" -path="/pr_cat" -pr_chunk_delay_ms 150

DEFINE_string(lat, "0.0", "Private key file path");
DEFINE_string(plr, "0.0", "Private key file path");
DEFINE_string(bytes, "24262167", "Private key file path");

using namespace quic::samples;

quic::CongestionControlType flagsToCongestionControlType(
    const std::string& congestionControlType) {
  if (congestionControlType == "cubic") {
    return quic::CongestionControlType::Cubic;
  } else if (congestionControlType == "newreno") {
    return quic::CongestionControlType::NewReno;
  } else if (congestionControlType == "bbr") {
    return quic::CongestionControlType::BBR;
  } else if (congestionControlType == "none") {
    return quic::CongestionControlType::None;
  }
  throw std::invalid_argument(folly::to<std::string>(
      "Unknown congestion controller ", congestionControlType));
}

int main(int argc, char* argv[]) {
#if FOLLY_HAVE_LIBGFLAGS
  // Enable glog logging to stderr by default.
  gflags::SetCommandLineOptionWithMode(
      "logtostderr", "1", gflags::SET_FLAGS_DEFAULT);
#endif
  folly::init(&argc, &argv, false);
  folly::ssl::init();
  proxygen::ConnIdLogSink sink(FLAGS_logdir, FLAGS_mode);
  if (sink.isValid()) {
    AddLogSink(&sink);
  } else if (!FLAGS_logdir.empty()) {
    LOG(ERROR) << "Cannot open " << FLAGS_logdir;
  }

  folly::Optional<quic::QuicVersion> draftVersion;
  if (FLAGS_draft_version != 0) {
    draftVersion =
        static_cast<quic::QuicVersion>(0xff000000 | FLAGS_draft_version);
  }
  quic::TransportSettings transportSettings;
  transportSettings.advertisedInitialConnectionWindowSize =
      FLAGS_conn_flow_control;
  // TODO FLAGS_stream*
  transportSettings.advertisedInitialBidiLocalStreamWindowSize =
      FLAGS_stream_flow_control;
  transportSettings.advertisedInitialBidiRemoteStreamWindowSize =
      FLAGS_stream_flow_control;
  transportSettings.advertisedInitialUniStreamWindowSize =
      FLAGS_stream_flow_control;
  transportSettings.defaultCongestionController =
      flagsToCongestionControlType(FLAGS_congestion);
  if (folly::to<uint16_t>(FLAGS_max_receive_packet_size) <
      quic::kDefaultUDPSendPacketLen) {
    LOG(ERROR) << "max_receive_packet_size needs to be at least "
               << quic::kDefaultUDPSendPacketLen;
    return -4;
  }
  transportSettings.maxRecvPacketSize = FLAGS_max_receive_packet_size;
  transportSettings.pacingEnabled = FLAGS_pacing;
  transportSettings.batchingMode =
      quic::getQuicBatchingMode(FLAGS_quic_batching_mode);
  transportSettings.maxBatchSize = FLAGS_quic_batch_size;
  transportSettings.turnoffPMTUD = true;
  transportSettings.partialReliabilityEnabled = FLAGS_use_pr;
  if (FLAGS_mode == "server") {
    if (FLAGS_body != "") {
      LOG(ERROR) << "the 'body' argument is allowed only in client mode";
      return -3;
    }

    auto h2server = H2Server::run(
        folly::SocketAddress(FLAGS_host, FLAGS_h2port, true),
        FLAGS_cert, FLAGS_key,
        FLAGS_stream_flow_control, FLAGS_conn_flow_control);
    HQServer server(FLAGS_host,
                    FLAGS_port,
                    FLAGS_httpversion,
                    std::chrono::milliseconds(FLAGS_txn_timeout),
                    transportSettings,
                    draftVersion,
                    FLAGS_use_draft,
                    FLAGS_qlogger_path,
                    FLAGS_pretty_json,
                    FLAGS_pr_chunk_size,
                    FLAGS_pr_chunk_delay_ms);
    server.setTlsSettings(
        FLAGS_cert, FLAGS_key, fizz::server::ClientAuthMode::None);
    server.start();
    server.getAddress();
    server.run();
    h2server.join();
  } else if (FLAGS_mode == "client") {
    if (FLAGS_host.empty() || FLAGS_port == 0) {
      LOG(ERROR) << "H1Client expected --host and --port";
      return -2;
    }
    HQClient client(FLAGS_host,
                    FLAGS_port,
                    FLAGS_headers,
                    FLAGS_body,
                    FLAGS_path,
                    FLAGS_httpversion,
                    transportSettings,
                    draftVersion,
                    FLAGS_use_draft,
                    std::chrono::milliseconds(FLAGS_txn_timeout),
                    FLAGS_qlogger_path,
                    FLAGS_pretty_json,
                    FLAGS_use_pr,
                    FLAGS_pr_chunk_delay_ms
                    FLAGS_lat,
                    FLAGS_plr,
                    FLAGS_bytes);
    if (!FLAGS_protocol.empty()) {
      client.setProtocol(FLAGS_protocol);
    }
    if (!FLAGS_psk_file.empty()) {
      auto pskCache = std::make_shared<proxygen::PersistentQuicPskCache>(
          FLAGS_psk_file,
          wangle::PersistentCacheConfig::Builder()
              .setCapacity(1000)
              .setSyncInterval(std::chrono::seconds(1))
              .build());
      client.setQuicPskCache(std::move(pskCache));
    }
    client.setEarlyData(FLAGS_early_data);
    client.start();
  } else {
    LOG(ERROR) << "Unknown mode specified: " << FLAGS_mode;
    return -1;
  }
  return 0;
}
