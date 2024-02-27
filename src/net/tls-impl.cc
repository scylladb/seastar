/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2015 Cloudius Systems
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <boost/any.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/range/adaptor/map.hpp>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include "net/tls-impl.hh"
#include <seastar/net/tls.hh>
#include <seastar/core/file.hh>
#include <seastar/core/fsnotify.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/reactor.hh>


#endif

namespace seastar {

// Helper
struct file_info {
    sstring filename;
    std::chrono::system_clock::time_point modified;
};

struct file_result {
    temporary_buffer<char> buf;
    file_info file;
    operator temporary_buffer<char>&&() && {
        return std::move(buf);
    }
};

static future<file_result> read_fully(const sstring& name, const sstring& what) {
    return open_file_dma(name, open_flags::ro).then([name = name](file f) mutable {
        return do_with(std::move(f), [name = std::move(name)](file& f) mutable {
            return f.stat().then([&f, name = std::move(name)](struct stat s) mutable {
                return f.dma_read_bulk<char>(0, s.st_size).then([s, name = std::move(name)](temporary_buffer<char> buf) mutable {
                    return file_result{ std::move(buf), file_info{
                        std::move(name), std::chrono::system_clock::from_time_t(s.st_mtim.tv_sec) +
                            std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(s.st_mtim.tv_nsec))
                    } };
                });
            }).finally([&f]() {
                return f.close();
            });
        });
    }).handle_exception([name = name, what = what](std::exception_ptr ep) -> future<file_result> {
       try {
           std::rethrow_exception(std::move(ep));
       } catch (...) {
           std::throw_with_nested(std::runtime_error(sstring("Could not read ") + what + " " + name));
       }
    });
}

future<tls::dh_params> tls::dh_params::from_file(
        const sstring& filename, x509_crt_format fmt) {
    return read_fully(filename, "dh parameters").then([fmt](temporary_buffer<char> buf) {
        return make_ready_future<dh_params>(dh_params(blob(buf.get()), fmt));
    });
}

future<> tls::abstract_credentials::set_x509_trust_file(
        const sstring& cafile, x509_crt_format fmt) {
    return read_fully(cafile, "trust file").then([this, fmt](temporary_buffer<char> buf) {
        set_x509_trust(blob(buf.get(), buf.size()), fmt);
    });
}

future<> tls::abstract_credentials::set_x509_crl_file(
        const sstring& crlfile, x509_crt_format fmt) {
    return read_fully(crlfile, "crl file").then([this, fmt](temporary_buffer<char> buf) {
        set_x509_crl(blob(buf.get(), buf.size()), fmt);
    });
}

future<> tls::abstract_credentials::set_x509_key_file(
        const sstring& cf, const sstring& kf, x509_crt_format fmt) {
    return read_fully(cf, "certificate file").then([this, fmt, kf = kf](temporary_buffer<char> buf) {
        return read_fully(kf, "key file").then([this, fmt, buf = std::move(buf)](temporary_buffer<char> buf2) {
                    set_x509_key(blob(buf.get(), buf.size()), blob(buf2.get(), buf2.size()), fmt);
                });
    });
}

future<> tls::abstract_credentials::set_simple_pkcs12_file(
        const sstring& pkcs12file, x509_crt_format fmt,
        const sstring& password) {
    return read_fully(pkcs12file, "pkcs12 file").then([this, fmt, password = password](temporary_buffer<char> buf) {
        set_simple_pkcs12(blob(buf.get(), buf.size()), fmt, password);
    });
}

static const sstring dh_level_key = "dh_level";
static const sstring x509_trust_key = "x509_trust";
static const sstring x509_crl_key = "x509_crl";
static const sstring x509_key_key = "x509_key";
static const sstring pkcs12_key = "pkcs12";
static const sstring system_trust = "system_trust";

using buffer_type = std::basic_string<tls::blob::value_type, tls::blob::traits_type, std::allocator<tls::blob::value_type>>;

struct x509_simple {
    buffer_type data;
    tls::x509_crt_format format;
    file_info file;
};

struct x509_key {
    buffer_type cert;
    buffer_type key;
    tls::x509_crt_format format;
    file_info cert_file;
    file_info key_file;
};

struct pkcs12_simple {
    buffer_type data;
    tls::x509_crt_format format;
    sstring password;
    file_info file;
};

