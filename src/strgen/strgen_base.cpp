/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file strgen_base.cpp Tool to create computer readable (stand-alone) translation files. */

#include "../stdafx.h"
#include "../core/endian_func.hpp"
#include "../core/math_func.hpp"
#include "../error_func.h"
#include "../string_func.h"
#include "../core/string_builder.hpp"
#include "../table/control_codes.h"

#include "strgen.h"

#include "../table/strgen_tables.h"

#include "../safeguards.h"

StrgenState _strgen;
static bool _translated;              ///< Whether the current language is not the master language
static std::string_view _cur_ident;
static ParsedCommandStruct _cur_pcs;
static size_t _cur_argidx;

struct ParsedCommandString {
	const CmdStruct *cmd = nullptr;
	std::string param;
	std::optional<size_t> argno;
	std::optional<uint8_t> casei;
};
static ParsedCommandString ParseCommandString(StringConsumer &consumer);
static size_t TranslateArgumentIdx(size_t arg, size_t offset = 0);

/**
 * Create a new case.
 * @param caseidx The index of the case.
 * @param string  The translation of the case.
 */
Case::Case(uint8_t caseidx, std::string_view string) :
		caseidx(caseidx), string(string)
{
}

/**
 * Create a new string.
 * @param name    The name of the string.
 * @param english The english "translation" of the string.
 * @param index   The index in the string table.
 * @param line    The line this string was found on.
 */
LangString::LangString(std::string_view name, std::string_view english, size_t index, size_t line) :
		name(name), english(english), index(index), line(line)
{
}

/** Free all data related to the translation. */
void LangString::FreeTranslation()
{
	this->translated.clear();
	this->translated_cases.clear();
}

/**
 * Create a new string data container.
 * @param tabs The maximum number of strings.
 */
StringData::StringData(size_t tabs) : tabs(tabs), max_strings(tabs * TAB_SIZE)
{
	this->strings.resize(max_strings);
	this->next_string_id = 0;
}

/** Free all data related to the translation. */
void StringData::FreeTranslation()
{
	for (size_t i = 0; i < this->max_strings; i++) {
		LangString *ls = this->strings[i].get();
		if (ls != nullptr) ls->FreeTranslation();
	}
}

/**
 * Add a newly created LangString.
 * @param s  The name of the string.
 * @param ls The string to add.
 */
void StringData::Add(std::shared_ptr<LangString> ls)
{
	this->name_to_string[ls->name] = ls;
	this->strings[ls->index] = std::move(ls);
}

/**
 * Find a LangString based on the string name.
 * @param s The string name to search on.
 * @return The LangString or nullptr if it is not known.
 */
LangString *StringData::Find(std::string_view s)
{
	auto it = this->name_to_string.find(s);
	if (it == this->name_to_string.end()) return nullptr;

	return it->second.get();
}

/**
 * Create a compound hash.
 * @param hash The hash to add the string hash to.
 * @param s    The string hash.
 * @return The new hash.
 */
static uint32_t VersionHashStr(uint32_t hash, std::string_view s)
{
	for (auto c : s) {
		hash = std::rotl(hash, 3) ^ c;
		hash = (hash & 1 ? hash >> 1 ^ 0xDEADBEEF : hash >> 1);
	}
	return hash;
}

/**
 * Make a hash of the file to get a unique "version number"
 * @return The version number.
 */
uint32_t StringData::Version() const
{
	uint32_t hash = 0;

	for (size_t i = 0; i < this->max_strings; i++) {
		const LangString *ls = this->strings[i].get();

		if (ls != nullptr) {
			hash ^= i * 0x717239;
			hash = (hash & 1 ? hash >> 1 ^ 0xDEADBEEF : hash >> 1);
			hash = VersionHashStr(hash, ls->name);

			StringConsumer consumer(ls->english);
			ParsedCommandString cs;
			while ((cs = ParseCommandString(consumer)).cmd != nullptr) {
				if (cs.cmd->flags.Test(CmdFlag::DontCount)) continue;

				hash ^= (cs.cmd - _cmd_structs) * 0x1234567;
				hash = (hash & 1 ? hash >> 1 ^ 0xF00BAA4 : hash >> 1);
			}
		}
	}

	return hash;
}

