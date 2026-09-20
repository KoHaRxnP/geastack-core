// SPDX-License-Identifier: Apache-2.0
#include "internal.h"
#include "memory.h"
#include "refresh_perf.h"

#include "graphics/font.h"

#include <cstdio>
#include <cstring>

namespace gea::embedded::ui {

namespace {

constexpr int kScratchDepth = 16;

struct FlexLine {
	int start = 0;
	int count = 0;
	int mainSize = 0;
	int crossSize = 0;
};

class LayoutScratch {
public:
	static LayoutScratch &instance()
	{
		static LayoutScratch scratch;
		return scratch;
	}

	int enter()
	{
		const int current = depth_;
		depth_++;
		return current;
	}

	void leave()
	{
		if (depth_ > 0) depth_--;
	}

	int *childrenForDepth(int depth)
	{
		if (depth < 0 || depth >= kScratchDepth) return nullptr;
		if (!children_) children_ = static_cast<int *>(allocate(sizeof(int) * kScratchDepth * kMaxChildren));
		if (!children_) return nullptr;
		return children_ + depth * kMaxChildren;
	}

	FlexLine *linesForDepth(int depth)
	{
		if (depth < 0 || depth >= kScratchDepth) return nullptr;
		if (!lines_) lines_ = static_cast<FlexLine *>(allocate(sizeof(FlexLine) * kScratchDepth * kMaxFlexLines));
		if (!lines_) return nullptr;
		return lines_ + depth * kMaxFlexLines;
	}

private:
	static void *allocate(size_t size)
	{
		return gea::framework::memory::Allocator::allocatePreferSpiram(size);
	}

	int depth_ = 0;
	int *children_ = nullptr;
	FlexLine *lines_ = nullptr;
};

class ScratchFrame {
public:
	ScratchFrame()
		: scratch_(LayoutScratch::instance()), depth_(scratch_.enter())
	{
		children_ = scratch_.childrenForDepth(depth_);
		lines_ = scratch_.linesForDepth(depth_);
	}

	~ScratchFrame()
	{
		scratch_.leave();
	}

	bool valid() const
	{
		return depth_ >= 0 && depth_ < kScratchDepth && children_ && lines_;
	}

	int *children() const { return children_; }
	FlexLine *lines() const { return lines_; }

private:
	LayoutScratch &scratch_;
	int depth_ = 0;
	int *children_ = nullptr;
	FlexLine *lines_ = nullptr;
};

int resolvePercentSize(int basis, int percent)
{
	const long long numerator = static_cast<long long>(basis) * percent;
	return static_cast<int>((numerator + (numerator >= 0 ? 500 : -500)) / 1000);
}

bool hasExplicitWidth(const Node &node)
{
	return node.style.width != kUnset || node.style.width_percent != kUnset;
}

int16_t clampInt16(int value)
{
	if (value > 32767) return 32767;
	if (value < -32768) return -32768;
	return static_cast<int16_t>(value);
}

bool hasExplicitHeight(const Node &node)
{
	return node.style.height != kUnset || node.style.height_percent != kUnset;
}

int resolvedStyleWidth(const Node &node, int basis)
{
	if (node.style.width != kUnset) return node.style.width;
	if (node.style.width_percent != kUnset) return resolvePercentSize(basis, node.style.width_percent);
	return basis;
}

int resolvedStyleHeight(const Node &node, int basis)
{
	if (node.style.height != kUnset) return node.style.height;
	if (node.style.height_percent != kUnset) return resolvePercentSize(basis, node.style.height_percent);
	return basis;
}

class FlexLayoutPass {
public:
	FlexLayoutPass(LayoutEngine &engine, Node &node, int *children, int childCount, bool isRow, int mainAvail, int padWidth, int padHeight, FlexLine *lines, bool inlineRow = false)
		: engine_(engine),
		  node_(node),
		  children_(children),
		  childCount_(childCount),
		  isRow_(isRow),
		  inlineRow_(inlineRow && isRow),
		  mainAvail_(mainAvail),
		  padWidth_(padWidth),
		  padHeight_(padHeight),
		  lines_(lines)
	{
	}

	void measureChildren()
	{
		for (int i = 0; i < childCount_; i++) {
			const int child = children_[i];
			const int childAvailWidth = isRow_ ? mainAvail_ : padWidth_;
			const int childAvailHeight = isRow_ ? padHeight_ : mainAvail_;
			engine_.layoutNode(child, childAvailWidth, childAvailHeight);
			applyFlexBasis(child);
		}
	}

	// Flex line packing. An inline formatting context does NOT come through here —
	// layoutInlineFlow owns it — because flex lines cannot express one: their items
	// are atomic, so a text run may neither share a line with its neighbours nor
	// split across two.
	int buildLines()
	{
		int lineStart = 0;
		int lineMain = 0;
		int lineCross = 0;
		int gapCount = 0;
		int lineCount = 0;
		Node *nodes = Tree::instance().nodes();

		for (int i = 0; i < childCount_; i++) {
			const int child = children_[i];
			Node &childNode = nodes[child];
			const int childMain = isRow_
				? (childNode.layout.width + childNode.style.margin[1] + childNode.style.margin[3])
				: (childNode.layout.height + childNode.style.margin[0] + childNode.style.margin[2]);
			const int childCross = isRow_
				? (childNode.layout.height + childNode.style.margin[0] + childNode.style.margin[2])
				: (childNode.layout.width + childNode.style.margin[1] + childNode.style.margin[3]);

			const int withGap = gapCount > 0 ? node_.style.gap : 0;
			if (node_.style.flex_wrap && lineMain + childMain + withGap > mainAvail_ && gapCount > 0) {
				appendLine(lineCount, lineStart, gapCount, lineMain, lineCross);
				lineStart = i;
				lineMain = childMain;
				lineCross = childCross;
				gapCount = 1;
			} else {
				lineMain += childMain + withGap;
				if (childCross > lineCross) lineCross = childCross;
				gapCount++;
			}
		}

		if (gapCount > 0) appendLine(lineCount, lineStart, gapCount, lineMain, lineCross);
		return lineCount;
	}

	// Lays this node's children out as a sequence of LINE BOXES — a real inline
	// formatting context. Flex line packing cannot express one: it treats every
	// item as ATOMIC, so a text run that overflows the space left on a line is
	// pushed whole onto the next one and the line before it ends far short of the
	// right edge (typography's mixed-composition paragraphs lost ~40% of every
	// line to that). CSS instead lets a run START on the line box it inherits,
	// wrap its remainder at the block's left edge, and hand the pen to the next
	// box on its LAST line.
	//
	// One forward walk. Per line box it first SCANS ahead to learn which items
	// share it — and therefore the line's ascent, and so its baseline — then
	// places them. The scans partition the item list, so the pass is linear.
	void layoutInlineFlow(bool autosize)
	{
		Node *nodes = Tree::instance().nodes();
		const int contentW = mainAvail_ < 0 ? 0 : mainAvail_;
		const int contentLeft = node_.style.padding[3];
		const int contentTop = node_.style.padding[0];
		const int gapStyle = node_.style.gap;

		// CSS drops leading whitespace at the start of a LINE BOX. This block opens
		// one only when it is not itself an inline box that its parent may have
		// placed part-way along a line: `<p>…baseline.<span> Inline emphasis</span>…`
		// runs its own flow inside the span, and the space that separates it from
		// the text before it is content, not indentation.
		const bool blockOpensItsFirstLine = !LayoutEngine::isInlineLevelNode(node_);
		int i = 0;
		int lineTop = contentTop;
		int flowBottom = contentTop;
		int maxLineWidth = 0;
		// Carried across a wrapped run: the pen its last line ended at, that
		// line's already-fixed baseline, and the ascent/descent it contributes.
		int penStart = 0;
		bool continuing = false;
		int continuedBaseline = 0;
		int inheritedAscent = 0;
		int inheritedDescent = 0;

		while (i < childCount_) {
			// A line box that opens as the tail of a wrapped run already has content
			// even if no further item joins it.
			const bool lineIsContinuation = continuing;
			int pen = penStart;
			int lineAscent = inheritedAscent;
			int lineDescent = inheritedDescent;
			int j = i;
			int wrapIdx = -1;
			int wrapSpans = 0;
			int wrapAdvance = 0;
			int wrapLastWidth = 0;

			for (; j < childCount_; j++) {
				const int child = children_[j];
				Node &cn = nodes[child];
				const int marginL = cn.style.margin[3];
				const int marginR = cn.style.margin[1];
				const int gap = pen > 0 ? gapStyle : 0;

				// The run this item contributes to the line: itself when it IS a text
				// run, or the one inside a transparent <span> wrapper.
				const int innerRun = isFragmentableRun(cn) ? -1 : transparentInlineRun(cn);
				Node &subject = innerRun >= 0 ? nodes[innerRun] : cn;
				if (isFragmentableRun(subject)) {
					const int padH = subject.style.padding[1] + subject.style.padding[3];
					const int padV = subject.style.padding[0] + subject.style.padding[2];
					// Budgets are TEXT widths: the run's own padding is part of its box,
					// not of the line it can fill.
					const int rest = contentW - marginL - marginR - padH;
					const int firstAvail = rest - pen - gap;
					const bool atLineStart = pen == 0 && (j > 0 || blockOpensItsFirstLine);
					const InlineFlowMeasure m =
					    TextRenderer::measureInlineFlow(subject, firstAvail, rest, atLineStart);
					// Without real line metrics childFirstBaseline reports the box's
					// bottom edge, which is only a baseline for a ONE-line box. Such a
					// run keeps flowing whole, exactly as before.
					const bool haveLineMetrics =
					    m.lineAdvance > 0 &&
					    childFirstBaseline(subject) <= subject.style.padding[0] + m.lineAdvance;
					if (m.lineCount > 0 && haveLineMetrics) {
						// Its first WORD does not fit what is left: the run starts the
						// next line box rather than overflowing it or being cut open.
						if (pen > 0 && (m.firstLineWidth > firstAvail || m.firstLineForcedSplit)) break;
						const int boxH = m.lineCount * m.lineAdvance + padV;
						const bool wraps = m.lineCount > 1;
						const int boxW = wraps ? rest + padH : m.firstLineWidth + padH;
						const int indent =
						    (wraps ? pen + gap : 0) + m.firstLineIndentAdjust;
						subject.layout.height = clampInt16(boxH);
						subject.layout.width = clampInt16(boxW);
						subject.layout.inline_indent = clampInt16(indent);
						if (innerRun >= 0) {
							// The wrapper is transparent: it takes the run's box exactly, and
							// the run sits at its origin, so childFirstBaseline's recursion
							// through the wrapper still lands on the run's own baseline.
							subject.layout.x = 0;
							subject.layout.y = 0;
							cn.layout.height = clampInt16(boxH);
							cn.layout.width = clampInt16(boxW);
							cn.layout.inline_indent = clampInt16(indent);
						}
						if (wraps) {
							const int ascent = cn.style.margin[0] + childFirstBaseline(cn);
							const int descent = runLineDescent(subject, m.lineAdvance);
							if (ascent > lineAscent) lineAscent = ascent;
							if (descent > lineDescent) lineDescent = descent;
							pen += gap + marginL + m.firstLineWidth + marginR + padH;
							if (m.maxLineWidth + padH > maxLineWidth) maxLineWidth = m.maxLineWidth + padH;
							wrapIdx = j;
							wrapSpans = m.lineCount;
							wrapAdvance = m.lineAdvance;
							wrapLastWidth = m.lastLineWidth + padH;
							j++;
							break;  // the run closes this line box
						}
					}
				} else {
					cn.layout.inline_indent = 0;
				}

				const int itemMain = cn.layout.width + marginL + marginR;
				if (pen > 0 && pen + gap + itemMain > contentW) break;
				const int ascent = cn.style.margin[0] + childFirstBaseline(cn);
				const int descent = cn.layout.height + cn.style.margin[2] - childFirstBaseline(cn);
				if (ascent > lineAscent) lineAscent = ascent;
				if (descent > lineDescent) lineDescent = descent;
				pen += gap + itemMain;
			}

			if (pen > maxLineWidth) maxLineWidth = pen;

			const int baseline = continuing ? continuedBaseline : lineTop + lineAscent;
			lineTop = baseline - lineAscent;
			int lineCross = lineAscent + lineDescent;
			if (lineCross < 0) lineCross = 0;
			// The one case the flex path's expandCrossSizes is observable here: a
			// lone line box in a block with a definite height fills it, so
			// `align-self: center/end` on an inline child has room to work.
			if (i == 0 && j >= childCount_ && wrapIdx < 0 && hasExplicitHeight(node_) &&
			    padHeight_ > lineCross)
				lineCross = padHeight_;

			placeLineItems(nodes, i, j, contentLeft, contentW, lineTop, lineCross, baseline, penStart,
			               !lineIsContinuation);

			if (baseline + lineDescent > flowBottom) flowBottom = baseline + lineDescent;
			if (lineTop + lineCross > flowBottom) flowBottom = lineTop + lineCross;

			if (wrapIdx >= 0) {
				Node &run = nodes[children_[wrapIdx]];
				const int runBaseline = run.layout.y + childFirstBaseline(run);
				continuedBaseline = runBaseline + (wrapSpans - 1) * wrapAdvance;
				inheritedAscent = run.style.margin[0] + childFirstBaseline(run);
				inheritedDescent = runLineDescent(run, wrapAdvance);
				penStart = run.style.margin[3] + wrapLastWidth + run.style.margin[1];
				continuing = true;
				if (continuedBaseline + inheritedDescent > flowBottom)
					flowBottom = continuedBaseline + inheritedDescent;
				if (penStart > maxLineWidth) maxLineWidth = penStart;
			} else {
				lineTop = baseline + lineDescent + gapStyle;
				penStart = 0;
				continuing = false;
				inheritedAscent = 0;
				inheritedDescent = 0;
			}

			// A line box that took no item and inherited no run tail would spin. The
			// scan only produces that for an item wider than the whole box, so place
			// it anyway. A continuation line legitimately ends empty — its content is
			// the run's last line — and the item that did not fit opens the next one.
			if (j == i && !lineIsContinuation) j = i + 1;
			i = j;
		}

		if (!autosize) return;
		// Same rules as autosizeParent: a scroll container keeps the size it was
		// given on its scrolling axis so its overflow becomes scroll range.
		const int flowHeight = flowBottom - contentTop;
		const bool keepScrollHeight = scrollsOverflowY(node_.style) && node_.layout.height > 0;
		const bool keepScrollWidth = scrollsOverflowX(node_.style) && node_.layout.width > 0;
		if (!hasExplicitHeight(node_) && !keepScrollHeight) {
			const int height = (flowHeight < 0 ? 0 : flowHeight) + node_.style.padding[0] + node_.style.padding[2];
			node_.layout.height = clampInt16(engine_.clampSize(height, node_.style.min_height, node_.style.max_height));
		}
		if (!hasExplicitWidth(node_) && !keepScrollWidth) {
			const int width = maxLineWidth + node_.style.padding[1] + node_.style.padding[3];
			node_.layout.width = clampInt16(engine_.clampSize(width, node_.style.min_width, node_.style.max_width));
		}
	}

