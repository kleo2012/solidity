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

#pragma once

#include <libsolidity/ast/AST.h>
#include <libsolidity/codegen/ir/IRVariable.h>
#include <libsolidity/interface/OptimiserSettings.h>
#include <libsolidity/interface/DebugSettings.h>

#include <libsolidity/codegen/MultiUseYulFunctionCollector.h>

#include <liblangutil/EVMVersion.h>

#include <libsolutil/Common.h>

#include <algorithm>
#include <set>
#include <string>
#include <memory>
#include <vector>

namespace solidity::frontend
{

class YulUtilFunctions;
class ABIFunctions;

/**
 * Structure that describes arity and co-arity of a function, i.e. the number of its inputs and outputs.
 */
struct Arity
{
	size_t in;  /// Number of input parameters
	size_t out; /// Number of output parameters

	bool operator==(Arity const& _other) const { return in == _other.in && out == _other.out; }
	bool operator!=(Arity const& _other) const { return !(*this == _other); }
};

}

// Overloading std::less() makes it possible to use Arity as a map key. We could define operator<
// instead but that would be confusing since e.g. Arity{2, 2} would be greater than Arity{1, 10}.
template<>
struct std::less<solidity::frontend::Arity>
{
	bool operator() (solidity::frontend::Arity const& _lhs, solidity::frontend::Arity const& _rhs) const
	{
		return _lhs.in < _rhs.in || (_lhs.in == _rhs.in && _lhs.out < _rhs.out);
	}
};

namespace solidity::frontend
{

using InternalDispatchMap = std::map<Arity, std::set<FunctionDefinition const*>>;

/**
 * Class that contains contextual information during IR generation.
 */
class IRGenerationContext
{
public:
	IRGenerationContext(
		langutil::EVMVersion _evmVersion,
		RevertStrings _revertStrings,
		OptimiserSettings _optimiserSettings
	):
		m_evmVersion(_evmVersion),
		m_revertStrings(_revertStrings),
		m_optimiserSettings(std::move(_optimiserSettings))
	{}

	MultiUseYulFunctionCollector& functionCollector() { return m_functions; }

	/// Adds a Solidity function to the function generation queue and returns the name of the
	/// corresponding Yul function.
	std::string enqueueFunctionForCodeGeneration(FunctionDefinition const& _function);

	/// Pops one item from the function generation queue. Must not be called if the queue is empty.
	FunctionDefinition const* dequeueFunctionForCodeGeneration();

	bool functionGenerationQueueEmpty() { return m_functionGenerationQueue.empty(); }

	/// Sets the most derived contract (the one currently being compiled)>
	void setMostDerivedContract(ContractDefinition const& _mostDerivedContract)
	{
		m_mostDerivedContract = &_mostDerivedContract;
	}
	ContractDefinition const& mostDerivedContract() const;


	IRVariable const& addLocalVariable(VariableDeclaration const& _varDecl);
	bool isLocalVariable(VariableDeclaration const& _varDecl) const { return m_localVariables.count(&_varDecl); }
	IRVariable const& localVariable(VariableDeclaration const& _varDecl);

	/// Registers an immutable variable of the contract.
	/// Should only be called at construction time.
	void registerImmutableVariable(VariableDeclaration const& _varDecl);
	/// @returns the reserved memory for storing the value of the
	/// immutable @a _variable during contract creation.
	size_t immutableMemoryOffset(VariableDeclaration const& _variable) const;
	/// @returns the reserved memory and resets it to mark it as used.
	/// Intended to be used only once for initializing the free memory pointer
	/// to after the area used for immutables.
	size_t reservedMemory();

	void addStateVariable(VariableDeclaration const& _varDecl, u256 _storageOffset, unsigned _byteOffset);
	bool isStateVariable(VariableDeclaration const& _varDecl) const { return m_stateVariables.count(&_varDecl); }
	std::pair<u256, unsigned> storageLocationOfVariable(VariableDeclaration const& _varDecl) const
	{
		return m_stateVariables.at(&_varDecl);
	}

	std::string functionName(FunctionDefinition const& _function);
	std::string functionName(VariableDeclaration const& _varDecl);

	std::string creationObjectName(ContractDefinition const& _contract) const;
	std::string runtimeObjectName(ContractDefinition const& _contract) const;

	std::string newYulVariable();

	/// Initializes the collection of dispatch candidates with specified functions.
	void setInternalDispatchCandidates(InternalDispatchMap _internalDispatchMap);

	/// Returns two collections: functions that need to be callable via internal dispatch and
	/// candidates that were rejected because they're never actually called via pointers.
	/// This is the last step in gathering content for internal dispatch generation and the function
	/// also clears the collections stored in the context so that the process can be started again
	/// from scratch.
	///
	/// Preserving the candidates is necessary when generating multiple, distinct assemblies that
	/// can share function pointers. For example when a constructor puts a pointer to an
	/// internal function in a storage variable and an external function uses that variable to call
	/// that internal function. Such a function will not be recognized as a candidate for internal
	/// dispatch when visiting the runtime code. You need to have the candidates detected in the
	/// deployment code to be able to generate valid internal dispatch in this situation.
	///
	/// Can only be called immediately after @a moveCollectedReferencesToDispatch().
	std::tuple<InternalDispatchMap, InternalDispatchMap> consumeInternalDispatchMap();

	/// Prepares internal dispatch content to be consumed. This involves moving functions from
	/// @a m_dispatchableInternalFunctionReferences to the candidate pool and then promoting candidates
	/// to the dispatch if a pointer through which they might be called was found.
	///
	/// This function should be called after all the code has been visited by the generator. Note
	/// that the promoted candidates are added to the code generation queue which may introduce
	/// more code to be visited. For this reason you need to call it multiple times alternating
	/// with code generation until the queue is empty. Only then it's safe to call @a consumeInternalDispatchMap().
	void moveCollectedReferencesToDispatch();

