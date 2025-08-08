/******************************************************************************
 *
 *  osis2mod.cpp - Utility to import a module in OSIS format
 *
 * $Id$
 *
 * Copyright 2003-2014 CrossWire Bible Society (http://www.crosswire.org)
 *      CrossWire Bible Society
 *      P. O. Box 2528
 *      Tempe, AZ  85280-2528
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation version 2.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */

#ifdef _MSC_VER
	#pragma warning( disable: 4251 )
#endif

#include <cctype>
#include <cstdio>
//#include <fcntl.h>
#include <cerrno>
#include <cstdlib>
#include <stack>
#include <vector>
#include <iostream>
#include <fstream>
#include <cstring>

#include <utilstr.h>
#include <swmgr.h>
#include <rawtext.h>
#include <rawtext4.h>
#include <swbuf.h>
#include <utilxml.h>
#include <listkey.h>
#include <versekey.h>
#include <versificationmgr.h>
#include <swversion.h>

#include <ztext.h>
#include <ztext4.h>
#include <lzsscomprs.h>
#ifndef EXCLUDEZLIB
#include <zipcomprs.h>
#endif
#ifndef EXCLUDEBZIP2
#include <bz2comprs.h>
#endif
#ifndef EXCLUDEXZ
#include <xzcomprs.h>
#endif
#include <cipherfil.h>

#ifdef _ICU_
#include <utf8nfc.h>
#include <latin1utf8.h>
#include <utf8scsu.h>
#include <scsuutf8.h>
#endif
#include <utf8utf16.h>
#include <utf16utf8.h>

#ifndef NO_SWORD_NAMESPACE
using namespace sword;
#endif

//using namespace std;

int       debug            =    0; // mask of debug flags
const int DEBUG_WRITE      =    1; // writing to module
const int DEBUG_VERSE      =    2; // verse start and end
const int DEBUG_QUOTE      =    4; // quotes, especially Words of Christ (WOC)
const int DEBUG_TITLE      =    8; // titles
const int DEBUG_INTERVERSE =   16; // inter-verse material
const int DEBUG_XFORM      =   32; // transformations
const int DEBUG_REV11N     =   64; // versification
const int DEBUG_REF        =  128; // parsing of osisID and osisRef
const int DEBUG_STACK      =  256; // cleanup of references
const int DEBUG_OTHER      =  512; // ins and outs of books, chapters and verses
const int DEBUG_PARSE      = 1024; // parsing of numeric and character entities.

// Exit codes
const int EXIT_BAD_ARG     =   1; // Bad parameter given for program
const int EXIT_NO_WRITE    =   2; // Could not open the module for writing
const int EXIT_NO_CREATE   =   3; // Could not create the module
const int EXIT_NO_READ     =   4; // Could not open the input file for reading.
const int EXIT_BAD_NESTING =   5; // BSP or BCV nesting is bad
const int EXIT_BAD_COMMENT =   6; // XML Comment is bad
const int EXIT_BAD_ENTITY  =   7; // XML Entity is bad

#ifdef _ICU_
UTF8NFC    normalizer;
Latin1UTF8 converter;
#endif
SWFilter*  outputEncoder = NULL;
SWFilter*  outputDecoder = NULL;

int normalized = 0;
int converted  = 0;

SWText *module = 0;
unsigned int linePos = 0;
unsigned int charPos = 0;
VerseKey currentVerse;
SWBuf v11n     = "KJV";
char currentOsisID[255] = "N/A";

SWBuf activeVerseText;

std::vector<ListKey> linkedVerses;

static bool inCanonicalOSISBook = true; // osisID is for a book that is not in Sword's canon
static bool normalize           = true; // Whether to normalize UTF-8 to NFC

/**
 * @brief Generate a standardized identifier message for error or status reporting.
 *
 * This routine efficiently constructs a message identifier of the form:
 *   type(kind)[linePos,charPos] osisID=osisID:
 *
 * - If linePos is 0, the position ([linePos,charPos]) is omitted.
 * - If osisID is nullptr or empty, the osisID part is omitted.
 * - The returned string always ends with a colon and a trailing space (": ").
 *
 * @param type           The message type (e.g., "ERROR", "WARNING", "INFO").
 * @param kind           The message category or kind (e.g., "REF", "PARSE").
 * @param osisID         (Optional) The current OSIS ID to include. May be nullptr or empty.
 * @return SWBuf         The formatted identifier string.
 *
 * @note Uses the global variables linePos and charPos for position information.
 *
 * @example
 *   SWBuf id = identifyMsg("ERROR", "REF");
 *   // Possible output: "ERROR(REF): "
 *
 *   SWBuf id = identifyMsg("ERROR", "REF", "GEN.1.1");
 *   // Possible output: "ERROR(REF)[12,34] osisID=GEN.1.1: "
 */
inline SWBuf identifyMsg(const char* type, const char* kind, const char* osisID = nullptr) {
	char buf[192];
	int len = std::snprintf(buf, sizeof(buf), "%s(%s)", type, kind);

	// Only include position if linePos > 0
	if (linePos > 0) {
		len += std::snprintf(buf + len, sizeof(buf) - len, "[%u,%u]", linePos, charPos);
	}

	// Only include osisID if provided and not empty
	if (osisID && *osisID) {
		len += std::snprintf(buf + len, sizeof(buf) - len, "(%s)", osisID);
	}

	// Always end with ": "
	len += std::snprintf(buf + len, sizeof(buf) - len, ": ");

	// Clamp len to buffer size in case of truncation
	if (len < 0 || len >= (int)sizeof(buf)) {
		len = sizeof(buf) - 1;
	}
	return SWBuf(buf, len);
}

/**
 * Resolves an abbreviation or partial name against a list of candidate strings.
 *
 * The matching strategy is:
 *   1. Case-sensitive exact match: returns immediately if a single exact match is found.
 *   2. Case-insensitive exact match: uses UTF-8 safe toUpper() and returns immediately on match.
 *   3. Case-insensitive prefix match: returns all matching candidates that begin with the input.
 *
 * This function does not assume anything about the semantic meaning of the entries —
 * it can be used for versification systems, module names, etc.
 *
 * @param input The user-provided input string (abbreviation or full name).
 * @param candidates The list of valid full names to resolve against.
 * @return A StringList of matching entries (0 = no match, 1 = exact match, >1 = ambiguous).
 */
static StringList resolve_abbreviation(const SWBuf &input, const StringList &candidates) {
	StringList matches;

	// 1. Case-sensitive exact match
	for (const SWBuf &candidate : candidates) {
		if (input == candidate) {
			matches.push_back(candidate);
			return matches;
		}
	}

	// Convert input to uppercase for case-insensitive comparisons
	SWBuf inputUpper = input;
	inputUpper.toUpper();

	// 2. Case-insensitive exact match
	for (const SWBuf &candidate : candidates) {
		SWBuf candidateUpper = candidate;
		candidateUpper.toUpper();

		if (inputUpper == candidateUpper) {
			matches.push_back(candidate);
			return matches;
		}
	}

	// 3. Case-insensitive prefix match
	for (const SWBuf &candidate : candidates) {
		SWBuf candidateUpper = candidate;
		candidateUpper.toUpper();

		if (candidateUpper.startsWith(inputUpper)) {
			matches.push_back(candidate);
		}
	}

	return matches;
}

bool isOSISAbbrev(const char *buf) {
	VersificationMgr *vmgr = VersificationMgr::getSystemVersificationMgr();
	const VersificationMgr::System *av11n = vmgr->getVersificationSystem(v11n);
	return av11n->getBookNumberByOSISName(buf) >= 0;
}

/**
 * Determine whether the string contains a valid unicode sequence.
 * The following table give the pattern of a valid UTF-8 character.
 * Unicode Range               1st       2nd       3rd       4th
 * U-00000000 - U-0000007F  0nnnnnnn
 * U-00000080 - U-000007FF  110nnnnn  10nnnnnn
 * U-00000800 - U-0000FFFF  1110nnnn  10nnnnnn  10nnnnnn
 * U-00010000 - U-0010FFFF  11110nnn  10nnnnnn  10nnnnnn  10nnnnnn
 *
 * Note:
 *   1.  The latest UTF-8 RFC allows for a max of 4 bytes.
 *       Earlier allowed 6.
 *   2.  The number of bits of the leading byte before the first 0
 *       is the total number of bytes.
 *   3.  The "n" are the bits of the unicode codepoint.
 * This routine does not check to see if the code point is in the range.
 * It could.
 *
 * param  txt the text to check
 * return   1 if all high order characters form a valid unicode sequence
 *         -1 if there are no high order characters.
 *            Note: this is also a valid unicode sequence
 *          0 if there are high order characters that do not form
 *            a valid unicode sequence
 * author DM Smith
 */
int detectUTF8(const char *txt) {
	unsigned int  countUTF8 = 0;
	int           count     = 0;
	
	// Cast it to make masking and shifting easier
	const unsigned char *p = (const unsigned char*) txt;
	while (*p) {
		// Is the high order bit set?
		if (*p & 0x80) {
			// Then count the number of high order bits that are set.
			// This determines the number of following bytes
			// that are a part of the unicode character
			unsigned char i = *p;
			for (count = 0; i & 0x80; count++) {
				i <<= 1;
			}

			// Validate count:
			// Count 0: bug in code that would cause core walking
			// Count 1: is a pattern of 10nnnnnn,
			//          which does not signal the start of a unicode character
			// Count 5 to 8: 111110nn, 1111110n and 11111110 and 11111111
			//          are not legal starts, either
			if (count < 2 || count > 4) return 0;

			// At this point we expect (count - 1) following characters
			// of the pattern 10nnnnnn
			while (--count && *++p) {
				// The pattern of each following character must be: 10nnnnnn
				// So, compare the top 2 bits.
				if ((0xc0 & *p) != 0x80) return  0;
			}

			// Oops, we've run out of bytes too soon: Cannot be UTF-8
			if (count) return 0;

			// We have a valid UTF-8 character, so count it
			countUTF8++;
		}

		// Advance to the next character to examine.
		p++;
	}
	
	// At this point it is either UTF-8 or 7-bit ascii
	return countUTF8 ? 1 : -1;
}

void prepareSWText(const char *osisID, SWBuf &text)
{
	// Always check on UTF8 and report on non-UTF8 entries
	int utf8State = detectUTF8(text.c_str());

	// Trust, but verify.
	if (!normalize && !utf8State) {
		std::cout << identifyMsg("WARNING", "UTF8", osisID)
			  << "Should be converted to UTF-8 ("
			  << text
			  << ")"
			  << std::endl;
	}

#ifdef _ICU_
	if (normalize) {
		// Don't need to normalize text that is ASCII
		// But assume other non-UTF-8 text is Latin1 (cp1252) and convert it to UTF-8
		if (!utf8State) {
			std::cout << identifyMsg("INFO", "UTF8", osisID)
				  << "Converting to UTF-8 ("
				  << text
				  << ")"
				  << std::endl;
			converter.processText(text, (SWKey *)2);  // note the hack of 2 to mimic a real key. TODO: remove all hacks
			converted++;

			// Prepare for double check. This probably can be removed.
			// But for now we are running the check again.
			// This is to determine whether we need to normalize output of the conversion.
			utf8State = detectUTF8(text.c_str());
		}

		// Double check. This probably can be removed.
		if (!utf8State) {
			std::cout << identifyMsg("ERROR", "UTF8", osisID)
				  << "Converting to UTF-8 ("
				  << text
				  << ")"
				  << std::endl;
		}

		if (utf8State > 0) {
			SWBuf before = text;
			normalizer.processText(text, (SWKey *)2);  // note the hack of 2 to mimic a real key. TODO: remove all hacks
			if (before != text) {
				normalized++;
				std::cout << identifyMsg("INFO", "UTF8", osisID)
					  << "Converting to UTF-8 ("
					  << before
					  << ")"
					  << std::endl;
			}
		}
	}
#endif
}

