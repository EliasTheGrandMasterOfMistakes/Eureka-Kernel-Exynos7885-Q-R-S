/*
 *
 * (C) COPYRIGHT 2010-2020 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 * SPDX-License-Identifier: GPL-2.0
 *
 */



/**
 * @file mali_kbase_pm.c
 * Base kernel power management APIs
 */
#include <mali_kbase.h>
#include <gpu/mali_kbase_gpu_regmap.h>
#include <mali_kbase_vinstr.h>
#include <mali_kbase_hwcnt_context.h>

#include <mali_kbase_pm.h>
#include <mali_kbase_pm_internal.h>

#ifdef CONFIG_MALI_ARBITER_SUPPORT
#include <arbiter/mali_kbase_arbiter_pm.h>
#endif /* CONFIG_MALI_ARBITER_SUPPORT */

#include <mali_kbase_clk_rate_trace_mgr.h>

int kbase_pm_powerup(struct kbase_device *kbdev, unsigned int flags)
{
	return kbase_hwaccess_pm_powerup(kbdev, flags);
}

void kbase_pm_halt(struct kbase_device *kbdev)
{
	kbase_hwaccess_pm_halt(kbdev);
}

void kbase_pm_context_active(struct kbase_device *kbdev)
{
	(void)kbase_pm_context_active_handle_suspend(kbdev,
		KBASE_PM_SUSPEND_HANDLER_NOT_POSSIBLE);
}

int kbase_pm_context_active_handle_suspend(struct kbase_device *kbdev,
	enum kbase_pm_suspend_handler suspend_handler)
{
	int c;

	KBASE_DEBUG_ASSERT(kbdev != NULL);
	dev_dbg(kbdev->dev, "%s - reason = %d, pid = %d\n", __func__,
		suspend_handler, current->pid);
	kbase_pm_lock(kbdev);

#ifdef CONFIG_MALI_ARBITER_SUPPORT
	if (kbase_arbiter_pm_ctx_active_handle_suspend(kbdev, suspend_handler))
		return 1;

