/*
 * Virtio Balloon Device
 *
 * Copyright IBM, Corp. 2008
 * Copyright (C) 2011 Red Hat, Inc.
 * Copyright (C) 2011 Amit Shah <amit.shah@redhat.com>
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/timer.h"
#include "qemu-common.h"
#include "hw/virtio/virtio.h"
#include "hw/mem/pc-dimm.h"
#include "sysemu/balloon.h"
#include "hw/virtio/virtio-balloon.h"
#include "sysemu/kvm.h"
#include "exec/address-spaces.h"
#include "qapi/error.h"
#include "qapi/qapi-events-misc.h"
#include "qapi/visitor.h"
#include "trace.h"
#include "qemu/error-report.h"

#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"

#define BALLOON_PAGE_SIZE  (1 << VIRTIO_BALLOON_PFN_SHIFT)

static void balloon_page(void *addr, int deflate)
{
    if (!qemu_balloon_is_inhibited() && (!kvm_enabled() ||
                                         kvm_has_sync_mmu())) {
        qemu_madvise(addr, BALLOON_PAGE_SIZE,
                deflate ? QEMU_MADV_WILLNEED : QEMU_MADV_DONTNEED);
    }
}

static const char *balloon_stat_names[] = {
   [VIRTIO_BALLOON_S_SWAP_IN] = "stat-swap-in",
   [VIRTIO_BALLOON_S_SWAP_OUT] = "stat-swap-out",
   [VIRTIO_BALLOON_S_MAJFLT] = "stat-major-faults",
   [VIRTIO_BALLOON_S_MINFLT] = "stat-minor-faults",
   [VIRTIO_BALLOON_S_MEMFREE] = "stat-free-memory",
   [VIRTIO_BALLOON_S_MEMTOT] = "stat-total-memory",
   [VIRTIO_BALLOON_S_AVAIL] = "stat-available-memory",
   [VIRTIO_BALLOON_S_CACHES] = "stat-disk-caches",
   [VIRTIO_BALLOON_S_NR] = NULL
};

/*
 * reset_stats - Mark all items in the stats array as unset
 *
 * This function needs to be called at device initialization and before
 * updating to a set of newly-generated stats.  This will ensure that no
 * stale values stick around in case the guest reports a subset of the supported
 * statistics.
 */
static inline void reset_stats(VirtIOBalloon *dev)
{
    int i;
    for (i = 0; i < VIRTIO_BALLOON_S_NR; dev->stats[i++] = -1);
}

static bool balloon_stats_supported(const VirtIOBalloon *s)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    return virtio_vdev_has_feature(vdev, VIRTIO_BALLOON_F_STATS_VQ);
}

static bool balloon_stats_enabled(const VirtIOBalloon *s)
{
    return s->stats_poll_interval > 0;
}

static void balloon_stats_destroy_timer(VirtIOBalloon *s)
{
    if (balloon_stats_enabled(s)) {
        timer_del(s->stats_timer);
        timer_free(s->stats_timer);
        s->stats_timer = NULL;
        s->stats_poll_interval = 0;
    }
}

static void balloon_stats_change_timer(VirtIOBalloon *s, int64_t secs)
{
    timer_mod(s->stats_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + secs * 1000);
}

static void balloon_stats_poll_cb(void *opaque)
{
    VirtIOBalloon *s = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    if (s->stats_vq_elem == NULL || !balloon_stats_supported(s)) {
        /* re-schedule */
        balloon_stats_change_timer(s, s->stats_poll_interval);
        return;
    }

    virtqueue_push(s->svq, s->stats_vq_elem, s->stats_vq_offset);
    virtio_notify(vdev, s->svq);
    g_free(s->stats_vq_elem);
    s->stats_vq_elem = NULL;
}

static void balloon_stats_get_all(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    Error *err = NULL;
    VirtIOBalloon *s = opaque;
    int i;

    visit_start_struct(v, name, NULL, 0, &err);
    if (err) {
        goto out;
    }
    visit_type_int(v, "last-update", &s->stats_last_update, &err);
    if (err) {
        goto out_end;
    }

    visit_start_struct(v, "stats", NULL, 0, &err);
    if (err) {
        goto out_end;
    }
    for (i = 0; i < VIRTIO_BALLOON_S_NR; i++) {
        visit_type_uint64(v, balloon_stat_names[i], &s->stats[i], &err);
        if (err) {
            goto out_nested;
        }
    }
    visit_check_struct(v, &err);
out_nested:
    visit_end_struct(v, NULL);

    if (!err) {
        visit_check_struct(v, &err);
    }
out_end:
    visit_end_struct(v, NULL);
out:
    error_propagate(errp, err);
}