/**
 * @brief Converts an osisID or osisRef into a SWORD-parseable verse list.
 *
 * osisRef can be:
 * - a single osisID
 * - an osisID-osisID
 * - or a sequence: osisRef osisRef
 *
 * osisID may have a work prefix (terminated by ':') and/or a grain suffix (started by '!').
 * SWORD cannot handle work prefixes or grains and expects sequences separated by a ';'.
 * This routine modifies the input buffer in place, stripping work prefixes and grains,
 * and replacing whitespace between osisRefs with ';'.
 *
 * @param buf [in,out] SWBuf containing the osisRef (will be modified in place)
 */
void prepareSWVerseKey(SWBuf &buf) {
	SWBuf orig       = buf;
	char* bufStart   = buf.getRawData();
	char* bufWrite   = bufStart;
	char* bufRead    = bufStart;
	char* tokenStart = bufStart;
	bool inRange = false;

	// Early exit if no work prefix, grain, or whitespace
	if (!std::strpbrk(bufStart, "! :")) {
		if (debug & DEBUG_REF) {
			std::cout << identifyMsg("DEBUG", "REF", orig)
				  << "VerseKey can parse this as is."
				  << std::endl;
		}
		return;
	}

	while (*bufRead) {
		if (inRange) {
			// Range markers are copied as is
			*bufWrite++ = *bufRead++;

			if (debug & DEBUG_REF) {
				std::cout << identifyMsg("DEBUG", "REF", orig)
					  << "Found a range marker."
					  << " Progress: "
					  << std::string(bufStart, bufWrite)
					  << " Remaining: "
					  << bufRead
					  << std::endl;
			}
		}

		// Look ahead to see if we are in a work prefix
		// but don't look past an osisID
		char* lookahead = std::strpbrk(bufRead, ": -");
		// We have found a work prefix
		if (lookahead && *lookahead == ':') {
			tokenStart = bufRead;
			// set bufRead to skip the work prefix
			bufRead = ++lookahead;

			if (debug & DEBUG_REF) {
				std::cout << identifyMsg("DEBUG", "REF", orig)
					  << "Found a work prefix "
					  << std::string(tokenStart, lookahead)
					  << " Progress: "
					  << std::string(bufStart, bufWrite)
					  << " Remaining: "
					  << bufRead
					  << std::endl;
			}
		}

		// Now we are in the meat of an osisID.
		// Copy it to its end but stop on a grain marker of '!'
		// Look ahead to see if we have a grain suffix
		// but don't look past an osisID
		lookahead = std::strpbrk(bufRead, "! -");
		if (!lookahead) {
			lookahead = bufRead + strlen(bufRead);
		}

		if (debug & DEBUG_REF) {
			std::cout << identifyMsg("DEBUG", "REF", orig)
				  << "Found an osisID: "
				  << std::string(bufRead, lookahead);
		}

		while (bufRead < lookahead) {
			*bufWrite++ = *bufRead++;
		}

		if (debug & DEBUG_REF) {
			std::cout << " Progress: "
				  << std::string(bufStart, bufWrite)
				  << " Remaining: "
				  << bufRead
				  << std::endl;
		}

		// The ! and everything following until we hit
		// the end of the osisID is part of the grain reference
		if (*bufRead == '!') {
			tokenStart = bufRead;
			bufRead = std::strpbrk(tokenStart, " -");
			if (!bufRead) {
				bufRead = tokenStart + strlen(tokenStart);
			}

			if (debug & DEBUG_REF) {
				std::cout << identifyMsg("DEBUG", "REF", orig)
					  << "Found a grain suffix "
					  << std::string(tokenStart, bufRead)
					  << " Progress: "
					  << std::string(bufStart, bufWrite)
					  << " Remaining: "
					  << bufRead
					  << std::endl;
			}
		}

		// At this point we have processed an osisID

		// if we are not in a range and the next characer is a -
		// then we are entering a range
		inRange = !inRange && *bufRead == '-';

		// between ranges and stand alone osisIDs we might have whitespace
		if (!inRange && *bufRead == ' ') {
			// skip this and subsequent spaces
			while (*bufRead == ' ') {
				bufRead++;
			}

			// replacing them all with a ';'
			*bufWrite++ = ';';

			if (debug & DEBUG_REF) {
				std::cout << identifyMsg("DEBUG", "REF", orig)
					  << "Replacing space with ;. "
					  << " Progress "
					  << std::string(bufStart, bufWrite)
					  << " Remaining: "
					  << bufRead
					  << std::endl;
			}
		}
	}

	// Now that the buffer is modified, it needs to be terminated
	*bufWrite = '\0';
	// Since we modified the swbuf, we need to tell it what we have done
	buf.setSize(bufWrite - buf.c_str());

	if (debug & DEBUG_REF) {
		std::cout << identifyMsg("DEBUG", "REF", orig)
			  << "Parseable VerseKey -- "
			  << buf.c_str()
			  << std::endl;
	}
}

/**
 * Determine whether a verse as given is valid for the versification.
 * This is done by comparing the before and after of normalization.
 */
bool isValidRef(const char *buf, const char *caller) {
	// Create a VerseKey that does not do auto normalization
	// Note: need to turn on headings so that a heading does not get normalized anyway
	// And set it to the reference under question
	VerseKey before;
	before.setVersificationSystem(v11n);
	before.setAutoNormalize(false);
	before.setIntros(true);
	before.setText(buf);

	// If we are a heading we must bail
	// These will autonormalize to the last verse of the prior chapter
	//if (!before.getTestament() || !before.getBook() || !before.getChapter() || !before.getVerse()) {
	//	return true;
	//}

	// Create a VerseKey that does do auto normalization
	// And set it to the reference under question
	VerseKey after;
	after.setVersificationSystem(v11n);
	after.setAutoNormalize(true);
	after.setIntros(true);
	after.setText(buf);

	if (before == after)
	{
		return true;
	}

	// If we have gotten here the reference is not in the selected versification.
	// std::cout << identifyMsg("INFO", "V11N", before.getOSISRef()) << " is not in the " << currentVerse.getVersificationSystem() << " versification." << std::endl;
	if (debug & DEBUG_REV11N) {
		std::cout << identifyMsg("DEBUG", "V11N", before.getOSISRef())
			  << "{"
			  << caller
			  << "}  normalizes to "
			  << after.getOSISRef()
			  << std::endl;
	}

	return false;
}

/**
 * This routine is used to ensure that all the text in the input is saved to the module.
 * Assumption: The input orders all the verses for a chapter in numerical order. Thus, any
 * verses that are not in the chosen versification (v11n) follow those that are.
 *
 * The prior implementation of this adjusted the verse to the last one that is in the chosen v11n.
 * If it the chapter were extra, then it is appended to the last verse of the last
 * chapter in the chosen v11n for that book. If it is just extra verses for a chapter, then it is
 * appended to the last verse of the chapter.
 *
 * The problem with this is when a OSIS verse refers to more than one verse, e.g.
 * osisID="Gen.1.29 Gen.1.30 Gen.1.31" (Gen.1.31 is the last verse of the chapter in the chosen v11n)
 * and then it is followed by Gen.1.32.
 *
 * This routine assumes that linking is postponed to the end so that in the example Gen.1.30-31
 * are not linked but rather empty. This routine will then find the last verse in the computed
 * chapter that has content.
 *
 * Alternative, we could have done linking as we went, but this routine would have needed
 * to find the first entry in the link set and elsewhere in the code when appending to a
 * verse, it would need to be checked for adjacent links and those would have needed to be adjusted.
 *
 * param key the key that may need to be adjusted
 */
void makeValidRef(VerseKey &key) {
	VerseKey saveKey;
	saveKey.setVersificationSystem(v11n);
	saveKey.setAutoNormalize(false);
	saveKey.setIntros(true);
	saveKey = key;

	// Since isValidRef returned false constrain the key to the nearest prior reference.
	// If we are past the last chapter set the reference to the last chapter
	int chapterMax = key.getChapterMax();
	bool beyondChapter = key.getChapter() > chapterMax;
	if (beyondChapter) {
		key.setChapter(chapterMax);
	}

	// Either we set the chapter to the last chapter and now need to set to the last verse in the chapter
	// Or the verse is beyond the end of the chapter.
	// In any case we need to constrain the verse to it's chapter.
	int verseMax   = key.getVerseMax();
	key.setVerse(verseMax);

	if (debug & DEBUG_REV11N) {
		std::cout << identifyMsg("DEBUG", "V11N", saveKey.getOSISRef())
			  << "Chapter max:"
			  << chapterMax
			  << ", Verse Max:"
			  << verseMax
			  << std::endl;
	}

	// There are three cases we want to handle:
	// In the examples we are using the KJV versification where the last verse of Matt.7 is Matt.7.29.
	// In each of these cases the out-of-versification, extra verse is Matt.7.30.
	// 1) The "extra" verse follows the last verse in the chapter.
	//      <verse osisID="Matt.7.29">...</verse><verse osisID="Matt.7.30">...</verse>
	//    In this case re-versify Matt.7.30 as Matt.7.29.
	//
	// 2) The "extra" verse follows a range (a set of linked verses).
	//      <verse osisID="Matt.7.28-Matt.7.29">...</verse><verse osisID="Matt.7.30">...</verse>
	//    In this case, re-versify Matt.7.30 as Matt.7.28, the first verse in the linked set.
	//    Since we are post-poning linking, we want to re-reversify to the last entry in the module.
	//
	// 3) The last verse in the chapter is not in the input. There may be other verses missing as well.
	//      <verse osisID="Matt.7.8">...</verse><verse osisID="Matt.7.30">...</verse>
	//    In this case we should re-versify Matt.7.30 as Matt.7.29.
	//    However, since this and 2) are ambiguous, we'll re-reversify to the last entry in the module.
	
	while (!beyondChapter && !key.popError() && !module->hasEntry(&key)) {
		key.decrement(1);
	}

	std::cout << identifyMsg("INFO", "V11N", saveKey.getOSISRef())
		  << " Verse is not in the "
		  << v11n
		  << " versification. Appending content to "
		  << key.getOSISRef()
		  << std::endl;
}

