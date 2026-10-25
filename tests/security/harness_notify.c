/*
** Regression harness for matching NOTIFY replies to slaves (src/mydns/notify.c).
** HARNESS-REPLACES: notify.o
**
** A reply only counts for a slave if it comes from that slave's address. For IPv6 the
** comparison used to be memcmp(a, b, sizeof(struct in6_addr) != 0) - i.e. 1 byte.
*/
#include "config.h"
#include "notify.c"				/* the real src/mydns/notify.c */
#include "common.h"

static TASK *
task_with_slave(int family, const void *addr) {
  TASK *t = ALLOCATE(sizeof(TASK), TASK);
  NOTIFYDATA *nd = ALLOCATE(sizeof(NOTIFYDATA), NOTIFYDATA);
  NOTIFYSLAVE *slave = ALLOCATE(sizeof(NOTIFYSLAVE), NOTIFYSLAVE);

  t->family = family;
  nd->slaves = array_init(1);
  slave->slaveaddr.ss_family = family;
  if (family == AF_INET6)
    memcpy(&((struct sockaddr_in6 *)&slave->slaveaddr)->sin6_addr, addr, sizeof(struct in6_addr));
  else
    memcpy(&((struct sockaddr_in *)&slave->slaveaddr)->sin_addr, addr, sizeof(struct in_addr));
  array_append(nd->slaves, slave);
  t->extension = nd;
  return t;
}

static int
reply_from(TASK *t, int family, const void *addr) {
  struct sockaddr_storage from;

  memset(&from, 0, sizeof(from));
  from.ss_family = family;
  if (family == AF_INET6)
    memcpy(&((struct sockaddr_in6 *)&from)->sin6_addr, addr, sizeof(struct in6_addr));
  else
    memcpy(&((struct sockaddr_in *)&from)->sin_addr, addr, sizeof(struct in_addr));
  return _notify_read(t, &from, sizeof(from));		/* slaves still waiting for a reply */
}

int
main(void) {
#if HAVE_IPV6
  struct in6_addr slave6, other6;
  TASK *t6;

  inet_pton(AF_INET6, "2001:db8::53", &slave6);
  inet_pton(AF_INET6, "2001:db8::bad", &other6);		/* same first byte as the slave */

  t6 = task_with_slave(AF_INET6, &slave6);
  check(reply_from(t6, AF_INET6, &other6) == 1, "S6a", "IPv6 reply from a different host is not accepted");
  check(reply_from(t6, AF_INET6, &slave6) == 0, "S6b", "IPv6 reply from the slave is accepted");
#endif
  {
    struct in_addr slave4, other4;
    TASK *t4;

    inet_pton(AF_INET, "192.0.2.53", &slave4);
    inet_pton(AF_INET, "192.0.2.99", &other4);
    t4 = task_with_slave(AF_INET, &slave4);
    check(reply_from(t4, AF_INET, &other4) == 1, "S4a", "IPv4 reply from a different host is not accepted");
    check(reply_from(t4, AF_INET, &slave4) == 0, "S4b", "IPv4 reply from the slave is accepted");
  }
  return failures();
}
