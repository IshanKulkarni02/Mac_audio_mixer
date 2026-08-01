import SwiftUI

struct ContentView: View {
    @State private var store = MixerStore()

    var body: some View {
        HStack(alignment: .top, spacing: 16) {
            VStack(alignment: .leading, spacing: 10) {
                Text("INPUTS")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundStyle(.secondary)
                ForEach(store.inputs) { strip in
                    StripView(strip: strip)
                }
            }
            .frame(width: 220)

            Spacer(minLength: 40)

            VStack(alignment: .leading, spacing: 10) {
                Text("OUTPUTS")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundStyle(.secondary)
                ForEach(store.outputs) { strip in
                    StripView(strip: strip)
                }
            }
            .frame(width: 220)
        }
        .padding(20)
        .frame(minWidth: 560, minHeight: 340)
        .background(.regularMaterial)
    }
}