	// A text run whose line breaking this engine owns, and which therefore flows
	// into the block's line boxes instead of occupying one whole. An author-sized
	// or nowrap run keeps its own box.
	static bool isFragmentableRun(const Node &n)
	{
		return n.type == NodeType::Text && !n.text.empty() && n.style.white_space != 1 &&
		       n.style.width == kUnset && n.style.width_percent == kUnset;
	}

	// True when nothing about `n`'s BOX is visible — no background, border, shadow,
	// transform or blur. Such a wrapper can take whatever geometry inline flow finds
	// convenient, because only its content is ever painted.
	static bool paintsNoBox(const Node &n)
	{
		if (n.style.has_bg || n.style.border_width > 0 || hasSideBorder(n.style)) return false;
#if GEA_EMBEDDED_RARE_STYLE_INLINE
		const RareStyle &r = rstyle(n.style);
		if (r.transform_rotate || r.transform_rotate_x || r.transform_rotate_y ||
		    r.transform_translate_x || r.transform_translate_y || r.transform_translate_z ||
		    r.transform_translate_x_percent || r.transform_translate_y_percent ||
		    r.transform_scale_x != 1000 || r.transform_scale_y != 1000 || r.perspective ||
		    r.box_shadow_alpha || r.filter_blur_radius)
			return false;
#else
		// No rare-style record at all: no transform, shadow, gradient or filter was
		// ever set on this node.
		if (n.style.rare_style >= 0) return false;
#endif
		return true;
	}

	// A <span> that only wraps text is TRANSPARENT to inline flow: CSS breaks lines
	// inside it exactly as if its content sat directly in the paragraph, which is
	// why `<p>… <span>score value</span> …</p>` may put "score" on one line and
	// "value" on the next. Returns that inner run, or -1 when the wrapper has a box
	// a reader could see, or holds anything other than one plain text run.
	int transparentInlineRun(const Node &wrapper) const
	{
		if (wrapper.type != NodeType::View) return -1;
		if (!paintsNoBox(wrapper)) return -1;
		if (wrapper.style.padding[0] || wrapper.style.padding[1] || wrapper.style.padding[2] ||
		    wrapper.style.padding[3])
			return -1;
		if (hasExplicitWidth(wrapper) || hasExplicitHeight(wrapper)) return -1;
		if (wrapper.style.min_width > 0 || wrapper.style.min_height > 0) return -1;
		if (wrapper.style.max_width != kUnset || wrapper.style.max_height != kUnset) return -1;
		if (wrapper.style.overflow_x || wrapper.style.overflow_y) return -1;
		if (wrapper.style.align_self >= 0 || wrapper.style.flex > 0 ||
		    wrapper.style.flex_basis != kUnset)
			return -1;
		const Node *nodes = Tree::instance().nodes();
		int run = -1;
		for (int c = wrapper.first_child; c >= 0; c = nodes[c].next_sibling) {
			const Node &child = nodes[c];
			if (isDisplayNone(child.style) || child.style.position == 1) continue;
			if (run >= 0) return -1;  // more than one in-flow child
			if (child.type != NodeType::Text) return -1;
			if (child.style.margin[0] || child.style.margin[1] || child.style.margin[2] ||
			    child.style.margin[3])
				return -1;
			if (!isFragmentableRun(child)) return -1;
			run = c;
		}
		return run;
	}

	// Distance from ONE of a run's line baselines to the bottom of that line box,
	// including the run's own bottom padding/margin (which only the last line
	// actually carries — the others are flush, so this is the taller, safe value).
	int runLineDescent(const Node &n, int lineAdvance) const
	{
		const int ascender = childFirstBaseline(n) - n.style.padding[0];
		const int descent = lineAdvance - ascender + n.style.padding[2] + n.style.margin[2];
		return descent < 0 ? 0 : descent;
	}

	void placeLineItems(Node *nodes, int from, int to, int contentLeft, int contentW, int lineTop,
	                    int lineCross, int baseline, int penStart, bool ownsLineStart)
	{
		const bool soleRun =
		    ownsLineStart && (to - from) == 1 && nodes[children_[from]].type == NodeType::Text;
		// Anything sharing this line box is placed on its shared baseline, and must
		// be DRAWN there — see RenderState::inline_baseline. A run that has the line
		// to itself keeps the renderer's ink centring.
		const bool sharesLine = (to - from) > 1 || !ownsLineStart;
		int pen = penStart;
		for (int k = from; k < to; k++) {
			const int child = children_[k];
			Node &cn = nodes[child];
			const int marginL = cn.style.margin[3];
			const int marginR = cn.style.margin[1];
			const int gap = pen > 0 ? node_.style.gap : 0;

			if (cn.layout.inline_indent > 0) {
				// A wrapped continuation keeps a full-width box at the block's left
				// edge; its indent is what carries the pen it started from.
				cn.layout.x = clampInt16(contentLeft + marginL);
			} else {
				cn.layout.x = clampInt16(contentLeft + pen + gap + marginL);
			}
			pen += gap + marginL + cn.layout.width + marginR;

			if (soleRun && !hasExplicitWidth(cn)) {
				// text-align needs a box as wide as the line box to align inside.
				const int full = contentW - marginL - marginR;
				if (full > cn.layout.width) cn.layout.width = clampInt16(full);
			}

			int y = baseline - childFirstBaseline(cn);
			const int align = crossAlignFor(cn);
			if (align != 5) {
				const int crossBefore = cn.style.margin[0];
				const int crossAfter = cn.style.margin[2];
				const int crossTotal = cn.layout.height + crossBefore + crossAfter;
				y = lineTop + crossBefore;
				if (align == 1) y += (lineCross - crossTotal) / 2;
				else if (align == 2) y += lineCross - crossTotal;
				else if (align == 0 && !hasExplicitHeight(cn)) {
					cn.layout.height = clampInt16(lineCross - crossBefore - crossAfter);
					engine_.repositionChildren(child);
				}
			}
			cn.layout.y = clampInt16(y);
			cn.render.inline_baseline = sharesLine ? 1 : 0;
			// An inline wrapper (<span>text</span>) contributes its CHILD's glyphs to
			// this line box, so the flag has to reach the node that actually draws.
			if (cn.type == NodeType::View) {
				for (int c = cn.first_child; c >= 0; c = nodes[c].next_sibling)
					if (nodes[c].type == NodeType::Text) nodes[c].render.inline_baseline = sharesLine ? 1 : 0;
			}
		}
	}

	void expandCrossSizes(int lineCount, bool onlyIfDefinite)
	{
		const bool hasDefiniteCross = isRow_ ? hasExplicitHeight(node_) : hasExplicitWidth(node_);
		if (onlyIfDefinite && !hasDefiniteCross) return;

		const int crossAvail = isRow_ ? padHeight_ : padWidth_;
		if (lineCount == 1) {
			lines_[0].crossSize = crossAvail;
		} else if (lineCount > 1) {
			int totalLineCross = 0;
			for (int i = 0; i < lineCount; i++) totalLineCross += lines_[i].crossSize;
			const int extra = crossAvail - totalLineCross - (lineCount - 1) * node_.style.gap;
			if (extra > 0) {
				const int perLine = extra / lineCount;
				for (int i = 0; i < lineCount; i++) lines_[i].crossSize += perLine;
			}
		}
	}

