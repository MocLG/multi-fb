// SPDX-License-Identifier: MIT

#include <linux/fb.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <drm/drm_client.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>

#include "drm_crtc_internal.h"

#include "drm_client_internal.h"

/*
 * fbdev-multi: one independent fb_info per driven CRTC, instead of one
 * fb_info spanning the bounding box of every connector (what the stock
 * "fbdev" client's drm_fb_helper_single_fb_probe() builds). That single
 * shared canvas is why fbcon cannot rotate one output independently of
 * another (see the sw_rotations fallback in drm_setup_crtcs_fb()) and why
 * con2fbmap(8) cannot bind different VTs to genuinely different buffers.
 *
 * Resource solving (which CRTC drives which connector) still has to happen
 * exactly once for the whole device: drm_client_modeset_commit() commits
 * every CRTC in a client's array atomically, so two independently-committing
 * clients would each blank the CRTCs the other one owns. That solve is
 * therefore owned by one "primary" drm_fb_helper/drm_client_dev, same as
 * the stock client. Every other driven CRTC gets its own "sibling"
 * drm_fb_helper/drm_client_dev, which never calls drm_client_modeset_probe()
 * or drm_client_modeset_commit() on its own client -- it only borrows a
 * stable pointer into the primary's already-solved client->modesets[]
 * array to learn its target mode and to publish its own fb into it before
 * the primary commits.
 *
 * Because the commit is all-or-nothing, the primary's modesets[] array has
 * to be fully consistent before anything commits it:
 *
 * - Every driven entry needs a struct drm_mode_set.fb. An entry that has a
 *   mode and connectors but no fb fails the "CRTC set but no FB" test in
 *   drm_atomic_plane_check() and takes the whole device down with it, not
 *   just its own CRTC. drm_client_modeset_probe() clears .fb on every
 *   entry (see drm_client_modeset_release()), so the fbs have to be
 *   republished after every probe, and a driven CRTC that no helper owns
 *   has to be released rather than left half-solved.
 * - Every driven entry needs .x/.y of zero. drm_client_modeset_probe()
 *   lays the outputs out inside the one bounding-box buffer the stock
 *   client allocates; those offsets become the plane's source coordinates
 *   in __drm_atomic_helper_set_config(), and a nonzero one runs off the
 *   end of a buffer that is only as large as its own CRTC.
 *
 * That is what drm_fbdev_multi_publish_fbs() maintains, and why no fb_info
 * is registered with the fbdev core before the first publish has run: the
 * primary's register_framebuffer() hands the device to fbcon, which
 * immediately calls fb_set_par and commits.
 *
 * struct drm_fb_helper's exported contract (->fb, ->info, ->buffer,
 * ->fbdefio are singular, and info->par resolves fbops callbacks straight
 * back to exactly one fb_helper) is relied on by every other fbdev_probe
 * driver (i915, msm, radeon, ...), so it is not touched here. Each CRTC
 * gets a genuinely separate struct drm_fb_helper instead.
 *
 * v1 scope: siblings are provisioned once, at setup, for connectors that
 * already have a solved mode (i.e. connected). A monitor plugged into a
 * connector that had no mode at setup time will not get its own fb device
 * until the next probe (module reload/reboot) -- creating a new client at
 * runtime is not possible from within a client's own ->hotplug callback,
 * since drm_client_register() holds dev->clientlist_mutex across the
 * synchronous initial ->hotplug() call, and the same mutex is held across
 * the whole iteration by the runtime hotplug/suspend/resume dispatchers in
 * drm_client_event.c. Such a CRTC is left disabled by
 * drm_fbdev_multi_publish_fbs() rather than being allowed to break the
 * commit for the outputs that do have an fb device.
 *
 * A sibling also keeps the struct drm_mode_set it was given at setup for
 * its whole life. If drm_client_modeset_probe() ever remaps which
 * connector drives which CRTC, a sibling's fb stays installed in an entry
 * that now describes a different output; when the new mode no longer fits
 * that fb the entry is released and the CRTC goes dark instead.
 */

struct drm_fbdev_multi;

struct drm_fbdev_multi_helper {
	struct drm_fb_helper fb_helper;
	struct drm_fbdev_multi *multi;
	/* Target CRTC entry, borrowed from multi->primary.fb_helper.client.
	 * Stable for the client's lifetime once assigned; the modeset_mutex
	 * of that client protects its mode/connectors contents (not this
	 * pointer itself).
	 */
	struct drm_mode_set *modeset;
	/* Linked into multi->siblings. Unused by the primary itself. */
	struct list_head sibling_node;
	/*
	 * Set once register_framebuffer() has succeeded, i.e. once the fbdev
	 * core owns a reference to fb_helper.info and will run the driver's
	 * fb_destroy on the last put. Until then ->unregister has to release
	 * the client itself.
	 */
	bool registered;
	/*
	 * Sibling-only: a per-instance copy of the driver's fb_ops with
	 * fb_set_par/fb_pan_display/fb_blank/fb_destroy replaced.
	 * dev->driver->fbdev_probe() points info->fbops at a *driver-static*
	 * struct fb_ops whose default fb_set_par/fb_pan_display/fb_blank
	 * (DRM_FB_HELPER_DEFAULT_OPS) all operate on info->par's own
	 * fb_helper->client. A sibling's own client is never probed (see the
	 * file comment), so its modesets[] is all zeroed crtc/NULL-mode
	 * entries; committing that array -- which is exactly what
	 * fb_set_par/fb_pan_display/fb_blank do on every VT bind, console
	 * repaint pan and screen-blank timeout -- would disable every CRTC on
	 * the device. Unused by the primary, whose own client is the one that
	 * is actually probed.
	 */
	struct fb_ops fbops;
	/* Sibling-only: the driver's own fb_destroy, wrapped by fbops. */
	void (*driver_fb_destroy)(struct fb_info *info);
};

