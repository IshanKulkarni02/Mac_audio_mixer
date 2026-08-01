/*
 * MixerDriver — Phase 0 spike 1 / Phase 1 MVP virtual output device.
 *
 * A minimal AudioServerPlugIn (HAL user-space driver, no kernel extension)
 * that publishes ONE virtual output device named "AudioMixer Output": 2ch,
 * 32-bit float, 48kHz. Any app can select it as an output device; audio
 * written to it lands in an internal ring buffer. Nothing reads that
 * buffer back out yet — wiring it to the Rust dsp-engine ring buffer (so
 * the mixer app can actually route/process it) is the next increment.
 * The goal of this pass is narrower: prove the plugin loads, the device
 * appears in Audio MIDI Setup, and audio can be written to it without
 * crackling — the two open questions from the build plan's Phase 0.
 *
 * Object model follows CoreAudio/AudioServerPlugIn.h directly. No code
 * here is copied from any existing driver (BlackHole, etc.) — only the
 * public Apple header contract was used as reference.
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <string.h>
#include <stdatomic.h>

/* An unimplemented property makes coreaudiod throw and silently drop the
   whole device — the only trace is an opaque "Caught exception trying to
   add device" in its log. Logging every miss with the selector spelled
   out as a FourCC turns that dead end into a one-line diagnosis.
   Read with: log stream --predicate 'subsystem == "com.audiomixer.halplugin"' */
#define MixerLogUnknownProperty(objectName, selector)                                  \
    os_log_error(OS_LOG_DEFAULT,                                                        \
        "[com.audiomixer.halplugin] %s: unimplemented property '%c%c%c%c'",             \
        (objectName),                                                                   \
        (char)(((selector) >> 24) & 0xFF), (char)(((selector) >> 16) & 0xFF),            \
        (char)(((selector) >> 8) & 0xFF), (char)((selector) & 0xFF))

#pragma mark - Constants

enum {
    kObjectID_PlugIn = kAudioObjectPlugInObject,
    kObjectID_Device = 2,
    kObjectID_Stream_Output = 3
};

#define kDevice_ChannelCount 2
#define kDevice_SampleRate 48000.0
#define kDevice_RingBufferFrames 65536

#pragma mark - Driver state

typedef struct {
    _Atomic uint32_t writeIndex;
    float samples[kDevice_RingBufferFrames * kDevice_ChannelCount];
} RingBuffer;

static RingBuffer gRing;
static AudioServerPlugInHostRef gPlugInHost = NULL;
static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32 gStartCount = 0;
static UInt64 gAnchorHostTime = 0;
static Boolean gHaveAnchor = false;

#pragma mark - IUnknown boilerplate

static HRESULT MixerDriver_QueryInterface(void *driver, REFIID uuid, LPVOID *outInterface);
static ULONG MixerDriver_AddRef(void *driver);
static ULONG MixerDriver_Release(void *driver);

#pragma mark - Driver interface forward declarations

static OSStatus MixerDriver_Initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host);
static OSStatus MixerDriver_CreateDevice(AudioServerPlugInDriverRef driver, CFDictionaryRef description, const AudioServerPlugInClientInfo *clientInfo, AudioObjectID *outDeviceObjectID);
static OSStatus MixerDriver_DestroyDevice(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID);
static OSStatus MixerDriver_AddDeviceClient(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, const AudioServerPlugInClientInfo *clientInfo);
static OSStatus MixerDriver_RemoveDeviceClient(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, const AudioServerPlugInClientInfo *clientInfo);
static OSStatus MixerDriver_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt64 changeAction, void *changeInfo);
static OSStatus MixerDriver_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt64 changeAction, void *changeInfo);
static Boolean MixerDriver_HasProperty(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t clientProcessID, const AudioObjectPropertyAddress *address);
static OSStatus MixerDriver_IsPropertySettable(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t clientProcessID, const AudioObjectPropertyAddress *address, Boolean *outSettable);
static OSStatus MixerDriver_GetPropertyDataSize(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t clientProcessID, const AudioObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 *outDataSize);
static OSStatus MixerDriver_GetPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t clientProcessID, const AudioObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData);
static OSStatus MixerDriver_SetPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t clientProcessID, const AudioObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 inDataSize, const void *inData);
static OSStatus MixerDriver_StartIO(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID);
static OSStatus MixerDriver_StopIO(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID);
static OSStatus MixerDriver_GetZeroTimeStamp(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID, Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed);
static OSStatus MixerDriver_WillDoIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID, UInt32 operationID, Boolean *outWillDo, Boolean *outWillDoInPlace);
static OSStatus MixerDriver_BeginIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID, UInt32 operationID, UInt32 ioBufferFrameSize, const AudioServerPlugInIOCycleInfo *ioCycleInfo);
static OSStatus MixerDriver_DoIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, AudioObjectID streamObjectID, UInt32 clientID, UInt32 operationID, UInt32 ioBufferFrameSize, const AudioServerPlugInIOCycleInfo *ioCycleInfo, void *ioMainBuffer, void *ioSecondaryBuffer);
static OSStatus MixerDriver_EndIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID, UInt32 operationID, UInt32 ioBufferFrameSize, const AudioServerPlugInIOCycleInfo *ioCycleInfo);

