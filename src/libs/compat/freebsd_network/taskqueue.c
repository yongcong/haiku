/*
 * Copyright 2007, Hugo Santos. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *      Hugo Santos, hugosantos@gmail.com
 */

#include "device.h"

#include <stdio.h>

#include <compat/sys/taskqueue.h>
#include <compat/sys/haiku-module.h>

struct taskqueue {
	char tq_name[64];
	mutex tq_mutex;
	struct list tq_list;
	taskqueue_enqueue_fn tq_enqueue;
	void *tq_arg;
	int tq_fast;
	int32 tq_spinlock;
	sem_id tq_sem;
	thread_id *tq_threads;
	thread_id tq_thread_storage;
	int tq_threadcount;
};


struct taskqueue *taskqueue_fast = NULL;
struct taskqueue *taskqueue_swi = NULL;


static struct taskqueue *
_taskqueue_create(const char *name, int mflags, int fast,
	taskqueue_enqueue_fn enqueue, void *context)
{
	struct taskqueue *tq = malloc(sizeof(struct taskqueue));
	if (tq == NULL)
		return NULL;

	tq->tq_fast = fast;

	if (fast) {
		tq->tq_spinlock = 0;
	} else {
		mutex_init_etc(&tq->tq_mutex, name, MUTEX_FLAG_CLONE_NAME);
	}

	strlcpy(tq->tq_name, name, sizeof(tq->tq_name));
	list_init_etc(&tq->tq_list, offsetof(struct task, ta_link));
	tq->tq_enqueue = enqueue;
	tq->tq_arg = context;

	tq->tq_sem = -1;
	tq->tq_threads = NULL;
	tq->tq_threadcount = 0;

	return tq;
}


static void
tq_lock(struct taskqueue *tq, cpu_status *status)
{
	if (tq->tq_fast) {
		*status = disable_interrupts();
		acquire_spinlock(&tq->tq_spinlock);
	} else {
		mutex_lock(&tq->tq_mutex);
	}
}


static void
tq_unlock(struct taskqueue *tq, cpu_status status)
{
	if (tq->tq_fast) {
		release_spinlock(&tq->tq_spinlock);
		restore_interrupts(status);
	} else {
		mutex_unlock(&tq->tq_mutex);
	}
}


struct taskqueue *
taskqueue_create(const char *name, int mflags, taskqueue_enqueue_fn enqueue,
	void *context, void **unused)
{
	return _taskqueue_create(name, mflags, 0, enqueue, context);
}


static int32
tq_handle_thread(void *data)
{
	struct taskqueue *tq = data;
	cpu_status cpu_state;
	struct task *t;
	int pending;
	sem_id sem;

	/* just a synchronization point */
	tq_lock(tq, &cpu_state);
	sem = tq->tq_sem;
	tq_unlock(tq, cpu_state);

	while (1) {
		status_t status = acquire_sem(sem);
		if (status < B_OK)
			break;

		tq_lock(tq, &cpu_state);
		t = list_remove_head_item(&tq->tq_list);
		pending = t->ta_pending;
		t->ta_pending = 0;
		tq_unlock(tq, cpu_state);

		t->ta_handler(t->ta_argument, pending);
	}

	return 0;
}


static int
_taskqueue_start_threads(struct taskqueue **tqp, int count, int prio,
	const char *name)
{
	struct taskqueue *tq = (*tqp);
	int i, j;

	if (count == 0)
		return -1;

	if (tq->tq_threads != NULL)
		return -1;

	if (count == 1) {
		tq->tq_threads = &tq->tq_thread_storage;
	} else {
		tq->tq_threads = malloc(sizeof(thread_id) * count);
		if (tq->tq_threads == NULL)
			return B_NO_MEMORY;
	}

	tq->tq_sem = create_sem(0, tq->tq_name);
	if (tq->tq_sem < B_OK) {
		if (count > 1)
			free(tq->tq_threads);
		tq->tq_threads = NULL;
		return tq->tq_sem;
	}

	for (i = 0; i < count; i++) {
		tq->tq_threads[i] = spawn_kernel_thread(tq_handle_thread, tq->tq_name,
			prio, tq);
		if (tq->tq_threads[i] < B_OK) {
			status_t status = tq->tq_threads[i];
			for (j = 0; j < i; j++)
				kill_thread(tq->tq_threads[j]);
			if (count > 1)
				free(tq->tq_threads);
			tq->tq_threads = NULL;
			delete_sem(tq->tq_sem);
			return status;
		}
	}

	tq->tq_threadcount = count;

	for (i = 0; i < count; i++)
		resume_thread(tq->tq_threads[i]);

	return 0;
}


