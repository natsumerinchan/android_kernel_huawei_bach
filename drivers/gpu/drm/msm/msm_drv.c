/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * Copyright (c) 2017-2018 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "msm_drv.h"
#include "msm_gpu.h"
#include "msm_kms.h"
#include "display_manager.h"
#include "sde_wb.h"
#include "msm_mmu.h"

static void msm_fb_output_poll_changed(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	if (priv->fbdev)
		drm_fb_helper_hotplug_event(priv->fbdev);
}

static const struct drm_mode_config_funcs mode_config_funcs = {
	.fb_create = msm_framebuffer_create,
	.output_poll_changed = msm_fb_output_poll_changed,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = msm_atomic_commit,
};

int msm_register_mmu(struct drm_device *dev, struct msm_mmu *mmu)
{
	struct msm_drm_private *priv = dev->dev_private;
	int idx = priv->num_mmus++;

	if (WARN_ON(idx >= ARRAY_SIZE(priv->mmus)))
		return -EINVAL;

	priv->mmus[idx] = mmu;

	return idx;
}

#ifdef CONFIG_DRM_MSM_REGISTER_LOGGING
static bool reglog = false;
MODULE_PARM_DESC(reglog, "Enable register read/write logging");
module_param(reglog, bool, 0600);
#else
#define reglog 0
#endif

#ifdef CONFIG_DRM_FBDEV_EMULATION
static bool fbdev = true;
MODULE_PARM_DESC(fbdev, "Enable fbdev compat layer");
module_param(fbdev, bool, 0600);
#endif

static char *vram = "16m";
MODULE_PARM_DESC(vram, "Configure VRAM size (for devices without IOMMU/GPUMMU");
module_param(vram, charp, 0);

/*
 * Util/helpers:
 */

void __iomem *msm_ioremap(struct platform_device *pdev, const char *name,
		const char *dbgname)
{
	struct resource *res;
	unsigned long size;
	void __iomem *ptr;

	if (name)
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	else
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (!res) {
		dev_err(&pdev->dev, "failed to get memory resource: %s\n", name);
		return ERR_PTR(-EINVAL);
	}

	size = resource_size(res);

	ptr = devm_ioremap_nocache(&pdev->dev, res->start, size);
	if (!ptr) {
		dev_err(&pdev->dev, "failed to ioremap: %s\n", name);
		return ERR_PTR(-ENOMEM);
	}

	if (reglog)
		pr_debug("IO:region %s %pK %08lx\n",
			dbgname, ptr, size);

	return ptr;
}

void msm_writel(u32 data, void __iomem *addr)
{
	if (reglog)
		pr_debug("IO:W %pK %08x\n", addr, data);
	writel(data, addr);
}

u32 msm_readl(const void __iomem *addr)
{
	u32 val = readl(addr);
	if (reglog)
		pr_debug("IO:R %pK %08x\n", addr, val);
	return val;
}

struct vblank_event {
	struct list_head node;
	int crtc_id;
	bool enable;
};

static void vblank_ctrl_worker(struct work_struct *work)
{
	struct msm_vblank_ctrl *vbl_ctrl = container_of(work,
						struct msm_vblank_ctrl, work);
	struct msm_drm_private *priv = container_of(vbl_ctrl,
					struct msm_drm_private, vblank_ctrl);
	struct msm_kms *kms = priv->kms;
	struct vblank_event *vbl_ev, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&vbl_ctrl->lock, flags);
	list_for_each_entry_safe(vbl_ev, tmp, &vbl_ctrl->event_list, node) {
		list_del(&vbl_ev->node);
		spin_unlock_irqrestore(&vbl_ctrl->lock, flags);

		if (vbl_ev->enable)
			kms->funcs->enable_vblank(kms,
						priv->crtcs[vbl_ev->crtc_id]);
		else
			kms->funcs->disable_vblank(kms,
						priv->crtcs[vbl_ev->crtc_id]);

		kfree(vbl_ev);

		spin_lock_irqsave(&vbl_ctrl->lock, flags);
	}

	spin_unlock_irqrestore(&vbl_ctrl->lock, flags);
}

