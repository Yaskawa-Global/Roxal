#include <sstream>
#include <iomanip>
#include <algorithm>

#include <core/common.h>
#include "ASTEditor.h"

using namespace roxal;
using namespace roxal::ast;


//all operations either act on a removed, or inserted node
//verify their type for cases where multiple types might be used ...
template<typename T>
bool ASTEditor::isChildA()
{
    bool childIsAT = false;
    if(m_removed != nullptr)
    {
        if(std::dynamic_pointer_cast<T>(m_removed) != nullptr)
        {
            childIsAT = true;
        }
    }
    else if(m_inserted != nullptr)
    {
        if(std::dynamic_pointer_cast<T>(m_inserted) != nullptr)
        {
            childIsAT = true;
        }
    }
    
    return childIsAT;
}

int ASTEditor::findChildInSuite(std::vector<std::variant<ptr<Declaration>, ptr<Statement>>>& vec, ptr<AST> toFind)
{
    int position = 0;
    bool found = false;

    if((isChildA<Statement>() == false) && (isChildA<Declaration>() == false))
        return -1;

    
    if (std::dynamic_pointer_cast<Declaration>(toFind))
    {
        for(const auto& child:vec)
        {
            if((std::holds_alternative<ptr<Declaration>>(child)) && 
            (std::get<ptr<Declaration>>(child) == std::dynamic_pointer_cast<Declaration>(toFind)))
            {
                found = true;
                break;
            }
            position++;
        }
    }
    else if(std::dynamic_pointer_cast<Statement>(toFind))
    {
        for(const auto& child:vec)
        {
            if((std::holds_alternative<ptr<Statement>>(child)) && 
            (std::get<ptr<Statement>>(child) == std::dynamic_pointer_cast<Statement>(toFind)))
            {
                found = true;
                break;
            }
            position++;
        }
    }

    if(found == false)
        position = -1;

    return position;
}

void ASTEditor::deleteFromSuite(std::vector<std::variant<ptr<Declaration>, ptr<Statement>>>& vec)
{   
    //find the iterator to sibling
    int position = findChildInSuite(vec, m_removed);
    bool found = (position > -1);

    //delete
    if (found)
    {
        //delete from source member of ast
        std::string newSource = deleteStringLinesAtInterval(*(m_tree->source),
            m_removed->interval.first.line, m_removed->interval.first.pos,
            m_removed->interval.second.line, m_removed->interval.second.pos);

        //update line positions
        int linedelta = m_removed->interval.second.line - m_removed->interval.first.line + 1;
        updateAstLinePositions(m_tree, m_removed->interval.first.line, 0, -1*linedelta, 0);
        
        //erase the ast node
        vec.erase(vec.begin() + position);

        *m_tree->source = newSource;
    }
}

