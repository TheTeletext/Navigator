// lib.typ - the manual's helpers, imported by manual.typ and by the generated
// buildront.typ and buildody.typ (an included file has its own scope).
// Part of Teletext Navigator. Copyright (C) 2026 Dimitri Papadopoulos and
// The Teletext. GNU Affero General Public License v3.0 or later.
#let serif = "Liberation Serif"
#let sans = "Liberation Sans"
#let mono = "Liberation Mono"
#let navy = rgb(0, 0, 128)

// ---------------------------------------------------------------- helpers
#let pagenum(l) = context {
  let loc = locate(l)
  numbering(loc.page-numbering() + "", ..counter(page).at(loc))
}
#let xref(l, t) = [#t (page #pagenum(l))]
#let seeline(l, t) = block(spacing: 0.45em, inset: (left: 1.2em),
  [#link(l, t) #box(width: 1fr, repeat(gap: 0.25em)[.]) #pagenum(l)])
#let term(t) = emph(t)
#let icon(name) = box(baseline: 20%, image("build/" + name.replace(".BMP", ".png"), height: 1.6em, scaling: "pixelated"))
#let codeline(t) = block(spacing: 0.3em, inset: (left: 1.6em), raw(t))
#let shot(path, caption, w) = figure(
  box(stroke: 0.5pt + luma(60%), image(path, width: w * 1in, scaling: "pixelated")),
  caption: text(font: sans, size: 9pt, caption), gap: 0.7em)
#let idx(t) = [#metadata(t)<idx>]
#let gloss(t, d) = block(spacing: 0.9em, [#text(font: sans, weight: "bold", t) \ #pad(left: 1.2em, d)])
#let chapicon = state("chapicon", none)
#let numbered = state("numbered", true)

#let chapter(title, icon: none) = { chapicon.update(icon) }
#let foreword(title, body) = {
  chapicon.update(none)
  heading(level: 1, numbering: none, title)
  body
}
