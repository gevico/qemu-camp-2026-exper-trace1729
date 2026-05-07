#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/g233_spi.h"
#include "migration/vmstate.h"

static void g233_spi_update_irq(G233SPIState *s)
{
    int level = 0;

    if ((s->sr & G233_SPI_SR_TXE) && (s->cr1 & G233_SPI_CR1_TXEIE)) {
        level = 1;
    }
    if ((s->sr & G233_SPI_SR_RXNE) && (s->cr1 & G233_SPI_CR1_RXNEIE)) {
        level = 1;
    }
    if ((s->sr & G233_SPI_SR_OVERRUN) && (s->cr1 & G233_SPI_CR1_ERRIE)) {
        level = 1;
    }

    qemu_set_irq(s->irq, level);
}

static void g233_spi_update_cs(G233SPIState *s)
{
    for (int i = 0; i < G233_SPI_NUM_CS; i++) {
        qemu_set_irq(s->cs_lines[i], 1);
    }
    s->cs_asserted = false;

    if (s->cr1 & G233_SPI_CR1_SPE) {
        int cs = s->cr2 & 0x3;
        if (cs < G233_SPI_NUM_CS) {
            qemu_set_irq(s->cs_lines[cs], 0);
            s->cs_asserted = true;
        }
    }
}

static void g233_spi_transfer_done(void *opaque)
{
    G233SPIState *s = opaque;
    uint8_t rx;

    rx = (uint8_t)ssi_transfer(s->ssi_bus, s->tx_byte);
    s->tx_byte = 0;

    if (s->sr & G233_SPI_SR_RXNE) {
        s->sr |= G233_SPI_SR_OVERRUN;
    }

    s->rx_byte = rx;
    s->sr |= G233_SPI_SR_RXNE | G233_SPI_SR_TXE;
    g233_spi_update_irq(s);
}

static uint64_t g233_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    G233SPIState *s = G233_SPI(opaque);

    switch (offset) {
    case G233_SPI_CR1:
        return s->cr1;
    case G233_SPI_CR2:
        return s->cr2;
    case G233_SPI_SR:
        return s->sr;
    case G233_SPI_DR:
        s->sr &= ~G233_SPI_SR_RXNE;
        g233_spi_update_irq(s);
        return s->rx_byte;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void g233_spi_write(void *opaque, hwaddr offset,
                           uint64_t value, unsigned size)
{
    G233SPIState *s = G233_SPI(opaque);

    switch (offset) {
    case G233_SPI_CR1:
        s->cr1 = value & 0xE5;
        g233_spi_update_cs(s);
        if (!(s->cr1 & G233_SPI_CR1_SPE)) {
            s->sr &= ~(G233_SPI_SR_RXNE | G233_SPI_SR_OVERRUN);
        }
        g233_spi_update_irq(s);
        break;
    case G233_SPI_CR2:
        s->cr2 = value & 0x3;
        g233_spi_update_cs(s);
        break;
    case G233_SPI_SR:
        s->sr &= ~(value & G233_SPI_SR_OVERRUN);
        g233_spi_update_irq(s);
        break;
    case G233_SPI_DR:
        if (!(s->cr1 & G233_SPI_CR1_SPE)) {
            break;
        }
        if (!timer_pending(s->transfer_timer)) {
            s->tx_byte = (uint8_t)value;
            s->sr &= ~G233_SPI_SR_TXE;
            g233_spi_update_irq(s);
            timer_mod(s->transfer_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10000);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
}

static const MemoryRegionOps g233_spi_ops = {
    .read = g233_spi_read,
    .write = g233_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void g233_spi_reset(DeviceState *dev)
{
    G233SPIState *s = G233_SPI(dev);

    s->cr1 = 0;
    s->cr2 = 0;
    s->sr = G233_SPI_SR_TXE;
    s->tx_byte = 0;
    s->rx_byte = 0;
    s->cs_asserted = false;
}

static const VMStateDescription vmstate_g233_spi = {
    .name = TYPE_G233_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr1, G233SPIState),
        VMSTATE_UINT32(cr2, G233SPIState),
        VMSTATE_UINT32(sr, G233SPIState),
        VMSTATE_UINT8(tx_byte, G233SPIState),
        VMSTATE_UINT8(rx_byte, G233SPIState),
        VMSTATE_BOOL(cs_asserted, G233SPIState),
        VMSTATE_TIMER_PTR(transfer_timer, G233SPIState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_spi_realize(DeviceState *dev, Error **errp)
{
    G233SPIState *s = G233_SPI(dev);

    s->ssi_bus = ssi_create_bus(dev, "ssi");
    s->transfer_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     g233_spi_transfer_done, s);
}

static void g233_spi_init(Object *obj)
{
    G233SPIState *s = G233_SPI(obj);

    memory_region_init_io(&s->mmio, OBJECT(obj), &g233_spi_ops, s,
                          TYPE_G233_SPI, G233_SPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out(DEVICE(obj), s->cs_lines, G233_SPI_NUM_CS);
}

static void g233_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_g233_spi;
    dc->realize = g233_spi_realize;
    device_class_set_legacy_reset(dc, g233_spi_reset);
    dc->desc = "G233 SPI";
}

static const TypeInfo g233_spi_info = {
    .name          = TYPE_G233_SPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233SPIState),
    .instance_init = g233_spi_init,
    .class_init    = g233_spi_class_init,
};

static void g233_spi_register_types(void)
{
    type_register_static(&g233_spi_info);
}

type_init(g233_spi_register_types)

DeviceState *g233_spi_create(hwaddr addr, qemu_irq irq, SSIBus **bus)
{
    DeviceState *dev;
    SysBusDevice *sbd;

    dev = qdev_new(TYPE_G233_SPI);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, addr);
    sysbus_connect_irq(sbd, 0, irq);

    if (bus) {
        *bus = G233_SPI(dev)->ssi_bus;
    }

    return dev;
}