	void positionLines(int lineCount)
	{
		Node *nodes = Tree::instance().nodes();
		int crossOffset = isRow_ ? node_.style.padding[0] : node_.style.padding[3];

		// Free main-axis space is measured against the node's FINAL padded main size
		// (set by autosizeParent before this runs), NOT the available space it was
		// offered. An auto-sized box shrinks to its content, so it has zero free
		// space — distributing `mainAvail_ - content` here would, e.g., center a
		// label using the parent's full width and then leave it stranded once the
		// box shrinks to fit. Reading layout.{width,height} also gives the correct
		// free space when a flex item was GROWN past its content by its parent.
		const int mainBox = isRow_
			? (node_.layout.width - node_.style.padding[1] - node_.style.padding[3])
			: (node_.layout.height - node_.style.padding[0] - node_.style.padding[2]);
		const int mainContentBox = mainBox < 0 ? 0 : mainBox;

		for (int lineIndex = 0; lineIndex < lineCount; lineIndex++) {
			FlexLine &line = lines_[lineIndex];
			int remaining = mainContentBox - line.mainSize;

			const int totalGrow = totalFlexGrowForLine(line, nodes);
			if (remaining > 0 && totalGrow > 0) {
				growFlexChildren(line, nodes, totalGrow, remaining);
				remaining = 0;
			} else if (remaining < 0) {
				// A container that scrolls along the main axis lets overflowing content
				// stay at its natural size and become scroll range instead of being
				// flex-shrunk to fit. Without this, a scrollable column (typography
				// specimen page) compresses every section into the viewport —
				// scroll_content_height never exceeds height and the page can't scroll.
				// (CSS floors shrink at min-content, which for stacked text is its
				// natural size; this engine has no min-content plumbing, so the scroll
				// container skips main-axis shrink entirely — the pre-flex-shrink
				// behavior for scrollable boxes.)
				const bool mainAxisScrolls =
					isRow_ ? scrollsOverflowX(node_.style) : scrollsOverflowY(node_.style);
				if (!mainAxisScrolls) {
					const int totalShrink = totalFlexShrinkForLine(line, nodes);
					if (totalShrink > 0) {
						shrinkFlexChildren(line, nodes, totalShrink, remaining);
						remaining = 0;
					}
				}
			} else if (totalGrow > 0) {
				// Free space is exactly zero, so no resize call runs — but the
				// flex items were zeroed for measurement (applyFlexBasis) and no
				// longer reposition there; give the still-zeroed ones their one
				// subtree layout at the final (zero) size.
				for (int i = 0; i < line.count; i++) {
					const int child = children_[line.start + i];
					Node &childNode = nodes[child];
					if (childNode.style.flex <= 0) continue;
					const int mainSize = isRow_ ? childNode.layout.width : childNode.layout.height;
					if (mainSize == 0) engine_.repositionChildren(child);
				}
			}
			if (remaining < 0 && node_.style.justify_content != 1 && node_.style.justify_content != 2) remaining = 0;

			int mainOffset = isRow_ ? node_.style.padding[3] : node_.style.padding[0];
			int gapSpace = 0;
			applyJustifyContent(line, remaining, &mainOffset, &gapSpace);
			positionLineChildren(line, nodes, mainOffset, crossOffset, gapSpace);
			crossOffset += line.crossSize + node_.style.gap;
		}
	}

	void autosizeParent(int lineCount)
	{
		// A scroll container keeps the size its parent already gave it (flex grow,
		// stretch, or a definite parent box in `layout.{height,width}` from
		// prepareOwnSize) instead of auto-growing to its content on the scrolling
		// axis. Otherwise scroll_content_{height,width} == layout.{height,width},
		// scrollMax is 0, and the container never scrolls. This matches CSS:
		// overflow:scroll/auto clips/scrolls its overflow, it does not expand to
		// fit content. Guarded on a positive assigned size so an unsized scroll
		// container still autosizes as before.
		const bool keepScrollHeight = scrollsOverflowY(node_.style) && node_.layout.height > 0;
		const bool keepScrollWidth = scrollsOverflowX(node_.style) && node_.layout.width > 0;

		if (!hasExplicitHeight(node_) && !keepScrollHeight) {
			const int height = (isRow_ ? totalCrossSize(lineCount) : maxMainSize(lineCount)) + node_.style.padding[0] + node_.style.padding[2];
			node_.layout.height = engine_.clampSize(height, node_.style.min_height, node_.style.max_height);
		}

		if (!hasExplicitWidth(node_) && !keepScrollWidth) {
			const int width = (isRow_ ? maxMainSize(lineCount) : totalCrossSize(lineCount)) + node_.style.padding[1] + node_.style.padding[3];
			node_.layout.width = engine_.clampSize(width, node_.style.min_width, node_.style.max_width);
		}
	}

private:
	void applyFlexBasis(int child)
	{
		Node &childNode = Tree::instance().nodes()[child];
		// A definite flex-basis (e.g. `flex: 0 0 26px`) sets the item's main-axis
		// size directly, overriding the size measured from content/fill. flex-grow
		// and flex-shrink then resize from this basis in positionLines, and a
		// flex:0 0 item stays exactly at its basis. This is what makes the weather
		// forecast's `.hour`/`.day` cards 26/40px wide instead of stretching to fill
		// the row. An explicit width/height still wins.
		if (childNode.style.flex_basis != kUnset) {
			if (isRow_) {
				if (hasExplicitWidth(childNode)) return;
				const int basis = clampFlexMainSize(childNode, childNode.style.flex_basis);
				if (childNode.layout.width != basis) {
					childNode.layout.width = basis;
					engine_.repositionChildren(child);
				}
			} else {
				if (hasExplicitHeight(childNode)) return;
				const int basis = clampFlexMainSize(childNode, childNode.style.flex_basis);
				if (childNode.layout.height != basis) {
					childNode.layout.height = basis;
					engine_.repositionChildren(child);
				}
			}
			return;
		}
		if (childNode.style.flex <= 0) return;
		if (isRow_) {
			if (hasExplicitWidth(childNode)) return;
			if (childNode.layout.width == 0) return;
			childNode.layout.width = 0;
		} else {
			if (hasExplicitHeight(childNode)) return;
			if (childNode.layout.height == 0) return;
			childNode.layout.height = 0;
		}
		// No repositionChildren here: the zeroed main size exists only for
		// buildLines' measurement, and growFlexChildren repositions the item
		// at its grown size moments later — recursively re-laying the subtree
		// at width/height 0 in between was the biggest source of redundant
		// layout work on nested-flex trees. The one case where no grow follows
		// (free space exactly 0) is compensated in positionLines.
	}

	void appendLine(int &lineCount, int start, int count, int mainSize, int crossSize)
	{
		if (lineCount >= kMaxFlexLines) return;
		lines_[lineCount].start = start;
		lines_[lineCount].count = count;
		lines_[lineCount].mainSize = mainSize;
		lines_[lineCount].crossSize = crossSize;
		lineCount++;
	}

	int totalFlexGrowForLine(const FlexLine &line, Node *nodes) const
	{
		int totalFlex = 0;
		for (int i = 0; i < line.count; i++) {
			totalFlex += nodes[children_[line.start + i]].style.flex;
		}
		return totalFlex;
	}

	int childMainSize(const Node &childNode) const
	{
		return isRow_ ? childNode.layout.width : childNode.layout.height;
	}

	// A text leaf's height IS its wrapped content height: the renderer draws
	// every wrapped line regardless of layout.height, so compressing it on a
	// column's main axis just makes the following sibling overlap the glyphs
	// (CSS floors flex shrink at the item's automatic minimum size — for text,
	// its min-content height). Treat column-axis text as non-shrinkable; the
	// deficit redistributes to shrinkable siblings or becomes overflow.
	bool childMainSizeIsShrinkable(const Node &childNode) const
	{
		if (childNode.style.flex_shrink <= 0) return false;
		if (!isRow_ && childNode.type == NodeType::Text) return false;
		return true;
	}

	int totalFlexShrinkForLine(const FlexLine &line, Node *nodes) const
	{
		int totalShrink = 0;
		for (int i = 0; i < line.count; i++) {
			const Node &childNode = nodes[children_[line.start + i]];
			if (!childMainSizeIsShrinkable(childNode)) continue;
			const int basis = childMainSize(childNode);
			if (basis <= 0) continue;
			totalShrink += childNode.style.flex_shrink * basis;
		}
		return totalShrink;
	}

	void growFlexChildren(const FlexLine &line, Node *nodes, int totalGrow, int delta)
	{
		if (totalGrow <= 0 || delta <= 0) return;
		int applied = 0;
		int lastGrowChild = -1;

		for (int i = 0; i < line.count; i++) {
			const int child = children_[line.start + i];
			Node &childNode = nodes[child];
			if (childNode.style.flex <= 0) continue;
			lastGrowChild = child;
			const int change = (delta * childNode.style.flex) / totalGrow;
			applied += applyFlexMainSizeDelta(child, childNode, change);
		}

		const int remainder = delta - applied;
		if (remainder > 0 && lastGrowChild >= 0) {
			Node &childNode = nodes[lastGrowChild];
			applyFlexMainSizeDelta(lastGrowChild, childNode, remainder);
		}
	}

	void shrinkFlexChildren(const FlexLine &line, Node *nodes, int totalShrink, int delta)
	{
		if (totalShrink <= 0 || delta >= 0) return;
		const int magnitude = -delta;
		int applied = 0;
		int lastShrinkChild = -1;

		for (int i = 0; i < line.count; i++) {
			const int child = children_[line.start + i];
			Node &childNode = nodes[child];
			if (!childMainSizeIsShrinkable(childNode)) continue;
			const int basis = childMainSize(childNode);
			if (basis <= 0) continue;
			lastShrinkChild = child;
			const int change = (magnitude * childNode.style.flex_shrink * basis) / totalShrink;
			applied += applyFlexMainSizeDelta(child, childNode, -change);
		}

		const int remainder = magnitude - applied;
		if (remainder > 0 && lastShrinkChild >= 0) {
			Node &childNode = nodes[lastShrinkChild];
			applyFlexMainSizeDelta(lastShrinkChild, childNode, -remainder);
		}
	}