/**
 * Count the number of tab elements that are in use.
 * @param tab The tab to count the elements of.
 */
size_t StringData::CountInUse(size_t tab) const
{
	size_t count = TAB_SIZE;
	while (count > 0 && this->strings[(tab * TAB_SIZE) + count - 1] == nullptr) --count;
	return count;
}

void EmitSingleChar(StringBuilder &builder, std::string_view param, char32_t value)
{
	if (!param.empty()) StrgenWarning("Ignoring trailing letters in command");
	builder.PutUtf8(value);
}

/* The plural specifier looks like
 * {NUM} {PLURAL <ARG#> passenger passengers} then it picks either passenger/passengers depending on the count in NUM */
static std::pair<std::optional<size_t>, std::optional<size_t>> ParseRelNum(StringConsumer &consumer)
{
	consumer.SkipUntilCharNotIn(StringConsumer::WHITESPACE_NO_NEWLINE);
	std::optional<size_t> v = consumer.TryReadIntegerBase<size_t>(10);
	std::optional<size_t> offset;
	if (v.has_value() && consumer.ReadCharIf(':')) {
		/* Take the Nth within */
		offset = consumer.TryReadIntegerBase<size_t>(10);
		if (!offset.has_value()) StrgenFatal("Expected number for substring parameter");
	}
	return {v, offset};
}

/* Parse out the next word, or nullptr */
std::optional<std::string_view> ParseWord(StringConsumer &consumer)
{
	consumer.SkipUntilCharNotIn(StringConsumer::WHITESPACE_NO_NEWLINE);
	if (!consumer.AnyBytesLeft()) return {};

	if (consumer.ReadCharIf('"')) {
		/* parse until next " or NUL */
		auto result = consumer.ReadUntilChar('"', StringConsumer::KEEP_SEPARATOR);
		if (!consumer.ReadCharIf('"')) StrgenFatal("Unterminated quotes");
		return result;
	} else {
		/* proceed until whitespace or NUL */
		return consumer.ReadUntilCharIn(StringConsumer::WHITESPACE_NO_NEWLINE);
	}
}

/* This is encoded like
 *  CommandByte <ARG#> <NUM> {Length of each string} {each string} */
static void EmitWordList(StringBuilder &builder, const std::vector<std::string> &words)
{
	builder.PutUint8(static_cast<uint8_t>(words.size()));
	for (size_t i = 0; i < words.size(); i++) {
		size_t len = words[i].size();
		if (len > UINT8_MAX) StrgenFatal("WordList {}/{} string '{}' too long, max bytes {}", i + 1, words.size(), words[i], UINT8_MAX);
		builder.PutUint8(static_cast<uint8_t>(len));
	}
	for (size_t i = 0; i < words.size(); i++) {
		builder.Put(words[i]);
	}
}

void EmitPlural(StringBuilder &builder, std::string_view param, char32_t)
{
	StringConsumer consumer(param);

	/* Parse out the number, if one exists. Otherwise default to prev arg. */
	auto [argidx, offset] = ParseRelNum(consumer);
	if (!argidx.has_value()) {
		if (_cur_argidx == 0) StrgenFatal("Plural choice needs positional reference");
		argidx = _cur_argidx - 1;
	}

	const CmdStruct *cmd = _cur_pcs.consuming_commands[*argidx];
	if (!offset.has_value()) {
		/* Use default offset */
		if (cmd == nullptr || !cmd->default_plural_offset.has_value()) {
			StrgenFatal("Command '{}' has no (default) plural position", cmd == nullptr ? "<empty>" : cmd->cmd);
		}
		offset = cmd->default_plural_offset;
	}

	/* Parse each string */
	std::vector<std::string> words;
	for (;;) {
		auto word = ParseWord(consumer);
		if (!word.has_value()) break;
		words.emplace_back(*word);
	}

	if (words.empty()) {
		StrgenFatal("{}: No plural words", _cur_ident);
	}

	size_t expected = _plural_forms[_strgen.lang.plural_form].plural_count;
	if (expected != words.size()) {
		if (_translated) {
			StrgenFatal("{}: Invalid number of plural forms. Expecting {}, found {}.", _cur_ident,
				expected, words.size());
		} else {
			if (_strgen.show_warnings) StrgenWarning("'{}' is untranslated. Tweaking english string to allow compilation for plural forms", _cur_ident);
			if (words.size() > expected) {
				words.resize(expected);
			} else {
				while (words.size() < expected) {
					words.push_back(words.back());
				}
			}
		}
	}

	builder.PutUtf8(SCC_PLURAL_LIST);
	builder.PutUint8(_strgen.lang.plural_form);
	builder.PutUint8(static_cast<uint8_t>(TranslateArgumentIdx(*argidx, *offset)));
	EmitWordList(builder, words);
}

