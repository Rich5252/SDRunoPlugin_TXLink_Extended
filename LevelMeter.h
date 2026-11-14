#pragma once
// ---------------------------------------------------------------------------
// LevelMeter.h  -  horizontal segmented-LED level meter for Nana C++ (1.7.x)
//
// Header-only port of the TXcalUi C# LevelMeter control, sized for the
// SDRunoPlugin_TXLink form. Feed it a level in dB (default range -99..+3 dB).
//
//   * Smooth attack / decay on the lit bar (dB per second; 0 or less = instant).
//   * Peak hold marker: held for PeakHold ms, then falls back to the bar.
//   * Optional dB tick labels under the bar and a short caption (e.g. "dB"),
//     drawn with a built-in 3x5 pixel font (no Nana text calls, no Windows fonts).
//   * Green / yellow / red segments by dB threshold.
//
// THREADING - matches the plugin's "one form timer + queues" pattern:
//   SetLevel()  may be called from ANY thread. It only writes two atomics and
//               never touches Nana.
//   Tick()      must be called from the form (Nana) thread - call it once per
//               pass of SDRunoPlugin_TXLinkForm::OnTimer(). It runs the
//               animation and repaints only when the picture actually changes.
//   Everything else (setters, Show, ResetPeak...) is form-thread only.
//
// The meter has no timer of its own, so it adds no re-entrancy to the form.
// ---------------------------------------------------------------------------

#include <nana/gui.hpp>
#include <nana/gui/widgets/panel.hpp>
#include <nana/gui/drawing.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>

class LevelMeter
{
public:
	struct Rgb { int r, g, b; };

	LevelMeter(nana::window parent, const nana::rectangle& r, bool visible = true)
		: m_panel(parent, r, visible),
		  m_drawing(m_panel)
	{
		m_panel.bgcolor(ToColor(m_back));
		m_drawing.draw([this](nana::paint::graphics& g) { Paint(g); });
		m_lastTick = Clock::now();
		m_peakSet = m_lastTick;
	}

	LevelMeter(const LevelMeter&) = delete;
	LevelMeter& operator=(const LevelMeter&) = delete;

	// ------------------------------------------------------------------
	// Value
	// ------------------------------------------------------------------

	/// Latest level in dB. Safe from any thread. Out-of-range values are clamped
	/// when drawn; NaN and -infinity read as Minimum.
	void SetLevel(double db)
	{
		if (std::isnan(db)) db = -std::numeric_limits<double>::infinity();
		m_inputDb.store(db, std::memory_order_relaxed);

		// Remember the highest value fed in since the last Tick(), so a burst that
		// comes and goes between two timer passes still moves the peak marker.
		double seen = m_pendingMaxDb.load(std::memory_order_relaxed);
		while (db > seen &&
			!m_pendingMaxDb.compare_exchange_weak(seen, db, std::memory_order_relaxed))
		{
		}
		m_newInput.store(true, std::memory_order_relaxed);
	}

	double Level() const { return m_inputDb.load(std::memory_order_relaxed); }

	/// Run the animation and repaint if needed. Form thread only - call from OnTimer().
	void Tick()
	{
		const Clock::time_point now = Clock::now();
		double dt = std::chrono::duration<double>(now - m_lastTick).count();
		if (dt < m_minFrameSeconds) return;          // form timer runs faster than we need
		m_lastTick = now;
		if (dt > 0.25) dt = 0.25;                    // don't leap after a UI stall

		const double target = Clamp(m_inputDb.load(std::memory_order_relaxed));

		if (m_newInput.exchange(false, std::memory_order_relaxed))
		{
			const double burst = Clamp(m_pendingMaxDb.exchange(
				-std::numeric_limits<double>::infinity(), std::memory_order_relaxed));
			if (burst >= m_peakDb)
			{
				m_peakDb = burst;
				m_peakSet = now;                     // re-arm the hold time
			}
		}

		if (target > m_displayDb)
			m_displayDb = m_attackDbPerSec > 0 ? (std::min)(target, m_displayDb + m_attackDbPerSec * dt) : target;
		else if (target < m_displayDb)
			m_displayDb = m_decayDbPerSec > 0 ? (std::max)(target, m_displayDb - m_decayDbPerSec * dt) : target;

		if (m_peakDb < m_displayDb)
			m_peakDb = m_displayDb;                  // marker never sits below the bar
		else if (m_peakDb > m_displayDb &&
			std::chrono::duration<double, std::milli>(now - m_peakSet).count() > m_peakHoldMs)
			m_peakDb = (std::max)(m_displayDb, m_peakDb - m_peakFallDbPerSec * dt);

		// Only repaint when a segment actually changes state (or a setter asked for it).
		const int lit = LitCount();
		const int peak = PeakIndex();
		if (!m_dirty && lit == m_paintedLit && peak == m_paintedPeak) return;
		if (!m_panel.visible()) return;              // stays dirty; Nana repaints on show
		m_drawing.update();
	}