	// CSS `min-width: auto` / `min-height: auto` — a flex item's AUTOMATIC MINIMUM
	// SIZE: an item may not be flex-shrunk below what its own in-flow content needs.
	// The engine already honours this for text leaves (see childMainSizeIsShrinkable);
	// a container has to be measured, because a line's whole deficit lands on however
	// few items are shrinkable. On the temperature dial the face column overflows by
	// ~49px and its fan pill (`height: 8.125vmin`) was the ONLY shrinkable item, so it
	// absorbed all of it, clamped to zero, and laid out 112x0 — the pill, its dot and
	// its label recorded no commands at all. CSS overflows the container instead.
	// Applies exactly where CSS applies it: no explicit min on that axis (an explicit
	// one already floors clampSize), and overflow visible on the main axis — a
	// scroll/hidden box really does have a zero automatic minimum.
	int automaticMinimumMainSize(const Node &childNode) const
	{
		const int explicitMin = isRow_ ? childNode.style.min_width : childNode.style.min_height;
		if (explicitMin > 0) return 0;
		if (isRow_ ? childNode.style.overflow_x : childNode.style.overflow_y) return 0;
		if (childNode.first_child < 0) return 0;
		const Node *nodes = Tree::instance().nodes();
		int extent = 0;
		for (int c = childNode.first_child; c >= 0; c = nodes[c].next_sibling) {
			const Node &grand = nodes[c];
			if (isDisplayNone(grand.style) || grand.style.position == 1) continue;
			// layout.{x,y} are still parent-relative here: resolveAbsoluteCoords runs
			// after the whole layout pass.
			const int end = isRow_
				? grand.layout.x + grand.layout.width + grand.style.margin[1]
				: grand.layout.y + grand.layout.height + grand.style.margin[2];
			if (end > extent) extent = end;
		}
		if (extent <= 0) return 0;
		extent += isRow_ ? childNode.style.padding[1] : childNode.style.padding[2];
		return extent;
	}

	int applyFlexMainSizeDelta(int child, Node &childNode, int delta)
	{
		if (delta == 0) return 0;
		const int before = isRow_ ? childNode.layout.width : childNode.layout.height;
		int after = before + delta;
		after = clampFlexMainSize(childNode, after);
		if (delta < 0) {
			// Floor the shrink at the automatic minimum size, and never let that floor
			// GROW an item that was already smaller than its content.
			const int autoMin = automaticMinimumMainSize(childNode);
			const int minAllowed = autoMin < before ? autoMin : before;
			if (after < minAllowed) after = minAllowed;
		}
		if (after == before) return 0;
		if (isRow_) childNode.layout.width = after;
		else childNode.layout.height = after;
		engine_.repositionChildren(child);
		return delta > 0 ? after - before : before - after;
	}

	int clampFlexMainSize(const Node &childNode, int size) const
	{
		const int minSize = isRow_ ? childNode.style.min_width : childNode.style.min_height;
		const int maxSize = isRow_ ? childNode.style.max_width : childNode.style.max_height;
		const int clamped = engine_.clampSize(size, minSize, maxSize);
		return clamped < 0 ? 0 : clamped;
	}

	void applyJustifyContent(const FlexLine &line, int remaining, int *mainOffset, int *gapSpace) const
	{
		switch (node_.style.justify_content) {
		case 0:
			break;
		case 1:
			*mainOffset += remaining / 2;
			break;
		case 2:
			*mainOffset += remaining;
			break;
		case 3:
			if (line.count > 1) *gapSpace = remaining / (line.count - 1);
			break;
		case 4:
			if (line.count > 0) {
				const int space = remaining / line.count;
				*mainOffset += space / 2;
				*gapSpace = space;
			}
			break;
		}
	}

	void positionLineChildren(const FlexLine &line, Node *nodes, int mainOffset, int crossOffset, int gapSpace)
	{
		// First baselines of this line's baseline-aligned children (rows only):
		// every such child's baseline lands on the line's max baseline, matching
		// CSS `align-items: baseline` (a 12px range label shares the 14px
		// condition's baseline instead of top-aligning).
		int lineMaxBaseline = 0;
		if (isRow_) {
			for (int i = 0; i < line.count; i++) {
				Node &probe = nodes[children_[line.start + i]];
				if (crossAlignFor(probe) != 5) continue;
				const int baseline = probe.style.margin[0] + childFirstBaseline(probe);
				if (baseline > lineMaxBaseline) lineMaxBaseline = baseline;
			}
		}

		for (int i = 0; i < line.count; i++) {
			const int child = children_[line.start + i];
			Node &childNode = nodes[child];

			const int mainMarginBefore = isRow_ ? childNode.style.margin[3] : childNode.style.margin[0];
			const int mainMarginAfter = isRow_ ? childNode.style.margin[1] : childNode.style.margin[2];
			const int crossMarginBefore = isRow_ ? childNode.style.margin[0] : childNode.style.margin[3];
			const int crossMarginAfter = isRow_ ? childNode.style.margin[2] : childNode.style.margin[1];
			const int crossSize = isRow_ ? childNode.layout.height : childNode.layout.width;
			const int crossTotal = crossSize + crossMarginBefore + crossMarginAfter;

			mainOffset += mainMarginBefore;
			int crossPosition = crossOffset + crossMarginBefore;
			positionCrossAxisChild(child, childNode, line.crossSize, crossMarginBefore, crossMarginAfter, crossTotal, &crossPosition, lineMaxBaseline);

			if (isRow_) {
				childNode.layout.x = mainOffset;
				childNode.layout.y = crossPosition;
			} else {
				childNode.layout.x = crossPosition;
				childNode.layout.y = mainOffset;
			}

			const int mainSize = isRow_ ? childNode.layout.width : childNode.layout.height;
			mainOffset += mainSize + mainMarginAfter + node_.style.gap + gapSpace;
			// Flex items own their boxes; nothing here shares a line box.
			childNode.render.inline_baseline = 0;
		}
	}

	// Cross-axis alignment in force for one item. `align-self` always wins.
	// Otherwise an INLINE formatting row aligns on the shared baseline — that is
	// what an inline formatting context does, and `align-items` has no effect on
	// a block container, so the parent's value is not consulted there. A real
	// flex container keeps reading `align-items`.
	int crossAlignFor(const Node &childNode) const
	{
		if (childNode.style.align_self >= 0) return childNode.style.align_self;
		return inlineRow_ ? 5 : node_.style.align_items;
	}

	void positionCrossAxisChild(int child, Node &childNode, int lineCrossSize, int crossMarginBefore, int crossMarginAfter, int crossTotal, int *crossPosition, int lineMaxBaseline)
	{
		const int align = crossAlignFor(childNode);
		switch (align) {
			case 0:
			if (isRow_) {
				if (!hasExplicitHeight(childNode)) {
					childNode.layout.height = lineCrossSize - crossMarginBefore - crossMarginAfter;
					engine_.repositionChildren(child);
				}
			} else if (!hasExplicitWidth(childNode)) {
				childNode.layout.width = lineCrossSize - crossMarginBefore - crossMarginAfter;
				engine_.repositionChildren(child);
			}
			break;
		case 1:
			*crossPosition += (lineCrossSize - crossTotal) / 2;
			break;
		case 2:
			*crossPosition += lineCrossSize - crossTotal;
			break;
		case 5:
			// align-items/align-self: baseline (rows). Columns fall back to start,
			// matching CSS, where baseline alignment only applies across an inline axis.
			if (isRow_) {
				const int shift = lineMaxBaseline - (crossMarginBefore + childFirstBaseline(childNode));
				if (shift > 0) *crossPosition += shift;
			}
			break;
		}
	}

	// First-baseline of a flex child, measured from its border-box top. Text
	// leaves use the resolved atlas font's ascender — the glyph rasterizer
	// places runs at `top + ascender - bearingY`, so this matches the drawn
	// pixels exactly. Anything without measurable text synthesizes its baseline
	// from the bottom margin edge, as CSS does for replaced/empty boxes.
	int childFirstBaseline(const Node &childNode) const
	{
		if (childNode.type == NodeType::Text && childNode.style.font_id >= 0) {
			const int fontSize = childNode.style.font_size > 0 ? childNode.style.font_size : 16;
			const gea::framework::graphics::RasterizedFont font =
			    gea::framework::graphics::FontRegistry::rasterizedFamily(childNode.style.font_id, fontSize);
			if (font.valid()) {
				// CSS half-leading: when line-height exceeds the font's own line box the
				// extra room is split above and below it, so the glyphs — and the
				// baseline with them — sit that much lower. TextDrawer applies exactly
				// this offset when it paints (`lineBoxOffset`), and a baseline that
				// ignores it is wrong by half the difference. That went unnoticed while
				// every run occupied its own line; once runs of different FONTS share
				// one, each carries a different font line height and they visibly
				// stagger (typography's ", a" riding above the Inter and Oswald spans
				// around it).
				const int lineAdvance =
				    childNode.style.line_height > 0 ? childNode.style.line_height : font.lineHeight();
				const int halfLeading =
				    childNode.style.line_height > 0 ? (lineAdvance - font.lineHeight()) / 2 : 0;
				return childNode.style.padding[0] + halfLeading + font.ascender();
			}
		}
		// An inline wrapper (a <span> around text) carries no text of its own, so
		// CSS takes the baseline of its FIRST in-flow line box. Its children were
		// already positioned by measureChildren, and layout.y is still parent-
		// relative at this point, so the child's own offset inside the wrapper is
		// exactly its baseline's distance from the wrapper's top. Without this a
		// span-wrapped run reported its bottom margin edge as its baseline and
		// dropped below every bare text run beside it.
		if (childNode.type == NodeType::View && childNode.first_child >= 0) {
			const Node *nodes = Tree::instance().nodes();
			for (int c = childNode.first_child; c >= 0; c = nodes[c].next_sibling) {
				const Node &inner = nodes[c];
				if (isDisplayNone(inner.style) || inner.style.position == 1) continue;
				return inner.layout.y + childFirstBaseline(inner);
			}
		}
		return childNode.layout.height + childNode.style.margin[2];
	}

	int totalCrossSize(int lineCount) const
	{
		int total = 0;
		for (int i = 0; i < lineCount; i++) total += lines_[i].crossSize + (i > 0 ? node_.style.gap : 0);
		return total;
	}

	int maxMainSize(int lineCount) const
	{
		int max = 0;
		for (int i = 0; i < lineCount; i++) {
			if (lines_[i].mainSize > max) max = lines_[i].mainSize;
		}
		return max;
	}

	LayoutEngine &engine_;
	Node &node_;
	int *children_ = nullptr;
	int childCount_ = 0;
	bool isRow_ = false;
	// True when this row is a synthesized INLINE FORMATTING context (a block
	// whose in-flow children are all inline-level), not a declared flex row.
	bool inlineRow_ = false;
	int mainAvail_ = 0;
	int padWidth_ = 0;
	int padHeight_ = 0;
	FlexLine *lines_ = nullptr;
};

// CSS inline-level classification. Browsers flow inline-level boxes (text,
// replaced <img>, and inline elements like <span>/<a>/<em>) horizontally on a
// line inside a block; gea has no separate inline formatting context, so a plain
// block whose in-flow children are ALL inline-level is laid out as a flex row
// (see LayoutNodePass::resolveRowDirection) to match that. A child the app gave
// display:flex/grid is its own formatting context, not inline.
static bool isInlineLevelTag(const char *tag)
{
	return std::strcmp(tag, "span") == 0 || std::strcmp(tag, "a") == 0 || std::strcmp(tag, "b") == 0 ||
	       std::strcmp(tag, "i") == 0 || std::strcmp(tag, "em") == 0 || std::strcmp(tag, "strong") == 0 ||
	       std::strcmp(tag, "small") == 0 || std::strcmp(tag, "label") == 0 || std::strcmp(tag, "code") == 0 ||
	       std::strcmp(tag, "u") == 0 || std::strcmp(tag, "sub") == 0 || std::strcmp(tag, "sup") == 0 ||
	       std::strcmp(tag, "mark") == 0;
}

class LayoutNodePass {
public:
	LayoutNodePass(LayoutEngine &engine, int id, int availWidth, int availHeight)
		: engine_(engine), id_(id), availWidth_(availWidth), availHeight_(availHeight), nodes_(Tree::instance().nodes()), node_(nodes_[id])
	{
	}

