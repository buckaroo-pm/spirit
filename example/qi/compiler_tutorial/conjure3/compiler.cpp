/*=============================================================================
    Copyright (c) 2001-2011 Joel de Guzman

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#include "config.hpp"
#include "compiler.hpp"
#include "annotation.hpp"
#include "vm.hpp"

#include <boost/foreach.hpp>
#include <boost/variant/apply_visitor.hpp>
#include <boost/assert.hpp>
#include <boost/lexical_cast.hpp>
#include <set>

namespace client { namespace code_gen
{
    void compiler::init_fpm()
    {
        // Set up the optimizer pipeline.  Start with registering info about how the
        // target lays out data structures.
        fpm.add(new llvm::TargetData(*vm.execution_engine()->getTargetData()));
        // Provide basic AliasAnalysis support for GVN.
        fpm.add(llvm::createBasicAliasAnalysisPass());
        // Promote allocas to registers.
        fpm.add(llvm::createPromoteMemoryToRegisterPass());
        // Do simple "peephole" optimizations and bit-twiddling optzns.
        fpm.add(llvm::createInstructionCombiningPass());
        // Reassociate expressions.
        fpm.add(llvm::createReassociatePass());
        // Eliminate Common SubExpressions.
        fpm.add(llvm::createGVNPass());
        // Simplify the control flow graph (deleting unreachable blocks, etc).
        fpm.add(llvm::createCFGSimplificationPass());

        fpm.doInitialization();
    }

    llvm::Value* compiler::operator()(unsigned int x)
    {
        return llvm::ConstantInt::get(context(), llvm::APInt(int_size, x));
    }

    llvm::Value* compiler::operator()(bool x)
    {
        return llvm::ConstantInt::get(context(), llvm::APInt(1, x));
    }

    llvm::Value* compiler::operator()(ast::literal const& x)
    {
        return boost::apply_visitor(*this, x.val);
    }

    llvm::Value* compiler::operator()(ast::identifier const& x)
    {
        // Look this variable up in the function.
        llvm::Value* value = named_values[x.name];
        if (value == 0)
        {
            error_handler(x.id, "Undeclared variable: " + x.name);
            return 0;
        }

        // Load the value.
        return builder.CreateLoad(value, x.name.c_str());
    }

    llvm::Value* compiler::operator()(ast::unary const& x)
    {
        llvm::Value* operand = boost::apply_visitor(*this, x.operand_);
        if (operand == 0)
            return 0;

        switch (x.operator_)
        {
            case token::minus: return builder.CreateNeg(operand, "negtmp");
            case token::not_: return builder.CreateNot(operand, "nottmp");
            case token::plus: return operand;
            default: BOOST_ASSERT(0); return 0;
        }
    }

    llvm::Value* compiler::operator()(ast::function_call const& x)
    {
        llvm::Function* callee = vm.module()->getFunction(x.function_name.name);
        if (callee == 0)
        {
            error_handler(x.function_name.id, "Function not found: " + x.function_name.name);
            return false;
        }

        if (callee->arg_size() != x.args.size())
        {
            error_handler(x.function_name.id, "Wrong number of arguments: " + x.function_name.name);
            return 0;
        }

        std::vector<llvm::Value*> args;
        BOOST_FOREACH(ast::expression const& expr, x.args)
        {
            args.push_back((*this)(expr));
            if (args.back() == 0)
                return 0;
        }

        return builder.CreateCall(callee, args.begin(), args.end(), "calltmp");
    }

    namespace
    {
        int precedence[] = {
            // precedence 1
            1, // op_comma

            // precedence 2
            2, // op_assign
            2, // op_plus_assign
            2, // op_minus_assign
            2, // op_times_assign
            2, // op_divide_assign
            2, // op_mod_assign
            2, // op_bit_and_assign
            2, // op_bit_xor_assign
            2, // op_bitor_assign
            2, // op_shift_left_assign
            2, // op_shift_right_assign

            // precedence 3
            3, // op_logical_or

            // precedence 4
            4, // op_logical_and

            // precedence 5
            5, // op_bit_or

            // precedence 6
            6, // op_bit_xor

            // precedence 7
            7, // op_bit_and

            // precedence 8
            8, // op_equal
            8, // op_not_equal

            // precedence 9
            9, // op_less
            9, // op_less_equal
            9, // op_greater
            9, // op_greater_equal

            // precedence 10
            10, // op_shift_left
            10, // op_shift_right

            // precedence 11
            11, // op_plus
            11, // op_minus

            // precedence 12
            12, // op_times
            12, // op_divide
            12 // op_mod
        };
    }

    inline int precedence_of(token::type op)
    {
        return precedence[op & 0xFF];
    }

    inline bool is_left_assoc(token::type op)
    {
        // only the assignment operators are right to left
        return (op & token::op_assign) == 0;
    }

    llvm::Value* compiler::compile_binary_expression(
        llvm::Value* lhs, llvm::Value* rhs, token::type op)
    {
        switch (op)
        {
            case token::plus: return builder.CreateAdd(lhs, rhs, "addtmp");
            case token::minus: return builder.CreateSub(lhs, rhs, "subtmp");
            case token::times: return builder.CreateMul(lhs, rhs, "multmp");
            case token::divide: return builder.CreateSDiv(lhs, rhs, "divtmp");

            case token::equal: return builder.CreateICmpEQ(lhs, rhs, "eqtmp");
            case token::not_equal: return builder.CreateICmpNE(lhs, rhs, "netmp");
            case token::less: return builder.CreateICmpSLT(lhs, rhs, "slttmp");
            case token::less_equal: return builder.CreateICmpSLE(lhs, rhs, "sletmp");
            case token::greater: return builder.CreateICmpSGT(lhs, rhs, "sgttmp");
            case token::greater_equal: return builder.CreateICmpSGE(lhs, rhs, "sgetmp");

            case token::logical_or: return builder.CreateOr(lhs, rhs, "ortmp");
            case token::logical_and: return builder.CreateAnd(lhs, rhs, "andtmp");
            default: BOOST_ASSERT(0); return 0;
        }
    }

    // The Shunting-yard algorithm
    llvm::Value* compiler::compile_expression(
        int min_precedence,
        llvm::Value* lhs,
        std::list<ast::operation>::const_iterator& rest_begin,
        std::list<ast::operation>::const_iterator rest_end)
    {
        while ((rest_begin != rest_end) &&
            (precedence_of(rest_begin->operator_) >= min_precedence))
        {
            token::type op = rest_begin->operator_;
            llvm::Value* rhs = boost::apply_visitor(*this, rest_begin->operand_);
            if (rhs == 0)
                return 0;
            ++rest_begin;

            while ((rest_begin != rest_end) &&
                (precedence_of(rest_begin->operator_) > precedence_of(op)))
            {
                token::type next_op = rest_begin->operator_;
                rhs = compile_expression(
                    precedence_of(next_op), rhs, rest_begin, rest_end);
            }

            lhs = compile_binary_expression(lhs, rhs, op);
        }
        return lhs;
    }

    llvm::Value* compiler::operator()(ast::expression const& x)
    {
        llvm::Value* lhs = boost::apply_visitor(*this, x.first);
        if (lhs == 0)
            return 0;
        std::list<ast::operation>::const_iterator rest_begin = x.rest.begin();
        return compile_expression(0, lhs, rest_begin, x.rest.end());
    }

    llvm::Value* compiler::operator()(ast::assignment const& x)
    {
        llvm::Value* lhs = named_values[x.lhs.name];
        if (lhs == 0)
        {
            error_handler(x.lhs.id, "Undeclared variable: " + x.lhs.name);
            return 0;
        }

        llvm::Value* rhs = (*this)(x.rhs);
        if (rhs == 0)
            return 0;

        builder.CreateStore(rhs, lhs);
        return rhs;
    }

    //  Create an alloca instruction in the entry block of
    //  the function. This is used for mutable variables etc.
    llvm::AllocaInst*
    create_entry_block_alloca(
        llvm::Function* function, std::string const& var, llvm::LLVMContext& context)
    {
        llvm::IRBuilder<> builder(
            &function->getEntryBlock(), function->getEntryBlock().begin());
        return builder.CreateAlloca(
            llvm::Type::getIntNTy(context, int_size), 0, var.c_str());
    }

    bool compiler::operator()(ast::variable_declaration const& x)
    {
        if (named_values.find(x.lhs.name) != named_values.end())
        {
            error_handler(x.lhs.id, "Duplicate variable: " + x.lhs.name);
            return false;
        }

        llvm::Function* function = builder.GetInsertBlock()->getParent();
        llvm::Value* init = 0;
        std::string const& var = x.lhs.name;

        if (x.rhs) // if there's an RHS initializer
        {
            init = (*this)(*x.rhs);
            if (init == 0) // don't add the variable if the RHS fails
                return false;
        }

        llvm::AllocaInst* alloca
            = create_entry_block_alloca(function, var, context());
        if (init != 0)
            builder.CreateStore(init, alloca);

        // Remember this binding.
        named_values[var] = alloca;
        return true;
    }

    struct compiler::statement_compiler : compiler
    {
        typedef bool result_type;
    };

    compiler::statement_compiler& compiler::as_statement()
    {
        return *static_cast<statement_compiler*>(this);
    }

    bool compiler::operator()(ast::statement const& x)
    {
        return boost::apply_visitor(as_statement(), x);
    }

    bool compiler::operator()(ast::statement_list const& x)
    {
        BOOST_FOREACH(ast::statement const& s, x)
        {
            if (!(*this)(s))
                return false;
        }
        return true;
    }

    bool compiler::operator()(ast::if_statement const& x)
    {
        llvm::Value* condition = (*this)(x.condition);
        if (condition == 0)
            return 0;

        llvm::Function* function = builder.GetInsertBlock()->getParent();

        // Create blocks for the then and else cases.  Insert the 'then' block at the
        // end of the function.
        llvm::BasicBlock* then_block = llvm::BasicBlock::Create(context(), "if.then", function);
        llvm::BasicBlock* else_block = 0;
        llvm::BasicBlock* exit_block = 0;

        if (x.else_)
        {
            else_block = llvm::BasicBlock::Create(context(), "if.else");
            builder.CreateCondBr(condition, then_block, else_block);
        }
        else
        {
            exit_block = llvm::BasicBlock::Create(context(), "if.end");
            builder.CreateCondBr(condition, then_block, exit_block);
        }

        // Emit then value.
        builder.SetInsertPoint(then_block);
        if (!(*this)(x.then))
            return 0;
        if (then_block->getTerminator() == 0)
        {
            if (exit_block == 0)
                exit_block = llvm::BasicBlock::Create(context(), "if.end");
            builder.CreateBr(exit_block);
        }
        // Codegen of 'then' can change the current block, update then_block
        then_block = builder.GetInsertBlock();

        if (x.else_)
        {
            // Emit else block.
            function->getBasicBlockList().push_back(else_block);
            builder.SetInsertPoint(else_block);
            if (!(*this)(*x.else_))
                return 0;
            if (else_block->getTerminator() == 0)
            {
                if (exit_block == 0)
                    exit_block = llvm::BasicBlock::Create(context(), "if.end");
                builder.CreateBr(exit_block);
            }
            // Codegen of 'else' can change the current block, update else_block
            else_block = builder.GetInsertBlock();
        }

        if (exit_block != 0)
        {
            // Emit exit block
            function->getBasicBlockList().push_back(exit_block);
            builder.SetInsertPoint(exit_block);
        }
        return true;
    }

    bool compiler::operator()(ast::while_statement const& x)
    {
        llvm::Function* function = builder.GetInsertBlock()->getParent();

        llvm::BasicBlock* cond_block = llvm::BasicBlock::Create(context(), "while.cond", function);
        llvm::BasicBlock* body_block = llvm::BasicBlock::Create(context(), "while.body");
        llvm::BasicBlock* exit_block = llvm::BasicBlock::Create(context(), "while.end");

        builder.CreateBr(cond_block);
        builder.SetInsertPoint(cond_block);
        llvm::Value* condition = (*this)(x.condition);
        if (condition == 0)
            return false;
        builder.CreateCondBr(condition, body_block, exit_block);
        function->getBasicBlockList().push_back(body_block);
        builder.SetInsertPoint(body_block);

        if (!(*this)(x.body))
            return false;

        if (body_block->getTerminator() == 0)
            builder.CreateBr(cond_block); // loop back

        // Emit exit block
        function->getBasicBlockList().push_back(exit_block);
        builder.SetInsertPoint(exit_block);

        return true;
    }

    bool compiler::operator()(ast::return_statement const& x)
    {
        if (void_return)
        {
            if (x.expr)
            {
                error_handler(
                    x.id, "'void' function returning a value: ");
                return false;
            }
        }
        else
        {
            if (!x.expr)
            {
                error_handler(
                    x.id, current_function_name
                    + " function must return a value: ");
                return false;
            }
        }

        if (x.expr)
        {
            llvm::Value* return_val = (*this)(*x.expr);
            if (return_val == 0)
                return false;
            builder.CreateStore(return_val, return_alloca);
        }

        builder.CreateBr(return_block);
        return true;
    }

    llvm::Function* compiler::function_decl(ast::function const& x)
    {
        void_return = x.return_type == "void";
        current_function_name = x.function_name.name;

        llvm::Type* int_type =
            llvm::Type::getIntNTy(context(), int_size);
        llvm::Type* void_type = llvm::Type::getVoidTy(context());

        std::vector<llvm::Type*> ints(x.args.size(), int_type);
        llvm::Type* return_type = void_return ? void_type : int_type;

        llvm::FunctionType* function_type =
            llvm::FunctionType::get(void_return ? void_type : int_type, ints, false);

        llvm::Function* function =
            llvm::Function::Create(
                function_type, llvm::Function::ExternalLinkage,
                current_function_name, vm.module());

        // If function conflicted, the function already exixts. If it has a
        // body, don't allow redefinition or reextern.
        if (function->getName() != current_function_name)
        {
            // Delete the one we just made and get the existing one.
            function->eraseFromParent();
            function = vm.module()->getFunction(current_function_name);

            // If function already has a body, reject this.
            if (!function->empty())
            {
                error_handler(
                    x.function_name.id,
                    "Duplicate function: " + x.function_name.name);
                return 0;
            }

            // If function took a different number of args, reject.
            if (function->arg_size() != x.args.size())
            {
                error_handler(
                    x.function_name.id,
                    "Redefinition of function with different # args: "
                        + x.function_name.name);
                return 0;
            }

            // Set names for all arguments.
            llvm::Function::arg_iterator iter = function->arg_begin();
            BOOST_FOREACH(ast::identifier const& arg, x.args)
            {
                iter->setName(arg.name);
                ++iter;
            }
        }
        return function;
    }

    void compiler::function_allocas(ast::function const& x, llvm::Function* function)
    {
        // CreateArgumentAllocas - Create an alloca for each argument and register the
        // argument in the symbol table so that references to it will succeed.
        llvm::Function::arg_iterator iter = function->arg_begin();
        BOOST_FOREACH(ast::identifier const& arg, x.args)
        {
            // Create an alloca for this variable.
            llvm::AllocaInst* alloca =
                create_entry_block_alloca(function, arg.name, context());

            // Store the initial value into the alloca.
            builder.CreateStore(iter, alloca);

            // Add arguments to variable symbol table.
            named_values[arg.name] = alloca;
            ++iter;
        }

        if (!void_return)
        {
            // Create an alloca for the return value
            return_alloca =
                create_entry_block_alloca(function, "return.val", context());
        }
    }

    bool compiler::operator()(ast::function const& x)
    {
        ///////////////////////////////////////////////////////////////////////
        // the signature:
        llvm::Function* function = function_decl(x);
        if (function == 0)
            return false;

        ///////////////////////////////////////////////////////////////////////
        // the body:
        if (x.body) // compile the body if this is not a prototype
        {
            // Create a new basic block to start insertion into.
            llvm::BasicBlock* block =
                llvm::BasicBlock::Create(context(), "entry", function);
            builder.SetInsertPoint(block);

            function_allocas(x, function);
            return_block = llvm::BasicBlock::Create(context(), "return");

            if (!(*this)(*x.body))
            {
                // Error reading body, remove function.
                function->eraseFromParent();
                return false;
            }

            llvm::BasicBlock* last_block =
                &function->getBasicBlockList().back();

            // If the last block is unterminated, connect it to return_block
            if (last_block->getTerminator() == 0)
            {
                builder.SetInsertPoint(last_block);
                builder.CreateBr(return_block);
            }

            function->getBasicBlockList().push_back(return_block);
            builder.SetInsertPoint(return_block);

            if (void_return)
                builder.CreateRetVoid();
            else
                builder.CreateRet(builder.CreateLoad(return_alloca, "return.val"));

            //~ vm.module()->dump();

            // Validate the generated code, checking for consistency.
            llvm::verifyFunction(*function);

            // Optimize the function.
            fpm.run(*function);
        }

        return true;
    }

    bool compiler::operator()(ast::function_list const& x)
    {
        BOOST_FOREACH(ast::function const& f, x)
        {
            named_values.clear(); // clear the variables
            if (!(*this)(f))
                return false;
        }
        return true;
    }
}}