void ASTEditor::insertBeforeOrAfterIntoSuite(std::vector<std::variant<ptr<Declaration>, ptr<Statement>>>& vec, bool before)
{   
    //find the position of the sibling
    int position = findChildInSuite(vec, m_sibling);
    bool found = (position > -1);

    
    //if not before, increment the position
    if(before == false)
    {
        position++;
    }

    //adjust the line intervals of the tree and then
    //insert the new subtree based on the position of the sibling
    if (found)
    {
        //update line positions
        int numInsertedLines = m_inserted->interval.second.line - m_inserted->interval.first.line + 1;
        if(before)
        {
            //fix up the nodes in the inserted subtree
            updateAstLinePositions(m_inserted, 0, 0, m_sibling->interval.first.line - m_inserted->interval.first.line, m_sibling->interval.first.pos);

            //fix the main tree before inserting the new node
            //line fix includes sibling for inserting before 
            updateAstLinePositions(m_tree, m_sibling->interval.first.line, 0, numInsertedLines, 0);
        }
        else
        {
            //fix up the nodes in the inserted subtree
            int delta = m_sibling->interval.second.line + 1 - m_inserted->interval.first.line;
            updateAstLinePositions(m_inserted, 0, 0, m_sibling->interval.second.line + 1 - m_inserted->interval.first.line, m_sibling->interval.first.pos);

            //fix the main tree before inserting the new node
            //line fix includes everything after sibling for inserting after 
            updateAstLinePositions(m_tree, m_sibling->interval.second.line + 1, 0, numInsertedLines, 0);
        }

        //insert the node into the main tree
        if (std::dynamic_pointer_cast<Declaration>(m_inserted))
        {
            vec.insert(vec.begin() + position, std::dynamic_pointer_cast<Declaration>(m_inserted));
        }
        else if (std::dynamic_pointer_cast<Statement>(m_inserted))
        {
            vec.insert(vec.begin() + position, std::dynamic_pointer_cast<Statement>(m_inserted));
        }

        //insert new text code into the main tree's source
        std::string newSource = insertStringLinesAtInterval(*(m_tree->source),
            *(m_inserted->source), m_inserted->interval.first.line, m_inserted->interval.first.pos);

        //update the inserted node's source to point to the main tree's
        updateInsertedNodeSource();

        *m_tree->source = newSource;
    }
}

void ASTEditor::insertSubtreeAfter(ptr<roxal::ast::AST> tree, ptr<roxal::ast::AST> parent, ptr<roxal::ast::AST> sibling, ptr<roxal::ast::AST> toInsert)
{
    std::lock_guard<std::mutex> guard(m_memberLock);

    m_tree = tree;
    m_parent = parent; 
    m_sibling = sibling; 
    m_removed = nullptr;
    m_inserted = toInsert;

    m_activeOperation = AstOperation::InsertAfter;
    m_parent->accept(*this);    
}

void ASTEditor::insertSubtreeBefore(ptr<roxal::ast::AST> tree, ptr<roxal::ast::AST> parent, ptr<roxal::ast::AST> sibling, ptr<roxal::ast::AST> toInsert)
{
    std::lock_guard<std::mutex> guard(m_memberLock);

    m_tree = tree;
    m_parent = parent; 
    m_sibling = sibling; 
    m_removed = nullptr;
    m_inserted = toInsert;

    m_activeOperation = AstOperation::InsertBefore;
    m_parent->accept(*this);    
}

void ASTEditor::deleteSubtree(ptr<roxal::ast::AST> tree, ptr<roxal::ast::AST> parent, ptr<roxal::ast::AST> toRemove)
{
    std::lock_guard<std::mutex> guard(m_memberLock);

    m_tree = tree;
    m_parent = parent; 
    m_sibling = nullptr; 
    m_removed = toRemove;
    m_inserted = nullptr;

    m_activeOperation = AstOperation::Delete;
    m_parent->accept(*this);    
}

void ASTEditor::replaceSubtree(ptr<roxal::ast::AST> tree, ptr<roxal::ast::AST> parent, ptr<roxal::ast::AST> toRemove, ptr<roxal::ast::AST> toInsert)
{
    //insert adjacent to the node we will remove
    insertSubtreeAfter(tree, parent, toRemove, toInsert);

    //then remove the original
    deleteSubtree(tree, parent, toRemove);
}

std::any ASTEditor::visit(ptr<ast::File> ast)
{
    //
    switch(m_activeOperation)
    {
        case AstOperation::InsertBefore:
        {  
            insertBeforeOrAfterIntoSuite(ast->declsOrStmts, true);
            break;
        }
        case AstOperation::InsertAfter:
        {
            insertBeforeOrAfterIntoSuite(ast->declsOrStmts, false);
            break;
        }
        case AstOperation::Delete:
        {
           deleteFromSuite(ast->declsOrStmts);

            break;
        }
    }
    return {};
}

std::any ASTEditor::visit(ptr<ast::SingleInput> ast)
{
    //Nothing to do here?
    //ptr<Statement> stmt;
    return {};
}