void tls::credentials_builder::set_dh_level(dh_params::level level) {
    _blobs.emplace(dh_level_key, level);
}

void tls::credentials_builder::set_x509_trust(const blob& b, x509_crt_format fmt) {
    _blobs.emplace(x509_trust_key, x509_simple{ std::string(b), fmt });
}

void tls::credentials_builder::set_x509_crl(const blob& b, x509_crt_format fmt) {
    _blobs.emplace(x509_crl_key, x509_simple{ std::string(b), fmt });
}

void tls::credentials_builder::set_x509_key(const blob& cert, const blob& key, x509_crt_format fmt) {
    _blobs.emplace(x509_key_key, x509_key { std::string(cert), std::string(key), fmt });
}

void tls::credentials_builder::set_simple_pkcs12(const blob& b, x509_crt_format fmt, const sstring& password) {
    _blobs.emplace(pkcs12_key, pkcs12_simple{std::string(b), fmt, password });
}

static buffer_type to_buffer(const temporary_buffer<char>& buf) {
    return buffer_type(buf.get(), buf.get() + buf.size());
}

future<> tls::credentials_builder::set_x509_trust_file(const sstring& cafile, x509_crt_format fmt) {
    return read_fully(cafile, "trust file").then([this, fmt](file_result f) {
        _blobs.emplace(x509_trust_key, x509_simple{ to_buffer(f.buf), fmt, std::move(f.file) });
    });
}

future<> tls::credentials_builder::set_x509_crl_file(const sstring& crlfile, x509_crt_format fmt) {
    return read_fully(crlfile, "crl file").then([this, fmt](file_result f) {
        _blobs.emplace(x509_crl_key, x509_simple{ to_buffer(f.buf), fmt, std::move(f.file) });
    });
}

future<> tls::credentials_builder::set_x509_key_file(const sstring& cf, const sstring& kf, x509_crt_format fmt) {
    return read_fully(cf, "certificate file").then([this, fmt, kf = kf](file_result cf) {
        return read_fully(kf, "key file").then([this, fmt, cf = std::move(cf)](file_result kf) {
            _blobs.emplace(x509_key_key, x509_key{ to_buffer(cf.buf), to_buffer(kf.buf), fmt, std::move(cf.file), std::move(kf.file) });
        });
    });
}

future<> tls::credentials_builder::set_simple_pkcs12_file(const sstring& pkcs12file, x509_crt_format fmt, const sstring& password) {
    return read_fully(pkcs12file, "pkcs12 file").then([this, fmt, password = password](file_result f) {
        _blobs.emplace(pkcs12_key, pkcs12_simple{ to_buffer(f.buf), fmt, password, std::move(f.file) });
    });
}

future<> tls::credentials_builder::set_system_trust() {
    // TODO / Caveat:
    // We cannot actually issue a loading of system trust here,
    // because we have no actual tls context.
    // And we probably _don't want to get into the guessing game
    // of where the system trust cert chains are, since this is
    // super distro dependent, and usually compiled into the library.
    // Pretent it is raining, and just set a flag.
    // Leave the function returning future, so if we change our
    // minds and want to do explicit loading, we can...
    _blobs.emplace(system_trust, true);
    return make_ready_future();
}

void tls::credentials_builder::set_client_auth(client_auth auth) {
    _client_auth = auth;
}

void tls::credentials_builder::set_priority_string(const sstring& prio) {
    _priority = prio;
}

template<typename Blobs, typename Visitor>
static void visit_blobs(Blobs& blobs, Visitor&& visitor) {
    auto visit = [&](const sstring& key, auto* vt) {
        auto tr = blobs.equal_range(key);
        for (auto& p : boost::make_iterator_range(tr.first, tr.second)) {
            auto* v = boost::any_cast<std::decay_t<decltype(*vt)>>(&p.second);
            visitor(key, *v);
        }
    };
    visit(x509_trust_key, static_cast<x509_simple*>(nullptr));
    visit(x509_crl_key, static_cast<x509_simple*>(nullptr));
    visit(x509_key_key, static_cast<x509_key*>(nullptr));
    visit(pkcs12_key, static_cast<pkcs12_simple*>(nullptr));
}