static int vblank_ctrl_queue_work(struct msm_drm_private *priv,
					int crtc_id, bool enable)
{
	struct msm_vblank_ctrl *vbl_ctrl = &priv->vblank_ctrl;
	struct vblank_event *vbl_ev;
	unsigned long flags;

	vbl_ev = kzalloc(sizeof(*vbl_ev), GFP_ATOMIC);
	if (!vbl_ev)
		return -ENOMEM;

	vbl_ev->crtc_id = crtc_id;
	vbl_ev->enable = enable;

	spin_lock_irqsave(&vbl_ctrl->lock, flags);
	list_add_tail(&vbl_ev->node, &vbl_ctrl->event_list);
	spin_unlock_irqrestore(&vbl_ctrl->lock, flags);

	queue_work(priv->wq, &vbl_ctrl->work);

	return 0;
}

/*
 * DRM operations:
 */

static int msm_unload(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	struct msm_gpu *gpu = priv->gpu;
	struct msm_vblank_ctrl *vbl_ctrl = &priv->vblank_ctrl;
	struct vblank_event *vbl_ev, *tmp;

	/* We must cancel and cleanup any pending vblank enable/disable
	 * work before drm_irq_uninstall() to avoid work re-enabling an
	 * irq after uninstall has disabled it.
	 */
	cancel_work_sync(&vbl_ctrl->work);
	list_for_each_entry_safe(vbl_ev, tmp, &vbl_ctrl->event_list, node) {
		list_del(&vbl_ev->node);
		kfree(vbl_ev);
	}

	drm_kms_helper_poll_fini(dev);
	drm_mode_config_cleanup(dev);
	drm_vblank_cleanup(dev);

	pm_runtime_get_sync(dev->dev);
	drm_irq_uninstall(dev);
	pm_runtime_put_sync(dev->dev);

	flush_workqueue(priv->wq);
	destroy_workqueue(priv->wq);

	if (kms) {
		pm_runtime_disable(dev->dev);
		kms->funcs->destroy(kms);
	}

	if (gpu) {
		mutex_lock(&dev->struct_mutex);
		gpu->funcs->pm_suspend(gpu);
		mutex_unlock(&dev->struct_mutex);
		gpu->funcs->destroy(gpu);
	}

	if (priv->vram.paddr) {
		DEFINE_DMA_ATTRS(attrs);
		dma_set_attr(DMA_ATTR_NO_KERNEL_MAPPING, &attrs);
		drm_mm_takedown(&priv->vram.mm);
		dma_free_attrs(dev->dev, priv->vram.size, NULL,
				priv->vram.paddr, &attrs);
	}

	component_unbind_all(dev->dev, dev);

	dev->dev_private = NULL;

	kfree(priv);

	return 0;
}

#define KMS_MDP4 0
#define KMS_MDP5 1
#define KMS_SDE  2

static int get_mdp_ver(struct platform_device *pdev)
{
#ifdef CONFIG_OF
	static const struct of_device_id match_types[] = { {
		.compatible = "qcom,mdss_mdp",
		.data	= (void	*)KMS_MDP5,
	},
	{
		.compatible = "qcom,sde-kms",
		.data	= (void	*)KMS_SDE,
		/* end node */
	} };
	struct device *dev = &pdev->dev;
	const struct of_device_id *match;
	match = of_match_node(match_types, dev->of_node);
	if (match)
		return (int)(unsigned long)match->data;
#endif
	return KMS_MDP4;
}

#include <linux/of_address.h>

