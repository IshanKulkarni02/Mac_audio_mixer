// Phase 0, Spike 2 — Process Tap feasibility test.
//
// Confirms two things before any other code gets written:
//   1. CATapDescription + AudioHardwareCreateProcessTap can capture a
//      single running app's audio in-process on this machine's current
//      macOS version.
//   2. muteBehavior actually silences that app at the system output
//      (no double-audio) once tapped.
//
// Usage: swift run ProcessTapSpike [bundle-id]
// Defaults to com.apple.Music. Play audio in the target app while this runs.

import CoreAudio
import AudioToolbox
import Foundation

func check(_ status: OSStatus, _ what: String) throws {
    guard status == noErr else {
        throw NSError(domain: "ProcessTapSpike", code: Int(status),
                       userInfo: [NSLocalizedDescriptionKey: "\(what) failed: OSStatus \(status)"])
    }
}

func processObjectList() throws -> [AudioObjectID] {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyProcessObjectList,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain)

    var dataSize: UInt32 = 0
    try check(AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &dataSize),
              "get process list size")

    let count = Int(dataSize) / MemoryLayout<AudioObjectID>.size
    var ids = [AudioObjectID](repeating: 0, count: count)
    try check(AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &dataSize, &ids),
              "get process list")
    return ids
}

func pid(of processObject: AudioObjectID) -> pid_t? {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioProcessPropertyPID,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain)
    var value: pid_t = 0
    var size = UInt32(MemoryLayout<pid_t>.size)
    let status = AudioObjectGetPropertyData(processObject, &address, 0, nil, &size, &value)
    return status == noErr ? value : nil
}

func bundleID(of processObject: AudioObjectID) -> String? {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioProcessPropertyBundleID,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain)
    var value: CFString = "" as CFString
    var size = UInt32(MemoryLayout<CFString>.size)
    let status = withUnsafeMutablePointer(to: &value) { ptr -> OSStatus in
        AudioObjectGetPropertyData(processObject, &address, 0, nil, &size, ptr)
    }
    return status == noErr ? (value as String) : nil
}

func findProcessObject(bundleID target: String) throws -> AudioObjectID {
    let candidates = try processObjectList()
    for object in candidates {
        if let bid = bundleID(of: object), bid == target {
            return object
        }
    }
    let seen = candidates.compactMap { bundleID(of: $0) }
    throw NSError(domain: "ProcessTapSpike", code: -1, userInfo: [
        NSLocalizedDescriptionKey: "No running audio process with bundle id \(target). Seen: \(seen.joined(separator: ", "))"
    ])
}

func tapUID(of tapID: AudioObjectID) throws -> String {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioTapPropertyUID,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain)
    var value: CFString = "" as CFString
    var size = UInt32(MemoryLayout<CFString>.size)
    try check(withUnsafeMutablePointer(to: &value) { ptr -> OSStatus in
        AudioObjectGetPropertyData(tapID, &address, 0, nil, &size, ptr)
    }, "get tap UID")
    return value as String
}

func rms(of bufferList: UnsafePointer<AudioBufferList>) -> Float {
    let list = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: bufferList))
    var sum: Float = 0
    var count: Int = 0
    for buffer in list {
        guard let data = buffer.mData else { continue }
        let frameCount = Int(buffer.mDataByteSize) / MemoryLayout<Float>.size
        let samples = data.bindMemory(to: Float.self, capacity: frameCount)
        for i in 0..<frameCount {
            sum += samples[i] * samples[i]
            count += 1
        }
    }
    guard count > 0 else { return 0 }
    return sqrtf(sum / Float(count))
}

let targetBundleID = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "com.apple.Music"

print("ProcessTapSpike — looking for \(targetBundleID)...")

do {
    let processObject = try findProcessObject(bundleID: targetBundleID)
    print("Found process object \(processObject) (pid \(pid(of: processObject).map(String.init) ?? "?"))")

    let tapDescription = CATapDescription(stereoMixdownOfProcesses: [processObject])
    tapDescription.name = "com.audiomixer.spike.tap"
    tapDescription.muteBehavior = .mutedWhenTapped

    var tapID: AudioObjectID = AudioObjectID(kAudioObjectUnknown)
    try check(AudioHardwareCreateProcessTap(tapDescription, &tapID), "create process tap")
    print("Created tap \(tapID). \(targetBundleID) should now be SILENT at the system output — audio is only flowing into this process.")

    let uid = try tapUID(of: tapID)

    let subTap: [String: Any] = [
        kAudioSubTapUIDKey as String: uid,
        kAudioSubTapDriftCompensationKey as String: true
    ]
    let aggregateDescription: [String: Any] = [
        kAudioAggregateDeviceNameKey as String: "ProcessTapSpike Aggregate",
        kAudioAggregateDeviceUIDKey as String: UUID().uuidString,
        kAudioAggregateDeviceIsPrivateKey as String: true,
        kAudioAggregateDeviceTapAutoStartKey as String: true,
        kAudioAggregateDeviceTapListKey as String: [subTap]
    ]

    var aggregateID: AudioObjectID = AudioObjectID(kAudioObjectUnknown)
    try check(AudioHardwareCreateAggregateDevice(aggregateDescription as CFDictionary, &aggregateID),
              "create aggregate device")

    var ioProcID: AudioDeviceIOProcID?
    var peak: Float = 0
    let lock = NSLock()

    let status = AudioDeviceCreateIOProcIDWithBlock(&ioProcID, aggregateID, nil) { _, inputData, _, _, _ in
        let level = rms(of: inputData)
        lock.lock()
        peak = max(peak, level)
        lock.unlock()
    }
    try check(status, "create IO proc")
    try check(AudioDeviceStart(aggregateID, ioProcID), "start aggregate device")

    print("Capturing for 8 seconds — play audio in \(targetBundleID) now...")
    for second in 1...8 {
        Thread.sleep(forTimeInterval: 1.0)
        lock.lock()
        let level = peak
        peak = 0
        lock.unlock()
        let bars = String(repeating: "#", count: min(40, Int(level * 400)))
        print(String(format: "  t+%ds  rms=%.4f  %@", second, level, bars))
    }

    try check(AudioDeviceStop(aggregateID, ioProcID), "stop aggregate device")
    try check(AudioDeviceDestroyIOProcID(aggregateID, ioProcID!), "destroy IO proc")
    try check(AudioHardwareDestroyAggregateDevice(aggregateID), "destroy aggregate device")
    try check(AudioHardwareDestroyProcessTap(tapID), "destroy process tap")

    print("Done. \(targetBundleID) audio should be audible again now that the tap is destroyed.")
} catch {
    print("SPIKE FAILED: \(error.localizedDescription)")
    exit(1)
}