static void balloon_stats_get_poll_interval(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    VirtIOBalloon *s = opaque;
    visit_type_int(v, name, &s->stats_poll_interval, errp);
}

static void balloon_stats_set_poll_interval(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    VirtIOBalloon *s = opaque;
    Error *local_err = NULL;
    int64_t value;

    visit_type_int(v, name, &value, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    if (value < 0) {
        error_setg(errp, "timer value must be greater than zero");
        return;
    }

    if (value > UINT32_MAX) {
        error_setg(errp, "timer value is too big");
        return;
    }

    if (value == s->stats_poll_interval) {
        return;
    }

    if (value == 0) {
        /* timer=0 disables the timer */
        balloon_stats_destroy_timer(s);
        return;
    }

    if (balloon_stats_enabled(s)) {
        /* timer interval change */
        s->stats_poll_interval = value;
        balloon_stats_change_timer(s, value);
        return;
    }

    /* create a new timer */
    g_assert(s->stats_timer == NULL);
    s->stats_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, balloon_stats_poll_cb, s);
    s->stats_poll_interval = value;
    balloon_stats_change_timer(s, 0);
}

/*
 * Free page reporting (VIRTIO_BALLOON_F_REPORTING), ported from upstream
 * QEMU 5.1 (91b867191ddf "virtio-balloon: Provide an interface for free
 * page reporting" and 7483cbbaf820 "virtio-balloon: Implement support for
 * page poison reporting feature").
 *
 * The guest hands us scatter lists of free buddy blocks (2^page_reporting_order
 * pages each) that it keeps isolated until we return the element. We discard
 * the host backing of each block; the guest then puts the pages back on its
 * free lists and may reuse them at any time.
 */
static void virtio_balloon_handle_report(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(vdev);
    VirtQueueElement *elem;

    while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
        unsigned int i;

        /*
         * When we discard the page it has the effect of removing the page
         * from the hypervisor itself and causing it to be zeroed when it
         * is returned to us. So we must not discard the page if it is
         * accessible by another device or process, or if the guest is
         * expecting it to retain a non-zero value.
         *
         * On Windows a discarded page comes back either with its old
         * contents or zero-filled, so the poison_val == 0 (init_on_free)
         * case is still safe there: both outcomes read as zero.
         */
        if (qemu_balloon_is_inhibited() || dev->poison_val) {
            goto skip_element;
        }

        for (i = 0; i < elem->in_num; i++) {
            void *addr = elem->in_sg[i].iov_base;
            size_t size = elem->in_sg[i].iov_len;
            ram_addr_t ram_offset;
            RAMBlock *rb;

            /*
             * There is no need to check the memory section to see if
             * it is ram/readonly/romd like there is for handle_output
             * below. If the region is not meant to be written to then
             * address_space_map will have allocated a bounce buffer
             * and it will be freed in address_space_unmap and trigger
             * and unassigned_mem_write before failing to copy over the
             * buffer. If more than one bad descriptor is provided it
             * will return NULL after the first bounce buffer and fail
             * to map any resources.
             */
            rb = qemu_ram_block_from_host(addr, false, &ram_offset);
            if (!rb) {
                trace_virtio_balloon_bad_addr(elem->in_addr[i]);
                continue;
            }

            /*
             * For now we will simply ignore unaligned memory regions, or
             * regions that overrun the end of the RAMBlock.
             */
            if (!QEMU_IS_ALIGNED(ram_offset | size, qemu_ram_pagesize(rb)) ||
                (ram_offset + size) > qemu_ram_get_used_length(rb)) {
                continue;
            }

            if (ram_block_discard_range(rb, ram_offset, size) == 0) {
                dev->reported_bytes += size;
            }
        }

        /* One line per GiB discarded, to line up with host memory graphs. */
        if (dev->reported_bytes - dev->reported_bytes_logged >= (1ULL << 30)) {
            dev->reported_bytes_logged = dev->reported_bytes;
            info_report("virtio-balloon: free page reporting has discarded "
                        "%" PRIu64 " MiB in total",
                        dev->reported_bytes >> 20);
        }

skip_element:
        virtqueue_push(vq, elem, 0);
        virtio_notify(vdev, vq);
        g_free(elem);
    }
}

