import dbus
import pprint
import sys

bus = dbus.SystemBus()
proxy = bus.get_object("org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager/Settings")
settings = dbus.Interface(proxy, "org.freedesktop.NetworkManager.Settings")
for path in settings.ListConnections():
    con_proxy = bus.get_object("org.freedesktop.NetworkManager", path)
    conn = dbus.Interface(con_proxy, "org.freedesktop.NetworkManager.Settings.Connection")
    settings = conn.GetSettings()
    print("path: %s" % path)
    print("    id: %s" % settings['connection']['id'])
    print("    uuid: %s" % settings['connection']['uuid'])
    print("    type: %s" % settings['connection']['type'])
