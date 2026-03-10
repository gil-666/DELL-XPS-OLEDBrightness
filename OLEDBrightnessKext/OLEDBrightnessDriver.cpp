/*
 * OLEDBrightnessDriver.cpp
 * OLEDBrightness
 *
 * Controls OLED panel brightness on the Dell XPS 15 7590 (Hackintosh).
 *
 * Panel:  Samsung ATNA56WR04 (SDC vendor=0x4C83 product=0xA029)
 * GPU:    Intel UHD 630 (CoffeeLake, PCI 8086:3e9b)
 *
 * How it works:
 *   WhateverGreen intercepts macOS brightness slider/key changes and
 *   writes a duty cycle value to the BLC PWM register (0xC8258). This
 *   kext polls that register at 30Hz and translates changes to DPCD
 *   0x354 AUX writes, which the Samsung OLED TCON uses to set the
 *   actual panel luminance in nits.
 *
 * The AUX brightness path requires:
 *   1. Writing Intel Source OUI to DPCD 0x300
 *   2. Setting ctrl bit 4 at DPCD 0x344
 *   3. Writing brightness (nits, little-endian u16) to DPCD 0x354
 */

#include "OLEDBrightnessDriver.h"
#include <IOKit/IOLib.h>
#include <mach/kmod.h>

#define DLOG(fmt, ...) IOLog("OLEDBrightness: " fmt "\n", ##__VA_ARGS__)

/*
 * CNP (Cannon Point) PCH register layout:
 *   0xC8250 = BLC_PWM_CTL1  (bit 31 = enable)
 *   0xC8254 = BLC_PWM_FREQ  (max/period — WhateverGreen sets to 120000)
 *   0xC8258 = BLC_PWM_DUTY  (current brightness duty cycle)
 */
#define BLC_PWM_FREQ_REG  0xC8254
#define BLC_PWM_DUTY_REG  0xC8258

#define POLL_INTERVAL_MS  33  /* ~30Hz */
#define FADE_STEPS        12  /* ~400ms fade at 33ms interval */

/* ─── Module entry points for OpenCore prelinked injection ─── */

OSDefineMetaClassAndStructors(OLEDBrightnessDriver, IOService)

extern "C" kern_return_t OLEDBrightness_start(kmod_info_t *, void *) {
    return KERN_SUCCESS;
}
extern "C" kern_return_t OLEDBrightness_stop(kmod_info_t *, void *) {
    return KERN_SUCCESS;
}
KMOD_EXPLICIT_DECL(com.oledbrightness.driver, "3.0.0", OLEDBrightness_start, OLEDBrightness_stop)

/* ─── IOService lifecycle ─── */

bool OLEDBrightnessDriver::init(OSDictionary *dict) {
    if (!IOService::init(dict)) return false;

    fGPU = NULL;
    fMMIODesc = NULL;
    fMMIOMap = NULL;
    fMMIOBase = NULL;
    fPollTimer = NULL;
    fWorkLoop = NULL;
    fLastBLCValue = 0;
    fBLCMax = 0;
    fCurrentBrightness = 0x7FFF;
    fAuxBrightnessReady = false;
    fMaxNits = 0;
    fFadeActive = false;
    fFadeTarget = 0;
    fFadeCurrent = 0;
    fFadeStep = 0;
    fFadeSteps = 0;

    return true;
}

void OLEDBrightnessDriver::free() {
    if (fPollTimer) {
        fPollTimer->cancelTimeout();
        if (fWorkLoop) fWorkLoop->removeEventSource(fPollTimer);
        fPollTimer->release();
        fPollTimer = NULL;
    }
    unmapGPU();
    IOService::free();
}

IOService *OLEDBrightnessDriver::probe(IOService *provider, SInt32 *score) {
    *score = 10000;
    return this;
}