	void run()
	{
		if (isDisplayNone(node_.style)) return;
		prepareOwnSize();

		if (node_.type == NodeType::Text) {
			const std::int64_t __tmT0 = refreshPerfNowUs();
			TextRenderer::layout(id_, availWidth_);
			refreshPerfStatsMutable().treeLayoutTextUs += refreshPerfNowUs() - __tmT0;
			return;
		}

		if (node_.type == NodeType::Image && node_.image_id >= 0) {
			ImageRenderer::layout(id_);
			return;
		}

		if (node_.type == NodeType::View && std::strcmp(tagFromId(node_.tag_id), "input") == 0) {
			InputRenderer::layout(id_, availWidth_);
			return;
		}

		const int padWidth = paddedWidth();
		const int padHeight = paddedHeight();
		const bool inlineRow = resolveInlineFormattingRow();
		const bool isRow = inlineRow || resolveRowDirection();
		const int mainAvail = isRow ? padWidth : padHeight;

		ScratchFrame scratch;
		if (!scratch.valid()) return;

		const int childCount = engine_.collectChildren(id_, scratch.children(), kMaxChildren, true);
		if (childCount == 0) {
			autosizeEmptyNode();
			positionAbsoluteChildren();
			applyRelativeOffsets();
			applyVirtualListContentHeight();
			return;
		}

		if (isDisplayGrid(node_.style)) {
			layoutGridChildren(scratch.children(), childCount, padWidth, padHeight, true);
			updateScrollContentSize();
			positionAbsoluteChildren();
			applyRelativeOffsets();
			applyVirtualListContentHeight();
			return;
		}

		FlexLayoutPass flex(engine_, node_, scratch.children(), childCount, isRow, mainAvail, padWidth, padHeight, scratch.lines(), inlineRow);
		flex.measureChildren();
		if (inlineRow) {
			// An inline formatting context flows line boxes, not flex lines — see
			// FlexLayoutPass::layoutInlineFlow.
			flex.layoutInlineFlow(true);
		} else {
			const int lineCount = flex.buildLines();
			flex.expandCrossSizes(lineCount, true);
			// Finalize this node's own size BEFORE positioning its children: positionLines
			// measures free main-axis space against the node's final padded box, so an
			// auto-sized box must shrink to content first (otherwise children are placed
			// against the larger available space and stranded after the shrink).
			flex.autosizeParent(lineCount);
			flex.positionLines(lineCount);
		}
		updateScrollContentSize();
		positionAbsoluteChildren();
		applyRelativeOffsets();
		applyVirtualListContentHeight();
	}

	void repositionChildren()
	{
		if (node_.type == NodeType::Text || node_.type == NodeType::Image) return;

		const int padWidth = paddedWidth();
		const int padHeight = paddedHeight();
		const bool inlineRow = resolveInlineFormattingRow();
		const bool isRow = inlineRow || resolveRowDirection();
		const int mainAvail = isRow ? padWidth : padHeight;

		ScratchFrame scratch;
		if (!scratch.valid()) return;

		const int childCount = engine_.collectChildren(id_, scratch.children(), kMaxChildren, true);
		if (childCount == 0) {
			positionAbsoluteChildren();
			return;
		}

		if (isDisplayGrid(node_.style)) {
			layoutGridChildren(scratch.children(), childCount, padWidth, padHeight, false);
			positionAbsoluteChildren();
			applyRelativeOffsets();
			return;
		}

		FlexLayoutPass flex(engine_, node_, scratch.children(), childCount, isRow, mainAvail, padWidth, padHeight, scratch.lines(), inlineRow);
		flex.measureChildren();
		if (inlineRow) {
			flex.layoutInlineFlow(false);
		} else {
			const int lineCount = flex.buildLines();
			flex.expandCrossSizes(lineCount, false);
			flex.positionLines(lineCount);
		}
		positionAbsoluteChildren();
		applyRelativeOffsets();
	}

private:
	// Direction for FlexLayoutPass. Explicit flex-direction / display:flex win
	// (usesRowLayout). Otherwise a plain block whose in-flow children are all
	// inline-level flows them on a row (CSS inline formatting); a block with any
	// block-level in-flow child stacks them (column), as a browser would.
	bool resolveRowDirection() const
	{
		if (usesRowLayout(node_.style)) return true;
		return resolveInlineFormattingRow();
	}

	// True when the row direction is SYNTHESIZED from inline-level children —
	// gea's stand-in for an inline formatting context — rather than declared by
	// display:flex / flex-direction. Such a row behaves like a run of line boxes
	// (wraps, never flex-shrinks, aligns on the shared baseline), which is what
	// FlexLayoutPass's `inlineRow` flag switches on.
	bool resolveInlineFormattingRow() const
	{
		if (usesRowLayout(node_.style)) return false;
		if (node_.style.flex_direction_explicit || node_.style.display != kDisplayBlock) return false;
		bool anyInFlow = false;
		for (int c = node_.first_child; c >= 0; c = nodes_[c].next_sibling) {
			const Node &child = nodes_[c];
			if (child.style.display == kDisplayNone || child.style.position == 1) continue; // none / absolute
			if (!LayoutEngine::isInlineLevelNode(child)) return false;
			anyInFlow = true;
		}
		return anyInFlow;
	}

	int explicitGridColumnCount(int childCount) const
	{
		if (rstyle(node_.style).grid_column_count > 0) return rstyle(node_.style).grid_column_count;
		return childCount > 0 ? 1 : 0;
	}

	int explicitGridRowCount(int childCount, int columnCount) const
	{
		if (rstyle(node_.style).grid_row_count > 0) return rstyle(node_.style).grid_row_count;
		if (columnCount <= 0) return 0;
		int rows = (childCount + columnCount - 1) / columnCount;
		if (rows > kMaxGridLayoutTracks) rows = kMaxGridLayoutTracks;
		return rows;
	}

	void measureGridChildren(int *children, int childCount, int padWidth, int padHeight)
	{
		for (int i = 0; i < childCount; ++i) {
			(void)padHeight;
			engine_.layoutNode(children[i], padWidth, 0);
		}
	}

	void computeAutoTrackSizes(int *children,
	                           int childCount,
	                           int columnCount,
	                           int rowCount,
	                           int *autoColumnSizes,
	                           int *autoRowSizes)
	{
		for (int i = 0; i < kMaxGridLayoutTracks; ++i) {
			autoColumnSizes[i] = 0;
			autoRowSizes[i] = 0;
		}
		for (int i = 0; i < childCount; ++i) {
			const int column = columnCount > 0 ? i % columnCount : 0;
			const int row = columnCount > 0 ? i / columnCount : 0;
			if (column >= columnCount || row >= rowCount) continue;
			const Node &childNode = nodes_[children[i]];
			const int childWidth = childNode.layout.width + childNode.style.margin[1] + childNode.style.margin[3];
			const int childHeight = childNode.layout.height + childNode.style.margin[0] + childNode.style.margin[2];
			if (childWidth > autoColumnSizes[column]) autoColumnSizes[column] = childWidth;
			if (childHeight > autoRowSizes[row]) autoRowSizes[row] = childHeight;
		}
	}

	void computeTrackSizes(int count,
	                       const int8_t *types,
	                       const int16_t *values,
	                       const int *autoSizes,
	                       int available,
	                       int *outSizes)
	{
		int fixedTotal = 0;
		int totalFr = 0;
		for (int i = 0; i < count; ++i) {
			outSizes[i] = 0;
			if (types[i] == 1) {
				outSizes[i] = values[i];
				fixedTotal += outSizes[i];
			} else if (types[i] == 2) {
				totalFr += values[i] > 0 ? values[i] : 1;
			} else {
				outSizes[i] = autoSizes ? autoSizes[i] : 0;
				fixedTotal += outSizes[i];
			}
		}

		const int gapTotal = count > 1 ? (count - 1) * node_.style.gap : 0;
		int remaining = available - fixedTotal - gapTotal;
		if (remaining < 0) remaining = 0;

		int assignedFr = 0;
		for (int i = 0; i < count; ++i) {
			if (types[i] != 2) continue;
			const int fr = values[i] > 0 ? values[i] : 1;
			int size = totalFr > 0 ? remaining * fr / totalFr : 0;
			outSizes[i] = size;
			assignedFr += size;
		}
		if (totalFr > 0 && assignedFr < remaining) {
			for (int i = count - 1; i >= 0; --i) {
				if (types[i] == 2) {
					outSizes[i] += remaining - assignedFr;
					break;
				}
			}
		}
	}

	int gridTrackTotalSize(int count, const int *sizes) const
	{
		int total = 0;
		for (int i = 0; i < count; ++i) total += sizes[i] + (i > 0 ? node_.style.gap : 0);
		return total;
	}

	int gridContentOffset(int available, int total, int alignment, int paddingBefore) const
	{
		int free = available - total;
		if (free < 0 && alignment != 1 && alignment != 2) free = 0;
		if (alignment == 1) return paddingBefore + free / 2;
		if (alignment == 2) return paddingBefore + free;
		return paddingBefore;
	}

	void stretchGridChild(int child, Node &childNode, int cellWidth, int cellHeight, int alignX, int alignY)
	{
		bool resized = false;
		const int availableWidth = cellWidth - childNode.style.margin[1] - childNode.style.margin[3];
		const int availableHeight = cellHeight - childNode.style.margin[0] - childNode.style.margin[2];
		const int width = engine_.clampSize(availableWidth < 0 ? 0 : availableWidth, childNode.style.min_width, childNode.style.max_width);
		const int height = engine_.clampSize(availableHeight < 0 ? 0 : availableHeight, childNode.style.min_height, childNode.style.max_height);
		if (alignX == 0 && !hasExplicitWidth(childNode) && childNode.layout.width != width) {
			childNode.layout.width = width;
			resized = true;
		}
		if (alignY == 0 && !hasExplicitHeight(childNode) && childNode.layout.height != height) {
			childNode.layout.height = height;
			resized = true;
		}
		if (resized) engine_.repositionChildren(child);
	}