	/// Drop the peak marker back onto the bar.
	void ResetPeak()
	{
		m_peakDb = m_displayDb;
		m_peakSet = Clock::now();
		Redraw();
	}

	/// Empty the meter at once (e.g. when TX drops or the mode changes).
	void Reset()
	{
		const double floorDb = -std::numeric_limits<double>::infinity();
		m_inputDb.store(floorDb, std::memory_order_relaxed);
		m_pendingMaxDb.store(floorDb, std::memory_order_relaxed);
		m_newInput.store(false, std::memory_order_relaxed);
		m_displayDb = m_peakDb = m_minDb;
		Redraw();
	}

	// ------------------------------------------------------------------
	// Window
	// ------------------------------------------------------------------

	void Show(bool visible)
	{
		if (visible) { m_dirty = true; m_panel.show(); }
		else m_panel.hide();
	}
	bool Visible() const { return m_panel.visible(); }
	void Move(const nana::rectangle& r) { m_panel.move(r); Redraw(); }
	nana::panel<true>& Widget() { return m_panel; }  // for tooltips, events, etc.

	// ------------------------------------------------------------------
	// Appearance / behaviour (form thread only)
	// ------------------------------------------------------------------

	/// dB at the left (empty) and right (full) ends of the bar.
	void Range(double minDb, double maxDb)
	{
		if (!(maxDb > minDb)) return;
		m_minDb = minDb;
		m_maxDb = maxDb;
		m_displayDb = Clamp(m_displayDb);
		m_peakDb = Clamp(m_peakDb);
		Redraw();
	}

	/// Number of LED segments and the pixel gap between them (the gap is dropped
	/// automatically if the control is too narrow).
	void Segments(int count, int gapPx = 1)
	{
		m_segmentCount = (std::max)(1, (std::min)(400, count));
		m_segmentGap = (std::max)(0, (std::min)(10, gapPx));
		Redraw();
	}

	/// Segments at or above these levels are drawn in the mid / high colour.
	void Thresholds(double yellowDb, double redDb) { m_yellowDb = yellowDb; m_redDb = redDb; Redraw(); }

	void Colors(Rgb low, Rgb mid, Rgb high) { m_low = low; m_mid = mid; m_high = high; Redraw(); }
	void BackColor(Rgb c) { m_back = c; m_panel.bgcolor(ToColor(c)); Redraw(); }
	void TextColor(Rgb c) { m_fore = c; Redraw(); }

	/// Tick marks and dB labels under the bar. Off = the bar uses the full height.
	void ShowScale(bool show) { m_showScale = show; Redraw(); }

	/// Tick spacing in dB. Labels are thinned automatically if they would collide.
	void ScaleStep(double stepDb) { m_scaleStepDb = stepDb > 0 ? stepDb : 10.0; Redraw(); }

	/// Size multiplier for the built-in 3x5 pixel font (1 = 5 px high, 2 = 10 px high).
	void TextScale(int scale) { m_textScale = (std::max)(1, (std::min)(4, scale)); Redraw(); }

	/// Short caption (about 3 characters, e.g. "dB") at the right end of the label row.
	/// Available characters: 0-9, A-Z, lower-case d, and - + . %
	void Caption(const std::string& text) { m_caption = text; Redraw(); }

	void ShowPeakHold(bool show) { m_showPeak = show; Redraw(); }