	if (kbase_pm_is_suspending(kbdev) ||
		kbase_pm_is_gpu_lost(kbdev)) {
#else
	if (kbase_pm_is_suspending(kbdev)) {
#endif /* CONFIG_MALI_ARBITER_SUPPORT */
		switch (suspend_handler) {
		case KBASE_PM_SUSPEND_HANDLER_DONT_REACTIVATE:
			if (kbdev->pm.active_count != 0)
				break;
			/* FALLTHROUGH */
		case KBASE_PM_SUSPEND_HANDLER_DONT_INCREASE:
			kbase_pm_unlock(kbdev);
			return 1;

		case KBASE_PM_SUSPEND_HANDLER_NOT_POSSIBLE:
			/* FALLTHROUGH */
		default:
			KBASE_DEBUG_ASSERT_MSG(false, "unreachable");
			break;
		}
	}
	c = ++kbdev->pm.active_count;
	KBASE_KTRACE_ADD(kbdev, PM_CONTEXT_ACTIVE, NULL, c);

	if (c == 1) {
		/* First context active: Power on the GPU and
		 * any cores requested by the policy
		 */
		kbase_hwaccess_pm_gpu_active(kbdev);
#ifdef CONFIG_MALI_ARBITER_SUPPORT
		kbase_arbiter_pm_vm_event(kbdev, KBASE_VM_REF_EVENT);
#endif /* CONFIG_MALI_ARBITER_SUPPORT */
		kbase_clk_rate_trace_manager_gpu_active(kbdev);
	}

	kbase_pm_unlock(kbdev);
	dev_dbg(kbdev->dev, "%s %d\n", __func__, kbdev->pm.active_count);

	return 0;
}

KBASE_EXPORT_TEST_API(kbase_pm_context_active);

void kbase_pm_context_idle(struct kbase_device *kbdev)
{
	int c;

	KBASE_DEBUG_ASSERT(kbdev != NULL);


	kbase_pm_lock(kbdev);

	c = --kbdev->pm.active_count;
	KBASE_KTRACE_ADD(kbdev, PM_CONTEXT_IDLE, NULL, c);

	KBASE_DEBUG_ASSERT(c >= 0);

	if (c == 0) {
		/* Last context has gone idle */
		kbase_hwaccess_pm_gpu_idle(kbdev);
		kbase_clk_rate_trace_manager_gpu_idle(kbdev);

		/* Wake up anyone waiting for this to become 0 (e.g. suspend).
		 * The waiters must synchronize with us by locking the pm.lock
		 * after waiting.
		 */
		wake_up(&kbdev->pm.zero_active_count_wait);
	}

	kbase_pm_unlock(kbdev);
	dev_dbg(kbdev->dev, "%s %d (pid = %d)\n", __func__,
		kbdev->pm.active_count, current->pid);
}

KBASE_EXPORT_TEST_API(kbase_pm_context_idle);

void kbase_pm_driver_suspend(struct kbase_device *kbdev)
{
	KBASE_DEBUG_ASSERT(kbdev);

	/* Suspend vinstr. This blocks until the vinstr worker and timer are
	 * no longer running.
	 */
	kbase_vinstr_suspend(kbdev->vinstr_ctx);

	/* Disable GPU hardware counters.
	 * This call will block until counters are disabled.
	 */
	kbase_hwcnt_context_disable(kbdev->hwcnt_gpu_ctx);

	mutex_lock(&kbdev->pm.lock);
	if (WARN_ON(kbase_pm_is_suspending(kbdev))) {
		mutex_unlock(&kbdev->pm.lock);
		return;
	}
	kbdev->pm.suspending = true;
	mutex_unlock(&kbdev->pm.lock);

#ifdef CONFIG_MALI_ARBITER_SUPPORT
	if (kbdev->arb.arb_if) {
		int i;
		unsigned long flags;

		spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
		kbase_disjoint_state_up(kbdev);
		for (i = 0; i < kbdev->gpu_props.num_job_slots; i++)
			kbase_job_slot_softstop(kbdev, i, NULL);
		spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
	}
#endif /* CONFIG_MALI_ARBITER_SUPPORT */

	/* From now on, the active count will drop towards zero. Sometimes,
	 * it'll go up briefly before going down again. However, once
	 * it reaches zero it will stay there - guaranteeing that we've idled
	 * all pm references
	 */
	/* MALI_SEC_INTEGRATION */
	KBASE_KTRACE_ADD(kbdev, LSI_PM_SUSPEND, NULL, 0);

	/* Suspend job scheduler and associated components, so that it releases all
	 * the PM active count references */
	kbasep_js_suspend(kbdev);

	/* Wait for the active count to reach zero. This is not the same as
	 * waiting for a power down, since not all policies power down when this
	 * reaches zero.
	 */
	dev_dbg(kbdev->dev, ">wait_event - waiting for active_count == 0 (pid = %d)\n",
		current->pid);
	wait_event(kbdev->pm.zero_active_count_wait,
		kbdev->pm.active_count == 0);
	dev_dbg(kbdev->dev, ">wait_event - waiting done\n");

	/* NOTE: We synchronize with anything that was just finishing a
	 * kbase_pm_context_idle() call by locking the pm.lock below
	 */
	kbase_hwaccess_pm_suspend(kbdev);

#ifdef CONFIG_MALI_ARBITER_SUPPORT
	if (kbdev->arb.arb_if) {
		mutex_lock(&kbdev->pm.arb_vm_state->vm_state_lock);
		kbase_arbiter_pm_vm_stopped(kbdev);
		mutex_unlock(&kbdev->pm.arb_vm_state->vm_state_lock);
	}
#endif /* CONFIG_MALI_ARBITER_SUPPORT */
}

void kbase_pm_driver_resume(struct kbase_device *kbdev, bool arb_gpu_start)
{
	unsigned long flags;

	/* MUST happen before any pm_context_active calls occur */
	kbase_hwaccess_pm_resume(kbdev);

	/* Initial active call, to power on the GPU/cores if needed */
#ifdef CONFIG_MALI_ARBITER_SUPPORT
	(void)kbase_pm_context_active_handle_suspend(kbdev,
		(arb_gpu_start ?
			KBASE_PM_SUSPEND_HANDLER_VM_GPU_GRANTED :
			KBASE_PM_SUSPEND_HANDLER_NOT_POSSIBLE));
#else
	kbase_pm_context_active(kbdev);
#endif

	/* Resume any blocked atoms (which may cause contexts to be scheduled in
	 * and dependent atoms to run)
	 */
	kbase_resume_suspended_soft_jobs(kbdev);

	/* Resume the Job Scheduler and associated components, and start running
	 * atoms
	 */
	kbasep_js_resume(kbdev);

	/* Matching idle call, to power off the GPU/cores if we didn't actually
	 * need it and the policy doesn't want it on
	 */
	kbase_pm_context_idle(kbdev);

	/* Re-enable GPU hardware counters */
	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	kbase_hwcnt_context_enable(kbdev->hwcnt_gpu_ctx);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	/* Resume vinstr */
	kbase_vinstr_resume(kbdev->vinstr_ctx);
}

void kbase_pm_suspend(struct kbase_device *kbdev)
{
#ifdef CONFIG_MALI_ARBITER_SUPPORT
	if (kbdev->arb.arb_if)
		kbase_arbiter_pm_vm_event(kbdev, KBASE_VM_OS_SUSPEND_EVENT);
	else
		kbase_pm_driver_suspend(kbdev);
#else
	kbase_pm_driver_suspend(kbdev);
#endif /* CONFIG_MALI_ARBITER_SUPPORT */
}

void kbase_pm_resume(struct kbase_device *kbdev)
{
#ifdef CONFIG_MALI_ARBITER_SUPPORT
	if (kbdev->arb.arb_if)
		kbase_arbiter_pm_vm_event(kbdev, KBASE_VM_OS_RESUME_EVENT);
	else
		kbase_pm_driver_resume(kbdev, false);
#else
	kbase_pm_driver_resume(kbdev, false);
#endif /* CONFIG_MALI_ARBITER_SUPPORT */
}

/**
 * kbase_pm_apc_power_off_worker - Power off worker running on mali_apc_thread
 * @data: A &struct kthread_work
 *
 * This worker runs kbase_pm_context_idle on mali_apc_thread.
 */
static void kbase_pm_apc_power_off_worker(struct kthread_work *data)
{
	struct kbase_device *kbdev = container_of(data, struct kbase_device,
			apc.power_off_work);

	kbase_pm_context_idle(kbdev);
}

/**
 * kbase_pm_apc_timer_callback - Timer callback for powering off the GPU
 * @data: A &struct kthread_work
 *
 * This hrtimer callback queues the power off work to mali_apc_thread.
 *
 * Return: Always returns HRTIMER_NORESTART.
 */
static enum hrtimer_restart kbase_pm_apc_timer_callback(struct hrtimer *timer)
{
	struct kbase_device *kbdev =
			container_of(timer, struct kbase_device, apc.timer);

