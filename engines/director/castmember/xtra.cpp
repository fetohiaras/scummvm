/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "common/stream.h"
#include "common/util.h"
#include "graphics/macgui/mactext.h"

#include "director/director.h"
#include "director/cast.h"
#include "director/movie.h"
#include "director/channel.h"
#include "director/window.h"
#include "director/castmember/xtra.h"
#include "director/lingo/lingo-the.h"

namespace Director {

static bool isLikelyXmedTextByte(byte b) {
	return b == '\r' || b == '\n' || b == '\t' || (b >= 0x20 && b != 0x7f);
}

static bool looksLikeStructuredData(const Common::String &text) {
	return text.contains("[#") || text.contains("#num:") || text.contains("#card:");
}

static int scorePrintableRun(const Common::String &text) {
	int score = 0;

	for (uint i = 0; i < text.size(); i++) {
		byte c = (byte)text[i];
		if (Common::isAlpha(c))
			score += 3;
		else if (Common::isDigit(c))
			score += 1;
		else if (Common::isSpace(c))
			score += 1;
		else if (c == '.' || c == ',' || c == '!' || c == '?' || c == ':' || c == ';' || c == '"' || c == '-')
			score += 1;
	}

	if (looksLikeStructuredData(text))
		score -= 1000;

	if (text.size() > 256)
		score -= (text.size() - 256);

	return score;
}

static Common::String extractBestPrintableRun(const byte *data, uint32 size) {
	Common::String current;
	Common::String best;
	int bestScore = -1000000;

	for (uint32 i = 0; i < size; i++) {
		byte b = data[i];
		if (isLikelyXmedTextByte(b)) {
			if (b == '\r')
				current += '\n';
			else
				current += (char)b;
			continue;
		}

		if (current.size() >= 4) {
			int score = scorePrintableRun(current);
			if (score > bestScore) {
				bestScore = score;
				best = current;
			}
		}
		current.clear();
	}

	if (current.size() >= 4) {
		int score = scorePrintableRun(current);
		if (score > bestScore)
			best = current;
	}

	return best;
}

XtraCastMember::XtraCastMember(Cast *cast, uint16 castId, Common::SeekableReadStreamEndian &stream, uint16 version)
		: CastMember(cast, castId, stream) {
	_type = kCastXtra;
	_payloadKind = kPayloadUnknown;

	loadPayloadSummary();
}

XtraCastMember::XtraCastMember(Cast *cast, uint16 castId, XtraCastMember &source)
		: CastMember(cast, castId) {
	_type = kCastXtra;
	_payloadKind = source._payloadKind;
	_payloadText = source._payloadText;
	_initialRect = source._initialRect;
	_boundingRect = source._boundingRect;
	if (cast == source._cast)
		_children = source._children;
}

bool XtraCastMember::hasField(int field) {
	switch (field) {
	case kTheCuePointNames:		// D6
	case kTheCuePointTimes:		// D6
	case kTheCurrentTime:		// D6
	case kTheMediaBusy:			// D6, undocumented
		return true;
	default:
		break;
	}
	return CastMember::hasField(field);
}

Datum XtraCastMember::getField(int field) {
	Datum d;

	switch (field) {
	default:
		d = CastMember::getField(field);
		break;
	}

	return d;
}

void XtraCastMember::setField(int field, const Datum &d) {
	switch (field) {
	default:
		break;
	}

	CastMember::setField(field, d);
}

Common::String XtraCastMember::formatInfo() {
	return Common::String::format("Xtra kind: %d text: \"%s\"", (int)_payloadKind, Common::toPrintable(_payloadText).c_str());
}

uint32 XtraCastMember::getCastDataSize() {
	warning("XtraCastMember()::getCastDataSize(): CastMember version invalid or not handled");
	return 0;
}

void XtraCastMember::writeCastData(Common::SeekableWriteStream *writeStream) {
	warning("XtraCastMember()::writeCastData(): CastMember version invalid or not handled");
}

void XtraCastMember::loadPayloadSummary() {
	CastMemberInfo *info = getInfo();
	if (info) {
		Common::Rect infoRect(info->xtraRect.left, info->xtraRect.top, info->xtraRect.right, info->xtraRect.bottom);
		if (!infoRect.isEmpty())
			_initialRect = infoRect;
	}

	for (auto &it : _children) {
		if (it.tag != MKTAG('X', 'M', 'E', 'D'))
			continue;

		Common::SeekableReadStreamEndian *xmedData = _cast->getResource(it.tag, it.index);
		if (!xmedData)
			continue;

		Common::Array<byte> bytes;
		bytes.resize(xmedData->size());
		if (!bytes.empty())
			xmedData->read(&bytes[0], bytes.size());
		delete xmedData;

		if (bytes.size() >= 4 && !memcmp(&bytes[0], "PFR1", 4)) {
			_payloadKind = kPayloadFont;
			continue;
		}

		Common::String candidate = extractBestPrintableRun(bytes.empty() ? nullptr : &bytes[0], bytes.size());
		if (candidate.empty())
			continue;

		if (looksLikeStructuredData(candidate)) {
			if (_payloadKind == kPayloadUnknown)
				_payloadKind = kPayloadData;
			continue;
		}

		_payloadText = candidate;
		_payloadKind = kPayloadText;
		debugC(3, kDebugLoading, "XtraCastMember::loadPayloadSummary(): cast %d using XMED text '%s'", _castId, Common::toPrintable(candidate).c_str());
		return;
	}

	if (_payloadKind == kPayloadUnknown && debugChannelSet(2, kDebugLoading))
		debugC(2, kDebugLoading, "XtraCastMember::loadPayloadSummary(): cast %d has no renderable XMED text", _castId);
}

Graphics::MacWidget *XtraCastMember::createWidget(Common::Rect &bbox, Channel *channel, SpriteType spriteType) {
	if (_payloadKind != kPayloadText || _payloadText.empty())
		return nullptr;

	Common::Rect dims(bbox);
	if ((!dims.width() || !dims.height()) && !_initialRect.isEmpty()) {
		dims.right = dims.left + _initialRect.width();
		dims.bottom = dims.top + _initialRect.height();
	}

	if (!dims.width() || !dims.height())
		return nullptr;

	Graphics::MacFont macFont(Graphics::kMacFontSystem, 12);
	Common::U32String text = _cast->decodeString(_payloadText);
	Graphics::MacText *widget = new Graphics::MacText(
		g_director->getCurrentWindow()->getMacWindow(),
		dims.left, dims.top, dims.width(), dims.height(),
		g_director->_wm,
		text,
		&macFont,
		channel ? channel->_sprite->getForeColor() : g_director->_wm->_colorBlack,
		channel ? channel->_sprite->getBackColor() : g_director->_wm->_colorWhite,
		dims.width(),
		Graphics::kTextAlignLeft,
		0, 0, 0, 0, 0,
		true,
		false
	);
	return widget;
}

} // End of namespace Director