	/// Hold time before the peak marker starts to fall, and how fast it then falls.
	void PeakHold(int holdMs, double fallDbPerSec = 20.0)
	{
		m_peakHoldMs = (std::max)(0, holdMs);
		m_peakFallDbPerSec = (std::max)(0.1, fallDbPerSec);
	}

	/// Bar rise / fall rates in dB per second. 0 or less makes that direction instant.
	void Ballistics(double attackDbPerSec, double decayDbPerSec)
	{
		m_attackDbPerSec = attackDbPerSec;
		m_decayDbPerSec = decayDbPerSec;
	}

	/// Upper limit on the repaint rate (default 33 frames per second).
	void MaxFrameRate(double fps) { m_minFrameSeconds = fps > 0 ? 1.0 / fps : 0.0; }

private:
	using Clock = std::chrono::steady_clock;

	static nana::color ToColor(Rgb c)
	{
		return nana::color(static_cast<unsigned>(c.r), static_cast<unsigned>(c.g), static_cast<unsigned>(c.b));
	}

	static Rgb Blend(Rgb a, Rgb b, double t)    // t = 0 gives a, 1 gives b
	{
		return Rgb{ static_cast<int>(a.r + (b.r - a.r) * t),
					static_cast<int>(a.g + (b.g - a.g) * t),
					static_cast<int>(a.b + (b.b - a.b) * t) };
	}

	double Clamp(double db) const
	{
		if (std::isnan(db) || db < m_minDb) return m_minDb;
		if (db > m_maxDb) return m_maxDb;
		return db;
	}

	void Redraw()
	{
		m_dirty = true;
		if (m_panel.visible()) m_drawing.update();
	}

	// Number of segments lit by the bar: a segment lights when the level reaches its centre.
	int LitCount() const
	{
		const double span = m_maxDb - m_minDb;
		const int n = m_segmentCount;
		const int lit = static_cast<int>(std::floor((m_displayDb - m_minDb) / span * n + 0.5));
		return (std::max)(0, (std::min)(n, lit));
	}

	// Segment carrying the peak marker, or -1 for none.
	int PeakIndex() const
	{
		const double span = m_maxDb - m_minDb;
		const int n = m_segmentCount;
		if (!m_showPeak || !(m_peakDb > m_minDb + span / n * 0.5)) return -1;
		const int idx = static_cast<int>(std::floor((m_peakDb - m_minDb) / span * n));
		return (std::max)(0, (std::min)(n - 1, idx));
	}

