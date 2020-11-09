#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <mips/trapframe.h>
#include "opt-A2.h"

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;

  #if OPT_A2
    if (curproc->parent) {
      struct proc *p = curproc->parent;
      lock_acquire(p->child_lock);
      for (unsigned int i = 0; i < array_num(p->children); i++) {
        proc_info *info = array_get(p->children, i);
        if (curproc->pid == info->pid) {
          info->exitcode = exitcode;
          break;
        }
      }
      lock_release(p->child_lock);
      cv_signal(curproc->dying, curproc->child_lock);
    }
  #else 
    /* for now, just include this to keep the compiler from complaining about
      an unused variable */
    (void)exitcode;
  #endif

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  #if OPT_A2
    *retval = curproc->pid;
  #else
    /* for now, this is just a stub that always returns a PID of 1 */
    /* you need to fix this to make it work properly */
    *retval = 1;
  #endif
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }
#if OPT_A2
  lock_acquire(curproc->child_lock);
  bool found = false;
  for (unsigned int i = 0; i < array_num(curproc->children); i++) {
    proc_info *info = array_get(curproc->children, i);
    if (pid == info->pid) {
      found = true;
      while (info->exitcode == -1) 
        cv_wait(info->procedure->dying, curproc->child_lock);
      exitstatus = _MKWAIT_EXIT(info->exitcode);
    }
  }
  lock_release(curproc->child_lock);
  if (!found) {
    *retval = -1;
    return (ESRCH);
  }
#else
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
#endif
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

#if OPT_A2
int
sys_fork(struct trapframe *tf, pid_t *retval) {
  struct proc *child = proc_create_runprogram(curproc->p_name);
  KASSERT(child != NULL);
  KASSERT(child->pid > 0);

  child->parent = curproc;

  proc_info *info = kmalloc(sizeof(proc_info));
  info->pid = child->pid;
  info->procedure = child;
  info->exitcode = -1;
  array_add(curproc->children, info, NULL);

  int err = as_copy(curproc->p_addrspace, &(child->p_addrspace));
  if (err != 0) {
    proc_destroy(child);
    return ENOMEM;
  }

  curproc->p_tf = kmalloc(sizeof(struct trapframe));
  KASSERT(curproc->p_tf != NULL);
  memcpy(curproc->p_tf, tf, sizeof(struct trapframe));

  thread_fork(child->p_name, child, (void *) &enter_forked_process, curproc->p_tf, 10);

  *retval = child->pid;
  return(0);
}
#endif
