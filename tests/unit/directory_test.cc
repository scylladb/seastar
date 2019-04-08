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


#include <seastar/core/reactor.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/print.hh>
#include <seastar/core/shared_ptr.hh>

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
    assert(0 && "should not get here");
    return nullptr;
}

int main(int ac, char** av) {
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
                    assert(*de.type == sd.type);
                } else {
                    assert(sd.type == directory_entry_type::unknown);
                }
                fmt::print("{} (type={})\n", de.name, de_type_desc(sd.type));
                return make_ready_future<>();
            });
        }
    };
    return app_template().run_deprecated(ac, av, [] {
        return engine().open_directory(".").then([] (file f) {
            auto l = make_lw_shared<lister>(std::move(f));
            return l->done().then([l] {
                // ugly thing to keep *l alive
                engine().exit(0);
            });
        });
    });
}
