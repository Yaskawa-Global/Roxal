
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
    lambdaState = LambdaState::NONE;
    funcParenOpenedLevel = 0;
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

        // Lambda state tracking: detect FUNC/PROC tokens
        if (currentToken->getType() == FUNC || currentToken->getType() == PROC) {
            lambdaState = LambdaState::SAW_FUNC;
        }

        switch (currentToken->getType()) {
            case OPEN_PAREN: {
                // Track if this is the opening paren for func/proc parameters
                if (lambdaState == LambdaState::SAW_FUNC) {
                    funcParenOpenedLevel = this->opened;  // before incrementing
                    lambdaState = LambdaState::IN_PARAMS;
                }
                // Determine if this is a call context by checking previous token
                // Use pendingTokens.back() if available, otherwise use lastAppendedTokenType
                // (the token that was just returned in the previous nextToken() call)
                auto prevType = !this->pendingTokens.empty()
                    ? this->pendingTokens.back()->getType()
                    : this->lastAppendedTokenType;
                // Call context: preceded by something that can be called
                // IDENTIFIER, ), ], THIS, or type keywords that can be constructors
                bool isCall = (prevType == IDENTIFIER ||
                              prevType == CLOSE_PAREN ||
                              prevType == CLOSE_BRACK ||
                              prevType == THIS);
                this->opened++;
                this->openers.push({static_cast<int>(OPEN_PAREN), isCall});
                this->pendingTokens.push_back(std::move(currentToken));
                break;
            }
            case OPEN_BRACK: {
                // Determine if this is indexing (call-like) or list literal
                // Use pendingTokens.back() if available, otherwise use lastAppendedTokenType
                auto prevType = !this->pendingTokens.empty()
                    ? this->pendingTokens.back()->getType()
                    : this->lastAppendedTokenType;
                // Indexing: preceded by something that can be indexed
                bool isIndexing = (prevType == IDENTIFIER ||
                              prevType == CLOSE_PAREN ||
                              prevType == CLOSE_BRACK ||
                              prevType == THIS);
                this->opened++;
                this->openers.push({static_cast<int>(OPEN_BRACK), isIndexing});
                this->pendingTokens.push_back(std::move(currentToken));
                break;
            }
            case OPEN_BRACE:
                // Dict/set literal - not a call context
                this->opened++;
                this->openers.push({static_cast<int>(OPEN_BRACE), false});
                this->pendingTokens.push_back(std::move(currentToken));
                break;
            case CLOSE_PAREN:
                this->opened--;
                if (!this->openers.empty())
                    this->openers.pop();
                // Check if this closes the func/proc parameter list
                if (lambdaState == LambdaState::IN_PARAMS && this->opened == funcParenOpenedLevel) {
                    lambdaState = LambdaState::AFTER_PARAMS;
                }
                // Pop any lambda suites whose enclosing call has ended
                // (opened dropped below their openedAtStart)
                while (!this->lambdaSuites.empty() &&
                       this->opened < this->lambdaSuites.top().openedAtStart) {
                    this->lambdaSuites.pop();
                }
                this->pendingTokens.push_back(std::move(currentToken));
                break;
            case CLOSE_BRACK:
            case CLOSE_BRACE:
                this->opened--;
                if (!this->openers.empty())
                    this->openers.pop();
                // Pop any lambda suites whose enclosing context has ended
                while (!this->lambdaSuites.empty() &&
                       this->opened < this->lambdaSuites.top().openedAtStart) {
                    this->lambdaSuites.pop();
                }
                this->pendingTokens.push_back(std::move(currentToken));
                break;
            case COLON:
                // Check if this starts a lambda suite
                if (lambdaState == LambdaState::AFTER_PARAMS) {
                    // Start a lambda suite - record current indent level and opened count
                    LambdaSuiteInfo info;
                    info.baseIndentLevel = this->indentLengths.top();
                    info.openedAtStart = this->opened;
                    this->lambdaSuites.push(info);
                    lambdaState = LambdaState::NONE;
                }
                this->pendingTokens.push_back(std::move(currentToken));
                break;
            case NEWLINE:
                // Reset lambda detection state on NEWLINE (lambda header must be on one line)
                if (lambdaState != LambdaState::NONE) {
                    lambdaState = LambdaState::NONE;
                }

                if (this->opened > 0) {                             //*** https://docs.python.org/3/reference/lexical_analysis.html#implicit-line-joining
                    // Check if we're inside a lambda suite
                    if (!this->lambdaSuites.empty()) {
                        // Calculate effective opened (parens opened within the lambda)
                        int effectiveOpened = this->opened - this->lambdaSuites.top().openedAtStart;
                        if (effectiveOpened > 0) {
                            // Inside parens/brackets within the lambda
                            // Check if the innermost opener is a call/indexing context
                            if (!this->openers.empty() && this->openers.top().isCallContext) {
                                // Call context - emit NEWLINE (optional comma feature)
                                // But skip blank lines and comment-only lines
                                switch (_input->LA(1)) {
                                    case '\r': case '\n': case '\f': case '#':
                                        continue;  // blank line
                                    case '/':
                                        if (_input->LA(2) == '/')
                                            continue;  // comment line
                                        // fall through
                                    default:
                                        this->pendingTokens.push_back(std::move(currentToken));
                                }
                                break;
                            }
                            // Expression grouping or literal - skip NEWLINE (implicit line joining)
                            continue;
                        }
                        // In lambda suite but not inside inner parens (effectiveOpened <= 0)
                        // Check if this is a one-liner lambda:
                        // - Previous token is NOT COLON (expression followed the :, not NEWLINE)
                        // - We haven't entered the lambda body yet (no INDENT was inserted)
                        // Use pendingTokens.back() if available, otherwise use lastAppendedTokenType
                        auto prevTokenType = !this->pendingTokens.empty()
                            ? this->pendingTokens.back()->getType()
                            : this->lastAppendedTokenType;
                        bool prevWasColon = (prevTokenType == COLON);
                        bool noIndentYet = this->indentLengths.top() <= this->lambdaSuites.top().baseIndentLevel;
                        if (!prevWasColon && noIndentYet) {
                            // One-liner lambda is complete - pop the suite
                            this->lambdaSuites.pop();
                            // Now handle NEWLINE based on current context (after lambda)
                            if (!this->openers.empty()) {
                                if (this->openers.top().isCallContext) {
                                    // Call context - emit NEWLINE as argument separator
                                    switch (_input->LA(1)) {
                                        case '\r': case '\n': case '\f': case '#':
                                            continue;  // blank line
                                        case '/':
                                            if (_input->LA(2) == '/')
                                                continue;  // comment line
                                            // fall through
                                        default:
                                            this->pendingTokens.push_back(std::move(currentToken));
                                    }
                                    break;
                                } else {
                                    // Expression grouping - skip NEWLINE (implicit line joining)
                                    continue;
                                }
                            }
                            // No openers - fall through to emit NEWLINE
                        }
                        // Multi-line lambda (prevWasColon=true) or inside lambda body - fall through to emit NEWLINE with INDENT/DEDENT
                    } else if (!this->openers.empty()) {
                        // Not in lambda suite, inside some bracket
                        if (this->openers.top().tokenType == OPEN_BRACK) {
                            // Inside [] - check if it's indexing or list literal
                            if (this->openers.top().isCallContext) {
                                // Indexing - skip NEWLINE (implicit line joining for index expressions)
                                continue;
                            }
                            // List literal - emit NEWLINE (element separator)
                            this->pendingTokens.push_back(std::move(currentToken));
                            break;
                        } else if (this->openers.top().tokenType == OPEN_PAREN) {
                            if (this->openers.top().isCallContext) {
                                // Call context - emit NEWLINE (optional comma feature)
                                // But skip blank lines and comment-only lines
                                switch (_input->LA(1)) {
                                    case '\r': case '\n': case '\f': case '#':
                                        continue;  // blank line
                                    case '/':
                                        if (_input->LA(2) == '/')
                                            continue;  // comment line
                                        // fall through
                                    default:
                                        this->pendingTokens.push_back(std::move(currentToken));
                                }
                                break;
                            }
                            // Expression grouping - skip NEWLINE (implicit line joining)
                            continue;
                        } else {
                            // Inside {} - dict literal, emit NEWLINE (element separator)
                            // But skip blank lines and comment-only lines
                            switch (_input->LA(1)) {
                                case '\r': case '\n': case '\f': case '#':
                                    continue;  // blank line
                                case '/':
                                    if (_input->LA(2) == '/')
                                        continue;  // comment line
                                    // fall through
                                default:
                                    this->pendingTokens.push_back(std::move(currentToken));
                            }
                            break;
                        }
                    } else {
                        continue;  // We're inside an implicit line joining section, skip the NEWLINE token
                    }
                }
                // Emit NEWLINE (either opened == 0 or we're in a lambda suite)
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
                        // Check if any lambda suites have ended (DEDENT back to or below base level)
                        bool lambdaSuiteEnded = false;
                        while (!this->lambdaSuites.empty() &&
                               this->indentLengths.top() <= this->lambdaSuites.top().baseIndentLevel) {
                            this->lambdaSuites.pop();
                            lambdaSuiteEnded = true;
                        }
                        // If lambda suite ended and we're in a call context, emit extra NEWLINE
                        // as argument separator (the NEWLINE that triggered DEDENT was consumed by suite)
                        // But only if the next token isn't already a separator (comma or close paren)
                        if (lambdaSuiteEnded && !this->openers.empty() && this->openers.top().isCallContext) {
                            auto nextChar = _input->LA(1);
                            // Skip whitespace to peek at actual next token
                            int offset = 1;
                            while (nextChar == ' ' || nextChar == '\t') {
                                nextChar = _input->LA(++offset);
                            }
                            // Don't emit NEWLINE if next is comma, close paren/bracket/brace, or newline
                            if (nextChar != ',' && nextChar != ')' && nextChar != ']' && nextChar != '}' &&
                                nextChar != '\n' && nextChar != '\r') {
                                this->insertToken("NEWLINE", NEWLINE);
                            }
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
            case FUNC:
            case PROC:
                // Already handled above - just add to pending
                this->pendingTokens.push_back(std::move(currentToken));
                break;
            default:
                // Reset lambda detection for tokens that can't be part of lambda signature
                // (keep AFTER_PARAMS state for YIELDS/return type tokens)
                if (lambdaState == LambdaState::SAW_FUNC) {
                    // After FUNC, we expect '(' - anything else cancels
                    lambdaState = LambdaState::NONE;
                }
                // AFTER_PARAMS stays until we see COLON or NEWLINE (allows -> and type names)
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