struct drm_fbdev_multi {
	struct drm_fbdev_multi_helper primary;
	/*
	 * drm_fbdev_multi_helper.sibling_node list + siblings_ready lock.
	 * dev->clientlist_mutex does *not* cover every mutator: a sibling's
	 * ->free can run from put_fb_info() at the last userspace close() of
	 * its /dev/fbN, with no relation to that mutex, concurrently with the
	 * primary's ->hotplug walking this same list under clientlist_mutex.
	 *
	 * Lock order where both are held (primary's ->hotplug):
	 * siblings_lock, then primary_client->modeset_mutex (taken inside
	 * drm_fbdev_multi_publish_fbs()). Never the reverse.
	 */
	struct mutex siblings_lock;
	struct list_head siblings; /* drm_fbdev_multi_helper.sibling_node */
	/*
	 * Set once drm_fbdev_multi_client_setup() is done provisioning
	 * siblings. Until then the primary's ->hotplug must not register any
	 * fb_info: registering the primary's hands the device to fbcon, which
	 * commits the primary's modesets[] array before the siblings have had
	 * a chance to publish their fbs into it.
	 */
	bool siblings_ready;
	unsigned int color_mode;
	/*
	 * Released once by the primary and once by every sibling's ->free.
	 * Teardown order across independently-registered clients is not
	 * guaranteed, so the container can only be freed once nothing
	 * references it any more.
	 */
	struct kref refcount;
};

#define to_multi_helper(fbh) container_of(fbh, struct drm_fbdev_multi_helper, fb_helper)

static void drm_fbdev_multi_release(struct kref *ref)
{
	struct drm_fbdev_multi *multi = container_of(ref, struct drm_fbdev_multi, refcount);

	mutex_destroy(&multi->siblings_lock);
	kfree(multi);
}

/*
 * struct fb_info allocation/release
 *
 * drm_fb_helper_alloc_info()/_release_info() do exactly this, but are not
 * exported -- duplicated here rather than exporting internals that no
 * other fbdev_probe driver needs.
 */

static struct fb_info *drm_fbdev_multi_alloc_info(struct drm_fb_helper *fb_helper)
{
	struct device *dev = fb_helper->dev->dev;
	struct fb_info *info;
	int ret;

	info = framebuffer_alloc(0, dev);
	if (!info)
		return ERR_PTR(-ENOMEM);

	/* No drm_leak_fbdev_smem equivalent: fbdev-multi has no legacy users
	 * relying on a leaked physical address.
	 */
	info->flags |= FBINFO_HIDE_SMEM_START;

	ret = fb_alloc_cmap(&info->cmap, 256, 0);
	if (ret) {
		framebuffer_release(info);
		return ERR_PTR(ret);
	}

	fb_helper->info = info;
	info->skip_vt_switch = true;
	info->skip_panic = drm_panic_is_enabled(fb_helper->dev);

	return info;
}

static void drm_fbdev_multi_release_info(struct drm_fb_helper *fb_helper)
{
	struct fb_info *info = fb_helper->info;

	if (!info)
		return;

	fb_helper->info = NULL;

	if (info->cmap.len)
		fb_dealloc_cmap(&info->cmap);
	framebuffer_release(info);
}

/*
 * struct drm_mode_set lookup/ownership within the primary's client
 */

static struct drm_mode_set *
drm_fbdev_multi_find_modeset(struct drm_client_dev *primary_client,
			     struct drm_connector *connector)
{
	struct drm_mode_set *modeset, *found = NULL;
	unsigned int i;

	mutex_lock(&primary_client->modeset_mutex);
	drm_client_for_each_modeset(modeset, primary_client) {
		for (i = 0; i < modeset->num_connectors; i++) {
			if (modeset->connectors[i] == connector) {
				found = modeset;
				break;
			}
		}
		if (found)
			break;
	}
	mutex_unlock(&primary_client->modeset_mutex);

	return found;
}

static bool drm_fbdev_multi_modeset_claimed(struct drm_fbdev_multi *multi,
					    struct drm_mode_set *modeset)
{
	struct drm_fbdev_multi_helper *sibling;
	bool claimed = false;