static int msm_init_vram(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	unsigned long size = 0;
	int ret = 0;
	dma_addr_t start;

#ifdef CONFIG_OF
	/* In the device-tree world, we could have a 'memory-region'
	 * phandle, which gives us a link to our "vram".  Allocating
	 * is all nicely abstracted behind the dma api, but we need
	 * to know the entire size to allocate it all in one go. There
	 * are two cases:
	 *  1) device with no IOMMU, in which case we need exclusive
	 *     access to a VRAM carveout big enough for all gpu
	 *     buffers
	 *  2) device with IOMMU, but where the bootloader puts up
	 *     a splash screen.  In this case, the VRAM carveout
	 *     need only be large enough for fbdev fb.  But we need
	 *     exclusive access to the buffer to avoid the kernel
	 *     using those pages for other purposes (which appears
	 *     as corruption on screen before we have a chance to
	 *     load and do initial modeset)
	 */
	struct device_node *node;

	node = of_parse_phandle(dev->dev->of_node, "memory-region", 0);
	if (node) {
		struct resource r;
		ret = of_address_to_resource(node, 0, &r);
		if (ret)
			return ret;
		size = r.end - r.start;
		start = (dma_addr_t)r.start;
		DRM_INFO("using VRAM carveout: %lx@%pa\n", size, &r.start);
	} else
#endif

	/* if we have no IOMMU, then we need to use carveout allocator.
	 * Grab the entire CMA chunk carved out in early startup in
	 * mach-msm:
	 */
	if (!iommu_present(&platform_bus_type)) {
		DRM_INFO("using %s VRAM carveout\n", vram);
		size = memparse(vram, NULL);
	}

	if (size) {
		DEFINE_DMA_ATTRS(attrs);
		void *p;

		priv->vram.size = size;

		drm_mm_init(&priv->vram.mm, 0, (size >> PAGE_SHIFT) - 1);

		/*
		 * The carveout physical memory region is defined in
		 * dtsi file. Don't need to allocate from dma.
		 */
		priv->vram.paddr = start;

		dev_info(dev->dev, "VRAM: %08x->%08x\n",
				(uint32_t)priv->vram.paddr,
				(uint32_t)(priv->vram.paddr + size));
	}

	return ret;
}

#ifdef MSM_FORCE_SUBMIT
static inline void msm_force_submit(struct msm_drm_private *priv)
{
	priv->force_submit = true;
}
#else
static inline void msm_force_submit(struct msm_drm_private *priv)
{
}
#endif

static int msm_load(struct drm_device *dev, unsigned long flags)
{
	struct platform_device *pdev = dev->platformdev;
	struct msm_drm_private *priv;
	struct msm_kms *kms;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		dev_err(dev->dev, "failed to allocate private data\n");
		return -ENOMEM;
	}

	dev->dev_private = priv;

	priv->wq = alloc_ordered_workqueue("msm_drm", 0);
	init_waitqueue_head(&priv->fence_event);
	init_waitqueue_head(&priv->pending_crtcs_event);

	INIT_LIST_HEAD(&priv->inactive_list);
	INIT_LIST_HEAD(&priv->fence_cbs);
	INIT_LIST_HEAD(&priv->vblank_ctrl.event_list);
	INIT_WORK(&priv->vblank_ctrl.work, vblank_ctrl_worker);
	spin_lock_init(&priv->vblank_ctrl.lock);

	msm_force_submit(priv);
	drm_mode_config_init(dev);

	platform_set_drvdata(pdev, dev);

	/* Bind all our sub-components: */
	ret = component_bind_all(dev->dev, dev);
	if (ret)
		return ret;

	ret = msm_init_vram(dev);
	if (ret)
		goto fail;

	switch (get_mdp_ver(pdev)) {
	case KMS_MDP4:
		kms = mdp4_kms_init(dev);
		break;
	case KMS_MDP5:
		kms = mdp5_kms_init(dev);
		break;
	case KMS_SDE:
		kms = sde_kms_init(dev);
		break;
	default:
		kms = ERR_PTR(-ENODEV);
		break;
	}

	if (IS_ERR(kms)) {
		/*
		 * NOTE: once we have GPU support, having no kms should not
		 * be considered fatal.. ideally we would still support gpu
		 * and (for example) use dmabuf/prime to share buffers with
		 * imx drm driver on iMX5
		 */
		dev_err(dev->dev, "failed to load kms\n");
		ret = PTR_ERR(kms);
		goto fail;
	}

	priv->kms = kms;

	if (kms) {
		pm_runtime_enable(dev->dev);
		ret = kms->funcs->hw_init(kms);
		if (ret) {
			dev_err(dev->dev, "kms hw init failed: %d\n", ret);
			goto fail;
		}
	}

	dev->mode_config.funcs = &mode_config_funcs;

	ret = drm_vblank_init(dev, priv->num_crtcs);
	if (ret < 0) {
		dev_err(dev->dev, "failed to initialize vblank\n");
		goto fail;
	}

	pm_runtime_get_sync(dev->dev);
	ret = drm_irq_install(dev, platform_get_irq(dev->platformdev, 0));
	pm_runtime_put_sync(dev->dev);
	if (ret < 0) {
		dev_err(dev->dev, "failed to install IRQ handler\n");
		goto fail;
	}

	drm_mode_config_reset(dev);

#ifdef CONFIG_DRM_FBDEV_EMULATION
	if (fbdev)
		priv->fbdev = msm_fbdev_init(dev);
