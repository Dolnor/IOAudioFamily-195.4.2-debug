/*
 * Copyright (c) 1998-2010 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include "IOAudioDebug.h"
#include "IOAudioDevice.h"
#include "IOAudioEngine.h"
#include "IOAudioPort.h"
#include "IOAudioTypes.h"
#include "IOAudioDefines.h"
#include "IOAudioLevelControl.h"
#include "IOAudioToggleControl.h"
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOKitKeys.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSSet.h>
#include <libkern/c++/OSCollectionIterator.h>

#define NUM_POWER_STATES	2

class IOAudioTimerEvent : public OSObject
{
    friend class IOAudioDevice;

    OSDeclareDefaultStructors(IOAudioTimerEvent)

protected:
    OSObject *	target;
    IOAudioDevice::TimerEvent event;
    AbsoluteTime interval;
};

OSDefineMetaClassAndStructors(IOAudioTimerEvent, OSObject)

class IOAudioEngineEntry : public OSObject
{
    friend class IOAudioDevice;

    OSDeclareDefaultStructors(IOAudioEngineEntry);

protected:
    IOAudioEngine *audioEngine;
    bool shouldStopAudioEngine;
};

OSDefineMetaClassAndStructors(IOAudioEngineEntry, OSObject)

#define super IOService
OSDefineMetaClassAndStructors(IOAudioDevice, IOService)
OSMetaClassDefineReservedUsed(IOAudioDevice, 0);
OSMetaClassDefineReservedUsed(IOAudioDevice, 1);
OSMetaClassDefineReservedUsed(IOAudioDevice, 2);
OSMetaClassDefineReservedUsed(IOAudioDevice, 3);
OSMetaClassDefineReservedUsed(IOAudioDevice, 4);
OSMetaClassDefineReservedUsed(IOAudioDevice, 5);

OSMetaClassDefineReservedUnused(IOAudioDevice, 6);
OSMetaClassDefineReservedUnused(IOAudioDevice, 7);
OSMetaClassDefineReservedUnused(IOAudioDevice, 8);
OSMetaClassDefineReservedUnused(IOAudioDevice, 9);
OSMetaClassDefineReservedUnused(IOAudioDevice, 10);
OSMetaClassDefineReservedUnused(IOAudioDevice, 11);
OSMetaClassDefineReservedUnused(IOAudioDevice, 12);
OSMetaClassDefineReservedUnused(IOAudioDevice, 13);
OSMetaClassDefineReservedUnused(IOAudioDevice, 14);
OSMetaClassDefineReservedUnused(IOAudioDevice, 15);
OSMetaClassDefineReservedUnused(IOAudioDevice, 16);
OSMetaClassDefineReservedUnused(IOAudioDevice, 17);
OSMetaClassDefineReservedUnused(IOAudioDevice, 18);
OSMetaClassDefineReservedUnused(IOAudioDevice, 19);
OSMetaClassDefineReservedUnused(IOAudioDevice, 20);
OSMetaClassDefineReservedUnused(IOAudioDevice, 21);
OSMetaClassDefineReservedUnused(IOAudioDevice, 22);
OSMetaClassDefineReservedUnused(IOAudioDevice, 23);
OSMetaClassDefineReservedUnused(IOAudioDevice, 24);
OSMetaClassDefineReservedUnused(IOAudioDevice, 25);
OSMetaClassDefineReservedUnused(IOAudioDevice, 26);
OSMetaClassDefineReservedUnused(IOAudioDevice, 27);
OSMetaClassDefineReservedUnused(IOAudioDevice, 28);
OSMetaClassDefineReservedUnused(IOAudioDevice, 29);
OSMetaClassDefineReservedUnused(IOAudioDevice, 30);
OSMetaClassDefineReservedUnused(IOAudioDevice, 31);

// New code added here
void IOAudioDevice::setDeviceModelName(const char *modelName)
{
    deviceDbgLog("+ IOAudioDevice[%p]::setDeviceModelName(%p)\n", this, modelName);

    if (modelName) {
        setProperty(kIOAudioDeviceModelIDKey, modelName);
    }
    deviceDbgLog("- IOAudioDevice[%p]::setDeviceModelName(%p)\n", this, modelName);
}

void IOAudioDevice::setDeviceTransportType(const UInt32 transportType)
{
    if (transportType) {
        setProperty(kIOAudioDeviceTransportTypeKey, transportType, 32);
    }
}

// This needs to be overridden by driver if it wants to know about power manager changes.
// If overridden, be sure to still call super::setAggressiveness() so we can call our parent.
IOReturn IOAudioDevice::setAggressiveness(unsigned long type, unsigned long newLevel)
{
	return super::setAggressiveness(type, newLevel);
}

// This was modified for <rdar://problem/3942297> 
void IOAudioDevice::setIdleAudioSleepTime(unsigned long long sleepDelay)
{
	assert(reserved);
	
	deviceDbgLog("+ IOAudioDevice[%p]::setIdleAudioSleepTime: sleepDelay = %lx%lx\n", this, (long unsigned int)(sleepDelay >> 32), (long unsigned int)sleepDelay);
	
	if ( reserved->idleTimer ) {
		reserved->idleTimer->cancelTimeout();
	}
	
	if (reserved->idleSleepDelayTime != sleepDelay) { 	// <rdar://problem/6601320> 
		reserved->idleSleepDelayTime = sleepDelay;
	}
		
	if ( kNoIdleAudioPowerDown != sleepDelay ) {
		scheduleIdleAudioSleep();
	}
	deviceDbgLog("- IOAudioDevice[%p]::setIdleAudioSleepTime: sleepDelay = %lx%lx\n", this, (long unsigned int)(sleepDelay >> 32), (long unsigned int)sleepDelay);
}

// Set up a timer to power down the hardware if we haven't used it in a while.
//	<rdar://8121989>	Restructured for single point of entry and single point of exit so that 
//	the indentifier post processing tool can properly insert scope when post processing a log file
//	obtained via fwkpfv.

void IOAudioDevice::scheduleIdleAudioSleep(void)
{
    AbsoluteTime				fireTime;
    UInt64						nanos;
	bool						exit = false;

	assert(reserved);

	deviceDbgLog("+ IOAudioDevice[%p]::scheduleIdleAudioSleep: idleSleepDelayTime = %lx%lx\n", this, (long unsigned int)(reserved->idleSleepDelayTime >> 32), (long unsigned int)reserved->idleSleepDelayTime);
	if ( 0 == reserved->idleSleepDelayTime )
	{
		// For backwards compatibility, or drivers that don't care, tell them about idle right away.
		initiatePowerStateChange ();
	}
	else
	{
		if ( !reserved->idleTimer && ( kNoIdleAudioPowerDown != reserved->idleSleepDelayTime ) )
		{
			reserved->idleTimer = IOTimerEventSource::timerEventSource ( this, idleAudioSleepHandlerTimer );
			if ( !reserved->idleTimer )
			{
				exit = true;
			}
			else
			{
				workLoop->addEventSource ( reserved->idleTimer );
			}
		}
	
		if ( !exit && ( kNoIdleAudioPowerDown != reserved->idleSleepDelayTime ) )
		{
			// If the driver wants to know about idle sleep after a specific amount of time, then set the timer to tell them at that time.
			// If idleSleepDelayTime == 0xffffffff then don't ever tell the driver about going idle
            clock_get_uptime ( (uint64_t *)&fireTime );
            absolutetime_to_nanoseconds ( *((uint64_t *)&fireTime), &nanos );
			nanos += reserved->idleSleepDelayTime;
			nanoseconds_to_absolutetime ( nanos, (uint64_t *)&fireTime );
			reserved->idleTimer->wakeAtTime ( fireTime );		// will call idleAudioSleepHandlerTimer
		}
	}

	deviceDbgLog("- IOAudioDevice[%p]::scheduleIdleAudioSleep: idleSleepDelayTime = %lx%lx\n", this, (long unsigned int)(reserved->idleSleepDelayTime >> 32), (long unsigned int)reserved->idleSleepDelayTime);
	return;
}

void IOAudioDevice::idleAudioSleepHandlerTimer(OSObject *owner, IOTimerEventSource *sender)
{
	IOAudioDevice *				audioDevice;

	audioDevice = OSDynamicCast(IOAudioDevice, owner);
	assert(audioDevice);

	deviceDbgLog("+ IOAudioDevice[%p]idleAudioSleepHandlerTimer: pendingPowerState = %d, idleSleepDelayTime = %lx%lx\n", audioDevice, audioDevice->pendingPowerState, (long unsigned int)(audioDevice->reserved->idleSleepDelayTime >> 32), (long unsigned int)audioDevice->reserved->idleSleepDelayTime);
	if (audioDevice->reserved->idleSleepDelayTime != kNoIdleAudioPowerDown &&
		audioDevice->getPendingPowerState () == kIOAudioDeviceIdle) {
		// If we're still idle, tell the device to go idle now that the requested amount of time has elapsed.
		audioDevice->initiatePowerStateChange();
	}

	deviceDbgLog("- IOAudioDevice[%p]idleAudioSleepHandlerTimer: pendingPowerState = %d, idleSleepDelayTime = %lx%lx\n", audioDevice, audioDevice->pendingPowerState, (long unsigned int)(audioDevice->reserved->idleSleepDelayTime >> 32), (long unsigned int)audioDevice->reserved->idleSleepDelayTime);
	return;
}

void IOAudioDevice::setConfigurationApplicationBundle(const char *bundleID)
{
    deviceDbgLog("+ IOAudioDevice[%p]::setConfigurationApplicationBundle(%p)\n", this, bundleID);

    if (bundleID) {
        setProperty(kIOAudioDeviceConfigurationAppKey, bundleID);
    }
    deviceDbgLog("- IOAudioDevice[%p]::setConfigurationApplicationBundle(%p)\n", this, bundleID);
}

// OSMetaClassDefineReservedUsed(IOAudioDevice, 4);
void IOAudioDevice::setDeviceCanBeDefault(UInt32 defaultsFlags)
{
	setProperty(kIOAudioDeviceCanBeDefaults, defaultsFlags, sizeof(UInt32) * 8);
}

//	<rdar://8121989>	Restructured for single point of entry and single point of exit so that 
//	the indentifier post processing tool can properly insert scope when post processing a log file
//	obtained via fwkpfv.

bool IOAudioDevice::init(OSDictionary *properties)
{
	bool			result = false;
	
    deviceDbgLog("+ IOAudioDevice[%p]::init(%p)\n", this, properties);

	if ( super::init ( properties ) )
	{
		reserved = (ExpansionData *)IOMalloc (sizeof(struct ExpansionData));
		if ( 0 != reserved )
		{
			reserved->idleSleepDelayTime = 0;
			reserved->idleTimer = NULL;
			
			audioEngines = OSArray::withCapacity ( 2 );
			if ( 0 != audioEngines )
			{
				audioPorts = OSSet::withCapacity ( 1 );
				if ( 0 != audioPorts )
				{
					workLoop = IOWorkLoop::workLoop ();
					if ( 0 != workLoop )
					{
						familyManagePower = true;
						asyncPowerStateChangeInProgress = false;

						currentPowerState = kIOAudioDeviceIdle;
						pendingPowerState = kIOAudioDeviceIdle;

						numRunningAudioEngines = 0;
						duringStartup = true;
						result = true;
					}
				}
			}
		}
	}
    
    deviceDbgLog("- IOAudioDevice[%p]::init(%p) returns %d\n", this, properties, result);
    return result;
}

void IOAudioDevice::free()
{
    deviceDbgLog("+ IOAudioDevice[%p]::free()\n", this);

    if (audioEngines) {
        deactivateAllAudioEngines();
        audioEngines->release();
        audioEngines = 0;
    }
	
	deviceDbgLog("  did deactiveateAllAudioEngines ()\n" );
	
    if (audioPorts) {
        detachAllAudioPorts();
        audioPorts->release();
        audioPorts = 0;
    }
    
	deviceDbgLog("  did detachAllAudioPorts ()\n" );
	
    if (timerEvents) {
        timerEvents->release();
        timerEvents = 0;
    }

	deviceDbgLog("  did timerEvents->release ()\n" );
	
    if (timerEventSource) {
        if (workLoop) {
			timerEventSource->cancelTimeout();					// <rdar://problem/7493627,8426296>
            workLoop->removeEventSource(timerEventSource);
        }
        
        timerEventSource->release();
        timerEventSource = NULL;
    }

	deviceDbgLog("  did workLoop->removeEventSource ( timerEventSource )\n" );
	
	if (reserved->idleTimer) {
		if (workLoop) {
			reserved->idleTimer->cancelTimeout();			// <rdar://problem/7493627,8426296>
			workLoop->removeEventSource(reserved->idleTimer);
		}

		reserved->idleTimer->release();
		reserved->idleTimer = NULL;
	}

	deviceDbgLog("  did workLoop->removeEventSource ( reserved->idleTimer )\n" );
	
    if (commandGate) {
        if (workLoop) {
            workLoop->removeEventSource(commandGate);
        }
        
        commandGate->release();
        commandGate = NULL;
    }

	deviceDbgLog("  did workLoop->removeEventSource ( commandGate )\n" );
	
    if (workLoop) {
        workLoop->release();
        workLoop = NULL;
    }

	deviceDbgLog("  did workLoop->release ()\n" );
	
	if (reserved) {
		IOFree (reserved, sizeof(struct ExpansionData));
	}
    
	deviceDbgLog("  did IOFree ()\n" );
	
    super::free();

    deviceDbgLog("- IOAudioDevice[%p]::free()\n", this);
}

bool IOAudioDevice::initHardware(IOService *provider)
{
    deviceDbgLog("+-IOAudioDevice[%p]::initHardware(%p)\n", this, provider);

    return true;
}

//	<rdar://8121989>	Restructured for single point of entry and single point of exit so that 
//	the indentifier post processing tool can properly insert scope when post processing a log file
//	obtained via fwkpfv.

bool IOAudioDevice::start(IOService *provider)
{
	bool			result = false;
	
    static IOPMPowerState powerStates[2] = {
        {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, IOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
    };
    
    deviceDbgLog("+ IOAudioDevice[%p]::start(%p)\n", this, provider);
	
	if ( super::start ( provider ) )
	{
		if ( 0 != provider->getProperty("preserveIODeviceTree\n") )		// <rdar://3206968>
		{
			provider->callPlatformFunction("mac-io-publishChildren\n", 0, (void*)this, (void*)0, (void*)0, (void*)0);
		}

		assert(workLoop);
		
		commandGate = IOCommandGate::commandGate(this);
		if ( 0 != commandGate)
		{
			workLoop->addEventSource(commandGate);

			setDeviceCanBeDefault (kIOAudioDeviceCanBeDefaultInput | kIOAudioDeviceCanBeDefaultOutput | kIOAudioDeviceCanBeSystemOutput);

			if ( initHardware ( provider ) )
			{
				if ( familyManagePower ) {
					PMinit ();
					provider->joinPMtree ( this );
					
					if ( NULL != pm_vars ) {
						//	duringStartup = true;
						registerPowerDriver ( this, powerStates, NUM_POWER_STATES );
						changePowerStateTo ( 1 );
						//	duringStartup = false;
					}
				}

				registerService();
				result = true;
			}
		}
	}
	
    deviceDbgLog("- IOAudioDevice[%p]::start(%p)\n", this, provider);
	return result;
}

void IOAudioDevice::stop(IOService *provider)
{    
    deviceDbgLog("+ IOAudioDevice[%p]::stop(%p)\n", this, provider);
    
    removeAllTimerEvents();					// <rdar://problem/7493627,8426296>

    if (timerEventSource) {
        if (workLoop) {
			timerEventSource->cancelTimeout();					// <rdar://problem/7493627,8426296>
            workLoop->removeEventSource(timerEventSource);
        }
        
        timerEventSource->release();
        timerEventSource = NULL;
    }

    if (reserved->idleTimer) {
        if (workLoop) {
			reserved->idleTimer->cancelTimeout();				// <rdar://problem/7493627,8426296>
            workLoop->removeEventSource(reserved->idleTimer);
        }

        reserved->idleTimer->release();
        reserved->idleTimer = NULL;
    }

    deactivateAllAudioEngines();
    detachAllAudioPorts();

    if (familyManagePower) {
		if (pm_vars != NULL) {
			PMstop();
		}
    }
    
    if (commandGate) {
        if (workLoop) {
            workLoop->removeEventSource(commandGate);
        }
        
        commandGate->release();
        commandGate = NULL;
    }

    super::stop(provider);
    deviceDbgLog("- IOAudioDevice[%p]::stop(%p)\n", this, provider);
}

bool IOAudioDevice::willTerminate(IOService *provider, IOOptionBits options)
{
	bool			result = false;
	
    deviceDbgLog("+ IOAudioDevice[%p]::willTerminate(%p, %lx)\n", this, provider, (long unsigned int)options);

    OSCollectionIterator *engineIterator;
    
    engineIterator = OSCollectionIterator::withCollection(audioEngines);
    if (engineIterator) {
        IOAudioEngine *audioEngine;
        
        while ( (audioEngine = OSDynamicCast(IOAudioEngine, engineIterator->getNextObject())) ) {
            audioEngine->setState(kIOAudioEngineStopped);
        }
        engineIterator->release();
    }

	result = super::willTerminate(provider, options);
    deviceDbgLog("- IOAudioDevice[%p]::willTerminate(%p, %lx) returns %d\n", this, provider, (long unsigned int)options, result );
	return result;
}

void IOAudioDevice::setFamilyManagePower(bool manage)
{
    familyManagePower = manage;
}

IOReturn IOAudioDevice::setPowerState(unsigned long powerStateOrdinal, IOService *device)
{
    IOReturn result = IOPMAckImplied;
    
    deviceDbgLog("+ IOAudioDevice[%p]::setPowerState(%lu, %p)\n", this, powerStateOrdinal, device);
    if (!duringStartup) 
	{
        if (powerStateOrdinal >= NUM_POWER_STATES) 
		{
            result = IOPMNoSuchState;
        } else 
		{
			if (workLoop) {																					//	<rdar://8508064>
				workLoop->runAction(_setPowerStateAction, this, (void *)powerStateOrdinal, (void *)device);	//	<rdar://8508064>
			}
        }
    }
	duringStartup = false;
    deviceDbgLog("- IOAudioDevice[%p]::setPowerState(%lu, %p) returns 0x%lX\n", this, powerStateOrdinal, device, (long unsigned int)result );
	return result;
}

// <rdar://8508064>
IOReturn IOAudioDevice::_setPowerStateAction(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    IOReturn result = IOPMAckImplied;
    
    if (target) {
        IOAudioDevice *audioDevice = OSDynamicCast(IOAudioDevice, target);
        if (audioDevice) {
            IOCommandGate *cg;
            
            cg = audioDevice->getCommandGate();
            
            if (cg) {
                result = cg->runAction(setPowerStateAction, arg0, arg1, arg2, arg3);
            }
        }
    }
    
    return result;
}

IOReturn IOAudioDevice::setPowerStateAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    IOReturn result = IOPMAckImplied;
    
    if (owner) {
        IOAudioDevice *audioDevice = OSDynamicCast(IOAudioDevice, owner);
        
        if (audioDevice) {
            result = audioDevice->protectedSetPowerState((unsigned long)arg1, (IOService *)arg2);
        }
    }
    
    return result;
}

IOReturn IOAudioDevice::protectedSetPowerState(unsigned long powerStateOrdinal, IOService *device)
{
    IOReturn result = IOPMAckImplied;

    deviceDbgLog("+ IOAudioDevice[%p]::protectedSetPowerState(%lu, %p)\n", this, powerStateOrdinal, device);
    
    if (asyncPowerStateChangeInProgress) {
        waitForPendingPowerStateChange();
    }
    
    if (powerStateOrdinal == 0) {	// Sleep
        if (getPowerState() != kIOAudioDeviceSleep) {
            pendingPowerState = kIOAudioDeviceSleep;

            // Stop all audio engines
            if (audioEngines && (numRunningAudioEngines > 0)) {
                OSCollectionIterator *audioEngineIterator;
                
                audioEngineIterator = OSCollectionIterator::withCollection(audioEngines);
                
                if (audioEngineIterator) {
                    IOAudioEngine *audioEngine;
                    
                    while ( (audioEngine = (IOAudioEngine *)audioEngineIterator->getNextObject()) ) {
                        if (audioEngine->getState() == kIOAudioEngineRunning) {
                            audioEngine->pauseAudioEngine();
                        }
                    }
                    
                    audioEngineIterator->release();
                }
            }
        }
    } else if (powerStateOrdinal == 1) {	// Wake
        if (getPowerState() == kIOAudioDeviceSleep) {	// Need to change state if sleeping
            if (numRunningAudioEngines == 0) {
                pendingPowerState = kIOAudioDeviceIdle;
            } else {
                pendingPowerState = kIOAudioDeviceActive;
            }
        }
    }
    
    if (currentPowerState != pendingPowerState) {
        UInt32 microsecondsUntilComplete = 0;
        
        result = initiatePowerStateChange(&microsecondsUntilComplete);
        if (result == kIOReturnSuccess) {
            result = microsecondsUntilComplete;
        }
    }
    
    deviceDbgLog("- IOAudioDevice[%p]::protectedSetPowerState(%lu, %p) returns 0x%lX\n", this, powerStateOrdinal, device, (long unsigned int)result );
    return result;
}

void IOAudioDevice::waitForPendingPowerStateChange()
{
    deviceDbgLog("+ IOAudioDevice[%p]::waitForPendingPowerStateChange()\n", this);

    if (asyncPowerStateChangeInProgress) {
        IOCommandGate *cg;
        
        cg = getCommandGate();
        
        if (cg) {
            cg->commandSleep((void *)&asyncPowerStateChangeInProgress);
            assert(!asyncPowerStateChangeInProgress);
        } else {
            IOLog("IOAudioDevice[%p]::waitForPendingPowerStateChange() - internal error - unable to get the command gate.\n", this);
        }
    }
    deviceDbgLog("- IOAudioDevice[%p]::waitForPendingPowerStateChange()\n", this);
	return;
}

IOReturn IOAudioDevice::initiatePowerStateChange(UInt32 *microsecondsUntilComplete)
{
    IOReturn result = kIOReturnSuccess;

    deviceDbgLog("+ IOAudioDevice[%p]::initiatePowerStateChange(%p) - current = %d - pending = %d\n", this, microsecondsUntilComplete, currentPowerState, pendingPowerState);
    
    if (currentPowerState != pendingPowerState) {
        UInt32 localMicsUntilComplete, *micsUntilComplete = NULL;
        
        if (microsecondsUntilComplete != NULL) {
            micsUntilComplete = microsecondsUntilComplete;
        } else {
            micsUntilComplete = &localMicsUntilComplete;
        }
        
        *micsUntilComplete = 0;
        
        asyncPowerStateChangeInProgress = true;
        
        result = performPowerStateChange(currentPowerState, pendingPowerState, micsUntilComplete);
        
        if (result == kIOReturnSuccess) {
            if (*micsUntilComplete == 0) {
                asyncPowerStateChangeInProgress = false;
                protectedCompletePowerStateChange();
            }
        } else {
            asyncPowerStateChangeInProgress = false;
        }
    }
    
    deviceDbgLog("- IOAudioDevice[%p]::initiatePowerStateChange(%p) - current = %d - pending = %d returns 0x%lX\n", this, microsecondsUntilComplete, currentPowerState, pendingPowerState, (long unsigned int)result );
    return result;
}

IOReturn IOAudioDevice::completePowerStateChange()
{
    IOReturn result = kIOReturnError;
    IOCommandGate *cg;
    
    cg = getCommandGate();
    
    if (cg) {
        result = cg->runAction(completePowerStateChangeAction);
    }
    
    return result;
}

IOReturn IOAudioDevice::completePowerStateChangeAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    IOReturn result = kIOReturnBadArgument;
    
    if (owner) {
        IOAudioDevice *audioDevice = OSDynamicCast(IOAudioDevice, owner);
        
        if (audioDevice) {
            result = audioDevice->protectedCompletePowerStateChange();
        }
    }
    
    return result;
}

IOReturn IOAudioDevice::protectedCompletePowerStateChange()
{
    IOReturn result = kIOReturnSuccess;

    deviceDbgLog("+ IOAudioDevice[%p]::protectedCompletePowerStateChange() - current = %d - pending = %d\n", this, currentPowerState, pendingPowerState);

    if (currentPowerState != pendingPowerState) {
		IOCommandGate *cg;

		cg = getCommandGate();
        // If we're waking, we fire off the timers and resync them
        // Then restart the audio engines that were running before the sleep
        if (currentPowerState == kIOAudioDeviceSleep) {	
            clock_get_uptime((uint64_t *)&previousTimerFire);
            SUB_ABSOLUTETIME(&previousTimerFire, &minimumInterval);
            
            if (timerEvents && (timerEvents->getCount() > 0)) {
                dispatchTimerEvents(true);
            }
            
            if (audioEngines && (numRunningAudioEngines > 0)) {
                OSCollectionIterator *audioEngineIterator;
                
                audioEngineIterator = OSCollectionIterator::withCollection(audioEngines);
                
                if (audioEngineIterator) {
                    IOAudioEngine *audioEngine;
                    
                    while ( (audioEngine = (IOAudioEngine *)audioEngineIterator->getNextObject()) ) {
                        if (audioEngine->getState() == kIOAudioEnginePaused) {
                            audioEngine->resumeAudioEngine();
                        }
                    }
                        
                    audioEngineIterator->release();
                }
            }
        }
    
        if (asyncPowerStateChangeInProgress) {
            acknowledgeSetPowerState();
            asyncPowerStateChangeInProgress = false;
        
            if (cg) {
                cg->commandWakeup((void *)&asyncPowerStateChangeInProgress);
            }
        }
        
        currentPowerState = pendingPowerState;
		
		if (cg) {
			cg->commandWakeup(&currentPowerState);
		}
    }
    
    deviceDbgLog("- IOAudioDevice[%p]::protectedCompletePowerStateChange() - current = %d - pending = %d returns 0x%lX\n", this, currentPowerState, pendingPowerState, (long unsigned int)result );
    return result;
}

IOReturn IOAudioDevice::performPowerStateChange(IOAudioDevicePowerState oldPowerState,
                                                IOAudioDevicePowerState newPowerState,
                                                UInt32 *microsecondsUntilComplete)
{
    return kIOReturnSuccess;
}

IOAudioDevicePowerState IOAudioDevice::getPowerState()
{
    return currentPowerState;
}

IOAudioDevicePowerState IOAudioDevice::getPendingPowerState()
{
    return pendingPowerState;
}

void IOAudioDevice::audioEngineStarting()
{
    deviceDbgLog("+ IOAudioDevice[%p]::audioEngineStarting() - numRunningAudioEngines = %ld\n", this, (long int)( numRunningAudioEngines + 1 ) );

    numRunningAudioEngines++;
    
    if (numRunningAudioEngines == 1) {	// First audio engine starting - need to be in active state
        if (getPowerState() == kIOAudioDeviceIdle) {	// Go active
            if (asyncPowerStateChangeInProgress) {	// Sleep if there is a transition in progress
                waitForPendingPowerStateChange();
            }
            
            pendingPowerState = kIOAudioDeviceActive;
            
            initiatePowerStateChange();

            if (asyncPowerStateChangeInProgress) {	// Sleep if there is a transition in progress
                waitForPendingPowerStateChange();
            }
        } else if (getPendingPowerState () != kIOAudioDeviceSleep) {
			// Make sure that when the idle timer fires that we won't go to sleep.
            pendingPowerState = kIOAudioDeviceActive;
		}
    }
    deviceDbgLog("- IOAudioDevice[%p]::audioEngineStarting() - numRunningAudioEngines = %ld\n", this, (long int)( numRunningAudioEngines + 1 ) );
}

void IOAudioDevice::audioEngineStopped()
{
    deviceDbgLog("+ IOAudioDevice[%p]::audioEngineStopped() - numRunningAudioEngines = %ld\n", this, (long int)( numRunningAudioEngines - 1 ) );

    numRunningAudioEngines--;
    
    if (numRunningAudioEngines == 0) {	// Last audio engine stopping - need to be idle
        if (getPowerState() == kIOAudioDeviceActive) {	// Go idle
            if (asyncPowerStateChangeInProgress) {	// Sleep if there is a transition in progress
                waitForPendingPowerStateChange();
            }
            
            pendingPowerState = kIOAudioDeviceIdle;

			scheduleIdleAudioSleep();
        }
    }
    deviceDbgLog("- IOAudioDevice[%p]::audioEngineStopped() - numRunningAudioEngines = %ld\n", this, (long int)( numRunningAudioEngines - 1 ) );
}

IOWorkLoop *IOAudioDevice::getWorkLoop() const
{
    return workLoop;
}

IOCommandGate *IOAudioDevice::getCommandGate() const
{
    return commandGate;
}

void IOAudioDevice::setDeviceName(const char *deviceName)
{
    deviceDbgLog("+ IOAudioDevice[%p]::setDeviceName(%p)\n", this, deviceName);

    if (deviceName) {
        setProperty(kIOAudioDeviceNameKey, deviceName);
		if (NULL == getProperty (kIOAudioDeviceModelIDKey)) {
			int			stringLen, tempLength;
			char *		string;

			stringLen = 1;
			stringLen += strlen (deviceName) + 1;
			stringLen += strlen (getName ());
			string = (char *)IOMalloc (stringLen);
			if ( string )									// we should not panic for this
			{	
				strncpy (string, getName (), stringLen);
				tempLength = strlen (".");					//	<rdar://problem/6411827>
				strncat (string, ":", tempLength);
				tempLength = strlen (deviceName);			//	<rdar://problem/6411827>
				strncat (string, deviceName, tempLength);
				setDeviceModelName (string);
				IOFree (string, stringLen);
			}
		}
    }
    deviceDbgLog("- IOAudioDevice[%p]::setDeviceName(%p)\n", this, deviceName);
}

void IOAudioDevice::setDeviceShortName(const char *shortName)
{
    deviceDbgLog("+ IOAudioDevice[%p]::setDeviceShortName(%p)\n", this, shortName);

    if (shortName) {
        setProperty(kIOAudioDeviceShortNameKey, shortName);
    }
    deviceDbgLog("- IOAudioDevice[%p]::setDeviceShortName(%p)\n", this, shortName);
}

void IOAudioDevice::setManufacturerName(const char *manufacturerName)
{
    deviceDbgLog("+ IOAudioDevice[%p]::setManufacturerName(%p)\n", this, manufacturerName);

    if (manufacturerName) {
        setProperty(kIOAudioDeviceManufacturerNameKey, manufacturerName);
    }
    deviceDbgLog("- IOAudioDevice[%p]::setManufacturerName(%p)\n", this, manufacturerName);
}

IOReturn IOAudioDevice::activateAudioEngine(IOAudioEngine *audioEngine)
{
    return activateAudioEngine(audioEngine, true);
}

/*
class Mini9MuteControl : public IOAudioToggleControl
{
    	OSDeclareDefaultStructors(Mini9MuteControl);
    
    	IOMemoryDescriptor *ioreg_;
    	bool mute_;
    	static const IOPMPowerState kPowerStates[2];
    
    public:
    	virtual bool start(IOService *provider) {
        		IOAudioToggleControl::start(provider);
        		// init power manager
        		PMinit();
        		registerPowerDriver(this, const_cast<IOPMPowerState*>(kPowerStates), sizeof(kPowerStates) / sizeof(kPowerStates[0]));
        		provider->joinPMtree(this);
        		return true;
        	}
    	virtual void stop(IOService *provider) {
        		PMstop();
        		IOAudioToggleControl::stop(provider);
        	}
    	virtual IOReturn setPowerState(unsigned long state, IOService *) {
        		if (state != 0 && ! mute_) {
            			::IODelay(100000); // nasty wait so that all initialization would finish before adjusting mute control
            			updateMuteControl();
            		}
        		return kIOPMAckImplied;
        	}
        virtual IOReturn _setValue(OSObject *newValue) {
            if (value != newValue) {
                if (value) {
                    value->release();
                }
                value = newValue;
                value->retain();
                }
        
                return kIOReturnSuccess;
            }
        virtual IOReturn performValueChange(OSObject *newValue) {
        		OSNumber *v = OSDynamicCast(OSNumber, newValue);
        		if (v == NULL) {
            			IOLog("Mini9MuteControl: cast failure\n");
            			return kIOReturnError;
            		}
        		mute_ = v->unsigned32BitValue() != 0;
        		updateMuteControl();
        		return kIOReturnSuccess;
        	}
    	static Mini9MuteControl *create() {
        		Mini9MuteControl *control = new Mini9MuteControl;
        		control->init();
        		return control;
        	}
    	bool init() {
        		mute_ = false;
        		if (! IOAudioToggleControl::init(mute_, kIOAudioControlChannelIDAll, kIOAudioControlChannelNameAll, 0,
                                                 				kIOAudioToggleControlSubTypeMute, kIOAudioControlUsageOutput)) {
            			return false;
            		}
        		ioreg_ = NULL;
        		return true;
        	}
    	void startUpdate() {
        		IORegistryEntry *hdaDeviceEntry = IORegistryEntry::fromPath("IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/HDEF@8");
        		if (hdaDeviceEntry != NULL) {
            			IOService *service = OSDynamicCast(IOService, hdaDeviceEntry);
            			if (service != NULL && service->getDeviceMemoryCount() != 0) {
                				ioreg_ = service->getDeviceMemoryWithIndex(0);
                			}
            			hdaDeviceEntry->release();
                }
                else
                    IOLog("Mini9MuteControl: unable to locate HDEF device\n");
        		//updateMuteControl();
        	}
    	void updateMuteControl() {
        		if (ioreg_ == NULL) {
            			return;
            		}
                UInt32 cmd = 0x01470c00 | (mute_ ? 0x0 : 0x2);
            

                for (int i = 0; i<2; i++) {

                    // write the command
                    IOLog("Mini9MuteControl: pushing verb %04x\n", cmd);
                    ioreg_->writeBytes(0x60, &cmd, sizeof(cmd));
                    UInt16 status = 1;
                    ioreg_->writeBytes(0x68, &status, sizeof(status));
                    // wait for response
                    for (int i = 0; i < 1000; i++) {
                            ::IODelay(100);
                            ioreg_->readBytes(0x68, &status, sizeof(status));
                            if (status & 0x2) {
                                    goto Success;
                                }
                        }
                    // timeout
                    IOLog("Mini9MuteControl: request to change EAPD status timed out.\n");
                Success:
                    // clear Immediate Result Valid flag
                    status = 0x2;
                    ioreg_->writeBytes(0x68, &status, sizeof(status));
                    cmd = 0x01570c00 | (mute_ ? 0x0 : 0x2);
                }
                IOLog("Mini9MuteControl: done\n");
            }
};

OSDefineMetaClassAndStructors(Mini9MuteControl, IOAudioToggleControl)
const IOPMPowerState Mini9MuteControl::kPowerStates[] = {
    	{ kIOPMPowerStateVersion1 },
    	{ kIOPMPowerStateVersion1, kIOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn },
};
*/
 
