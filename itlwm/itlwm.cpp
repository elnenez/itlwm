/* add your code here */

/*
* Copyright (C) 2020  钟先耀
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/
#include "itlwm.hpp"
#include "types.h"
#include "kernel.h"

#include <IOKit/IOInterruptController.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/network/IONetworkMedium.h>
#include <net/ethernet.h>
#include "sha1.h"
#include <net80211/ieee80211_node.h>
#include <net80211/ieee80211_ioctl.h>

#define super IOEthernetController
OSDefineMetaClassAndStructors(itlwm, IOEthernetController)
OSDefineMetaClassAndStructors(CTimeout, OSObject)

IOWorkLoop *_fWorkloop;
IOCommandGate *_fCommandGate;

bool itlwm::init(OSDictionary *properties)
{
    super::init(properties);
    fwLoadLock = IOLockAlloc();
    return true;
}

IOService* itlwm::probe(IOService *provider, SInt32 *score)
{
    super::probe(provider, score);
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
    if (!device) {
        return NULL;
    }
    return iwm_match(device) == 0?NULL:this;
}

bool itlwm::configureInterface(IONetworkInterface *netif) {
    IONetworkData *nd;
    
    if (super::configureInterface(netif) == false) {
        XYLog("super failed\n");
        return false;
    }
    
    nd = netif->getNetworkData(kIONetworkStatsKey);
    if (!nd || !(fpNetStats = (IONetworkStats *)nd->getBuffer())) {
        XYLog("network statistics buffer unavailable?\n");
        return false;
    }
    com.sc_ic.ic_ac.ac_if.netStat = fpNetStats;
    com.sc_ic.ic_ac.ac_if.iface = OSDynamicCast(IOEthernetInterface, netif);
    
    return true;
}

IONetworkInterface *itlwm::createInterface()
{
    itlwm_interface *netif = new itlwm_interface;
    if (!netif) {
        return NULL;
    }
    if (!netif->init(this)) {
        netif->release();
        return NULL;
    }
    return netif;
}

bool itlwm::createMediumTables(const IONetworkMedium **primary)
{
    IONetworkMedium    *medium;
    
    OSDictionary *mediumDict = OSDictionary::withCapacity(1);
    if (mediumDict == NULL) {
        XYLog("Cannot allocate OSDictionary\n");
        return false;
    }
    
    medium = IONetworkMedium::medium(kIOMediumEthernetAuto, 480 * 1000000);
    IONetworkMedium::addMedium(mediumDict, medium);
    medium->release();
    if (primary) {
        *primary = medium;
    }
    
    bool result = publishMediumDictionary(mediumDict);
    if (!result) {
        XYLog("Cannot publish medium dictionary!\n");
    }

    mediumDict->release();
    return result;
}

static char * trim(const char *strIn, char *strOut)
{
    size_t i, j ;
    i = 0;
    j = strlen(strIn) - 1;
    if (j < 0) {
        memset(strOut, 0, 1);
        return strOut;
    }
    while(strIn[i] == ' ')
        ++i;

    while(strIn[j] == ' ')
        --j;
    
    if (j - i + 1 < 0) {
        memset(strOut, 0, 1);
        return strOut;
    }
    strncpy(strOut,  strIn + i, j - i + 1);
    strOut[j - i + 1] = '\0';
    return strOut;
}

static bool wifiConfigIterateCallback(void * refcon, const OSSymbol * key, OSObject * object)
{
    itlwm *that = (itlwm *)refcon;
    OSDictionary *wifiEntry = OSDynamicCast(OSDictionary, object);
    if (wifiEntry == NULL) {
        return false;
    }
    OSString *ssidObj = OSDynamicCast(OSString, wifiEntry->getObject("ssid"));
    OSString *pwdObj = OSDynamicCast(OSString, wifiEntry->getObject("password"));
    if (ssidObj == NULL || pwdObj == NULL || ssidObj->isEqualTo("")) {
        return false;
    }
    char ssid[255];
    char password[255];
    trim(ssidObj->getCStringNoCopy(), (char *)&ssid);
    trim(pwdObj->getCStringNoCopy(), (char *)&password);
    
    XYLog("%s [%s] [%s]\n", __FUNCTION__, ssid, password);
    that->joinSSID(ssid, password);
    return false;
}

ieee80211_wpaparams wpa;
ieee80211_wpapsk psk;
ieee80211_nwkey nwkey;
ieee80211_join join;

void itlwm::joinSSID(const char *ssid_name, const char *ssid_pwd)
{
    struct ieee80211com *ic = &com.sc_ic;
    
    if (strlen(ssid_pwd) == 0) {
        memset(&nwkey, 0, sizeof(ieee80211_nwkey));
        nwkey.i_wepon = IEEE80211_NWKEY_OPEN;
        nwkey.i_defkid = 0;
        memcpy(join.i_nwid, ssid_name, strlen(ssid_name));
        join.i_len = strlen(ssid_name);
        join.i_flags = IEEE80211_JOIN_NWKEY;
    } else {
        memset(&wpa, 0, sizeof(ieee80211_wpaparams));
        wpa.i_enabled = 1;
        wpa.i_ciphers = IEEE80211_WPA_CIPHER_CCMP | IEEE80211_WPA_CIPHER_TKIP;
        wpa.i_groupcipher = IEEE80211_WPA_CIPHER_CCMP | IEEE80211_WPA_CIPHER_TKIP;
        wpa.i_protos = IEEE80211_WPA_PROTO_WPA1 | IEEE80211_WPA_PROTO_WPA2;
        wpa.i_akms = IEEE80211_WPA_AKM_PSK | IEEE80211_WPA_AKM_8021X | IEEE80211_WPA_AKM_SHA256_PSK | IEEE80211_WPA_AKM_SHA256_8021X;
        memcpy(wpa.i_name, "zxy", strlen("zxy"));
        memset(&psk, 0, sizeof(ieee80211_wpapsk));
        memcpy(psk.i_name, "zxy", strlen("zxy"));
        psk.i_enabled = 1;
        pbkdf2_sha1(ssid_pwd, (const uint8_t*)ssid_name, strlen(ssid_name),
                    4096, psk.i_psk , 32);
        memset(&nwkey, 0, sizeof(ieee80211_nwkey));
        nwkey.i_wepon = 0;
        nwkey.i_defkid = 0;
        memset(&join, 0, sizeof(ieee80211_join));
        join.i_wpaparams = wpa;
        join.i_wpapsk = psk;
        join.i_flags = IEEE80211_JOIN_WPAPSK | IEEE80211_JOIN_ANY | IEEE80211_JOIN_WPA | IEEE80211_JOIN_8021X;
        join.i_nwkey = nwkey;
        join.i_len = strlen(ssid_name);
        memcpy(join.i_nwid, ssid_name, join.i_len);
    }
    if (ieee80211_add_ess(ic, &join) == 0)
        ic->ic_flags |= IEEE80211_F_AUTO_JOIN;
}

bool itlwm::start(IOService *provider)
{
    if (!super::start(provider)) {
        return false;
    }
    pciNub = OSDynamicCast(IOPCIDevice, provider);
    if (!pciNub) {
        return false;
    }
    pciNub->setBusMasterEnable(true);
    pciNub->setIOEnable(true);
    pciNub->setMemoryEnable(true);
    pciNub->configWrite8(0x41, 0);
    if (pciNub->requestPowerDomainState(kIOPMPowerOn,
                                        (IOPowerConnection *) getParentEntry(gIOPowerPlane), IOPMLowestState) != IOPMNoErr) {
        return false;
    }
    if (initPCIPowerManagment(pciNub) == false) {
        return false;
    }
    _fWorkloop = getWorkLoop();
    _fCommandGate = IOCommandGate::commandGate(this, (IOCommandGate::Action)tsleepHandler);
    if (_fCommandGate == 0) {
        XYLog("No command gate!!\n");
        releaseAll();
        return false;
    }
    _fWorkloop->addEventSource(_fCommandGate);
    const IONetworkMedium *primaryMedium;
    if (!createMediumTables(&primaryMedium) ||
        !setCurrentMedium(primaryMedium) || !setSelectedMedium(primaryMedium)) {
        releaseAll();
        return false;
    }
    pci.workloop = _fWorkloop;
    pci.pa_tag = pciNub;
    if (!iwm_attach(&com, &pci)) {
        releaseAll();
        return false;
    }
    if (!attachInterface((IONetworkInterface **)&fNetIf, true)) {
        XYLog("attach to interface fail\n");
        releaseAll();
        return false;
    }
    watchdogTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &itlwm::watchdogAction));
    if (!watchdogTimer) {
        XYLog("init watchdog fail\n");
        releaseAll();
        return false;
    }
    _fWorkloop->addEventSource(watchdogTimer);
    setLinkStatus(kIONetworkLinkValid);
    OSDictionary *wifiDict = OSDynamicCast(OSDictionary, getProperty("WiFiConfig"));
    if (wifiDict) {
        wifiDict->iterateObjects(this, wifiConfigIterateCallback);
    }
    return true;
}

void itlwm::watchdogAction(IOTimerEventSource *timer)
{
    iwm_watchdog(&com.sc_ic.ic_ac.ac_if);
    watchdogTimer->setTimeoutMS(1000);
}

const OSString * itlwm::newVendorString() const
{
    return OSString::withCString("Apple");
}

const OSString * itlwm::newModelString() const
{
    return OSString::withCString("Intel wireless card");
}

bool itlwm::initPCIPowerManagment(IOPCIDevice *provider)
{
    UInt16 reg16;

    reg16 = provider->configRead16(kIOPCIConfigCommand);

    reg16 |= ( kIOPCICommandBusMaster       |
               kIOPCICommandMemorySpace     |
               kIOPCICommandMemWrInvalidate );

    reg16 &= ~kIOPCICommandIOSpace;  // disable I/O space

    provider->configWrite16( kIOPCIConfigCommand, reg16 );
    provider->findPCICapability(kIOPCIPowerManagementCapability,
                                &pmPCICapPtr);
    if (pmPCICapPtr) {
        UInt16 pciPMCReg = provider->configRead32( pmPCICapPtr ) >> 16;
        if (pciPMCReg & kPCIPMCPMESupportFromD3Cold) {
            magicPacketSupported = true;
        }
        provider->configWrite16((pmPCICapPtr + 4), 0x8000 );
        IOSleep(10);
    }
    return true;
}

bool itlwm::createWorkLoop()
{
    _fWorkloop = IOWorkLoop::workLoop();
    return _fWorkloop != 0;
}

IOWorkLoop *itlwm::getWorkLoop() const
{
    return _fWorkloop;
}

IOReturn itlwm::selectMedium(const IONetworkMedium *medium) {
    setSelectedMedium(medium);
    return kIOReturnSuccess;
}

void itlwm::stop(IOService *provider)
{
    XYLog("%s\n", __FUNCTION__);
    struct ifnet *ifp = &com.sc_ic.ic_ac.ac_if;
    
    super::stop(provider);
    setLinkStatus(kIONetworkLinkValid);
    iwm_stop(ifp);
    ieee80211_ifdetach(ifp);
    detachInterface(fNetIf);
    OSSafeReleaseNULL(fNetIf);
    ifp->iface = NULL;
    taskq_destroy(systq);
    taskq_destroy(com.sc_nswq);
    releaseAll();
}

void itlwm::releaseAll()
{
    pci_intr_handle *intrHandler = com.ih;
    
    if (com.sc_calib_to) {
        timeout_del(&com.sc_calib_to);
        timeout_free(&com.sc_calib_to);
    }
    if (com.sc_led_blink_to) {
        timeout_del(&com.sc_led_blink_to);
        timeout_free(&com.sc_led_blink_to);
    }
    if (_fWorkloop) {
        if (intrHandler) {
            if (intrHandler->intr) {
                intrHandler->intr->disable();
                intrHandler->workloop->removeEventSource(intrHandler->intr);
                intrHandler->intr->release();
            }
            intrHandler->intr = NULL;
            intrHandler->workloop = NULL;
            intrHandler->arg = NULL;
            intrHandler->dev = NULL;
            intrHandler->func = NULL;
            intrHandler->release();
            intrHandler = NULL;
        }
        if (_fCommandGate) {
            if (lastSleepChan) {
                wakeupOn(lastSleepChan);
            }
            _fCommandGate->disable();
            _fWorkloop->removeEventSource(_fCommandGate);
            _fCommandGate->release();
            _fCommandGate = NULL;
        }
        if (watchdogTimer) {
            watchdogTimer->cancelTimeout();
            _fWorkloop->removeEventSource(watchdogTimer);
            watchdogTimer->release();
            watchdogTimer = NULL;
        }
        _fWorkloop->release();
        _fWorkloop = NULL;
    }
    unregistPM();
}

void itlwm::free()
{
    XYLog("%s\n", __FUNCTION__);
    if (fwLoadLock) {
        IOLockFree(fwLoadLock);
        fwLoadLock = NULL;
    }
    super::free();
}

IOReturn itlwm::enable(IONetworkInterface *netif)
{
    XYLog("%s\n", __FUNCTION__);
    struct ifnet *ifp = &com.sc_ic.ic_ac.ac_if;
    super::enable(netif);
    ifp->if_flags |= IFF_UP;
    _fCommandGate->enable();
    iwm_activate(&com, DVACT_WAKEUP);
    setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, getCurrentMedium());
    watchdogTimer->setTimeoutMS(1000);
    watchdogTimer->enable();
    return kIOReturnSuccess;
}

IOReturn itlwm::disable(IONetworkInterface *netif)
{
    XYLog("%s\n", __FUNCTION__);
    super::disable(netif);
    watchdogTimer->cancelTimeout();
    watchdogTimer->disable();
    iwm_activate(&com, DVACT_QUIESCE);
    return kIOReturnSuccess;
}

IOReturn itlwm::getHardwareAddress(IOEthernetAddress *addrP) {
    if (IEEE80211_ADDR_EQ(etheranyaddr, com.sc_ic.ic_myaddr)) {
        return kIOReturnError;
    } else {
        IEEE80211_ADDR_COPY(addrP, com.sc_ic.ic_myaddr);
        return kIOReturnSuccess;
    }
}

UInt32 itlwm::outputPacket(mbuf_t m, void *param)
{
//    XYLog("%s\n", __FUNCTION__);
    ifnet *ifp = &com.sc_ic.ic_ac.ac_if;
    
    if (com.sc_ic.ic_state != IEEE80211_S_RUN || ifp == NULL || ifp->if_snd == NULL) {
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (m == NULL) {
        XYLog("%s m==NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        return kIOReturnOutputDropped;
    }
    if (!(mbuf_flags(m) & MBUF_PKTHDR) ){
        XYLog("%s pkthdr is NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        return kIOReturnOutputDropped;
    }
    if (mbuf_type(m) == MBUF_TYPE_FREE) {
        XYLog("%s mbuf is FREE!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        return kIOReturnOutputDropped;
    }
    ifp->if_snd->lockEnqueue(m);
    (*ifp->if_start)(ifp);
    
    return kIOReturnOutputSuccess;
}

//UInt32 itlwm::getFeatures() const
//{
//    UInt32 features = (kIONetworkFeatureMultiPages | kIONetworkFeatureHardwareVlan);
//    features |= kIONetworkFeatureTSOIPv4;
//    features |= kIONetworkFeatureTSOIPv6;
//    return features;
//}

IOReturn itlwm::setPromiscuousMode(IOEnetPromiscuousMode mode) {
    return kIOReturnSuccess;
}

IOReturn itlwm::setMulticastMode(IOEnetMulticastMode mode) {
    return kIOReturnSuccess;
}

IOReturn itlwm::setMulticastList(IOEthernetAddress* addr, UInt32 len) {
    return kIOReturnSuccess;
}

//IOReturn itlwm::getPacketFilters(const OSSymbol *group, UInt32 *filters) const {
//    IOReturn    rtn = kIOReturnSuccess;
//    if (group == gIOEthernetWakeOnLANFilterGroup && magicPacketSupported) {
//        *filters = kIOEthernetWakeOnMagicPacket;
//    } else if (group == gIONetworkFilterGroup) {
//        *filters = kIOPacketFilterUnicast | kIOPacketFilterBroadcast
//        | kIOPacketFilterPromiscuous | kIOPacketFilterMulticast
//        | kIOPacketFilterMulticastAll;
//    } else {
//        rtn = IOEthernetController::getPacketFilters(group, filters);
//    }
//    return rtn;
//}

void itlwm::wakeupOn(void *ident)
{
//    XYLog("%s\n", __FUNCTION__);
    if (_fCommandGate == 0)
        return;
    else
        _fCommandGate->commandWakeup(ident);
}

int itlwm::tsleep_nsec(void *ident, int priority, const char *wmesg, int timo)
{
//    XYLog("%s %s\n", __FUNCTION__, wmesg);
    IOReturn ret;
    if (_fCommandGate == 0) {
        IOSleep(timo);
        return 0;
    }
    lastSleepChan = ident;
    if (timo == 0) {
        ret = _fCommandGate->runCommand(ident);
    } else {
        ret = _fCommandGate->runCommand(ident, &timo);
    }
    if (ret == kIOReturnSuccess)
        return 0;
    else
        return 1;
}

IOReturn itlwm::tsleepHandler(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3)
{
    itlwm* dev = OSDynamicCast(itlwm, owner);
    if (dev == 0)
        return kIOReturnError;
    
    if (arg1 == 0) {
        if (_fCommandGate->commandSleep(arg0, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    } else {
        AbsoluteTime deadline;
        clock_interval_to_deadline((*(int*)arg1), kNanosecondScale, reinterpret_cast<uint64_t*> (&deadline));
        if (_fCommandGate->commandSleep(arg0, deadline, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    }
}

IOReturn itlwm::getMaxPacketSize(UInt32 *maxSize) const {
    *maxSize = ETHERNET_MTU + 18;
    return kIOReturnSuccess;
}