#endif

	ret = msm_debugfs_late_init(dev);
	if (ret)
		goto fail;

	drm_kms_helper_poll_init(dev);

	return 0;

fail:
	msm_unload(dev);
	return ret;
}

#ifdef CONFIG_MSM_KGSL
static void load_gpu(struct drm_device *dev)
{
}
#else
static void load_gpu(struct drm_device *dev)
{
	static DEFINE_MUTEX(init_lock);
	struct msm_drm_private *priv = dev->dev_private;

	mutex_lock(&init_lock);

	if (!priv->gpu)
		priv->gpu = adreno_load_gpu(dev);

	mutex_unlock(&init_lock);
}
#endif

static int msm_open(struct drm_device *dev, struct drm_file *file)
{
	struct msm_file_private *ctx;

	/* For now, load gpu on open.. to avoid the requirement of having
	 * firmware in the initrd.
	 */
	load_gpu(dev);

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	file->driver_priv = ctx;

	if (dev && dev->dev_private) {
		struct msm_drm_private *priv = dev->dev_private;
		struct msm_kms *kms;

		kms = priv->kms;
		if (kms && kms->funcs && kms->funcs->postopen)
			kms->funcs->postopen(kms, file);
	}
	return 0;
}

static void msm_preclose(struct drm_device *dev, struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_file_private *ctx = file->driver_priv;
	struct msm_kms *kms = priv->kms;

	if (kms)
		kms->funcs->preclose(kms, file);

	mutex_lock(&dev->struct_mutex);
	if (ctx == priv->lastctx)
		priv->lastctx = NULL;
	mutex_unlock(&dev->struct_mutex);

	kfree(ctx);
}

static void msm_lastclose(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	if (priv->fbdev)
		drm_fb_helper_restore_fbdev_mode_unlocked(priv->fbdev);
}

static irqreturn_t msm_irq(int irq, void *arg)
{
	struct drm_device *dev = arg;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	BUG_ON(!kms);
	return kms->funcs->irq(kms);
}

static void msm_irq_preinstall(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	BUG_ON(!kms);
	kms->funcs->irq_preinstall(kms);
}

static int msm_irq_postinstall(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	BUG_ON(!kms);
	return kms->funcs->irq_postinstall(kms);
}

static void msm_irq_uninstall(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	BUG_ON(!kms);
	kms->funcs->irq_uninstall(kms);
}

static int msm_enable_vblank(struct drm_device *dev, unsigned int pipe)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	if (!kms)
		return -ENXIO;
	DBG("dev=%pK crtc=%u", dev, pipe);
	return vblank_ctrl_queue_work(priv, pipe, true);
}

static void msm_disable_vblank(struct drm_device *dev, unsigned int pipe)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	if (!kms)
		return;
	DBG("dev=%pK, crtc=%u", dev, pipe);
	vblank_ctrl_queue_work(priv, pipe, false);
}

/*
 * DRM debugfs:
 */

#ifdef CONFIG_DEBUG_FS
static int msm_gpu_show(struct drm_device *dev, struct seq_file *m)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_gpu *gpu = priv->gpu;

	if (gpu) {
		seq_printf(m, "%s Status:\n", gpu->name);
		gpu->funcs->show(gpu, m);
	}

	return 0;
}

static int msm_gem_show(struct drm_device *dev, struct seq_file *m)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_gpu *gpu = priv->gpu;

	if (gpu) {
		seq_printf(m, "Active Objects (%s):\n", gpu->name);
		msm_gem_describe_objects(&gpu->active_list, m);
	}

	seq_printf(m, "Inactive Objects:\n");
	msm_gem_describe_objects(&priv->inactive_list, m);

	return 0;
}

static int msm_mm_show(struct drm_device *dev, struct seq_file *m)
{
	return drm_mm_dump_table(m, &dev->vma_offset_manager->vm_addr_space_mm);
}

static int msm_fb_show(struct drm_device *dev, struct seq_file *m)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_framebuffer *fb, *fbdev_fb = NULL;

	if (priv->fbdev) {
		seq_printf(m, "fbcon ");
		fbdev_fb = priv->fbdev->fb;
		msm_framebuffer_describe(fbdev_fb, m);
	}

	mutex_lock(&dev->mode_config.fb_lock);
	list_for_each_entry(fb, &dev->mode_config.fb_list, head) {
		if (fb == fbdev_fb)
			continue;

		seq_printf(m, "user ");
		msm_framebuffer_describe(fb, m);
	}
	mutex_unlock(&dev->mode_config.fb_lock);

	return 0;
}

