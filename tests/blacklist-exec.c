#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gdesktopappinfo.h>
#include <sys/stat.h>

static const gchar *exec_override;
static gchar *test_get_string(GDesktopAppInfo *info, const gchar *key) {
    if (exec_override && g_str_equal(key, G_KEY_FILE_DESKTOP_KEY_EXEC))
        return g_strdup(exec_override);
    return g_desktop_app_info_get_string(info, key);
}
static int checked_stat(const gchar *path, GStatBuf *buf) {
    g_assert_nonnull(path);
    return g_stat(path, buf);
}
#define main blacklist_helper_main
#define g_desktop_app_info_get_string test_get_string
#undef g_stat
#define g_stat checked_stat
#include "../src/gooroom-update-blacklist-helper.c"
#undef main
#undef g_stat
#undef g_desktop_app_info_get_string

int main(int argc, char **argv) {
    g_assert_cmpint(argc, ==, 2);
    g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    gchar *dir = g_dir_make_tmp("blacklist-test-XXXXXX", NULL);
    gchar *bin = g_build_filename(dir, "program", NULL);
    gchar *desktop = g_build_filename(dir, "test.desktop", NULL);
    gchar *text = g_strdup_printf("[Desktop Entry]\nType=Application\nName=Test\nExec=%s\n", bin);
    g_assert_true(g_file_set_contents(bin, "#!/bin/sh\nexit 0\n", -1, NULL));
    g_assert_cmpint(g_chmod(bin, 0755), ==, 0);
    g_assert_true(g_file_set_contents(desktop, text, -1, NULL));
    if (g_str_equal(argv[1], "invalid-desktop")) {
        make_exec_executable("/nonexistent/test.desktop", FALSE);
    } else if (g_str_equal(argv[1], "invalid-exec")) {
        exec_override = "'unterminated";
        make_exec_executable(desktop, FALSE);
    } else if (g_str_equal(argv[1], "missing-program")) {
        exec_override = "/nonexistent/missing-blacklist-program";
        make_exec_executable(desktop, FALSE);
    } else {
        GStatBuf st;
        make_exec_executable(desktop, FALSE);
        g_assert_cmpint(stat(bin, &st), ==, 0);
        g_assert_cmpint(st.st_mode & S_IXOTH, ==, 0);
        make_exec_executable(desktop, TRUE);
        g_assert_cmpint(stat(bin, &st), ==, 0);
        g_assert_cmpint(st.st_mode & S_IXOTH, !=, 0);
    }
    g_remove(desktop); g_remove(bin); g_rmdir(dir);
    g_free(text); g_free(desktop); g_free(bin); g_free(dir);
    g_print("PASS: %s\n", argv[1]);
    return 0;
}