static void virtio_balloon_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(vdev);
    VirtQueueElement *elem;
    MemoryRegionSection section;

    for (;;) {
        size_t offset = 0;
        uint32_t pfn;
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            return;
        }

        while (iov_to_buf(elem->out_sg, elem->out_num, offset, &pfn, 4) == 4) {
            ram_addr_t pa;
            ram_addr_t addr;
            int p = virtio_ldl_p(vdev, &pfn);

            pa = (ram_addr_t) p << VIRTIO_BALLOON_PFN_SHIFT;
            offset += 4;

            /* FIXME: remove get_system_memory(), but how? */
            section = memory_region_find(get_system_memory(), pa, 1);
            if (!int128_nz(section.size) ||
                !memory_region_is_ram(section.mr) ||
                memory_region_is_rom(section.mr) ||
                memory_region_is_romd(section.mr)) {
                trace_virtio_balloon_bad_addr(pa);
                memory_region_unref(section.mr);
                continue;
            }

            trace_virtio_balloon_handle_output(memory_region_name(section.mr),
                                               pa);
            /* Using memory_region_get_ram_ptr is bending the rules a bit, but
               should be OK because we only want a single page.  */
            addr = section.offset_within_region;
            balloon_page(memory_region_get_ram_ptr(section.mr) + addr,
                         !!(vq == s->dvq));
            memory_region_unref(section.mr);
        }

        virtqueue_push(vq, elem, offset);
        virtio_notify(vdev, vq);
        g_free(elem);
    }
}

static void virtio_balloon_receive_stats(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(vdev);
    VirtQueueElement *elem;
    VirtIOBalloonStat stat;
    size_t offset = 0;
    qemu_timeval tv;

    elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
    if (!elem) {
        goto out;
    }

    if (s->stats_vq_elem != NULL) {
        /* This should never happen if the driver follows the spec. */
        virtqueue_push(vq, s->stats_vq_elem, 0);
        virtio_notify(vdev, vq);
        g_free(s->stats_vq_elem);
    }

    s->stats_vq_elem = elem;

    /* Initialize the stats to get rid of any stale values.  This is only
     * needed to handle the case where a guest supports fewer stats than it
     * used to (ie. it has booted into an old kernel).
     */
    reset_stats(s);

    while (iov_to_buf(elem->out_sg, elem->out_num, offset, &stat, sizeof(stat))
           == sizeof(stat)) {
        uint16_t tag = virtio_tswap16(vdev, stat.tag);
        uint64_t val = virtio_tswap64(vdev, stat.val);

        offset += sizeof(stat);
        if (tag < VIRTIO_BALLOON_S_NR)
            s->stats[tag] = val;
    }
    s->stats_vq_offset = offset;

    if (qemu_gettimeofday(&tv) < 0) {
        warn_report("%s: failed to get time of day", __func__);
        goto out;
    }

    s->stats_last_update = tv.tv_sec;

out:
    if (balloon_stats_enabled(s)) {
        balloon_stats_change_timer(s, s->stats_poll_interval);
    }
}

/*
 * Without page-poison the config space keeps its old 8-byte size, so a
 * device created without the new properties looks exactly as before.
 */
static size_t virtio_balloon_config_size(VirtIOBalloon *s)
{
    uint64_t features = s->host_features;

    if (virtio_has_feature(features, VIRTIO_BALLOON_F_PAGE_POISON)) {
        return sizeof(struct virtio_balloon_config);
    }
    return offsetof(struct virtio_balloon_config, free_page_hint_cmd_id);
}

static void virtio_balloon_get_config(VirtIODevice *vdev, uint8_t *config_data)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(vdev);
    struct virtio_balloon_config config = {};

    config.num_pages = cpu_to_le32(dev->num_pages);
    config.actual = cpu_to_le32(dev->actual);
    config.poison_val = cpu_to_le32(dev->poison_val);

    trace_virtio_balloon_get_config(config.num_pages, config.actual);
    memcpy(config_data, &config, virtio_balloon_config_size(dev));
}

