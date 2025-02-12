/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2004 Tim J. Robbins
 * Copyright (c) 2002 Doug Rabson
 * Copyright (c) 2000 Marcel Moolenaar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_compat.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/imgact.h>
#include <sys/ktr.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/racct.h>
#include <sys/sched.h>
#include <sys/syscallsubr.h>
#include <sys/sx.h>
#include <sys/umtxvar.h>
#include <sys/unistd.h>
#include <sys/wait.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>

#ifdef COMPAT_LINUX32
#include <machine/../linux32/linux.h>
#include <machine/../linux32/linux32_proto.h>
#else
#include <machine/../linux/linux.h>
#include <machine/../linux/linux_proto.h>
#endif
#include <compat/linux/linux_emul.h>
#include <compat/linux/linux_futex.h>
#include <compat/linux/linux_misc.h>
#include <compat/linux/linux_util.h>

#ifdef LINUX_LEGACY_SYSCALLS
int
linux_fork(struct thread *td, struct linux_fork_args *args)
{
	struct fork_req fr;
	int error;
	struct proc *p2;
	struct thread *td2;

	bzero(&fr, sizeof(fr));
	fr.fr_flags = RFFDG | RFPROC | RFSTOPPED;
	fr.fr_procp = &p2;
	if ((error = fork1(td, &fr)) != 0)
		return (error);

	td2 = FIRST_THREAD_IN_PROC(p2);

	linux_proc_init(td, td2, 0);

	td->td_retval[0] = p2->p_pid;

	/*
	 * Make this runnable after we are finished with it.
	 */
	thread_lock(td2);
	TD_SET_CAN_RUN(td2);
	sched_add(td2, SRQ_BORING);

	return (0);
}

int
linux_vfork(struct thread *td, struct linux_vfork_args *args)
{
	struct fork_req fr;
	int error;
	struct proc *p2;
	struct thread *td2;

	bzero(&fr, sizeof(fr));
	fr.fr_flags = RFFDG | RFPROC | RFMEM | RFPPWAIT | RFSTOPPED;
	fr.fr_procp = &p2;
	if ((error = fork1(td, &fr)) != 0)
		return (error);

	td2 = FIRST_THREAD_IN_PROC(p2);

	linux_proc_init(td, td2, 0);

	td->td_retval[0] = p2->p_pid;

	/*
	 * Make this runnable after we are finished with it.
	 */
	thread_lock(td2);
	TD_SET_CAN_RUN(td2);
	sched_add(td2, SRQ_BORING);

	return (0);
}
#endif

