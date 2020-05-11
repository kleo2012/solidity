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

#include <libyul/optimiser/ReasoningBasedSimplifier.h>

#include <libyul/optimiser/SSAValueTracker.h>
#include <libyul/AsmData.h>
#include <libyul/Utilities.h>
#include <libyul/Dialect.h>

#include <libyul/backends/evm/EVMDialect.h>

#include <libsolidity/formal/Z3Interface.h>

#include <libsolutil/Visitor.h>
#include <libsolutil/CommonData.h>

#include <utility>
#include <memory>

using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::yul;
using namespace solidity::frontend;
using namespace solidity::frontend::smt;

void ReasoningBasedSimplifier::run(OptimiserStepContext& _context, Block& _ast)
{
	set<YulString> ssaVars = SSAValueTracker::ssaVariables(_ast);
	ReasoningBasedSimplifier{_context.dialect, ssaVars}(_ast);
}

void ReasoningBasedSimplifier::operator()(VariableDeclaration& _varDecl)
{
	if (_varDecl.variables.size() != 1 || !_varDecl.value)
		return;
	YulString varName = _varDecl.variables.front().name;
	if (!m_ssaVariables.count(varName))
		return;
	m_variables.insert({varName, m_solver->newVariable("yul_" + varName.str(), SortProvider::intSort)});
	m_solver->addAssertion(m_variables.at(varName) == encodeExpression(*_varDecl.value));
}

void ReasoningBasedSimplifier::operator()(If& _if)
{
	smt::Expression condition = encodeExpression(*_if.condition);
	m_solver->push();
	m_solver->addAssertion(condition == size_t(0));
	CheckResult result = m_solver->check({}).first;
	m_solver->pop();
	if (result == CheckResult::UNSATISFIABLE)
		_if.condition = make_unique<yul::Expression>(Literal{locationOf(*_if.condition), LiteralKind::Number, "1"_yulstring, {}});

	m_solver->push();
	m_solver->addAssertion(condition != size_t(0));
	CheckResult result2 = m_solver->check({}).first;
	m_solver->pop();
	if (result2 == CheckResult::UNSATISFIABLE)
		// TODO we could actually skip the body is this case
		_if.condition = make_unique<yul::Expression>(Literal{locationOf(*_if.condition), LiteralKind::Number, "0"_yulstring, {}});

	m_solver->push();
	m_solver->addAssertion(condition != 0);

	ASTModifier::operator()(_if.body);

	m_solver->pop();
}

ReasoningBasedSimplifier::ReasoningBasedSimplifier(
	Dialect const& _dialect,
	set<YulString> const& _ssaVariables
):
	m_dialect(_dialect),
	m_ssaVariables(_ssaVariables)
{
	m_solver = make_unique<smt::Z3Interface>();
}

smt::Expression ReasoningBasedSimplifier::encodeExpression(Expression const& _expression)
{
	return std::visit(GenericVisitor{
		[&](FunctionCall const& _functionCall)
		{
			if (auto const* dialect = dynamic_cast<EVMDialect const*>(&m_dialect))
				if (auto const* builtin = dialect->builtin(_functionCall.functionName.name))
					if (builtin->instruction)
						return encodeBuiltin(*builtin->instruction, _functionCall.arguments);
			return newVariable();
		},
		[&](Identifier const& _identifier)
		{
			if (
				m_ssaVariables.count(_identifier.name) &&
				m_variables.count(_identifier.name)
			)
				return m_variables.at(_identifier.name);
			else
				return newVariable();
		},
		[&](Literal const& _literal)
		{
			return smt::Expression(valueOfLiteral(_literal));
		}
	}, _expression);
}

smt::Expression ReasoningBasedSimplifier::encodeBuiltin(
	evmasm::Instruction _instruction,
	vector<Expression> const& _arguments
)
{
	vector<smt::Expression> arguments = applyMap(
		_arguments,
		[=](Expression const& _expr) { return encodeExpression(_expr); }
	);
	switch (_instruction)
	{
	case evmasm::Instruction::LT:
		return smt::Expression::ite(arguments.at(0) < arguments.at(1), 1, 0);
	case evmasm::Instruction::GT:
		return smt::Expression::ite(arguments.at(0) > arguments.at(1), 1, 0);
	case evmasm::Instruction::ADD:
		// TODO apply wrapping
		return arguments.at(0) + arguments.at(1);
	default:
		break;
	}
	return newVariable();
}

smt::Expression ReasoningBasedSimplifier::newVariable()
{
	return m_solver->newVariable(uniqueName(), SortProvider::intSort);
}

string ReasoningBasedSimplifier::uniqueName()
{
	return "expr_" + to_string(m_varCounter++);
}