void EmitGender(StringBuilder &builder, std::string_view param, char32_t)
{
	StringConsumer consumer(param);
	if (consumer.ReadCharIf('=')) {
		/* This is a {G=DER} command */
		auto gender = consumer.Read(StringConsumer::npos);
		auto nw = _strgen.lang.GetGenderIndex(gender);
		if (nw >= MAX_NUM_GENDERS) StrgenFatal("G argument '{}' invalid", gender);

		/* now nw contains the gender index */
		builder.PutUtf8(SCC_GENDER_INDEX);
		builder.PutUint8(nw);
	} else {
		/* This is a {G 0 foo bar two} command.
		 * If no relative number exists, default to +0 */
		auto [argidx, offset] = ParseRelNum(consumer);
		if (!argidx.has_value()) argidx = _cur_argidx;
		if (!offset.has_value()) offset = 0;

		const CmdStruct *cmd = _cur_pcs.consuming_commands[*argidx];
		if (cmd == nullptr || !cmd->flags.Test(CmdFlag::Gender)) {
			StrgenFatal("Command '{}' can't have a gender", cmd == nullptr ? "<empty>" : cmd->cmd);
		}

		std::vector<std::string> words;
		for (;;) {
			auto word = ParseWord(consumer);
			if (!word.has_value()) break;
			words.emplace_back(*word);
		}
		if (words.size() != _strgen.lang.num_genders) StrgenFatal("Bad # of arguments for gender command");

		assert(IsInsideBS(cmd->value, SCC_CONTROL_START, UINT8_MAX));
		builder.PutUtf8(SCC_GENDER_LIST);
		builder.PutUint8(static_cast<uint8_t>(TranslateArgumentIdx(*argidx, *offset)));
		EmitWordList(builder, words);
	}
}

static const CmdStruct *FindCmd(std::string_view s)
{
	for (const auto &cs : _cmd_structs) {
		if (cs.cmd == s) return &cs;
	}
	return nullptr;
}

static uint8_t ResolveCaseName(std::string_view str)
{
	uint8_t case_idx = _strgen.lang.GetCaseIndex(str);
	if (case_idx >= MAX_NUM_CASES) StrgenFatal("Invalid case-name '{}'", str);
	return case_idx + 1;
}

/* returns cmd == nullptr on eof */
static ParsedCommandString ParseCommandString(StringConsumer &consumer)
{
	ParsedCommandString result;

	/* Scan to the next command, exit if there's no next command. */
	consumer.SkipUntilChar('{', StringConsumer::KEEP_SEPARATOR);
	if (!consumer.ReadCharIf('{')) return {};

	if (auto argno = consumer.TryReadIntegerBase<uint32_t>(10); argno.has_value()) {
		result.argno = argno;
		if (!consumer.ReadCharIf(':')) StrgenFatal("missing arg #");
	}

	/* parse command name */
	auto command = consumer.ReadUntilCharIn("} =.");
	result.cmd = FindCmd(command);
	if (result.cmd == nullptr) {
		StrgenError("Undefined command '{}'", command);
		return {};
	}

	/* parse case */
	if (consumer.ReadCharIf('.')) {
		if (!result.cmd->flags.Test(CmdFlag::Case)) {
			StrgenFatal("Command '{}' can't have a case", result.cmd->cmd);
		}

		auto casep = consumer.ReadUntilCharIn("} ");
		result.casei = ResolveCaseName(casep);
	}

	/* parse params */
	result.param = consumer.ReadUntilChar('}', StringConsumer::KEEP_SEPARATOR);

	if (!consumer.ReadCharIf('}')) {
		StrgenError("Missing }} from command '{}'", result.cmd->cmd);
		return {};
	}

	return result;
}

