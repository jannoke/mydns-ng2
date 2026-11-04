/*
** Definitions that src/mydns/main.c normally provides, for harnesses that link the
** server's object files without main.o (see run.sh). Data symbols get harmless
** defaults; functions abort loudly, so a harness can never pass by silently reaching
** server-lifecycle code it was not meant to exercise.
*/
#include "named.h"

QUEUE		*TaskArray[PERIODIC_TASK+1][LOW_PRIORITY_TASK+1];
struct timeval	current_tick;
time_t		current_time;
uint32_t	answer_then_quit = 0;
int		show_data_errors = 0;
SERVERSTATUS	Status;

static void die(const char *fn) {
  fprintf(stderr, "server_stubs: %s() reached from a harness\n", fn);
  abort();
}

SERVER *find_server_for_task(TASK *t) { die("find_server_for_task"); return NULL; }
void free_other_tasks(TASK *t, int need_socket) { die("free_other_tasks"); }
void kill_server(SERVER *server, int sig) { die("kill_server"); }
void named_cleanup(int signo) { die("named_cleanup"); }
void named_shutdown(int signo) { die("named_shutdown"); }
int named_shutting_down(void) { return 0; }
