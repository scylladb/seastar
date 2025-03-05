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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include <seastar/util/std-compat.hh>
#ifdef SEASTAR_COROUTINES_ENABLED
#include <seastar/core/coroutine.hh>
#endif
#include <seastar/core/reactor.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/print.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/file.hh>
#include <seastar/util/assert.hh>

using namespace seastar;

const char* de_type_desc(directory_entry_type t)
{
    switch (t) {
    case directory_entry_type::unknown:
        return "unknown";
    case directory_entry_type::block_device:
        return "block_device";
    case directory_entry_type::char_device:
        return "char_device";
    case directory_entry_type::directory:
        return "directory";
    case directory_entry_type::fifo:
        return "fifo";
    case directory_entry_type::link:
        return "link";
    case directory_entry_type::regular:
        return "regular";
    case directory_entry_type::socket:
        return "socket";
    }
    SEASTAR_ASSERT(0 && "should not get here");
    return nullptr;
}

future<> lister_test() {
    class lister {
        file _f;
        subscription<directory_entry> _listing;
    public:
        lister(file f)
                : _f(std::move(f))
                , _listing(_f.list_directory([this] (directory_entry de) { return report(de); })) {
        }
        future<> done() { return _listing.done(); }
    private:
        future<> report(directory_entry de) {
            return file_stat(de.name, follow_symlink::no).then([de = std::move(de)] (stat_data sd) {
                if (de.type) {
                    SEASTAR_ASSERT(*de.type == sd.type);
                } else {
                    SEASTAR_ASSERT(sd.type == directory_entry_type::unknown);
                }
                fmt::print("{} (type={})\n", de.name, de_type_desc(sd.type));
                return make_ready_future<>();
            });
        }
    };
    fmt::print("--- Regular lister test ---\n");
    return engine().open_directory(".").then([] (file f) {
        return do_with(lister(std::move(f)), [] (lister& l) {
          return l.done();
       });
    });
}

future<> lister_generator_test(file f) {
    auto lister = f.experimental_list_directory();
    while (auto de = co_await lister()) {
        auto sd = co_await file_stat(de->name, follow_symlink::no);
        if (de->type) {
            SEASTAR_ASSERT(*de->type == sd.type);
        } else {
            SEASTAR_ASSERT(sd.type == directory_entry_type::unknown);
        }
        fmt::print("{} (type={})\n", de->name, de_type_desc(sd.type));
    }
    co_await f.close();
}

class test_file_impl : public file_impl {
    file _lower;
public:
    test_file_impl(file&& f) : _lower(std::move(f)) {}

    virtual future<> flush() override { return get_file_impl(_lower)->flush(); }
    virtual future<struct stat> stat() override { return get_file_impl(_lower)->stat(); }
    virtual future<> truncate(uint64_t length) override { return get_file_impl(_lower)->truncate(length); }
    virtual future<> discard(uint64_t offset, uint64_t length) override { return get_file_impl(_lower)->discard(offset, length); }
    virtual future<> allocate(uint64_t position, uint64_t length) override { return get_file_impl(_lower)->allocate(position, length); }
    virtual future<uint64_t> size() override { return get_file_impl(_lower)->size(); }
    virtual future<> close() override { return _lower.close(); }
    virtual subscription<directory_entry> list_directory(std::function<future<> (directory_entry de)> next) override { return get_file_impl(_lower)->list_directory(std::move(next)); }
    // ! no override for generator list_directory, so that fallback is used

    virtual future<size_t> write_dma(uint64_t pos, const void* buffer, size_t len, io_intent* i) override { return get_file_impl(_lower)->write_dma(pos, buffer, len, i); }
    virtual future<size_t> write_dma(uint64_t pos, std::vector<iovec> iov, io_intent* i) override { return get_file_impl(_lower)->write_dma(pos, std::move(iov), i); }
    virtual future<size_t> read_dma(uint64_t pos, void* buffer, size_t len, io_intent* i) override { return get_file_impl(_lower)->read_dma(pos, buffer, len, i); }
    virtual future<size_t> read_dma(uint64_t pos, std::vector<iovec> iov, io_intent* i) override { return get_file_impl(_lower)->read_dma(pos, std::move(iov), i); }
    virtual future<temporary_buffer<uint8_t>> dma_read_bulk(uint64_t offset, size_t range_size, io_intent* i) override { return get_file_impl(_lower)->dma_read_bulk(offset, range_size, i); }
};

future<> lister_generator_test() {
    fmt::print("--- Generator lister test ---\n");
    auto f = co_await engine().open_directory(".");
    co_await lister_generator_test(std::move(f));

    fmt::print("--- Generator fallback test ---\n");
    auto lf = co_await engine().open_directory(".");
    auto tf = ::seastar::make_shared<test_file_impl>(std::move(lf));
    auto f2 = file(std::move(tf));
    co_await lister_generator_test(std::move(f2));
}

int main(int ac, char** av) {
    return app_template().run(ac, av, [] {
        return lister_test().then([] {
            return lister_generator_test();
        });
    });
}
