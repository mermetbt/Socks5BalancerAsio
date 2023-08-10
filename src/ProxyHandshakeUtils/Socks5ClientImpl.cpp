/**
 * Socks5BalancerAsio : A Simple TCP Socket Balancer for balance Multi Socks5 Proxy Backend Powered by Boost.Asio
 * Copyright (C) <2020>  <Jeremie>
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

#include "Socks5ClientImpl.h"
#include "../ProxyHandshakeAuth.h"

#include <boost/lexical_cast.hpp>

void Socks5ClientImpl::do_whenError(boost::system::error_code error) {
    auto ptr = parents.lock();
    if (ptr) {
        ptr->do_whenError(error);
    }
}

void Socks5ClientImpl::start() {
    do_socks5_handshake_write();
}

// https://wiyi.org/socks5-protocol-in-deep.html
// https://en.wikipedia.org/wiki/SOCKS

void Socks5ClientImpl::do_socks5_handshake_write() {
    // do_upstream_write
    auto ptr = parents.lock();
    if (ptr) {

        // send socks5 client handshake
        // +----+----------+----------+
        // |VER | NMETHODS | METHODS  |
        // +----+----------+----------+
        // | 1  |    1     | 1 to 255 |
        // +----+----------+----------+
        auto data_send = std::make_shared<std::string>(
                "\x05\x01\x00", 3
        );

        if (!ptr->nowServer->authUser.empty()) {
            // tell server, we can auth
            data_send->at(2) = '\x02';
        }

        boost::asio::async_write(
                ptr->upstream_socket_,
                boost::asio::buffer(*data_send),
                [this, self = shared_from_this(), data_send, ptr](
                        const boost::system::error_code &ec,
                        std::size_t bytes_transferred_) {
                    if (ec) {
                        return fail(ec, "socks5_handshake_write");
                    }
                    if (bytes_transferred_ != data_send->size()) {
                        std::stringstream ss;
                        ss << "socks5_handshake_write with bytes_transferred_:"
                           << bytes_transferred_ << " but data_send->size():" << data_send->size();
                        return fail(ec, ss.str());
                    }

                    // std::cout << "do_socks5_handshake_write()" << std::endl;

                    do_socks5_handshake_read();
                }
        );
    } else {
        badParentPtr();
    }
}

void Socks5ClientImpl::do_socks5_handshake_read() {
    // do_upstream_read
    auto ptr = parents.lock();
    if (ptr) {

        const size_t MAX_LENGTH = 8196;
        auto socks5_read_buf = std::make_shared<std::vector<uint8_t>>(MAX_LENGTH);

        ptr->upstream_socket_.async_read_some(
                boost::asio::buffer(*socks5_read_buf, MAX_LENGTH),
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this(), socks5_read_buf, ptr](
                                boost::beast::error_code ec,
                                const size_t &bytes_transferred) {
                            if (ec) {
                                return fail(ec, "socks5_handshake_read");
                            }

                            // check server response
                            //  +----+--------+
                            //  |VER | METHOD |
                            //  +----+--------+
                            //  | 1  |   1    |
                            //  +----+--------+
                            if (bytes_transferred < 2) {
                                return fail(ec, "socks5_handshake_read (bytes_transferred < 2)");
                            }
                            if (socks5_read_buf->at(0) != 5) {
                                return fail(ec, "socks5_handshake_read (socks5_read_buf->at(0) != 5)");
                            }
                            if (socks5_read_buf->at(1) != 0x00 && socks5_read_buf->at(1) != 0x02) {
                                return fail(ec, "socks5_handshake_read (invalid auth type)");
                            }
                            if (socks5_read_buf->at(1) == 0x02) {
                                if (ptr->nowServer->authUser.empty()) {
                                    return fail(ec, "socks5_handshake_read (we cannot auth)");
                                } else {
                                    // send auth
                                    do_socks5_auth_write();
                                    return;
                                }
                            }

                            // std::cout << "do_socks5_handshake_read()" << std::endl;
                            do_socks5_connect_write();
                        }));
    } else {
        badParentPtr();
    }
}

void Socks5ClientImpl::do_socks5_auth_write() {
    // do_upstream_write
    auto ptr = parents.lock();
    if (ptr) {

        // send socks5 client authentication
        //
        // +----+------+----------+------+----------+
        // |VER | ULEN |  UNAME   | PLEN |  PASSWD  |
        // +----+------+----------+------+----------+
        // | 1  |  1   | 1 to 255 |  1   | 1 to 255 |
        // +----+------+----------+------+----------+
        //
        auto data_send = std::make_shared<std::string>(
                std::string{"\x01"} +
                std::string{"\x01"} + ptr->nowServer->authUser +
                std::string{"\x01"} + ptr->nowServer->authPwd
        );
        data_send->at(1) = static_cast<uint8_t>(ptr->nowServer->authUser.length());
        data_send->at(2 + ptr->nowServer->authUser.length()) = static_cast<uint8_t>(ptr->nowServer->authPwd.length());
        BOOST_ASSERT(data_send->length() == (ptr->nowServer->authUser.length() + ptr->nowServer->authPwd.length() + 3));

        boost::asio::async_write(
                ptr->upstream_socket_,
                boost::asio::buffer(*data_send),
                [this, self = shared_from_this(), data_send, ptr](
                        const boost::system::error_code &ec,
                        std::size_t bytes_transferred_) {
                    if (ec) {
                        return fail(ec, "do_socks5_auth_write");
                    }
                    if (bytes_transferred_ != data_send->size()) {
                        std::stringstream ss;
                        ss << "do_socks5_auth_write with bytes_transferred_:"
                           << bytes_transferred_ << " but data_send->size():" << data_send->size();
                        return fail(ec, ss.str());
                    }

                    // std::cout << "do_socks5_handshake_write()" << std::endl;

                    do_socks5_auth_read();
                }
        );
    } else {
        badParentPtr();
    }
}

void Socks5ClientImpl::do_socks5_auth_read() {
    // do_upstream_read
    auto ptr = parents.lock();
    if (ptr) {

        const size_t MAX_LENGTH = 8196;
        auto socks5_read_buf = std::make_shared<std::vector<uint8_t>>(MAX_LENGTH);

        ptr->upstream_socket_.async_read_some(
                boost::asio::buffer(*socks5_read_buf, MAX_LENGTH),
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this(), socks5_read_buf, ptr](
                                boost::beast::error_code ec,
                                const size_t &bytes_transferred) {
                            if (ec) {
                                return fail(ec, "do_socks5_auth_read");
                            }

                            // check server response
                            //  +----+--------+
                            //  |VER | STATUS |
                            //  +----+--------+
                            //  | 1  |   1    |
                            //  +----+--------+
                            if (bytes_transferred < 2) {
                                return fail(ec, "do_socks5_auth_read (bytes_transferred < 2)");
                            }
                            if (socks5_read_buf->at(0) != 0x01 || socks5_read_buf->at(1) != 0x00) {
                                return fail(ec, "do_socks5_auth_read (failed)");
                            }

                            do_socks5_connect_write();
                        }));
    } else {
        badParentPtr();
    }
}

void Socks5ClientImpl::do_socks5_connect_write() {
    // do_upstream_write
    auto ptr = parents.lock();
    if (ptr) {

        // analysis targetHost and targetPort
        // targetHost,
        // targetPort,
        boost::beast::error_code ec;
        auto addr = boost::asio::ip::make_address(ptr->host, ec);

        // send socks5 client connect
        // +----+-----+-------+------+----------+----------+
        // |VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
        // +----+-----+-------+------+----------+----------+
        // | 1  |  1  | X'00' |  1   | Variable |    2     |
        // +----+-----+-------+------+----------+----------+
        auto data_send = std::make_shared<std::vector<uint8_t>>();
        data_send->insert(data_send->end(), {0x05, 0x01, 0x00});
        if (ec) {
            // is a domain name
            data_send->push_back(0x03); // ATYP
            if (ptr->host.size() > 253) {
                return fail(ec, "socks5_connect_write (targetHost.size() > 253)");
            }
            data_send->push_back(static_cast<uint8_t>(ptr->host.size()));
            data_send->insert(data_send->end(), ptr->host.begin(), ptr->host.end());
        } else if (addr.is_v4()) {
            data_send->push_back(0x01); // ATYP
            auto v4 = addr.to_v4().to_bytes();
            data_send->insert(data_send->end(), v4.begin(), v4.end());
        } else if (addr.is_v6()) {
            data_send->push_back(0x04); // ATYP
            auto v6 = addr.to_v6().to_bytes();
            data_send->insert(data_send->end(), v6.begin(), v6.end());
        }
        // port
        data_send->push_back(static_cast<uint8_t>(ptr->port >> 8));
        data_send->push_back(static_cast<uint8_t>(ptr->port & 0xff));

        // TODO UDP
        if (ptr->downside_in_udp_mode()) {
            // now we don't impl UDP
            //  data_send->at(1) = 0x03;
            //  udpEnabled = true;
        }

        boost::asio::async_write(
                ptr->upstream_socket_,
                boost::asio::buffer(*data_send),
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this(), data_send, ptr](
                                const boost::system::error_code &ec,
                                std::size_t bytes_transferred_) {
                            if (ec) {
                                return fail(ec, "socks5_connect_write");
                            }
                            if (bytes_transferred_ != data_send->size()) {
                                std::stringstream ss;
                                ss << "socks5_connect_write with bytes_transferred_:"
                                   << bytes_transferred_ << " but data_send->size():" << data_send->size();
                                return fail(ec, ss.str());
                            }

                            // std::cout << "do_socks5_connect_write()" << std::endl;
                            do_socks5_connect_read();
                        })
        );
    } else {
        badParentPtr();
    }
}

void Socks5ClientImpl::do_socks5_connect_read() {
    // do_upstream_read
    auto ptr = parents.lock();
    if (ptr) {

        const size_t MAX_LENGTH = 8196;
        auto socks5_read_buf = std::make_shared<std::vector<uint8_t>>(MAX_LENGTH);

        // Make the connection on the IP address we get from a lookup
        ptr->upstream_socket_.async_read_some(
                boost::asio::buffer(*socks5_read_buf, MAX_LENGTH),
                boost::beast::bind_front_handler(
                        [this, self = shared_from_this(), socks5_read_buf, ptr](
                                boost::beast::error_code ec,
                                const size_t &bytes_transferred) {
                            if (ec) {
                                return fail(ec, "socks5_connect_read");
                            }

                            // check server response
                            // +----+-----+-------+------+----------+----------+
                            // |VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
                            // +----+-----+-------+------+----------+----------+
                            // | 1  |  1  | X'00' |  1   | Variable |    2     |
                            // +----+-----+-------+------+----------+----------+
                            if (bytes_transferred < 6) {
//                                std::stringstream ss;
//                                ss << "socks5_connect_read (bytes_transferred < 6)"
//                                   << " the socks5_read_buf:" << std::hex
//                                   << socks5_read_buf->at(0)
//                                   << socks5_read_buf->at(1)
//                                   << socks5_read_buf->at(2)
//                                   << socks5_read_buf->at(3)
//                                        ;
//                                return fail(ec, ss.str());
                                return fail(ec, "do_socks5_connect_read (bytes_transferred < 6)");
                            }
                            if (socks5_read_buf->at(0) != 5
                                || socks5_read_buf->at(1) != 0x00
                                || socks5_read_buf->at(2) != 0x00
                                || (
                                        socks5_read_buf->at(3) != 0x01 &&
                                        socks5_read_buf->at(3) != 0x03 &&
                                        socks5_read_buf->at(3) != 0x04
                                )) {
                                ptr->do_whenUpReadyError();
                                ptr->do_whenUpEnd();

                                std::stringstream ss;
                                ss << "socks5_connect_read (invalid)"
                                   << " the socks5_read_buf:" << std::hex
                                   << socks5_read_buf->at(0)
                                   << socks5_read_buf->at(1)
                                   << socks5_read_buf->at(2)
                                   << socks5_read_buf->at(3);
                                std::cout << ss.str() << std::endl;
                                return;
//                                return fail(ec, ss.str());
                            }
                            if (socks5_read_buf->at(3) == 0x03
                                && bytes_transferred != (socks5_read_buf->at(4) + 4 + 1 + 2)
                                    ) {
                                return fail(ec, "do_socks5_connect_read (socks5_read_buf->at(3) == 0x03)");
                            }
                            if (socks5_read_buf->at(3) == 0x01
                                && bytes_transferred != (4 + 4 + 2)
                                    ) {
                                return fail(ec, "do_socks5_connect_read (socks5_read_buf->at(3) == 0x01)");
                            }
                            if (socks5_read_buf->at(3) == 0x04
                                && bytes_transferred != (4 + 16 + 2)
                                    ) {
                                return fail(ec, "do_socks5_connect_read (socks5_read_buf->at(3) == 0x04)");
                            }

                            {
                                // bind
                                int bindPort{
                                        socks5_read_buf->at(bytes_transferred - 2) << 8
                                        |
                                        socks5_read_buf->at(bytes_transferred - 1)
                                };
                                std::string bindAddr;
                                switch (socks5_read_buf->at(3)) {
                                    case 0x03:
                                        bindAddr = std::string{
                                                (char *) (socks5_read_buf->data()) + 4 + 1,
                                                socks5_read_buf->at(4)
                                        };
                                        break;
                                    case 0x01:
                                        bindAddr = std::string{
                                                (char *) (socks5_read_buf->data()) + 4,
                                                4
                                        };
                                        break;
                                    case 0x04:
                                        bindAddr = std::string{
                                                (char *) (socks5_read_buf->data()) + 4,
                                                16
                                        };
                                        break;
                                    default:
                                        return fail(ec, "do_socks5_connect_read (socks5_read_buf->at(3) invalid)");
                                }
                                boost::ignore_unused(bindPort);
                                boost::ignore_unused(bindAddr);
                                // if bindPort != 0 , we not support multi-homed socks5 server
                                if (bindPort != 0) {
                                    std::cout <<
                                              "do_socks5_connect_read (bindPort != 0), we not support multi-homed socks5 server"
                                              << std::endl;
                                }
                            }

                            // std::cout << "do_socks5_connect_read()" << std::endl;
                            // socks5 handshake now complete
                            ptr->do_whenUpReady();
                            ptr->do_whenUpEnd();
                        }));

    } else {
        badParentPtr();
    }
}
