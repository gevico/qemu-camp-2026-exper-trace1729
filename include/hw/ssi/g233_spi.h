#ifndef G233_SPI_H
#define G233_SPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_G233_SPI "g233-spi"
OBJECT_DECLARE_SIMPLE_TYPE(G233SPIState, G233_SPI)

#define G233_SPI_SIZE    0x1000
#define G233_SPI_NUM_CS  2

#define G233_SPI_CR1    0x00
#define G233_SPI_CR2    0x04
#define G233_SPI_SR     0x08
#define G233_SPI_DR     0x0C

#define G233_SPI_CR1_SPE     (1u << 0)
#define G233_SPI_CR1_MSTR    (1u << 2)
#define G233_SPI_CR1_ERRIE   (1u << 5)
#define G233_SPI_CR1_RXNEIE  (1u << 6)
#define G233_SPI_CR1_TXEIE   (1u << 7)

#define G233_SPI_SR_RXNE     (1u << 0)
#define G233_SPI_SR_TXE      (1u << 1)
#define G233_SPI_SR_OVERRUN  (1u << 4)

struct G233SPIState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    SSIBus *ssi_bus;
    QEMUTimer *transfer_timer;
    qemu_irq cs_lines[G233_SPI_NUM_CS];

    uint32_t cr1;
    uint32_t cr2;
    uint32_t sr;
    uint8_t tx_byte;
    uint8_t rx_byte;
    bool cs_asserted;
};

DeviceState *g233_spi_create(hwaddr addr, qemu_irq irq, SSIBus **bus);

#endif