static int show_locked(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *) m->private;
	struct drm_device *dev = node->minor->dev;
	int (*show)(struct drm_device *dev, struct seq_file *m) =
			node->info_ent->data;
	int ret;

	ret = mutex_lock_interruptible(&dev->struct_mutex);
	if (ret)
		return ret;

	ret = show(dev, m);

	mutex_unlock(&dev->struct_mutex);

	return ret;
}

static struct drm_info_list msm_debugfs_list[] = {
		{"gpu", show_locked, 0, msm_gpu_show},
		{"gem", show_locked, 0, msm_gem_show},
		{ "mm", show_locked, 0, msm_mm_show },
		{ "fb", show_locked, 0, msm_fb_show },
};

static int late_init_minor(struct drm_minor *minor)
{
	int ret;

	if (!minor)
		return 0;

	ret = msm_rd_debugfs_init(minor);
	if (ret) {
		dev_err(minor->dev->dev, "could not install rd debugfs\n");
		return ret;
	}

	ret = msm_perf_debugfs_init(minor);
	if (ret) {
		dev_err(minor->dev->dev, "could not install perf debugfs\n");
		return ret;
	}

	return 0;
}

int msm_debugfs_late_init(struct drm_device *dev)
{
	int ret;
	ret = late_init_minor(dev->primary);
	if (ret)
		return ret;
	ret = late_init_minor(dev->render);
	if (ret)
		return ret;
	ret = late_init_minor(dev->control);
	return ret;
}

static int msm_debugfs_init(struct drm_minor *minor)
{
	struct drm_device *dev = minor->dev;
	int ret;

	ret = drm_debugfs_create_files(msm_debugfs_list,
			ARRAY_SIZE(msm_debugfs_list),
			minor->debugfs_root, minor);

	if (ret) {
		dev_err(dev->dev, "could not install msm_debugfs_list\n");
		return ret;
	}

	return 0;
}

static void msm_debugfs_cleanup(struct drm_minor *minor)
{
	drm_debugfs_remove_files(msm_debugfs_list,
			ARRAY_SIZE(msm_debugfs_list), minor);
	if (!minor->dev->dev_private)
		return;
	msm_rd_debugfs_cleanup(minor);
	msm_perf_debugfs_cleanup(minor);
}
#endif

/*
 * Fences:
 */

int msm_wait_fence(struct drm_device *dev, uint32_t fence,
		ktime_t *timeout , bool interruptible)
{
	struct msm_drm_private *priv = dev->dev_private;
	int ret;

	if (!priv->gpu)
		return 0;

	if (fence > priv->gpu->submitted_fence) {
		DRM_ERROR("waiting on invalid fence: %u (of %u)\n",
				fence, priv->gpu->submitted_fence);
		return -EINVAL;
	}

	if (!timeout) {
		/* no-wait: */
		ret = fence_completed(dev, fence) ? 0 : -EBUSY;
	} else {
		ktime_t now = ktime_get();
		unsigned long remaining_jiffies;

		if (ktime_compare(*timeout, now) < 0) {
			remaining_jiffies = 0;
		} else {
			ktime_t rem = ktime_sub(*timeout, now);
			struct timespec ts = ktime_to_timespec(rem);
			remaining_jiffies = timespec_to_jiffies(&ts);
		}

		if (interruptible)
			ret = wait_event_interruptible_timeout(priv->fence_event,
				fence_completed(dev, fence),
				remaining_jiffies);
		else
			ret = wait_event_timeout(priv->fence_event,
				fence_completed(dev, fence),
				remaining_jiffies);

		if (ret == 0) {
			DBG("timeout waiting for fence: %u (completed: %u)",
					fence, priv->completed_fence);
			ret = -ETIMEDOUT;
		} else if (ret != -ERESTARTSYS) {
			ret = 0;
		}
	}

	return ret;
}

int msm_queue_fence_cb(struct drm_device *dev,
		struct msm_fence_cb *cb, uint32_t fence)
{
	struct msm_drm_private *priv = dev->dev_private;
	int ret = 0;