static int build_dimm_list(Object *obj, void *opaque)
{
    GSList **list = opaque;

    if (object_dynamic_cast(obj, TYPE_PC_DIMM)) {
        DeviceState *dev = DEVICE(obj);
        if (dev->realized) { /* only realized DIMMs matter */
            *list = g_slist_prepend(*list, dev);
        }
    }

    object_child_foreach(obj, build_dimm_list, opaque);
    return 0;
}

static ram_addr_t get_current_ram_size(void)
{
    GSList *list = NULL, *item;
    ram_addr_t size = ram_size;

    build_dimm_list(qdev_get_machine(), &list);
    for (item = list; item; item = g_slist_next(item)) {
        Object *obj = OBJECT(item->data);
        if (!strcmp(object_get_typename(obj), TYPE_PC_DIMM)) {
            size += object_property_get_int(obj, PC_DIMM_SIZE_PROP,
                                            &error_abort);
        }
    }
    g_slist_free(list);

    return size;
}

static bool virtio_balloon_page_poison_support(void *opaque)
{
    VirtIOBalloon *s = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    return virtio_vdev_has_feature(vdev, VIRTIO_BALLOON_F_PAGE_POISON);
}

static void virtio_balloon_set_config(VirtIODevice *vdev,
                                      const uint8_t *config_data)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(vdev);
    struct virtio_balloon_config config = {};
    uint32_t oldactual = dev->actual;
    ram_addr_t vm_ram_size = get_current_ram_size();

    memcpy(&config, config_data, virtio_balloon_config_size(dev));
    dev->actual = le32_to_cpu(config.actual);
    if (dev->actual != oldactual) {
        qapi_event_send_balloon_change(vm_ram_size -
                        ((ram_addr_t) dev->actual << VIRTIO_BALLOON_PFN_SHIFT),
                        &error_abort);
    }
    dev->poison_val = 0;
    if (virtio_balloon_page_poison_support(dev)) {
        dev->poison_val = le32_to_cpu(config.poison_val);
    }
    trace_virtio_balloon_set_config(dev->actual, oldactual);
}

static uint64_t virtio_balloon_get_features(VirtIODevice *vdev, uint64_t f,
                                            Error **errp)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(vdev);
    f |= dev->host_features;
    virtio_add_feature(&f, VIRTIO_BALLOON_F_STATS_VQ);
    return f;
}

static void virtio_balloon_stat(void *opaque, BalloonInfo *info)
{
    VirtIOBalloon *dev = opaque;
    info->actual = get_current_ram_size() - ((uint64_t) dev->actual <<
                                             VIRTIO_BALLOON_PFN_SHIFT);
}

static void virtio_balloon_to_target(void *opaque, ram_addr_t target)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(opaque);
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    ram_addr_t vm_ram_size = get_current_ram_size();

    if (target > vm_ram_size) {
        target = vm_ram_size;
    }
    if (target) {
        dev->num_pages = (vm_ram_size - target) >> VIRTIO_BALLOON_PFN_SHIFT;
        virtio_notify_config(vdev);
    }
    trace_virtio_balloon_to_target(target, dev->num_pages);
}

static int virtio_balloon_post_load_device(void *opaque, int version_id)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(opaque);

    if (balloon_stats_enabled(s)) {
        balloon_stats_change_timer(s, s->stats_poll_interval);
    }
    return 0;
}

/*
 * Only sent when the guest negotiated PAGE_POISON (it does with init_on_free
 * or page poisoning); upstream uses the same (misspelt) section name.
 */
static const VMStateDescription vmstate_virtio_balloon_page_poison = {
    .name = "vitio-balloon-device/page-poison",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = virtio_balloon_page_poison_support,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(poison_val, VirtIOBalloon),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_virtio_balloon_device = {
    .name = "virtio-balloon-device",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = virtio_balloon_post_load_device,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(num_pages, VirtIOBalloon),
        VMSTATE_UINT32(actual, VirtIOBalloon),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * []) {
        &vmstate_virtio_balloon_page_poison,
        NULL
    }
};

