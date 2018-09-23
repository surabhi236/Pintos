#include "threads/synch.h"
#include "threads/palloc.h"
#include "vm/frame.h"
#include "vm/page.h"
#include <malloc.h>
#include <list.h>
#include "userprog/pagedir.h"
#include <bitmap.h>
#include "vm/swap.h"

static struct list frame_table;
static struct lock frame_table_lock;

static void add_to_frame_table (void *, struct spt_entry *);
static void clear_frame_entry (struct frame_table_entry *);
static void *frame_alloc (enum palloc_flags);
bool evict_frame (struct frame_table_entry *);

void
frame_table_init (void)
{
  list_init (&frame_table);
  lock_init (&frame_table_lock);
  lock_init (&pin_lock);
  lock_init (&evict_lock);
}

/* Unoptimized enhanced second-chance page replacement. 
   Called with frame_table_lock in aquired state. */
static struct frame_table_entry *
get_victim_frame ()
{
  ASSERT (lock_held_by_current_thread (&frame_table_lock));
  /* 4 classes (low priority) 00 01 10 11 (high priority) 
     (Reference (higher priority) and Dirty bits).
     We choose a frame in the order of priority given above 
     and use FIFO order to resolve conflicts.
     
     In phase 1 we write all dirty pages except that 
     of the code type.
     
     In phase 2 we assume all pages are non dirty (those of 
     code might be dirty but if they are not accessed they 
     will be victim (since phase 2 only occurs if all 
     frames were dirty)) (also some FILE or MMAP might not 
     have been written properly and write_to_disk would 
     have returned false, these are also dirty in phase 2).
     
     If even after phase 2, none of the frames have been 
     evicted that means all were dirty and accessed,
     just evict any (chose first based on FIFO). */

  struct list_elem *e;
  
  /* Phase 1: Remove Dirty */
  for (e = list_begin (&frame_table);
       e != list_end (&frame_table);
       e = list_next (e))
  {
    struct frame_table_entry *fte =
      list_entry (e, struct frame_table_entry, elem);
    bool is_dirty = pagedir_is_dirty (fte->t->pagedir,
                                      fte->spte->upage);
    bool is_accessed = pagedir_is_accessed (fte->t->pagedir,
                                            fte->spte->upage);

    if (!fte->spte->pinned)
    {
      if (fte->spte->type != CODE)
      {
        if (is_dirty)
        {
          if (write_to_disk (fte->spte))
            pagedir_set_dirty (fte->t->pagedir, fte->spte->upage, false);
        }
        else if (!is_accessed)
          return fte;
      }
      else
      {
        if (!is_dirty && !is_accessed)
          return fte;
      }
    }
  }

  /* Phase 2: Remove Accessed */
  for (e = list_begin (&frame_table);
       e != list_end (&frame_table);
       e = list_next (e))
  {
    struct frame_table_entry *fte =
      list_entry (e, struct frame_table_entry, elem);
    bool is_dirty = pagedir_is_dirty (fte->t->pagedir,
                                      fte->spte->upage);
    bool is_accessed = pagedir_is_accessed (fte->t->pagedir,
                                            fte->spte->upage);

    if (!fte->spte->pinned)
    {
      if ((!is_dirty || fte->spte->type == CODE) && !is_accessed)
        return fte;
      else //Accessed or (Dirty (FILE or MMAP)).
        pagedir_set_accessed (fte->t->pagedir, fte->spte->upage, false);
    }
  }

  ASSERT (!list_empty (&frame_table));
  for (e = list_begin (&frame_table);
       e != list_end (&frame_table);
       e = list_next (e))
  {
    struct frame_table_entry *fte =
      list_entry (e, struct frame_table_entry, elem);
    if (!fte->spte->pinned){
      return fte;
    }
  }
  return NULL;
}

