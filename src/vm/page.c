#include "vm/page.h"
#include <malloc.h>
#include <bitmap.h>
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "userprog/process.h"
#include "filesys/file.h"
#include "vm/frame.h"

static struct spt_entry* create_spte ();
static bool install_load_file (struct spt_entry *);
static bool install_load_mmap (struct spt_entry *);
static bool install_load_swap (struct spt_entry *);
static void free_spte_elem (struct hash_elem *, void *);
static void free_spte (struct spt_entry *);

/*
  Naming convention for pages and frames:
  
  frame, kpage => always installed at PD, PT
  (associated with a kernel virtual address taken from user_pool)
  only free is available
  
  upage => referenced by SPT of the parent thread.
  may be uninstalled or installed (loaded) to a frame (kpage) 

  file means readonly executable file
  
  others are mmap files
*/

unsigned
spt_hash_func (const struct hash_elem *element, void *aux UNUSED)
{
  struct spt_entry *spte = hash_entry (element, struct spt_entry, elem);
  return hash_int ((int) spte->upage);
}

bool
spt_less_func (const struct hash_elem *a, const struct hash_elem *b,
               void *aux UNUSED)
{
  struct spt_entry *spte_a = hash_entry (a, struct spt_entry, elem);
  struct spt_entry *spte_b = hash_entry (b, struct spt_entry, elem);

  return (int) spte_a->upage < (int) spte_b->upage;
}

void
supp_page_table_init (struct hash *supp_page_table)
{
  hash_init (supp_page_table, spt_hash_func, spt_less_func, NULL);
}

struct spt_entry *
uvaddr_to_spt_entry (void *uvaddr)
{
  void *upage = pg_round_down (uvaddr);
  struct spt_entry spte;
  spte.upage = upage;

  struct hash_elem *e = hash_find (
    &thread_current()->supp_page_table, &spte.elem);

  if (e)
    return hash_entry (e, struct spt_entry, elem);
  else
    return NULL;
}

static struct spt_entry *
create_spte ()
{
  struct spt_entry *spte = (struct spt_entry *) malloc (
    sizeof (struct spt_entry));
  spte->frame = NULL;
  spte->upage = NULL;
  spte->is_in_swap = false;
  spte->idx = BITMAP_ERROR;
  spte->pinned = false;
  return spte;
}

struct spt_entry *
create_spte_code (void *upage)
{
  struct spt_entry *spte = create_spte ();
  spte->type = CODE;
  spte->upage = upage;
  hash_insert (&((thread_current())->supp_page_table), &spte->elem);
  return spte;
}

struct spt_entry *
create_spte_mmap (struct file *f, int read_bytes, void *upage)
{
  struct thread *t = thread_current();
  uint32_t page_read_bytes, page_zero_bytes;
  int ofs = 0;
  int i = 0;
  struct spt_entry *first_spte = NULL;
  
  while (read_bytes > 0)
  {
    page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    page_zero_bytes = PGSIZE - page_read_bytes;

    struct spt_entry *spte = uvaddr_to_spt_entry (upage);
    if (spte != NULL){
      free_spte_mmap (first_spte);
      return NULL;
    }
    
    spte = create_spte ();
    spte->type = MMAP;
    spte->upage = upage;
    spte->file = f;
    spte->ofs = ofs;
    spte->page_read_bytes = page_read_bytes;
    spte->page_zero_bytes = page_zero_bytes;
    spte->writable = true;

    ofs += page_read_bytes;
    read_bytes -= page_read_bytes;
    upage += PGSIZE;
    
    hash_insert (&(t->supp_page_table), &spte->elem);
    if (i == 0)
    {
      first_spte = spte;
      i++;
    }
    
  }
  return first_spte;
}
  

bool
create_spte_file (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      struct spt_entry *spte = create_spte ();
      spte->type = FILE;
      spte->upage = upage;
      spte->page_read_bytes = page_read_bytes;
      spte->page_zero_bytes = page_zero_bytes;
      spte->file = file;
      spte->ofs = ofs;
      spte->writable = writable;
      ofs += page_read_bytes;
      
      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;

      hash_insert (&((thread_current())->supp_page_table), &spte->elem);
    }
  return true;
}

