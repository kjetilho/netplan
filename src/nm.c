#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "nm.h"
#include "parse.h"
#include "util.h"

GString* udev_rules;

/**
 * Append NM device specifier of @def to @s.
 */
static void
g_string_append_netdef_match(GString* s, const net_definition* def)
{
    g_assert(!def->match.driver || def->set_name);
    if (def->match.mac) {
        g_string_append_printf(s, "mac:%s", def->match.mac);
    } else if (def->match.original_name || def->set_name || def->type >= ND_VIRTUAL) {
        /* we always have the renamed name here */
        g_string_append_printf(s, "interface-name:%s",
                (def->type >= ND_VIRTUAL) ? def->id
                                          : (def->set_name ?: def->match.original_name));
    } else {
        /* no matches → match all devices of that type */
        switch (def->type) {
            case ND_ETHERNET:
                g_string_append(s, "type:ethernet");
                break;
            /* This cannot be reached with just NM and networkd backends, as
             * networkd does not support wifi and thus we'll never blacklist a
             * wifi device from NM. This would become relevant with another
             * wifi-supporting backend, but until then this just spoils 100%
             * code coverage.
            case ND_WIFI:
                g_string_append(s, "type:wifi");
                break;
            */
            default:
                g_assert_not_reached(); /* LCOV_EXCL_LINE */
        }
    }
}

/**
 * Return NM "type=" string.
 */
static const char*
type_str(netdef_type type)
{
    switch (type) {
        case ND_ETHERNET:
            return "ethernet";
        case ND_WIFI:
            return "wifi";
        case ND_BRIDGE:
            return "bridge";
        default:
            g_assert_not_reached(); /* LCOV_EXCL_LINE */
    }
}

/**
 * Return NM wifi "mode=" string.
 */
static const char*
wifi_mode_str(wifi_mode mode)
{
    switch (mode) {
        case WIFI_MODE_INFRASTRUCTURE:
            return "infrastructure";
        case WIFI_MODE_ADHOC:
            return "adhoc";
        case WIFI_MODE_AP:
            return "ap";
        default:
            g_assert_not_reached(); /* LCOV_EXCL_LINE */
    }
}

/**
 * Generate NetworkManager configuration in @rootdir/run/NetworkManager/ for a
 * particular net_definition and wifi_access_point, as NM requires a separate
 * connection file for each SSID.
 * @def: The net_definition for which to create a connection
 * @rootdir: If not %NULL, generate configuration in this root directory
 *           (useful for testing).
 * @ap: The access point for which to create a connection. Must be %NULL for
 *      non-wifi types.
 */
static void
write_nm_conf_access_point(net_definition* def, const char* rootdir, const wifi_access_point* ap)
{
    GString *s = NULL;
    g_autofree char* conf_path = NULL;
    mode_t orig_umask;

    if (def->type == ND_WIFI)
        g_assert(ap);
    else
        g_assert(ap == NULL);

    s = g_string_new(NULL);
    g_string_append_printf(s, "[connection]\nid=ubuntu-network-%s", def->id);
    if (ap)
        g_string_append_printf(s, "-%s", ap->ssid);
    g_string_append_printf(s, "\ntype=%s\n", type_str(def->type));
    if (def->type < ND_VIRTUAL) {
        /* physical (existing) devices use matching; driver matching is not
         * supported, MAC matching is done below (different keyfile section),
         * so only match names here */
        if (def->set_name)
            g_string_append_printf(s, "interface-name=%s\n", def->set_name);
        else if (!def->has_match)
            g_string_append_printf(s, "interface-name=%s\n", def->id);
        else if (def->match.original_name) {
            /* NM does not support interface name globbing */
            if (strpbrk(def->match.original_name, "*[]?")) {
                g_fprintf(stderr, "ERROR: %s: NetworkManager definitions do not support name globbing\n", def->id);
                exit(1);
            }
            g_string_append_printf(s, "interface-name=%s\n", def->match.original_name);
        }
        /* else matches on something other than the name, do not restrict interface-name */
    } else {
        /* virtual (created) devices set a name */
        g_string_append_printf(s, "interface-name=%s\n", def->id);
    }
    if (def->bridge)
        g_string_append_printf(s, "slave-type=bridge\nmaster=%s\n", def->bridge);

    if (def->type < ND_VIRTUAL) {
        g_string_append_printf(s, "\n[ethernet]\nwake-on-lan=%i\n", def->wake_on_lan ? 1 : 0);

        if (!def->set_name && def->match.mac) {
            switch (def->type) {
                case ND_ETHERNET:
                    g_string_append(s, "\n[802-3-ethernet]\n");  break;
                case ND_WIFI:
                    g_string_append(s, "\n[802-11-wireless]\n");  break;
                default:
                    g_assert_not_reached(); /* LCOV_EXCL_LINE */
            }
            g_string_append_printf(s, "mac-address=%s\n", def->match.mac);
        }
    }

    if (def->dhcp4)
        g_string_append(s, "\n[ipv4]\nmethod=auto\n");

    conf_path = g_strjoin(NULL, "run/NetworkManager/system-connections/ubuntu-network-", def->id, NULL);

    if (ap) {
        g_autofree char* escaped_ssid = g_uri_escape_string(ap->ssid, NULL, TRUE);
        conf_path = g_strjoin(NULL, "run/NetworkManager/system-connections/ubuntu-network-", def->id, "-", escaped_ssid, NULL);

        if (ap->mode == WIFI_MODE_AP)
            g_string_append(s, "\n[ipv4]\nmethod=shared\n");

        g_string_append_printf(s, "\n[wifi]\nssid=%s\nmode=%s\n", ap->ssid, wifi_mode_str(ap->mode));
        if (ap->password)
            g_string_append_printf(s, "\n[wifi-security]\nkey-mgmt=wpa-psk\npsk=%s\n", ap->password);
    } else {
        conf_path = g_strjoin(NULL, "run/NetworkManager/system-connections/ubuntu-network-", def->id, NULL);
    }

    /* NM connection files might contain secrets, and NM insists on tight permissions */
    orig_umask = umask(077);
    g_string_free_to_file(s, rootdir, conf_path, NULL);
    umask(orig_umask);
}