//	<rdar://8121989>	Restructured for single point of entry and single point of exit so that 
//	the indentifier post processing tool can properly insert scope when post processing a log file
//	obtained via fwkpfv.

IOReturn IOAudioDevice::activateAudioEngine(IOAudioEngine *audioEngine, bool shouldStartAudioEngine)
{
	IOReturn			result = kIOReturnBadArgument;
	
	deviceDbgLog("+ IOAudioDevice[%p]::activateAudioEngine(%p, %d)\n", this, audioEngine, shouldStartAudioEngine);

	if ( audioEngine && audioEngines )
	{
       /*
        deviceDbgLog("+ IOAudioDevice[%p]:: activated AudioEngine MetaClass ClassName> %s\n", this, audioEngine->getMetaClass()->getClassName());
        
        Mini9MuteControl *mmc = NULL;
        if (::strcmp(audioEngine->getMetaClass()->getClassName(), "net_telestream_driver_TSAudioEngine") == 0) {
            mmc = Mini9MuteControl::create();
            audioEngine->addDefaultAudioControl(mmc);
        }
        else
            IOLog("Mini9MuteControl: AppleHDAEngineOutput hasn't matched !!!!\n");
        */
        if ( !audioEngine->attach ( this ) )
		{
			result = kIOReturnError;
		}
		else
		{
			if ( shouldStartAudioEngine ) 
			{
				if (!audioEngine->start ( this ) )
				{
					audioEngine->detach ( this );
					result =  kIOReturnError;
				}
				else
				{
					result =  kIOReturnSuccess;
				}
			}
			else // <rdar://8681286>
			{
				result =  kIOReturnSuccess;
			}
			
			if ( kIOReturnSuccess == result ) // <rdar://8681286>
			{
				audioEngine->deviceStartedAudioEngine = shouldStartAudioEngine;
				
				audioEngines->setObject ( audioEngine );
				audioEngine->setIndex ( audioEngines->getCount() - 1 );
				
				audioEngine->registerService ();
                /*
                if (mmc != NULL) {
                    mmc->startUpdate();
                }
                */
			}
		}
	}

	deviceDbgLog("- IOAudioDevice[%p]::activateAudioEngine(%p, %d) returns 0x%lX\n", this, audioEngine, shouldStartAudioEngine, (long unsigned int)result );
	return result;
}