	void layoutGridChildren(int *children, int childCount, int padWidth, int padHeight, bool allowAutosizeParent)
	{
		if (childCount <= 0) return;
		int columnCount = explicitGridColumnCount(childCount);
		if (columnCount <= 0) return;
		if (columnCount > kMaxGridLayoutTracks) columnCount = kMaxGridLayoutTracks;
		int rowCount = explicitGridRowCount(childCount, columnCount);
		if (rowCount <= 0) return;
		if (rowCount > kMaxGridLayoutTracks) rowCount = kMaxGridLayoutTracks;

		measureGridChildren(children, childCount, padWidth, padHeight);

		int8_t columnTypes[kMaxGridLayoutTracks] = {0};
		int8_t rowTypes[kMaxGridLayoutTracks] = {0};
		int16_t columnValues[kMaxGridLayoutTracks] = {0};
		int16_t rowValues[kMaxGridLayoutTracks] = {0};
		for (int i = 0; i < columnCount; ++i) {
			columnTypes[i] = i < rstyle(node_.style).grid_column_count ? rstyle(node_.style).grid_column_type[i] : 0;
			columnValues[i] = i < rstyle(node_.style).grid_column_count ? rstyle(node_.style).grid_column_value[i] : 0;
		}
		for (int i = 0; i < rowCount; ++i) {
			rowTypes[i] = i < rstyle(node_.style).grid_row_count ? rstyle(node_.style).grid_row_type[i] : 0;
			rowValues[i] = i < rstyle(node_.style).grid_row_count ? rstyle(node_.style).grid_row_value[i] : 0;
		}

		int autoColumns[kMaxGridLayoutTracks] = {0};
		int autoRows[kMaxGridLayoutTracks] = {0};
		computeAutoTrackSizes(children, childCount, columnCount, rowCount, autoColumns, autoRows);

		int columnSizes[kMaxGridLayoutTracks] = {0};
		int rowSizes[kMaxGridLayoutTracks] = {0};
		computeTrackSizes(columnCount, columnTypes, columnValues, autoColumns, padWidth, columnSizes);
		computeTrackSizes(rowCount, rowTypes, rowValues, autoRows, padHeight, rowSizes);
		if (rstyle(node_.style).grid_column_count == 0 && columnCount == 1) columnSizes[0] = padWidth;

		if (allowAutosizeParent && !hasExplicitWidth(node_)) {
			int width = node_.style.padding[1] + node_.style.padding[3];
			for (int i = 0; i < columnCount; ++i) width += columnSizes[i] + (i > 0 ? node_.style.gap : 0);
			node_.layout.width = engine_.clampSize(width, node_.style.min_width, node_.style.max_width);
		}
		if (allowAutosizeParent && !hasExplicitHeight(node_)) {
			int height = node_.style.padding[0] + node_.style.padding[2];
			for (int i = 0; i < rowCount; ++i) height += rowSizes[i] + (i > 0 ? node_.style.gap : 0);
			node_.layout.height = engine_.clampSize(height, node_.style.min_height, node_.style.max_height);
		}

		int rowY[kMaxGridLayoutTracks] = {0};
		int columnX[kMaxGridLayoutTracks] = {0};
		const int columnTotal = gridTrackTotalSize(columnCount, columnSizes);
		const int rowTotal = gridTrackTotalSize(rowCount, rowSizes);
		columnX[0] = gridContentOffset(padWidth, columnTotal, node_.style.justify_content, node_.style.padding[3]);
		for (int i = 1; i < columnCount; ++i) columnX[i] = columnX[i - 1] + columnSizes[i - 1] + node_.style.gap;
		// A single implicit row (no grid-template-rows) is positioned by align-items,
		// not align-content, so place-items:center can vertically center a lone child
		// when the container is taller than its content (e.g. a position:absolute
		// inset:0 face). Without this the child pins to the top (align-content
		// defaults to start). Mirrors the single implicit column filling width above.
		const int rowBlockAlign = (rstyle(node_.style).grid_row_count == 0 && rowCount == 1)
		                              ? node_.style.align_items
		                              : node_.style.align_content;
		rowY[0] = gridContentOffset(padHeight, rowTotal, rowBlockAlign, node_.style.padding[0]);
		for (int i = 1; i < rowCount; ++i) rowY[i] = rowY[i - 1] + rowSizes[i - 1] + node_.style.gap;

		for (int i = 0; i < childCount; ++i) {
			const int column = i % columnCount;
			const int row = i / columnCount;
			if (row >= rowCount) break;
			const int child = children[i];
			Node &childNode = nodes_[child];
			const int cellWidth = columnSizes[column];
			const int cellHeight = rowSizes[row];
			engine_.layoutNode(child, cellWidth, cellHeight);

			const int alignX = childNode.style.align_self >= 0 ? childNode.style.align_self : node_.style.justify_items;
			const int alignY = childNode.style.align_self >= 0 ? childNode.style.align_self : node_.style.align_items;
			stretchGridChild(child, childNode, cellWidth, cellHeight, alignX, alignY);

			int x = columnX[column] + childNode.style.margin[3];
			int y = rowY[row] + childNode.style.margin[0];
			const int freeX = cellWidth - childNode.layout.width - childNode.style.margin[1] - childNode.style.margin[3];
			const int freeY = cellHeight - childNode.layout.height - childNode.style.margin[0] - childNode.style.margin[2];
			if (alignX == 1 && freeX > 0) x += freeX / 2;
			else if (alignX == 2 && freeX > 0) x += freeX;
			if (alignY == 1 && freeY > 0) y += freeY / 2;
			else if (alignY == 2 && freeY > 0) y += freeY;
			childNode.layout.x = x;
			childNode.layout.y = y;
		}
	}

	void prepareOwnSize()
	{
		int contentWidth = resolvedStyleWidth(node_, availWidth_);
		int contentHeight = resolvedStyleHeight(node_, availHeight_);
		if (!hasExplicitWidth(node_)) {
			contentWidth -= node_.style.margin[1] + node_.style.margin[3];
		}
		if (!hasExplicitHeight(node_)) {
			contentHeight -= node_.style.margin[0] + node_.style.margin[2];
		}
		contentWidth = engine_.clampSize(contentWidth, node_.style.min_width, node_.style.max_width);
		contentHeight = engine_.clampSize(contentHeight, node_.style.min_height, node_.style.max_height);

		node_.layout.width = contentWidth;
		node_.layout.height = contentHeight;
	}

	int paddedWidth() const
	{
		int width = node_.layout.width - node_.style.padding[1] - node_.style.padding[3];
		return width < 0 ? 0 : width;
	}

	int paddedHeight() const
	{
		int height = node_.layout.height - node_.style.padding[0] - node_.style.padding[2];
		return height < 0 ? 0 : height;
	}

	void autosizeEmptyNode()
	{
		if (!hasExplicitWidth(node_)) node_.layout.width = node_.style.padding[1] + node_.style.padding[3];
		if (!hasExplicitHeight(node_)) node_.layout.height = node_.style.padding[0] + node_.style.padding[2];
		node_.layout.width = engine_.clampSize(node_.layout.width, node_.style.min_width, node_.style.max_width);
		node_.layout.height = engine_.clampSize(node_.layout.height, node_.style.min_height, node_.style.max_height);
		// A <virtual-list>'s scroll geometry is owned by
		// applyVirtualListContentHeight() (invoked right after this in the
		// empty-node branch). Its recycled slot pool is entirely
		// position:absolute, so collectChildren() returns 0 and a scrollable
		// virtual-list lands here — resetting scroll_y/scroll_content_height
		// would wipe the scroll position on every relayout the moment the store
		// rewrites its slots.
		if (node_.type != NodeType::VirtualList) {
			node_.layout.scroll_content_width = node_.layout.width;
			node_.layout.scroll_content_height = node_.layout.height;
			node_.layout.scroll_x = 0;
			node_.layout.scroll_y = 0;
		}
	}

	void updateScrollContentSize()
	{
		// A <virtual-list>'s scroll geometry is owned by
		// applyVirtualListContentHeight() (scroll_content_height = itemCount *
		// rowHeight). Its real children are the recycled, position:absolute slot
		// pool, which this children-bottom walk skips — so running here would
		// collapse scroll_content_height to the node height, force maxScroll to
		// 0, and clamp scroll_y back to 0 on every relayout (wiping the scroll
		// position the moment the store rewrites its slots).
		if (node_.type == NodeType::VirtualList) return;
		int contentWidth = node_.layout.width;
		int contentHeight = node_.layout.height;
		for (int child = node_.first_child; child >= 0; child = nodes_[child].next_sibling) {
			Node &childNode = nodes_[child];
			if (isDisplayNone(childNode.style) || childNode.style.position == 1) continue;
			const int right = childNode.layout.x + childNode.layout.width + childNode.style.margin[1] + node_.style.padding[1];
			if (right > contentWidth) contentWidth = right;
			const int bottom = childNode.layout.y + childNode.layout.height + childNode.style.margin[2] + node_.style.padding[2];
			if (bottom > contentHeight) contentHeight = bottom;
		}
		node_.layout.scroll_content_width = contentWidth;
		node_.layout.scroll_content_height = contentHeight;

		int maxScrollX = node_.layout.scroll_content_width - node_.layout.width;
		if (maxScrollX < 0) maxScrollX = 0;
		if (!scrollsOverflowX(node_.style)) {
			node_.layout.scroll_x = 0;
		} else {
			if (node_.layout.scroll_x < 0) node_.layout.scroll_x = 0;
			if (node_.layout.scroll_x > maxScrollX) node_.layout.scroll_x = maxScrollX;
		}

		int maxScroll = node_.layout.scroll_content_height - node_.layout.height;
		if (maxScroll < 0) maxScroll = 0;
		if (!scrollsOverflowY(node_.style)) {
			node_.layout.scroll_y = 0;
		} else {
			if (node_.layout.scroll_y < 0) node_.layout.scroll_y = 0;
			if (node_.layout.scroll_y > maxScroll) node_.layout.scroll_y = maxScroll;
		}
	}

	// A <virtual-list> only materializes a small pool of real child slot nodes
	// but scrolls over a virtual content height of itemCount * rowHeight. The
	// row height comes from the first slot's resolved CSS height, so the app
	// controls it purely through the template's stylesheet.
	void applyVirtualListContentHeight()
	{
		if (node_.type != NodeType::VirtualList) return;
		int rowHeight = 0;
		for (int child = node_.first_child; child >= 0; child = nodes_[child].next_sibling) {
			if (isDisplayNone(nodes_[child].style)) continue;
			rowHeight = nodes_[child].layout.height;
			break;
		}
		int content = VirtualListRenderer::virtualContentHeight(id_, rowHeight);
		if (content < node_.layout.height) content = node_.layout.height;
		node_.layout.scroll_content_width = node_.layout.width;
		node_.layout.scroll_content_height = content;
		node_.layout.scroll_x = 0;
		int maxScroll = node_.layout.scroll_content_height - node_.layout.height;
		if (maxScroll < 0) maxScroll = 0;
		if (node_.layout.scroll_y < 0) node_.layout.scroll_y = 0;
		if (node_.layout.scroll_y > maxScroll) node_.layout.scroll_y = maxScroll;
	}

