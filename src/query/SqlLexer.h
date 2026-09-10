#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace ds {

enum class TokenType {
  Keyword,
  Identifier,
  QuotedIdentifier,
  Number,
  String,
  Operator,
  Punctuation,
  Comment,
  Parameter,   // ?, :name, @var
  Whitespace,
  Unknown,
};

struct Token {
  TokenType type = TokenType::Unknown;
  std::string text;    // raw source text, quotes included
  size_t start = 0;    // byte offset into the source
  size_t length = 0;

  // Identifier text with surrounding quotes/backticks/brackets stripped.
  std::string unquoted() const;
  bool isKeyword(const char* kw) const;
};

// Tokenizes SQL for both dialects. Never throws: unterminated literals come
// back as a token running to end of input, which is the common case while the
// user is still typing.
std::vector<Token> tokenize(const std::string& sql, bool keepTrivia = false);

bool isSqlKeyword(const std::string& word);
bool isSqlFunction(const std::string& word);

// The keyword and function lists, for completion sources.
const std::vector<std::string>& sqlKeywords();
const std::vector<std::string>& sqlFunctions();

}  // namespace ds
