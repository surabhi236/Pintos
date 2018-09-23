#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/synch.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "vm/page.h"
#include "filesys/filesys.h"
#include "vm/frame.h"

static void syscall_handler (struct intr_frame *);
static void valid_up (const void*, const void *);
static void validate (const void*, const void *, size_t);
static void validate_string (const void *, const char *);
static void close_file (int);
static bool is_valid_fd (int);
static void is_writable (const void *);
static bool is_valid_page (void *);

static void
unpin_buffer (void *buffer, unsigned size)
{
  lock_acquire (&pin_lock);
  uint32_t *pd = thread_current ()->pagedir;
  struct spt_entry *spte = uvaddr_to_spt_entry (buffer);
  if (spte != NULL)
    spte->pinned = false;
  spte = uvaddr_to_spt_entry (buffer + size -1);
  if (spte != NULL)
    spte->pinned = false;

  int i;
  for (i = PGSIZE; i < size; i += PGSIZE)
  {
    spte = uvaddr_to_spt_entry (buffer + i);
    if (spte != NULL)
      spte->pinned = false;
  }
  lock_release (&pin_lock);
}

static void
unpin_str (char *str)
{
  int l = strlen (str);
  unpin_buffer ((void *) str, l);
}

static int
halt (void *esp)
{
  power_off ();
}

int
exit (void *esp)
{
  int status = 0;
  if (esp != NULL){
    validate (esp, esp, sizeof(int));
    status = *((int *)esp);
    esp += sizeof (int);
  }
  else {
    status = -1;
  }

  struct thread *t = thread_current ();

  int i;
  for (i = 2; i<MAX_FILES; i++)
  {
    if (t->files[i] != NULL){
      close_file (i);
    }
  }

  destroy_spt (&t->supp_page_table);
  
  char *name = t->name, *save;
  name = strtok_r (name, " ", &save);

  lock_acquire (&file_lock);
  printf ("%s: exit(%d)\n", name, status);
  lock_release (&file_lock);

  t->return_status = status;
  
  /* Preserve the kernel struct thread just deallocate user page.
     struct thread will be deleted once parent calls wait or parent terminates.*/
  process_exit ();

  enum intr_level old_level = intr_disable ();
  t->no_yield = true;
  sema_up (&t->sema_terminated);
  thread_block ();
  intr_set_level (old_level);

  thread_exit ();
  NOT_REACHED ();
}

static int
exec (void *esp)
{
  validate (esp, esp, sizeof (char *));
  const char *file_name = *((char **) esp);
  esp += sizeof (char *);

  validate_string (esp, file_name);

  lock_acquire (&file_lock);
  tid_t tid = process_execute (file_name);
  lock_release (&file_lock);
  
  struct thread *child = get_child_thread_from_id (tid);
  if (child == NULL){
    unpin_str (file_name);
    return -1;
  }
  
  sema_down (&child->sema_ready);
  if (!child->load_complete)
    tid = -1;
  
  sema_up (&child->sema_ack);
  unpin_str (file_name);
  return tid;
}

static int
wait (void *esp)
{
  validate (esp, esp, sizeof (int));
  int pid = *((int *) esp);
  esp += sizeof (int);

  struct thread *child = get_child_thread_from_id (pid);

  /* Either wait has already been called or 
     given pid is not a child of current thread. */
  if (child == NULL) 
    return -1;
    
  sema_down (&child->sema_terminated);
  int status = child->return_status;
  list_remove (&child->parent_elem);
  thread_unblock (child);
  return status;
}

static int
create (void *esp)
{
  validate (esp, esp, sizeof(char *));
  const char *file_name = *((char **) esp);
  esp += sizeof (char *);

  validate_string (esp, file_name);

  validate (esp, esp, sizeof(unsigned));
  unsigned initial_size = *((unsigned *) esp);
  esp += sizeof (unsigned);

  lock_acquire (&file_lock);
  int status = filesys_create (file_name, initial_size);
  lock_release (&file_lock);

  unpin_str (file_name);
  return status;
}

static int
remove (void *esp)
{
  validate (esp, esp, sizeof(char *));
  const char *file_name = *((char **) esp);
  esp += sizeof (char *);

  validate_string (esp, file_name);

  lock_acquire (&file_lock);
  int status = filesys_remove (file_name);
  lock_release (&file_lock);
  
  unpin_str (file_name);
  return status;
}