	if (multi->primary.modeset == modeset)
		return true;

	mutex_lock(&multi->siblings_lock);
	list_for_each_entry(sibling, &multi->siblings, sibling_node) {
		if (sibling->modeset == modeset) {
			claimed = true;
			break;
		}
	}
	mutex_unlock(&multi->siblings_lock);

	return claimed;
}

/*
 * Publishing fbs into the primary's modesets[] array
 *
 * Everything below runs with the primary client's modeset_mutex held.
 */

static void drm_fbdev_multi_release_modeset(struct drm_device *dev,
					    struct drm_mode_set *modeset)
{
	unsigned int i;

	drm_mode_destroy(dev, modeset->mode);
	modeset->mode = NULL;
	modeset->fb = NULL;

	for (i = 0; i < modeset->num_connectors; i++) {
		drm_connector_put(modeset->connectors[i]);
		modeset->connectors[i] = NULL;
	}
	modeset->num_connectors = 0;

	modeset->x = 0;
	modeset->y = 0;
}

static void drm_fbdev_multi_publish_one(struct drm_fbdev_multi_helper *helper)
{
	struct drm_framebuffer *fb = helper->fb_helper.fb;
	struct drm_mode_set *modeset = helper->modeset;

	/* No fb_info means no live fbdev device backing this helper. */
	if (!modeset || !fb || !helper->fb_helper.info)
		return;

	if (!modeset->mode || !modeset->num_connectors)
		return;

	/*
	 * The solve may have moved this CRTC to a mode that no longer fits
	 * the buffer this helper allocated for it. Leave the entry for the
	 * release pass below rather than feed the commit source coordinates
	 * that run off the end of the fb.
	 */
	if (modeset->mode->hdisplay > fb->width ||
	    modeset->mode->vdisplay > fb->height)
		return;

	modeset->fb = fb;
	modeset->x = 0;
	modeset->y = 0;
}

/*
 * Upper bound for a re-probe: the smallest buffer any helper owns.
 *
 * drm_fb_helper_hotplug_event() constrains its probe to the size of the one
 * buffer the stock client allocated, because that buffer is never resized. The
 * same applies per helper here, but drm_client_modeset_probe() takes a single
 * device-wide limit, so the only bound that holds for every CRTC at once is the
 * minimum. A probe that is free to pick a larger mode leaves that CRTC with a
 * mode its own buffer cannot scan out, and drm_fbdev_multi_publish_fbs() then
 * has to disable it for good, since drm_fbdev_multi_alloc_fb() never
 * reallocates.
 *
 * An asymmetric setup can therefore lose console resolution on the first
 * hotplug. It cannot go dark, which is the trade being made. Zero means
 * nothing is allocated yet and the probe runs unconstrained.
 */
static void drm_fbdev_multi_probe_limit(struct drm_fbdev_multi *multi,
					unsigned int *width, unsigned int *height)
{
	struct drm_fbdev_multi_helper *sibling;
	struct drm_framebuffer *fb;

	*width = 0;
	*height = 0;

	fb = multi->primary.fb_helper.fb;
	if (!fb)
		return;

	*width = fb->width;
	*height = fb->height;

	mutex_lock(&multi->siblings_lock);
	list_for_each_entry(sibling, &multi->siblings, sibling_node) {
		fb = sibling->fb_helper.fb;
		if (!fb)
			continue;

		*width = min(*width, fb->width);
		*height = min(*height, fb->height);
	}
	mutex_unlock(&multi->siblings_lock);
}

/*
 * Republish every helper's fb into the primary's modesets[] array and
 * release the driven entries that no helper owns. Must be called after
 * every drm_client_modeset_probe() on the primary's client, and before
 * anything can commit that client.
 */
static void drm_fbdev_multi_publish_fbs(struct drm_fbdev_multi *multi)
{
	struct drm_client_dev *primary_client = &multi->primary.fb_helper.client;
	struct drm_device *dev = primary_client->dev;
	struct drm_fbdev_multi_helper *sibling;
	struct drm_mode_set *modeset;

	mutex_lock(&multi->siblings_lock);
	mutex_lock(&primary_client->modeset_mutex);

	drm_fbdev_multi_publish_one(&multi->primary);
	list_for_each_entry(sibling, &multi->siblings, sibling_node)
		drm_fbdev_multi_publish_one(sibling);

	/*
	 * A driven entry with no fb only means nobody can paint it once
	 * drm_fbdev_multi_client_setup() has finished provisioning siblings.
	 * Before that the entries those siblings are about to claim are
	 * fb-less by construction, and releasing one here would drop the
	 * connectors that drm_fbdev_multi_setup_siblings() looks the entry up
	 * by, so the sibling would never be created. Nothing can commit the
	 * array in that window either, because no fb_info is registered until
	 * siblings_ready is set.
	 */
	if (!multi->siblings_ready)
		goto out;

	drm_client_for_each_modeset(modeset, primary_client) {
		if (!modeset->mode || modeset->fb)
			continue;

		drm_dbg_kms(dev,
			    "fbdev-multi: [CRTC:%d:%s] has no fb device, leaving it disabled\n",
			    modeset->crtc->base.id, modeset->crtc->name);
		drm_fbdev_multi_release_modeset(dev, modeset);
	}

out:
	mutex_unlock(&primary_client->modeset_mutex);
	mutex_unlock(&multi->siblings_lock);
}