void writeEntry(SWBuf &text, bool force = false) {
	char keyOsisID[255];

	static bool firstCall = true;
	static SWBuf revision;
	static VerseKey lastKey;
	static char activeOsisID[255] = "";
	static bool firstOut  = true;

	// do static initialization once
	if (firstCall) {
		revision.setFormatted("<milestone type=\"x-importer\" subType=\"x-osis2mod\" n=\"$Rev$ (SWORD: %s)\"/>", SWVersion::currentVersion.getText());
		lastKey.setVersificationSystem(v11n);
		lastKey.setAutoNormalize(false);
		lastKey.setIntros(true);
		firstCall = false;
	}

	// When we've seen a book and it is not in the v11n, skip it
	if (!inCanonicalOSISBook) {
		return;
	}

	// If we have module or testament intros we don't have a book and no osisID
	// so use the SWORD reference instead
	if (currentVerse.getBook()) {
		strcpy(keyOsisID, currentVerse.getOSISRef());
	} else {
		strcpy(keyOsisID, currentVerse.getText());
	}

	VerseKey saveKey;
	saveKey.setVersificationSystem(v11n);
	saveKey.setAutoNormalize(false);
	saveKey.setIntros(true);
	saveKey = currentVerse;

	// Do the write behind when have seen a verse and the supplied one is different then we output the collected one or forced.
	if (*activeOsisID && (force || strcmp(activeOsisID, keyOsisID))) {

		if (!isValidRef(lastKey, "writeEntry")) {
			makeValidRef(lastKey);
		}

		currentVerse = lastKey;

		prepareSWText(activeOsisID, activeVerseText);

		// Put the revision into the module
		int testmt = currentVerse.getTestament();
		if (firstOut) {
			// If we outputting a module or testament intro, prepend the revision.
			// otherwise output it as a module heading
			if (testmt == 0 || currentVerse.getBook() == 0) {
				activeVerseText = revision + activeVerseText;
			} else {
				// save off the current verse
				VerseKey t;
				t.setVersificationSystem(v11n);
				t.setAutoNormalize(false);
				t.setIntros(true);
				t = currentVerse;
				// Setting the testament will set Book, Chapter and Verse to 0
				currentVerse.setTestament(testmt);
				// write the revision
				module->setEntry(revision);
				// restore the current verse
				currentVerse = t;
			}
			firstOut = false;
		}

		// If the desired output encoding is non-UTF-8, convert to that encoding
		if (outputEncoder) {
			outputEncoder->processText(activeVerseText, (SWKey *)2);  // note the hack of 2 to mimic a real key. TODO: remove all hacks
		}

		// If the entry already exists, then append this entry to the text.
		// This is for verses that are outside the chosen versification. They are appended to the prior verse.
		// The space should not be needed if we retained verse tags.
		if (module->hasEntry(&currentVerse)) {
			module->flush();
			SWBuf currentText = module->getRawEntry();
			std::cout << identifyMsg("INFO", "WRITE", activeOsisID)
				  << "Appending entry to "
				  << currentVerse.getOSISRef()
				  << ": "
				  << activeVerseText
				  << std::endl;

			// If we have a non-UTF-8 encoding, we should decode it before concatenating, then re-encode it
			if (outputDecoder) {
				outputDecoder->processText(activeVerseText, (SWKey *)2);
				outputDecoder->processText(currentText, (SWKey *)2);
			}
			activeVerseText = currentText + " " + activeVerseText;
			if (outputEncoder) {
				outputEncoder->processText(activeVerseText, (SWKey *)2);
			}
		}

		// For further debugging introductions
//		if (debug & DEBUG_VERSE) {
//			SWBuf currentText = currentVerse.getText();
//			activeVerseText = currentText + ":" + activeVerseText;
//		}

		if (debug & DEBUG_WRITE) {
			std::cout << identifyMsg("DEBUG", "WRITE", activeOsisID)
				  << activeVerseText
				  << std::endl;
		}

		module->setEntry(activeVerseText);
		activeVerseText = "";
	}

	// The following is for initial verse content and for appending interverse colophon and end tags.
	if (activeVerseText.length()) {
		activeVerseText += text;
	}
	else {
		// Eliminate leading whitespace on the beginning of each verse
		text.trimStart();
		activeVerseText = text;
	}
	// text has been consumed so clear it out.
	text = "";

	currentVerse = saveKey;
	lastKey = currentVerse;
	strcpy(activeOsisID, keyOsisID);
}

void linkToEntry(VerseKey &linkKey, VerseKey &dest) {

	// Only link verses that are in the versification.
	if (!isValidRef(linkKey, "linkToEntry")) {
		return;
	}

	VerseKey saveKey;
	saveKey.setVersificationSystem(v11n);
	saveKey.setAutoNormalize(false);
	saveKey.setIntros(true);
	saveKey = currentVerse;
	currentVerse = linkKey;

	std::cout << identifyMsg("INFO", "LINK", currentVerse.getOSISRef()) 
		  << "Linking to " 
		  << dest.getOSISRef()
		  << "\n";
	module->linkEntry(&dest);

	currentVerse = saveKey;
}

