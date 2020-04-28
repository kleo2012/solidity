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
/**
 * Class that contains contextual information during IR generation.
 */

#include <libsolidity/codegen/ir/IRGenerationContext.h>

#include <libsolidity/codegen/YulUtilFunctions.h>
#include <libsolidity/codegen/ABIFunctions.h>
#include <libsolidity/codegen/CompilerUtils.h>
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>

#include <libsolutil/Whiskers.h>
#include <libsolutil/StringUtils.h>

#include <boost/range/adaptor/map.hpp>

using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::frontend;

string IRGenerationContext::enqueueFunctionForCodeGeneration(FunctionDefinition const& _function)
{
	string name = functionName(_function);

	if (!m_functions.contains(name))
		m_functionGenerationQueue.insert(&_function);

	return name;
}

FunctionDefinition const* IRGenerationContext::dequeueFunctionForCodeGeneration()
{
	solAssert(!m_functionGenerationQueue.empty(), "");

	FunctionDefinition const* result = *m_functionGenerationQueue.begin();
	m_functionGenerationQueue.erase(m_functionGenerationQueue.begin());
	return result;
}

ContractDefinition const& IRGenerationContext::mostDerivedContract() const
{
	solAssert(m_mostDerivedContract, "Most derived contract requested but not set.");
	return *m_mostDerivedContract;
}

IRVariable const& IRGenerationContext::addLocalVariable(VariableDeclaration const& _varDecl)
{
	auto const& [it, didInsert] = m_localVariables.emplace(
		std::make_pair(&_varDecl, IRVariable{_varDecl})
	);
	solAssert(didInsert, "Local variable added multiple times.");
	return it->second;
}

IRVariable const& IRGenerationContext::localVariable(VariableDeclaration const& _varDecl)
{
	solAssert(
		m_localVariables.count(&_varDecl),
		"Unknown variable: " + _varDecl.name()
	);
	return m_localVariables.at(&_varDecl);
}

void IRGenerationContext::registerImmutableVariable(VariableDeclaration const& _variable)
{
	solAssert(_variable.immutable(), "Attempted to register a non-immutable variable as immutable.");
	solUnimplementedAssert(
		_variable.annotation().type->isValueType(),
		"Only immutable variables of value type are supported."
	);
	solAssert(m_reservedMemory.has_value(), "Reserved memory has already been reset.");
	m_immutableVariables[&_variable] = CompilerUtils::generalPurposeMemoryStart + *m_reservedMemory;
	solAssert(_variable.annotation().type->memoryHeadSize() == 32, "Memory writes might overlap.");
	*m_reservedMemory += _variable.annotation().type->memoryHeadSize();
}

size_t IRGenerationContext::immutableMemoryOffset(VariableDeclaration const& _variable) const
{
	solAssert(
		m_immutableVariables.count(&_variable),
		"Unknown immutable variable: " + _variable.name()
	);
	return m_immutableVariables.at(&_variable);
}

size_t IRGenerationContext::reservedMemory()
{
	solAssert(m_reservedMemory.has_value(), "Reserved memory was used before.");
	size_t reservedMemory = *m_reservedMemory;
	m_reservedMemory = std::nullopt;
	return reservedMemory;
}

void IRGenerationContext::addStateVariable(
	VariableDeclaration const& _declaration,
	u256 _storageOffset,
	unsigned _byteOffset
)
{
	m_stateVariables[&_declaration] = make_pair(move(_storageOffset), _byteOffset);
}

string IRGenerationContext::functionName(FunctionDefinition const& _function)
{
	// @TODO previously, we had to distinguish creation context and runtime context,
	// but since we do not work with jump positions anymore, this should not be a problem, right?
	return "fun_" + _function.name() + "_" + to_string(_function.id());
}

string IRGenerationContext::functionName(VariableDeclaration const& _varDecl)
{
	return "getter_fun_" + _varDecl.name() + "_" + to_string(_varDecl.id());
}

string IRGenerationContext::creationObjectName(ContractDefinition const& _contract) const
{
	return _contract.name() + "_" + toString(_contract.id());
}
string IRGenerationContext::runtimeObjectName(ContractDefinition const& _contract) const
{
	return _contract.name() + "_" + toString(_contract.id()) + "_deployed";
}

string IRGenerationContext::newYulVariable()
{
	return "_" + to_string(++m_varCounter);
}

string IRGenerationContext::trySuccessConditionVariable(Expression const& _expression) const
{
	// NB: The TypeChecker already ensured that the Expression is of type FunctionCall.
	solAssert(
		static_cast<FunctionCallAnnotation const&>(_expression.annotation()).tryCall,
		"Parameter must be a FunctionCall with tryCall-annotation set."
	);

	return "trySuccessCondition_" + to_string(_expression.id());
}

void IRGenerationContext::setInternalDispatchCandidates(InternalDispatchMap _internalDispatchCandidates)
{
	solAssert(internalDispatchClean(), "");

	m_internalDispatchCandidates = move(_internalDispatchCandidates);
}

tuple<InternalDispatchMap, InternalDispatchMap> IRGenerationContext::consumeInternalDispatchMap()
{
	solAssert(
		m_dispatchableInternalFunctionReferences.empty(),
		"You must call moveCollectedReferencesToDispatch() before constructing internal dispatch map."
	);

	InternalDispatchMap internalDispatch = move(m_internalDispatch);
	InternalDispatchMap internalDispatchCandidates = move(m_internalDispatchCandidates);

	m_internalDispatch.clear();
	m_internalDispatchCandidates.clear();

	return {move(internalDispatch), move(internalDispatchCandidates)};
}

