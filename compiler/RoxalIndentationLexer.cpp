
#include "RoxalParser.h"

#include "RoxalIndentationLexer.h"

using namespace roxal;



RoxalIndentationLexer::RoxalIndentationLexer(antlr4::CharStream *input) 
    : RoxalLexer(input)
{
    indentLengths.push(0);
    opened = 0;
    wasSpaceIndentation = false;
    wasTabIndentation = false;
}

std::string RoxalIndentationLexer::TEXT_LEXER {"lexer --> "};
//std::string RoxalIndentationLexer::TEXT_INSERTED_INDENT {"inserted INDENT"};
std::string RoxalIndentationLexer::TEXT_INSERTED_INDENT {"INDENT"};

template<typename C, typename T>
bool contains(const C& container, const T& element) {
    return std::find(container.begin(), container.end(), element) != container.end();    
}


std::unique_ptr<antlr4::Token> RoxalIndentationLexer::nextToken()
{
    bool atVeryFirstCharWhichIsSpaceOrTAB = (getCharIndex() == 0) 
        && contains(std::vector<size_t>({size_t(' '), size_t('\t')}), _input->LA(1));
    std::unique_ptr<antlr4::Token> currentToken;

    while (true) {
        currentToken = RoxalLexer::nextToken(); // get a token from the inputstream
        this->insertLeadingTokens(atVeryFirstCharWhichIsSpaceOrTAB, currentToken->getType(), currentToken->getStartIndex());
        switch (currentToken->getType()) {
            case OPEN_PAREN:
            case OPEN_BRACK:
            case OPEN_BRACE:
                this->opened++;
                this->openers.push(currentToken->getType());
                this->pendingTokens.push_back(std::move(currentToken));  // insert the current open parentheses or square bracket or curly brace token
                break;
            case CLOSE_PAREN:
            case CLOSE_BRACK:
            case CLOSE_BRACE:
                this->opened--;
                if (!this->openers.empty())
                    this->openers.pop();
                this->pendingTokens.push_back(std::move(currentToken));  // insert the current close parentheses or square bracket or curly brace token
                break;
            case NEWLINE:
                if (this->opened > 0) {                             //*** https://docs.python.org/3/reference/lexical_analysis.html#implicit-line-joining
                    if (!this->openers.empty() && this->openers.top()==OPEN_BRACK) {
                        this->pendingTokens.push_back(std::move(currentToken));
                    } else {
                        continue;  // We're inside an implicit line joining section, skip the NEWLINE token
                    }
                } else {
                    switch (_input->LA(1) /* next symbol */) {    //*** https://www.antlr.org/api/Java/org/antlr/v4/runtime/IntStream.html#LA(int)
                        case '\r':
                        case '\n':
                        case '\f':
                        case '#':                                  //*** https://docs.python.org/3/reference/lexical_analysis.html#blank-lines
                            continue;  // We're on a blank line or before a comment, skip the NEWLINE token
                        case '/': // same for // style comments
                            if (_input->LA(2)=='/')
                                continue;
                        default:
                            this->pendingTokens.push_back(std::move(currentToken)); // insert the current NEWLINE token
                            this->insertIndentDedentTokens();       //*** https://docs.python.org/3/reference/lexical_analysis.html#indentation
                    }
                }
                break;
            case EOF:
                if ( !this->indentLengths.empty() ) {
                    this->insertTrailingTokens(); // indentLengths stack wil be empty
                    this->checkSpaceAndTabIndentation();
                    this->pendingTokens.push_back(std::move(currentToken)); // insert the current EOF token
                }
                break;
            default:
                this->pendingTokens.push_back(std::move(currentToken)); // insert the current token
        }
        break; // exit from the loop
    }
    //this->lastAppendedTokenType = this->pendingTokens.peekFirst().getType(); // save the token type before removing from the deque for the trailing tokens inserting later
    antlr4::Token* lastAppendedToken = this->pendingTokens.empty() ? nullptr : this->pendingTokens.front().release();
    this->lastAppendedTokenType = lastAppendedToken ? lastAppendedToken->getType() : EOF; // save the token type before removing from the deque for the trailing tokens inserting later
    //return this->pendingTokens.pollFirst(); // append a token to the token stream until the first returning EOF
    this->pendingTokens.pop_front();
    return std::unique_ptr<antlr4::Token>(lastAppendedToken); // append a token to the token stream until the first returning EOF
}


void RoxalIndentationLexer::insertLeadingTokens(bool atVeryFirstCharWhichIsSpaceOrTAB, int type, int startIndex) 
{
    if (atVeryFirstCharWhichIsSpaceOrTAB &&   // We're at the first line of the input starting with a space or TAB
        //!List.of(NEWLINE, EOF).contains(type) // and within that the first token that is visible (comments were skiped and OPEN_PAREN, OPEN_BRACK OPEN_BRACE cannot be the first token)
        !contains(std::vector<size_t>({NEWLINE, EOF}),type) // and within that the first token that is visible (comments were skiped and OPEN_PAREN, OPEN_BRACK OPEN_BRACE cannot be the first token)
        ) {                                   // We need to insert a NEWLINE and an INDENT token before the first token to raise an 'unexpected indent' error by the parser later
        //this->insertToken(0, startIndex - 1, "<inserted leading NEWLINE>" + std::string(startIndex,' '), NEWLINE, 1, 0);
        this->insertToken(0, startIndex - 1, "NEWLINE", NEWLINE, 1, 0);
        //this->insertToken(startIndex, startIndex - 1, "<" + TEXT_INSERTED_INDENT + ", " + this->getIndentationDescription(startIndex) + ">", RoxalParser::INDENT, 1, startIndex);
        this->insertToken(startIndex, startIndex - 1, "INDENT", RoxalParser::INDENT, 1, startIndex);
        this->indentLengths.push(startIndex);
    }
}