bool OLEDBrightnessDriver::start(IOService *provider) {
    DLOG("start() v3.0");

    if (!IOService::start(provider)) return false;

    setProperty("oled-brightness-version", "3.0.0");
    setProperty("panel-model", "Samsung ATNA56WR04");

    if (findAndMapGPU() && initAuxBrightness()) {
        setProperty("status", "active");
        setProperty("brightness-method", "DPCD-0x354-AUX-nits");
        setProperty("max-nits", (unsigned long long)fMaxNits, 16);

        fBLCMax = mmioRead32(BLC_PWM_FREQ_REG);
        if (fBLCMax == 0) fBLCMax = 120000;
        fLastBLCValue = mmioRead32(BLC_PWM_DUTY_REG);

        fWorkLoop = getWorkLoop();
        if (fWorkLoop) {
            fPollTimer = IOTimerEventSource::timerEventSource(
                this, &OLEDBrightnessDriver::pollTimerFired);
            if (fPollTimer) {
                fWorkLoop->addEventSource(fPollTimer);
                fPollTimer->setTimeoutMS(POLL_INTERVAL_MS);
                DLOG("BLC poll timer started (%dms)", POLL_INTERVAL_MS);
            }
        }
    } else {
        setProperty("status", "init-failed");
        DLOG("AUX brightness init failed");
    }

    registerService();
    return true;
}

void OLEDBrightnessDriver::stop(IOService *provider) {
    if (fPollTimer) {
        fPollTimer->cancelTimeout();
        if (fWorkLoop) fWorkLoop->removeEventSource(fPollTimer);
        fPollTimer->release();
        fPollTimer = NULL;
    }
    unmapGPU();
    IOService::stop(provider);
}

/* ─── AUX Channel — Native DPCD Read/Write via GPU MMIO ─── */

int OLEDBrightnessDriver::auxNativeRead(uint32_t addr, uint8_t *buf, int len) {
    if (!fMMIOBase || !buf || len < 1 || len > 16) return -1;

    mmioWrite32(AUX_CH_A_CTL, AUX_CTL_DONE | AUX_CTL_TIMEOUT_ERROR | AUX_CTL_RECEIVE_ERROR);
    IODelay(10);

    uint32_t hdr = ((uint32_t)(AUX_NATIVE_READ | ((addr >> 16) & 0x0F)) << 24)
                 | ((addr & 0xFFFF) << 8)
                 | ((len - 1) & 0xFF);
    mmioWrite32(AUX_CH_A_DATA1, hdr);

    uint32_t ctl = AUX_CTL_SEND_BUSY
                 | AUX_CTL_TIMEOUT_1600us
                 | (4U << AUX_CTL_MSG_SIZE_SHIFT)
                 | (5U << AUX_CTL_PRECHARGE_2US_SHIFT)
                 | 62;
    mmioWrite32(AUX_CH_A_CTL, ctl);

    for (int i = 0; i < 100; i++) {
        IODelay(100);
        uint32_t st = mmioRead32(AUX_CH_A_CTL);
        if (!(st & AUX_CTL_SEND_BUSY)) {
            if (st & AUX_CTL_TIMEOUT_ERROR) return -2;
            if (st & AUX_CTL_RECEIVE_ERROR) return -3;

            uint32_t replySize = (st >> AUX_CTL_MSG_SIZE_SHIFT) & 0x1F;
            if (replySize < 1) return -4;

            uint32_t data[4];
            data[0] = mmioRead32(AUX_CH_A_DATA1);
            uint8_t reply = (data[0] >> 24) & 0xFF;
            if ((reply >> 4) != 0) return -(10 + (reply >> 4));

            if (len > 3) data[1] = mmioRead32(AUX_CH_A_DATA2);
            if (len > 7) data[2] = mmioRead32(AUX_CH_A_DATA3);
            if (len > 11) data[3] = mmioRead32(AUX_CH_A_DATA4);

            uint8_t raw[16];
            for (int r = 0; r < 4; r++) {
                raw[r*4+0] = (data[r] >> 24) & 0xFF;
                raw[r*4+1] = (data[r] >> 16) & 0xFF;
                raw[r*4+2] = (data[r] >>  8) & 0xFF;
                raw[r*4+3] = (data[r]      ) & 0xFF;
            }
            for (int b = 0; b < len; b++)
                buf[b] = raw[b + 1];

            return len;
        }
    }
    return -5;
}