	void Paint(nana::paint::graphics& g)
	{
		m_dirty = false;
		const int lit = LitCount();
		const int peakIdx = PeakIndex();
		m_paintedLit = lit;
		m_paintedPeak = peakIdx;

		const int width = static_cast<int>(g.width());
		const int height = static_cast<int>(g.height());
		g.rectangle(true, ToColor(m_back));

		const bool hasText = !m_caption.empty();
		const bool labelLine = m_showScale || hasText;      // the row under the bar
		int textH = 0;
		int inset = 1;
		if (labelLine)
		{
			textH = 5 * m_textScale;
			if (m_showScale)
				inset = TextWidth("-99") / 2 + 2;       // room for the end labels to centre on their ticks
		}

		// Label row, top to bottom: 2 px of tick, 1 px gap, the text, 1 px margin.
		const int barTop = 1;
		const int barH = height - barTop - (labelLine ? textH + 4 : 1);
		const int barLeft = inset;
		const int barW = width - 2 * inset;
		if (barW < 4 || barH < 2) return;
		const int barBottom = barTop + barH;                // first row below the bar

		const double span = m_maxDb - m_minDb;
		const int n = m_segmentCount;
		int gap = m_segmentGap;
		double segW = (barW - gap * (n - 1)) / static_cast<double>(n);
		if (segW < 1.0)
		{
			gap = 0;
			segW = barW / static_cast<double>(n);
		}

		const nana::color litLow = ToColor(m_low), litMid = ToColor(m_mid), litHigh = ToColor(m_high);
		const nana::color dimLow = ToColor(Blend(m_low, m_back, 0.80));
		const nana::color dimMid = ToColor(Blend(m_mid, m_back, 0.80));
		const nana::color dimHigh = ToColor(Blend(m_high, m_back, 0.80));

		for (int i = 0; i < n; i++)
		{
			const double centre = m_minDb + span * (i + 0.5) / n;
			const bool on = i < lit || i == peakIdx;

			const nana::color* c;
			if (centre >= m_redDb) c = on ? &litHigh : &dimHigh;
			else if (centre >= m_yellowDb) c = on ? &litMid : &dimMid;
			else c = on ? &litLow : &dimLow;

			const int x0 = barLeft + static_cast<int>(std::lround(i * (segW + gap)));
			const int x1 = barLeft + static_cast<int>(std::lround(i * (segW + gap) + segW));
			g.rectangle(nana::rectangle(x0, barTop, static_cast<unsigned>((std::max)(1, x1 - x0)),
				static_cast<unsigned>(barH)), true, *c);
		}

		if (!labelLine) return;
		const int textY = barBottom + 3;
		const nana::color fore = ToColor(m_fore);

		// Caption sits on the label row, right-justified. Tick labels that would run
		// into it are dropped (their tick marks stay).
		int textLeft = (std::numeric_limits<int>::max)();
		if (hasText)
		{
			textLeft = (std::max)(0, width - 1 - TextWidth(m_caption));
			DrawText(g, textLeft, textY, m_caption, fore);
		}

		if (!m_showScale) return;

		// Label every k-th tick, where k is the smallest spacing that keeps labels apart.
		const double pxPerStep = m_scaleStepDb / span * barW;
		const int widest = (std::max)(
			TextWidth(std::to_string(static_cast<long>(std::lround(m_minDb)))),
			TextWidth(std::to_string(static_cast<long>(std::lround(m_maxDb)))));
		int every = 1;
		if (pxPerStep > 0.01)
			every = (std::max)(1, static_cast<int>(std::ceil((widest + 4) / pxPerStep)));

		const nana::color tickColor = ToColor(Blend(m_fore, m_back, 0.35));
		const long firstIdx = static_cast<long>(std::ceil(m_minDb / m_scaleStepDb - 1e-9));
		const long lastIdx = static_cast<long>(std::floor(m_maxDb / m_scaleStepDb + 1e-9));
		if (lastIdx - firstIdx > 2000) return;              // absurd step for this range

		for (long k = firstIdx; k <= lastIdx; k++)
		{
			const double db = k * m_scaleStepDb;
			const int x = barLeft + static_cast<int>(std::lround((db - m_minDb) / span * barW));
			g.line(nana::point(x, barBottom), nana::point(x, barBottom + 1), tickColor);

			if (k % every != 0) continue;                   // labels stay on round multiples
			const std::string text = std::to_string(static_cast<long>(std::lround(db)));
			const int w = TextWidth(text);
			const int left = x - w / 2;
			if (left + w + 2 >= textLeft) continue;         // would run into the caption
			DrawText(g, left, textY, text, fore);
		}
	}

	// ------------------------------------------------------------------
	// Built-in 3x5 pixel font. The meter draws its own text with filled rectangles
	// so it does not depend on Nana's text functions (whose signatures differ
	// between a pre-C++17 nana.lib and a C++17/20 project) or on Windows fonts.
	// ------------------------------------------------------------------
	struct Glyph { char ch; unsigned char w; unsigned char rows[5]; };