void IRGenerationContext::moveCollectedReferencesToDispatch()
{
	// First, find (empty) arities newly registered in m_internalDispatch and fill them with
	// candidates collected so far.
	for (auto dispatchIt = m_internalDispatch.begin(); dispatchIt != m_internalDispatch.end(); ++dispatchIt)
	{
		auto candidateIt = m_internalDispatchCandidates.find(dispatchIt->first);

		solAssert(dispatchIt->second.empty() || candidateIt == m_internalDispatchCandidates.end(), "");

		if (candidateIt != m_internalDispatchCandidates.end())
		{
			for (FunctionDefinition const* function: candidateIt->second)
				enqueueFunctionForCodeGeneration(*function);

			dispatchIt->second = move(candidateIt->second);
			m_internalDispatchCandidates.erase(candidateIt);
		}
	}

	// Now process the references, adding them either as candidates or as dispatch members,
	// depending on whether the arity has been registered or not.
	for (auto function: m_dispatchableInternalFunctionReferences | boost::adaptors::map_values)
	{
		solAssert(function, "");
		Arity arity = functionArity(*function);

		auto dispatchIt = m_internalDispatch.find(arity);
		auto candidateIt = m_internalDispatchCandidates.find(arity);
		solAssert((dispatchIt == m_internalDispatch.end()) || (candidateIt == m_internalDispatchCandidates.end()), "");

		if (dispatchIt != m_internalDispatch.end())
		{
			dispatchIt->second.insert(function);
			enqueueFunctionForCodeGeneration(*function);
		}
		else if (candidateIt != m_internalDispatchCandidates.end())
			candidateIt->second.insert(function);
		else
			m_internalDispatchCandidates[arity] = {function};
	}
	m_dispatchableInternalFunctionReferences.clear();
}

string IRGenerationContext::collectDispatchableReference(Expression const& _expression, FunctionDefinition const& _function)
{
	solAssert(m_dispatchableInternalFunctionReferences.count(&_expression) == 0, "");

	m_dispatchableInternalFunctionReferences[&_expression] = &_function;
	return internalDispatchFunctionName(functionArity(_function));
}

void IRGenerationContext::forgetDispatchableReference(Expression const& _expression)
{
	solAssert(m_dispatchableInternalFunctionReferences.count(&_expression) > 0, "");

	m_dispatchableInternalFunctionReferences.erase(&_expression);
}

string IRGenerationContext::registerInternalDispatch(Arity const& _arity)
{
	m_internalDispatch.try_emplace(_arity);
	return internalDispatchFunctionName(_arity);
}

Arity IRGenerationContext::functionArity(FunctionDefinition const& _function)
{
	FunctionType const* functionType = TypeProvider::function(_function)->asCallableFunction(false);
	solAssert(functionType, "");
	return functionArity(*functionType);
}

Arity IRGenerationContext::functionArity(FunctionType const& _functionType)
{
	return {
		TupleType(_functionType.parameterTypes()).sizeOnStack(),
		TupleType(_functionType.returnParameterTypes()).sizeOnStack()
	};
}

string IRGenerationContext::internalDispatchFunctionName(Arity const& _arity)
{
	return "dispatch_internal"
		"_in_" + to_string(_arity.in) +
		"_out_" + to_string(_arity.out);
}

string IRGenerationContext::internalDispatch(Arity const& _arity, set<FunctionDefinition const*> const& _functions)
{
	vector<map<string, string>> cases;
	for (auto const* function: _functions)
	{
		solAssert(function, "");
		solAssert(functionArity(*function) == _arity, "A single dispatch function can only handle functions of one arity");
		solAssert(!function->isConstructor(), "");
		// 0 is reserved for uninitialized function pointers
		solAssert(function->id() != 0, "Unexpected function ID: 0");

		cases.emplace_back(map<string, string>{
			{"funID", to_string(function->id())},
			{"name", functionName(*function)}
		});
	}

	string funName = internalDispatchFunctionName(_arity);
	return m_functions.createFunction(funName, [&, cases(move(cases))]() {
		Whiskers templ(R"(
			function <functionName>(fun <comma> <in>) <arrow> <out> {
				switch fun
				<#cases>
				case <funID>
				{
					<out> <assignment_op> <name>(<in>)
				}
				</cases>
				default { invalid() }
			}
		)");
		templ("functionName", funName);
		templ("comma", _arity.in> 0 ? "," : "");
		YulUtilFunctions utils(m_evmVersion, m_revertStrings, m_functions);
		templ("in", suffixedVariableNameList("in_", 0, _arity.in));
		templ("arrow", _arity.out> 0 ? "->" : "");
		templ("assignment_op", _arity.out> 0 ? ":=" : "");
		templ("out", suffixedVariableNameList("out_", 0, _arity.out));
		templ("cases", move(cases));
		return templ.render();
	});
}

YulUtilFunctions IRGenerationContext::utils()
{
	return YulUtilFunctions(m_evmVersion, m_revertStrings, m_functions);
}

ABIFunctions IRGenerationContext::abiFunctions()
{
	return ABIFunctions(m_evmVersion, m_revertStrings, m_functions);
}

std::string IRGenerationContext::revertReasonIfDebug(std::string const& _message)
{
	return YulUtilFunctions::revertReasonIfDebug(m_revertStrings, _message);
}

