#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/watchdog/g233_wdt.h"
#include "migration/vmstate.h"
#include "system/runstate.h"

static void g233_wdt_update_irq(G233WDTState *s)
{
    int level = (s->sr & G233_WDT_SR_TIMEOUT) &&
                (s->ctrl & G233_WDT_CTRL_INTEN);
    qemu_set_irq(s->irq, level);
}

static void g233_wdt_drive_outputs(G233WDTState *s)
{
    qemu_set_irq(s->output_wdog, !!(s->sr & G233_WDT_SR_TIMEOUT));
}

static void g233_wdt_catchup(G233WDTState *s)
{
    if (!(s->ctrl & G233_WDT_CTRL_EN)) {
        return;
    }
    if (s->sr & G233_WDT_SR_TIMEOUT) {
        return;
    }

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed = now - s->last_update_ns;
    if (elapsed <= 0) {
        return;
    }

    if ((uint64_t)elapsed >= s->load) {
        s->sr |= G233_WDT_SR_TIMEOUT;
        g233_wdt_update_irq(s);
        g233_wdt_drive_outputs(s);
        if (s->ctrl & G233_WDT_CTRL_RSTEN) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
    }
}

static uint64_t g233_wdt_get_val(G233WDTState *s)
{
    if (!(s->ctrl & G233_WDT_CTRL_EN)) {
        return s->load;
    }

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed = now - s->last_update_ns;
    if (elapsed <= 0) {
        return s->load;
    }

    if ((uint64_t)elapsed >= s->load) {
        return 0;
    }
    return s->load - (uint32_t)elapsed;
}

static uint64_t g233_wdt_read(void *opaque, hwaddr offset, unsigned size)
{
    G233WDTState *s = G233_WDT(opaque);

    switch (offset) {
    case G233_WDT_CTRL:
        return s->ctrl | (s->locked ? (1u << 3) : 0);
    case G233_WDT_LOAD:
        return s->load;
    case G233_WDT_VAL:
        g233_wdt_catchup(s);
        return g233_wdt_get_val(s);
    case G233_WDT_KEY:
        return 0;
    case G233_WDT_SR:
        g233_wdt_catchup(s);
        return s->sr;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void g233_wdt_write(void *opaque, hwaddr offset,
                           uint64_t value, unsigned size)
{
    G233WDTState *s = G233_WDT(opaque);

    switch (offset) {
    case G233_WDT_CTRL:
        if (s->locked) {
            break;
        }
        s->ctrl = value & G233_WDT_CTRL_WMASK;
        if (s->ctrl & G233_WDT_CTRL_EN) {
            s->last_update_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        break;
    case G233_WDT_LOAD:
        if (s->locked) {
            break;
        }
        s->load = value;
        break;
    case G233_WDT_VAL:
        break;
    case G233_WDT_KEY:
        if (value == G233_WDT_KEY_FEED) {
            s->sr &= ~G233_WDT_SR_TIMEOUT;
            s->last_update_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            g233_wdt_update_irq(s);
            g233_wdt_drive_outputs(s);
        } else if (value == G233_WDT_KEY_LOCK) {
            s->locked = true;
        }
        break;
    case G233_WDT_SR:
        s->sr &= ~(value & G233_WDT_SR_TIMEOUT);
        s->last_update_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        g233_wdt_update_irq(s);
        g233_wdt_drive_outputs(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
}

static const MemoryRegionOps g233_wdt_ops = {
    .read = g233_wdt_read,
    .write = g233_wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void g233_wdt_reset(DeviceState *dev)
{
    G233WDTState *s = G233_WDT(dev);

    s->ctrl = 0;
    s->load = 0xFFFF;
    s->sr = 0;
    s->last_update_ns = 0;
    s->locked = false;
}

static const VMStateDescription vmstate_g233_wdt = {
    .name = TYPE_G233_WDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, G233WDTState),
        VMSTATE_UINT32(load, G233WDTState),
        VMSTATE_UINT32(sr, G233WDTState),
        VMSTATE_INT64(last_update_ns, G233WDTState),
        VMSTATE_BOOL(locked, G233WDTState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_wdt_init(Object *obj)
{
    G233WDTState *s = G233_WDT(obj);

    memory_region_init_io(&s->mmio, OBJECT(obj), &g233_wdt_ops, s,
                          TYPE_G233_WDT, G233_WDT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out(DEVICE(obj), &s->output_wdog, 1);
}

static void g233_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_g233_wdt;
    device_class_set_legacy_reset(dc, g233_wdt_reset);
    dc->desc = "G233 WDT";
}

static const TypeInfo g233_wdt_info = {
    .name          = TYPE_G233_WDT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233WDTState),
    .instance_init = g233_wdt_init,
    .class_init    = g233_wdt_class_init,
};

static void g233_wdt_register_types(void)
{
    type_register_static(&g233_wdt_info);
}

type_init(g233_wdt_register_types)

DeviceState *g233_wdt_create(hwaddr addr, qemu_irq irq)
{
    DeviceState *dev;
    SysBusDevice *sbd;

    dev = qdev_new(TYPE_G233_WDT);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, addr);
    sysbus_connect_irq(sbd, 0, irq);

    return dev;
}
