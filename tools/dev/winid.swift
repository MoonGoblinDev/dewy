import CoreGraphics
let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as! [[String: Any]]
for w in list {
    let owner = w[kCGWindowOwnerName as String] as? String ?? ""
    let name = w[kCGWindowName as String] as? String ?? ""
    let b = w[kCGWindowBounds as String] as? [String: Any] ?? [:]
    if owner == "OBS Studio" || owner == "OBS", let h = b["Height"] as? Double, h > 300 {
        print(w[kCGWindowNumber as String]!, owner, "|", name, Int(b["Width"] as? Double ?? 0), Int(h))
    }
}