#pragma mark - vtable

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    MixerDriver_QueryInterface,
    MixerDriver_AddRef,
    MixerDriver_Release,
    MixerDriver_Initialize,
    MixerDriver_CreateDevice,
    MixerDriver_DestroyDevice,
    MixerDriver_AddDeviceClient,
    MixerDriver_RemoveDeviceClient,
    MixerDriver_PerformDeviceConfigurationChange,
    MixerDriver_AbortDeviceConfigurationChange,
    MixerDriver_HasProperty,
    MixerDriver_IsPropertySettable,
    MixerDriver_GetPropertyDataSize,
    MixerDriver_GetPropertyData,
    MixerDriver_SetPropertyData,
    MixerDriver_StartIO,
    MixerDriver_StopIO,
    MixerDriver_GetZeroTimeStamp,
    MixerDriver_WillDoIOOperation,
    MixerDriver_BeginIOOperation,
    MixerDriver_DoIOOperation,
    MixerDriver_EndIOOperation
};

static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

#pragma mark - CFPlugIn factory

void *MixerDriver_Create(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID);
void *MixerDriver_Create(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID) {
    (void)allocator;
    if (!CFEqual(requestedTypeUUID, kAudioServerPlugInTypeUUID)) {
        return NULL;
    }
    return gDriverRef;
}

#pragma mark - IUnknown

static HRESULT MixerDriver_QueryInterface(void *driver, REFIID uuid, LPVOID *outInterface) {
    (void)driver;
    if (outInterface == NULL) {
        return E_INVALIDARG;
    }
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    Boolean matches = CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(requested);
    if (!matches) {
        *outInterface = NULL;
        return E_NOINTERFACE;
    }
    MixerDriver_AddRef(driver);
    *outInterface = gDriverRef;
    return S_OK;
}

static _Atomic uint32_t gRefCount = 1;

static ULONG MixerDriver_AddRef(void *driver) {
    (void)driver;
    return atomic_fetch_add(&gRefCount, 1) + 1;
}

static ULONG MixerDriver_Release(void *driver) {
    (void)driver;
    uint32_t previous = atomic_fetch_sub(&gRefCount, 1);
    return previous > 0 ? previous - 1 : 0;
}

#pragma mark - Basic operations

static OSStatus MixerDriver_Initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host) {
    (void)driver;
    gPlugInHost = host;
    return noErr;
}