// Return true if the content was handled or is to be ignored.
//        false if the what has been seen is to be accumulated and considered later.
bool handleToken(SWBuf &text, XMLTag token) {

	// Flags identifying what part of the OSIS document is being seen.
	// Flag indicating whether we are processing the content of a module; false prior to the first div tag
	static bool               inModule          = false;

	// Everything from the begin module text and the first book or bookGroup div tag is inModuleIntro
	static bool               inModuleIntro     = false;

	// Flag indicating whether we are processing the Old Testament
	static bool               inOT              = false;

	// Flag indicating whether we are processing the New Testament
	static bool               inNT              = false;

	// Flag indicating whether we are processing the content of a book
	static bool               inBook          = false;

	// Everything between the begin book tag and the first begin chapter tag is inBookIntro
	static bool               inBookIntro     = false;

	// Flag indicating whether we are processing the content of a chapter
	static bool               inChapter       = false;

	// Everything between the begin chapter tag and the first begin verse tag is inChapterIntro
	static bool               inChapterIntro  = false;

	// Flag indicating whether we are processing the content of a verse
	static bool               inVerse         = false;

	// Flag indicating whether we are processing the content of to be prepended to a verse
	static bool               inPreVerse      = false;

	// Generative ID for sID/eID pair
	static int                genID           = 1;

	// Flag indicating whether we are in "Words of Christ"
	static bool               inWOC           = false;
	// Tag for WOC quotes within a verse
	static XMLTag             wocTag          = "<q who=\"Jesus\" marker=\"\">";

	// Flag used to indicate where useful text begins
	static bool               headerEnded     = false;

	// Retain the sID of book, chapter and verse (commentary) divs so that we can find them again.
	// This relies on transformBSP.
	static SWBuf              sidBook         = "";
	static SWBuf              sidChapter      = "";
	static SWBuf              sidVerse        = "";

	// Stack of quote elements used to handle Words of Christ
	static std::stack<XMLTag> quoteStack;

	// Stack of elements used to validate that books, chapters and verses are well-formed
	// This goes beyond simple xml well-formed and also considers milestoned div, chapter and verse
	// to be begin and end tags, too.
	// It is an error if books and chapters are not well formed (though not required by OSIS)
	// It is a warning that verses are not well formed (because some clients are not ready)
	static std::stack<XMLTag> tagStack;

	// The following are used to validate well-formedness
	static int                bookDepth       = 0;
	static int                chapterDepth    = 0;
	static int                verseDepth      = 0;

	int                       tagDepth        = tagStack.size();
	SWBuf                     tokenName       = token.getName();
	bool                      isEndTag        = token.isEndTag() || token.getAttribute("eID");
	SWBuf                     typeAttr        = token.getAttribute("type");
	SWBuf                     eidAttr         = token.getAttribute("eID");

	// process start tags
	if (!isEndTag) {

		// Remember non-empty start tags
		if (!token.isEmpty()) {
			tagStack.push(token);

			if (debug & DEBUG_STACK) {
				std::cout << identifyMsg("DEBUG", "STACK", currentOsisID)
					  << "Push("
					  << tagStack.size()
					  << ") "
					  << token
					  << std::endl;
			}
		}

		// throw away everything up to the first div (that is outside the header)
		if (!inModule) {
			if (headerEnded && (tokenName == "div")) {
				if (debug & DEBUG_OTHER) {
					std::cout << identifyMsg("DEBUG", "FOUND")
						  << "Found first div and pitching prior material: "
						  << text
						  << std::endl;
				}

				// TODO: Save off the content to use it to suggest the module's conf.
				inModule = true;
				inModuleIntro = true;

				// Setting the testament will set Book, Chapter and Verse to 0 when intros are true
				currentVerse.setTestament(0);
				text     = "";

				if (debug & DEBUG_TITLE) {
					std::cout << identifyMsg("DEBUG", "TITLE", currentOsisID)
						  << "Looking for module introduction"
						  << std::endl;
				}

			}
			else {
				// Collect the content so it can be used to suggest the module's conf.
				return false;
			}
		}

		//-- WITH osisID OR annotateRef -------------------------------------------------------------------------
		// Handle Book, Chapter, and Verse (or commentary equivalent)
		if (token.getAttribute("osisID") || token.getAttribute("annotateRef")) {

			// BOOK START, <div type="book" ...>
			if (tokenName == "div" && typeAttr == "book") {
				if (inModuleIntro) { // this one should never happen, but just in case
					// Setting the testament will set Book, Chapter and Verse to 0
					currentVerse.setTestament(0);

					if (debug & DEBUG_TITLE) {
						std::cout << identifyMsg("DEBUG", "TITLE", currentVerse)
							  << "MODULE INTRO(book) "
							  << text
							  << std::endl;
					}

					writeEntry(text);

					inModuleIntro = false;
				}
				else {
					// Now check to see if we have gathered a testament intro.

					// While SWORD allows for the input of books, chapters and verses to appear in any order
					// this code assumes that that all the books defined in a testament are together.
					// note the apocrypha, when present, is in either the OT or the NT.

					// Once we have seen a book we are in either the OT or the NT
					// and we'll remain in that testament until we get to a book in the next testament
					// Yeah, this allows for the OT to follow the NT and
					// for the books to be in any order within the testament.
					// Don't do that!

					// Convert the osisID to a VerseKey in order to grab the testament.
					VerseKey tmp;
					tmp.setVersificationSystem(v11n);
					tmp.setAutoNormalize(false);
					tmp.setIntros(true);
					tmp = token.getAttribute("osisID");

					// Setting the testament will set Book, Chapter and Verse to 0
					tmp.setTestament(tmp.getTestament());

					// The OT Intro only occurs once and is all the material before the OT
					// that hasn't been handled yet
					// !inOT verifies that we haven't processed anything in the OT yet
					if (!inOT && tmp.getTestament() == 1) {
						if (debug & DEBUG_TITLE) {
							std::cout << identifyMsg("DEBUG", "TITLE", tmp)
								  << "OT INTRO "
								  << text
								  << std::endl;
						}
						currentVerse.setTestament(1);
						writeEntry(text);
					}

					// same logic for the NT
					if (!inNT && tmp.getTestament() == 2) {
						if (debug & DEBUG_TITLE) {
							std::cout << identifyMsg("DEBUG", "TITLE", tmp)
								  << "NT INTRO "
								  << text << std::endl;
						}
						currentVerse.setTestament(2);
						writeEntry(text);
					}
				}

				currentVerse = token.getAttribute("osisID");
				currentVerse.setChapter(0);
				currentVerse.setVerse(0);
				strcpy(currentOsisID, currentVerse.getOSISRef());

				sidBook         = token.getAttribute("sID");
				inOT            = currentVerse.getTestament() == 1;
				inNT            = currentVerse.getTestament() == 2;
				inBook          = true;
				inChapter       = false;
				inVerse         = false;
				inPreVerse      = false;
				inModuleIntro   = false;
				inBookIntro     = true;
				inChapterIntro  = false;

				if (debug & DEBUG_TITLE) {
					std::cout << identifyMsg("DEBUG", "TITLE", currentOsisID)
						  << "Looking for book introduction"
						  << std::endl;
				}

				bookDepth       = tagStack.size();
				chapterDepth    = 0;
				verseDepth      = 0;

				inCanonicalOSISBook = isOSISAbbrev(token.getAttribute("osisID"));
				if (!inCanonicalOSISBook) {
					std::cout << identifyMsg("WARNING", "V11N", token.getAttribute("osisID"))
						  << "New book is not in "
						  << v11n
						  << " versification, ignoring"
						  << std::endl;
				}
				else if (debug & DEBUG_OTHER) {
					std::cout << identifyMsg("DEBUG", "FOUND", currentVerse.getOSISRef())
						  << "Found new book"
						  << std::endl;
				}

				return false;
			}

			// CHAPTER START, <chapter> or <div type="chapter" ...>
			if ((tokenName == "chapter") ||
			    (tokenName == "div" && typeAttr == "chapter")
			) {
				if (inBookIntro) {
					if (debug & DEBUG_TITLE) {
						std::cout << identifyMsg("DEBUG", "TITLE", currentOsisID)
							  << "BOOK INTRO "
							  << text
							  << std::endl;
					}

					writeEntry(text);

					inBookIntro     = false;
				}

				currentVerse = token.getAttribute("osisID");
				currentVerse.setVerse(0);

				if (debug & DEBUG_OTHER) {
					std::cout << identifyMsg("DEBUG", "FOUND", currentVerse.getOSISRef())
						  << "Current chapter is "
						  << token.getAttribute("osisID")
						  << std::endl;
				}

				strcpy(currentOsisID, currentVerse.getOSISRef());

				sidChapter      = token.getAttribute("sID");
				inChapter       = true;
				inVerse         = false;
				inPreVerse      = false;
				inChapterIntro  = true;

				if (debug & DEBUG_TITLE) {
					std::cout << identifyMsg("DEBUG", "TITLE", currentOsisID)
						  << "Looking for chapter introduction"
						  << std::endl;
				}

				chapterDepth    = tagStack.size();
				verseDepth      = 0;

				return false;
			}

			// VERSE, <verse ...> OR COMMENTARY START, <div annotateType="xxx" ...>
			if ((tokenName == "verse") ||
			    (tokenName == "div" && token.getAttribute("annotateType"))
			) {
				if (inChapterIntro) {
					if (debug & DEBUG_TITLE) {
						std::cout << identifyMsg("DEBUG", "TITLE", currentOsisID)
							  << "Done looking for chapter introduction"
							  << std::endl;
					}

					if (text.length()) {
						if (debug & DEBUG_TITLE) {
							std::cout << identifyMsg("DEBUG", "TITLE", currentOsisID)
								  << "CHAPTER INTRO "
								  << text
								  << std::endl;
						}

						writeEntry(text);
					}
				}

				// Did we have pre-verse material that needs to be marked?
				if (inPreVerse) {
					char genBuf[200];
					sprintf(genBuf, "<div type=\"x-milestone\" subType=\"x-preverse\" eID=\"pv%d\"/>", genID++);
					text.append(genBuf);
				}

				// Get osisID for verse or annotateRef for commentary
				SWBuf refVal = token.getAttribute(tokenName == "verse" ? "osisID" : "annotateRef");
				SWBuf keyVal = refVal;

				if (debug & DEBUG_OTHER) {
					std::cout << identifyMsg("DEBUG", "FOUND", refVal.c_str())
						  << "Entering verse"
						  << std::endl;
				}

				// Massage the key into a form that parseVerseList can accept
				prepareSWVerseKey(keyVal);

				// The osisID or annotateRef can be more than a single verse
				// The first or only one is the currentVerse
				// Use the last verse seen (i.e. the currentVerse) as the basis for recovering from bad parsing.
				// This should never happen if the references are valid OSIS references
				ListKey verseKeys = currentVerse.parseVerseList(keyVal, currentVerse, true);
				int memberKeyCount = verseKeys.getCount();
				if (memberKeyCount) {
					verseKeys.setPosition(TOP);
					// get the first single verse
					currentVerse = verseKeys;
					// See if this osisID or annotateRef refers to more than one verse.
					// This can be done by incrementing, which will produce an error
					// if there is only one verse.
					verseKeys.increment(1);
					if (!verseKeys.popError()) {
						// If it does, save it until all verses have been seen.
						// At that point we will output links.
						std::cout << identifyMsg("DEBUG", "LINK MASTER", currentVerse.getOSISRef())
							  << std::endl;
						linkedVerses.push_back(verseKeys);
					}
				}
				else {
					std::cout << identifyMsg("ERROR", "REF", refVal)
						  << "Invalid osisID/annotateRef"
						  << std::endl;
				}

				strcpy(currentOsisID, currentVerse.getOSISRef());

				if (debug & DEBUG_OTHER) {
					std::cout << identifyMsg("DEBUG", "FOUND", currentOsisID)
						  << "New current verse"
						  << std::endl;
				}

				sidVerse        = token.getAttribute("sID");
				inVerse         = true;
				inPreVerse      = false;
				inBookIntro     = false;
				inChapterIntro  = false;
				verseDepth      = tagStack.size();

				// Include the token if it is not a verse
				if (tokenName != "verse") {
					text.append(token);
				}
				else if (debug & DEBUG_VERSE)
				{
					// transform the verse into a milestone
					XMLTag t = "<milestone resp=\"v\" />";
					// copy all the attributes of the verse element to the milestone
					StringList attrNames = token.getAttributeNames();
					for (StringList::iterator loop = attrNames.begin(); loop != attrNames.end(); loop++) {
						const char* attr = (*loop).c_str();
						t.setAttribute(attr, token.getAttribute(attr));
					}
					text.append(t);
				}

				if (inWOC) {
					text.append(wocTag);
				}
				return true;
			}
		} // done with Handle Book, Chapter, and Verse (or commentary equivalent)

		// Now consider everything else.

		// The module intro consists of divs that are not book or bookGroup
		// Do we need to consider other divs that can surround books?
		if (inModuleIntro && tokenName == "div" && typeAttr != "bookGroup" && typeAttr != "book") {
			// keep collecting
			return false;
		}

		// The presence of a bookGroup will close a module intro
		// Do we need to consider other divs that can surround books?
		if (tokenName == "div" && typeAttr == "bookGroup") {
			if (inModuleIntro) {
				// Setting the testament will set Book, Chapter and Verse to 0
				currentVerse.setTestament(0);

				if (debug & DEBUG_TITLE) {
					std::cout << identifyMsg("DEBUG", "TITLE", currentVerse)
						  << "MODULE INTRO "
						  << text
						  << std::endl;
				}

				writeEntry(text);

				inModuleIntro = false;
			}
			return false;
		}

		// Handle WOC quotes.
		// Note this requires transformBSP to make them into milestones
		// Otherwise have to do it here
		if (tokenName == "q") {
			quoteStack.push(token);

			if (debug & DEBUG_QUOTE) {
				std::cout << identifyMsg("DEBUG", "QUOTE", currentOsisID)
					  << "Quote top("
					  << quoteStack.size()
					  << ") "
					  << token
					  << std::endl;
			}

			if (token.getAttribute("who") && !strcmp(token.getAttribute("who"), "Jesus")) {
				inWOC = true;

				// Output per verse WOC markup.
				text.append(wocTag);

				// Output the quotation mark if appropriate, inside the WOC.
				// If there is no marker attribute, let the SWORD engine manufacture one.
				// If there is a marker attribute and it has content, then output that.
				// If the marker attribute is present and empty, then there is nothing to do.
				// And have it within the WOC markup
				if (!token.getAttribute("marker") || token.getAttribute("marker")[0]) {
					token.setAttribute("who", 0); // remove the who="Jesus"
					text.append(token);
				}
				return true;
			}
			return false;
		}

		// Have we found the start of pre-verse material?
		// Pre-verse material follows the following rules
		// 1) Between the opening of a book and the first chapter, all the material is handled as an introduction to the book.
		// 2) Between the opening of a chapter and the first verse, the material is split between the introduction of the chapter
		//    and the first verse of the chapter.
		//    A <div> with a type of section, subSection or majorSection when the subType isn't x-introduction
		//      will be taken as surrounding verses.
		//    A <title> of type other than main, chapter or sub, will be taken as a title for the verse.
		//    Once one of these conditions is met, the division between chapter introduction and pre-verse is set.
		// 3) Between verses, the material is split between the prior verse and the next verse.
		//    Basically, while end and empty tags are found, they belong to the prior verse.
		//    Once a begin tag is found, it belongs to the next verse.
		if (inChapter && !inPreVerse) {
			if (inChapterIntro) {
				SWBuf subTypeAttr = token.getAttribute("subType");
				// Determine when we are no longer in a chapter heading, but in pre-verse material:
				// If we see one of the following:
				//     a section, subSection, majorSection div that's not marked with a subType of "x-introduction"
				//     a title that is not main, chapter or sub or unclassified (no type attribute)
				if ((tokenName == "div" && (typeAttr == "section" || typeAttr == "subSection" || typeAttr == "majorSection") && subTypeAttr != "x-introduction") ||
				    (tokenName == "title" && typeAttr.length() != 0 && typeAttr != "main" && typeAttr != "chapter" && typeAttr != "sub")
				) {
					if (debug & DEBUG_TITLE) {
						std::cout << identifyMsg("DEBUG", "TITLE", currentOsisID)
							  << "Done looking for chapter introduction"
							  << std::endl;
					}

					if (text.length()) {
						if (debug & DEBUG_TITLE) {
							std::cout << identifyMsg("DEBUG", "TITLE", currentOsisID)
								  << "CHAPTER INTRO "
								  << text
								  << std::endl;
						}

						// Since we have found the boundary, we need to write out the chapter heading
						writeEntry(text);
					}
					// And we are no longer in the chapter heading
					inChapterIntro  = false;
					// But rather, we are now in pre-verse material
					inPreVerse      = true;
				}
			}
			else if (!inVerse && inChapter) {
				inPreVerse = true;
			}

			if (inPreVerse) {
				char genBuf[200];
				sprintf(genBuf, "<div type=\"x-milestone\" subType=\"x-preverse\" sID=\"pv%d\"/>", genID);
				text.append(genBuf);
			}
		}

		if (debug & DEBUG_INTERVERSE) {
			if (!inVerse && inChapter) {
				std::cout << identifyMsg("DEBUG", "INTERVERSE", currentOsisID)
					<< "Interverse start token "
					<< token
					<< ":"
					<< text.c_str()
					<< std::endl;
			}
		}

		return false;
	} // Done with procesing start and empty tags

	// Process end tags
	else {

		if (tagStack.empty()) {
			std::cout << identifyMsg("FATAL", "NESTING", currentOsisID)
				  << "End tag expected"
				  << std::endl;
			exit(EXIT_BAD_NESTING);
		}

		// Note: empty end tags have the eID attribute
		if (!token.isEmpty()) {
			XMLTag topToken = tagStack.top();
			tagDepth = tagStack.size();

			if (debug & DEBUG_STACK) {
				std::cout << identifyMsg("DEBUG", "STACK", currentOsisID)
					  << "Pop("
					  << tagDepth
					  << ") "
					  << topToken
					  << std::endl;
			}

			tagStack.pop();

			if (tokenName != topToken.getName()) {
				std::cout << identifyMsg("FATAL", "NESTING", currentOsisID)
					  << "Expected "
					  << topToken.getName()
					  << " found "
					  << tokenName
					  << std::endl;
//				exit(EXIT_BAD_NESTING); // (OSK) I'm sure this validity check is a good idea, but there's a bug somewhere that's killing the converter here.
						// So I'm disabling this line. Unvalidated OSIS files shouldn't be run through the converter anyway.
						// (DM) This has nothing to do with well-form or valid. It checks milestoned elements for proper nesting.
			}
		}

		// We haven't seen the first div outside the header so there is little to do.
		if (!inModule) {
			if (tokenName == "header") {
				headerEnded = true;

				if (debug & DEBUG_OTHER) {
					std::cout << identifyMsg("DEBUG", "FOUND")
						  << "End of header found"
						  << std::endl;
				}
			}

			// Collect the content so it can be used to suggest the module's conf.
			return false;
		}

		// VERSE and COMMENTARY END
		if ((tokenName == "verse") ||
		    (tokenName == "div" && eidAttr == sidVerse)
		) {

			if (tagDepth != verseDepth) {
				std::cout << identifyMsg("WARNING", "NESTING", currentOsisID)
					  << "Verse is not well formed."
					  << " verseDepth=" << verseDepth
					  << " tagDepth=" << tagDepth
					  << std::endl;
			}

			// If we are in WOC then we need to terminate the <q who="Jesus" marker=""> that was added earlier in the verse.
			if (inWOC) {
				text.append("</q>");
			}

			// Include the token if it is not a verse
			if (tokenName != "verse") {
				text.append(token);
			}
			else if (debug & DEBUG_VERSE)
			{
				// transform the verse into a milestone
				XMLTag t = "<milestone resp=\"v\" />";
				// copy all the attributes of the verse element to the milestone
				StringList attrNames = token.getAttributeNames();
				for (StringList::iterator loop = attrNames.begin(); loop != attrNames.end(); loop++) {
					const char* attr = (*loop).c_str();
					t.setAttribute(attr, token.getAttribute(attr));
				}
				text.append(t);
			}

			writeEntry(text);

			inVerse     = false;
			inPreVerse  = false;
			verseDepth  = 0;

			return true;
		}

		// Handle WOC quotes.
		// Note this requires transformBSP to make them into milestones
		// Otherwise have to manage it here
		if (tokenName == "q") {
			XMLTag topToken = quoteStack.top();

			if (debug & DEBUG_QUOTE) {
				std::cout << identifyMsg("DEBUG", "QUOTE", currentOsisID)
					  << "Quote pop(" << quoteStack.size() << ") "
					  << topToken << " -- " << token
					  << std::endl;
			}

			quoteStack.pop();

			// If we have found an end tag for a <q who="Jesus"> then we are done with the WOC
			// and we need to terminate the <q who="Jesus" marker=""> that was added earlier in the verse.
			if (token.getAttribute("who") && !strcmp(token.getAttribute("who"), "Jesus")) {

				if (debug & DEBUG_QUOTE) {
					std::cout << identifyMsg("DEBUG", "QUOTE", currentOsisID)
						  << "(" << quoteStack.size() << ") "
						  << topToken << " -- " << token
						  << std::endl;
				}

				inWOC = false;
				const char *sID = topToken.getAttribute("sID");
				const char *eID = token.getAttribute("eID");
				if (!sID) {
					sID = "";
				}
				if (!eID) {
					eID = "";
				}
				if (strcmp(sID, eID)) {
					std::cout << identifyMsg("ERROR", "NESTING", currentOsisID)
						  << "Improper nesting. Matching (sID,eID) not found. Looking at ("
						  << sID << "," << eID << ")"
						  << std::endl;
				}

				// Output the quotation mark if appropriate, inside the WOC.
				// If there is no marker attribute, let the SWORD engine manufacture one.
				// If there is a marker attribute and it has content, then output that.
				// If the marker attribute is present and empty, then there is nothing to do.
				// And have it within the WOC markup
				if (!token.getAttribute("marker") || token.getAttribute("marker")[0]) {
					token.setAttribute("who", 0); // remove the who="Jesus"
					text.append(token);
				}

				// Now close the WOC
				text.append("</q>");
				return true;
			}
			return false;
		}

		bool inIntro = inModuleIntro || inBookIntro || inChapterIntro;
		// Look for the end of document, book and chapter
		// Also for material that goes with last entry
		if (!inVerse && !inIntro) {
			// Is this the end of a chapter.
			if ((tokenName == "chapter") ||
			    (tokenName == "div" && eidAttr == sidChapter)
			) {
				text.append(token);
				writeEntry(text);
				inChapter    = false;
				sidChapter   = "";
				chapterDepth = 0;
				verseDepth   = 0;
				return true;
			}

			// Is it the end of a book
			if (tokenName == "div" && eidAttr == sidBook) {
				text.append(token);
				writeEntry(text);
				bookDepth    = 0;
				chapterDepth = 0;
				verseDepth   = 0;
				inBook       = false;
				return true;
			}

			// Do we need to consider other divs that can surround books?
			if (tokenName == "div" && typeAttr == "bookGroup") {
				text.append(token);
				writeEntry(text);
				return true;
			}

			// Do not include the end of an osis document
			if (tokenName == "osisText" || tokenName == "osis") {
				bookDepth    = 0;
				chapterDepth = 0;
				verseDepth   = 0;
				return true;
			}

			// Within a book, when we are not inPreVerse, the interverse tags get appended to the preceeding verse.
			if (!inPreVerse && inBook) {
				text.append(token);
				writeEntry(text);

				if (debug & DEBUG_INTERVERSE) {
					std::cout << identifyMsg("DEBUG", "INTERVERSE", currentOsisID)
						  << "Appending interverse end tag: "
						  << token
						  << " tagDepth="
						  << tagDepth
						  << " chapterDepth="
						  << chapterDepth
						  << " bookDepth="
						  << bookDepth
						  << std::endl;
				}

				return true;
			}

			if (debug & DEBUG_INTERVERSE) {
				std::cout << identifyMsg("DEBUG", "INTERVERSE", currentOsisID)
					  << "Interverse end tag: "
					  << token
					  << " tagDepth="
					  << tagDepth
					  << " chapterDepth="
					  << chapterDepth
					  << " bookDepth="
					  << bookDepth
					  << std::endl;
			}

			return false;
		}

		return false;
	} // done with Processing end tags

	return false;
}

