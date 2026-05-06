#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/gpio/g233_gpio.h"
#include "migration/vmstate.h"

static void g233_gpio_update_irq(G233GPIOState *s)
{
    if (s->ie && s->is) {
        qemu_set_irq(s->irq, 1);
    } else {
        qemu_set_irq(s->irq, 0);
    }
}

/*
 *      One way to see the distinction in hardware terms:
                         ┌──────────────────────────────┐
                         │          G233 GPIO Chip        │
                         │                                │
       MMIO ←─→ regs ───┤  ┌─────┐                      │
                         │  │ IE&IS│── s->irq ──→ PLIC    │
                         │  └─────┘                      │
                         │                                │
                         │  DIR=1: s->out[i] ──→ output[i] ──→ external pin
                         │  DIR=0: high-Z                 │
                         └──────────────────────────────┘

     s->irq goes up to CPU, s->output[i] goes out to the board's physical pin. They're independent.
 * */ 
static void g233_gpio_update(G233GPIOState *s)
{
    uint32_t new_in;
    uint32_t changed;
    int i;

    new_in = (s->dir & s->out) | (~s->dir & s->ext_in);

    // 
    for (i = 0; i < (int)s->ngpio; i++) {
        if (extract32(s->dir, i, 1)) {
            qemu_set_irq(s->output[i], extract32(s->out, i, 1));
        }
    }

    changed = s->in ^ new_in;

    for (i = 0; i < (int)s->ngpio; i++) {
        if (!extract32(s->ie, i, 1)) {
            continue;
        }

        if (extract32(s->trig, i, 1)) {
            if (extract32(s->pol, i, 1)) {
                if (extract32(new_in, i, 1)) {
                    s->is |= (1u << i);
                } else {
                    s->is &= ~(1u << i);
                }
            } else {
                if (!extract32(new_in, i, 1)) {
                    s->is |= (1u << i);
                } else {
                    s->is &= ~(1u << i);
                }
            }
        } else {
            if (!extract32(changed, i, 1)) {
                continue;
            }
            if (extract32(s->pol, i, 1)) {
                if (extract32(new_in, i, 1) && !extract32(s->in, i, 1)) {
                    s->is |= (1u << i);
                }
            } else {
                if (!extract32(new_in, i, 1) && extract32(s->in, i, 1)) {
                    s->is |= (1u << i);
                }
            }
        }
    }

    s->in = new_in;
    g233_gpio_update_irq(s);
}

static uint64_t g233_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    G233GPIOState *s = G233_GPIO(opaque);

    switch (offset) {
    case G233_GPIO_REG_DIR:
        return s->dir;
    case G233_GPIO_REG_OUT:
        return s->out;
    case G233_GPIO_REG_IN:
        return s->in;
    case G233_GPIO_REG_IE:
        return s->ie;
    case G233_GPIO_REG_IS:
        return s->is;
    case G233_GPIO_REG_TRIG:
        return s->trig;
    case G233_GPIO_REG_POL:
        return s->pol;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void g233_gpio_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    G233GPIOState *s = G233_GPIO(opaque);

    switch (offset) {
    case G233_GPIO_REG_DIR:
        s->dir = value;
        g233_gpio_update(s);
        break;
    case G233_GPIO_REG_OUT:
        s->out = value;
        g233_gpio_update(s);
        break;
    case G233_GPIO_REG_IE:
        s->ie = value;
        g233_gpio_update_irq(s);
        break;
    case G233_GPIO_REG_IS:
        s->is &= ~value;
        g233_gpio_update_irq(s);
        break;
    case G233_GPIO_REG_TRIG:
        s->trig = value;
        break;
    case G233_GPIO_REG_POL:
        s->pol = value;
        break;
    case G233_GPIO_REG_IN:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
}

static const MemoryRegionOps g233_gpio_ops = {
    .read = g233_gpio_read,
    .write = g233_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void g233_gpio_set(void *opaque, int line, int value)
{
    G233GPIOState *s = G233_GPIO(opaque);

    assert(line >= 0 && line < (int)s->ngpio);

    if (value >= 0) {
        s->ext_in = deposit32(s->ext_in, line, 1, value != 0);
    }

    g233_gpio_update(s);
}

static void g233_gpio_reset(DeviceState *dev)
{
    G233GPIOState *s = G233_GPIO(dev);

    s->dir = 0;
    s->out = 0;
    s->in = 0;
    s->ext_in = 0;
    s->ie = 0;
    s->is = 0;
    s->trig = 0;
    s->pol = 0;
}

static void g233_gpio_realize(DeviceState *dev, Error **errp)
{
    G233GPIOState *s = G233_GPIO(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &g233_gpio_ops, s,
                          TYPE_G233_GPIO, G233_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    // 其他输入设备可以驱动这个 GPIO 引脚，更新 s->ext 的内部状态
    // s->ngpio 表示引脚的数量
    qdev_init_gpio_in(DEVICE(s), g233_gpio_set, s->ngpio);
    qdev_init_gpio_out(DEVICE(s), s->output, s->ngpio);
}

static const VMStateDescription vmstate_g233_gpio = {
    .name = TYPE_G233_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dir, G233GPIOState),
        VMSTATE_UINT32(out, G233GPIOState),
        VMSTATE_UINT32(in,  G233GPIOState),
        VMSTATE_UINT32(ext_in,  G233GPIOState),
        VMSTATE_UINT32(ie,  G233GPIOState),
        VMSTATE_UINT32(is,  G233GPIOState),
        VMSTATE_UINT32(trig, G233GPIOState),
        VMSTATE_UINT32(pol, G233GPIOState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property g233_gpio_properties[] = {
    DEFINE_PROP_UINT32("ngpio", G233GPIOState, ngpio, G233_GPIO_PINS),
};

static void g233_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, g233_gpio_properties);
    dc->vmsd = &vmstate_g233_gpio;
    dc->realize = g233_gpio_realize;
    device_class_set_legacy_reset(dc, g233_gpio_reset);
    dc->desc = "G233 GPIO";
}

static const TypeInfo g233_gpio_info = {
    .name = TYPE_G233_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233GPIOState),
    .class_init = g233_gpio_class_init,
};

static void g233_gpio_register_types(void)
{
    type_register_static(&g233_gpio_info);
}

type_init(g233_gpio_register_types)

DeviceState *g233_gpio_create(hwaddr addr, qemu_irq irq)
{
    DeviceState *dev;
    SysBusDevice *sbd;

    dev = qdev_new(TYPE_G233_GPIO);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, addr);
    sysbus_connect_irq(sbd, 0, irq);

    return dev;
}