/*
 * Sibling fb_ops overrides -- see the struct
 * drm_fbdev_multi_helper.fbops comment for why the stock
 * DRM_FB_HELPER_DEFAULT_OPS versions of set_par/pan_display/blank are
 * unsafe here.
 */

static int drm_fbdev_multi_sibling_set_par(struct fb_info *info)
{
	/*
	 * Deliberately does nothing. Two things were tried and both proved
	 * unsafe on real hardware (amdgpu/DCN) with a live compositor
	 * holding the device:
	 *
	 * 1. Honoring FB_ACTIVATE_KD_TEXT (what stock drm_fb_helper_set_par()
	 *    does, for a legacy Xorg VT-switch sequencing workaround) forces
	 *    drm_client_modeset_commit_locked(), which skips
	 *    drm_master_internal_acquire() entirely and races the commit
	 *    against the live master's own commits outright.
	 *
	 * 2. Going through the "safe", master-checked
	 *    drm_client_modeset_commit() (via
	 *    drm_fb_helper_restore_fbdev_mode_unlocked(..., false)) is *not*
	 *    actually safe either: drm_master_internal_acquire() only guards
	 *    against a *new* client racing to open the device, not against
	 *    the *existing* master's own concurrent commits. With sway
	 *    continuously committing to both CRTCs (wallpaper, bar), this
	 *    still wedged two kworkers forever in
	 *    amdgpu_dm_atomic_commit_tail() -> drm_atomic_helper_wait_for_flip_done()
	 *    during testing -- an unkillable, uninterruptible hang requiring
	 *    a reboot.
	 *
	 * There is no reachable "safe" commit here as long as any DRM client
	 * has the device open. A sibling's content is only ever painted via
	 * drm_fbdev_multi_primary_restore(), which fires from
	 * drm_client_dev_restore() at drm_lastclose() -- i.e. only once
	 * nothing (no compositor) has the device open at all. Switching to
	 * a sibling's VT while a compositor is running will not update its
	 * display; it also won't hang anything, which is the point.
	 */
	return 0;
}

static int drm_fbdev_multi_sibling_pan_display(struct fb_var_screeninfo *var,
					       struct fb_info *info)
{
	/*
	 * No virtual scroll area is allocated (surface_{width,height} ==
	 * fb_{width,height} in drm_fbdev_multi_alloc_fb()), so any actual
	 * pan request is out of bounds. A real implementation would need to
	 * move only this sibling's own modeset, not every modeset in the
	 * primary's client like drm_fb_helper_pan_display() does.
	 */
	if (var->xoffset || var->yoffset)
		return -EINVAL;

	return 0;
}

static void drm_fbdev_multi_sibling_destroy(struct fb_info *info)
{
	struct drm_fb_helper *fb_helper = info->par;
	struct drm_fbdev_multi_helper *sibling = to_multi_helper(fb_helper);
	void (*driver_fb_destroy)(struct fb_info *) = sibling->driver_fb_destroy;
	struct drm_fb_helper *dev_fb_helper;
	struct drm_device *dev = fb_helper->dev;

	if (!dev) {
		driver_fb_destroy(info);
		return;
	}

	/*
	 * Every driver fb_destroy ends in drm_fb_helper_fini(), which clears
	 * struct drm_device.fb_helper unconditionally. That pointer belongs
	 * to the primary and outlives this sibling, so save and restore it.
	 * The callback also frees this helper and drops the client's device
	 * reference, hence the local copies and the reference taken here.
	 */
	drm_dev_get(dev);
	dev_fb_helper = dev->fb_helper;

	driver_fb_destroy(info);

	if (dev_fb_helper != fb_helper)
		dev->fb_helper = dev_fb_helper;
	drm_dev_put(dev);
}

/*
 * Per-helper buffer allocation and fb_info registration
 *
 * Together these mirror drm_fb_helper_single_fb_probe() +
 * drm_setup_crtcs_fb(), but scoped to a single struct drm_mode_set instead
 * of merging every modeset in the client into one bounding box. They are
 * split because publishing into the primary's modesets[] array has to
 * happen in between: registering an fb_info hands the device to fbcon,
 * which commits that array right away.
 *
 * Both are a no-op once they have run, so the primary's ->hotplug can call
 * them unconditionally on every hotplug event.
 */
