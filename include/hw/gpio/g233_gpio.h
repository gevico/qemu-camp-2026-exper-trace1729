#ifndef G233_GPIO_H
#define G233_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_G233_GPIO "g233-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(G233GPIOState, G233_GPIO)

#define G233_GPIO_PINS 32
#define G233_GPIO_SIZE 0x100

#define G233_GPIO_REG_DIR   0x00
#define G233_GPIO_REG_OUT   0x04
#define G233_GPIO_REG_IN    0x08
#define G233_GPIO_REG_IE    0x0C
#define G233_GPIO_REG_IS    0x10
#define G233_GPIO_REG_TRIG  0x14
#define G233_GPIO_REG_POL   0x18

struct G233GPIOState {
    SysBusDevice parent_obj; // 保存父类对象，便于动态类型转换

    MemoryRegion mmio;
    qemu_irq irq;
    qemu_irq output[G233_GPIO_PINS];

    uint32_t dir; // 8 个 GPIO 方向寄存器，每位对应一个 GPIO，1 表示输出，0 表示输入
    uint32_t out;
    uint32_t in;
    uint32_t ext_in;
    uint32_t ie;
    uint32_t is;
    uint32_t trig;
    uint32_t pol;

    uint32_t ngpio;
};

DeviceState *g233_gpio_create(hwaddr addr, qemu_irq irq);

#endif