/**
 * Generate NetworkManager configuration in @rootdir/run/NetworkManager/ for a
 * particular net_definition.
 * @rootdir: If not %NULL, generate configuration in this root directory
 *           (useful for testing).
 */
void
write_nm_conf(net_definition* def, const char* rootdir)
{
    if (def->backend != BACKEND_NM) {
        g_debug("NetworkManager: definition %s is not for us (backend %i)", def->id, def->backend);
        return;
    }

    if (def->match.driver && !def->set_name) {
        g_fprintf(stderr, "ERROR: %s: NetworkManager definitions do not support matching by driver\n", def->id);
        exit(1);
    }

    /* for wifi we need to create a separate connection file for every SSID */
    if (def->type == ND_WIFI) {
        GHashTableIter iter;
        gpointer key;
        wifi_access_point* ap;
        g_assert(def->access_points);
        g_hash_table_iter_init(&iter, def->access_points);
        while (g_hash_table_iter_next(&iter, &key, (gpointer) &ap))
            write_nm_conf_access_point(def, rootdir, ap);
    } else {
        g_assert(def->access_points == NULL);
        write_nm_conf_access_point(def, rootdir, NULL);
    }
}

static void
nd_append_non_nm_ids(gpointer key, gpointer value, gpointer str)
{
    net_definition* nd = value;

    if (nd->backend != BACKEND_NM) {
        if (nd->match.driver) {
            /* NM cannot match on drivers, so ignore these via udev rules */
            if (!udev_rules)
                udev_rules = g_string_new(NULL);
            g_string_append_printf(udev_rules, "ACTION==\"add|change\", SUBSYSTEM==\"net\", ENV{ID_NET_DRIVER}==\"%s\", ENV{NM_UNMANAGED}=\"1\"\n", nd->match.driver);
        } else {
            g_string_append_netdef_match((GString*) str, nd);
            g_string_append((GString*) str, ",");
        }
    }
}

void
write_nm_conf_finish(const char* rootdir)
{
    GString *s = NULL;
    gsize len;

    if (g_hash_table_size(netdefs) == 0)
        return;

    /* Set all devices not managed by us to unmanaged, so that NM does not
     * auto-connect and interferes */
    s = g_string_new("[keyfile]\n# devices managed by networkd\nunmanaged-devices+=");
    len = s->len;
    g_hash_table_foreach(netdefs, nd_append_non_nm_ids, s);
    if (s->len > len)
        g_string_free_to_file(s, rootdir, "run/NetworkManager/conf.d/ubuntu-network.conf", NULL);
    else
        g_string_free(s, TRUE);

    /* write generated udev rules */
    if (udev_rules)
        g_string_free_to_file(udev_rules, rootdir, "run/udev/rules.d/90-ubuntu-network.rules", NULL);
}