//	<rdar://8121989>	Restructured for single point of entry and single point of exit so that 
//	the indentifier post processing tool can properly insert scope when post processing a log file
//	obtained via fwkpfv.

void IOAudioDevice::deactivateAllAudioEngines()
{
    OSCollectionIterator *engineIterator;
    
    deviceDbgLog("+ IOAudioDevice[%p]::deactivateAllAudioEngines()\n", this);

    if ( audioEngines )
	{
		engineIterator = OSCollectionIterator::withCollection ( audioEngines );
		if ( engineIterator )
		{
			IOAudioEngine *audioEngine;
			
			while ( (audioEngine = OSDynamicCast ( IOAudioEngine, engineIterator->getNextObject ()) ) )
			{
				audioEngine->stopAudioEngine ();
				if ( !isInactive () )
				{
					audioEngine->terminate ();
				}
			}
			engineIterator->release ();
		}

		audioEngines->flushCollection ();
    }

    deviceDbgLog("- IOAudioDevice[%p]::deactivateAllAudioEngines()\n", this);
	return;
}

IOReturn IOAudioDevice::attachAudioPort(IOAudioPort *port, IORegistryEntry *parent, IORegistryEntry *child)
{
    return kIOReturnSuccess;
}

void IOAudioDevice::detachAllAudioPorts()
{
}