static int
linux_clone_proc(struct thread *td, struct linux_clone_args *args)
{
	struct fork_req fr;
	int error, ff = RFPROC | RFSTOPPED, f2;
	struct proc *p2;
	struct thread *td2;
	int exit_signal;
	struct linux_emuldata *em;

	f2 = 0;
	exit_signal = args->flags & 0x000000ff;
	if (LINUX_SIG_VALID(exit_signal)) {
		exit_signal = linux_to_bsd_signal(exit_signal);
	} else if (exit_signal != 0)
		return (EINVAL);

	if (args->flags & LINUX_CLONE_VM)
		ff |= RFMEM;
	if (args->flags & LINUX_CLONE_SIGHAND)
		ff |= RFSIGSHARE;
	if (args->flags & LINUX_CLONE_FILES) {
		if (!(args->flags & LINUX_CLONE_FS))
			f2 |= FR2_SHARE_PATHS;
	} else {
		ff |= RFFDG;
		if (args->flags & LINUX_CLONE_FS)
			f2 |= FR2_SHARE_PATHS;
	}

	if (args->flags & LINUX_CLONE_PARENT_SETTID)
		if (args->parent_tidptr == NULL)
			return (EINVAL);

	if (args->flags & LINUX_CLONE_VFORK)
		ff |= RFPPWAIT;

	bzero(&fr, sizeof(fr));
	fr.fr_flags = ff;
	fr.fr_flags2 = f2;
	fr.fr_procp = &p2;
	error = fork1(td, &fr);
	if (error)
		return (error);

	td2 = FIRST_THREAD_IN_PROC(p2);

	/* create the emuldata */
	linux_proc_init(td, td2, args->flags);

	em = em_find(td2);
	KASSERT(em != NULL, ("clone_proc: emuldata not found.\n"));

	if (args->flags & LINUX_CLONE_CHILD_SETTID)
		em->child_set_tid = args->child_tidptr;
	else
		em->child_set_tid = NULL;

	if (args->flags & LINUX_CLONE_CHILD_CLEARTID)
		em->child_clear_tid = args->child_tidptr;
	else
		em->child_clear_tid = NULL;

	if (args->flags & LINUX_CLONE_PARENT_SETTID) {
		error = copyout(&p2->p_pid, args->parent_tidptr,
		    sizeof(p2->p_pid));
		if (error)
			linux_msg(td, "copyout p_pid failed!");
	}

	PROC_LOCK(p2);
	p2->p_sigparent = exit_signal;
	PROC_UNLOCK(p2);
	/*
	 * In a case of stack = NULL, we are supposed to COW calling process
	 * stack. This is what normal fork() does, so we just keep tf_rsp arg
	 * intact.
	 */
	linux_set_upcall(td2, PTROUT(args->stack));

	if (args->flags & LINUX_CLONE_SETTLS)
		linux_set_cloned_tls(td2, args->tls);

	/*
	 * If CLONE_PARENT is set, then the parent of the new process will be
	 * the same as that of the calling process.
	 */
	if (args->flags & LINUX_CLONE_PARENT) {
		sx_xlock(&proctree_lock);
		PROC_LOCK(p2);
		proc_reparent(p2, td->td_proc->p_pptr, true);
		PROC_UNLOCK(p2);
		sx_xunlock(&proctree_lock);
	}

	/*
	 * Make this runnable after we are finished with it.
	 */
	thread_lock(td2);
	TD_SET_CAN_RUN(td2);
	sched_add(td2, SRQ_BORING);

	td->td_retval[0] = p2->p_pid;

	return (0);
}

static int
linux_clone_thread(struct thread *td, struct linux_clone_args *args)
{
	struct linux_emuldata *em;
	struct thread *newtd;
	struct proc *p;
	int error;

	LINUX_CTR4(clone_thread, "thread(%d) flags %x ptid %p ctid %p",
	    td->td_tid, (unsigned)args->flags,
	    args->parent_tidptr, args->child_tidptr);

	if ((args->flags & LINUX_CLONE_PARENT) != 0)
		return (EINVAL);
	if (args->flags & LINUX_CLONE_PARENT_SETTID)
		if (args->parent_tidptr == NULL)
			return (EINVAL);

	/* Threads should be created with own stack */
	if (args->stack == NULL)
		return (EINVAL);

	p = td->td_proc;

#ifdef RACCT
	if (racct_enable) {
		PROC_LOCK(p);
		error = racct_add(p, RACCT_NTHR, 1);
		PROC_UNLOCK(p);
		if (error != 0)
			return (EPROCLIM);
	}
#endif

	/* Initialize our td */
	error = kern_thr_alloc(p, 0, &newtd);
	if (error)
		goto fail;

	cpu_copy_thread(newtd, td);

	bzero(&newtd->td_startzero,
	    __rangeof(struct thread, td_startzero, td_endzero));
	bcopy(&td->td_startcopy, &newtd->td_startcopy,
	    __rangeof(struct thread, td_startcopy, td_endcopy));

	newtd->td_proc = p;
	thread_cow_get(newtd, td);

	/* create the emuldata */
	linux_proc_init(td, newtd, args->flags);

	em = em_find(newtd);
	KASSERT(em != NULL, ("clone_thread: emuldata not found.\n"));

	if (args->flags & LINUX_CLONE_SETTLS)
		linux_set_cloned_tls(newtd, args->tls);

	if (args->flags & LINUX_CLONE_CHILD_SETTID)
		em->child_set_tid = args->child_tidptr;
	else
		em->child_set_tid = NULL;

	if (args->flags & LINUX_CLONE_CHILD_CLEARTID)
		em->child_clear_tid = args->child_tidptr;
	else
		em->child_clear_tid = NULL;

	cpu_thread_clean(newtd);

	linux_set_upcall(newtd, PTROUT(args->stack));

	PROC_LOCK(p);
	p->p_flag |= P_HADTHREADS;
	thread_link(newtd, p);
	bcopy(p->p_comm, newtd->td_name, sizeof(newtd->td_name));

	thread_lock(td);
	/* let the scheduler know about these things. */
	sched_fork_thread(td, newtd);
	thread_unlock(td);
	if (P_SHOULDSTOP(p))
		newtd->td_flags |= TDF_ASTPENDING | TDF_NEEDSUSPCHK;

	if (p->p_ptevents & PTRACE_LWP)
		newtd->td_dbgflags |= TDB_BORN;
	PROC_UNLOCK(p);

	tidhash_add(newtd);

	LINUX_CTR2(clone_thread, "thread(%d) successful clone to %d",
	    td->td_tid, newtd->td_tid);

	if (args->flags & LINUX_CLONE_PARENT_SETTID) {
		error = copyout(&newtd->td_tid, args->parent_tidptr,
		    sizeof(newtd->td_tid));
		if (error)
			linux_msg(td, "clone_thread: copyout td_tid failed!");
	}

	/*
	 * Make this runnable after we are finished with it.
	 */
	thread_lock(newtd);
	TD_SET_CAN_RUN(newtd);
	sched_add(newtd, SRQ_BORING);

	td->td_retval[0] = newtd->td_tid;

	return (0);

fail:
#ifdef RACCT
	if (racct_enable) {
		PROC_LOCK(p);
		racct_sub(p, RACCT_NTHR, 1);
		PROC_UNLOCK(p);
	}
#endif
	return (error);
}

