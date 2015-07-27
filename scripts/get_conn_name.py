#!/usr/bin/python3
#
# This file is open source software, licensed to you under the terms
# of the Apache License, Version 2.0 (the "License").  See the NOTICE file
# distributed with this work for additional information regarding copyright
# ownership.  You may not use this file except in compliance with the License.
#
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
import dbus
import sys

if len(sys.argv) < 2:
    print("usage: %s [ifname]" % sys.argv[0])
    sys.exit(1)

bus = dbus.SystemBus()
nm_proxy = bus.get_object("org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager")
nm_obj = dbus.Interface(nm_proxy, "org.freedesktop.NetworkManager")
for dev_path in nm_obj.GetDevices():
    dev_proxy = bus.get_object("org.freedesktop.NetworkManager", dev_path)
    dev_obj = dbus.Interface(dev_proxy, "org.freedesktop.DBus.Properties")
    dev_props = dev_obj.GetAll("org.freedesktop.NetworkManager.Device")
    if sys.argv[1] != dev_props['Interface']:
        continue
    conn_proxy = bus.get_object("org.freedesktop.NetworkManager", dev_props['ActiveConnection'])
    conn_obj = dbus.Interface(conn_proxy, "org.freedesktop.DBus.Properties")
    conn_props = conn_obj.GetAll("org.freedesktop.NetworkManager.Connection.Active")
    print(conn_props['Id'])
    break
