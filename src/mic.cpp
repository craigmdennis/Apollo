/**
 * @file src/mic.cpp
 * @brief Definitions for the remote microphone module.
 *
 * This module never touches streaming code. Every failure ends the mic session only.
 *
 * Playout is pull-based: the virtual microphone's render thread calls playout_t::fill()
 * when the device has room, so the audio device clock is the only playout clock.
 * Design evidence: docs/superpowers/research/2026-09-20-voice-playout.md.
 *
 * Threads:
 * - The mic thread runs one io_context. It owns the socket, the timer, and the mic session.
 * - The render thread belongs to the virtual microphone. It touches playout_t only.
 * - Config server threads call the functions in mic.h. They reach the mic session by
 *   posting to the io_context. state_mutex guards everything they share with the mic
 *   thread: the store, the pairing state, the connected device id, and the lifecycle
 *   (io, thread, stopped).
 */
// standard includes
#include <array>
#include <atomic>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "config.h"
#include "file_handler.h"
#include "logging.h"
#include "mic.h"
#include "mic_playout.h"
#include "mic_protocol.h"
#include "network.h"
#include "platform/common.h"
#include "system_tray.h"
#include "uuid.h"

using namespace std::literals;

namespace mic {
  namespace {
    namespace asio = boost::asio;
    using asio::ip::udp;
    using steady = std::chrono::steady_clock;

    constexpr int MIC_PORT_OFFSET = 13;
    constexpr auto HOUSEKEEPING = 1s;
    constexpr auto SESSION_TIMEOUT = 5s;
    constexpr auto REPLACED_LIFETIME = 5s;
    // Opening the virtual microphone can install a driver, which takes several seconds. The
    // config server has one thread, so this wait also freezes the web UI. Keep it short.
    constexpr auto SESSION_START_TIMEOUT = 10s;
    // Any LAN host can ask to pair, so the tray notice is limited to one per interval.
    constexpr auto PAIR_NOTICE_INTERVAL = 30s;

    struct session_t {
      std::uint32_t id;
      crypto::cipher::gcm_t cipher;
      device_t device;
      steady::time_point last_valid;

      // Pings count from 0 and only ever increase, so an old ping is a replay.
      bool have_ping = false;
      std::uint32_t last_ping = 0;

      protocol::error_e error_code = protocol::error_e::none;  ///< Sent in every pong
      protocol::error_e notified = protocol::error_e::none;  ///< Last error shown in the tray

      // Members die in reverse order. vmic is declared last, so the virtual microphone and its
      // render thread are gone before playout, which holds the decoder that thread uses.
      playout_t playout;
      std::unique_ptr<platf::virtual_mic_t> vmic;
    };

    struct replaced_t {
      std::uint32_t id;
      crypto::cipher::gcm_t cipher;
      steady::time_point expires;
    };

    // Guarded by state_mutex. Shared by config server threads and the mic thread.
    std::mutex state_mutex;
    std::unique_ptr<store_t> store;
    pairing_t pairing;
    crypto::sha256_t fingerprint {};
    std::string connected_uuid;
    std::shared_ptr<asio::io_context> io;
    std::thread thread;
    bool stopped = false;  ///< Set once by stop(). The mic thread never starts again.
    std::optional<steady::time_point> last_pair_notice;

    std::atomic<bool> running {false};

    // Owned by the mic thread. Created and destroyed inside run().
    std::unique_ptr<udp::socket> socket;
    std::unique_ptr<asio::steady_timer> timer;
    std::unique_ptr<session_t> session;
    std::optional<replaced_t> replaced;
    std::array<char, 2048> receive_buffer;
    udp::endpoint sender;

    void notify_connected(const std::string &name) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_mic_connected(name);
#endif
    }