	static const Glyph* FindGlyph(char c)
	{
		static const Glyph table[] = {
			{'0',3,{7,5,5,5,7}}, {'1',3,{2,6,2,2,7}}, {'2',3,{7,1,7,4,7}}, {'3',3,{7,1,7,1,7}},
			{'4',3,{5,5,7,1,1}}, {'5',3,{7,4,7,1,7}}, {'6',3,{7,4,7,5,7}}, {'7',3,{7,1,1,1,1}},
			{'8',3,{7,5,7,5,7}}, {'9',3,{7,5,7,1,7}},
			{'-',2,{0,0,3,0,0}}, {'+',3,{0,2,7,2,0}}, {'.',1,{0,0,0,0,1}}, {'%',3,{5,1,2,4,5}},
			{' ',2,{0,0,0,0,0}}, {'d',3,{1,1,7,5,7}},
			{'A',3,{2,5,7,5,5}}, {'B',3,{6,5,6,5,6}}, {'C',3,{3,4,4,4,3}}, {'D',3,{6,5,5,5,6}},
			{'E',3,{7,4,6,4,7}}, {'F',3,{7,4,6,4,4}}, {'G',3,{3,4,5,5,3}}, {'H',3,{5,5,7,5,5}},
			{'I',3,{7,2,2,2,7}}, {'J',3,{1,1,1,5,2}}, {'K',3,{5,5,6,5,5}}, {'L',3,{4,4,4,4,7}},
			{'M',3,{5,7,7,5,5}}, {'N',3,{6,5,5,5,5}}, {'O',3,{2,5,5,5,2}}, {'P',3,{6,5,6,4,4}},
			{'Q',3,{2,5,5,6,3}}, {'R',3,{6,5,6,5,5}}, {'S',3,{3,4,2,1,6}}, {'T',3,{7,2,2,2,2}},
			{'U',3,{5,5,5,5,7}}, {'V',3,{5,5,5,5,2}}, {'W',3,{5,5,7,7,5}}, {'X',3,{5,5,2,5,5}},
			{'Y',3,{5,5,2,2,2}}, {'Z',3,{7,1,2,4,7}},
		};
		if (c >= 'a' && c <= 'z' && c != 'd') c = static_cast<char>(c - 'a' + 'A');
		for (const Glyph& gl : table)
			if (gl.ch == c) return &gl;
		return nullptr;                                     // unknown characters are skipped
	}

	int TextWidth(const std::string& text) const
	{
		int w = 0;
		for (char c : text)
			if (const Glyph* gl = FindGlyph(c)) w += gl->w + 1;
		return w > 0 ? (w - 1) * m_textScale : 0;
	}

	void DrawText(nana::paint::graphics& g, int x, int y, const std::string& text, const nana::color& colour) const
	{
		const int s = m_textScale;
		for (char c : text)
		{
			const Glyph* gl = FindGlyph(c);
			if (!gl) continue;
			for (int row = 0; row < 5; row++)
				for (int col = 0; col < gl->w; col++)
					if (gl->rows[row] & (1 << (gl->w - 1 - col)))
						g.rectangle(nana::rectangle(x + col * s, y + row * s,
							static_cast<unsigned>(s), static_cast<unsigned>(s)), true, colour);
			x += (gl->w + 1) * s;
		}
	}

	// --- widgets (order matters: the drawing attaches to the panel) ---
	nana::panel<true> m_panel;
	nana::drawing m_drawing;

	// --- written from any thread ---
	std::atomic<double> m_inputDb{ -std::numeric_limits<double>::infinity() };
	std::atomic<double> m_pendingMaxDb{ -std::numeric_limits<double>::infinity() };
	std::atomic<bool> m_newInput{ false };

	// --- form thread only ---
	double m_minDb = -99.0;
	double m_maxDb = 3.0;
	double m_displayDb = -99.0;     // smoothed value actually drawn
	double m_peakDb = -99.0;        // peak hold marker position
	Clock::time_point m_peakSet;
	Clock::time_point m_lastTick;
	double m_minFrameSeconds = 0.030;

	int m_segmentCount = 51;        // 51 over 102 dB = 2 dB per segment by default
	int m_segmentGap = 1;
	double m_yellowDb = -18.0;
	double m_redDb = -6.0;
	bool m_showScale = true;
	double m_scaleStepDb = 10.0;
	bool m_showPeak = true;
	int m_peakHoldMs = 1500;
	double m_peakFallDbPerSec = 20.0;
	double m_attackDbPerSec = 400.0;
	double m_decayDbPerSec = 60.0;

	Rgb m_low{ 0, 200, 70 };
	Rgb m_mid{ 240, 200, 0 };
	Rgb m_high{ 230, 40, 40 };
	Rgb m_back{ 24, 24, 24 };
	Rgb m_fore{ 192, 192, 192 };

	std::string m_caption;
	int m_textScale = 1;

	bool m_dirty = true;
	int m_paintedLit = -1;
	int m_paintedPeak = -2;
};
