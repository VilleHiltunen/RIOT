#include <stdint.h>
#include <config.h>
#include <flashrom.h>

uint8_t config_save(void) {
    configmem_t mem = { CONFIG_KEY, sysconfig  };
    return (flashrom_erase((uint32_t) configmem) && flashrom_write((uint32_t) configmem, (char*) &mem, sizeof(mem)));
}
