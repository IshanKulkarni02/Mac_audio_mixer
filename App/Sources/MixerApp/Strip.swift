import Foundation

enum StripKind {
    case hardwareInput
    case virtualInput
    case applicationInput
    case output
}

@Observable
final class Strip: Identifiable {
    let id = UUID()
    let name: String
    let subtitle: String
    let kind: StripKind
    var gainDB: Double
    var muted: Bool

    init(name: String, subtitle: String, kind: StripKind, gainDB: Double = 0, muted: Bool = false) {
        self.name = name
        self.subtitle = subtitle
        self.kind = kind
        self.gainDB = gainDB
        self.muted = muted
    }
}