	void positionAbsoluteChildren()
	{
		for (int child = node_.first_child; child >= 0; child = nodes_[child].next_sibling) {
			Node &childNode = nodes_[child];
			if (isDisplayNone(childNode.style) || childNode.style.position != 1) continue;

			const int childAvailWidth = node_.layout.width;
			const int childAvailHeight = node_.layout.height;
			engine_.layoutNode(child, childAvailWidth, childAvailHeight);
			stretchAbsoluteChild(child, childNode, childAvailWidth, childAvailHeight);
			positionAbsoluteChild(childNode);
		}
	}

	void stretchAbsoluteChild(int child, Node &childNode, int childAvailWidth, int childAvailHeight)
	{
		bool resized = false;
		if (!hasExplicitWidth(childNode) &&
		    hasPositionOffset(childNode, 3) &&
		    hasPositionOffset(childNode, 1)) {
			int width = childAvailWidth - resolvedPositionOffset(childNode, 3) - resolvedPositionOffset(childNode, 1) -
			            childNode.style.margin[1] - childNode.style.margin[3];
			if (width < 0) width = 0;
			width = engine_.clampSize(width, childNode.style.min_width, childNode.style.max_width);
			if (childNode.layout.width != width) {
				childNode.layout.width = width;
				resized = true;
			}
		}

		if (!hasExplicitHeight(childNode) &&
		    hasPositionOffset(childNode, 0) &&
		    hasPositionOffset(childNode, 2)) {
			int height = childAvailHeight - resolvedPositionOffset(childNode, 0) - resolvedPositionOffset(childNode, 2) -
			             childNode.style.margin[0] - childNode.style.margin[2];
			if (height < 0) height = 0;
			height = engine_.clampSize(height, childNode.style.min_height, childNode.style.max_height);
			if (childNode.layout.height != height) {
				childNode.layout.height = height;
				resized = true;
			}
		}

		if (resized) engine_.repositionChildren(child);
	}

	void positionAbsoluteChild(Node &childNode)
	{
		if (hasPositionOffset(childNode, 3))
			childNode.layout.x = resolvedPositionOffset(childNode, 3);
		else if (hasPositionOffset(childNode, 1))
			childNode.layout.x = node_.layout.width - childNode.layout.width - resolvedPositionOffset(childNode, 1);
		else
			childNode.layout.x = alignedAbsoluteChildPosition(childNode, true);

		if (hasPositionOffset(childNode, 0))
			childNode.layout.y = resolvedPositionOffset(childNode, 0);
		else if (hasPositionOffset(childNode, 2))
			childNode.layout.y = node_.layout.height - childNode.layout.height - resolvedPositionOffset(childNode, 2);
		else
			childNode.layout.y = alignedAbsoluteChildPosition(childNode, false);
	}

	int alignedAbsoluteChildPosition(const Node &childNode, bool horizontal) const
	{
		const bool row = usesRowLayout(node_.style);
		const int crossAlign = childNode.style.align_self >= 0 ? childNode.style.align_self : node_.style.align_items;
		const int align = horizontal
			? (row ? node_.style.justify_content : crossAlign)
			: (row ? crossAlign : node_.style.justify_content);
		const int parentSize = horizontal ? node_.layout.width : node_.layout.height;
		const int childSize = horizontal ? childNode.layout.width : childNode.layout.height;
		const int beforeMargin = horizontal ? childNode.style.margin[3] : childNode.style.margin[0];
		const int afterMargin = horizontal ? childNode.style.margin[1] : childNode.style.margin[2];
		int available = parentSize;
		if (available < 0) available = 0;
		const int occupied = childSize + beforeMargin + afterMargin;
		int free = available - occupied;
		if (free < 0) free = 0;

		int position = beforeMargin;
		if (align == 1) position += free / 2;
		else if (align == 2) position += free;
		return position;
	}

	void applyRelativeOffsets()
	{
		for (int child = node_.first_child; child >= 0; child = nodes_[child].next_sibling) {
			Node &childNode = nodes_[child];
			if (childNode.style.position != 2) continue;
			if (hasPositionOffset(childNode, 0))
				childNode.layout.y += resolvedRelativePositionOffset(childNode, 0);
			else if (hasPositionOffset(childNode, 2))
				childNode.layout.y -= resolvedRelativePositionOffset(childNode, 2);
			if (hasPositionOffset(childNode, 3))
				childNode.layout.x += resolvedRelativePositionOffset(childNode, 3);
			else if (hasPositionOffset(childNode, 1))
				childNode.layout.x -= resolvedRelativePositionOffset(childNode, 1);
		}
	}

	bool hasPositionOffset(const Node &node, int side) const
	{
		return node.style.pos_offsets[side] != kUnset || node.style.pos_offset_percent[side] != kUnset;
	}

	int resolvedPositionOffset(const Node &node, int side) const
	{
		return resolvedPositionOffsetWithBasis(node, side, (side == 0 || side == 2) ? node_.layout.height : node_.layout.width);
	}

	int resolvedRelativePositionOffset(const Node &node, int side) const
	{
		int basis = (side == 0 || side == 2) ? node_.layout.height : node_.layout.width;
		if (isDisplayGrid(node_.style)) {
			basis = (side == 0 || side == 2) ? node.layout.height : node.layout.width;
		}
		return resolvedPositionOffsetWithBasis(node, side, basis);
	}

	int resolvedPositionOffsetWithBasis(const Node &node, int side, int percentBasis) const
	{
		int offset = node.style.pos_offsets[side] != kUnset ? node.style.pos_offsets[side] : 0;
		const int percent = node.style.pos_offset_percent[side];
		if (percent != kUnset) {
			const int numerator = percentBasis * percent;
			offset += (numerator + (numerator >= 0 ? 500 : -500)) / 1000;
		}
		return offset;
	}

