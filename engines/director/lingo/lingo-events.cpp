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

#include "director/director.h"
#include "director/debugger.h"
#include "director/lingo/lingo.h"
#include "director/lingo/lingo-builtins.h"
#include "director/lingo/lingo-code.h"
#include "director/lingo/lingo-object.h"
#include "director/cast.h"
#include "director/castmember/castmember.h"
#include "director/castmember/script.h"
#include "director/channel.h"
#include "director/frame.h"
#include "director/movie.h"
#include "director/score.h"
#include "director/sprite.h"
#include "director/types.h"
#include "director/window.h"
#include "common/config-manager.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/str.h"

namespace Director {

static bool parseInitializerInteger(const Common::String &text, int &value) {
	char *end = nullptr;
	long parsed = strtol(text.c_str(), &end, 10);
	if (end == text.c_str())
		return false;
	while (end && *end && Common::isSpace(*end))
		++end;
	if (end && *end)
		return false;
	value = (int)parsed;
	return true;
}

static bool splitInitializerTopLevel(const Common::String &text, char separator, Common::Array<Common::String> &parts) {
	int depth = 0;
	uint start = 0;

	for (uint i = 0; i < text.size(); ++i) {
		char c = text[i];
		if (c == '[' || c == '(')
			++depth;
		else if (c == ']' || c == ')')
			--depth;
		else if (c == separator && depth == 0) {
			parts.push_back(text.substr(start, i - start));
			start = i + 1;
		}
	}

	if (depth != 0)
		return false;

	parts.push_back(text.substr(start));
	return true;
}

static void replaceAll(Common::String &source, const Common::String &what, const Common::String &with) {
	if (what.empty())
		return;

	uint32 pos = 0;
	while ((pos = source.find(what, pos)) != Common::String::npos) {
		source.replace(pos, what.size(), with);
		pos += with.size();
	}
}

static int findInitializerTopLevelColon(const Common::String &text) {
	int depth = 0;

	for (uint i = 0; i < text.size(); ++i) {
		char c = text[i];
		if (c == '[' || c == '(')
			++depth;
		else if (c == ']' || c == ')')
			--depth;
		else if (c == ':' && depth == 0)
			return i;
	}

	return -1;
}

static bool parseInitializerValue(const Common::String &rawValue, Datum &out) {
	Common::String value = rawValue;
	value.trim();

	if (value.empty()) {
		out = Datum(0);
		return true;
	}

	if (value.firstChar() == '(' && value.lastChar() == ')') {
		Common::String inner = value.substr(1, value.size() - 2);
		inner.trim();
		return parseInitializerValue(inner, out);
	}

	if (value.firstChar() == '"' && value.lastChar() == '"' && value.size() >= 2) {
		out = Datum(value.substr(1, value.size() - 2));
		return true;
	}

	if (value.firstChar() == '#') {
		out = Datum(value.substr(1));
		return true;
	}

	int intValue = 0;
	if (parseInitializerInteger(value, intValue)) {
		out = Datum(intValue);
		return true;
	}

	Common::String lower = value;
	lower.toLowercase();
	if (lower.hasPrefix("member ")) {
		int ofPos = lower.find(" of castlib ");
		if (ofPos > 7) {
			Common::String memberText = value.substr(7, ofPos - 7);
			Common::String castLibText = value.substr(ofPos + 12);
			memberText.trim();
			castLibText.trim();

			int memberId = 0;
			int castLibId = 0;
			if (parseInitializerInteger(memberText, memberId) && parseInitializerInteger(castLibText, castLibId)) {
				out = Datum(CastMemberID(memberId, castLibId));
				return true;
			}
		}
	}

	return false;
}

static bool parseBehaviorInitializerParams(const Common::String &rawParams, Datum &out) {
	Common::String params = rawParams;
	params.trim();

	if (params.empty()) {
		out.type = PARRAY;
		out.u.parr = new PArray;
		return true;
	}

	if (params.firstChar() != '[' || params.lastChar() != ']') {
		return false;
	}

	Common::String inner = params.substr(1, params.size() - 2);
	inner.trim();

	out.type = PARRAY;
	out.u.parr = new PArray;
	if (inner.empty())
		return true;

	Common::Array<Common::String> pairs;
	if (!splitInitializerTopLevel(inner, ',', pairs)) {
		return false;
	}

	for (const Common::String &rawPair : pairs) {
		Common::String pair = rawPair;
		pair.trim();
		if (pair.empty())
			continue;

		int colonPos = findInitializerTopLevelColon(pair);
		if (colonPos <= 0) {
			return false;
		}

		Common::String key = pair.substr(0, colonPos);
		Common::String value = pair.substr(colonPos + 1);
		key.trim();
		value.trim();

		if (key.firstChar() == '#')
			key = key.substr(1);

		Datum parsedValue;
		if (!parseInitializerValue(value, parsedValue)) {
			return false;
		}

		out.u.parr->arr.push_back(PCell(Datum(key), parsedValue));
	}

	return true;
}

static Common::FSNode findChildIgnoreCase(const Common::FSNode &dir, const Common::String &name, bool wantDirectory) {
	if (!dir.exists() || !dir.isDirectory())
		return Common::FSNode();

	Common::FSList children;
	if (!dir.getChildren(children, Common::FSNode::kListAll))
		return Common::FSNode();

	for (const Common::FSNode &child : children) {
		if (wantDirectory != child.isDirectory())
			continue;
		if (child.getName().equalsIgnoreCase(name))
			return child;
	}

	return Common::FSNode();
}

static Common::FSNode findMovieDumpDir(const Common::FSNode &dumpRoot, const Common::String &movieName) {
	Common::String baseName = movieName;
	if (baseName.hasSuffixIgnoreCase(".dxr") || baseName.hasSuffixIgnoreCase(".dir") || baseName.hasSuffixIgnoreCase(".dcr"))
		baseName = baseName.substr(0, baseName.size() - 4);

	Common::FSNode exact = findChildIgnoreCase(dumpRoot, baseName, true);
	if (exact.exists())
		return exact;

	Common::FSList children;
	if (!dumpRoot.getChildren(children, Common::FSNode::kListDirectoriesOnly))
		return Common::FSNode();

	for (const Common::FSNode &child : children) {
		if (child.getName().equalsIgnoreCase(movieName))
			return child;
		if (child.getName().hasPrefixIgnoreCase(baseName))
			return child;
	}

	return Common::FSNode();
}

static bool isLikelyShorthandAssignment(const Common::String &trimmedLine) {
	if (trimmedLine.empty())
		return false;
	if (trimmedLine.hasPrefix("--") || trimmedLine.hasPrefix("//"))
		return false;
	if (trimmedLine.hasPrefixIgnoreCase("on ") ||
		trimmedLine.hasPrefixIgnoreCase("end") ||
		trimmedLine.hasPrefixIgnoreCase("if ") ||
		trimmedLine.hasPrefixIgnoreCase("else") ||
		trimmedLine.hasPrefixIgnoreCase("repeat ") ||
		trimmedLine.hasPrefixIgnoreCase("case ") ||
		trimmedLine.hasPrefixIgnoreCase("otherwise") ||
		trimmedLine.hasPrefixIgnoreCase("global ") ||
		trimmedLine.hasPrefixIgnoreCase("property ") ||
		trimmedLine.hasPrefixIgnoreCase("return ") ||
		trimmedLine.hasPrefixIgnoreCase("exit") ||
		trimmedLine.hasPrefixIgnoreCase("set "))
		return false;

	int eqPos = trimmedLine.find('=');
	if (eqPos <= 0)
		return false;
	char prev = trimmedLine[eqPos - 1];
	char next = (eqPos + 1 < (int)trimmedLine.size()) ? trimmedLine[eqPos + 1] : '\0';
	if (prev == '<' || prev == '>' || next == '=')
		return false;

	return true;
}

static Common::String normalizeBehaviorSourceLingo(const Common::String &source) {
	Common::String normalized;
	uint start = 0;
	while (start <= source.size()) {
		uint end = start;
		while (end < source.size() && source[end] != '\n')
			end++;

		Common::String line = source.substr(start, end - start);
		Common::String trimmed = line;
		trimmed.trim();
		if (isLikelyShorthandAssignment(trimmed)) {
			uint indentLen = 0;
			while (indentLen < line.size() && (line[indentLen] == ' ' || line[indentLen] == '\t'))
				indentLen++;
			line = line.substr(0, indentLen) + "set " + trimmed;
		}

		normalized += line;
		if (end < source.size())
			normalized += "\n";

		if (end >= source.size())
			break;
		start = end + 1;
	}

	return normalized;
}

static bool readUtf8TextFile(const Common::FSNode &fileNode, Common::U32String &out) {
	Common::SeekableReadStream *stream = fileNode.createReadStream();
	if (!stream)
		return false;

	Common::String script = stream->readString(0, stream->size());
	delete stream;

	script = normalizeBehaviorSourceLingo(script);
	out = Common::U32String(script, Common::kUtf8);
	return true;
}

static void normalizeDumpedScoreScriptSource(uint16 memberId, Common::U32String &source) {
	Common::String patched = source.encode();

	if (memberId == 33) {
		replaceAll(patched, "    addAt(cardjog, a)", "    add(cardjog, a)");
		replaceAll(patched, "    addAt(cardcomp, a)", "    add(cardcomp, a)");
		replaceAll(patched, "  cardcomp.addAt(random(count(cardcomp)), 41)", "  addAt(cardcomp, random(count(cardcomp)), 41)");
		replaceAll(patched, "  cardcomp.addAt(random(count(cardcomp)), 42)", "  addAt(cardcomp, random(count(cardcomp)), 42)");
		replaceAll(patched, "  cardcomp.addAt(random(count(cardcomp)), 43)", "  addAt(cardcomp, random(count(cardcomp)), 43)");
		replaceAll(patched, "  cardcomp.addAt(random(count(cardcomp)), 44)", "  addAt(cardcomp, random(count(cardcomp)), 44)");
		replaceAll(patched, "  cardcomp.addAt(random(count(cardcomp)), 45)", "  addAt(cardcomp, random(count(cardcomp)), 45)");
	}

	if (memberId == 55) {
		replaceAll(patched,
			"  myImmediateJump = myImmediateJump = \"jump immediately\"",
			"  if myImmediateJump = \"jump immediately\" then\n    myImmediateJump = 1\n  else\n    myImmediateJump = 0\n  end if");
	}

	source = Common::U32String(patched);
}

static bool loadBuiltinBehaviorSource(Movie *movie, uint16 memberId, ScriptType scriptType) {
	Common::String script;

	switch (memberId) {
	case 37:
		script =
			"property myDummy\n"
			"\n"
			"on beginSprite me\n"
			"  set myDummy = 0\n"
			"end\n";
		break;

	case 55:
		script =
			"property myTimeOut, myTimeUnit, myTimeOutFrame, myImmediateJump, myStartFrame, myStartTicks, myInitialized\n"
			"\n"
			"on initializeLoop me\n"
			"  set myStartFrame = the frame\n"
			"  set myStartTicks = the ticks\n"
			"  if myTimeUnit = \"Seconds\" then\n"
			"    set myTimeOut = myTimeOut * 60\n"
			"  end if\n"
			"  if myTimeUnit = \"Minutes\" then\n"
			"    set myTimeOut = myTimeOut * 60 * 60\n"
			"  end if\n"
			"  if myTimeUnit = \"Hours\" then\n"
			"    set myTimeOut = myTimeOut * 60 * 60 * 60\n"
			"  end if\n"
			"  set myTimeOut = myTimeOut + myStartTicks\n"
			"  if voidp(myTimeOutFrame) then\n"
			"    set myTimeOutFrame = myStartFrame + 1\n"
			"  end if\n"
			"  if myImmediateJump = \"jump immediately\" then\n"
			"    set myImmediateJump = 1\n"
			"  else\n"
			"    set myImmediateJump = 0\n"
			"  end if\n"
			"  set myInitialized = 1\n"
			"end\n"
			"\n"
			"on exitFrame me\n"
			"  if voidp(myInitialized) then\n"
			"    initializeLoop(me)\n"
			"  end if\n"
			"  if the ticks > myTimeOut then\n"
			"    go(myTimeOutFrame)\n"
			"  else\n"
			"    go(myStartFrame)\n"
			"  end if\n"
			"end\n";
		break;

	case 52:
		script =
			"on prepareFrame me\n"
			"  global cardcomp, monstrocomp, habil\n"
			"  puppetSound(1, 0)\n"
			"  set x = 50\n"
			"  repeat while x <= 84\n"
			"    set the visible of sprite x to 0\n"
			"    set x = x + 1\n"
			"  end repeat\n"
			"  if count(cardcomp) > 0 then\n"
			"    set x = getAt(cardcomp, random(count(cardcomp)))\n"
			"    set monstrocomp = value(line x of member \"monstros\")\n"
			"    if not voidp(monstrocomp) then\n"
			"      set the text of member \"nomecomp\" to monstrocomp.nome\n"
			"    end if\n"
			"  end if\n"
			"  if habil = 1 then\n"
			"    set the locV of sprite 95 to 268\n"
			"    set the text of member \"habil\" to \"Forca\"\n"
			"  end if\n"
			"  if habil = 2 then\n"
			"    set the locV of sprite 95 to 294\n"
			"    set the text of member \"habil\" to \"Apetite\"\n"
			"  end if\n"
			"  if habil = 3 then\n"
			"    set the locV of sprite 95 to 323\n"
			"    set the text of member \"habil\" to \"Velocidade\"\n"
			"  end if\n"
			"  if habil = 4 then\n"
			"    set the locV of sprite 95 to 351\n"
			"    set the text of member \"habil\" to \"Inteligencia\"\n"
			"  end if\n"
			"  if habil = 5 then\n"
			"    set the locV of sprite 95 to 380\n"
			"    set the text of member \"habil\" to \"Magia\"\n"
			"  end if\n"
			"end\n";
		break;

	case 45:
		script =
			"on mouseUp me\n"
			"  global cardjog, contacarta\n"
			"  set x = getAt(cardjog, 1)\n"
			"  deleteAt(cardjog, 1)\n"
			"  addAt(cardjog, count(cardjog) + 1, x)\n"
			"  if contacarta < count(cardjog) then\n"
			"    set contacarta = contacarta + 1\n"
			"  else\n"
			"    if contacarta >= count(cardjog) then\n"
			"      set contacarta = 1\n"
			"    end if\n"
			"  end if\n"
			"  go(#next)\n"
			"end\n";
		break;

	case 46:
		script =
			"on mouseUp me\n"
			"  global cardjog, contacarta\n"
			"  set x = getAt(cardjog, count(cardjog))\n"
			"  deleteAt(cardjog, count(cardjog))\n"
			"  addAt(cardjog, 1, x)\n"
			"  set contacarta = contacarta - 1\n"
			"  if contacarta <= 1 then\n"
			"    set contacarta = count(cardjog)\n"
			"  end if\n"
			"  go(#next)\n"
			"end\n";
		break;

	case 95:
		script =
			"property spriteNum, myBuiltInCursor, mySavedCursor\n"
			"\n"
			"on beginSprite me\n"
			"  set mySavedCursor = the cursor of sprite me.spriteNum\n"
			"  if voidp(myBuiltInCursor) then\n"
			"    set myBuiltInCursor = 280\n"
			"  end if\n"
			"  set the cursor of sprite me.spriteNum to myBuiltInCursor\n"
			"end\n"
			"\n"
			"on endSprite me\n"
			"  set the cursor of sprite me.spriteNum to mySavedCursor\n"
			"end\n";
		break;

	case 96:
		script =
			"property spriteNum, myStandardMember, myRollovermember\n"
			"\n"
			"on beginSprite me\n"
			"  set myStandardMember = the member of sprite me.spriteNum\n"
			"end\n"
			"\n"
			"on mouseEnter me\n"
			"  set the member of sprite me.spriteNum to myRollovermember\n"
			"end\n"
			"\n"
			"on mouseLeave me\n"
			"  set the member of sprite me.spriteNum to myStandardMember\n"
			"end\n";
		break;

	default:
		return false;
	}

	CastMemberID scriptMember(memberId, DEFAULT_CAST_LIB);
	Cast *cast = movie->getCast(scriptMember);
	if (!cast)
		return false;

	debugC(1, kDebugLingoExec, "Loading builtin replacement behavior source for member %u", memberId);
	cast->_lingoArchive->addCode(Common::U32String(script, Common::kUtf8), scriptType, memberId,
		Common::String::format("builtin-behavior-%u", memberId).c_str());
	return true;
}

static bool loadScoreScriptSourceFromProjectorRays(Movie *movie, uint16 memberId, ScriptType scriptType) {
	static Common::HashMap<Common::String, bool> sLookupCache;

	Common::String cacheKey = Common::String::format("%s:%u", movie->getMacName().c_str(), memberId);
	if (sLookupCache.contains(cacheKey))
		return sLookupCache[cacheKey];

	sLookupCache[cacheKey] = false;

	// RE scaffolding: look for a ProjectorRays dump relative to the game path
	// (expected layout: <game>/../analysis/projectorrays_dump/<MovieName>/...).
	// This is only used during development of McDonalds A Ilha dos Ogo Pogos support
	// and should be removed before any upstream submission.
	Common::FSNode gamePath(ConfMan.getPath("path"));
	Common::FSNode repoRoot = gamePath.getParent();
	Common::FSNode analysisDir = repoRoot.getChild("analysis");
	Common::FSNode dumpRoot = analysisDir.getChild("projectorrays_dump");
	if (!dumpRoot.exists())
		return false;

	Common::FSNode movieDir = findMovieDumpDir(dumpRoot, movie->getMacName());
	if (!movieDir.exists())
		movieDir = findMovieDumpDir(dumpRoot, movie->getMacName() + ".dxr");
	if (!movieDir.exists())
		return false;

	Common::FSNode internalDir = movieDir.getChild("casts").getChild("Internal");
	if (!internalDir.exists() || !internalDir.isDirectory())
		return false;

	Common::FSList files;
	if (!internalDir.getChildren(files, Common::FSNode::kListFilesOnly))
		return false;

	Common::String prefix = Common::String::format("BehaviorScript %u", memberId);
	for (const Common::FSNode &file : files) {
		if (!file.getName().hasPrefixIgnoreCase(prefix) || !file.getName().hasSuffixIgnoreCase(".ls"))
			continue;

		Common::U32String source;
		if (!readUtf8TextFile(file, source))
			return false;

		normalizeDumpedScoreScriptSource(memberId, source);

		CastMemberID scriptMember(memberId, DEFAULT_CAST_LIB);
		Cast *cast = movie->getCast(scriptMember);
		if (!cast)
			return false;

		debugC(1, kDebugLingoExec,
			"Loading dumped score script source %s for member %u",
			file.getName().c_str(), memberId);
		cast->_lingoArchive->addCode(source, scriptType, memberId, file.getName().c_str());
		sLookupCache[cacheKey] = true;
		return true;
	}

	return false;
}

static bool loadBehaviorSourceFromProjectorRays(Movie *movie, BehaviorElement *behavior, ScriptType scriptType) {
	if (loadBuiltinBehaviorSource(movie, behavior->memberID.member, scriptType)) {
		debugC(1, kDebugLingoExec,
			"Score::createScriptInstance(): Using builtin replacement behavior source for %s",
			behavior->toString().c_str());
		return true;
	}

	if (!loadScoreScriptSourceFromProjectorRays(movie, behavior->memberID.member, scriptType)) {
		warning("loadBehaviorSourceFromProjectorRays(): could not find dumped source for %s", behavior->toString().c_str());
		return false;
	}
	debugC(1, kDebugLingoExec,
		"Score::createScriptInstance(): Compiling dumped behavior source for %s",
		behavior->toString().c_str());
	return true;
}

struct EventHandlerType {
	LEvent handler;
	const char *name;
} static const eventHandlerDescs[] = {
	{ kEventPrepareMovie,		"prepareMovie" },		//					D6
	{ kEventStartMovie,			"startMovie" },			//		D3
	{ kEventStepMovie,			"stepMovie" },			//		D3
	{ kEventStopMovie,			"stopMovie" },			//		D3

	{ kEventBeginSprite,		"beginSprite" },		//					D6
	{ kEventEndSprite,			"endSprite" },			//					D6

	{ kEventEnterFrame,			"enterFrame" },			//			D4
	{ kEventPrepareFrame,		"prepareFrame" },		//					D6
	{ kEventIdle,				"idle" },				//		D3
	{ kEventStepFrame,			"stepFrame"},			//				D5
	{ kEventExitFrame,			"exitFrame" },			//			D4

	{ kEventActivateWindow,		"activateWindow" },		//				D5
	{ kEventDeactivateWindow,	"deactivateWindow" },	//				D5
	{ kEventMoveWindow,			"moveWindow" },			//				D5
	{ kEventResizeWindow,		"resizeWindow" },		//				D5
	{ kEventOpenWindow,			"openWindow" },			//				D5
	{ kEventCloseWindow,		"closeWindow" },		//				D5
	{ kEventZoomWindow,			"zoomWindow" },			//				D5

	{ kEventKeyUp,				"keyUp" },				//			D4
	{ kEventKeyDown,			"keyDown" },			// D2 w		D4 (as when from D2)
	{ kEventMouseUp,			"mouseUp" },			// D2 w	D3
	{ kEventMouseDown,			"mouseDown" },			// D2 w	D3
	{ kEventRightMouseDown,		"rightMouseDown" },		//				D5
	{ kEventRightMouseUp,		"rightMouseUp" },		//				D5
	{ kEventMouseEnter,			"mouseEnter" },			//					D6, present in D5
	{ kEventMouseLeave,			"mouseLeave" },			//					D6, present in D5
	{ kEventMouseUpOutSide,		"mouseUpOutSide" },		// 					D6
	{ kEventMouseWithin,		"mouseWithin" },		//					D6, present in D5

	{ kEventTimeout,			"timeout" },			// D2 as when
	{ kEventCuePassed,			"cuePassed" },			//					D6

	{ kEventStartUp,			"startUp" },

	{ kEventGetBehaviorDescription,	"getBehaviorDescription" }, //			D6
	{ kEventGetPropertyDescriptionList,	"getPropertyDescriptionList" }, //	D6
	{ kEventRunPropertyDialog,	"runPropertyDialog" },	 //					D6

	{ kEventGeneric,			"scummvm_generic" },

	{ kEventNone, nullptr }
};

void Lingo::initEventHandlerTypes() {
	for (const EventHandlerType *t = &eventHandlerDescs[0]; t->handler != kEventNone; ++t) {
		_eventHandlerTypeIds[t->name] = t->handler;
		_eventHandlerTypes[t->handler] = t->name;
	}
	_eventHandlerTypes[kEventNone] = 0;
}

ScriptType Lingo::event2script(LEvent ev) {
	if (_vm->getVersion() < 400) {
		switch (ev) {
		//case kEventStartMovie: // We are precompiling it now
		//	return kMovieScript;
		case kEventExitFrame:
			return kScoreScript;
		default:
			return kNoneScript;
		}
	}

	return kNoneScript;
}

void Movie::setPrimaryEventHandler(LEvent event, const Common::String &code) {
	debugC(3, kDebugLingoExec, "setting primary event handler (%s)", _lingo->_eventHandlerTypes[event]);
	LingoArchive *mainArchive = getMainLingoArch();
	mainArchive->primaryEventHandlers[event] = code;
	mainArchive->replaceCode(code, kEventScript, event);
}

void Movie::resolveScriptEvent(LingoEvent &event) {
	// Resolve the script details of an event.
	// This must be done at execution time, as it relies on
	// e.g. the current frame, the current arrangement of sprites...
	uint16 spriteId = 0;
	if (event.mousePos != Common::Point(-1, -1)) {
		// Fetch the sprite underneath the mouse cursor.

		// D3 doesn't have both mouse up and down.
		// But we still want to know if the mouse is down for press effects.
		// Since we don't have mouse up and down before D3, then we use ActiveSprite
		if (g_director->getVersion() < 400)
			spriteId = _score->getActiveSpriteIDFromPos(event.mousePos);
		else
			spriteId = _score->getMouseSpriteIDFromPos(event.mousePos);

		if (event.event == kEventMouseDown || event.event == kEventRightMouseDown)
			_lastClickedSpriteId = _score->getActiveSpriteIDFromPos(event.mousePos); // the clickOn
	}
	// Very occasionally, we want to specify an event with a channel ID
	// rather than infer it from the position. Allow it to override.
	if (event.channelId == 0) {
		event.channelId = spriteId;
	}

	if ((event.event == kEventMouseDown) || (event.event == kEventMouseUp) ||
		(event.event == kEventRightMouseDown) || (event.event == kEventRightMouseUp)) {
		if (event.channelId > 0 && event.channelId < _score->_channels.size() && _score->_channels[event.channelId]) {
			Sprite *clickedSprite = _score->_channels[event.channelId]->_sprite;
			Common::String behaviorList;
			for (uint i = 0; i < clickedSprite->_behaviors.size(); i++) {
				if (!behaviorList.empty())
					behaviorList += ",";
				behaviorList += clickedSprite->_behaviors[i].memberID.asString();
			}
			debugC(3, kDebugEvents,
				"Movie::resolveScriptEvent(): %s resolved to channel=%d cast=%s script=%s immediate=%d behaviors=[%s]",
				g_lingo->_eventHandlerTypes[event.event], event.channelId,
				clickedSprite->_castId.asString().c_str(),
				clickedSprite->_scriptId.asString().c_str(),
				clickedSprite->_immediate, behaviorList.c_str());
		} else {
			debugC(3, kDebugEvents,
				"Movie::resolveScriptEvent(): %s resolved to no channel",
				g_lingo->_eventHandlerTypes[event.event]);
		}
	}

	// mouseDown/mouseUp events will have one of each of the source types queued.
	// run these steps at the very beginning (i.e. before the first source type).
	if (event.eventHandlerSourceType == kPrimaryHandler) {
		if ((event.event == kEventMouseDown) || (event.event == kEventRightMouseDown)) {
			if (!event.channelId && _isBeepOn) {
				g_lingo->func_beep(1);
			}

			if (event.channelId > 0) {
				if (_score->_channels[event.channelId]->_sprite->shouldHilite()) {
					_currentHiliteChannelId = event.channelId;
					g_director->_wm->_hilitingWidget = true;
					g_director->getCurrentWindow()->setDirty(true);
					g_director->getCurrentWindow()->addDirtyRect(_score->_channels[_currentHiliteChannelId]->getBbox());
				}

				CastMember *cast = getCastMember(_score->_channels[event.channelId]->_sprite->_castId);
				if (cast && cast->_type == kCastButton)
					_mouseDownWasInButton = true;

				if (_score->_channels[event.channelId]->_sprite->_moveable) {
					_draggingSpriteOffset = _score->_channels[event.channelId]->getPosition() - event.mousePos;
					_currentDraggedChannel = _score->_channels[event.channelId];
				}

				// In the case of clicking the mouse, it is possible for a mouseDown action to
				// change the cast member underneath. on mouseUp should always load the cast
				// script for the original cast member, not the new one.
				_currentMouseDownCastID = _score->_channels[event.channelId]->_sprite->_castId;
				_currentMouseDownSpriteScriptID = _score->_channels[event.channelId]->_sprite->_scriptId;
				_currentMouseDownSpriteImmediate = _score->_channels[event.channelId]->_sprite->_immediate;
			} else {
				_currentHiliteChannelId = 0;
				_mouseDownWasInButton = false;
				_draggingSpriteOffset = Common::Point(0, 0);
				_currentDraggedChannel = nullptr;
				_currentMouseDownCastID = CastMemberID();
				_currentMouseDownSpriteScriptID = CastMemberID();
				_currentMouseDownSpriteImmediate = false;
			}

		} else if ((event.event == kEventMouseUp) || (event.event == kEventRightMouseUp)) {
			if (_currentHiliteChannelId && _score->_channels[_currentHiliteChannelId]) {
				g_director->getCurrentWindow()->setDirty(true);
				g_director->getCurrentWindow()->addDirtyRect(_score->_channels[_currentHiliteChannelId]->getBbox());
			}
			g_director->_wm->_hilitingWidget = false;

			_currentDraggedChannel = nullptr;

			// If this is a button cast member, and the last mouse down event was in a button
			// (any button), flip this button's hilite flag.
			// Now you might think, "Wait, we don't flip this flag in the mouseDown event.
			// And why any button??? This doesn't make any sense."
			// No, it doesn't make sense, but it's what Director does.
			if (_mouseDownWasInButton && event.channelId) {
				CastMember *cast = getCastMember(_score->_channels[event.channelId]->_sprite->_castId);
				if (cast && cast->_type == kCastButton)
					cast->_hilite = !cast->_hilite;
			}
			_currentHiliteChannelId = 0;
			_mouseDownWasInButton = false;
			g_director->loadSlowdownCooloff();
		}
	}

	switch (event.eventHandlerSourceType) {
	case kPrimaryHandler:
		// Run the primary event handler.
		// Note that this isn't a "real" cast member ID, it's just the enum
		// of the type of event, so it can be crammed into the script context
		// index. kEventScript is a script type reserved for the primary event
		// handlers (e.g. the mouseDownScript, the mouseUpScript), so there will
		// be no collision with script cast members like ScoreScripts.
		{
			CastMemberID scriptId(event.event, DEFAULT_CAST_LIB);
			if (getScriptContext(kEventScript, scriptId)) {
				event.event = kEventGeneric;
				event.scriptType = kEventScript;
				event.scriptId = scriptId;
			}
		}
		break;
	/* When the mouseDown or mouseUp occurs over a sprite, the message
	 * goes first to the sprite script, then to the script of the cast
	 * member, to the frame script and finally to the movie scripts.
	 *
	 * When the mouseDown or mouseUp doesn't occur over a sprite, the
	 * message goes to the frame script and then to the movie script.
	 *
	 * When more than one movie script [...]
	 * [D4 docs] */
	case kSpriteHandler:
		{
			CastMemberID scriptId;
			bool immediate = false;
			Common::String initializerParams;
			// mouseUp events seem to check the frame script ID from the original mouseDown event
			// In Director 5 and above, we always generate event for the actual sprite under the mouse
			if (((event.event == kEventMouseUp) || (event.event == kEventRightMouseUp)) && _vm->getVersion() < 500) {
				scriptId = _currentMouseDownSpriteScriptID;
				immediate = _currentMouseDownSpriteImmediate;
			} else {
				if (!event.channelId)
					return;
				Frame *currentFrame = _score->_currentFrame;
				assert(currentFrame != nullptr);
				Sprite *sprite = _score->getSpriteById(event.channelId);
				if (!sprite)
					return;

				if (_vm->getVersion() >= 600) {
					if (event.behaviorIndex >= 0) {
						if (event.behaviorIndex >= (int)sprite->_behaviors.size()) {
							Channel *dbgCh2 = (event.channelId > 0 && event.channelId < (int)_score->_channels.size()) ? _score->_channels[event.channelId] : nullptr;
						warning("Movie::resolveScriptEvent: invalid behavior index %d (channel=%d behaviors=%d instances=%d frame=%d)",
							event.behaviorIndex, event.channelId,
							(int)sprite->_behaviors.size(),
							dbgCh2 ? (int)dbgCh2->_scriptInstanceList.size() : -1,
							_score->_curFrameNumber);
						} else {
							scriptId = sprite->_behaviors[event.behaviorIndex].memberID;
							initializerParams = sprite->_behaviors[event.behaviorIndex].initializerParams;
						}
					} else {
						_lastClickedSpriteId = 0;
						return;
					}
				} else {
					if (!sprite->_scriptId.member) {
						_lastClickedSpriteId = 0;
						return;
					}

					scriptId = sprite->_scriptId;
				}

				immediate = sprite->_immediate;
			}

			if (_vm->getVersion() >= 600) {
				// HACK: McDonalds A Ilha dos Ogo Pogos (D8.5) — cast members 45 and 46
				// are pure mouseUp click handlers that the D8.5 frame-script path
				// incorrectly resolves as script-channel handlers, causing exitFrame
				// to be routed to them. Restrict to only the events they implement
				// until the general D8.5 frame-script resolution is correct.
				if ((scriptId.member == 45 || scriptId.member == 46) &&
					event.event != kEventBeginSprite && event.event != kEventMouseUp) {
					return;
				}

				event.scriptType = kScoreScript;
				event.scriptId = scriptId;
				if (event.behaviorIndex >= 0 && event.behaviorIndex < (int)_score->_channels[event.channelId]->_scriptInstanceList.size())
					event.scriptInstance = _score->_channels[event.channelId]->_scriptInstanceList[event.behaviorIndex].u.obj;
				else {
					Channel *dbgCh = (event.channelId > 0 && event.channelId < (int)_score->_channels.size()) ? _score->_channels[event.channelId] : nullptr;
					Sprite *dbgSp = dbgCh ? dbgCh->_sprite : nullptr;
					warning("resolveScriptEvent: behaviorIndex %d out of range: channel=%d instances=%d behaviors=%d frame=%d script=%s",
						event.behaviorIndex, event.channelId,
						dbgCh ? (int)dbgCh->_scriptInstanceList.size() : -1,
						dbgSp ? (int)dbgSp->_behaviors.size() : -1,
						_score->_curFrameNumber,
						scriptId.asString().c_str());
				}
				return;
			}

			// Sprite (score) script
			ScriptContext *script = getScriptContext(kScoreScript, scriptId);
			if (script) {
				if (script->_eventHandlers.contains(event.event)) {
					// D4-style event handler
					event.scriptType = kScoreScript;
					event.scriptId = scriptId;
				} else if (script->_eventHandlers.contains(kEventGeneric)) {
					// D3-style sprite script, not contained in a handler
					// If sprite is immediate, its script is run on mouseDown, otherwise on mouseUp
					if ((event.event == kEventMouseDown && immediate) || (event.event == kEventMouseUp && !immediate)) {
						event.event = kEventGeneric;
						event.scriptType = kScoreScript;
						event.scriptId = scriptId;
					}
					return; // FIXME: Do not execute the cast script if there is a D3-style sprite script
				}
			}
		}
		break;
	case kCastHandler:
		{
			// Cast script
			// A strange quirk; if we're in a mouseDown event, Director will test
			// at runtime to find out whatever is under the mouse and use that.
			// If we're in a mouseUp event, Director will use whatever was
			// discovered -at the very beginning- of the mouseDown event chain.
			// This means e.g. the cast member can be swapped out from underneath in
			// the mouseDown sprite script and the event passed down, which
			// will mean the old cast member cast script does not get a mouseDown
			// call, but it -does- get a mouseUp call.
			// A bit unhinged, but we have a test that proves Director does this,
			// so we have to do it too.
			//
			// mouseEnter and mouseLeave events should also defer to the value of channelId.
			CastMemberID targetCast = _currentMouseDownCastID;
			if ((event.event == kEventMouseDown) || (event.event == kEventRightMouseDown) ||
				(event.event == kEventMouseEnter) || (event.event == kEventMouseLeave)) {
				if (!event.channelId)
					return;
				Sprite *sprite = _score->getSpriteById(event.channelId);
				targetCast = sprite->_castId;
			}

			ScriptContext *script = getScriptContext(kCastScript, targetCast);
			if (script && script->_eventHandlers.contains(event.event)) {
				event.scriptType = kCastScript;
				event.scriptId = targetCast;
			}
		}
		break;
	case kFrameHandler:
		{
			/* [in D4] the enterFrame, exitFrame, idle and timeout messages
			 * are sent to a frame script and then a movie script.	If the
			 * current frame has no frame script when the event occurs, the
			 * message goes to movie scripts.
			 * [p.81 of D4 docs]
			 */

			if (_score->_currentFrame == nullptr)
				return;

			if (_vm->getVersion() >= 600) {
				if (_score->_scriptChannelScriptInstance.type == OBJECT) {
					Symbol method = _score->_scriptChannelScriptInstance.u.obj->getMethod(_lingo->_eventHandlerTypes[event.event]);
					if (method.type != VOIDSYM) {
						event.scriptType = kScoreScript;
						event.scriptId = CastMemberID(); // No ID for the script channel script
						event.scriptInstance = _score->_scriptChannelScriptInstance.u.obj;
						return;
					}
				}
				// D6+ still carries a frame action script. If the script channel
				// object does not implement this event, fall back to the frame's
				// action script handlers.
			}

			CastMemberID scriptId = _score->_currentFrame->_mainChannels.actionId;
			if (scriptId.isNull() && !_score->_currentFrame->_mainChannels.behaviors.empty())
				scriptId = _score->_currentFrame->_mainChannels.behaviors[0].memberID;
			if (!scriptId.member)
				return;

			// HACK: McDonalds A Ilha dos Ogo Pogos (D8.5) — cast members 45, 46, and 52
			// are sprite click/sound behaviors that the D8.5 frame-script fallback
			// incorrectly promotes to script-channel handlers. Routing prepareFrame
			// and exitFrame to them causes the score to autoplay past the wait frame.
			if (scriptId.member == 45 || scriptId.member == 46 || scriptId.member == 52)
				return;

			// HACK: McDonalds A Ilha dos Ogo Pogos (D8.5) — cast member 82 has an
			// empty bytecode context; load from the ProjectorRays dump as a fallback.
			if (scriptId.member == 82)
				loadScoreScriptSourceFromProjectorRays(_vm->getCurrentMovie(), scriptId.member, kScoreScript);

			ScriptContext *script = getScriptContext(kScoreScript, scriptId);
			if (script && script->_eventHandlers.empty()) {
				Cast *cast = getCast(scriptId);
				if (cast) {
					ScriptContext *alt = cast->_lingoArchive->findScriptContext(scriptId.member);
					if (alt && !alt->_eventHandlers.empty())
						script = alt;
				}
			}
			if (!script)
				return;

			if (script->_eventHandlers.contains(event.event)) {
				event.scriptType = kScoreScript;
				event.scriptId = scriptId;
				return;
			}

			// Scopeless statements (ie one lined lingo commands) are executed at exitFrame
			// A score script can have both scopeless and scoped lingo. (eg. porting from D3.1 to D4)
			// In the event of both being specified in the ScoreScript, the scopeless handler is ignored.

			if (event.event == kEventExitFrame && script->_eventHandlers.contains(kEventGeneric) &&
				!(script->_eventHandlers.contains(kEventExitFrame) || script->_eventHandlers.contains(kEventEnterFrame))) {
				event.event = kEventGeneric;
				event.scriptType = kScoreScript;
				event.scriptId = scriptId;
			}
		}
		break;
	case kMovieHandler:
		{
			/* If more than one movie script handles the same message, Lingo
			 * searches the movie scripts according to their order in the cast
			 * window [p.81 of D4 docs]
			 */

			// FIXME: shared cast movie scripts could come before main movie ones
			// Movie scripts are fixed, so it's fine to look them up in advance.
			for (auto &cast : _casts) {
				LingoArchive *archive = cast._value->_lingoArchive;
				for (auto &it : archive->scriptContexts[kMovieScript]) {
					if (it._value->_eventHandlers.contains(event.event)) {
						event.scriptType = kMovieScript;
						event.scriptId = CastMemberID(it._key, cast._key);
						return;
					}
				}
			}
			LingoArchive *sharedArchive = getSharedLingoArch();
			if (sharedArchive) {
				for (auto &it : sharedArchive->scriptContexts[kMovieScript]) {
					if (it._value->_eventHandlers.contains(event.event)) {
						event.scriptType = kMovieScript;
						event.scriptId = CastMemberID(it._key, DEFAULT_CAST_LIB);
						return;
					}
				}
			}
		}
		break;
	default:
		break;
	}
}

void Movie::queueEvent(Common::Queue<LingoEvent> &queue, LEvent event, int targetId, Common::Point pos) {
	if (_nextEventId < 0)
		_nextEventId = 0;
	_nextEventId++;
	int eventId = _nextEventId;

	int oldQueueSize = queue.size();

	uint16 channelId = 0;
	uint16 pointedSpriteId = 0;

	// In D6+ there are multiple behavors per sprite, find the sprite
	if (g_director->getVersion() >= 600) {
		if (targetId == 0) {
			pointedSpriteId = _score->getMouseSpriteIDFromPos(pos);
		} else {
			pointedSpriteId = targetId;
		}
	}

	/* When an event occurs the message [...] is first sent to a
	 * primary event handler: [... if exists it is executed] and the
	 * event is passed on to other objects unless you explicitly stop
	 * the message by including the dontPassEvent command in the script
	 * [D4 docs page 77]
	 */
	/* N.B.: No primary event handlers for events other than
	 * keyup, keydown, mouseup, mousedown, timeout
	 * [see: www.columbia.edu/itc/visualarts/r4110/s2001/handouts
	 * /03_03_Event_Hierarchy.pdf]
	 */
	switch (event) {
	case kEventMouseDown:
	case kEventMouseUp:
	case kEventRightMouseDown:
	case kEventRightMouseUp:
	case kEventKeyUp:
	case kEventKeyDown:
	case kEventTimeout:
		{
			// Queue a call to the the primary event handler.
			// As per above, by default this will pass through to any subsequent handlers,
			// unless the script calls "dontPassEvent".
			queue.push(LingoEvent(event, eventId, kPrimaryHandler, true, pos));

			// Key up and key down events can be sent to the channel with an active widget
			if ((event == kEventKeyUp) || (event == kEventKeyDown)) {
				channelId = targetId;
			}
		}
		break;
	case kEventMenuCallback:
		{
			CastMemberID scriptID = CastMemberID(targetId, DEFAULT_CAST_LIB);
			if (getScriptContext(kEventScript, scriptID)) {
				queue.push(LingoEvent(kEventGeneric, eventId, kEventScript, true, scriptID, pos));
			}
		}
		break;
	// For mouseEnter/mouseLeave events, we want to specify exactly what sprite channel to resolve to.
	case kEventMouseEnter:
	case kEventMouseLeave:
	case kEventEnterFrame:
	case kEventExitFrame:
	case kEventIdle:
	case kEventPrepareFrame:
	case kEventBeginSprite:
	case kEventEndSprite:
	case kEventMouseUpOutSide:	// D6+
	case kEventMouseWithin:		// D6+
		if (targetId != 0) {
			channelId = targetId;
		}
		break;
	default:
		break;
	}

	if (_vm->getVersion() < 400) {
		// In D2-3, specific objects handle each event, with no passing
		switch(event) {
		case kEventMouseUp:
		case kEventMouseDown:
			queue.push(LingoEvent(event, eventId, kSpriteHandler, false, pos));
			queue.push(LingoEvent(event, eventId, kCastHandler, false, pos));
			break;

		case kEventExitFrame:
			queue.push(LingoEvent(event, eventId, kFrameHandler, false, pos));
			break;

		case kEventIdle:
		case kEventStartUp:
		case kEventStartMovie:
		case kEventStepMovie:
		case kEventStopMovie:
			queue.push(LingoEvent(event, eventId, kMovieHandler, false, pos));
			break;

		// no-op; only handled by the primary event handler above
		// empty case avoids them generating logs from the default
		// unhandled event case below.
		case kEventKeyUp:
		case kEventKeyDown:
		case kEventTimeout:
			break;

		default:
			warning("registerEvent: Unhandled event %s", _lingo->_eventHandlerTypes[event]);
		}
	} else {
		/* In D4+, queue any objects that responds to this event, in order of precedence.
		 *   (Sprite -> Cast Member -> Frame -> Movie)
		 * Once one of these objects handles the event, any event handlers queued
		 * for the same event will be ignored unless the pass command was called.
		 */
		switch (event) {
		case kEventKeyUp:
		case kEventKeyDown:
		case kEventMouseUp:
		case kEventMouseDown:
		case kEventRightMouseUp:
		case kEventRightMouseDown:
		case kEventBeginSprite:
		case kEventEndSprite:
		case kEventMouseEnter:
		case kEventMouseLeave:
		case kEventEnterFrame:
		case kEventExitFrame:
		case kEventIdle:
		case kEventPrepareFrame:	// D6+
		case kEventMouseUpOutSide:	// D6+
		case kEventMouseWithin:		// D6+
			if (_vm->getVersion() >= 600) {
				if (pointedSpriteId != 0) {
					Channel *channel = _score->getChannelById(pointedSpriteId);

					// Generate event for each behavior, and pass through for all but the last one.
					// This is to allow multiple behaviors on a single sprite to each have a
					// chance to handle the event.
					uint behaviorCount = MIN<uint>(channel->_scriptInstanceList.size(), channel->_sprite->_behaviors.size());
					for (uint i = 0; i < behaviorCount; i++) {
						bool passThrough = (i != behaviorCount - 1);
						queue.push(LingoEvent(event, eventId, kSpriteHandler, passThrough, pos, pointedSpriteId, i));
					}

					if (event == kEventBeginSprite || event == kEventEndSprite || event == kEventMouseUpOutSide) {
						// These events do not go any further than the sprite behaviors
						break;
					}
				} else {
					// We have no sprite under the mouse, no SpriteHandler to queue.
				}
			} else {
				queue.push(LingoEvent(event, eventId, kSpriteHandler, false, pos, channelId));
			}
			queue.push(LingoEvent(event, eventId, kCastHandler, false, pos, channelId));
			queue.push(LingoEvent(event, eventId, kFrameHandler, false, pos, channelId));
			queue.push(LingoEvent(event, eventId, kMovieHandler, false, pos, channelId));
			break;

		case kEventTimeout:
			queue.push(LingoEvent(event, eventId, kFrameHandler, false, pos, channelId));
			// fall through

		case kEventStartUp:
		case kEventStartMovie:
		case kEventStepMovie:
		case kEventStopMovie:

		case kEventPrepareMovie:		// D6

		case kEventActivateWindow:		// D5
		case kEventDeactivateWindow:	// D5
		case kEventMoveWindow:  		// D5
		case kEventResizeWindow:  		// D5
		case kEventOpenWindow:  		// D5
		case kEventCloseWindow:  		// D5
		case kEventZoomWindow:  		// D5
			queue.push(LingoEvent(event, eventId, kMovieHandler, false, pos, channelId));
			break;

		default:
			warning("registerEvent: Unhandled event %s", _lingo->_eventHandlerTypes[event]);
		}
	}

	if (oldQueueSize == queue.size()) {
		debugC(9, kDebugEvents, "Lingo::queueEvent(%s): no event handler", _lingo->_eventHandlerTypes[event]);
	}
}

void Movie::queueInputEvent(LEvent event, int targetId, Common::Point pos) {
	queueEvent(_inputEventQueue, event, targetId, pos);
}

void Movie::processEvent(LEvent event, int targetId) {
	Common::Queue<LingoEvent> queue;
	queueEvent(queue, event, targetId);
	_vm->setCurrentWindow(this->getWindow());
	_lingo->processEvents(queue, false);
}

void Movie::broadcastEvent(LEvent event) {
	Common::Queue<LingoEvent> queue;
	const int initialQueueSize = queue.size();

	for (uint i = 1; i < _score->_channels.size(); i++) {
		if (_score->_channels[i] && _score->_channels[i]->_sprite && _score->_channels[i]->_sprite->_behaviors.size()) {
			queueEvent(queue, event, i);
		}
	}

	// D6+ frame scripts live on the script channel and must run in addition to
	// sprite behavior broadcasts. Otherwise a hold frame with interactive
	// buttons advances as soon as those button behaviors are present.
	if (_vm->getVersion() >= 600 || queue.size() == initialQueueSize)
		queueEvent(queue, event, 0);

	_vm->setCurrentWindow(this->getWindow());
	_lingo->processEvents(queue, false);
}

void Lingo::processEvents(Common::Queue<LingoEvent> &queue, bool isInputEvent) {
	if (isInputEvent && _currentInputEvent.type != VOIDSYM) {
		// only one input event should be in flight at a time.
		return;
	}
	Movie *movie = _vm->getCurrentMovie();
	Score *sc = movie->getScore();

	bool behavioursCompleted = false;

	while (!queue.empty()) {
		LingoEvent el = queue.pop();

		if (sc->_playState == kPlayStopped && el.event != kEventStopMovie)
			continue;

		// fetch the sprite ID, script ID to call, etc if not present.
		movie->resolveScriptEvent(el);

		if (el.scriptType == kNoneScript) {
			debugC(9, kDebugEvents, "Lingo::processEvents: no matching script for event (%s, %s, %s, %d), continuing",
				_eventHandlerTypes[el.event], scriptType2str(el.scriptType), el.scriptId.asString().c_str(), el.channelId
			);
			continue;
		}

		int lastEventId = movie->_lastEventId.getValOrDefault(el.event, 0);
		if (lastEventId && lastEventId == el.eventId && !_passEvent) {
			debugC(5, kDebugEvents, "Lingo::processEvents: swallowed event (%s, %s, %s, %d) because _passEvent was false",
				_eventHandlerTypes[el.event], scriptType2str(el.scriptType), el.scriptId.asString().c_str(), el.channelId
			);
			continue;
		}

		_passEvent = el.passByDefault;

		debugC(5, kDebugEvents, "Lingo::processEvents: starting event script (%s, %s, %s, %d)",
			_eventHandlerTypes[el.event], scriptType2str(el.scriptType), el.scriptId.asString().c_str(), el.channelId
		);
		bool completed = processEvent(el.event, el.scriptType, el.scriptId, el.channelId, el.scriptInstance);
		movie->_lastEventId[el.event] = el.eventId;

		if (_vm->getVersion() >= 600) {
			// Reset it for further event processing
			g_director->getCurrentMovie()->_currentSpriteNum = 0;

			// We need to execute all behaviours before deciding if we pass
			// through or not
			if (el.scriptType == kScoreScript && el.passByDefault == true) {
				behavioursCompleted |= completed;
				completed = true;
			} else {
				completed |= behavioursCompleted;
			}
		}

		if (isInputEvent && !completed) {
			debugC(5, kDebugEvents, "Lingo::processEvents: context frozen on an input event, stopping");
			LingoState *state = g_director->getCurrentWindow()->getLastFrozenLingoState();
			if (state && !state->callstack.empty()) {
				_currentInputEvent = state->callstack.front()->sp;
			}
			break;
		}

	}
}

bool Lingo::processEvent(LEvent event, ScriptType st, CastMemberID scriptId, int channelId, AbstractObject *obj) {
	_currentChannelId = channelId;

	if (!_eventHandlerTypes.contains(event))
		error("processEvent: Unknown event %d", event);


	if (g_director->getVersion() >= 600 && st == kScoreScript && obj) {
		Symbol method = obj->getMethod(_eventHandlerTypes[event]);
		if (method.type != VOIDSYM) {
			debugC(1, kDebugEvents, "Lingo::processEvent(%s, %s, object '%s', channel %d): executing object method",
				_eventHandlerTypes[event], scriptType2str(st), obj->getName().c_str(), channelId);
			g_director->getCurrentMovie()->_currentSpriteNum = channelId;
			push(Datum(obj));
			LC::call(_eventHandlerTypes[event], 1, false);
			return execute();
		} else {
			debugC(1, kDebugEvents, "Lingo::processEvent(%s, %s, object '%s', channel %d): no object method",
				_eventHandlerTypes[event], scriptType2str(st), obj->getName().c_str(), channelId);
			return true;
		}
	}

	ScriptContext *script = g_director->getCurrentMovie()->getScriptContext(st, scriptId);
	int nargs = 0;

	if (script && script->_eventHandlers.contains(event)) {
		debugC(1, kDebugEvents, "Lingo::processEvent(%s, %s, %s): executing event handler", _eventHandlerTypes[event], scriptType2str(st), scriptId.asString().c_str());
		g_debugger->eventHook(event);

		// Normally event handlers are called with no arguments, however RolloverToolkit expects
		// the first argument to be the sprite number.
		if ((event == kEventMouseEnter && script->_eventHandlers[event].name->equalsIgnoreCase("startRollover")) ||
			(event == kEventMouseLeave && script->_eventHandlers[event].name->equalsIgnoreCase("endRollover"))) {
			push(Datum(channelId));
			nargs = 1;
		}

		LC::call(script->_eventHandlers[event], nargs, false);
		return execute();
	} else {
		debugC(9, kDebugEvents, "Lingo::processEvent(%s, %s, %s): no handler", _eventHandlerTypes[event], scriptType2str(st), scriptId.asString().c_str());
	}
	return true;
}

/***********************
 * Script Instances
 ***********************/

void Score::killScriptInstances(int frameNum) {
	if (_version < kFileVer600) // No-op for early Directors
		return;

	if (frameNum < _currentFrame->_mainChannels.scriptSpriteInfo.startFrame ||
	    frameNum > _currentFrame->_mainChannels.scriptSpriteInfo.endFrame) {
		if (_scriptChannelScriptInstance.type == OBJECT) {
			_scriptChannelScriptInstance = Datum();
			debugC(1, kDebugLingoExec, "Score::killScriptInstances(): Killed script instances for script channel. frame %d [%d-%d]",
				frameNum,
				_currentFrame->_mainChannels.scriptSpriteInfo.startFrame,
				_currentFrame->_mainChannels.scriptSpriteInfo.endFrame);
		}
	}

	for (int i = 0; i < (int)_channels.size(); i++) {
		Channel *channel = _channels[i];

		if (channel->_scriptInstanceList.size() == 0)
			continue;

		if (frameNum < channel->_startFrame || frameNum > channel->_endFrame) {
			bool prevDis = _disableGoPlayUpdateStage;
			_disableGoPlayUpdateStage = true;
			_movie->processEvent(kEventEndSprite, i);
			_disableGoPlayUpdateStage = prevDis;

			channel->_scriptInstanceList.clear();
			channel->_sprite->_behaviors.clear();
			debugC(1, kDebugLingoExec, "Score::killScriptInstances(): Killed script instances for channel %d. frame %d [%d-%d]",
					i + 1, frameNum, channel->_startFrame, channel->_endFrame);

			channel->_startFrame = channel->_endFrame = -1;
		}
	}
}

Datum Score::createScriptInstance(BehaviorElement *behavior) {
	// Instantiate the behavior
	ScriptContext *scr = _movie->getScriptContext(kScoreScript, behavior->memberID);
	Cast *cast = _movie->getCast(behavior->memberID);
	// HACK: McDonalds A Ilha dos Ogo Pogos (D8.5) — these cast members have empty
	// or missing bytecode contexts in central.dxr. Load from the ProjectorRays dump
	// as a fallback so the behavior lifecycle (beginSprite, exitFrame, mouseUp) works.
	// Members: 37 (tile setup), 40 (advance), 45/46 (sound/click), 52 (transition),
	//          55 (timeout loop), 95/96 (rollover cursor/member change from Scripts.cxt).
	if (behavior->memberID.member == 40 ||
		behavior->memberID.member == 52 ||
		behavior->memberID.member == 45 ||
		behavior->memberID.member == 46 ||
		behavior->memberID.member == 37 ||
		behavior->memberID.member == 55 ||
		behavior->memberID.member == 95 ||
		behavior->memberID.member == 96) {
		// Only replace when the context has no handlers to avoid recompiling on
		// every frame for behaviors covering many sprite channels at once. The
		// D8.5 bytecode contexts for the standard rollover helpers are non-empty
		// but misdecoded, so force them once to the known source replacement.
		bool forceKnownRollover =
			(behavior->memberID.member == 95 || behavior->memberID.member == 96) &&
			(scr == nullptr || !scr->getName().hasPrefixIgnoreCase("builtin-behavior-"));
		bool needsReplacement = forceKnownRollover || (scr == nullptr) ||
			(scr->_functionHandlers.empty() && scr->_eventHandlers.empty());
		if (needsReplacement) {
			debugC(1, kDebugLingoExec,
				"Score::createScriptInstance(): Replacing bytecode context for %s with ProjectorRays source",
				behavior->toString().c_str());
			if (loadBehaviorSourceFromProjectorRays(_movie, behavior, kScoreScript)) {
				scr = _movie->getScriptContext(kScoreScript, behavior->memberID);
				if (scr == nullptr && cast)
					scr = cast->_lingoArchive->findScriptContext(behavior->memberID.member);
			}
		}
	}
	// HACK: McDonalds A Ilha dos Ogo Pogos (D8.5) — cast member 33 is the score
	// script for the opening frame; its bytecode context is present but empty.
	if (scr && scr->_functionHandlers.empty() && scr->_eventHandlers.empty()
			&& behavior->memberID.member == 33) {
		debugC(1, kDebugLingoExec,
			"Score::createScriptInstance(): Replacing empty bytecode context for %s with ProjectorRays source",
			behavior->toString().c_str());
		if (loadBehaviorSourceFromProjectorRays(_movie, behavior, kScoreScript)) {
			scr = _movie->getScriptContext(kScoreScript, behavior->memberID);
			if (scr == nullptr && cast)
				scr = cast->_lingoArchive->findScriptContext(behavior->memberID.member);
		}
	}
	if (scr && scr->_functionHandlers.empty() && cast) {
		ScriptContext *alt = cast->_lingoArchive->findScriptContext(behavior->memberID.member);
		if (alt && !alt->_functionHandlers.empty()) {
			debugC(1, kDebugLingoExec,
				"Score::createScriptInstance(): Replacing empty ScoreScript context for %s with %s context",
				behavior->toString().c_str(), scriptType2str(alt->_scriptType));
			scr = alt;
		}
	}
	if (scr == nullptr && cast) {
		scr = cast->_lingoArchive->findScriptContext(behavior->memberID.member);
		if (scr) {
			debugC(1, kDebugLingoExec,
				"Score::createScriptInstance(): Fallback-resolved behavior %s as script type %s",
				behavior->toString().c_str(), scriptType2str(scr->_scriptType));
		}
	}
	if (scr == nullptr) {
		CastMember *member = _movie->getCastMember(behavior->memberID);
		if (cast && member && member->_type == kCastLingoScript) {
			ScriptCastMember *scriptMember = static_cast<ScriptCastMember *>(member);
			CastMemberInfo *info = member->getInfo();
			if (info && !info->script.empty()) {
				debugC(1, kDebugLingoExec,
					"Score::createScriptInstance(): Compiling missing behavior source for %s as %s",
					behavior->toString().c_str(), scriptType2str(scriptMember->_scriptType));
				cast->_lingoArchive->addCode(info->script, scriptMember->_scriptType, behavior->memberID.member, info->name.c_str());
				scr = _movie->getScriptContext(scriptMember->_scriptType, behavior->memberID);
			}
		}
		if (scr == nullptr && loadBehaviorSourceFromProjectorRays(_movie, behavior, kScoreScript)) {
			scr = _movie->getScriptContext(kScoreScript, behavior->memberID);
			if (scr == nullptr && cast)
				scr = cast->_lingoArchive->findScriptContext(behavior->memberID.member);
		}
	}

	// Some movies have behaviors with missing scripts
	if (scr == nullptr) {
		warning("Score::createScriptInstance(): Missing script for behavior %s", behavior->toString().c_str());
		return Datum();
	}

	g_lingo->push(scr);
	LC::call("new", 1, true);
	Datum instance = g_lingo->pop();

	if (instance.type != OBJECT) {
		warning("Score::createScriptInstance(): Behavior %s new() returned %s instead of OBJECT",
			behavior->toString().c_str(), instance.type2str());
		warning("Score::createScriptInstance(): Could not instantiate behavior %s", behavior->toString().c_str());
		return Datum();
	}

	debugC(1, kDebugLingoExec, "   Instantiated behavior %s", behavior->toString().c_str());

	// No initializer, we are done
	if (behavior->initializerIndex == 0)
		return instance;

	Common::String initializerParams = behavior->initializerParams;
	initializerParams.trim();
	if (initializerParams.empty())
		return instance;

	Datum proplist;
	if (!parseBehaviorInitializerParams(initializerParams, proplist)) {
		// Evaluate the params
		g_lingo->push(behavior->initializerParams);
		LB::b_value(1);
		g_lingo->execute();

		if (debugChannelSet(5, kDebugLingoExec)) {
			g_lingo->printStack("  Parsed behavior parameters: ", 0);
		}

		if (g_lingo->_state->stack.size() == 0) {
			warning("Score::createScriptInstance(): Could not evaluate initializer params '%s' for behavior %s",
				behavior->initializerParams.c_str(), behavior->toString().c_str());
			return instance;
		}

		proplist = _lingo->pop();

		if (proplist.type != PARRAY) {
			warning("Score::createScriptInstance(): Could not evaluate initializer params '%s' for behavior %s",
				behavior->initializerParams.c_str(), behavior->toString().c_str());
			return instance;
		}
	}

	debugC(2, kDebugLingoExec, "   Setting %d properties", proplist.u.parr->arr.size());

	for (uint k = 0; k < proplist.u.parr->arr.size(); k++) {
		Datum key = proplist.u.parr->arr[k].p;
		Datum val = proplist.u.parr->arr[k].v;

		instance.u.obj->setProp(key.asString(), val);
	}

	return instance;
}


void Score::createScriptInstances(int frameNum) {
	if (_version < kFileVer600) // No-op for early Directors
		return;

	if (frameNum >= _currentFrame->_mainChannels.scriptSpriteInfo.startFrame &&
	    frameNum <= _currentFrame->_mainChannels.scriptSpriteInfo.endFrame) {

		// We have no instantiated script
		if (_scriptChannelScriptInstance.type != OBJECT) {
			if (_currentFrame->_mainChannels.behaviors.size() > 0) {
				debugC(1, kDebugLingoExec, "Score::createScriptInstances(): Creating script instances for script channel, frames [%d-%d]",
					_currentFrame->_mainChannels.scriptSpriteInfo.startFrame,
					_currentFrame->_mainChannels.scriptSpriteInfo.endFrame);
				_scriptChannelScriptInstance = createScriptInstance(&_currentFrame->_mainChannels.behaviors[0]);
			}
		}
	}

	for (int i = 0; i < (int)_channels.size(); i++) {
		Channel *channel = _channels[i];
		Sprite *sprite = channel->_sprite;

		if (debugChannelSet(1, kDebugLingoExec) &&
			(frameNum == 1 || frameNum == 20 || frameNum == 28 || frameNum == 350 || frameNum == 405) &&
			sprite && sprite->_behaviors.size()) {
			debugC(1, kDebugLingoExec,
				"Score::createScriptInstances(): frame %d channel %d behaviors=%d scriptInstances=%d frames=[%d-%d] cast=%s script=%s",
				frameNum, i, sprite->_behaviors.size(), channel->_scriptInstanceList.size(),
				channel->_startFrame, channel->_endFrame,
				sprite->_castId.asString().c_str(), sprite->_scriptId.asString().c_str());
		}

		// The frame does not belong to the range
		if (frameNum < channel->_startFrame || frameNum > channel->_endFrame)
			continue;

		// No behaviors, nothing to do
		if (sprite->_behaviors.size() == 0) {
			channel->_scriptInstanceList.clear();
			continue;
		}

		// Rebuild script instances if the sprite behavior list changed while reusing the channel.
		if (channel->_scriptInstanceList.size() != 0) {
			if (channel->_scriptInstanceList.size() == sprite->_behaviors.size())
				continue;

			debugC(1, kDebugLingoExec,
				"Score::createScriptInstances(): Rebuilding script instances for channel %d due to behavior count mismatch (%d instances vs %d behaviors)",
				i + 1, channel->_scriptInstanceList.size(), sprite->_behaviors.size());
			channel->_scriptInstanceList.clear();
		}

		debugC(1, kDebugLingoExec, "Score::createScriptInstances(): Creating script instances for channel %d, %d behaviors, frames [%d-%d]",
			i + 1, sprite->_behaviors.size(), channel->_startFrame, channel->_endFrame);

		for (uint j = 0; j < sprite->_behaviors.size(); j++) {
			Datum instance = createScriptInstance(&sprite->_behaviors[j]);

			if (instance.type != OBJECT) {
				if (!instance.isVoid())
					warning("Score::createScriptInstances(): Could not instantiate behavior %s", sprite->_behaviors[j].toString().c_str());

				continue;
			}

			channel->_scriptInstanceList.push_back(instance);
		}

		bool prevDis = _disableGoPlayUpdateStage;
		_disableGoPlayUpdateStage = true;
		_movie->processEvent(kEventBeginSprite, i);
		_disableGoPlayUpdateStage = prevDis;
	}
}

} // End of namespace Director