	kthread_init_work(&kbdev->apc.power_off_work,
			kbase_pm_apc_power_off_worker);
	kthread_queue_work(&kbdev->apc.worker, &kbdev->apc.power_off_work);
	return HRTIMER_NORESTART;
}

int kbase_pm_apc_init(struct kbase_device *kbdev)
{
	static const struct sched_param param = {
		.sched_priority = KBASE_APC_THREAD_RT_PRIO,
	};
	/* TODO(b/181145264) get this number from the device tree */
	static const unsigned int nr_little_cores = 4;
	cpumask_t mask = { CPU_BITS_NONE };

	kthread_init_worker(&kbdev->apc.worker);
	kbdev->apc.thread = kthread_create(kthread_worker_fn,
		&kbdev->apc.worker, "mali_apc_thread");
	if (IS_ERR(kbdev->apc.thread))
		return -ENOMEM;

	for (unsigned int i = 0; i < nr_little_cores; i++)
		cpumask_set_cpu(i, &mask);
	kthread_bind_mask(kbdev->apc.thread, &mask);
	wake_up_process(kbdev->apc.thread);

	if (sched_setscheduler(kbdev->apc.thread, SCHED_FIFO, &param))
		dev_warn(kbdev->dev, "mali_apc_thread not set to RT prio");
	else
		dev_dbg(kbdev->dev, "mali_apc_thread set to RT prio: %i",
			param.sched_priority);

	hrtimer_init(&kbdev->apc.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	kbdev->apc.timer.function = kbase_pm_apc_timer_callback;
	mutex_init(&kbdev->apc.lock);
	return 0;
}

void kbase_pm_apc_term(struct kbase_device *kbdev)
{
	hrtimer_cancel(&kbdev->apc.timer);
	kthread_flush_worker(&kbdev->apc.worker);
	kthread_stop(kbdev->apc.thread);
}

/**
 * kbase_pm_apc_power_on_worker - Power on worker running on mali_apc_thread
 * @data: A &struct kthread_work
 *
 * This worker handles the power on request on mali_apc_thread.
 *
 * Normally it will power on the GPU and schedule a timer to power off the GPU
 * based on the requested wake duration.
 *
 * If the driver is suspending, it won't power on the GPU or schedule the timer
 * for powering off.
 */
static void kbase_pm_apc_power_on_worker(struct kthread_work *data)
{
	struct kbase_device *kbdev =
			container_of(data, struct kbase_device,
					apc.power_on_work);
	ktime_t cur_ts;

	if (kbase_pm_context_active_handle_suspend(kbdev,
			KBASE_PM_SUSPEND_HANDLER_DONT_INCREASE))
		return;

	mutex_lock(&kbdev->apc.lock);
	cur_ts = ktime_get();
	if (ktime_after(kbdev->apc.end_ts, cur_ts)) {
		hrtimer_start(&kbdev->apc.timer,
				ktime_sub(kbdev->apc.end_ts, cur_ts),
				HRTIMER_MODE_REL);
		mutex_unlock(&kbdev->apc.lock);
		return;
	}
	mutex_unlock(&kbdev->apc.lock);

	/* When relative duration is non-positive, queue power off work here. */
	kthread_init_work(&kbdev->apc.power_off_work,
			kbase_pm_apc_power_off_worker);
	kthread_queue_work(&kbdev->apc.worker, &kbdev->apc.power_off_work);
}

void kbase_pm_apc_request(struct kbase_device *kbdev, u32 dur_usec)
{
	ktime_t req_ts;

	mutex_lock(&kbdev->apc.lock);
	req_ts = ktime_add_us(ktime_get(),
			min(dur_usec, (u32)KBASE_APC_MAX_DUR_USEC));
	if (!ktime_after(req_ts, kbdev->apc.end_ts))
		goto out;

	/* When the return value of hrtimer_try_to_cancel() is:
	 *  1: Timer is canceled, so restart to extend wake duration and exit.
	 *  0: Timer is inactive, so we follow normal power on sequence below.
	 * -1: Timer callback is running, so we need to follow normal power on
	 *     sequence again since we are not able to update the timer now.
	 */
	if (hrtimer_try_to_cancel(&kbdev->apc.timer) == 1) {
		hrtimer_start(&kbdev->apc.timer,
				ktime_sub(req_ts, kbdev->apc.end_ts),
				HRTIMER_MODE_REL);
		goto out;
	}
	kbdev->apc.end_ts = req_ts;
	mutex_unlock(&kbdev->apc.lock);

	kthread_init_work(&kbdev->apc.power_on_work,
			kbase_pm_apc_power_on_worker);
	kthread_queue_work(&kbdev->apc.worker, &kbdev->apc.power_on_work);
	return;

out:
	mutex_unlock(&kbdev->apc.lock);
}
