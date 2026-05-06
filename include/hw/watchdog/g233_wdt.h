#ifndef G233_WDT_H
#define G233_WDT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_G233_WDT "g233-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(G233WDTState, G233_WDT)

#define G233_WDT_SIZE    0x1000

#define G233_WDT_CTRL    0x00
#define G233_WDT_LOAD    0x04
#define G233_WDT_VAL     0x08
#define G233_WDT_KEY     0x0C
#define G233_WDT_SR      0x10

#define G233_WDT_CTRL_EN     (1u << 0)
#define G233_WDT_CTRL_INTEN  (1u << 1)
#define G233_WDT_CTRL_RSTEN  (1u << 2)

#define G233_WDT_CTRL_WMASK  0x7

#define G233_WDT_KEY_FEED    0x5A5A5A5A
#define G233_WDT_KEY_LOCK    0x1ACCE551

#define G233_WDT_SR_TIMEOUT  (1u << 0)

struct G233WDTState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    qemu_irq output_wdog;

    uint32_t ctrl;
    uint32_t load;
    uint32_t sr;
    int64_t last_update_ns;

    bool locked;
};

DeviceState *g233_wdt_create(hwaddr addr, qemu_irq irq);

#endif
