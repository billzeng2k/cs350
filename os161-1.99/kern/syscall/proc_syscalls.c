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
#include <vfs.h>
#include <kern/fcntl.h>
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


int sys_execv(const char *progname, char **args)
{
  int nargs = 0;
  for (; args[nargs] != NULL; nargs++);

  char **kern_args = kmalloc((nargs + 1) * sizeof(char *));
  KASSERT(kern_args != NULL);

  for (int i = 0; i <= nargs; i++) {
    if (i == nargs)
      kern_args[i] = NULL;
    else {
      size_t arg_size = (strlen(args[i]) + 1) * sizeof(char);
      kern_args[i] = kmalloc(arg_size);
      KASSERT(kern_args[i] != NULL);
      int err = copyin((const_userptr_t) args[i], (void *) kern_args[i], arg_size);
      KASSERT(err == 0);
    }
  }

  size_t progname_size = (strlen(progname) + 1) * sizeof(char);
  char *kern_progname = kmalloc(progname_size);
  KASSERT(kern_progname != NULL);
  int err = copyin((const_userptr_t) progname, (void *) kern_progname, progname_size);
  KASSERT(err == 0);

  // from runprogram
  struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	/* Open the file. */
	result = vfs_open(kern_progname, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* Create a new address space. */
	as = as_create();
	if (as ==NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	curproc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

  vaddr_t *stack_args = kmalloc((nargs + 1) * sizeof(vaddr_t));

  for (int i = nargs; i >= 0; i--) {
		if (args[i] == NULL) 
			stack_args[i] = (vaddr_t) NULL;
		else {
			size_t arg_size = ROUNDUP(strlen(kern_args[i]) + 1, 4);
			stackptr -= arg_size * sizeof(char);
			int err = copyout((void *) kern_args[i], (userptr_t) stackptr, arg_size);
			KASSERT(err == 0);
			stack_args[i] = stackptr;
		}
	}

  for (int i = nargs; i >= 0; i--) {
		stackptr -= sizeof(vaddr_t);
		int err = copyout((void *) &stack_args[i], (userptr_t) stackptr, sizeof(vaddr_t));
		KASSERT(err == 0);
	}

  kfree(kern_progname);
  for (int i = 0; i <= nargs; i++) 
    kfree(kern_args[i]);
  kfree(kern_args);

	/* Warp to user mode. */
	enter_new_process(nargs /*argc*/, (userptr_t) stackptr /*userspace addr of argv*/,
			  ROUNDUP(stackptr, 8), entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
  return EINVAL;

}
#endif
