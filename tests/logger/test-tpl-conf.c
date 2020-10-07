#include "config.h"

#include <telepathy-glib/telepathy-glib.h>

#include <telepathy-logger/conf-internal.h>
#include <telepathy-logger/debug.h>

typedef struct {
    int dummy;
} Fixture;

static void
setup (Fixture *f,
    gconstpointer data)
{
}

static void
test (Fixture *f,
    gconstpointer data)
{
  TplConf *conf, *conf2;

  tpl_debug_set_flags ("all");
  tp_debug_set_flags ("all");

  conf = _tpl_conf_dup ();

  /* TplConf is a singleton, be sure both point to the same memory */
  conf2 = _tpl_conf_dup ();
  g_assert (conf == conf2);

  /* unref the second singleton pointer and check that the it is still
   * valid: checking correct object ref-counting after each _dup () call */
  g_object_unref (conf2);
  g_assert (TPL_IS_CONF (conf));

  /* it points to the same mem area, it should be still valid */
  g_assert (TPL_IS_CONF (conf2));

  /* proper disposal for the singleton when no references are present */
  g_object_unref (conf);
}

static void
teardown (Fixture *f,
    gconstpointer data)
{
}

int
main (int argc,
    char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_bug_base ("http://bugs.freedesktop.org/show_bug.cgi?id=");

  g_test_add ("/tpl-conf", Fixture, NULL, setup, test, teardown);

  return g_test_run ();
}