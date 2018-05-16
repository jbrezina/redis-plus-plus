/**************************************************************************
   Copyright (c) 2017 sewenew

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 *************************************************************************/

#include "connection.h"
#include <cassert>
#include "reply.h"
#include "command.h"

namespace sw {

namespace redis {

class Connection::Connector {
public:
    explicit Connector(const ConnectionOptions &opts);

    ContextUPtr connect() const;

private:
    ContextUPtr _connect() const;

    redisContext* _connect_tcp() const;

    redisContext* _connect_unix() const;

    void _set_socket_timeout(redisContext &ctx) const;

    void _enable_keep_alive(redisContext &ctx) const;

    timeval _to_timeval(const std::chrono::steady_clock::duration &dur) const;

    const ConnectionOptions &_opts;
};

Connection::Connector::Connector(const ConnectionOptions &opts) : _opts(opts) {}

Connection::ContextUPtr Connection::Connector::connect() const {
    auto ctx = _connect();

    assert(ctx);

    _set_socket_timeout(*ctx);

    _enable_keep_alive(*ctx);

    return ctx;
}

Connection::ContextUPtr Connection::Connector::_connect() const {
    redisContext *context = nullptr;
    switch (_opts.type) {
    case ConnectionType::TCP:
        context = _connect_tcp();
        break;

    case ConnectionType::UNIX:
        context = _connect_unix();
        break;

    default:
        // Never goes here.
        throw Error("Unkonw connection type");
    }

    if (context == nullptr) {
        throw Error("Failed to allocate memory for connection.");
    }

    return ContextUPtr(context);
}

redisContext* Connection::Connector::_connect_tcp() const {
    if (_opts.connect_timeout > std::chrono::steady_clock::duration(0)) {
        return redisConnectWithTimeout(_opts.host.c_str(),
                    _opts.port,
                    _to_timeval(_opts.connect_timeout));
    } else {
        return redisConnect(_opts.host.c_str(), _opts.port);
    }
}

redisContext* Connection::Connector::_connect_unix() const {
    if (_opts.connect_timeout > std::chrono::steady_clock::duration(0)) {
        return redisConnectUnixWithTimeout(
                    _opts.path.c_str(),
                    _to_timeval(_opts.connect_timeout));
    } else {
        return redisConnectUnix(_opts.path.c_str());
    }
}

void Connection::Connector::_set_socket_timeout(redisContext &ctx) const {
    if (_opts.socket_timeout <= std::chrono::steady_clock::duration(0)) {
        return;
    }

    if (redisSetTimeout(&ctx, _to_timeval(_opts.socket_timeout)) != REDIS_OK) {
        throw_error(ctx, "Failed to set socket timeout");
    }
}

void Connection::Connector::_enable_keep_alive(redisContext &ctx) const {
    if (!_opts.keep_alive) {
        return;
    }

    if (redisEnableKeepAlive(&ctx) != REDIS_OK) {
        throw_error(ctx, "Failed to enable keep alive option");
    }
}

timeval Connection::Connector::_to_timeval(const std::chrono::steady_clock::duration &dur) const {
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(dur);
    auto msec = std::chrono::duration_cast<std::chrono::microseconds>(dur - sec);

    return {
            static_cast<std::time_t>(sec.count()),
            static_cast<suseconds_t>(msec.count())
    };
}

void swap(Connection &lhs, Connection &rhs) noexcept {
    std::swap(lhs._ctx, rhs._ctx);
    std::swap(lhs._last_active, rhs._last_active);
    std::swap(lhs._opts, rhs._opts);
}

Connection::Connection(const ConnectionOptions &opts) :
            _ctx(Connector(opts).connect()),
            _last_active(std::chrono::steady_clock::now()),
            _opts(opts) {
    assert(!_ctx);

    if (broken()) {
        throw_error(*_ctx, "Failed to connect to Redis");
    }

    _set_options();
}

void Connection::reconnect() {
    Connection connection(_opts);

    swap(*this, connection);
}

void Connection::send(int argc, const char **argv, const std::size_t *argv_len) {
    auto ctx = _context();

    assert(ctx != nullptr);

    if (redisAppendCommandArgv(ctx,
                                argc,
                                argv,
                                argv_len) != REDIS_OK) {
        throw_error(*ctx, "Failed to send command");
    }

    assert(!broken());
}

auto Connection::CmdArgs::operator<<(const StringView &arg) -> CmdArgs& {
    _argv.push_back(arg.data());
    _argv_len.push_back(arg.size());

    return *this;
}

void Connection::send(CmdArgs &args) {
    auto ctx = _context();

    assert(ctx != nullptr);

    if (redisAppendCommandArgv(ctx,
                                args.size(),
                                args.argv(),
                                args.argv_len()) != REDIS_OK) {
        throw_error(*ctx, "Failed to send command");
    }

    assert(!broken());
}

ReplyUPtr Connection::recv() {
    auto *ctx = _context();

    assert(ctx != nullptr);

    void *r = nullptr;
    if (redisGetReply(ctx, &r) != REDIS_OK) {
        throw_error(*ctx, "Failed to get reply");
    }

    assert(!broken());

    auto reply = ReplyUPtr(static_cast<redisReply*>(r));

    if (reply::is_error(*reply)) {
        throw_error(*reply);
    }

    return reply;
}

std::string Connection::_server_info() const {
    assert(_ctx);

    auto connection_type = _ctx->connection_type;
    switch (connection_type) {
    case REDIS_CONN_TCP:
        return std::string(_ctx->tcp.host)
                    + ":" + std::to_string(_ctx->tcp.port);

    case REDIS_CONN_UNIX:
        return _ctx->unix_sock.path;

    default:
        throw Error("Unknown connection type: "
                + std::to_string(connection_type));
    }
}

void Connection::_set_options() {
    _auth();

    _select_db();
}

void Connection::_auth() {
    if (_opts.password.empty()) {
        return;
    }

    cmd::auth(*this, _opts.password);

    auto reply = recv();

    reply::expect_ok_status(*reply);
}

void Connection::_select_db() {
    if (_opts.db == 0) {
        return;
    }

    cmd::select(*this, _opts.db);

    auto reply = recv();

    reply::expect_ok_status(*reply);
}

}

}