static OSStatus MixerDriver_CreateDevice(AudioServerPlugInDriverRef driver, CFDictionaryRef description, const AudioServerPlugInClientInfo *clientInfo, AudioObjectID *outDeviceObjectID) {
    (void)driver; (void)description; (void)clientInfo; (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus MixerDriver_DestroyDevice(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID) {
    (void)driver; (void)deviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus MixerDriver_AddDeviceClient(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, const AudioServerPlugInClientInfo *clientInfo) {
    (void)driver; (void)deviceObjectID; (void)clientInfo;
    return noErr;
}

static OSStatus MixerDriver_RemoveDeviceClient(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, const AudioServerPlugInClientInfo *clientInfo) {
    (void)driver; (void)deviceObjectID; (void)clientInfo;
    return noErr;
}

static OSStatus MixerDriver_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt64 changeAction, void *changeInfo) {
    (void)driver; (void)deviceObjectID; (void)changeAction; (void)changeInfo;
    return noErr;
}

static OSStatus MixerDriver_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt64 changeAction, void *changeInfo) {
    (void)driver; (void)deviceObjectID; (void)changeAction; (void)changeInfo;
    return noErr;
}

#pragma mark - Property helpers

static Boolean PlugIn_HasProperty(const AudioObjectPropertyAddress *address) {
    switch (address->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyBundleID:
            return true;
        default:
            return false;
    }
}

static OSStatus PlugIn_GetPropertyDataSize(const AudioObjectPropertyAddress *address, UInt32 *outDataSize) {
    switch (address->mSelector) {
        case kAudioObjectPropertyBaseClass: *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioObjectPropertyManufacturer: *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioObjectPropertyOwnedObjects: *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioPlugInPropertyDeviceList: *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioPlugInPropertyTranslateUIDToDevice: *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioPlugInPropertyBundleID: *outDataSize = sizeof(CFStringRef); return noErr;
        default: *outDataSize = 0; return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus PlugIn_GetPropertyData(const AudioObjectPropertyAddress *address, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    switch (address->mSelector) {
        case kAudioObjectPropertyBaseClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *(AudioClassID *)outData = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            return noErr;
        case kAudioObjectPropertyClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *(AudioClassID *)outData = kAudioPlugInClassID;
            *outDataSize = sizeof(AudioClassID);
            return noErr;
        case kAudioObjectPropertyOwner:
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *(AudioObjectID *)outData = kAudioObjectUnknown;
            *outDataSize = sizeof(AudioObjectID);
            return noErr;
        case kAudioObjectPropertyManufacturer:
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *(CFStringRef *)outData = CFSTR("AudioMixer Project");
            *outDataSize = sizeof(CFStringRef);
            return noErr;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *(AudioObjectID *)outData = kObjectID_Device;
            *outDataSize = sizeof(AudioObjectID);
            return noErr;
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *(AudioObjectID *)outData = kObjectID_Device;
            *outDataSize = sizeof(AudioObjectID);
            return noErr;
        }
        case kAudioPlugInPropertyBundleID:
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *(CFStringRef *)outData = CFSTR("com.audiomixer.halplugin");
            *outDataSize = sizeof(CFStringRef);
            return noErr;
        default:
            MixerLogUnknownProperty("plugin", address->mSelector);
            return kAudioHardwareUnknownPropertyError;
    }
}

static Boolean Device_HasProperty(const AudioObjectPropertyAddress *address) {
    switch (address->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
        case kAudioDevicePropertyZeroTimeStampPeriod:
            return true;
        default:
            return false;
    }
}

static OSStatus Device_GetPropertyDataSize(const AudioObjectPropertyAddress *address, UInt32 *outDataSize) {
    switch (address->mSelector) {
        case kAudioObjectPropertyBaseClass: *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioObjectPropertyName: *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioObjectPropertyManufacturer: *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioObjectPropertyOwnedObjects: *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioDevicePropertyDeviceUID: *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioDevicePropertyModelUID: *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioDevicePropertyTransportType: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyRelatedDevices: *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioDevicePropertyClockDomain: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyDeviceIsAlive: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyDeviceIsRunning: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyLatency: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyStreams: *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioObjectPropertyControlList: *outDataSize = 0; return noErr;
        case kAudioDevicePropertySafetyOffset: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyNominalSampleRate: *outDataSize = sizeof(Float64); return noErr;
        case kAudioDevicePropertyAvailableNominalSampleRates: *outDataSize = sizeof(AudioValueRange); return noErr;
        case kAudioDevicePropertyIsHidden: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyPreferredChannelsForStereo: *outDataSize = 2 * sizeof(UInt32); return noErr;
        case kAudioDevicePropertyPreferredChannelLayout: *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions); return noErr;
        case kAudioDevicePropertyZeroTimeStampPeriod: *outDataSize = sizeof(UInt32); return noErr;
        default: *outDataSize = 0; return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus Device_GetPropertyData(const AudioObjectPropertyAddress *address, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    switch (address->mSelector) {
        case kAudioObjectPropertyBaseClass:
            *(AudioClassID *)outData = kAudioObjectClassID; *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyClass:
            *(AudioClassID *)outData = kAudioDeviceClassID; *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyOwner:
            *(AudioObjectID *)outData = kObjectID_PlugIn; *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioObjectPropertyName:
            *(CFStringRef *)outData = CFSTR("AudioMixer Output"); *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioObjectPropertyManufacturer:
            *(CFStringRef *)outData = CFSTR("AudioMixer Project"); *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *(AudioObjectID *)outData = kObjectID_Stream_Output; *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioDevicePropertyDeviceUID:
            *(CFStringRef *)outData = CFSTR("com.audiomixer.halplugin.output"); *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioDevicePropertyModelUID:
            *(CFStringRef *)outData = CFSTR("com.audiomixer.halplugin.output.model"); *outDataSize = sizeof(CFStringRef); return noErr;
        case kAudioDevicePropertyTransportType:
            *(UInt32 *)outData = kAudioDeviceTransportTypeVirtual; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyRelatedDevices:
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *(AudioObjectID *)outData = kObjectID_Device; *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioDevicePropertyClockDomain:
            *(UInt32 *)outData = 0; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyDeviceIsAlive:
            *(UInt32 *)outData = 1; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyDeviceIsRunning:
            *(UInt32 *)outData = gStartCount > 0 ? 1 : 0; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            *(UInt32 *)outData = 1; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyLatency:
            *(UInt32 *)outData = 0; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioObjectPropertyControlList:
            *outDataSize = 0; return noErr;
        case kAudioDevicePropertySafetyOffset:
            *(UInt32 *)outData = 0; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyNominalSampleRate:
            *(Float64 *)outData = kDevice_SampleRate; *outDataSize = sizeof(Float64); return noErr;
        case kAudioDevicePropertyAvailableNominalSampleRates:
            if (inDataSize < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
            ((AudioValueRange *)outData)->mMinimum = kDevice_SampleRate;
            ((AudioValueRange *)outData)->mMaximum = kDevice_SampleRate;
            *outDataSize = sizeof(AudioValueRange);
            return noErr;
        case kAudioDevicePropertyIsHidden:
            *(UInt32 *)outData = 0; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyZeroTimeStampPeriod:
            /* Must match the stride GetZeroTimeStamp advances by, or the
               HAL can't build a sample-time↔host-time mapping and refuses
               to add the device. */
            *(UInt32 *)outData = kDevice_RingBufferFrames; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioDevicePropertyPreferredChannelsForStereo:
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)outData)[0] = 1;
            ((UInt32 *)outData)[1] = 2;
            *outDataSize = 2 * sizeof(UInt32);
            return noErr;
        case kAudioDevicePropertyPreferredChannelLayout: {
            UInt32 headerSize = (UInt32)offsetof(AudioChannelLayout, mChannelDescriptions);
            if (inDataSize < headerSize) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *layout = (AudioChannelLayout *)outData;
            layout->mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
            layout->mChannelBitmap = 0;
            layout->mNumberChannelDescriptions = 0;
            *outDataSize = headerSize;
            return noErr;
        }
        default:
            MixerLogUnknownProperty("device", address->mSelector);
            return kAudioHardwareUnknownPropertyError;
    }
}

static Boolean Stream_HasProperty(const AudioObjectPropertyAddress *address) {
    switch (address->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        default:
            return false;
    }
}

static AudioStreamBasicDescription StreamFormat(void) {
    AudioStreamBasicDescription format;
    format.mSampleRate = kDevice_SampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = sizeof(Float32) * kDevice_ChannelCount;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(Float32) * kDevice_ChannelCount;
    format.mChannelsPerFrame = kDevice_ChannelCount;
    format.mBitsPerChannel = 32;
    format.mReserved = 0;
    return format;
}

static OSStatus Stream_GetPropertyDataSize(const AudioObjectPropertyAddress *address, UInt32 *outDataSize) {
    switch (address->mSelector) {
        case kAudioObjectPropertyBaseClass: *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioStreamPropertyIsActive: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyDirection: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyTerminalType: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyStartingChannel: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyLatency: *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outDataSize = sizeof(AudioStreamBasicDescription); return noErr;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = sizeof(AudioStreamRangedDescription); return noErr;
        default: *outDataSize = 0; return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus Stream_GetPropertyData(const AudioObjectPropertyAddress *address, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    switch (address->mSelector) {
        case kAudioObjectPropertyBaseClass:
            *(AudioClassID *)outData = kAudioObjectClassID; *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyClass:
            *(AudioClassID *)outData = kAudioStreamClassID; *outDataSize = sizeof(AudioClassID); return noErr;
        case kAudioObjectPropertyOwner:
            *(AudioObjectID *)outData = kObjectID_Device; *outDataSize = sizeof(AudioObjectID); return noErr;
        case kAudioStreamPropertyIsActive:
            *(UInt32 *)outData = 1; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyDirection:
            *(UInt32 *)outData = 0; /* output */ *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyTerminalType:
            *(UInt32 *)outData = kAudioStreamTerminalTypeSpeaker; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyStartingChannel:
            *(UInt32 *)outData = 1; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyLatency:
            *(UInt32 *)outData = 0; *outDataSize = sizeof(UInt32); return noErr;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            if (inDataSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
            *(AudioStreamBasicDescription *)outData = StreamFormat();
            *outDataSize = sizeof(AudioStreamBasicDescription);
            return noErr;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            if (inDataSize < sizeof(AudioStreamRangedDescription)) return kAudioHardwareBadPropertySizeError;
            ((AudioStreamRangedDescription *)outData)->mFormat = StreamFormat();
            ((AudioStreamRangedDescription *)outData)->mSampleRateRange.mMinimum = kDevice_SampleRate;
            ((AudioStreamRangedDescription *)outData)->mSampleRateRange.mMaximum = kDevice_SampleRate;
            *outDataSize = sizeof(AudioStreamRangedDescription);
            return noErr;
        default:
            MixerLogUnknownProperty("stream", address->mSelector);
            return kAudioHardwareUnknownPropertyError;
    }
}

static Boolean MixerDriver_HasProperty(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t clientProcessID, const AudioObjectPropertyAddress *address) {
    (void)driver; (void)clientProcessID;
    if (objectID == kObjectID_PlugIn) return PlugIn_HasProperty(address);
    if (objectID == kObjectID_Device) return Device_HasProperty(address);
    if (objectID == kObjectID_Stream_Output) return Stream_HasProperty(address);
    return false;
}

static OSStatus MixerDriver_IsPropertySettable(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t clientProcessID, const AudioObjectPropertyAddress *address, Boolean *outSettable) {
    (void)driver; (void)objectID; (void)clientProcessID; (void)address;
    *outSettable = false;
    return noErr;
}

static OSStatus MixerDriver_GetPropertyDataSize(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t clientProcessID, const AudioObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 *outDataSize) {
    (void)driver; (void)clientProcessID; (void)qualifierDataSize; (void)qualifierData;
    if (objectID == kObjectID_PlugIn) return PlugIn_GetPropertyDataSize(address, outDataSize);
    if (objectID == kObjectID_Device) return Device_GetPropertyDataSize(address, outDataSize);
    if (objectID == kObjectID_Stream_Output) return Stream_GetPropertyDataSize(address, outDataSize);
    return kAudioHardwareBadObjectError;
}

static OSStatus MixerDriver_GetPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t clientProcessID, const AudioObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    (void)driver; (void)clientProcessID; (void)qualifierDataSize; (void)qualifierData;
    if (objectID == kObjectID_PlugIn) return PlugIn_GetPropertyData(address, inDataSize, outDataSize, outData);
    if (objectID == kObjectID_Device) return Device_GetPropertyData(address, inDataSize, outDataSize, outData);
    if (objectID == kObjectID_Stream_Output) return Stream_GetPropertyData(address, inDataSize, outDataSize, outData);
    return kAudioHardwareBadObjectError;
}

static OSStatus MixerDriver_SetPropertyData(AudioServerPlugInDriverRef driver, AudioObjectID objectID, pid_t clientProcessID, const AudioObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 inDataSize, const void *inData) {
    (void)driver; (void)objectID; (void)clientProcessID; (void)address; (void)qualifierDataSize; (void)qualifierData; (void)inDataSize; (void)inData;
    return kAudioHardwareUnknownPropertyError;
}

#pragma mark - IO

static OSStatus MixerDriver_StartIO(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID) {
    (void)driver; (void)deviceObjectID; (void)clientID;
    pthread_mutex_lock(&gStateMutex);
    gStartCount += 1;
    if (!gHaveAnchor) {
        gAnchorHostTime = mach_absolute_time();
        gHaveAnchor = true;
    }
    pthread_mutex_unlock(&gStateMutex);
    return noErr;
}

static OSStatus MixerDriver_StopIO(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID) {
    (void)driver; (void)deviceObjectID; (void)clientID;
    pthread_mutex_lock(&gStateMutex);
    if (gStartCount > 0) gStartCount -= 1;
    pthread_mutex_unlock(&gStateMutex);
    return noErr;
}

static OSStatus MixerDriver_GetZeroTimeStamp(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID, Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed) {
    (void)driver; (void)deviceObjectID; (void)clientID;

    static mach_timebase_info_data_t timebase = {0, 0};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }

    UInt64 now = mach_absolute_time();
    UInt64 elapsedNanos = (now - gAnchorHostTime) * timebase.numer / timebase.denom;
    Float64 elapsedSeconds = (Float64)elapsedNanos / 1000000000.0;
    Float64 sampleTime = elapsedSeconds * kDevice_SampleRate;

    /* Snap to ring-buffer-sized boundaries, matching the classic
       null-driver pattern so IO stays aligned across cycles. */
    Float64 bucket = (Float64)kDevice_RingBufferFrames;
    Float64 snappedSampleTime = floor(sampleTime / bucket) * bucket;
    UInt64 snappedHostTime = gAnchorHostTime + (UInt64)(snappedSampleTime / kDevice_SampleRate * 1000000000.0 * timebase.denom / timebase.numer);

    *outSampleTime = snappedSampleTime;
    *outHostTime = snappedHostTime;
    *outSeed = 1;
    return noErr;
}

static OSStatus MixerDriver_WillDoIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID, UInt32 operationID, Boolean *outWillDo, Boolean *outWillDoInPlace) {
    (void)driver; (void)deviceObjectID; (void)clientID;
    switch (operationID) {
        case kAudioServerPlugInIOOperationWriteMix:
            *outWillDo = true;
            *outWillDoInPlace = true;
            return noErr;
        default:
            *outWillDo = false;
            *outWillDoInPlace = true;
            return noErr;
    }
}

static OSStatus MixerDriver_BeginIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID, UInt32 operationID, UInt32 ioBufferFrameSize, const AudioServerPlugInIOCycleInfo *ioCycleInfo) {
    (void)driver; (void)deviceObjectID; (void)clientID; (void)operationID; (void)ioBufferFrameSize; (void)ioCycleInfo;
    return noErr;
}

static OSStatus MixerDriver_DoIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, AudioObjectID streamObjectID, UInt32 clientID, UInt32 operationID, UInt32 ioBufferFrameSize, const AudioServerPlugInIOCycleInfo *ioCycleInfo, void *ioMainBuffer, void *ioSecondaryBuffer) {
    (void)driver; (void)deviceObjectID; (void)streamObjectID; (void)clientID; (void)ioCycleInfo; (void)ioSecondaryBuffer;

    if (operationID != kAudioServerPlugInIOOperationWriteMix || ioMainBuffer == NULL) {
        return noErr;
    }

    /* Copy the host's mixed output into our ring buffer. Nothing reads
       this back out yet in this pass — the point here is proving the
       write path is glitch-free. A reader (feeding the Rust dsp-engine
       ring buffer) is the next increment. */
    const Float32 *source = (const Float32 *)ioMainBuffer;
    UInt32 frames = ioBufferFrameSize;
    uint32_t writeIndex = atomic_load(&gRing.writeIndex);
    for (UInt32 frame = 0; frame < frames; frame++) {
        for (UInt32 channel = 0; channel < kDevice_ChannelCount; channel++) {
            uint32_t slot = (writeIndex + frame) % kDevice_RingBufferFrames;
            gRing.samples[slot * kDevice_ChannelCount + channel] = source[frame * kDevice_ChannelCount + channel];
        }
    }
    atomic_store(&gRing.writeIndex, (writeIndex + frames) % kDevice_RingBufferFrames);

    return noErr;
}

static OSStatus MixerDriver_EndIOOperation(AudioServerPlugInDriverRef driver, AudioObjectID deviceObjectID, UInt32 clientID, UInt32 operationID, UInt32 ioBufferFrameSize, const AudioServerPlugInIOCycleInfo *ioCycleInfo) {
    (void)driver; (void)deviceObjectID; (void)clientID; (void)operationID; (void)ioBufferFrameSize; (void)ioCycleInfo;
    return noErr;
}