void IOAudioDevice::flushAudioControls()
{
    deviceDbgLog("+ IOAudioDevice[%p]::flushAudioControls()\n", this);

    if (audioPorts) {
        OSCollectionIterator *portIterator;

        portIterator = OSCollectionIterator::withCollection(audioPorts);
        if (portIterator) {
            IOAudioPort *audioPort;

            while ( (audioPort = (IOAudioPort *)portIterator->getNextObject()) ) {
                if (OSDynamicCast(IOAudioPort, audioPort)) {
                    if (audioPort->audioControls) {
                        OSCollectionIterator *controlIterator;

                        controlIterator = OSCollectionIterator::withCollection(audioPort->audioControls);

                        if (controlIterator) {
                            IOAudioControl *audioControl;

                            while ( (audioControl = (IOAudioControl *)controlIterator->getNextObject()) ) {
                                audioControl->flushValue();
                            }
                            controlIterator->release();
                        }
                    }
                }
            }
            portIterator->release();
        }
    }
    
    // This code will flush controls attached to an IOAudioPort and a default on a audio engine
    // more than once
    // We need to update this to create a single master list of controls and use that to flush
    // each only once
    if (audioEngines) {
        OSCollectionIterator *audioEngineIterator;
        
        audioEngineIterator = OSCollectionIterator::withCollection(audioEngines);
        if (audioEngineIterator) {
            IOAudioEngine *audioEngine;
            
            while ( (audioEngine = (IOAudioEngine *)audioEngineIterator->getNextObject()) ) {
                if (audioEngine->defaultAudioControls) {
                    OSCollectionIterator *controlIterator;
                    
                    controlIterator = OSCollectionIterator::withCollection(audioEngine->defaultAudioControls);
                    if (controlIterator) {
                        IOAudioControl *audioControl;
                        
                        while ( (audioControl = (IOAudioControl *)controlIterator->getNextObject()) ) {
                            audioControl->flushValue();
                        }
                        controlIterator->release();
                    }
                }
            }
            
            audioEngineIterator->release();
        }
    }
    deviceDbgLog("- IOAudioDevice[%p]::flushAudioControls()\n", this);
}