/**
 * Prepare reading.
 * @param data        The data to fill during reading.
 * @param file        The file we are reading.
 * @param master      Are we reading the master file?
 * @param translation Are we reading a translation?
 */
StringReader::StringReader(StringData &data, const std::string &file, bool master, bool translation) :
		data(data), file(file), master(master), translation(translation)
{
}

ParsedCommandStruct ExtractCommandString(std::string_view s, bool)
{
	ParsedCommandStruct p;
	StringConsumer consumer(s);

	size_t argidx = 0;
	for (;;) {
		/* read until next command from a. */
		auto cs = ParseCommandString(consumer);

		if (cs.cmd == nullptr) break;

		/* Sanity checking */
		if (cs.argno.has_value() && cs.cmd->consumes == 0) StrgenFatal("Non consumer param can't have a paramindex");

		if (cs.cmd->consumes > 0) {
			if (cs.argno.has_value()) argidx = *cs.argno;
			if (argidx >= p.consuming_commands.max_size()) StrgenFatal("invalid param idx {}", argidx);
			if (p.consuming_commands[argidx] != nullptr && p.consuming_commands[argidx] != cs.cmd) StrgenFatal("duplicate param idx {}", argidx);

			p.consuming_commands[argidx++] = cs.cmd;
		} else if (!cs.cmd->flags.Test(CmdFlag::DontCount)) { // Ignore some of them
			p.non_consuming_commands.emplace_back(cs.cmd, std::move(cs.param));
		}
	}

	return p;
}

const CmdStruct *TranslateCmdForCompare(const CmdStruct *a)
{
	if (a == nullptr) return nullptr;

	if (a->cmd == "STRING1" ||
			a->cmd == "STRING2" ||
			a->cmd == "STRING3" ||
			a->cmd == "STRING4" ||
			a->cmd == "STRING5" ||
			a->cmd == "STRING6" ||
			a->cmd == "STRING7" ||
			a->cmd == "RAW_STRING") {
		return FindCmd("STRING");
	}

	return a;
}

static bool CheckCommandsMatch(std::string_view a, std::string_view b, std::string_view name)
{
	/* If we're not translating, i.e. we're compiling the base language,
	 * it is pointless to do all these checks as it'll always be correct.
	 * After all, all checks are based on the base language.
	 */
	if (!_strgen.translation) return true;

	bool result = true;

	ParsedCommandStruct templ = ExtractCommandString(b, true);
	ParsedCommandStruct lang = ExtractCommandString(a, true);

	/* For each string in templ, see if we find it in lang */
	if (templ.non_consuming_commands.max_size() != lang.non_consuming_commands.max_size()) {
		StrgenWarning("{}: template string and language string have a different # of commands", name);
		result = false;
	}

	for (auto &templ_nc : templ.non_consuming_commands) {
		/* see if we find it in lang, and zero it out */
		bool found = false;
		for (auto &lang_nc : lang.non_consuming_commands) {
			if (templ_nc.cmd == lang_nc.cmd && templ_nc.param == lang_nc.param) {
				/* it was found in both. zero it out from lang so we don't find it again */
				lang_nc.cmd = nullptr;
				found = true;
				break;
			}
		}

		if (!found) {
			StrgenWarning("{}: command '{}' exists in template file but not in language file", name, templ_nc.cmd->cmd);
			result = false;
		}
	}

	/* if we reach here, all non consumer commands match up.
	 * Check if the non consumer commands match up also. */
	for (size_t i = 0; i < templ.consuming_commands.max_size(); i++) {
		if (TranslateCmdForCompare(templ.consuming_commands[i]) != lang.consuming_commands[i]) {
			StrgenWarning("{}: Param idx #{} '{}' doesn't match with template command '{}'", name, i,
				lang.consuming_commands[i]  == nullptr ? "<empty>" : TranslateCmdForCompare(lang.consuming_commands[i])->cmd,
				templ.consuming_commands[i] == nullptr ? "<empty>" : templ.consuming_commands[i]->cmd);
			result = false;
		}
	}

	return result;
}