int OLEDBrightnessDriver::auxNativeWrite(uint32_t addr, const uint8_t *buf, int len) {
    if (!fMMIOBase || !buf || len < 1 || len > 16) return -1;

    mmioWrite32(AUX_CH_A_CTL, AUX_CTL_DONE | AUX_CTL_TIMEOUT_ERROR | AUX_CTL_RECEIVE_ERROR);
    IODelay(10);

    uint8_t raw[20] = {};
    raw[0] = AUX_NATIVE_WRITE | ((addr >> 16) & 0x0F);
    raw[1] = (addr >> 8) & 0xFF;
    raw[2] = addr & 0xFF;
    raw[3] = (len - 1) & 0xFF;
    for (int i = 0; i < len; i++)
        raw[4 + i] = buf[i];

    int totalBytes = 4 + len;
    for (int r = 0; r < (totalBytes + 3) / 4; r++) {
        uint32_t val = ((uint32_t)raw[r*4+0] << 24)
                     | ((uint32_t)raw[r*4+1] << 16)
                     | ((uint32_t)raw[r*4+2] <<  8)
                     | ((uint32_t)raw[r*4+3]);
        mmioWrite32(AUX_CH_A_DATA1 + r * 4, val);
    }

    uint32_t ctl = AUX_CTL_SEND_BUSY
                 | AUX_CTL_TIMEOUT_1600us
                 | ((uint32_t)totalBytes << AUX_CTL_MSG_SIZE_SHIFT)
                 | (5U << AUX_CTL_PRECHARGE_2US_SHIFT)
                 | 62;
    mmioWrite32(AUX_CH_A_CTL, ctl);

    for (int i = 0; i < 100; i++) {
        IODelay(100);
        uint32_t st = mmioRead32(AUX_CH_A_CTL);
        if (!(st & AUX_CTL_SEND_BUSY)) {
            if (st & AUX_CTL_TIMEOUT_ERROR) return -2;
            if (st & AUX_CTL_RECEIVE_ERROR) return -3;

            uint32_t d1 = mmioRead32(AUX_CH_A_DATA1);
            uint8_t reply = (d1 >> 24) & 0xFF;
            if ((reply >> 4) != 0) return -(10 + (reply >> 4));

            return len;
        }
    }
    return -5;
}

/* ─── AUX Brightness Init (Intel HDR TCON via DPCD 0x354) ─── */

