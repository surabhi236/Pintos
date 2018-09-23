#ifndef VM_FRAME
#define VM_FRAME

#include <list.h>
#include "vm/page.h"
#include "threads/thread.h"
#include "threads/palloc.h"

struct frame_table_entry
{
  void *frame;
  struct spt_entry *spte;
  struct thread *t;
  struct list_elem elem;
};

struct lock pin_lock;
struct lock evict_lock;

void free_frame (void *);
void frame_table_init (void);
void *get_frame_for_page (enum palloc_flags, struct spt_entry *);

#endif