/**
 * Support normalizations necessary for a SWORD module.
 * OSIS allows for document structure (Book, Section, Paragraph or BSP)
 * to overlap Bible versification (Book, Chapter, Verse).
 * Most SWORD applications need to display verses in isolation or in HTML table cells,
 * requiring each stored entry (i.e. verses) to be well-formed xml.
 * This routine normalizes container elements which could cross verse boundaries into milestones.
 * For most of these OSIS elements, there is a milestone form. However, p is not milestoneable.
 * For this reason, p is transformed into div elements with type x-p.
 * param t the tag to transform
 * return the transformed tag or the original one
 */
XMLTag transformBSP(XMLTag t) {
	static std::stack<XMLTag> bspTagStack;
	static int sID = 1;
	char buf[11];
	SWBuf typeAttr = t.getAttribute("type");

	// Support simplification transformations
	if (t.isEmpty()) {
		return t;
	}

	SWBuf tagName = t.getName();
	XMLTag orig = t;
	bool changed = false;
	if (!t.isEndTag()) {
		// Transform <p> into <div type="x-p"> and milestone it
		if (tagName == "p") {
			t.setText("<div type=\"x-p\" />");
			sprintf(buf, "gen%d", sID++);
			t.setAttribute("sID", buf);
			changed = true;
		}

		// Transform <tag> into <tag  sID="">, where tag is a milestoneable element.
		// The following containers are milestoneable.
		// abbr, closer, div, foreign, l, lg, salute, signed, speech
		// Leaving out:
		//   abbr       When would this ever cross a boundary?
		//   seg        as it is used for a divineName hack
		//   foreign    so that it can be easily italicized
		//   div type="colophon" so that it can be treated as a block
		else if (tagName == "chapter" ||
			 tagName == "closer"  ||
			 (tagName == "div" && typeAttr != "colophon") ||
			 tagName == "l"       ||
			 tagName == "lg"      ||
			 tagName == "q"       ||
			 tagName == "salute"  ||
			 tagName == "signed"  ||
			 tagName == "speech"  ||
			 tagName == "verse"
		) {
			t.setEmpty(true);
			if (tagName == "verse" || tagName == "chapter" || (tagName == "div" && typeAttr == "book")) {
				t.setAttribute("sID", t.getAttribute("osisID"));
			} else {
				sprintf(buf, "gen%d", sID++);
				t.setAttribute("sID", buf);
			}
			changed = true;
		}
		bspTagStack.push(t);

		if (changed && debug & DEBUG_XFORM) {
			std::cout << identifyMsg("DEBUG", "XFORM", currentOsisID)
				  << "Transform start tag from "
				  << orig
				  << " to "
				  << t
				  << std::endl;
		}
	}
	else {
		if (!bspTagStack.empty()) {
			XMLTag topToken = bspTagStack.top();

			// <p> is transformed to <div ...>
			if (tagName != "p" && strcmp(tagName, topToken.getName())) {
				std::cout << identifyMsg("FATAL", "XFORM", currentOsisID)
					  << "Closing tag ("
					  << tagName
					  << ") does not match opening tag ("
					  << topToken.getName()
					  << ")"
					  << std::endl;
			}

			bspTagStack.pop();
			SWBuf topTypeAttr = topToken.getAttribute("type");

			// Look for the milestoneable container tags handled above.
			// Have to treat div type="colophon" differently
			if (tagName == "chapter" ||
			    tagName == "closer"  ||
			    (tagName == "div" && topTypeAttr != "colophon") ||
			    tagName == "l"       ||
			    tagName == "lg"      ||
			    tagName == "p"       ||
			    tagName == "q"       ||
			    tagName == "salute"  ||
			    tagName == "signed"  ||
			    tagName == "speech"  ||
			    tagName == "verse"
			) {
				// make this a clone of the start tag with sID changed to eID
				// Note: in the case of </p> the topToken is a <div type="x-p">
				t = topToken;
				t.setAttribute("eID", t.getAttribute("sID"));
				t.setAttribute("sID", 0);
				changed = true;
			}

			if (changed && debug & DEBUG_XFORM) {
				std::cout << identifyMsg("DEBUG", "XFORM", currentOsisID)
					  << "Transform end tag from "
					  << orig
					  << " to "
					  << t
					  << std::endl;
			}
		}
		else {
			std::cout << identifyMsg("FATAL", "XFORM", currentOsisID)
				  << "Closing tag without opening tag"
				  << std::endl;
		}
	}

	return t;
}

/**
 * Write out all links in the module.
 * Waiting is necessary because writeEntry might ultimately append
 * text to a verse moving it's offset in the data file.
 * While we are minimizing it by postponing the write until we have
 * gathered the next verse, the following scenario is happening:
 * A module is using linked verses and has some verses that are not
 * in the chosen versification. If the out-of-canon verse happens following
 * a linked verse, the out-of-canon verse is appended to the prior
 * verse. Care has to be taken that the linked verses all point to
 * the first of the set.
 */
void writeLinks()
{
	// Link all the verses
	VerseKey destKey;
	destKey.setVersificationSystem(v11n);
	destKey.setAutoNormalize(false);
	destKey.setIntros(true);

	VerseKey linkKey;
	linkKey.setVersificationSystem(v11n);
	linkKey.setAutoNormalize(false);
	linkKey.setIntros(true);
	for (unsigned int i = 0; i < linkedVerses.size(); i++) {
		// The verseKeys is a list of verses
		// where the first is the real verse
		// and the others link to it.
		ListKey verseKeys = linkedVerses[i];
		verseKeys.setPosition(TOP);
		destKey = verseKeys.getElement();
		verseKeys.increment(1);

		while (!verseKeys.popError()) {
			linkKey = verseKeys.getElement();
			linkToEntry(linkKey, destKey);
			verseKeys.increment(1);
		}
	}
}

void usage(const char *app, const char *error = 0, const bool verboseHelp = false) {
	
	if (error) fprintf(stderr, "\n%s: %s\n", app, error);
	
	fprintf(stderr, "OSIS Bible/commentary module creation tool for The SWORD Project\n");
	fprintf(stderr, "\nusage: %s <output/path> <osisDoc> [OPTIONS]\n", app);
	fprintf(stderr, "  <output/path>\t\t an existing folder that the module will be written\n");
	fprintf(stderr, "  <osisDoc>\t\t path to the validated OSIS document, or '-' to\n");
	fprintf(stderr, "\t\t\t\t read from standard input\n");
	fprintf(stderr, "  -a\t\t\t augment module if exists (default is to create new)\n");
	fprintf(stderr, "  -z <l|z|b|x>\t\t compression type (default: none)\n");
	fprintf(stderr, "\t\t\t\t l - LZSS; z - ZIP; b - bzip2; x - xz\n");
	fprintf(stderr, "  -b <2|3|4>\t\t compression block size (default: 4)\n");
	fprintf(stderr, "\t\t\t\t 2 - verse; 3 - chapter; 4 - book\n");
	fprintf(stderr, "  -l <1-9>\t\t compression level (default varies by compression type)\n");
	fprintf(stderr, "  -c <cipher_key>\t encipher module using supplied key\n");
	fprintf(stderr, "\t\t\t\t (default no enciphering)\n");

#ifdef _ICU_
	fprintf(stderr, "  -e <1|2|s>\t\t convert Unicode encoding (default: 1)\n");
	fprintf(stderr, "\t\t\t\t 1 - UTF-8 ; 2 - UTF-16 ; s - SCSU\n");
	fprintf(stderr, "  -N\t\t\t do not normalize to NFC\n");
	if (verboseHelp) {
		fprintf(stderr, "\t\t\t\t (default is to convert to UTF-8, if needed,\n");
		fprintf(stderr, "\t\t\t\t  and then normalize to NFC)\n");
		fprintf(stderr, "\t\t\t\t Note: UTF-8 texts should be normalized to NFC.\n");
	}
#endif

	fprintf(stderr, "  -s <2|4>\t\t bytes used to store entry size (default is 2).\n");
	if (verboseHelp) {
		fprintf(stderr, "\t\t\t\t Note: useful for commentaries with very large\n");
		fprintf(stderr, "\t\t\t\t entries in uncompressed modules\n");
		fprintf(stderr, "\t\t\t\t (2 bytes to store size equal 65535 characters)\n");
	}
	fprintf(stderr, "  -v <v11n>\t\t specify a versification scheme to use (default is KJV)\n");
	fprintf(stderr, "\t\t\t\t Note: This is case insensitive and allows unique prefixes, e.g. cal for Calvin\n");
	fprintf(stderr, "\t\t\t\t Note: The following are valid values for v11n:");

	VersificationMgr *vmgr = VersificationMgr::getSystemVersificationMgr();
	StringList av11n = vmgr->getVersificationSystems();
	for (StringList::iterator loop = av11n.begin(); loop != av11n.end(); loop++) {
		if ((distance(av11n.begin(), loop) % 3) == 0) {
			fprintf(stderr, "\n\t\t\t\t   %-12s", (*loop).c_str());
		}
		else {
			fprintf(stderr, "\t%-12s", (*loop).c_str());
		}
	}
	fprintf(stderr, "\n");
	
	if (verboseHelp) {
		fprintf(stderr, "  -d <flags>\t\t turn on debugging (default is 0)\n");
		fprintf(stderr, "\t\t\t\t Note: This flag may change in the future.\n");
		fprintf(stderr, "\t\t\t\t Flags: The following are valid values:\n");
		fprintf(stderr, "\t\t\t\t\t0   - no debugging\n");
		fprintf(stderr, "\t\t\t\t\t1   - writes to module, very verbose\n");
		fprintf(stderr, "\t\t\t\t\t2   - verse start and end\n");
		fprintf(stderr, "\t\t\t\t\t4   - quotes, esp. Words of Christ\n");
		fprintf(stderr, "\t\t\t\t\t8   - titles\n");
		fprintf(stderr, "\t\t\t\t\t16  - inter-verse material\n");
		fprintf(stderr, "\t\t\t\t\t32  - BSP to BCV transformations\n");
		fprintf(stderr, "\t\t\t\t\t64  - v11n exceptions\n");
		fprintf(stderr, "\t\t\t\t\t128 - parsing of osisID and osisRef\n");
		fprintf(stderr, "\t\t\t\t\t256 - internal stack\n");
		fprintf(stderr, "\t\t\t\t\t512 - miscellaneous\n");
		fprintf(stderr, "\t\t\t\t\t1024 - parsing of numeric and character entities and comments.\n");
		fprintf(stderr, "\t\t\t\t This argument can be used more than once. (Or\n");
		fprintf(stderr, "\t\t\t\t the flags may be added together.)\n");
	}
	fprintf(stderr, "  -h \t\t\t print verbose usage text\n");
	
	fprintf(stderr, "\n");
	fprintf(stderr, "See http://www.crosswire.org/wiki/osis2mod for more details.\n");
	fprintf(stderr, "\n");
	exit(EXIT_BAD_ARG);
}

