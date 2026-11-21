// Copyright 2026 ALTESSA SOLUTIONS INC.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.
import Foundation
import PDFKit

let document = PDFDocument(url: URL(fileURLWithPath: CommandLine.arguments[1]))!
let expected = "Alpha Beta : Gamma Delta .\nEpsilon Zeta Eta\nЖёлтый цвет"
let actual = (document.string ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
precondition(actual == expected, "PDFKit order: \(actual)")
let page = document.page(at: 0)!
let factor = 72.0 / 25.4
for (word, x, y, w, h) in [
    ("Alpha", 8.0, 10.0, 15.0, 6.0),
    ("Beta", 25.0, 11.0, 12.0, 5.0),
    ("Gamma", 41.0, 10.5, 20.0, 6.5),
    ("Epsilon", 8.0, 18.0, 22.0, 7.0),
    ("Жёлтый", 8.0, 26.0, 22.0, 6.0),
] {
    let matches = document.findString(word, withOptions: [])
    precondition(matches.count == 1, "PDFKit search: \(word)")
    let bounds = matches[0].bounds(for: page)
    let centerY = page.bounds(for: .mediaBox).height - bounds.midY
    precondition(abs(bounds.minX - x * factor) < 1)
    precondition(bounds.maxX <= (x + w + h * 0.25) * factor + 1)
    precondition(centerY >= y * factor && centerY <= (y + h) * factor)
}
precondition(document.findString("Gamma Delta", withOptions: []).count == 1)
print("PDFKit recognition order, cleaning, phrase search and word rectangles passed")

var tallBaseline: [CGRect] = []
for path in CommandLine.arguments.dropFirst(2) {
    let pdf = PDFDocument(url: URL(fileURLWithPath: path))!
    let firstLine = path.hasSuffix("line_same_x.pdf") ? "Echo Echo Echo" : "Alpha Beta ."
    let value = (pdf.string ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
    let expected = path.hasSuffix("line_disjoint.pdf") ? "Alpha a .\nGamma Delta"
        : path.hasSuffix("line_slivers.pdf") ? ". . . . . . . . Gamma Delta"
        : path.hasSuffix("line_tall_base.pdf") ? "Hello world"
        : path.hasSuffix("line_tall.pdf") ? "Hello world\nTall"
        : path.hasSuffix("line_tall_same.pdf") ? "Hello world Tall"
        : firstLine + "\nGamma Delta"
    precondition(value == expected, "PDFKit edge case \(path): \(value)")
    if path.contains("line_tall") {
        let page = pdf.page(at: 0)!
        let boxes = ["Hello", "world"].map { word -> CGRect in
            let matches = pdf.findString(word, withOptions: [])
            precondition(matches.count == 1)
            let bounds = matches[0].bounds(for: page)
            let center = page.bounds(for: .mediaBox).height - bounds.midY
            precondition(center >= 10 * factor && center <= 16 * factor)
            precondition(abs(bounds.height - 6 * factor) < 0.1)
            return bounds
        }
        if path.hasSuffix("line_tall_base.pdf") {
            tallBaseline = boxes
        } else if path.hasSuffix("line_tall.pdf") {
            precondition(boxes == tallBaseline, "Tall box changed another line's rectangles")
        }
        precondition(pdf.findString("Hello world", withOptions: []).count == 1)
    } else {
        precondition(pdf.findString("Gamma Delta", withOptions: []).count == 1)
    }
    if path.hasSuffix("line_same_x.pdf") {
        let matches = pdf.findString("Echo", withOptions: [])
        precondition(matches.count == 3)
        let page = pdf.page(at: 0)!
        let boxes = matches.map { $0.bounds(for: page) }.sorted { $0.minX < $1.minX }
        precondition(boxes[0].maxX < boxes[1].minX && boxes[1].maxX < boxes[2].minX)
    }
}
print("PDFKit edge-clipped, tiny-dot, coincident-x, disjoint punctuation and majority-sliver lines passed without blank lines")