bool
evict_frame (struct frame_table_entry *fte)
{
  ASSERT (lock_held_by_current_thread (&frame_table_lock));
  struct spt_entry *spte = fte->spte;
  size_t idx;
  switch (spte->type){
  case MMAP:

    /* Given frame to evict of type FILE or MMAP will 
       never be dirty. */

    if (pagedir_is_dirty (fte->t->pagedir, spte->upage))
        if (!write_to_disk (spte))
        {
          PANIC ("Not able to write out");
          return false;
        }

    spte->frame = NULL;
    
    clear_frame_entry (fte);
    return true;
    break;
  case FILE:
    spte->type = CODE;
  case CODE:
    ASSERT (spte->frame != NULL);
    idx = swap_out (spte);
    if (idx == BITMAP_ERROR){
      PANIC ("Not able to swap out");
      return false;
    }

    spte->idx = idx;
    spte->is_in_swap = true;
    spte->frame = NULL;

    clear_frame_entry (fte);
    return true;
    break;
  default:
    PANIC ("Corrupt fte or spte");
    return false;
  }
  return true;
}

void *
get_frame_for_page (enum palloc_flags flags, struct spt_entry *spte)
{
  if(spte == NULL)
    return NULL;
  
  if (flags & PAL_USER == 0)
    return NULL;

  void *frame = frame_alloc (flags);

  if (frame != NULL){
    add_to_frame_table (frame, spte);
    return frame;
  }
  else
    PANIC ("Not able to get frame");
}

static void
add_to_frame_table (void *frame, struct spt_entry *spte) {
  struct frame_table_entry *fte =
    (struct frame_table_entry *) malloc (sizeof (struct frame_table_entry));

  lock_acquire (&frame_table_lock);
  fte->frame = frame;
  fte->spte = spte;
  ASSERT (fte->spte->type < 3 && fte->spte->type >= 0);
  fte->t = thread_current ();
  list_push_back (&frame_table, &fte->elem);
  lock_release (&frame_table_lock);
}

/* Returns kernel virtual address of a kpage taken from user_pool. */
static void *
frame_alloc (enum palloc_flags flags)
{
  if (flags & PAL_USER == 0)
    return NULL;

  void *frame = palloc_get_page (flags);
  if (frame != NULL)
    return frame;
  else
  {
    lock_acquire (&pin_lock);
    lock_acquire (&frame_table_lock);
    do {
      if (list_empty (&frame_table))
        PANIC ("palloc_get_page returned NULL when frame table empty.");

      struct frame_table_entry *fte = get_victim_frame ();

      /* Always get some frame to evict. */
      ASSERT (fte != NULL);

      /* Check not corrupt fte or spte. */
      /*printf ("\nhere::%p, %s, %p, %d, %p, %p", fte->t,
        fte->t->name, fte->spte, fte->spte->type,
        fte->spte->frame, fte->spte->upage);*/
      ASSERT (fte->spte->type < 3 && fte->spte->type >= 0 &&
              fte->frame != NULL);
      ASSERT (fte->spte->frame != NULL);

      bool evicted = evict_frame (fte);
      if (evicted){
        frame = palloc_get_page (flags);
      }
      else
        PANIC ("Not able to evict. ");
    } while (frame == NULL);
      
    lock_release (&frame_table_lock);
    lock_release (&pin_lock);
    return frame;
  }
}

void
free_frame (void *frame)
{
  struct frame_table_entry *fte;
  struct list_elem *e;

  lock_acquire (&frame_table_lock);
  for (e = list_begin (&frame_table);
       e != list_end (&frame_table);
       e = list_next (e))
  {
    fte = list_entry (e, struct frame_table_entry, elem);
    if (fte->frame == frame)
    {
      list_remove (&fte->elem);
      break;
    }
  }
  lock_release (&frame_table_lock);

  palloc_free_page (frame);
}

static void
clear_frame_entry (struct frame_table_entry *fte)
{
  ASSERT (lock_held_by_current_thread (&frame_table_lock));
  //printf ("\nnext %p ", fte->elem.next);
  //printf ("\nprev %p ", fte->elem.prev);
  list_remove (&fte->elem);

  pagedir_clear_page (fte->t->pagedir, fte->spte->upage);
  palloc_free_page (fte->frame);
  free (fte);
}