// Maximum length for an entity (including & and ;), sufficient for valid XML/HTML entities
constexpr size_t MAX_ENTITY_LENGTH = 32;

// Enum for entity types
enum class EntityType { START, NUM_HASH, NUM_DEC, NUM_HEX, CHAR, ERR };

enum class CommentState {
	START,         // Not in a comment or have seen '<'
	SLAM,          // Seen '<!'
	DASH1,         // Seen '<!-'
	COMMENT,       // Having seen '<--' inside comment content
	END_DASH1,     // Seen '-' in comment
	END_DASH2      // Seen '--' in comment
};

/**
 * @brief Handles XML comment parsing for a single character at a time.
 * @param c The current character to process.
 * @param currentOsisID The current OSIS ID for error reporting.
 * @param incomment Whether currently inside a comment.
 * @param commentstate The current comment parsing state.
 * @param token The token buffer to append characters during comment start.
 * @return true if the character is consumed (continue loop), false otherwise.
 */
bool handleComment(unsigned char c, const char* currentOsisID, bool& intoken, bool& incomment, CommentState& commentstate, SWBuf& token) {
	if (!incomment) {
		switch (commentstate) {
		case CommentState::START:
			if (c == '!') {
				if (debug & DEBUG_PARSE) {
					std::cout << identifyMsg("DEBUG", "COMMENTS")
						  << "Found <!"
						  << std::endl;
				}
				commentstate = CommentState::SLAM;
				token.append((char)c);
				return true;
			}
			return false;

		case CommentState::SLAM:
			if (c == '-') {
				if (debug & DEBUG_PARSE) {
					std::cout << identifyMsg("DEBUG", "COMMENTS")
						  << "Found <!-"
						  << std::endl;
				}
				commentstate = CommentState::DASH1;
				token.append((char)c);
				return true;
			}
			commentstate = CommentState::START;
			return false;

		case CommentState::DASH1:
			if (c == '-') { // having seen the second dash we are in the comment
				if (debug & DEBUG_PARSE) {
					std::cout << identifyMsg("DEBUG", "COMMENTS")
						  << "Found <!-- Now in comment."
						  << std::endl;
				}
				incomment = true;
				commentstate = CommentState::COMMENT;
				token.append((char)c);
				if (debug & DEBUG_PARSE) {
					std::cout << identifyMsg("DEBUG", "COMMENTS")
						  << "In comment"
						  << std::endl;
				}
				return true;
			}
			commentstate = CommentState::START;
			return false;

		default:
			std::cout << identifyMsg("FATAL", "COMMENTS")
				  << "Unknown commentstate on comment start: "
				  << (int) commentstate
				  << std::endl;
			exit(EXIT_BAD_COMMENT);
		}
	}
	else {
		switch (commentstate) {
		case CommentState::COMMENT:
			if (c == '-') {
				if (debug & DEBUG_PARSE) {
					std::cout << identifyMsg("DEBUG", "COMMENTS")
						  << "Found - in comment."
						  << std::endl;
				}
				commentstate = CommentState::END_DASH1;
				return true;
			}
			// Ignore the character
			return true;

		case CommentState::END_DASH1:
			if (c == '-') {
				if (debug & DEBUG_PARSE) {
					std::cout << identifyMsg("DEBUG", "COMMENTS")
						  << "Found -- in comment."
						  << std::endl;
				}
				commentstate = CommentState::END_DASH2;
				return true;
			}
			// Ignore the character
			commentstate = CommentState::COMMENT;
			return true;

		case CommentState::END_DASH2:
			if (c == '>') { // having seen the --> we are done and return to the original state
				if (debug & DEBUG_PARSE) {
					std::cout << identifyMsg("DEBUG", "COMMENTS")
						  << "Found --> comment ended."
						  << std::endl;
				}
				intoken = false;
				incomment = false;
				commentstate = CommentState::START;
				if (debug & DEBUG_PARSE) {
					std::cout << identifyMsg("DEBUG", "COMMENTS")
						  << "Out of comment"
						  << std::endl;
				}
				return true;
			}
			// Ignore the character
			commentstate = CommentState::COMMENT;
			return true;

		default:
			std::cout << identifyMsg("FATAL", "COMMENTS")
				  << "Unknown commentstate on comment end: "
				  << (int) commentstate
				  << std::endl;
			exit(EXIT_BAD_COMMENT);
		}
	}
	return false; // Should never reach here
}

/**
 * \brief Handles &apos; and &quot; entities, converting them to plain characters or keeping them based on attribute context.
 *
 * This function processes `&apos;` and `&quot;` entities, replacing them with `'` or `"` respectively when outside attributes
 * or when used in attributes with non-matching quote characters (e.g., `&apos;` in a double-quoted attribute). It logs
 * appropriate warning messages and updates the entityToken with the converted value.
 *
 * \param entityToken [in/out] The entity string (e.g., "&apos;" or "&quot;") to process; modified to the converted value (e.g., "'" or "&apos;").
 * \param currentOsisID [in] The OSIS ID for context in warning messages.
 * \param msgPrefix [in] Pre-formatted message prefix for logging (includes level, type, and OSIS ID).
 * \param inattribute [in] True if the entity is within an attribute value, false otherwise.
 * \param attrQuoteChar [in] The quote character (' or ") used in the attribute, or '\0' if not applicable.
 * \param debug [in] Debug flags from osis2mod; logs if (debug & DEBUG_PARSE) is set.
 *
 * \note Logs warnings to std::cout if (debug & DEBUG_PARSE).
 * \note Thread-safe as it does not modify shared state beyond std::cout.
 */
void handleQuoteEntity(SWBuf& entityToken, const char* currentOsisID, SWBuf& msgPrefix, bool inattribute, char attrQuoteChar) {
	if (entityToken == "&apos;") {
		if (!inattribute) {
			if (debug & DEBUG_PARSE) {
				std::cout << msgPrefix
					  << "&apos; unnecessary outside attributes. Replacing with '."
					  << std::endl;
			}
			entityToken = "'";
		} else if (attrQuoteChar == '"') {
			if (debug & DEBUG_PARSE) {
				std::cout << msgPrefix
					  << "&apos; unnecessary in double-quoted attributes. Replacing with '."
					  << std::endl;
			}
			entityToken = "'";
		} else if (attrQuoteChar == '\'') {
			if (debug & DEBUG_PARSE) {
				std::cout << msgPrefix
					  << "&apos; only needed in single-quoted attributes. Consider double quotes." 
					  << std::endl;
			}
		} else {
			if (debug & DEBUG_PARSE) {
				std::cout << identifyMsg("ERROR", "PARSE", currentOsisID)
					  << "Invalid attrQuoteChar: "
				 	  << attrQuoteChar
					  << std::endl;
			}
		}
	} else if (entityToken == "&quot;") {
		if (!inattribute) {
			if (debug & DEBUG_PARSE) {
				std::cout << msgPrefix
					  << "&quot; unnecessary outside attributes. Replacing with \"." 
					  << std::endl;
			}
			entityToken = "\"";
		} else if (attrQuoteChar == '\'') {
			if (debug & DEBUG_PARSE) {
				std::cout << msgPrefix
					  << "&quot; unnecessary in single-quoted attributes. Replacing with \"." 
					  << std::endl;
			}
			entityToken = "\"";
		} else if (attrQuoteChar == '"') {
			if (debug & DEBUG_PARSE) {
				std::cout << msgPrefix
			  		  << "&quot; only needed in double-quoted attributes. Consider single quotes."
					  << std::endl;
			}
		} else {
			if (debug & DEBUG_PARSE) {
				std::cout << identifyMsg("ERROR", "PARSE", currentOsisID)
					  << "Invalid attrQuoteChar: " 
					  << attrQuoteChar 
					  << std::endl;
			}
		}
	}
}

/**
 * \brief Converts a validated Unicode code point to its UTF-8 representation.
 *
 * This function takes a pre-parsed Unicode code point (1 to 0x10FFFF) and converts it to its UTF-8 encoded form,
 * storing the result in entityToken. It handles single-byte, two-byte, three-byte, and four-byte UTF-8 sequences
 * based on the code point value. The original entity string is provided for diagnostic logging.
 *
 * \param entityToken [in/out] The original entity string (e.g., "&#65;") for logging; modified to contain the UTF-8 encoded character(s).
 * \param codepoint [in] The Unicode code point (1 to 0x10FFFF) to convert to UTF-8.
 * \param msgPrefix [in] Pre-formatted message prefix for logging (includes level, type, and OSIS ID).
 *
 * \return Always returns true, as the codepoint is assumed to be valid.
 *
 * \note The codepoint must be pre-validated (1 to 0x10FFFF) by the caller to avoid undefined behavior.
 * \note Logs conversion details to std::cout if (debug & DEBUG_PARSE).
 * \note Thread-safe as it does not modify shared state beyond std::cout.
 */
void convertNumericEntityToUTF8(SWBuf& entityToken, long codepoint, SWBuf& msgPrefix) {
	// Save original entity for logging
	SWBuf originalEntity = entityToken;

	// Convert to UTF-8
	if (codepoint <= 0x7F) {
		entityToken.setSize(1);
		entityToken[0] = static_cast<char>(codepoint);
	} else if (codepoint <= 0x7FF) {
		entityToken.setSize(2);
		entityToken[0] = 0xC0 | (codepoint >> 6);
		entityToken[1] = 0x80 | (codepoint & 0x3F);
	} else if (codepoint <= 0xFFFF) {
		entityToken.setSize(3);
		entityToken[0] = 0xE0 | (codepoint >> 12);
		entityToken[1] = 0x80 | ((codepoint >> 6) & 0x3F);
		entityToken[2] = 0x80 | (codepoint & 0x3F);
	} else {
		entityToken.setSize(4);
		entityToken[0] = 0xF0 | (codepoint >> 18);
		entityToken[1] = 0x80 | ((codepoint >> 12) & 0x3F);
		entityToken[2] = 0x80 | ((codepoint >> 6) & 0x3F);
		entityToken[3] = 0x80 | (codepoint & 0x3F);
	}
	if (debug & DEBUG_PARSE) {
		std::cout << msgPrefix
			  << "Converted numeric entity "
			  << originalEntity
			  << " to UTF-8 character "
			  << entityToken
			  << std::endl;
	}
}