	mutex_lock(&dev->struct_mutex);
	if (!list_empty(&cb->work.entry)) {
		ret = -EINVAL;
	} else if (fence > priv->completed_fence) {
		cb->fence = fence;
		list_add_tail(&cb->work.entry, &priv->fence_cbs);
	} else {
		queue_work(priv->wq, &cb->work);
	}
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

/* called from workqueue */
void msm_update_fence(struct drm_device *dev, uint32_t fence)
{
	struct msm_drm_private *priv = dev->dev_private;

	mutex_lock(&dev->struct_mutex);
	priv->completed_fence = max(fence, priv->completed_fence);

	while (!list_empty(&priv->fence_cbs)) {
		struct msm_fence_cb *cb;

		cb = list_first_entry(&priv->fence_cbs,
				struct msm_fence_cb, work.entry);

		if (cb->fence > priv->completed_fence)
			break;

		list_del_init(&cb->work.entry);
		queue_work(priv->wq, &cb->work);
	}

	mutex_unlock(&dev->struct_mutex);

	wake_up_all(&priv->fence_event);
}

void __msm_fence_worker(struct work_struct *work)
{
	struct msm_fence_cb *cb = container_of(work, struct msm_fence_cb, work);
	cb->func(cb);
}

/*
 * DRM ioctls:
 */

static int msm_ioctl_get_param(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_msm_param *args = data;
	struct msm_gpu *gpu;

	/* for now, we just have 3d pipe.. eventually this would need to
	 * be more clever to dispatch to appropriate gpu module:
	 */
	if (args->pipe != MSM_PIPE_3D0)
		return -EINVAL;

	gpu = priv->gpu;

	if (!gpu)
		return -ENXIO;

	return gpu->funcs->get_param(gpu, args->param, &args->value);
}

static int msm_ioctl_gem_new(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_new *args = data;

	if (args->flags & ~MSM_BO_FLAGS) {
		DRM_ERROR("invalid flags: %08x\n", args->flags);
		return -EINVAL;
	}

	return msm_gem_new_handle(dev, file, args->size,
			args->flags, &args->handle);
}

static inline ktime_t to_ktime(struct drm_msm_timespec timeout)
{
	return ktime_set(timeout.tv_sec, timeout.tv_nsec);
}

static int msm_ioctl_gem_cpu_prep(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_cpu_prep *args = data;
	struct drm_gem_object *obj;
	ktime_t timeout = to_ktime(args->timeout);
	int ret;

	if (args->op & ~MSM_PREP_FLAGS) {
		DRM_ERROR("invalid op: %08x\n", args->op);
		return -EINVAL;
	}

	obj = drm_gem_object_lookup(dev, file, args->handle);
	if (!obj)
		return -ENOENT;

	ret = msm_gem_cpu_prep(obj, args->op, &timeout);

	drm_gem_object_unreference_unlocked(obj);

	return ret;
}

static int msm_ioctl_gem_cpu_fini(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_cpu_fini *args = data;
	struct drm_gem_object *obj;
	int ret;

	obj = drm_gem_object_lookup(dev, file, args->handle);
	if (!obj)
		return -ENOENT;

	ret = msm_gem_cpu_fini(obj);

	drm_gem_object_unreference_unlocked(obj);

	return ret;
}

static int msm_ioctl_gem_info(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_info *args = data;
	struct drm_gem_object *obj;
	int ret = 0;

	obj = drm_gem_object_lookup(dev, file, args->handle);
	if (!obj)
		return -ENOENT;

	if (args->hint == 1) {
		uint32_t iova;

		/* iova is 32 bits for now, later could be 64 bits. */
		msm_gem_get_iova_in_domain(obj, MSM_SMMU_DOMAIN_GPU, &iova);
		args->offset = iova;
	} else {
		args->offset = msm_gem_mmap_offset(obj);
	}

	drm_gem_object_unreference_unlocked(obj);

	return ret;
}

static int msm_ioctl_wait_fence(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_wait_fence *args = data;
	ktime_t timeout = to_ktime(args->timeout);

	if (args->pad) {
		DRM_ERROR("invalid pad: %08x\n", args->pad);
		return -EINVAL;
	}

	return msm_wait_fence(dev, args->fence, &timeout, true);
}

static int msm_ioctl_get_last_fence(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_msm_get_last_fence *args = data;
	struct msm_gpu *gpu = priv->gpu;

	if (!gpu)
		return -ENXIO;

	if (args->pad) {
		DRM_ERROR("invalid pad: %08x\n", args->pad);
		return -EINVAL;
	}
	if (gpu->funcs->last_fence)
		args->fence = gpu->funcs->last_fence(gpu);
	else
		DRM_ERROR("gpu->funcs->last_fence is NULL\n");

	return 0;
}

static const struct drm_ioctl_desc msm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(MSM_GET_PARAM,    msm_ioctl_get_param,    DRM_AUTH|DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_NEW,      msm_ioctl_gem_new,      DRM_AUTH|DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_INFO,     msm_ioctl_gem_info,     DRM_AUTH|DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_CPU_PREP, msm_ioctl_gem_cpu_prep, DRM_AUTH|DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_CPU_FINI, msm_ioctl_gem_cpu_fini, DRM_AUTH|DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_SUBMIT,   msm_ioctl_gem_submit,   DRM_AUTH|DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_WAIT_FENCE,   msm_ioctl_wait_fence,   DRM_AUTH|DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GET_LAST_FENCE,	 msm_ioctl_get_last_fence,
				DRM_UNLOCKED|DRM_AUTH|DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(SDE_WB_CONFIG, sde_wb_config, DRM_UNLOCKED|DRM_AUTH),
};

static const struct vm_operations_struct vm_ops = {
	.fault = msm_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static const struct file_operations fops = {
	.owner              = THIS_MODULE,
	.open               = drm_open,
	.release            = drm_release,
	.unlocked_ioctl     = drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl       = drm_compat_ioctl,
#endif
	.poll               = drm_poll,
	.read               = drm_read,
	.llseek             = no_llseek,
	.mmap               = msm_gem_mmap,
};

static struct drm_driver msm_driver = {
	.driver_features    = DRIVER_HAVE_IRQ |
				DRIVER_GEM |
				DRIVER_PRIME |
				DRIVER_RENDER |
				DRIVER_ATOMIC |
				DRIVER_MODESET,
	.load               = msm_load,
	.unload             = msm_unload,
	.open               = msm_open,
	.preclose           = msm_preclose,
	.lastclose          = msm_lastclose,
	.set_busid          = drm_platform_set_busid,
	.irq_handler        = msm_irq,
	.irq_preinstall     = msm_irq_preinstall,
	.irq_postinstall    = msm_irq_postinstall,
	.irq_uninstall      = msm_irq_uninstall,
	.get_vblank_counter = drm_vblank_no_hw_counter,
	.enable_vblank      = msm_enable_vblank,
	.disable_vblank     = msm_disable_vblank,
	.gem_free_object    = msm_gem_free_object,
	.gem_vm_ops         = &vm_ops,
	.dumb_create        = msm_gem_dumb_create,
	.dumb_map_offset    = msm_gem_dumb_map_offset,
	.dumb_destroy       = drm_gem_dumb_destroy,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_export   = drm_gem_prime_export,
	.gem_prime_import   = drm_gem_prime_import,
	.gem_prime_pin      = msm_gem_prime_pin,
	.gem_prime_unpin    = msm_gem_prime_unpin,
	.gem_prime_get_sg_table = msm_gem_prime_get_sg_table,
	.gem_prime_import_sg_table = msm_gem_prime_import_sg_table,
	.gem_prime_vmap     = msm_gem_prime_vmap,
	.gem_prime_vunmap   = msm_gem_prime_vunmap,
	.gem_prime_mmap     = msm_gem_prime_mmap,
#ifdef CONFIG_DEBUG_FS
	.debugfs_init       = msm_debugfs_init,
	.debugfs_cleanup    = msm_debugfs_cleanup,
#endif
	.ioctls             = msm_ioctls,
	.num_ioctls         = DRM_MSM_NUM_IOCTLS,
	.fops               = &fops,
	.name               = "msm_drm",
	.desc               = "MSM Snapdragon DRM",
	.date               = "20130625",
	.major              = 1,
	.minor              = 0,
};

#ifdef CONFIG_PM_SLEEP
static int msm_pm_suspend(struct device *dev)
{
	struct drm_device *ddev = dev_get_drvdata(dev);

	drm_kms_helper_poll_disable(ddev);

	return 0;
}

static int msm_pm_resume(struct device *dev)
{
	struct drm_device *ddev = dev_get_drvdata(dev);

	drm_kms_helper_poll_enable(ddev);

	return 0;
}
#endif

static const struct dev_pm_ops msm_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(msm_pm_suspend, msm_pm_resume)
};

/*
 * Componentized driver support:
 */

#ifdef CONFIG_OF
/* NOTE: the CONFIG_OF case duplicates the same code as exynos or imx
 * (or probably any other).. so probably some room for some helpers
 */
static int compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static int add_components(struct device *dev, struct component_match **matchptr,
		const char *name)
{
	struct device_node *np = dev->of_node;
	unsigned i;

	for (i = 0; ; i++) {
		struct device_node *node;

		node = of_parse_phandle(np, name, i);
		if (!node)
			break;

		component_match_add(dev, matchptr, compare_of, node);
	}

	return 0;
}
#else
static int compare_dev(struct device *dev, void *data)
{
	return dev == data;
}
#endif

static int msm_drm_bind(struct device *dev)
{
	return drm_platform_init(&msm_driver, to_platform_device(dev));
}

static void msm_drm_unbind(struct device *dev)
{
	drm_put_dev(platform_get_drvdata(to_platform_device(dev)));
}

static const struct component_master_ops msm_drm_ops = {
	.bind = msm_drm_bind,
	.unbind = msm_drm_unbind,
};

/*
 * Platform driver:
 */

static int msm_pdev_probe(struct platform_device *pdev)
{
	struct component_match *match = NULL;
#ifdef CONFIG_OF
	add_components(&pdev->dev, &match, "connectors");
	add_components(&pdev->dev, &match, "gpus");
#else
	/* For non-DT case, it kinda sucks.  We don't actually have a way
	 * to know whether or not we are waiting for certain devices (or if
	 * they are simply not present).  But for non-DT we only need to
	 * care about apq8064/apq8060/etc (all mdp4/a3xx):
	 */
	static const char *devnames[] = {
			"hdmi_msm.0", "kgsl-3d0.0",
	};
	int i;

	DBG("Adding components..");

	for (i = 0; i < ARRAY_SIZE(devnames); i++) {
		struct device *dev;

		dev = bus_find_device_by_name(&platform_bus_type,
				NULL, devnames[i]);
		if (!dev) {
			dev_info(&pdev->dev, "still waiting for %s\n", devnames[i]);
			return -EPROBE_DEFER;
		}

		component_match_add(&pdev->dev, &match, compare_dev, dev);
	}
#endif

	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	return component_master_add_with_match(&pdev->dev, &msm_drm_ops, match);
}

static int msm_pdev_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &msm_drm_ops);

	return 0;
}