void tls::credentials_builder::apply_to(certificate_credentials& creds) const {
    // Could potentially be templated down, but why bother...
    visit_blobs(_blobs, make_visitor(
        [&](const sstring& key, const x509_simple& info) {
            if (key == x509_trust_key) {
                creds.set_x509_trust(info.data, info.format);
            } else if (key == x509_crl_key) {
                creds.set_x509_crl(info.data, info.format);
            }
        },
        [&](const sstring&, const x509_key& info) {
            creds.set_x509_key(info.cert, info.key, info.format);
        },
        [&](const sstring&, const pkcs12_simple& info) {
            creds.set_simple_pkcs12(info.data, info.format, info.password);
        }
    ));

    // TODO / Caveat:
    // We cannot do this immediately, because we are not a continuation, and
    // potentially blocking calls are a no-no.
    // Doing this detached would be indeterministic, so set a flag in
    // credentials, and do actual loading in first handshake (see session)
    if (_blobs.count(system_trust)) {
        creds._impl->_load_system_trust = true;
    }

    if (!_priority.empty()) {
        creds.set_priority_string(_priority);
    }

    creds._impl->set_client_auth(_client_auth);
}

shared_ptr<tls::certificate_credentials> tls::credentials_builder::build_certificate_credentials() const {
    auto creds = make_shared<certificate_credentials>();
    apply_to(*creds);
    return creds;
}

shared_ptr<tls::server_credentials> tls::credentials_builder::build_server_credentials() const {
    auto i = _blobs.find(dh_level_key);
    if (i == _blobs.end()) {
#if GNUTLS_VERSION_NUMBER < 0x030600
        throw std::invalid_argument("No DH level set");
#else
        auto creds = make_shared<server_credentials>();
        apply_to(*creds);
        return creds;
#endif
    }
    auto creds = make_shared<server_credentials>(dh_params(boost::any_cast<dh_params::level>(i->second)));
    apply_to(*creds);
    return creds;
}

using namespace std::chrono_literals;

class tls::reloadable_credentials_base {
public:
    using delay_type = std::chrono::milliseconds;
    static inline constexpr delay_type default_tolerance = 500ms;