//	<rdar://8121989>	Restructured for single point of entry and single point of exit so that 
//	the indentifier post processing tool can properly insert scope when post processing a log file
//	obtained via fwkpfv.

IOReturn IOAudioDevice::addTimerEvent(OSObject *target, TimerEvent event, AbsoluteTime interval)
{
    IOReturn			result = kIOReturnSuccess;
    IOAudioTimerEvent *	newEvent;
    
#ifdef DEBUG
    UInt64 newInt;
    absolutetime_to_nanoseconds(*((uint64_t *)&interval), &newInt);
    deviceDbgLog("+ IOAudioDevice::addTimerEvent(%p, %p, %lums)\n", target, event, (long unsigned int)(newInt/1000000));
#endif

    if ( !event )
	{
        result = kIOReturnBadArgument;
    }
	else
	{
		newEvent = new IOAudioTimerEvent;
		newEvent->target = target;
		newEvent->event = event;
		newEvent->interval = interval;

		if (!timerEvents) {
			IOWorkLoop *wl;

			timerEvents = OSDictionary::withObjects((const OSObject **)&newEvent, (const OSSymbol **)&target, 1, 1);
			
			timerEventSource = IOTimerEventSource::timerEventSource(this, timerFired);
			wl = getWorkLoop();
			if ( !timerEventSource || !wl || ( kIOReturnSuccess != wl->addEventSource ( timerEventSource ) ) )
			{
				result = kIOReturnError;
			}
			else
			{
				timerEventSource->enable ();
			}
		}
		else
		{
			timerEvents->setObject((OSSymbol *)target, newEvent);
		}
		
		if ( kIOReturnSuccess == result )
		{
			newEvent->release();
			
			assert(timerEvents);

			if (timerEvents->getCount() == 1) {
				AbsoluteTime nextTimerFire;
				
				minimumInterval = interval;

				assert(timerEventSource);

				clock_get_uptime((uint64_t *)&previousTimerFire);
				
				nextTimerFire = previousTimerFire;
				ADD_ABSOLUTETIME(&nextTimerFire, &minimumInterval);
				
				result = timerEventSource->wakeAtTime(nextTimerFire);
				
#ifdef DEBUG
				{
					UInt64 nanos;
					absolutetime_to_nanoseconds(*((uint64_t *)&minimumInterval), &nanos);
#ifdef __LP64__
					deviceDbgLog("  scheduling timer to fire in %lums - previousTimerFire = {%llu}\n", (long unsigned int) (nanos / 1000000), previousTimerFire);
#else	/* __LP64__ */
					deviceDbgLog("  scheduling timer to fire in %lums - previousTimerFire = {%ld,%lu}\n", (long unsigned int) (nanos / 1000000), previousTimerFire.hi, previousTimerFire.lo);
#endif	/* __LP64__ */
				}
#endif

				if (result != kIOReturnSuccess) {
					IOLog("IOAudioDevice::addTimerEvent() - error 0x%x setting timer wake time - timer events will be disabled.\n", result);
				}
			} else if (CMP_ABSOLUTETIME(&interval, &minimumInterval) < 0) {
				AbsoluteTime currentNextFire, desiredNextFire;
				
				clock_get_uptime((uint64_t *)&desiredNextFire);
				ADD_ABSOLUTETIME(&desiredNextFire, &interval);

				currentNextFire = previousTimerFire;
				ADD_ABSOLUTETIME(&currentNextFire, &minimumInterval);
				
				minimumInterval = interval;

				if (CMP_ABSOLUTETIME(&desiredNextFire, &currentNextFire) < 0) {
					assert(timerEventSource);
					
#ifdef DEBUG
					{
						UInt64 nanos;
						absolutetime_to_nanoseconds(*((uint64_t *)&interval), &nanos);
#ifdef __LP64__
						deviceDbgLog("  scheduling timer to fire in %lums at {%llu} - previousTimerFire = {%llu}\n", (long unsigned int) (nanos / 1000000), desiredNextFire, previousTimerFire);
#else	/* __LP64__ */
						deviceDbgLog("  scheduling timer to fire in %lums at {%ld,%lu} - previousTimerFire = {%ld,%lu}\n", (long unsigned int) (nanos / 1000000), desiredNextFire.hi, desiredNextFire.lo, previousTimerFire.hi, previousTimerFire.lo);
#endif	/* __LP64__ */		
					}
#endif

					result = timerEventSource->wakeAtTime(desiredNextFire);
					if (result != kIOReturnSuccess) {
						IOLog("IOAudioDevice::addTimerEvent() - error 0x%x setting timer wake time - timer events will be disabled.\n", result);
					}
				}
			}
		}
	}
    
#ifdef DEBUG
    deviceDbgLog("- IOAudioDevice::addTimerEvent(%p, %p, %lums) returns 0x%lX\n", target, event, (long unsigned int)(newInt/1000000), (long unsigned int)result );
#endif
    return result;
}

