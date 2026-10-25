//go:build guest_windows

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 1993, 1994 Alexandre Julliard; 2002 Bill Medland
// Copyright (C) 1998-2001 ReactOS Team
// Derived from ReactOS win32ss/user/rtl/text.c, Wine-derived DrawText worker.
// Upstream: https://github.com/reactos/reactos/tree/dacf2a6c47fa562799f9c8a41ab6d5c558556558

package win32

// DrawText flags (DT_*)
const (
	DT_TOP                  = 0x00000000
	DT_LEFT                 = 0x00000000
	DT_CENTER               = 0x00000001
	DT_RIGHT                = 0x00000002
	DT_VCENTER              = 0x00000004
	DT_BOTTOM               = 0x00000008
	DT_WORDBREAK            = 0x00000010
	DT_SINGLELINE           = 0x00000020
	DT_EXPANDTABS           = 0x00000040
	DT_TABSTOP              = 0x00000080
	DT_NOCLIP               = 0x00000100
	DT_EXTERNALLEADING      = 0x00000200
	DT_CALCRECT             = 0x00000400
	DT_NOPREFIX             = 0x00000800
	DT_INTERNAL             = 0x00001000
	DT_EDITCONTROL          = 0x00002000
	DT_PATH_ELLIPSIS        = 0x00004000
	DT_END_ELLIPSIS         = 0x00008000
	DT_MODIFYSTRING         = 0x00010000
	DT_RTLREADING           = 0x00020000
	DT_WORD_ELLIPSIS        = 0x00040000
	DT_NOFULLWIDTHCHARBREAK = 0x00080000
	DT_HIDEPREFIX           = 0x00100000
	DT_PREFIXONLY           = 0x00200000
)

// DrawTextMeasurer is the interface required by DrawText to measure and draw
// text. It is implemented by the GDI DC in main.go and passed through the
// handler.
type DrawTextMeasurer interface {
	// TextExtent returns the pixel width and height of s using the current font.
	TextExtent(s string) (w, h int32)
	// TextOut draws s at (x,y) within the target surface.
	TextOut(x, y int32, s string)
	// FontHeight returns the line height of the current font.
	FontHeight() int32
}

// DrawTextResult is returned by DrawTextW and contains the painted/calculated
// bounding rectangle and the total height in pixels.
type DrawTextResult struct {
	Rect   Rect
	Height int32
}

// DrawTextW draws text into r using dc for measurement and rendering.
// Implements the same subset of flags as ReactOS DrawTextExWorker.
//
// Port of Wine/ReactOS DrawTextExWorker from
// win32ss/user/rtl/text.c, whose ReactOS version carries GPL-2.0-or-later.
func DrawTextW(dc DrawTextMeasurer, text []rune, r *Rect, flags uint32) int32 {
	if r == nil || dc == nil {
		return 0
	}

	lineH := dc.FontHeight()
	if lineH <= 0 {
		lineH = 16
	}

	// Strip accelerator prefix ('&') unless DT_NOPREFIX.
	display := text
	if flags&DT_NOPREFIX == 0 {
		display = stripPrefix(text)
	}

	if len(display) == 0 {
		if flags&DT_CALCRECT != 0 {
			r.Bottom = r.Top + lineH
		}
		return lineH
	}

	// Build list of lines.
	var lines [][]rune
	if flags&DT_SINGLELINE != 0 {
		lines = [][]rune{display}
	} else {
		lines = splitLines(dc, display, r.Width(), flags)
	}

	totalH := int32(len(lines)) * lineH

	if flags&DT_CALCRECT != 0 {
		// Compute the maximum line width and adjust rect.
		maxW := int32(0)
		for _, ln := range lines {
			w, _ := dc.TextExtent(string(ln))
			if w > maxW {
				maxW = w
			}
		}
		r.Right = r.Left + maxW
		r.Bottom = r.Top + totalH
		return totalH
	}

	// Vertical alignment.
	y := r.Top
	if flags&DT_VCENTER != 0 && flags&DT_SINGLELINE != 0 {
		y = r.Top + (r.Height()-totalH)/2
	} else if flags&DT_BOTTOM != 0 && flags&DT_SINGLELINE != 0 {
		y = r.Bottom - totalH
	}

	for _, ln := range lines {
		lineW, _ := dc.TextExtent(string(ln))
		x := r.Left
		if flags&DT_CENTER != 0 {
			x = r.Left + (r.Width()-lineW)/2
		} else if flags&DT_RIGHT != 0 {
			x = r.Right - lineW
		}
		dc.TextOut(x, y, string(ln))
		y += lineH
	}

	return totalH
}

// stripPrefix removes the first '&' that is not followed by another '&'.
func stripPrefix(runes []rune) []rune {
	out := make([]rune, 0, len(runes))
	for i := 0; i < len(runes); i++ {
		if runes[i] == '&' {
			if i+1 < len(runes) && runes[i+1] == '&' {
				out = append(out, '&')
				i++
			}
			// skip single '&'
		} else {
			out = append(out, runes[i])
		}
	}
	return out
}

// splitLines breaks text into lines respecting hard newlines and optionally
// word-wrapping to maxW.
func splitLines(dc DrawTextMeasurer, text []rune, maxW int32, flags uint32) [][]rune {
	var lines [][]rune
	start := 0
	for i := 0; i <= len(text); i++ {
		if i == len(text) || text[i] == '\n' || text[i] == '\r' {
			line := text[start:i]
			if i < len(text) && text[i] == '\r' && i+1 < len(text) && text[i+1] == '\n' {
				i++ // skip \r\n pair
			}
			if flags&DT_WORDBREAK != 0 && maxW > 0 {
				lines = append(lines, wordWrap(dc, line, maxW)...)
			} else {
				lines = append(lines, line)
			}
			start = i + 1
		}
	}
	return lines
}

// wordWrap breaks a single line into multiple lines that fit within maxW.
func wordWrap(dc DrawTextMeasurer, line []rune, maxW int32) [][]rune {
	if len(line) == 0 {
		return [][]rune{line}
	}
	w, _ := dc.TextExtent(string(line))
	if w <= maxW {
		return [][]rune{line}
	}
	// Binary-search for how many characters fit.
	lo, hi := 0, len(line)
	for lo < hi {
		mid := (lo + hi + 1) / 2
		w, _ := dc.TextExtent(string(line[:mid]))
		if w <= maxW {
			lo = mid
		} else {
			hi = mid - 1
		}
	}
	// Try to break at word boundary.
	breakAt := lo
	for breakAt > 0 && line[breakAt-1] != ' ' {
		breakAt--
	}
	if breakAt == 0 {
		breakAt = lo // no space found, hard break
	}
	rest := line[breakAt:]
	for len(rest) > 0 && rest[0] == ' ' {
		rest = rest[1:]
	}
	result := [][]rune{line[:breakAt]}
	return append(result, wordWrap(dc, rest, maxW)...)
}
