/*
 * Copyright (C) 2015-2019 Gooroom <gooroom@gooroom.kr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Author: Thomas Wood <thomas.wood@intel.com>
 *
 */

#include <sys/stat.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include <pwd.h>
#include <stdlib.h>
#include <json-c/json.h>

#include "panel-glib.h"


static gchar *
find_full_desktop_id (const gchar *find_str)
{
	gchar *ret = NULL;
	GList *all_apps = NULL, *l = NULL;

	if (!find_str || g_str_equal (find_str, ""))
		return NULL;

	all_apps = g_app_info_get_all ();

	for (l = all_apps; l; l = l->next) {
		GAppInfo *appinfo = G_APP_INFO (l->data);
		if (!appinfo)
			continue;

		const gchar *id = NULL;
		GDesktopAppInfo *dt_info;
		gchar *locale_name = NULL, *name = NULL, *exec = NULL;

		dt_info = G_DESKTOP_APP_INFO (appinfo);

		id = g_app_info_get_id (appinfo);
		if (g_str_equal (id, find_str))
			goto find;

		name = g_desktop_app_info_get_string (dt_info, G_KEY_FILE_DESKTOP_KEY_NAME);
		if (name && ((g_utf8_collate (name, find_str) == 0) || (panel_g_utf8_strstrcase (name, find_str) != NULL))) {
			goto find;
		}

		locale_name = g_desktop_app_info_get_locale_string (dt_info, G_KEY_FILE_DESKTOP_KEY_NAME);
		if (locale_name && ((g_utf8_collate (locale_name, find_str) == 0) || (panel_g_utf8_strstrcase (locale_name, find_str) != NULL))) {
			goto find;
		}

		exec = g_desktop_app_info_get_string (dt_info, G_KEY_FILE_DESKTOP_KEY_EXEC);
		if (exec && ((g_utf8_collate (exec, find_str) == 0) || (panel_g_utf8_strstrcase (exec, find_str) != NULL))) {
			goto find;
		}
		continue;

find:
		ret = g_strdup (g_desktop_app_info_get_filename (dt_info));

		g_free (name);
		g_free (locale_name);
		g_free (exec);
		break;
	}
	g_list_free_full (all_apps, g_object_unref);

	return ret;
}

static void
make_exec_executable (const gchar *full_desktop_id, gboolean executable)
{
	g_return_if_fail (full_desktop_id != NULL);

	gchar *exec = NULL;
	gboolean changed = FALSE;
	GDesktopAppInfo *dt_appinfo;

	dt_appinfo = g_desktop_app_info_new_from_filename (full_desktop_id);
	if (!dt_appinfo) {
		g_warning ("Cannot load desktop entry: %s", full_desktop_id);
		return;
	}

	exec = g_desktop_app_info_get_string (dt_appinfo, G_KEY_FILE_DESKTOP_KEY_EXEC);

	if (exec) {
		GStatBuf stat_buf;
		gchar **argv = NULL;
		GError *error = NULL;

		if (!g_shell_parse_argv (exec, NULL, &argv, &error)) {
			g_warning ("Cannot parse Exec for %s: %s", full_desktop_id, error->message);
			g_clear_error (&error);
			g_free (exec);
			g_object_unref (dt_appinfo);
			return;
		}

		if (g_strv_length (argv) > 0) {
			gchar *cmd = g_find_program_in_path (argv[0]);

			if (cmd && g_stat (cmd, &stat_buf) == 0) {
				mode_t perm = stat_buf.st_mode;
				gboolean cur_exec = perm & S_IXOTH;

				if (executable) {
					if (!cur_exec) {
						g_chmod (cmd, perm | S_IXOTH);
						changed = TRUE;
					}
				} else {
					if (cur_exec) {
						g_chmod (cmd, perm & ~(S_IXOTH));
						changed = TRUE;
					}
				}
			}
			g_free (cmd);
		}
		g_strfreev (argv);
		g_free (exec);
	}

	if (changed) {
		/* For updating libgnome-menu cache */
		GKeyFile *keyfile = g_key_file_new ();
		if (g_key_file_load_from_file (keyfile, full_desktop_id,
					G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, NULL)) {
			g_key_file_save_to_file (keyfile, full_desktop_id, NULL);
		}
		g_key_file_free (keyfile);
	}

	g_object_unref (dt_appinfo);
}