void StringReader::HandleString(std::string_view src)
{
	/* Ignore blank lines */
	if (src.empty()) return;

	StringConsumer consumer(src);
	if (consumer.ReadCharIf('#')) {
		if (consumer.ReadCharIf('#') && !consumer.ReadCharIf('#')) this->HandlePragma(consumer.Read(StringConsumer::npos), _strgen.lang);
		return; // ignore comments
	}

	/* Read string name */
	std::string_view str_name = StrTrimView(consumer.ReadUntilChar(':', StringConsumer::KEEP_SEPARATOR), StringConsumer::WHITESPACE_NO_NEWLINE);
	if (!consumer.ReadCharIf(':')) {
		StrgenError("Line has no ':' delimiter");
		return;
	}

	/* Read string case */
	std::optional<std::string_view> casep;
	if (auto index = str_name.find("."); index != std::string_view::npos) {
		casep = str_name.substr(index + 1);
		str_name = str_name.substr(0, index);
	}

	/* Read string data */
	std::string_view value = consumer.Read(StringConsumer::npos);

	/* Check string is valid UTF-8 */
	for (StringConsumer validation_consumer(value); validation_consumer.AnyBytesLeft(); ) {
		auto c = validation_consumer.TryReadUtf8();
		if (!c.has_value()) StrgenFatal("Invalid UTF-8 sequence in '{}'", value);
		if (*c <= 0x001F || // ASCII control character range
				*c == 0x200B || // Zero width space
				(*c >= 0xE000 && *c <= 0xF8FF) || // Private range
				(*c >= 0xFFF0 && *c <= 0xFFFF)) { // Specials range
			StrgenFatal("Unwanted UTF-8 character U+{:04X} in sequence '{}'", static_cast<uint32_t>(*c), value);
		}
	}

	/* Check if this string already exists.. */
	LangString *ent = this->data.Find(str_name);

	if (this->master) {
		if (casep.has_value()) {
			StrgenError("Cases in the base translation are not supported.");
			return;
		}

		if (ent != nullptr) {
			StrgenError("String name '{}' is used multiple times", str_name);
			return;
		}

		if (this->data.strings[this->data.next_string_id] != nullptr) {
			StrgenError("String ID 0x{:X} for '{}' already in use by '{}'", this->data.next_string_id, str_name, this->data.strings[this->data.next_string_id]->name);
			return;
		}

		/* Allocate a new LangString */
		this->data.Add(std::make_unique<LangString>(str_name, value, this->data.next_string_id++, _strgen.cur_line));
	} else {
		if (ent == nullptr) {
			StrgenWarning("String name '{}' does not exist in master file", str_name);
			return;
		}

		if (!ent->translated.empty() && !casep.has_value()) {
			StrgenError("String name '{}' is used multiple times", str_name);
			return;
		}

		/* make sure that the commands match */
		if (!CheckCommandsMatch(value, ent->english, str_name)) return;

		if (casep.has_value()) {
			ent->translated_cases.emplace_back(ResolveCaseName(*casep), value);
		} else {
			ent->translated = value;
			/* If the string was translated, use the line from the
			 * translated language so errors in the translated file
			 * are properly referenced to. */
			ent->line = _strgen.cur_line;
		}
	}
}

