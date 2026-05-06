#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/timer/g233_pwm.h"
#include "migration/vmstate.h"

static void g233_pwm_update_irq(G233PWMState *s)
{
    uint32_t level = 0;
    for (int i = 0; i < G233_PWM_CHANS; i++) {
        if ((s->glb & G233_PWM_GLB_CH_DONE(i)) &&
            (s->ch[i].ctrl & G233_PWM_CTRL_INTIE)) {
            level = 1;
            break;
        }
    }
    qemu_set_irq(s->irq, level);
}

static void g233_pwm_drive_outputs(G233PWMState *s)
{
    for (int i = 0; i < G233_PWM_CHANS; i++) {
        uint32_t pol = s->ch[i].ctrl & G233_PWM_CTRL_POL;
        int level;

        if (!(s->ch[i].ctrl & G233_PWM_CTRL_EN)) {
            level = pol ? 1 : 0;
        } else {
            level = (s->ch[i].cnt < s->ch[i].duty) ^ !!pol;
        }

        qemu_set_irq(s->output[i], level);
    }
}

static void g233_pwm_catchup(G233PWMState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (int i = 0; i < G233_PWM_CHANS; i++) {
        if (!(s->ch[i].ctrl & G233_PWM_CTRL_EN)) {
            continue;
        }
        if (s->ch[i].period == 0) {
            continue;
        }

        int64_t elapsed = now - s->ch[i].last_update_ns;
        if (elapsed <= 0) {
            continue;
        }

        uint64_t ticks = (uint64_t)elapsed;
        uint64_t total = (uint64_t)s->ch[i].cnt + ticks;
        uint64_t wraps = total / s->ch[i].period;

        if (wraps > 0) {
            s->glb |= G233_PWM_GLB_CH_DONE(i);
        }

        s->ch[i].cnt = total % s->ch[i].period;
        s->ch[i].last_update_ns = now;
    }

    g233_pwm_drive_outputs(s);
}

static uint64_t g233_pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    G233PWMState *s = G233_PWM(opaque);
    int ch;

    if (offset == G233_PWM_GLB) {
        g233_pwm_catchup(s);
        return s->glb;
    }

    for (ch = 0; ch < G233_PWM_CHANS; ch++) {
        hwaddr base = offset - G233_PWM_CH(ch);
        if (base < 0x10) {
            g233_pwm_catchup(s);
            switch (base) {
            case G233_PWM_CH_CTRL:
                return s->ch[ch].ctrl;
            case G233_PWM_CH_PERIOD:
                return s->ch[ch].period;
            case G233_PWM_CH_DUTY:
                return s->ch[ch].duty;
            case G233_PWM_CH_CNT:
                return s->ch[ch].cnt;
            }
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
    return 0;
}

static void g233_pwm_write(void *opaque, hwaddr offset,
                           uint64_t value, unsigned size)
{
    G233PWMState *s = G233_PWM(opaque);
    int ch;

    if (offset == G233_PWM_GLB) {
        g233_pwm_catchup(s);
        s->glb &= ~(value & G233_PWM_GLB_DONE_MASK);
        g233_pwm_update_irq(s);
        return;
    }

    for (ch = 0; ch < G233_PWM_CHANS; ch++) {
        hwaddr base = offset - G233_PWM_CH(ch);
        if (base < 0x10) {
            g233_pwm_catchup(s);
            switch (base) {
            case G233_PWM_CH_CTRL:
                s->ch[ch].ctrl = value;
                if (value & G233_PWM_CTRL_EN) {
                    s->ch[ch].last_update_ns =
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                }
                break;
            case G233_PWM_CH_PERIOD:
                s->ch[ch].period = value;
                if (s->ch[ch].period == 0) {
                    s->ch[ch].cnt = 0;
                }
                break;
            case G233_PWM_CH_DUTY:
                s->ch[ch].duty = value;
                break;
            case G233_PWM_CH_CNT:
                break;
            }
            s->glb = (s->glb & ~G233_PWM_GLB_CH_EN(ch)) |
                     ((s->ch[ch].ctrl & G233_PWM_CTRL_EN) ? G233_PWM_GLB_CH_EN(ch) : 0);
            g233_pwm_update_irq(s);
            return;
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
}

static const MemoryRegionOps g233_pwm_ops = {
    .read = g233_pwm_read,
    .write = g233_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void g233_pwm_reset(DeviceState *dev)
{
    G233PWMState *s = G233_PWM(dev);

    s->glb = 0;
    for (int i = 0; i < G233_PWM_CHANS; i++) {
        s->ch[i].ctrl = 0;
        s->ch[i].period = 0;
        s->ch[i].duty = 0;
        s->ch[i].cnt = 0;
        s->ch[i].last_update_ns = 0;
    }
}

static const VMStateDescription vmstate_g233_pwm_ch = {
    .name = "g233-pwm-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, G233PWMChannel),
        VMSTATE_UINT32(period, G233PWMChannel),
        VMSTATE_UINT32(duty, G233PWMChannel),
        VMSTATE_UINT32(cnt, G233PWMChannel),
        VMSTATE_INT64(last_update_ns, G233PWMChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_g233_pwm = {
    .name = TYPE_G233_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(glb, G233PWMState),
        VMSTATE_STRUCT_ARRAY(ch, G233PWMState, G233_PWM_CHANS, 0,
                             vmstate_g233_pwm_ch, G233PWMChannel),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_pwm_init(Object *obj)
{
    G233PWMState *s = G233_PWM(obj);

    memory_region_init_io(&s->mmio, OBJECT(obj), &g233_pwm_ops, s,
                          TYPE_G233_PWM, G233_PWM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out(DEVICE(obj), s->output, G233_PWM_CHANS);
}

static void g233_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_g233_pwm;
    device_class_set_legacy_reset(dc, g233_pwm_reset);
    dc->desc = "G233 PWM";
}

static const TypeInfo g233_pwm_info = {
    .name          = TYPE_G233_PWM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233PWMState),
    .instance_init = g233_pwm_init,
    .class_init    = g233_pwm_class_init,
};

static void g233_pwm_register_types(void)
{
    type_register_static(&g233_pwm_info);
}

type_init(g233_pwm_register_types)

DeviceState *g233_pwm_create(hwaddr addr, qemu_irq irq)
{
    DeviceState *dev;
    SysBusDevice *sbd;

    dev = qdev_new(TYPE_G233_PWM);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, addr);
    sysbus_connect_irq(sbd, 0, irq);

    return dev;
}