static void virtio_balloon_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOBalloon *s = VIRTIO_BALLOON(dev);
    int ret;

    virtio_init(vdev, "virtio-balloon", VIRTIO_ID_BALLOON,
                virtio_balloon_config_size(s));

    ret = qemu_add_balloon_handler(virtio_balloon_to_target,
                                   virtio_balloon_stat, s);

    if (ret < 0) {
        error_setg(errp, "Only one balloon device is supported");
        virtio_cleanup(vdev);
        return;
    }

    s->ivq = virtio_add_queue(vdev, 128, virtio_balloon_handle_output);
    s->dvq = virtio_add_queue(vdev, 128, virtio_balloon_handle_output);
    s->svq = virtio_add_queue(vdev, 128, virtio_balloon_receive_stats);

    /*
     * The guest numbers its queues densely: inflate, deflate, stats, then
     * (free page hint, not offered here) and reporting, so this is queue 3.
     * Linux needs at least PAGE_REPORTING_CAPACITY (32) descriptors.
     */
    if (virtio_has_feature(s->host_features, VIRTIO_BALLOON_F_REPORTING)) {
        s->reporting_vq = virtio_add_queue(vdev, 32,
                                           virtio_balloon_handle_report);
        info_report("virtio-balloon: free page reporting enabled");
    }

    reset_stats(s);
}

static void virtio_balloon_device_unrealize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOBalloon *s = VIRTIO_BALLOON(dev);

    balloon_stats_destroy_timer(s);
    qemu_remove_balloon_handler(s);
    virtio_cleanup(vdev);
}

static void virtio_balloon_device_reset(VirtIODevice *vdev)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(vdev);

    if (s->stats_vq_elem != NULL) {
        virtqueue_unpop(s->svq, s->stats_vq_elem, 0);
        g_free(s->stats_vq_elem);
        s->stats_vq_elem = NULL;
    }

    s->poison_val = 0;
}

static void virtio_balloon_set_status(VirtIODevice *vdev, uint8_t status)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(vdev);

    if (!s->stats_vq_elem && vdev->vm_running &&
        (status & VIRTIO_CONFIG_S_DRIVER_OK) && virtqueue_rewind(s->svq, 1)) {
        /* poll stats queue for the element we have discarded when the VM
         * was stopped */
        virtio_balloon_receive_stats(vdev, s->svq);
    }
}

static void virtio_balloon_instance_init(Object *obj)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(obj);

    object_property_add(obj, "guest-stats", "guest statistics",
                        balloon_stats_get_all, NULL, NULL, s, NULL);

    object_property_add(obj, "guest-stats-polling-interval", "int",
                        balloon_stats_get_poll_interval,
                        balloon_stats_set_poll_interval,
                        NULL, s, NULL);
}

static const VMStateDescription vmstate_virtio_balloon = {
    .name = "virtio-balloon",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static Property virtio_balloon_properties[] = {
    DEFINE_PROP_BIT("deflate-on-oom", VirtIOBalloon, host_features,
                    VIRTIO_BALLOON_F_DEFLATE_ON_OOM, false),
    /*
     * Same defaults as upstream: page-poison on (a guest with init_on_free
     * or page poisoning refuses REPORTING without it), reporting opt-in.
     */
    DEFINE_PROP_BIT("page-poison", VirtIOBalloon, host_features,
                    VIRTIO_BALLOON_F_PAGE_POISON, true),
    DEFINE_PROP_BIT("free-page-reporting", VirtIOBalloon, host_features,
                    VIRTIO_BALLOON_F_REPORTING, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_balloon_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->props = virtio_balloon_properties;
    dc->vmsd = &vmstate_virtio_balloon;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = virtio_balloon_device_realize;
    vdc->unrealize = virtio_balloon_device_unrealize;
    vdc->reset = virtio_balloon_device_reset;
    vdc->get_config = virtio_balloon_get_config;
    vdc->set_config = virtio_balloon_set_config;
    vdc->get_features = virtio_balloon_get_features;
    vdc->set_status = virtio_balloon_set_status;
    vdc->vmsd = &vmstate_virtio_balloon_device;
}

static const TypeInfo virtio_balloon_info = {
    .name = TYPE_VIRTIO_BALLOON,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOBalloon),
    .instance_init = virtio_balloon_instance_init,
    .class_init = virtio_balloon_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_balloon_info);
}

type_init(virtio_register_types)
