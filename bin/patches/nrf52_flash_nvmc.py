# PlatformIO pre-Script: patcht den Adafruit-nRF52-Flashtreiber (InternalFS), damit er bei
# abgeschalteter SoftDevice direkt ueber die NVMC-Register schreibt/loescht, statt ueber
# sd_flash_*(). Hintergrund: Auf Geraeten mit beschaedigter SoftDevice friert sonst jedes
# Speichern ein. Idempotent (Marker), aendert nur die Framework-Kopie dieses Rechners.
Import("env")
import os, re

MARK = "/* MESHCORE_NVMC_PATCH */"
fw = env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52")
path = os.path.join(fw, "libraries", "InternalFileSytem", "src", "flash", "flash_nrf5x.c")
if os.path.isfile(path):
    src = open(path, encoding="utf-8").read()
    if MARK not in src:
        helpers = MARK + '''
static bool _sd_on(void) { uint8_t en = 0; (void) sd_softdevice_is_enabled(&en); return en != 0; }
static void _nvmc_wait(void) { while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {} }
static bool _nvmc_erase(uint32_t addr) {
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een << NVMC_CONFIG_WEN_Pos; _nvmc_wait();
  NRF_NVMC->ERASEPAGE = addr; _nvmc_wait();
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos; _nvmc_wait();
  return true;
}
static bool _nvmc_write(uint32_t dst, void const * src, uint32_t len) {
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos; _nvmc_wait();
  const uint32_t* s = (const uint32_t*) src; volatile uint32_t* d = (volatile uint32_t*) dst;
  for (uint32_t i = 0; i < len / 4; i++) { d[i] = s[i]; _nvmc_wait(); }
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos; _nvmc_wait();
  return true;
}
'''
        src = src.replace("static uint8_t _cache_buffer[FLASH_CACHE_SIZE]", helpers + "\nstatic uint8_t _cache_buffer[FLASH_CACHE_SIZE]", 1)
        src = src.replace("static bool fal_erase (uint32_t addr)\n{",
                          "static bool fal_erase (uint32_t addr)\n{\n  if (!_sd_on()) return _nvmc_erase(addr);", 1)
        src = src.replace("static bool fal_sub_program(uint32_t dst, void const * src, uint32_t len) {",
                          "static bool fal_sub_program(uint32_t dst, void const * src, uint32_t len) {\n  if (!_sd_on()) return _nvmc_write(dst, src, len);", 1)
        if src.count("_nvmc_") >= 4:
            open(path, "w", encoding="utf-8").write(src)
            print("nrf52_flash_nvmc: Flashtreiber gepatcht")
        else:
            print("nrf52_flash_nvmc: WARNUNG - Patch nicht anwendbar")