bool OLEDBrightnessDriver::initAuxBrightness() {
    fAuxBrightnessReady = false;
    fMaxNits = 0;

    if (!fMMIOBase) return false;

    /* Read TCON capabilities at 0x340 */
    uint8_t tcon_caps[4] = {};
    int rc = auxNativeRead(0x340, tcon_caps, 4);
    if (rc < 4 || tcon_caps[0] == 0) {
        DLOG("initAuxBrightness: TCON not present (rc=%d)", rc);
        return false;
    }

    /* cap2 bit 0 = SDR uses AUX */
    if (!(tcon_caps[2] & 0x01)) {
        DLOG("initAuxBrightness: sdr_uses_aux not set (cap2=0x%02X)", tcon_caps[2]);
        return false;
    }

    /* Read current brightness to determine max nits */
    uint8_t brt_current[4] = {};
    rc = auxNativeRead(0x354, brt_current, 4);
    if (rc < 2) return false;
    fMaxNits = brt_current[0] | ((uint16_t)brt_current[1] << 8);
    if (fMaxNits == 0) fMaxNits = 440;  /* ATNA56WR04 default */

    /* Write Intel Source OUI to DPCD 0x300 */
    uint8_t oui[3] = { 0x00, 0xAA, 0x01 };
    auxNativeWrite(0x300, oui, 3);
    uint8_t dev[3] = { 0x14, 0x00, 0x00 };
    auxNativeWrite(0x303, dev, 3);

    /* Enable AUX brightness control (ctrl bit 4 at 0x344) */
    uint8_t old_ctrl = 0;
    auxNativeRead(0x344, &old_ctrl, 1);
    uint8_t new_ctrl = old_ctrl | 0x10;
    auxNativeWrite(0x344, &new_ctrl, 1);

    uint8_t ctrl_rb = 0;
    auxNativeRead(0x344, &ctrl_rb, 1);
    if (!(ctrl_rb & 0x10)) {
        DLOG("initAuxBrightness: ctrl bit 4 rejected (rb=0x%02X)", ctrl_rb);
        return false;
    }

    fAuxBrightnessReady = true;
    fCurrentBrightness = 0xFFFF;
    DLOG("initAuxBrightness: OK — maxNits=%d", fMaxNits);
    return true;
}

/* ─── BLC PWM Polling ─── */

void OLEDBrightnessDriver::pollTimerFired(OSObject *owner,
                                            IOTimerEventSource *sender) {
    OLEDBrightnessDriver *self = OSDynamicCast(OLEDBrightnessDriver, owner);
    if (self) {
        self->handleBLCPoll();
        if (sender) sender->setTimeoutMS(POLL_INTERVAL_MS);
    }
}

void OLEDBrightnessDriver::handleBLCPoll() {
    if (!fMMIOBase || !fBLCMax || !fAuxBrightnessReady) return;

    uint32_t currentBLC = mmioRead32(BLC_PWM_DUTY_REG);

    uint16_t targetLevel;
    if (currentBLC >= fBLCMax)
        targetLevel = 0xFFFF;
    else
        targetLevel = (uint16_t)((uint64_t)currentBLC * 0xFFFF / fBLCMax);

    /* Detect BLC change → start fade */
    if (currentBLC != fLastBLCValue) {
        fLastBLCValue = currentBLC;
        fFadeCurrent = fCurrentBrightness;
        fFadeTarget = targetLevel;
        fFadeSteps = FADE_STEPS;
        fFadeStep = 0;
        fFadeActive = true;
    }

    if (fFadeActive) {
        fFadeStep++;
        if (fFadeStep >= fFadeSteps) {
            fCurrentBrightness = fFadeTarget;
            fFadeActive = false;
        } else {
            /* Ease-in-out cubic */
            int32_t diff = (int32_t)fFadeTarget - (int32_t)fFadeCurrent;
            uint32_t num = fFadeStep;
            uint32_t den = fFadeSteps;
            /* Fixed-point cubic ease: t < 0.5 → 4t³, else 1 - (-2t+2)³/2 */
            if (num * 2 < den) {
                /* t < 0.5: 4 * (num/den)^3 = 4*num^3 / den^3 */
                uint64_t n3 = (uint64_t)num * num * num;
                uint64_t d3 = (uint64_t)den * den * den;
                fCurrentBrightness = fFadeCurrent + (int32_t)(diff * (int64_t)(4 * n3) / (int64_t)d3);
            } else {
                /* t >= 0.5: 1 - (-2t+2)^3 / 2 */
                uint32_t r = 2 * den - 2 * num; /* (-2t+2)*den */
                uint64_t r3 = (uint64_t)r * r * r;
                uint64_t d3 = (uint64_t)den * den * den;
                int64_t ease = (int64_t)d3 - (int64_t)(r3 / 2);
                fCurrentBrightness = fFadeCurrent + (int32_t)(diff * ease / (int64_t)d3);
            }
        }

        uint16_t nits = (uint16_t)((uint32_t)fCurrentBrightness * fMaxNits / 0xFFFF);
        uint8_t brt[4] = {
            (uint8_t)(nits & 0xFF),
            (uint8_t)((nits >> 8) & 0xFF),
            0, 0
        };
        auxNativeWrite(0x354, brt, 4);
        setProperty("brightness-level", fCurrentBrightness, 16);
    }
}

