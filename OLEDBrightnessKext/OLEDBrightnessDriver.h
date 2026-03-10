/*
 * OLEDBrightnessDriver.h
 * OLEDBrightness
 *
 * Kernel extension that controls OLED panel brightness on the
 * Dell XPS 15 7590 via Intel HDR TCON AUX brightness (DPCD 0x354).
 *
 * Architecture:
 *   1. Maps Intel UHD 630 GPU BAR0 MMIO
 *   2. Initializes AUX brightness (Intel Source OUI + ctrl bit 4)
 *   3. Polls BLC PWM duty register for macOS slider/key changes
 *   4. Forwards brightness to OLED panel via DPCD 0x354 AUX writes
 *   5. Exposes IOUserClient for CLI control
 */
#ifndef OLED_BRIGHTNESS_DRIVER_H
#define OLED_BRIGHTNESS_DRIVER_H

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>

/* AUX Channel A (eDP) registers — CoffeeLake offsets from BAR0 */
#define AUX_CH_A_CTL        0x64010
#define AUX_CH_A_DATA1      0x64014
#define AUX_CH_A_DATA2      0x64018
#define AUX_CH_A_DATA3      0x6401C
#define AUX_CH_A_DATA4      0x64020
#define AUX_CH_A_DATA5      0x64024

/* AUX_CTL bit definitions */
#define AUX_CTL_SEND_BUSY           (1U << 31)
#define AUX_CTL_DONE                (1U << 30)
#define AUX_CTL_TIMEOUT_ERROR       (1U << 28)
#define AUX_CTL_TIMEOUT_1600us      (0U << 26)
#define AUX_CTL_RECEIVE_ERROR       (1U << 25)
#define AUX_CTL_MSG_SIZE_SHIFT      20
#define AUX_CTL_PRECHARGE_2US_SHIFT 16

/* AUX native request commands */
#define AUX_NATIVE_WRITE            0x80
#define AUX_NATIVE_READ             0x90

/* User client external method selectors */
enum {
    kOLEDBrightnessMethodGetBrightness = 0,
    kOLEDBrightnessMethodSetBrightness = 1,
    kOLEDBrightnessMethodGetMax        = 2,
    kOLEDBrightnessMethodCount         = 3
};

class OLEDBrightnessDriver : public IOService {
    OSDeclareDefaultStructors(OLEDBrightnessDriver)

public:
    virtual bool     init(OSDictionary *dict = 0) override;
    virtual void     free() override;
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool     start(IOService *provider) override;
    virtual void     stop(IOService *provider) override;

    IOReturn         setBrightness(uint16_t level);
    IOReturn         getBrightness(uint16_t *level);
    uint16_t         getMaxBrightness() const { return 0xFFFF; }

private:
    /* GPU MMIO */
    IOPCIDevice          *fGPU;
    IOMemoryDescriptor   *fMMIODesc;
    IOMemoryMap          *fMMIOMap;
    volatile uint32_t    *fMMIOBase;
    bool     findAndMapGPU();
    void     unmapGPU();
    uint32_t mmioRead32(uint32_t offset);
    void     mmioWrite32(uint32_t offset, uint32_t value);

    /* AUX channel (eDP DPCD) */
    int      auxNativeRead(uint32_t addr, uint8_t *buf, int len);
    int      auxNativeWrite(uint32_t addr, const uint8_t *buf, int len);
    bool     initAuxBrightness();

    /* BLC PWM polling */
    IOTimerEventSource   *fPollTimer;
    IOWorkLoop           *fWorkLoop;
    uint32_t              fLastBLCValue;
    uint32_t              fBLCMax;
    static void pollTimerFired(OSObject *owner, IOTimerEventSource *sender);
    void     handleBLCPoll();

    /* State */
    uint16_t              fCurrentBrightness;
    bool                  fAuxBrightnessReady;
    uint16_t              fMaxNits;

    /* Fade animation */
    uint16_t              fFadeTarget;
    uint16_t              fFadeCurrent;
    uint16_t              fFadeStep;
    uint16_t              fFadeSteps;
    bool                  fFadeActive;
};

class OLEDBrightnessUserClient : public IOUserClient {
    OSDeclareDefaultStructors(OLEDBrightnessUserClient)

public:
    virtual bool     start(IOService *provider) override;
    virtual void     stop(IOService *provider) override;
    virtual IOReturn clientClose() override;
    virtual IOReturn externalMethod(uint32_t selector,
                                    IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch,
                                    OSObject *target,
                                    void *reference) override;

protected:
    OLEDBrightnessDriver *fDriver;

    static IOReturn sGetBrightness(OLEDBrightnessDriver *target, void *ref,
                                   IOExternalMethodArguments *args);
    static IOReturn sSetBrightness(OLEDBrightnessDriver *target, void *ref,
                                   IOExternalMethodArguments *args);
    static IOReturn sGetMax(OLEDBrightnessDriver *target, void *ref,
                            IOExternalMethodArguments *args);

    static const IOExternalMethodDispatch sMethods[kOLEDBrightnessMethodCount];
};

#endif /* OLED_BRIGHTNESS_DRIVER_H */