int
linux_clone(struct thread *td, struct linux_clone_args *args)
{

	if (args->flags & LINUX_CLONE_THREAD)
		return (linux_clone_thread(td, args));
	else
		return (linux_clone_proc(td, args));
}

int
linux_exit(struct thread *td, struct linux_exit_args *args)
{
	struct linux_emuldata *em;

	em = em_find(td);
	KASSERT(em != NULL, ("exit: emuldata not found.\n"));

	LINUX_CTR2(exit, "thread(%d) (%d)", em->em_tid, args->rval);

	linux_thread_detach(td);

	/*
	 * XXX. When the last two threads of a process
	 * exit via pthread_exit() try thr_exit() first.
	 */
	kern_thr_exit(td);
	exit1(td, args->rval, 0);
		/* NOTREACHED */
}

int
linux_set_tid_address(struct thread *td, struct linux_set_tid_address_args *args)
{
	struct linux_emuldata *em;

	em = em_find(td);
	KASSERT(em != NULL, ("set_tid_address: emuldata not found.\n"));

	em->child_clear_tid = args->tidptr;

	td->td_retval[0] = em->em_tid;

	LINUX_CTR3(set_tid_address, "tidptr(%d) %p, returns %d",
	    em->em_tid, args->tidptr, td->td_retval[0]);

	return (0);
}

void
linux_thread_detach(struct thread *td)
{
	struct linux_emuldata *em;
	int *child_clear_tid;
	int error;

	em = em_find(td);
	KASSERT(em != NULL, ("thread_detach: emuldata not found.\n"));

	LINUX_CTR1(thread_detach, "thread(%d)", em->em_tid);

	release_futexes(td, em);

	child_clear_tid = em->child_clear_tid;

	if (child_clear_tid != NULL) {
		LINUX_CTR2(thread_detach, "thread(%d) %p",
		    em->em_tid, child_clear_tid);

		error = suword32(child_clear_tid, 0);
		if (error != 0)
			return;

		error = futex_wake(td, child_clear_tid, 1, false);
		/*
		 * this cannot happen at the moment and if this happens it
		 * probably means there is a user space bug
		 */
		if (error != 0)
			linux_msg(td, "futex stuff in thread_detach failed.");
	}

	/*
	 * Do not rely on the robust list which is maintained by userspace,
	 * cleanup remaining pi (if any) after release_futexes anyway.
	 */
	umtx_thread_exit(td);
}