static void
init_blacklist (void)
{
	GList *all_apps = NULL, *l = NULL;

	all_apps = g_app_info_get_all ();

	for (l = all_apps; l; l = l->next) {
		GAppInfo *appinfo = G_APP_INFO (l->data);
		if (appinfo) {
			const gchar *full_desktop_id = g_desktop_app_info_get_filename (G_DESKTOP_APP_INFO (appinfo));

			/* don't care gooroomupdate.desktop */
			if (g_str_has_suffix (full_desktop_id, "gooroomupdate.desktop"))
				continue;

			/* restore exec permission */
			make_exec_executable (full_desktop_id, TRUE);
		}
	}

	g_list_free_full (all_apps, g_object_unref);
}

/* This helper runs as root through pkexec, so the list to apply is never
 * taken from the caller: it is fetched from the root agent, which got it
 * from the management server. */
static gchar *
fetch_blacklist (void)
{
	GDBusProxy *proxy;
	GVariant *reply, *v = NULL;
	gchar *arg, *data = NULL, *ret = NULL;
	const gchar *uid_str = g_getenv ("PKEXEC_UID");
	struct passwd *pw = uid_str ? getpwuid ((uid_t) strtoul (uid_str, NULL, 10)) : NULL;

	if (!pw)
		return NULL;

	proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
	                                       "kr.gooroom.agent", "/kr/gooroom/agent",
	                                       "kr.gooroom.agent", NULL, NULL);
	if (!proxy)
		return NULL;

	arg = g_strdup_printf ("{\"module\":{\"module_name\":\"config\",\"task\":{\"task_name\":\"get_app_list\",\"in\":{\"login_id\":\"%s\"}}}}",
	                       pw->pw_name);
	reply = g_dbus_proxy_call_sync (proxy, "do_task", g_variant_new ("(v)", g_variant_new_string (arg)),
	                                G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
	g_free (arg);
	g_object_unref (proxy);

	if (!reply)
		return NULL;
	g_variant_get (reply, "(v)", &v);
	if (v && g_variant_is_of_type (v, G_VARIANT_TYPE_STRING))
		data = g_variant_dup_string (v, NULL);
	if (v)
		g_variant_unref (v);
	g_variant_unref (reply);

	if (data) {
		json_object *root = json_tokener_parse (data), *mod, *task, *out, *status, *list;
		if (root &&
		    json_object_object_get_ex (root, "module", &mod) &&
		    json_object_object_get_ex (mod, "task", &task) &&
		    json_object_object_get_ex (task, "out", &out) &&
		    json_object_object_get_ex (out, "status", &status) &&
		    g_strcmp0 (json_object_get_string (status), "200") == 0 &&
		    json_object_object_get_ex (out, "black_list", &list)) {
			ret = g_strdup (json_object_get_string (list));
		}
		if (root)
			json_object_put (root);
		g_free (data);
	}

	return ret;
}

int
main (int argc, char **argv)
{
	gchar *list = fetch_blacklist ();

	/* no policy from the agent: change nothing */
	if (!list)
		return 1;

	init_blacklist ();

	gchar **items = g_strsplit (list, ",", -1);
	guint i;
	for (i = 0; items[i]; i++) {
		gchar *full_desktop_id;
		if (items[i][0] == '\0')
			continue;
		full_desktop_id = find_full_desktop_id (items[i]);
		if (full_desktop_id) {
			g_debug ("Blacklist Destkop = %s", full_desktop_id);
			if (!g_str_has_suffix (full_desktop_id, "gooroomupdate.desktop")) {
				/* remove exec permission */
				make_exec_executable (full_desktop_id, FALSE);
			}
			g_free (full_desktop_id);
		}
	}
	g_strfreev (items);
	g_free (list);

	return 0;
}