	/// Returns true if the context has not collected any functions or candidates for inclusion
	/// in the internal dispatch.
	bool internalDispatchClean() const {
		return
			m_internalDispatch.empty() &&
			m_internalDispatchCandidates.empty() &&
			m_dispatchableInternalFunctionReferences.empty();
	}

	/// Registers an expression that references an internal function by name as a tentative candidate
	/// for inclusion in internal dispatch. The function will become an actual candidate
	/// if it's not removed using @a forgetDispatchableReference() before the next call to
	/// @a moveCollectedReferencesToDispatch().
	///
	/// Must not be called more than once with the same expression.
	std::string collectDispatchableReference(Expression const& _expression, FunctionDefinition const& _function);

	/// Removes an expression that references an internal function by name from the collection of
	/// tentative candidates for inclusion in internal dispatch. The functions should be called
	/// if it turns out that the expression represents a direct function call and does not really need to
	/// to through the dispatch.
	///
	/// Must not be called if the expression has not been previously added using @a forgetDispatchableReference().
	void forgetDispatchableReference(Expression const& _expression);

	/// Registers the fact that an internal function call through a pointer of specified arity has
	/// been detected. This means that all candidates of that arity will now be included in the dispatch.
	/// Note: the candidates are not actually moved until you call @a moveCollectedReferencesToDispatch().
	std::string registerInternalDispatch(Arity const& _arity);

	static Arity functionArity(FunctionDefinition const& _function);
	static Arity functionArity(FunctionType const& _functionType);
	static std::string internalDispatchFunctionName(Arity const& _arity);

	/// Generates a Yul function that can simulate a call to one of the specified functions via a pointer.
	/// All the functions must have the same number of input and output arguments. If they differ,
	/// it's necessary to make multiple calls to generate all the internal dispatch functions
	/// (one for each arity).
	std::string internalDispatch(Arity const& _arity, std::set<FunctionDefinition const*> const& _functions);

	/// @returns a new copy of the utility function generator (but using the same function set).
	YulUtilFunctions utils();

	langutil::EVMVersion evmVersion() const { return m_evmVersion; };

	ABIFunctions abiFunctions();

	/// @returns code that stores @param _message for revert reason
	/// if m_revertStrings is debug.
	std::string revertReasonIfDebug(std::string const& _message = "");

	RevertStrings revertStrings() const { return m_revertStrings; }

	/// @returns the variable name that can be used to inspect the success or failure of an external
	/// function call that was invoked as part of the try statement.
	std::string trySuccessConditionVariable(Expression const& _expression) const;

	std::set<ContractDefinition const*, ASTNode::CompareByID>& subObjectsCreated() { return m_subObjects; }

private:
	langutil::EVMVersion m_evmVersion;
	RevertStrings m_revertStrings;
	OptimiserSettings m_optimiserSettings;
	ContractDefinition const* m_mostDerivedContract = nullptr;
	std::map<VariableDeclaration const*, IRVariable> m_localVariables;
	/// Memory offsets reserved for the values of immutable variables during contract creation.
	/// This map is empty in the runtime context.
	std::map<VariableDeclaration const*, size_t> m_immutableVariables;
	/// Total amount of reserved memory. Reserved memory is used to store
	/// immutable variables during contract creation.
	std::optional<size_t> m_reservedMemory = {0};
	/// Storage offsets of state variables
	std::map<VariableDeclaration const*, std::pair<u256, unsigned>> m_stateVariables;
	MultiUseYulFunctionCollector m_functions;
	size_t m_varCounter = 0;

	/// Function definitions queued for code generation. They're the Solidity functions whose calls
	/// were discovered by the IR generator during AST traversal.
	/// Note that the queue gets filled in a lazy way - new definitions can be added while the
	/// collected ones get removed and traversed.
	/// The order and duplicates are irrelevant here (hence std::set rather than std::queue) as
	/// long as the order of Yul functions in the generated code is deterministic and the same on
	/// all platforms - which is a property guaranteed by MultiUseYulFunctionCollector.
	std::set<FunctionDefinition const*> m_functionGenerationQueue;

	/// Collection of functions that need to be callable via internal dispatch.
	/// These are internal functions which satisfy all of the following conditions:
	/// 1. Are referenced by name in an expression other than a direct function call.
	/// 2. There exists at least one call of any internal function of the same arity via a pointer.
	/// Note that having a key with an empty set of functions is a valid situation. It means that
	/// the code contains a call via a pointer even though a specific function is never assigned to it.
	/// It will fail at runtime but the code must still compile.
	InternalDispatchMap m_internalDispatch;

	/// Collection of functions that are referenced by name in expressions other than direct
	/// function calls but are never actually called via pointers. We do not need a dispatch for
	/// them yet but we keep track of them in case such a call is detected later.
	/// May contain keys matching arities present in @a m_internalDispatch but only temporarily
	/// (until the next call to @a moveCollectedReferencesToDispatch()).
	InternalDispatchMap m_internalDispatchCandidates;

	/// A helper collection for detecting functions referenced by name in expressions other than
	/// direct function calls. It receives all expressions where a function is mentioned by
	/// name and if they're later determined to be direct function calls, they're removed.
	/// Once all the reachable code has been visited, @a moveCollectedReferencesToDispatch() must
	/// be called to move the content to @a m_internalDispatch and @a m_internalDispatchCandidates.
	std::map<Expression const*, FunctionDefinition const*> m_dispatchableInternalFunctionReferences;

	std::set<ContractDefinition const*, ASTNode::CompareByID> m_subObjects;
};

}