//	<rdar://8121989>	Restructured for single point of entry and single point of exit so that 
//	the indentifier post processing tool can properly insert scope when post processing a log file
//	obtained via fwkpfv.

void IOAudioDevice::removeTimerEvent(OSObject *target)
{
    IOAudioTimerEvent *removedTimerEvent;
    
    deviceDbgLog("+ IOAudioDevice::removeTimerEvent(%p)\n", target);
    
	if ( timerEvents )
	{
		removedTimerEvent = (IOAudioTimerEvent *)timerEvents->getObject((const OSSymbol *)target);
		if (removedTimerEvent) {
			removedTimerEvent->retain();
			timerEvents->removeObject((const OSSymbol *)target);
			if (timerEvents->getCount() == 0) {
				assert(timerEventSource);
				timerEventSource->cancelTimeout();
			} else if (CMP_ABSOLUTETIME(&removedTimerEvent->interval, &minimumInterval) <= 0) { // Need to find a new minimum interval
				OSCollectionIterator *iterator;
				IOAudioTimerEvent *timerEvent;
				AbsoluteTime nextTimerFire;
				OSSymbol *obj;

				iterator = OSCollectionIterator::withCollection(timerEvents);
				
				if (iterator) {
					obj = (OSSymbol *)iterator->getNextObject();
					timerEvent = (IOAudioTimerEvent *)timerEvents->getObject(obj);
		
					if (timerEvent) {
						minimumInterval = timerEvent->interval;
		
						while ((obj = (OSSymbol *)iterator->getNextObject()) && (timerEvent = (IOAudioTimerEvent *)timerEvents->getObject(obj))) {
							if (CMP_ABSOLUTETIME(&timerEvent->interval, &minimumInterval) < 0) {
								minimumInterval = timerEvent->interval;
							}
						}
					}
		
					iterator->release();
				}

				assert(timerEventSource);

				nextTimerFire = previousTimerFire;
				ADD_ABSOLUTETIME(&nextTimerFire, &minimumInterval);
				
#ifdef DEBUG
				{
					AbsoluteTime now, then;
					UInt64 nanos, mi;
					clock_get_uptime((uint64_t *)&now);
					then = nextTimerFire;
					absolutetime_to_nanoseconds(*((uint64_t *)&minimumInterval), &mi);
					if (CMP_ABSOLUTETIME(&then, &now)) {
						SUB_ABSOLUTETIME(&then, &now);
						absolutetime_to_nanoseconds(*((uint64_t *)&then), &nanos);
#ifdef __LP64__
						deviceDbgLog("IOAudioDevice::removeTimerEvent() - scheduling timer to fire in %lums at {%llu} - previousTimerFire = {%llu} - interval=%lums\n", (long unsigned int) (nanos / 1000000), nextTimerFire, previousTimerFire, (long unsigned int)(mi/1000000));
#else	/* __LP64__ */
						deviceDbgLog("IOAudioDevice::removeTimerEvent() - scheduling timer to fire in %lums at {%ld,%lu} - previousTimerFire = {%ld,%lu} - interval=%lums\n", (long unsigned int) (nanos / 1000000), nextTimerFire.hi, nextTimerFire.lo, previousTimerFire.hi, previousTimerFire.lo, (long unsigned int)(mi/1000000));
#endif	/* __LP64__ */
						
					
					} else {
						SUB_ABSOLUTETIME(&now, &then);
						absolutetime_to_nanoseconds(*((uint64_t *)&now), &nanos);
#ifdef __LP64__
						deviceDbgLog("IOAudioDevice::removeTimerEvent() - scheduling timer to fire in -%lums - previousTimerFire = {%llu}\n", (long unsigned int) (nanos / 1000000), previousTimerFire);
#else	/* __LP64__ */
						deviceDbgLog("IOAudioDevice::removeTimerEvent() - scheduling timer to fire in -%lums - previousTimerFire = {%ld,%lu}\n", (long unsigned int) (nanos / 1000000), previousTimerFire.hi, previousTimerFire.lo);
#endif	/* __LP64__ */
						
					}
				}
#endif

				timerEventSource->wakeAtTime(nextTimerFire);
			}

			removedTimerEvent->release();
		}
	}
    deviceDbgLog("- IOAudioDevice::removeTimerEvent(%p)\n", target);
	return;
}