static int drm_fbdev_multi_alloc_fb(struct drm_fbdev_multi_helper *helper)
{
	struct drm_fb_helper *fb_helper = &helper->fb_helper;
	struct drm_client_dev *primary_client = &helper->multi->primary.fb_helper.client;
	struct drm_device *dev = fb_helper->dev;
	struct drm_mode_set *modeset = helper->modeset;
	struct drm_fb_helper_surface_size sizes;
	const struct drm_format_info *format_info;
	struct drm_connector *connector;
	struct fb_info *info;
	unsigned int rotation;
	bool hw_rotates;
	u32 format;
	int ret;

	if (fb_helper->fb)
		return 0;

	if (drm_WARN_ON(dev, !dev->driver->fbdev_probe))
		return -EINVAL;

	mutex_lock(&primary_client->modeset_mutex);

	if (!modeset->mode || !modeset->num_connectors) {
		mutex_unlock(&primary_client->modeset_mutex);
		return -EAGAIN;
	}

	memset(&sizes, 0, sizeof(sizes));
	format = drm_driver_color_mode_format(dev, helper->multi->color_mode);
	format_info = drm_format_info(format);
	sizes.surface_bpp = drm_format_info_bpp(format_info, 0);
	sizes.surface_depth = format_info->depth;
	sizes.surface_width = modeset->mode->hdisplay;
	sizes.surface_height = modeset->mode->vdisplay;
	sizes.fb_width = modeset->mode->hdisplay;
	sizes.fb_height = modeset->mode->vdisplay;

	hw_rotates = drm_client_rotation(modeset, &rotation);
	if (hw_rotates)
		rotation = DRM_MODE_ROTATE_0;

	connector = modeset->connectors[0];

	mutex_unlock(&primary_client->modeset_mutex);

	info = drm_fbdev_multi_alloc_info(fb_helper);
	if (IS_ERR(info))
		return PTR_ERR(info);

	ret = dev->driver->fbdev_probe(fb_helper, &sizes);
	if (ret)
		goto err_release_info;

	if (helper != &helper->multi->primary) {
		/*
		 * Not every fbdev_probe implementation honors the fb_helper it
		 * is handed: i915 fills the fb_info in from struct
		 * drm_device.fb_helper, which is always the primary. Every
		 * fbops callback of this sibling would then resolve to the
		 * primary's helper, and its console writes would land in the
		 * primary's buffer. Give up on the sibling instead.
		 *
		 * Unwinding the driver's probe is not possible here: its
		 * fb_destroy resolves everything it has to free through
		 * info->par, i.e. through somebody else's helper. Drop only
		 * what this client owns and leave the rest; the caller stops
		 * provisioning siblings on -EOPNOTSUPP, so this is bounded to
		 * one buffer on such a driver.
		 */
		if (info->par != fb_helper) {
			drm_warn(dev, "fbdev-multi: %s does not support more than one fb device\n",
				 dev->driver->name);
			drm_fbdev_multi_release_info(fb_helper);
			fb_helper->fb = NULL;
			fb_helper->buffer = NULL;
			return -EOPNOTSUPP;
		}

		helper->fbops = *info->fbops;
		helper->driver_fb_destroy = info->fbops->fb_destroy;
		helper->fbops.fb_set_par = drm_fbdev_multi_sibling_set_par;
		helper->fbops.fb_pan_display = drm_fbdev_multi_sibling_pan_display;
		helper->fbops.fb_blank = NULL;
		if (helper->driver_fb_destroy)
			helper->fbops.fb_destroy = drm_fbdev_multi_sibling_destroy;
		info->fbops = &helper->fbops;
	}

	strscpy(fb_helper->fb->comm, "[fbcon]", sizeof(fb_helper->fb->comm));

	info->var.width = connector->display_info.width_mm;
	info->var.height = connector->display_info.height_mm;
	info->var.pixclock = 0;

	switch (rotation) {
	case DRM_MODE_ROTATE_90:
		info->fbcon_rotate_hint = FB_ROTATE_CCW;
		break;
	case DRM_MODE_ROTATE_180:
		info->fbcon_rotate_hint = FB_ROTATE_UD;
		break;
	case DRM_MODE_ROTATE_270:
		info->fbcon_rotate_hint = FB_ROTATE_CW;
		break;
	default:
		info->fbcon_rotate_hint = FB_ROTATE_UR;
	}

	fb_helper->deferred_setup = false;

	return 0;

err_release_info:
	drm_fbdev_multi_release_info(fb_helper);
	return ret;
}

static int drm_fbdev_multi_register_fb(struct drm_fbdev_multi_helper *helper)
{
	struct drm_fb_helper *fb_helper = &helper->fb_helper;
	struct drm_device *dev = fb_helper->dev;
	int ret;

	if (helper->registered || !fb_helper->info)
		return 0;

	ret = register_framebuffer(fb_helper->info);
	if (ret < 0) {
		/*
		 * Matches __drm_fb_helper_initial_config_and_unlock(): the
		 * backing buffer is not unwound, since fb_helper->fb/buffer
		 * are already live. Dropping the fb_info is enough to keep
		 * ->unregister and drm_fbdev_multi_publish_one() away from a
		 * helper that has no fbdev device; the caller re-publishes so
		 * that this helper's CRTC is released rather than committed.
		 */
		drm_err(dev, "fbdev-multi: failed to register fb device: %d\n", ret);
		drm_fbdev_multi_release_info(fb_helper);
		return ret;
	}

	helper->registered = true;

	drm_info(dev, "fb%d: %s frame buffer device (%s)\n",
		 fb_helper->info->node, fb_helper->info->fix.id,
		 fb_helper->client.name);

	return 0;
}