    class reloading_builder
        : public credentials_builder
        , public enable_shared_from_this<reloading_builder>
    {
    public:
        using time_point = std::chrono::system_clock::time_point;

        reloading_builder(credentials_builder b, reload_callback_with_creds cb, reloadable_credentials_base* creds, delay_type delay)
            : credentials_builder(std::move(b))
            , _cb(std::move(cb))
            , _creds(creds)
            , _delay(delay)
        {}
        future<> init() {
            std::vector<future<>> futures;
            visit_blobs(_blobs, make_visitor(
                [&](const sstring&, const x509_simple& info) {
                    _all_files.emplace(info.file.filename);
                },
                [&](const sstring&, const x509_key& info) {
                    _all_files.emplace(info.cert_file.filename);
                    _all_files.emplace(info.key_file.filename);
                },
                [&](const sstring&, const pkcs12_simple& info) {
                    _all_files.emplace(info.file.filename);
                }
            ));
            return parallel_for_each(_all_files, [this](auto& f) {
                if (!f.empty()) {
                    return add_watch(f).discard_result();
                }
                return make_ready_future<>();
            }).finally([me = shared_from_this()] {});
        }
        void start() {
            // run the loop in a thread. makes code almost readable.
            (void)async(std::bind(&reloading_builder::run, this)).finally([me = shared_from_this()] {});
        }
        void run() {
            while (_creds) {
                try {
                    auto events = _fsn.wait().get();
                    if (events.empty() && _creds == nullptr) {
                        return;
                    }
                    rebuild(events);
                    _timer.cancel();
                } catch (...) {
                    if (!_timer.armed()) {
                        _timer.set_callback([this, ep = std::current_exception()]() mutable {
                            do_callback(std::move(ep));
                        });
                        _timer.arm(_delay);
                    }
                }
            }
        }
        void detach() {
            _creds = nullptr;
            _cb = {};
            _fsn.shutdown();
            _timer.cancel();
        }

    private:
        using fsnotifier = experimental::fsnotifier;

        // called from seastar::thread
        void rebuild(const std::vector<fsnotifier::event>& events) {
            for (auto& e : events) {
                // don't use at. We could be getting two events for
                // same watch (mod + delete), but we only need to care
                // about one...
                auto i = _watches.find(e.id);
                if (i != _watches.end()) {
                    auto& filename = i->second.second;
                    // only add actual file watches to
                    // query set. If this was a directory
                    // watch, the file should already be
                    // in there.
                    if (_all_files.count(filename)) {
                        _files[filename] = e.mask;
                    }
                    _watches.erase(i);
                }
            }
            auto num_changed = 0;

            auto maybe_reload = [&](const sstring& filename, buffer_type& dst) {
                if (filename.empty() || !_files.count(filename)) {
                    return;
                }
                // #756
                // first, add a watch to nearest parent dir we
                // can find. If user deleted folders, we could end
                // up looking at modifications to root.
                // The idea is that should adding a watch to actual file
                // fail (deleted file/folder), we wait for changes to closest
                // parent. When this happens, we will retry all files
                // that have not been successfully replaced (and maybe more),
                // repeating the process. At some point, we hopefully
                // get new, current data.
                add_dir_watch(filename);
                // #756 add watch _first_. File could change while we are
                // reading this.
                try {
                    add_watch(filename).get();
                } catch (...) {
                    // let's just assume if this happens, it's because the file or folder was deleted.
                    // just ignore for now, and hope the dir watch will tell us when it is back...
                    return;
                }
                temporary_buffer<char> buf = read_fully(filename, "reloading").get();
                dst = to_buffer(buf);
                ++num_changed;
            };
            visit_blobs(_blobs, make_visitor(
                [&](const sstring&, x509_simple& info) {
                    maybe_reload(info.file.filename, info.data);
                },
                [&](const sstring&, x509_key& info) {
                    maybe_reload(info.cert_file.filename, info.cert);
                    maybe_reload(info.key_file.filename, info.key);
                },
                [&](const sstring&, pkcs12_simple& info) {
                    maybe_reload(info.file.filename, info.data);
                }
            ));
            // only try this if anything was in fact successfully loaded.
            // if files were missing, or pairs incomplete, we can just skip.
            if (num_changed == 0) {
                return;
            }
            try {
                if (_creds) {
                    _creds->rebuild(*this);
                }
            } catch (...) {
                if (std::any_of(_files.begin(), _files.end(), [](auto& p) { return p.second == fsnotifier::flags::ignored; })) {
                    // if any file in the reload set was deleted - i.e. we have not seen a "closed" yet - assume
                    // this is a spurious reload and we'd better wait for next event - hopefully a "closed" -
                    // and try again
                    return;
                }
                throw;
            }
            // if we got here, all files loaded, all watches were created,
            // and gnutls was ok with the content. success.
            do_callback();
            on_success();
        }
        void on_success() {
            _files.clear();
            // remove all directory watches, since we've successfully
            // reloaded -> the file watches themselves should suffice now
            auto i = _watches.begin();
            auto e = _watches.end();
            while (i != e) {
                if (!_all_files.count(i->second.second)) {
                    i = _watches.erase(i);
                    continue;
                }
                ++i;
            }
        }
        void do_callback(std::exception_ptr ep = {}) {
            if (_cb && !_files.empty() && _creds) {
                const auto &creds = _creds->as_certificate_credentials();
                _cb(boost::copy_range<std::unordered_set<sstring>>(_files | boost::adaptors::map_keys), creds, std::move(ep), get_trust_file_blob());
            }
        }
        // called from seastar::thread
        fsnotifier::watch_token add_dir_watch(const sstring& filename) {
            auto dir = std::filesystem::path(filename).parent_path();
            for (;;) {
                try {
                    return add_watch(dir.native(), fsnotifier::flags::create_child | fsnotifier::flags::move).get();
                } catch (...) {
                    auto parent = dir.parent_path();
                    if (parent.empty() || dir == parent) {
                        throw;
                    }
                    dir = std::move(parent);
                    continue;
                }
            }
        }
        future<fsnotifier::watch_token> add_watch(const sstring& filename, fsnotifier::flags flags = fsnotifier::flags::close_write|fsnotifier::flags::delete_self) {
            return _fsn.create_watch(filename, flags).then([this, filename = filename](fsnotifier::watch w) {
                auto t = w.token();
                // we might create multiple watches for same token in case of dirs, avoid deleting previously
                // created one
                if (_watches.count(t)) {
                    w.release();
                } else {
                    _watches.emplace(t, std::make_pair(std::move(w), filename));
                }
                return t;
            });
        }

        reload_callback_with_creds _cb;
        reloadable_credentials_base* _creds;
        fsnotifier _fsn;
        std::unordered_map<fsnotifier::watch_token, std::pair<fsnotifier::watch, sstring>> _watches;
        std::unordered_map<sstring, fsnotifier::flags> _files;
        std::unordered_set<sstring> _all_files;
        timer<> _timer;
        delay_type _delay;
    };
    reloadable_credentials_base(credentials_builder builder, reload_callback_with_creds cb, delay_type delay = default_tolerance)
        : _builder(seastar::make_shared<reloading_builder>(std::move(builder), std::move(cb), this, delay))
    {
        _builder->start();
    }
    future<> init() {
        return _builder->init();
    }
    virtual ~reloadable_credentials_base() {
        _builder->detach();
    }
    virtual void rebuild(const credentials_builder&) = 0;
    virtual const tls::certificate_credentials& as_certificate_credentials() const noexcept = 0;
private:
    shared_ptr<reloading_builder> _builder;
};

