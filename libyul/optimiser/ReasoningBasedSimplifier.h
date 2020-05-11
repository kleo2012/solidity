/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

#include <libyul/optimiser/ASTWalker.h>
#include <libyul/optimiser/OptimiserStep.h>
#include <libyul/Dialect.h>

// because of instruction
#include <libyul/backends/evm/EVMDialect.h>

#include <map>

namespace solidity::frontend::smt
{
class SolverInterface;
class Expression;
}

namespace solidity::yul
{

/**
 * Reasoning-based simplifier.
 *
 */
class ReasoningBasedSimplifier: public ASTModifier
{
public:
	static constexpr char const* name{"ReasoningBasedSimplifier"};
	static void run(OptimiserStepContext& _context, Block& _ast);

	using ASTModifier::operator();
	void operator()(VariableDeclaration& _varDecl) override;
	void operator()(If& _if) override;

private:
	explicit ReasoningBasedSimplifier(
		Dialect const& _dialect,
		std::set<YulString> const& _ssaVariables
	);

	frontend::smt::Expression encodeExpression(
		Expression const& _expression
	);

	frontend::smt::Expression encodeBuiltin(
		evmasm::Instruction _instruction,
		std::vector<Expression> const& _arguments
	);

	frontend::smt::Expression newVariable();
	std::string uniqueName();

	Dialect const& m_dialect;
	std::set<YulString> const& m_ssaVariables;
	std::unique_ptr<frontend::smt::SolverInterface> m_solver;
	std::map<YulString, frontend::smt::Expression> m_variables;

	size_t m_varCounter = 0;
};

}