std::any ASTEditor::visit(ptr<ast::Annotation> ast)
{
    //icu::UnicodeString name;
    //std::vector<ArgNameExpr> args;
    //handle ArgNameExpr
    return {};
}

std::any ASTEditor::visit(ptr<ast::TypeDecl> ast)
{
    //std::optional<icu::UnicodeString> extends;
    //std::vector<icu::UnicodeString> implements;

    //for all suites in all methods
    //std::vector<ptr<Function>> methods;
    for(auto &method: ast->methods)
    {
        method->accept(*this);
    }
    return {};
}

std::any ASTEditor::visit(ptr<ast::FuncDecl> ast)
{
    //single function
    //ptr<Function> func;
    ast->func->accept(*this);
    return {};
}

std::any ASTEditor::visit(ptr<ast::VarDecl> ast)
{
    //icu::UnicodeString name;
    //std::optional<ptr<Expression>> initializer;
    //std::optional<std::variant<BuiltinType,icu::UnicodeString>> varType;
    /*
     if(ast->initializer.has_value())
        ast->initializer.value();
    */
    return {};
}

std::any ASTEditor::visit(ptr<ast::Suite> ast)
{
    //handles only suite
    switch(m_activeOperation)
    {
        case AstOperation::InsertBefore:
        {  
            insertBeforeOrAfterIntoSuite(ast->declsOrStmts, true);
            break;
        }
        case AstOperation::InsertAfter:
        {
            insertBeforeOrAfterIntoSuite(ast->declsOrStmts, false);
            break;
        }
        case AstOperation::Delete:
        {
           deleteFromSuite(ast->declsOrStmts);

            break;
        }
    }
    return {};
}

std::any ASTEditor::visit(ptr<ast::ExpressionStatement> ast)
{
    //only a single expression
    //ptr<ast::Expression> expr;
    return {};
}

std::any ASTEditor::visit(ptr<ast::ReturnStatement> ast)
{
    //can only have one and its optional
    //ptr<ast::Expression> expr;
    //handle replace and delete
    /*
     if(ast->expr.has_value())
        ast->expr.value();
    */
    return {};
}

std::any ASTEditor::visit(ptr<ast::IfStatement> ast)
{
    //need to handle expressions in conditionalSuites
    //std::vector<std::pair<ptr<ast::Expression>, ptr<ast::Suite>>> conditionalSuites;
    //std::optional<ptr<ast::Suite>> elseSuite;
    
    if(isChildA<Expression>())
    {
        //modify expression
        for(auto e_s_pair: ast->conditionalSuites)
            e_s_pair.first->accept(*this);
    }
    else
    {
        //insert into suites
        for(auto e_s_pair: ast->conditionalSuites)
            e_s_pair.second->accept(*this);
        if(ast->elseSuite.has_value())
            ast->elseSuite.value()->accept(*this);
    }
    return {};
}

std::any ASTEditor::visit(ptr<ast::WhileStatement> ast)
{
    //ptr<ast::Expression> condition;
    //ptr<ast::Suite> body;

    if(isChildA<Expression>())
    {
        //modify expression
        ast->condition->accept(*this);
    }
    else
    {
        //insert into suite
        ast->body->accept(*this);
    }
    return {};
}

std::any ASTEditor::visit(ptr<ast::Function> ast)
{
    //icu::UnicodeString name;
    //std::vector<ptr<Parameter>> params;
    //std::optional<std::variant<BuiltinType,icu::UnicodeString>> returnType;
    //ptr<Suite> body;

    //insert into suite
    ast->body->accept(*this);
    return {};
}

std::any ASTEditor::visit(ptr<ast::Parameter> ast)
{
    //icu::UnicodeString name;
    //std::optional<std::variant<BuiltinType,icu::UnicodeString>> type;
    //std::optional<ptr<Expression>> defaultValue;
    return {};
}

std::any ASTEditor::visit(ptr<ast::Assignment> ast)
{
    //ptr<Expression> lhs;
    //ptr<Expression> rhs;
    return {};
}