void StringReader::HandlePragma(std::string_view str, LanguagePackHeader &lang)
{
	StringConsumer consumer(str);
	auto name = consumer.ReadUntilChar(' ', StringConsumer::SKIP_ALL_SEPARATORS);
	if (name == "plural") {
		lang.plural_form = consumer.ReadIntegerBase<uint32_t>(10);
		if (lang.plural_form >= lengthof(_plural_forms)) {
			StrgenFatal("Invalid pluralform {}", lang.plural_form);
		}
	} else {
		StrgenFatal("unknown pragma '{}'", name);
	}
}

void StringReader::ParseFile()
{
	_strgen.warnings = _strgen.errors = 0;

	_strgen.translation = this->translation;
	_strgen.file = this->file;

	/* For each new file we parse, reset the genders, and language codes. */
	_strgen.lang = {};

	_strgen.cur_line = 1;
	while (this->data.next_string_id < this->data.max_strings) {
		std::optional<std::string> line = this->ReadLine();
		if (!line.has_value()) return;

		this->HandleString(StrTrimView(line.value(), StringConsumer::WHITESPACE_OR_NEWLINE));
		_strgen.cur_line++;
	}

	if (this->data.next_string_id == this->data.max_strings) {
		StrgenError("Too many strings, maximum allowed is {}", this->data.max_strings);
	}
}

/**
 * Write the header information.
 * @param data The data about the string.
 */
void HeaderWriter::WriteHeader(const StringData &data)
{
	size_t last = 0;
	for (size_t i = 0; i < data.max_strings; i++) {
		if (data.strings[i] != nullptr) {
			this->WriteStringID(data.strings[i]->name, i);
			last = i;
		}
	}

	this->WriteStringID("STR_LAST_STRINGID", last);
}

static size_t TranslateArgumentIdx(size_t argidx, size_t offset)
{
	if (argidx >= _cur_pcs.consuming_commands.max_size()) {
		StrgenFatal("invalid argidx {}", argidx);
	}
	const CmdStruct *cs = _cur_pcs.consuming_commands[argidx];
	if (cs != nullptr && cs->consumes <= offset) {
		StrgenFatal("invalid argidx offset {}:{}", argidx, offset);
	}

	if (_cur_pcs.consuming_commands[argidx] == nullptr) {
		StrgenFatal("no command for this argidx {}", argidx);
	}

	size_t sum = 0;
	for (size_t i = 0; i < argidx; i++) {
		cs = _cur_pcs.consuming_commands[i];

		if (cs == nullptr && sum > i) continue;

		sum += (cs != nullptr) ? cs->consumes : 1;
	}

	return sum + offset;
}

static void PutArgidxCommand(StringBuilder &builder)
{
	builder.PutUtf8(SCC_ARG_INDEX);
	builder.PutUint8(static_cast<uint8_t>(TranslateArgumentIdx(_cur_argidx)));
}

static std::string PutCommandString(std::string_view str)
{
	std::string result;
	StringBuilder builder(result);
	StringConsumer consumer(str);
	_cur_argidx = 0;

	for (;;) {
		/* Process characters as they are until we encounter a { */
		builder.Put(consumer.ReadUntilChar('{', StringConsumer::KEEP_SEPARATOR));
		if (!consumer.AnyBytesLeft()) break;

		auto cs = ParseCommandString(consumer);
		auto *cmd = cs.cmd;
		if (cmd == nullptr) break;

		if (cs.casei.has_value()) {
			builder.PutUtf8(SCC_SET_CASE); // {SET_CASE}
			builder.PutUint8(*cs.casei);
		}

		/* For params that consume values, we need to handle the argindex properly */
		if (cmd->consumes > 0) {
			/* Check if we need to output a move-param command */
			if (cs.argno.has_value() && *cs.argno != _cur_argidx) {
				_cur_argidx = *cs.argno;
				PutArgidxCommand(builder);
			}

			/* Output the one from the master string... it's always accurate. */
			cmd = _cur_pcs.consuming_commands[_cur_argidx++];
			if (cmd == nullptr) {
				StrgenFatal("{}: No argument exists at position {}", _cur_ident, _cur_argidx - 1);
			}
		}

		cmd->proc(builder, cs.param, cmd->value);
	}
	return result;
}