static const struct platform_device_id msm_id[] = {
	{ "mdp", 0 },
	{ }
};

/*
 * Since "qcom,mdss_mdp" is still being used by fb driver,
 * we can't enable it for kms driver
 */
static const struct of_device_id dt_match[] = {
	{ .compatible = "qcom,mdp" },      /* mdp4 */
	{ .compatible = "qcom,sde-kms" },  /* sde  */
	{}
};
MODULE_DEVICE_TABLE(of, dt_match);

static struct platform_driver msm_platform_driver = {
	.probe      = msm_pdev_probe,
	.remove     = msm_pdev_remove,
	.driver     = {
		.name   = "msm_drm",
		.of_match_table = dt_match,
		.pm     = &msm_pm_ops,
	},
	.id_table   = msm_id,
};

#ifdef CONFIG_MSM_KGSL
void __init adreno_register(void)
{
}

void __exit adreno_unregister(void)
{
}
#endif

static int __init msm_drm_register(void)
{
	DBG("init");
	display_manager_register();
	msm_dsi_register();
	msm_edp_register();
	hdmi_register();
	adreno_register();
	return platform_driver_register(&msm_platform_driver);
}

static void __exit msm_drm_unregister(void)
{
	DBG("fini");
	platform_driver_unregister(&msm_platform_driver);
	hdmi_unregister();
	adreno_unregister();
	msm_edp_unregister();
	msm_dsi_unregister();
	display_manager_unregister();
}

module_init(msm_drm_register);
module_exit(msm_drm_unregister);

MODULE_AUTHOR("Rob Clark <robdclark@gmail.com");
MODULE_DESCRIPTION("MSM DRM Driver");
MODULE_LICENSE("GPL");