/*
 * Hand every helper's fb_info to the fbdev core. The primary goes first
 * and outside siblings_lock: its register_framebuffer() is the one that
 * triggers the fbcon take-over, and fbcon calls straight back into
 * fb_set_par and commits the primary's client.
 */
static void drm_fbdev_multi_register_fbs(struct drm_fbdev_multi *multi)
{
	struct drm_fbdev_multi_helper *sibling;
	bool failed = false;

	if (drm_fbdev_multi_register_fb(&multi->primary))
		failed = true;

	mutex_lock(&multi->siblings_lock);
	list_for_each_entry(sibling, &multi->siblings, sibling_node) {
		if (drm_fbdev_multi_register_fb(sibling))
			failed = true;
	}
	mutex_unlock(&multi->siblings_lock);

	/* Release the CRTCs whose fb device just went away. */
	if (failed)
		drm_fbdev_multi_publish_fbs(multi);
}

/*
 * Shared struct drm_client_funcs callbacks (identical for primary and
 * siblings)
 */

static void drm_fbdev_multi_unregister(struct drm_client_dev *client)
{
	struct drm_fbdev_multi_helper *helper = to_multi_helper(drm_fb_helper_from_client(client));

	if (helper->registered) {
		/*
		 * Fully probed framebuffer device: unregister_framebuffer()
		 * ends in the driver's fb_destroy, which releases the client.
		 */
		drm_fb_helper_unregister_info(&helper->fb_helper);
	} else {
		/*
		 * Partially initialized client. The fbdev core never took a
		 * reference on the fb_info, so no fb_destroy will run for it.
		 */
		drm_fbdev_multi_release_info(&helper->fb_helper);
		drm_client_release(client);
	}
}

static int drm_fbdev_multi_suspend(struct drm_client_dev *client)
{
	drm_fb_helper_set_suspend_unlocked(drm_fb_helper_from_client(client), true);
	return 0;
}

static int drm_fbdev_multi_resume(struct drm_client_dev *client)
{
	drm_fb_helper_set_suspend_unlocked(drm_fb_helper_from_client(client), false);
	return 0;
}

/*
 * Primary struct drm_client_funcs callbacks
 */

static int drm_fbdev_multi_primary_hotplug(struct drm_client_dev *client)
{
	struct drm_fbdev_multi_helper *primary = to_multi_helper(drm_fb_helper_from_client(client));
	struct drm_fbdev_multi *multi = primary->multi;
	struct drm_device *dev = client->dev;
	struct drm_fbdev_multi_helper *sibling;
	struct drm_mode_set *modeset;
	unsigned int width, height;
	bool siblings_ready;
	int ret;

	/*
	 * Mirrors drm_fbdev_client_hotplug(): dev->fb_helper is set here,
	 * from inside the client's own ->hotplug, not from the setup
	 * function -- drm_client_register() runs this synchronously while
	 * holding dev->clientlist_mutex, and only completed registration
	 * should make dev->fb_helper visible.
	 */
	if (!dev->fb_helper) {
		ret = drm_fb_helper_init(dev, &primary->fb_helper);
		if (ret)
			return ret;
	}

	/*
	 * Not fatal: a probe that fails part way through has already cleared
	 * every struct drm_mode_set.fb, so the publish pass below still has
	 * to run to keep the array committable.
	 */
	drm_fbdev_multi_probe_limit(multi, &width, &height);
	ret = drm_client_modeset_probe(client, width, height);
	if (ret)
		drm_warn(dev, "fbdev-multi: modeset probe failed: %d\n", ret);

	if (!primary->modeset) {
		mutex_lock(&client->modeset_mutex);
		drm_client_for_each_modeset(modeset, client) {
			if (modeset->mode && modeset->num_connectors) {
				primary->modeset = modeset;
				break;
			}
		}
		mutex_unlock(&client->modeset_mutex);
	}

	if (primary->modeset)
		drm_fbdev_multi_alloc_fb(primary);

	/*
	 * Nothing is driven yet. drm_fbdev_multi_helper_prepare() left the
	 * deferred_setup flag set so that ->restore does not dereference the
	 * struct drm_fb_helper_funcs that dev->driver->fbdev_probe() has not
	 * assigned yet; retry on the next hotplug event.
	 */
	if (!primary->fb_helper.info)
		return 0;

	mutex_lock(&multi->siblings_lock);
	list_for_each_entry(sibling, &multi->siblings, sibling_node)
		drm_fbdev_multi_alloc_fb(sibling);
	siblings_ready = multi->siblings_ready;
	mutex_unlock(&multi->siblings_lock);

	/*
	 * drm_client_modeset_probe() above cleared struct drm_mode_set.fb on
	 * every entry (drm_client_modeset_release()). Without this the next
	 * commit fails the "CRTC set but no FB" test in
	 * drm_atomic_plane_check() and blanks every output on the device.
	 */
	drm_fbdev_multi_publish_fbs(multi);

	if (siblings_ready)
		drm_fbdev_multi_register_fbs(multi);

	return 0;
}