/**
 * Write the length as a simple gamma.
 * @param length The number to write.
 */
void LanguageWriter::WriteLength(size_t length)
{
	char buffer[2];
	size_t offs = 0;
	if (length >= 0x4000) {
		StrgenFatal("string too long");
	}

	if (length >= 0xC0) {
		buffer[offs++] = static_cast<char>(static_cast<uint8_t>((length >> 8) | 0xC0));
	}
	buffer[offs++] = static_cast<char>(static_cast<uint8_t>(length & 0xFF));
	this->Write({buffer, offs});
}

/**
 * Actually write the language.
 * @param data The data about the string.
 */
void LanguageWriter::WriteLang(const StringData &data)
{
	std::vector<size_t> in_use;
	for (size_t tab = 0; tab < data.tabs; tab++) {
		size_t n = data.CountInUse(tab);

		in_use.push_back(n);
		_strgen.lang.offsets[tab] = TO_LE16(static_cast<uint16_t>(n));

		for (size_t j = 0; j != in_use[tab]; j++) {
			const LangString *ls = data.strings[(tab * TAB_SIZE) + j].get();
			if (ls != nullptr && ls->translated.empty()) _strgen.lang.missing++;
		}
	}

	_strgen.lang.ident = TO_LE32(LanguagePackHeader::IDENT);
	_strgen.lang.version = TO_LE32(data.Version());
	_strgen.lang.missing = TO_LE16(_strgen.lang.missing);
	_strgen.lang.winlangid = TO_LE16(_strgen.lang.winlangid);

	this->WriteHeader(&_strgen.lang);

	for (size_t tab = 0; tab < data.tabs; tab++) {
		for (size_t j = 0; j != in_use[tab]; j++) {
			const LangString *ls = data.strings[(tab * TAB_SIZE) + j].get();

			/* For undefined strings, just set that it's an empty string */
			if (ls == nullptr) {
				this->WriteLength(0);
				continue;
			}

			std::string output;
			StringBuilder builder(output);
			_cur_ident = ls->name;
			_strgen.cur_line = ls->line;

			/* Produce a message if a string doesn't have a translation. */
			if (ls->translated.empty()) {
				if (_strgen.show_warnings) {
					StrgenWarning("'{}' is untranslated", ls->name);
				}
				if (_strgen.annotate_todos) {
					builder.Put("<TODO> ");
				}
			}

			/* Extract the strings and stuff from the english command string */
			_cur_pcs = ExtractCommandString(ls->english, false);

			_translated = !ls->translated_cases.empty() || !ls->translated.empty();
			const std::string &cmdp = _translated ? ls->translated : ls->english;

			if (!ls->translated_cases.empty()) {
				/* Need to output a case-switch.
				 * It has this format
				 * <0x9E> <NUM CASES> <CASE1> <LEN1> <STRING1> <CASE2> <LEN2> <STRING2> <CASE3> <LEN3> <STRING3> <LENDEFAULT> <STRINGDEFAULT>
				 * Each LEN is printed using 2 bytes in little endian order. */
				builder.PutUtf8(SCC_SWITCH_CASE);
				builder.PutUint8(static_cast<uint8_t>(ls->translated_cases.size()));

				/* Write each case */
				for (const Case &c : ls->translated_cases) {
					auto case_str = PutCommandString(c.string);
					builder.PutUint8(c.caseidx);
					builder.PutUint16LE(static_cast<uint16_t>(case_str.size()));
					builder.Put(case_str);
				}
			}

			std::string def_str;
			if (!cmdp.empty()) def_str = PutCommandString(cmdp);
			if (!ls->translated_cases.empty()) {
				builder.PutUint16LE(static_cast<uint16_t>(def_str.size()));
			}
			builder.Put(def_str);

			this->WriteLength(output.size());
			this->Write(output);
		}
	}
}