static int
open (void *esp)
{
  validate (esp, esp, sizeof(char *));
  const char *file_name = *((char **) esp);
  esp += sizeof (char *);

  validate_string (esp, file_name);
  
  lock_acquire (&file_lock);
  struct file *f = filesys_open (file_name);
  lock_release (&file_lock);

  if (f == NULL){
    unpin_str (file_name);
    return -1;
  }
  
  struct thread *t = thread_current ();

  int i;
  for (i = 2; i<MAX_FILES; i++)
  {
    if (t->files[i] == NULL){
      t->files[i] = f;
      break;
    }
  }

  int ret;
  if (i == MAX_FILES)
    ret = -1;
  else
    ret = i;
  unpin_str (file_name);
  return ret;
}

static int
filesize (void *esp)
{
  validate (esp, esp, sizeof(int));
  int fd = *((int *) esp);
  esp += sizeof (int);

  struct thread *t = thread_current ();

  if (is_valid_fd (fd) && t->files[fd] != NULL)
  {  
    lock_acquire (&file_lock);
    int size = file_length (t->files[fd]);
    lock_release (&file_lock);

    return size;
  }
  return -1;
}

static int
read (void *esp)
{
  validate (esp, esp, sizeof(int));
  int fd = *((int *)esp);
  esp += sizeof (int);

  validate (esp, esp, sizeof(void *));
  const void *buffer = *((void **) esp);
  esp += sizeof (void *);

  validate (esp, esp, sizeof(unsigned));
  unsigned size = *((unsigned *) esp);
  esp += sizeof (unsigned);
  
  validate (esp, buffer, size);

  struct thread *t = thread_current ();
  int ret = 0;
  if (fd == STDIN_FILENO)
  {
    lock_acquire (&file_lock);

    int i;
    for (i = 0; i<size; i++)
      *((uint8_t *) buffer+i) = input_getc ();

    lock_release (&file_lock);
    ret = i;
  }
  else if (is_valid_fd (fd) && fd >=2 && t->files[fd] != NULL)
  {
    is_writable (buffer);
    lock_acquire (&file_lock);
    int read = file_read (t->files[fd], buffer, size);
    lock_release (&file_lock);
    ret = read;
  }

  unpin_buffer (buffer, size);
  return ret;
}

static int
write (void *esp)
{
  validate (esp, esp, sizeof(int));
  int fd = *((int *)esp);
  esp += sizeof (int);

  validate (esp, esp, sizeof(void *));
  const void *buffer = *((void **) esp);
  esp += sizeof (void *);

  validate (esp, esp, sizeof(unsigned));
  unsigned size = *((unsigned *) esp);
  esp += sizeof (unsigned);
  
  validate (esp, buffer, size);
  
  struct thread *t = thread_current ();
  int ret = 0;
  if (fd == STDOUT_FILENO)
  {
    /* putbuf (buffer, size); */
    lock_acquire (&file_lock);

    int i;
    for (i = 0; i<size; i++)
      putchar (*((char *) buffer + i));

    lock_release (&file_lock);
    ret = i;
  }
  else if (is_valid_fd (fd) && fd >=2 && t->files[fd] != NULL)
  {
    lock_acquire (&file_lock);
    int written = file_write (t->files[fd], buffer, size);
    lock_release (&file_lock);
    ret = written;
  }
    int status = 0;
  if (esp != NULL){
    validate (esp, esp, sizeof(int));
    status = *((int *)esp);
    esp += sizeof (int);
  }
  else {
    status = -1;
  }

  unpin_buffer (buffer, size);
  return ret;
}

static int
seek (void *esp)
{
  validate (esp, esp, sizeof(int));
  int fd = *((int *)esp);
  esp += sizeof (int);

  validate (esp, esp, sizeof(unsigned));
  unsigned position = *((unsigned *) esp);
  esp += sizeof (unsigned);

  struct thread *t = thread_current ();

  if (is_valid_fd (fd) && t->files[fd] != NULL)
  {
    lock_acquire (&file_lock);
    file_seek (t->files[fd], position);
    lock_release (&file_lock);
  }
}

static int
tell (void *esp)
{
  validate (esp, esp, sizeof(int));
  int fd = *((int *)esp);
  esp += sizeof (int);

  struct thread *t = thread_current ();

  if (is_valid_fd (fd) && t->files[fd] != NULL)
  {
    lock_acquire (&file_lock);
    int position = file_tell (t->files[fd]);
    lock_release (&file_lock);
    return position;
  }
  return -1;
}

static int
close (void *esp)
{
  validate (esp, esp, sizeof(int));
  int fd = *((int *) esp);
  esp += sizeof (int);

  if (is_valid_fd (fd))
    close_file (fd);
}