static int drm_fbdev_multi_primary_restore(struct drm_client_dev *client, bool force)
{
	drm_fb_helper_restore_fbdev_mode_unlocked(drm_fb_helper_from_client(client), force);

	/*
	 * Zero regardless of the commit result: drm_client_dev_restore()
	 * hands the device to the next client in the list otherwise, and the
	 * siblings that follow have no business restoring it.
	 */
	return 0;
}

static void drm_fbdev_multi_primary_free(struct drm_client_dev *client)
{
	struct drm_fbdev_multi_helper *primary = to_multi_helper(drm_fb_helper_from_client(client));
	struct drm_fbdev_multi *multi = primary->multi;

	drm_fbdev_multi_release_info(&primary->fb_helper);
	drm_fb_helper_unprepare(&primary->fb_helper);
	kref_put(&multi->refcount, drm_fbdev_multi_release);
}

static const struct drm_client_funcs drm_fbdev_multi_primary_funcs = {
	.owner		= THIS_MODULE,
	.free		= drm_fbdev_multi_primary_free,
	.unregister	= drm_fbdev_multi_unregister,
	.restore	= drm_fbdev_multi_primary_restore,
	.hotplug	= drm_fbdev_multi_primary_hotplug,
	.suspend	= drm_fbdev_multi_suspend,
	.resume		= drm_fbdev_multi_resume,
};

/*
 * Sibling struct drm_client_funcs callbacks
 *
 * Siblings never probe or commit modesets themselves -- they are driven
 * synchronously by the primary's ->hotplug (a plain function call into an
 * already-registered client, never through drm_client_register()/the
 * generic per-client hotplug dispatch, both of which hold
 * dev->clientlist_mutex across the call and would deadlock on re-entry).
 * So siblings have no ->hotplug and no ->restore of their own; the first
 * client in dev->clientlist whose ->restore returns 0 is the only one
 * called (see drm_client_dev_restore()), and that must stay the primary.
 */

static void drm_fbdev_multi_sibling_free(struct drm_client_dev *client)
{
	struct drm_fbdev_multi_helper *sibling = to_multi_helper(drm_fb_helper_from_client(client));
	struct drm_fbdev_multi *multi = sibling->multi;

	/*
	 * dev->clientlist_mutex does not cover every caller of ->free: it
	 * can also run from put_fb_info() at the last userspace close() of
	 * this sibling's /dev/fbN, unrelated to that mutex and concurrent
	 * with the primary's ->hotplug walking multi->siblings. Use our own
	 * lock instead.
	 */
	mutex_lock(&multi->siblings_lock);
	list_del(&sibling->sibling_node);
	mutex_unlock(&multi->siblings_lock);

	drm_fbdev_multi_release_info(&sibling->fb_helper);
	drm_fb_helper_unprepare(&sibling->fb_helper);
	kfree(sibling);
	kref_put(&multi->refcount, drm_fbdev_multi_release);
}

static const struct drm_client_funcs drm_fbdev_multi_sibling_funcs = {
	.owner		= THIS_MODULE,
	.free		= drm_fbdev_multi_sibling_free,
	.unregister	= drm_fbdev_multi_unregister,
	.suspend	= drm_fbdev_multi_suspend,
	.resume		= drm_fbdev_multi_resume,
};

/*
 * Setup
 */

static unsigned int drm_fbdev_multi_color_mode(struct drm_device *dev,
					       const struct drm_format_info *format)
{
	if (format) {
		unsigned int bpp = drm_format_info_bpp(format, 0);

		if (bpp == 16)
			return format->depth; /* could also be 15 */
		return bpp;
	}

	switch (dev->mode_config.preferred_depth) {
	case 0:
	case 24:
		return 32;
	default:
		return dev->mode_config.preferred_depth;
	}
}

static int drm_fbdev_multi_helper_prepare(struct drm_device *dev,
					  struct drm_fbdev_multi *multi,
					  struct drm_fbdev_multi_helper *helper,
					  const struct drm_client_funcs *funcs,
					  const char *name)
{
	int ret;

	helper->multi = multi;
	INIT_LIST_HEAD(&helper->sibling_node);

	drm_fb_helper_prepare(dev, &helper->fb_helper, multi->color_mode, NULL);

	/*
	 * No buffer and no struct drm_fb_helper_funcs until
	 * dev->driver->fbdev_probe() has run. Keep ->restore out of
	 * drm_client_modeset_commit() and out of fb_helper->funcs until then.
	 */
	helper->fb_helper.deferred_setup = true;

	ret = drm_client_init(dev, &helper->fb_helper.client, name, funcs);
	if (ret)
		drm_fb_helper_unprepare(&helper->fb_helper);

	return ret;
}

