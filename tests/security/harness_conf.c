/*
** Regression harness for the config option store (lib/conf.c).
**
** Option names used to be compared as prefixes (memcmp over strlen of the stored name),
** which read past shorter names and made e.g. "recursive-timeout" resolve to "recursive".
*/
#include "config.h"
#include "conf.c"				/* the real lib/conf.c */
#include "common.h"

int
main(void) {
  CONF *list = NULL;
  const char *v;

  { /* An option must not be found under a longer name it is a prefix of. */
    conf_set(&list, "recursive", "192.0.2.53", 0);
    conf_set(&list, "recursive-timeout", "5", 0);
    v = conf_get(&list, "recursive-timeout", NULL);
    check(v && !strcmp(v, "5"), "C1", "\"recursive-timeout\" is its own option, not \"recursive\"");
    v = conf_get(&list, "recursive", NULL);
    check(v && !strcmp(v, "192.0.2.53"), "C2", "\"recursive\" keeps its own value");
  }
  { /* Same for notify-source / notify-source6. */
    conf_set(&list, "notify-source", "192.0.2.1", 0);
    v = conf_get(&list, "notify-source6", NULL);
    check(v == NULL, "C3", "\"notify-source6\" unset does not return \"notify-source\"");
  }
  { /* Looking up a name shorter than a stored one must not read past it (ASAN). */
    char *shortname = (char *)exact_copy("db", 3);
    v = conf_get(&list, shortname, NULL);
    check(v == NULL, "C4", "short name lookup stays in bounds");
    conf_set(&list, shortname, "x", 0);
    v = conf_get(&list, "db", NULL);
    check(v && !strcmp(v, "x"), "C5", "short name can be set without matching a longer one");
    free(shortname);
  }
  { /* A line ending in a backslash must not walk past its terminating NUL (ASAN). */
    char *line = (char *)exact_copy("log = stderr\\", 14);
    char *name = NULL, *value = NULL;
    conf_trim_comments(line);
    conf_get_option(line, &name, &value);
    check(name && !strcmp(name, "log"), "C6", "trailing backslash handled in bounds");
    free(line);
  }

  return failures();
}
