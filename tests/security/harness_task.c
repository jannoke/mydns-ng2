/*
** Regression harness for task-slot allocation (src/mydns/task.c).
** HARNESS-REPLACES: task.o
**
** Each task's internal id is tracked as one bit in `taskvec' (32 ids per uint32_t word);
** the bit mask used to come from taskvec_masks[id & 0x1ff], reading far past the 32-entry
** table for most ids and setting/clearing the wrong bits.
*/
#include "config.h"
#include "task.c"				/* the real src/mydns/task.c */
#include "common.h"

#define NTASKS 600

int
main(void) {
  static TASK *tasks[NTASKS];
  struct sockaddr_in addr;
  int i, all_created = 1, bitmap_clear = 1;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  TaskArray[NORMAL_TASK][NORMAL_PRIORITY_TASK] = queue_init("normal", "normal");

  for (i = 0; i < NTASKS; i++)				/* ids 0..599: crosses many mask words */
    if (!(tasks[i] = task_init(NORMAL_PRIORITY_TASK, NEED_ANSWER, -1, SOCK_DGRAM, AF_INET, &addr)))
      all_created = 0;
  check(all_created, "T1", "600 tasks allocated without overrunning taskvec_masks");

  for (i = 0; i < NTASKS; i++)
    if (tasks[i])
      dequeue(tasks[i]);
  for (i = 0; i < (int)(TASKVECSZ / sizeof(uint32_t)); i++)
    if (taskvec[i])
      bitmap_clear = 0;
  check(bitmap_clear, "T2", "freeing every task clears every slot bit");

  return failures();
}
