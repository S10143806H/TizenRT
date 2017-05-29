/****************************************************************************
 *
 * Copyright 2016 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/
/****************************************************************************
 * kernel/task/task_delete.c
 *
 *   Copyright (C) 2007-2009, 2011-2013 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <tinyara/config.h>

#include <stdlib.h>
#include <errno.h>

#include <tinyara/sched.h>

#include "sched/sched.h"
#include "task/task.h"

/****************************************************************************
 * Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Type Declarations
 ****************************************************************************/

/****************************************************************************
 * Global Variables
 ****************************************************************************/

/****************************************************************************
 * Private Variables
 ****************************************************************************/

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: task_delete
 *
 * Description:
 *   This function causes a specified task to cease to exist. Its  stack and
 *   TCB will be deallocated.  This function is the companion to task_create().
 *   This is the version of the function exposed to the user; it is simply
 *   a wrapper around the internal, task_terminate function.
 *
 *   The logic in this function only deletes non-running tasks.  If the 'pid'
 *   parameter refers to to the currently runing task, then processing is
 *   redirected to exit().  This can only happen if a task calls task_delete()
 *   in order to delete itself.
 *
 *   This function obeys the semantics of pthread cancellation: task
 *  deletion is deferred if cancellation is disabled or if deferred
 *  cancellation is supported (with cancellation points enabled).
 *
 * Inputs:
 *   pid - The task ID of the task to delete.  A pid of zero
 *         signifies the calling task.
 *
 * Return Value:
 *  OK on success; or ERROR on failure with the errno variable set
 *  appropriately.
 *
 ****************************************************************************/

int task_delete(pid_t pid)
{
	FAR struct tcb_s *dtcb;
	FAR struct tcb_s *rtcb;
	int ret;

	/* Check if the task to delete is the calling task:  PID=0 means to delete
	 * the calling task.  In this case, task_delete() is much like exit()
	 * except that it obeys the cancellation semantics.
	 */

	rtcb = (FAR struct tcb_s *)g_readytorun.head;
	if (pid == 0) {
		pid = rtcb->pid;
	}

	/* Get the TCB of the task to be deleted */

	dtcb = (FAR struct tcb_s *)sched_gettcb(pid);
	if (dtcb == NULL) {
		/* The pid does not correspond to any known thread.  The task
		 * has probably already exited.
		 */

		set_errno(ESRCH);
		return ERROR;
	}

	/* Only tasks and kernel threads should use this interface */

	DEBUGASSERT((dtcb->flags & TCB_FLAG_TTYPE_MASK) != TCB_FLAG_TTYPE_PTHREAD);

	/* Check to see if this task has the non-cancelable bit set in its
	 * flags. Suppress context changes for a bit so that the flags are stable.
	 * (the flags should not change in interrupt handling).
	 */

	sched_lock();
	if ((dtcb->flags & TCB_FLAG_NONCANCELABLE) != 0) {
		/* Then we cannot cancel the thread now.  Here is how this is
		 * supposed to work:
		 *
		 * "When cancelability is disabled, all cancels are held pending
		 *  in the target thread until the thread changes the cancelability.
		 *  When cancelability is deferred, all cancels are held pending in
		 *  the target thread until the thread changes the cancelability, calls
		 *  a function which is a cancellation point or calls pthread_testcancel(),
		 *  thus creating a cancellation point. When cancelability is asynchronous,
		 *  all cancels are acted upon immediately, interrupting the thread with its
		 *  processing."
		 */

		dtcb->flags |= TCB_FLAG_CANCEL_PENDING;
		sched_unlock();
		return OK;
	}

#ifdef CONFIG_CANCELLATION_POINTS
	/* Check if this task supports deferred cancellation */

	if ((dtcb->flags & TCB_FLAG_CANCEL_DEFERRED) != 0) {

		/* If the task is waiting at a cancellation point, then notify of the
		 * cancellation thereby waking the task up with an ECANCELED error.
		 *
		 * REVISIT: is locking the scheduler sufficent in SMP mode?
		 */

		dtcb->flags |= TCB_FLAG_CANCEL_PENDING;

		if (dtcb->cpcount > 0) {
			notify_cancellation(dtcb);
		}

		sched_unlock();
		return OK;
	}
#endif

	/* Check if the task to delete is the calling task */

	sched_unlock();
	if (pid == rtcb->pid) {
		/* If it is, then what we really wanted to do was exit. Note that we
		 * don't bother to unlock the TCB since it will be going away.
		 */

		exit(EXIT_SUCCESS);
	}

	/* Otherwise, perform the asynchronous cancellation, letting
	 * task_terminate() do all of the heavy lifting.
	 */

	ret = task_terminate(pid, false);
	if (ret < 0) {
		set_errno(-ret);
		return ERROR;
	}

	return OK;
}