template<typename Base>
class tls::reloadable_credentials : public Base, public tls::reloadable_credentials_base {
public:
    reloadable_credentials(credentials_builder builder, reload_callback_with_creds cb, Base b, delay_type delay = default_tolerance)
        : Base(std::move(b))
        , tls::reloadable_credentials_base(std::move(builder), std::move(cb), delay)
    {}
    void rebuild(const credentials_builder&) override;
    const tls::certificate_credentials& as_certificate_credentials() const noexcept override;

};

template<>
void tls::reloadable_credentials<tls::certificate_credentials>::rebuild(const credentials_builder& builder) {
    auto tmp = builder.build_certificate_credentials();
    this->_impl = std::move(tmp->_impl);
}

template <>
const tls::certificate_credentials& tls::reloadable_credentials<tls::certificate_credentials>::as_certificate_credentials() const noexcept {
    return *this;
}

template<>
void tls::reloadable_credentials<tls::server_credentials>::rebuild(const credentials_builder& builder) {
    auto tmp = builder.build_server_credentials();
    this->_impl = std::move(tmp->_impl);
}

template <>
const tls::certificate_credentials& tls::reloadable_credentials<tls::server_credentials>::as_certificate_credentials() const noexcept{
    return *this;
}

future<shared_ptr<tls::certificate_credentials>> tls::credentials_builder::build_reloadable_certificate_credentials(reload_callback cb, std::optional<std::chrono::milliseconds> tolerance) const {
    return build_reloadable_certificate_credentials(wrap_reload_callback(std::move(cb)), tolerance);
}

future<shared_ptr<tls::certificate_credentials>> tls::credentials_builder::build_reloadable_certificate_credentials(reload_callback_with_creds cb, std::optional<std::chrono::milliseconds> tolerance) const {
    auto creds = seastar::make_shared<reloadable_credentials<tls::certificate_credentials>>(*this, std::move(cb), std::move(*build_certificate_credentials()), tolerance.value_or(reloadable_credentials_base::default_tolerance));
    return creds->init().then([creds] {
        return make_ready_future<shared_ptr<tls::certificate_credentials>>(creds);
    });
}

future<shared_ptr<tls::server_credentials>> tls::credentials_builder::build_reloadable_server_credentials(reload_callback cb, std::optional<std::chrono::milliseconds> tolerance) const {
    return build_reloadable_server_credentials(wrap_reload_callback(std::move(cb)), tolerance);
}

future<shared_ptr<tls::server_credentials>> tls::credentials_builder::build_reloadable_server_credentials(reload_callback_with_creds cb, std::optional<std::chrono::milliseconds> tolerance) const {
    auto creds = seastar::make_shared<reloadable_credentials<tls::server_credentials>>(*this, std::move(cb), std::move(*build_server_credentials()), tolerance.value_or(reloadable_credentials_base::default_tolerance));
    return creds->init().then([creds] {
        return make_ready_future<shared_ptr<tls::server_credentials>>(creds);
    });
}

