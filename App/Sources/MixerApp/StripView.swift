import SwiftUI
import MixerCore

struct StripView: View {
    @Bindable var strip: Strip

    private var symbol: String {
        switch strip.kind {
        case .hardwareInput: return "mic.fill"
        case .virtualInput: return "arrow.triangle.2.circlepath"
        case .applicationInput: return "app.badge"
        case .output: return "speaker.wave.2.fill"
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Image(systemName: symbol)
                    .frame(width: 22, height: 22)
                    .background(.quaternary, in: RoundedRectangle(cornerRadius: 6))
                VStack(alignment: .leading, spacing: 1) {
                    Text(strip.name).font(.system(size: 12, weight: .semibold))
                    Text(strip.subtitle).font(.system(size: 9.5)).foregroundStyle(.secondary)
                }
                Spacer()
                Button {
                    strip.muted.toggle()
                } label: {
                    Image(systemName: strip.muted ? "speaker.slash.fill" : "speaker.fill")
                }
                .buttonStyle(.borderless)
                .foregroundStyle(strip.muted ? .red : .secondary)
            }

            HStack(spacing: 6) {
                Text(String(format: "%+.0fdB", strip.gainDB))
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .frame(width: 34, alignment: .leading)
                Slider(value: $strip.gainDB, in: -40...12)
            }
        }
        .padding(10)
        .opacity(strip.muted ? 0.5 : 1)
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 10))
    }
}