void IOAudioDevice::removeAllTimerEvents()
{
    deviceDbgLog("+ IOAudioDevice[%p]::removeAllTimerEvents()\n", this);

    if (timerEventSource) {
        timerEventSource->cancelTimeout();
    }
    
    if (timerEvents) {
        timerEvents->flushCollection();
    }
    deviceDbgLog("- IOAudioDevice[%p]::removeAllTimerEvents()\n", this);
}

void IOAudioDevice::timerFired(OSObject *target, IOTimerEventSource *sender)
{
    if (target) {
        IOAudioDevice *audioDevice = OSDynamicCast(IOAudioDevice, target);
        
        if (audioDevice) {
            audioDevice->dispatchTimerEvents(false);
        }
    }
}

void IOAudioDevice::dispatchTimerEvents(bool force)
{
	//deviceDbgLog("+ IOAudioDevice::dispatchTimerEvents( %d )\n", force );
	
    if (timerEvents) {
#ifdef DEBUG
        AbsoluteTime now, delta;
        UInt64 nanos;
        
        clock_get_uptime((uint64_t *)&now);
        delta = now;
        SUB_ABSOLUTETIME(&delta, &previousTimerFire);
        absolutetime_to_nanoseconds(*((uint64_t *)&delta), &nanos);
#ifdef __LP64__
        deviceDbgLog("  woke up %lums after last fire - now = {%llu} - previousFire = {%llu}\n", (long unsigned int)(nanos / 1000000), now, previousTimerFire);
#else	/* __LP64__ */
		deviceDbgLog("  woke up %lums after last fire - now = {%ld,%lu} - previousFire = {%ld,%lu}\n", (UInt32)(nanos / 1000000), now.hi, now.lo, previousTimerFire.hi, previousTimerFire.lo);
#endif	/* __LP64__ */
#endif	/* DEBUG */
		
        if (force || (getPowerState() != kIOAudioDeviceSleep)) {
            OSIterator *iterator;
            OSSymbol *target;
            AbsoluteTime nextTimerFire, currentInterval;
            
            currentInterval = minimumInterval;
        
            assert(timerEvents);
        
            iterator = OSCollectionIterator::withCollection(timerEvents);
        
            if (iterator) {
                while ( (target = (OSSymbol *)iterator->getNextObject()) ) {
                    IOAudioTimerEvent *timerEvent;
                    timerEvent = (IOAudioTimerEvent *)timerEvents->getObject(target);
        
                    if (timerEvent) {
                        (*timerEvent->event)(timerEvent->target, this);
                    }
                }
        
                iterator->release();
            }
        
            if (timerEvents->getCount() > 0) {
                ADD_ABSOLUTETIME(&previousTimerFire, &currentInterval);
                nextTimerFire = previousTimerFire;
                ADD_ABSOLUTETIME(&nextTimerFire, &minimumInterval);
        
                assert(timerEventSource);
                
#ifdef DEBUG
                {
                    AbsoluteTime later;
                    UInt64 mi;
                    later = nextTimerFire;
                    absolutetime_to_nanoseconds(*((uint64_t *)&minimumInterval), &mi);
                    if (CMP_ABSOLUTETIME(&later, &now)) {
                        SUB_ABSOLUTETIME(&later, &now);
                        absolutetime_to_nanoseconds(*((uint64_t *)&later), &nanos);
#ifdef __LP64__
						deviceDbgLog("  scheduling timer to fire in %lums at {%llu} - previousTimerFire = {%llu} - interval=%lums\n", (long unsigned int) (nanos / 1000000), nextTimerFire, previousTimerFire, (long unsigned int)(mi/1000000));
#else	/* __LP64__ */
						deviceDbgLog("  scheduling timer to fire in %lums at {%ld,%lu} - previousTimerFire = {%ld,%lu} - interval=%lums\n", (UInt32) (nanos / 1000000), nextTimerFire.hi, nextTimerFire.lo, previousTimerFire.hi, previousTimerFire.lo, (UInt32)(mi/1000000));
#endif	/* __LP64__*/
                    } 
					else 
					{
                        SUB_ABSOLUTETIME(&now, &later);
                        absolutetime_to_nanoseconds(*((uint64_t *)&now), &nanos);
#ifdef __LP64__
                        deviceDbgLog("  scheduling timer to fire in -%lums - previousTimerFire = {%llu}\n", (long unsigned int) (nanos / 1000000), previousTimerFire);
#else	/* __LP64__ */
						deviceDbgLog("  scheduling timer to fire in -%lums - previousTimerFire = {%ld,%lu}\n", (UInt32) (nanos / 1000000), previousTimerFire.hi, previousTimerFire.lo);
#endif	/* __LP64__*/
                    }
                }
#endif	/* DEBUG */
    
                timerEventSource->wakeAtTime(nextTimerFire);
            }
        }
    }
	deviceDbgLog("- IOAudioDevice::dispatchTimerEvents()\n" );
	return;
}