void RoxalIndentationLexer::insertIndentDedentTokens() 
{
    const int currentIndentLength = this->getIndentationLength(getText());
    int previousIndentLength = this->indentLengths.top();

    if (currentIndentLength > previousIndentLength) { // insert an INDENT token
        //this->insertToken("<" + TEXT_INSERTED_INDENT + ", " + this->getIndentationDescription(currentIndentLength) + ">", RoxalParser::INDENT);
        this->insertToken("INDENT", RoxalParser::INDENT);
        this->indentLengths.push(currentIndentLength);
    } else if (currentIndentLength < previousIndentLength) {
        do {   // More than 1 DEDENT token may be inserted
            this->indentLengths.pop();
            previousIndentLength = this->indentLengths.top();
            if (currentIndentLength <= previousIndentLength) {
                //this->insertToken("<inserted DEDENT, " + this->getIndentationDescription(previousIndentLength) + ">", RoxalParser::DEDENT);
                this->insertToken("DEDENT", RoxalParser::DEDENT);
            } else {
                //this->insertToken("<inserted (I N C O N S I S T E N T!) DEDENT, " + this->getIndentationDescription(currentIndentLength) + ">", RoxalParser::DEDENT);
                this->insertToken("DEDENT", RoxalParser::DEDENT);
                this->errors.push_back(TEXT_LEXER + "line " + std::to_string(getLine()) + ":" + std::to_string(getCharPositionInLine()) + "\t IndentationError: unindent does not match any outer indentation level");
            }
        } while (currentIndentLength < previousIndentLength);
    }
}

void RoxalIndentationLexer::insertTrailingTokens() 
{
    if ( !contains(std::vector<size_t>({NEWLINE, RoxalParser::DEDENT}), this->lastAppendedTokenType) ) { // If the last token was not NEWLINE or DEDENT then
        //this->insertToken("<inserted trailing NEWLINE>", NEWLINE); // insert an extra trailing NEWLINE token that serves as the end of the statement
        this->insertToken("NEWLINE", NEWLINE); // insert an extra trailing NEWLINE token that serves as the end of the statement
    }

    while ( this->indentLengths.size() > 1 ) { // Now insert as much trailing DEDENT tokens as needed
        //this->insertToken("<inserted trailing DEDENT, " + this->getIndentationDescription(this->indentLengths.top()) + ">", RoxalParser::DEDENT);
        this->insertToken("DEDENT", RoxalParser::DEDENT);
        this->indentLengths.pop();
    }
    this->indentLengths.pop(); // Remove the default 0 indentation length
}

std::string RoxalIndentationLexer::getIndentationDescription(int lengthOfIndent)
{
    return "length=" + std::to_string(lengthOfIndent) + ", level=" + std::to_string(this->indentLengths.size());
}

void RoxalIndentationLexer::insertToken(std::string text, int type) 
{
    const int startIndex = tokenStartCharIndex + getText().size(); //*** https://www.antlr.org/api/Java/org/antlr/v4/runtime/Lexer.html#_tokenStartCharIndex
    this->insertToken(startIndex, startIndex - 1, text, type, getLine(), getCharPositionInLine());
}

void RoxalIndentationLexer::insertToken(int startIndex, int stopIndex, std::string text, int type, int line, int charPositionInLine) 
{
    std::pair<antlr4::TokenSource*, antlr4::CharStream*> tokenFactorySourcePair { this, _input };
    auto token = new antlr4::CommonToken(tokenFactorySourcePair, type, DEFAULT_TOKEN_CHANNEL, startIndex, stopIndex); //*** https://www.antlr.org/api/Java/org/antlr/v4/runtime/CommonToken.html
    token->setText(text);
    token->setLine(line);
    token->setCharPositionInLine(charPositionInLine);
    this->pendingTokens.push_back(std::unique_ptr<antlr4::Token>(dynamic_cast<antlr4::Token*>(token)));
}

// Calculates the indentation of the provided spaces, taking the
// following rules into account:
//
// "Tabs are replaced (from left to right) by one to eight spaces
//  such that the total number of characters up to and including
//  the replacement is a multiple of eight [...]"
//
//  -- https://docs.python.org/3.1/reference/lexical_analysis.html#indentation
int RoxalIndentationLexer::getIndentationLength(std::string textOfMatchedNEWLINE) 
{
    int count = 0;

    for (char const &ch : textOfMatchedNEWLINE) {
        switch (ch) {
            case ' ': // A normal space char
                this->wasSpaceIndentation = true;
                count++;
                break;
            case '\t':
                this->wasTabIndentation = true;
                count += 8 - (count % 8);
                break;
        }
    }
    return count;
}

void RoxalIndentationLexer::checkSpaceAndTabIndentation() 
{
    if (this->wasSpaceIndentation && this->wasTabIndentation) {
        this->warnings.push_back("Mixture of space and tab were used for indentation.");
    }
}

std::list<std::string> RoxalIndentationLexer::getWarnings() 
{ // can be called from a grammar embedded action also
    return this->warnings;
}

std::list<std::string> RoxalIndentationLexer::getErrorMessages() 
{ // can be called from a grammar embedded action also
    return this->errors;
}