static int
mmap (void *esp)
{
  validate (esp, esp, sizeof(int));
  int fd = *((int *)esp);
  esp += sizeof (int);

  if (!is_valid_fd (fd))
    return -1;
  
  validate (esp, esp, sizeof(void *));
  const void *address = *((void **) esp);
  esp += sizeof (void *);
  
  if (!is_valid_page (address))
    return -1;

  struct thread *t = thread_current();
  struct file* old = t->files[fd];

  if (old == NULL)
    return -1;

  struct file *f = file_reopen (old);
  if (f == NULL)
    return -1;
  
  lock_acquire (&file_lock);
  int size = file_length (f);
  lock_release (&file_lock);

  struct spt_entry *spte = create_spte_mmap (f, size, address);
  if (spte == NULL)
    return -1;
  
  int i;
  for (i = 0; i<MAX_FILES; i++)
  {
    if (t->mmap_files[i] == NULL){
      t->mmap_files[i] = spte;
      break;
    }
  }

  if (i == MAX_FILES)
    return -1;
  else
    return i;
}

static int
munmap (void *esp)
{
  validate (esp, esp, sizeof(int));
  int map_id = *((int *)esp);
  esp += sizeof (int);

  if (is_valid_fd (map_id)){
    
    struct thread *t = thread_current();
    struct spt_entry *spte = t->mmap_files[map_id];

    if (spte != NULL)
      free_spte_mmap (spte);
  }
}

static int
chdir (void *esp)
{
  exit (NULL);
}

static int
mkdir (void *esp)
{
  exit (NULL);
}

static int
readdir (void *esp)
{
  exit (NULL);
}

static int
isdir (void *esp)
{
  exit (NULL);
}

static int
inumber (void *esp)
{
  exit (NULL);
}

static int (*syscalls []) (void *) =
  {
    halt,
    exit,
    exec,
    wait,
    create,
    remove,
    open,
    filesize,
    read,
    write,
    seek,
    tell,
    close,

    mmap,
    munmap,

    chdir,
    mkdir,
    readdir,
    isdir,
    inumber
  };

const int num_calls = sizeof (syscalls) / sizeof (syscalls[0]);

void
syscall_init (void) 
{
  lock_init (&file_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall"); 
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  void *esp = f->esp;

  validate (esp, esp, sizeof(int));
  int syscall_num = *((int *) esp);
  esp += sizeof(int);

  /* printf("\nSys: %d", syscall_num); */

  /* Just for sanity, we will anyway be checking inside all functions. */ 
  validate (esp, esp, sizeof(void *));

  if (syscall_num >= 0 && syscall_num < num_calls)
  {
    int (*function) (void *) = syscalls[syscall_num];
    int ret = function (esp);
    f->eax = ret;
  }
  else
  {
    /* TODO:: Raise Exception */
    printf ("\nError, invalid syscall number.");
    exit (NULL);
  }
  unpin_buffer (f->esp, sizeof (esp));
  unpin_buffer (f->esp+PGSIZE, sizeof (esp));
}

static void
close_file (int fd)
{
  struct thread *t = thread_current ();
  if (t->files[fd] != NULL)
  {
    lock_acquire (&file_lock);
    file_close (t->files[fd]);
    t->files[fd] = NULL;
    lock_release (&file_lock);
  }
}

static bool
is_valid_fd (int fd)
{
  return fd >= 0 && fd < MAX_FILES; 
}

static void
validate_string (const void *esp, const char *s)
{
  validate (esp, s, sizeof(char));
  while (*s != '\0')
    validate (esp, s++, sizeof(char));
}

static void
validate (const void *esp, const void *ptr, size_t size)
{
  valid_up (esp, ptr);
  if(size != 1)
    valid_up (esp, ptr + size - 1);

  int i;
  for (i = PGSIZE; i < size; i += PGSIZE)
    valid_up (esp, ptr + i);
}

static void
valid_up (const void *esp, const void *ptr)
{
  uint32_t *pd = thread_current ()->pagedir;
  if (ptr == NULL || !is_user_vaddr (ptr))
  {
    exit (NULL);
  }

  struct spt_entry *spte = uvaddr_to_spt_entry (ptr);
  if (spte != NULL)
  {
    lock_acquire (&pin_lock);
    spte->pinned = true;
    lock_release (&pin_lock);

    if (pagedir_get_page (pd, ptr) == NULL)
      if(!install_load_page (spte))
        exit (NULL);
  }
  else if (pagedir_get_page (pd, ptr) == NULL)
  {
    if(!(ptr >= esp - STACK_HEURISTIC &&
         grow_stack (ptr, true)))
      exit (NULL);
  }
}

static bool
is_valid_page (void *upage)
{
  /* non-zero */
  if (upage == 0)
    return false;
  
  /* Page aligned */
  if ((uintptr_t) upage % PGSIZE != 0)
    return false;

  return true;
}

static void
is_writable (const void *ptr)
{
  struct spt_entry *spte = uvaddr_to_spt_entry (ptr);
  if (spte->type == FILE && !spte->writable)
    exit (NULL);
}