/* ─── GPU MMIO ─── */

bool OLEDBrightnessDriver::findAndMapGPU() {
    OSDictionary *matching = IOService::serviceMatching("IOPCIDevice");
    if (!matching) return false;

    OSIterator *iter = IOService::getMatchingServices(matching);
    if (!iter) return false;

    IOPCIDevice *gpu = NULL;
    while (IOService *svc = OSDynamicCast(IOService, iter->getNextObject())) {
        IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, svc);
        if (!pci) continue;
        if (pci->configRead16(kIOPCIConfigVendorID) == 0x8086 &&
            pci->configRead16(kIOPCIConfigDeviceID) == 0x3E9B) {
            gpu = pci;
            break;
        }
    }
    iter->release();

    if (!gpu) {
        DLOG("findAndMapGPU: Intel GPU 8086:3e9b not found");
        return false;
    }

    gpu->setMemoryEnable(true);
    gpu->setBusMasterEnable(true);

    IODeviceMemory *bar0Mem = gpu->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!bar0Mem) return false;

    bool opened = gpu->open(this);
    if (opened) {
        fMMIOMap = gpu->mapDeviceMemoryWithRegister(
            kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    }

    if (!fMMIOMap) {
        if (opened) gpu->close(this);
        IOPhysicalAddress bar0Phys = bar0Mem->getPhysicalAddress();
        IOByteCount bar0Len = bar0Mem->getLength();
        fMMIODesc = IOMemoryDescriptor::withPhysicalAddress(
            bar0Phys, bar0Len, kIODirectionInOut);
        if (!fMMIODesc) return false;

        fMMIOMap = fMMIODesc->createMappingInTask(
            kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache);
        if (!fMMIOMap) {
            fMMIODesc->release();
            fMMIODesc = NULL;
            return false;
        }
    }

    fMMIOBase = (volatile uint32_t *)fMMIOMap->getVirtualAddress();
    fGPU = gpu;
    gpu->retain();
    return true;
}

void OLEDBrightnessDriver::unmapGPU() {
    if (fMMIOMap) { fMMIOMap->release(); fMMIOMap = NULL; }
    if (fMMIODesc) { fMMIODesc->release(); fMMIODesc = NULL; }
    fMMIOBase = NULL;
    if (fGPU) { fGPU->close(this); fGPU->release(); fGPU = NULL; }
}

uint32_t OLEDBrightnessDriver::mmioRead32(uint32_t offset) {
    uint32_t val = fMMIOBase[offset / 4];
    __asm__ volatile("lfence" ::: "memory");
    return val;
}

void OLEDBrightnessDriver::mmioWrite32(uint32_t offset, uint32_t value) {
    fMMIOBase[offset / 4] = value;
    __asm__ volatile("mfence" ::: "memory");
}

/* ─── Public Brightness API ─── */

IOReturn OLEDBrightnessDriver::setBrightness(uint16_t level) {
    if (!fAuxBrightnessReady || !fMMIOBase)
        return kIOReturnNotReady;

    uint16_t nits = (uint16_t)((uint32_t)level * fMaxNits / 0xFFFF);
    uint8_t brt[4] = {
        (uint8_t)(nits & 0xFF),
        (uint8_t)((nits >> 8) & 0xFF),
        0, 0
    };
    int rc = auxNativeWrite(0x354, brt, 4);
    if (rc != 4) return kIOReturnIOError;

    fCurrentBrightness = level;
    setProperty("brightness-level", level, 16);
    return kIOReturnSuccess;
}

