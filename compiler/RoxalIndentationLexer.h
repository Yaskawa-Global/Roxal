/*
 * Inform2 Indent/Dedent handler 
 *
 * Adapted from Java Python3 version from: 
 *   https://github.com/antlr/grammars-v4/tree/master/python/python3-without-actions
 *   by : Robert Einhorn, robert.einhorn.hu@gmail.com
 */

#pragma once

#include <stack>
#include <deque>
#include <list>

#include "RoxalLexer.h"

namespace roxal { 


class RoxalIndentationLexer : public RoxalLexer 
{ 
public:
    RoxalIndentationLexer(antlr4::CharStream *input); 

    std::list<std::string> getWarnings();
    std::list<std::string> getErrorMessages();
    
protected:
    std::stack<int> indentLengths;
    // A queue where extra tokens are pushed on
    std::deque<std::unique_ptr<antlr4::Token>> pendingTokens;
    // An integer that stores the type of the last appended token to the token stream
    int lastAppendedTokenType;

    // The amount of opened braces, brackets and parenthesis
    int opened;

    // Was there space char in the indentations?
    bool wasSpaceIndentation;
    // Was there TAB char in the indentations?
    bool wasTabIndentation;

    // A string list that stores the lexer warnings
    std::list<std::string> warnings;
    // A string list that stores the lexer error messages
    std::list<std::string> errors;

    // Patterns for the custom error listener to recognize error messages
    static std::string TEXT_LEXER;
    static std::string TEXT_INSERTED_INDENT;

    virtual std::unique_ptr<antlr4::Token> nextToken() override;

private:
    void insertLeadingTokens(bool atVeryFirstCharWhichIsSpaceOrTAB, int type, int startIndex);

    void insertIndentDedentTokens(); 

    void insertTrailingTokens();

    std::string getIndentationDescription(int lengthOfIndent);

    void insertToken(std::string text, int type);

    void insertToken(int startIndex, int stopIndex, std::string text, int type, int line, int charPositionInLine);

    // Calculates the indentation of the provided spaces, taking the
    // following rules into account:
    //
    // "Tabs are replaced (from left to right) by one to eight spaces
    //  such that the total number of characters up to and including
    //  the replacement is a multiple of eight [...]"
    //
    //  -- https://docs.python.org/3.1/reference/lexical_analysis.html#indentation
    int getIndentationLength(std::string textOfMatchedNEWLINE);
    
    void checkSpaceAndTabIndentation();
};

} // namespace roxal