std::any ASTEditor::visit(ptr<ast::BinaryOp> ast)
{
    //Op op;
    //ptr<Expression> lhs;
    //ptr<Expression> rhs;
    return {};
}

std::any ASTEditor::visit(ptr<ast::UnaryOp> ast)
{
    //Op op;
    //std::optional<icu::UnicodeString> member;
    //ptr<Expression> arg;
    return {};
}

std::any ASTEditor::visit(ptr<ast::Variable> ast)
{
    //icu::UnicodeString name;
    return {};
}

std::any ASTEditor::visit(ptr<ast::Call> ast)
{
    //std::vector<ArgNameExpr> args;
    //ptr<Expression> lhs;
    return {};
}

std::any ASTEditor::visit(ptr<ast::Range> ast)
{
    //ptr<Expression> start;
    //ptr<Expression> stop; 
    //ptr<Expression> step;  
    return {};
}

std::any ASTEditor::visit(ptr<ast::Index> ast)
{
    //ptr<Expression> indexable;
    //std::vector<ptr<Expression>> args;
    return {};
}

std::any ASTEditor::visit(ptr<ast::Literal> ast)
{
    //LiteralType literalType;
    return {};
}

std::any ASTEditor::visit(ptr<ast::Bool> ast)
{
    // bool value;
    return {};
}

std::any ASTEditor::visit(ptr<ast::Str> ast)
{
    //icu::UnicodeString str;
    return {};
}

std::any ASTEditor::visit(ptr<ast::Type> ast)
{
    //BuiltinType t;
    return {};
}

std::any ASTEditor::visit(ptr<ast::Num> ast)
{
    //std::variant<int32_t,double> num;
    return {};
}

std::any ASTEditor::visit(ptr<ast::List> ast)
{
    //std::vector<ptr<Expression>> elements;
    return {};
}

std::any ASTEditor::visit(ptr<ast::Dict> ast)
{
    //std::vector<std::pair<ptr<Expression>,ptr<Expression>>> entries;
    return {};
}

void ASTEditor::updateAstLinePositions(ptr<roxal::ast::AST> ast, int startingLine, int startingPos, int lineDelta, int posDelta)
{
    auto incrementer = [startingLine, startingPos, lineDelta, posDelta](ptr<roxal::ast::AST> ast){

        //starting interval
        if(ast->interval.first.line > startingLine)
        {
            ast->interval.first.line += lineDelta;
            ast->interval.first.pos += posDelta;
        }
        else if((ast->interval.first.line == startingLine) && (ast->interval.first.pos  >= startingPos))
        {
            ast->interval.first.line += lineDelta;
            ast->interval.first.pos += posDelta;
        }

        //ending interval
        if(ast->interval.second.line > startingLine)
        {
            ast->interval.second.line += lineDelta;
            ast->interval.second.pos += posDelta;
        }
        else if((ast->interval.second.line == startingLine) && (ast->interval.second.pos  >= startingPos))
        {
            ast->interval.second.line += lineDelta;
            ast->interval.second.pos += posDelta;
        }

    };

    AstAllNodeCallback lineIncrementVisitor;
    lineIncrementVisitor.setCallback(incrementer);
    lineIncrementVisitor.run(ast);
}

void ASTEditor::updateInsertedNodeSource()
{
    auto sourceUpdater = [this](ptr<roxal::ast::AST> ast){
        ast->source = m_tree->source;
    };

    AstAllNodeCallback sourceUpdateVisitor;
    sourceUpdateVisitor.setCallback(sourceUpdater);
    sourceUpdateVisitor.run(m_inserted);
}

void AstAllNodeCallback::run(ptr<roxal::ast::AST> ast)
{
    ast->accept(*this);
}

void AstAllNodeCallback::setCallback(std::function<void(ptr<roxal::ast::AST>)> f)
{
    m_f = f;
}