std::optional<tls::blob> tls::credentials_builder::get_trust_file_blob() const {
    if (auto i = _blobs.find(x509_trust_key); i != _blobs.end()) {
        return std::make_optional<tls::blob>(boost::any_cast<const x509_simple&>(i->second).data);
    }
    return std::nullopt;
}

data_source tls::tls_connected_socket_impl::source() {
    return data_source(std::make_unique<source_impl>(_session));
}

data_sink tls::tls_connected_socket_impl::sink() {
    return data_sink(std::make_unique<sink_impl>(_session));
}

future<connected_socket> tls::connect(shared_ptr<certificate_credentials> cred, socket_address sa, sstring name) {
    tls_options options{.server_name = std::move(name)};
    return connect(std::move(cred), std::move(sa), std::move(options));
}

future<connected_socket> tls::connect(shared_ptr<certificate_credentials> cred, socket_address sa, socket_address local, sstring name) {
    tls_options options{.server_name = std::move(name)};
    return connect(std::move(cred), std::move(sa), std::move(local), std::move(options));
}

future<connected_socket> tls::connect(shared_ptr<certificate_credentials> cred, socket_address sa, tls_options options) {
    return engine().connect(sa).then([cred = std::move(cred), options = std::move(options)](connected_socket s) mutable {
        return wrap_client(std::move(cred), std::move(s), std::move(options));
    });
}

future<connected_socket> tls::connect(shared_ptr<certificate_credentials> cred, socket_address sa, socket_address local, tls_options options) {
    return engine().connect(sa, local).then([cred = std::move(cred), options = std::move(options)](connected_socket s) mutable {
        return wrap_client(std::move(cred), std::move(s), std::move(options));
    });
}

socket tls::socket(shared_ptr<certificate_credentials> cred, sstring name) {
    tls_options options{.server_name = std::move(name)};
    return tls::socket(std::move(cred), std::move(options));
}

socket tls::socket(shared_ptr<certificate_credentials> cred, tls_options options) {
    return ::seastar::socket(std::make_unique<tls_socket_impl>(std::move(cred), std::move(options)));
}

server_socket tls::listen(shared_ptr<server_credentials> creds, socket_address sa, listen_options opts) {
    return listen(std::move(creds), seastar::listen(sa, opts));
}

server_socket tls::listen(shared_ptr<server_credentials> creds, server_socket ss) {
    server_socket ssls(std::make_unique<server_session>(creds, std::move(ss)));
    return server_socket(std::move(ssls));
}

static tls::tls_connected_socket_impl* get_tls_socket(connected_socket& socket) {
    auto impl = net::get_impl::maybe_get_ptr(socket);
    if (impl == nullptr) {
        // the socket is not yet created or moved from
        throw std::system_error(ENOTCONN, std::system_category());
    }
    auto tls_impl = dynamic_cast<tls::tls_connected_socket_impl*>(impl);
    if (!tls_impl) {
        // bad cast here means that we're dealing with wrong socket type
        throw std::invalid_argument("Not a TLS socket");
    }
    return tls_impl;
}

future<std::optional<session_dn>> tls::get_dn_information(connected_socket& socket) {
    return get_tls_socket(socket)->get_distinguished_name();
}

future<std::vector<tls::subject_alt_name>> tls::get_alt_name_information(connected_socket& socket, std::unordered_set<subject_alt_name_type> types) {
    return get_tls_socket(socket)->get_alt_name_information(std::move(types));
}

std::string_view tls::format_as(subject_alt_name_type type) {
    switch (type) {
        case subject_alt_name_type::dnsname:
            return "DNS";
        case subject_alt_name_type::rfc822name:
            return "EMAIL";
        case subject_alt_name_type::uri:
            return "URI";
        case subject_alt_name_type::ipaddress:
            return "IP";
        case subject_alt_name_type::othername:
            return "OTHERNAME";
        case subject_alt_name_type::dn:
            return "DIRNAME";
        default:
            return "UNKNOWN";
    }
}

std::ostream& tls::operator<<(std::ostream& os, subject_alt_name_type type) {
    return os << format_as(type);
}

std::ostream& tls::operator<<(std::ostream& os, const subject_alt_name::value_type& v) {
    fmt::print(os, "{}", v);
    return os;
}

std::ostream& tls::operator<<(std::ostream& os, const subject_alt_name& a) {
    fmt::print(os, "{}", a);
    return os;
}

}