    void notify_disconnected(const std::string &name, const std::string &reason) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_mic_disconnected(name, reason);
#endif
    }

    void notify_error(protocol::error_e error_code) {
      std::string text;
      switch (error_code) {
        case protocol::error_e::device_missing:
          text = "Apollo cannot find the Steam Streaming Microphone. Install Steam on this PC, then connect again from Calliope.";
          break;
        case protocol::error_e::device_open_failed:
          text = "Another program on this PC is blocking the Steam Streaming Microphone. Close it, then connect again from Calliope.";
          break;
        case protocol::error_e::decode_failed:
          text = "Apollo cannot decode the audio from the microphone device. Disconnect and connect again in Calliope.";
          break;
        default:
          return;
      }
      BOOST_LOG(warning) << "Remote microphone: "sv << text;
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_mic_error(text);
#endif
    }

    /**
     * @brief Write the store and log a failure. Call with state_mutex held.
     */
    bool save_store() {
      if (store->save()) {
        return true;
      }
      BOOST_LOG(error) << "Remote microphone: could not write mic_state.json"sv;
      return false;
    }

    /**
     * @brief Show a session error in the tray once. Runs on the mic thread only.
     */
    void report_error(session_t &target) {
      if (target.error_code != target.notified) {
        notify_error(target.error_code);
        target.notified = target.error_code;
      }
    }

    /**
     * @brief End the mic session on the mic thread.
     */
    void end_session(const std::string &reason, bool notify) {
      if (!session) {
        return;
      }

      auto name = session->device.name;
      // Destroying the session destroys the virtual microphone first (see session_t), which
      // joins the render thread and restores the default capture device.
      session.reset();

      {
        std::lock_guard lock {state_mutex};
        connected_uuid.clear();
        store->previous_default_capture.clear();
        save_store();
      }

      BOOST_LOG(info) << "Remote microphone: session ended for ["sv << name << "] "sv << reason;
      if (notify) {
        notify_disconnected(name, reason);
      }
    }

    std::variant<session_info_t, session_error_e> begin_session(const device_t &device) {
      if (session) {
        // Keep the old key for a short time so the replaced device receives pong code 4.
        replaced = replaced_t {session->id, std::move(session->cipher), steady::now() + REPLACED_LIFETIME};
        // End the old session first. Its destructor restores the default capture device,
        // so the new virtual microphone records the real previous default.
        end_session("replaced by "s + device.name, false);
      }

      auto key_bytes = crypto::rand(16);
      crypto::aes_t key {key_bytes.begin(), key_bytes.end()};
      std::uint32_t id = 0;
      auto id_bytes = crypto::rand(4);
      std::memcpy(&id, id_bytes.data(), sizeof(id));

      auto next = std::make_unique<session_t>();
      next->id = id;
      next->cipher = crypto::cipher::gcm_t {key, false};
      next->device = device;
      next->last_valid = steady::now();

      if (!next->playout.ready()) {
        next->error_code = protocol::error_e::decode_failed;
      } else {
        // The callback runs on the render thread until vmic is destroyed. vmic is the last
        // member of session_t, so it is destroyed before playout.
        auto *playout = &next->playout;
        platf::virtual_mic_error_e vmic_error = platf::virtual_mic_error_e::none;
        next->vmic = platf::virtual_mic(
          [playout](float *out, std::size_t capacity) {
            return playout->fill(out, capacity);
          },
          vmic_error
        );

        if (vmic_error == platf::virtual_mic_error_e::unsupported) {
          return session_error_e::unsupported;
        }
        if (vmic_error == platf::virtual_mic_error_e::device_missing) {
          next->error_code = protocol::error_e::device_missing;
        } else if (vmic_error == platf::virtual_mic_error_e::device_open_failed) {
          next->error_code = protocol::error_e::device_open_failed;
        }
      }

      session = std::move(next);

      {
        std::lock_guard lock {state_mutex};
        connected_uuid = device.uuid;
        if (session->vmic) {
          store->previous_default_capture = session->vmic->previous_default_capture();
          save_store();
        }
      }

      BOOST_LOG(info) << "Remote microphone: session started for ["sv << device.name << ']';
      if (session->error_code == protocol::error_e::none) {
        notify_connected(device.name);
      } else {
        report_error(*session);
      }

      return session_info_t {id, key_bytes, net::map_port(MIC_PORT_OFFSET)};
    }

    void send_pong(crypto::cipher::gcm_t &cipher, std::uint32_t session_id, std::uint32_t sequence, protocol::error_e error_code) {
      const char code = static_cast<char>(error_code);
      auto datagram = protocol::encrypt_packet(cipher, protocol::direction_e::to_client, {protocol::packet_type_e::pong, session_id, sequence}, std::string_view {&code, 1});
      if (datagram.empty()) {
        return;
      }
      boost::system::error_code ec;
      socket->send_to(asio::buffer(datagram), sender, 0, ec);
    }

    void handle_datagram(std::string_view datagram) {
      auto header = protocol::parse_header(datagram);
      if (!header) {
        return;
      }

      if (session && header->session_id == session->id) {
        auto packet = protocol::decrypt_packet(session->cipher, protocol::direction_e::to_host, datagram);
        if (!packet) {
          return;
        }

        // Only a fresh packet keeps the session alive. A captured packet sent again must not.
        if (packet->header.type == protocol::packet_type_e::audio) {
          if (session->error_code == protocol::error_e::none && session->playout.push(packet->header.sequence, std::move(packet->payload))) {
            session->last_valid = steady::now();
          }
        } else if (packet->header.type == protocol::packet_type_e::ping) {
          if (session->have_ping && packet->header.sequence <= session->last_ping) {
            return;
          }
          session->have_ping = true;
          session->last_ping = packet->header.sequence;
          session->last_valid = steady::now();
          send_pong(session->cipher, session->id, packet->header.sequence, session->error_code);
        }
        return;
      }

      if (replaced && header->session_id == replaced->id && header->type == protocol::packet_type_e::ping) {
        if (protocol::decrypt_packet(replaced->cipher, protocol::direction_e::to_host, datagram)) {
          send_pong(replaced->cipher, replaced->id, header->sequence, protocol::error_e::replaced);
        }
      }
    }

    /**
     * @brief Expire the replaced key, time out a silent session, and check the virtual microphone.
     *
     * One second is enough for these. Playout does not depend on this timer.
     */
    void on_housekeeping(const boost::system::error_code &ec) {
      if (ec) {
        return;
      }

      auto now = steady::now();
      if (replaced && now > replaced->expires) {
        replaced.reset();
      }

      if (session) {
        if (now - session->last_valid > SESSION_TIMEOUT) {
          end_session("connection lost", true);
        } else {
          // The render thread only latches a failure. The tray is driven from this thread.
          if (session->error_code == protocol::error_e::none && session->playout.decode_failed()) {
            session->error_code = protocol::error_e::decode_failed;
          }
          if (session->vmic && !session->vmic->healthy()) {
            session->error_code = protocol::error_e::device_open_failed;
            session->vmic.reset();  // restores the default capture device

            std::lock_guard lock {state_mutex};
            store->previous_default_capture.clear();
            save_store();
          }
          report_error(*session);
        }
      }

      timer->expires_after(HOUSEKEEPING);
      timer->async_wait(on_housekeeping);
    }

    void receive_next() {
      socket->async_receive_from(asio::buffer(receive_buffer), sender, [](const boost::system::error_code &ec, std::size_t bytes) {
        if (ec == asio::error::operation_aborted) {
          return;
        }
        if (!ec) {
          try {
            handle_datagram(std::string_view {receive_buffer.data(), bytes});
          } catch (const std::exception &e) {
            BOOST_LOG(warning) << "Remote microphone: dropped a packet: "sv << e.what();
          }
        }
        receive_next();
      });
    }

    /**
     * @brief The mic thread. Holds its own reference to the io_context for its whole life.
     */
    void run(std::shared_ptr<asio::io_context> context, std::promise<bool> ready) {
      try {
        auto address_family = net::af_from_enum_string(config::sunshine.address_family);
        auto protocol_family = address_family == net::IPV4 ? udp::v4() : udp::v6();
        auto port = net::map_port(MIC_PORT_OFFSET);

        socket = std::make_unique<udp::socket>(*context);
        socket->open(protocol_family);
        socket->bind(udp::endpoint(protocol_family, port));

        timer = std::make_unique<asio::steady_timer>(*context);
        timer->expires_after(HOUSEKEEPING);
        timer->async_wait(on_housekeeping);
        receive_next();

        BOOST_LOG(info) << "Remote microphone: listening on UDP port "sv << port;
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Remote microphone: could not open the UDP port: "sv << e.what();
        // I/O objects must die before their io_context.
        timer.reset();
        socket.reset();
        ready.set_value(false);
        return;
      }

      running = true;
      ready.set_value(true);

      try {
        context->run();
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Remote microphone: the mic thread stopped: "sv << e.what();
      }

      // A session must not outlive its thread: nothing would time it out or restore the device.
      // A throw here would terminate Apollo, and a live stream with it.
      try {
        end_session("the mic thread stopped", false);
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Remote microphone: could not end the session cleanly: "sv << e.what();
      }
      timer.reset();
      socket.reset();
      // Last statement. ensure_thread() joins this thread while it holds state_mutex, and
      // end_session() above takes state_mutex, so running stays true until that is done.
      running = false;
    }

    /**
     * @brief Start the mic thread when it is not running. Call with state_mutex held.
     * @return false after stop(), or when the UDP port cannot be opened.
     */
    bool ensure_thread() {
      if (stopped) {
        return false;
      }
      if (running) {
        return true;
      }
      if (thread.joinable()) {
        thread.join();
      }

      // The previous thread destroyed its socket and timer before it cleared running.
      io = std::make_shared<asio::io_context>();
      std::promise<bool> ready;
      auto started = ready.get_future();
      thread = std::thread {run, io, std::move(ready)};
      return started.get();
    }

    /**
     * @brief The io_context to post to, or null when the mic thread is not running.
     */
    std::shared_ptr<asio::io_context> live_context() {
      std::lock_guard lock {state_mutex};
      if (stopped || !running) {
        return nullptr;
      }
      return io;
    }
  }  // namespace

  void start() {
    std::lock_guard lock {state_mutex};

    // The store exists before anything that can throw, because every handler dereferences it.
    auto file = std::filesystem::path {config::nvhttp.file_state}.parent_path() / "mic_state.json";
    store = std::make_unique<store_t>(file);

    // This runs on Apollo's startup path. A microphone problem must never stop Apollo from starting.
    try {
      if (!store->load()) {
        BOOST_LOG(error) << "Remote microphone: could not read "sv << file.string() << ". No microphone is paired. The next pairing replaces the file."sv;
      }

      if (auto value = protocol::cert_fingerprint(file_handler::read_file(config::nvhttp.cert.c_str()))) {
        fingerprint = *value;
      } else {
        BOOST_LOG(error) << "Remote microphone: could not read the host certificate. Pairing a microphone fails until Apollo restarts with a readable certificate."sv;
      }

      if (!store->previous_default_capture.empty()) {
        BOOST_LOG(info) << "Remote microphone: restoring the default capture device after an unclean exit"sv;
        platf::restore_default_capture(store->previous_default_capture);
        store->previous_default_capture.clear();
        save_store();
      }

      // Inert until a mic device is paired: no thread and no socket.
      if (!store->devices().empty()) {
        ensure_thread();
      }
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Remote microphone: startup failed, the remote microphone is unavailable: "sv << e.what();
    }
  }

  void stop() {
    std::shared_ptr<asio::io_context> context;
    {
      std::lock_guard lock {state_mutex};
      stopped = true;  // from here on ensure_thread() refuses, so nobody else touches thread
      context = io;
    }

    if (context) {
      // run() ends the session on its way out. No handler is posted here: a handler that
      // holds the io_context it is queued in keeps that io_context alive when it never runs.
      context->stop();
    }
    if (thread.joinable()) {
      thread.join();
    }

    // Dropping the last references destroys the io_context and every handler still queued in
    // it. That breaks the promise of a session_start() whose handler never ran.
    context.reset();
    std::lock_guard lock {state_mutex};
    io.reset();
  }

  std::optional<std::string> pair_request(const std::string &address, const std::string &name, const std::string &nonce, const crypto::sha256_t &proof) {
    std::optional<std::string> request_id;
    bool notify = false;
    {
      std::lock_guard lock {state_mutex};
      auto now = steady::now();
      request_id = pairing.request(address, name, nonce, proof, now);
      if (request_id && (!last_pair_notice || now - *last_pair_notice >= PAIR_NOTICE_INTERVAL)) {
        last_pair_notice = now;
        notify = true;
      }
    }
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    if (notify) {
      system_tray::update_tray_mic_pair_request();
    }
#endif
    return request_id;
  }

  pairing_t::status_t pair_status(const std::string &request_id) {
    std::lock_guard lock {state_mutex};
    return pairing.status(request_id, steady::now());
  }

  std::optional<std::string> submit_pin(const std::string &pin, const std::string &name) {
    std::lock_guard lock {state_mutex};
    auto now = steady::now();
    auto match = pairing.submit_pin(pin, fingerprint, now);
    if (!match) {
      return std::nullopt;
    }

    auto device_name = name.empty() ? match->name : name;
    auto uuid = uuid_util::uuid_t::generate().string();
    auto token = crypto::rand_alphabet(48, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");

    store->add(device_name, uuid, token);
    if (!save_store()) {
      store->remove(uuid);
      return std::nullopt;
    }

    pairing.complete(match->request_id, device_name, uuid, token, now);
    ensure_thread();
    BOOST_LOG(info) << "Remote microphone: paired ["sv << device_name << ']';
    return device_name;
  }

  nlohmann::json list() {
    std::lock_guard lock {state_mutex};
    auto devices = nlohmann::json::array();
    for (const auto &device : store->devices()) {
      devices.push_back({{"name", device.name}, {"uuid", device.uuid}, {"connected", device.uuid == connected_uuid}});
    }
    return devices;
  }

  bool remove(const std::string &uuid) {
    bool was_connected;
    bool saved;
    {
      std::lock_guard lock {state_mutex};
      if (!store->remove(uuid)) {
        return false;
      }
      saved = save_store();
      was_connected = connected_uuid == uuid;
    }
    if (was_connected) {
      session_end(uuid);
    }
    // false tells the web UI that the device returns after a restart.
    return saved;
  }

  std::optional<device_t> authorize(const std::string &token) {
    std::lock_guard lock {state_mutex};
    return store->authorize(token);
  }

  std::variant<session_info_t, session_error_e> session_start(const device_t &device) {
    using result_t = std::variant<session_info_t, session_error_e>;

    std::shared_ptr<asio::io_context> context;
    {
      std::lock_guard lock {state_mutex};
      if (!ensure_thread()) {
        return session_error_e::failed;
      }
      context = io;
    }

    // Shared, and captured by value: the handler can run after this function gave up waiting.
    auto promise = std::make_shared<std::promise<result_t>>();
    auto result = promise->get_future();
    asio::post(*context, [promise, device]() {
      try {
        promise->set_value(begin_session(device));
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Remote microphone: could not start a session: "sv << e.what();
        promise->set_value(session_error_e::failed);
      }
    });
    // stop() destroys the io_context to break this promise when the handler never runs.
    context.reset();

    try {
      if (result.wait_for(SESSION_START_TIMEOUT) != std::future_status::ready) {
        // The handler can still run later. That session ends itself after 5 seconds with no packet.
        return session_error_e::busy;
      }
      return result.get();
    } catch (const std::future_error &) {
      return session_error_e::failed;
    }
  }

  void session_end(const std::string &uuid) {
    auto context = live_context();
    if (!context) {
      return;
    }
    asio::post(*context, [uuid]() {
      if (session && session->device.uuid == uuid) {
        end_session("", true);
      }
    });
  }
}  // namespace mic