	LayoutEngine &engine_;
	int id_ = 0;
	int availWidth_ = 0;
	int availHeight_ = 0;
	Node *nodes_ = nullptr;
	Node &node_;
};

bool establishesAbsoluteContainingBlock(const Node &node)
{
	return node.style.position == 1 || node.style.position == 2;
}

int containingBlockForAbsoluteNode(int node, int root, Node *nodes)
{
	for (int cursor = nodes[node].parent; cursor >= 0; cursor = nodes[cursor].parent) {
		if (establishesAbsoluteContainingBlock(nodes[cursor])) return cursor;
	}
	return root;
}

bool hasPositionOffsetValue(const Node &node, int side)
{
	return node.style.pos_offsets[side] != kUnset || node.style.pos_offset_percent[side] != kUnset;
}

int resolvedPositionOffsetForBasis(const Node &node, int side, int percentBasis)
{
	int offset = node.style.pos_offsets[side] != kUnset ? node.style.pos_offsets[side] : 0;
	const int percent = node.style.pos_offset_percent[side];
	if (percent != kUnset) {
		const int numerator = percentBasis * percent;
		offset += (numerator + (numerator >= 0 ? 500 : -500)) / 1000;
	}
	return offset;
}

void offsetFromAncestorToNode(int ancestor, int node, Node *nodes, int *outX, int *outY)
{
	int x = 0;
	int y = 0;
	for (int cursor = node; cursor >= 0 && cursor != ancestor; cursor = nodes[cursor].parent) {
		const Node &current = nodes[cursor];
		x += current.layout.x;
		y += current.layout.y;
		if (scrollsOverflowX(current.style)) x -= current.layout.scroll_x;
		if (scrollsOverflowY(current.style)) y -= current.layout.scroll_y;
	}
	*outX = x;
	*outY = y;
}

void stretchAbsoluteNodeToContainingBlock(int node, int containingWidth, int containingHeight, Node *nodes)
{
	Node &absolute = nodes[node];
	bool resized = false;
	if (!hasExplicitWidth(absolute) &&
	    hasPositionOffsetValue(absolute, 3) &&
	    hasPositionOffsetValue(absolute, 1)) {
		int width = containingWidth -
		            resolvedPositionOffsetForBasis(absolute, 3, containingWidth) -
		            resolvedPositionOffsetForBasis(absolute, 1, containingWidth) -
		            absolute.style.margin[1] - absolute.style.margin[3];
		if (width < 0) width = 0;
		width = LayoutEngine::instance().clampSize(width, absolute.style.min_width, absolute.style.max_width);
		if (absolute.layout.width != width) {
			absolute.layout.width = width;
			resized = true;
		}
	}

	if (!hasExplicitHeight(absolute) &&
	    hasPositionOffsetValue(absolute, 0) &&
	    hasPositionOffsetValue(absolute, 2)) {
		int height = containingHeight -
		             resolvedPositionOffsetForBasis(absolute, 0, containingHeight) -
		             resolvedPositionOffsetForBasis(absolute, 2, containingHeight) -
		             absolute.style.margin[0] - absolute.style.margin[2];
		if (height < 0) height = 0;
		height = LayoutEngine::instance().clampSize(height, absolute.style.min_height, absolute.style.max_height);
		if (absolute.layout.height != height) {
			absolute.layout.height = height;
			resized = true;
		}
	}

	if (resized) LayoutEngine::instance().repositionChildren(node);
}

void positionAbsoluteNodeInContainingBlock(int node, int containing, Node *nodes)
{
	Node &absolute = nodes[node];
	const Node &containingNode = nodes[containing];
	const int parent = absolute.parent;
	if (parent < 0) return;

	stretchAbsoluteNodeToContainingBlock(node, containingNode.layout.width, containingNode.layout.height, nodes);

	int parentOffsetX = 0;
	int parentOffsetY = 0;
	offsetFromAncestorToNode(containing, parent, nodes, &parentOffsetX, &parentOffsetY);

	int containingX = absolute.layout.x + parentOffsetX;
	if (hasPositionOffsetValue(absolute, 3))
		containingX = resolvedPositionOffsetForBasis(absolute, 3, containingNode.layout.width);
	else if (hasPositionOffsetValue(absolute, 1))
		containingX = containingNode.layout.width - absolute.layout.width -
		              resolvedPositionOffsetForBasis(absolute, 1, containingNode.layout.width);

	int containingY = absolute.layout.y + parentOffsetY;
	if (hasPositionOffsetValue(absolute, 0))
		containingY = resolvedPositionOffsetForBasis(absolute, 0, containingNode.layout.height);
	else if (hasPositionOffsetValue(absolute, 2))
		containingY = containingNode.layout.height - absolute.layout.height -
		              resolvedPositionOffsetForBasis(absolute, 2, containingNode.layout.height);

	absolute.layout.x = containingX - parentOffsetX;
	absolute.layout.y = containingY - parentOffsetY;
}

void resolveAbsoluteContainingBlocks(int root)
{
	Node *nodes = Tree::instance().nodes();
	const int nodeCount = Tree::instance().nodeCount();
	for (int node = 0; node < nodeCount; ++node) {
		if (isDisplayNone(nodes[node].style) || nodes[node].style.position != 1 || nodes[node].parent < 0) continue;
		const int containing = containingBlockForAbsoluteNode(node, root, nodes);
		if (containing < 0) continue;
		positionAbsoluteNodeInContainingBlock(node, containing, nodes);
	}
}

}  // namespace

LayoutEngine &LayoutEngine::instance()
{
	static LayoutEngine engine;
	return engine;
}

bool LayoutEngine::isInlineLevelNode(const Node &n)
{
	if (n.style.display == kDisplayFlex || n.style.display == kDisplayGrid || n.style.display == kDisplayNone)
		return false;
	// An explicit `display:block` makes even an inline-level tag (a <span>) — or a
	// Text/Image node — a BLOCK-LEVEL box, so it stacks vertically among siblings
	// instead of flowing into an inline row. Browsers do this; gea must too (e.g.
	// the weather city list's .city-name/.city-detail spans set display:block to
	// stack the name over the region). Without an explicit display, fall back to
	// the tag's intrinsic inline-ness.
	if (n.style.display == kDisplayBlock && n.style.display_explicit) return false;
	// In Gea's native layout model, vertical margins on inline-level wrappers are
	// used as an authoring signal that the wrapper occupies its own line. Keep
	// default spans inline, but do not merge title/score spans with margin-bottom
	// into one row.
	if (n.style.margin[0] != 0 || n.style.margin[2] != 0) return false;
	if (n.type == NodeType::Text || n.type == NodeType::Image) return true;
	return isInlineLevelTag(tagFromId(n.tag_id));
}

int LayoutEngine::clampSize(int size, int minSize, int maxSize) const
{
	if (minSize > 0 && size < minSize) size = minSize;
	if (maxSize != kUnset && size > maxSize) size = maxSize;
	return size;
}

int LayoutEngine::collectChildren(int parent, int *out, int max, bool skipAbs) const
{
	int n = 0;
	Node *nodes = Tree::instance().nodes();
	for (int child = nodes[parent].first_child; child >= 0; child = nodes[child].next_sibling) {
		if (isDisplayNone(nodes[child].style)) continue;
		if (skipAbs && nodes[child].style.position == 1) continue;
		if (n < max) out[n++] = child;
	}
	return n;
}

namespace {
// Intra-pass memo serial (see LayoutBox::memo_pass). 0 is never a valid pass
// so zero-initialized nodes can't produce a stale hit.
std::uint32_t gLayoutPassSerial = 1;
}  // namespace

void LayoutEngine::beginLayoutPass()
{
	if (++gLayoutPassSerial == 0) gLayoutPassSerial = 1;
}

void LayoutEngine::layoutNode(int id, int avail_w, int avail_h)
{
	Node &node = Tree::instance().nodes()[id];
	const bool memoizable = avail_w >= INT16_MIN && avail_w <= INT16_MAX &&
	                        avail_h >= INT16_MIN && avail_h <= INT16_MAX;
	if (memoizable) {
		if (node.layout.memo_pass == gLayoutPassSerial &&
		    node.layout.memo_avail_w == avail_w &&
		    node.layout.memo_avail_h == avail_h) {
			refreshPerfStatsMutable().treeLayoutMemoHits++;
			return;
		}
		if (node.layout.memo2_pass == gLayoutPassSerial &&
		    node.layout.memo2_avail_w == avail_w &&
		    node.layout.memo2_avail_h == avail_h &&
		    node.layout.memo2_result_w == node.layout.width &&
		    node.layout.memo2_result_h == node.layout.height) {
			// Promote the hit to the MRU slot.
			std::swap(node.layout.memo_avail_w, node.layout.memo2_avail_w);
			std::swap(node.layout.memo_avail_h, node.layout.memo2_avail_h);
			std::swap(node.layout.memo_result_w, node.layout.memo2_result_w);
			std::swap(node.layout.memo_result_h, node.layout.memo2_result_h);
			std::swap(node.layout.memo_pass, node.layout.memo2_pass);
			refreshPerfStatsMutable().treeLayoutMemoHits++;
			return;
		}
	}
	refreshPerfStatsMutable().treeLayoutNodeCalls++;
	LayoutNodePass(*this, id, avail_w, avail_h).run();
	node.layout.memo2_avail_w = node.layout.memo_avail_w;
	node.layout.memo2_avail_h = node.layout.memo_avail_h;
	node.layout.memo2_result_w = node.layout.memo_result_w;
	node.layout.memo2_result_h = node.layout.memo_result_h;
	node.layout.memo2_pass = node.layout.memo_pass;
	if (memoizable) {
		node.layout.memo_avail_w = static_cast<std::int16_t>(avail_w);
		node.layout.memo_avail_h = static_cast<std::int16_t>(avail_h);
		node.layout.memo_result_w = node.layout.width;
		node.layout.memo_result_h = node.layout.height;
		node.layout.memo_pass = gLayoutPassSerial;
	} else {
		node.layout.memo_pass = 0;
	}
}

void LayoutEngine::repositionChildren(int id)
{
	refreshPerfStatsMutable().treeLayoutRepositionCalls++;
	Node *node = &Tree::instance().nodes()[id];
	LayoutNodePass(*this, id, node->layout.width, node->layout.height).repositionChildren();
}

bool LayoutEngine::layoutNodeScoped(int scope, int treeRoot)
{
	Tree &tree = Tree::instance();
	Node *nodes = tree.nodes();
	const int nodeCount = tree.nodeCount();
	if (scope < 0 || scope >= nodeCount) return false;
	Node &a = nodes[scope];
	if (a.layout.memo_pass == 0) {
		refreshPerfStatsMutable().treeScopedRejectReason = 2;
		refreshPerfStatsMutable().treeScopedRejectNode = scope;
		return false;
	}

	auto inScope = [&](int node) {
		for (int c = node; c >= 0 && c < nodeCount; c = nodes[c].parent) {
			if (c == scope) return true;
		}
		return false;
	};

	// Absolute descendants positioned against a containing block OUTSIDE the
	// scope mix coordinate spaces (the global containing-block pass runs in
	// pre-resolve relative space) — those trees must take the full path.
	for (int i = 0; i < nodeCount; ++i) {
		const Node &n = nodes[i];
		if (n.parent < 0 || n.style.position != 1 || isDisplayNone(n.style)) continue;
		if (!inScope(i)) continue;
		const int containing = containingBlockForAbsoluteNode(i, treeRoot, nodes);
		if (containing >= 0 && !inScope(containing)) {
			refreshPerfStatsMutable().treeScopedRejectReason = 3;
			refreshPerfStatsMutable().treeScopedRejectNode = i;
			return false;
		}
	}

	const int prevX = a.layout.x;
	const int prevY = a.layout.y;
	const int prevW = a.layout.width;
	const int prevH = a.layout.height;
	beginLayoutPass();
	layoutNode(scope, a.layout.memo_avail_w, a.layout.memo_avail_h);
	const bool widthChanged = a.layout.width != prevW;
	const bool heightChanged = a.layout.height != prevH;
	if (widthChanged || heightChanged) {
		// layoutNode() derives an auto-sized scope from its content. Restore an
		// axis only when the parent genuinely owns that axis (stretch, grid,
		// flex-grow/basis) or the scope has an explicit size. Otherwise the new
		// intrinsic size must propagate through the parent: reject this scope so
		// refresh() retries at a stable ancestor. Treating every changed axis as
		// parent-owned kept a city chip at its placeholder width when "--" became
		// "13°", forcing flex-shrink to turn the shorter "Berlin" into "Ber…".
		bool parentOwnsWidth = hasExplicitWidth(a);
		bool parentOwnsHeight = hasExplicitHeight(a);
		if (a.parent >= 0 && a.parent < nodeCount) {
			const Node &parent = nodes[a.parent];
			if (isDisplayGrid(parent.style)) {
				const int alignX = a.style.align_self >= 0 ? a.style.align_self : parent.style.justify_items;
				const int alignY = a.style.align_self >= 0 ? a.style.align_self : parent.style.align_items;
				parentOwnsWidth = parentOwnsWidth || alignX == 0;
				parentOwnsHeight = parentOwnsHeight || alignY == 0;
			} else {
				const bool row = usesRowLayout(parent.style);
				const bool parentOwnsMain = a.style.flex > 0 || a.style.flex_basis != kUnset;
				const int crossAlign = a.style.align_self >= 0 ? a.style.align_self : parent.style.align_items;
				const bool parentOwnsCross = crossAlign == 0;
				parentOwnsWidth = parentOwnsWidth || (row ? parentOwnsMain : parentOwnsCross);
				parentOwnsHeight = parentOwnsHeight || (row ? parentOwnsCross : parentOwnsMain);
			}
		}
		if ((widthChanged && !parentOwnsWidth) || (heightChanged && !parentOwnsHeight)) {
			refreshPerfStatsMutable().treeScopedRejectReason = 4;
			refreshPerfStatsMutable().treeScopedRejectNode = scope;
			return false;
		}

		// A parent-owned box cannot be reproduced by laying only the child's
		// subtree. Put those dimensions back and position its descendants inside
		// the same box exactly as the omitted parent pass would.
		a.layout.width = static_cast<std::int16_t>(prevW);
		a.layout.height = static_cast<std::int16_t>(prevH);
		repositionChildren(scope);
	}
	a.layout.x = prevX;
	a.layout.y = prevY;

	// Scoped equivalent of the root containing-block pass (both the node and
	// its containing block are inside the scope, in the same relative space).
	for (int i = 0; i < nodeCount; ++i) {
		if (isDisplayNone(nodes[i].style) || nodes[i].style.position != 1 || nodes[i].parent < 0) continue;
		if (!inScope(i) || i == scope) continue;
		const int containing = containingBlockForAbsoluteNode(i, scope, nodes);
		if (containing < 0) continue;
		positionAbsoluteNodeInContainingBlock(i, containing, nodes);
	}

	int childParentX = a.layout.x;
	int childParentY = a.layout.y;
	if (a.style.overflow == 2) {
		if (scrollsOverflowX(a.style)) childParentX -= a.layout.scroll_x;
		if (scrollsOverflowY(a.style)) childParentY -= a.layout.scroll_y;
	}
	for (int child = a.first_child; child >= 0; child = nodes[child].next_sibling) {
		resolveAbsoluteCoords(child, childParentX, childParentY);
	}
	return true;
}

void LayoutEngine::resolveAbsoluteCoords(int id, int parent_x, int parent_y)
{
	Node *nodes = Tree::instance().nodes();
	Node *node = &nodes[id];
	if (node->parent < 0) resolveAbsoluteContainingBlocks(id);
	node->layout.x += parent_x;
	node->layout.y += parent_y;

	int childParentX = node->layout.x;
	int childParentY = node->layout.y;
	if (node->style.overflow == 2) {
		if (scrollsOverflowX(node->style)) childParentX -= node->layout.scroll_x;
		if (scrollsOverflowY(node->style)) childParentY -= node->layout.scroll_y;
	}

	for (int child = node->first_child; child >= 0; child = nodes[child].next_sibling) {
		resolveAbsoluteCoords(child, childParentX, childParentY);
	}
}

}  // namespace gea::embedded::ui
