#ifndef G233_PWM_H
#define G233_PWM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_G233_PWM "g233-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(G233PWMState, G233_PWM)

#define G233_PWM_CHANS   4
#define G233_PWM_SIZE    0x1000

#define G233_PWM_GLB      0x00
#define G233_PWM_CH(n)    (0x10 + (n) * 0x10)
#define G233_PWM_CH_CTRL   0x00
#define G233_PWM_CH_PERIOD 0x04
#define G233_PWM_CH_DUTY   0x08
#define G233_PWM_CH_CNT    0x0C

#define G233_PWM_CTRL_EN   (1u << 0)
#define G233_PWM_CTRL_POL  (1u << 1)
#define G233_PWM_CTRL_INTIE (1u << 2)

#define G233_PWM_GLB_CH_EN(n)   (1u << (n))
#define G233_PWM_GLB_CH_DONE(n) (1u << (4 + (n)))
#define G233_PWM_GLB_EN_MASK    0x0F
#define G233_PWM_GLB_DONE_MASK  0xF0

typedef struct {
    uint32_t ctrl;
    uint32_t period;
    uint32_t duty;
    uint32_t cnt;
    int64_t last_update_ns;
} G233PWMChannel;

struct G233PWMState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    qemu_irq output[G233_PWM_CHANS];

    uint32_t glb;
    G233PWMChannel ch[G233_PWM_CHANS];
};

DeviceState *g233_pwm_create(hwaddr addr, qemu_irq irq);

#endif