static bool
install_load_file (struct spt_entry *spte)
{
  lock_acquire (&evict_lock);
  void *frame = get_frame_for_page (PAL_USER, spte);
  ASSERT (frame != NULL);

  if (frame == NULL)
  {
    lock_release (&evict_lock);
    return false;
  }

  /* Load this page. */
  lock_acquire (&file_lock);
  file_seek (spte->file, spte->ofs);
  int read_bytes = file_read (spte->file, frame, spte->page_read_bytes);
  lock_release (&file_lock);
  
  if (read_bytes != (int) spte->page_read_bytes)
  {
    free_frame (frame);
    lock_release (&evict_lock);
    return false; 
  }
  memset (frame + spte->page_read_bytes, 0, spte->page_zero_bytes);

  /* Add the page to the process's address space. */
  if (!install_page (spte->upage, frame, spte->writable)) 
  {
    free_frame (frame);
    lock_release (&evict_lock);
    return false; 
  }
  spte->frame = frame;
  lock_release (&evict_lock);
  return true;
}

static bool
install_load_mmap (struct spt_entry *spte)
{
  return install_load_file (spte);
}

static bool
install_load_swap (struct spt_entry *spte)
{
  lock_acquire (&evict_lock);
  void *frame = get_frame_for_page (PAL_USER | PAL_ZERO, spte);
  ASSERT (frame != NULL);
  
  if (frame == NULL)
  {
    lock_release (&evict_lock);
    return false;
  }

  if (install_page (spte->upage, frame, true))
  {
    spte->frame = frame;
    if (spte->is_in_swap) /* Add empty page (stack growth). */
    {
      swap_in (spte);
      spte->is_in_swap = false;
      spte->idx = BITMAP_ERROR;
    }
    lock_release (&evict_lock);
    return true;
  }
  else
    free_frame (frame);

  lock_release (&evict_lock);
  return false;
}

bool
install_load_page (struct spt_entry *spte)
{
  switch (spte->type){
  case FILE:
    return install_load_file (spte);
    break;
  case MMAP:
    return install_load_mmap (spte);
    break;
  case CODE:
    return install_load_swap (spte);
    break;
  default:
    return false;
  }
}

static void
free_spte_elem (struct hash_elem *e, void *aux)
{
  struct spt_entry *spte = hash_entry (e, struct spt_entry, elem);
  free_spte (spte);
  /*
  if (spte->frame != NULL)
  {
    pagedir_clear_page (&thread_current()->pagedir, spte->upage);
    free_frame (spte->frame);
  }
  free (spte);*/
}

void
free_spte_mmap (struct spt_entry *first_spte)
{
  if (first_spte != NULL)
  {
    int read_bytes = file_length (first_spte->file);
    void *upage = first_spte->upage;
    struct spt_entry *spte;
    while (read_bytes > 0)
    {
      spte = uvaddr_to_spt_entry (upage);
      upage += PGSIZE;
      read_bytes -= spte->page_read_bytes;

      if (spte->file == first_spte->file)
        free_spte (spte);
    }
  }
}

static void
free_spte (struct spt_entry *spte)
{
  /* If mmap file and is dirty then write to disk
     If stack or read only file then no need. */
  if (spte != NULL)
  {
    if (spte->frame != NULL)
    {
      if(spte->type == MMAP || (spte->type == FILE && spte->writable))
        write_to_disk (spte);

      void *pd = thread_current()->pagedir;
      pagedir_clear_page (pd, spte->upage);
      free_frame (spte->frame);
    }
    
    hash_delete (&thread_current()->supp_page_table,
                   &spte->elem);
    free (spte);
  }
}

void destroy_spt (struct hash *supp_page_table){
  hash_destroy (supp_page_table, free_spte_elem);
}

bool
grow_stack (void *uaddr, bool pinned)
{
  void *upage = pg_round_down (uaddr);

  if ((size_t) (PHYS_BASE - uaddr) > MAX_STACK_SIZE)
    return false;
  
  struct spt_entry *spte = create_spte_code (upage);
  lock_acquire (&pin_lock);
  spte->pinned = pinned;
  lock_release (&pin_lock);
  return install_load_page (spte);
}

/* spte is not NULL and is loaded i.e. a frame exists for it. */
bool
write_to_disk (struct spt_entry *spte)
{
  struct thread *t = thread_current ();
  if (pagedir_is_dirty (t->pagedir, spte->upage))
  {
    lock_acquire (&file_lock);
    off_t written = file_write_at (spte->file, spte->upage,
                                   spte->page_read_bytes, spte->ofs);
    lock_release (&file_lock);
    if (written != spte->page_read_bytes)
      return false;
  }
  return true;
}