IOReturn OLEDBrightnessDriver::getBrightness(uint16_t *level) {
    if (!level) return kIOReturnBadArgument;

    if (fAuxBrightnessReady && fMMIOBase) {
        uint8_t brt[4] = {};
        int rc = auxNativeRead(0x354, brt, 4);
        if (rc == 4) {
            uint16_t nits = brt[0] | ((uint16_t)brt[1] << 8);
            if (fMaxNits > 0)
                *level = (uint16_t)((uint32_t)nits * 0xFFFF / fMaxNits);
            else
                *level = (uint16_t)nits;
            fCurrentBrightness = *level;
            return kIOReturnSuccess;
        }
    }

    *level = fCurrentBrightness;
    return kIOReturnSuccess;
}

/* ─── UserClient ─── */

OSDefineMetaClassAndStructors(OLEDBrightnessUserClient, IOUserClient)

const IOExternalMethodDispatch
OLEDBrightnessUserClient::sMethods[kOLEDBrightnessMethodCount] = {
    [kOLEDBrightnessMethodGetBrightness] = {
        .function = (IOExternalMethodAction)&sGetBrightness,
        .checkScalarInputCount  = 0,
        .checkStructureInputSize  = 0,
        .checkScalarOutputCount = 1,
        .checkStructureOutputSize = 0,
    },
    [kOLEDBrightnessMethodSetBrightness] = {
        .function = (IOExternalMethodAction)&sSetBrightness,
        .checkScalarInputCount  = 1,
        .checkStructureInputSize  = 0,
        .checkScalarOutputCount = 0,
        .checkStructureOutputSize = 0,
    },
    [kOLEDBrightnessMethodGetMax] = {
        .function = (IOExternalMethodAction)&sGetMax,
        .checkScalarInputCount  = 0,
        .checkStructureInputSize  = 0,
        .checkScalarOutputCount = 1,
        .checkStructureOutputSize = 0,
    },
};

bool OLEDBrightnessUserClient::start(IOService *provider) {
    fDriver = OSDynamicCast(OLEDBrightnessDriver, provider);
    if (!fDriver) return false;
    return IOUserClient::start(provider);
}

void OLEDBrightnessUserClient::stop(IOService *provider) {
    fDriver = NULL;
    IOUserClient::stop(provider);
}

IOReturn OLEDBrightnessUserClient::clientClose() {
    terminate();
    return kIOReturnSuccess;
}

IOReturn OLEDBrightnessUserClient::externalMethod(uint32_t selector,
                                                    IOExternalMethodArguments *args,
                                                    IOExternalMethodDispatch *dispatch,
                                                    OSObject *target,
                                                    void *reference) {
    if (selector >= kOLEDBrightnessMethodCount)
        return kIOReturnUnsupported;

    dispatch = (IOExternalMethodDispatch *)&sMethods[selector];
    target = fDriver;
    reference = NULL;
    return IOUserClient::externalMethod(selector, args, dispatch, target, reference);
}

IOReturn OLEDBrightnessUserClient::sGetBrightness(OLEDBrightnessDriver *target,
                                                    void *ref,
                                                    IOExternalMethodArguments *args) {
    uint16_t level = 0;
    IOReturn ret = target->getBrightness(&level);
    if (ret == kIOReturnSuccess)
        args->scalarOutput[0] = level;
    return ret;
}

IOReturn OLEDBrightnessUserClient::sSetBrightness(OLEDBrightnessDriver *target,
                                                    void *ref,
                                                    IOExternalMethodArguments *args) {
    uint16_t level = (uint16_t)(args->scalarInput[0] & 0xFFFF);
    return target->setBrightness(level);
}

IOReturn OLEDBrightnessUserClient::sGetMax(OLEDBrightnessDriver *target,
                                            void *ref,
                                            IOExternalMethodArguments *args) {
    args->scalarOutput[0] = target->getMaxBrightness();
    return kIOReturnSuccess;
}