/**
 * \brief Parses and processes XML/HTML entities in a character stream using a finite state automaton.
 *
 * This function processes a single character in the context of an XML/HTML entity, maintaining a finite state automaton
 * to track entity parsing states (START, NUM_HASH, NUM_DEC, NUM_HEX, CHAR, ERR). It handles named entities (e.g., &amp;),
 * numeric entities (e.g., &#65;, &#x41;), and malformed entities. Special numeric entities (e.g., &#38; to &amp;) are
 * converted to named entities, while others are converted to UTF-8. The function updates the entityToken and appends
 * results to either the text or token buffer based on intoken. Malformed entities are replaced with &amp; followed by
 * the invalid sequence.
 *
 * \param curChar [in] The current character to process.
 * \param inentity [in/out] True if currently parsing an entity (starts with '&'), false otherwise.
 * \param inWhitespace [in/out] True if the parser is in a whitespace sequence, reset when an entity starts.
 * \param entitytype [in/out] The current state of the entity parser (START, NUM_HASH, NUM_DEC, NUM_HEX, CHAR, ERR).
 * \param entityToken [in/out] The current entity being built (e.g., "&amp;" or "&#65;"); modified with the converted value.
 * \param token [in/out] Buffer for entity output if intoken is true (e.g., within a tag).
 * \param text [in/out] Buffer for entity output if intoken is false (e.g., plain text).
 * \param intoken [in] True if the entity is within a token (e.g., tag name or attribute), false for plain text.
 * \param inattribute [in] True if the entity is within an attribute value, false otherwise.
 * \param attrQuoteChar [in] The quote character (' or ") used in the attribute, or '\0' if not applicable.
 * \param currentOsisID [in] The OSIS ID for context in warning messages.
 *
 * \return True if the character was consumed by the entity parser, false otherwise.
 *
 * \note Logs warnings and errors to std::cout if (debug & DEBUG_PARSE).
 * \note Thread-safe as long as inentity, inWhitespace, entitytype, entityToken, token, and text are not shared across threads without synchronization.
 * \note Uses SWBuf::operator<< for shifting entityToken in error cases.
 * \note Throws std::runtime_error for invalid entitytype values.
 */
bool handleEntity(char curChar, bool& inentity, bool& inWhitespace, EntityType& entitytype,
		SWBuf& entityToken, SWBuf& token, SWBuf& text, bool intoken,
		bool inattribute, char attrQuoteChar, const char* currentOsisID) {
	if (!inentity && curChar != '&') {
		return false; // Fast-path for non-entity characters
	}
	if (!inentity && curChar == '&') {
		inentity = true;
		inWhitespace = false;
		entitytype = EntityType::START;
		entityToken = "&";
		return true;
	}
	if (inentity) {
		if (entityToken.length() >= MAX_ENTITY_LENGTH) {
			inentity = false;
			entitytype = EntityType::ERR;
			if (debug & DEBUG_PARSE) {
				auto msgPrefix = identifyMsg("WARNING", "PARSE", currentOsisID);
				std::cout << msgPrefix
					  << "Entity length exceeds maximum ("
					  << MAX_ENTITY_LENGTH
					  << " characters), treating as malformed: "
					  << entityToken
					  << std::endl;
			}
		} else if (curChar == ';') {
			inentity = false;
		}
		if (entitytype != EntityType::ERR) {
			entityToken.append(curChar);
		}
		if (inentity) {
			switch (entitytype) {
			case EntityType::START:
				if (curChar == '#') {
					entitytype = EntityType::NUM_HASH;
				} else if (std::isalnum(curChar)) {
					entitytype = EntityType::CHAR;
				} else {
					inentity = false;
					entitytype = EntityType::ERR;
				}
				break;
			case EntityType::NUM_HASH:
				if (curChar == 'x' || curChar == 'X') {
					entitytype = EntityType::NUM_HEX;
				} else if (std::isdigit(curChar)) {
					entitytype = EntityType::NUM_DEC;
				} else {
					inentity = false;
					entitytype = EntityType::ERR;
				}
				break;
			case EntityType::NUM_DEC:
				if (!std::isdigit(curChar)) {
					inentity = false;
					entitytype = EntityType::ERR;
				}
				break;
			case EntityType::NUM_HEX:
				if (!std::isxdigit(curChar)) {
					inentity = false;
					entitytype = EntityType::ERR;
				}
				break;
			case EntityType::CHAR:
				if (!std::isalnum(curChar)) {
					inentity = false;
					entitytype = EntityType::ERR;
				}
				break;
			default:
				std::cout << identifyMsg("FATAL", "PARSE")
					  << "Unknown EntityType: "
					  << (int) entitytype
					  << std::endl;
				exit(EXIT_BAD_ENTITY);
			}
			return true;
		}
		if (!inentity) {
			auto msgPrefix = identifyMsg("WARNING", "PARSE", currentOsisID);
			// Handle numeric entities before switch
			if (entitytype == EntityType::NUM_DEC || entitytype == EntityType::NUM_HEX) {
				const char* p = entityToken.c_str();
				p += 2; // Skip &#
				int base = 10;
				if (*p == 'x' || *p == 'X') {
					base = 16;
					++p;
				}
				char* end = nullptr;
				errno = 0;
				long codepoint = strtol(p, &end, base);
				bool isValid = end && *end == ';' && codepoint > 0 && codepoint <= 0x10FFFF && errno != ERANGE;
				if (isValid) {
					switch (codepoint) {
					case 38: // & -> &amp;
						if (debug & DEBUG_PARSE) {
							std::cout << msgPrefix
								  << "Converted numeric entity "
								  << entityToken
								  << " to named entity &amp;"
								  << std::endl;
						}
						entityToken = "&amp;";
						entitytype = EntityType::CHAR;
						break;
					case 60: // < -> &lt;
						if (debug & DEBUG_PARSE) {
							std::cout << msgPrefix
								  << "Converted numeric entity "
								  << entityToken
								  << " to named entity &lt;"
								  << std::endl;
						}
						entityToken = "&lt;";
						entitytype = EntityType::CHAR;
						break;
					case 62: // > -> &gt;
						if (debug & DEBUG_PARSE) {
							std::cout << msgPrefix
								  << "Converted numeric entity "
								  << entityToken
								  << " to named entity &gt;"
								  << std::endl;
						}
						entityToken = "&gt;";
						entitytype = EntityType::CHAR;
						break;
					case 34: // " -> &quot;
						if (debug & DEBUG_PARSE) {
							std::cout << msgPrefix
								  << "Converted numeric entity "
								  << entityToken
								  << " to named entity &quot;"
								  << std::endl;
						}
						entityToken = "&quot;";
						entitytype = EntityType::CHAR;
						break;
					case 39: // ' -> &apos;
						if (debug & DEBUG_PARSE) {
							std::cout << msgPrefix
								  << "Converted numeric entity "
								  << entityToken
								  << " to named entity &apos;"
								  << std::endl;
						}
						entityToken = "&apos;";
						entitytype = EntityType::CHAR;
						break;
					default:
						// Non-special codepoints go to UTF-8 conversion
						break;
					}
				} else {
					if (debug & DEBUG_PARSE) {
						std::cout << msgPrefix
							  << "Invalid numeric entity, codepoint out of range or malformed: " 
							  << entityToken
							  << std::endl;
					}
					entitytype = EntityType::ERR;
				}
				// Handle non-special valid codepoints
				if (entitytype == EntityType::NUM_DEC || entitytype == EntityType::NUM_HEX) {
					convertNumericEntityToUTF8(entityToken, codepoint, msgPrefix);
				}
			}
			switch (entitytype) {
			case EntityType::ERR:
				entityToken << 1;
				if (debug & DEBUG_PARSE) {
					std::cout << msgPrefix
						  << "Malformed entity, replacing with &amp;" 
						  << entityToken
						  << std::endl;
				}
				(intoken ? token : text).append("&amp;").append(entityToken);
				break;
			case EntityType::NUM_HEX:
			case EntityType::NUM_DEC:
				(intoken ? token : text).append(entityToken);
				break;
			case EntityType::CHAR:
				if (entityToken != "&amp;" && entityToken != "&lt;" && 
				    entityToken != "&gt;" && entityToken != "&quot;" && 
				    entityToken != "&apos;") {
					if (debug & DEBUG_PARSE) {
						std::cout << msgPrefix 
							  << "XML only supports &amp;, &lt;, &gt;, &quot;, &apos;, found " 
							  << entityToken
							  << std::endl;
					}
					(intoken ? token : text).append(entityToken);
				} else if (entityToken == "&apos;" || entityToken == "&quot;") {
					handleQuoteEntity(entityToken, currentOsisID, msgPrefix, inattribute, attrQuoteChar);
					(intoken ? token : text).append(entityToken);
				} else {
					(intoken ? token : text).append(entityToken);
				}
				break;
			default:
				(intoken ? token : text).append(entityToken);
				break;
			}
			if (curChar == ';') {
				return true;
			}
		}
	}
		
	return false;
}

void processOSIS(std::istream& infile) {

	strcpy(currentOsisID,"N/A");

	currentVerse.setVersificationSystem(v11n);
	currentVerse.setAutoNormalize(false);
	currentVerse.setIntros(true);  // turn on mod/testmnt/book/chap headings
	currentVerse.setPersist(true);

	module->setKey(currentVerse);
	module->setPosition(TOP);

	SWBuf token;
	SWBuf text;
	bool incomment = false;
	CommentState commentstate = CommentState::START;
	bool intoken = false;
	bool inWhitespace = false;
	bool seeingSpace = false;
	unsigned char curChar = '\0';
	SWBuf entityToken;
	bool inentity = false;
	EntityType entitytype = EntityType::START;
	unsigned char attrQuoteChar = '\0';
	bool inattribute = false;

	linePos = 1;
	charPos = 0;

	while (infile.good()) {

		int possibleChar = infile.get();

		// skip the character if it is bad. infile.good() will catch the problem
		if (possibleChar == -1) {
			continue;
		}

		curChar = (unsigned char) possibleChar;

		// All newlines are simply whitespace
		// Does a SWORD module actually require this?
		if (curChar == '\n') {
			curChar = ' ';
			charPos = 0;
			linePos++;
		}
		charPos++;

		// For entity diagnostics track whether the text is an attribute value
		if (inattribute && (curChar == '\'' || curChar == '"')) {
			if (attrQuoteChar == curChar) {
				inattribute = false;
				attrQuoteChar = '\0';
			}
			else {
				attrQuoteChar = curChar;
			}
		}

		if (intoken && curChar == '=') {
			inattribute = true;
			attrQuoteChar = '\0';
		}

		if (handleEntity(curChar, inentity, inWhitespace, entitytype, entityToken, token, text, intoken, inattribute, attrQuoteChar, currentOsisID)) {
			continue; // Character consumed, move to next
		}

		if (!intoken && curChar == '<') {
			intoken = true;
			token = "<";
			inattribute = false;
			attrQuoteChar = '\0';
			continue;
		}

		// Handle XML comments starting with "<!--", ending with "-->"
		if (intoken && !incomment) {
			if (handleComment(curChar, currentOsisID, intoken, incomment, commentstate, token)) {
				continue; // Character consumed, move to next
			}
		}

		if (incomment && handleComment(curChar, currentOsisID, intoken, incomment, commentstate, token)) {
			continue; // Character consumed, move to next
		}

		// Outside of tokens merge adjacent whitespace
		if (!intoken) {
			seeingSpace = isspace(curChar)!=0;
			if (seeingSpace) {
				if (inWhitespace) {
					continue;
				}
				// convert all whitespace to blanks
				curChar = ' ';
			}
			inWhitespace = seeingSpace;
		}

		if (intoken && curChar == '>') {
			intoken = false;
			inWhitespace = false;
			token.append('>');
			// take this isalpha if out to check for bugs in text
			if (isalpha(token[1]) ||
			    (((token[1] == '/') || (token[1] == '?')) && isalpha(token[2]))) {
				//std::cout << "Handle:" << token.c_str() << std::endl;
				XMLTag t = transformBSP(token.c_str());

				if (!handleToken(text, t)) {
					text.append(t);
				}
			} else {
				std::cout << identifyMsg("WARNING", "PARSE", currentOsisID)
					  << "malformed token: "
					  << token
					  << std::endl;
			}
			continue;
		}

		if (intoken) {
			token.append((char) curChar);
		} else {
			switch (curChar) {
			case '>' :
				std::cout << identifyMsg("WARNING", "PARSE", currentOsisID)
					  << "> should be &gt;"
					  << std::endl;
				text.append("&gt;");
				break;
			case '<' :
				std::cout << identifyMsg("WARNING", "PARSE", currentOsisID)
					  << "< should be &lt;"
					  << std::endl;
				text.append("&lt;");
				break;
			default  :
				text.append((char) curChar);
				break;
			}
		}
	}

	// Force the last entry from the text buffer.
	text = "";
	writeEntry(text, true);
	writeLinks();

#ifdef _ICU_
	if (converted)  fprintf(stderr, "osis2mod converted %d verses to UTF-8\n", converted);
	if (normalized) fprintf(stderr, "osis2mod normalized %d verses to NFC\n", normalized);
#endif
}