int
taskqueue_start_threads(struct taskqueue **tqp, int count, int prio,
	const char *format, ...)
{
	/* we assume that start_threads is called in a sane place, and thus
	 * don't need to be locked. This is mostly due to the fact that if
	 * the TQ is 'fast', locking the TQ disables interrupts... and then
	 * we can't create semaphores, threads and bananas. */

	/* cpu_status state; */
	char name[64];
	int result;
	va_list vl;

	va_start(vl, format);
	vsnprintf(name, sizeof(name), format, vl);
	va_end(vl);

	/*tq_lock(*tqp, &state);*/
	result = _taskqueue_start_threads(tqp, count, prio, name);
	/*tq_unlock(*tqp, state);*/

	return result;
}


void
taskqueue_free(struct taskqueue *tq)
{
	/* lock and  drain list? */
	if (!tq->tq_fast)
		mutex_destroy(&tq->tq_mutex);
	if (tq->tq_sem != -1) {
		int i;

		delete_sem(tq->tq_sem);

		for (i = 0; i < tq->tq_threadcount; i++) {
			status_t status;
			wait_for_thread(tq->tq_threads[i], &status);
		}

		if (tq->tq_threadcount > 1)
			free(tq->tq_threads);
	}

	free(tq);
}


void
taskqueue_drain(struct taskqueue *tq, struct task *task)
{
	cpu_status status;

	tq_lock(tq, &status);
	if (task->ta_pending != 0)
		UNIMPLEMENTED();
	tq_unlock(tq, status);
}


int
taskqueue_enqueue(struct taskqueue *tq, struct task *task)
{
	cpu_status status;
	tq_lock(tq, &status);
	/* we don't really support priorities */
	if (task->ta_pending) {
		task->ta_pending++;
	} else {
		list_add_item(&tq->tq_list, task);
		task->ta_pending = 1;
		tq->tq_enqueue(tq->tq_arg);
	}
	tq_unlock(tq, status);
	return 0;
}


void
taskqueue_thread_enqueue(void *context)
{
	struct taskqueue **tqp = context;
	release_sem_etc((*tqp)->tq_sem, 1, B_DO_NOT_RESCHEDULE);
}


int
taskqueue_enqueue_fast(struct taskqueue *tq, struct task *task)
{
	return taskqueue_enqueue(tq, task);
}


struct taskqueue *
taskqueue_create_fast(const char *name, int mflags,
	taskqueue_enqueue_fn enqueue, void *context)
{
	return _taskqueue_create(name, mflags, 1, enqueue, context);
}


void
task_init(struct task *t, int prio, task_handler_t handler, void *context)
{
	t->ta_priority = prio;
	t->ta_handler = handler;
	t->ta_argument = context;
	t->ta_pending = 0;
}


status_t
init_taskqueues()
{
	status_t status = B_NO_MEMORY;

	if (HAIKU_DRIVER_REQUIRES(FBSD_FAST_TASKQUEUE)) {
		taskqueue_fast = taskqueue_create_fast("fast taskq", 0,
			taskqueue_thread_enqueue, &taskqueue_fast);
		if (taskqueue_fast == NULL)
			return B_NO_MEMORY;

		status = taskqueue_start_threads(&taskqueue_fast, 1,
			B_REAL_TIME_PRIORITY, "fast taskq");
		if (status < B_OK)
			goto err_1;
	}

	if (HAIKU_DRIVER_REQUIRES(FBSD_SWI_TASKQUEUE)) {
		taskqueue_swi = taskqueue_create_fast("swi taskq", 0,
			taskqueue_thread_enqueue, &taskqueue_swi);
		if (taskqueue_swi == NULL) {
			status = B_NO_MEMORY;
			goto err_1;
		}

		status = taskqueue_start_threads(&taskqueue_swi, 1,
			B_REAL_TIME_PRIORITY, "swi taskq");
		if (status < B_OK)
			goto err_2;
	}

	return B_OK;

err_2:
	if (taskqueue_swi)
		taskqueue_free(taskqueue_swi);

err_1:
	if (taskqueue_fast)
		taskqueue_free(taskqueue_fast);

	return status;
}


void
uninit_taskqueues()
{
	if (HAIKU_DRIVER_REQUIRES(FBSD_SWI_TASKQUEUE))
		taskqueue_free(taskqueue_swi);

	if (HAIKU_DRIVER_REQUIRES(FBSD_FAST_TASKQUEUE))
		taskqueue_free(taskqueue_fast);
}