static void drm_fbdev_multi_setup_siblings(struct drm_device *dev,
					   struct drm_fbdev_multi *multi)
{
	struct drm_client_dev *primary_client = &multi->primary.fb_helper.client;
	struct drm_connector_list_iter conn_iter;
	struct drm_connector *connector;
	int ret;

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_client_for_each_connector_iter(connector, &conn_iter) {
		struct drm_fbdev_multi_helper *sibling;
		struct drm_mode_set *modeset;

		modeset = drm_fbdev_multi_find_modeset(primary_client, connector);
		if (!modeset || drm_fbdev_multi_modeset_claimed(multi, modeset))
			continue;

		sibling = kzalloc_obj(*sibling);
		if (!sibling) {
			drm_warn(dev, "fbdev-multi: out of memory, skipping connector %s\n",
				 connector->name);
			continue;
		}

		ret = drm_fbdev_multi_helper_prepare(dev, multi, sibling,
						     &drm_fbdev_multi_sibling_funcs,
						     "fbdev-multi");
		if (ret) {
			kfree(sibling);
			continue;
		}

		/* Dropped again by drm_fbdev_multi_sibling_free(). */
		kref_get(&multi->refcount);
		sibling->modeset = modeset;

		ret = drm_fbdev_multi_alloc_fb(sibling);
		if (ret) {
			drm_warn(dev, "fbdev-multi: no fb device for connector %s: %d\n",
				 connector->name, ret);
			drm_client_release(&sibling->fb_helper.client);
			if (ret == -EOPNOTSUPP)
				break;
			continue;
		}

		mutex_lock(&multi->siblings_lock);
		list_add_tail(&sibling->sibling_node, &multi->siblings);
		mutex_unlock(&multi->siblings_lock);

		drm_client_register(&sibling->fb_helper.client);
	}
	drm_connector_list_iter_end(&conn_iter);
}

/**
 * drm_fbdev_multi_client_setup() - Setup fbdev emulation with one fb_info
 *                                  per driven CRTC
 * @dev: DRM device
 * @format: Preferred color format for the device. DRM_FORMAT_XRGB8888
 *          is used if this is zero.
 *
 * Alternative to drm_fbdev_client_setup() for drivers/setups where each
 * connected output should get its own fbdev/fbcon device (own /dev/fbN,
 * own con2fbmap(8) target, own independent fbcon rotation) instead of one
 * fb_info spanning every connector's bounding box. Selected via the
 * drm_client_lib.active="fbdev-multi" module parameter, in place of the
 * stock "fbdev" client.
 *
 * See the file-level comment in drm_fbdev_multi_client.c for the resource
 * solving and locking rationale, and for this first version's scope
 * limitations.
 *
 * Returns:
 * 0 on success, or a negative errno code otherwise.
 */
int drm_fbdev_multi_client_setup(struct drm_device *dev, const struct drm_format_info *format)
{
	struct drm_fbdev_multi *multi;
	int ret;

	drm_WARN(dev, !dev->registered, "Device has not been registered.\n");
	drm_WARN(dev, dev->fb_helper, "fb_helper is already set!\n");

	multi = kzalloc_obj(*multi);
	if (!multi)
		return -ENOMEM;

	mutex_init(&multi->siblings_lock);
	INIT_LIST_HEAD(&multi->siblings);
	kref_init(&multi->refcount);
	multi->color_mode = drm_fbdev_multi_color_mode(dev, format);

	ret = drm_fbdev_multi_helper_prepare(dev, multi, &multi->primary,
					     &drm_fbdev_multi_primary_funcs,
					     "fbdev-multi");
	if (ret)
		goto err_free_multi;

	/*
	 * Synchronously runs drm_fbdev_multi_primary_hotplug() while holding
	 * dev->clientlist_mutex (see drm_client_register()): this solves the
	 * whole-device modeset and allocates the primary's buffer. Siblings
	 * must not be registered until this returns and the mutex is
	 * released, or their own drm_client_register() would recurse on the
	 * same non-recursive mutex. Because multi->siblings_ready is still
	 * clear, no fb_info is handed to the fbdev core yet.
	 */
	drm_client_register(&multi->primary.fb_helper.client);

	if (multi->primary.fb_helper.info)
		drm_fbdev_multi_setup_siblings(dev, multi);
	else
		drm_dbg_kms(dev, "fbdev-multi: no driven CRTC at setup\n");

	mutex_lock(&multi->siblings_lock);
	multi->siblings_ready = true;
	mutex_unlock(&multi->siblings_lock);

	/*
	 * Only now that every helper owns a buffer is the primary's
	 * modesets[] array consistent enough to be committed, and only now
	 * may an fb_info reach fbcon.
	 */
	drm_fbdev_multi_publish_fbs(multi);
	drm_fbdev_multi_register_fbs(multi);

	return 0;

err_free_multi:
	mutex_destroy(&multi->siblings_lock);
	kfree(multi);
	return ret;
}