int main(int argc, char **argv) {

	fprintf(stderr, "You are running osis2mod: $Rev$ (SWORD: %s)\n", SWVersion::currentVersion.getText());
	
	if (argc > 1) {
		for (int i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
				usage(*argv, "", true);
			}
		}
	}

	// Let's test our command line arguments
	if (argc < 3) {
		usage(*argv);
	}

	// variables for arguments, holding defaults
	const char* program    = argv[0];
	const char* path       = argv[1];
	const char* osisDoc    = argv[2];
	int append             = 0;
	SWBuf compType         = "";
	bool isCommentary      = false;
	int iType              = 4;
	int entrySize          = 0;
	SWBuf cipherKey        = "";
	SWCompress *compressor = 0;
	int compLevel      = 0;

	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "-a")) {
			append = 1;
		}
		else if (!strcmp(argv[i], "-z")) {
			compType = "ZIP";
			if (i+1 < argc && argv[i+1][0] != '-') {
				switch (argv[++i][0]) {
				case 'l': compType = "LZSS"; break;
				case 'z': compType = "ZIP"; break;
				case 'b': compType = "BZIP2"; break;
				case 'x': compType = "XZ"; break;
				}
			}
		}
		else if (!strcmp(argv[i], "-Z")) {
			if (compType.size()) usage(*argv, "Cannot specify both -z and -Z");
			compType = "LZSS";
		}
		else if (!strcmp(argv[i], "-b")) {
			if (i+1 < argc) {
				iType = atoi(argv[++i]);
				if ((iType >= 2) && (iType <= 4)) continue;
			}
			usage(*argv, "-b requires one of <2|3|4>");
		}
		else if (!strcmp(argv[i], "-N")) {
			normalize = false;
		}
		else if (!strcmp(argv[i], "-e")) {
			if (i+1 < argc) {
				switch (argv[++i][0]) {
				case '1': // leave as UTF-8
					outputEncoder = NULL;
					outputDecoder = NULL;
					break;

				case '2':
					outputEncoder = new UTF8UTF16();
					outputDecoder = new UTF16UTF8();
					break;
#ifdef _ICU_
				case 's':
					outputEncoder = new UTF8SCSU();
					outputDecoder = new SCSUUTF8();
					break;
#endif
				default:
					outputEncoder = NULL;
					outputDecoder = NULL;
				}
			}
		}
		else if (!strcmp(argv[i], "-c")) {
			if (i+1 < argc) cipherKey = argv[++i];
			else usage(*argv, "-c requires <cipher_key>");
		}
		else if (!strcmp(argv[i], "-v")) {
			if (i + 1 >= argc) {
				usage(*argv, "-v requires <v11n>");
			}

			const char *arg = argv[++i];
			SWBuf v11nInput = arg;

			VersificationMgr *vmgr = VersificationMgr::getSystemVersificationMgr();
			const StringList &av11ns = vmgr->getVersificationSystems();
			StringList matches = resolve_abbreviation(v11nInput, av11ns);

			if (matches.empty()) {
				SWBuf error = "-v ";
				error += v11nInput;
				error += " is unknown";
				usage(*argv, error);
			}

			if (matches.size() > 1) {
				SWBuf error = "-v ";
				error += v11nInput;
				error += " is ambiguous, matching ";
				bool first = true;
				for (const auto &v : matches) {
					if (!first) {
						error += ", ";
					}
					error += v;
					first = false;
				}
				usage(*argv, error);
			}

			v11n = matches.front();  // single unambiguous match
			std::cout << identifyMsg("INFO", "V11N")
				  << "Using the "
				  << v11n
				  << " versification."
				  << std::endl;
		}
		else if (!strcmp(argv[i], "-s")) {
			if (i+1 < argc) {
				entrySize = atoi(argv[++i]);
				if (entrySize == 2 || entrySize == 4) {
					continue;
				}
			}
			usage(*argv, "-s requires one of <2|4>");
		}
		else if (!strcmp(argv[i], "-C")) {
			isCommentary = true;
		}
		else if (!strcmp(argv[i], "-d")) {
			if (i+1 < argc) debug |= atoi(argv[++i]);
			else usage(*argv, "-d requires <flags>");
		}
		else if (!strcmp(argv[i], "-l")) {
			if (i+1 < argc) {
				compLevel = atoi(argv[++i]);
			}
			else usage(*argv, "-l requires a value from 1-9");
			
			if (compLevel < 0 || compLevel > 10) {
				usage(*argv, "-l requires a value from 1-9");
			}
		}
		else usage(*argv, (((SWBuf)"Unknown argument: ")+ argv[i]).c_str());
	}

	if (isCommentary) isCommentary = true;  // avoid unused warning for now

	if (compType == "LZSS") {
		compressor = new LZSSCompress();
	}
	else if (compType == "ZIP") {
#ifndef EXCLUDEZLIB
		compressor = new ZipCompress();
#else
		usage(*argv, "ERROR: SWORD library not compiled with ZIP compression support.\n\tBe sure libz is available when compiling SWORD library");
#endif
	}
	else if (compType == "BZIP2") {
#ifndef EXCLUDEBZIP2
		compressor = new Bzip2Compress();
#else
		usage(*argv, "ERROR: SWORD library not compiled with bzip2 compression support.\n\tBe sure libbz2 is available when compiling SWORD library");
#endif
	}
	else if (compType == "XZ") {
#ifndef EXCLUDEXZ
		compressor = new XzCompress();
#else
		usage(*argv, "ERROR: SWORD library not compiled with xz compression support.\n\tBe sure liblzma is available when compiling SWORD library");
#endif		
	}

	if (compressor && compLevel > 0) {
		compressor->setLevel(compLevel);
	}

#ifndef _ICU_
	if (normalize) {
		normalize = false;
		std::cout << identifyMsg("WARNING", "UTF8")
			  << program
			  << " is not compiled with support for ICU. Assuming -N."
			  << std::endl;
	}
#endif

	if (debug & DEBUG_OTHER) {
		std::cout << identifyMsg("DEBUG", "ARGS")
			  << "\n\tpath: " << path
			  << "\n\tosisDoc: " << osisDoc
			  << "\n\tcreate: " << append
			  << "\n\tcompressType: " << compType
			  << "\n\tblockType: " << iType
			  << "\n\tcompressLevel: " << compLevel
			  << "\n\tcipherKey: " << cipherKey.c_str()
			  << "\n\tnormalize: " << normalize
			  << std::endl;
	}

	if (!append) {  // == 0 then create module
	// Try to initialize a default set of datafiles and indicies at our
	// datapath location passed to us from the user.
		if (compressor) {
			if (entrySize == 4) {
				if (zText4::createModule(path, iType, v11n)) {
					fprintf(stderr, "ERROR: %s: couldn't create module at path: %s \n", program, path);
					exit(EXIT_NO_CREATE);
				}
			}
			else {
				if (zText::createModule(path, iType, v11n)) {
					fprintf(stderr, "ERROR: %s: couldn't create module at path: %s \n", program, path);
					exit(EXIT_NO_CREATE);
				}
			}
		}
		else if (entrySize == 4) {
			if (RawText4::createModule(path, v11n)) {
				fprintf(stderr, "ERROR: %s: couldn't create module at path: %s \n", program, path);
				exit(EXIT_NO_CREATE);
			}
		}
		else {
			if (RawText::createModule(path, v11n)) {
				fprintf(stderr, "ERROR: %s: couldn't create module at path: %s \n", program, path);
				exit(EXIT_NO_CREATE);
			}
		}
	}

	// Do some initialization stuff
	if (compressor) {
		if (entrySize == 4) {
			// Create a compressed text module allowing very large entries
			// Taking defaults except for first, fourth, fifth and last argument
			module = new zText4(
				path,           // ipath
				0,              // iname
				0,              // idesc
				iType,          // iblockType
				compressor,     // icomp
				0,              // idisp
				ENC_UNKNOWN,    // enc
				DIRECTION_LTR,  // dir
				FMT_UNKNOWN,    // markup
				0,              // lang
				v11n            // versification
			);
		}
		else {
			// Create a compressed text module allowing reasonable sized entries
			// Taking defaults except for first, fourth, fifth and last argument
			module = new zText(
				path,           // ipath
				0,              // iname
				0,              // idesc
				iType,          // iblockType
				compressor,     // icomp
				0,              // idisp
				ENC_UNKNOWN,    // enc
				DIRECTION_LTR,  // dir
				FMT_UNKNOWN,    // markup
				0,              // lang
				v11n            // versification
			);
		}
	}
	else if (entrySize == 4) {
		// Create a raw text module allowing very large entries
		// Taking defaults except for first and last argument
		module = new RawText4(
				path,           // ipath
				0,              // iname
				0,              // idesc
				0,              // idisp
				ENC_UNKNOWN,    // encoding
				DIRECTION_LTR,  // dir
				FMT_UNKNOWN,    // markup
				0,              // ilang
				v11n            // versification
			);
	}
	else {
		// Create a raw text module allowing reasonable sized entries
		// Taking defaults except for first and last argument
		module = new RawText(
				path,           // ipath
				0,              // iname
				0,              // idesc
				0,              // idisp
				ENC_UNKNOWN,    // encoding
				DIRECTION_LTR,  // dir
				FMT_UNKNOWN,    // markup
				0,              // ilang
				v11n            // versification
			);
	}

	SWFilter *cipherFilter = 0;

	if (cipherKey.length()) {
		fprintf(stderr, "Adding cipher filter with phrase: %s\n", cipherKey.c_str() );
		cipherFilter = new CipherFilter(cipherKey.c_str());
		module->addRawFilter(cipherFilter);
	}

	if (!module->isWritable()) {
		fprintf(stderr, "The module is not writable. Writing text to it will not work.\nExiting.\n" );
		exit(EXIT_NO_WRITE);
	}

	// Either read from std::cin (aka stdin), when the argument is a '-'
	// or from a specified file.
	if (!strcmp(osisDoc, "-")) {
		processOSIS(std::cin);
	}
	else {
		// Let's see if we can open our input file
		std::ifstream infile(osisDoc);
		if (infile.fail()) {
			fprintf(stderr, "ERROR: %s: couldn't open input file: %s \n", program, osisDoc);
			exit(EXIT_NO_READ);
		}
		processOSIS(infile);
		infile.close();
	}

	delete module;
	if (cipherFilter)
		delete cipherFilter;
	if (outputEncoder)
		delete outputEncoder;
	if (outputDecoder)
		delete outputDecoder;

	fprintf(stderr, "SUCCESS: %s: has finished its work and will now rest\n", program);
	exit(0); // success
}

